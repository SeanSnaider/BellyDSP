"""
Noise gate reference simulation and design study (BUILD_PLAN "Gates"), sample by sample in double
precision. The C++ block (src/dsp/Gate.cpp) has to match the renders this writes: the golden test in
tests/GateTests.cpp.

Per sample:
  1. Detector input: the DI or the gate's own input, through a 24 dB/oct Butterworth sidechain
     high-pass (default 100 Hz), so 60 Hz hum can't hold the gate open.
  2. Peak level D: the largest |sample| in the last ~10 ms (a sliding window, kept as 20 chunk maxima
     of 0.5 ms plus the chunk being filled). A pick registers on its first sample; a stop shows up
     ~10 ms later, when the last loud peak leaves the window. L = 20 log10(D).
  3. Gate state, with hysteresis: closed -> opening when L >= T_open (the threshold); open stays
     open while L >= T_close = T_open - hysteresis, then counts down the hold, then releases.
  4. Openness e in [0, 1]: the attack is a raised cosine from where e was to 1 over `attack`; the
     release is an exponential decay e *= a, which is a straight line in dB, with the release
     time defined as the time to fall 60 dB. The gain is 1 - (1 - floor)(1 - e), floor = the range
     (0 at -90 dB: a true mute).
  5. Adaptive release. Two envelope followers: the fast one is D itself (it lets go of a stop after
     the 10 ms window); the slow one S = max(D, S * k) falls at most SLOW_FALL_DB_PER_S. A note
     that decays slower than that is tracked exactly by S, so the gap d = 20 log10(S / D) stays
     near 0 dB; a stop drops D faster than S can follow and opens the gap at (rate - 100) dB/s.
     The largest gap since the level fell below T_close picks the release time: the knob (slow)
     for d <= D_LOW, FAST_RELEASE_MS for d >= D_HIGH, log-interpolated between.

Usage:
  uv run --with numpy --with scipy python prototypes/gate.py --study      # the design study tables
  uv run --with numpy --with scipy python prototypes/gate.py --golden tests/fixtures/gate
"""

import argparse
import json
import math
import os

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt

FS = 48000.0

# ---- Constants (mirrored in src/dsp/Gate.h) ----------------------------------------------------
CHUNK_SAMPLES = 24          # 0.5 ms at 48 kHz
NUM_CHUNKS = 20             # window: 20 completed chunks plus the current one, 10.0 to 10.5 ms
SLOW_FALL_DB_PER_S = 100.0  # the slow follower's fastest fall: faster than a ringing note decays
D_LOW, D_HIGH = 8.0, 20.0   # adaptive mapping: gap in dB for the slow and the fast release
FAST_RELEASE_MS = 20.0      # adaptive release for a stop (time to fall 60 dB)
RELEASE_RANGE_DB = 60.0     # the release knob is the time to fall this far
MUTE_DB = -90.0             # range at or below this is a true mute (gain exactly 0)
E_SNAP = 1.0e-6             # openness below this snaps to 0 (-120 dB)
MIN_LEVEL = 1.0e-9          # -180 dB: keeps log10 finite in silence
BUTTERWORTH_Q = (1.3065629648763766, 0.5411961001461970)  # 24 dB/oct: 1 / (2 sin((2k-1) pi / 8))
LEARN_SECONDS = 2.0
LEARN_PERCENTILE = 0.95     # of the detector level while learning: robust to a 100 ms glitch
LEARN_MARGIN_DB = 6.0       # the close threshold goes this far above the noise
LEARN_BIN_DB = 0.25         # histogram resolution, from LEARN_FLOOR_DB to 0 dB
LEARN_FLOOR_DB = -160.0

DEFAULTS = dict(threshold_db=-55.0, hysteresis_db=8.0, hold_ms=10.0, attack_ms=0.5, release_ms=250.0,
                release_mode="adaptive", range_db=-90.0, detector="di", sidechain_high_pass=True,
                sidechain_hz=100.0)


class SvfHighpass:
    """Simper's trapezoidal SVF, high-pass output: the same structure as src/dsp/Svf.h."""

    def __init__(self, fc, q):
        g = math.tan(math.pi * fc / FS)
        k = 1.0 / q
        self.a1 = 1.0 / (1.0 + g * (g + k))
        self.a2 = g * self.a1
        self.a3 = g * self.a2
        self.k = k
        self.ic1 = self.ic2 = 0.0

    def __call__(self, v0):
        v3 = v0 - self.ic2
        v1 = self.a1 * self.ic1 + self.a2 * v3
        v2 = self.ic2 + self.a2 * self.ic1 + self.a3 * v3
        self.ic1 = 2.0 * v1 - self.ic1
        self.ic2 = 2.0 * v2 - self.ic2
        return v0 - self.k * v1 - v2


def release_coefficient(ms):
    """Per-sample factor that falls RELEASE_RANGE_DB in `ms` milliseconds: 10^(-60 / 20 / samples)."""
    return math.pow(10.0, -RELEASE_RANGE_DB / 20.0 / (ms * 0.001 * FS))


def adaptive_release_ms(gap_db, knob_ms):
    slow, fast = knob_ms, min(FAST_RELEASE_MS, knob_ms)
    if gap_db <= D_LOW:
        return slow
    if gap_db >= D_HIGH:
        return fast
    t = (gap_db - D_LOW) / (D_HIGH - D_LOW)
    return math.exp(math.log(slow) + t * (math.log(fast) - math.log(slow)))


OPENING, OPEN, RELEASING = 0, 1, 2


class Gate:
    """One gate with static settings (the golden renders don't move knobs)."""

    def __init__(self, s):
        self.s = dict(DEFAULTS, **s)
        s = self.s
        self.filters = [SvfHighpass(s["sidechain_hz"], q) for q in BUTTERWORTH_Q]
        self.k_slow = math.pow(10.0, -SLOW_FALL_DB_PER_S / 20.0 / FS)
        self.hold_samples = int(round(s["hold_ms"] * 0.001 * FS))
        self.attack_step = 1.0 / max(1.0, s["attack_ms"] * 0.001 * FS)
        self.floor = 0.0 if s["range_db"] <= MUTE_DB else math.pow(10.0, s["range_db"] / 20.0)
        self.t_open = s["threshold_db"]
        self.t_close = s["threshold_db"] - s["hysteresis_db"]
        self.ring = [0.0] * NUM_CHUNKS
        self.ring_pos = 0
        self.ring_max = 0.0
        self.chunk_max = 0.0
        self.chunk_fill = 0
        self.slow = 0.0
        # Fresh state: open, hold armed. If nothing is playing it closes through its release.
        self.state = OPEN
        self.e = 1.0
        self.e0 = 1.0
        self.phase = 0.0
        self.hold_left = self.hold_samples
        self.below = False
        self.gap_peak = 0.0
        self.coef_gap = None
        self.coef = 1.0

    def detector_level(self, v):
        if self.s["sidechain_high_pass"]:
            for f in self.filters:
                v = f(v)
        a = abs(v)
        # Sliding-window peak: the chunk being filled plus the last NUM_CHUNKS completed chunks.
        if a > self.chunk_max:
            self.chunk_max = a
        d = self.chunk_max if self.chunk_max > self.ring_max else self.ring_max
        self.chunk_fill += 1
        if self.chunk_fill == CHUNK_SAMPLES:
            self.ring[self.ring_pos] = self.chunk_max
            self.ring_pos = (self.ring_pos + 1) % NUM_CHUNKS
            self.ring_max = max(self.ring)
            self.chunk_max = 0.0
            self.chunk_fill = 0
        return d

    def step(self, di, own):
        s = self.s
        d_lin = self.detector_level(di if s["detector"] == "di" else own)
        level_db = 20.0 * math.log10(max(d_lin, MIN_LEVEL))
        held = self.slow * self.k_slow
        self.slow = d_lin if d_lin > held else held
        gap = 20.0 * math.log10(max(self.slow, MIN_LEVEL)) - level_db

        if self.state == RELEASING:
            if level_db >= self.t_open:
                self.state, self.phase, self.e0 = OPENING, 0.0, self.e
                self.hold_left, self.below = self.hold_samples, False
            elif gap > self.gap_peak:
                self.gap_peak = gap
        else:
            if level_db >= self.t_close:
                self.hold_left, self.below = self.hold_samples, False
            else:
                if not self.below:
                    self.below, self.gap_peak = True, gap
                elif gap > self.gap_peak:
                    self.gap_peak = gap
                if self.hold_left > 0:
                    self.hold_left -= 1
                else:
                    self.state = RELEASING

        if self.state == OPENING:
            self.phase += self.attack_step
            if self.phase >= 1.0:
                self.state, self.e = OPEN, 1.0
            else:
                self.e = self.e0 + (1.0 - self.e0) * 0.5 * (1.0 - math.cos(math.pi * self.phase))
        elif self.state == RELEASING:
            gap_used = self.gap_peak if s["release_mode"] == "adaptive" else 0.0
            if gap_used != self.coef_gap:
                self.coef_gap = gap_used
                self.coef = release_coefficient(adaptive_release_ms(gap_used, s["release_ms"]))
            self.e *= self.coef
            if self.e < E_SNAP:
                self.e = 0.0

        gain = 1.0 - (1.0 - self.floor) * (1.0 - self.e)
        return gain, level_db, gap


def learn(settings, di, own=None):
    """What Learn measures on 2 s of signal: (noise floor dB, threshold dB). The noise floor is the 95th
    percentile of the detector level, from a 0.25 dB histogram, taken at the bin's upper edge; the close
    threshold goes LEARN_MARGIN_DB above it, so the threshold (open) is margin + hysteresis above it."""
    own = di if own is None else own
    g = Gate(settings)
    bins = int(-LEARN_FLOOR_DB / LEARN_BIN_DB)
    counts = [0] * bins
    total = int(LEARN_SECONDS * FS)
    for i in range(total):
        _, level_db, _ = g.step(float(di[i]), float(own[i]))
        b = int(math.floor((level_db - LEARN_FLOOR_DB) / LEARN_BIN_DB))
        counts[min(bins - 1, max(0, b))] += 1
    needed = math.ceil(LEARN_PERCENTILE * total)
    running = 0
    for b in range(bins):
        running += counts[b]
        if running >= needed:
            break
    noise = LEARN_FLOOR_DB + (b + 1) * LEARN_BIN_DB
    s = dict(DEFAULTS, **settings)
    threshold = min(0.0, max(-100.0, noise + LEARN_MARGIN_DB + s["hysteresis_db"]))
    return noise, threshold


def run(settings, di, own=None):
    """Returns (gain curve, detector level dB, gap dB, output) as float64 arrays. The output and the
    curve are what the C++ produces in float32: gain cast to float, then float multiply."""
    own = di if own is None else own
    g = Gate(settings)
    n = len(di)
    gain, level, gap = np.zeros(n), np.zeros(n), np.zeros(n)
    for i in range(n):
        gain[i], level[i], gap[i] = g.step(float(di[i]), float(own[i]))
    out = (own.astype(np.float32) * gain.astype(np.float32)).astype(np.float64)
    return gain, level, gap, out


# ---- Signals -----------------------------------------------------------------------------------

def tone(f0, peak_db, seconds, decay_db_per_s, rng, stop_at=None, stop_db_per_s=None, harmonics=20):
    """A plucked-string-like tone: harmonics at 1/k with random phases, harmonic k decaying at
    decay * (1 + 0.15 (k - 1)) dB/s (highs die faster, as on a real string), scaled to peak_db.
    From stop_at on, every harmonic also dies at stop_db_per_s (a muted string)."""
    n = int(seconds * FS)
    t = np.arange(n) / FS
    x = np.zeros(n)
    for k in range(1, harmonics + 1):
        if f0 * k > 0.45 * FS:
            break
        rate = decay_db_per_s * (1.0 + 0.15 * (k - 1))
        x += np.sin(2 * np.pi * f0 * k * t + rng.uniform(0, 2 * np.pi)) / k * 10 ** (-rate * t / 20)
    if stop_at is not None:
        extra = np.where(t > stop_at, -(t - stop_at) * stop_db_per_s, 0.0)
        x *= 10 ** (extra / 20)
    return x * 10 ** (peak_db / 20) / np.max(np.abs(x))


def noise_floor(seconds, rng, hum_db=-66.0, hiss_db=-84.0):
    """A DI noise floor: 60 Hz hum with harmonics (120 Hz -6 dB, 180 Hz -9, 240 Hz -14, 300 Hz -16,
    as single coils and ground loops make, peaking at hum_db) plus Gaussian hiss at hiss_db RMS."""
    n = int(seconds * FS)
    t = np.arange(n) / FS
    hum = np.zeros(n)
    for k, rel in zip(range(1, 6), (0.0, -6.0, -9.0, -14.0, -16.0)):
        hum += 10 ** (rel / 20) * np.sin(2 * np.pi * 60.0 * k * t + 0.7 * k)
    hum *= 10 ** (hum_db / 20) / np.max(np.abs(hum))
    return hum + rng.normal(0.0, 10 ** (hiss_db / 20), n)


def place(base, x, at):
    i = int(at * FS)
    end = min(len(base), i + len(x))
    base[i:end] += x[:end - i]


# ---- Design study ------------------------------------------------------------------------------

def first_release(gain, start):
    """Index where the gain first starts falling at or after `start`, and where it reaches -60 dB."""
    i = start
    while i + 1 < len(gain) and not (gain[i + 1] < gain[i] and gain[i] >= 0.999):
        i += 1
    j = i
    while j < len(gain) and gain[j] > 1e-3:
        j += 1
    return i, j


def study_adaptive():
    print("Adaptive release: a note at -15 dBFS over a -66 dBFS hum floor, threshold -52 dB (learned-ish),")
    print("hysteresis 8 dB, hold 10 ms, release knob 250 ms.")
    print(f"{'case':38s} {'gap at release':>15s} {'release used':>13s} {'stop/decay -> -60 dB':>21s}")
    rng = np.random.default_rng(1)
    settings = dict(threshold_db=-52.0)
    cases = []
    for f0 in (82.41, 196.0, 659.3):
        for rate in (10, 20, 40, 80):
            cases.append((f"natural decay {rate:3d} dB/s, f0 {f0:6.1f}", f0, rate, None, None))
        for stop in (300, 1000, 3000):
            cases.append((f"stop at 0.3 s, {stop:4d} dB/s, f0 {f0:6.1f}", f0, 20, 0.3, stop))
        cases.append((f"palm mute 250 dB/s, f0 {f0:6.1f}", f0, 250, None, None))
    for name, f0, rate, stop_at, stop_rate in cases:
        seconds = 6.0 if stop_at is None and rate <= 20 else 3.0
        x = noise_floor(seconds + 0.5, rng)
        place(x, tone(f0, -15.0, seconds, rate, rng, stop_at, stop_rate), 0.5)
        gain, level, gap, _ = run(settings, x)
        g = Gate(settings)
        # Release start: the first time the gain drops from fully open after the note began.
        i, j = first_release(gain, int(0.55 * FS))
        # The gap the adaptive mapping used: replay to that sample and read gap_peak.
        for k in range(i + 1):
            g.step(float(x[k]), float(x[k]))
        used = adaptive_release_ms(g.gap_peak, 250.0)
        ref = int((0.5 + (stop_at if stop_at is not None else 0.0)) * FS)
        print(f"{name:38s} {g.gap_peak:12.1f} dB {used:10.1f} ms {1000 * (j - ref) / FS:16.0f} ms")


def study_ripple():
    """How much the 10 ms windowed peak ripples on a steady note, for low notes through the
    high-pass: the floor of what d reads during a natural decay."""
    print("\nDetector ripple on steady notes (max - min of L over 0.5 s, after the first 100 ms):")
    rng = np.random.default_rng(2)
    for name, f0 in (("drop A  55.0 Hz", 55.0), ("low E   82.4 Hz", 82.41), ("A      110.0 Hz", 110.0),
                     ("G      196.0 Hz", 196.0)):
        x = tone(f0, -20.0, 0.7, 0.0, rng)
        _, level, gap, _ = run({}, x)
        seg = level[int(0.1 * FS):int(0.6 * FS)]
        gseg = gap[int(0.1 * FS):int(0.6 * FS)]
        print(f"  {name}: L ripples {np.max(seg) - np.min(seg):4.1f} dB, gap reads up to {np.max(gseg):4.1f} dB")
    x = rng.normal(0.0, 10 ** (-80 / 20), int(2 * FS))
    _, level, gap, _ = run({"sidechain_high_pass": True}, x)
    print(f"  Gaussian hiss: L ripples {np.max(level[4800:]) - np.min(level[4800:]):4.1f} dB, "
          f"gap reads up to {np.max(gap[4800:]):4.1f} dB")


def hf_energy_db(y, ref_rms, at, before_ms=2.0, after_ms=40.0, cutoff=4000.0):
    """Peak 1 ms RMS of y above `cutoff` (8th-order Butterworth high-pass) near index `at`, in dB
    relative to ref_rms."""
    sos = butter(8, cutoff, "highpass", fs=FS, output="sos")
    hf = sosfilt(sos, y)
    a, b = at - int(before_ms * 0.001 * FS), at + int(after_ms * 0.001 * FS)
    w = 48
    peak = max(np.sqrt(np.mean(hf[k:k + w] ** 2)) for k in range(a, b - w))
    return 20 * np.log10(max(peak, 1e-12) / ref_rms)


def study_clicks():
    print("\nClicks: a 220 Hz sine at -12 dBFS gated by DI bursts (1 kHz, -20 dBFS, 150 ms on, 250 ms off).")
    print("HF = peak 1 ms RMS above 4 kHz around each transition, relative to the sine's RMS.")
    n = int(1.6 * FS)
    t = np.arange(n) / FS
    audio = 10 ** (-12 / 20) * np.sin(2 * np.pi * 220.0 * t)
    di = np.zeros(n)
    onsets = (0.4, 0.8, 1.2)
    for on in onsets:
        a, b = int(on * FS), int((on + 0.15) * FS)
        di[a:b] = 0.1 * np.sin(2 * np.pi * 1000.0 * (t[a:b] - on))
    ref = np.sqrt(np.mean(audio ** 2))
    for label, s in (("default (adaptive: stops take the 20 ms release)", {}),
                     ("classic, release 20 ms", dict(release_mode="classic", release_ms=20.0)),
                     ("classic, release 5 ms", dict(release_mode="classic", release_ms=5.0))):
        gain, _, _, out = run(dict(s, detector="di"), di, audio)
        hard = audio * (gain > 0.5)
        opens = [int(on * FS) for on in onsets]
        closes = []
        for on in onsets:
            k = int((on + 0.15) * FS)
            while gain[k] >= 0.999:
                k += 1
            closes.append(k)
        o = [hf_energy_db(out, ref, k) for k in opens]
        c = [hf_energy_db(out, ref, k) for k in closes]
        ho = [hf_energy_db(hard, ref, k) for k in opens]
        hc = [hf_energy_db(hard, ref, k) for k in closes]
        print(f"  {label}:")
        print(f"    open  {', '.join(f'{v:6.1f}' for v in o)} dB   (hard gate {', '.join(f'{v:6.1f}' for v in ho)})")
        print(f"    close {', '.join(f'{v:6.1f}' for v in c)} dB   (hard gate {', '.join(f'{v:6.1f}' for v in hc)})")


def study_legato():
    print("\nLegato after Learn: DI floor = hum (-66 dBFS peak, harmonics) + hiss (-84 dBFS RMS).")
    rng = np.random.default_rng(5)
    floor = noise_floor(2.2, rng)
    noise, threshold = learn({}, floor)
    print(f"  Learn: noise floor {noise:.2f} dBFS -> threshold {threshold:.2f} dB (close {threshold - 8:.2f})")
    x = noise_floor(4.0, rng)
    t0 = 0.3
    for k in range(4):  # picked 16ths at -12 dBFS
        place(x, tone(110.0 * (1 + 0.06 * k), -12.0, 0.125, 30.0, rng), t0 + 0.125 * k)
    for k in range(10):  # hammer-ons and taps, 15 to 20 dB quieter, soft 3 ms onsets
        note = tone(220.0 * 2 ** (k / 12), -27.0 - 5.0 * (k % 2), 0.125, 35.0, rng)
        note[:144] *= np.linspace(0.0, 1.0, 144)
        place(x, note, t0 + 0.5 + 0.125 * k)
    ring = tone(392.0, -27.0, 1.6, 35.0, rng)  # a last tapped note left ringing (not part of the check)
    ring[:144] *= np.linspace(0.0, 1.0, 144)
    place(x, ring, t0 + 0.5 + 1.25)
    gain, level, _, _ = run(dict(threshold_db=threshold), x)
    a, b = int((t0 + 0.5) * FS), int((t0 + 0.5 + 1.25) * FS)
    print(f"  legato section: min gain {20 * np.log10(np.min(gain[a:b])):.2f} dB, min detector {np.min(level[a:b]):.1f} dB "
          f"({np.min(level[a:b]) - (threshold - 8):.1f} dB above close)")
    tight = run(dict(threshold_db=-30.0, hysteresis_db=2.0, hold_ms=0.0, release_mode="classic", release_ms=20.0), x)[0]
    print(f"  a by-ear 'tight metal' setting (-30 dB, 2 dB hysteresis, no hold, classic 20 ms): min gain in the legato "
          f"{20 * np.log10(max(np.min(tight[a:b]), 1e-9)):.1f} dB")


# ---- Golden renders ----------------------------------------------------------------------------

CASES = {
    "default": dict(),
    "classic_own_input": dict(threshold_db=-40.0, hysteresis_db=4.0, hold_ms=30.0, attack_ms=2.0, release_ms=120.0,
                              release_mode="classic", range_db=-30.0, detector="own", sidechain_hz=150.0),
    "adaptive_no_highpass": dict(threshold_db=-50.0, hysteresis_db=12.0, hold_ms=0.0, attack_ms=0.25, release_ms=600.0,
                                 range_db=-60.0, sidechain_high_pass=False),
}


def golden_signals():
    """3.5 s of DI: a hum-and-hiss floor; picked notes, a hard stop, legato notes 18 dB quieter, a natural
    decay into the floor, a 150 dB/s decay (the adaptive mapping's middle), palm-muted chugs. The gate's
    own input is a crude amp: the DI driven hard into tanh, so own-input detection sees something
    different from the DI."""
    rng = np.random.default_rng(11)
    di = noise_floor(3.5, rng)
    for at, f0 in ((0.30, 82.41), (0.45, 110.0), (0.60, 82.41)):
        place(di, tone(f0, -14.0, 0.15, 30.0, rng), at)
    place(di, tone(146.8, -12.0, 0.4, 20.0, rng, stop_at=0.25, stop_db_per_s=1500.0), 0.75)
    for k, f0 in enumerate((196.0, 220.0, 246.9, 261.6, 293.7)):
        place(di, tone(f0, -12.0 if k == 0 else -30.0, 0.12, 25.0, rng), 1.25 + 0.12 * k)
    place(di, tone(329.6, -28.0, 1.2, 45.0, rng), 1.85)
    place(di, tone(220.0, -20.0, 0.5, 150.0, rng), 2.4)
    for k in range(4):
        place(di, tone(82.41, -16.0, 0.09, 250.0, rng), 3.05 + 0.1 * k)
    di = di.astype(np.float32)
    own = (np.tanh(30.0 * di.astype(np.float64)) * 0.3).astype(np.float32)
    return di, own


def write_golden(folder):
    os.makedirs(folder, exist_ok=True)
    di, own = golden_signals()
    wavfile.write(os.path.join(folder, "input_di.wav"), int(FS), di)
    wavfile.write(os.path.join(folder, "input_own.wav"), int(FS), own)
    for name, settings in CASES.items():
        # The output is the own input times this curve, both in float32, so the curve is all that's stored.
        gain, _, gap, _ = run(settings, di.astype(np.float64), own.astype(np.float64))
        wavfile.write(os.path.join(folder, f"expected_gain_{name}.wav"), int(FS), gain.astype(np.float32))
        closed = np.mean(gain < 0.5)
        print(f"{name}: gain below -6 dB for {100 * closed:.0f}% of the 3.5 s, "
              f"min gain {20 * np.log10(max(np.min(gain), 1e-12)):.1f} dB, largest gap {np.max(gap):.1f} dB")

    # Learn on 2.1 s of the floor alone.
    noise = noise_floor(2.1, np.random.default_rng(12)).astype(np.float32)
    wavfile.write(os.path.join(folder, "input_noise.wav"), int(FS), noise)
    floor_db, threshold = learn({}, noise.astype(np.float64))
    print(f"learn: noise floor {floor_db:.2f} dBFS -> threshold {threshold:.2f} dB")

    cases = {k: dict(DEFAULTS, **v) for k, v in CASES.items()}
    cases["learn"] = dict(DEFAULTS, expected_noise_floor_db=floor_db, expected_threshold_db=threshold)
    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump(cases, f, indent=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", help="write the golden renders into this folder")
    parser.add_argument("--study", action="store_true", help="print the design study")
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    if args.study or not args.golden:
        study_ripple()
        study_adaptive()
        study_clicks()
        study_legato()

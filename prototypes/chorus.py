"""
Chorus reference (BUILD_PLAN "Chorus" and "Shared modulated-delay engine").

Two jobs:

1. The golden reference. The same modulated delay as src/dsp/ModulatedDelay.cpp and the same chorus as
   src/dsp/Chorus.cpp, sample by sample, with the same LFO formulas, the same Hermite form, and the same
   TPT SVF steps, in double precision except where the C++ rounds to float (the LFO's output, the delay
   line's contents, the fractional read position, the engine's output). The C++ has to match these renders
   within -100 dB: the golden tests in tests/ChorusTests.cpp. Settings are held steady (the C++ starts each
   case from prepare(), which jumps every ramp to its target), analog character off except one case without
   noise, and only the deterministic LFO shapes (sine, triangle).

   The engine, per voice v and sample n (read before write, so the newest stored sample is x[n-1]):
       m_v      = LFO_v(phase), rounded to float; the phase advances by rate / fs and wraps at 1
       d_v      = clamp(base_v + depth_v * (+-m_v), 2, max)                    in samples
       tap_v    = Hermite read of the line at d_v - 1 samples back from the newest entry
       wet[n]   = sum_v level_v * tap_v
       line    <- x[n] + tanh(feedback * wet[n])                               (feedback optional)
   Doppler: f_out = f (1 - d'(t)); a triangle of depth A (s) at rate r holds f (1 -+ 4 r A), a sine peaks
   at f (1 -+ 2 pi r A).

   The chorus, per channel (Chorus.h has the reasons):
       high      = 4th-order Butterworth high-pass of x (two SVF sections)  when the wet high-pass is on
       low       = 2nd-order Butterworth low-pass of x at the same frequency
       line in   = high, through tanh when analog is on
       wet       = the mode's voices: Classic one per side (right inverted); Dimension the antiphase pair's
                   difference (A - B)/sqrt(2), opposite on the right; Tri three voices 120 degrees apart
                   panned left, centre, right. Then the 7 kHz low-pass when analog is on
       mid, side = (wetL + wetR) / 2, (wetL - wetR) / 2 * width
       out       = cos(mix pi/2) x + (1 - cos(mix pi/2)) low + sin(mix pi/2) (mid +- side)
                   (equal power: the chorused band is uncorrelated with the dry, so its level holds at
                   every mix, while the protected lows stay at exactly unity)

2. The design study behind the choices in Chorus.h (vectorized, with scipy's Butterworth filters standing
   in for the SVFs): how the lows are split off, how Tri and Dimension lay their voices out in stereo, the
   mono sum, and the mix law.

Usage:
  uv run --with numpy --with scipy python prototypes/chorus.py --golden tests/fixtures/chorus
  uv run --with numpy --with scipy python prototypes/chorus.py --study
"""

import argparse
import json
import math
import os

import numpy as np
from scipy import signal
from scipy.io import wavfile

FS = 48000.0
TWO_PI = 2.0 * math.pi  # juce::MathConstants<double>::twoPi
BUTTERWORTH_Q = 0.70710678118654752


def f32(v):
    """Round to float, as the C++ does at the same points."""
    return float(np.float32(v))


# ---- The engine (ModulatedDelay.cpp) ------------------------------------------------------------

def hermite(xm1, x0, x1, x2, t):
    """DelayLine.h's 4-point Hermite (Catmull-Rom), Niemitalo's form."""
    c1 = 0.5 * (x1 - xm1)
    c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2
    c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1)
    return ((c3 * t + c2) * t + c1) * t + x0


class DelayLine:
    def __init__(self, max_delay):
        size = 1
        while size < max_delay + 4:
            size <<= 1
        self.buf = [0.0] * size
        self.mask = size - 1
        self.w = 0
        self.max_delay = max_delay

    def write(self, x):
        self.w = (self.w + 1) & self.mask
        self.buf[self.w] = f32(x)

    def read_integer(self, k):
        return self.buf[(self.w - k) & self.mask]

    def read(self, delay):
        d = min(max(delay, 0.0), float(self.max_delay))
        whole = int(d)
        t = f32(d - whole)
        x0 = self.read_integer(whole)
        x1 = self.read_integer(whole + 1)
        xm1 = self.read_integer(whole - 1) if whole > 0 else x0
        x2 = self.read_integer(whole + 2)
        return hermite(xm1, x0, x1, x2, t)


class Lfo:
    """Lfo.h for the sine and triangle shapes."""

    def __init__(self, shape, rate, phase):
        self.shape = shape
        self.increment = rate / FS
        self.phase = phase - math.floor(phase)

    def next(self):
        p = self.phase
        if self.shape == "triangle":
            v = 4.0 * p if p < 0.25 else (2.0 - 4.0 * p if p < 0.75 else 4.0 * p - 4.0)
        else:
            v = math.sin(TWO_PI * p)
        self.phase += self.increment
        if self.phase >= 1.0:
            self.phase -= math.floor(self.phase)
        return f32(v)


class ModulatedDelay:
    """ModulatedDelay at steady settings: voices = list of dicts (base_ms, depth_ms, shape, rate, phase,
    inverted, level); restarted at phase 0, as reset() does."""

    def __init__(self, voices, feedback=0.0, max_delay_ms=20.0):
        self.max_delay = max(4.0, math.ceil(max_delay_ms * FS / 1000.0))
        self.line = DelayLine(int(self.max_delay))
        self.voices = []
        for v in voices:
            base = min(max(v["base_ms"] * FS / 1000.0, 2.0), self.max_delay)
            depth = min(max(v["depth_ms"] * FS / 1000.0, 0.0), self.max_delay)
            rate = max(1.0e-3, v["rate"])
            self.voices.append(dict(lfo=Lfo(v["shape"], rate, 0.0 + v["phase"]), base=base, depth=depth,
                                    inverted=v["inverted"], level=v["level"]))
        self.feedback = min(max(feedback, -0.99), 0.99)

    def process(self, x):
        wet = 0.0
        for v in self.voices:
            m = v["lfo"].next()
            if v["inverted"]:
                m = -m
            d = min(max(v["base"] + v["depth"] * m, 2.0), self.max_delay)
            wet += v["level"] * self.line.read(d - 1.0)
        value = x
        if self.feedback != 0.0:
            value += math.tanh(self.feedback * wet)
        self.line.write(value)
        return f32(wet)


# ---- Filters (Svf.h) ----------------------------------------------------------------------------

class Svf:
    def __init__(self, kind, fc, q=BUTTERWORTH_Q):
        fc = min(max(fc, 1.0), 0.49 * FS)
        g = math.tan(math.pi * fc / FS)
        self.k = 1.0 / q
        self.a1 = 1.0 / (1.0 + g * (g + self.k))
        self.a2 = g * self.a1
        self.a3 = g * self.a2
        self.m = {"lowpass": (0.0, 0.0, 1.0), "highpass": (1.0, -self.k, -1.0)}[kind]
        self.ic1 = self.ic2 = 0.0

    def __call__(self, v0):
        v3 = v0 - self.ic2
        v1 = self.a1 * self.ic1 + self.a2 * v3
        v2 = self.ic2 + self.a2 * self.ic1 + self.a3 * v3
        self.ic1 = 2.0 * v1 - self.ic1
        self.ic2 = 2.0 * v2 - self.ic2
        m0, m1, m2 = self.m
        return m0 * v0 + m1 * v1 + m2 * v2


# ---- The chorus (Chorus.cpp) --------------------------------------------------------------------

MODES = {  # voices per channel, base delay (ms), maximum depth (ms)
    "classic": (1, 11.0, 4.0),
    "dimension": (1, 7.0, 1.0),
    "tri": (2, 12.0, 3.0),
}
DIMENSION_GAIN = 0.70710678118654752
TRI_SIDE_LEVEL = 0.81649658092772603    # 1/sqrt(1.5)
TRI_CENTRE_LEVEL = 0.57735026918962576  # 1/sqrt(1.5) / sqrt(2)
ANALOG_LOW_PASS_HZ = 7000.0


def high_pass_q(section):
    """Chorus::highPassQ: the 4th-order Butterworth's sections, 1.307 and 0.541."""
    return 1.0 / (2.0 * math.sin((2.0 * section + 1.0) * math.pi / 8.0))


def voice_settings(mode, shape, channel, rate, depth):
    """Chorus::voiceSettings."""
    count, base, max_depth = MODES[mode]
    depth_ms = min(max(depth, 0.0), 1.0) * max_depth
    rate = min(max(rate, 0.05), 10.0)
    voices = []
    for v in range(count):
        if mode == "tri":
            index = channel + v  # voices 0 and 1 on the left, 1 and 2 on the right
            phase, inverted = index / 3.0, False
            level = TRI_CENTRE_LEVEL if index == 1 else TRI_SIDE_LEVEL
        else:
            phase, inverted, level = 0.0, channel == 1, 1.0
        voices.append(dict(base_ms=base, depth_ms=depth_ms, shape=shape, rate=rate, phase=phase, inverted=inverted,
                           level=level))
    return voices


def mix_gains(mix):
    """Equal power, with exact endpoints (the C++ Chorus::mixGains)."""
    if mix <= 0.0:
        return 1.0, 0.0
    if mix >= 1.0:
        return 0.0, 1.0
    return math.cos(mix * math.pi / 2), math.sin(mix * math.pi / 2)


def chorus(x, s):
    """x: (2, samples) float64 holding float32 values. s: settings with the C++ field names."""
    # The C++ settings are floats.
    rate, depth, mix, width, hz = (f32(s[k]) for k in ("rateHz", "depth", "mix", "width", "wetHighPassHz"))
    mode, shape = s["mode"], s["shape"]
    hz = min(max(hz, 40.0), 1000.0)
    split = 1.0 if s["wetHighPass"] else 0.0
    character = 1.0 if s["analog"] else 0.0

    engines = [ModulatedDelay(voice_settings(mode, shape, ch, rate, depth)) for ch in (0, 1)]
    high_pass = [[Svf("highpass", hz, high_pass_q(0)), Svf("highpass", hz, high_pass_q(1))] for _ in (0, 1)]
    low_band = [Svf("lowpass", hz) for _ in (0, 1)]
    low_pass = [Svf("lowpass", ANALOG_LOW_PASS_HZ) for _ in (0, 1)]
    dry_gain, wet_gain = mix_gains(mix)
    y = np.zeros_like(x)

    for n in range(x.shape[1]):
        xs = (x[0, n], x[1, n])
        low = [0.0, 0.0]
        line_in = [xs[0], xs[1]]
        if split > 0.0:
            for ch in (0, 1):
                high = high_pass[ch][1](high_pass[ch][0](xs[ch]))
                line_in[ch] = xs[ch] + split * (high - xs[ch])
                low[ch] = split * low_band[ch](xs[ch])
        if character > 0.0:
            line_in = [v + character * (math.tanh(v) - v) for v in line_in]

        l = engines[0].process(f32(line_in[0]))
        r = engines[1].process(f32(line_in[1]))
        g = 1.0
        if mode == "dimension":
            difference = g * DIMENSION_GAIN * (l - r)
            wet = [0.0 + difference, 0.0 - difference]
        else:
            wet = [0.0 + g * l, 0.0 + g * r]

        if character > 0.0:
            wet = [wet[ch] + character * (low_pass[ch](wet[ch]) - wet[ch]) for ch in (0, 1)]

        mid = 0.5 * (wet[0] + wet[1])
        side = 0.5 * (wet[0] - wet[1]) * width
        y[0, n] = f32(dry_gain * xs[0] + (1.0 - dry_gain) * low[0] + wet_gain * (mid + side))
        y[1, n] = f32(dry_gain * xs[1] + (1.0 - dry_gain) * low[1] + wet_gain * (mid - side))
    return y


# ---- Golden renders -----------------------------------------------------------------------------

def test_signal(seconds=1.0, seed=5):
    """1 s of stereo guitar-like material: decaying harmonic plucks from a low E up to a high lead note (so
    both the protected lows and the swept highs are exercised), bright upper partials up to 8 kHz to stress
    the interpolation, and a short noise burst. The right channel is slightly different (detuned, other
    phases), as panned cab mics would be."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = np.zeros((2, n))
    notes = [(0.00, 82.41, -8.0), (0.12, 123.5, -12.0), (0.25, 196.0, -10.0), (0.40, 329.6, -12.0),
             (0.55, 659.3, -14.0), (0.70, 987.8, -16.0), (0.85, 110.0, -9.0)]
    for ch in range(2):
        for start, f, level in notes:
            f *= 1.0 + 0.004 * ch
            env = np.where(t >= start, np.exp(-(t - start) / 0.15), 0.0)
            tone = sum(np.sin(2 * np.pi * f * k * (t - start) + rng.uniform(0, 2 * np.pi)) / k ** 1.2
                       for k in range(1, 12) if f * k < 8000.0)
            out[ch] += 10 ** (level / 20) * env * tone / 2.0
        burst = (t >= 0.62) & (t < 0.66)
        out[ch, burst] += 0.05 * rng.uniform(-1, 1, burst.sum())
    return out


CASES = {
    "classic": dict(mode="classic", shape="triangle", rateHz=1.1, depth=0.7, mix=0.5, width=0.8, analog=False,
                    noise=False, wetHighPass=True, wetHighPassHz=150.0),
    "dimension": dict(mode="dimension", shape="sine", rateHz=0.6, depth=0.9, mix=0.6, width=1.0, analog=False,
                      noise=False, wetHighPass=True, wetHighPassHz=180.0),
    "tri": dict(mode="tri", shape="triangle", rateHz=2.3, depth=0.4, mix=0.45, width=0.6, analog=False,
                noise=False, wetHighPass=False, wetHighPassHz=150.0),
    "tri_analog": dict(mode="tri", shape="sine", rateHz=0.9, depth=0.6, mix=0.5, width=1.0, analog=True,
                       noise=False, wetHighPass=True, wetHighPassHz=150.0),
}

# The bare engine with feedback and three voices of different shapes, rates, offsets, and polarities: the
# flanger-style path the chorus doesn't use.
ENGINE_CASE = dict(feedback=0.6, max_delay_ms=30.0, voices=[
    dict(base_ms=5.0, depth_ms=2.0, shape="sine", rate=0.7, phase=0.1, inverted=False, level=0.6),
    dict(base_ms=9.0, depth_ms=3.0, shape="triangle", rate=1.3, phase=0.6, inverted=True, level=0.5),
    dict(base_ms=14.0, depth_ms=0.5, shape="triangle", rate=4.0, phase=0.25, inverted=False, level=0.3),
])


def write_golden(folder):
    os.makedirs(folder, exist_ok=True)
    x = test_signal().astype(np.float32).astype(np.float64)  # the C++ reads these float32 values
    wavfile.write(os.path.join(folder, "input_stereo.wav"), int(FS), x.T.astype(np.float32))

    for name, settings in CASES.items():
        y = chorus(x, settings)
        wavfile.write(os.path.join(folder, f"expected_{name}.wav"), int(FS), y.T.astype(np.float32))
        print(f"{name}: peak in {20 * np.log10(np.max(np.abs(x))):.1f} dBFS, out {20 * np.log10(np.max(np.abs(y))):.1f} dBFS")

    engine = ModulatedDelay(ENGINE_CASE["voices"], ENGINE_CASE["feedback"], ENGINE_CASE["max_delay_ms"])
    wet = np.array([engine.process(v) for v in x[0]])
    wavfile.write(os.path.join(folder, "expected_engine_feedback.wav"), int(FS), wet.astype(np.float32))
    print(f"engine_feedback: wet peak {20 * np.log10(np.max(np.abs(wet))):.1f} dBFS")

    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump(dict(chorus=CASES, engine=ENGINE_CASE), f, indent=2)


# ---- Design study (vectorized; scipy's Butterworth stands in for the SVFs) -----------------------

def _tri(phase):
    p = phase - np.floor(phase)
    return np.where(p < 0.25, 4 * p, np.where(p < 0.75, 2 - 4 * p, 4 * p - 4))


def _read(x, pos):
    i = np.floor(pos).astype(int)
    t = pos - i
    take = lambda k: x[np.clip(i + k, 0, len(x) - 1)]
    return hermite(take(-1), take(0), take(1), take(2), t)


def _voice(x, phase, inverted, base_ms, depth_ms, rate=0.8, shape="triangle"):
    n = np.arange(len(x))
    ph = n * rate / FS + phase
    m = _tri(ph) if shape == "triangle" else np.sin(2 * np.pi * ph)
    d = (base_ms + depth_ms * (-m if inverted else m)) * FS / 1000.0
    return _read(x, n - d)


def _butter(x, fc, kind, order):
    return signal.sosfilt(signal.butter(order, fc, kind, fs=FS, output="sos"), x)


def _layout(x, mode, mix=0.5, depth=0.5, split="final", layout="final", shape="triangle", rate=0.8):
    """The chorus with candidate crossovers and stereo layouts. "final" is what Chorus.cpp does."""
    _, base, max_depth = MODES[mode]
    depth_ms = depth * max_depth
    if split == "final":  # 24 dB/oct into the lines, 12 dB/oct low band
        high, low = _butter(x, 150.0, "highpass", 4), _butter(x, 150.0, "lowpass", 2)
    elif split == "matched2":  # the 2nd-order Butterworth pair
        high, low = _butter(x, 150.0, "highpass", 2), _butter(x, 150.0, "lowpass", 2)
    elif split == "matched4":  # the 4th-order Butterworth pair
        high, low = _butter(x, 150.0, "highpass", 4), _butter(x, 150.0, "lowpass", 4)
    elif split == "subtract4":  # x minus its 4th-order high-pass
        high = _butter(x, 150.0, "highpass", 4)
        low = x - high
    else:
        high, low = x, np.zeros_like(x)
    voice = lambda phase, inverted: _voice(high, phase, inverted, base, depth_ms, rate=rate, shape=shape)

    if mode == "classic":
        wets = [voice(0.0, False), voice(0.0, True)]
    elif mode == "tri" and layout.startswith("per_channel"):  # three voices in each channel, the right set offset
        offset = 1 / 6 if layout == "per_channel_60" else 0.25
        wets = [sum(voice(v / 3 + ch * offset, False) for v in range(3)) / np.sqrt(3) for ch in (0, 1)]
    elif mode == "tri":  # final: left, centre, right
        v0, v1, v2 = (voice(v / 3, False) for v in range(3))
        wets = [TRI_SIDE_LEVEL * v0 + TRI_CENTRE_LEVEL * v1, TRI_CENTRE_LEVEL * v1 + TRI_SIDE_LEVEL * v2]
    elif layout == "both":  # Dimension: both voices summed in each channel, the right pair 90 degrees on
        wets = [0.5 * (voice(0.25 * ch, False) + voice(0.25 * ch, True)) for ch in (0, 1)]
    elif layout == "juno":  # Dimension: A left, B right
        wets = [voice(0.0, False), voice(0.0, True)]
    elif layout == "sdd320":  # Dimension: own voice minus the other side's, high-passed at 300 Hz
        a, b = voice(0.0, False), voice(0.0, True)
        wets = [DIMENSION_GAIN * (a - _butter(b, 300.0, "highpass", 2)), DIMENSION_GAIN * (b - _butter(a, 300.0, "highpass", 2))]
    else:  # Dimension final: the pure difference
        a, b = voice(0.0, False), voice(0.0, True)
        wets = [DIMENSION_GAIN * (a - b), DIMENSION_GAIN * (b - a)]
    dry_gain, wet_gain = mix_gains(mix)
    return [dry_gain * x + (1 - dry_gain) * low + wet_gain * w for w in wets]


def _ratio_db(y, x):
    f, pyy = signal.welch(y, FS, nperseg=8192)
    _, pxx = signal.welch(x, FS, nperseg=8192)
    return f, 10 * np.log10(pyy / pxx)


def _band_db(f, db, lo, hi):
    sel = (f >= lo) & (f < hi)
    return 10 * np.log10(np.mean(10 ** (db[sel] / 10)))


def _wobble_cents(y, f0):
    """95th percentile of the instantaneous frequency's deviation, in cents, where the tone is audible."""
    z = signal.hilbert(y)
    amplitude = np.abs(z)[1:]
    inst = np.diff(np.unwrap(np.angle(z))) * FS / (2 * np.pi)
    keep = amplitude > 0.3 * np.percentile(amplitude, 99)
    keep[: int(0.3 * FS)] = keep[-int(0.3 * FS):] = False
    return np.percentile(np.abs(1200 * np.log2(np.maximum(inst[keep], 1.0) / f0)), 95)


def _report(x, label, l, r, mix):
    f, left = _ratio_db(l, x)
    _, right = _ratio_db(r, x)
    _, mono = _ratio_db(0.5 * (l + r), x)
    band = (f >= 100) & (f <= 10000)
    loss = mono - 10 * np.log10(0.5 * (10 ** (left / 10) + 10 ** (right / 10)))  # what summing adds
    print(f"  {label:28s} mix {mix}: deepest dip one side {np.min(left[band]):+6.1f} dB, mono sum {np.min(mono[band]):+6.1f} dB, "
          f"summing adds {np.min(loss[band]):+6.1f} dB; mono 2-8 kHz {_band_db(f, mono, 2000, 8000):+.1f} dB")


def study():
    rng = np.random.default_rng(1)
    x = rng.uniform(-0.5, 0.5, int(30 * FS))
    tone = 0.5 * np.sin(2 * np.pi * 1000 * np.arange(int(4 * FS)) / FS)

    print("Low-end protection (Classic): output vs dry around the 150 Hz crossover on white noise, and low notes")
    print("through a strong chorus (rate 2 Hz, depth 50%, mix 50%): their pitch wobble and level:")
    for split in ("subtract4", "matched2", "matched4", "final"):
        for mix in (0.5, 1.0):
            l, _ = _layout(x, "classic", mix=mix, split=split)
            f, db = _ratio_db(l, x)
            bands = [(60, 100), (100, 150), (150, 220), (220, 300), (300, 500), (2000, 8000)]
            print(f"  {split:9s} mix {mix}: " + ", ".join(f"{a}-{b} Hz {_band_db(f, db, a, b):+.1f}" for a, b in bands))
        notes = []
        for f0 in (61.74, 82.41, 110.0):
            note = 0.5 * np.sin(2 * np.pi * f0 * np.arange(int(5 * FS)) / FS)
            y = _layout(note, "classic", split=split, rate=2.0)[0][int(FS):]
            crossings = np.where((y[:-1] < 0) & (y[1:] >= 0))[0]
            t = crossings - y[crossings] / (y[crossings + 1] - y[crossings])
            wobble = np.max(np.abs(1200 * np.log2(FS / np.diff(t) / f0)))
            notes.append(f"{f0:.0f} Hz +-{wobble:.2f} cents {20 * np.log10(np.std(y) / np.std(note)):+.2f} dB")
        print(f"  {split:9s} low notes: " + ", ".join(notes))

    print("Tri layouts (deepest dips vs dry, 100 Hz - 10 kHz, white noise, defaults):")
    for layout in ("per_channel_60", "per_channel_90", "final"):
        for mix in (0.5, 1.0):
            l, r = _layout(x, "tri", mix=mix, layout=layout)
            _report(x, layout, l, r, mix)

    print("Dimension layouts (plus the pitch wobble of a 1 kHz tone through one side's wet):")
    for layout in ("both", "juno", "sdd320", "final"):
        wobble = _wobble_cents(_layout(tone, "dimension", mix=1.0, split="none", layout=layout)[0], 1000.0)
        print(f"  {layout}: 1 kHz wobble {wobble:.1f} cents")
        for mix in (0.5, 1.0):
            l, r = _layout(x, "dimension", mix=mix, layout=layout)
            _report(x, layout, l, r, mix)

    print("Final design, every mode:")
    for mode in MODES:
        for mix in (0.5, 1.0):
            l, r = _layout(x, mode, mix=mix)
            _report(x, mode, l, r, mix)

    print("Mix law, Classic, level of the chorused band (2-8 kHz) vs dry at mix 50% (dB):")
    l, _ = _layout(x, "classic")
    f, db = _ratio_db(l, x)
    print(f"  linear {_band_db(f, db, 2000, 8000):+.1f} (equal power would be about 0)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", help="write the golden renders into this folder")
    parser.add_argument("--study", action="store_true", help="print the design study")
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    if args.study or not args.golden:
        study()

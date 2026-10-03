# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Phaser reference for Bloom (BUILD_PLAN "Bloom (modulation container)"), centred on the Vibe mode's lamp and
photocell model.

Two jobs:

1. The golden reference. The same phaser as src/dsp/Phaser.cpp, sample by sample, in double precision except
   where the C++ rounds to float (the LFO's value and the output): the C++ must match these renders within
   -100 dB, and its Vibe sweep must match these sweep trajectories and their asymmetry (tests/BloomTests.cpp).
   Settings are held steady (the C++ starts each case from prepare(), which jumps every ramp to its target).

2. The design study behind the Vibe constants (--study): how asymmetric and how deep the sweep is at each
   rate, and where the mismatched stages put their notches.

The phaser, per channel and sample:
    phase      the LFO's position in cycles, shared; the right channel reads it plus the stereo offset
    m          the LFO's value, rounded to float: Modern sine or triangle, Classic triangle, Vibe sine
    corner     each stage's corner frequency fc (where it shifts the phase by 90 degrees), by mode:
                 Modern   fc = low (high/low)^p,  p = 1/2 + depth m / 2       (exponential: even in octaves)
                 Classic  fc = 160 + (1600 - 160) v,  v = 1/2 + depth m / 2   (a JFET's conductance, below)
                 Vibe     fc_i = r_i (400 + (3500 - 400) c),  c from the lamp and photocell, below
    stages     first-order TPT allpasses (Zavalishin, "The Art of VA Filter Design", ch. 3):
                 g = tan(pi fc / fs), G = g / (1 + g)
                 v = (u - s) G, lp = v + s, s <- lp + v, out = 2 lp - u
               so out = a u + b with a = 2G - 1 and b = 2 (1 - G) s, the instantaneous (affine) response
    feedback   u0 = x + fb y, where y is the last stage's output (Modern 0 to 0.9; Classic's later version 0.35;
               Vibe none): a delay-free loop, solved exactly ("zero delay feedback"): chaining the stages' affine
               responses gives y = A u0 + B, so
                   u0 = (x + fb B) / (1 - fb A)
               |A| < 1 and |fb| < 1, so it's always solvable; the loop is the bilinear transform of the analog
               one, stable for |fb| < 1 at any sweep position.
    wet        y sqrt(1 - fb^2): the loop's resonances add power 1 / (1 - fb^2) on average (the Poisson
               kernel's mean, exact when the phase of A is spread evenly), so this keeps the level steady
               while feedback colours the sound
    out        (1 - mix) x + mix wet, linear: the wet is the dry with its phase turned, so they're correlated, and
               at mix 1/2 with no feedback |out| = |cos(phase / 2)|: total notches where the phase is an odd
               multiple of 180 degrees, one per two stages

Classic (Phase 90-style). Each stage's resistor is a JFET in its ohmic region. In the square-law model its
channel conductance is linear in the gate voltage above pinch-off, g_ds = 2 beta (V_gs - V_p) (any
electronics text, e.g. Sedra and Smith's JFET chapter), so its resistance 1 / g_ds is hyperbolic: the
"nonlinear resistance". The stage's corner is proportional to the conductance (plus a fixed resistor's), and
the pedal's triangle LFO moves the gate voltage linearly, so the corner sweeps linearly in Hz: in octaves it
rushes through the bottom of the sweep and lingers at the top, unlike Modern's even exponential sweep. The
range, 160 Hz to 1.6 kHz at the corner (two notches covering 66 Hz to 3.8 kHz), is an estimate, not taken
from a schematic. The two well-known versions differ by one feedback resistor: off is the original (script
logo), on the later (block logo), modelled as a loop gain of 0.35 (also an estimate).

Vibe (Uni-Vibe-style). One incandescent lamp lights four photocells (CdS light-dependent resistors), one per
stage. The stages' capacitors are deliberately mismatched; the ratios here are those of the four values
usually quoted for the original (15 nF, 220 nF, 470 pF, 4.7 nF, not checked against an original
schematic), so the corners spread over 470:1 and the notches fall irregularly: one sweeping the low mids
(about 110 to 810 Hz) and a second appearing near 5 to 7 kHz only while the lamp is dim. The sweep, per
sample with time step dt = 1 / fs:
    drive      d = 0.25 + 0.75 depth (1 + m) / 2      the lamp's voltage: a bias keeps it glowing dimly
    power      p = d^1.55                              tungsten's resistance rises with temperature, so the
                                                       power grows slower than V^2 (lamp rule of thumb:
                                                       P ~ V^1.55, light ~ V^3.4)
    filament   theta += (p - theta)(1 - e^(-dt/25 ms)) the filament's thermal lag: its temperature (and so
                                                       its light) trails the drive and can't follow fast
                                                       swings fully
    light      L = theta^(3.4/1.55)                    light ~ V^3.4 = P^(3.4/1.55) in steady state
    photocell  c* = L^0.8                              a CdS cell's conductance ~ illuminance^gamma, gamma
                                                       about 0.8 (datasheets' log-log slope)
               c += (c* - c)(1 - e^(-dt/tau))          tau = 4 ms while c* > c (more light: the resistance
                                                       drops fast), 60 ms otherwise (in the dark it recovers
                                                       slowly). This asymmetry is the CdS "light memory"
                                                       that opto compressors rely on (fast attack, slow
                                                       release; Vactrol datasheets give turn-on times of a
                                                       few ms and turn-off times of tens). Both time
                                                       constants are estimates to tune by ear
    corners    fc_i = r_i (400 + 3100 c),  r = (1, 15/220, 15000/470, 15/4.7): the 15 nF stage spans 400 Hz
               (a fixed resistor across the photocell sets the floor) to 3.5 kHz
The result rises faster than it falls, more so the faster the LFO (rise takes 43% of a cycle at 1 Hz, 31% at
4 Hz), and its depth shrinks at high rates because the photocell never recovers in the dark: the throb. The
lamp's state starts at its steady state for the LFO's starting value (warm, as in a pedal that's been on).

Usage:
  uv run --with numpy --with scipy python prototypes/vibe.py --golden tests/fixtures/bloom
  uv run --with numpy --with scipy python prototypes/vibe.py --study
"""

import argparse
import json
import math
import os

import numpy as np
from scipy.io import wavfile

FS = 48000.0
TWO_PI = 2.0 * math.pi  # juce::MathConstants<double>::twoPi

MAX_STAGES = 12
MAX_FEEDBACK = 0.9
CLASSIC_LOW_HZ, CLASSIC_HIGH_HZ = 160.0, 1600.0
CLASSIC_BLOCK_FEEDBACK = 0.35
MIN_RANGE_HZ, MAX_RANGE_HZ = 20.0, 20000.0
VIBE_STAGE_RATIOS = (1.0, 15.0 / 220.0, 15000.0 / 470.0, 15.0 / 4.7)
VIBE_DARK_HZ, VIBE_LIGHT_HZ = 400.0, 3500.0
LAMP_BIAS = 0.25
LAMP_POWER_EXPONENT = 1.55
LAMP_LIGHT_EXPONENT = 3.4 / 1.55
PHOTOCELL_GAMMA = 0.8
LAMP_SECONDS = 0.025
PHOTOCELL_ON_SECONDS, PHOTOCELL_OFF_SECONDS = 0.004, 0.060


def f32(v):
    """Round to float, as the C++ does at the same points."""
    return float(np.float32(v))


def lfo_value(shape, phase):
    """Lfo::shapeAt (sine and triangle), rounded to float."""
    phase -= math.floor(phase)
    if shape == "triangle":
        if phase < 0.25:
            v = 4.0 * phase
        elif phase < 0.75:
            v = 2.0 - 4.0 * phase
        else:
            v = 4.0 * phase - 4.0
    else:
        v = math.sin(TWO_PI * phase)
    return f32(v)


def one_pole(seconds):
    return 1.0 - math.exp(-1.0 / (seconds * FS))


# ---- The lamp and photocell (Phaser::LampPhotocell) ------------------------------------------------

class LampPhotocell:
    def __init__(self):
        self.k_thermal = one_pole(LAMP_SECONDS)
        self.k_on = one_pole(PHOTOCELL_ON_SECONDS)
        self.k_off = one_pole(PHOTOCELL_OFF_SECONDS)
        self.theta = 0.0
        self.c = 0.0

    @staticmethod
    def drive(m, depth):
        return min(max(LAMP_BIAS + (1.0 - LAMP_BIAS) * depth * 0.5 * (1.0 + m), 0.0), 1.0)

    def warm_start(self, m, depth):
        self.theta = LampPhotocell.drive(m, depth) ** LAMP_POWER_EXPONENT
        self.c = self.theta ** (LAMP_LIGHT_EXPONENT * PHOTOCELL_GAMMA)

    def next(self, m, depth):
        p = LampPhotocell.drive(m, depth) ** LAMP_POWER_EXPONENT
        self.theta += (p - self.theta) * self.k_thermal
        target = self.theta ** (LAMP_LIGHT_EXPONENT * PHOTOCELL_GAMMA)
        self.c += (target - self.c) * (self.k_on if target > self.c else self.k_off)
        return self.c


def vibe_reference_hz(c):
    """The 15 nF stage's corner for photocell state c."""
    return VIBE_DARK_HZ + (VIBE_LIGHT_HZ - VIBE_DARK_HZ) * c


# ---- The phaser (Phaser.cpp) ----------------------------------------------------------------------

def stage_count(n):
    """Settings::stages snapped to 2, 4, 6, 8, or 12 (the nearest; ties go up)."""
    choices = (2, 4, 6, 8, 12)
    best = choices[0]
    for c in choices:
        if abs(c - n) <= abs(best - n):
            best = c
    return best


class Phaser:
    """The phaser at steady settings, both channels. s: settings with the C++ field names."""

    def __init__(self, s):
        self.mode = s["mode"]
        self.rate = min(max(f32(s["rateHz"]), 0.05), 10.0)
        self.depth = min(max(f32(s["depth"]), 0.0), 1.0)
        self.offset = min(max(f32(s["stereoOffset"]), 0.0), 0.5)
        self.mix = min(max(f32(s["mix"]), 0.0), 1.0)
        low = min(max(f32(s["lowHz"]), MIN_RANGE_HZ), MAX_RANGE_HZ)
        high = min(max(f32(s["highHz"]), MIN_RANGE_HZ), MAX_RANGE_HZ)
        self.low, self.high = min(low, high), max(low, high)
        if self.mode == "modern":
            self.stages = stage_count(int(s["stages"]))
            self.shape = s["shape"]
            self.feedback = min(max(f32(s["feedback"]), 0.0), MAX_FEEDBACK)
        elif self.mode == "classic":
            self.stages, self.shape = 4, "triangle"
            self.feedback = CLASSIC_BLOCK_FEEDBACK if s["classicFeedback"] else 0.0
        else:
            self.stages, self.shape, self.feedback = 4, "sine", 0.0
        self.compensation = math.sqrt(1.0 - self.feedback * self.feedback)
        self.state = [[0.0] * MAX_STAGES for _ in (0, 1)]
        self.lamps = [LampPhotocell(), LampPhotocell()]
        self.phase = 0.0
        for ch in (0, 1):
            self.lamps[ch].warm_start(lfo_value(self.shape, self.channel_phase(ch)), self.depth)
        self.corners = [[0.0] * MAX_STAGES for _ in (0, 1)]

    def channel_phase(self, ch):
        p = self.phase + (self.offset if ch == 1 else 0.0)
        return p - math.floor(p)

    def corner_frequencies(self, ch, m):
        if self.mode == "modern":
            p = 0.5 + 0.5 * self.depth * m
            return [self.low * (self.high / self.low) ** p] * self.stages
        if self.mode == "classic":
            v = 0.5 + 0.5 * self.depth * m
            return [CLASSIC_LOW_HZ + (CLASSIC_HIGH_HZ - CLASSIC_LOW_HZ) * v] * 4
        c = self.lamps[ch].next(m, self.depth)
        ref = vibe_reference_hz(c)
        return [ref * r for r in VIBE_STAGE_RATIOS]

    def process_sample(self, ch, x):
        m = lfo_value(self.shape, self.channel_phase(ch))
        fcs = self.corner_frequencies(ch, m)
        G = []
        for fc in fcs:
            fc = min(max(fc, 1.0), 0.49 * FS)
            g = math.tan(math.pi * fc / FS)
            G.append(g / (1.0 + g))
        s = self.state[ch]
        A, B = 1.0, 0.0
        for i, Gi in enumerate(G):
            a = 2.0 * Gi - 1.0
            A = a * A
            B = a * B + 2.0 * (1.0 - Gi) * s[i]
        u = (x + self.feedback * B) / (1.0 - self.feedback * A)
        for i, Gi in enumerate(G):
            v = (u - s[i]) * Gi
            lp = v + s[i]
            s[i] = lp + v
            u = 2.0 * lp - u
        self.corners[ch] = fcs
        wet = u * self.compensation
        return (1.0 - self.mix) * x + self.mix * wet

    def advance(self):
        self.phase += self.rate / FS
        if self.phase >= 1.0:
            self.phase -= math.floor(self.phase)

    def process(self, x):
        """x: (2, samples) float64 holding float32 values."""
        y = np.zeros_like(x)
        for n in range(x.shape[1]):
            y[0, n] = f32(self.process_sample(0, x[0, n]))
            y[1, n] = f32(self.process_sample(1, x[1, n]))
            self.advance()
        return y


def vibe_sweep(rate, depth, seconds):
    """The left channel's 15 nF stage corner, sample by sample, with silence going in."""
    p = Phaser(dict(mode="vibe", stages=4, rateHz=rate, depth=depth, shape="sine", lowHz=100.0, highHz=4000.0,
                    feedback=0.0, classicFeedback=False, stereoOffset=0.0, mix=0.5))
    out = np.zeros(int(seconds * FS))
    for n in range(len(out)):
        p.process_sample(0, 0.0)
        out[n] = p.corners[0][0]
        p.advance()
    return out


def sweep_metrics(hz, rate):
    """The sweep's shape over its last LFO cycle, in octaves (log2 of the corner):
         rise_fraction    the share of the cycle spent rising from the lowest point to the highest
         rise_ms, fall_ms the 10%-to-90% times on the way up and back down
         octaves          the swing, lowest to highest
       A symmetric sweep (a sine) has rise fraction 0.5 and equal rise and fall times."""
    period = FS / rate
    whole = int(round(period))
    cycle = np.log2(hz[-whole:])
    lo_i, hi_i = int(np.argmin(cycle)), int(np.argmax(cycle))
    lo, hi = cycle[lo_i], cycle[hi_i]
    rise_fraction = ((hi_i - lo_i) % whole) / period

    def crossing_time(start, rising, level):
        # From index `start`, walk forward (wrapping) until the trace passes `level`, interpolating linearly.
        for k in range(1, whole):
            i0, i1 = (start + k - 1) % whole, (start + k) % whole
            a, b = cycle[i0], cycle[i1]
            if (rising and a < level <= b) or (not rising and a > level >= b):
                return k - 1 + (level - a) / (b - a)
        return float("nan")

    ten, ninety = lo + 0.1 * (hi - lo), lo + 0.9 * (hi - lo)
    rise = crossing_time(lo_i, True, ninety) - crossing_time(lo_i, True, ten)
    fall = crossing_time(hi_i, False, ten) - crossing_time(hi_i, False, ninety)
    return dict(rise_fraction=rise_fraction, rise_ms=1000.0 * rise / FS, fall_ms=1000.0 * fall / FS,
                octaves=hi - lo, low_hz=2.0 ** lo, high_hz=2.0 ** hi)


# ---- Golden renders -------------------------------------------------------------------------------

def test_signal(seconds=1.0, seed=8):
    """1 s of stereo guitar-like material: decaying harmonic plucks from a low E to a high lead note, upper
    partials to 8 kHz so every notch has something to cut, a noise burst, and a slightly different right
    channel (detuned, other phases), as panned cab mics would give."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = np.zeros((2, n))
    notes = [(0.00, 82.41, -8.0), (0.15, 146.8, -11.0), (0.30, 246.9, -12.0), (0.45, 392.0, -12.0),
             (0.60, 784.0, -15.0), (0.75, 1318.5, -17.0), (0.88, 98.0, -9.0)]
    for ch in range(2):
        for start, f, level in notes:
            f *= 1.0 + 0.003 * ch
            env = np.where(t >= start, np.exp(-(t - start) / 0.18), 0.0)
            tone = sum(np.sin(2 * np.pi * f * k * (t - start) + rng.uniform(0, 2 * np.pi)) / k ** 1.1
                       for k in range(1, 14) if f * k < 9000.0)
            out[ch] += 10 ** (level / 20) * env * tone / 2.0
        burst = (t >= 0.52) & (t < 0.56)
        out[ch, burst] += 0.05 * rng.uniform(-1, 1, burst.sum())
    return out


BASE = dict(mode="modern", stages=4, rateHz=0.5, depth=1.0, shape="sine", lowHz=100.0, highHz=4000.0, feedback=0.0,
            classicFeedback=False, stereoOffset=0.25, mix=0.5)
CASES = {
    "vibe": dict(BASE, mode="vibe", rateHz=1.3, depth=0.9, stereoOffset=0.25, mix=0.5),
    "vibe_fast_wet": dict(BASE, mode="vibe", rateHz=5.0, depth=1.0, stereoOffset=0.5, mix=1.0),
    "classic_block": dict(BASE, mode="classic", rateHz=0.9, depth=1.0, classicFeedback=True, stereoOffset=0.0, mix=0.5),
    "modern_6_triangle": dict(BASE, mode="modern", stages=6, rateHz=2.1, depth=0.8, shape="triangle", lowHz=150.0,
                              highHz=3000.0, feedback=0.6, stereoOffset=0.3, mix=0.6),
    "modern_12_resonant": dict(BASE, mode="modern", stages=12, rateHz=0.4, depth=1.0, shape="sine", lowHz=80.0,
                               highHz=5000.0, feedback=0.85, stereoOffset=0.1, mix=0.5),
}
SWEEPS = {"vibe_sweep_1hz": (1.0, 1.0, 4.0), "vibe_sweep_4hz": (4.0, 1.0, 3.0)}


def write_golden(folder):
    os.makedirs(folder, exist_ok=True)
    x = test_signal().astype(np.float32).astype(np.float64)  # the C++ reads these float32 values
    wavfile.write(os.path.join(folder, "phaser_input.wav"), int(FS), x.T.astype(np.float32))

    for name, settings in CASES.items():
        y = Phaser(settings).process(x)
        wavfile.write(os.path.join(folder, f"expected_phaser_{name}.wav"), int(FS), y.T.astype(np.float32))
        print(f"{name}: peak in {20 * np.log10(np.max(np.abs(x))):.1f} dBFS, out {20 * np.log10(np.max(np.abs(y))):.1f} dBFS")

    sweeps = {}
    for name, (rate, depth, seconds) in SWEEPS.items():
        hz = vibe_sweep(rate, depth, seconds)
        # Stored in kHz as float32 (relative precision 6e-8, about 1e-4 cents).
        wavfile.write(os.path.join(folder, f"expected_{name}.wav"), int(FS), (hz / 1000.0).astype(np.float32))
        metrics = sweep_metrics(hz, rate)
        sweeps[name] = dict(rateHz=rate, depth=depth, seconds=seconds, metrics=metrics)
        print(f"{name}: " + ", ".join(f"{k} {v:.4f}" for k, v in metrics.items()))

    with open(os.path.join(folder, "phaser_cases.json"), "w") as f:
        json.dump(dict(phaser=CASES, sweeps=sweeps), f, indent=2)


# ---- Design study ---------------------------------------------------------------------------------

def phase_response(f, corners):
    total = 0.0
    for fc in corners:
        fc = min(max(fc, 1.0), 0.49 * FS)
        g = math.tan(math.pi * fc / FS)
        total -= 2.0 * math.atan(math.tan(math.pi * f / FS) / g)
    return total


def notches(corners):
    """Frequencies where the chain's phase passes an odd multiple of -180 degrees (total notches at mix 1/2)."""
    grid = np.geomspace(5.0, 0.4999 * FS, 40000)
    ph = np.array([phase_response(f, corners) for f in grid])
    found = []
    for k in range(len(corners)):
        target = -(2 * k + 1) * math.pi
        idx = np.where((ph[:-1] > target) & (ph[1:] <= target))[0]
        if len(idx):
            found.append(float(grid[idx[0]]))
    return found


def study():
    print("Vibe sweep (15 nF stage) at full depth, by LFO rate:")
    print("  rate    rise share  10-90 rise  10-90 fall   range")
    for rate in (0.5, 1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 10.0):
        m = sweep_metrics(vibe_sweep(rate, 1.0, max(3.0, 8.0 / rate)), rate)
        print(f"  {rate:4.1f} Hz   {m['rise_fraction']:.3f}     {m['rise_ms']:7.1f} ms  {m['fall_ms']:7.1f} ms   "
              f"{m['low_hz']:6.0f} to {m['high_hz']:6.0f} Hz ({m['octaves']:.2f} octaves)")

    print("\nWhat each part contributes at 4 Hz (rise share; 0.5 = symmetric):")
    global PHOTOCELL_ON_SECONDS, PHOTOCELL_OFF_SECONDS, LAMP_SECONDS
    saved = (PHOTOCELL_ON_SECONDS, PHOTOCELL_OFF_SECONDS, LAMP_SECONDS)
    variants = {
        "full model": saved,
        "symmetric photocell (both 4 ms)": (0.004, 0.004, saved[2]),
        "symmetric photocell (both 60 ms)": (0.060, 0.060, saved[2]),
        "no filament lag (1 ms)": (saved[0], saved[1], 0.001),
    }
    for label, (on, off, lamp) in variants.items():
        PHOTOCELL_ON_SECONDS, PHOTOCELL_OFF_SECONDS, LAMP_SECONDS = on, off, lamp
        m = sweep_metrics(vibe_sweep(4.0, 1.0, 3.0), 4.0)
        print(f"  {label:34s} rise share {m['rise_fraction']:.3f}, range {m['octaves']:.2f} octaves")
    PHOTOCELL_ON_SECONDS, PHOTOCELL_OFF_SECONDS, LAMP_SECONDS = saved

    print("\nVibe notches (mix 1/2) as the photocell brightens, stages at r_i x the 15 nF corner:")
    for ref in (400.0, 600.0, 1000.0, 1600.0, 2500.0, 3500.0):
        corners = [ref * r for r in VIBE_STAGE_RATIOS]
        print(f"  15 nF stage at {ref:6.0f} Hz: notches at " + ", ".join(f"{n:.0f}" for n in notches(corners)) + " Hz")

    print("\nClassic notches (4 matched stages) across its corner range:")
    for fc in (CLASSIC_LOW_HZ, 500.0, 1000.0, CLASSIC_HIGH_HZ):
        print(f"  corner {fc:6.0f} Hz: notches at " + ", ".join(f"{n:.0f}" for n in notches([fc] * 4)) + " Hz")

    print("\nClassic vs Modern over one sweep: share of the cycle the corner spends in the top octave of its range")
    for name, fn in (("Classic (linear in Hz)", lambda v: CLASSIC_LOW_HZ + (CLASSIC_HIGH_HZ - CLASSIC_LOW_HZ) * v),
                     ("Modern (exponential)", lambda v: CLASSIC_LOW_HZ * (CLASSIC_HIGH_HZ / CLASSIC_LOW_HZ) ** v)):
        v = 0.5 + 0.5 * np.array([lfo_value("triangle", k / 1000.0) for k in range(1000)])
        f = np.array([fn(x) for x in v])
        print(f"  {name:24s} {np.mean(f >= CLASSIC_HIGH_HZ / 2.0):.2f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--golden", metavar="FOLDER", help="write the golden fixtures")
    parser.add_argument("--study", action="store_true", help="print the design study")
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    if args.study:
        study()
    if not args.golden and not args.study:
        parser.print_help()

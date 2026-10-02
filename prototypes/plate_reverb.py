"""
Dattorro plate reverberator, written straight from the paper, sample by sample, in double precision:

  J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES 45(9), 1997.
  Fig. 1 (the topology and every delay length), Table 1 (the parameters), Table 2 (the output taps).

The C++ engine (DattorroPlate in src/dsp/Reverb.cpp) has to match these renders: the golden tests in
tests/ReverbTests.cpp. This file is the reference, so it follows the figure node by node rather than
being ported from the C++.

The network (Fig. 1, "in the style of Griesinger"):

    x = (xL + xR) / 2 -> predelay -> bandwidth: y = bandwidth x + (1 - bandwidth) y[n-1]
      -> four input diffusers: lattices 142, 107 (input diffusion 1) and 379, 277 (input diffusion 2)
      -> the tank, a figure eight. Each half takes the diffused input plus decay x the other half's output:
         left:  lattice 672 + EXCURSION (decay diffusion 1, signs reversed, tap modulated)
                -> z^-4453 -> damping: y = (1 - damping) x + damping y[n-1] -> x decay
                -> lattice 1800 (decay diffusion 2) -> z^-3720 -> x decay -> right half's input
         right: lattice 908 + EXCURSION -> z^-4217 -> damping -> x decay -> lattice 2656 -> z^-3163
                -> x decay -> left half's input
    yL, yR: Table 2, seven taps each at +-0.6, all wet.

The lattice (two-multiplier, all-pass), as drawn in Fig. 1:
    w[n] = x[n] - g w[n - N]        top summing node (the minus sign), feeds the delay line
    y[n] = w[n - N] + g w[n]        bottom summing node
    => Y/X = (g + z^-N) / (1 + g z^-N)
The two "decay diffusion 1" lattices are marked "note sign": the minus moves to the bottom node,
w = x + g w[n - N], y = w[n - N] - g w, i.e. the same all-pass with -g (section 1.3.3).

Lengths are samples at 29761 Hz (Table 1). Everything is rendered at 48 kHz with every length and tap
scaled by 48000 / 29761 and rounded to the nearest sample. Delay modulation (section 1.3.7): the taps at
nodes 24 and 48 move as N + EXCURSION x sin, with a quadrature pair of sine LFOs (left at phase 0, right
a quarter cycle ahead), read with 4-point Hermite interpolation (the project's choice for every
modulated delay; the paper suggests linear or all-pass interpolation). EXCURSION = 16 is Table 1's peak.

Two additions of the product (src/dsp/Reverb.cpp), both off at the paper's settings, rendered by the
"block" case only:
  - a 300 Hz low shelf and a 4 kHz high shelf (TPT SVFs, Q 0.707, as in src/dsp/Svf.h) after each
    damping filter, for the low and high decay multipliers; at 0 dB they pass the signal through exactly;
  - the mapping from the block's knobs (T60 and multipliers) to decay and the shelves. The block keeps
    the damping one-pole at 0 (Table 1's "no damping"): fitted to the high band it would cut the mid
    band too.

Usage:
  uv run --with numpy --with scipy python prototypes/plate_reverb.py --golden tests/fixtures/reverb
  uv run --with numpy --with scipy python prototypes/plate_reverb.py          # print lengths and T60
"""

import argparse
import json
import math
import os

import numpy as np
from scipy.io import wavfile

PAPER_FS = 29761.0
FS = 48000.0
LOW_CROSSOVER_HZ = 300.0
HIGH_CROSSOVER_HZ = 4000.0
BUTTERWORTH_Q = 0.7071067811865476

# Table 1.
TABLE_1 = dict(decay=0.50, decay_diffusion_1=0.70, input_diffusion_1=0.750, input_diffusion_2=0.625,
               bandwidth=0.9995, damping=0.0005, excursion=16.0)

# Fig. 1 lengths (samples at 29761 Hz).
INPUT_DIFFUSERS = [142, 107, 379, 277]
LEFT = dict(allpass1=672, delay1=4453, allpass2=1800, delay2=3720)
RIGHT = dict(allpass1=908, delay1=4217, allpass2=2656, delay2=3163)

# Table 2: (sign, line, tap) in the table's order. Line names are Fig. 1's node pairs.
TAPS_LEFT = [(+1, "48_54", 266), (+1, "48_54", 2974), (-1, "55_59", 1913), (+1, "59_63", 1996),
             (-1, "24_30", 1990), (-1, "31_33", 187), (-1, "33_39", 1066)]
TAPS_RIGHT = [(+1, "24_30", 353), (+1, "24_30", 3627), (-1, "31_33", 1228), (+1, "33_39", 2673),
              (-1, "48_54", 2111), (-1, "55_59", 335), (-1, "59_63", 121)]


def scaled(n):
    """A length from the paper (samples at 29761 Hz) at 48 kHz, rounded to the nearest sample."""
    return int(math.floor(n * FS / PAPER_FS + 0.5))


def decay_diffusion_2(decay):
    """Table 1: decay diffusion 2 = decay + 0.15, floor 0.25, ceiling 0.50."""
    return min(0.50, max(0.25, decay + 0.15))


class Line:
    """A delay line. push() writes the newest sample; tap(k) is the sample pushed k pushes ago, so right
    after pushing x[n], tap(k) = x[n - k]."""

    def __init__(self, size):
        self.buf = [0.0] * size
        self.size = size
        self.w = 0

    def push(self, x):
        self.w = (self.w + 1) % self.size
        self.buf[self.w] = x

    def tap(self, k):
        return self.buf[(self.w - k) % self.size]

    def hermite(self, d):
        """The signal d pushes ago, between whole pushes by 4-point Hermite (Catmull-Rom) interpolation,
        in Olli Niemitalo's form: c1 = (x1 - xm1)/2, c2 = xm1 - 5/2 x0 + 2 x1 - x2/2,
        c3 = (x2 - xm1)/2 + 3/2 (x0 - x1), y = ((c3 t + c2) t + c1) t + x0. Going back in time, x0 is d's
        whole part ago, x1 one older, xm1 one newer."""
        whole = int(math.floor(d))
        t = d - whole
        x0 = self.tap(whole)
        x1 = self.tap(whole + 1)
        xm1 = self.tap(whole - 1) if whole > 0 else x0
        x2 = self.tap(whole + 2)
        c1 = 0.5 * (x1 - xm1)
        c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2
        c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1)
        return ((c3 * t + c2) * t + c1) * t + x0


def lattice(line, n, x, g):
    """Fig. 1's lattice. The line holds w up to the previous sample, so w[n - N] is N - 1 pushes ago."""
    d = line.tap(n - 1)
    w = x - g * d
    line.push(w)
    return d + g * w


def lattice_reversed(line, delay, x, g):
    """The "note sign" lattice (decay diffusion 1): signs reversed, fractional, modulated delay."""
    d = line.hermite(delay - 1.0)
    w = x + g * d
    line.push(w)
    return d - g * w


class SineLfo:
    """sin(2 pi phase), phase advancing rate / fs per sample and wrapping at 1. Output rounded to float32,
    as the product's LFO (src/dsp/Lfo.h) returns float."""

    def __init__(self, rate_hz, phase):
        self.increment = rate_hz / FS
        self.phase = phase

    def next(self):
        value = float(np.float32(math.sin(2.0 * math.pi * (self.phase - math.floor(self.phase)))))
        self.phase += self.increment
        if self.phase >= 1.0:
            self.phase -= math.floor(self.phase)
        return value


class SvfShelf:
    """Andrew Simper's trapezoidal SVF as a shelf, the same designs as Svf::design in src/dsp/Svf.h,
    A = 10^(dB/40):
      low shelf (gain_db below fc):  poles at fc / sqrt(A), m = (1, k (A - 1), A^2 - 1)
      high shelf (gain_db above fc): poles at fc sqrt(A),   m = (A^2, k (1 - A) A, 1 - A^2)"""

    def __init__(self, kind, fc, gain_db, q=BUTTERWORTH_Q):
        fc = min(max(fc, 1.0), 0.49 * FS)
        a = 10 ** (gain_db / 40.0)
        self.k = 1.0 / q
        g = math.tan(math.pi * fc / FS)
        if kind == "low":
            g /= math.sqrt(a)
            self.m0, self.m1, self.m2 = 1.0, self.k * (a - 1.0), a * a - 1.0
        else:
            g *= math.sqrt(a)
            self.m0, self.m1, self.m2 = a * a, self.k * (1.0 - a) * a, 1.0 - a * a
        self.a1 = 1.0 / (1.0 + g * (g + self.k))
        self.a2 = g * self.a1
        self.a3 = g * self.a2
        self.ic1 = self.ic2 = 0.0

    def __call__(self, v0):
        v3 = v0 - self.ic2
        v1 = self.a1 * self.ic1 + self.a2 * v3
        v2 = self.ic2 + self.a2 * self.ic1 + self.a3 * v3
        self.ic1 = 2.0 * v1 - self.ic1
        self.ic2 = 2.0 * v2 - self.ic2
        return self.m0 * v0 + self.m1 * v1 + self.m2 * v2


def loop_seconds():
    """One trip around the figure eight: all eight tank lengths (at 48 kHz, rounded)."""
    return sum(scaled(n) for half in (LEFT, RIGHT) for n in half.values()) / FS


def plate(x, p, mod_rate_hz=1.0, low_shelf_db=0.0, high_shelf_db=0.0):
    """Render the plate. x: the mono input (already (xL + xR) / 2 and predelayed), float64.
    p: Table 1 names. Returns (yL, yR)."""
    n_in = [scaled(n) for n in INPUT_DIFFUSERS]
    nl = {k: scaled(v) for k, v in LEFT.items()}
    nr = {k: scaled(v) for k, v in RIGHT.items()}
    excursion = p["excursion"] * FS / PAPER_FS  # Table 1 counts samples at 29761 Hz
    room = int(math.ceil(16.0 * FS / PAPER_FS)) + 8

    inputs = [Line(n + 8) for n in n_in]
    lines = {
        # node pairs of Fig. 1
        "23_24": Line(nl["allpass1"] + room), "24_30": Line(nl["delay1"] + 8),
        "31_33": Line(nl["allpass2"] + 8), "33_39": Line(nl["delay2"] + 8),
        "46_48": Line(nr["allpass1"] + room), "48_54": Line(nr["delay1"] + 8),
        "55_59": Line(nr["allpass2"] + 8), "59_63": Line(nr["delay2"] + 8),
    }
    taps_left = [(s, name, scaled(k)) for s, name, k in TAPS_LEFT]
    taps_right = [(s, name, scaled(k)) for s, name, k in TAPS_RIGHT]
    lfo_left, lfo_right = SineLfo(mod_rate_hz, 0.0), SineLfo(mod_rate_hz, 0.25)  # quadrature
    low_left, low_right = SvfShelf("low", LOW_CROSSOVER_HZ, low_shelf_db), SvfShelf("low", LOW_CROSSOVER_HZ, low_shelf_db)
    high_left, high_right = SvfShelf("high", HIGH_CROSSOVER_HZ, high_shelf_db), SvfShelf("high", HIGH_CROSSOVER_HZ, high_shelf_db)

    decay = p["decay"]
    dd1, dd2 = p["decay_diffusion_1"], decay_diffusion_2(decay)
    id1, id2 = p["input_diffusion_1"], p["input_diffusion_2"]
    bandwidth, damping = p["bandwidth"], p["damping"]

    bw_state = damp_left = damp_right = 0.0
    y_left = np.zeros(len(x))
    y_right = np.zeros(len(x))

    for n in range(len(x)):
        mod_left, mod_right = lfo_left.next(), lfo_right.next()

        bw_state = bandwidth * x[n] + (1.0 - bandwidth) * bw_state

        v = lattice(inputs[0], n_in[0], bw_state, id1)
        v = lattice(inputs[1], n_in[1], v, id1)
        v = lattice(inputs[2], n_in[2], v, id2)
        v = lattice(inputs[3], n_in[3], v, id2)

        # Both halves' inputs come from samples already in the lines (nodes 63 and 39).
        from_right = lines["59_63"].tap(nr["delay2"] - 1)
        from_left = lines["33_39"].tap(nl["delay2"] - 1)

        # Left half.
        u = v + decay * from_right
        u = lattice_reversed(lines["23_24"], nl["allpass1"] + excursion * mod_left, u, dd1)
        delayed = lines["24_30"].tap(nl["delay1"] - 1)  # node 30
        lines["24_30"].push(u)
        damp_left = (1.0 - damping) * delayed + damping * damp_left
        u = decay * high_left(low_left(damp_left))
        u = lattice(lines["31_33"], nl["allpass2"], u, dd2)
        lines["33_39"].push(u)

        # Right half.
        u = v + decay * from_left
        u = lattice_reversed(lines["46_48"], nr["allpass1"] + excursion * mod_right, u, dd1)
        delayed = lines["48_54"].tap(nr["delay1"] - 1)  # node 54
        lines["48_54"].push(u)
        damp_right = (1.0 - damping) * delayed + damping * damp_right
        u = decay * high_right(low_right(damp_right))
        u = lattice(lines["55_59"], nr["allpass2"], u, dd2)
        lines["59_63"].push(u)

        # Table 2, in its order: accumulator = 0.6 x first tap, then += or -= 0.6 x each next one.
        for taps, out in ((taps_left, y_left), (taps_right, y_right)):
            acc = 0.0
            for i, (sign, name, k) in enumerate(taps):
                term = 0.6 * lines[name].tap(k)
                acc = term if i == 0 else (acc + term if sign > 0 else acc - term)
            out[n] = acc
    return y_left, y_right


def block_parameters(s):
    """The product's knob mapping (Reverb::plateParameters in src/dsp/Reverb.cpp), from the settings.
    r = 60 / T60 is the decay rate in dB per second; tau is one trip around the tank, which passes the
    decay multiplier four times and each shelf twice:
        decay^4:     80 log10(decay) = -r_mid tau
        low shelf:   2 x shelf dB = -(r_low - r_mid) tau
        high shelf:  2 x shelf dB = -(r_high - r_mid) tau
    and the damping one-pole off."""
    t60 = min(max(s["decay_seconds"], 0.1), 30.0)
    low = min(max(s["low_multiplier"], 0.25), 4.0)
    high = min(max(s["high_multiplier"], 0.1), 2.0)
    r_mid, r_low, r_high = 60.0 / t60, 60.0 / (t60 * low), 60.0 / (t60 * high)
    tau = loop_seconds()

    p = dict(TABLE_1)
    p["decay"] = 10.0 ** (-r_mid * tau / 80.0)
    low_shelf_db = -(r_low - r_mid) * tau / 2.0
    high_shelf_db = -(r_high - r_mid) * tau / 2.0
    p["damping"] = 0.0
    diffusion = min(max(s["diffusion"], 0.0), 1.0)
    p["input_diffusion_1"] = 0.75 * diffusion
    p["input_diffusion_2"] = 0.625 * diffusion
    p["excursion"] = 16.0 * min(max(s["mod_depth"], 0.0), 1.0)
    return p, low_shelf_db, high_shelf_db


def test_input(seconds=1.0, seed=5):
    """Plucked-like tones and a noise burst: decaying harmonic notes at different levels, then silence for
    the tail. Float32-exact (the C++ reads it from a float32 file)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    x = np.zeros(n)
    for start, f, level in [(0.0, 196.0, -6.0), (0.12, 330.0, -12.0), (0.25, 82.4, -8.0), (0.4, 494.0, -14.0)]:
        env = np.where(t >= start, np.exp(-(t - start) / 0.08), 0.0)
        tone = sum(np.sin(2 * np.pi * f * k * (t - start) + rng.uniform(0, 2 * np.pi)) / k for k in range(1, 7))
        x += 10 ** (level / 20) * env * tone / 1.8
    burst = (t >= 0.5) & (t < 0.52)
    x[burst] += 0.2 * rng.uniform(-1, 1, burst.sum())
    return x.astype(np.float32)


CASES = {
    # The paper exactly: Table 1, modulation off, impulse response.
    "impulse": dict(kind="core", input="impulse", parameters=dict(TABLE_1, excursion=0.0), mod_rate_hz=1.0),
    # Table 1 with the full EXCURSION = 16 at 1 Hz (the moving taps), on a musical input.
    "modulated": dict(kind="core", input="music", parameters=dict(TABLE_1), mod_rate_hz=1.0),
    # The block in plate mode: knob mapping, both shelves, Hermite modulation. All values are exact in float32.
    "block": dict(kind="block", input="music",
                  settings=dict(decay_seconds=3.0, low_multiplier=1.5, high_multiplier=0.375, diffusion=0.875,
                                mod_depth=0.5, mod_rate_hz=0.75)),
}


def write_golden(folder, seconds=1.0):
    os.makedirs(folder, exist_ok=True)
    music = test_input(seconds)
    impulse = np.zeros(int(seconds * FS), dtype=np.float32)
    impulse[0] = 1.0
    wavfile.write(os.path.join(folder, "input_music.wav"), int(FS), music)

    for name, case in CASES.items():
        x = (impulse if case["input"] == "impulse" else music).astype(np.float64)
        if case["kind"] == "core":
            y_left, y_right = plate(x, case["parameters"], case["mod_rate_hz"])
        else:
            p, low_db, high_db = block_parameters(case["settings"])
            y_left, y_right = plate(x, p, case["settings"]["mod_rate_hz"], low_db, high_db)
            case["mapped"] = dict(p, low_shelf_db=low_db, high_shelf_db=high_db)
        out = np.stack([y_left, y_right], axis=1).astype(np.float32)
        wavfile.write(os.path.join(folder, f"expected_{name}.wav"), int(FS), out)
        rms = lambda v: 20 * np.log10(np.sqrt(np.mean(v ** 2)) + 1e-30)
        print(f"{name}: wet RMS L {rms(y_left):.1f} dB, R {rms(y_right):.1f} dB, "
              f"L/R correlation {np.corrcoef(y_left, y_right)[0, 1]:.3f}")

    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump(CASES, f, indent=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", help="write the golden renders into this folder")
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    else:
        print("input diffusers:", [scaled(n) for n in INPUT_DIFFUSERS])
        print("left half:", {k: scaled(v) for k, v in LEFT.items()})
        print("right half:", {k: scaled(v) for k, v in RIGHT.items()})
        print("taps left:", [scaled(k) for _, _, k in TAPS_LEFT], "right:", [scaled(k) for _, _, k in TAPS_RIGHT])
        tau = loop_seconds()
        print(f"loop {tau * 1000:.3f} ms; Table 1's decay 0.5 gives T60 = {-0.75 * tau / math.log10(0.5):.3f} s")

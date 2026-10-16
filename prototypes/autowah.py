# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Auto-wah reference (BUILD_PLAN "Auto-wah"; the walkthrough is docs/guides/AUTOWAH_GUIDE.md).

Written straight from the equations, sample by sample, in double precision, in exactly the order the C++
block (src/dsp/AutoWah.cpp) runs them. The C++ has to match these renders within -100 dB: the golden test in
tests/AutoWahTests.cpp. Settings are held steady here (the C++ starts each case from prepare(), which snaps
every smoother to its target), and are rounded to float32 first, because the C++ Settings are floats.

Per sample n, with d the detector's input (the clean DI in the app; here the input itself) and x the block's
input:

    every INTERVAL samples, before the sample (n % INTERVAL == 0), from the envelope as it stands:
        level_db = 20 log10(e + 1e-9)                          the offset keeps log10 finite in silence
        floor_db = -20 - 0.4 sensitivity_percent                -20 dB at 0 %, -60 dB at 100 %
        c        = clamp((level_db - floor_db) / 30, 0, 1)      a 30 dB span from closed to open
        c        = 1 - c                                        with Direction Down
        fc       = f_low (f_high / f_low)^c                     exponential: c = 0.5 is the geometric mean
        the SVF band-pass redesigned at fc and Q (src/dsp/Svf.h: g = tan(pi fc / fs), k = 1/Q)
        gain     = 10^(13.1 / 20) sqrt(Q / 4)                   the level hold: a band-pass's loss on a guitar goes as 1/sqrt(Q)
    then:
        r   = |d[n]|
        a   = attack coefficient if r > e else release          a = exp(-1 / (tau fs)): 63 % of a step in tau
        e   = a e + (1 - a) r
        wet = gain * SVF band-pass of x[n]                      Simper's trapezoidal SVF, k s / (s^2 + k s + 1)
        y   = x[n] if mix == 0 else (1 - mix) x[n] + mix wet    (mix 0 leaves the dry untouched, bit for bit)

The envelope follower is the branching one-pole of Giannoulis, Massberg and Reiss, "Digital Dynamic Range
Compressor Design: A Tutorial and Analysis", JAES 60(6), 2012 (the compressor's). The filter is Andrew Simper's
"Linear Trapezoidal Integrated State Variable Filter With Low Noise Optimisation" (Cytomic, 2013), as in
Vadim Zavalishin, "The Art of VA Filter Design", chapter 4.

Usage:
  uv run --with numpy --with scipy --with matplotlib python prototypes/autowah.py <di.wav>          # listen + plot
  uv run --with numpy --with scipy --with matplotlib python prototypes/autowah.py <di.wav> --q 8 --direction down
  uv run --with numpy --with scipy python prototypes/autowah.py --golden tests/fixtures/autowah
  uv run --with numpy --with scipy python prototypes/autowah.py --study                         # redesign interval
"""

import argparse
import json
import math
import os

import numpy as np
from scipy.io import wavfile

FS = 48000.0
PI = math.pi           # juce::MathConstants<double>::pi
INTERVAL = 4           # samples between filter redesigns (the interval study, BUILD_PLAN "Auto-wah")
SPAN_DB = 30.0         # from closed to fully open
FLOOR_AT_ZERO_DB = -20.0
FLOOR_PER_PERCENT_DB = 0.4
LEVEL_OFFSET = 1e-9    # keeps log10 finite in digital silence (-180 dB)
MAKEUP_DB = 13.1       # the level hold at the reference Q (AutoWah.h, 6; the gain staging calibration)
REFERENCE_Q = 4.0

DEFAULTS = dict(sensitivity=50.0, freq_low=350.0, freq_high=2200.0, q=4.0, attack_ms=5.0, release_ms=150.0,
                direction="up", mix=1.0)


def f32(v):
    """The C++ Settings hold floats: round a setting the way the C++ sees it."""
    return float(np.float32(v))


def coefficient(ms):
    """The one-pole coefficient for a time constant. A step from 0 to 1 gives 1 - a^n; that reaches 1 - 1/e (63 %)
    after tau seconds, n = tau fs samples, when a^(tau fs) = 1/e, so a = exp(-1 / (tau fs))."""
    return math.exp(-1.0 / (ms * 0.001 * FS))


def control(e, sensitivity, direction):
    """Envelope to a control between 0 (closed) and 1 (open), in dB (guitar dynamics are logarithmic)."""
    level_db = 20.0 * math.log10(e + LEVEL_OFFSET)
    floor_db = FLOOR_AT_ZERO_DB - FLOOR_PER_PERCENT_DB * sensitivity
    c = min(1.0, max(0.0, (level_db - floor_db) / SPAN_DB))
    return 1.0 - c if direction == "down" else c


def wet_gain(q):
    """The level hold: makeup at the reference Q, rising as sqrt(Q) (a band-pass passes power ~ 1/Q of a pink-ish
    guitar)."""
    return math.pow(10.0, MAKEUP_DB / 20.0) * math.sqrt(q / REFERENCE_Q)


def centre_frequency(c, low, high):
    """The exponential sweep: equal steps of c are equal musical intervals."""
    return low * math.pow(high / low, c)


class SvfBandpass:
    """Simper's trapezoidal SVF with the band-pass output mix (m0 = 0, m1 = k, m2 = 0): src/dsp/Svf.h exactly."""

    def __init__(self):
        self.ic1 = self.ic2 = 0.0
        self.a1 = 1.0
        self.a2 = self.a3 = 0.0
        self.m0 = self.m1 = self.m2 = 0.0

    def design(self, fc, q):
        fc = min(0.49 * FS, max(1.0, fc))        # Svf::design's clamp (tan blows up at Nyquist)
        g = math.tan(PI * fc / FS)
        k = 1.0 / q
        self.m0, self.m1, self.m2 = 0.0, k, 0.0
        self.a1 = 1.0 / (1.0 + g * (g + k))
        self.a2 = g * self.a1
        self.a3 = g * self.a2

    def __call__(self, v0):
        v3 = v0 - self.ic2
        v1 = self.a1 * self.ic1 + self.a2 * v3
        v2 = self.ic2 + self.a2 * self.ic1 + self.a3 * v3
        self.ic1 = 2.0 * v1 - self.ic1
        self.ic2 = 2.0 * v2 - self.ic2
        return self.m0 * v0 + self.m1 * v1 + self.m2 * v2


def autowah(x, s, interval=INTERVAL, detector=None, trace=False):
    """x: mono float64. s: settings (the names in DEFAULTS). Returns y (and the envelope and fc per sample with
    trace=True)."""
    sensitivity, low, high, q = f32(s["sensitivity"]), f32(s["freq_low"]), f32(s["freq_high"]), f32(s["q"])
    attack, release = coefficient(f32(s["attack_ms"])), coefficient(f32(s["release_ms"]))
    mix, direction = f32(s["mix"]), s["direction"]
    d = x if detector is None else detector
    svf = SvfBandpass()
    e = 0.0
    fc = low
    gain = 1.0
    y = np.zeros_like(x)
    envelope = np.zeros_like(x) if trace else None
    centre = np.zeros_like(x) if trace else None

    for n in range(len(x)):
        if n % interval == 0:
            fc = centre_frequency(control(e, sensitivity, direction), low, high)
            svf.design(fc, q)
            gain = wet_gain(q)
        r = abs(d[n])
        a = attack if r > e else release
        e = a * e + (1.0 - a) * r
        wet = gain * svf(x[n])
        y[n] = x[n] if mix == 0.0 else (1.0 - mix) * x[n] + mix * wet
        if trace:
            envelope[n], centre[n] = e, fc
    return (y, envelope, centre) if trace else y


def test_signal(seconds=2.5, seed=7):
    """Plucked-like notes at very different levels (decaying harmonic tones from -40 to -6 dBFS) with gaps, a fast
    run of picks, and a -80 dBFS noise floor, so the envelope opens, closes, and sits in the noise."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = 10 ** (-80 / 20) * rng.standard_normal(n)
    notes = [(0.00, 110.0, -6.0, 0.25), (0.40, 196.0, -20.0, 0.15), (0.70, 82.4, -40.0, 0.3), (1.00, 330.0, -12.0, 0.2),
             (1.40, 247.0, -30.0, 0.12)]
    notes += [(1.70 + 0.06 * i, 220.0 * 2 ** (i / 12), -10.0 - 3 * (i % 3), 0.05) for i in range(8)]  # a fast run
    for start, f, level, decay in notes:
        env = np.where(t >= start, np.exp(-(t - start) / decay), 0.0)
        tone = sum(np.sin(2 * np.pi * f * k * (t - start) + rng.uniform(0, 2 * np.pi)) / k for k in range(1, 9))
        out += 10 ** (level / 20) * env * tone / 2.0
    return out


CASES = {
    "defaults": dict(DEFAULTS),
    "down": dict(DEFAULTS, direction="down"),
    "high_q_fast": dict(DEFAULTS, q=12.0, attack_ms=1.0, release_ms=60.0),
    "narrow": dict(DEFAULTS, freq_low=600.0, freq_high=900.0, mix=0.5),
    "low_sensitivity": dict(DEFAULTS, sensitivity=10.0, release_ms=400.0, mix=0.75),
}


def write_golden(folder):
    os.makedirs(folder, exist_ok=True)
    x = test_signal()
    wavfile.write(os.path.join(folder, "input_mono.wav"), int(FS), x.astype(np.float32))
    x = x.astype(np.float32).astype(np.float64)  # the C++ reads its input from the float32 file
    for name, settings in CASES.items():
        y, e, fc = autowah(x, settings, trace=True)
        wavfile.write(os.path.join(folder, f"expected_{name}.wav"), int(FS), y.astype(np.float32))
        print(f"{name}: out peak {20 * np.log10(np.max(np.abs(y))):.1f} dBFS, fc {fc.min():.0f} to {fc.max():.0f} Hz")
    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump({"interval": INTERVAL, "cases": CASES}, f, indent=2)


def study():
    """The redesign interval: per-sample redesign (1) is the reference. Stepping (fc held for a chunk, then jumped)
    adds energy above the notes' own top end and larger sample steps, so measure both with the fastest attack on
    picked notes whose harmonics stay below 3 kHz (nothing up there masks it). tests/AutoWahTests.cpp repeats this
    on the C++ block; BUILD_PLAN "Auto-wah" has the numbers and the choice."""
    x = test_signal().astype(np.float32).astype(np.float64)
    f = np.fft.rfftfreq(len(x), 1 / FS)

    def above_6k(y):
        return np.sum(np.abs(np.fft.rfft(y)[f > 6000]) ** 2)

    for q in (4.0, 12.0):
        s = dict(DEFAULTS, attack_ms=1.0, q=q)
        reference = autowah(x, s, interval=1)
        print(f"Q {q:.0f}, attack 1 ms, against redesigning every sample:")
        for interval in (2, 4, 8, 16, 32):
            y = autowah(x, s, interval=interval)
            print(f"  {interval:2d}: energy above 6 kHz {10 * np.log10(above_6k(y) / above_6k(reference)):+.2f} dB, max step "
                  f"x{np.max(np.abs(np.diff(y))) / np.max(np.abs(np.diff(reference))):.4f}, difference "
                  f"{20 * np.log10(np.sqrt(np.mean((y - reference) ** 2) / np.mean(reference ** 2))):.1f} dB")


def listen(path, settings, out_path, plot):
    rate, data = wavfile.read(path)
    if rate != int(FS):
        raise SystemExit(f"{path} is {rate} Hz; the chain runs at 48 kHz only")
    if data.dtype.kind == "i":
        data = data / float(np.iinfo(data.dtype).max)
    x = (data[:, 0] if data.ndim > 1 else data).astype(np.float64)  # the guitar is on the first channel
    y, e, fc = autowah(x, settings, trace=True)
    wavfile.write(out_path, int(FS), y.astype(np.float32))
    print(f"wrote {out_path}: in peak {20 * np.log10(np.max(np.abs(x)) + 1e-12):.1f} dBFS, out peak "
          f"{20 * np.log10(np.max(np.abs(y)) + 1e-12):.1f} dBFS, fc {fc.min():.0f} to {fc.max():.0f} Hz")
    if plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        t = np.arange(len(x)) / FS
        floor = FLOOR_AT_ZERO_DB - FLOOR_PER_PERCENT_DB * settings["sensitivity"]
        fig, (top, bottom) = plt.subplots(2, 1, sharex=True, figsize=(12, 7))
        top.plot(t, 20 * np.log10(np.abs(x) + 1e-9), lw=0.3, color="0.6", label="|input| (dBFS)")
        top.plot(t, 20 * np.log10(e + LEVEL_OFFSET), lw=1.2, label="envelope (dBFS)")
        top.axhline(floor, ls="--", color="tab:red", lw=0.8, label=f"closed below {floor:.0f} dBFS")
        top.axhline(floor + SPAN_DB, ls="--", color="tab:green", lw=0.8, label=f"open above {floor + SPAN_DB:.0f} dBFS")
        top.set_ylim(-90, 0)
        top.set_ylabel("dBFS")
        top.legend(loc="lower right", fontsize=8)
        bottom.semilogy(t, fc, lw=1.2, color="tab:orange")
        bottom.set_ylabel("centre frequency (Hz)")
        bottom.set_xlabel("time (s)")
        fig.suptitle(f"Auto-wah: sensitivity {settings['sensitivity']:.0f} %, {settings['freq_low']:.0f} to "
                     f"{settings['freq_high']:.0f} Hz, Q {settings['q']}, attack {settings['attack_ms']} ms, "
                     f"release {settings['release_ms']} ms, {settings['direction']}")
        png = os.path.splitext(out_path)[0] + ".png"
        fig.tight_layout()
        fig.savefig(png, dpi=120)
        print(f"wrote {png}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("di", nargs="?", help="a 48 kHz DI recording to render (listen mode)")
    parser.add_argument("--out", help="where the render goes (default: <di>_autowah.wav)")
    parser.add_argument("--no-plot", action="store_true", help="skip the envelope and fc plot")
    parser.add_argument("--golden", help="write the golden renders into this folder")
    parser.add_argument("--study", action="store_true", help="print the redesign interval study")
    for key, value in DEFAULTS.items():
        parser.add_argument("--" + key.replace("_", "-"), default=value, type=type(value))
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    if args.study:
        study()
    if args.di:
        chosen = {k: getattr(args, k) for k in DEFAULTS}
        listen(args.di, chosen, args.out or os.path.splitext(args.di)[0] + "_autowah.wav", not args.no_plot)
    if not (args.golden or args.study or args.di):
        parser.print_help()

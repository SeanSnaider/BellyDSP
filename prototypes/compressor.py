# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Compressor reference simulation (BUILD_PLAN "Compressor"): both loops, sample by sample, in double
precision, written straight from the equations rather than ported from the C++. The C++ block
(src/dsp/Compressor.cpp) has to match these renders: the golden tests in tests/CompressorTests.cpp.

Studio, feed-forward (Giannoulis, Massberg, Reiss, JAES 2012, the log-domain design):
    x_L = level of the sidechain-filtered input (peak, or 10 ms RMS), in dB
    g   = x_L - static_curve(x_L)                      gain reduction, dB
    r   = branching one-pole of g (attack when rising, release when falling)
    y   = (1 - mix) x + mix x 10^((makeup - r) / 20)

Pedal, feedback: the detector hears the previous output (before makeup and mix), the attack is
fixed at 2 ms, and the reduction for detected level y_L is (R - 1)(y_L - T) above the knee, which gives
ratio R in steady state.

Auto release: a second, slow smoother (400 ms attack, 1.5 s release) runs alongside a 60 ms release,
and the larger reduction wins.

Usage:
  uv run --with numpy --with scipy python prototypes/compressor.py --golden tests/fixtures/compressor
  uv run --with numpy --with scipy python prototypes/compressor.py            # static curves, printed
"""

import argparse
import json
import math
import os

import numpy as np
from scipy.io import wavfile

FS = 48000.0
MIN_LEVEL = 1e-9


class SvfHighpass:
    """Simper's trapezoidal SVF, high-pass output: the same structure as src/dsp/Svf.h."""

    def __init__(self, fc, q=0.7071067811865476):
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


def static_curve(x, t, r, w):
    over = x - t
    if w < 1e-9:
        return x if over <= 0 else t + over / r
    if 2 * over < -w:
        return x
    if 2 * over > w:
        return t + over / r
    return x + (1 / r - 1) * (over + w / 2) ** 2 / (2 * w)


def feedback_reduction(y, t, r, w):
    over = y - t
    if w < 1e-9:
        return 0.0 if over <= 0 else (r - 1) * over
    if 2 * over < -w:
        return 0.0
    if 2 * over > w:
        return (r - 1) * over
    return (r - 1) * (over + w / 2) ** 2 / (2 * w)


def one_pole(ms):
    return math.exp(-1.0 / (max(0.01, ms) * 0.001 * FS))


def compress(x, s):
    """x: (channels, samples) float64. s: settings dict (same names as Compressor::Settings)."""
    channels, length = x.shape
    pedal = s["mode"] == "pedal"
    rms = s["detector"] == "rms"
    t, r, w = s["threshold_db"], s["ratio"], s["knee_db"]
    attack = one_pole(2.0 if pedal else s["attack_ms"])
    release = one_pole(60.0 if s["auto_release"] else s["release_ms"])
    slow_attack, slow_release, rms_c = one_pole(400.0), one_pole(1500.0), one_pole(10.0)
    makeup = (-12.0 - static_curve(-12.0, t, r, w)) if s["auto_makeup"] else s["makeup_db"]
    mix = s["mix"]
    filters = [SvfHighpass(s["sidechain_hz"]) for _ in range(channels)]
    mean_square = reduction = slow = 0.0
    last = [0.0] * channels
    y = np.zeros_like(x)

    for n in range(length):
        level = 0.0
        for ch in range(channels):
            v = last[ch] if pedal else x[ch, n]
            if s["sidechain_high_pass"]:
                v = filters[ch](v)
            level = max(level, v * v if rms else abs(v))
        if rms:
            mean_square = rms_c * mean_square + (1 - rms_c) * level
            level_db = 10 * math.log10(max(mean_square, MIN_LEVEL ** 2))
        else:
            level_db = 20 * math.log10(max(level, MIN_LEVEL))

        target = feedback_reduction(level_db, t, r, w) if pedal else level_db - static_curve(level_db, t, r, w)
        a = attack if target > reduction else release
        reduction = a * reduction + (1 - a) * target
        applied = reduction
        if s["auto_release"]:
            b = slow_attack if target > slow else slow_release
            slow = b * slow + (1 - b) * target
            applied = max(reduction, slow)

        gain = 10 ** (-applied / 20)
        wet_gain = gain * 10 ** (makeup / 20)
        for ch in range(channels):
            last[ch] = x[ch, n] * gain
            y[ch, n] = (1 - mix) * x[ch, n] + mix * x[ch, n] * wet_gain
    return y


def test_signal(seconds=1.5, channels=1, seed=3):
    """Plucked-like bursts at different levels with gaps: decaying harmonic tones from -40 to -3 dBFS,
    a low note (to exercise the sidechain high-pass), and silence."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = np.zeros((channels, n))
    notes = [(0.00, 196.0, -6.0), (0.25, 82.4, -3.0), (0.50, 330.0, -20.0), (0.70, 247.0, -40.0),
             (0.90, 440.0, -10.0), (1.10, 110.0, -14.0)]
    for ch in range(channels):
        for start, f, level in notes:
            f *= 1.0 + 0.01 * ch  # a slightly different second channel
            env = np.where(t >= start, np.exp(-(t - start) / 0.12), 0.0)
            tone = sum(np.sin(2 * np.pi * f * k * (t - start) + rng.uniform(0, 2 * np.pi)) / k for k in range(1, 6))
            out[ch] += 10 ** (level / 20) * env * tone / 1.6
    return out


CASES = {
    "studio_peak": dict(mode="studio", detector="peak", threshold_db=-24.0, ratio=4.0, knee_db=6.0, attack_ms=8.0,
                        release_ms=120.0, auto_release=False, makeup_db=0.0, auto_makeup=False, mix=1.0,
                        sidechain_high_pass=True, sidechain_hz=100.0),
    "studio_rms_auto": dict(mode="studio", detector="rms", threshold_db=-30.0, ratio=6.0, knee_db=10.0, attack_ms=3.0,
                            release_ms=200.0, auto_release=True, makeup_db=0.0, auto_makeup=True, mix=0.7,
                            sidechain_high_pass=True, sidechain_hz=150.0),
    "pedal": dict(mode="pedal", detector="peak", threshold_db=-30.0, ratio=8.0, knee_db=6.0, attack_ms=20.0,
                  release_ms=250.0, auto_release=False, makeup_db=6.0, auto_makeup=False, mix=1.0,
                  sidechain_high_pass=False, sidechain_hz=100.0),
    "studio_stereo_linked": dict(mode="studio", detector="peak", threshold_db=-20.0, ratio=3.0, knee_db=0.0,
                                 attack_ms=5.0, release_ms=80.0, auto_release=False, makeup_db=2.0, auto_makeup=False,
                                 mix=0.5, sidechain_high_pass=True, sidechain_hz=100.0),
}


def write_golden(folder):
    os.makedirs(folder, exist_ok=True)
    mono, stereo = test_signal(channels=1), test_signal(channels=2)
    wavfile.write(os.path.join(folder, "input_mono.wav"), int(FS), mono[0].astype(np.float32))
    wavfile.write(os.path.join(folder, "input_stereo.wav"), int(FS), stereo.T.astype(np.float32))
    for name, settings in CASES.items():
        x = stereo if "stereo" in name else mono
        # The C++ reads its input from the float32 file, so simulate on exactly those values.
        y = compress(x.astype(np.float32).astype(np.float64), settings)
        wavfile.write(os.path.join(folder, f"expected_{name}.wav"), int(FS), (y.T if y.shape[0] > 1 else y[0]).astype(np.float32))
        print(f"{name}: peak in {20 * np.log10(np.max(np.abs(x))):.1f} dBFS, out {20 * np.log10(np.max(np.abs(y))):.1f} dBFS")
    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump(CASES, f, indent=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", help="write the golden renders into this folder")
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    else:
        for x in range(-60, 1, 6):
            print(f"in {x:4d} dB -> studio {static_curve(x, -24, 4, 6):7.2f} dB")

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Accurate cascade graphic EQ: prototype and design study (BUILD_PLAN "EQ", graphic mode).

Nine octave bands (62.5 Hz to 16 kHz), each a second-order bell. Neighbouring bells overlap, so setting
each band's gain to its slider's value gives a response that misses the sliders (two adjacent +12 dB
sliders make about +15 dB between and at their centres). The fix follows

    V. Valimaki and J. Liski, "Accurate cascade graphic equalizer",
    IEEE Signal Processing Letters 24(2), 2017.

  1. The interaction matrix B: band m set to a prototype gain gp, its dB response at each design
     frequency k, divided by gp. Design frequencies are the 9 band centres plus the 8 geometric
     midpoints between them (17 rows); targets at midpoints are the average of the two neighbours.
  2. Band gains g = argmin |B g - t|^2 (least squares; B is 17 x 9).
  3. A bell's dB shape isn't exactly proportional to its gain (it narrows as the gain grows), so a
     second pass rebuilds B with each band at its own first-pass gain and solves again.

Design choices from the study below (2000 random settings, +-12 dB):
  * Band Q 0.9. With every design point weighted equally no Q met the plan's +-1 dB at the centres
    (best: 1.05 dB at Q 0.9, after two passes).
  * Midpoints weighted 0.5 against the centres. The centres then land within 0.35 dB (worst case:
    alternating +12/-12), and between bands the response stays within 2.4 dB of a straight line
    between neighbouring sliders. Band gains reach about +-29 dB internally for alternating sliders.
  * Two passes; a third changes nothing measurable.

The bells are the same as the C++ SVF's peak type: the RBJ bell, bilinear-transformed with the cutoff
prewarped, so the digital response at f equals the analog prototype's at tan(pi f / fs) / tan(pi fc / fs).

Usage:
  uv run --with numpy python prototypes/graphic_eq.py            # the Q and weighting study
  uv run --with numpy python prototypes/graphic_eq.py --q 0.9    # one Q in detail
  uv run --with numpy python prototypes/graphic_eq.py --golden tests/fixtures/graphic_eq_design.csv
"""

import argparse

import numpy as np

FS = 48000.0
CENTRES = 1000.0 * 2.0 ** np.arange(-4, 5)          # 62.5 Hz ... 16 kHz
MIDPOINTS = np.sqrt(CENTRES[:-1] * CENTRES[1:])
DESIGN_FREQS = np.sort(np.concatenate([CENTRES, MIDPOINTS]))
PROTOTYPE_GAIN = 17.0
Q = 0.9
MIDPOINT_WEIGHT = 0.5


def bell_db(f, fc, q, gain_db):
    """dB response of the SVF/RBJ bell at frequencies f."""
    if abs(gain_db) < 1e-12:
        return np.zeros_like(f, dtype=float)
    a = 10.0 ** (gain_db / 40.0)
    w = np.tan(np.pi * np.asarray(f) / FS) / np.tan(np.pi * fc / FS)   # prewarped analog frequency
    s = 1j * w
    h = (s * s + s * a / q + 1.0) / (s * s + s / (a * q) + 1.0)
    return 20.0 * np.log10(np.abs(h))


def total_db(f, gains, q):
    return sum(bell_db(f, fc, q, g) for fc, g in zip(CENTRES, gains))


def targets(sliders):
    """Slider values at the centres, neighbour averages at the midpoints, in DESIGN_FREQS order."""
    t = np.empty(len(DESIGN_FREQS))
    t[0::2] = sliders
    t[1::2] = 0.5 * (sliders[:-1] + sliders[1:])
    return t


def interaction_matrix(q, band_gains):
    """B[k, m]: band m's dB response at design frequency k per dB of its gain."""
    b = np.empty((len(DESIGN_FREQS), len(CENTRES)))
    for m, (fc, g) in enumerate(zip(CENTRES, band_gains)):
        g = g if abs(g) > 1e-3 else PROTOTYPE_GAIN
        b[:, m] = bell_db(DESIGN_FREQS, fc, q, g) / g
    return b


def design(sliders, q=Q, passes=2, midpoint_weight=MIDPOINT_WEIGHT):
    t = targets(sliders)
    w = np.ones(len(DESIGN_FREQS))
    w[1::2] = midpoint_weight

    def solve(b):
        return np.linalg.lstsq(b * w[:, None], t * w, rcond=None)[0]

    gains = solve(interaction_matrix(q, np.full(9, PROTOTYPE_GAIN)))
    for _ in range(passes - 1):
        gains = solve(interaction_matrix(q, gains))
    return gains


def write_golden(path, count=60, seed=7):
    """Slider settings and the band gains this design gives, for the C++ golden test."""
    rng = np.random.default_rng(seed)
    settings = list(rng.uniform(-12.0, 12.0, (count, 9)))
    settings[:5] = [np.full(9, 12.0), np.full(9, -12.0), np.tile([12.0, -12.0], 5)[:9],
                    np.r_[0, 0, 0, 0, 12, 0, 0, 0, 0], np.zeros(9)]
    with open(path, "w") as f:
        f.write("# Accurate cascade graphic EQ golden values (prototypes/graphic_eq.py): 9 slider dB, then 9 band gain dB\n")
        for sliders in settings:
            sliders = np.round(sliders, 3)
            gains = design(sliders)
            f.write(",".join(f"{v:.3f}" for v in sliders) + "," + ",".join(f"{v:.12f}" for v in gains) + "\n")
    print(f"wrote {len(settings)} settings to {path}")


def study(qs, trials=2000, seed=1, midpoint_weight=1.0):
    rng = np.random.default_rng(seed)
    settings = rng.uniform(-12.0, 12.0, (trials, 9))
    settings[:6] = [np.full(9, 12.0), np.full(9, -12.0), np.tile([12.0, -12.0], 5)[:9],
                    np.r_[np.full(4, 12.0), np.full(5, -12.0)], np.r_[0, 0, 0, 0, 12, 0, 0, 0, 0], np.zeros(9)]
    print(f"{trials} slider settings (the first six are extremes: all +12, all -12, alternating, step, single band, flat)")
    print("max |error| at band centres, dB:   naive    1 pass   2 passes   3 passes   (worst band gain, 2 passes)")
    for q in qs:
        errs = {name: 0.0 for name in ("naive", 1, 2, 3)}
        worst_gain = 0.0
        for sliders in settings:
            errs["naive"] = max(errs["naive"], np.max(np.abs(total_db(CENTRES, sliders, q) - sliders)))
            for p in (1, 2, 3):
                g = design(sliders, q, p, midpoint_weight)
                errs[p] = max(errs[p], np.max(np.abs(total_db(CENTRES, g, q) - sliders)))
                if p == 2:
                    worst_gain = max(worst_gain, np.max(np.abs(g)))
        print(f"  Q = {q:4.2f}:  {errs['naive']:7.2f}  {errs[1]:8.3f}  {errs[2]:9.3f}  {errs[3]:9.3f}     {worst_gain:6.1f}")


def detail(q):
    for sliders in (np.full(9, 12.0), np.tile([12.0, -12.0], 5)[:9], np.r_[0, 0, 0, 0, 12, 0, 0, 0, 0]):
        g = design(sliders, q)
        print("sliders:", np.round(sliders, 1))
        print("  band gains:", np.round(g, 2))
        print("  response at centres:", np.round(total_db(CENTRES, g, q), 2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--q", type=float)
    parser.add_argument("--golden", help="write the C++ golden fixture to this path")
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    elif args.q:
        detail(args.q)
    else:
        print("Equal weights:")
        study([0.7, 0.9, 1.0, 1.2, 1.41])
        print(f"Midpoints weighted {MIDPOINT_WEIGHT}:")
        study([0.8, 0.9, 1.0, 1.1], midpoint_weight=MIDPOINT_WEIGHT)

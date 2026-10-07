# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""Per-note measurements for the pools (pool.py), so objectives built on them can be judged in seconds
(docs/TONE_MATCH.md, "Round 2: the objective").

    uv run prototypes/tone_bench/notefeat.py --split dev [--workers 6]

A play-along take plays the target's notes (in the app: a take of the selected section, in either mode), so its
notes and the target's can be paired: learn_tone.align_notes (the take's onsets, chroma DTW in the 0.5 s band,
each note's onset found again in the target). Per pair, each signal's note (from its onset, the shorter of the
two notes' lengths and 300 ms) is measured on the benchmark's own scale: the power per ERB band (metrics: one band
per ERB, 50 Hz to 15 kHz, 2048-point Hann frames, hop 512, the mean power), and its level.

Stored per case in build/tone_bench/pool/<id>.notes.npz:
  T       (notes, bands)               the target's notes, dB
  N       (slots, gains, notes, bands)  the take's notes through each built-in at each pool Gain, before the cab, dB
  Lt, Ln  the notes' levels (dB), Ld the take's DI per note (dB)
  H       (candidates, bands)          each pool candidate's linear part (cab, tone, match EQ) on the ERB bands, dB
  LT, LN  the long-term ERB spectra (all playing frames): the target's and (slots, gains) the amp renders', dB
A linear filter after the amp adds its dB curve to every band (to within the band's own ripple), so a candidate's
note spectrum is N[s, g] + H[c]. That lets an objective try every candidate without convolving anything.
"""

import argparse
import concurrent.futures
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402
import scipy.fft as sfft  # noqa: E402

import cases as cs  # noqa: E402
import learn_tone as lt  # noqa: E402
import metrics as mx  # noqa: E402
import oracle as orc  # noqa: E402
import pool as pl  # noqa: E402
from common import SR, tm  # noqa: E402

NOTE_MAX = int(0.3 * SR)
WIN = np.hanning(2048)
FINE_F = np.fft.rfftfreq(orc.N_IR, 1.0 / SR)
E_FINE = mx.erb_number(FINE_F)
E_C = mx.erb_number(mx.ERB_CENTRES)
FINE_BANDS = [np.nonzero(np.abs(E_FINE - c) <= 0.5)[0] for c in E_C]


def note_bands(x, starts, lengths):
    out = np.zeros((len(starts), len(mx.ERB_CENTRES)))
    lev = np.zeros(len(starts))
    for k, (o, n) in enumerate(zip(starts, lengths)):
        seg = x[o:o + n]
        if len(seg) < 2048:
            seg = np.concatenate([seg, np.zeros(2048 - len(seg))])
        fr = mx.frames(seg, 2048, 512)
        p = np.abs(sfft.rfft(fr * WIN, axis=1)) ** 2
        out[k] = 10 * np.log10(np.maximum(p.mean(axis=0) @ mx._NOTE_MATRIX, 1e-20))
        lev[k] = 10 * np.log10(np.mean(seg[:n] ** 2) + 1e-20)
    return out, lev


# Harmonicity bands: 4 ERBs each, 100 Hz to 12 kHz (wide enough to hold a few harmonics of a low note).
HB_EDGES = mx.erb_hz(np.arange(mx.erb_number(100.0), mx.erb_number(12000.0) + 1e-9, 4.0))
HFFT = 16384
HF = np.fft.rfftfreq(HFFT, 1.0 / SR)
HB_BINS = [np.nonzero((HF >= a) & (HF < b))[0] for a, b in zip(HB_EDGES[:-1], HB_EDGES[1:])]


def harmonicity(x, starts, lengths, f0s):
    """Per note and band (HB_EDGES), the share of the band's power within its harmonics' lobes (each harmonic
    h f0 +- (1% of it + 1.5 bins)), dB (<= 0). A linear filter scales a band's harmonics and what lies between
    them alike, so this doesn't depend on the cab or the EQ; distortion (intermodulation between the notes of a
    chord, between the partials of one note through a nonlinearity, noise) fills the gaps, so it does depend on
    the drive. Also the band's power (dB), to weight it."""
    out = np.zeros((len(starts), len(HB_BINS)))
    pw = np.zeros((len(starts), len(HB_BINS)))
    for k, (o, n, f0) in enumerate(zip(starts, lengths, f0s)):
        seg = x[o:o + n]
        spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), HFFT)) ** 2
        if f0 <= 0:
            out[k] = 0.0
            continue
        hmask = np.zeros(len(HF), dtype=bool)
        for h in range(1, int(12000 / f0) + 2):
            hmask |= np.abs(HF - h * f0) <= 0.03 * h * f0 / 3 + 1.5 * SR / HFFT
        for b, idx in enumerate(HB_BINS):
            tot = spec[idx].sum() + 1e-30
            out[k, b] = 10 * np.log10(spec[idx][hmask[idx]].sum() / tot + 1e-12)
            pw[k, b] = 10 * np.log10(tot)
    return out, pw


def ltas(x):
    p = mx.power_frames(x)
    lvl = 10 * np.log10(np.maximum(p.sum(axis=1), 1e-20))
    return mx.erb_ltas(p, mx.active_mask(lvl))


def linear_bands(tone, eq, cab):
    """The linear part's power response averaged over each ERB band, dB."""
    h = orc.linear_ir(tone, eq, orc.cab_of(cab))
    H = np.abs(np.fft.rfft(h)) ** 2
    return np.array([10 * np.log10(np.mean(H[b]) + 1e-30) for b in FINE_BANDS])


def pairs(take, target):
    notes = lt.align_notes(take, target, band_seconds=tm.PLAY_ALONG_BAND_SECONDS)
    o = np.array([n["o"] for n in notes])
    t = np.array([n["t"] for n in notes])
    L = np.array([min(n["L"], NOTE_MAX) for n in notes])
    f0 = np.array([n["f0"] for n in notes])
    keep = L >= 2048
    return o[keep], t[keep], L[keep], f0[keep]


def extract(a):
    spec, split = a
    out = pl.POOL / f"{spec['id']}.notes.npz"
    if out.exists() and "HT" in np.load(out).files:
        return spec["id"]
    pool = json.loads((pl.POOL / f"{spec['id']}.json").read_text())
    data = cs.build(spec, None)
    target, take = data["target"], data["perf"]["b"]
    o, t, L, f0 = pairs(take, target)
    T, Lt = note_bands(target, t, L)
    _, Ld = note_bands(take, o, L)
    N = np.zeros((3, len(pl.GAINS), len(o), len(mx.ERB_CENTRES)))
    Ln = np.zeros((3, len(pl.GAINS), len(o)))
    LN = np.zeros((3, len(pl.GAINS), len(mx.ERB_CENTRES)))
    HT, PT = harmonicity(target, t, L, f0)
    HN = np.zeros((3, len(pl.GAINS), len(o), len(HB_BINS)))
    FN = np.zeros((3, len(pl.GAINS), 4))
    for s in range(3):
        for gi, g in enumerate(pl.GAINS):
            y = tm.render(take, s, g)
            N[s, gi], Ln[s, gi] = note_bands(y, o, L)
            LN[s, gi] = ltas(y)
            HN[s, gi], _ = harmonicity(y, o, L, f0)
            FN[s, gi] = tm.Analysis(y).features()
    H = np.array([linear_bands(c["tone"], c["eq"], c["cab"]) for c in pool["candidates"]])
    np.savez_compressed(out, T=T, N=N, Lt=Lt, Ln=Ln, Ld=Ld, H=H, LT=ltas(target), LN=LN, f0=f0, L=L,
                        HT=HT, PT=PT, HN=HN, FN=FN, FT=tm.Analysis(target).features(), o=o, t=t)
    print(f"  {spec['id']}: {len(o)} notes", flush=True)
    return spec["id"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--workers", type=int, default=6)
    args = ap.parse_args()
    specs = [s for s in cs.load_specs() if s["split"] == args.split and (pl.POOL / f"{s['id']}.json").exists()]
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        list(pool.map(extract, [(s, args.split) for s in specs]))


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
"""Tone match's tempo suggestion for the play-along count-in (docs/TONE_MATCH.md, "Play along"): the
section's tempo from the periodicity of its onsets, after Ellis, "Beat tracking by dynamic programming",
J. New Music Research 36(1), 2007, section 3.1 (the tempo estimate; not the beat tracker).

  1. Onset strength: STFT (2048-point periodic Hann, hop 480 = 10 ms at 48 kHz), each bin's level as
     log10(1e-4 + |X|^2 / max|X|^2) (a log, so a quiet note's attack counts like a loud one's), and the
     spectral flux, the sum over 30 Hz to 8 kHz of each bin's rise since the last frame (falls ignored).
  2. Its slow trend removed: minus its 1 s centred moving average, negative values set to 0.
  3. Its mean removed (a non-negative signal's autocorrelation is otherwise mostly its mean squared, which
     made noise look periodic), then its autocorrelation r(tau), unbiased (each lag's sum over the
     overlapping frames divided by their count), for lags from 0.25 s (240 BPM) to 1.5 s (40 BPM).
  4. Weighted by Ellis's tempo preference, a Gaussian in log2 of the lag around 0.5 s (120 BPM) with a
     standard deviation of 1.4 octaves, W(tau) = exp(-0.5 (log2(tau / 0.5 s) / 1.4)^2); the largest of
     W r picked and refined by a parabola through it and its neighbours.
  5. Confident when that lag's autocorrelation is at least 0.15 of r(0) (onsets that repeat), or no
     suggestion: noise doesn't get one (0.08 measured).

What it can't do (measured on the synthetic leads and riffs, `--study`): a solo guitar line has no
drummer, so the strongest period is often a multiple of the beat (half or double: still in time for a
count-in) or a dotted grouping (4:3 of it: not). On the synthetic band (drums and bass) it's exact. It's a
suggestion with the number shown; Tap and the BPM field are there for when it's wrong.

Usage: uv run --with numpy --with scipy python prototypes/tempo_estimate.py FILE.wav [...] [--json OUT]
       uv run --with numpy --with scipy python prototypes/tempo_estimate.py --study
"""

import argparse
import json
import math
import sys

import numpy as np
import scipy.io.wavfile as wavfile

SR = 48000
N_FFT = 2048
HOP = 480
F_LO, F_HI = 30.0, 8000.0
DETREND_SECONDS = 1.0
MIN_BPM, MAX_BPM = 40.0, 240.0
PREFERRED_SECONDS, PREFERENCE_OCTAVES = 0.5, 1.4
CONFIDENCE = 0.15


def onset_strength(x):
    w = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(N_FFT) / N_FFT)       # periodic Hann
    frames = 1 + max(0, (len(x) - N_FFT) // HOP)
    if len(x) < N_FFT:
        return np.zeros(0)
    idx = np.arange(N_FFT)[None, :] + HOP * np.arange(frames)[:, None]
    p = np.abs(np.fft.rfft(x[idx] * w, axis=1)) ** 2
    f = np.arange(N_FFT // 2 + 1) * SR / N_FFT
    keep = (f >= F_LO) & (f < F_HI)
    level = np.log10(1e-4 + p[:, keep] / (np.max(p[:, keep]) + 1e-30))
    flux = np.zeros(frames)
    flux[1:] = np.maximum(0.0, np.diff(level, axis=0)).sum(axis=1)
    return flux


def detrend(o):
    half = int(round(DETREND_SECONDS * SR / HOP / 2.0))
    c = np.concatenate([[0.0], np.cumsum(o)])
    out = np.zeros_like(o)
    for t in range(len(o)):
        a, b = max(0, t - half), min(len(o), t + half + 1)
        out[t] = o[t] - (c[b] - c[a]) / (b - a)
    return np.maximum(out, 0.0)


def estimate(x):
    """Returns (bpm, confidence, confident)."""
    o = detrend(onset_strength(np.asarray(x, dtype=float)))
    o = o - o.mean() if len(o) else o
    frame = HOP / SR
    lo, hi = int(math.floor(60.0 / MAX_BPM / frame)), int(math.ceil(60.0 / MIN_BPM / frame))
    if len(o) < 2 * hi or not np.any(o != 0):
        return 0.0, 0.0, False
    r = np.array([np.dot(o[: len(o) - k], o[k:]) / (len(o) - k) for k in range(hi + 2)])
    lags = np.arange(len(r)) * frame
    weight = np.zeros(len(r))
    weight[1:] = np.exp(-0.5 * (np.log2(lags[1:] / PREFERRED_SECONDS) / PREFERENCE_OCTAVES) ** 2)
    score = weight * r
    k = lo + 1 + int(np.argmax(score[lo + 1 : hi]))                     # a neighbour each side for the parabola
    a, b, c = score[k - 1], score[k], score[k + 1]
    den = a - 2.0 * b + c
    delta = 0.5 * (a - c) / den if den < 0.0 else 0.0
    bpm = 60.0 / ((k + delta) * frame)
    confidence = float(r[k] / r[0]) if r[0] > 0 else 0.0
    return float(bpm), confidence, confidence >= CONFIDENCE


def study():
    """The synthetic signals the docs quote: the tone match fixtures and a few phrases at other tempos."""
    import pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    import tone_match as tm
    fx = tm.REPO / "tests/fixtures/tone_match"
    cases = [("target_anything.wav, lead_d", tm.read_wav(fx / "target_anything.wav")[1], 120.0),
             ("target_same.wav, lead_a", tm.read_wav(fx / "target_same.wav")[1], 120.0 * 0.97),
             ("target_di_playalong.wav, lead_a", tm.read_wav(fx / "target_di_playalong.wav")[1], 120.0),
             ("reference_di.wav, lead_a", tm.read_wav(fx / "reference_di.wav")[1], 120.0),
             ("separation_mix.wav, lead in the band", tm.read_wav(fx / "separation_mix.wav")[1], 120.0),
             ("riff_a + riff_b at 108", tm.synth_di(["riff_a", "riff_b"], 5, tempo=0.9), 108.0),
             ("lead_c at 138", tm.synth_di(["lead_c"], 6, tempo=1.15), 138.0),
             ("lead_b at 96", tm.synth_di(["lead_b"], 8, tempo=0.8), 96.0),
             ("white noise", np.random.default_rng(1).normal(0.0, 0.1, SR * 10), None)]
    for name, x, truth in cases:
        bpm, conf, ok = estimate(x)
        ratio = "" if truth is None else f", {bpm / truth:.3f} x the truth ({truth:.1f})"
        print(f"  {name:38s} {bpm:7.2f} BPM, confidence {conf:.2f}{'' if ok else ' (no suggestion)'}{ratio}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--json")
    ap.add_argument("--study", action="store_true", help="the synthetic cases (needs prototypes/tone_match.py's renders)")
    args = ap.parse_args()
    if args.study:
        study()
        return
    out = {}
    for name in args.files:
        sr, x = wavfile.read(name)
        x = x.astype(float) / (32768.0 if x.dtype == np.int16 else 1.0)
        if x.ndim == 2:
            x = x.mean(axis=1)
        if sr != SR:
            sys.exit(f"{name}: {sr} Hz (48 kHz only)")
        bpm, conf, ok = estimate(x)
        out[name.split("/")[-1]] = {"bpm": bpm, "confidence": conf, "confident": ok}
        print(f"{name}: {bpm:.2f} BPM, confidence {conf:.3f}{'' if ok else ' (no suggestion)'}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()

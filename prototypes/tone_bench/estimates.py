# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Round 2 estimates: cheap what-ifs on the saved results (run.py --estimates), for the tools the diagnosis
points at but the oracle configurations don't measure directly.

  hires_pick    The matcher's pick (Anything, no cleanup: the best configuration measured) with its 5-band
                match EQ replaced by a high-resolution minimum-phase match curve fitted the way the matcher
                would fit it, WITHOUT the oracle: the target's long-term spectrum against the pick's render on
                the take, per FFT bin (8192 points), smoothed over 1/12 octave, scaled toward 0 dB where the
                target has little energy (tone_match.band_weights' confidence), capped at +-12 dB, made
                minimum phase (the folded real cepstrum, rigs.minimum_phase) as an 8192-tap FIR.
  hires_space   The oracle's in-space configuration with the same curve fitted to the hidden rig (the oracle's
                version: how far the curve can go once the amp is right).
  hires_cab     The oracle's cab-only configuration (true amp, best searchable IR) with the curve instead of
                its 5-band EQ: how much of the cab coverage error a fine curve absorbs.
  timefx_pick   The pick with the hidden rig's own time effect added after it: what a perfect time-effect fit
                would do to the time-effect facet (it changes nothing else).
"""

import json
import math

import numpy as np
import scipy.signal as sps

import cases as cs
import matcher as mt
import metrics as mx
import oracle as orc
import rigs
from common import SR, tm

N = tm.N_FFT


def smooth_octave(f, d, frac=12.0):
    """d (dB per bin) smoothed with a Gaussian of width 1/frac octave (sigma) on log frequency."""
    out = np.copy(d)
    lf = np.log2(np.maximum(f, 1.0))
    for i in range(1, len(f)):
        w = np.exp(-0.5 * ((lf - lf[i]) * frac) ** 2)
        lo, hi = np.searchsorted(lf, lf[i] - 3.0 / frac), np.searchsorted(lf, lf[i] + 3.0 / frac)
        out[i] = np.sum(w[lo:hi] * d[lo:hi]) / np.sum(w[lo:hi])
    return out


def hires_curve(target, candidate):
    """The curve (dB per bin of tone_match's 8192-point analysis) taking candidate's long-term spectrum to
    target's (module docstring)."""
    ta, ca = tm.Analysis(target), tm.Analysis(candidate)
    f = tm.FREQS
    d = tm.db(ta.ltas_bins) - tm.db(ca.ltas_bins)
    sm_t = smooth_octave(f, tm.db(ta.ltas_bins))
    d = smooth_octave(f, d)
    band = (f >= 60.0) & (f <= 14000.0)
    below = np.max(sm_t[band]) - sm_t
    conf = np.clip((tm.IGNORED_DB - below) / (tm.IGNORED_DB - tm.CONFIDENT_DB), tm.MIN_CONFIDENCE, 1.0)
    conf[~band] = 0.0
    mean = np.sum(d[band] * conf[band]) / np.sum(conf[band])
    return np.clip((d - mean) * conf, -tm.EQ_CAP_DB, tm.EQ_CAP_DB)


def curve_fir(curve_db):
    """A minimum-phase FIR (N taps) with |H| = 10^(curve/20) on the analysis bins."""
    mag = 10 ** (curve_db / 20.0)
    full = np.concatenate([mag, mag[-2:0:-1]])
    c = np.fft.ifft(np.log(np.maximum(full, 1e-6))).real
    fold = np.zeros(len(full))
    fold[0], fold[1:len(full) // 2], fold[len(full) // 2] = c[0], 2 * c[1:len(full) // 2], c[len(full) // 2]
    h = np.fft.ifft(np.exp(np.fft.fft(fold))).real
    return h[:N] * np.hanning(2 * N)[N:]


def estimate_case(row, split):
    spec = next(s for s in cs.load_specs() if s["id"] == row["id"])
    data = cs.build(spec, separate=None)          # the stems are cached by the run
    perf = data["perf"]
    ev, lay = mx.eval_signal(perf["c"], perf["c_onsets"])
    tone = rigs.tone_rig(ev, spec)
    full = rigs.time_fx(tone, spec.get("time_fx"), spec["seed"]) if spec.get("time_fx") else tone
    ref = mx.Reference(tone, full, lay)
    out = {}

    # The pick (Anything, no cleanup) with the curve fitted to the target against its render on the take.
    s = row["configs"]["any_raw"]["settings"]
    flat = dict(s, eq=[])
    on_take = mt.render_settings(perf["b"], flat)
    h = curve_fir(hires_curve(data["target"], on_take))
    y = sps.fftconvolve(mt.render_settings(ev, flat), h)[: len(ev)]
    out["hires_pick"] = mx.facets(ref, y)

    # The oracle's in-space configuration, the curve fitted against the hidden rig on C.
    o = row["oracle"]["space"]["settings"]
    base = orc.apply_linear(tm.render(ev, o["slot"], o["gain"]), o["tone"], [], orc.cab_of(o["cab"]))
    h = curve_fir(hires_curve(mx.seg(tone, lay, "c"), mx.seg(base, lay, "c")))
    out["hires_space"] = mx.facets(ref, sps.fftconvolve(base, h)[: len(ev)])

    # Cab only: the true pedal and amp, the best searchable IR, the mic EQ, the curve, the bus compressor.
    c = row["oracle"]["cab_only"]["settings"]
    amp_y = rigs.amp_out(rigs.pedal_out(ev, spec["pedal"]), spec["amp"])
    yc = rigs.apply_eq(sps.fftconvolve(amp_y, orc.cab_of(c["cab"]))[: len(ev)], spec["mic_eq"])
    pre = rigs.tone_rig(ev, spec, stages=("pedal", "amp", "cab", "eq"))
    h = curve_fir(hires_curve(mx.seg(pre, lay, "c"), mx.seg(yc, lay, "c")))
    yc = sps.fftconvolve(yc, h)[: len(ev)]
    if spec.get("bus_comp"):
        g = math.sqrt(np.mean(mx.seg(pre, lay, "c") ** 2) / np.mean(mx.seg(yc, lay, "c") ** 2))
        yc = rigs.post_comp(yc * g, spec["bus_comp"])
    out["hires_cab"] = mx.facets(ref, yc)

    # A perfect time-effect fit on the pick.
    y = mt.render_settings(ev, s)
    if spec.get("time_fx"):
        y = rigs.time_fx(y, spec["time_fx"], spec["seed"])
    out["timefx_pick"] = mx.facets(ref, y)
    return out

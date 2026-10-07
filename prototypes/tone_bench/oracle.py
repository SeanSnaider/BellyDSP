# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Oracle-assisted configurations: the best BellyDSP can do when the hidden rig is known.

The oracle searches with the benchmark's own metric against the hidden rig on the evaluation signal (the
same DI on both sides, so no performance or alignment error), and fits the linear parts (the amp's five
tone knobs, the match EQ) with the matcher's own fits (tone_match.fit_tone, fit_match_eq) to the hidden
rig's long-term spectrum on C. What each one answers:

  space      Built-in amp, Gain, tone, a searchable cab, match EQ: the best in the matcher's own search
             space. Its score is the floor the search space puts under ANY objective and optimizer.
  full       The same plus BellyDSP's own pedal in front (the hidden rig's, exactly: the pedals are the
             app's circuits) and its post compressor after (the hidden bus compressor's settings): what
             adding pedals and a compressor to the search could reach.
  amp_only   The hidden rig with only its amp replaced: the true pedal, cab, mic EQ, and compressor, a
             built-in amp at its best Gain, with tone and a match EQ refitted. The error the amp's coverage
             accounts for.
  cab_only   The hidden rig with only its cab replaced by the best searchable IR (and a match EQ refitted):
             the error the cab coverage accounts for.
  eq_only    The hidden rig with its mic EQ replaced by the fitted 5-band match EQ: whether the post EQ can
             express a studio's EQ.

The linear part after a model (tone bands, cab, post EQ) is applied here by FFT convolution with its exact
impulse response (each SVF band's complex response, tone_match.svf_response, times the cab IR's spectrum),
not by re-rendering: the chain is linear and time-invariant after the model, so it's the same signal
(checked against ampsim_render in the report: the SNR is printed).
"""

import numpy as np
import scipy.signal as sps

import metrics as mx
import rigs
from common import SR, cached, read_wav, tm

GAINS = list(np.arange(-24.0, 24.0 + 1e-9, 4.0))   # then 2 and 1 dB around the best
N_IR = 1 << 17


def cab_of(path):
    _, ir = read_wav(path)
    return (ir[:, 0] if ir.ndim == 2 else ir)[:SR]


def linear_ir(tone, eq, cab):
    """The impulse response of tone bands -> cab -> post EQ (exact, N_IR long)."""
    f = np.fft.rfftfreq(N_IR, 1.0 / SR)
    H = np.fft.rfft(cab, N_IR) if cab is not None else np.ones(len(f), dtype=complex)
    for (kind, fc, q), g in zip(tm.TONE_BANDS, tone):
        if g != 0.0:
            H = H * tm.svf_response(kind, fc, q, g, f)
    for kind, fc, g, q in (eq or []):
        if g != 0.0:
            H = H * tm.svf_response(kind, fc, q, g, f)
    return np.fft.irfft(H, N_IR)


def apply_linear(y, tone, eq, cab):
    return sps.fftconvolve(y, linear_ir(tone, eq, cab))[: len(y)]


def fit_linear(ref_c, cand_c, tone=True, eq=True):
    """The matcher's fits of the tone knobs (if the slot has them) and the match EQ, from cand_c's spectrum to
    ref_c's (anything mode: the same DI on both sides, so the long-term spectra correspond exactly). Returns
    (tone, eq bands, the spectral error left in dB)."""
    ra, ca = tm.Analysis(ref_c), tm.Analysis(cand_c)
    r = tm.smooth_bands(ra.ltas - ca.ltas)
    th = np.zeros(5)
    if tone:
        th, _ = tm.fit_tone(r, ra.weights, polish=True)
        r = r - tm.tone_db(th, tm.COARSE_CENTRES)
    if not eq:
        return [float(v) for v in th], [], tm.weighted_rms_centred(r, ra.weights)
    eq, _ = tm.fit_match_eq(r, ra.weights)
    after = tm.weighted_rms_centred(r - tm.eq_db(eq, tm.COARSE_CENTRES), ra.weights)
    return [float(v) for v in th], [list(b) for b in eq], after


def screen_cab(ref_c_analysis, amp_c, cabs, count=3):
    """tone_match.screen_cabs against the hidden rig's spectrum (the amp's long-term spectrum times each
    IR's |H|^2, the linear tone fit): the count best."""
    ya = tm.Analysis(amp_c)
    scores = []
    for cab in cabs:
        _, hb = tm.cab_bands_db(cab)
        pred = tm.db((ya.ltas_bins * hb) @ tm.COARSE)
        _, e = tm.fit_tone(tm.smooth_bands(ref_c_analysis.ltas - pred), ref_c_analysis.weights, polish=False)
        scores.append((e, cab))
    scores.sort(key=lambda s: s[0])
    return [c for _, c in scores[:count]]


def _scaled_comp(y, target_rms_db, comp, lay):
    """The bus compressor on y, with y first brought to the level the hidden rig had there (its threshold is
    absolute)."""
    c = mx.seg(y, lay, "c")
    g = 10 ** ((target_rms_db - 10 * np.log10(np.mean(c ** 2) + 1e-20)) / 20)
    return rigs.post_comp(y * g, comp)


def search_builtin(ev, ref, rig, cabs, pedal=None, comp=None, comp_level=None, fixed_cab=None, post=None, render_workers=4):
    """The best built-in amp configuration by the combined metric: every slot at GAINS (4 dB apart), the
    best of the screened cabs for each (or fixed_cab: an IR array placed where the hidden rig has its cab, with
    `post` (a function) after it), tone and match EQ fitted; then 2 and 1 dB steps around the best Gain.
    Returns (facets, settings)."""
    lay = ref.layout
    ref_c = mx.seg(ref.y, lay, "c")
    ra = tm.Analysis(ref_c)
    x = rigs.pedal_out(ev, pedal)
    results = {}

    def candidate(slot, gain):
        key = (slot, round(gain, 2))
        if key in results:
            return results[key]
        y = tm.render(x, slot, gain)
        if fixed_cab is not None:
            yc = post(sps.fftconvolve(y, fixed_cab)[: len(y)])
            tone, eq, _ = fit_linear(ref_c, mx.seg(yc, lay, "c"))
            # The tone knobs sit before the cab, the match EQ after it; both are linear, so in this order.
            out = apply_linear(post(sps.fftconvolve(apply_linear(y, tone, None, None), fixed_cab)[: len(y)]), [0] * 5, eq, None)
            s = dict(slot=slot, gain=gain, tone=tone, eq=eq, cab="true")
        else:
            best_cab = None
            best_e = None
            yc = mx.seg(y, lay, "c")
            for cab in screen_cab(ra, yc, cabs):
                _, _, after = fit_linear(ref_c, sps.fftconvolve(yc, cab_of(cab))[: len(yc)], eq=False)
                if best_e is None or after < best_e:
                    best_e, best_cab = after, cab
            cab = best_cab
            tone, eq, _ = fit_linear(ref_c, sps.fftconvolve(yc, cab_of(cab))[: len(yc)])
            out = apply_linear(y, tone, eq, cab_of(cab))
            s = dict(slot=slot, gain=gain, tone=tone, eq=eq, cab=str(cab))
        if comp:
            out = _scaled_comp(out, comp_level, comp, lay)
        f = mx.facets(ref, out)
        results[key] = (f, s, out)
        return results[key]

    # The amp renders first, in parallel (each is its own ampsim_render process; cached).
    slots = range(len(tm.MODEL_FILES))   # the three built-ins, or every built-in amp with run.py --amps all
    tm.render_many([(x, slot, g) for slot in slots for g in GAINS], workers=render_workers)
    for slot in slots:
        for g in GAINS:
            candidate(slot, g)
    for step in (2.0, 1.0):
        s0 = min(results.values(), key=lambda r: r[0]["combined"])[1]
        gains = [g for g in (s0["gain"] - step, s0["gain"] + step) if -24.0 <= g <= 24.0]
        tm.render_many([(x, s0["slot"], g) for g in gains], workers=render_workers)
        for g in gains:
            candidate(s0["slot"], g)
    return min(results.values(), key=lambda r: r[0]["combined"])


def cab_only(ev, ref, rig, cabs):
    """The hidden rig with its cab replaced by each searchable IR, a match EQ refitted; the best by the
    combined metric over the 3 best by the screen."""
    lay = ref.layout
    ref_c = mx.seg(ref.y, lay, "c")
    amp_y = rigs.amp_out(rigs.pedal_out(ev, rig["pedal"]), rig["amp"])
    best = None
    ra = tm.Analysis(ref_c)
    for cab in screen_cab(ra, mx.seg(amp_y, lay, "c"), cabs):
        y = rigs.apply_eq(sps.fftconvolve(amp_y, cab_of(cab))[: len(amp_y)], rig["mic_eq"])
        _, eq, _ = fit_linear(ref_c, mx.seg(y, lay, "c"), tone=False)
        y = apply_linear(y, [0] * 5, eq, None)
        if rig.get("bus_comp"):
            y = rigs.post_comp(y, rig["bus_comp"])
        f = mx.facets(ref, y)
        if best is None or f["combined"] < best[0]["combined"]:
            best = (f, dict(cab=str(cab), eq=eq))
    return best


def eq_only(ev, ref, rig):
    lay = ref.layout
    ref_c = mx.seg(ref.y, lay, "c")
    y = rigs.tone_rig(ev, rig, stages=("pedal", "amp", "cab"))
    _, eq, _ = fit_linear(ref_c, mx.seg(y, lay, "c"), tone=False)
    y = apply_linear(y, [0] * 5, eq, None)
    if rig.get("bus_comp"):
        y = rigs.post_comp(y, rig["bus_comp"])
    return mx.facets(ref, y), dict(eq=eq)


def all_oracles(ev, ref, rig, cabs, comp_level, render_workers=4):
    """Every oracle configuration of the module docstring. Returns {name: (facets, settings)}.

    "full" is the best of: the in-space configuration, the search with the true pedal in front, and either of
    those with the true bus compressor after (applied to that configuration's output at the hidden rig's
    level). Forcing the pedal on isn't always better (a built-in amp behind the hidden rig's pedal can land
    further off than without it), and the oracle may leave a tool out, as a user would."""
    space = search_builtin(ev, ref, rig, cabs, render_workers=render_workers)
    out = {"space": space[:2]}
    variants = [("space", space)]
    if rig.get("pedal") is not None:
        variants.append(("pedal", search_builtin(ev, ref, rig, cabs, pedal=rig.get("pedal"), render_workers=render_workers)))
    if rig.get("bus_comp"):
        for name, v in list(variants):
            y = _scaled_comp(v[2], comp_level, rig["bus_comp"], ref.layout)
            variants.append((name + "+comp", (mx.facets(ref, y), v[1], y)))
    best_name, best = min(variants, key=lambda nv: nv[1][0]["combined"])
    out["full"] = (best[0], dict(best[1], tools=best_name))
    out["pedal_forced"] = next(v[:2] for n, v in variants if n == "pedal") if rig.get("pedal") is not None else space[:2]
    true_cab = rigs.cab_ir(rig["cab"])
    post = lambda y: rigs.apply_eq(y, rig["mic_eq"])
    out["amp_only"] = search_builtin(ev, ref, rig, cabs, pedal=rig.get("pedal"), comp=rig.get("bus_comp"), comp_level=comp_level,
                                     fixed_cab=true_cab, post=post, render_workers=render_workers)[:2]
    out["cab_only"] = cab_only(ev, ref, rig, cabs)
    out["eq_only"] = eq_only(ev, ref, rig)
    return out

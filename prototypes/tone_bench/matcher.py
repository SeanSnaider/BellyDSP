# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""The methods the benchmark judges: the current matcher in the app's configurations, and the anchors.

The current matcher is prototypes/tone_match.py's match() with the built-in gain sets: the C++ matcher
(src/tonematch/ToneMatcher.*) is its line-by-line port, golden-tested against it (identical spectra,
features, DTW path, fits, and matched result on the fixtures; docs/TONE_MATCH.md, "The C++ port and its
tests"). The cleanup is learn_tone.py's align_notes and informed_mask with the app's time smoothing
(APP_TIME_SMOOTH), which src/tonematch/InformedMask.* matches within 1e-7. The play-along take is lined up
with the target, so Same part runs in the 0.5 s band, as the app does after a take.

Configurations:
  same_clean   Same part, cleaned up with the take: the app's default after a play-along take
  same_raw     Same part, the cleanup off
  any_clean    Anything, on the target cleaned up with the take (ASSUMPTIONS TM55's proposal; not in the app)
  any_raw      Anything, as the app ran it before Round 2
  any_auto     Anything, the cleanup only when the bleed detector says so (learn_tone.bleed_excess_db: separation
               on and the excess above BLEED_EXCESS_DB): the app's default after a take since Round 2
  same_auto    Same part, the cleanup whenever the target was separated (Round 1: it rescues Same part there)
  any_auto_take  any_auto with the take-aware score (tone_match.take_score: the take's notes paired with the
               target's, Round 2 step 3)
  any_auto_take_fx  any_auto_take with the pedals and the post compressor in the search (tone_match.pedal_variants,
               search_post_comp; Round 2 step 2)
  any_fx_curve     any_auto_take_fx with the match curve instead of the 5-band match EQ (tone_match.fit_match_curve), at
               tone_match.CURVE_AMOUNT_PERCENT
  any_fx_curve_eq  any_auto_take_fx with the match curve fitted on top of the match EQ
The DI the matcher gets is always the take (B). Its search is the split's searchable IRs (rigs.HELD_OUT).
"""

import numpy as np

import learn_tone as lt
import rigs
from common import SR, tm

CONFIGS = {
    "same_clean": dict(mode="same", cleanup=True),
    "same_raw": dict(mode="same", cleanup=False),
    "any_clean": dict(mode="anything", cleanup=True),
    "any_raw": dict(mode="anything", cleanup=False),
    "any_auto": dict(mode="anything", cleanup="auto"),
    "same_auto": dict(mode="same", cleanup="auto"),
    "any_auto_take": dict(mode="anything", cleanup="auto", score="take"),
    "any_auto_take_fx": dict(mode="anything", cleanup="auto", score="take", pedals=True),
    "any_fx_curve": dict(mode="anything", cleanup="auto", score="take", pedals=True, curve="replace"),
    "any_fx_curve_eq": dict(mode="anything", cleanup="auto", score="take", pedals=True, curve="on_top"),
    # The search's breadth (docs/TONE_MATCH.md, "Round 2: eight amps"): tone_match's REFINE_SLOTS, TAKE_SHORTLIST,
    # TAKE_PER_AMP, PEDAL_SLOTS for this configuration only; trace keeps every scored candidate in the result.
    "any_fx_curve_r2": dict(mode="anything", cleanup="auto", score="take", pedals=True, curve="replace",
                            search=dict(REFINE_SLOTS=2, TAKE_SHORTLIST=16, TAKE_PER_AMP=0, PEDAL_SLOTS=1, TAKE_PRE_SHORTLIST=0,
                                        TAKE_REFINE_ALL=False, PEDAL_SLOTS_PRE=0)),
    "any_fx_curve_wide": dict(mode="anything", cleanup="auto", score="take", pedals=True, curve="replace",
                              search=dict(TAKE_REFINE_ALL=True, TAKE_PRE_SHORTLIST=16, PEDAL_SLOTS=1, PEDAL_SLOTS_PRE=1, TAKE_PER_AMP=0,
                                          PRE_WITH_EQ_TARGET=False)),
    "any_fx_curve_all": dict(mode="anything", cleanup="auto", score="take", pedals=True, curve="replace", trace=True,
                             search=dict(REFINE_SLOTS=None, TAKE_SHORTLIST=10 ** 6, TAKE_PER_AMP=0, PEDAL_SLOTS=10 ** 6)),
}


def cleanup(target, take):
    """The app's "Clean up with my take": the take's notes placed in the target within the play-along band,
    the informed harmonic mask. Returns (cleaned, notes); the target as it was if no note has a pitch."""
    notes = lt.align_notes(take, target, band_seconds=tm.PLAY_ALONG_BAND_SECONDS)
    if not any(n["f0"] > 0 for n in notes):
        return target, 0
    return lt.informed_mask(target, notes, time_smooth=lt.APP_TIME_SMOOTH), len(notes)


def use_split(split):
    tm.CAB_FILES = rigs.searchable_irs(split)


def run(target, take, config, split, workers=4, separated=False):
    """The matcher in this configuration. separated: the target is a separated stem (the app's "Separate the
    guitar first"), which the auto cleanup needs to know."""
    knobs = CONFIGS[config].get("search", {})
    saved = {k: getattr(tm, k) for k in knobs}
    try:
        for k, v in knobs.items():
            setattr(tm, k, v)
        return _run(target, take, config, split, workers, separated)
    finally:
        for k, v in saved.items():
            setattr(tm, k, v)


def _run(target, take, config, split, workers, separated):
    use_split(split)
    c = CONFIGS[config]
    band = tm.PLAY_ALONG_BAND_SECONDS if c["mode"] == "same" else None

    def match(t):
        notes = lt.align_notes(take, t, band_seconds=tm.PLAY_ALONG_BAND_SECONDS) if c.get("score") == "take" else None
        trace = {} if c.get("trace") else None
        r = tm.match(t, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band, take_notes=notes,
                     pedals=c.get("pedals", False), curve=c.get("curve"), trace=trace)
        r["trace"] = trace
        return r

    t, notes, excess = target, None, None
    if c["cleanup"] == "auto":
        if separated and c["mode"] == "same":
            t, notes = cleanup(target, take)
        elif separated:
            r = match(target)
            excess = lt.bleed_excess_db(target, take, render_settings(take, result_settings(r)))
            if excess <= lt.BLEED_EXCESS_DB:
                r["notes"], r["bleed_excess_db"] = None, excess
                return r, target
            t, notes = cleanup(target, take)
    elif c["cleanup"]:
        t, notes = cleanup(target, take)
    r = match(t)
    r["notes"], r["bleed_excess_db"] = notes, excess
    return r, t


def run_all_amps(target, take, config, split, workers=4):
    """The matcher over every built-in amp loaded (run.py --amps all), WITHOUT changing tone_match.py, whose
    search is three slots (range(3)): match() is run on the amps three at a time (the three slot defaults,
    then the others in order, the last triple padded with the first amp), each run's pick is scored by the
    matcher's own objective (objective(), the same Candidate.total() match() ranks by, on the same target),
    and the best pick wins, its slot given as an index into the full list. Close to what a match() searching
    all eight at once would choose; it refines the best REFINE_SLOTS amps of every triple rather than of all
    eight, so it searches slightly more. Returns (result, target used, objective of the pick)."""
    use_split(split)
    full_amps, full_files = list(tm.AMPS), list(tm.MODEL_FILES)
    c = CONFIGS[config]
    t, notes = (cleanup(target, take) if c["cleanup"] else (target, None))
    band = tm.PLAY_ALONG_BAND_SECONDS if c["mode"] == "same" else None
    order = list(range(len(full_files)))
    triples = [order[i:i + 3] for i in range(0, len(order), 3)]
    triples[-1] = (triples[-1] + order)[:3]
    best = None
    try:
        for tri in triples:
            tm.AMPS = [full_amps[i] for i in tri]
            tm.MODEL_FILES = [full_files[i] for i in tri]
            r = tm.match(t, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band)
            r["slot"] = tri[r["slot"]]
            r["amp"] = full_amps[r["slot"]]
            tm.AMPS, tm.MODEL_FILES = full_amps, full_files
            cab = next(p for p in rigs.ALL_IRS if p.name == r["cab"])
            score = objective(t, take, c["mode"], band, r["slot"], r["gain_db"], cab)
            if best is None or score < best[2]:
                best = (r, t, score)
    finally:
        tm.AMPS, tm.MODEL_FILES = full_amps, full_files
    best[0]["notes"] = notes
    return best


def result_settings(r):
    cab = next(p for p in rigs.ALL_IRS if p.name == r["cab"])
    return dict(slot=r["slot"], gain=r["gain_db"], tone=list(r["tone_db"]), cab=str(cab), eq=[list(b) for b in r["eq"]],
                pedal=r.get("pedal"), post_comp=r.get("post_comp"), match_curve=r.get("match_curve"),
                match_curve_amount=r.get("match_curve_amount"))


def render_settings(x, s, pedal=None, comp=None):
    """x through BellyDSP with these settings (the matcher's result format), exactly as the app renders
    them (ampsim_render: the gain set, its Gain and tone, the cab IR as close mic 1, the post EQ), with
    BellyDSP's own pedal in front and its post compressor after if asked (the oracle's)."""
    pedal = pedal if pedal is not None else s.get("pedal")
    comp = comp if comp is not None else s.get("post_comp")
    y = rigs.pedal_out(x, pedal)
    y = tm.render(y, s["slot"], s["gain"], tone=s["tone"], cab=s["cab"], eq=[tuple(b) for b in s["eq"]] if s["eq"] else None,
                  curve=s.get("match_curve"), curve_amount=s.get("match_curve_amount") or 100.0)
    if comp:
        y = rigs.post_comp(y, comp)
    return y


def objective(target, take, mode, band, slot, gain, cab_path):
    """The matcher's own score (tone_match.Candidate.total: the spectral error after the linear tone fit
    plus lambda times the distortion distance) of one (slot, Gain, cab) on this target, computed exactly as
    match() computes it (in Same part, aligned against that slot's render at Gain 0)."""
    path = None
    ta = tm.Target(target)
    if mode == "same":
        path, _ = tm.align(ta.analysis.power, tm.stft_power(tm.render(take, slot, 0.0)), tm.band_frames(band))
    y = tm.render(take, slot, gain)
    return tm.Candidate(slot, gain, cab_path, y, ta, mode, path).total()

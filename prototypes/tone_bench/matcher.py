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
    use_split(split)
    c = CONFIGS[config]
    t = target
    notes = None
    band = tm.PLAY_ALONG_BAND_SECONDS if c["mode"] == "same" else None
    if c["cleanup"] == "auto":
        if not separated:
            r = tm.match(target, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band)
            r["notes"], r["bleed_excess_db"] = None, None
            return r, target
        if c["mode"] == "same":
            t, notes = cleanup(target, take)
            r = tm.match(t, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band)
            r["notes"], r["bleed_excess_db"] = notes, None
            return r, t
        r = tm.match(target, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band)
        excess = lt.bleed_excess_db(target, take, render_settings(take, result_settings(r)))
        r["bleed_excess_db"], r["notes"] = excess, None
        if excess <= lt.BLEED_EXCESS_DB:
            return r, target
        t, notes = cleanup(target, take)
        r = tm.match(t, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band)
        r["notes"], r["bleed_excess_db"] = notes, excess
        return r, t
    if c["cleanup"]:
        t, notes = cleanup(target, take)
    r = tm.match(t, take, c["mode"], log=lambda *a: None, workers=workers, band_seconds=band)
    r["notes"] = notes
    return r, t


def result_settings(r):
    cab = next(p for p in rigs.ALL_IRS if p.name == r["cab"])
    return dict(slot=r["slot"], gain=r["gain_db"], tone=list(r["tone_db"]), cab=str(cab), eq=[list(b) for b in r["eq"]])


def render_settings(x, s, pedal=None, comp=None):
    """x through BellyDSP with these settings (the matcher's result format), exactly as the app renders
    them (ampsim_render: the gain set, its Gain and tone, the cab IR as close mic 1, the post EQ), with
    BellyDSP's own pedal in front and its post compressor after if asked (the oracle's)."""
    y = rigs.pedal_out(x, pedal)
    y = tm.render(y, s["slot"], s["gain"], tone=s["tone"], cab=s["cab"], eq=[tuple(b) for b in s["eq"]] if s["eq"] else None)
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

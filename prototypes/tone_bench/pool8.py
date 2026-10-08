# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""Candidate pools over every built-in amp, with the take-aware score's terms (docs/TONE_MATCH.md, "Round 3: the
score with eight amps").

    uv run prototypes/tone_bench/pool8.py --split dev [--cases id,...] [--workers 2] [--render-workers 3]

pool.py's pools (every amp at Gain -24 .. +24 dB in 4 dB steps, the four cabs the matcher's screen ranks best, the
tone polished and the match EQ fitted as the matcher fits them, each candidate's TRUE score against the hidden rig on
the evaluation signal), over all eight built-in gain sets (common.GAIN_SETS), and per candidate what the take-aware
score is made of, computed as tone_match.match computes it on the raw target (anything mode, no cleanup):
  spectral, nl            the old score's spectral error after the linear tone fit and distortion distance
  flux, crest             Analysis of the take through the amp and the cab
  erb, erb_pre            D_erb with the polished tone and the match EQ (S's), and with the linear tone and no EQ (S_pre's)
and per (amp, Gain) render, the take's paired notes through the amp: their attack (the share of the note's first
300 ms in its first 15 ms, dB), level (dB), and envelope profile (notefeat.envelope_profile: the share in 0-15, 15-40,
40-90, 90-170, 170-300 ms, dB). The target's own measures are stored once, with the take's DI level per note. So any
score built from these terms can be judged in milliseconds (refit_score.py). Stored in build/tone_bench/pool8/<id>.json.
"""

import argparse
import concurrent.futures
import json
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402

import cases as cs  # noqa: E402
import learn_tone as lt  # noqa: E402
import notefeat as nf  # noqa: E402
import pool as pl  # noqa: E402
from common import OUT, tm  # noqa: E402

POOL8 = OUT / "pool8"


def build_case(a):
    spec, split, render_workers = a
    path = POOL8 / f"{spec['id']}.json"
    if path.exists():
        return spec["id"]
    t0 = time.time()
    case = pl.Case(spec, split)
    target, take = case.target, case.take
    ta = tm.Analysis(target)
    tgt = tm.Target(target)
    notes = lt.align_notes(take, target, band_seconds=tm.PLAY_ALONG_BAND_SECONDS)
    tn = tm.TakeNotes(target, notes)
    slots = range(len(tm.MODEL_FILES))
    tm.render_many([(x, s, g) for x in (take, case.ev) for s in slots for g in pl.GAINS], workers=render_workers)
    renders, cands = [], []
    for s in slots:
        for g in pl.GAINS:
            y = tm.render(take, s, g)
            ya = tm.Analysis(y)
            att, lev = tm.note_measures(y, tn.o, tn.L)
            renders.append(dict(slot=s, gain=g, attack=att.tolist(), level=lev.tolist(),
                                env=nf.envelope_profile(y, tn.o, tn.L).tolist(), features=[float(v) for v in ya.features()]))
            yev = tm.render(case.ev, s, g)
            for cab in tm.screen_cabs(ya, ta.ltas, ta.weights, pl.CABS_PER):
                c = tm.Candidate(s, g, cab, y, tgt, "anything", None)
                w = ta.weights
                tone, _ = tm.fit_tone(c.residual, w, polish=True)
                eq, _ = tm.fit_match_eq(c.residual - tm.tone_db(tone, tm.COARSE_CENTRES), w)
                erb_bins = tm.erb_ltas_bins(c.y)
                args = (tn, ta, c.analysis.flux, c.analysis.crest, (att, lev), erb_bins)
                _, terms = tm.take_score_of(*args, tone, eq, c.spectral)
                _, pre_terms = tm.take_score_of(*args, c.tone, [], c.spectral)
                f = case.true_facets(yev, [float(v) for v in tone], [list(b) for b in eq], cab)
                cands.append(dict(slot=s, gain=g, cab=cab.name, render=len(renders) - 1, spectral=c.spectral, nl=c.nl,
                                  flux=terms["flux"], crest=terms["crest"], erb=terms["erb"], erb_pre=pre_terms["erb"],
                                  features=[float(v) for v in c.analysis.features()], true=f["combined"],
                                  facets={k: f[k] for k in ("lt_erb", "note_erb", "feel", "harm", "imd", "crest")}))
    out = dict(id=spec["id"], split=split, style=spec["style"], production=spec["production"], amp=spec["amp"]["name"],
               amps=list(tm.AMPS), notes=len(tn.o), usable=bool(tn.usable()),
               target=dict(attack=tn.attack.tolist(), level=tn.level.tolist(), weight=tn.weight.tolist(),
                           env=nf.envelope_profile(target, tn.t, tn.L).tolist(), features=[float(v) for v in ta.features()],
                           di_level=tm.note_measures(take, tn.o, tn.L)[1].tolist(), flux=ta.flux, crest=ta.crest),
               renders=renders, candidates=cands)
    path.write_text(json.dumps(out, default=float))
    print(f"  {spec['id']}: {len(cands)} candidates, {len(tn.o)} note pairs, in {time.time() - t0:.0f} s", flush=True)
    return spec["id"]


def load(split, cases=None):
    specs = [s for s in cs.load_specs() if s["split"] == split and (cases is None or s["id"] in cases)]
    return [json.loads((POOL8 / f"{s['id']}.json").read_text()) for s in specs if (POOL8 / f"{s['id']}.json").exists()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--cases")
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--render-workers", type=int, default=3)
    args = ap.parse_args()
    POOL8.mkdir(parents=True, exist_ok=True)
    specs = [s for s in cs.load_specs() if s["split"] == args.split]
    if args.cases:
        ids = set(args.cases.split(","))
        specs = [s for s in specs if s["id"] in ids]
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        list(pool.map(build_case, [(s, args.split, args.render_workers) for s in specs]))


if __name__ == "__main__":
    main()

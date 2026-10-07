# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""Candidate pools: a fast way to compare matcher objectives (docs/TONE_MATCH.md, "tone_bench, Round 2").

Round 1 found the matcher's search finds what its score prefers, and its score prefers the wrong answer (the
oracle's configuration beat the pick by the matcher's own score in only 5 of 50 cases). So Round 2 changes the
score, and needs to try many scores without running the whole benchmark for each. A pool is, per case, a fixed
set of configurations the matcher could pick, each with its TRUE benchmark score (metrics.facets against the
hidden rig on the evaluation signal) and what an objective needs to judge it (the take through it). An objective
is then judged by the true score of the configuration it ranks first, in seconds.

    uv run prototypes/tone_bench/pool.py build --split dev [--cases id,...] [--workers 3]
    uv run prototypes/tone_bench/pool.py pedals --split dev       # adds the pedal and compressor variants

The pool (build): every built-in amp at Gain -24 .. +24 dB in 4 dB steps (the oracle's grid, so the evaluation
renders are cached already), the four cabs the matcher's own screen ranks best for each (anything mode, the
split's searchable IRs), the linear part (tone knobs, polished, and the match EQ) fitted as the matcher fits it
(the target's long-term spectrum against the take through the candidate). Stored in
build/tone_bench/pool/<id>.json; renders in the shared cache.
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
import matcher as mt  # noqa: E402
import metrics as mx  # noqa: E402
import oracle as orc  # noqa: E402
import rigs  # noqa: E402
from common import OUT, tm  # noqa: E402

POOL = OUT / "pool"
GAINS = [float(g) for g in np.arange(-24.0, 24.0 + 1e-9, 4.0)]
CABS_PER = 4


class Case:
    """What a pool's objectives and its true scores need from one case, built once (renders cached)."""

    def __init__(self, spec, split):
        self.spec, self.split = spec, split
        data = cs.build(spec, None)
        self.data = data
        self.target = data["target"]
        self.take = data["perf"]["b"]
        self.take_onsets = data["perf"]["b_onsets"]
        perf = data["perf"]
        self.ev, self.lay = mx.eval_signal(perf["c"], perf["c_onsets"])
        tone_ev, full_ev = rigs.tone_rig(self.ev, spec), None
        full_ev = rigs.time_fx(tone_ev, spec.get("time_fx"), spec["seed"]) if spec.get("time_fx") else tone_ev
        self.ref = mx.Reference(tone_ev, full_ev, self.lay)
        mt.use_split(split)
        self.cabs = rigs.searchable_irs(split)

    def true_facets(self, ev_amp_render, tone, eq, cab, comp=None):
        out = orc.apply_linear(ev_amp_render, tone, eq, orc.cab_of(cab))
        if comp is not None:
            out = rigs.post_comp(out, comp)
        return mx.facets(self.ref, out)


def fit_linear_like_matcher(target_analysis, y_amp, cab):
    """The matcher's anything-mode candidate (tone_match.Candidate) and its final fits (the tone polished, the
    match EQ). Returns (candidate, tone, eq)."""
    c = tm.Candidate(0, 0.0, cab, y_amp, _T(target_analysis), "anything", None)
    w = target_analysis.weights
    tone, _ = tm.fit_tone(c.residual, w, polish=True)
    residual = c.residual - tm.tone_db(tone, tm.COARSE_CENTRES)
    eq, _ = tm.fit_match_eq(residual, w)
    return c, [float(v) for v in tone], [list(b) for b in eq]


class _T:
    def __init__(self, analysis):
        self.analysis = analysis


def build_case(a):
    spec, split, render_workers = a
    path = POOL / f"{spec['id']}.json"
    if path.exists():
        return spec["id"]
    t0 = time.time()
    case = Case(spec, split)
    ta = tm.Analysis(case.target)
    jobs = [(x, s, g) for x in (case.take, case.ev) for s in range(3) for g in GAINS]
    tm.render_many(jobs, workers=render_workers)
    cands = []
    for s in range(3):
        for g in GAINS:
            y = tm.render(case.take, s, g)
            ya = tm.Analysis(y)
            cabs = tm.screen_cabs(ya, ta.ltas, ta.weights, CABS_PER)
            yev = tm.render(case.ev, s, g)
            for cab in cabs:
                c, tone, eq = fit_linear_like_matcher(ta, y, cab)
                f = case.true_facets(yev, tone, eq, cab)
                cands.append(dict(slot=s, gain=g, cab=str(cab), tone=tone, eq=eq, spectral=c.spectral, nl=c.nl,
                                  features=[float(v) for v in c.analysis.features()], true=f))
    path.write_text(json.dumps(dict(id=spec["id"], split=split, style=spec["style"], production=spec["production"],
                                    pedal=spec["pedal"]["kind"] if spec["pedal"] else None, bus_comp=bool(spec.get("bus_comp")),
                                    candidates=cands), indent=0, default=float))
    print(f"  {spec['id']}: {len(cands)} candidates in {time.time() - t0:.0f} s", flush=True)
    return spec["id"]


def load(split, cases=None):
    specs = [s for s in cs.load_specs() if s["split"] == split and (cases is None or s["id"] in cases)]
    return [json.loads((POOL / f"{s['id']}.json").read_text()) for s in specs if (POOL / f"{s['id']}.json").exists()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["build"])
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--cases")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--render-workers", type=int, default=5)
    args = ap.parse_args()
    POOL.mkdir(parents=True, exist_ok=True)
    specs = [s for s in cs.load_specs() if s["split"] == args.split]
    if args.cases:
        ids = set(args.cases.split(","))
        specs = [s for s in specs if s["id"] in ids]
    if args.cmd == "build":
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            list(pool.map(build_case, [(s, args.split, args.render_workers) for s in specs]))


if __name__ == "__main__":
    main()

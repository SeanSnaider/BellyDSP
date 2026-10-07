# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""Round 2, the match curve's fit (docs/TONE_MATCH.md, "Round 2: the match curve"): its smoothing and its amount,
on the results of a run with the curve (run.py --config any_fx_curve). Per case, the run's settings with the curve
refitted (tone_match.fit_match_curve at 1/12 and 1/6 octave) and played at 50, 75, and 100% (the block's amount
scales the dB exactly), against the hidden rig (metrics.facets). The post compressor stays as the run chose it.

    uv run prototypes/tone_bench/curve_study.py --split dev [--run dev_r2_curve]
"""

import argparse
import concurrent.futures
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402

import cases as cs  # noqa: E402
import matcher as mt  # noqa: E402
import metrics as mx  # noqa: E402
import pool as pl  # noqa: E402
from common import OUT, tm  # noqa: E402

AMOUNTS = (0.5, 0.75, 1.0)
SMOOTHING = (12.0, 6.0)


def job(a):
    spec, row = a
    case = pl.Case(spec, spec["split"])
    s = dict(row["configs"]["any_fx_curve"]["settings"])
    take = case.take
    ta = tm.Analysis(case.target if not row["configs"]["any_fx_curve"].get("notes") else mt.cleanup(case.target, take)[0])
    flat = dict(s, match_curve=None, post_comp=None)
    cand = tm.Analysis(mt.render_settings(take, flat)).ltas_bins
    out = {}
    for frac in SMOOTHING:
        old = tm.smooth_octave_bins.__defaults__
        tm.smooth_octave_bins.__defaults__ = (frac,)
        points = tm.fit_match_curve(ta, cand)
        tm.smooth_octave_bins.__defaults__ = old
        for amount in AMOUNTS:
            p = [[f, d * amount] for f, d in points]
            y = mt.render_settings(case.ev, dict(s, match_curve=p, match_curve_amount=100.0))
            out[f"1/{frac:.0f} oct, {amount:.0%}"] = mx.facets(case.ref, y)["combined"]
    y = mt.render_settings(case.ev, dict(s, match_curve=None))
    out["no curve (tone only)"] = mx.facets(case.ref, y)["combined"]
    print(f"  {spec['id']}: " + ", ".join(f"{k} {v:.3f}" for k, v in out.items()), flush=True)
    return spec["id"], out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--run", default=None)
    ap.add_argument("--workers", type=int, default=6)
    args = ap.parse_args()
    run = OUT / (args.run or f"{args.split}_r2_curve") / "cases"
    specs = [s for s in cs.load_specs() if s["split"] == args.split]
    jobs = [(s, json.loads((run / f"{s['id']}.json").read_text())) for s in specs]
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        res = dict(pool.map(job, jobs))
    (OUT / f"curve_study_{args.split}.json").write_text(json.dumps(res, indent=1))
    for k in next(iter(res.values())):
        v = [r[k] for r in res.values()]
        print(f"{k}: median {np.median(v):.3f}, mean {np.mean(v):.3f}")


if __name__ == "__main__":
    main()

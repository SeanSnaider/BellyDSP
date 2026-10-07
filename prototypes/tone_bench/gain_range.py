# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""Round 2, the gain range (docs/TONE_MATCH.md): would letting the matcher drive the built-in amps past the
Gain knob's +24 dB close part of the coverage gap? Round 1's oracle put its best Gain at +24 dB in 17 of 50
cases. Here, per case, every built-in amp is pushed further with the input trim (ampsim_render --input-gain,
the chain's own input level, in front of the amp) at +4, +8, and +12 dB on top of Gain +24 (so 28, 32, 36),
the cabs screened and the linear part fitted as in pool.py, and the best true score with the extended range
is compared with the pool's best (the same grid up to +24).

    uv run prototypes/tone_bench/gain_range.py --split dev
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
import pool as pl  # noqa: E402
from common import GAIN_SETS, OUT, ampsim_render, tm  # noqa: E402

EXTRA = [4.0, 8.0, 12.0]


def render_hot(x, slot, extra):
    return ampsim_render(x, ["--model", GAIN_SETS[slot], "--trim", "24.000", "--tone", "0,0,0,0,0", "--input-gain", f"{extra:.3f}"], "hot")


def job(a):
    spec, split = a
    d = json.loads((pl.POOL / f"{spec['id']}.json").read_text())
    case = pl.Case(spec, split)
    ta = tm.Analysis(case.target)
    rows = []
    for s in range(3):
        for e in EXTRA:
            y, yev = render_hot(case.take, s, e), render_hot(case.ev, s, e)
            for cab in tm.screen_cabs(tm.Analysis(y), ta.ltas, ta.weights, pl.CABS_PER):
                c, tone, eq = pl.fit_linear_like_matcher(ta, y, cab)
                f = case.true_facets(yev, tone, eq, cab)
                rows.append(dict(slot=s, gain=24.0 + e, cab=str(cab), true=f["combined"], spectral=c.spectral, nl=c.nl))
    t = [x["true"]["combined"] for x in d["candidates"]]
    best = d["candidates"][int(np.argmin(t))]
    hot = min(rows, key=lambda r: r["true"])
    out = dict(id=spec["id"], style=spec["style"], pool_best=float(min(t)), pool_best_gain=best["gain"], hot_best=hot["true"], hot_gain=hot["gain"],
               extended_best=float(min(min(t), hot["true"])))
    print(f"  {spec['id']}: pool best {out['pool_best']:.3f} (Gain {best['gain']:+.0f}), past +24: {hot['true']:.3f} (Gain {hot['gain']:+.0f})", flush=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--workers", type=int, default=5)
    args = ap.parse_args()
    specs = [s for s in cs.load_specs() if s["split"] == args.split and (pl.POOL / f"{s['id']}.json").exists()]
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        rows = list(pool.map(job, [(s, args.split) for s in specs]))
    (OUT / f"gain_range_{args.split}.json").write_text(json.dumps(rows, indent=1))
    top = [r for r in rows if r["pool_best_gain"] >= 24.0]
    print(f"pool best median {np.median([r['pool_best'] for r in rows]):.3f}, with the range to +36: {np.median([r['extended_best'] for r in rows]):.3f}; "
          f"on the {len(top)} cases whose pool best sits at +24: {np.mean([r['pool_best'] for r in top]):.3f} -> "
          f"{np.mean([r['extended_best'] for r in top]):.3f} (mean); better past +24 in {sum(r['hot_best'] < r['pool_best'] for r in rows)} of {len(rows)}")


if __name__ == "__main__":
    main()

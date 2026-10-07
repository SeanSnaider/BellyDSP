# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""The bleed detector study (docs/TONE_MATCH.md, "tone_bench, Round 2"): when should the cleanup with the take
run? Round 1 found the informed mask hurts on a target that's already clean (chords, bends, a saturated amp's
inter-harmonic energy) and helps only where there's something to remove. This measures candidate features per
case and how well each predicts the cleanup's effect on the combined score (any_clean minus any_raw, from the
Round 1 results in build/tone_bench/<split>_current).

    uv run prototypes/tone_bench/bleed.py --split dev|test

Features (all from what the app has at match time: the target, the take, and the matcher's raw pick):
  kept_target   10 log10 of the cleaned target's energy over the raw target's (dB, <= 0): how much the mask removes
  kept_proxy    the same for a bleed-free stand-in for the target: the take through the raw pick (no bleed by
                construction, the same notes, about the same distortion), masked with the take's own notes
  excess        kept_proxy - kept_target: what the mask removes beyond what this tone loses on its own (dB)
  separated     the target is a separated stem
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
from common import OUT, tm  # noqa: E402


def energy_db(x):
    return 10.0 * np.log10(np.sum(np.square(x)) + 1e-20)


def features(spec, settings):
    """The candidate bleed features for one case (module docstring)."""
    data = cs.build(spec, None if spec["production"] != "mix" else (lambda x: None))
    target, take = data["target"], data["perf"]["b"]
    cleaned, _ = mt.cleanup(target, take)
    proxy = mt.render_settings(take, settings)
    proxy_clean, _ = mt.cleanup(proxy, take)
    kt = energy_db(cleaned) - energy_db(target)
    kp = energy_db(proxy_clean) - energy_db(proxy)
    return dict(kept_target=kt, kept_proxy=kp, excess=kp - kt, separated=spec["production"] == "mix")


def job(a):
    spec, row = a
    f = features(spec, row["configs"]["any_raw"]["settings"])
    f["delta"] = row["configs"]["any_clean"]["facets"]["combined"] - row["configs"]["any_raw"]["facets"]["combined"]
    f["id"] = spec["id"]
    f["production"] = spec["production"]
    print(f"  {spec['id']}: kept {f['kept_target']:.2f} proxy {f['kept_proxy']:.2f} excess {f['excess']:+.2f} delta {f['delta']:+.3f}", flush=True)
    return f


def study(split, workers):
    specs = [s for s in cs.load_specs() if s["split"] == split]
    folder = OUT / f"{split}_current" / "cases"
    rows = {s["id"]: json.loads((folder / f"{s['id']}.json").read_text()) for s in specs}
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
        out = list(pool.map(job, [(s, rows[s["id"]]) for s in specs]))
    path = OUT / f"bleed_{split}.json"
    path.write_text(json.dumps(out, indent=1, default=float))
    print(f"wrote {path}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--workers", type=int, default=6)
    args = ap.parse_args()
    study(args.split, args.workers)


if __name__ == "__main__":
    main()

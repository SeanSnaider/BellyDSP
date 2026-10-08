# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""The search with eight amps (docs/TONE_MATCH.md, "Round 2: eight amps"): search designs simulated on the exhaustive
run's traces, and the score's preferences against the truth.

    uv run --no-project --with scipy python prototypes/tone_bench/search_study.py [--runs build/tone_bench] [--oracle <coverage cases>]

Reads <runs>/dev_r2_all (any_fx_curve_all: every amp refined, the pedals in front of every amp, every candidate fitted
and scored by S, traced), dev_r2_curve50 (three amps), and dev_r2_amps (eight amps, the Round 2 search), all
any_fx_curve. A design's candidates are a subset of the exhaustive run's with the same values, so its pick (the lowest
S in its shortlist) follows from the trace. The trace's rows are (slot, Gain, cab, pedal, old score, S, S_pre[, S_pre
with the EQ's target]).
"""

import argparse
import collections
import glob
import json
import os
import statistics as st

AMPS = ["Glass", "Ember", "Monolith", "Basalt", "Comet", "Forge", "Lantern", "Quartz"]   # common.GAIN_SETS's order
GRID = {-18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0}
WEIGHTS = dict(spectral=1.0, flux=7.5, crest=4.0, attack=14.0, spread=2.5, erb=6.5)


def load(d, cfg):
    return {os.path.basename(p)[:-5]: json.load(open(p))["configs"][cfg] for p in glob.glob(os.path.join(d, "cases", "*.json"))}


def shortlist(pool, n, k_old, m_pre):
    """The old score's n best, each amp's k_old best by it, and S_pre's m_pre best."""
    keep = {id(c) for c in sorted(pool, key=lambda c: c[6])[:m_pre]}
    out, per = [], {}
    for i, c in enumerate(sorted(pool, key=lambda c: c[4])):
        q = per.get(c[0], 0)
        if i < n or q < k_old or id(c) in keep:
            out.append(c)
        per[c[0]] = q + 1
    return out


def design(rows, n=16, k_old=0, m_pre=0, pedal_old=1, pedal_pre=0, refine=None):
    """refine: None every amp's Gain; (r, "old" or "pre") the r best amps by that score on the grid."""
    plain = [c for c in rows if c[3] is None]
    if refine is not None:
        r, by = refine
        best = {}
        for c in plain:
            if c[1] in GRID:
                best[c[0]] = min(best.get(c[0], 1e9), c[4] if by == "old" else c[6])
        chosen = sorted(best, key=best.get)[:r]
        plain = [c for c in plain if c[1] in GRID or c[0] in chosen]
    best_old, best_pre = {}, {}
    for c in plain:
        best_old[c[0]] = min(best_old.get(c[0], 1e9), c[4])
        best_pre[c[0]] = min(best_pre.get(c[0], 1e9), c[6])
    slots = sorted(best_old, key=best_old.get)[:pedal_old]
    slots += [s for s in sorted(best_pre, key=best_pre.get)[:pedal_pre] if s not in slots]
    sl = shortlist(plain + [c for c in rows if c[3] is not None and c[0] in slots], n, k_old, m_pre)
    return min(sl, key=lambda c: c[5]), len(sl)


DESIGNS = [
    ("the Round 2 search (refine 2)", dict(refine=(2, "old"))),
    ("every amp refined", dict()),
    ("+ the old score's 4 best per amp", dict(k_old=4)),
    ("+ the old score's 8 best per amp", dict(k_old=8)),
    ("+ S_pre's 16 best", dict(m_pre=16)),
    ("+ S_pre's 16 best, pedals in front of S_pre's best amp too (the wide search)", dict(m_pre=16, pedal_pre=1)),
    ("+ S_pre's 16 best, pedals in front of S_pre's 2 best amps too", dict(m_pre=16, pedal_pre=2)),
    ("the wide search, Gain refined for S_pre's 3 best amps only", dict(m_pre=16, pedal_pre=1, refine=(3, "pre"))),
    ("everything (S's optimum)", dict(n=10 ** 6, pedal_old=8)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default="build/tone_bench")
    ap.add_argument("--oracle", help="the coverage study's cases folder (the oracle with every amp), for the score's preferences")
    args = ap.parse_args()
    ex = load(os.path.join(args.runs, "dev_r2_all"), "any_fx_curve_all")
    three = load(os.path.join(args.runs, "dev_r2_curve50"), "any_fx_curve")
    ids = sorted(i for i in ex if ex[i].get("trace") and i in three)
    same = [i for i in ids if three[i]["notes"] == ex[i]["notes"]]
    opt = {i: min(c[5] for c in ex[i]["trace"]["scored"]) for i in ids}
    print(f"{len(ids)} cases, {len(same)} with the same target as the three-amp run (Auto's cleanup decided the same)")
    for name, kw in DESIGNS:
        worse, at_opt, sizes = [], 0, []
        for i in ids:
            best, size = design(ex[i]["trace"]["scored"], **kw)
            sizes.append(size)
            at_opt += best[5] <= opt[i] + 1e-9
            if i in same and best[5] > three[i]["take_terms"]["total"] + 1e-6:
                worse.append(f"{i} {best[5] - three[i]['take_terms']['total']:+.2f}")
        print(f"  {name}: S worse than three amps in {len(worse)} {worse}; at S's optimum in {at_opt}; shortlist {min(sizes)} to {max(sizes)}")

    # The score's preferences: S's optimum (the exhaustive run's pick) against the three-amp pick and the oracle.
    d = {k: ([], []) for k in WEIGHTS}
    for i in same:
        a, b = ex[i]["take_terms"], three[i]["take_terms"]
        if abs(a["total"] - b["total"]) < 1e-9:
            continue
        g = 0 if ex[i]["facets"]["combined"] > three[i]["facets"]["combined"] + 0.005 else 1
        for k in WEIGHTS:
            d[k][g].append(WEIGHTS[k] * (a[k] - b[k]))
    for g, label in ((0, "truly further"), (1, "truly closer or the same")):
        n = len(d["attack"][g])
        print(f"S's optimum against the three-amp pick, {label} ({n}): weighted term differences "
              + ", ".join(f"{k} {st.mean(v[g]):+.2f}" for k, v in d.items()))
    picks = collections.Counter(AMPS[min(ex[i]["trace"]["scored"], key=lambda c: c[5])[0]] for i in ids)
    print("S's optimum's amp:", picks.most_common())
    if args.oracle:
        orc = {i: json.load(open(os.path.join(args.oracle, f"{i}.json")))["oracle"]["space"]["label"].split()[0] for i in ids}
        print("the oracle's amp (in space):", collections.Counter(orc.values()).most_common())
        for a in AMPS:
            sel = [i for i in ids if AMPS[min(ex[i]["trace"]["scored"], key=lambda c: c[5])[0]] == a]
            if sel:
                diff = [ex[i]["facets"]["combined"] - three[i]["facets"]["combined"] for i in sel]
                print(f"  {a}: S's pick in {len(sel)}, the oracle's too in {sum(orc[i] == a for i in sel)}; against the three-amp pick "
                      f"{st.mean(diff):+.3f} mean, further in {sum(x > 0.005 for x in diff)}")


if __name__ == "__main__":
    main()

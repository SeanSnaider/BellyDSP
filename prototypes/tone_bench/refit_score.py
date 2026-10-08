# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Refitting the take-aware score on the eight-amp pools (docs/TONE_MATCH.md, "Round 3: the score with eight amps").

    uv run --no-project --with numpy --with scipy python prototypes/tone_bench/refit_score.py [--split dev] [--amps 8]

A score is judged as Round 2 judged it (TONE_MATCH.md, "3. The take-aware score"): per case, the true score of the
pool candidate it ranks first; medians over the cases. Its weights are fitted by minimizing the soft-min of the true
score (sum over cases of the true scores weighted by softmax(-S / (tau std(S))), the case's own spread of S making
it scale-free), nonnegative, with the spectral error's weight fixed at 1, from the current weights and a few restarts; judged by leave-one-out (each case's pick by the
weights fitted on the others). Terms (pool8.py measures them; every one in dB, >= 0):
  flux, crest, attack, spread, erb   S's own (tone_match.take_score_of)
  nl        the old score's distortion distance (four whole-signal features, Round 1)
  hold      the envelope profile after the attack (notefeat.envelope_profile's 15-40, 40-90, 90-170, 170-300 ms windows):
            per note the mean |target - candidate| over those windows, averaged with the notes' loudness weights. How a
            note holds after its pick (sag, compression, the power amp's bloom), which the attack share alone doesn't see
  slope     the notes' level response to the player's picking: |b_t - b_c|, b the least-squares slope of the note's
            output level (dB) on the take's DI level for that note (dB), the target's (played by another player but
            the same notes) against the candidate's (the take through the amp). Compression and drive flatten it
"""

import argparse
import glob
import json
import os
import pathlib
import statistics as st

import numpy as np
from scipy.optimize import minimize

HERE = pathlib.Path(__file__).resolve().parent
POOL8 = HERE.parents[1] / "build/tone_bench/pool8"
CURRENT = dict(flux=7.5, crest=4.0, attack=14.0, spread=2.5, erb=6.5)


def slope(y, x):
    x = np.asarray(x) - np.mean(x)
    return float(np.dot(x, np.asarray(y) - np.mean(y)) / max(np.dot(x, x), 1e-9))


class Pool:
    """One case's candidates as arrays of terms."""

    def __init__(self, d, amps=8):
        self.id, self.style = d["id"], d["style"]
        t = d["target"]
        ta, tl, tw = np.array(t["attack"]), np.array(t["level"]), np.array(t["weight"])
        te, di = np.array(t["env"]), np.array(t["di_level"])
        rend = []
        for r in d["renders"]:
            att, lev, env = np.array(r["attack"]), np.array(r["level"]), np.array(r["env"])
            rend.append(dict(
                attack=float(np.sum(tw * np.abs(ta - att)) / np.sum(tw)),
                spread=abs(float(np.std(tl)) - float(np.std(lev))),
                hold=float(np.sum(tw * np.mean(np.abs(te[:, 1:] - env[:, 1:]), axis=1)) / np.sum(tw)),
                slope=abs(slope(tl, di) - slope(lev, di)),
            ))
        cands = [c for c in d["candidates"] if c["slot"] < amps]
        self.slot = np.array([c["slot"] for c in cands])
        self.gain = np.array([c["gain"] for c in cands])
        self.true = np.array([c["true"] for c in cands])
        self.terms = {k: np.array([c[k] for c in cands]) for k in ("spectral", "nl", "flux", "crest", "erb", "erb_pre")}
        for k in ("attack", "spread", "hold", "slope"):
            self.terms[k] = np.array([rend[c["render"]][k] for c in cands])
        self.usable = d["usable"]

    def score(self, w):
        s = self.terms["spectral"].copy()
        for k, v in w.items():
            s = s + v * self.terms[k]
        return s

    def pick(self, w):
        return int(np.argmin(self.score(w)))


def judge(pools, w):
    trues = [p.true[p.pick(w)] for p in pools]
    return st.median(trues), st.mean(trues)


def softmin_loss(pools, names, x, tau):
    w = dict(zip(names, x))
    loss = 0.0
    for p in pools:
        s = p.score(w)
        z = np.exp(-(s - s.min()) / (tau * max(float(np.std(s)), 1e-9)))
        loss += float(np.sum(z * p.true) / np.sum(z))
    return loss / len(pools)


def fit(pools, names, start, tau, restarts=5, seed=0):
    """Nelder-Mead on log weights (so each stays positive and can go to ~0), from the current weights and restarts
    around them; the lowest soft-min loss wins."""
    rng = np.random.default_rng(seed)
    x0 = np.log(np.array([max(start.get(k, 1.0), 1e-3) for k in names]))
    best = None
    for r in range(restarts + 1):
        x = x0 if r == 0 else x0 + rng.normal(0.0, 1.0, len(names))
        res = minimize(lambda u: softmin_loss(pools, names, np.exp(np.clip(u, -12.0, 6.0)), tau), x, method="Nelder-Mead",
                       options=dict(maxiter=600 * len(names), xatol=1e-3, fatol=1e-6))
        if best is None or res.fun < best.fun:
            best = res
    return dict(zip(names, np.exp(np.clip(best.x, -12.0, 6.0))))


def loo(pools, names, start, tau):
    trues, weights = [], []
    for i, p in enumerate(pools):
        w = fit(pools[:i] + pools[i + 1:], names, start, tau, restarts=2, seed=i)
        trues.append(p.true[p.pick(w)])
        weights.append(w)
    return st.median(trues), st.mean(trues), weights


def grid_loo(pools, grids, make):
    """Grid search over len(grids) parameters (make(values) -> weights), the training objective the mean true score of
    the hard picks; leave-one-out. Returns (held-out median, held-out mean, the values fitted on all)."""
    import itertools
    combos = list(itertools.product(*grids))
    table = np.array([[p.true[p.pick(make(c))] for p in pools] for c in combos])
    held = [table[int(np.argmin(np.delete(table, i, axis=1).mean(axis=1))), i] for i in range(len(pools))]
    return st.median(held), st.mean(held), combos[int(np.argmin(table.mean(axis=1)))]


def study(raw):
    """The low-dimensional refits, what S separates (the amp against Gain and cab), and two-stage amp rules."""
    from scipy.stats import spearmanr
    pools = [Pool(d, 8) for d in raw]
    for p in pools:
        p.terms["take"] = sum(v * p.terms[k] for k, v in CURRENT.items())
    lam = list(np.round(np.arange(0.0, 3.01, 0.1), 2))
    med, mean, c = grid_loo(pools, [lam], lambda c: dict(take=c[0]))
    print(f"  S = E + l (the current take terms): leave-one-out median {med:.3f}, mean {mean:.3f} (l = {c[0]} on all)")
    for x in ("hold", "slope", "nl", "attack", "erb", "erb_pre", "crest"):
        med, mean, c = grid_loo(pools, [lam[::2], [0.0, 0.5, 1, 2, 4, 8, 16, 32]], lambda c, x=x: dict(take=c[0], **{x: c[1]}))
        print(f"  S = E + l take + m {x}: leave-one-out median {med:.3f}, mean {mean:.3f} ({c} on all)")
    for x in CURRENT:
        med, mean, c = grid_loo(pools, [[0.0, 0.25, 0.5, 1.0, 1.5, 2.0, 3.0]], lambda c, x=x: {**CURRENT, x: CURRENT[x] * c[0]})
        print(f"  the current S with {x}'s weight times f: leave-one-out median {med:.3f}, mean {mean:.3f} (f = {c[0]} on all)")
    amps = raw[0]["amps"]
    for n in (3, 8):
        ps = [Pool(d, n) for d in raw]
        total, within, amp_err, rho, right = [], [], [], [], 0
        for p in ps:
            sc = p.score(CURRENT)
            k, b = int(np.argmin(sc)), int(np.argmin(p.true))
            sel = np.flatnonzero(p.slot == p.slot[b])
            kk = sel[int(np.argmin(sc[sel]))]
            total.append(p.true[k] - p.true[b])
            within.append(p.true[kk] - p.true[b])
            amp_err.append(p.true[k] - p.true[kk])
            right += p.slot[k] == p.slot[b]
            rho.append(spearmanr([sc[p.slot == a].min() for a in range(n)], [p.true[p.slot == a].min() for a in range(n)]).correlation)
        print(f"  {n} amps: S's pick over the pool's best {st.mean(total):.3f} (mean): Gain and cab within the best amp {st.mean(within):.3f}, "
              f"the amp {st.mean(amp_err):.3f}; the best amp in {right} of {len(ps)}; Spearman of the amps' best S against their best "
              f"true score: median {st.median(rho):.2f}")
    import collections
    print("  the pool's best amp:", collections.Counter(amps[p.slot[int(np.argmin(p.true))]] for p in pools).most_common())
    print("  S's amp:", collections.Counter(amps[p.slot[p.pick(CURRENT)]] for p in pools).most_common())
    gaps = [sorted(p.true[p.slot == a].min() for a in range(8))[1] - p.true.min() for p in pools]
    print(f"  the best amp's margin over the next best, in truth: median {st.median(gaps):.3f}")

    def two_stage(p, key):
        sc = p.score(CURRENT)
        a = min(range(8), key=lambda a: key(p, sc, a))
        sel = np.flatnonzero(p.slot == a)
        return p.true[sel[int(np.argmin(sc[sel]))]]
    rules = {"S": lambda p, sc, a: sc[p.slot == a].min(),
             "the old score": lambda p, sc, a: (p.terms["spectral"] + 0.5 * p.terms["nl"])[p.slot == a].min(),
             "the mean of its 3 best S": lambda p, sc, a: np.sort(sc[p.slot == a])[:3].mean()}
    for t in ("spectral", "nl", "attack", "crest", "erb"):
        rules[t] = lambda p, sc, a, t=t: p.terms[t][p.slot == a].min()
    for name, key in rules.items():
        v = [two_stage(p, key) for p in pools]
        print(f"  the amp by {name}, Gain and cab by S: median {st.median(v):.3f}, mean {st.mean(v):.3f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--split", default="dev")
    ap.add_argument("--amps", type=int, default=8)
    ap.add_argument("--tau", type=float, nargs="*", default=[0.1])
    ap.add_argument("--sets", default="current,hold,slope,nl,hold+slope,drop")
    ap.add_argument("--study", action="store_true", help="the low-dimensional refits and what S separates (eight amps)")
    args = ap.parse_args()
    if args.study:
        raw = [json.load(open(p)) for p in sorted(glob.glob(str(POOL8 / "*.json")))]
        study([d for d in raw if d["usable"] and d["split"] == args.split])
        return
    pools = [Pool(json.load(open(p)), args.amps) for p in sorted(glob.glob(str(POOL8 / "*.json")))]
    pools = [p for p in pools if p.usable and json.load(open(POOL8 / f"{p.id}.json"))["split"] == args.split]
    print(f"{len(pools)} pools ({args.split}, {args.amps} amps, {len(pools[0].true)} candidates each)")
    print(f"  the pool's best: median {st.median(p.true.min() for p in pools):.3f}")
    print("  the old score: median %.3f, mean %.3f" % judge(pools, dict(nl=0.5)))
    print("  the current S: median %.3f, mean %.3f" % judge(pools, CURRENT))
    sets = dict(current=list(CURRENT), hold=list(CURRENT) + ["hold"], slope=list(CURRENT) + ["slope"], nl=list(CURRENT) + ["nl"],
                **{"hold+slope": list(CURRENT) + ["hold", "slope"]})
    sets["drop"] = None
    for tau in args.tau:
        for name in args.sets.split(","):
            if name == "drop":
                for k in CURRENT:
                    names = [n for n in CURRENT if n != k]
                    med, mean, _ = loo(pools, names, CURRENT, tau)
                    print(f"  tau {tau}: refit without {k}: leave-one-out median {med:.3f}, mean {mean:.3f}")
                continue
            names = sets[name]
            w = fit(pools, names, CURRENT, tau)
            med_in, mean_in = judge(pools, w)
            med, mean, ws = loo(pools, names, CURRENT, tau)
            l_cur = softmin_loss(pools, names, np.array([CURRENT.get(k, 0.0) for k in names]), tau)
            l_fit = softmin_loss(pools, names, np.array([w[k] for k in names]), tau)
            print(f"  tau {tau}: {name}: soft-min loss {l_cur:.4f} (current) -> {l_fit:.4f}; fitted on all {', '.join(f'{k} {v:.2f}' for k, v in w.items())}: in-sample median {med_in:.3f} (mean {mean_in:.3f}); "
                  f"leave-one-out median {med:.3f}, mean {mean:.3f}")


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "demucs", "lameenc"]
# ///

"""The tone match benchmark (docs/TONE_MATCH.md, "tone_bench"): hidden rigs the matcher has never seen,
realistic productions, the player's take, and a held-out DI to judge on.

    uv run prototypes/tone_bench/run.py --split dev|test --matcher current [--config same_clean,any_raw,...]
        [--cases id,id] [--workers 4] [--no-diagnose]
    uv run prototypes/tone_bench/run.py --make-cases        # (re)write cases.json: the specs, calibrated
    uv run prototypes/tone_bench/run.py --calibrate         # the wrong rig's facet medians on DEV (metrics.SCALES)
    uv run prototypes/tone_bench/run.py --report --split dev   # the report again from the saved per-case results

Writes build/tone_bench/<split>_<matcher>/report.md and results.json, and one JSON per case in cases/ there
(a rerun skips the cases already done; delete the folder to start over). Renders, separations, and records
are cached under build/tone_bench/cache. Needs the Release build of ampsim_render (cmake --build build -j).

Per case:
  1. the record (cases.build): the hidden rig on the record's guitarist (A), produced as its spec says,
     separated by Demucs if it's a mix; the player's play-along take B; the held-out DI C;
  2. the evaluation signal (metrics.eval_signal: C and the probes) through the hidden rig: the reference;
  3. anchors: the hidden rig itself (must score 0), a deliberately wrong rig (another case's, another style),
     and "nothing matched" (each built-in amp at Gain 0, tone flat, the factory cab, no EQ);
  4. the matcher in each configuration (matcher.CONFIGS), its result rendered on the evaluation signal;
  5. the diagnosis (unless --no-diagnose): the ladder (the matcher on the target without the production,
     and on the rig played by the player himself), the oracle configurations (oracle.py), and the
     objective test (the matcher's own score of the oracle's configuration against its pick's).
"""

import argparse
import concurrent.futures
import json
import math
import os
import pathlib
import sys
import time
import traceback

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402

import cases as cs  # noqa: E402
import matcher as mt  # noqa: E402
import metrics as mx  # noqa: E402
import oracle as orc  # noqa: E402
import rigs  # noqa: E402
from common import OUT, SR, tm  # noqa: E402

LADDER_CONFIGS = ["same_clean", "any_clean"]


def separate(x):
    return tm.separate_guitar(x)


def reference_for(spec, ev):
    tone = rigs.tone_rig(ev, spec)
    full = rigs.time_fx(tone, spec.get("time_fx"), spec["seed"]) if spec.get("time_fx") else tone
    return tone, full


def wrong_rig_for(spec, specs):
    i = spec["index"]
    for k in range(1, len(specs)):
        other = specs[(i + 17 * k) % len(specs)]
        if other["style"] != spec["style"]:
            return other
    raise RuntimeError("no other style")


def describe(s):
    if s is None:
        return None
    if "slot" in s:
        cab = pathlib.Path(s["cab"]).stem if s.get("cab") not in (None, "true") else s.get("cab")
        return f"{tm.AMPS[s['slot']]} {s['gain']:+.1f}, {cab}"
    return s.get("cab") and pathlib.Path(s["cab"]).stem


def evaluate_case(spec, specs, split, configs, diagnose, workers):
    t0 = time.time()
    data = cs.build(spec, separate)
    perf = data["perf"]
    ev, lay = mx.eval_signal(perf["c"], perf["c_onsets"])
    tone_ev, full_ev = reference_for(spec, ev)
    ref = mx.Reference(tone_ev, full_ev, lay)
    row = dict(id=spec["id"], style=spec["style"], production=spec["production"], split=spec["split"],
               pedal=spec["pedal"]["kind"] if spec["pedal"] else None, amp=spec["amp"]["name"], amp_kind=spec["amp"]["kind"],
               cab=spec["cab"]["base_name"] + (" (modified: " + ", ".join(m[0] for m in spec["cab"]["mods"]) + ")" if spec["cab"]["mods"] else ""),
               time_fx=spec["time_fx"]["kind"] if spec.get("time_fx") else None, bus_comp=bool(spec.get("bus_comp")),
               double=bool(spec.get("double")), codec=spec.get("codec"), nl_target=spec["nl_target"], nl_reached=spec["amp"]["nl_reached"])

    # Anchors.
    row["anchors"] = {"oracle": mx.facets(ref, tone_ev)}
    wrong = wrong_rig_for(spec, specs)
    row["anchors"]["wrong_rig"] = mx.facets(ref, rigs.tone_rig(ev, wrong))
    row["anchors"]["wrong_rig"]["which"] = wrong["id"]
    defaults = []
    for slot in range(3):
        s = dict(slot=slot, gain=0.0, tone=[0.0] * 5, cab=str(tm.DEFAULT_CAB), eq=[])
        defaults.append(mx.facets(ref, mt.render_settings(ev, s)))
    row["anchors"]["default_mean"] = {k: float(np.mean([d[k] for d in defaults])) for k in defaults[0]}
    row["anchors"]["default_best"] = min(defaults, key=lambda d: d["combined"])

    # Separation and production quality, against the dry record.
    from learn_tone import si_sdr
    if spec["production"] != "dry":
        row["target_vs_dry"] = dict(si_sdr=si_sdr(data["dry"], data["target"]), legacy=mx.lt.spectral_distance(data["dry"], data["target"])[0])

    timing = {"build_and_anchors": round(time.time() - t0, 1)}
    # The matcher.
    t1 = time.time()
    take = perf["b"]
    row["configs"] = {}
    targets = {}
    for cfg in configs:
        r, used = mt.run(data["target"], take, cfg, split, workers)
        s = mt.result_settings(r)
        y = mt.render_settings(ev, s)
        f = mx.facets(ref, y)
        mode = mt.CONFIGS[cfg]["mode"]
        band = tm.PLAY_ALONG_BAND_SECONDS if mode == "same" else None
        row["configs"][cfg] = dict(facets=f, settings=s, label=describe(s), closeness=r["closeness"], notes=r["notes"],
                                   objective=mt.objective(used, take, mode, band, s["slot"], s["gain"], s["cab"]),
                                   matcher_spectral_after_eq=r["spectral_error_after_eq_db"], matcher_distortion=r["distortion_distance"])
        targets[cfg] = used
        if cfg == configs[0]:
            # The linear shortcut (oracle.apply_linear) against the real render, on this result: same signal?
            lin = orc.apply_linear(tm.render(ev, s["slot"], s["gain"]), s["tone"], s["eq"], orc.cab_of(s["cab"]))
            n = min(len(lin), len(y))
            # The cab block normalizes its IR's loudness; compare after a least-squares gain.
            g = float(np.dot(lin[:n], y[:n]) / np.dot(lin[:n], lin[:n]))
            row["linear_check_snr_db"] = float(10 * np.log10(np.sum(y[:n] ** 2) / np.sum((y[:n] - g * lin[:n]) ** 2)))

    timing["matcher"] = round(time.time() - t1, 1)
    if diagnose:
        t1 = time.time()
        cabs = rigs.searchable_irs(split)
        pre = rigs.tone_rig(ev, spec, stages=("pedal", "amp", "cab", "eq"))
        comp_level = 10 * np.log10(np.mean(mx.seg(pre, lay, "c") ** 2) + 1e-20)
        ors = orc.all_oracles(ev, ref, spec, cabs, comp_level, workers)
        row["oracle"] = {k: dict(facets=v[0], settings=v[1], label=describe(v[1])) for k, v in ors.items()}
        timing["oracle"] = round(time.time() - t1, 1)
        t1 = time.time()
        row["ladder"] = {}
        for cfg in [c for c in LADDER_CONFIGS if c in configs]:
            mode = mt.CONFIGS[cfg]["mode"]
            band = tm.PLAY_ALONG_BAND_SECONDS if mode == "same" else None
            lad = {}
            for name, tgt in (("dry_target", data["dry"]), ("players_guitar", data["dry_gb"]), ("same_performance", data["rig_b"])):
                c2 = cfg.replace("clean", "raw") if name == "same_performance" else cfg
                r, _ = mt.run(tgt, take, c2, split, workers)
                s = mt.result_settings(r)
                lad[name] = dict(facets=mx.facets(ref, mt.render_settings(ev, s)), label=describe(s))
            # The objective test: the matcher's own score of the oracle's in-space configuration, on the
            # matcher's own target, against its pick's.
            o = ors["space"][1]
            lad["objective_oracle"] = mt.objective(targets[cfg], take, mode, band, o["slot"], o["gain"], o["cab"])
            lad["objective_pick"] = row["configs"][cfg]["objective"]
            row["ladder"][cfg] = lad
        timing["ladder"] = round(time.time() - t1, 1)
    row["timing"] = timing
    row["seconds"] = round(time.time() - t0, 1)
    return row


def worker(args):
    spec, specs, split, configs, diagnose, workers, out_dir = args
    path = out_dir / "cases" / f"{spec['id']}.json"
    if path.exists():
        return json.loads(path.read_text())
    try:
        row = evaluate_case(spec, specs, split, configs, diagnose, workers)
    except Exception:
        traceback.print_exc()
        raise
    path.write_text(json.dumps(row, indent=1, default=float))
    print(f"  {spec['id']}: done in {row['seconds']} s: " + ", ".join(f"{k} {v['facets']['combined']:.3f}" for k, v in row["configs"].items()), flush=True)
    return row


def oracle_worker(a):
    """--oracle-only: a saved case (from --from) with its oracle configurations and estimates computed again
    with the amps now available (--amps all: every built-in gain set). The matcher's results, the anchors, and
    the ladder are kept as they were (the matcher still searches the three slot defaults); the case's earlier
    oracle and estimates are kept as oracle_before and estimates_before."""
    import estimates
    spec, src, dst, split, workers = a
    if dst.exists():
        return json.loads(dst.read_text())
    t0 = time.time()
    row = json.loads(src.read_text())
    data = cs.build(spec, separate)
    perf = data["perf"]
    ev, lay = mx.eval_signal(perf["c"], perf["c_onsets"])
    tone_ev, full_ev = reference_for(spec, ev)
    ref = mx.Reference(tone_ev, full_ev, lay)
    cabs = rigs.searchable_irs(split)
    pre = rigs.tone_rig(ev, spec, stages=("pedal", "amp", "cab", "eq"))
    comp_level = 10 * np.log10(np.mean(mx.seg(pre, lay, "c") ** 2) + 1e-20)
    ors = orc.all_oracles(ev, ref, spec, cabs, comp_level, workers)
    row["oracle_before"] = row.get("oracle")
    row["oracle"] = {k: dict(facets=v[0], settings=v[1], label=describe(v[1])) for k, v in ors.items()}
    row["oracle_amps"] = list(tm.AMPS)
    row["estimates_before"] = row.get("estimates")
    row["estimates"] = estimates.estimate_case(row, split)
    row["oracle_seconds"] = round(time.time() - t0, 1)
    dst.write_text(json.dumps(row, indent=1, default=float))
    print(f"  {spec['id']}: oracle again in {row['oracle_seconds']} s: space {row['oracle_before']['space']['facets']['combined']:.3f} -> "
          f"{row['oracle']['space']['facets']['combined']:.3f} ({row['oracle']['space']['label']}), amp only "
          f"{row['oracle_before']['amp_only']['facets']['combined']:.3f} -> {row['oracle']['amp_only']['facets']['combined']:.3f}", flush=True)
    return row


def coverage_report(rows, split, out_dir):
    """--oracle-only's report: what the amps now available change in the oracle configurations and the error
    decomposition (docs/TONE_MATCH.md, "Diagnosis"), against the saved run's."""
    recombine(rows)
    for r in rows:
        for v in (r.get("oracle_before") or {}).values():
            if isinstance(v.get("facets"), dict) and all(k in v["facets"] for k in mx.FACETS):
                v["facets"]["combined"] = mx.combined(v["facets"])
    m = lambda v: float(np.mean(v)) if v else float("nan")
    get = lambda r, which, o: r[which][o]["facets"]["combined"]
    lines = [f"# tone_bench coverage: {split.upper()}, the oracle with {', '.join(rows[0]['oracle_amps'])}", "",
             f"{len(rows)} cases. Before: the saved run's oracle (the three slot defaults); after: every built-in gain set. "
             "The matcher's results and the ladder are the saved run's (it still searches the three).", ""]
    groups = {"all": rows}
    for r in rows:
        groups.setdefault(r["style"], []).append(r)
    lines += ["| Group | n | L3 space before | after | L4 full before | after | amp only before | after | amp-only change |", "|---|---|---|---|---|---|---|---|---|"]
    for g in ["all", "clean", "edge", "crunch", "high_gain", "lead"]:
        rs = groups.get(g, [])
        if not rs:
            continue
        cells = []
        for o in ("space", "full", "amp_only"):
            cells += [fmt(m([get(r, "oracle_before", o) for r in rs])), fmt(m([get(r, "oracle", o) for r in rs]))]
        d = m([get(r, "oracle", "amp_only") - get(r, "oracle_before", "amp_only") for r in rs])
        lines.append(f"| {g} | {len(rs)} | " + " | ".join(cells) + f" | {fmt(d)} |")
    lines += ["", "Means (combined score). Medians, all cases: "
              + ", ".join(f"{o} {fmt(med([get(r, 'oracle_before', o) for r in rows]))} -> {fmt(med([get(r, 'oracle', o) for r in rows]))}"
                          for o in ("space", "full", "amp_only")), ""]
    # The decomposition's last share: what's left at L4 (amp and cab coverage), before and after.
    for cfg in ("same_clean", "any_clean"):
        if cfg not in rows[0]["configs"]:
            continue
        l0 = m([r["configs"][cfg]["facets"]["combined"] for r in rows])
        before, after = m([get(r, "oracle_before", "full") for r in rows]), m([get(r, "oracle", "full") for r in rows])
        sp_b, sp_a = m([get(r, "oracle_before", "space") for r in rows]), m([get(r, "oracle", "space") for r in rows])
        lines.append(f"- {cfg}: L0 {fmt(l0)}; left at L4 (amp and cab coverage) {fmt(before)} ({100 * before / l0:.0f}% of L0) -> {fmt(after)} "
                     f"({100 * after / l0:.0f}%); L3 {fmt(sp_b)} -> {fmt(sp_a)}")
    lines.append("")
    # What the oracle picks now.
    picks = {}
    for o in ("space", "amp_only"):
        for r in rows:
            name = r["oracle_amps"][r["oracle"][o]["settings"]["slot"]]
            picks.setdefault(o, {}).setdefault(name, 0)
            picks[o][name] += 1
    for o, counts in picks.items():
        lines.append(f"- the oracle's {o} amp: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: -kv[1])))
    top_b = sum(r["oracle_before"]["space"]["settings"]["gain"] >= 23.0 for r in rows)
    top_a = sum(r["oracle"]["space"]["settings"]["gain"] >= 23.0 for r in rows)
    lines += [f"- the in-space Gain at the top (+23 to 24 dB): {top_b} of {len(rows)} before, {top_a} after", ""]
    lines += ["| Case | style | hidden amp | space before | after | amp only before | after |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append(f"| {r['id']} | {r['style']} | {r['amp']} | {fmt(get(r, 'oracle_before', 'space'))} ({r['oracle_before']['space']['label']}) | "
                     f"{fmt(get(r, 'oracle', 'space'))} ({r['oracle']['space']['label']}) | {fmt(get(r, 'oracle_before', 'amp_only'))} "
                     f"({r['oracle_before']['amp_only']['label']}) | {fmt(get(r, 'oracle', 'amp_only'))} ({r['oracle']['amp_only']['label']}) |")
    if all(r.get("estimates") and r.get("estimates_before") for r in rows):
        lines += ["", "| Estimate | before | after (means) |", "|---|---|---|"]
        for k in ("hires_pick", "hires_space", "hires_cab"):
            lines.append(f"| {k} | {fmt(m([r['estimates_before'][k]['combined'] for r in rows]))} | {fmt(m([r['estimates'][k]['combined'] for r in rows]))} |")
    lines.append("")
    (out_dir / "coverage.md").write_text("\n".join(lines))
    print("\n".join(lines))
    print(f"wrote {out_dir / 'coverage.md'}")


def estimate_worker(a):
    import estimates
    path, split = a
    row = json.loads(path.read_text())
    if "estimates" not in row:
        row["estimates"] = estimates.estimate_case(row, split)
        path.write_text(json.dumps(row, indent=1, default=float))
        print(f"  {row['id']}: estimates done", flush=True)
    return row["id"]


# ---- Report -----------------------------------------------------------------------------------------------

def med(v):
    v = [x for x in v if x is not None and not (isinstance(x, float) and math.isnan(x))]
    return float(np.median(v)) if v else float("nan")


def fmt(v, d=3):
    return "n/a" if v is None or (isinstance(v, float) and math.isnan(v)) else f"{v:.{d}f}"


def recombine(rows):
    """The combined score again from the stored facets, with the current metrics.SCALES and WEIGHTS."""
    def fix(f):
        if isinstance(f, dict) and all(k in f for k in mx.FACETS):
            f["combined"] = mx.combined(f)
    for r in rows:
        for v in r["anchors"].values():
            fix(v)
        for v in r["configs"].values():
            fix(v["facets"])
        for v in r.get("oracle", {}).values():
            fix(v["facets"])
        for lad in r.get("ladder", {}).values():
            for v in lad.values():
                if isinstance(v, dict):
                    fix(v["facets"])


def report(rows, split, matcher_name, configs, out_dir):
    recombine(rows)
    lines = [f"# tone_bench: {split.upper()}, matcher `{matcher_name}`", "",
             f"{len(rows)} cases. Lower is closer; 0 is the hidden rig; about 1 a wrong rig (metrics.py). Medians unless said.", ""]
    facets = ["combined"] + mx.FACETS + ["mcd", "legacy", "time_fx"]

    def table(title, group_key):
        groups = {}
        for r in rows:
            groups.setdefault(group_key(r), []).append(r)
        lines.append(f"## {title}")
        lines.append("")
        lines.append("| Group | n | " + " | ".join(configs) + " | default (mean) | wrong rig |")
        lines.append("|---|---|" + "---|" * len(configs) + "---|---|")
        for g in sorted(groups, key=str):
            rs = groups[g]
            cells = [fmt(med([r["configs"][c]["facets"]["combined"] for r in rs])) for c in configs]
            lines.append(f"| {g} | {len(rs)} | " + " | ".join(cells) + f" | {fmt(med([r['anchors']['default_mean']['combined'] for r in rs]))} | "
                         f"{fmt(med([r['anchors']['wrong_rig']['combined'] for r in rs]))} |")
        lines.append("")

    table("Combined score, all cases", lambda r: "all")
    table("By style", lambda r: r["style"])
    table("By production", lambda r: r["production"])
    table("By pedal in the hidden rig", lambda r: r["pedal"] or "none")
    table("By time effect", lambda r: r["time_fx"] or "none")
    table("By amp kind", lambda r: r["amp_kind"])

    lines.append("## Every facet (all cases)")
    lines.append("")
    lines.append("| Method | " + " | ".join(facets) + " |")
    lines.append("|---|" + "---|" * len(facets))
    methods = [(c, lambda r, c=c: r["configs"][c]["facets"]) for c in configs]
    methods += [(f"anchor: {a}", lambda r, a=a: r["anchors"][a]) for a in ("oracle", "default_mean", "default_best", "wrong_rig")]
    if rows and "oracle" in rows[0]:
        methods += [(f"oracle: {o}", lambda r, o=o: r["oracle"][o]["facets"]) for o in ("space", "full", "pedal_forced", "amp_only", "cab_only", "eq_only")]
    for name, get in methods:
        lines.append(f"| {name} | " + " | ".join(fmt(med([get(r)[k] for r in rows])) for k in facets) + " |")
    lines.append("")

    lines += ["## Against the anchors, and what each picked", "",
              "| Method | beats the best default | beats the mean default | same amp as the oracle's in-space pick | Gain at the matcher's limit (+-22.5 dB) |",
              "|---|---|---|---|---|"]
    for c in configs:
        beat_b = sum(r["configs"][c]["facets"]["combined"] < r["anchors"]["default_best"]["combined"] for r in rows)
        beat_m = sum(r["configs"][c]["facets"]["combined"] < r["anchors"]["default_mean"]["combined"] for r in rows)
        same = sum(r["configs"][c]["settings"]["slot"] == r["oracle"]["space"]["settings"]["slot"] for r in rows) if rows and "oracle" in rows[0] else 0
        edge = sum(abs(r["configs"][c]["settings"]["gain"]) >= 22.5 for r in rows)
        lines.append(f"| {c} | {beat_b} of {len(rows)} | {beat_m} of {len(rows)} | {same} of {len(rows)} | {edge} of {len(rows)} |")
    lines.append("")
    if rows and "oracle" in rows[0]:
        edge = sum(abs(r["oracle"]["space"]["settings"]["gain"]) >= 23.0 for r in rows)
        up = sum(r["oracle"]["space"]["settings"]["gain"] >= 23.0 for r in rows)
        by = {}
        for r in rows:
            by.setdefault(r["style"], []).append(tm.AMPS[r["oracle"]["space"]["settings"]["slot"]] + f" {r['oracle']['space']['settings']['gain']:+.0f}")
        lines += [f"The oracle's in-space Gain is at the end of the range (+-23 to 24 dB) in {edge} of {len(rows)} cases ({up} at the top): "
                  "the built-in amps can't reach the hidden rig's drive there.", "",
                  "| Style | the oracle's in-space amp and Gain |", "|---|---|"]
        lines += [f"| {k} | {', '.join(v)} |" for k, v in sorted(by.items())]
        lines.append("")

    # TM55: Anything after cleanup against Same part after cleanup, paired.
    if "same_clean" in configs and "any_clean" in configs:
        from scipy.stats import wilcoxon
        a = np.array([r["configs"]["any_clean"]["facets"]["combined"] for r in rows])
        s = np.array([r["configs"]["same_clean"]["facets"]["combined"] for r in rows])
        d = a - s
        p = wilcoxon(a, s).pvalue if len(rows) >= 6 and np.any(d != 0) else float("nan")
        lines += ["## TM55: Anything after cleanup against Same part after cleanup", "",
                  f"Anything better in {int(np.sum(d < 0))} of {len(d)}; median difference (Anything minus Same) {fmt(float(np.median(d)))}, "
                  f"mean {fmt(float(np.mean(d)))}; Wilcoxon signed-rank p = {fmt(p, 4)}.", ""]
        for prod in ("dry", "fx", "mix"):
            k = [i for i, r in enumerate(rows) if r["production"] == prod]
            if k:
                lines.append(f"- {prod}: Anything better in {int(np.sum(d[k] < 0))} of {len(k)}, median difference {fmt(float(np.median(d[k])))}")
        lines.append("")

    if rows and "ladder" in rows[0]:
        snr = [r["linear_check_snr_db"] for r in rows]
        lines += ["## Diagnosis", "", f"The oracle's linear shortcut (oracle.apply_linear) against ampsim_render on each case's first result: "
                  f"{min(snr):.1f} to {max(snr):.1f} dB SNR.", ""]
        for cfg in [c for c in LADDER_CONFIGS if c in configs]:
            l0 = [r["configs"][cfg]["facets"]["combined"] for r in rows]
            l1 = [r["ladder"][cfg]["dry_target"]["facets"]["combined"] for r in rows]
            l1b = [r["ladder"][cfg]["players_guitar"]["facets"]["combined"] for r in rows]
            l2 = [r["ladder"][cfg]["same_performance"]["facets"]["combined"] for r in rows]
            sp = [r["oracle"]["space"]["facets"]["combined"] for r in rows]
            fu = [r["oracle"]["full"]["facets"]["combined"] for r in rows]
            lines += [f"### The ladder, {cfg}", "",
                      "| Step | median | mean |", "|---|---|---|",
                      f"| L0 the matcher on the real target | {fmt(med(l0))} | {fmt(float(np.mean(l0)))} |",
                      f"| L1 on the dry target (no production, no separation) | {fmt(med(l1))} | {fmt(float(np.mean(l1)))} |",
                      f"| L1b on the record's performance played on the player's guitar | {fmt(med(l1b))} | {fmt(float(np.mean(l1b)))} |",
                      f"| L2 on the rig played by the player (no performance difference) | {fmt(med(l2))} | {fmt(float(np.mean(l2)))} |",
                      f"| L3 the oracle in the matcher's search space | {fmt(med(sp))} | {fmt(float(np.mean(sp)))} |",
                      f"| L4 the oracle with BellyDSP's pedal and compressor | {fmt(med(fu))} | {fmt(float(np.mean(fu)))} |", ""]
            m = lambda v: float(np.mean(v))
            steps = [("production and separation (L0 - L1)", m(l0) - m(l1)), ("the player's guitar (L1 - L1b)", m(l1) - m(l1b)),
                     ("the playing: timing, dynamics, attack, and the alignment (L1b - L2)", m(l1b) - m(l2)),
                     ("objective and optimizer, given a perfect target (L2 - L3)", m(l2) - m(sp)),
                     ("pedal and compressor not searched (L3 - L4)", m(sp) - m(fu)), ("left: amp and cab coverage (L4)", m(fu))]
            lines += ["Decomposition of the mean (each step's mean difference; they add up to L0's mean):", "",
                      "| Part | mean | share of L0 |", "|---|---|---|"]
            lines += [f"| {name} | {fmt(v)} | {100 * v / m(l0):.0f}% |" for name, v in steps]
            lines.append("")
            obj_better = sum(r["ladder"][cfg]["objective_oracle"] < r["ladder"][cfg]["objective_pick"] for r in rows)
            lines.append(f"Objective test ({cfg}): the oracle's in-space configuration scored better than the matcher's pick BY THE MATCHER'S OWN "
                         f"SCORE in {obj_better} of {len(rows)} cases (optimizer misses); in the other {len(rows) - obj_better} the matcher's score "
                         f"preferred its own pick although the oracle's is closer by the benchmark in "
                         f"{sum((r['ladder'][cfg]['objective_oracle'] >= r['ladder'][cfg]['objective_pick']) and (r['oracle']['space']['facets']['combined'] < r['configs'][cfg]['facets']['combined']) for r in rows)} (objective misses).")
            lines.append("")
        lines += ["### Component coverage (the hidden rig with one part replaced by BellyDSP's best)", "",
                  "| Replaced | median | mean | by style (median) |", "|---|---|---|---|"]
        for o in ("amp_only", "cab_only", "eq_only", "pedal_forced"):
            v = [r["oracle"][o]["facets"]["combined"] for r in rows]
            by = {}
            for r in rows:
                by.setdefault(r["style"], []).append(r["oracle"][o]["facets"]["combined"])
            lines.append(f"| {o} | {fmt(med(v))} | {fmt(float(np.mean(v)))} | " + ", ".join(f"{k} {fmt(med(x))}" for k, x in sorted(by.items())) + " |")
        lines.append("")

    if rows and "estimates" in rows[0]:
        e = lambda r, k: r["estimates"][k]["combined"]
        lines += ["## Round 2 estimates (estimates.py)", "", "| What | before | after | mean change |", "|---|---|---|---|"]
        for name, before, k in (("the pick (any_raw) with a high-resolution match curve instead of the 5-band EQ, fitted to the target",
                                 lambda r: r["configs"]["any_raw"]["facets"]["combined"], "hires_pick"),
                                ("the oracle's in-space configuration with the curve", lambda r: r["oracle"]["space"]["facets"]["combined"], "hires_space"),
                                ("cab only (true amp, best searchable IR) with the curve", lambda r: r["oracle"]["cab_only"]["facets"]["combined"], "hires_cab")):
            b = [before(r) for r in rows]
            a = [e(r, k) for r in rows]
            lines.append(f"| {name} | {fmt(med(b))} | {fmt(med(a))} | {fmt(float(np.mean(np.array(a) - np.array(b))))} |")
        tf = [r for r in rows if r["time_fx"]]
        if tf:
            b = [r["configs"]["any_raw"]["facets"]["time_fx"] for r in tf]
            a = [r["estimates"]["timefx_pick"]["time_fx"] for r in tf]
            lines.append(f"| the time-effect facet of the pick with the true time effect added ({len(tf)} cases with one) | {fmt(med(b))} | {fmt(med(a))} | "
                         f"{fmt(float(np.mean(np.array(a) - np.array(b))))} |")
        lines.append("")

    lines += ["## Per case", "", "| Case | style | production | pedal | amp | cab | " + " | ".join(configs) + " | oracle space | default | wrong |",
              "|---|---|---|---|---|---|" + "---|" * len(configs) + "---|---|---|"]
    for r in rows:
        cells = [f"{fmt(r['configs'][c]['facets']['combined'])} ({r['configs'][c]['label']})" for c in configs]
        osp = fmt(r["oracle"]["space"]["facets"]["combined"]) + f" ({r['oracle']['space']['label']})" if "oracle" in r else "n/a"
        lines.append(f"| {r['id']} | {r['style']} | {r['production']}{' +' + r['time_fx'] if r['time_fx'] else ''}{' double' if r['double'] else ''} | "
                     f"{r['pedal'] or ''} | {r['amp']} | {r['cab']} | " + " | ".join(cells) + f" | {osp} | {fmt(r['anchors']['default_mean']['combined'])} | "
                     f"{fmt(r['anchors']['wrong_rig']['combined'])} |")
    lines.append("")
    (out_dir / "report.md").write_text("\n".join(lines))
    (out_dir / "results.json").write_text(json.dumps(rows, indent=1, default=float))
    print("\n".join(lines[:60]))
    print(f"wrote {out_dir / 'report.md'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--split", choices=["dev", "test"], default="dev")
    ap.add_argument("--matcher", default="current", choices=["current"])
    ap.add_argument("--config", default=",".join(mt.CONFIGS))
    ap.add_argument("--cases")
    ap.add_argument("--workers", type=int, default=3, help="cases in parallel")
    ap.add_argument("--render-workers", type=int, default=4, help="renders in parallel within a case")
    ap.add_argument("--no-diagnose", action="store_true")
    ap.add_argument("--make-cases", action="store_true")
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--estimates", action="store_true", help="the Round 2 what-ifs (estimates.py) on the saved cases, then the report")
    ap.add_argument("--out")
    ap.add_argument("--amps", choices=["defaults", "all"], default="defaults",
                    help="the amps the ORACLE may use: the three slot defaults, or every built-in gain set (the matcher searches the three either way)")
    ap.add_argument("--oracle-only", action="store_true",
                    help="with --from: the saved cases' oracle configurations and estimates again (with --amps), then coverage.md")
    ap.add_argument("--from", dest="from_dir", help="a saved run's folder (e.g. build/tone_bench/dev_current)")
    args = ap.parse_args()
    if args.amps == "all":
        os.environ["TONE_BENCH_AMPS"] = "all"   # for the worker processes, which import common afresh
        from common import use_all_amps
        use_all_amps()

    if args.make_cases:
        specs = cs.make_specs()
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            specs = list(pool.map(cs.realize, specs))
        cs.save_specs(specs)
        for s in specs:
            print(f"{s['id']:14s} {s['split']:4s} {s['production']:4s} {s['amp']['name']:16s} NL target {s['nl_target']:6.1f} reached "
                  f"{s['amp']['nl_reached']:6.1f} (drive {s['amp']['drive_db']:+.1f}) pedal {s['pedal']['kind'] if s['pedal'] else '-':12s} "
                  f"cab {s['cab']['base_name']} {[m[0] for m in s['cab']['mods']]}")
        print(f"wrote {cs.CASES_JSON}")
        return

    specs = cs.load_specs()
    configs = args.config.split(",")
    out_dir = pathlib.Path(args.out) if args.out else OUT / f"{args.split}_{args.matcher}"
    (out_dir / "cases").mkdir(parents=True, exist_ok=True)
    chosen = [s for s in specs if s["split"] == args.split]
    if args.cases:
        ids = set(args.cases.split(","))
        chosen = [s for s in chosen if s["id"] in ids]

    if args.calibrate:
        # The wrong rig's facets over the DEV cases: their medians are metrics.SCALES.
        vals = {k: [] for k in mx.FACETS}
        for spec in [s for s in specs if s["split"] == "dev"]:
            perf = cs.performances(spec)
            ev, lay = mx.eval_signal(perf["c"], perf["c_onsets"])
            tone, full = reference_for(spec, ev)
            f = mx.facets(mx.Reference(tone, full, lay), rigs.tone_rig(ev, wrong_rig_for(spec, specs)))
            for k in vals:
                vals[k].append(f[k])
            print(spec["id"], {k: round(f[k], 2) for k in mx.FACETS}, flush=True)
        print("medians:", {k: round(float(np.median(v)), 2) for k, v in vals.items()})
        return

    if args.oracle_only:
        src_dir = pathlib.Path(args.from_dir) / "cases"
        jobs = [(s, src_dir / f"{s['id']}.json", out_dir / "cases" / f"{s['id']}.json", args.split, args.render_workers)
                for s in chosen if (src_dir / f"{s['id']}.json").exists()]
        for spec in [j[0] for j in jobs]:
            if spec["production"] == "mix":
                cs.build(spec, separate)       # the stems first, one at a time (cached by the saved run, usually)
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            rows = list(pool.map(oracle_worker, jobs))
        coverage_report(rows, args.split, out_dir)
        return
    if args.estimates:
        paths = [out_dir / "cases" / f"{s['id']}.json" for s in chosen]
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            list(pool.map(estimate_worker, [(p, args.split) for p in paths]))
        args.report = True
    if not args.report:
        # Separate the mixes first, one at a time (Demucs on the GPU), so the workers find them cached.
        for spec in chosen:
            if spec["production"] == "mix":
                t0 = time.time()
                cs.build(spec, separate)
                print(f"  {spec['id']}: record and stem ready ({time.time() - t0:.0f} s)", flush=True)
        jobs = [(s, specs, args.split, configs, not args.no_diagnose, args.render_workers, out_dir) for s in chosen]
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            rows = list(pool.map(worker, jobs))
    else:
        rows = [json.loads((out_dir / "cases" / f"{s['id']}.json").read_text()) for s in chosen if (out_dir / "cases" / f"{s['id']}.json").exists()]
    report(rows, args.split, args.matcher, configs, out_dir)


if __name__ == "__main__":
    main()

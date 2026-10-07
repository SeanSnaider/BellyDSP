# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""The benchmark's cases: the specs (seeded, written to cases.json) and what they render to.

50 cases, 10 per style (clean, edge, crunch, high_gain, lead), each style's ten split by production:
  dry  (3)  the rig's tone alone, lossless: an isolated guitar from the multitrack;
  fx   (3)  produced: a bus compressor (70%), a time effect (plate, room, slap, or a dotted-eighth delay at
            record mix levels), a mastering limiter (80%), and a lossy file (AAC or MP3, 80%), the guitar
            still alone (an intro, a stem);
  mix  (4)  a record: the guitar (double-tracked and hard-panned on rhythm parts, 70%; a lead gets a
            rhythm guitar under it), a time effect (50%), drums and bass, the limiter, a lossy file, then
            separated with Demucs (htdemucs_6s) as the app's "Separate the guitar first" does.
Splits: per (style, production) group, the first 60% (rounded: 2 of 3, 2 of 4) is DEV and the rest TEST,
so DEV has 30 cases and TEST 20, with the same mix of styles and productions. Each split's hidden cabs come
from its own held-out third of the IRs (rigs.HELD_OUT), which the matcher can't choose in that split.

Amps: per style, 3 of the 10 cases use a NAM core example model of that style (rigs.NAM_AMPS), the other 7
a gray-box voicing of that style (rigs.VOICINGS), alternating between its two. Each case's drive is
calibrated so the amp's own distortion (from its input, after any pedal, to its output) hits a target drawn
from its style's range (STYLE_NL, the nonlinear energy ratio in
dB; the built-in amps span -36 to -20 (Glass), -18 to -7 (Ember), -9 to -4.3 (Monolith)).
"""

import json
import math
import pathlib

import numpy as np
import scipy.signal as sps

import performance as pf
import rigs
from common import HERE, SR, cached, fit_length, rms_db

CASES_JSON = HERE / "cases.json"
STYLE_NL = {"clean": (-40.0, -28.0), "edge": (-26.0, -17.0), "crunch": (-16.0, -9.0), "high_gain": (-9.0, -5.5), "lead": (-6.0, -3.5)}
PRODUCTIONS = ["dry", "dry", "dry", "fx", "fx", "fx", "mix", "mix", "mix", "mix"]
RECORD_SECONDS, HELD_OUT_SECONDS = 16.0, 12.0

PEDAL_CHOICES = {
    "clean": [("comp", 0.4), (None, 0.6)],
    "edge": [("comp", 0.15), ("boost", 0.15), ("transparent", 0.15), (None, 0.55)],
    "crunch": [("mid", 0.2), ("distortion", 0.15), ("boost", 0.1), (None, 0.55)],
    "high_gain": [("mid", 0.3), ("boost", 0.15), (None, 0.55)],
    "lead": [("fuzz", 0.2), ("distortion", 0.15), ("mid", 0.15), (None, 0.5)],
}


def _pick(rng, choices):
    names, p = zip(*choices)
    return names[int(rng.choice(len(names), p=np.array(p) / sum(p)))]


def make_pedal(rng, kind):
    if kind is None:
        return None
    if kind == "comp":
        return dict(kind="comp", threshold=float(rng.uniform(-42, -30)), ratio=float(rng.uniform(3, 8)), release=float(rng.uniform(120, 350)),
                    makeup=float(rng.uniform(4, 10)))
    if kind == "boost":
        return dict(kind="boost", level=float(rng.uniform(4, 10)), tilt=float(rng.uniform(-2, 4)))
    drive = dict(mid=(0.15, 0.7), distortion=(0.25, 0.7), transparent=(0.2, 0.6), fuzz=(0.4, 0.9))[kind]
    tight = float(rng.choice([20.0, 20.0, rng.uniform(80, 200)])) if kind in ("mid", "distortion") else 20.0
    return dict(kind=kind, drive=float(rng.uniform(*drive)), tone=float(rng.uniform(0.3, 0.75)), level=float(rng.uniform(0, 6)), tight=tight)


def make_cab(rng, split):
    base = int(rng.choice(rigs.HELD_OUT[split]))
    mods = []
    if rng.random() < 0.6:
        kinds = list(rng.choice(["resonance_shift", "mic_distance", "off_axis", "small_speaker", "big_cab", "minimum_phase"],
                                size=int(rng.integers(1, 3)), replace=False))
        for k in kinds:
            if k == "resonance_shift":
                mods.append([k, dict(alpha=float(rng.choice([rng.uniform(0.85, 0.95), rng.uniform(1.06, 1.2)])))])
            elif k == "mic_distance":
                mods.append([k, dict(delay_ms=float(rng.uniform(0.4, 2.5)), gain=float(rng.uniform(0.2, 0.45)), hf_loss_db=float(rng.uniform(0.5, 3)))])
            elif k == "off_axis":
                mods.append([k, dict(f=float(rng.uniform(2500, 5000)), db=float(rng.uniform(3, 9)))])
            elif k == "small_speaker":
                mods.append([k, dict(hp=float(rng.uniform(90, 130)), peak_f=float(rng.uniform(1500, 2500)), peak_db=float(rng.uniform(2, 4)))])
            elif k == "big_cab":
                mods.append([k, dict(f=float(rng.uniform(85, 120)), db=float(rng.uniform(3, 5)))])
            else:
                mods.append([k, {}])
    return dict(base=base, base_name=rigs.ALL_IRS[base].stem, mods=mods)


def make_mic_eq(rng):
    eq = [["highpass", float(rng.uniform(60, 110)), 0.0, 0.7071]]
    for _ in range(int(rng.integers(1, 4))):
        kind = rng.choice(["peak", "peak", "highshelf", "lowshelf"])
        if kind == "peak":
            eq.append(["peak", float(np.exp(rng.uniform(np.log(200), np.log(6000)))), float(rng.choice([-1, 1]) * rng.uniform(1.5, 5)), float(rng.uniform(0.7, 2.0))])
        elif kind == "highshelf":
            eq.append(["highshelf", float(rng.uniform(5000, 10000)), float(rng.uniform(-4, 3)), 0.7071])
        else:
            eq.append(["lowshelf", float(rng.uniform(100, 220)), float(rng.uniform(-3, 3)), 0.7071])
    return eq


def make_time_fx(rng, bpm):
    kind = str(rng.choice(["plate", "room", "slap", "dotted"]))
    if kind == "plate":
        return dict(kind=kind, rt60=float(rng.uniform(1.2, 2.5)), predelay_ms=float(rng.uniform(10, 30)), mix_db=float(rng.uniform(-18, -12)))
    if kind == "room":
        return dict(kind=kind, rt60=float(rng.uniform(0.4, 0.9)), mix_db=float(rng.uniform(-16, -10)))
    if kind == "slap":
        return dict(kind=kind, delay_ms=float(rng.uniform(80, 140)), mix_db=float(rng.uniform(-10, -6)))
    return dict(kind=kind, delay_s=0.75 * 60.0 / bpm, feedback=float(rng.uniform(0.25, 0.45)), mix_db=float(rng.uniform(-14, -8)))


def make_specs():
    """The 50 specs, before calibration (realize() adds the calibrated drive and thresholds)."""
    specs = []
    idx = 0
    for s_i, style in enumerate(pf.STYLES):
        voicings = [v for v, d in rigs.VOICINGS.items() if d["style"] == style]
        nams = [n for n, d in rigs.NAM_AMPS.items() if d["style"] == style]
        if style == "crunch":
            nams = ["nam_wavenet"]
        if style == "lead":
            nams = ["nam_standard", "nam_a2"]
        nam_slots = {1, 5, 8}
        counts = {}
        for k, prod in enumerate(PRODUCTIONS):
            seed = 1000 + idx
            rng = np.random.default_rng(seed)
            counts[prod] = counts.get(prod, 0) + 1
            n_in_group = PRODUCTIONS.count(prod)
            split = "dev" if counts[prod] <= round(0.6 * n_in_group + 1e-9) else "test"
            if k in nam_slots:
                amp = dict(kind="nam", name=nams[k % len(nams)])
            else:
                amp = dict(kind="graybox", name=voicings[k % 2],
                           knobs=dict(bass=float(rng.uniform(3, 7)), mid=float(rng.uniform(3, 7)), treble=float(rng.uniform(4, 7.5)), master_db=float(rng.uniform(-3, 3))))
            score_seed, c_seed = seed * 10 + 1, seed * 10 + 2
            bpm_probe = pf.make_score(style, score_seed, RECORD_SECONDS)["bpm"]
            spec = dict(id=f"{style}_{k:02d}", index=idx, style=style, production=prod, split=split, seed=seed,
                        score_seed=score_seed, c_seed=c_seed, guitar_a=seed * 10 + 3, guitar_b=seed * 10 + 4,
                        nl_target=float(rng.uniform(*STYLE_NL[style])), amp=amp, pedal=make_pedal(rng, _pick(rng, PEDAL_CHOICES[style])),
                        cab=make_cab(rng, split), mic_eq=make_mic_eq(rng))
            if prod == "fx":
                spec["bus_comp"] = dict(ratio=float(rng.uniform(2, 4)), attack=float(rng.uniform(5, 30)), release=float(rng.uniform(80, 250)),
                                        gr_db=float(rng.uniform(2, 6))) if rng.random() < 0.7 else None
                spec["time_fx"] = make_time_fx(rng, bpm_probe)
                spec["limiter"] = dict(push_db=float(rng.uniform(3, 8)), release_ms=float(rng.uniform(50, 150))) if rng.random() < 0.8 else None
                spec["codec"] = (dict(kind=str(rng.choice(["aac", "mp3"])), kbps=int(rng.choice([128, 160, 192, 256])))) if rng.random() < 0.8 else None
            elif prod == "mix":
                spec["bus_comp"] = dict(ratio=float(rng.uniform(2, 4)), attack=float(rng.uniform(5, 30)), release=float(rng.uniform(80, 250)),
                                        gr_db=float(rng.uniform(2, 6))) if rng.random() < 0.7 else None
                spec["time_fx"] = make_time_fx(rng, bpm_probe) if rng.random() < 0.5 else None
                spec["double"] = style != "lead" and rng.random() < 0.7
                spec["rhythm_guitar"] = style == "lead"
                spec["band_db"] = float(rng.uniform(-3, 3))
                spec["limiter"] = dict(push_db=float(rng.uniform(4, 9)), release_ms=float(rng.uniform(50, 150)))
                spec["codec"] = dict(kind=str(rng.choice(["aac", "mp3"])), kbps=int(rng.choice([128, 160, 192, 256])))
            else:
                spec.update(bus_comp=None, time_fx=None, limiter=None, codec=None)
            specs.append(spec)
            idx += 1
    return specs


# ---- Performances and realization -----------------------------------------------------------------------

def performances(spec):
    """(A, A2, B, C) with their onsets: the record's guitarist (and double), the player's take, the
    held-out DI; and a_gb, the record's performance (A's timing, velocities, and picks) on the player's
    guitar, for the diagnosis (it separates the guitar's difference from the playing's)."""
    score = pf.make_score(spec["style"], spec["score_seed"], RECORD_SECONDS)
    score_c = pf.make_score(spec["style"], spec["c_seed"], HELD_OUT_SECONDS)
    ga, gb = pf.guitar(spec["guitar_a"]), pf.guitar(spec["guitar_b"])
    out = {}
    for name, sc, kind, seed, g in (("a", score, "A", spec["seed"] * 7 + 1, ga), ("a_gb", score, "A", spec["seed"] * 7 + 1, gb),
                                    ("a2", score, "A2", spec["seed"] * 7 + 2, ga),
                                    ("b", score, "B", spec["seed"] * 7 + 3, gb), ("c", score_c, "B", spec["seed"] * 7 + 4, gb)):
        di, on, v, p = pf.render(sc, kind, seed, g)
        out[name], out[name + "_onsets"] = di, on
    out["bpm"], out["score"] = score["bpm"], score
    return out


def realize(spec):
    """Calibrates what depends on levels (the drive, the bus compressor's threshold): spec plus "amp"
    drive_db and nl_reached, and "bus_comp" threshold. Deterministic, so cases.json can be regenerated."""
    if "drive_db" in spec["amp"]:
        return spec
    perf = performances(spec)
    a = perf["a"][: int(8 * SR)]
    d, reached = rigs.calibrate_drive(a, spec["amp"], spec["pedal"], spec["nl_target"],
                                      lo=-36.0, hi=30.0)
    spec["amp"]["drive_db"], spec["amp"]["nl_reached"] = d, reached
    if spec.get("bus_comp"):
        y = rigs.tone_rig(perf["a"], spec, stages=("pedal", "amp", "cab", "eq"))
        # The threshold gr_db under the playing level (the 90th percentile of a 50 ms RMS), so the gain
        # reduction on loud notes is about gr_db (1 - 1/ratio).
        e = np.sqrt(np.convolve(y ** 2, np.ones(2400) / 2400, mode="same"))
        play = 20 * np.log10(np.percentile(e[e > 1e-6], 90))
        spec["bus_comp"]["threshold"] = float(play - spec["bus_comp"]["gr_db"] / (1 - 1 / spec["bus_comp"]["ratio"]))
    return spec


def load_specs():
    return json.loads(CASES_JSON.read_text())


def save_specs(specs):
    CASES_JSON.write_text(json.dumps(specs, indent=1))


# ---- What a case renders to ------------------------------------------------------------------------------

def backing(n, bpm, score, seed, level_ref, rhythm):
    """Drums and a bass following the score's lowest notes (tone_match.synth_backing's drums at this tempo,
    with its own bass), plus, for a lead, a palm-muted rhythm guitar through another hidden rig (the
    crunch gray-box with a held-out cab): Demucs keeps it with the lead."""
    import tone_match as tm
    drums = tm.synth_backing(n / SR + 0.01, seed, tempo=bpm / 120.0)[:n]
    out = drums * math.sqrt(level_ref / max(np.mean(drums ** 2), 1e-20))
    if rhythm:
        sc = pf.make_score("crunch", seed + 5, RECORD_SECONDS)
        sc["bpm"] = bpm
        di, _, _, _ = pf.render(sc, "A", seed + 6, pf.guitar(seed + 7))
        rig = dict(amp=dict(kind="graybox", name="crunch_sag", drive_db=-6.0, knobs={}), pedal=None, cab=dict(base=rigs.HELD_OUT["test"][3], mods=[]),
                   mic_eq=[["highpass", 90.0, 0.0, 0.7071]], bus_comp=None)
        r = fit_length(rigs.tone_rig(di, rig), n)
        out = out + r * math.sqrt(level_ref / max(np.mean(r ** 2), 1e-20)) * 10 ** (-3 / 20)
    return out


def build(spec, separate=None):
    """Everything the benchmark needs from one case. separate: the Demucs function (None: not needed).
    Returns dict: target (what the matcher is given), dry (the rig's tone on A: the target without the
    production), dry_gb (the same on the record's performance played on the player's guitar), rig_b (the rig's
    tone on the take: what the record would be had the player recorded it),
    perf (the performances), and the case's spec."""
    perf = performances(spec)
    a, b = perf["a"], perf["b"]
    n = len(a)
    tone_a = rigs.tone_rig(a, spec)
    out = dict(spec=spec, perf=perf, dry=tone_a, rig_b=rigs.tone_rig(b, spec), dry_gb=rigs.tone_rig(perf["a_gb"], spec))
    prod = spec["production"]
    if prod == "dry":
        out["target"] = tone_a
        return out

    def produce():
        if spec.get("double"):
            left = rigs.time_fx(tone_a, spec["time_fx"], spec["seed"])
            right = rigs.time_fx(fit_length(rigs.tone_rig(perf["a2"], spec), n), spec["time_fx"], spec["seed"] + 3)
            gtr = np.stack([left, right])
        elif spec.get("time_fx") and spec["time_fx"]["kind"] in ("plate", "room"):
            gtr = np.stack(rigs.time_fx(tone_a, spec["time_fx"], spec["seed"], stereo=True))
        else:
            y = rigs.time_fx(tone_a, spec["time_fx"], spec["seed"])
            gtr = np.stack([y, y])
        if prod == "mix":
            level = float(np.mean(gtr ** 2))
            band = backing(n, perf["bpm"], perf["score"], spec["seed"], level * 10 ** (spec["band_db"] / 10), spec.get("rhythm_guitar"))
            gtr = gtr + band[None, :]
        gtr = gtr * 10 ** (-12 / 20) / np.max(np.abs(gtr))
        if spec.get("limiter"):
            gtr = rigs.limiter(gtr, spec["limiter"]["push_db"], spec["limiter"]["release_ms"])
        gtr = rigs.codec(gtr, spec.get("codec"))
        return gtr.mean(axis=0)                      # the app mixes the file to mono

    record = cached("record", produce, spec)
    out["record"] = record
    if prod == "mix":
        out["target"] = cached("demucs", lambda: separate(record), record)
    else:
        out["target"] = record
    return out

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Makes the three built-in gain sets (content/models/Glass/, Ember/, Monolith/): stand-ins trained from
BellyDSP's own gray-box reference amp, prototypes/amp_sim.py, until Sean's own captures replace them
(docs/CAPTURING.md, "Replacing a built-in capture").

A capture is a snapshot of an amp at ONE gain setting, so a gain knob can't be faked by turning the input up:
past what the capture was trained on, the model extrapolates (docs/ASSUMPTIONS.md AG1). Each built-in amp is
therefore a GAIN SET: five captures of the same gray-box channel at its own gain knob's 0, 2.5, 5, 7.5, and
10, plus a gainset.json listing them. The app's Gain knob moves across the steps and blends the two nearest
(src/dsp/NamAmp.h).

    uv run --no-project --with numpy --with scipy --with soundfile python tools/content/make_default_captures.py
    ... --voice                      # find each channel's gain taper (prints it for prototypes/amp_sim.py)
    ... --only Monolith              # one amp (repeatable); separate --only runs can go in parallel
    ... --steps 0 10                 # only these steps of it
    ... --render-only                # the gray-box renders and their level checks, no training
    ... --heldout-only               # the held-out comparison of the .nam files already in content/models
    ... --package-only               # rewrite the gainset.json files, the manifest entries, and the presets' refs
    ... --measure                    # the gain sets in the app's engine: loudness, crest, distortion per position

Needs NAM's input file (tools/fetch_nam_input.sh), the built tools (cmake --build build -j; ampsim_render
runs Monolith's boost, plays the trained models, and does the --measure renders), and the test suite's DIs
(build/proof/render_0_synthetic_guitar_di.wav and build/proof/default_captures/guitar_di.wav, written by
`build/ampsim_tests_artefacts/Release/ampsim_tests --proof-dir build/proof`).

What it does, for each amp and each gain step:
  1. "The capture": NAM's input file through the gray-box amp, AMP ONLY (amp_sim.amp(..., cab=False), no cab
     sim; BellyDSP has its own cab), at 48 kHz, with the gain knob at the step. The input scaling is unity:
     0 dBFS of the file is 1.0 into amp_sim (docs/ASSUMPTIONS.md DS46). Monolith first goes through the app's
     own Boost block in Screamer mode (the TS808 circuit at minimum drive, tone at noon) at +6 dB of level,
     via ampsim_render --boost: the boost is part of every Monolith step.
  2. A 15 Hz DC blocker (the output transformer's coupling; see gray_box), then the output is turned down 6 dB
     (the power amp's tanh can't exceed 1.0, so nothing reaches 0 dBFS) and delayed by 100 samples, a
     simulated round trip: amp_sim's oversampling filters are linear phase, so its output starts a few
     samples BEFORE the input that causes it, which a causal WaveNet can't do; the delay makes it causal, and
     the trainer measures and removes it from the blips like any real latency. Written as 24-bit PCM.
  3. tools/train_capture.sh trains a standard WaveNet on it (NAM's trainer, pinned 0.12.3; 100 epochs, 200
     for Monolith), with the metadata: name ("Glass, gain 2.5"), modeled_by "BellyDSP", gear_type amp,
     tone_type, and input_level_dbu 12 (DS47).
  4. The .nam goes to content/models/<Amp>/<Amp>, gain <g>.nam.
  5. Held out: the tests' synthetic guitar DI (never seen by the trainer) through the gray-box amp and through
     the .nam in our engine (ampsim_render, no loudness normalization), compared after the best lag and a
     least-squares gain: ESR (the trainer's measure), and the level difference before that gain.
  6. Packaging: content/models/<Amp>/gainset.json (the steps, their gain positions and files),
     content/manifest.json's entry for each .nam (CC BY 4.0, Sean Snaider, the stand-in note), and the
     factory presets' "amps" refs (factory:models/<Amp>/gainset.json, with its hash and size).

--voice: each channel's gain taper (prototypes/amp_sim.py, gain_taper_db) is the first stage's drive at the
five knob positions, chosen so the five steps are evenly spread in distortion. The measure is the nonlinear
energy ratio on the tests' 8 s guitar DI: with the magnitude-squared coherence C(f) between the DI x and the
output y (Welch, 4096-point segments), the part of y's power at f that a linear time-invariant filter of x
can predict is C(f) Pyy(f), so the share no linear filter explains (harmonics, intermodulation, compression)
is
    NL = sum over f of (1 - C(f)) Pyy(f)  /  sum over f of Pyy(f),      60 Hz < f < 12 kHz,
reported in dB: about -37 dB for a clean amp on this DI, rising to about -4 dB for a fully saturated one.
For each step the drive is found by bisection (NL rises monotonically with drive) so NL lands on equal dB
steps between the amp's two ends (NL_RANGE below).

The large renders and training folders stay in build/default_captures/ (not committed).
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time

import numpy as np
import soundfile as sf
from scipy.signal import coherence, welch

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "prototypes"))
import amp_sim  # noqa: E402  (the gray-box reference amp)

INPUT = REPO / "build-deps/nam/input.wav"
RENDER = REPO / "build/ampsim_render_artefacts/Release/ampsim_render"
HELDOUT_DI = REPO / "build/proof/render_0_synthetic_guitar_di.wav"  # 4 s of the tests' guitarDI(), peaks -6 dBFS
LISTENING = REPO / "build/proof/default_captures"                   # the app's renders, from the tests
VOICING_DI = LISTENING / "guitar_di.wav"                            # 8 s of the tests' guitarDI(), peaks -6 dBFS
CONTENT = REPO / "content"
MODELS = CONTENT / "models"
PRESETS = REPO / "presets/factory"

INPUT_SCALE = 1.0          # 0 dBFS of the input -> 1.0 into amp_sim (DS46)
OUTPUT_TRIM_DB = -6.0      # after the power amp, whose output never exceeds 1.0
ROUND_TRIP = 100           # samples of simulated latency (makes the linear-phase oversampling causal)
INPUT_LEVEL_DBU = 12.0     # metadata: the input file's 0 dBFS is the interface's default full scale (DS47)
STEPS = [0.0, 2.5, 5.0, 7.5, 10.0]  # the gain knob positions each set is captured at

# The three amps, in slot order. Knobs are amp_sim's 0-10. The gain knob is the step; its taper is the
# channel's (amp_sim.CHANNELS[...]["gain_taper_db"], found by --voice with these other knobs). NL_RANGE is
# the nonlinear energy ratio (dB) at gain 0 and gain 10 that --voice aims for.
AMPS = {
    "Glass": dict(channel="clean", tone_type="clean", boost_db=None, epochs=100, nl_range=(-36.0, -20.0),
                  description="Clean to the edge of breakup",
                  knobs=dict(bass=5.0, mid=5.0, treble=6.0, master=4.0)),
    "Ember": dict(channel="crunch", tone_type="crunch", boost_db=None, epochs=100, nl_range=(-18.0, -7.0),
                  description="Light crunch to heavy crunch",
                  knobs=dict(bass=5.0, mid=6.0, treble=5.0, master=3.5)),
    # Monolith: 200 epochs. A single high-gain capture at 100 had a validation ESR of 0.0048 and 0.0033 at
    # 200 (ASSUMPTIONS DS49).
    "Monolith": dict(channel="lead", tone_type="hi_gain", boost_db=6.0, epochs=200, nl_range=(-9.0, -4.3),
                     description="Tight high gain to a saturated lead",
                     knobs=dict(bass=4.0, mid=6.0, treble=6.0, master=2.0)),
}


def run(cmd):
    print("$ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, text=True, capture_output=True).stdout


def read_mono(path):
    x, fs = sf.read(str(path), always_2d=True)
    if fs != 48000:
        raise SystemExit(f"{path} is {fs} Hz; everything here is 48 kHz")
    return x[:, 0].astype(np.float64)


def db(v):
    return 20 * np.log10(max(float(v), 1e-12))


def step_name(name, gain):
    return f"{name}, gain {gain:g}"


def step_file(name, gain):
    return MODELS / name / f"{step_name(name, gain)}.nam"


# ---- Measures --------------------------------------------------------------------------------------------

def nl_ratio_db(x, y):
    """The nonlinear energy ratio (module docstring): the share of y's power no linear filter of x explains."""
    n = min(len(x), len(y))
    f, c = coherence(x[:n], y[:n], fs=48000, nperseg=4096)
    _, pyy = welch(y[:n], fs=48000, nperseg=4096)
    band = (f > 60) & (f < 12000)
    return 10 * np.log10(np.sum((1 - c[band]) * pyy[band]) / np.sum(pyy[band]))


def crest_db(y):
    """Peak over RMS (dB), after the first 0.1 s. A clean DI is about 18 dB; a saturated amp about 2.5 dB."""
    y = y[4800:]
    return db(np.max(np.abs(y))) - db(np.sqrt(np.mean(y**2)))


def thd_percent(y, f0=110.0):
    """Total harmonic distortion of a steady f0 sine: the RMS of harmonics 2..39 over the fundamental's,
    from a Hann-windowed 1 s FFT (1 Hz bins, f0 on a bin), each component summed over +-2 bins."""
    n = 48000
    y = y[-n:]
    spectrum = np.abs(np.fft.rfft(y * np.hanning(n)))
    k0 = int(round(f0))

    def power(k):
        return np.sum(spectrum[k - 2: k + 3] ** 2)

    harmonics = sum(power(k0 * h) for h in range(2, 40) if k0 * h + 3 < len(spectrum))
    return 100 * np.sqrt(harmonics / power(k0))


def sine(level_db=-12.0, f0=110.0, seconds=2.0):
    t = np.arange(int(48000 * seconds)) / 48000
    return 10 ** (level_db / 20) * np.sin(2 * np.pi * f0 * t)


# ---- The gray-box amp ------------------------------------------------------------------------------------

def boosted(x_path, spec, work):
    """Monolith's input: the file through the app's Screamer boost (cached per file)."""
    out = work / f"{pathlib.Path(x_path).stem}_boosted_{spec['boost_db']:g}.wav"
    if not out.exists():
        run([RENDER, "--boost", "screamer", "--boost-level", spec["boost_db"], x_path, out])
    return read_mono(out)


def gray_box(x, spec, gain=None, drive_db=None):
    """The gray-box amp on an (already boosted, for Monolith) signal, amp only, at a gain knob position or a
    direct drive."""
    knobs = dict(spec["knobs"], gain=5.0 if gain is None else gain)
    y = amp_sim.amp(INPUT_SCALE * x, 48000, spec["channel"], cab=False, drive_db=drive_db, **knobs)
    # The output transformer's coupling: the power amp's tanh is symmetric, but the preamp's biased stages
    # make the waveform asymmetric, so its output carries an offset that follows the playing (measured
    # without this: -33 dBFS of DC over the file for Monolith). amp_sim's own DC blocker, at r = 0.998:
    # y[n] = x[n] - x[n-1] + 0.998 y[n-1], a -3 dB corner of (1 - r) fs / (2 pi) = 15 Hz.
    return amp_sim.dc_block(y, r=0.998)


def gray_box_file(x_path, spec, work, gain):
    x = boosted(x_path, spec, work) if spec["boost_db"] is not None else read_mono(x_path)
    return gray_box(x, spec, gain=gain)


def voice(name, spec, work):
    """Bisection for the drive (dB) that puts each step's NL on its target."""
    di = read_mono(VOICING_DI)
    x = boosted(VOICING_DI, spec, work) if spec["boost_db"] is not None else di
    targets = np.linspace(spec["nl_range"][0], spec["nl_range"][1], len(STEPS))
    taper = []
    print(f"\n{name} ({spec['channel']} channel): NL targets " + ", ".join(f"{t:.1f}" for t in targets))
    for gain, target in zip(STEPS, targets):
        lo, hi = -70.0, 30.0
        for _ in range(14):  # 100 dB / 2^14: well under 0.01 dB
            mid = 0.5 * (lo + hi)
            if nl_ratio_db(di, gray_box(x, spec, drive_db=mid)) < target:
                lo = mid
            else:
                hi = mid
        drive = round(0.5 * (lo + hi), 1)
        y = gray_box(x, spec, drive_db=drive)
        taper.append(drive)
        print(f"  gain {gain:4g}: drive {drive:+6.1f} dB -> NL {nl_ratio_db(di, y):6.1f} dB, crest {crest_db(y):5.1f} dB", flush=True)
    print(f'  gain_taper_db={taper}  (prototypes/amp_sim.py, CHANNELS["{spec["channel"]}"])')
    return taper


# ---- Capturing -------------------------------------------------------------------------------------------

def render_capture(name, spec, gain, work):
    x_len = sf.info(str(INPUT)).frames
    y = gray_box_file(INPUT, spec, work, gain) * 10 ** (OUTPUT_TRIM_DB / 20)
    y = np.concatenate([np.zeros(ROUND_TRIP), y])[:x_len]
    peak, rms, dc = np.max(np.abs(y)), np.sqrt(np.mean(y**2)), np.mean(y)
    overs = int(np.sum(np.abs(y) >= 1.0))
    out = work / f"{step_name(name, gain)}_output.wav"
    sf.write(str(out), y, 48000, subtype="PCM_24")
    stats = dict(peak_dbfs=round(db(peak), 2), rms_dbfs=round(db(rms), 2), dc_dbfs=round(db(abs(dc)), 1), overs=overs)
    print(f"  {step_name(name, gain)}: peak {stats['peak_dbfs']} dBFS, RMS {stats['rms_dbfs']} dBFS, DC {stats['dc_dbfs']} dBFS, "
          f"samples at or over full scale: {overs}", flush=True)
    return out, stats


def train(name, spec, gain, output_wav, work, epochs):
    label = step_name(name, gain)
    log = run([REPO / "tools/train_capture.sh", "--input", INPUT, "--output", output_wav, "--name", label,
               "--modeled-by", "BellyDSP", "--tone-type", spec["tone_type"], "--gear-type", "amp",
               "--input-level-dbu", INPUT_LEVEL_DBU, "--epochs", epochs, "--out-dir", work])
    (work / f"{label}_training_log.txt").write_text(log)
    for line in log.splitlines():
        if line.startswith(("Done in", "Final validation", "WARNING")):
            print("  " + line, flush=True)
    summary = json.loads((work / f"{label} training.json").read_text())
    step_file(name, gain).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(work / f"{label}.nam", step_file(name, gain))
    return summary


def compare(model_out, source, max_lag=400):
    """ESR of the model against the source after the best lag (model later) and least-squares gain."""
    n = min(len(model_out), len(source)) - max_lag
    best = None
    for lag in range(max_lag):
        m, s = model_out[lag: lag + n], source[:n]
        g = np.dot(m, s) / np.dot(m, m)
        esr = np.sum((g * m - s) ** 2) / np.sum(s**2)
        if best is None or esr < best[0]:
            best = (esr, lag, g)
    return best


def held_out(name, spec, gain, work):
    if not HELDOUT_DI.exists():
        print(f"  (no {HELDOUT_DI.relative_to(REPO)}: run ampsim_tests --proof-dir build/proof first)")
        return {}
    source = gray_box_file(HELDOUT_DI, spec, work, gain) * 10 ** (OUTPUT_TRIM_DB / 20)
    model_wav = work / f"{step_name(name, gain)}_heldout_model.wav"
    run([RENDER, "--model", step_file(name, gain), "--no-normalize", HELDOUT_DI, model_wav])
    model = read_mono(model_wav)
    esr, lag, g = compare(model, source)
    level_db = db(np.sqrt(np.mean(model**2))) - db(np.sqrt(np.mean(source**2)))
    print(f"  {step_name(name, gain)}: held-out DI, model in our engine vs the gray-box amp: ESR {esr:.4f}, model {lag} "
          f"samples later, level {level_db:+.2f} dB", flush=True)
    return dict(heldout_esr=float(esr), heldout_lag=int(lag), heldout_level_db=round(level_db, 2))


# ---- Packaging -------------------------------------------------------------------------------------------

def fnv1a64(path):
    """presets::contentHash of a file: 64-bit FNV-1a over its bytes, h = (h xor byte) * 1099511628211."""
    h = 14695981039346656037
    for b in pathlib.Path(path).read_bytes():
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{h:016x}"


def settings_text(spec, gain):
    k = spec["knobs"]
    text = (f'{spec["channel"]} channel, gain {gain:g} (first stage {amp_sim.gain_drive_db(spec["channel"], gain):+.1f} dB), '
            f'bass {k["bass"]:g}, mid {k["mid"]:g}, treble {k["treble"]:g}, master {k["master"]:g}')
    if spec["boost_db"] is not None:
        text = f'the app\'s Screamer boost (TS808 at minimum drive, tone noon, level +{spec["boost_db"]:g} dB) into the ' + text
    return text


def write_gainset(name, spec):
    """content/models/<Amp>/gainset.json, the format NamAmp reads (src/dsp/GainSet.h)."""
    steps = [dict(gain=g, file=step_file(name, g).name, hash=fnv1a64(step_file(name, g))) for g in STEPS]
    data = {
        "format": "bellydsp-gain-set",
        "version": 1,
        "name": name,
        "description": spec["description"],
        "tone_type": spec["tone_type"],
        "steps": steps,
    }
    path = MODELS / name / "gainset.json"
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"  wrote {path.relative_to(REPO)}")


def update_manifest(results):
    manifest_path = CONTENT / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    others = [f for f in manifest["files"] if not f["path"].startswith("models/")]
    entries = []
    for slot, (name, spec) in enumerate(AMPS.items()):
        for gain in STEPS:
            r = results.get(name, {}).get(f"{gain:g}", {})
            esr = r.get("training", {}).get("validation_esr")
            epochs = r.get("training", {}).get("epochs", spec["epochs"])
            entries.append({
                "path": f"models/{name}/{step_name(name, gain)}.nam",
                "title": f"{step_name(name, gain)} ({spec['tone_type'].replace('hi_gain', 'high gain')}, amp only)",
                "description": f"Built-in gain set for amp slot {slot + 1}, step {STEPS.index(gain) + 1} of {len(STEPS)}",
                "author": "Sean Snaider",
                "source": "Made for BellyDSP by tools/content/make_default_captures.py (https://github.com/SeanSnaider/BellyDSP)",
                "license": "CC BY 4.0",
                "license_file": "licenses/CC-BY-4.0.txt",
                "notes": ("Trained from BellyDSP's own gray-box reference amp (prototypes/amp_sim.py, " + settings_text(spec, gain)
                          + f"; amp only, no cab) with neural-amp-modeler 0.12.3, standard WaveNet, {epochs} epochs"
                          + (f", validation ESR {esr:.4f}" if esr is not None else "")
                          + "; a stand-in until Sean's own captures"),
            })
    manifest["files"] = sorted(others + entries, key=lambda f: f["path"])
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print(f"  updated the model entries in {manifest_path.relative_to(REPO)}")


def update_factory_presets():
    refs = []
    for name in AMPS:  # slot order: Glass, Ember, Monolith
        f = MODELS / name / "gainset.json"
        refs.append({"path": f"factory:models/{name}/gainset.json", "hash": fnv1a64(f), "size": f.stat().st_size})
    for preset in sorted(PRESETS.glob("*.json")):
        data = json.loads(preset.read_text())
        if data.get("amps") != refs:
            data["amps"] = refs
            preset.write_text(json.dumps(data, indent=2) + "\n")
            print(f"  updated the captures in {preset.relative_to(REPO)}")


# ---- Measuring the sets in the app's engine --------------------------------------------------------------

def measure(work, legacy_dir=None):
    """Each set (or, with legacy_dir, the old single captures at the matching input trims) rendered by
    ampsim_render with the app's loudness normalization: loudness, crest factor, NL, and THD per position."""
    di = read_mono(VOICING_DI)
    tone = work / "sine_110_-12.wav"
    sf.write(str(tone), sine(), 48000, subtype="FLOAT")
    table = {}
    for name in AMPS:
        rows = []
        for gain in [0.0, 1.25, 2.5, 3.75, 5.0, 6.25, 7.5, 8.75, 10.0]:
            if legacy_dir:
                model, args = pathlib.Path(legacy_dir) / f"{name}.nam", ["--trim", (gain - 5.0) * 4.8]
            else:
                model, args = MODELS / name / "gainset.json", ["--gain", gain]
            out, out_sine = work / f"measure_{name}_{gain:g}.wav", work / f"measure_{name}_{gain:g}_sine.wav"
            run([RENDER, "--model", model, *args, VOICING_DI, out])
            run([RENDER, "--model", model, *args, tone, out_sine])
            y = read_mono(out)
            rows.append(dict(gain=gain, rms_dbfs=round(db(np.sqrt(np.mean(y[4800:] ** 2))), 1), crest_db=round(crest_db(y), 1),
                             nl_db=round(nl_ratio_db(di, y), 1), thd_percent=round(thd_percent(read_mono(out_sine)), 1)))
        table[name] = rows
        print(f"\n{name}{' (legacy single capture, Gain = input trim)' if legacy_dir else ''}")
        print("| Gain | RMS dBFS | Crest dB | NL dB | THD % (110 Hz, -12 dBFS) |")
        print("|---|---|---|---|---|")
        for r in rows:
            print(f"| {r['gain']:g} | {r['rms_dbfs']} | {r['crest_db']} | {r['nl_db']} | {r['thd_percent']} |")
    (work / ("measure_legacy.json" if legacy_dir else "measure.json")).write_text(json.dumps(table, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", choices=list(AMPS), action="append", help="just this amp (repeatable)")
    p.add_argument("--steps", type=float, nargs="+", help="just these gain steps")
    p.add_argument("--epochs", type=int, help="override each amp's own (Glass and Ember 100, Monolith 200)")
    p.add_argument("--work-dir", default=str(REPO / "build/default_captures"))
    p.add_argument("--voice", action="store_true", help="find each channel's gain taper and print it")
    p.add_argument("--render-only", action="store_true")
    p.add_argument("--heldout-only", action="store_true")
    p.add_argument("--package-only", action="store_true", help="gainset.json, the manifest, and the presets' refs")
    p.add_argument("--measure", action="store_true", help="the sets in the app's engine, per Gain position")
    p.add_argument("--measure-legacy", metavar="DIR", help="the same for old single captures <DIR>/<Amp>.nam, Gain = trim")
    args = p.parse_args()
    work_root = pathlib.Path(args.work_dir)
    work_root.mkdir(parents=True, exist_ok=True)
    names = args.only or list(AMPS)

    if args.voice:
        for name in names:
            voice(name, AMPS[name], work_root)
        return
    if args.measure or args.measure_legacy:
        measure(work_root, args.measure_legacy)
        return

    def load_results():
        results = {}
        for name in AMPS:
            f = work_root / name / "results.json"
            results[name] = json.loads(f.read_text()) if f.exists() else {}
        return results

    if not args.package_only:
        for needed, how in [(INPUT, "tools/fetch_nam_input.sh"), (RENDER, "cmake --build build -j")]:
            if not needed.exists():
                raise SystemExit(f"{needed.relative_to(REPO)} is missing: run {how}")
        for name in names:
            spec = AMPS[name]
            work = work_root / name
            work.mkdir(parents=True, exist_ok=True)
            results_file = work / "results.json"
            results = json.loads(results_file.read_text()) if results_file.exists() else {}
            for gain in args.steps or STEPS:
                print(f"\n{step_name(name, gain)}: {settings_text(spec, gain)}", flush=True)
                entry = results.get(f"{gain:g}", {})
                entry["settings"] = settings_text(spec, gain)
                if not args.heldout_only:
                    started = time.time()
                    output_wav, entry["render"] = render_capture(name, spec, gain, work)
                    print(f"  rendered in {time.time() - started:.0f} s", flush=True)
                    if not args.render_only:
                        summary = train(name, spec, gain, output_wav, work, args.epochs or spec["epochs"])
                        entry["training"] = {k: summary[k] for k in ("epochs", "seconds", "validation_esr", "latency_samples", "device")}
                        entry["nam_bytes"] = step_file(name, gain).stat().st_size
                if not args.render_only and step_file(name, gain).exists():
                    entry.update(held_out(name, spec, gain, work))
                results[f"{gain:g}"] = entry
                results_file.write_text(json.dumps(results, indent=2))

    if args.render_only:
        return
    for name in AMPS:
        if all(step_file(name, g).exists() for g in STEPS):
            write_gainset(name, AMPS[name])
    if all((MODELS / name / "gainset.json").exists() for name in AMPS):
        update_manifest(load_results())
        update_factory_presets()


if __name__ == "__main__":
    main()

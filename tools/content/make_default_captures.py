# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Makes the three built-in captures (content/models/Glass.nam, Ember.nam, Monolith.nam): stand-ins trained
from BellyDSP's own gray-box reference amp, prototypes/amp_sim.py, until Sean's own captures replace them
(docs/CAPTURING.md, "Replacing a built-in capture").

    uv run --with numpy --with scipy --with soundfile python tools/content/make_default_captures.py
    ... --only Monolith --epochs 300        # one of them, with a different number of epochs
    ... --render-only                       # the renders and their level checks, no training
    ... --heldout-only                      # the held-out comparison of the .nam files already in content/models
    ... --presets-only                      # only rewrite the factory presets' "amps" hashes (after replacing a .nam)

Needs NAM's input file (tools/fetch_nam_input.sh), the built tools (cmake --build build -j; ampsim_render
runs Monolith's boost and plays the trained models), and for the held-out comparison the test suite's
synthetic guitar DI (build/proof/render_0_synthetic_guitar_di.wav, written by
`build/ampsim_tests_artefacts/Release/ampsim_tests --proof-dir build/proof`).

What it does, for each amp:
  1. "The capture": NAM's input file through the gray-box amp, AMP ONLY (amp_sim.amp(..., cab=False), no
     cab sim; BellyDSP has its own cab), at 48 kHz. The input scaling is unity: 0 dBFS of the file is 1.0
     into amp_sim, so a guitar DI peaking at -12 to -6 dBFS (a typical interface level, and the level of
     the input file's own guitar sections) drives each channel the way it was designed (docs/ASSUMPTIONS.md
     DS46). Monolith first goes through the app's own Boost block in Screamer mode (the TS808 circuit at
     minimum drive, tone at noon) at +6 dB of level, via ampsim_render --boost.
  2. A 15 Hz DC blocker (the output transformer's coupling; see gray_box), then the output is turned down 6 dB (the power amp's tanh can't exceed 1.0, so nothing reaches 0 dBFS) and
     delayed by 100 samples, a simulated round trip: amp_sim's oversampling filters are linear phase, so
     its output starts a few samples BEFORE the input that causes it, which a causal WaveNet can't do; the
     delay makes it causal, and the trainer measures and removes it from the blips like any real latency.
     Peak, RMS, DC, and overs are printed. Written as 24-bit PCM like ampsim_capture's recordings.
  3. tools/train_capture.sh trains a standard WaveNet on it (NAM's trainer, pinned 0.12.3; 100 epochs, 200
     for Monolith), with the
     metadata: name, modeled_by "BellyDSP", gear_type amp, tone_type, and input_level_dbu 12 (DS47).
  4. The .nam goes to content/models/<Name>.nam.
  5. Held out: the synthetic guitar DI (never seen by the trainer) through the gray-box amp and through the
     .nam in our engine (ampsim_render, no loudness normalization), compared after the best lag and a
     least-squares gain: ESR (the trainer's measure), and the level difference before that gain. The
     gray-box render also goes to build/proof/default_captures/<name>_graybox_amp_only.wav for listening
     next to the app's own renders there (tests/BuiltInCaptureTests.cpp), at the same RMS.
  6. The factory presets (presets/factory/*.json) get the three files in their slots, with the new content
     hashes and sizes, so they load without a "has changed" warning.

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

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "prototypes"))
import amp_sim  # noqa: E402  (the gray-box reference amp)

INPUT = REPO / "build-deps/nam/input.wav"
RENDER = REPO / "build/ampsim_render_artefacts/Release/ampsim_render"
HELDOUT_DI = REPO / "build/proof/render_0_synthetic_guitar_di.wav"  # 4 s of the tests' guitarDI(), peaks -6 dBFS
LISTENING = REPO / "build/proof/default_captures"                   # the app's renders, from the tests
CONTENT = REPO / "content/models"
PRESETS = REPO / "presets/factory"

INPUT_SCALE = 1.0          # 0 dBFS of the input -> 1.0 into amp_sim (DS46)
OUTPUT_TRIM_DB = -6.0      # after the power amp, whose output never exceeds 1.0
ROUND_TRIP = 100           # samples of simulated latency (makes the linear-phase oversampling causal)
INPUT_LEVEL_DBU = 12.0     # metadata: the input file's 0 dBFS is the interface's default full scale (DS47)

# The three amps. Knobs are amp_sim's 0-10; 5 is its neutral setting (gain 5 = the channel's design drive).
AMPS = {
    "Glass": dict(channel="clean", tone_type="clean", boost_db=None, epochs=100,
                  knobs=dict(gain=4.0, bass=5.0, mid=5.0, treble=6.0, master=5.0)),
    "Ember": dict(channel="crunch", tone_type="crunch", boost_db=None, epochs=100,
                  knobs=dict(gain=5.0, bass=5.0, mid=6.0, treble=5.0, master=5.0)),
    # Monolith: 200 epochs. At 100 its validation ESR was 0.0048 (best epoch; 0.0080 at the last) and the
    # held-out 0.0540; at 200, 0.0033 and 0.0477 (ASSUMPTIONS DS49).
    "Monolith": dict(channel="lead", tone_type="hi_gain", boost_db=6.0, epochs=200,
                     knobs=dict(gain=4.0, bass=4.0, mid=6.0, treble=6.0, master=5.0)),
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


def settings_text(spec):
    k = spec["knobs"]
    text = f'{spec["channel"]} channel, gain {k["gain"]:g}, bass {k["bass"]:g}, mid {k["mid"]:g}, treble {k["treble"]:g}, master {k["master"]:g}'
    if spec["boost_db"] is not None:
        text = f'the app\'s Screamer boost (TS808 at minimum drive, tone noon, level +{spec["boost_db"]:g} dB) into the ' + text
    return text


def gray_box(x_path, spec, work, tag):
    """The gray-box amp on a file: the boost (if any) through ampsim_render, then amp_sim, amp only."""
    if spec["boost_db"] is not None:
        boosted = work / f"{tag}_boosted.wav"
        run([RENDER, "--boost", "screamer", "--boost-level", spec["boost_db"], x_path, boosted])
        x = read_mono(boosted)
    else:
        x = read_mono(x_path)
    y = amp_sim.amp(INPUT_SCALE * x, 48000, spec["channel"], cab=False, **spec["knobs"])
    # The output transformer's coupling: the power amp's tanh is symmetric, but the preamp's biased stages
    # make the waveform asymmetric, so its output carries an offset that follows the playing (measured
    # without this: -33 dBFS of DC over the file for Monolith). amp_sim's own DC blocker, at r = 0.998:
    # y[n] = x[n] - x[n-1] + 0.998 y[n-1], a -3 dB corner of (1 - r) fs / (2 pi) = 15 Hz.
    return amp_sim.dc_block(y, r=0.998)


def render_capture(name, spec, work):
    x_len = sf.info(str(INPUT)).frames
    y = gray_box(INPUT, spec, work, name) * 10 ** (OUTPUT_TRIM_DB / 20)
    y = np.concatenate([np.zeros(ROUND_TRIP), y])[:x_len]
    peak, rms, dc = np.max(np.abs(y)), np.sqrt(np.mean(y**2)), np.mean(y)
    overs = int(np.sum(np.abs(y) >= 1.0))
    out = work / f"{name}_output.wav"
    sf.write(str(out), y, 48000, subtype="PCM_24")
    stats = dict(peak_dbfs=round(db(peak), 2), rms_dbfs=round(db(rms), 2), dc=float(dc), dc_dbfs=round(db(abs(dc)), 1), overs=overs)
    print(f"  {name}: peak {stats['peak_dbfs']} dBFS, RMS {stats['rms_dbfs']} dBFS, DC {dc:.2e} ({stats['dc_dbfs']} dBFS), "
          f"samples at or over full scale: {overs}")
    return out, stats


def train(name, spec, output_wav, work, epochs):
    log = run([REPO / "tools/train_capture.sh", "--input", INPUT, "--output", output_wav, "--name", name,
               "--modeled-by", "BellyDSP", "--tone-type", spec["tone_type"], "--gear-type", "amp",
               "--input-level-dbu", INPUT_LEVEL_DBU, "--epochs", epochs, "--out-dir", work])
    (work / f"{name}_training_log.txt").write_text(log)
    for line in log.splitlines():
        if line.startswith(("Done in", "Validation ESR", "Final validation", "neural-amp", "Delay based", "After aplying", "WARNING")):
            print("  " + line)
    summary = json.loads((work / f"{name} training.json").read_text())
    CONTENT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(work / f"{name}.nam", CONTENT / f"{name}.nam")
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


def held_out(name, spec, work):
    if not HELDOUT_DI.exists():
        print(f"  (no {HELDOUT_DI.relative_to(REPO)}: run ampsim_tests --proof-dir build/proof first)")
        return {}
    source = gray_box(HELDOUT_DI, spec, work, f"{name}_heldout") * 10 ** (OUTPUT_TRIM_DB / 20)
    model_wav = work / f"{name}_heldout_model.wav"
    run([RENDER, "--model", CONTENT / f"{name}.nam", "--no-normalize", HELDOUT_DI, model_wav])
    model = read_mono(model_wav)
    esr, lag, gain = compare(model, source)
    level_db = db(np.sqrt(np.mean(model**2))) - db(np.sqrt(np.mean(source**2)))
    sf.write(str(work / f"{name}_heldout_source.wav"), source, 48000, subtype="FLOAT")
    # For listening: the gray-box amp on the tests' 8 s DI, at the RMS of the app's amp-only render of it.
    app_render, di = LISTENING / f"{name.lower()}_amp_only.wav", LISTENING / "guitar_di.wav"
    if app_render.exists() and di.exists():
        gray = gray_box(di, spec, work, f"{name}_listening")
        target = np.sqrt(np.mean(read_mono(app_render) ** 2))
        gray *= target / np.sqrt(np.mean(gray**2))
        sf.write(str(LISTENING / f"{name.lower()}_graybox_amp_only.wav"), gray, 48000, subtype="FLOAT")
    print(f"  {name}: held-out DI, model in our engine vs the gray-box amp: ESR {esr:.4f}, model {lag} samples later, "
          f"level {level_db:+.2f} dB (RMS, before the matching gain of {db(abs(gain)):+.2f} dB)")
    return dict(heldout_esr=float(esr), heldout_lag=int(lag), heldout_level_db=round(level_db, 2), heldout_gain_db=round(db(abs(gain)), 2))


def fnv1a64(path):
    """presets::contentHash: 64-bit FNV-1a over the file's bytes, h = (h xor byte) * 1099511628211."""
    h = 14695981039346656037
    for b in pathlib.Path(path).read_bytes():
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"fnv1a64:{h:016x}"


def update_factory_presets():
    refs = []
    for name in AMPS:  # slot order: Glass, Ember, Monolith
        nam = CONTENT / f"{name}.nam"
        refs.append({"path": f"factory:models/{name}.nam", "hash": fnv1a64(nam), "size": nam.stat().st_size})
    for preset in sorted(PRESETS.glob("*.json")):
        data = json.loads(preset.read_text())
        if data.get("amps") != refs:
            data["amps"] = refs
            preset.write_text(json.dumps(data, indent=2) + "\n")
            print(f"  updated the captures in {preset.relative_to(REPO)}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", choices=list(AMPS), action="append", help="just this amp (repeatable)")
    p.add_argument("--epochs", type=int, help="override each amp's own (Glass and Ember 100, Monolith 200)")
    p.add_argument("--work-dir", default=str(REPO / "build/default_captures"))
    p.add_argument("--render-only", action="store_true")
    p.add_argument("--heldout-only", action="store_true")
    p.add_argument("--presets-only", action="store_true", help="only update the factory presets' hashes of content/models/*.nam")
    args = p.parse_args()
    if args.presets_only:
        update_factory_presets()
        return
    work = pathlib.Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    for needed, how in [(INPUT, "tools/fetch_nam_input.sh"), (RENDER, "cmake --build build -j")]:
        if not needed.exists():
            raise SystemExit(f"{needed.relative_to(REPO)} is missing: run {how}")

    results = {}
    results_file = work / "results.json"
    if results_file.exists():
        results = json.loads(results_file.read_text())
    for name in args.only or list(AMPS):
        spec = AMPS[name]
        print(f"\n{name}: {settings_text(spec)}")
        entry = results.get(name, {})
        entry["settings"] = settings_text(spec)
        if not args.heldout_only:
            started = time.time()
            output_wav, entry["render"] = render_capture(name, spec, work)
            print(f"  rendered in {time.time() - started:.0f} s")
            if not args.render_only:
                summary = train(name, spec, output_wav, work, args.epochs or spec["epochs"])
                entry["training"] = {k: summary[k] for k in ("epochs", "seconds", "validation_esr", "latency_samples", "device")}
                entry["nam_bytes"] = (CONTENT / f"{name}.nam").stat().st_size
        if not args.render_only and (CONTENT / f"{name}.nam").exists():
            entry.update(held_out(name, spec, work))
        results[name] = entry
        results_file.write_text(json.dumps(results, indent=2))
    if not args.render_only and all((CONTENT / f"{name}.nam").exists() for name in AMPS):
        update_factory_presets()
    print(f"\nResults: {results_file}")


if __name__ == "__main__":
    main()

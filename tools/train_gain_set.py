# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Trains a gain set in one command: one capture of your amp per gain-knob position, each trained with NAM's
official trainer (tools/train_capture.sh), then the gainset.json that makes them one amp in the app, whose Gain
knob moves across them (docs/CAPTURING.md, "Gain sets"; BUILD_PLAN "Amp gain").

    uv run --no-project python tools/train_gain_set.py --name "Crunch amp" --tone-type crunch --gear-type amp \\
        --step 0 "captures/Crunch g0.wav" --step 2.5 "captures/Crunch g2.5.wav" --step 5 "captures/Crunch g5.wav" \\
        --step 7.5 "captures/Crunch g7.5.wav" --step 10 "captures/Crunch g10.wav" --out-dir "trained/Crunch amp"

Each --step is a position on the APP's Gain knob (0 to 10) and the ampsim_capture recording you made with your
amp's gain knob where you want that position to sound. Use your amp's own knob numbers where they run 0 to 10;
what matters is that the steps go from your cleanest setting to your most driven one, spread so each sounds
about as different from the next. Two steps work; five (0, 2.5, 5, 7.5, 10) is what the built-ins use.

What it does:
  1. For each step, tools/train_capture.sh --input build-deps/nam/input.wav --output <recording> --name "<name>,
     gain <g>" with your tone and gear type (and --epochs, --arch, --input-level-dbu if given), into --out-dir.
     A step whose .nam is already there is skipped, so an interrupted run picks up where it stopped (pass
     --retrain to train them all again).
  2. Writes <out-dir>/gainset.json: format "bellydsp-gain-set", version 1, the name, your description and tone
     type, and the steps in ascending gain with their files.
  3. Prints each step's validation ESR. Load the gainset.json into an amp slot to play it.

Every step is trained with the same latency (the trainer's measurement on the first step, or --latency): a
measurement a sample off between steps would leave their models a sample apart, which blends as a comb filter.

Arguments after --train-args are passed to every training (e.g. --train-args --seed 1).
"""

import argparse
import json
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", required=True, help='the amp, e.g. "Crunch amp" (no brands); steps are named "<name>, gain <g>"')
    p.add_argument("--step", nargs=2, action="append", metavar=("GAIN", "RECORDING"), required=True,
                   help="a Gain position (0 to 10) and its recording from ampsim_capture; repeat for each step")
    p.add_argument("--tone-type", choices=["clean", "overdrive", "crunch", "hi_gain", "fuzz"], required=True)
    p.add_argument("--gear-type", choices=["amp", "pedal", "pedal_amp", "amp_cab", "amp_pedal_cab", "preamp", "studio"], required=True)
    p.add_argument("--out-dir", required=True, help="the set's folder: the .nam files and gainset.json go here")
    p.add_argument("--description", default="", help='shown with the set, e.g. "Light crunch to heavy crunch"')
    p.add_argument("--input", default=str(REPO / "build-deps/nam/input.wav"), help="NAM's input file")
    p.add_argument("--epochs", type=int)
    p.add_argument("--arch", choices=["standard", "lite", "feather", "nano"])
    p.add_argument("--input-level-dbu", type=float)
    p.add_argument("--latency", type=int, help="the round trip in samples for every step (default: what the trainer measures on the first)")
    p.add_argument("--modeled-by")
    p.add_argument("--retrain", action="store_true", help="train every step even if its .nam exists")
    p.add_argument("--package-only", action="store_true", help="only write gainset.json from the .nam files already there")
    p.add_argument("--train-args", nargs=argparse.REMAINDER, default=[], help="passed to every tools/train_capture.sh run")
    args = p.parse_args()

    steps = sorted((float(g), pathlib.Path(rec)) for g, rec in args.step)
    gains = [g for g, _ in steps]
    if any(not 0.0 <= g <= 10.0 for g in gains) or len(set(gains)) != len(gains) or not 1 <= len(steps) <= 11:
        raise SystemExit("--step gains must be 1 to 11 different positions in 0 to 10")
    out = pathlib.Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    # One latency for every step. The trainer measures the round trip from each recording's blips, and the
    # measurement can move by a sample between recordings of the same rig; models a sample apart in time blend
    # into a comb filter (a treble loss halfway between steps). So the first step's measurement (or --latency)
    # is passed to every other step.
    latency = args.latency
    if latency is None:
        for gain, _ in steps:
            stats = out / f"{args.name}, gain {gain:g} training.json"
            if stats.exists() and json.loads(stats.read_text()).get("latency_samples") is not None:
                latency = int(json.loads(stats.read_text())["latency_samples"])
                break

    entries, summary = [], []
    for gain, recording in steps:
        label = f"{args.name}, gain {gain:g}"
        nam = out / f"{label}.nam"
        if not args.package_only and (args.retrain or not nam.exists()):
            if not recording.exists():
                raise SystemExit(f"{recording} doesn't exist")
            cmd = [str(REPO / "tools/train_capture.sh"), "--input", args.input, "--output", str(recording), "--name", label,
                   "--tone-type", args.tone_type, "--gear-type", args.gear_type, "--out-dir", str(out)]
            for flag, value in (("--epochs", args.epochs), ("--arch", args.arch), ("--input-level-dbu", args.input_level_dbu),
                                ("--modeled-by", args.modeled_by), ("--latency", latency)):
                if value is not None:
                    cmd += [flag, str(value)]
            cmd += args.train_args
            print(f"\n== {label}: training on {recording}" + (f" (latency pinned at {latency} samples)" if latency is not None else ""), flush=True)
            if subprocess.run(cmd).returncode != 0:
                raise SystemExit(f"Training {label} failed (see above); the steps already trained are kept.")
            stats = out / f"{label} training.json"
            if latency is None and stats.exists():
                latency = json.loads(stats.read_text()).get("latency_samples")
        if not nam.exists():
            raise SystemExit(f"{nam} is missing")
        entries.append({"gain": gain, "file": nam.name})
        stats = out / f"{label} training.json"
        esr = json.loads(stats.read_text()).get("validation_esr") if stats.exists() else None
        summary.append(f"  gain {gain:g}: {nam.name}" + (f", validation ESR {esr:.4f}" if esr is not None else ""))

    data = {"format": "bellydsp-gain-set", "version": 1, "name": args.name, "description": args.description,
            "tone_type": args.tone_type, "steps": entries}
    (out / "gainset.json").write_text(json.dumps(data, indent=2) + "\n")
    print("\n".join([f"\nWrote {out / 'gainset.json'} ({len(entries)} steps):"] + summary))
    print("Load that gainset.json into an amp slot (click the grille) to play it; docs/CAPTURING.md, \"Gain sets\", for bundling it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

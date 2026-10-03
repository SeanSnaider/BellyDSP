# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""The whole capture workflow end to end without hardware: capture -> train -> play in our engine -> compare.

    uv run --with numpy --with soundfile python tools/synthetic_capture_check.py --arch nano --epochs 20

The "gear" is the app's own Distortion pedal circuit (src/dsp, the RAT-style model, Drive 60%), played
through ampsim_capture's simulated device, so the right answer is known exactly:
  1. ampsim_capture --simulate drive records NAM's input file through the circuit, with a simulated round
     trip of 300 samples plus one buffer, as a real capture would have.
  2. tools/train_capture.sh trains a model on it with NAM's official trainer (the ESR it reports is on
     the input file's own validation audio).
  3. A held-out DI the trainer never saw (Karplus-Strong plucked strings, prototypes/amp_sim.py's
     synth_test_riff, at -14 dBFS peak) goes through the circuit directly and through the trained model in
     our engine (ampsim_render, the app's chain). The capture played the input file 12 dB down, so the
     model gets the DI 12 dB up: what the app's input calibration does with the capture's metadata
     (input_level_dbu 0 against the interface's +12 dBu, docs/ASSUMPTIONS.md C9).
  4. The two are lined up (the circuit's output carries the simulated round trip, the model's doesn't) and
     gain-matched (the trainer normalizes the output level), and compared: ESR = energy of the difference
     over energy of the circuit's output, the same measure the trainer uses.

Needs the built tools (cmake --build build -j) and build-deps/nam/input.wav (tools/fetch_nam_input.sh).
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

import numpy as np
import soundfile as sf

REPO = pathlib.Path(__file__).resolve().parents[1]
CAPTURE = REPO / "build/ampsim_capture_artefacts/Release/ampsim_capture"
RENDER = REPO / "build/ampsim_render_artefacts/Release/ampsim_render"
INPUT = REPO / "build-deps/nam/input.wav"


def run(cmd, **kw):
    print("$ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, text=True, capture_output=True, **kw).stdout


def held_out_di(rate: int) -> np.ndarray:
    sys.path.insert(0, str(REPO / "prototypes"))
    from amp_sim import synth_test_riff  # noqa: E402  (the repo's own synthetic guitar)

    x = synth_test_riff(rate, seconds=4.0)
    return (x / np.max(np.abs(x)) * 10 ** (-14 / 20)).astype(np.float32)


def compare(model_out: np.ndarray, circuit_out: np.ndarray, max_lag: int = 2000):
    """ESR of the model against the circuit after the best lag (circuit later) and least-squares gain."""
    n = min(len(model_out), len(circuit_out)) - max_lag
    best = None
    for lag in range(max_lag):
        c = circuit_out[lag : lag + n]
        m = model_out[:n]
        g = np.dot(m, c) / np.dot(m, m)
        esr = np.sum((g * m - c) ** 2) / np.sum(c**2)
        if best is None or esr < best[0]:
            best = (esr, lag, g)
    return best


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--arch", default="nano", choices=["standard", "lite", "feather", "nano"])
    p.add_argument("--epochs", type=int, default=20)
    p.add_argument("--work-dir", default=str(REPO / "build/synthetic_capture"))
    p.add_argument("--skip-training", help="use this .nam instead of training one")
    args = p.parse_args()
    work = pathlib.Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    if not INPUT.exists():
        print("build-deps/nam/input.wav is missing: run tools/fetch_nam_input.sh")
        return 1

    print("\n1. The capture, through the simulated circuit")
    out = run([CAPTURE, "--simulate", "drive", "--input", INPUT, "--output", work / "output.wav"])
    print("   " + "\n   ".join(l.strip() for l in out.splitlines() if "Return:" in l or "Round trip" in l))

    if args.skip_training:
        nam = pathlib.Path(args.skip_training)
        summary = {}
    else:
        print(f"\n2. Training ({args.arch}, {args.epochs} epochs)")
        name = f"Synthetic drive, {args.arch}"
        log = run([REPO / "tools/train_capture.sh", "--input", INPUT, "--output", work / "output.wav", "--name", name,
                   "--tone-type", "crunch", "--gear-type", "pedal", "--input-level-dbu", "0", "--arch", args.arch,
                   "--epochs", str(args.epochs), "--out-dir", work])
        (work / "training_log.txt").write_text(log)
        print("   " + "\n   ".join(l for l in log.splitlines() if re.match(r"(Done in|Validation ESR|Final validation|neural-amp)", l)))
        nam = work / f"{name}.nam"
        summary = json.loads((work / f"{name} training.json").read_text())

    print("\n3. A held-out DI through the circuit and through the model in our engine")
    di = held_out_di(48000)
    sf.write(work / "heldout_di.wav", di, 48000, subtype="FLOAT")
    run([CAPTURE, "--simulate", "drive", "--input", work / "heldout_di.wav", "--output-level-db", "0", "--output", work / "heldout_circuit.wav"])
    run([RENDER, "--model", nam, "--input-gain", "12", "--no-normalize", work / "heldout_di.wav", work / "heldout_model.wav"])
    circuit, _ = sf.read(work / "heldout_circuit.wav", always_2d=True)
    model, _ = sf.read(work / "heldout_model.wav", always_2d=True)
    esr, lag, gain = compare(model[:, 0], circuit[:, 0])
    dry_esr, _, _ = compare(di.astype(np.float64), circuit[:, 0])

    print("\n4. Results")
    if summary:
        hist = summary.get("esr_by_epoch", [])
        print(f"   Trainer: {summary['architecture']}, {summary['epochs']} epochs on {summary['device']}, {summary['seconds']} s "
              f"({summary['seconds_per_epoch']} s per epoch); validation ESR {summary['validation_esr']:.4f}")
        if hist:
            print("   ESR by epoch: " + ", ".join(f"{i + 1}: {v:.4f}" for i, v in enumerate(hist) if i in {0, 1, 2, 4, 9, 19, 39, 59, 79, 99} or i == len(hist) - 1))
    print(f"   Held-out DI, model in our engine vs the circuit: ESR {esr:.4f} ({10 * np.log10(esr):.1f} dB), "
          f"after a {lag}-sample shift (the simulated round trip) and a gain of {20 * np.log10(abs(gain)):.1f} dB")
    print(f"   For scale, the dry DI as the 'model' (no distortion at all): ESR {dry_esr:.4f}")
    (work / "result.json").write_text(json.dumps({"heldout_esr": esr, "lag": lag, "gain_db": 20 * np.log10(abs(gain)),
                                                  "dry_esr": dry_esr, **summary}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

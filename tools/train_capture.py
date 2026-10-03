# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Trains a NAM capture with NAM's official trainer (the neural-amp-modeler package). Run it through
tools/train_capture.sh, which pins the trainer's version and runs this with uv (docs/CAPTURING.md).

    tools/train_capture.sh --input build-deps/nam/input.wav --output my_amp.wav \
        --name "Crunch, amp only" --tone-type crunch --gear-type amp

What happens, in order (all inside NAM's own nam.train.core.train, the same code its GUI runs):
  1. Checks: both files 48 kHz and the same length; the input recognised as NAM's v3.0.0 file.
  2. Latency: measured from the two calibration blips at 10.5 s and 11.5 s, and removed.
  3. Data checks: the two copies of the validation audio (start and end of the file) must match, or the
     gear changed during the capture (a knob moved, a tube warming up, noise).
  4. Training: a WaveNet learns to turn the input into the output, for --epochs passes over the data,
     on the Mac's GPU (Apple's MPS) when PyTorch can use it, otherwise the CPU.
  5. The best epoch (lowest validation error) is exported as a .nam file with your metadata.

ESR ("error-to-signal ratio") is NAM's accuracy number: the energy of (model - real) over the energy of
real, on the validation audio the model never trained on. 0 is perfect. The trainer's own verdicts:
below 0.01 "Great!", below 0.035 "Not bad!", below 0.1 "might sound ok", above that, something's off.
"""

import argparse
import json
import pathlib
import re
import sys
import time

# Brand names never go in a capture's metadata (CLAUDE.md); the app shows only tone_type, but the file
# travels. A warning, not a refusal: the name is yours.
BRANDS = ["marshall", "mesa", "boogie", "fender", "vox", "orange", "peavey", "5150", "evh", "engl", "bogner",
          "diezel", "friedman", "soldano", "laney", "ampeg", "randall", "revv", "fortin", "neural dsp", "ibanez",
          "boss", "tube screamer", "klon", "proco", "rat", "big muff", "celestion", "line 6", "kemper", "fractal"]


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input", required=True, help="NAM's input file (build-deps/nam/input.wav)")
    p.add_argument("--output", required=True, help="the recording from ampsim_capture")
    p.add_argument("--name", required=True, help='what the capture is, e.g. "Crunch, amp only" (no brands)')
    p.add_argument("--out-dir", default="trained", help="where the .nam and the training files go (default trained/)")
    p.add_argument("--modeled-by", default="Sean Snaider")
    p.add_argument("--tone-type", choices=["clean", "overdrive", "crunch", "hi_gain", "fuzz"], required=True)
    p.add_argument("--gear-type", choices=["amp", "pedal", "pedal_amp", "amp_cab", "amp_pedal_cab", "preamp", "studio"], required=True)
    p.add_argument("--arch", choices=["standard", "lite", "feather", "nano"], default="standard",
                   help="model size: standard (default, the most accurate) down to nano (the cheapest to run)")
    p.add_argument("--epochs", type=int, default=100, help="passes over the training audio (default 100, NAM's default)")
    p.add_argument("--input-level-dbu", type=float, help="analog level (dBu) that 0 dBFS of the input file reached the gear at")
    p.add_argument("--output-level-dbu", type=float, help="analog level (dBu) that 0 dBFS of the recording corresponds to")
    p.add_argument("--latency", type=int, help="the round trip in samples, if the automatic measurement fails")
    p.add_argument("--device", choices=["auto", "cpu"], default="auto", help="auto: Apple's GPU (MPS) if available")
    p.add_argument("--seed", type=int, default=0)
    return p.parse_args()


def esr_history(train_dir: pathlib.Path):
    """The validation ESR after each epoch, from the TensorBoard log Lightning writes."""
    try:
        from tensorboard.backend.event_processing.event_accumulator import EventAccumulator
    except ImportError:
        return []
    runs = sorted(train_dir.glob("lightning_logs/version_*"), key=lambda d: d.stat().st_mtime)
    if not runs:
        return []
    acc = EventAccumulator(str(runs[-1]), size_guidance={"scalars": 0})
    acc.Reload()
    if "ESR" not in acc.Tags().get("scalars", []):
        return []
    return [e.value for e in acc.Scalars("ESR")]


def main() -> int:
    args = parse_args()
    lowered = args.name.lower()
    for brand in BRANDS:
        if re.search(r"\b" + re.escape(brand) + r"\b", lowered):
            print(f'WARNING: the name mentions "{brand}". Captures that ship in the app keep brand and model names out (CLAUDE.md).')

    import torch

    from nam import __version__ as nam_version
    from nam.models.metadata import GearType, ToneType, UserMetadata
    from nam.train import core
    from nam.train import metadata as train_metadata

    device = "Apple GPU (MPS)" if torch.backends.mps.is_available() else "CPU"
    if args.device == "cpu":
        # NAM's trainer picks the accelerator itself (CUDA, then MPS, then CPU) in core._get_configs; this
        # wraps that one function to ask Lightning for the CPU instead.
        original = core._get_configs

        def cpu_configs(*a, **kw):
            data_config, model_config, learning_config = original(*a, **kw)
            learning_config["trainer"].pop("devices", None)
            learning_config["trainer"]["accelerator"] = "cpu"
            return data_config, model_config, learning_config

        core._get_configs = cpu_configs
        device = "CPU"

    user_metadata = UserMetadata(
        name=args.name,
        modeled_by=args.modeled_by,
        gear_type=GearType(args.gear_type),
        tone_type=ToneType(args.tone_type),
        input_level_dbu=args.input_level_dbu,
        output_level_dbu=args.output_level_dbu,
    )
    out_dir = pathlib.Path(args.out_dir).resolve()
    basename = re.sub(r"[^A-Za-z0-9 ,._()+-]", "", args.name).strip() or "capture"
    train_dir = out_dir / (basename + " training")
    train_dir.mkdir(parents=True, exist_ok=True)

    print(f"neural-amp-modeler {nam_version}, PyTorch {torch.__version__}, training on: {device}")
    print(f"Architecture {args.arch}, {args.epochs} epochs. Training files go to {train_dir}")
    started = time.time()
    result = core.train(
        args.input,
        args.output,
        str(train_dir),
        epochs=args.epochs,
        latency=args.latency,
        architecture=core.Architecture(args.arch),
        seed=args.seed,
        save_plot=True,
        silent=True,
        modelname=basename,
        local=False,
        user_metadata=user_metadata,
    )
    elapsed = time.time() - started

    if result is None or result.model is None:
        print("\nTraining didn't produce a model: the checks above say why (most often the two copies of the")
        print("validation audio didn't match, meaning the gear changed during the capture, or the latency")
        print("couldn't be measured). Fix that and capture again; see docs/CAPTURING.md, 'When something fails'.")
        return 1

    result.model.net.export(
        str(out_dir),
        basename=basename,
        user_metadata=user_metadata,
        other_metadata={train_metadata.TRAINING_KEY: result.metadata.model_dump()},
    )
    nam_file = out_dir / (basename + ".nam")
    esr = result.metadata.validation_esr
    history = esr_history(train_dir)
    latency = result.metadata.data.latency
    summary = {
        "nam_file": str(nam_file),
        "trainer": nam_version,
        "architecture": args.arch,
        "epochs": args.epochs,
        "device": device,
        "seconds": round(elapsed, 1),
        "seconds_per_epoch": round(elapsed / max(1, args.epochs), 2),
        "validation_esr": esr,
        "esr_by_epoch": history,
        "latency_samples": latency.calibration.recommended if latency.calibration else None,
    }
    (out_dir / (basename + " training.json")).write_text(json.dumps(summary, indent=2))

    def verdict(e):
        return "Great!" if e < 0.01 else "Not bad!" if e < 0.035 else "might sound ok" if e < 0.1 else "probably won't sound great" if e < 0.3 else "something went wrong"

    print(f"\nDone in {elapsed / 60:.1f} min ({elapsed / max(1, args.epochs):.1f} s per epoch on {device}).")
    if history:
        picks = sorted({0, len(history) // 4, len(history) // 2, 3 * len(history) // 4, len(history) - 1})
        print("Validation ESR by epoch: " + ", ".join(f"{i + 1}: {history[i]:.4f}" for i in picks))
    if esr is not None:
        print(f"Final validation ESR (best epoch): {esr:.4f}  ({verdict(esr)})")
    print(f"Model: {nam_file}")
    print(f"Plot of the model against the real gear: {train_dir / (basename + '.png')}")
    print("Next: listen to it in the app (load it in an amp slot), then see docs/CAPTURING.md step 7 to bundle it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

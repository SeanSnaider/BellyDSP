#!/usr/bin/env bash
# Trains a NAM capture from ampsim_capture's recording, with NAM's official trainer at the version pinned in
# tools/deps.conf (NAM_TRAINER_VERSION). See docs/CAPTURING.md, and tools/train_capture.py --help for options.
#
#   tools/train_capture.sh --input build-deps/nam/input.wav --output my_amp.wav \
#       --name "Crunch, amp only" --tone-type crunch --gear-type amp [--arch standard] [--epochs 100]
#
# What uv does here: `uv run --with neural-amp-modeler==X` makes (and caches) a private Python environment
# with exactly that trainer and its dependencies (PyTorch among them, about 1 GB the first time), runs the
# script in it, and leaves the system's Python alone. Later runs reuse the cache and start in seconds.
set -euo pipefail
source "$(dirname "$0")/release/lib.sh"
version="$(conf_get NAM_TRAINER_VERSION "$DEPS_CONF")"
command -v uv > /dev/null || die "uv isn't installed (it lives in ~/.local/bin; see docs/SETUP.md)."
exec uv run --quiet --python 3.12 --with "neural-amp-modeler==$version" python "$REPO_ROOT/tools/train_capture.py" "$@"

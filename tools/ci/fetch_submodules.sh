#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# CI: fetch the pinned submodules, with retries. Used by .github/workflows/windows.yml and macos-ci.yml
# instead of actions/checkout's `submodules: recursive`.
#
# Why: Eigen (inside NeuralAmpModelerCore) is hosted on gitlab.com, which sometimes refuses with "GitLab is
# currently unable to handle this request due to load". On 2026-10-05 that failed the first public CI runs
# before anything compiled. So every fetch here is retried with growing pauses, and the last tries fetch
# without --depth in case a shallow fetch of the pinned commit is what failed. The workflows also cache the
# result (keyed by the pinned commits), so most runs don't touch GitLab at all.
#
# What it fetches: JUCE and demucs.cpp without their own submodules (demucs.cpp vendors libraries the
# build never uses, ASSUMPTIONS TM17), and NeuralAmpModelerCore with its nested ones (Eigen, AudioDSPTools).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

attempt() # attempt <tries> <command...>: run the command until it succeeds, pausing 30, 60, ... s between tries
{
    local tries="$1"; shift
    local n=1
    until "$@"; do
        if [ "$n" -ge "$tries" ]; then return 1; fi
        echo "    try $n failed; trying again in $((n * 30)) s"
        sleep $((n * 30))
        n=$((n + 1))
    done
}

fetch() # fetch <path> <recursive: yes|no>
{
    local path="$1" recursive="$2"
    local flags=(--init --force)
    [ "$recursive" = yes ] && flags+=(--recursive)
    echo "==> $path"
    attempt 3 git -c protocol.version=2 submodule update "${flags[@]}" --depth=1 -- "$path" && return 0
    echo "    the shallow fetch kept failing; fetching the full history instead"
    attempt 2 git -c protocol.version=2 submodule update "${flags[@]}" -- "$path"
}

git submodule sync --recursive
fetch third_party/JUCE no
fetch third_party/demucs.cpp no
fetch third_party/NeuralAmpModelerCore yes
git submodule status --recursive

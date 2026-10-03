#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# Downloads the pinned auto-update frameworks into build-deps/ (gitignored) and checks each download's
# SHA-256 against tools/deps.conf, so a build can never pick up a different file than the one that was
# reviewed. Safe to run again: anything already there and verified is skipped.
#
#   tools/fetch_deps.sh               Sparkle (what a macOS release build needs)
#   tools/fetch_deps.sh --windows     also WinSparkle (to look at its headers; CI fetches its own copy)
#
# Why not git submodules: Sparkle and WinSparkle are used as their official prebuilt release binaries
# (Sparkle.framework is built and signed by its project), not compiled from source here.
set -euo pipefail
source "$(dirname "$0")/release/lib.sh"

want_windows=0
for arg in "$@"; do
    case "$arg" in
        --windows) want_windows=1 ;;
        -h|--help) awk 'NR < 5 { next } /^#/ { print; next } { exit }' "$0"; exit 0 ;;
        *) die "unknown option $arg" ;;
    esac
done

mkdir -p "$REPO_ROOT/build-deps/downloads"

# fetch NAME URL SHA256 FILE: downloads FILE (unless it's there with the right hash) and verifies it.
fetch() {
    local name="$1" url="$2" expected="$3" file="$4"
    if [ -f "$file" ] && [ "$(sha256 "$file")" = "$expected" ]; then
        ok "$name already downloaded and verified"
        return
    fi
    run curl --fail --location --silent --show-error --output "$file.part" "$url"
    local actual
    actual="$(sha256 "$file.part")"
    if [ "$actual" != "$expected" ]; then
        rm -f "$file.part"
        die "$name: SHA-256 mismatch. Expected $expected (tools/deps.conf), got $actual. Don't use this file."
    fi
    mv "$file.part" "$file"
    ok "$name downloaded, SHA-256 $actual matches tools/deps.conf"
}

step "Sparkle $SPARKLE_VERSION" "The macOS updater: Sparkle.framework goes inside the app, and its bin/ tools sign updates."
tarball="$REPO_ROOT/build-deps/downloads/Sparkle-$SPARKLE_VERSION.tar.xz"
fetch "Sparkle $SPARKLE_VERSION" \
    "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz" \
    "$SPARKLE_SHA256" "$tarball"
if [ ! -f "$SPARKLE_DIR/.verified" ] || [ "$(cat "$SPARKLE_DIR/.verified")" != "$SPARKLE_SHA256" ]; then
    rm -rf "$SPARKLE_DIR"
    mkdir -p "$SPARKLE_DIR"
    run tar -xJf "$tarball" -C "$SPARKLE_DIR"
    printf '%s' "$SPARKLE_SHA256" > "$SPARKLE_DIR/.verified"
fi
ok "Sparkle is in ${SPARKLE_DIR#$REPO_ROOT/}"

if [ "$want_windows" = 1 ]; then
    step "WinSparkle $WINSPARKLE_VERSION" "The Windows updater (the CI workflow fetches and checks the same pinned file)."
    zip="$REPO_ROOT/build-deps/downloads/WinSparkle-$WINSPARKLE_VERSION.zip"
    fetch "WinSparkle $WINSPARKLE_VERSION" \
        "https://github.com/vslavik/winsparkle/releases/download/v$WINSPARKLE_VERSION/WinSparkle-$WINSPARKLE_VERSION.zip" \
        "$WINSPARKLE_SHA256" "$zip"
    dest="$REPO_ROOT/build-deps/WinSparkle-$WINSPARKLE_VERSION"
    if [ ! -d "$dest" ]; then
        tmp="$REPO_ROOT/build-deps/winsparkle.tmp"
        rm -rf "$tmp" && mkdir -p "$tmp"
        run unzip -q "$zip" -d "$tmp"
        mv "$tmp/WinSparkle-$WINSPARKLE_VERSION" "$dest"
        rm -rf "$tmp"
    fi
    ok "WinSparkle is in ${dest#$REPO_ROOT/}"
fi

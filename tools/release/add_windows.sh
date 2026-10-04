#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# Adds the Windows installer to a release that's already published, signing it here on the Mac (so the
# Ed25519 private key never has to be a GitHub secret). Use it when the Windows CI job didn't publish by
# itself (no AMPSIM_ED_PRIVATE_KEY secret on the public repo):
#
#   1. GitHub > SeanSnaider/BellyDSP > Actions > the "Windows build" run for tag vX.Y.Z > Artifacts: download
#      BellyDSP-X.Y.Z-windows and unzip it (it holds BellyDSP-X.Y.Z-windows-setup.exe).
#   2. tools/release/add_windows.sh X.Y.Z ~/Downloads/BellyDSP-X.Y.Z-windows
#
# It copies the installer to BellyDSP-X.Y.Z-windows-update.exe (the same bytes under the name only
# WinSparkle downloads, so GitHub's counts tell first installs from updates; docs/WEBSITE.md), signs that
# copy with the same key as the Mac update (from the login keychain, or AMPSIM_ED_KEY_FILE), writes
# appcast-windows.xml pointing at it, and uploads the installer, the update copy, and the appcast to the
# vX.Y.Z release.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

VERSION="${1:-}"; DIR="${2:-}"
[ -n "$VERSION" ] && [ -d "$DIR" ] || die "usage: $0 <version> <folder with BellyDSP-<version>-windows-setup.exe>"
SETUP="$DIR/BellyDSP-$VERSION-windows-setup.exe"
[ -f "$SETUP" ] || die "no $SETUP"
[ -x "$SPARKLE_BIN/sign_update" ] || "$REPO_ROOT/tools/fetch_deps.sh"
[ -n "$ED_PUBLIC_KEY" ] || die "ED_PUBLIC_KEY is empty in tools/release/release.conf"
OUT="$REPO_ROOT/dist/$VERSION"
mkdir -p "$OUT"
cp "$SETUP" "$OUT/"
SETUP="$OUT/$(basename "$SETUP")"

UPDATE="$(windows_update_file "$SETUP")"
ok "$(basename "$UPDATE"): the installer's bytes under the name only the updater downloads"

if [ -n "${AMPSIM_ED_KEY_FILE:-}" ]; then sign_args=(--ed-key-file "$AMPSIM_ED_KEY_FILE"); else sign_args=(--account "${AMPSIM_ED_KEY_ACCOUNT:-ed25519}"); fi
step "Signing $(basename "$UPDATE")" "WinSparkle checks this Ed25519 signature before running the installer."
sig="$("$SPARKLE_BIN/sign_update" "${sign_args[@]}" -p "$UPDATE")"
python_run "$REPO_ROOT/tools/release/ed25519.py" verify "$ED_PUBLIC_KEY" "$UPDATE" "$sig" >/dev/null || die "the signature doesn't verify against ED_PUBLIC_KEY"
ok "signed and verified"

notes=(); [ -f "$REPO_ROOT/release-notes/$VERSION.md" ] && notes=(--notes "$REPO_ROOT/release-notes/$VERSION.md")
python_run "$REPO_ROOT/tools/release/make_appcast.py" --platform windows --version "$VERSION" --file "$UPDATE" \
    --url "${AMPSIM_DOWNLOAD_BASE:-https://github.com/$RELEASES_REPO/releases/download/v$VERSION}/$(basename "$UPDATE")" \
    --signature "$sig" --repo "$RELEASES_REPO" --out "$OUT/appcast-windows.xml" ${notes[@]+"${notes[@]}"}

"$REPO_ROOT/tools/release/github_release.sh" "v$VERSION" - "$SETUP" "$UPDATE" "$OUT/appcast-windows.xml"

#!/usr/bin/env bash
# Adds the Windows installer to a release that's already published, signing it here on the Mac (so the
# Ed25519 private key never has to be a GitHub secret). Use it when the Windows CI job didn't publish by
# itself (no AMPSIM_ED_PRIVATE_KEY or RELEASES_TOKEN secret):
#
#   1. GitHub > the source repo > Actions > the "Windows build" run for tag vX.Y.Z > Artifacts: download
#      AmpSim-X.Y.Z-windows and unzip it (it holds AmpSim-X.Y.Z-windows-setup.exe).
#   2. tools/release/add_windows.sh X.Y.Z ~/Downloads/AmpSim-X.Y.Z-windows
#
# It signs the installer with the same key as the Mac update (from the login keychain, or
# AMPSIM_ED_KEY_FILE), writes appcast-windows.xml, and uploads both to the vX.Y.Z release.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

VERSION="${1:-}"; DIR="${2:-}"
[ -n "$VERSION" ] && [ -d "$DIR" ] || die "usage: $0 <version> <folder with AmpSim-<version>-windows-setup.exe>"
SETUP="$DIR/AmpSim-$VERSION-windows-setup.exe"
[ -f "$SETUP" ] || die "no $SETUP"
[ -x "$SPARKLE_BIN/sign_update" ] || "$REPO_ROOT/tools/fetch_deps.sh"
[ -n "$ED_PUBLIC_KEY" ] || die "ED_PUBLIC_KEY is empty in tools/release/release.conf"
OUT="$REPO_ROOT/dist/$VERSION"
mkdir -p "$OUT"
cp "$SETUP" "$OUT/"
SETUP="$OUT/$(basename "$SETUP")"

if [ -n "${AMPSIM_ED_KEY_FILE:-}" ]; then sign_args=(--ed-key-file "$AMPSIM_ED_KEY_FILE"); else sign_args=(--account "${AMPSIM_ED_KEY_ACCOUNT:-ed25519}"); fi
step "Signing $(basename "$SETUP")" "WinSparkle checks this Ed25519 signature before running the installer."
sig="$("$SPARKLE_BIN/sign_update" "${sign_args[@]}" -p "$SETUP")"
python_run "$REPO_ROOT/tools/release/ed25519.py" verify "$ED_PUBLIC_KEY" "$SETUP" "$sig" >/dev/null || die "the signature doesn't verify against ED_PUBLIC_KEY"
ok "signed and verified"

notes=(); [ -f "$REPO_ROOT/release-notes/$VERSION.md" ] && notes=(--notes "$REPO_ROOT/release-notes/$VERSION.md")
python_run "$REPO_ROOT/tools/release/make_appcast.py" --platform windows --version "$VERSION" --file "$SETUP" \
    --url "${AMPSIM_DOWNLOAD_BASE:-https://github.com/$RELEASES_REPO/releases/download/v$VERSION}/$(basename "$SETUP")" \
    --signature "$sig" --repo "$RELEASES_REPO" --out "$OUT/appcast-windows.xml" ${notes[@]+"${notes[@]}"}

"$REPO_ROOT/tools/release/github_release.sh" "v$VERSION" - "$SETUP" "$OUT/appcast-windows.xml"

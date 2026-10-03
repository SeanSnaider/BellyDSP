#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# Packages a signed "BellyDSP.app" two ways (called by release.sh):
#
#   tools/release/package_mac.sh "path/to/BellyDSP.app" <version> <output dir>
#
#   BellyDSP-<version>.dmg     for a first install: a disk image with the app, a shortcut to
#                              Applications (drag one onto the other), "Read me first.txt", LICENSE.txt
#                              (the AGPLv3), and the notices
#   BellyDSP-<version>-mac.zip for Sparkle's updates: the app alone, zipped with ditto so the bundle's
#                              symlinks, permissions, and code signatures survive (a plain `zip` breaks
#                              Sparkle.framework's symlinks and with them the signature)
#
# Both use only tools built into macOS (hdiutil, ditto).
set -euo pipefail
source "$(dirname "$0")/lib.sh"

APP="${1:-}"; VERSION="${2:-}"; OUT="${3:-}"
[ -d "$APP" ] && [ -n "$VERSION" ] && [ -n "$OUT" ] || die "usage: $0 path/to/BellyDSP.app <version> <output dir>"
mkdir -p "$OUT"
ZIP="$OUT/BellyDSP-$VERSION-mac.zip"
DMG="$OUT/BellyDSP-$VERSION.dmg"

step "Update archive: $(basename "$ZIP")" "What Sparkle downloads. --keepParent puts \"BellyDSP.app\" at the top of the zip."
rm -f "$ZIP"
run ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"
info "$(du -h "$ZIP" | cut -f1) $ZIP"

step "Disk image: $(basename "$DMG")" "What friends download first. Compressed (UDZO), read-only."
stage="$(mktemp -d)/BellyDSP $VERSION"
mkdir -p "$stage"
run ditto "$APP" "$stage/BellyDSP.app"
ln -s /Applications "$stage/Applications"
sed "s/VERSION_PLACEHOLDER/$VERSION/" "$REPO_ROOT/installer/macos/Read me first.txt" > "$stage/Read me first.txt"
# BellyDSP's licence (the AGPLv3) and the notices, next to the app as well as inside it.
for f in LICENSE.txt THIRD_PARTY_NOTICES.txt; do
    [ -f "$APP/Contents/Resources/$f" ] || die "the app has no $f in Contents/Resources (a release must carry it)"
    cp "$APP/Contents/Resources/$f" "$stage/"
done
rm -f "$DMG"
run hdiutil create -quiet -volname "BellyDSP $VERSION" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$DMG"
rm -rf "$(dirname "$stage")"
info "$(du -h "$DMG" | cut -f1) $DMG"

step "Checking the disk image" "Mount it read-only, list what a friend will see, and verify the app's signature inside it."
mnt="$(mktemp -d)"
run hdiutil attach -quiet -nobrowse -readonly -mountpoint "$mnt" "$DMG"
ls -la "$mnt" | sed 's/^/      /'
codesign --verify --deep --strict "$mnt/BellyDSP.app" && ok "the app inside the DMG verifies"
hdiutil detach -quiet "$mnt"
rmdir "$mnt" 2>/dev/null || true

#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# Notarizes and staples a DMG. Only for the Developer ID path (AMPSIM_SIGN_IDENTITY set): Apple won't
# notarize a self-signed app. release.sh calls it after packaging.
#
#   tools/release/notarize.sh dist/0.2.0/AmpSim-0.2.0.dmg
#
# UNTESTED: there's no Developer ID to test with. What it needs, once you have one ($99/year):
#   1. A "Developer ID Application" certificate in your login keychain (Xcode or developer.apple.com).
#   2. An app-specific password (appleid.apple.com > Sign-In and Security), stored once for notarytool:
#        xcrun notarytool store-credentials ampsim-notary --apple-id YOU@EXAMPLE.COM --team-id TEAMID
#      (it asks for the password and keeps it in your keychain under the profile name).
#   3. AMPSIM_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" when you run release.sh.
# The app is then signed with the hardened runtime (sign_app.sh) and Gatekeeper opens it with no
# "Open Anyway" step. Note that switching from the self-signed identity to a Developer ID changes the
# app's designated requirement: Sparkle still installs that update (its Ed25519 signature is valid and
# the key hasn't changed), but macOS asks friends for the microphone permission once more.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

DMG="${1:-}"
[ -f "$DMG" ] || die "usage: $0 path/to/AmpSim-x.y.z.dmg"
PROFILE="${AMPSIM_NOTARY_PROFILE:-ampsim-notary}"

step "Signing the disk image" "Developer ID signs the DMG itself too, so Gatekeeper can check it before mounting."
run codesign --force --sign "$AMPSIM_SIGN_IDENTITY" --timestamp "$DMG"

step "Submitting to Apple's notary service" "Uploads the DMG and waits for the verdict (usually a few minutes)."
run xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait

step "Stapling the ticket" "Attaches Apple's approval to the DMG so it opens offline too."
run xcrun stapler staple "$DMG"
run xcrun stapler validate "$DMG"
run spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
ok "notarized"

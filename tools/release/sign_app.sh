#!/usr/bin/env bash
# Code-signs "Amp Sim.app" (called by release.sh; runnable on its own):
#
#   tools/release/sign_app.sh "path/to/Amp Sim.app"
#
# Signing goes inside out, because signing a bundle seals everything inside it: Sparkle's helper tool
# (Autoupdate) and helper app (Updater.app) first, then Sparkle.framework, then the app. (`codesign
# --deep` would do it in one go, but Apple discourages it for signing: it can't give each piece the
# right options.)
#
# Two identities:
#   default                  the self-signed certificate from make_signing_identity.sh, without the
#                            hardened runtime (see below). Friends approve the app once on first launch.
#   AMPSIM_SIGN_IDENTITY=... a real "Developer ID Application: ..." identity (in your login keychain):
#                            hardened runtime, a secure timestamp, and the audio-input entitlement, ready
#                            for notarize.sh. UNTESTED (there's no Developer ID yet).
#
# Why no hardened runtime for the self-signed build: the hardened runtime turns on library validation,
# which only loads frameworks signed by Apple or by the same Team ID. A self-signed certificate has no
# Team ID, so the app couldn't load Sparkle.framework (tested: dyld refuses it). The runtime is only
# required for notarization, which a self-signed app can't get anyway. AMPSIM_HARDENED_RUNTIME=1 forces
# it on with a self-signed identity, for experiments.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

APP="${1:-}"
[ -d "$APP" ] && [ -f "$APP/Contents/Info.plist" ] || die "usage: $0 path/to/Amp\\ Sim.app"
ENTITLEMENTS="$REPO_ROOT/tools/release/AmpSim.entitlements"

flags=(--force)
if [ -n "${AMPSIM_SIGN_IDENTITY:-}" ]; then
    step "Signing with \"$AMPSIM_SIGN_IDENTITY\"" "Developer ID mode: hardened runtime, secure timestamp, entitlements (for notarization)."
    identity="$AMPSIM_SIGN_IDENTITY"
    flags+=(--sign "$identity" --options runtime --timestamp)
    runtime=1
else
    step "Signing with the self-signed identity \"$SIGN_IDENTITY_NAME\"" \
         "The same certificate every release, so macOS keeps the microphone permission and Sparkle accepts updates."
    [ -f "$SIGNING_KEYCHAIN" ] || die "No signing keychain at $SIGNING_KEYCHAIN. Run tools/release/make_signing_identity.sh once (docs/RELEASING.md)."
    pw="$(signing_keychain_password)" || true
    [ -n "$pw" ] || die "Can't find the signing keychain's password (login keychain item '$SIGNING_KEYCHAIN_SERVICE' or \$AMPSIM_KEYCHAIN_PASSWORD)."
    security unlock-keychain -p "$pw" "$SIGNING_KEYCHAIN"
    ensure_keychain_in_search_list "$SIGNING_KEYCHAIN"
    identity="$(identity_hash "$SIGN_IDENTITY_NAME" "$SIGNING_KEYCHAIN")"
    [ -n "$identity" ] || die "No identity called \"$SIGN_IDENTITY_NAME\" in $SIGNING_KEYCHAIN"
    info "Certificate SHA-1 $identity, keychain ${SIGNING_KEYCHAIN/#$HOME/~}"
    flags+=(--sign "$identity" --keychain "$SIGNING_KEYCHAIN")
    runtime="${AMPSIM_HARDENED_RUNTIME:-0}"
    [ "$runtime" = 1 ] && flags+=(--options runtime)
fi

FW="$APP/Contents/Frameworks/Sparkle.framework"
if [ -d "$FW" ]; then
    step "Sparkle.framework, inside out"
    # Only for sandboxed apps; the build already leaves them out (CMakeLists.txt). Belt and braces.
    rm -rf "$FW/Versions/B/XPCServices" "$FW/XPCServices"
    run codesign "${flags[@]}" "$FW/Versions/B/Autoupdate"
    run codesign "${flags[@]}" "$FW/Versions/B/Updater.app"
    run codesign "${flags[@]}" "$FW"
else
    warn "No Sparkle.framework in the app: this build won't update itself (configure with -DAMPSIM_UPDATER=ON)."
fi

step "The app"
app_flags=("${flags[@]}")
[ "$runtime" = 1 ] && app_flags+=(--entitlements "$ENTITLEMENTS")
run codesign "${app_flags[@]}" "$APP"

step "Verifying" "--deep --strict checks every nested signature and that nothing was added or changed after signing."
run codesign --verify --deep --strict --verbose=2 "$APP"
info "Designated requirement (pins the certificate, so every release has the same identity):"
codesign -d -r- "$APP" 2>&1 | sed -n 's/^designated => /      /p'
codesign -dv "$APP" 2>&1 | grep -E "^(Identifier|Format|CodeDirectory|Authority|TeamIdentifier|Runtime Version)" | sed 's/^/      /'
ok "signed"

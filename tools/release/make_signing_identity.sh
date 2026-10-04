#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# ONE-TIME SETUP: creates BellyDSP's self-signed code-signing identity (docs/RELEASING.md, step 3).
#
# Why: macOS remembers permissions (the microphone) and Sparkle accepts updates by the app's code
# signature. An unsigned or ad-hoc signed app gets a new identity every build, so every update would
# lose the mic permission and Sparkle couldn't tell the new version is from the same developer. A
# self-signed certificate is free and gives every build the same identity. (It doesn't satisfy
# Gatekeeper the way a paid Developer ID does: players still approve the app once on first launch.)
#
# What it does:
#   1. makes an RSA key and a certificate (20 years) marked for code signing, with openssl;
#   2. creates a keychain just for it, ~/Library/Keychains/ampsim-signing.keychain-db, with a random
#      password stored in your login keychain (or $AMPSIM_KEYCHAIN_PASSWORD if you set it);
#   3. imports the identity and allows codesign to use it without asking (set-key-partition-list), so
#      signing never pops a dialog;
#   4. writes a backup .p12 to ~/.ampsim-release/ and prints the certificate's fingerprint.
#
# BACK UP THE .p12 AND ITS PASSWORD (a password manager is good). A new certificate means every player's
# app sees a different developer: the next update still installs (the Ed25519 key vouches for it) but
# macOS asks for the microphone again. Losing it isn't fatal; losing the Ed25519 key is (RELEASING.md).
#
#   tools/release/make_signing_identity.sh            create it (refuses if it already exists)
#   tools/release/make_signing_identity.sh --check    show what's there and test-sign a file
#
# Environment (for tests; Sean doesn't need these): AMPSIM_SIGNING_KEYCHAIN (keychain path),
# AMPSIM_KEYCHAIN_PASSWORD (its password, not stored), AMPSIM_SIGNING_BACKUP_DIR, AMPSIM_SIGN_IDENTITY_NAME.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

NAME="${AMPSIM_SIGN_IDENTITY_NAME:-$SIGN_IDENTITY_NAME}"
BACKUP_DIR="${AMPSIM_SIGNING_BACKUP_DIR:-$HOME/.ampsim-release}"
mode=create
case "${1:-}" in
    --check) mode=check ;;
    "") ;;
    -h|--help) awk 'NR < 5 { next } /^#/ { print; next } { exit }' "$0"; exit 0 ;;
    *) die "unknown option $1" ;;
esac

[ "$(uname)" = Darwin ] || die "This runs on macOS (it uses the keychain and codesign)."

# test_sign: signs a copy of /usr/bin/true with the identity and shows its designated requirement.
test_sign() {
    local pw="$1" tmp
    tmp="$(mktemp -d)"
    cp /usr/bin/true "$tmp/ampsim-signing-test"
    security unlock-keychain -p "$pw" "$SIGNING_KEYCHAIN"
    ensure_keychain_in_search_list "$SIGNING_KEYCHAIN"
    local hash
    hash="$(identity_hash "$NAME" "$SIGNING_KEYCHAIN")"
    [ -n "$hash" ] || die "No identity called \"$NAME\" in $SIGNING_KEYCHAIN"
    run codesign --force --sign "$hash" --keychain "$SIGNING_KEYCHAIN" --identifier com.seansnaider.bellydsp.signingtest "$tmp/ampsim-signing-test"
    run codesign --verify --strict "$tmp/ampsim-signing-test"
    info "Designated requirement (what macOS and Sparkle compare between versions):"
    codesign -d -r- "$tmp/ampsim-signing-test" 2>&1 | sed -n 's/^designated => /      /p'
    rm -rf "$tmp"
}

if [ "$mode" = check ]; then
    step "Checking the signing identity" "Is it there, and can codesign use it without a prompt?"
    [ -f "$SIGNING_KEYCHAIN" ] || die "No signing keychain at $SIGNING_KEYCHAIN. Run this script without --check first."
    pw="$(signing_keychain_password)" || true
    [ -n "$pw" ] || die "Can't find the keychain's password (login keychain item '$SIGNING_KEYCHAIN_SERVICE' or \$AMPSIM_KEYCHAIN_PASSWORD)."
    security unlock-keychain -p "$pw" "$SIGNING_KEYCHAIN"
    # "CSSMERR_TP_NOT_TRUSTED" is expected: nobody but us vouches for a self-signed certificate, and
    # codesign signs with it anyway.
    run security find-identity -p codesigning "$SIGNING_KEYCHAIN"
    test_sign "$pw"
    ok "codesign can sign with \"$NAME\""
    exit 0
fi

step "Creating the self-signed code-signing identity \"$NAME\"" \
     "One time only. Every release is signed with it so installed copies recognise their updates."
if [ -f "$SIGNING_KEYCHAIN" ]; then
    die "$SIGNING_KEYCHAIN already exists. Making a second identity would change the app's identity for everyone.
       To inspect it: $0 --check. To really start over, delete that keychain yourself first."
fi
command -v openssl >/dev/null || die "openssl not found (macOS ships one at /usr/bin/openssl)"

# The keychain password: given, or random and kept in the login keychain.
if [ -n "${AMPSIM_KEYCHAIN_PASSWORD:-}" ]; then
    pw="$AMPSIM_KEYCHAIN_PASSWORD"
    info "Using the keychain password from \$AMPSIM_KEYCHAIN_PASSWORD (not stored anywhere)."
else
    pw="$(openssl rand -base64 24)"
    # (Not echoed: the command line holds the password.)
    security add-generic-password -U -a "$USER" -s "$SIGNING_KEYCHAIN_SERVICE" \
        -l "BellyDSP signing keychain password" -w "$pw"
    ok "Stored a random password for the signing keychain in your login keychain (item \"$SIGNING_KEYCHAIN_SERVICE\")."
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat > "$work/cert.cnf" <<EOF
[ req ]
distinguished_name = dn
x509_extensions = codesign
prompt = no

[ dn ]
CN = $NAME
O = Sean Snaider

[ codesign ]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
subjectKeyIdentifier = hash
EOF

step "1/4 Key and certificate" "RSA 3072, SHA-256, valid 20 years, extended key usage: code signing."
run openssl req -x509 -newkey rsa:3072 -sha256 -days 7300 -nodes -config "$work/cert.cnf" \
    -keyout "$work/key.pem" -out "$work/cert.pem" 2>/dev/null
openssl x509 -in "$work/cert.pem" -noout -subject -enddate | sed 's/^/    /'
# A .p12 bundles the key and certificate for importing (LibreSSL's default encryption, which the
# keychain can read).
info "openssl pkcs12 -export ... -out identity.p12   (password not shown)"
openssl pkcs12 -export -inkey "$work/key.pem" -in "$work/cert.pem" -name "$NAME" \
    -out "$work/identity.p12" -passout "pass:$pw"

step "2/4 A keychain just for signing" "Separate from your login keychain, so its settings can't affect anything else."
info "security create-keychain $SIGNING_KEYCHAIN   (password not shown)"
security create-keychain -p "$pw" "$SIGNING_KEYCHAIN"
run security set-keychain-settings "$SIGNING_KEYCHAIN"   # no auto-lock timeout
security unlock-keychain -p "$pw" "$SIGNING_KEYCHAIN"
# codesign only looks in keychains on the search list (Keychain Access will show it too).
ensure_keychain_in_search_list "$SIGNING_KEYCHAIN"
run security list-keychains -d user

step "3/4 Import, and let codesign use the key without asking"
info "security import identity.p12 -k ${SIGNING_KEYCHAIN##*/} -T /usr/bin/codesign   (password not shown)"
security import "$work/identity.p12" -k "$SIGNING_KEYCHAIN" -P "$pw" -T /usr/bin/codesign -T /usr/bin/security
# The partition list says which Apple tools may use the private key without a GUI prompt.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$pw" "$SIGNING_KEYCHAIN" >/dev/null
ok "codesign may use the key without prompting"

step "4/4 Backup and test"
mkdir -p "$BACKUP_DIR" && chmod 700 "$BACKUP_DIR"
cp "$work/identity.p12" "$BACKUP_DIR/ampsim-signing-identity.p12"
chmod 600 "$BACKUP_DIR/ampsim-signing-identity.p12"
fingerprint="$(openssl x509 -in "$work/cert.pem" -noout -fingerprint -sha1 | sed 's/.*=//; s/://g')"
test_sign "$pw"

cat <<EOF

${GREEN}Done.${RESET} The identity "$NAME" is in $SIGNING_KEYCHAIN.
  Certificate SHA-1: $fingerprint (the designated requirement above pins this)
  Backup: $BACKUP_DIR/ampsim-signing-identity.p12
  Its password: $(if [ -n "${AMPSIM_KEYCHAIN_PASSWORD:-}" ]; then echo "the one you gave in \$AMPSIM_KEYCHAIN_PASSWORD"; else echo "the login keychain item \"$SIGNING_KEYCHAIN_SERVICE\"
       (security find-generic-password -a \"\$USER\" -s $SIGNING_KEYCHAIN_SERVICE -w)"; fi)

${BOLD}Copy the .p12 and its password somewhere safe now (a password manager).${RESET}
EOF

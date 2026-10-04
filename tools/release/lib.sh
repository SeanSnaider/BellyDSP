# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

# Shared helpers for the release scripts. Sourced, not run: `source "$(dirname "$0")/lib.sh"`.
#
# Every script talks a lot on purpose: each step says what it's doing and why, so a release log reads
# like a checklist and a failure says where it happened.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RELEASE_CONF="$REPO_ROOT/tools/release/release.conf"
DEPS_CONF="$REPO_ROOT/tools/deps.conf"

if [ -t 1 ]; then
    BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RESET=$'\033[0m'
else
    BOLD=""; DIM=""; RED=""; GREEN=""; YELLOW=""; RESET=""
fi

# step "Title" "why this step exists"
step() { printf '\n%s==> %s%s\n' "$BOLD" "$1" "$RESET"; [ -n "${2:-}" ] && printf '%s    %s%s\n' "$DIM" "$2" "$RESET"; return 0; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %sOK%s %s\n' "$GREEN" "$RESET" "$*"; }
warn() { printf '    %sWARNING%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
die()  { printf '\n%sERROR%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }

# Runs a command after printing it, so the log shows exactly what ran.
run() { printf '    %s$ %s%s\n' "$DIM" "$*" "$RESET"; "$@"; }

# conf_get KEY FILE: the value of KEY=VALUE in a config file (no shell sourcing, so values may have
# spaces and parentheses).
conf_get() {
    local key="$1" file="$2"
    sed -n "s/^${key}=//p" "$file" | tail -n 1
}

# The pinned dependency versions.
SPARKLE_VERSION="$(conf_get SPARKLE_VERSION "$DEPS_CONF")"
SPARKLE_SHA256="$(conf_get SPARKLE_SHA256 "$DEPS_CONF")"
WINSPARKLE_VERSION="$(conf_get WINSPARKLE_VERSION "$DEPS_CONF")"
WINSPARKLE_SHA256="$(conf_get WINSPARKLE_SHA256 "$DEPS_CONF")"
SPARKLE_DIR="$REPO_ROOT/build-deps/Sparkle-$SPARKLE_VERSION"
SPARKLE_BIN="$SPARKLE_DIR/bin"

# Release settings (environment variables override the file, for tests and for a real Developer ID).
RELEASES_REPO="${AMPSIM_RELEASES_REPO:-$(conf_get RELEASES_REPO "$RELEASE_CONF")}"
ED_PUBLIC_KEY="${AMPSIM_ED_PUBLIC_KEY:-$(conf_get ED_PUBLIC_KEY "$RELEASE_CONF")}"
SIGN_IDENTITY_NAME="$(conf_get SIGN_IDENTITY_NAME "$RELEASE_CONF")"

# Where the self-signed identity lives (tools/release/make_signing_identity.sh).
SIGNING_KEYCHAIN="${AMPSIM_SIGNING_KEYCHAIN:-$HOME/Library/Keychains/ampsim-signing.keychain-db}"
SIGNING_KEYCHAIN_SERVICE="ampsim-signing-keychain"   # login keychain item holding its password
GITHUB_TOKEN_SERVICE="ampsim-github-token"           # login keychain item holding the GitHub token

# The password of the signing keychain: $AMPSIM_KEYCHAIN_PASSWORD, or the login keychain item that
# make_signing_identity.sh stored (reading an item the `security` tool created itself doesn't prompt).
signing_keychain_password() {
    if [ -n "${AMPSIM_KEYCHAIN_PASSWORD:-}" ]; then
        printf '%s' "$AMPSIM_KEYCHAIN_PASSWORD"
    else
        security find-generic-password -a "$USER" -s "$SIGNING_KEYCHAIN_SERVICE" -w 2>/dev/null
    fi
}

# codesign only finds identities in keychains on the user's search list (its --keychain option narrows
# the search but doesn't widen it), so the signing keychain has to be on it. Adds it, keeping the others.
ensure_keychain_in_search_list() {
    local kc="$1" current=()
    while IFS= read -r line; do
        line="${line#"${line%%[![:space:]]*}"}"; line="${line%\"}"; line="${line#\"}"
        [ -n "$line" ] && current+=("$line")
    done < <(security list-keychains -d user)
    for k in ${current[@]+"${current[@]}"}; do
        [ "$k" = "$kc" ] && return 0
        # The same file through a symlinked path (/tmp is /private/tmp).
        [ "$(cd "$(dirname "$k")" 2>/dev/null && pwd -P)/$(basename "$k")" = "$(cd "$(dirname "$kc")" && pwd -P)/$(basename "$kc")" ] && return 0
    done
    security list-keychains -d user -s ${current[@]+"${current[@]}"} "$kc"
}

# The SHA-1 of the signing certificate called NAME in KEYCHAIN (codesign accepts it as the identity,
# which can't be ambiguous the way a name can).
identity_hash() {
    security find-identity "$2" 2>/dev/null | awk -v n="\"$1\"" 'index($0, n) { print $2; exit }'
}

# sha256 FILE: the hex SHA-256 of a file, on macOS (shasum) or Linux/Git Bash (sha256sum).
sha256() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi
}

# Python for the small helper scripts (stdlib only). CLAUDE.md: Python runs through uv.
python_run() {
    if command -v uv >/dev/null 2>&1; then
        uv run --no-project --quiet python "$@"
    elif [ -x "$HOME/.local/bin/uv" ]; then
        "$HOME/.local/bin/uv" run --no-project --quiet python "$@"
    else
        python3 "$@"
    fi
}

# windows_update_file SETUP: copies BellyDSP-<v>-windows-setup.exe to BellyDSP-<v>-windows-update.exe next
# to it and prints the copy's path. The copy is the file appcast-windows.xml points at, so WinSparkle
# downloads it and first installs download the setup: GitHub counts each file's downloads, and the two
# counts tell new installs from updates (docs/WEBSITE.md). Same bytes, so the same Ed25519 signature;
# the caller still signs the copy itself, so the appcast's signature is always made from the exact file
# it names. (The Windows workflow does the same in its own signing step.)
windows_update_file() {
    local setup="$1" update
    case "$setup" in
        *-windows-setup.exe) ;;
        *) die "windows_update_file: expected a ...-windows-setup.exe, got $setup" ;;
    esac
    update="${setup%-windows-setup.exe}-windows-update.exe"
    cp -p "$setup" "$update"
    cmp -s "$setup" "$update" || die "the copy $update differs from $setup"
    printf '%s\n' "$update"
}

# version_gt A B: true if version A is newer than B (numeric, dot-separated).
version_gt() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1)" = "$1" ]
}

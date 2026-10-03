#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# Creates (or updates) a GitHub release on the public repo (RELEASES_REPO) and uploads files to it, with the
# GitHub REST API and curl. Called by release.sh and by the Windows CI workflow:
#
#   tools/release/github_release.sh <tag> <notes file or -> <file>...
#
# The token: $GITHUB_TOKEN, or the login keychain item "ampsim-github-token" (docs/RELEASING.md, step 2:
# a fine-grained token with Contents: read and write on the public repo only). Never put it in the repo.
#
# Idempotent: if the release exists it's reused, and an asset with the same name is replaced, so a failed
# upload can simply be run again.
#
# API used (docs.github.com/rest/releases):
#   GET    /repos/{repo}/releases/tags/{tag}            does the release exist?
#   POST   /repos/{repo}/releases                       create it (marked latest)
#   DELETE /repos/{repo}/releases/assets/{id}           remove an old copy of an asset
#   POST   https://uploads.github.com/repos/{repo}/releases/{id}/assets?name=...   upload one file
set -euo pipefail
source "$(dirname "$0")/lib.sh"

TAG="${1:-}"; NOTES="${2:-}"
[ -n "$TAG" ] && [ -n "$NOTES" ] && [ $# -ge 3 ] || die "usage: $0 <tag> <notes.md|-> <file>..."
shift 2
REPO="$RELEASES_REPO"
[ -n "$REPO" ] || die "RELEASES_REPO is empty in tools/release/release.conf"
# (The two base URLs can be overridden to test this script against a stand-in server.)
API_BASE="${AMPSIM_GITHUB_API:-https://api.github.com}"
UPLOADS_BASE="${AMPSIM_GITHUB_UPLOADS:-https://uploads.github.com}"
API="$API_BASE/repos/$REPO"

TOKEN="${GITHUB_TOKEN:-}"
if [ -z "$TOKEN" ] && command -v security >/dev/null 2>&1; then
    TOKEN="$(security find-generic-password -a "$USER" -s "$GITHUB_TOKEN_SERVICE" -w 2>/dev/null || true)"
fi
[ -n "$TOKEN" ] || die "No GitHub token: set GITHUB_TOKEN or store one in the login keychain:
       security add-generic-password -a \"\$USER\" -s $GITHUB_TOKEN_SERVICE -w
       (it asks for the token; see docs/RELEASING.md)"

# The token goes to curl through a private header file, not the command line (where `ps` would show it).
HEADERS="$(mktemp)"
chmod 600 "$HEADERS"
trap 'rm -f "$HEADERS"' EXIT
printf 'Authorization: Bearer %s\nAccept: application/vnd.github+json\nX-GitHub-Api-Version: 2022-11-28\n' "$TOKEN" > "$HEADERS"

# api METHOD URL [curl args...]: prints the response body, fails on HTTP errors with GitHub's message.
api() {
    local method="$1" url="$2" body status
    shift 2
    body="$(mktemp)"
    status="$(curl --silent --show-error --location --output "$body" --write-out '%{http_code}' -X "$method" \
        -H "@$HEADERS" "$@" "$url")"
    if [ "$status" -ge 400 ]; then
        printf '%s\n' "$(cat "$body")" >&2
        rm -f "$body"
        return 22
    fi
    cat "$body"
    rm -f "$body"
}

json_field() { python_run -c "import json,sys; d=json.load(sys.stdin); print(d.get('$1', '') if isinstance(d, dict) else '')"; }

step "GitHub release $TAG in $REPO" "The public repo: people download from its Releases page, and the apps read its latest appcast."
if existing="$(api GET "$API/releases/tags/$TAG" 2>/dev/null)"; then
    release_id="$(printf '%s' "$existing" | json_field id)"
    ok "release $TAG exists (id $release_id); adding or replacing files"
else
    if [ "$NOTES" = "-" ] || [ ! -f "$NOTES" ]; then notes_text="BellyDSP ${TAG#v}"; else notes_text="$(cat "$NOTES")"; fi
    payload="$(TAG="$TAG" NOTES_TEXT="$notes_text" python_run -c '
import json, os
tag = os.environ["TAG"]
print(json.dumps({"tag_name": tag, "name": "BellyDSP " + tag.lstrip("v"), "body": os.environ["NOTES_TEXT"],
                  "draft": False, "prerelease": False, "make_latest": "true"}))')"
    created="$(api POST "$API/releases" -H "Content-Type: application/json" --data "$payload")" \
        || die "Couldn't create the release. Check the token's access (Contents: read and write on $REPO), and that
       the tag is on the repo (release.sh pushes it to the public remote first)."
    release_id="$(printf '%s' "$created" | json_field id)"
    ok "created release $TAG (id $release_id), marked as the latest"
fi
[ -n "$release_id" ] || die "no release id from GitHub"

assets="$(api GET "$API/releases/$release_id/assets?per_page=100")"
for file in "$@"; do
    [ -f "$file" ] || die "not a file: $file"
    name="$(basename "$file")"
    old_id="$(printf '%s' "$assets" | NAME="$name" python_run -c '
import json, os, sys
print(next((str(a["id"]) for a in json.load(sys.stdin) if a["name"] == os.environ["NAME"]), ""))')"
    if [ -n "$old_id" ]; then
        api DELETE "$API/releases/assets/$old_id" >/dev/null
        info "replaced the old $name"
    fi
    case "$name" in
        *.xml) type="application/xml" ;;
        *.dmg) type="application/x-apple-diskimage" ;;
        *.zip) type="application/zip" ;;
        *.exe) type="application/vnd.microsoft.portable-executable" ;;
        *) type="application/octet-stream" ;;
    esac
    encoded="$(NAME="$name" python_run -c 'import os, urllib.parse; print(urllib.parse.quote(os.environ["NAME"]))')"
    api POST "$UPLOADS_BASE/repos/$REPO/releases/$release_id/assets?name=$encoded" \
        -H "Content-Type: $type" --data-binary "@$file" >/dev/null
    ok "uploaded $name ($(du -h "$file" | cut -f1))"
done
info "https://github.com/$REPO/releases/tag/$TAG"

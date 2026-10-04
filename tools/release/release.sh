#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# THE RELEASE COMMAND. Builds, tests, signs, packages, tags, and publishes one version of BellyDSP.
#
#   tools/release/release.sh <version> [options]
#
#   <version>              major.minor.patch, higher than every earlier release (Sparkle and WinSparkle
#                          only install a higher version). VERSION must already say it, committed.
#   --notes FILE           release notes (Markdown). Default: release-notes/<version>.md
#   --dry-run              do everything except tag, push, and publish: the files land in dist/<version>/.
#                          The public-repo checks below only warn.
#   --preflight-only       run every check (the public-repo ones for real) and stop before building (for
#                          testing the checks; nothing is built, tagged, pushed, or published)
#   --windows-dir DIR      include Windows files built by CI (BellyDSP-<version>-windows-setup.exe in DIR):
#                          the installer is copied to BellyDSP-<version>-windows-update.exe (what updates
#                          download, so installs and updates are counted apart; docs/WEBSITE.md), which
#                          is signed here and gets its own appcast-windows.xml. Without it, the Windows CI
#                          job adds them to the release itself after you push the tag.
#   --allow-branch         don't insist on being on main (for testing)
#   --allow-dirty          don't insist on a clean working tree (for testing)
#   --allow-undecided-licence   release even though JUCE_LICENCE in release.conf is still "undecided"
#
# Environment (all optional; docs/RELEASING.md):
#   AMPSIM_ED_KEY_FILE       sign updates with this private key file instead of the login keychain
#   AMPSIM_ED_KEY_ACCOUNT    the keychain account Sparkle's generate_keys used (default ed25519)
#   AMPSIM_ED_PUBLIC_KEY     override release.conf's public key (tests)
#   AMPSIM_SIGN_IDENTITY     a real "Developer ID Application: ..." identity (then notarize.sh runs too)
#   AMPSIM_DOWNLOAD_BASE     where the appcast says the files are (default: the GitHub release's URL; tests)
#   GITHUB_TOKEN             the token for the upload (default: login keychain item ampsim-github-token)
#
# The public repo rule (the AGPL, docs/RELEASING.md): every binary anyone gets is built from a commit
# that's on the public repo and tagged there, so its exact source is public. A real release therefore
# refuses unless a git remote named `public` exists and the commit being released is already on
# public/main (git fetch, then merge-base --is-ancestor). It then creates the tag, pushes the tag to
# `public`, and only after that uploads the binaries. Nothing but --dry-run or --preflight-only (which
# publish nothing) gets past these checks.
#
# Every step prints what it does and why. If something fails, fix it and run the same command again:
# the build starts clean, a tag already made at this commit is reused, and the upload replaces files
# that are already there.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

VERSION=""; NOTES=""; DRY_RUN=0; PREFLIGHT_ONLY=0; WINDOWS_DIR=""; ALLOW_BRANCH=0; ALLOW_DIRTY=0; ALLOW_UNDECIDED=0
PUBLIC_REMOTE=public
while [ $# -gt 0 ]; do
    case "$1" in
        --notes) NOTES="$2"; shift ;;
        --dry-run) DRY_RUN=1 ;;
        --preflight-only) PREFLIGHT_ONLY=1 ;;
        --windows-dir) WINDOWS_DIR="$2"; shift ;;
        --allow-branch) ALLOW_BRANCH=1 ;;
        --allow-dirty) ALLOW_DIRTY=1 ;;
        --allow-undecided-licence) ALLOW_UNDECIDED=1 ;;
        -h|--help) awk 'NR < 5 { next } /^#/ { print; next } { exit }' "$0"; exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) [ -z "$VERSION" ] || die "one version at a time"; VERSION="$1" ;;
    esac
    shift
done
[ -n "$VERSION" ] || die "usage: $0 <version> [--notes FILE] [--dry-run] (see --help)"

cd "$REPO_ROOT"
TAG="v$VERSION"
DIST="$REPO_ROOT/dist/$VERSION"
BUILD="$REPO_ROOT/build-release"
JUCE_LICENCE="$(conf_get JUCE_LICENCE "$RELEASE_CONF")"; JUCE_LICENCE="${JUCE_LICENCE:-undecided}"
SOURCE_URL="$(conf_get SOURCE_URL "$RELEASE_CONF")"
DOWNLOAD_BASE="${AMPSIM_DOWNLOAD_BASE:-https://github.com/$RELEASES_REPO/releases/download/$TAG}"
started=$(date +%s)

mode=""
[ $DRY_RUN = 1 ] && mode=" (dry run: nothing is tagged, pushed, or published)"
[ $PREFLIGHT_ONLY = 1 ] && mode="$mode (preflight only: checks, then stop)"
printf '%sBellyDSP release %s%s%s\n' "$BOLD" "$VERSION" "$mode" "$RESET"

# ---------------------------------------------------------------------------------------------------
step "1. Preflight checks" "Refuse early, before a 10-minute build, if anything would stop the release later."
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the version must be major.minor.patch, like 0.1.1 (got $VERSION)"

branch="$(git rev-parse --abbrev-ref HEAD)"
# The commit this release is: checked against public/main here, built, and tagged in step 7. If HEAD
# moves while the build runs (a commit made meanwhile), step 7 refuses rather than tag something else.
RELEASE_COMMIT="$(git rev-parse HEAD)"
if [ "$branch" != main ]; then
    [ $ALLOW_BRANCH = 1 ] || die "releases come from main (you're on $branch). Merge first, or --allow-branch for a test."
    warn "on branch $branch, not main (--allow-branch)"
fi
ok "branch $branch"

# A clean tree: a release must be exactly a commit (the one that gets tagged and is public).
dirty="$(git status --porcelain --untracked-files=normal || true)"
if [ -n "$dirty" ]; then
    [ $ALLOW_DIRTY = 1 ] || die "the working tree has uncommitted changes; a release must be exactly a commit:
$dirty"
    [ $DRY_RUN = 1 ] || [ $PREFLIGHT_ONLY = 1 ] || die "--allow-dirty is for dry runs: a published build must match its tagged commit"
    warn "uncommitted changes (--allow-dirty): this build won't match any commit"
fi
ok "working tree"

# The committed VERSION file says this version, so the tagged source builds as exactly this version.
committed_version="$(git show HEAD:VERSION 2>/dev/null | tr -d '[:space:]' || true)"
if [ "$committed_version" != "$VERSION" ]; then
    if [ $DRY_RUN = 1 ] || [ $PREFLIGHT_ONLY = 1 ]; then
        warn "VERSION at HEAD says ${committed_version:-nothing}, not $VERSION (a real release refuses this)"
    else
        die "VERSION at HEAD says ${committed_version:-nothing}, not $VERSION. Set it, commit it (with the release
       notes), push main to $PUBLIC_REMOTE, and run this again: the tagged commit must build as $VERSION."
    fi
else
    ok "VERSION at HEAD is $VERSION"
fi

last_tag="$(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | sed 's/^v//' | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1)"
REUSE_TAG=0
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    if [ "$(git rev-parse "$TAG^{commit}")" = "$(git rev-parse HEAD)" ]; then
        # An earlier run tagged this commit and stopped (during the upload, say): carry on with the same tag.
        REUSE_TAG=1
        warn "tag $TAG already exists at this commit (an earlier run stopped after tagging?): reusing it"
    else
        [ $DRY_RUN = 1 ] || die "tag $TAG already exists at another commit: this version was released. Pick a higher one."
        warn "tag $TAG exists (dry run, so carrying on)"
    fi
elif [ -n "$last_tag" ] && ! version_gt "$VERSION" "$last_tag"; then
    die "$VERSION isn't higher than the last release ($last_tag). Installed copies only update to a higher version."
fi
ok "version $VERSION (last release: ${last_tag:-none})"

[ -n "$ED_PUBLIC_KEY" ] || die "no Ed25519 public key: set ED_PUBLIC_KEY in tools/release/release.conf (docs/RELEASING.md, one-time setup)"
ok "update public key ${ED_PUBLIC_KEY:0:12}..."

if [ "$JUCE_LICENCE" = undecided ]; then
    if [ $DRY_RUN = 0 ] && [ $ALLOW_UNDECIDED = 0 ]; then
        die "JUCE_LICENCE in tools/release/release.conf is \"undecided\". Distributing a JUCE app means either
       complying with the AGPLv3 (offering the source to everyone who gets the app) or accepting the JUCE
       licence. Decide (docs/RELEASING.md, \"Licences\"), set it, and run again."
    fi
    warn "JUCE_LICENCE is undecided: the notices will say so"
fi
if [ "$JUCE_LICENCE" = AGPLv3 ] && [ -z "$SOURCE_URL" ]; then
    die "JUCE_LICENCE is AGPLv3, which means offering the source: set SOURCE_URL in release.conf"
fi
ok "licence: BellyDSP under AGPL-3.0-or-later, JUCE under $JUCE_LICENCE, source at ${SOURCE_URL:-?}"

# The public repo rule (the top of this file). In a dry run each failure is a warning; otherwise fatal.
public_problem() {
    if [ $DRY_RUN = 1 ]; then warn "$1 (a real release refuses this)"; else die "$1"; fi
}
info "the public repo rule: the release commit must already be on $PUBLIC_REMOTE/main, so its source is public"
if ! public_url="$(git remote get-url "$PUBLIC_REMOTE" 2>/dev/null)"; then
    public_problem "no git remote named '$PUBLIC_REMOTE'. Add the public repo once:
       git remote add $PUBLIC_REMOTE https://github.com/$RELEASES_REPO.git"
else
    ok "remote $PUBLIC_REMOTE is $public_url"
    if ! run git fetch --quiet --no-tags "$PUBLIC_REMOTE"; then
        public_problem "couldn't fetch from $PUBLIC_REMOTE"
    elif ! git rev-parse -q --verify "refs/remotes/$PUBLIC_REMOTE/main" >/dev/null; then
        public_problem "$PUBLIC_REMOTE has no main branch yet. Push it first: git push $PUBLIC_REMOTE main"
    elif ! git merge-base --is-ancestor HEAD "refs/remotes/$PUBLIC_REMOTE/main"; then
        public_problem "this commit ($(git rev-parse --short HEAD)) isn't on $PUBLIC_REMOTE/main ($(git rev-parse --short "refs/remotes/$PUBLIC_REMOTE/main")).
       Push it first, so its source is public: git push $PUBLIC_REMOTE main"
    else
        ok "commit $(git rev-parse --short HEAD) is on $PUBLIC_REMOTE/main"
    fi
    # (An annotated tag's commit is listed as <tag>^{}; a lightweight tag's as the tag itself.)
    remote_tag="$(git ls-remote --tags "$PUBLIC_REMOTE" "refs/tags/$TAG^{}" | cut -f1)"
    [ -n "$remote_tag" ] || remote_tag="$(git ls-remote --tags "$PUBLIC_REMOTE" "refs/tags/$TAG" | cut -f1)"
    if [ -n "$remote_tag" ] && [ "$remote_tag" != "$(git rev-parse HEAD)" ]; then
        public_problem "$PUBLIC_REMOTE already has a tag $TAG, at another commit ($remote_tag)"
    fi
fi

[ -n "$NOTES" ] || NOTES="$REPO_ROOT/release-notes/$VERSION.md"
if [ -f "$NOTES" ]; then ok "release notes ${NOTES#$REPO_ROOT/}"; else warn "no release notes at ${NOTES#$REPO_ROOT/}; the update says just \"BellyDSP $VERSION\""; NOTES=""; fi

[ -x "$SPARKLE_BIN/sign_update" ] || run "$REPO_ROOT/tools/fetch_deps.sh"
[ -x "$SPARKLE_BIN/sign_update" ] || die "Sparkle's tools are missing from $SPARKLE_BIN"

# The update signing key: a file, or Sparkle's login keychain item.
if [ -n "${AMPSIM_ED_KEY_FILE:-}" ]; then
    [ -f "$AMPSIM_ED_KEY_FILE" ] || die "AMPSIM_ED_KEY_FILE=$AMPSIM_ED_KEY_FILE doesn't exist"
    sign_args=(--ed-key-file "$AMPSIM_ED_KEY_FILE")
    info "update signing key: the file $AMPSIM_ED_KEY_FILE"
else
    sign_args=(--account "${AMPSIM_ED_KEY_ACCOUNT:-ed25519}")
    info "update signing key: the login keychain (Sparkle's generate_keys, account ${AMPSIM_ED_KEY_ACCOUNT:-ed25519})"
fi
# Prove the private key matches the public key the apps will have, before building anything: sign a
# scratch file and verify it against ED_PUBLIC_KEY. A mismatch would make every installed copy refuse
# this update.
probe="$(mktemp)"; printf 'BellyDSP key check %s\n' "$VERSION" > "$probe"
probe_sig="$("$SPARKLE_BIN/sign_update" "${sign_args[@]}" -p "$probe")" || die "sign_update couldn't sign with that key"
python_run "$REPO_ROOT/tools/release/ed25519.py" verify "$ED_PUBLIC_KEY" "$probe" "$probe_sig" >/dev/null \
    || die "the update signing key does NOT match ED_PUBLIC_KEY. Installed copies would reject this update."
rm -f "$probe"
ok "the signing key matches the public key"

if [ -z "${AMPSIM_SIGN_IDENTITY:-}" ]; then
    [ -f "$SIGNING_KEYCHAIN" ] || die "no code-signing identity: run tools/release/make_signing_identity.sh once"
fi
if [ $DRY_RUN = 0 ]; then
    if [ -z "${GITHUB_TOKEN:-}" ] && ! security find-generic-password -a "$USER" -s "$GITHUB_TOKEN_SERVICE" >/dev/null 2>&1; then
        die "no GitHub token (GITHUB_TOKEN or the login keychain item $GITHUB_TOKEN_SERVICE): docs/RELEASING.md, step 2"
    fi
    ok "GitHub token found"
fi

if [ $PREFLIGHT_ONLY = 1 ]; then
    step "Preflight finished: every check passed" "--preflight-only: nothing built, tagged, pushed, or published."
    exit 0
fi

# ---------------------------------------------------------------------------------------------------
step "2. Clean universal build" "Apple silicon (arm64) and Intel (x86_64) in one app, with the updater, in build-release/
    (your dev build in build/ is untouched). From scratch, so nothing stale gets shipped."
rm -rf "$BUILD" "$DIST"
mkdir -p "$DIST"
run cmake -S "$REPO_ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
    -DAMPSIM_VERSION="$VERSION" \
    -DAMPSIM_UPDATER=ON \
    -DAMPSIM_ED_PUBLIC_KEY="$ED_PUBLIC_KEY" \
    -DAMPSIM_JUCE_LICENCE="$JUCE_LICENCE" \
    -DAMPSIM_SOURCE_URL="$SOURCE_URL" > "$DIST/configure.log" 2>&1 || { tail -30 "$DIST/configure.log"; die "configure failed"; }
info "compiling (both architectures, so about twice a dev build; the log is dist/$VERSION/build.log)"
if ! cmake --build "$BUILD" -j > "$DIST/build.log" 2>&1; then
    grep -E "error" "$DIST/build.log" | head -20
    die "the build failed (dist/$VERSION/build.log)"
fi
BUILT_APP="$BUILD/BellyDSP_artefacts/Release/Standalone/BellyDSP.app"
[ -d "$BUILT_APP" ] || die "no app at $BUILT_APP"
archs="$(lipo -archs "$BUILT_APP/Contents/MacOS/BellyDSP")"
info "lipo: $(lipo -info "$BUILT_APP/Contents/MacOS/BellyDSP" | sed 's/.*are: //')"
[[ "$archs" == *arm64* && "$archs" == *x86_64* ]] || die "the app isn't universal (architectures: $archs)"
plist_version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$BUILT_APP/Contents/Info.plist")"
[ "$plist_version" = "$VERSION" ] || die "the app's CFBundleVersion is $plist_version, not $VERSION"
ok "universal, CFBundleVersion $plist_version"

# ---------------------------------------------------------------------------------------------------
step "3. The full test suite" "On this exact build (its arm64 half runs here). Any failure stops the release."
# (-V keeps the suite's own output in the log: every measurement, and the final count.)
if ! ctest --test-dir "$BUILD" --output-on-failure -V > "$DIST/tests.log" 2>&1; then
    tail -40 "$DIST/tests.log"
    die "tests failed (dist/$VERSION/tests.log): not releasing"
fi
summary="$(grep -E "checks passed" "$DIST/tests.log" | tail -n 1 || true)"
ok "${summary:-ctest passed}"

# ---------------------------------------------------------------------------------------------------
step "4. Sign the app" "A copy in dist/$VERSION/, so the build folder stays as built."
APP="$DIST/BellyDSP.app"
rm -rf "$APP"
run ditto "$BUILT_APP" "$APP"
"$REPO_ROOT/tools/release/sign_app.sh" "$APP"

# ---------------------------------------------------------------------------------------------------
step "5. Package" "A DMG for first installs and a zip for Sparkle updates."
"$REPO_ROOT/tools/release/package_mac.sh" "$APP" "$VERSION" "$DIST"
ZIP="$DIST/BellyDSP-$VERSION-mac.zip"
DMG="$DIST/BellyDSP-$VERSION.dmg"

if [ -n "${AMPSIM_SIGN_IDENTITY:-}" ]; then
    step "5b. Notarize" "Developer ID builds only: Apple scans the DMG, and the ticket is stapled to it."
    "$REPO_ROOT/tools/release/notarize.sh" "$DMG"
fi

# ---------------------------------------------------------------------------------------------------
step "6. Sign the update and write the appcast" "The Ed25519 signature is what installed copies check before installing anything."
zip_sig="$("$SPARKLE_BIN/sign_update" "${sign_args[@]}" -p "$ZIP")"
python_run "$REPO_ROOT/tools/release/ed25519.py" verify "$ED_PUBLIC_KEY" "$ZIP" "$zip_sig" >/dev/null || die "the zip's signature doesn't verify"
ok "zip signed and verified against the public key"
appcast_notes=(); [ -n "$NOTES" ] && appcast_notes=(--notes "$NOTES")
python_run "$REPO_ROOT/tools/release/make_appcast.py" --platform mac --version "$VERSION" --file "$ZIP" \
    --url "$DOWNLOAD_BASE/$(basename "$ZIP")" --signature "$zip_sig" --min-os 12.0 \
    --repo "$RELEASES_REPO" --out "$DIST/appcast.xml" ${appcast_notes[@]+"${appcast_notes[@]}"}
assets=("$DMG" "$ZIP" "$DIST/appcast.xml")

if [ -n "$WINDOWS_DIR" ]; then
    step "6b. Windows files" "Built by CI; signed here with the same key, with their own appcast."
    SETUP="$WINDOWS_DIR/BellyDSP-$VERSION-windows-setup.exe"
    [ -f "$SETUP" ] || die "no $SETUP (download the CI run's artifact and unzip it there)"
    cp "$SETUP" "$DIST/"
    SETUP="$DIST/$(basename "$SETUP")"
    # Updates download their own copy of the installer (lib.sh windows_update_file; docs/WEBSITE.md).
    WIN_UPDATE="$(windows_update_file "$SETUP")"
    win_sig="$("$SPARKLE_BIN/sign_update" "${sign_args[@]}" -p "$WIN_UPDATE")"
    python_run "$REPO_ROOT/tools/release/ed25519.py" verify "$ED_PUBLIC_KEY" "$WIN_UPDATE" "$win_sig" >/dev/null || die "the Windows update's signature doesn't verify"
    python_run "$REPO_ROOT/tools/release/make_appcast.py" --platform windows --version "$VERSION" --file "$WIN_UPDATE" \
        --url "$DOWNLOAD_BASE/$(basename "$WIN_UPDATE")" --signature "$win_sig" \
        --repo "$RELEASES_REPO" --out "$DIST/appcast-windows.xml" ${appcast_notes[@]+"${appcast_notes[@]}"}
    assets+=("$SETUP" "$WIN_UPDATE" "$DIST/appcast-windows.xml")
fi

# ---------------------------------------------------------------------------------------------------
if [ $DRY_RUN = 1 ]; then
    step "Dry run finished: nothing tagged, pushed, or published"
    ls -la "$DIST" | sed 's/^/    /'
    info "a real release would now tag $TAG at $(git rev-parse --short HEAD), push the tag to $PUBLIC_REMOTE, then upload to $RELEASES_REPO"
    info "$(( $(date +%s) - started )) s"
    exit 0
fi

step "7. Tag $TAG and push the tag to $PUBLIC_REMOTE" "Before any binary goes out, so the exact source of every download is public at
    $SOURCE_URL/tree/$TAG (the AGPL's Corresponding Source). The tag also starts the Windows build there."
[ "$(git rev-parse HEAD)" = "$RELEASE_COMMIT" ] || die "HEAD moved during the build (was ${RELEASE_COMMIT:0:7}, now $(git rev-parse --short HEAD)): not tagging. Check out ${RELEASE_COMMIT:0:7} and run again."
if [ $REUSE_TAG = 1 ]; then
    ok "reusing tag $TAG at ${RELEASE_COMMIT:0:7}"
else
    run git tag -a "$TAG" -m "BellyDSP $VERSION" "$RELEASE_COMMIT"
    ok "tagged $TAG at ${RELEASE_COMMIT:0:7}"
fi
run git push "$PUBLIC_REMOTE" "refs/tags/$TAG"
pushed="$(git ls-remote --tags "$PUBLIC_REMOTE" "refs/tags/$TAG^{}" | cut -f1)"
[ "$pushed" = "$RELEASE_COMMIT" ] || die "$PUBLIC_REMOTE doesn't show $TAG at ${RELEASE_COMMIT:0:7} after the push (got '${pushed:-nothing}'): not uploading"
ok "$PUBLIC_REMOTE has $TAG at ${RELEASE_COMMIT:0:7}"

step "8. Publish" "A GitHub release $TAG in $RELEASES_REPO, marked latest, with the files. From now on every installed copy finds it."
"$REPO_ROOT/tools/release/github_release.sh" "$TAG" "${NOTES:--}" "${assets[@]}"

cat <<DONE

${GREEN}Released BellyDSP $VERSION.${RESET} ($(( $(date +%s) - started )) s)
  https://github.com/$RELEASES_REPO/releases/tag/$TAG
  Source of this exact build: $SOURCE_URL/tree/$TAG
  Macs pick it up within a day and install it when they quit BellyDSP. The tag started the Windows build
  on $RELEASES_REPO's Actions, which adds its installer to this release (or: tools/release/add_windows.sh).

Keep the private dev repo in step too:
  git push origin main $TAG
DONE

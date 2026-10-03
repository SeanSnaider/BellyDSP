#!/usr/bin/env bash
# End-to-end update test, on this Mac, with no GitHub involved: proves an installed old version really
# updates itself to a new one through Sparkle, and that a wrongly signed update is refused.
#
#   tools/release/update_e2e.sh <old version> <new version> [--wrong-key KEYFILE] [--port 8765]
#
# Needs dist/<old>/ and dist/<new>/ from release.sh --dry-run, with the new one built with
# AMPSIM_DOWNLOAD_BASE=http://127.0.0.1:<port>/releases/download/v<new> so its appcast points here.
#
# What it does:
#   1. installs the old app from its update zip into a scratch folder (not /Applications);
#   2. serves the new appcast and zip through serve_updates.py, which imitates GitHub's redirects;
#   3. points the app at it with `defaults write com.seansnaider.ampsim SUFeedURL ...` (Sparkle reads the
#      feed URL from the app's user defaults before its Info.plist; no special build needed) and marks
#      the last check as long ago, so Sparkle checks at launch;
#   4. launches the old app, waits for Sparkle to download the update, quits the app (as Cmd-Q would),
#      and waits for Sparkle's installer to replace it;
#   5. compares the installed app with the new one: version, executable hash, signature.
# With --wrong-key, the zip is re-signed with a different key first, and the test passes only if the
# old app is left exactly as it was.
#
# While it runs it changes, and afterwards restores: the app's user defaults (com.seansnaider.ampsim)
# and the standalone app's settings file (~/Library/Application Support/Amp Sim.settings), which gets a
# no-audio-device setup so the app doesn't stop at launch to ask for the microphone. It also clears
# Sparkle's download cache (~/Library/Caches/com.seansnaider.ampsim/org.sparkle-project.Sparkle) before
# and after, so a pending update from one run can't leak into the next. Quit Amp Sim before running it.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

OLD="${1:-}"; NEW="${2:-}"
[ -n "$OLD" ] && [ -n "$NEW" ] || die "usage: $0 <old version> <new version> [--wrong-key KEYFILE] [--port N]"
shift 2
WRONG_KEY=""; PORT=8765
while [ $# -gt 0 ]; do
    case "$1" in
        --wrong-key) WRONG_KEY="$2"; shift ;;
        --port) PORT="$2"; shift ;;
        *) die "unknown option $1" ;;
    esac
    shift
done

BUNDLE_ID=com.seansnaider.ampsim
OLD_ZIP="$REPO_ROOT/dist/$OLD/AmpSim-$OLD-mac.zip"
NEW_ZIP="$REPO_ROOT/dist/$NEW/AmpSim-$NEW-mac.zip"
NEW_APPCAST="$REPO_ROOT/dist/$NEW/appcast.xml"
for f in "$OLD_ZIP" "$NEW_ZIP" "$NEW_APPCAST"; do [ -f "$f" ] || die "missing $f (run release.sh --dry-run for both versions)"; done
grep -q "http://127.0.0.1:$PORT/" "$NEW_APPCAST" || die "dist/$NEW/appcast.xml doesn't point at http://127.0.0.1:$PORT/ (build it with AMPSIM_DOWNLOAD_BASE)"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ampsim-e2e.XXXXXX")"
# Sparkle keeps a downloaded, not yet installed update here between launches; each test starts without one.
SPARKLE_CACHE="$HOME/Library/Caches/$BUNDLE_ID/org.sparkle-project.Sparkle"
INSTALLED="$WORK/install/Amp Sim.app"
SETTINGS="$HOME/Library/Application Support/Amp Sim.settings"
LOG="$WORK/server.log"
server_pid=""

# ---- Restore everything on the way out, whatever happens ----------------------------------------
had_defaults=0; had_settings=0; had_cache=0
[ -d "$HOME/Library/Caches/$BUNDLE_ID" ] && had_cache=1
# (`defaults export` succeeds even for a domain that doesn't exist, so check with `defaults read`.)
if defaults read "$BUNDLE_ID" >/dev/null 2>&1; then defaults export "$BUNDLE_ID" "$WORK/defaults-backup.plist"; had_defaults=1; fi
if [ -f "$SETTINGS" ]; then cp -p "$SETTINGS" "$WORK/settings-backup"; had_settings=1; fi
cleanup() {
    quit_app >/dev/null 2>&1 || true
    # The server runs under uv, so kill it by its command line (killing uv's pid leaves python running).
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
    pkill -f "serve_updates.py $WORK/server" 2>/dev/null || true
    rm -rf "$SPARKLE_CACHE"
    [ $had_cache = 0 ] && rm -rf "$HOME/Library/Caches/$BUNDLE_ID"   # the app's URL cache, made by this test
    if [ $had_defaults = 1 ]; then defaults import "$BUNDLE_ID" "$WORK/defaults-backup.plist"; else
        defaults delete "$BUNDLE_ID" >/dev/null 2>&1 || true
        # `defaults delete` leaves an empty plist behind; it didn't exist before, so it goes too.
        prefs="$HOME/Library/Preferences/$BUNDLE_ID.plist"
        [ -f "$prefs" ] && [ "$(plutil -convert json -o - "$prefs" 2>/dev/null)" = "{}" ] && rm -f "$prefs"
    fi
    if [ $had_settings = 1 ]; then cp -p "$WORK/settings-backup" "$SETTINGS"; else rm -f "$SETTINGS"; fi
    info "restored the app's user defaults and settings file; test files are in $WORK"
}
trap cleanup EXIT

# Quits every running Amp Sim the way the Dock's Quit does (an NSRunningApplication terminate, which
# sends the app a quit request; it needs no Automation permission, unlike `osascript ... to quit`).
quit_app() {
    osascript -l JavaScript -e "ObjC.import('AppKit'); var a = \$.NSRunningApplication.runningApplicationsWithBundleIdentifier('$BUNDLE_ID'); for (var i = 0; i < a.count; i++) a.objectAtIndex(i).terminate; a.count"
}
running() { pgrep -f "$INSTALLED/Contents/MacOS/Amp Sim" >/dev/null; }
exe_hash() { shasum -a 256 "$1/Contents/MacOS/Amp Sim" | cut -c1-16; }
plist_version() { /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$1/Contents/Info.plist"; }

if curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then die "something is already listening on port $PORT (an earlier test's server?)"; fi
running_any="$(osascript -l JavaScript -e "ObjC.import('AppKit'); \$.NSRunningApplication.runningApplicationsWithBundleIdentifier('$BUNDLE_ID').count")"
[ "$running_any" = 0 ] || die "Amp Sim is running; quit it first (the test quits every copy)"
rm -rf "$SPARKLE_CACHE"

step "1. Install $OLD into a scratch folder" "Unzipped from its update zip, the way a friend's copy would look."
mkdir -p "$WORK/install"
run ditto -x -k "$OLD_ZIP" "$WORK/install"
codesign --verify --deep --strict "$INSTALLED" && ok "installed $(plist_version "$INSTALLED") at $INSTALLED"
before_hash="$(exe_hash "$INSTALLED")"
mkdir -p "$WORK/new" && ditto -x -k "$NEW_ZIP" "$WORK/new"
new_hash="$(exe_hash "$WORK/new/Amp Sim.app")"
info "executable SHA-256: installed $before_hash, the $NEW update $new_hash"

step "2. Serve the update like GitHub does" "appcast.xml and the zip, behind the same two redirects as github.com."
root="$WORK/server"
mkdir -p "$root/releases/download/v$NEW"
cp "$NEW_APPCAST" "$NEW_ZIP" "$root/releases/download/v$NEW/"
if [ -n "$WRONG_KEY" ]; then
    wrong_sig="$("$SPARKLE_BIN/sign_update" --ed-key-file "$WRONG_KEY" -p "$NEW_ZIP")"
    sed -i '' "s#sparkle:edSignature=\"[^\"]*\"#sparkle:edSignature=\"$wrong_sig\"#" "$root/releases/download/v$NEW/appcast.xml"
    warn "the appcast now carries a signature made with a DIFFERENT key ($WRONG_KEY): the update must be refused"
fi
python_run "$REPO_ROOT/tools/release/serve_updates.py" "$root" --latest "v$NEW" --port "$PORT" > "$LOG" 2>&1 &
server_pid=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.2; done
FEED="http://127.0.0.1:$PORT/releases/latest/download/appcast.xml"
run curl -sS -L -o /dev/null -w '    feed reachable: HTTP %{http_code} after %{num_redirects} redirect(s)\n' "$FEED"

step "3. Point the installed app at the local feed" "User defaults beat Info.plist for Sparkle's feed URL; an old last-check date makes it check at launch."
run defaults write "$BUNDLE_ID" SUFeedURL "$FEED"
run defaults write "$BUNDLE_ID" SULastCheckTime -date "2000-01-01 00:00:00 +0000"
# The standalone app's settings: no audio device at all, so it never asks for the microphone (that
# prompt blocks the launch until someone clicks it).
cat > "$SETTINGS" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<PROPERTIES>
  <VALUE name="audioSetup">
    <DEVICESETUP deviceType="CoreAudio" audioOutputDeviceName="" audioInputDeviceName="" audioDeviceRate="48000.0" audioDeviceBufferSize="512" audioDeviceInChans="0" audioDeviceOutChans="0"/>
  </VALUE>
  <VALUE name="shouldMuteInput" val="1"/>
</PROPERTIES>
EOF

step "4. Launch $OLD and let Sparkle find the update"
log_start="$(date '+%Y-%m-%d %H:%M:%S')"
run open -n "$INSTALLED"
downloaded=0
for _ in $(seq 1 120); do
    if grep -q "AmpSim-$NEW-mac.zip HTTP/1.1\" 200" "$LOG"; then downloaded=1; break; fi
    sleep 1
done
sed 's/^/      /' "$LOG"
if [ $downloaded = 1 ]; then ok "the app fetched the appcast and downloaded the $NEW zip"; else warn "no download within 2 minutes"; fi
# Sparkle verifies and unpacks in the background after the download; give it time to get ready.
sleep 15

step "5. Quit (Sparkle installs on quit)"
quit_app >/dev/null
for _ in $(seq 1 30); do running || break; sleep 1; done
running && die "the app didn't quit"
ok "quit"
result="unchanged"
for _ in $(seq 1 60); do
    if [ "$(plist_version "$INSTALLED" 2>/dev/null || true)" = "$NEW" ]; then result="updated"; break; fi
    sleep 1
done
sleep 2

step "6. Result"
after_version="$(plist_version "$INSTALLED")"
after_hash="$(exe_hash "$INSTALLED")"
info "installed app now: CFBundleVersion $after_version, executable SHA-256 $after_hash"
info "                    (was $OLD / $before_hash; the $NEW update is $new_hash)"
codesign --verify --deep --strict "$INSTALLED" && ok "the installed app's signature verifies"
info "designated requirement: $(codesign -d -r- "$INSTALLED" 2>&1 | sed -n 's/^designated => //p')"
log show --start "$log_start" --style compact --predicate 'subsystem BEGINSWITH "org.sparkle-project"' 2>/dev/null \
    | grep -v "^Timestamp" | cut -c1-220 | tail -n 25 > "$WORK/sparkle.log" || true
if [ -s "$WORK/sparkle.log" ]; then info "Sparkle's own log lines:"; sed 's/^/      /' "$WORK/sparkle.log"; fi

if [ -z "$WRONG_KEY" ]; then
    [ "$after_version" = "$NEW" ] && [ "$after_hash" = "$new_hash" ] || die "NOT UPDATED: still $after_version"
    printf '\n%sPASS%s %s updated itself to %s on quit (version and executable match the update).\n' "$GREEN" "$RESET" "$OLD" "$NEW"
else
    [ "$after_version" = "$OLD" ] && [ "$after_hash" = "$before_hash" ] || die "the wrongly signed update was INSTALLED"
    printf '\n%sPASS%s the update signed with the wrong key was refused; %s is untouched.\n' "$GREEN" "$RESET" "$OLD"
fi

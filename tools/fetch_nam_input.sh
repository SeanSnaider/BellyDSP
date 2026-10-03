#!/usr/bin/env bash
# Downloads NAM's standard training input file (v3.0.0, "input.wav") into build-deps/nam/ (gitignored) and
# checks it against the SHA-256 pinned in tools/deps.conf, plus the MD5 that NAM's trainer itself uses to
# recognise the file. Safe to run again: a verified copy is left alone.
#
#   tools/fetch_nam_input.sh
#
# What the file is: 190 s of test signals and guitar that ampsim_capture plays into your gear. NAM's trainer
# knows its exact layout (where the calibration blips and the validation audio are), which is why every
# capture uses this one file. It isn't committed or shipped: no licence is stated for it, so each machine
# downloads it from the link NAM's own trainer offers.
set -euo pipefail
source "$(dirname "$0")/release/lib.sh"

url="$(conf_get NAM_INPUT_URL "$DEPS_CONF")"
expected="$(conf_get NAM_INPUT_SHA256 "$DEPS_CONF")"
expected_md5="$(conf_get NAM_INPUT_MD5 "$DEPS_CONF")"
dest_dir="$REPO_ROOT/build-deps/nam"
dest="$dest_dir/input.wav"
mkdir -p "$dest_dir"

step "NAM's input file (v3.0.0)" "What ampsim_capture plays into your gear, and what the trainer expects."
if [ -f "$dest" ] && [ "$(sha256 "$dest")" = "$expected" ]; then
    ok "already downloaded and verified: ${dest#$REPO_ROOT/}"
    exit 0
fi
run curl --fail --location --silent --show-error --output "$dest.part" "$url"
actual="$(sha256 "$dest.part")"
if [ "$actual" != "$expected" ]; then
    rm -f "$dest.part"
    die "SHA-256 mismatch: expected $expected (tools/deps.conf), got $actual. Google Drive may have sent a web page instead of the file; don't use it."
fi
md5_actual="$(md5 -q "$dest.part" 2>/dev/null || md5sum "$dest.part" | cut -d' ' -f1)"
[ "$md5_actual" = "$expected_md5" ] || die "MD5 $md5_actual isn't the one NAM's trainer recognises ($expected_md5)."
mv "$dest.part" "$dest"
ok "downloaded ${dest#$REPO_ROOT/}: SHA-256 $actual matches tools/deps.conf, and NAM's trainer will recognise it as v3.0.0"

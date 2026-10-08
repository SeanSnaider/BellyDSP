# Releasing BellyDSP

How a version gets from this repo to everyone's Macs and PCs, and how it keeps updating itself after that. Written for you, Sean: what each piece is, what to do once, what to do per release, and what to do when something breaks.

> **The two keys you must never lose**
>
> 1. **The Ed25519 update key** (made by Sparkle's `generate_keys`, kept in your login keychain). Every installed copy of BellyDSP has the matching public key baked in and refuses any update that isn't signed with this private key. **If you lose it, every copy already out there can never update again.** Everyone would have to download and install a new version by hand, once, and you'd start over with a new key. Back it up the day you make it (step 5 below).
> 2. **The self-signed code-signing certificate** (made by `make_signing_identity.sh`). Losing it is less bad: updates still install (the Ed25519 key vouches for them), but macOS sees a new developer and asks everyone for microphone access again. Back it up too.
>
> Neither goes in the repo. A password manager is the right place for both.

## Two repos, and the rule that ties them together

BellyDSP is free software under the GNU AGPL v3 or later (`LICENSE`), and that's not optional in practice: JUCE is used under its AGPLv3 option. The AGPL says everyone who gets a binary must be able to get its **Corresponding Source**, the exact source it was built from. The setup makes that automatic:

| Repo | Remote | What goes there |
|---|---|---|
| `SeanSnaider/amp_sim_priv` (private) | `origin` | Everything: every branch, experiments, half-done work. The dev and test environment. |
| `SeanSnaider/BellyDSP` (public, "prod") | `public` | Only `main` and version tags (`v0.1.0`, ...). Its Releases page holds the downloads and the appcasts; its Actions build Windows and run the tests on every push. |

**The rule: every binary given to anyone is built from a commit that's on `public/main` and tagged there.** `release.sh` enforces it: a real release refuses unless the remote `public` exists and the commit is already on `public/main`, and it pushes the version tag to `public` before it uploads a single file. So the source of every download is public before the download is, at `https://github.com/SeanSnaider/BellyDSP/tree/v<version>`, and the app's brand menu links straight to it ("Source code for this version"). A build you hand someone outside a release (a test build for a friend) falls under the same rule: build it from a pushed, tagged commit, or don't hand it out.

The flow, every time: work on branches in the private repo; merge to `main`; push `main` to `public` (`git push public main`); then release, which tags and pushes the tag. Only `main` and tags ever go to `public` (never `git push public --all`).

## The moving parts

| Piece | What it is | Where |
|---|---|---|
| Public repo | `SeanSnaider/BellyDSP`: the source (main and tags) and the releases: the downloads and the appcasts. People download from its Releases page. | `RELEASES_REPO` and `SOURCE_URL` in `tools/release/release.conf` (the only place to change them) |
| Appcast | A small RSS file describing the newest version: its number, its download URL, its signature, release notes. `appcast.xml` for the Mac, `appcast-windows.xml` for Windows, attached to every release. | The apps read `https://github.com/SeanSnaider/BellyDSP/releases/latest/download/appcast.xml` (and `appcast-windows.xml`). GitHub redirects "latest" to the newest release's copy, so publishing a release is all it takes; no website. |
| Sparkle 2 | The standard macOS updater (sparkle-project.org). Inside the app as `Contents/Frameworks/Sparkle.framework`. Checks once a day, downloads silently, installs when the app quits. | `tools/deps.conf` pins 2.10.0 by SHA-256; `tools/fetch_deps.sh` downloads it to `build-deps/` |
| WinSparkle | Sparkle's Windows counterpart (winsparkle.org). `WinSparkle.dll` next to `BellyDSP.exe`. Checks once a day and offers updates in a dialog. | Pinned (0.9.4) in the same file; the Windows CI fetches it |
| Code signature | macOS's proof of who made an app and that it hasn't changed. We sign with a free self-signed certificate. It gives every version the same identity, which is what keeps the microphone permission across updates and lets Sparkle accept them. It doesn't make Gatekeeper trust the app (that takes Apple's paid Developer ID and notarization), so people click "Open Anyway" once. | `make_signing_identity.sh`, `sign_app.sh` |
| Ed25519 signature | A signature over each update file. The apps check it before installing anything, so nobody can push a fake update even if they got into the repo. | Private key in your login keychain; public key in `release.conf` (`ED_PUBLIC_KEY`) and from there in every build |

### What a player experiences

**Mac, first install.** They download `BellyDSP-x.y.z.dmg` from the releases page, open it, and drag BellyDSP onto Applications. The first launch says Apple can't check it for malicious software (an unnotarized app); they click Done, go to System Settings > Privacy & Security, click **Open Anyway**, and confirm. From then on it opens normally. (The `xattr` line in the DMG's read-me does the same in one Terminal command.) Then: allow the microphone, set the interface to 48 kHz, pick the input in Options. `docs/INSTALL.md` is their version of this.

**Mac, updates.** Nothing to do. Once a day (and at launch, if a day has passed) BellyDSP reads the appcast. If there's a newer version it downloads the zip in the background, checks its Ed25519 signature and that the new app is signed by the same certificate, and installs it when they quit BellyDSP. Next launch is the new version, with no microphone prompt (same signing identity) and, as far as Sparkle's documented behaviour goes, no new Gatekeeper prompt (Sparkle clears the quarantine flag on what it installs; the local test's updated app carried no quarantine attribute, but a real player's first update hasn't been seen yet). "Check for updates..." under the brand ("BellyDSP", top left) does it on demand, with a window.

**Windows, first install.** They download `BellyDSP-x.y.z-windows-setup.exe`. SmartScreen says "Windows protected your PC" (it isn't signed with a paid certificate): **More info > Run anyway**. The installer needs no admin rights: it installs for the current user into `%LOCALAPPDATA%\Programs\BellyDSP`, with a Start menu entry and an uninstaller (Settings > Apps).

**Windows, updates.** Once a day WinSparkle checks `appcast-windows.xml`. If there's a newer version it shows a dialog ("A new version of BellyDSP is available", with the release notes): **Install update** downloads the installer (its own copy, `BellyDSP-x.y.z-windows-update.exe`, so the download counts tell updates from first installs; docs/WEBSITE.md), checks its Ed25519 signature, closes BellyDSP, runs the installer silently (a small progress window, no UAC prompt because it's per-user), and reopens BellyDSP. They can also pick Remind me later or Skip this version. It's not silent like the Mac, and can't be: WinSparkle has no install-on-quit mode, and its only "no question" mode (`win_sparkle_check_update_with_ui_and_install`) shows a progress window and quits the app immediately, which is worse in the middle of playing. See "Open questions" below.

## One-time setup, in order

Each step says what it does. You need a terminal in the repo folder.

### 1. Create the public repo, and add it as the `public` remote

On github.com: New repository, name **`BellyDSP`**, **Public**, and **don't** add a README, licence, or .gitignore (the repo starts empty, and the first push brings everything, LICENSE included). Then, in this repo:

```
git remote add public https://github.com/SeanSnaider/BellyDSP.git
```

That's a second remote next to `origin` (the private repo). Nothing goes to it until you push: the first push is `main` and any tags, once the UI has been played and merged (`git push public main`, then `git push public --tags` only if the tags are version tags you mean to publish). `release.sh` checks for this remote and refuses without it.

### 2. A token that can publish to it, and only to it

GitHub > Settings > Developer settings > Personal access tokens > **Fine-grained tokens** > Generate new token:

- Repository access: **Only select repositories** > `BellyDSP`
- Permissions > Repository permissions > **Contents: Read and write** (Metadata: read-only gets added automatically)
- Expiration: up to a year; put a reminder in your calendar to make a new one

Copy the token, then store it in your login keychain (it asks for it; nothing lands in your shell history):

```
security add-generic-password -a "$USER" -s ampsim-github-token -w
```

`github_release.sh` reads it from there (or from `$GITHUB_TOKEN` if set). Never commit it. (Pushing the tag uses your normal git credentials for `public`, not this token.)

### 3. Download Sparkle

```
tools/fetch_deps.sh
```

Downloads Sparkle 2.10.0 into `build-deps/` (gitignored) and checks its SHA-256 against `tools/deps.conf`. Release builds embed its framework; its `bin/` tools sign updates.

### 4. Make the code-signing identity

```
tools/release/make_signing_identity.sh
```

Creates a self-signed code-signing certificate (RSA 3072, 20 years) in its own keychain, `~/Library/Keychains/ampsim-signing.keychain-db`, with a random password kept in your login keychain, and sets it up so `codesign` uses it without dialogs. It test-signs a file and prints the certificate's SHA-1, which the app's designated requirement will pin.

**Back up** `~/.ampsim-release/ampsim-signing-identity.p12` and its password (`security find-generic-password -a "$USER" -s ampsim-signing-keychain -w` prints it) into your password manager. `make_signing_identity.sh --check` re-tests it any time.

### 5. Make the Ed25519 update key, and back it up

```
build-deps/Sparkle-2.10.0/bin/generate_keys
```

Sparkle's tool makes the key pair, saves the private key in your login keychain ("Private key for signing Sparkle updates"), and prints the public key with a snippet like `<key>SUPublicEDKey</key><string>...</string>`. macOS may ask whether `generate_keys` may use the keychain; allow it. (It was not run during the build of this pipeline, because that dialog would have hung an unattended run; the tests used a throwaway key.)

1. Paste the public key into `tools/release/release.conf` as `ED_PUBLIC_KEY=...` and commit that. It's public; it's fine in the repo.
2. **Back up the private key now**:
   ```
   build-deps/Sparkle-2.10.0/bin/generate_keys -x ~/Desktop/ampsim-ed25519.key
   ```
   writes it to a file (one line of base64). Put that line in your password manager, then delete the file. To restore it on a new Mac: `generate_keys -f thatfile`.

Again: **without this private key, no installed copy can ever be updated.** There is no recovery.

### 6. Licensing: decided

Decided on 2026-10-03: BellyDSP is AGPL-3.0-or-later and JUCE is used under the AGPLv3, so `release.conf` has `JUCE_LICENCE=AGPLv3` and `SOURCE_URL=https://github.com/SeanSnaider/BellyDSP`. Nothing to do here; see "Licences" below.

### 7. CI on the public repo, and (optionally) its secret

The same two workflows live in both repos. On the public repo they run on every push and pull request (Actions minutes on standard runners are free for public repos): the macOS test suite, and the Windows build, tests, and installer. On the private repo they only run by hand (Actions > the workflow > Run workflow), because there the minutes count against the free allowance (Windows double, macOS ten times). Each job checks `github.event.repository.private` to tell which repo it's in.

For a release tag, the Windows workflow also adds its installer, the installer's update copy (`-windows-update.exe`, what WinSparkle downloads), and `appcast-windows.xml` to the release, using the workflow's own token (the release is on the same repo, so no token secret is needed). It can only do that if the installer is signed, which needs one optional secret on the public repo (Settings > Secrets and variables > Actions):

| Secret | What | Needed for |
|---|---|---|
| `AMPSIM_ED_PRIVATE_KEY` | The one-line private key from step 5's backup | The Windows job signing its installer by itself (tag builds and manual runs only) |

**My recommendation: skip it.** It's the one thing that must never leak, and as a repo secret it's readable by any workflow code that runs in the repo, now a public one. Without it, the Windows job still builds and tests and keeps the installer as an artifact, and `tools/release/add_windows.sh` signs it on your Mac and uploads it (two commands per release instead of zero). If you'd rather have Windows fully automatic, add it; it works either way.

## Every release

0. **Run the full suite on Windows**, on Sean's Windows PC at this commit (`ampsim_tests.exe --proof-dir build\proof` in a Release build). CI on the public repo skips the five slowest groups and the CPU benchmarks (ASSUMPTIONS DS54), so this is where they run on Windows. CPU-budget checks there fail by machine speed alone (the PC needed about 1.75x the Mac's time on 2026-10-05): read their numbers, don't treat them as regressions unless they moved. Also run `ampsim_tests.exe --bench` there and compare its summary with the last release's (BUILD_PLAN "CPU").
1. **Set the version and write the notes**, on `main`: put the new version in `VERSION` and write `release-notes/<version>.md` (see `release-notes/README.md`). Commit both ("Release 0.1.1").
2. **Push `main` to both repos**: `git push origin main` and `git push public main`. The second is what makes the source public; the release refuses until it's done.
3. **Release**:
   ```
   tools/release/release.sh 0.1.1
   ```
   It refuses unless you're on `main` with a clean tree, `VERSION` at that commit says 0.1.1, the version is higher than every earlier `v*` tag, the signing key matches `ED_PUBLIC_KEY`, the token exists, a remote named `public` exists, that commit is on `public/main` (it fetches `public` to check), and `public` has no `v0.1.1` at another commit. Then, saying what each step does: a clean universal build in `build-release/` (arm64 and x86_64; your `build/` is untouched), the full test suite (any failure stops it), signing, the DMG and the update zip, the Ed25519 signature and `appcast.xml`, **the tag `v0.1.1` on that commit, pushed to `public`** (it checks the tag arrived), and only then the GitHub release on `SeanSnaider/BellyDSP` (marked latest) with the files attached. It takes about 15 to 20 minutes, most of it compiling everything twice (once per architecture).
   - `--dry-run` does everything except tag, push, and publish; the files land in `dist/0.1.1/`, and the public-repo checks only warn. Worth doing before every real release.
   - `--preflight-only` runs every check for real and stops before building: a quick way to see whether a release would be refused.
   - If anything fails, fix it and run the same command again: the build starts clean, a tag already made at this commit is reused, and the upload replaces files already there.
4. **Keep the private repo in step**: `git push origin v0.1.1` (the script prints it).
5. **Windows**: the tag push started the Windows build on the public repo. If you added the `AMPSIM_ED_PRIVATE_KEY` secret, it attaches `BellyDSP-0.1.1-windows-setup.exe`, `BellyDSP-0.1.1-windows-update.exe`, and `appcast-windows.xml` to the release by itself. Otherwise, when the run is green: download its artifact (Actions > the run > Artifacts), unzip it, and run `tools/release/add_windows.sh 0.1.1 <that folder>`.

Within a day, every installed Mac has it (on its next quit); on Windows, when they click Install.

### Versions

`VERSION` holds the current version (`major.minor.patch`). It becomes `CFBundleShortVersionString` and `CFBundleVersion` on the Mac, which Sparkle compares, and the version WinSparkle compares on Windows: an update is only offered if it's higher, so versions only ever go up. `release.sh` enforces that against the existing tags, checks that the committed `VERSION` says the version you're releasing (so the public, tagged source builds as exactly that version), and passes it to the build (`-DAMPSIM_VERSION`).

## Signing, in more detail

`sign_app.sh` signs inside out: Sparkle's `Autoupdate` tool and `Updater.app`, then `Sparkle.framework`, then the app (`--deep` signing is discouraged by Apple). It verifies with `codesign --verify --deep --strict` and prints the designated requirement, which looks like:

```
identifier "com.seansnaider.bellydsp" and certificate root = H"<your certificate's SHA-1>"
```

That pins the certificate, not the build, so every version you sign has the same requirement. Sparkle checks that the new app satisfies the old app's requirement, and macOS's privacy database (TCC) keys the microphone permission on it.

**No hardened runtime for the self-signed build.** The hardened runtime turns on library validation, which only lets an app load frameworks signed by Apple or by the same Team ID. A self-signed certificate has no Team ID, so the app can't load Sparkle.framework. Tested: with `--options runtime` the app dies at launch with `Library not loaded: @rpath/Sparkle.framework/... mapping process and mapped file (non-platform) have different Team IDs`. The hardened runtime only matters for notarization, which a self-signed app can't get anyway. (`AMPSIM_HARDENED_RUNTIME=1` turns it on for experiments.)

**Sparkle's XPC services are left out.** `Installer.xpc` and `Downloader.xpc` exist only for sandboxed apps; BellyDSP isn't sandboxed (it needs to read captures and IRs anywhere), so the build removes them.

**The Developer ID path, for later** (written, untested). If you ever pay for the Apple Developer Program ($99/year): put your "Developer ID Application" certificate in the login keychain, store notarization credentials once (`xcrun notarytool store-credentials ampsim-notary --apple-id ... --team-id ...`), and release with `AMPSIM_SIGN_IDENTITY="Developer ID Application: Sean Snaider (TEAMID)" tools/release/release.sh x.y.z`. `sign_app.sh` then uses the hardened runtime, a secure timestamp, and `tools/release/BellyDSP.entitlements` (audio input), and `notarize.sh` notarizes and staples the DMG. People would no longer need "Open Anyway". The switch changes the designated requirement: that one update still installs (same Ed25519 key), but macOS asks for the microphone once more.

## Testing updates locally

`tools/release/update_e2e.sh` proves the whole update path on your own Mac without GitHub: it installs an old build into a scratch folder, serves a newer build's appcast and zip from a local server that imitates GitHub's redirects, points the app at it through user defaults, launches it, quits it, and checks that the new version replaced the old one. With `--wrong-key` it checks that an update signed with a different key is refused. It restores the defaults and settings it touches.

```
# two dry-run builds whose appcasts point at the local server
AMPSIM_DOWNLOAD_BASE=http://127.0.0.1:8765/releases/download/v0.1.0 tools/release/release.sh 0.1.0 --dry-run
AMPSIM_DOWNLOAD_BASE=http://127.0.0.1:8765/releases/download/v0.1.1 tools/release/release.sh 0.1.1 --dry-run
tools/release/update_e2e.sh 0.1.0 0.1.1
tools/release/update_e2e.sh 0.1.0 0.1.1 --wrong-key some-other.key   # make one with: tools/release/ed25519.py keygen some-other.key
```

(While it runs, the test app has no audio device, so it doesn't stop at launch to ask for the microphone.)

## Bundled content

`content/` holds captures and IRs that ship inside the app: so far 21 cab IRs from two CC0 packs (`content/irs`), with Sean's own captures to come (`docs/CAPTURING.md`). The build copies it into `BellyDSP.app/Contents/Resources/content` (Windows: `content\` next to the exe, which the installer installs). Presets refer to those files as `factory:models/...` and `factory:irs/...`, resolved against the installed app wherever it is; the preset format stays version 2. Every file needs an entry in `content/manifest.json` (source, author, licence, licence text), or the build stops; the entries become their section of `THIRD_PARTY_NOTICES.txt`. Details in `content/README.md`.

## Licences

**BellyDSP itself**: Copyright (C) 2026 Sean Snaider, GNU AGPL v3 or later. `LICENSE` is the official text, verbatim from gnu.org; every source file starts with `SPDX-License-Identifier: AGPL-3.0-or-later` and the copyright line (`tools/spdx_headers.py`). The app carries `LICENSE.txt` in `Contents/Resources` (next to the exe on Windows), the DMG and the Windows install carry it too, and the brand menu says "Free software under the GNU AGPL v3 or later" and links to "Source code for this version". **Sean's own captures** (`content/models`, to come) are CC BY 4.0, attribution "Sean Snaider".

`THIRD_PARTY_NOTICES.txt` is generated at build time (`tools/notices/make_notices.cmake`) from the actual licence files: BellyDSP's own notice and `LICENSE` first, then JUCE (under the AGPLv3), then everything else, from the submodules, downloads, and `content/`. It ships in the app (shown under BellyDSP > About / licenses), in the DMG, and next to the Windows exe:

| Component | Licence | What it asks of you |
|---|---|---|
| BellyDSP | AGPL-3.0-or-later | Offer the Corresponding Source to everyone who gets a binary: the public repo at the release's tag (the rule at the top) |
| JUCE 8.0.15 | AGPLv3 (its other option, the JUCE 8 licence, isn't used) | The same: the whole program's source, under the AGPL (done by the above) |
| NeuralAmpModelerCore | MIT | Keep the notice (done) |
| Eigen | MPL 2.0 | Keep the notice; Eigen's source is available upstream, unmodified (done) |
| nlohmann/json | MIT | Keep the notice (done) |
| Sparkle | MIT (plus bsdiff and sais notices) | Keep the notice (done) |
| WinSparkle | MIT (its COPYING also carries an OpenSSL acknowledgement and expat's licence file) | Keep the notice (done) |
| Geist, Fraunces | SIL Open Font License | Keep the licence with the fonts (done) |
| zlib, libpng, IJG JPEG, FLAC, Ogg Vorbis, HarfBuzz, SheenBidi (inside JUCE) | Permissive | Keep the notices (done) |
| The bundled cab IRs | CC0 1.0 | Nothing (credited anyway) |
| ASIO SDK headers (Windows builds) | **GPLv3** (Steinberg's dual licence: its ASIO licence or GPLv3) | Keep the notice (done: the notices generator adds Steinberg's licence when `AMPSIM_WITH_ASIO` is on) |

**ASIO (decided 2026-10-05: Windows ships it).** JUCE 8.0.15 already bundles Steinberg's ASIO SDK headers, so `AMPSIM_WITH_ASIO` (on by default; the Windows workflow always sets it) is all a build needs; nothing to download. Steinberg offers the SDK under GPLv3 or under its proprietary ASIO licence, which requires a signed agreement before distributing; BellyDSP uses the GPLv3 option, which is compatible with the AGPLv3 (section 13 of each allows combining them), so no agreement is needed. Why: on Sean's Scarlett Solo, WASAPI can't run below 224 samples without dropping callbacks, and Focusrite's ASIO driver runs 64 to 256 cleanly (BUILD_PLAN decision log). WASAPI (JUCE's "Windows Audio", shared or exclusive mode) stays for interfaces without an ASIO driver. `-DAMPSIM_WITH_ASIO=OFF` builds without it.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `release.sh`: "the update signing key does NOT match ED_PUBLIC_KEY" | `ED_PUBLIC_KEY` was pasted wrong, or the keychain holds a different key (another Mac, a regenerated key). Compare with `generate_keys -p`. Never "fix" this by changing `ED_PUBLIC_KEY` after a release is out. |
| `codesign`: "no identity found" | The signing keychain isn't on your keychain search list (the script adds it; `security list-keychains -d user` shows it), or it's missing. `make_signing_identity.sh --check`. |
| `codesign`: errSecInternalComponent | The signing keychain is locked and the script couldn't unlock it: the login keychain item `ampsim-signing-keychain` is missing or wrong. |
| Upload: 401 or 403 | The token expired or lacks Contents: read and write on the public repo. |
| Upload: 422 when creating the release | The tag isn't on the public repo (release.sh pushes it first; check `git ls-remote --tags public`), or a release for it exists in a different state. |
| `release.sh`: "no git remote named 'public'" | One-time setup step 1: `git remote add public https://github.com/SeanSnaider/BellyDSP.git` |
| `release.sh`: "this commit isn't on public/main" | `git push public main` first: a release is only ever built from public source. |
| A player's Mac never updates | They're running the app from the DMG or Downloads (macOS runs quarantined apps from a read-only random path; Sparkle can't replace it). Move it to Applications. Or they never quit the app: updates install on quit. |
| A Mac asks for the microphone after an update | The update was signed with a different certificate (see the key box at the top). |
| "Check for updates..." is greyed out | A dev build (`AMPSIM_UPDATER` off): only release builds update themselves. |
| Windows build fails in CI | Expected the first time: none of it has been compiled with MSVC yet. The log says where. |

## Tone match's separation model

Tone match can separate the guitar out of a song with Demucs (docs/TONE_MATCH.md, Stage C). The model's weights (htdemucs_6s, 54,885,744 bytes) are **not** in the app, in content/, or in any release: the app downloads them the first time someone switches separation on, from the Demucs author's Hugging Face repository at a pinned commit, checks the size and SHA-256 (`GuitarSeparator::weightsUrl`, `weightsSha256`, `weightsBytes` in `src/tonematch/GuitarSeparator.h`), converts them to demucs.cpp's format in `~/Library/Application Support/BellyDSP/Separation` (`%APPDATA%\BellyDSP\Separation`), and deletes the download.

Why not a release asset on SeanSnaider/BellyDSP: the Demucs code is MIT, but nothing states a licence for the weights, and their training data includes MUSDB18, whose tracks are for academic use only. Re-hosting them would be redistributing something nobody has licensed to us (ASSUMPTIONS TM14). The upstream URL is pinned to a commit, so it can't change under us; if it ever disappears, separation reports that it couldn't download and everything else keeps working.

If Sean decides to host them after all (for example after getting the author's permission):
1. Upload the same file (`5c90dfd2.safetensors`, same SHA-256) as an asset of a release on the public repo, for example a release tagged `models-1`.
2. Change `weightsUrl` to `https://github.com/SeanSnaider/BellyDSP/releases/download/models-1/5c90dfd2.safetensors`. The SHA-256 and size stay the same, so nothing else changes.
3. Add the weights' licence to the notices (tools/notices/make_notices.cmake) next to Demucs's.

Nothing in `release.sh` touches the weights. `ampsim_separate` (tools/separate) runs the same download and separation from the command line, and prints the time per minute of audio and the peak memory.

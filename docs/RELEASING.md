# Releasing Amp Sim

How a version gets from this repo to your friends' Macs and PCs, and how it keeps updating itself after that. Written for you, Sean: what each piece is, what to do once, what to do per release, and what to do when something breaks.

> **The two keys you must never lose**
>
> 1. **The Ed25519 update key** (made by Sparkle's `generate_keys`, kept in your login keychain). Every installed copy of Amp Sim has the matching public key baked in and refuses any update that isn't signed with this private key. **If you lose it, every copy already out there can never update again.** Your friends would have to download and install a new version by hand, once, and you'd start over with a new key. Back it up the day you make it (step 5 below).
> 2. **The self-signed code-signing certificate** (made by `make_signing_identity.sh`). Losing it is less bad: updates still install (the Ed25519 key vouches for them), but macOS sees a new developer and asks every friend for microphone access again. Back it up too.
>
> Neither goes in the repo. A password manager is the right place for both.

## The moving parts

| Piece | What it is | Where |
|---|---|---|
| Source repo | This repo. Stays private. | github.com/SeanSnaider/... (wherever you push it) |
| Releases repo | A **public** repo that holds nothing but releases: the downloads and the appcasts. Friends download from its Releases page. | `RELEASES_REPO` in `tools/release/release.conf` (assumed `SeanSnaider/amp-sim-releases`; that file is the only place to change it) |
| Appcast | A small RSS file describing the newest version: its number, its download URL, its signature, release notes. `appcast.xml` for the Mac, `appcast-windows.xml` for Windows, attached to every release. | The apps read `https://github.com/<releases repo>/releases/latest/download/appcast.xml`. GitHub redirects "latest" to the newest release's copy, so publishing a release is all it takes; no website. |
| Sparkle 2 | The standard macOS updater (sparkle-project.org). Inside the app as `Contents/Frameworks/Sparkle.framework`. Checks once a day, downloads silently, installs when the app quits. | `tools/deps.conf` pins 2.10.0 by SHA-256; `tools/fetch_deps.sh` downloads it to `build-deps/` |
| WinSparkle | Sparkle's Windows counterpart (winsparkle.org). `WinSparkle.dll` next to `Amp Sim.exe`. Checks once a day and offers updates in a dialog. | Pinned (0.9.4) in the same file; the Windows CI fetches it |
| Code signature | macOS's proof of who made an app and that it hasn't changed. We sign with a free self-signed certificate. It gives every version the same identity, which is what keeps the microphone permission across updates and lets Sparkle accept them. It doesn't make Gatekeeper trust the app (that takes Apple's paid Developer ID and notarization), so friends click "Open Anyway" once. | `make_signing_identity.sh`, `sign_app.sh` |
| Ed25519 signature | A signature over each update file. The apps check it before installing anything, so nobody can push a fake update even if they got into the releases repo. | Private key in your login keychain; public key in `release.conf` (`ED_PUBLIC_KEY`) and from there in every build |

### What a friend experiences

**Mac, first install.** They download `AmpSim-x.y.z.dmg` from the releases page, open it, and drag Amp Sim onto Applications. The first launch says Apple can't check it for malicious software (an unnotarized app); they click Done, go to System Settings > Privacy & Security, click **Open Anyway**, and confirm. From then on it opens normally. (The `xattr` line in the DMG's read-me does the same in one Terminal command.) Then: allow the microphone, set the interface to 48 kHz, pick the input in Options. `docs/INSTALL.md` is their version of this.

**Mac, updates.** Nothing to do. Once a day (and at launch, if a day has passed) Amp Sim reads the appcast. If there's a newer version it downloads the zip in the background, checks its Ed25519 signature and that the new app is signed by the same certificate, and installs it when they quit Amp Sim. Next launch is the new version, with no microphone prompt (same signing identity) and, as far as Sparkle's documented behaviour goes, no new Gatekeeper prompt (Sparkle clears the quarantine flag on what it installs; the local test's updated app carried no quarantine attribute, but a real friend's first-update hasn't been seen yet). "Check for updates..." under the brand ("rig", top left) does it on demand, with a window.

**Windows, first install.** They download `AmpSim-x.y.z-windows-setup.exe`. SmartScreen says "Windows protected your PC" (it isn't signed with a paid certificate): **More info > Run anyway**. The installer needs no admin rights: it installs for the current user into `%LOCALAPPDATA%\Programs\Amp Sim`, with a Start menu entry and an uninstaller (Settings > Apps).

**Windows, updates.** Once a day WinSparkle checks `appcast-windows.xml`. If there's a newer version it shows a dialog ("A new version of Amp Sim is available", with the release notes): **Install update** downloads the installer, checks its Ed25519 signature, closes Amp Sim, runs the installer silently (a small progress window, no UAC prompt because it's per-user), and reopens Amp Sim. They can also pick Remind me later or Skip this version. It's not silent like the Mac, and can't be: WinSparkle has no install-on-quit mode, and its only "no question" mode (`win_sparkle_check_update_with_ui_and_install`) shows a progress window and quits the app immediately, which is worse in the middle of playing. See "Open questions" below.

## One-time setup, in order

Each step says what it does. You need a terminal in the repo folder.

### 1. Create the public releases repo

On github.com: New repository, name `amp-sim-releases` (or anything, then put `owner/name` in `RELEASES_REPO` in `tools/release/release.conf`), **Public**, and tick **Add a README** (GitHub can't attach a release to an empty repo; the README gives it its first commit). Something like "Downloads for Amp Sim. Get the latest from Releases." is plenty for the README.

### 2. A token that can publish to it, and only to it

GitHub > Settings > Developer settings > Personal access tokens > **Fine-grained tokens** > Generate new token:

- Repository access: **Only select repositories** > `amp-sim-releases`
- Permissions > Repository permissions > **Contents: Read and write** (Metadata: read-only gets added automatically)
- Expiration: up to a year; put a reminder in your calendar to make a new one

Copy the token, then store it in your login keychain (it asks for it; nothing lands in your shell history):

```
security add-generic-password -a "$USER" -s ampsim-github-token -w
```

`github_release.sh` reads it from there (or from `$GITHUB_TOKEN` if set). Never commit it.

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

### 6. Decide how JUCE is licensed

`JUCE_LICENCE` in `release.conf` is `undecided`, and a real release refuses to run until you set it. See "Licences" below; it's your call, not a technicality.

### 7. Push the source repo, and (optionally) add the CI secrets

The Windows build runs on GitHub Actions, so the source repo has to be on GitHub (private is fine). After the first push, in the source repo's Settings > Secrets and variables > Actions, you can add:

| Secret | What | Needed for |
|---|---|---|
| `RELEASES_TOKEN` | A token like step 2's (Contents: read and write on the releases repo only). Make a second one so you can revoke them separately. | The Windows job uploading its installer to the release by itself |
| `AMPSIM_ED_PRIVATE_KEY` | The one-line private key from step 5's backup | The Windows job signing its installer by itself |

**My recommendation: skip `AMPSIM_ED_PRIVATE_KEY`.** It's the one thing that must never leak, and as a repo secret it's readable by any workflow code that runs in the repo. Without it, the Windows job still builds and tests and keeps the installer as an artifact, and `tools/release/add_windows.sh` signs it on your Mac and uploads it (two commands per release instead of zero). If you'd rather have Windows fully automatic, add both secrets; it works either way.

Actions minutes: the source repo is private, so builds use GitHub's free allowance (2,000 minutes a month on the Free plan), and Windows minutes count double. That's why the Windows workflow runs only on release tags or by hand, and the macOS test workflow only by hand (macOS minutes count ten times).

## Every release

1. **Write the notes**: `release-notes/<version>.md` (see `release-notes/README.md`), commit it on `main`.
2. **Release**:
   ```
   tools/release/release.sh 0.1.1
   ```
   It refuses unless you're on `main` with a clean tree, the version is higher than every earlier `v*` tag, the signing key matches `ED_PUBLIC_KEY`, and the token exists. Then, saying what each step does: a clean universal build in `build-release/` (arm64 and x86_64; your `build/` is untouched), the full test suite (any failure stops it), signing, the DMG and the update zip, the Ed25519 signature and `appcast.xml`, the GitHub release (marked latest) with the files attached, and finally a `Release 0.1.1` commit of `VERSION` and a local tag `v0.1.1`. It takes about 15 to 20 minutes, most of it compiling everything twice (once per architecture).
   - `--dry-run` does everything except publish, commit, and tag; the files land in `dist/0.1.1/`. Worth doing before the first real release.
   - If anything fails, fix it and run the same command again: the build starts clean and the upload replaces files already there.
3. **Push**: `git push origin main v0.1.1` (the script prints it). The tag starts the Windows build.
4. **Windows**: if you added both secrets, the Windows job attaches `AmpSim-0.1.1-windows-setup.exe` and `appcast-windows.xml` to the release by itself. Otherwise, when the run is green: download its artifact (Actions > the run > Artifacts), unzip it, and run `tools/release/add_windows.sh 0.1.1 <that folder>`.

Within a day, every friend's copy has it (Mac: on their next quit; Windows: when they click Install).

### Versions

`VERSION` holds the current version (`major.minor.patch`). It becomes `CFBundleShortVersionString` and `CFBundleVersion` on the Mac, which Sparkle compares, and the version WinSparkle compares on Windows: an update is only offered if it's higher, so versions only ever go up. `release.sh` enforces that against the existing tags, sets `VERSION` itself, and passes the version to the build (`-DAMPSIM_VERSION`), so you never edit it by hand.

## Signing, in more detail

`sign_app.sh` signs inside out: Sparkle's `Autoupdate` tool and `Updater.app`, then `Sparkle.framework`, then the app (`--deep` signing is discouraged by Apple). It verifies with `codesign --verify --deep --strict` and prints the designated requirement, which looks like:

```
identifier "com.seansnaider.ampsim" and certificate root = H"<your certificate's SHA-1>"
```

That pins the certificate, not the build, so every version you sign has the same requirement. Sparkle checks that the new app satisfies the old app's requirement, and macOS's privacy database (TCC) keys the microphone permission on it.

**No hardened runtime for the self-signed build.** The hardened runtime turns on library validation, which only lets an app load frameworks signed by Apple or by the same Team ID. A self-signed certificate has no Team ID, so the app can't load Sparkle.framework. Tested: with `--options runtime` the app dies at launch with `Library not loaded: @rpath/Sparkle.framework/... mapping process and mapped file (non-platform) have different Team IDs`. The hardened runtime only matters for notarization, which a self-signed app can't get anyway. (`AMPSIM_HARDENED_RUNTIME=1` turns it on for experiments.)

**Sparkle's XPC services are left out.** `Installer.xpc` and `Downloader.xpc` exist only for sandboxed apps; Amp Sim isn't sandboxed (it needs to read captures and IRs anywhere), so the build removes them.

**The Developer ID path, for later** (written, untested). If you ever pay for the Apple Developer Program ($99/year): put your "Developer ID Application" certificate in the login keychain, store notarization credentials once (`xcrun notarytool store-credentials ampsim-notary --apple-id ... --team-id ...`), and release with `AMPSIM_SIGN_IDENTITY="Developer ID Application: Sean Snaider (TEAMID)" tools/release/release.sh x.y.z`. `sign_app.sh` then uses the hardened runtime, a secure timestamp, and `tools/release/AmpSim.entitlements` (audio input), and `notarize.sh` notarizes and staples the DMG. Friends would no longer need "Open Anyway". The switch changes the designated requirement: that one update still installs (same Ed25519 key), but macOS asks for the microphone once more.

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

`content/` holds captures and IRs that ship inside the app (empty until the starter pack is chosen). The build copies it into `Amp Sim.app/Contents/Resources/content` (Windows: `content\` next to the exe, which the installer installs). Presets refer to those files as `factory:models/...` and `factory:irs/...`, resolved against the installed app wherever it is; the preset format stays version 2. Every file needs an entry in `content/manifest.json` (source, author, licence, licence text), or the build stops; the entries become their section of `THIRD_PARTY_NOTICES.txt`. Details in `content/README.md`.

## Licences

`THIRD_PARTY_NOTICES.txt` is generated at build time (`tools/notices/make_notices.cmake`) from the actual licence files in the submodules and downloads, and ships in the app (shown under rig > About / licenses), in the DMG, and next to the Windows exe:

| Component | Licence | What it asks of you |
|---|---|---|
| JUCE 8.0.15 | **AGPLv3, or the JUCE 8 licence** | **Your decision, see below** |
| NeuralAmpModelerCore | MIT | Keep the notice (done) |
| Eigen | MPL 2.0 | Keep the notice; Eigen's source is available upstream, unmodified (done) |
| nlohmann/json | MIT | Keep the notice (done) |
| Sparkle | MIT (plus bsdiff and sais notices) | Keep the notice (done) |
| WinSparkle | MIT (its COPYING also carries an OpenSSL acknowledgement and expat's licence file) | Keep the notice (done) |
| Geist, Fraunces | SIL Open Font License | Keep the licence with the fonts (done) |
| zlib, libpng, IJG JPEG, FLAC, Ogg Vorbis, HarfBuzz, SheenBidi (inside JUCE) | Permissive | Keep the notices (done) |
| ASIO SDK headers (only with `AMPSIM_WITH_ASIO`) | **Steinberg's ASIO licence, or GPLv3** | See below |

**JUCE (open question for you).** JUCE is dual-licensed. Giving builds to anyone, even free to friends, means picking one:
- **AGPLv3**: free. You must offer everyone who gets the app its complete source under the AGPLv3 (and their right to share it). In practice: make this repo (or a source snapshot per release) public under the AGPLv3 and put its URL in `SOURCE_URL`. Your own code then has to be AGPL-compatible.
- **The JUCE 8 licence**: JUCE's own terms (juce.com/legal/juce-8-licence). There's a free tier for individuals and small businesses under a revenue limit; check the current terms, including what it says about splash screens and data collection, before choosing it.
Set `JUCE_LICENCE` to `AGPLv3` or `JUCE`. `release.sh` refuses a real release while it's `undecided`, and the notices state the choice.

**ASIO (open question).** JUCE 8.0.15 already bundles Steinberg's ASIO SDK headers, so `-DAMPSIM_WITH_ASIO=ON` (or the Windows workflow's "asio" checkbox) is all a build needs; nothing to download. Licensing: Steinberg offers them under GPLv3 or under its proprietary ASIO licence, which requires a signed agreement with Steinberg before distributing. GPLv3 fits with JUCE under the AGPLv3 (the two may be combined), not with keeping the source closed. Without ASIO, Windows friends use WASAPI (JUCE's "Windows Audio", shared or exclusive mode), which needs nothing extra and gets reasonably low latency in exclusive mode; many interfaces' ASIO drivers would be lower. Default: off.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `release.sh`: "the update signing key does NOT match ED_PUBLIC_KEY" | `ED_PUBLIC_KEY` was pasted wrong, or the keychain holds a different key (another Mac, a regenerated key). Compare with `generate_keys -p`. Never "fix" this by changing `ED_PUBLIC_KEY` after a release is out. |
| `codesign`: "no identity found" | The signing keychain isn't on your keychain search list (the script adds it; `security list-keychains -d user` shows it), or it's missing. `make_signing_identity.sh --check`. |
| `codesign`: errSecInternalComponent | The signing keychain is locked and the script couldn't unlock it: the login keychain item `ampsim-signing-keychain` is missing or wrong. |
| Upload: 401 or 403 | The token expired or lacks Contents: read and write on the releases repo. |
| Upload: 422 when creating the release | The releases repo is empty (no README commit), or the tag already exists there in a different state. |
| A friend's Mac never updates | They're running the app from the DMG or Downloads (macOS runs quarantined apps from a read-only random path; Sparkle can't replace it). Move it to Applications. Or they never quit the app: updates install on quit. |
| A friend's Mac asks for the microphone after an update | The update was signed with a different certificate (see the key box at the top). |
| "Check for updates..." is greyed out | A dev build (`AMPSIM_UPDATER` off): only release builds update themselves. |
| Windows build fails in CI | Expected the first time: none of it has been compiled with MSVC yet. The log says where. |

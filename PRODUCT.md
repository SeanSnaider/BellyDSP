# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

Two audiences, weighted equally (Sean, 2026-10-13):

- **Guitarists who'll download and play it**: Sean's friends and the wider NAM (Neural Amp Modeler) community, on a Mac or a Windows PC with an audio interface. Their job: find out what BellyDSP does, whether it runs on their computer, download it, and get sound on the first launch.
- **Engineers and recruiters looking at it as Sean's project**: their job is to see what was built and how (C++20, JUCE, NeuralAmpModelerCore, the real-time rules, the tests and benchmarks), and to reach the source.

Sean also reads the stats page (downloads and active installs).

## Product Purpose

BellyDSP is a free, open-source guitar amp simulator and multi-effects app for macOS and Windows. The website (bellydsp.com) explains it, gets it onto a player's computer, shows what changed in each version, publishes its download statistics, and points to the source. Success: a guitarist downloads the right build and plays through it the first time they open it; an engineer finds the substance and the code quickly.

## Positioning

Three pillars, weighted equally (Sean, 2026-10-13):

- **Match a tone from a song.** Load a song, pick a section, optionally separate the guitar from the mix and play the part along with it; BellyDSP sets the amp, its Gain and tone, a pedal and compressor if they help, the cab, and a fine correction curve to get close to that recording.
- **Free and open source.** AGPL-3.0-or-later; every download is built from a commit that's public and tagged first, so the exact source of every build is on GitHub.
- **The rig.** Eight built-in neural amps (Glass, Ember, Monolith, Lantern, Basalt, Comet, Forge, Quartz), each with its own head, played one at a time; a cab with movable mics; pedals, effects, a tuner, presets with scenes, MIDI footswitch control; zero added latency.

## Operating Context

- **Pages:** the overview (`site/index.html`), the download page (`site/download/`, which detects the visitor's OS and puts that button first, with first-launch steps and the changelog), the stats page (`site/stats/`), and a 404.
- **Hosting:** GitHub Pages from the public repo SeanSnaider/BellyDSP. The `Website` workflow builds the site's data at deploy time: `data/releases.json` from the GitHub releases, and `stats.json` from a daily stats workflow. The changelog comes from `release-notes/*.md`.
- **Releases:** come out a few at a time. Installed copies update themselves, on the Mac when BellyDSP quits and on Windows when the player clicks Install.
- **Setup a player needs:** an audio interface set to 48 kHz with its input gain at minimum. On Windows, ASIO is recommended.
- **How the app is used** (Sean, 2026-10-13):
  - **The split:** about 90% of the time playing through it, about 10% dialing tones.
  - **While playing:** the player needs to see the preset and scene and switch between them, see what's on at a glance, and reach a few key knobs, with nothing they need hidden in menus or behind extra clicks.
  - **Control:** mouse or trackpad and the keyboard. The app also takes a MIDI footswitch, but Sean doesn't use one.
  - **What would feel wrong:** a redesign that's too minimal and loses the amps' character, or one that hides controls.

## The app

- **What it is:** a standalone desktop app for macOS and Windows, built with C++ and JUCE. Every pixel is drawn in code.
- **The window:** a 1280 x 760 canvas that scales with the window.
- **Pages:** the top bar (presets, A/B, undo, tuner and meters), one page per section of the signal chain (Input, Pre FX, Amp, EQ, Cab, Post FX, Output), the tuner, and the Match tone page, with the chain along the bottom as navigation.
- **Redesign scope:** the whole look is open (Sean, 2026-10-13: "nothing is sacred"), including the pages the original UI handoff never designed. The website follows whatever the app becomes.
- **Direction for the redesign:** the category standard, executed at full craft (Sean chose it on 2026-10-13 over the rolled directions):
  - **Layout:** a preset bar on top, the amp on a dark stage, a row of pedals, an inspector for the selected block, and scenes along the bottom.
  - **Quality bar:** Neural DSP's plugins, for one amp per screen, rendered gear, and polish. They're an internal reference only: no other company's product or amp names ever appear in the app or on the site.

## Capabilities and Constraints

- **Stack:** static HTML, CSS and a little JavaScript in `site/`. Python scripts in `tools/site/` make the images (`make_site_assets.py`), build the data (`build_site_data.py`), and check the site (`check_site.py`: links, images, no outside requests).
- **No trackers today:** the site has no trackers or cookies and loads nothing from outside the site. Download counting comes from GitHub's own release statistics. Sean didn't make "static, no trackers" binding in this interview, so it's the current state, not a hard constraint.
- **Platforms the app supports:** macOS (one universal build for Apple silicon and Intel) and Windows x64. 48 kHz only.
- **Licences:** the app is AGPL-3.0-or-later (JUCE under AGPLv3). The built-in amp captures are Sean's, under CC BY 4.0, and the built-in cab IRs are CC0. The Geist font is under the SIL OFL.
- **Never:**
  - other companies' product or amp names, or band names;
  - claims about how it sounds (players judge that);
  - implying the built-in captures are real amps (they're trained from BellyDSP's own amp models);
  - invented testimonials, users, or numbers.

## Brand Commitments

- **The name and mark:** BellyDSP, and the "B." mark (the app icon, `resources/icon/`).
- **The site looks like the app:** the app's dark look, its accent, and the "B." mark carry over to the site (binding, Sean, 2026-10-13). The accent was emerald when Sean said this; the app redesign he chose the same day made it orange (#ff8a00), so the site follows the orange.
- **Imagery:** real app snapshots rendered by the test suite, no mockups or stock (binding, Sean, 2026-10-13).
- **The stats page stays public** (binding, Sean, 2026-10-13).
- **Voice:** direct, plain, a little casual, no corporate buzzwords, and no em dashes.
- **How it was built:** when the site describes how it was built, it says plainly that Sean designed and directed it and built it with AI coding agents (Anthropic's Claude Code), as his LinkedIn and resume material does.

## Evidence on Hand

- **App snapshots,** rendered by the test suite into `build/proof/` and converted to WebP in `site/assets/img/` by `make_site_assets.py`:
  - the Amp page with each of the eight amps on the shelf (`build/proof/amp_shelf/`);
  - every block's page (`editor_<page>.png`);
  - the cab with its built-in IRs, and the tuner;
  - the tone match page in each state (`build/proof/tone_match/`);
  - the nine amp heads (`amp_head_<material>.png`).
- **Release notes:** `release-notes/0.1.0.md` and `0.2.0.md`.
- **Real statistics:** download counts and active-install estimates, from the stats data.
- **Measured numbers:**
  - about 8,700 automated checks;
  - the real-time safety test (0 allocations, frees, locks);
  - CPU per 128-sample buffer, on the Mac and on GitHub's Windows runner;
  - the tone matcher's benchmark results (`docs/TONE_MATCH.md`, `docs/BUILD_PLAN.md` "CPU").
- **Absent, so never fabricate them:** audio demos (a TODO in `site/index.html`), a video, testimonials, user quotes, press.

## Product Principles

1. **Show the real thing.** Real app snapshots, real measurements, real release notes; nothing invented or staged.
2. **One step from sound.** The right download and the first-launch setup are never more than a click away from anywhere on the site.
3. **Two readers, both served.** The guitarist's path (what it does, get it, set it up) and the engineer's path (how it works, how it's tested, the source) both have depth, and neither buries the other.
4. **Say what it does, measure the rest.** Describe features plainly and back technical claims with measured numbers; leave the sound to the player's ears.
5. **Open in practice.** The source of every download, the licence, and the statistics are a click away.

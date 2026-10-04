# BellyDSP

A guitar amp simulator and multi-effects processor in C++ on JUCE, with neural amp captures (NAM) running on NeuralAmpModelerCore, the official NAM engine. Built as a personal tool for practicing and writing, and free software: AGPL-3.0-or-later (see [License](#license)).

Current target: a standalone macOS app that plays live through a Focusrite Scarlett Solo 4th Gen. VST3/AU plugin export comes later from the same JUCE target.

## Features

Everything in the plan is built (see `docs/PROGRESS.md` for the measurements behind each item, and `docs/ASSUMPTIONS.md` for the choices waiting on Sean's review):

- **Amps.** Three always-running NAM slots (any `.nam` capture), switched seamlessly from the panel or a MIDI footswitch (program change 1/2/3). Each is loudness-normalized, calibrated to the capture's recorded input level, and has trims plus five tone bands. Each slot comes with a built-in capture, so there's something to play at once: Glass (clean), Ember (crunch), and Monolith (high gain), stand-ins trained from the project's own gray-box amp until real captures replace them.
- **Cab.** Two close mics and a stereo room mic, each an IR, with level, pan, polarity, delay, and mute; auto phase alignment; low and high cuts. A close mic loaded with a cab pack moves on a position pad, morphing between captures.
- **Gates.** Gate A before the amp and Gate B after it, linked by default (one decision applied at both points), detecting from the clean DI, with adaptive release, hysteresis, hold, and Learn.
- **Drive.** A boost (Clean, Tight, Screamer) and an overdrive (Mid Drive, Distortion, Transparent, Fuzz), modelled from the schematics and validated against circuit simulations, oversampled 4x or 8x with no added latency.
- **Pre FX** (reorderable): Gate A, compressor (Studio and Pedal), boost, overdrive, EQ (accurate 9-band graphic or 5-band parametric, plus cuts).
- **Post FX** (reorderable): EQ, compressor, a key-aware 4-voice harmonizer, an 8-voice multivoicer (Poly or Mono), Bloom (bitcrush, phaser with Classic, Modern, and Vibe, flanger with through-zero), chorus (Classic, Dimension, Tri), delay (digital, analog, tape; stereo, ping-pong, dual; ducking), and reverb (Room, Hall, Plate; freeze; shimmer). Delay and reverb tails spill over when bypassed.
- **Tuner.** A needle on a +-50 cent scale, in tune within 3 cents, the six strings of four tunings, A4 from 430 to 450 Hz, muting while engaged.
- **Tempo.** A global tempo, tapped from the panel or a footswitch; the delay, chorus, phaser, flanger, and reverb pre-delay can sync to it.
- **Presets and scenes.** JSON presets with library-relative files found again by content hash when moved, eight scenes per preset switched from a footswitch, undo/redo, A/B, and five factory style presets (Modern Prog, Math Rock, Tech Death, Metal, Midwest Emo) on the built-in captures and cabs.
- **MIDI.** Right-click any control to MIDI-learn a footswitch (toggle or momentary) or an expression pedal; mappings are saved with presets.
- **Real time.** Zero added latency (only the opt-in through-zero flanger reports 5 ms), no allocation, freeing, or locking on the audio thread (tested on every feature), and the whole rig with every block on at about 18% of a 128-sample buffer's time.

The GUI follows the UI handoff in `docs/ui/amp-ui-handoff/` (`docs/UI_DESIGN.md` lists where it differs): a fixed 1280 x 760 canvas that scales with the window; a top bar with the preset (browser, previous and next), Save, the tuner, and the meters; the signal chain along the bottom (Input, Pre FX, Amp, EQ, Cab, Post FX, Output: click a block for its page, its dot to bypass it); the amp page with three heads (Glass, Ember, Monolith), the output spectrum, and a shared Input / Gate / Output strip; the cab page with the cab library, a speaker with two draggable mics, and the mic panel; the tuner page; and the effects as tabs (drag to reorder). Undo and redo are Cmd-Z and Shift-Cmd-Z; A/B, scenes, tempo, and the footswitch settings are on the Output page.

## Building

You need Apple's command line tools (`xcode-select --install`) and CMake (installed in `~/.local/bin`). Full setup, including the Scarlett Solo gotchas, is in `docs/SETUP.md`.

```
git submodule update --init --recursive      # JUCE and NeuralAmpModelerCore
cmake -B build -DCMAKE_BUILD_TYPE=Release    # configure
cmake --build build -j                       # compile
ctest --test-dir build --output-on-failure   # run the tests
open build/BellyDSP_artefacts/Release/Standalone/BellyDSP.app
```

## Releasing

Releases are published on the public repo, https://github.com/SeanSnaider/BellyDSP/releases, and the app updates itself (Sparkle on the Mac, WinSparkle on Windows). Every release is built from a commit that's on the public repo's `main` and tagged there, so the exact source of every download is public. One command checks that, builds, tests, signs, packages, tags, pushes the tag, and publishes: `tools/release/release.sh <version>`. `docs/RELEASING.md` has the one-time setup and the details; `docs/INSTALL.md` is the install guide.

## Repo layout

| Path | What it is |
|---|---|
| `src/dsp/` | The DSP: the Block interface, the chain, every amp, cab, filter, and effect |
| `src/PluginProcessor.*` | Parameters, state, file loading, MIDI, and the audio callback, which just drives the chain |
| `src/BlockParameters.*`, `src/Presets.*` | Parameter glue for each effect block, and the preset format |
| `src/PluginEditor.*` | The editor: lays out the GUI and runs presets, scenes, MIDI learn menus, and undo |
| `src/platform/` | What the shipped app knows about itself: its version, its bundled files, and the auto-updater |
| `src/ui/` | The GUI: tokens and fonts, LookAndFeel, controls, meters, the analyzer, the signal chain, the amp, cab, and tuner pages, and every effect's page |
| `presets/factory/` | The factory presets, embedded in the app |
| `docs/ui/amp-ui-handoff/` | The GUI's design: the handoff spec, its reference HTML, and screenshots (`docs/UI_DESIGN.md` lists where the build differs) |
| `resources/fonts/` | Geist and Fraunces (SIL OFL, licences alongside), embedded in the app |
| `tools/render/` | `ampsim_render`: runs a WAV through the chain offline, with CPU timing |
| `tools/device_probe/` | `ampsim_device_probe`: opens the interface and reports timing and input levels |
| `tools/capture/`, `tools/train_capture.sh`, `tools/fetch_nam_input.sh` | Capturing your own gear for NAM: `ampsim_capture` plays NAM's input file through the gear and records it, and the trainer wrapper runs NAM's official trainer on it (`docs/CAPTURING.md`) |
| `tools/content/` | The scripts that imported the bundled cab IRs and ranked them |
| `tools/release/`, `tools/fetch_deps.sh`, `tools/notices/` | The release pipeline: signing, packaging, appcasts, publishing, the update test, the licence notices |
| `installer/` | The DMG's read-me and the Windows installer script (Inno Setup) |
| `content/` | Captures and IRs bundled with the app (so far two CC0 cab IR packs, 21 IRs), each with its licence in `manifest.json` |
| `release-notes/` | One Markdown file per released version |
| `.github/workflows/` | The Windows build and installer, and an on-demand macOS test run |
| `tests/` | The test suite (`ampsim_tests`), including real-time safety and NAM differential tests |
| `third_party/` | JUCE 8.0.15 and NeuralAmpModelerCore v0.6.0, as pinned git submodules |
| `prototypes/` | Python lab bench. Algorithms get prototyped and listened to here before being ported |
| `docs/BUILD_PLAN.md` | Full feature spec, architecture, build order, and decision log |
| `docs/PROGRESS.md` | Phase-by-phase status |
| `docs/SETUP.md` | Mac + Scarlett Solo setup and troubleshooting |
| `docs/DSP_REFERENCE.md` | Glossary and the sources behind each algorithm |
| `docs/ASSUMPTIONS.md` | Every decision made on Sean's behalf during the unattended build, to review |
| `docs/NAM_UPSTREAM.md` | NAM core findings worth taking upstream |
| `docs/RELEASING.md`, `docs/INSTALL.md` | How releases and updates work (for Sean), and how to install (for players) |
| `CLAUDE.md` | Context and rules for AI assistants working in this repo |
| `LICENSE` | The GNU AGPL v3, the licence of all the code |
| `tools/spdx_headers.py` | Adds (or checks) the SPDX licence header on every source file |

## Amp models

No `.nam` models are bundled. Download amp-only captures (no cab baked in) from TONE3000, or train your own with NAM's trainer, and keep them in `models/`, which is gitignored. NAM core's example models in `third_party/NeuralAmpModelerCore/example_models/` work for testing.

## Cab IRs

21 public-domain (CC0) IRs from Jester Dyne Productions are bundled (`content/`, listed under "Built in" on the cab page). Load your own `.wav` files too (free and paid packs are widely available); keep them in `irs/`, which is gitignored.

## License

BellyDSP Copyright (C) 2026 Sean Snaider

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (SPDX: `AGPL-3.0-or-later`). This program comes with ABSOLUTELY NO WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [`LICENSE`](LICENSE) for the full text.

Source: https://github.com/SeanSnaider/BellyDSP. The app's brand menu links to the exact source of each version.

- Every source file carries an SPDX header (`tools/spdx_headers.py`).
- JUCE is used under its AGPLv3 option, which is why the whole program is AGPL.
- Sean's own NAM captures (in `content/`, as they're added) are licensed under [CC BY 4.0](content/licenses/CC-BY-4.0.txt), attribution "Sean Snaider". The bundled cab IRs are CC0 1.0.
- Third-party components keep their own licences, listed with their full texts in `THIRD_PARTY_NOTICES.txt` (generated by `tools/notices/make_notices.cmake`, shipped in the app and the downloads).

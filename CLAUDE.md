# CLAUDE.md

Context for AI assistants (Claude Code, Claude chat) working on this repo. Read this first, then `docs/BUILD_PLAN.md` for the full spec and `docs/PROGRESS.md` for current status.

If another CLAUDE.md is also loaded (for example `~/Desktop/CLAUDE.md`, left over from the older Rust version of this project, which says Sean writes all the code), this file wins for this repo.

## The project

Sean's personal guitar amp sim and multi-FX, in C++ on JUCE, with neural amp models (NAM) running on NeuralAmpModelerCore, the official NAM engine. Sean wants to contribute to the NAM project, so understanding NAM's architecture deeply matters here. It's a passion project and a tool he'll actually use, not a portfolio piece, so sound quality and his own playing experience are the success criteria. He plays progressive and math rock and wants a genuinely tight tech death tone.

The current target is the standalone macOS app with a Focusrite Scarlett Solo 4th Gen. CoreAudio calls it "Scarlett Solo 4th Gen", with 4 inputs (Input 1, Input 2, Loopback 1, Loopback 2) and 2 outputs. The guitar is on Input 1, which is channel 0. A level probe confirmed that on 2026-09-28, and `ampsim_device_probe` showed the same layout on 2026-10-01. VST3/AU export comes later from the same JUCE target.

## Working agreement

Claude may write code for this project, including full implementations, with three conditions:

1. Nontrivial DSP (a transfer function, a filter design, a crossfade law, a pitch detection step) gets comments explaining the math and where it comes from, because understanding the algorithms is half the point.
2. Every feature gets automated tests, and Claude shows Sean the evidence: real test output and measured numbers, plus renders or editor snapshots where they help. "It builds" isn't proof. Anything only Sean can verify (how it sounds, how it feels live) is labeled as unverified, with steps for him to check it.
3. Sean does the listening. Claude can verify math and measure behavior, but never claims something "sounds good".

The amp engine is NeuralAmpModelerCore, so nobody writes the WaveNet core for the product. If Sean takes the optional side track of writing his own WaveNet (Conv1D with its ring buffer, Layer, LayerArray) and checking it against NAM core, he writes that by hand. Claude explains each piece first and reviews it after, but doesn't hand over the implementation.

Claude summarizes what each module does and why, rather than walking through it line by line.

## About Sean

Strong in Python and Java, knows some Rust, new to C++ (Java background maps reasonably well: classes and interfaces work similarly; manual memory, headers, and templates don't). Comfortable with algorithms and systems. Not experienced with tooling outside the languages themselves (CMake, git submodules, audio drivers, DAWs, plugin formats, GUI frameworks). When a task involves a tool, explain what it is, what the command actually does, and why it's needed. Never just drop a command without context.

When Claude recommends something and Sean chooses differently, Claude argues its case once, clearly, and asks again before writing the decision into the docs. If Sean still disagrees, his call stands and gets written in. Design decisions go in the decision log in `docs/BUILD_PLAN.md`.

Communication: start responses with "Sean". Talk like a real person: direct, blunt, swearing is fine. Challenge bad ideas and say so when something won't work or isn't worth doing. Never use em dashes.

## Architecture rules

Layout: `src/dsp/` holds the DSP blocks (they may use JUCE's `juce_dsp` and `juce_audio_basics`, never processor or GUI types). `src/PluginProcessor.*` and `src/PluginEditor.*` are thin glue. `tools/` holds the offline renderer and the hardware probe, `tests/` the test suite, and `third_party/` the pinned git submodules (JUCE 8.0.15, NeuralAmpModelerCore v0.6.0). Don't edit submodules. Upgrading one is a deliberate decision that goes in the decision log, and changes to NAM core go upstream as PRs.

Every DSP unit implements the `ampsim::Block` interface (`prepare`, `process`, `reset`, `latencySamples`, `isStereo`). The chain owns the blocks as typed members in signal-chain order, takes the DI snapshot, makes the one mono-to-stereo copy, and crossfades bypass. Don't special-case blocks inside the chain. See "Block design" in `docs/BUILD_PLAN.md`.

The real-time rules in `process()` are absolute: no heap allocation or freeing, no locks, no file or network I/O, no printing or logging. Allocate everything in `prepare()`. Anything slow (model loading, IR loading) runs on a background thread and is handed over through `ampsim::Handoff`, and the audio thread hands old objects back to be freed elsewhere. The "Real-time safety" test must stay at 0 allocations, 0 frees, 0 blocking locks, and new audio-path features get exercised inside it. If a change would break these rules, stop and flag it rather than working around it.

Every user-facing parameter is smoothed: blocks own a `juce::SmoothedValue` and the processor sets targets once per buffer. Knob-dependent filter coefficients are recomputed from smoothed values.

Parameter IDs (`input_gain`, `output_gain`, `cab_bypass` so far) are permanent once presets exist. Never rename one. Add new IDs instead.

The chain runs at 48 kHz only, because NAM captures are trained at 48 kHz. Anything else mutes the output and shows a warning.

Zero added latency is the rule. The only exception is the flanger's opt-in through-zero mode, reported to the host and only while active. The harmonizer is not an exception: its voices trail the dry signal, and the dry signal is never delayed.

Pitch detection for the harmonizer, and both gates' detectors, read the clean DI snapshot, never the processed signal. Harmonies are generated after the amp and cab. These are deliberate decisions documented in `docs/BUILD_PLAN.md`. Don't change them without discussing.

## Python prototypes and golden tests

New algorithms get prototyped in `prototypes/` first when there's any doubt about the math or the sound. Every DSP block should have a golden test comparing it to a trusted reference: the Python prototype, a brute-force computation (as the cab tests do), or an official implementation (as the NAM tests do with NAM core's own render tool). When porting, match the reference first and improve second. Python runs through `uv` (already installed in `~/.local/bin`).

## Commands

| Command | What it does |
|---|---|
| `git submodule update --init --recursive` | Fetch JUCE and NAM core after a fresh clone |
| `cmake -B build -DCMAKE_BUILD_TYPE=Release` | Configure: generates the build files (once, or after editing CMakeLists.txt) |
| `cmake --build build -j` | Compile everything |
| `ctest --test-dir build --output-on-failure` | Run the test suite |
| `build/ampsim_tests_artefacts/Release/ampsim_tests --proof-dir build/proof` | Run the tests with every measurement printed. Renders and editor snapshots land in `build/proof` |
| `open "build/AmpSim_artefacts/Release/Standalone/Amp Sim.app"` | Run the app |
| `build/ampsim_render_artefacts/Release/ampsim_render --model amp.nam --ir cab.wav di.wav out.wav` | Render a DI file offline through the same chain, with CPU timing |
| `build/ampsim_device_probe_artefacts/Release/ampsim_device_probe` | Open the Solo with silent output and report callback timing and input levels |
| `build/ampsim_capture_artefacts/Release/ampsim_capture --level-check --in-channel 1` | Capture tool (docs/CAPTURING.md): play NAM's input file through gear and record it; `--simulate drive` needs no hardware |
| `tools/train_capture.sh --input build-deps/nam/input.wav --output <rec.wav> --name <n> --tone-type <t> --gear-type <g>` | Train a capture with NAM's official trainer (pinned in tools/deps.conf) |
| `python prototypes/amp_sim.py <di.wav> --all-channels` | Render the old gray-box Python amps (reference only) |
| `tools/fetch_deps.sh` | Download the pinned Sparkle (and with `--windows`, WinSparkle) into `build-deps/` |
| `tools/release/release.sh <version> [--dry-run]` | Build, test, sign, package, and publish a release (docs/RELEASING.md). Never run it without `--dry-run` unless Sean asks |

Use Release builds for anything involving live audio. NAM core is always compiled with -O3, but JUCE and our code aren't in Debug.

## Things not to do

Don't bundle or reference proprietary IRs, captures, or assets from Neural DSP or any other company, and don't use their product or amp names in the UI. Don't build the real GUI before Phase 11; a basic panel is fine. The GUI is agent-built under Sean's visual direction, following `docs/UI_DESIGN.md` once it exists, and should be polished. Don't add a dependency when a small hand-written DSP implementation is the educational point (filters, delay lines, YIN, PSOLA). Using libraries for plumbing (JUCE for audio I/O, FFT, and file formats; NAM core for inference) is fine.

## Keeping docs current

When a phase or sub-feature is finished, update `docs/PROGRESS.md`. When a design decision changes, update `docs/BUILD_PLAN.md` and add a line to its decision log.

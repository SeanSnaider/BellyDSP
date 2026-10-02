# Amp Sim

A guitar amp simulator and multi-effects processor in C++ on JUCE, with neural amp captures (NAM) running on NeuralAmpModelerCore, the official NAM engine. Built as a personal tool for practicing and writing.

Current target: a standalone macOS app that plays live through a Focusrite Scarlett Solo 4th Gen. VST3/AU plugin export comes later from the same JUCE target.

## Features

Working now (see `docs/PROGRESS.md` for status and the measurements behind each item):

- **Amps.** Three always-running NAM slots (any `.nam` capture), switched seamlessly from the GUI or a MIDI footswitch (program change 1/2/3). Each is loudness-normalized, calibrated to the capture's recorded input level, and has trims plus five tone bands (depth, bass, mid, treble, presence).
- **Cab.** Two close mics and a stereo room mic, each an IR, with level, pan, polarity, delay, and mute; auto phase alignment; low and high cuts. A close mic loaded with a cab pack (a folder of IRs at different mic positions) moves on a position pad, morphing between captures without comb filtering.
- **Pre FX** (reorderable): compressor (Studio and Pedal modes) and EQ (accurate 9-band graphic or 5-band parametric, plus cuts).
- **Post FX** (reorderable): EQ, compressor, chorus (Classic, Dimension, Tri), delay (digital, analog, tape; stereo, ping-pong, dual; tempo sync, tap tempo, ducking), and reverb (Room, Hall, Plate; freeze). Delay and reverb tails spill over when bypassed.
- **Tempo.** A global tempo, tapped from the GUI or a footswitch CC; the delay, chorus, and reverb pre-delay can sync to it.
- **Presets.** Save and load the whole sound as JSON; loading while playing fades out and back in.
- A basic panel (Amps, Cab, Pre FX, Post FX, Time FX); the real GUI comes last.

Planned: two linked noise gates, boost, overdrive and fuzz, a tuner, Bloom (bitcrush, phaser, flanger), a multivoicer and shimmer reverb, a key-aware 4-voice harmonizer, scenes, MIDI learn, five style presets (Polyphia, CHON, Tech Death, Metal, Midwest Emo), and the real UI.

## Building

You need Apple's command line tools (`xcode-select --install`) and CMake (installed in `~/.local/bin`). Full setup, including the Scarlett Solo gotchas, is in `docs/SETUP.md`.

```
git submodule update --init --recursive      # JUCE and NeuralAmpModelerCore
cmake -B build -DCMAKE_BUILD_TYPE=Release    # configure
cmake --build build -j                       # compile
ctest --test-dir build --output-on-failure   # run the tests
open "build/AmpSim_artefacts/Release/Standalone/Amp Sim.app"
```

## Repo layout

| Path | What it is |
|---|---|
| `src/dsp/` | The DSP: the Block interface, the chain, every amp, cab, filter, and effect |
| `src/PluginProcessor.*` | Parameters, state, file loading, MIDI, and the audio callback, which just drives the chain |
| `src/BlockParameters.*`, `src/Presets.*` | Parameter glue for each effect block, and the preset format |
| `src/PluginEditor.*` | The basic panel |
| `tools/render/` | `ampsim_render`: runs a WAV through the chain offline, with CPU timing |
| `tools/device_probe/` | `ampsim_device_probe`: opens the interface and reports timing and input levels |
| `tests/` | The test suite (`ampsim_tests`), including real-time safety and NAM differential tests |
| `third_party/` | JUCE 8.0.15 and NeuralAmpModelerCore v0.6.0, as pinned git submodules |
| `prototypes/` | Python lab bench. Algorithms get prototyped and listened to here before being ported |
| `docs/BUILD_PLAN.md` | Full feature spec, architecture, build order, and decision log |
| `docs/PROGRESS.md` | Phase-by-phase status |
| `docs/SETUP.md` | Mac + Scarlett Solo setup and troubleshooting |
| `docs/DSP_REFERENCE.md` | Glossary and the sources behind each algorithm |
| `docs/ASSUMPTIONS.md` | Every decision made on Sean's behalf during the unattended build, to review |
| `docs/NAM_UPSTREAM.md` | NAM core findings worth taking upstream |
| `CLAUDE.md` | Context and rules for AI assistants working in this repo |

## Amp models

No `.nam` models are bundled. Download amp-only captures (no cab baked in) from TONE3000, or train your own with NAM's trainer, and keep them in `models/`, which is gitignored. NAM core's example models in `third_party/NeuralAmpModelerCore/example_models/` work for testing.

## Cab IRs

No impulse responses are bundled. Commercial IRs are proprietary, so load your own `.wav` files (free and paid packs are widely available) and keep them in `irs/`, which is gitignored.

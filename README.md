# Amp Sim

A guitar amp simulator and multi-effects processor in C++ on JUCE, with neural amp captures (NAM) running on NeuralAmpModelerCore, the official NAM engine. Built as a personal tool for practicing and writing.

Current target: a standalone macOS app that plays live through a Focusrite Scarlett Solo 4th Gen. VST3/AU plugin export comes later from the same JUCE target.

## Features

Working now: one NAM amp slot (any `.nam` capture, loudness-normalized, crossfaded when you load a new one), a one-mic IR cab (any `.wav` impulse response, zero latency), input and output level, cab bypass, and a basic panel. See `docs/PROGRESS.md` for status.

Planned: three always-running amp slots with five style presets (Polyphia, CHON, Tech Death, Metal, Midwest Emo). Pre FX: two linked gates, compressor, boost, overdrive and fuzz, EQ. A three-mic cab (two close mics plus room) with movable mics. Post FX: EQ, compressor, chorus, delay, reverb with shimmer and freeze. Bloom, a container with bitcrush, phaser, and flanger. A multivoicer, a key-aware 4-voice harmonizer, a tuner, presets with scenes, and MIDI footswitch control.

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
| `src/dsp/` | The DSP: the Block interface, the chain, the NAM amp slot, the cab, gain stages |
| `src/PluginProcessor.*` | Parameters, state, file loading, and the audio callback, which just drives the chain |
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
| `CLAUDE.md` | Context and rules for AI assistants working in this repo |

## Amp models

No `.nam` models are bundled. Download amp-only captures (no cab baked in) from TONE3000, or train your own with NAM's trainer, and keep them in `models/`, which is gitignored. NAM core's example models in `third_party/NeuralAmpModelerCore/example_models/` work for testing.

## Cab IRs

No impulse responses are bundled. Commercial IRs are proprietary, so load your own `.wav` files (free and paid packs are widely available) and keep them in `irs/`, which is gitignored.

# Setup: macOS + Scarlett Solo 4th Gen

## Toolchain

**Command line tools.** Apple's compiler (clang) and linker come with the command line tools. If a build complains about a missing compiler or SDK, run `xcode-select --install`. The full Xcode app isn't needed.

**CMake.** It's installed at `~/.local/opt/cmake-4.4.3-macos-universal`, with `cmake` and `ctest` linked into `~/.local/bin`, which is already on your PATH. CMake doesn't compile anything itself: it reads `CMakeLists.txt` (a description of what to build, a bit like Maven's pom.xml) and generates the real build instructions, which `cmake --build` then runs. To remove it, delete that folder and the two links.

**Submodules.** JUCE and NeuralAmpModelerCore live in `third_party/` as git submodules: separate git repos pinned at exact versions inside this one. After cloning this repo, run `git submodule update --init --recursive` once to download them. `--recursive` matters, because NAM core has its own submodules (Eigen, the linear algebra library).

**Python.** `uv` is installed in `~/.local/bin` with a Python 3.10. Use it for the prototypes (`uv run --with numpy --with scipy python prototypes/amp_sim.py`) rather than Apple's Python 3.9.

## Building

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The first configure also builds `juceaide`, a JUCE helper tool, which takes about 20 seconds. The first full build takes a minute or two, and later builds only recompile what changed. Use Release builds for live audio.

## Interface settings

The Solo shows up in CoreAudio as "Scarlett Solo 4th Gen" with 4 input channels and 2 outputs:

| Channel | Name | What's on it |
|---|---|---|
| 0 | Input 1 | The guitar |
| 1 | Input 2 | The other analog input, unused |
| 2, 3 | Loopback 1, 2 | Whatever your Mac is playing. Never route these to the output: that's a feedback loop |

Turn the INST button on for the guitar input. It switches the input to high impedance so the pickups aren't loaded down; without it the tone gets dull and quiet.

Turn Direct Monitor off. When it's on, the Solo sends your dry guitar straight to the headphones in hardware, and you hear it mixed with the processed signal. It's the number one cause of "my code does nothing" confusion.

Set the input gain while strumming as hard as you ever play. The ring around the gain knob should light green, occasionally amber, never red. Red means the interface is clipping before the software even gets the signal, and no amount of code fixes that.

The Solo should run at 48 kHz (`ampsim_device_probe` reported 48000 Hz on 2026-10-01). The app mutes itself and shows a warning at any other rate.

## Running the app

```
open build/BellyDSP_artefacts/Release/Standalone/BellyDSP.app
```

The app was called Amp Sim until 2026-10-03. The first launch of BellyDSP copies `~/Library/Application Support/AmpSim` (presets, models, irs) and `Amp Sim.settings` (the audio setup) to `.../BellyDSP` and `BellyDSP.settings`, once, and leaves the old ones alone; `BellyDSP/migration-log.txt` lists what it copied. The new bundle ID (`com.seansnaider.bellydsp`) also means macOS asks for the microphone again.

The first launch asks for microphone access, because macOS treats every audio input as a microphone. Allow it. If it was denied, the app runs but hears silence. Fix it in System Settings, Privacy & Security, Microphone.

First-time setup, which the app remembers afterwards:

1. Click **Options** at the top of the window.
2. Audio device: **Scarlett Solo 4th Gen** for both input and output. Sample rate **48000**, buffer size **128** (2.7 ms).
3. Active input channels: **Input 1** only.
4. Untick **Mute audio input** (next to "Feedback Loop:"). JUCE mutes the input by default to protect against feedback (a laptop mic into laptop speakers howls), so until you untick it, the app gets silence. While it's muted, a pale yellow bar across the top of the window says "Audio input is muted to avoid feedback loop", and its **Settings...** button opens the same dialog.
5. Close Options. On the Amp page (the **Amp** block in the signal chain along the bottom), click the amp head's grille (or the model name in the line under the head) and pick an amp-only `.nam` capture for the playing slot; the Glass, Ember, and Monolith tabs are slots 1 to 3. Then click the **Cab** block and pick a cab from the Cabinet list (IR files and cab pack folders in `~/Library/Application Support/BellyDSP/irs`), or use the drop zone's **Browse**, or drop a `.wav` on the page. Play.

Buffer size is how many samples the driver hands over per callback. At 48 kHz, 128 samples is 2.7 ms. Smaller means lower latency but less time to finish processing. Drop to 64 once things are stable if latency bugs you, and go up to 256 if you get crackles.

## Checking the hardware without the app

`build/ampsim_device_probe_artefacts/Release/ampsim_device_probe` opens the Solo through the same JUCE CoreAudio code the app uses, outputs silence for 5 seconds, and prints the callback count and timing plus each input channel's level. Strum while it runs and the guitar's channel lights up.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Silence | "Mute audio input" still ticked in Options, mic permission denied, guitar in the wrong jack, or the wrong input channel enabled |
| Yellow warning about the sample rate | The device isn't at 48000 Hz. Change it in Options (or Audio MIDI Setup) |
| Dry and processed sound mixed together | Direct Monitor is on |
| Crackles and pops | Debug build, buffer too small, or something in `process()` allocating (the real-time safety test should catch that) |
| Distorted even with no amp loaded | Interface input gain too high (red ring) |
| A capture fails to load | The Amp page's line under the head says why (and the pilot light stays dark): not 48 kHz, not a mono model, or not a valid .nam file |

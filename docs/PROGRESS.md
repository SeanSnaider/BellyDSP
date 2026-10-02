# Progress

Status key: Not started, In progress, Done. See `BUILD_PLAN.md` for what each phase includes and its done criteria. Run `ampsim_tests --proof-dir build/proof` for the current measurements behind every "Done".

| Phase | Scope | Status | Notes |
|---|---|---|---|
| 0 | Python prototype: gray-box amp, Yeh tone stack, 3 channels | Done (reference only) | `prototypes/amp_sim.py`. Tone stack verified passive and stable over 2000 random knob settings. Its crude cab filter doubles as the synthetic test IR. |
| 1 | Standalone app on the Scarlett Solo, smoothed gain | Done except the live check | C++/JUCE as of 2026-10-01. The app builds, launches, and quits cleanly. `ampsim_device_probe` opened the Solo at 48 kHz / 128 samples with all 4 inputs and both outputs, and got 1876 callbacks in 5 s (expected 1875) at 2.667 ms ± 0.05 ms. **Not verified yet:** Sean playing live through it at 128 samples with no dropouts. |
| 2 | Offline render harness, test suite, basic panel | Done | `ampsim_render` (WAV in, chain, WAV out, CPU timing), `ampsim_tests` (158 checks, run by `ctest`), and the panel (load model, load IR, two knobs, cab bypass, status lines). |
| 3 | NAM amp slots, three-mic IR cab with movable mics, basic MIDI switching | In progress | Done: one NAM slot and a one-mic cab (see the 2026-10-01 log). To do: three always-running slots with switching, MIDI footswitch slot select, three mics with auto alignment and movable mics, measured loudness normalization. |
| 4 | Compressor (pre and post) and EQ (graphic and parametric) | Not started | |
| 5 | Post FX: delay (MIDI tap tempo), reverb, chorus; basic preset save/load | Not started | Milestone: CHON-style clean is the daily practice rig |
| 6 | Linked dual gates, boost, overdrive | Not started | Milestone: tight tech death with legato surviving the gate |
| 7 | Tuner (YIN or McLeod, chosen empirically), needle and strobe | Not started | Strum tuner is a stretch |
| 8 | Bloom: bitcrush, phaser, flanger | Not started | |
| 9 | Pitch shifter (granular + PSOLA), multivoicer, shimmer reverb | Not started | Pitch shifter is shared with the harmonizer |
| 10 | Harmonizer | Not started | Depends on 7 and 9 |
| 11 | Full preset system, scenes, undo/A-B, real GUI | Not started | UI_DESIGN.md written first |

## Next tasks

| # | Task | Done when |
|---|---|---|
| 1.L | Sean plays through the app live: set Options as in SETUP.md, load a real amp-only capture and a real IR | 128-sample buffer, no dropouts over a few minutes, and the cab bypass toggles without a click |
| 3.1 | Three always-running amp slots plus a slot selector parameter, switched with the 20 ms equal-power crossfade | Tests: all three slots stay warm (switching doesn't restart a model's history), the switch matches the crossfade formula, the real-time safety test covers switching, CPU for 3 × A1 stays under 50% |
| 3.2 | MIDI program change selects the slot (JUCE standalone opens MIDI devices in Options) | A test feeds MIDI messages to the processor and checks the slot; Sean checks it with the footswitch |
| 3.3 | Measured loudness: K-weighted (BS.1770-style) loudness of a reference DI through each model, used when the file has no loudness data, and pink-noise loudness for IRs, replacing unit-energy normalization | Tests against a reference BS.1770 computation; two different IRs land within 1 dB of each other |
| 3.4 | Three-mic cab: two close mics and a room mic with level, pan, polarity, and delay, plus auto phase alignment of close mic 2 | Tests: alignment finds a known offset and polarity, true stereo out |
| 3.5 | Movable mics: cab packs, the position map, minimum-phase spectral morphing | The BUILD_PLAN cab tests (identity at captured points, continuity, no comb notches) |
| 3.6 | Input calibration from model metadata (`input_level_dbu`) | Test with a model that has the field |

## Log

Add a dated line here when something meaningful lands or a decision changes.

2026-09-27: Project started. Python prototype with Yeh tone stack and three channels. Full spec written. Phase 1 skeleton written for standalone on the Scarlet Solo.

2026-10-01: Switched amps to NAM models. Design review rounds 1 to 15 complete (see BUILD_PLAN.md).
2026-10-01: Switched the product from Rust/nih-plug to C++ with JUCE 8.0.15 and NeuralAmpModelerCore v0.6.0 as the amp engine (Sean's criterion: easiest to integrate and fastest). The Rust code stays in the old repo (`~/Desktop/bitchlessDSP/amp_sim_priv`, tags `reference-v0` and `phase-1`). Its Block trait design came across as `ampsim::Block`.
2026-10-01: Built the first C++ milestone and its test suite. 158 checks pass. Measured:
- NAM slot vs. NAM core's own render tool: bit-exact on 8 of the 9 example models; `slimmable_wavenet.nam` differs by -128.6 dB (about one part in a million, same at any block size).
- Every example model loads, including the 0.6/0.7 features (A2 slimmable container, condition DSP). Bad files (missing, invalid JSON, 44.1 kHz) are rejected with a reason.
- CPU at 128 samples, five runs: A1 standard x1 averages 4.2-5.7% of the 2.67 ms deadline, and x3 averages 12.6-15.6%. The worst x3 block was 0.44-0.81 ms in four runs, but one run had a single 3.4 ms block, over the deadline. That ran on a normal-priority test thread while the Mac was busy, and the real audio thread runs at real-time priority, so it's probably scheduling noise. The live test (1.L) settles it. A2 x1 averages 2.3-3.1% and x3 averages 7.0-8.1%.
- Real-time safety: 10 s of audio through the processor, including 2 model switches, an IR swap, cab bypass off and on, and 2 knob ramps, made 0 allocations, 0 frees, and 0 blocking locks on the audio thread. A positive control proves the detector catches all three.
- Loading models on another thread while audio runs: worst audio block 177.5 us (6.7% of deadline).
- Model switch: matches the sin/cos crossfade formula exactly over 960 samples (20 ms).
- Cab: error vs. brute-force convolution -134.8 dB, the same (-134.5 to -135.5 dB) at buffer sizes 1, 7, 64, 128, and 512, zero latency, and an IR swap mid-stream with no click.
- Cab bypass: exact linear 10 ms crossfade, then the block is skipped. Re-enabling clears the stale tail (silence vs. -17.9 dBFS of ringing without the reset).
- State save/restore: knobs, model, and IR all come back, and 2 s of audio through both processors is bit-identical.
2026-10-01: `ampsim_device_probe` on the Solo: JUCE's CoreAudio opened all 4 inputs and 2 outputs on the same device at 48 kHz / 128 with no patch and no deadlock, and callback timing was stable. Input 1 (channel 0) showed a live input's noise floor (-80 dBFS RMS). The loopback channels carried the Mac's own playback (-23 dBFS RMS).

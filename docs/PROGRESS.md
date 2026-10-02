# Progress

Status key: Not started, In progress, Done. See `BUILD_PLAN.md` for what each phase includes and its done criteria. Run `ampsim_tests --proof-dir build/proof` for the current measurements behind every "Done".

| Phase | Scope | Status | Notes |
|---|---|---|---|
| 0 | Python prototype: gray-box amp, Yeh tone stack, 3 channels | Done (reference only) | `prototypes/amp_sim.py`. Tone stack verified passive and stable over 2000 random knob settings. Its crude cab filter doubles as the synthetic test IR. |
| 1 | Standalone app on the Scarlett Solo, smoothed gain | Done except the live check | C++/JUCE as of 2026-10-01. The app builds, launches, and quits cleanly. `ampsim_device_probe` opened the Solo at 48 kHz / 128 samples with all 4 inputs and both outputs, and got 1876 callbacks in 5 s (expected 1875) at 2.667 ms ± 0.05 ms. **Not verified yet:** Sean playing live through it at 128 samples with no dropouts. |
| 2 | Offline render harness, test suite, basic panel | Done | `ampsim_render` (WAV in, chain, WAV out, CPU timing), `ampsim_tests` (158 checks, run by `ctest`), and the panel (load model, load IR, two knobs, cab bypass, status lines). |
| 3 | NAM amp slots, three-mic IR cab with movable mics, basic MIDI switching | In progress (branch `phase-3`) | Done: the SVF filter, three always-running slots with per-slot trims and tone controls, the 20 ms slot switch, MIDI program change slot select, measured loudness matching for models and IRs, the three-mic cab with auto alignment and cuts, and movable mics (cab packs, minimum-phase morphing). To do: input calibration. |
| 4 | Compressor (pre and post) and EQ (graphic and parametric) | Not started | |
| 5 | Post FX: delay (MIDI tap tempo), reverb, chorus; basic preset save/load | Not started | Milestone: CHON-style clean is the daily practice rig |
| 6 | Linked dual gates, boost, overdrive | Not started | Milestone: tight tech death with legato surviving the gate |
| 7 | Tuner (YIN or McLeod, chosen empirically), needle and strobe | Not started | Strum tuner is a stretch |
| 8 | Bloom: bitcrush, phaser, flanger | Not started | |
| 9 | Pitch shifter (granular + PSOLA), multivoicer, shimmer reverb | Not started | Pitch shifter is shared with the harmonizer |
| 10 | Harmonizer | Not started | Depends on 7 and 9 |
| 11 | Full preset system, scenes, undo/A-B, real GUI | Not started | UI_DESIGN.md written first |

## Workflow

Each phase is built at full fidelity on its own branch (`phase-3`, `phase-4`, ...), with the BUILD_PLAN tests and proof, and merges into `main` after Sean has played it. Tag `milestone-1` is the state before Phase 3 continued.

Since 2026-10-01 the build runs to completion without stopping for questions: phase branches stack on each other, tasks that need Sean are listed below and skipped, and every assumption made on his behalf goes in `docs/ASSUMPTIONS.md` for review at the end.

## Next tasks

| # | Task | Done when |
|---|---|---|
| 1.L | Sean plays through the app live: set Options as in SETUP.md, load a real amp-only capture and a real IR | 128-sample buffer, no dropouts over a few minutes, and the cab bypass toggles without a click |
| 3.2.L | Sean checks slot switching with the footswitch (program change 1/2/3) and plays all three slots | Switches are instant and clickless, and the tone knobs do what their names say |
| 3.3.Q | Sean decides: keep per-IR loudness matching (cab swaps stay within about 2 LU), or normalize each amp-plus-cab combination (exact on the reference DI, but cabs stop changing loudness at all) | A decision in the BUILD_PLAN log |
| 3.4.L | Sean loads real IRs into the three mics and plays: checks auto alignment by ear (try toggling it), pan width, room level and pre-delay, and the cuts | Sounds right, and nothing clicks when moving controls while playing |
| 3.5.L | Sean loads a real cab pack (a folder of IRs of one cab at several mic positions) into a close mic and drags the mic while playing | Moving the mic sounds like moving a mic, with no hollow or phasey spots between captures and no clicks |
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
2026-10-01: Correction: NAM core's `example_audio/input.wav`, used as "the example input" above, is 1 s of digital silence followed by 1 s of a steady test tone, not a guitar DI. The bit-exactness and real-time results hold, but the CPU numbers above came from a half-silent signal and understate real use (see below). Tests now use a synthetic Karplus-Strong guitar DI (`testing::guitarDI`), and the NAM comparison also uses a broadband stimulus (guitar DI, 20 Hz to 20 kHz sweep, noise).
2026-10-01: Phase 3 so far (branch `phase-3`, 235 checks pass):
- SVF filter (TPT, after Simper): matches its analog prototype at the prewarped frequency to -232 to -297 dB across 10 configurations; design targets exact (a +9 dB bell measures 9.000000 dB at fc); bit-transparent at 0 dB; bounded under a 20 Hz full-range sweep (a direct-form biquad was equally bounded in that test); 32-sample coefficient updates are -61.9 dB from per-sample ones.
- Three always-running amp slots: after a switch, the new slot matches a model that ran the whole time bit-exactly, while a model started at the switch would be off by -40.7 dB for 85 ms. The switch follows the sin/cos formula to 1.1e-6 over 20 ms, and a second switch mid-fade redirects without a click and settles bit-exactly.
- Tone controls (Depth 90 Hz bell, Bass 180 Hz shelf, Mid 800 Hz bell, Treble 2.8 kHz shelf, Presence 5 kHz bell, each +-12 dB): every band lands exactly on target, flat is bit-transparent, and a 0 to +12 dB ramp is -51.5 dB from an ideal per-sample filter.
- MIDI program change 1/2/3 switches the slot in the same audio block; the slot knob catches up within 50 ms. State keeps per-slot captures and the active slot, which starts without a fade, and milestone 1's single "modelPath" migrates to slot 1.
- CPU on the guitar DI, as the app runs it: three A1 standard slots with tone controls average 17.3% of the 2.67 ms deadline (p99 0.53 ms, worst 0.58-0.71 ms). Flushing denormals makes no difference; the guitar signal itself costs more than the half-silent clip did. A2 x3 averages 7.0%.
- Real-time safety now also covers slot switches from the GUI and the footswitch, a capture loading into a slot that isn't playing, and tone knob ramps: still 0 allocations, 0 frees, 0 blocking locks.
2026-10-01: `ampsim_device_probe` on the Solo: JUCE's CoreAudio opened all 4 inputs and 2 outputs on the same device at 48 kHz / 128 with no patch and no deadlock, and callback timing was stable. Input 1 (channel 0) showed a live input's noise floor (-80 dBFS RMS). The loopback channels carried the Mac's own playback (-23 dBFS RMS).
2026-10-01: Phase 3, measured loudness (252 checks pass):
- BS.1770-4 meter: K-weighting coefficients equal the standard's published 48 kHz values exactly; EBU Tech 3341 cases 1 to 5 read -22.99, -32.99, -23.01, -23.01, -22.98 LUFS (spec -23/-33/-23/-23/-23, tolerance 0.1); mono reads 3 LU below stereo as specified.
- Models: every capture is normalized to -18 LUFS measured on the reference guitar DI (preferred over the file's loudness field, per the plan). The four example models land at exactly -18.00 LUFS, and on a different take of the riff they span 0.45 LU. Their files' own loudness fields disagree with each other's scale (lstm says -37.8 dB but measures -44.9 LUFS; A2 says -26.4 but measures -20.3), which is why measuring matters.
- IRs: the plan's pink-noise matching left distorted guitar spanning 6.1 LU across stock, dark, and bright cabs, worse than milestone 1's unit energy. A study of 10 reference signals found white noise has the best worst case (2.11 LU across clean and distorted guitar; clean tones prefer about -1.5 dB/oct, distorted about +1 dB/oct). IRs are now matched with K-weighted white noise: the three cabs stay within 1.18 LU on distorted guitar and 1.94 LU on clean.
2026-10-01: Phase 3, three-mic cab (303 checks pass):
- Two close mics (level, constant-power pan, polarity, whole-sample delay, mute, left or right channel of a stereo file) and a true-stereo room mic (level, pre-delay, mute), then 12 or 24 dB/oct Butterworth low and high cuts. The full mix matches a brute-force double-precision computation to -133 dB at buffer sizes 1, 7, 64, 128, and 512, with zero latency.
- Auto alignment recovers a planted 23-sample offset with inverted polarity exactly (match 1.0000), delays whichever mic is early, and on two differently coloured mics turns a -31.2 dB comb notch into at most a -2.8 dB dip.
- Pan gains are exactly sqrt(2) cos/sin of the pan angle (unity at centre, +3 dB hard-panned, L^2 + R^2 = 2 everywhere); delay, polarity, mute, and cut on/off changes crossfade with no step larger than steady playing; the room's left and right IRs and its pre-delay land on the exact sample.
- Cuts hit their Butterworth targets to 0.01 dB (-3.010 dB at fc; -12.30/-24.10 dB an octave below a low cut; -14.33/-28.34 dB an octave above a 5 kHz high cut, the bilinear-warped targets).
- CPU: two 500 ms close mics plus a 1 s stereo room with both cuts on cost 1.2% of the deadline (the room uses JUCE's head/tail partitioning).
- Bug found by mutation testing: an IR loaded into an empty mic during playback never took effect, because the cab skipped mics without IRs, and a mic only installs a waiting IR while being processed. The first version of the leak test passed for that reason. Fixed (every mic polls for a waiting IR each buffer), and the test now compares block by block against a single-mic cab: until mic 2's engine is ready plus a 60 ms settle, the output is identical to mic 1 alone; afterwards it's exactly 1.5012 x (mic 2 at -6 dB, coherent). With the fade-in logic deliberately disabled, the test fails with a 0.33 error, so it does catch a leak.
2026-10-01: Phase 3, movable mics (409 checks pass):
- Cab packs: a folder of IRs placed on a position map by `cabpack.json`, by file names (Cap, CapEdge, Cone, Edge; 1in, 2.5cm, 10mm), or in file order as a fallback the status line admits to. Bilinear weights on a grid, linear on a line, inverse distance over the nearest four for scattered points, all exact in tests.
- Minimum-phase morphing: magnitude kept to 0.0009 dB from 30 Hz to 16 kHz, partial energy dominates the original's at every sample, and the analytic case 0.4 + z^-1 becomes exactly 1 + 0.4 z^-1. Arrival times are put back to -286 dB for whole samples and 0.0003 samples for fractions.
- On a captured point the mic uses the capture itself, bit-exact. Moving between two captures that differ by 15 dB changes the response by at most 0.150 dB per 1% of the way (exactly linear in dB), and halfway it's their dB average to within 0.0001 dB.
- Comb test: halfway between two captures 12 samples apart, a naive crossfade notches 46 dB at 2.2 kHz; the morph stays within 0.0001 dB of the target (plot: `build/proof/cab_morph_vs_crossfade.png`).
- Dragging: the loader re-morphs at most every 40 ms and always for the latest position. One second of dragging gave 25 morphs exactly 40.0 ms apart and ended on the final position; with the limit deliberately removed, the test fails with 336 morphs 2.2 ms apart. A move costs the loader 1.2 ms for 85 ms IRs.
- Two fixes found by the new tests: the morph's minimum-phase step left a -60 dB error from IRs' exact zeros at DC and Nyquist (now -77 dB, with a double-precision FFT and neighbour values at those two bins), and IR loudness matching took 70 ms per move (now computed exactly from the IR's K-weighted energy in 0.27 ms, agreeing with the 4 s noise measurement within 0.034 dB).
- Real-time safety now includes a pack loading into close mic 2 and 12 re-morphs while it's dragged: still 0 allocations, 0 frees, 0 blocking locks. Saving and restoring brings the pack and the mic position back, bit-identical over 1 s.


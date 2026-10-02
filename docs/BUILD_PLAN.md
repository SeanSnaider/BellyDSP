# Amp Sim Build Plan

A Neural DSP style guitar rig in C++ on JUCE: three always-running neural amp slots (NAM models on NeuralAmpModelerCore, the official engine), pre FX (two gates, compressor, boost, overdrive, EQ), a three-mic cab with movable mics, post FX (EQ, compressor, chorus, delay, reverb), a Bloom modulation container, a multivoicer, a key-aware harmonizer, a tuner, presets with scenes, and MIDI footswitch control. Standalone on macOS first; VST3/AU export later from the same JUCE target.

## Architecture

### Code layout

One CMake project. `src/dsp/` holds every amp, filter, and effect, the Block interface, and the chain. Blocks may use JUCE's DSP and audio-buffer modules but never processor or GUI types, so the offline renderer and the tests drive them directly. `src/PluginProcessor.*` is the thin glue: parameters, state, file loading, and an audio callback that only drives the chain. `tools/` holds the offline renderer (`ampsim_render`) and the hardware probe (`ampsim_device_probe`), and `tests/` holds the test suite (`ampsim_tests`). JUCE (audio I/O, the standalone wrapper, GUI, convolution, file formats) and NeuralAmpModelerCore (amp inference) are pinned git submodules in `third_party/`. This replaces the Rust workspace split (see the decision log, 2026-10-01).

### Two languages, two jobs

The product is C++ on JUCE. Python (`prototypes/amp_sim.py` and friends) stays as the lab bench: prototype a DSP algorithm offline, plot its frequency response, listen to renders, then port it. Every block gets checked against a trusted reference with golden tests: the Python prototype, a brute-force computation (the cab against direct convolution), or an official implementation (the NAM slot against NAM core's own render tool). That catches porting bugs without relying on your ears.

### Block design

Every effect, amp, and cab implements one interface, `ampsim::Block` (an abstract class, C++'s version of a Java interface): `prepare(sampleRate, maxBlockSize)` allocates everything up front, `process(block, context)` does the audio work, `reset()` clears internal state without allocating, `latencySamples()` reports added delay, and `isStereo()` says whether the block is mono or stereo. Six decisions, worked out in the Rust version (Phase 2.2) and carried over:

1. **Worst-case allocation in prepare().** A delay line is sized for the knob's maximum time, an oversampling buffer for `factor x maxBlockSize`. process() only ever uses part of it. Terminology: a block is a DSP unit, a buffer is the chunk of samples handed to process().
2. **The DI is a snapshot plus a read-only view.** Before any block runs, the chain copies channel 0 (the guitar) into a preallocated buffer and passes `BlockContext { const float* di; int numSamples; }` to every block. Blocks that need the clean guitar (gates, tuner, harmonizer) read it, and the rest ignore it, so the chain never special-cases anyone. In-place processing overwrites the input, which is why it has to be a copy. `const` makes it impossible to modify.
3. **Mono, then stereo, with one copy made by the chain.** Mono blocks touch channel 0 only. Right before the first stereo block, the chain copies channel 0 to channel 1, once per buffer, or at the end if no block is stereo. No mono block may come after a stereo one (a debug assertion in prepare()).
4. **The chain owns bypass and crossfades it** over 10 ms. The fade is linear, because dry and wet (the same signal with and without one block) are strongly correlated. A fully bypassed block isn't called at all. Re-enabling a fully bypassed block calls its reset() first, to clear stale state. Reversing mid-fade doesn't, because the block never stopped running.
5. **Blocks own their smoothers and never see the processor's parameters.** The processor reads the knobs once per buffer and calls block setters (`inputGain.setGainDecibels()`), so the renderer and the tests drive blocks with exact values.
6. **The chain holds typed members in signal-chain order**, plus `inOrder()`, which returns them as `Block*` for the generic logic (the snapshot, the stereo copy, bypass). Typed members let the processor call block-specific setters. Runtime reordering (Foundation decisions) will become a per-section index array over these preallocated members.

### Real-time rules (non-negotiable)

The audio callback hands you a buffer of 32 to 512 samples and expects it back within a few milliseconds. Missing that deadline produces an audible click called a dropout. So inside `process()` there is no memory allocation, no locks, no file or network I/O, and no printing. Anything slow (loading an IR file, rebuilding an FFT plan) happens on a background thread, and the finished result is swapped into the audio thread through a lock-free handoff.

Every knob gets parameter smoothing (ramping from the old value to the new one over ~20 ms), otherwise turning a knob produces "zipper noise." Blocks use `juce::SmoothedValue` for this. Filters whose coefficients depend on knobs (the tone stack, EQ bands) recompute coefficients from the smoothed value, not the raw one.

### Default signal chain

```
Input (mono)
 ├─► Tuner (analysis tap, can mute output)
 ├─► DI tap ──► detection for both gates and the harmonizer
 ▼
PRE FX (mono, reorderable):
  Gate A ─► Compressor ─► Boost ─► Overdrive ─► EQ (pre instance)
 ▼
AMP (fixed): three NAM slots, all running, crossfaded on switch
 ▼
Gate B (fixed, linked to Gate A by default)
 ▼
CAB (fixed, mono ─► stereo): close mic 1 + close mic 2 + room mic
 ▼
POST FX (stereo, reorderable):
  EQ (post instance) ─► Compressor (post instance, off) ─► Harmonizer ─► Multivoicer
  ─► Bloom (bitcrush, phaser, flanger) ─► Chorus ─► Delay ─► Reverb
 ▼
Output (stereo)
```

Two deliberate choices here. Pitch detection for the harmonizer always reads the clean DI, because tracking pitch on a distorted signal is dramatically less reliable. And harmonies are generated after the amp and cab rather than before, because pushing two pitch-shifted notes through a high-gain amp together creates intermodulation distortion, which sounds like mud. A pre-amp harmonizer placement can be added later as a toggle for the gritty version.

## Foundation decisions

These apply to every block and were settled in the foundation design review.

**Channel layout.** Mono from input through the pre-FX section, the amp, and Gate B. The chain goes stereo at the cab (the two mic slots can be panned) and stays stereo through every post-cab block.

**Reorderable chain.** The chain has four sections. Pre FX (Gate A, the pre compressor, boost, overdrive, and the pre EQ) are freely reorderable among themselves. The amp is fixed. The cab is fixed and is the mono-to-stereo transition point. Gate B is fixed between the amp and the cab. Post FX (the post EQ, the post compressor, harmonizer, multivoicer, Bloom, chorus, delay, reverb) are freely reorderable among themselves. Blocks can't cross sections, because the sections differ in channel count and because moving the amp or cab produces nonsense. Inside Bloom, bitcrush, phaser, and flanger are reorderable too. Reordering on the audio thread is a swap of indices into preallocated blocks, never an allocation, and it crossfades to avoid clicks. The order is saved in presets.

**Sample rate.** Fixed internal rate of 48 kHz, because NAM models are trained at 48k. v1 refuses to start at any other rate with a clear error message. Resampling support is deferred to DAW-plugin work.

**Buffer size.** Target 128 samples (2.67 ms at 48 kHz). Every block must handle any buffer size from 1 up to the max. Blocks that need fixed-size chunks buffer internally and report the added latency.

**Oversampling.** Per block, owned by each block internally. There is no global oversampling setting: NAM runs at native rate, clipping blocks (boost, overdrive) oversample, and the bitcrusher deliberately doesn't.

**Parameters.** JUCE parameters (an AudioProcessorValueTreeState) live in the processor, which sets block targets once per buffer. Smoothing is per sample inside the blocks; filter coefficients are recomputed from smoothed values every 32 samples.

**Threading and loading.** Three threads: audio, GUI, and a background loader (a one-thread JUCE ThreadPool). The loader builds a complete ready-to-run object (a NAM model, an IR) and sends it to the audio thread through `ampsim::Handoff`, an atomic-pointer mailbox in each direction. The audio thread swaps it in with a crossfade and sends the old object back to the loader to be dropped there, because dropping frees memory and freeing is as forbidden on the audio thread as allocating.

**Seamless amp switching.** All three amp slots run continuously, fed the same input, so every model's internal history (its ring buffers) is always current. Switching is an equal-power crossfade of about 20 ms between slot outputs. This triples NAM CPU cost, which is the price of seamless switching. Measured 2026-10-01 at 128 samples on a synthetic guitar DI: three A1 standard slots with tone controls average 17.3% of the deadline (p99 0.53 ms), and three A2 slots 7.0%. Earlier figures (12.6-15.6%) came from a half-silent test clip and understated it. The CPU fallback order below isn't needed at these numbers. The switch itself is built from independent linear ramps: each slot's position p ramps toward 1 (selected) or 0, and the slot contributes sin(pi/2 p) of its output, which is the equal-power sin/cos pair for a plain switch and redirects smoothly if you switch again mid-fade.

**CPU fallback order** (decided in advance, applied in this order only as far as needed):
1. Smaller models in inactive slots only. Each inactive slot runs a lite stand-in of its capture. On switch, the output crossfades to the warm stand-in immediately (seamless), the full model starts processing live input, and once its receptive field has filled (tens of milliseconds) the output crossfades from stand-in to full model. Stand-ins come from the capture author when they publish multiple sizes; otherwise they're distilled offline with NAM's trainer by rendering audio through the full model and training a lite model on it. This is real extra work and is built only if the benchmark demands it.
2. A brief switch blip, offered as a setting.
3. Raising the buffer to 256 samples.

**Bypass and spillover.** Every bypass crossfades over about 10 ms. Time-based effects (delay, reverb, and anything with a tail) support spillover: when bypassed they stop taking new input but let the existing tail ring out.

**Precision.** Audio buffers are f32. IIR filter state and coefficient math are f64, because low-frequency filters in f32 produce audible noise and drift.

**Denormals.** Flush-to-zero is enabled for every callback with `juce::ScopedNoDenormals`. Test that a decaying reverb tail reaches silence without a CPU spike (Phase 5).

**CPU budget.** The whole chain uses under 50% of the 2.67 ms callback deadline on Sean's Mac. A benchmark harness exists from Phase 2 so this is measured, not guessed.

**Real-time enforcement.** The "Real-time safety" test runs the whole processor through 10 s of audio while models switch, IRs swap, bypass toggles, and knobs move. It counts heap allocations and frees (through macOS's malloc_logger hook, which sees every library) and blocking mutex locks (code compiled into the binary) on the audio thread. The count must stay at zero, and a positive control proves the detector catches all three. Apple's clang has no RealtimeSanitizer, which would otherwise be the tool.

**Audio to GUI.** Meters and the tuner readout go from the audio thread to the GUI through atomics, never locks.

**Latency.** The whole chain adds zero latency beyond the buffer. The single exception is the flanger's opt-in through-zero mode (about 5 ms), reported to the host and paid only while active. The harmonizer is not an exception: its voices trail the dry signal by a few milliseconds, but the dry signal is never delayed and the block reports zero latency (design review round 14).

**Hands-free control.** A USB MIDI footswitch controls amp switching and block toggles while playing. See MIDI control in the feature spec.

## Feature spec

### Amps

The amps are Neural Amp Modeler (NAM) models: trained neural networks (usually a WaveNet-style dilated convolutional network) that map input guitar audio to an amp's output, sample by sample. Models are standard `.nam` files, which are JSON containing the architecture, its configuration, and the trained weights. Captures come from the community (TONE3000 is the main hub) or are trained with NAM's Python trainer.

Inference runs on NeuralAmpModelerCore, the official C++ engine, linked directly and pinned at v0.6.0. Sean's NAM learning and upstream contributions go through reading, profiling, and patching that code (as PRs upstream). Writing his own WaveNet, checked against it, is an optional side track.

| Amp slot | Character | Model choice |
|---|---|---|
| Clean | Full-range, sparkly, edge of breakup when pushed | A clean amp capture |
| Crunch | Classic rock to modern rock rhythm | A crunch capture |
| Tech Death | Very tight, fast attack, huge gain with no low-end flub | A modern high-gain capture, ideally with a tight boost already in front of it at capture time or supplied by our Boost block |

Each slot loads any `.nam` file, so the three slots are defaults, not hardcoded amps.

**Slots vs presets.** Slots are what switches seamlessly mid-song; there are three, all running. Presets are what loads between songs: a preset chooses the capture in each slot plus every other setting, and loading one is a short fade, not seamless. Five style presets ship as starting points, each mapping Sean's styles onto the three slot types (clean, crunch, high gain):

| Preset | Slot 1 | Slot 2 | Slot 3 | Character notes |
|---|---|---|---|---|
| Polyphia | Bright modern clean | Crunch | High gain | Heavy pre compression, chorused delay, ducking |
| CHON | Bright modern clean | Edge-of-breakup | Crunch | Compressed sparkle, dotted-eighth and dual delays, chorus |
| Tech Death | Tight high gain | High gain lead | Clean | Boost into the amp, linked gates with adaptive release |
| Metal | High gain | Crunch | Clean | Boost, gates, less extreme than Tech Death |
| Midwest Emo | Vintage American clean (warmer, chimey) | Crunch | Edge-of-breakup | Light compression, more room mic, analog or tape delay |

Specific captures are chosen by Sean by ear; the table defines roles, not models.

Use amp-only captures (amp into a load box, no cab or mic), not full-rig captures. Full-rig captures bake in a cab, which would double up with our cab section.

Controls per amp: Input gain, Output gain, plus Bass/Mid/Treble/Presence/Depth implemented as EQ around the model. Built as five SVF bands after the model, each +-12 dB: Depth a bell at 90 Hz (Q 1.2), Bass a low shelf at 180 Hz, Mid a bell at 800 Hz (Q 0.7), Treble a high shelf at 2.8 kHz, Presence a bell at 5 kHz (Q 0.8). Starting points, to be tuned by ear. A capture is a snapshot of one amp at one knob setting, so "amp knobs" here are post-model shaping, not the real amp's interacting tone stack. Knob-conditioned models (amp knob values fed to the network as extra inputs) are a possible later upgrade.

Models are trained at 48 kHz. The standalone runs at 48 kHz; any other host rate needs resampling around the model.

Real-time constraints: NAM core's process() doesn't allocate after Reset(). Its own tests check that, and ours confirm it inside the full chain. Loading, Reset() (buffer sizing), and prewarming happen on the loader thread, and then the model swaps in lock-free.

Correctness is tested differentially: our slot against NAM core's own render tool (built unchanged from its source) on every example model, within -100 dB. It's currently bit-exact on 8 of 9. The PyTorch trainer is still worth adding as a third oracle, because a disagreement between the trainer and the core is a finding worth reporting upstream.

The gray-box amp in `prototypes/amp_sim.py` (Yeh tone stack, oversampled waveshaping) stays as a learning reference and is not ported.

#### NAM engine (design review round 2, revised 2026-10-01)

The original round 2 plan was a hand-written Rust engine: channel-major, time-contiguous activations auto-vectorized to NEON, a hand-written loader asserting weight consumption, and Sean writing the dilated Conv1D, Layer, and LayerArray. The switch to C++ with NAM core as the engine superseded it (decision log). What stands now:

**Format scope.** Whatever NAM core supports: `.nam` versions 0.5.0 to 0.7.0, WaveNet A1 (all sizes) and A2, LSTM, ConvNet, Linear, Sequential, slimmable containers, FiLM, and condition DSP. All nine of NAM core's example models load. An amp slot also requires a mono (1-in, 1-out) model trained at 48 kHz, or one that supports arbitrary rates. Anything else is rejected at load with a specific error, never loaded wrong.

**Loader.** `nam::get_dsp()` on the loader thread, then `Reset(48000, maxBlockSize)`, which sizes the model's buffers and prewarms it (runs silence through until its receptive field is settled).

**Input level.** Per-slot input trim (the input gain knob for now). Input calibration from model metadata (`input_level_dbu`) is still to do.

**Loudness matching.** Built as planned: at load, 4 s of the reference guitar DI is rendered through the model, its BS.1770 integrated loudness measured, and the slot normalized to -18 LUFS, for every model, preferred over the file's own loudness field. The per-slot output trim is the manual adjustment. The reference DI is a deterministic Karplus-Strong riff (`ampsim::referenceGuitarDI`) until Sean records a real one.

**Differential testing.** See above. Synthetic random-weight models from the trainer, as a trainer-vs-core fuzzer, are still a good upstream contribution.

**CPU.** Measured (see "Seamless amp switching").

**Ownership.** Claude writes the integration. Sean's hand-written WaveNet is an optional side track (see CLAUDE.md).

### Pre FX

| Block | Controls | Notes |
|---|---|---|
| Gates (two) | See Gates below | Two independent gates, one pre-amp and one post-amp. |
| Compressor | See Compressor below | Placeable in pre-FX and post-FX. |
| Boost | See Boost and Overdrive below | Shares one drive engine. |
| Overdrive | See Boost and Overdrive below | Shares one drive engine. |

### Boost and Overdrive

Design settled in design review round 10.

**Shared drive engine.** A metal-style boost is an overdrive circuit at near-zero gain, so boost and overdrive share one drive engine.

**Boost modes.**
- **Clean.** Flat gain with optional tilt EQ; the lead boost.
- **Tight.** Linear high-pass plus mid push, no clipping.
- **Screamer.** The drive engine's Mid Drive circuit at minimal drive: the classic metal boost, with slight compression.

Controls: Level, plus Tight frequency and mid amount where the mode uses them.

**Modeling approach.** Physically informed, after David Yeh's work on distortion pedals: each circuit is decomposed into linear filter, static nonlinearity derived from the real diode or transistor physics, linear filter. Full per-sample circuit simulation (wave digital filters or Newton-Raphson) is a stretch goal.

**Overdrive modes** (generic UI names; original circuits referenced in docs only).
- **Mid Drive** (Tube Screamer-style): diodes in the op-amp feedback loop, soft clipping, mid hump.
- **Transparent** (Klon-style): clean and clipped signals blended.
- **Distortion** (RAT-style): hard-clipping diodes to ground plus op-amp slew limiting at high gain.
- **Fuzz** (Big Muff-style): cascaded transistor clipping stages with a tone control between them, each stage decomposed the same way. Built last in Phase 6, after the other three prove out the drive engine. Fuzz Face-style circuits are deliberately excluded: their character depends on interaction with the guitar pickup's impedance, which is gone once the interface has buffered and digitized the signal.

**Controls.** Drive, Tone, Level, Mix (parallel blend in every mode), Tight pre-high-pass, mode.

**Oversampling.** 4x default, 8x optional, using polyphase IIR halfband filters (in the style of Laurent de Soras' HIIR) rather than linear-phase FIR, because FIR halfbands add fixed latency and the foundation rules require zero. DC blocker after every asymmetric clipping stage.

**Validation.** Reference renders from ngspice (a free command-line circuit simulator) running the actual schematics, compared against the models.

**Testing.**
- **Linear behavior.** At very low levels the frequency response matches the circuit's analytic response.
- **Aliasing.** A high-frequency sine sweep at full drive keeps inharmonic energy below a set limit at 4x.
- **DC.** No DC offset at the output.
- **Transparency.** Boost Clean at unity gain is bit-transparent.
- **Circuit match.** Models track ngspice renders within a set tolerance.

### Gates

Design settled in design review round 4. Sean plays a lot of legato, tapping, and sweeps in heavy parts, so the defaults are tuned for quiet notes surviving, not just tight stops.

**Structure.** Two gates, each with its own detector, knobs, Learn button, and meters, linked by default. When linked, Gate B follows Gate A's detector and gain envelope (one gating decision applied at two points), which prevents two independent releases from compounding and chopping note tails, or stuttering when thresholds differ. Unlinking makes them fully independent. Gate A sits in the pre-FX section (reorderable like any pre-FX block) and cleans up noise going into the amp. Gate B is a fixed stage between the amp and the cab and kills the amp's amplified hiss.

**Detection.** Each gate has a detector source setting, defaulting to the clean DI for both, because the DI separates picking from the noise floor far better than an amplified signal. Gate A can also detect from its own input. Each detector has a sidechain high-pass (default about 100 Hz) so 60 Hz mains hum doesn't hold it open; low notes, even in drop A, still trigger through their harmonics. Peak-based detection, so pick attacks register instantly.

**Envelope.** Attack about 0.5 ms (no lookahead, zero latency). Hold to bridge gaps between tremolo-picked notes. Hysteresis: separate open and close thresholds to prevent chatter, with a wider default gap than a typical metal gate (around 8 dB) so quiet legato and tapped notes don't close it. Release exponential in dB. Range (closed attenuation) defaults to full mute.

**Release modes.** Adaptive release by default, chosen because of Sean's legato and tapping. Classic fixed release is an option. Adaptive: a fast and a slow envelope follower are compared, so a sudden drop (a deliberate stop) closes quickly while a slow decay (a ringing or legato note) closes slowly.

**Learn.** With strings muted, Learn measures the noise floor for 2 seconds and sets the threshold a few dB above it.

**Metering.** Detector level against both thresholds, plus live gain reduction, via atomics to the GUI.

**Out of scope for now.** Hum removal while playing (a comb of notch filters at 60 Hz harmonics), since a gate can only act in gaps. Easy to add later if needed.

**Testing.**
- **Attenuation.** Note bursts over a noise floor, checking closed sections reach the target range.
- **Opening speed.** Opens within 1 ms of an onset.
- **Chatter.** A decaying sine hovering at threshold, counting transitions to prove hysteresis works.
- **Clicks.** High-frequency energy at each open and close stays below a limit.
- **Adaptive release.** A sharp stop closes faster than a natural decay.
- **Legato.** A sequence of loud picked notes followed by notes 15 to 20 dB quieter (simulating hammer-ons and taps) must stay open at the default settings after Learn.

### Compressor

Design settled in design review round 5. Sean's main playing is CHON and Polyphia style (clean and edge-of-breakup tones, tapping, hybrid picking, fast legato), so defaults target an even, sparkly, compressed clean.

**Placement.** One block type, available as one instance in pre-FX (mono, pedal-style use before the amp) and one in post-FX (stereo, glue on the finished tone). Both reorderable within their section. The post instance is bypassed by default.

**Modes.**
- **Studio.** Feed-forward, log-domain gain computer with smooth peak detection and a soft knee, following Giannoulis, Massberg and Reiss (JAES 2012).
- **Pedal.** Feedback topology (the detector reads the comp's own output) with a fast fixed attack and heavy squash, in the style of classic OTA pedal compressors.

**Detector.** Peak or RMS, selectable. Sidechain high-pass so low notes don't pump the whole sound. Reads the compressor's own input, not the DI. In post-FX, stereo-linked (max of left and right) so the image doesn't shift.

**Controls.** Threshold, ratio, knee width, attack, release (with an auto, program-dependent mode), makeup gain (with auto-makeup), and mix for parallel compression.

**Default (pre instance).** Studio mode, about 4:1 ratio, 6 dB soft knee, attack around 8 ms so the pluck of taps and picked notes survives, auto release, mix around 70% to keep transients, sidechain high-pass at 100 Hz. These are starting points to be tuned by ear on real CHON-style playing.

**No lookahead.** Zero latency per foundation rules.

**Gate interaction.** Because the gates detect from the clean DI, a compressor lifting the noise floor doesn't fool them. Default pre-FX order: Gate A, compressor, boost, overdrive.

**Metering.** Gain reduction, input, and output levels via atomics.

**Testing.**
- **Static curve.** Stepped sine levels compared against the theoretical threshold, ratio, and knee curve.
- **Knee continuity.** No jump or kink at the knee edges.
- **Time constants.** Measured attack and release match the knobs.
- **Mix and makeup.** Parallel blend and auto-makeup land at expected levels.
- **Stereo linking.** Signal on one channel compresses both equally.
- **Pedal mode.** Matches a Python reference simulation of the feedback loop.

### EQ

Design settled in design review round 6.

**Placement.** One block type, one instance in pre-FX and one in post-FX. Only these two placements are distinct: the EQ and the cab are both linear time-invariant filters, which commute, so an EQ between the amp and the cab would sound identical to the same EQ at the start of post-FX. Before the amp is different, because the amp is nonlinear: cutting lows pre-amp tightens the distortion itself.

**Modes.** Each instance switches between Graphic and Parametric.

**Filters.** Trapezoidal (TPT) state-variable filters in the style of Andrew Simper's Cytomic designs, with f64 state. Chosen over RBJ biquads because they stay stable and click-free under fast parameter changes (relevant if an expression pedal ever drives a band) and one structure provides every response type. Known trade-off: response shapes compress slightly near Nyquist (above roughly 8 to 10 kHz at 48 kHz). Matched-response designs (Vicanek) would fix that, but guitar content after a cab is minimal up there, so it was deliberately skipped.

**Graphic mode.** 9 bands at octave centers (63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz), ±12 dB, plus high-pass and low-pass. Neighboring bands overlap, so naive designs show slider positions that don't match the real response. The fix follows Liski and Välimäki's accurate cascade graphic equalizer: a precomputed 9x9 band interaction matrix is used to solve for internal band gains that make the actual response hit the slider positions. Applied as a 9x9 matrix-vector multiply when sliders move.

**Parametric mode.** 5 bands, band 1 defaulting to low shelf and band 5 to high shelf with peaks between; each band's type is switchable (peak, shelf, notch), with frequency, gain, and Q. High-pass and low-pass at 12, 24, or 48 dB per octave (cascaded Butterworth sections).

**Smoothing.** Per-sample parameter smoothing, coefficients recomputed every 32 samples, per the foundation rules.

**GUI (Phase 11).** Neural DSP-style visual EQ: a draggable response curve with a live spectrum analyzer behind it. The audio thread copies output samples into a lock-free ring buffer; the GUI thread runs the FFT and draws. The analyzer never runs FFTs on the audio thread.

**Testing.**
- **Magnitude response.** Measured against the analytic target at many frequencies for every band type.
- **Graphic accuracy.** Random slider settings land within ±1 dB of the sliders at every band center.
- **Fast-sweep stability.** Rapid frequency sweeps under noise don't blow up or click.
- **Zipper noise.** Gain changes produce no audible stepping.

### Cab

The cab section is an IR system, not a set of bundled cabs. Neural DSP's own IRs are proprietary, so the plugin loads user-provided IR `.wav` files. Design settled in design review round 3.

**Mic slots.** Three: two close mics and a room mic.

Each close mic has: IR source (a position on the cab pack's position map, or a single file), level, pan, polarity invert, delay in whole samples (one sample at 48 kHz is about 7 mm of mic distance), and mute. There's no separate blend knob, because two level knobs already do that.

The room mic has: IR file (mono, or true stereo where the IR's left and right channels feed the left and right outputs), level, pre-delay, and mute. The room mic is deliberately not phase-aligned to the close mics, since its time offset is what creates depth.

Global after the mics: low cut and high cut (12 or 24 dB per octave) and a cab bypass for full-rig captures or a real cab/FRFR.

**Auto phase alignment.** On by default for the two close mics, with manual override of delay and polarity. Built: the cross-correlation peak over +-200 samples of the IRs' first 8192 samples gives the offset and polarity; whichever mic arrives first is delayed (the plan said mic 2, but mic 2 can be the early one), and mic 2 is flipped when the peak is negative. With auto on, the computed delays and polarity replace the manual ones; turning it off restores the manual settings. At load (and after each mic move settles), the loader cross-correlates close mic 2's IR against close mic 1's, finds the time offset and polarity of best alignment, and sets mic 2's delay and polarity. The room mic is excluded.

**Movable mics (cab packs and the position map).** A cab pack is a folder of IRs from one cab plus a small manifest file mapping each IR to a point on a 2D map: horizontal axis is position across the speaker (center of the cone to the edge), vertical axis is distance from the grille. The loader tries to build the manifest automatically from common file-naming patterns; otherwise IRs are placed by hand. Adding IRs means dropping a file into the pack and placing it on the map, so packs grow over time. Packs with only one axis (a list of positions at one distance) become a 1D slider.

Moving a close mic to an arbitrary point uses the nearest captured IRs (up to four, bilinear weights), but not by crossfading them. Crossfading IRs with different phase is itself comb filtering, which is exactly the hollow sound we're avoiding. Instead, the morph is spectral:

1. Each IR is split into a bulk delay (its arrival time) and a minimum-phase component. A minimum-phase IR has the same magnitude response as the original with all its energy as early as possible; it's computed from the log magnitude spectrum via the real cepstrum.
2. Log-magnitude spectra are interpolated with the bilinear weights, and so are the delays.
3. The result is rebuilt as a minimum-phase IR and the interpolated delay is reapplied.

This gives smooth, comb-free movement. The trade-off is that between captured points, the IR's fine phase character is approximated by minimum phase. When the mic sits exactly on a captured point, the original IR is used unmodified, not its minimum-phase version.

Movement never touches the audio thread's math. While the mic is dragged or automated, the loader recomputes the morphed IR at most every ~40 ms, builds a convolver, and hands it over through the normal swap-and-crossfade path. During heavy movement this costs background-thread CPU only.

Built (Phase 3): packs load from a folder, placed by `cabpack.json` (`{"points": [{"file", "x", "y"}]}`), by file names (Cap/Center 0, CapEdge 0.25, CapCone 0.4, Cone 0.6, Edge 1.0 across; 1in/2.5cm/10mm distances scaled 0 to 1), or in file order as a 1D fallback. Scattered points (neither a full grid nor a line) use inverse-distance weights over the nearest four. The minimum-phase step runs in double precision with the spectrum floored 120 dB below its peak and the DC and Nyquist bins given their neighbours' values, since cab IRs' near-exact zeros there otherwise leave a -60 dB error across the whole result. Each mic has permanent position parameters (`cab_mic1_pos_x`, `cab_mic1_pos_y`, and the same for mic 2), and the processor re-morphs only once the previous morph has finished and 40 ms have passed, always for the newest position. A move costs the loader about 1 ms for typical close-mic IRs.

**Convolution.** For now, JUCE's `juce::dsp::Convolution` in its default mode: uniformly partitioned FFT convolution with zero added latency. It matches brute-force convolution to -135 dB at every buffer size from 1 to 512. It builds new engines on its own background thread and crossfades IR swaps over 50 ms. A hand-written hybrid (Gardner 1995: direct-form head, partitioned FFT tail) stays the plan if three mics plus a 1 s room IR need less CPU.

**IR loading.** `.wav` in 16 or 24-bit integer or 32-bit float, mono or stereo. Close mics take the left channel of a stereo IR by default, with an option to pick the right. IRs not at 48 kHz are resampled at load by JUCE's convolution. Full length is kept up to a 1 second cap, with an optional trim that applies a short fade-out.

**Normalization.** Built with white noise rather than the pink noise originally planned: at load, the IR is scaled so white noise keeps its BS.1770 loudness through it. That's computed exactly from the IR's K-weighted energy (Parseval), the expected value of measuring 4 s of white noise before and after, which is how it was first built and stays in the tests as the reference. Measured on clean and distorted guitar through very different cabs, pink noise left up to 6.6 LU of difference and white noise 2.1 LU (decision log). The original plan read: at load, the loudness of pink noise through each IR is measured with the same K-weighted LUFS-style method used for the amps, and a normalization gain is applied so swapping cabs or moving mics doesn't jump in volume.

**Switching and memory.** Each mic slot preallocates room for two convolvers. A new IR (file change, position move, or morph update) is built on the loader thread, then old and new run together for about 30 ms while crossfading, and the old one goes back to the loader to be dropped.

**Testing.**
- **Correctness.** Brute-force direct convolution in numpy as the reference, error below -100 dB.
- **Buffer sizes.** Every buffer size in {1, 7, 64, 128, 512}.
- **Zero latency.** An impulse test asserting output begins at sample zero.
- **Morph checks.** A morph at a captured point returns the original IR, and small moves produce small spectral changes (continuity).
- **Comb test.** The morph has no comb notches where a naive crossfade does.
- **CPU.** A benchmark with two close mics plus a 1 second stereo room IR.

### Post FX

| Block | Controls | Implementation |
|---|---|---|
| Chorus | See Chorus below | |
| Delay | See Delay below | |
| Reverb | See Reverb below | |

### Bloom (modulation container)

Design settled in design review round 12.

**Container.** Holds bitcrush, phaser, and flanger, each with its own bypass, reorderable inside the container (default: bitcrush, phaser, flanger). Container-level mix and bypass. All LFOs can sync to the shared tempo (internal BPM or tap tempo).

**Bitcrush.** The one block where aliasing is intended.
- **Bit depth.** Quantization to 2^bits levels with a continuous (fractional) bits control so sweeps are smooth.
- **Sample rate reduction.** Sample-and-hold driven by a fractional phase accumulator. No oversampling and no anti-aliasing, deliberately.
- **Dither.** Optional. Off gives gated, sputtering decays; on turns them into hiss.
- **Tone and mix.** Post low-pass and dry/wet.

**Phaser.** Chain of zero-delay-feedback first-order allpass filters mixed with dry (default 50%, deepest notches). Stages 2, 4, 6, 8, or 12 (one notch per two stages). Exponential sweep mapping across roughly 100 Hz to 4 kHz, feedback, stereo LFO offset. Modes, physically informed as in round 10:
- **Classic** (Phase 90-style): four JFET stages with the JFETs' nonlinear resistance shaping the sweep curve; feedback optional, matching the two well-known versions of the pedal.
- **Modern.** Fully adjustable stages, range, and feedback.
- **Vibe** (Uni-Vibe-style): four stages with deliberately mismatched frequencies, swept by a lamp-and-photocell LFO whose sluggish, asymmetric response gives the characteristic throb.

**Flanger.** On the shared modulated-delay engine: 0.5 to 10 ms base delay, positive or negative feedback with a soft clipper and high-pass in the loop, Manual (static comb) control, LFO shapes, tempo sync, Hermite interpolation.
- **Through-zero mode (opt-in).** The dry path is delayed by about 5 ms so the wet path can sweep through it, producing the total cancellation of tape flanging. Adds about 5 ms of latency, reported to the host, only while this mode is active.

**Testing.**
- **Bitcrush.** Exactly 2^bits output levels, correct hold period, aliasing present by design, correct dither statistics.
- **Phaser.** Notch count equals stages divided by two, notch positions follow the sweep mapping, wet-only output has unity magnitude, stable at maximum feedback.
- **Vibe.** Sweep asymmetry matches a Python reference of the lamp/photocell model.
- **Flanger.** Comb notch spacing matches 1/delay, feedback bounded; in through-zero mode, total cancellation at the crossing, and reported latency equals measured latency.

### Tuner

Design settled in design review round 11.

**Algorithm.** YIN (de Cheveigné and Kawahara, 2002) and the McLeod Pitch Method (McLeod and Wyvill, 2005) are both prototyped in Python and evaluated on Sean's real DI recordings; the more accurate one becomes the shared pitch detector for the tuner and the harmonizer.

**Hosting.** The audio thread copies the DI into a lock-free ring buffer; the tuner's analysis runs on a separate analysis thread at 30 to 60 updates per second. The harmonizer reuses the same detector code but hosts it on the audio thread with its own latency settings (see Harmonizer).

**Range.** Tracks down to 30 Hz to cover 7 and 8-string tunings. The analysis signal is low-passed and downsampled 4x (to 12 kHz) before detection, since guitar fundamentals sit below about 1.3 kHz; this cuts cost roughly 16x, and sub-sample interpolation preserves precision. At 30 Hz the window must cover about two periods (roughly 67 ms), which is fine for a tuner display.

**Robustness.** Octave errors (a strong second harmonic on wound strings) are handled by the algorithm's threshold step, a median filter across successive readings, and note hysteresis. The readout holds the last valid value after a note decays.

**Display.** Needle mode (smoothed cents with note name) and strobe mode (pattern speed proportional to the cents offset, stationary when in tune). Displays to ±0.1 cent; accuracy target within 0.5 cent on real strings. Reference A4 adjustable from 430 to 450 Hz.

**Mute.** Output mutes while the tuner is engaged, footswitch-toggled; mute on by default, with an option to tune while hearing yourself.

**Polyphonic strum tuning (stretch, after the basic tuner).** Strum all open strings and see each string's tuning at once. Tractable because the target tuning is known: each string only needs a narrow frequency search around its expected pitch.

**Testing.**
- **Synthetic accuracy.** Sines, sawtooths, and inharmonic stiff-string tones from 30 Hz to 1.3 kHz, all within 0.5 cent.
- **Octave errors.** None on synthetic low strings with a dominant second harmonic.
- **Real strings.** DI recordings of open strings, cross-checked with a hardware tuner.
- **Hold and smoothing.** Readout holds after decay and doesn't flicker.
- **Cost.** Analysis thread stays light.

### Pitch shifter

Design settled in design review round 13. One module, two engines, shared by the multivoicer, the harmonizer, and the shimmer reverb.

- **Granular (Poly).** Built on the shared modulated-delay engine: 3 to 4 overlapping read heads with equal-power windows, swept by a sawtooth. Works on chords. The dry path is untouched; wet voices trail by roughly 10 to 20 ms because the read heads must stay behind the write head, which behaves like a doubling delay, not system latency. Weaknesses: a slight warble, and muddy octave-down on chords.
- **PSOLA (Mono).** Pitch-synchronous overlap-add using the shared pitch detector's period. Excellent on single notes, especially octave down. Fails on chords, so it must degrade gracefully when fed one.
- **Rejected:** phase vocoder (40 to 90 ms latency is unplayable live). **Skipped:** formant preservation (significant work, matters far less on guitar than voice).

Inheritance: multivoicer uses both via a Poly/Mono switch; the harmonizer uses PSOLA; the shimmer reverb uses granular, where the reverb hides the smearing.

### Multivoicer

Design settled in design review round 13.

**Voices.** Up to 8, each with pitch offset (semitones plus cents), delay offset (0 to 50 ms), pan, level, and optional slow random drift so doubles sound like separate takes.

**Engine switch.** Poly (granular, default, since Sean plays chords constantly) or Mono (PSOLA, for single-note lines and clean octave-down).

**Starting points.**
- **Unison double.** ±7 to 12 cents, short delays, wide pan.
- **Octave stack.** -12 and +12 semitones.
- **Fifths stack.** +7 semitones, parallel and chromatic (diatonic intervals belong to the harmonizer).
- **Double + Octaves (default).** Two detuned doubles plus quieter -12 and +12 voices, since Sean uses doubling and octaves about equally.

**Global controls.** Spread, voice count, mix, optional wet high-pass (off by default so octave-down voices keep their lows).

**Testing.**
- **Pitch accuracy.** A shifted sine is within 1 cent of target in both engines.
- **Grain ripple.** Crossfade amplitude modulation stays below a set threshold.
- **Chords.** In Poly mode, a synthetic chord's spectral peaks all shift by the correct ratio.
- **Mono quality.** PSOLA is clean on single-note sweeps and degrades gracefully on chords.
- **Dry path.** Untouched, zero added latency.
- **Switching.** Poly/Mono changes don't click.

### Harmonizer

Original scope plus design review round 14.

**Scope.** Monophonic by design: single-note lines, leads, and tapping runs. Real-time polyphonic pitch tracking of chords is a research problem; on chords the voices mute (see Confidence gating).

**Timing.** The dry signal is never delayed. Harmony voices trail it by the detection time, like a tight second guitarist, which is how hardware harmonizers behave. The block reports zero latency.

**Detection.** The shared pitch detector (YIN or McLeod, per the tuner evaluation) runs on the audio thread on a 4x-decimated copy of the DI, updated every 64 samples. A minimum-frequency setting, default 110 Hz (open A string), bounds the search; lowering it slows detection for every note. Approximate harmony start times: high E about 6 to 8 ms, G about 10 to 12 ms, A about 20 ms, low E about 25 to 30 ms (if the floor is lowered to reach it).

**Onsets.** A voice fades in over a couple of milliseconds once the detector is confident about the new note, never briefly playing the previous note's harmony.

**Note transitions.** The interval is chosen at note onset and locked. Bends, vibrato, and slides keep the locked interval, so the harmony moves in parallel. A discrete pitch jump (more than about 70 cents within about 10 ms, as with hammer-ons, pull-offs, and taps) counts as a new note and re-evaluates immediately. A slide re-evaluates once pitch holds steady on a new note for about 30 ms. A glide control sets how quickly the shift moves when the interval changes.

**Key and scale.** Manual root plus scale: major, natural minor, harmonic minor, melodic minor, the seven diatonic modes, phrygian dominant, whole-half diminished, and custom scales via a 12-note on/off mask. Key presets are switchable over MIDI for key changes between song sections. Note mapping follows the tuner's A4 reference.

**Voices.** Up to 4, each with interval mode (diatonic or chromatic), interval (diatonic: -7 to +7 scale steps, where +2 is a diatonic third; chromatic: semitones), octave offset (-2 to +2, which covers octave and unison stacking), level, pan, and a small humanize delay. One voice at a 3rd, triads from two voices, and octave stacks all come from this one model. Shifting uses the shared pitch shifter's PSOLA engine.

**How "smart" works.** Each detected note maps to its scale degree in the selected key, moves the requested number of steps, and the resulting semitone shift is applied, so a diatonic third is sometimes 3 semitones and sometimes 4. Out-of-scale notes default to Parallel (use the same shift as the nearest in-scale note, so chromatic passing tones move smoothly), with Snap (force the harmony onto a scale note) as the alternative. Ties go to the lower degree.

**Confidence gating.** When detector confidence drops (chords, noise, heavy muting), voices fade out instead of producing garbage, and return when a confident single note does.

**Testing.**
- **Correctness.** Synthetic melodies in a known key: each voice's output pitch track matches the expected harmony.
- **Bends.** Harmony-to-dry pitch ratio stays constant through a bend.
- **Legato.** A discrete jump re-evaluates the interval within about 10 ms.
- **Slides.** A slide doesn't re-evaluate until it settles.
- **Onset timing.** Harmony starts within the per-string budget above.
- **Chords.** Voices mute on chords and recover cleanly.
- **Scale rules.** Parallel, Snap, and the tie-break behave as specified.
- **Clicks.** None on any transition.

### MIDI control

Hardware: a USB class-compliant MIDI foot controller (class-compliant means macOS recognizes it with no drivers). USB rather than Bluetooth MIDI, because Bluetooth adds latency and jitter. Ideally it sends both Program Change and CC messages, has at least 4 switches, and has an expression pedal input for later.

Input path: the JUCE standalone app opens MIDI input devices (enabled in its Options dialog) and delivers events to processBlock() with sample positions, so no separate MIDI thread is needed.

Built so far (Phase 3): Program Change 0, 1, 2 (labeled 1, 2, 3 on most footswitches) selects amp slot 1, 2, 3 immediately on the audio thread, read from the raw MIDI bytes so nothing allocates. The slot parameter catches up from a 20 Hz timer, because setting a parameter from the audio thread notifies listeners, which can lock. Mappings, stored in presets: Program Change or a CC selects the amp slot, CCs toggle any block's bypass, a CC toggles the tuner, a CC acts as tap tempo for the delay, and an expression pedal can drive any continuous parameter (volume, wah-style sweeps, harmonizer mix). Mapping is done with MIDI learn: click a control, press a switch, done. Until the GUI exists, mappings live in a config file.

### Delay

Design settled in design review round 7.

**Buffer.** Preallocated stereo circular buffer, 4 seconds max.

**Time and tempo.** Milliseconds, or synced to note divisions (whole through 1/16, dotted, and triplet). Tempo source: internal BPM, a tap button, or MIDI tap tempo from the footswitch. Tap tempo averages the last few intervals, rejects outliers, and resets after a 2 second gap.

**Time changes.** Digital mode crossfades between two read heads (no pitch artifacts). Analog and tape modes glide the read position, bending the pitch of the repeats like a real tape or BBD delay. Fractional reads use 4-point Hermite interpolation, since linear interpolation dulls highs when the read position moves.

**Feedback loop.** Feedback up to 110% for deliberate self-oscillation, with a soft clipper in the loop to keep it bounded. High-pass and low-pass filters in the loop so each repeat gets thinner and darker and sits behind the dry signal.

**Character modes.**
- **Digital.** Pristine repeats.
- **Analog.** BBD-style: progressively darker, lightly saturated repeats.
- **Tape.** Wow and flutter, saturation, high-frequency loss.

All modes have an optional modulation LFO on the repeats.

**Stereo modes.** Stereo (independent left and right with an offset), ping-pong (input summed to mono, repeats alternate sides), and dual (different time per side, such as quarter left and dotted eighth right).

**Ducking.** On by default at a moderate amount. An envelope follower on the delay's input lowers the wet signal while Sean plays and restores it in the gaps, so fast runs stay clear and tails bloom in the spaces.

**Mix and bypass.** Dry/wet mix. Spillover on bypass per the foundation rules.

**Testing.**
- **Timing.** An impulse arrives at exactly the expected sample.
- **Tempo math.** A dotted eighth at 120 BPM is exactly 375 ms.
- **Feedback.** Each repeat decays by the set amount; output stays bounded at maximum feedback.
- **Clicks.** Digital-mode time changes produce no clicks.
- **Ping-pong.** Repeats alternate channels.
- **Ducking.** Wet level drops by the set amount while signal is present.
- **Tap tempo.** Averaging and outlier rejection behave as specified.

### Shared modulated-delay engine

Chorus, flanger, multivoicer, and vibrato are the same machine with different parameters: a delay line whose read position is moved by an LFO, mixed with the dry signal. One engine is built in Phase 5 for the chorus (preallocated delay line, Hermite interpolation, per-voice LFO with phase offset, optional feedback), and the flanger (Phase 8) and the granular pitch shifter (Phase 9) are thin layers over it. Decided in design review round 9 to avoid building the same thing three times. Correction from round 13: constant detune (a voice held at +7 cents) is pitch shifting, not chorus, because an oscillating read head only produces pitch that wobbles around the original. The granular shifter covers it: a sawtooth sweeps the read heads steadily and crossfaded heads hand off at the end of each sweep.

| Effect | Base delay | Voices | Feedback |
|---|---|---|---|
| Chorus | 5 to 25 ms | 1 to 3 | None |
| Granular pitch shifter (multivoicer Poly mode, shimmer) | Sweeping | 3 to 4 crossfaded read heads driven by a sawtooth | None |
| Flanger | 0.5 to 10 ms | 1 | Yes |
| Vibrato | Any | 1, 100% wet | None |

### Chorus

Design settled in design review round 9.

**Modes.**
- **Classic.** One voice, triangle LFO, about 7 to 15 ms base delay, CE-2-style warble. In stereo the right channel's LFO is inverted.
- **Dimension.** Two voices with LFOs in exact antiphase and shallow depth: width and lushness with very little audible pitch wobble.
- **Tri.** Three voices with LFOs 120 degrees apart, the big studio rack chorus; pitch movement averages out across voices.

**LFO.** Triangle, sine, or smoothed random. Rate and depth, optional tempo sync.

**Analog character.** On by default at a subtle setting: gentle low-pass on the wet signal (BBD anti-aliasing style), light saturation, optional faint noise. Off gives pristine digital chorus.

**Low-end protection.** High-pass on the wet signal only, on by default at about 150 Hz, so low notes and open low strings stay solid and centered while the chorus works on top.

**Stereo and mono safety.** Each channel gets its own phase-offset LFO, with a Width control. Dry signal stays centered so mono summing never collapses badly.

**Mix.** Dry/wet; 100% wet gives vibrato.

**Testing.**
- **Depth.** Pitch deviation on a sine, in cents, matches the depth setting.
- **LFO phases.** 180 and 120 degree voice offsets are correct.
- **Wet high-pass.** Low frequencies pass unmodulated.
- **Mono sum.** No deep cancellation when left and right are summed.
- **Interpolation.** Matches a high-resolution reference.
- **Zipper noise.** Knob moves don't step.

### Reverb

Design settled in design review round 8.

**Engines.**
- **Room and Hall.** A feedback delay network (FDN, after Jot): 8 delay lines (16 in Hall) with mutually prime lengths spread over roughly 30 to 100 ms and scaled by Size, mixed through a Householder matrix. Householder is orthogonal (energy-preserving, so stability depends only on the decay filters) and costs O(N).
- **Plate.** The Dattorro plate topology from "Effect Design, Part 1" (JAES 1997), using the paper's published delay lengths and coefficients.
- **Freeverb** exists only as a Python learning prototype, not in the product.
- **Convolution reverb** deferred (multi-second IRs need non-uniform partitioning, and decay can't be adjusted).

**FDN internals.**
- **Decay.** Per-line gain computed from the target T60 and the line's length, plus a shelving filter per line for separate low and high decay times.
- **Early reflections.** A multi-tap delay ahead of the network, patterned from Size.
- **Input diffusion.** Series allpasses smear transients before they enter the network.
- **Modulation.** Slow random modulation of several delay lengths with Hermite interpolation, to remove metallic ringing; depth and rate controls.
- **Stereo.** Left and right inject into different lines; outputs tap different line combinations for decorrelated sides, with a Width control.

**Controls.** Mix, pre-delay (optionally tempo-synced), size, decay, low and high decay multipliers, diffusion, modulation depth and rate, width, early/late balance, wet low cut and high cut, ducking (off by default).

**Room mic interplay.** The cab room mic and the reverb both add early reflections and can blur together. The early/late balance control handles it; presets that lean on the room mic (Midwest Emo) keep the reverb toward late.

**Freeze.** Footswitchable. Sets decay filters to unity gain and mutes the input, holding the current tail indefinitely. FDN and Room/Hall only at first; plate freeze if the topology allows it cleanly.

**Shimmer.** An octave-up pitch shifter in the FDN feedback loop for ambient pads. Built after the pitch shifter exists (Phase 9), not in Phase 5.

**Safety.** Reverb tails are the main denormal risk in the project, so this block carries the decay-to-silence CPU test. Spillover on bypass.

**Testing.**
- **Decay accuracy.** T60 measured from the impulse response's energy decay curve (Schroeder backward integration) is within 10% of the setting for low and high bands.
- **Stability.** Matrix orthogonality checked; energy bounded at maximum decay.
- **Freeze.** Energy holds constant for 60 seconds.
- **Stereo.** Low left/right cross-correlation at full width.
- **Denormals.** No CPU spike as a tail decays to silence.
- **Plate.** Matches a Python reference built from the paper's coefficients.

## Global features

Input and output level meters, a CPU meter, and correct latency reporting to the host. Oversampling is per block, not global (see Foundation decisions).

### Presets and scenes

Design settled in design review round 15.

**Preset vs global.** A preset holds every parameter, each amp slot's capture, cab IRs and mic positions, pre and post chain order, Bloom order, MIDI mappings, harmonizer key presets, custom scales, tempo, and its scenes. Global settings (never changed by loading a preset): input calibration, audio and MIDI devices, A4 reference, tuner mute preference, CPU fallback mode, window size and UI scale.

**Format.** One JSON file per preset with a `format_version` field, stored in `~/Library/Application Support/AmpSim/presets/`.

**Evolution.** Missing parameter IDs take defaults; unknown IDs are ignored with a warning; structural changes bump `format_version` with one small, pure, tested migration function per step. A set of golden preset files lives in the test suite and must load identically on every build.

**Missing files.** Model and IR references store a path relative to the `models/` or `irs/` root plus a content hash. A broken path is relinked by searching the library for the same hash; a truly missing file loads as a "missing" slot without failing the preset.

**Loading.** The background thread parses, loads, and prewarms everything, then the audio thread swaps it in with a short fade (between songs, not seamless).

**Scenes.** 8 per preset, switched instantly from the footswitch since everything is already loaded. A scene snapshots the active amp slot, every block's bypass state, and a chosen set of parameter values (such as delay mix or reverb level). One stomp moves a song from verse to chorus.

**Editing.** Undo/redo and A/B compare, both entirely on the GUI thread.

**Factory presets.** The five style presets (see Amps).

### GUI

Design settled in design review round 15. Sean directs the visual design; the agent builds it. The goal is the most polished result, not toolkit learning.

**Toolkit.** JUCE's own Component system throughout, from the basic panel to the final UI (it replaced egui when the project moved to C++). JUCE is the most widely used audio plugin GUI framework, so an agent builds it reliably, and its custom painting (Graphics, paths, images, LookAndFeel) gives full visual control. Polish comes from custom-drawn widgets and a consistent design system, not from the toolkit's defaults. JUCE also has a web-view UI option (HTML/CSS) if more visual freedom is ever worth it.

**Design system.** Before Phase 11 builds anything, a `docs/UI_DESIGN.md` captures Sean's direction: color palette, typography (embedded fonts), spacing scale, knob and meter styles, and reference screenshots for mood. Everything is drawn in code; no artwork or trade dress copied from other products.

**Layout.**
- **Top bar.** Preset browser, A/B, undo, tuner button, tempo and tap, input/output meters, CPU meter.
- **Center.** The signal chain as a strip of blocks, drag to reorder within a section, click to bypass, select to edit; section boundaries visible.
- **Lower panel.** The selected block's editor: amp page with three slot cards, cab page with a speaker drawing and three draggable mics, EQ page with the draggable curve over the live analyzer.
- **Bottom.** Scenes bar.
- **Tuner.** Full-window overlay with needle and strobe.

Dark theme, crisp on Retina, resizable with a UI scale setting. Knobs: drag, shift-drag for fine control, double-click to type a value, scroll wheel, right-click for MIDI learn.

**Thread communication.**
- **Parameters.** JUCE's parameter attachments over atomic parameter values.
- **Everything else.** Chain reorders, file loads, MIDI map changes go through a lock-free single-producer single-consumer queue (JUCE's AbstractFifo) from GUI to audio, detouring through the background loader for heavy work. The audio thread confirms applied changes on a return queue so the GUI shows what's actually running.
- **Audio to GUI.** Atomics for meters, tuner, and gain reduction; a ring buffer for analyzer samples.
- **Rule.** No mutex is ever shared with the audio thread. If the command queue is full, the GUI retries; the audio thread never blocks.

**Performance.** Repaint only on change, about 30 fps while meters move.

**Testing.**
- **Round trip.** Save then load gives identical state.
- **Migration.** All golden presets load identically.
- **Relinking.** Moved files relink by hash; deleted files load as "missing" without errors.
- **Scenes.** Switching applies exactly the snapshot, without clicks.
- **Queue.** A full command queue never blocks the audio thread.

## Build order

The order is driven by dependencies and by getting a usable tool as early as possible.

**Phase 1, Skeleton.** JUCE standalone app with a smoothed gain knob, running on the audio interface. Done when you can play through it live with no dropouts at a 128-sample buffer.

**Phase 2, Offline render harness and tests.** A CLI that renders a `.wav` through the chain, plus the test suite: golden comparisons against references, real-time safety, and CPU. Done when the harness renders a DI file and the tests run under ctest.

**Phase 3, NAM slots, cab, and basic MIDI.** NAM core integration with differential tests against its own render tool, three always-running amp slots, the three-mic IR cab, and MIDI amp switching from the footswitch. Done when the slots match the official core within tolerance on several community models, run live at a 128-sample buffer without dropouts, and you'd genuinely practice through it.

**Phase 4, Compressor and EQ.** Pre and post compressor instances (Studio and Pedal modes), graphic and parametric EQ.

**Phase 5, Post FX and basic presets.** Delay (with MIDI tap tempo), reverb, chorus, plus basic preset save/load: plain JSON holding parameters, slot captures, IRs, and chain order, written as `format_version` 1 of the full preset format so Phase 11's migrations start from it. Done when Sean's CHON/Polyphia-style clean tone is good enough to be his daily practice rig.

**Phase 6, Gates, boost, overdrive.** Linked dual gates with adaptive release, boost, overdrive (Mid Drive, Transparent, Distortion, then Fuzz last). Done when a fast tech death riff with hard stops is tight and silent between stops, and legato and tapping in heavy parts survive the gate.

**Phase 7, Tuner.** YIN pitch detection with a basic display.

**Phase 8, Bloom.** Bitcrush, phaser, flanger in a container block.

**Phase 9, Pitch shifter, multivoicer, and shimmer.** The shared pitch shifter (granular and PSOLA), then the multivoicer and the shimmer reverb mode on top of it.

**Phase 10, Harmonizer.** Builds on the tuner's pitch detector and the multivoicer's pitch shifter.

**Phase 11, Presets, scenes, and GUI.** The full preset system on top of Phase 5's basic format (migrations, golden files, hash relinking), scenes, undo and A/B, then the real JUCE UI built from `docs/UI_DESIGN.md` under Sean's direction. A basic panel exists from Phase 2 onward; the real UI comes last because it's the part most likely to eat weeks with zero sound improvement.

## Toolchain reference

The compiler is Apple clang from the command line tools (`xcode-select --install`). CMake (in `~/.local/bin`) reads `CMakeLists.txt` and generates the build: `cmake --build build` compiles, and `ctest` runs the tests. JUCE and NeuralAmpModelerCore are git submodules: separate repos pinned at exact commits inside this one, fetched with `git submodule update --init --recursive`. JUCE's `juce_add_plugin` builds the standalone `.app` today. Adding `VST3` or `AU` to its FORMATS list builds plugins a DAW can load (Reaper is the recommended DAW: cheap, lightweight, and supports both). An IR is just a short `.wav` file containing a cab's response to an impulse, so loading one is ordinary file reading.

## Decision log

2026-09-27: Rust + nih-plug for the product, Python as the prototyping lab bench.
2026-09-27: Harmonizer is monophonic, manual key and scale, up to 4 voices with diatonic or chromatic intervals and octave offsets.
2026-09-27: Pitch detection and gate detection read the clean DI. Harmonies generated after amp and cab.
2026-09-27: No bundled IRs. Cab section is a dual-mic IR loader for user-supplied files.
2026-09-27: Standalone-first on macOS with the Scarlet Solo; DAW export deferred. Basic egui knob panel pulled forward to Phase 2 because there's no DAW-provided parameter UI.
2026-10-01: Cargo workspace split into a framework-free DSP crate and a thin nih-plug plugin crate, done at the start of Phase 2.
2026-10-01: Amps are NAM models on a hand-written Rust inference engine instead of gray-box models. Amp-only captures, 48 kHz, differential testing against NeuralAmpModelerCore and the PyTorch trainer. Gray-box prototype kept as reference only.
2026-10-01: Foundation review settled: mono until the cab, stereo after; reorderable pre-FX and post-FX sections with fixed amp and cab; fixed 48 kHz; 128-sample target buffer; per-block oversampling; f64 filter state; lock-free loading with off-thread drops; all three amp slots always running for seamless switching; spillover on time-based effects; allocation assertions on in debug.
2026-10-01: Hands-free control is a USB class-compliant MIDI footswitch. Basic MIDI (amp switching) lands in Phase 3; tap tempo with the delay, expression and MIDI learn later.
2026-10-01: NAM engine review settled: A1 + LSTM first, full current .nam spec as the immediate next milestone; channel-major time-contiguous layout; prewarm on load; automatic measured loudness matching plus manual trim; three-oracle differential testing with synthetic models; Sean hand-writes the WaveNet core, Claude writes the rest.
2026-10-01: Cab review settled: three mics (two close, one room); hybrid zero-latency convolution; auto phase alignment on for close mics; Neural DSP-style movable mics via cab packs with a 2D position map and minimum-phase spectral morphing between captured IRs; user can add IRs to packs; loudness-normalized IRs.
2026-10-01: Gate review settled: two fully independent gates (pre-amp reorderable, post-amp fixed before cab), both DI-detected by default; classic release default with adaptive option; legato-friendly defaults with a wide hysteresis gap and a legato survival test.
2026-10-01: Conflict review: gate release default changed to adaptive; gates linked by default (unlinkable); CPU fallback order fixed as lite stand-ins in inactive slots, then switch blip, then 256 buffer.
2026-10-01: Compressor review settled: one block type placeable in pre and post, Studio (feed-forward) and Pedal (feedback) modes, defaults aimed at CHON/Polyphia-style cleans.
2026-10-01: Build order changed to match Sean's main playing (CHON/Polyphia): after amps and cab come compressor and EQ, then delay/reverb/chorus, then gates/boost/overdrive.
2026-10-01: EQ review settled: pre and post instances, each Graphic or Parametric; TPT SVF filters; accurate graphic EQ via band interaction compensation; visual EQ with spectrum analyzer in the GUI phase. Removed the redundant amp-to-cab EQ slot (linear filters commute) and updated the signal-chain diagram to current decisions.
2026-10-01: Amps stay at three always-running slots; Sean's five styles (Polyphia, CHON, Tech Death, Metal, Midwest Emo) become five style presets that load captures into the slots.
2026-10-01: Delay review settled: digital, analog (BBD), and tape modes; tempo sync with MIDI tap tempo; stereo, ping-pong, and dual modes; ducking on by default.
2026-10-01: Reverb review settled: FDN Room/Hall plus Dattorro plate; shimmer added in Phase 9 once the pitch shifter exists; footswitchable freeze; convolution reverb deferred.
2026-10-01: Chorus review settled: one shared modulated-delay engine for chorus, flanger, multivoicer, and vibrato; Classic, Dimension, and Tri modes; wet high-pass on by default; subtle analog character on by default.
2026-10-01: Boost/overdrive review settled: shared drive engine; Boost modes Clean, Tight, Screamer; Yeh-style physically informed Mid Drive, Transparent, and Distortion; polyphase IIR oversampling for zero latency; ngspice validation. 
2026-10-01: Fuzz added as a Big Muff-style mode, built last in Phase 6; Fuzz Face-style excluded.
2026-10-01: Tuner review settled: YIN vs McLeod chosen empirically on real DI; analysis thread hosting; tracks to 30 Hz; needle and strobe displays; mute by default; polyphonic strum tuning as a stretch.
2026-10-01: Bloom review settled: phaser Classic, Modern, and Vibe modes; bitcrusher with fractional bits and optional dither; flanger on the shared engine with an opt-in through-zero mode, which becomes the second documented latency exception.
2026-10-01: Multivoicer review settled: pitch shifter with granular (Poly, default) and PSOLA (Mono) engines shared by multivoicer, harmonizer, and shimmer; phase vocoder rejected; formants skipped; default voice preset Double + Octaves. Corrected the round 9 claim that constant detune runs on the chorus engine.
2026-10-01: Harmonizer review settled: dry never delayed and harmonies trail, so the harmonizer reports zero latency and through-zero flanging is the only latency exception; interval locked per note with bend-following and legato/slide re-evaluation; Parallel out-of-scale default; custom scales; MIDI key presets; confidence gating; 110 Hz default detection floor.
2026-10-01: Presets/GUI review settled: preset vs global split; JSON presets with versioned migrations and golden files; hash-based relinking; 8 footswitchable scenes per preset; undo/redo and A/B; egui throughout, agent-built under Sean's design direction with a UI_DESIGN.md design system; lock-free GUI/audio communication. Design review complete.
2026-10-01: Basic preset save/load moved into Phase 5 so the daily-driver milestone can save tones; full preset system stays in Phase 11.
2026-10-01: Language and framework switched from Rust + nih-plug to C++ + JUCE, with NeuralAmpModelerCore as the amp engine. This was Sean's call, by the criterion "whatever is easiest to integrate and fastest". Claude had recommended staying in Rust and keeping the hand-written engine for the learning goal. Under Sean's criterion, C++ with NAM core wins: native integration, every .nam version supported, and JUCE's mature CoreAudio, MIDI, standalone wrapper, and plugin formats with no framework patches. Given up: hand-writing the WaveNet core (now an optional side track) and the Rust code (kept in ~/Desktop/bitchlessDSP/amp_sim_priv). Supersedes the 2026-09-27 Rust decision, the 2026-10-01 workspace split, and the hand-written engine parts of the NAM engine review.
2026-10-01: JUCE pinned at 8.0.15, not 9.0.x. JUCE 9.0 (July 2026) replaced the macOS CoreAudio implementation, and a flaky audio backend is what we just left. Revisit once 9.x matures.
2026-10-01: NeuralAmpModelerCore pinned at v0.6.0 and linked whole-archive, because its architectures register through static objects that a normal static link drops.
2026-10-01: Block design decisions 1 to 6 carried over from the Rust version as `ampsim::Block` and `ampsim::Chain`. Blocks own `juce::SmoothedValue`s.
2026-10-01: The GUI toolkit is JUCE's Component system (egui was tied to Rust).
2026-10-01: For now, cab convolution uses `juce::dsp::Convolution` (zero latency, uniform partitions), and IRs are normalized to unit energy until the pink-noise loudness measurement exists. Models use the trainer's loudness metadata, normalized to -18 dB, until measured loudness exists.
2026-10-01: Every feature ships with automated tests, and the evidence (measurements, renders, snapshots) is shown to Sean. Real-time safety is enforced by a test that counts allocations, frees, and blocking locks on the audio thread, since Apple's clang has no RealtimeSanitizer.
2026-10-01: Build approach: full fidelity, phase by phase in BUILD_PLAN order. Each phase gets its own branch (phase-3, phase-4, ...) with the plan's full tests and proof, and merges into main after Sean has played it. A full-app prototype first was considered and dropped, because a full-fidelity preview is the real build done twice, and several parts need Sean anyway (captures, DI recordings, tone tuning, UI direction). Tag milestone-1 marks the state before Phase 3 continued.
2026-10-01: Amp tone controls are five SVF bands after the model: Depth (90 Hz bell), Bass (180 Hz low shelf), Mid (800 Hz bell), Treble (2.8 kHz high shelf), Presence (5 kHz bell), each +-12 dB, coefficients every 32 samples. Frequencies are starting points to tune by ear.
2026-10-01: Footswitch: Program Change 0/1/2 selects slot 1/2/3 on the audio thread immediately; the slot parameter syncs from the message thread.
2026-10-01: Test signals: NAM core's example input is silence plus a test tone, not a DI. Tests use a deterministic Karplus-Strong guitar DI until Sean records real DI clips for tests/fixtures, and NAM differential tests also use a broadband stimulus.
2026-10-01: Loudness: models are normalized to -18 LUFS on a reference guitar DI with a hand-written BS.1770-4 meter (verified against the standard's coefficients and the EBU Tech 3341 cases). Cab IRs are loudness-matched with K-weighted white noise instead of pink noise, because a study across clean and distorted guitar through stock, dark, and bright cabs found pink noise leaves up to 6.6 LU of difference and white noise 2.1 LU. Normalizing whole amp-plus-cab combinations would be exact but takes away a cab's own effect on loudness; that's Sean's call (PROGRESS task 3.3.Q).
2026-10-01: Movable mics built as planned (cab packs, position map, minimum-phase morphing, 40 ms re-morph limit), with packs placed by `cabpack.json`, file names, or file order, and inverse-distance weights for scattered points. IR loudness matching switched from measuring 4 s of white noise to computing its exact expected value from the IR's K-weighted energy: within 0.034 dB of the measurement and 500x faster, which re-morphing every 40 ms needs.
2026-10-01: Cab mixing: constant-power panning scaled by sqrt(2), so a centred mic is unity on each side, matching the cab's passthrough and the chain's own mono-to-stereo copy when the cab is bypassed; hard-panned is +3 dB on that side. A mic whose first IR is loading stays out of the mix until JUCE's engine is in plus a 60 ms settle, then fades in over 20 ms, because JUCE starts each convolver from an identity engine that would pass raw amp fizz. The room uses JUCE's non-uniform (head/tail) convolution with a 512-sample head, still zero latency. Cuts are SVF Butterworth sections (one for 12 dB/oct, two with Q 0.541 and 1.307 for 24) with a 10 ms on/off crossfade.


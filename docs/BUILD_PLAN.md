# BellyDSP Build Plan

BellyDSP, a modern neural amp rig in C++ on JUCE: nine neural amps, one running at a time (eight built-in gain sets and the player's own capture, NAM models on NeuralAmpModelerCore, the official engine), pre FX (two gates, compressor, boost, overdrive, EQ), a three-mic cab with movable mics, post FX (EQ, compressor, chorus, delay, reverb), a Bloom modulation container, a multivoicer, a key-aware harmonizer, a tuner, presets with scenes, and MIDI footswitch control. Standalone on macOS first; VST3/AU export later from the same JUCE target.

## Architecture

### Code layout

One CMake project. `src/dsp/` holds every amp, filter, and effect, the Block interface, and the chain. Blocks may use JUCE's DSP and audio-buffer modules but never processor or GUI types, so the offline renderer and the tests drive them directly. `src/PluginProcessor.*` is the thin glue: parameters, state, file loading, and an audio callback that only drives the chain. `tools/` holds the offline renderer (`ampsim_render`) and the hardware probe (`ampsim_device_probe`), plus the release pipeline (`tools/release/`, `tools/fetch_deps.sh`, `tools/notices/`; docs/RELEASING.md), and `tests/` holds the test suite (`ampsim_tests`). `src/platform/` is what the shipped app knows about itself: its version, its bundled files, and the auto-updater (Sparkle on macOS, WinSparkle on Windows), which runs only in the standalone app and never touches the audio thread. `installer/` holds the DMG read-me and the Windows installer script, `content/` the bundled starter content. JUCE (audio I/O, the standalone wrapper, GUI, convolution, file formats) and NeuralAmpModelerCore (amp inference) are pinned git submodules in `third_party/`. This replaces the Rust workspace split (see the decision log, 2026-10-01).

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
AMP (fixed): nine NAM amps, only the selected one running; a switch warms the new one, then crossfades
 ▼
Gate B (fixed, linked to Gate A by default)
 ▼
CAB (fixed, mono ─► stereo): close mic 1 + close mic 2 + room mic
 ▼
MATCH CURVE (fixed, stereo, off by default): tone match's minimum-phase correction
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

**Amp switching (2026-10-07, Sean's decision; it replaced "Seamless amp switching").** One amp is chosen at a time from nine (`amp_model`): the eight built-in amps in the shelf's order (Glass, Ember, Monolith, Lantern, Basalt, Comet, Forge, Quartz) and the user's own capture. Each amp keeps its own knobs (amp1_* .. amp9_*) and its own capture, and only the selected amp runs. A switch can't simply crossfade, because an amp that hasn't been running has stale history (its models' receptive field, the past samples each output depends on). So it goes in two stages: the incoming amp starts running on the live input unheard while the outgoing one keeps playing, for the capture's receptive field (NAM's prewarm length, 4093 samples for a standard WaveNet, counted in whole buffers: 4096 at 128, 85.3 ms); NAM's WaveNet has finite memory, so from then on its output is exactly what it would have been had it run all along. Then the output crossfades over 20 ms with the equal-power sin law (independent linear ramps; each amp contributes sin(pi/2 p) of its output), and the outgoing amp stops. A switch never clicks, but it lands about 0.1 s after the request. A switch made during a warm-up retargets (the overtaken amp stops unheard, the newest starts warming); back to the amp being heard, nothing more happens; to an amp still fading out (still warm), the ramps redirect at once. The amp fading out holds its gain-set blend (`NamAmp::Blend::hold`). Steady state is one amp: one model on a gain set's step, two between steps, three while its Gain moves; a switch adds the incoming amp for the warm-up and the fade. All eight built-in sets stay loaded, so a switch only waits for the warm-up (the memory and load times are in ASSUMPTIONS AS7). Measured: Measured (tests "Amp section", "Amp gain (amp switching)", "Full rig"; 128-sample buffers): the new amp is first heard 85.3 ms after the request (4096 samples) and the fade ends 105.3 ms after it; from its first heard sample a WaveNet amp is bit-identical to one that ran all along (3375 of 3375 buffers across 14 switches between four gain sets, 8 of them 13 to 32 ms apart), against the ideal crossfade of two always-running amps within 1.3e-6, and no step larger than 0.79x the largest of any step alone. Models running: 1 on a step, 2 between steps, 3 while the Gain sweeps; switching every 0.25 s through all eight with the Gains between steps, at most 2 amps and 4 models. Full rig on the built-in amps (one machine, back to back, normal-priority test thread; before = three always-running slots): defaults 8.4% of the deadline (was 21.5%), every block on 14.7% (27.5%), the heaviest settings 20.9% (33.8%), the heaviest with the Gain between steps 26.1% (39.4%), with the Gain sweeping 26.4% (53.3% with all three slots' Gains sweeping), and switching amps every 0.25 s 33.0%, p99 45.3% (44.6%, p99 60.0%). Preloading the eight sets costs about 200 MB (the process's footprint with them: 225.5 MB against 30.1 MB with an empty processor), and they load in 9.8 s on the one loader thread, the playing amp's first, ready in 1.1 s.

The three-slot design this replaced (2026-10-01 to 10-07): all three slots ran all the time, fed the same input, so every model's history was always current and a switch was an immediate 20 ms crossfade, at three models' CPU (later, with gain sets, the unheard slots played their nearest step alone: "Amp gain", Blending only where it's heard). Its CPU fallback order (below) is moot now.

**CPU fallback order** (for the three always-running slots; moot since only the selected amp runs. Decided in advance, applied in this order only as far as needed):
1. Smaller models in inactive slots only. Each inactive slot runs a lite stand-in of its capture. On switch, the output crossfades to the warm stand-in immediately (seamless), the full model starts processing live input, and once its receptive field has filled (tens of milliseconds) the output crossfades from stand-in to full model. Stand-ins come from the capture author when they publish multiple sizes; otherwise they're distilled offline with NAM's trainer by rendering audio through the full model and training a lite model on it. This is real extra work and is built only if the benchmark demands it.
2. A brief switch blip, offered as a setting.
3. Raising the buffer to 256 samples.

**Bypass and spillover.** Every bypass crossfades over about 10 ms. Time-based effects (delay, reverb, and anything with a tail) support spillover: when bypassed they stop taking new input but let the existing tail ring out. Blocks whose state has to stay live keep running while bypassed, on a copy of their input, with the audio left alone (Phase 6, `Chain::BypassState::keepRunning`): Gate A, which a linked Gate B follows, and the boost and overdrive, whose circuits would otherwise start cold.

**Precision.** Audio buffers are f32. IIR filter state and coefficient math are f64, because low-frequency filters in f32 produce audible noise and drift.

**Denormals.** Flush-to-zero is enabled for every callback with `juce::ScopedNoDenormals`. Test that a decaying reverb tail reaches silence without a CPU spike (Phase 5).

**CPU budget.** The whole chain uses under 50% of the 2.67 ms callback deadline on Sean's Mac. A benchmark harness exists from Phase 2 so this is measured, not guessed. Since 2026-10-08 the budget is set for weaker machines and checked block by block: see "CPU" under Global features.

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

Each slot loaded any `.nam` file, so the three slots were defaults, not hardcoded amps. Since 2026-10-07 there are no slots: the amp is one of the eight built-in amps or the player's own capture ("Amp switching" above), and the roles below are which built-in amp a preset's scenes pick.

**Slots vs presets.** (Until 2026-10-07; now the amp choice, which switches mid-song after its warm-up.) Slots are what switches seamlessly mid-song; there are three, all running. Presets are what loads between songs: a preset chooses the capture in each slot plus every other setting, and loading one is a short fade, not seamless. Five style presets ship as starting points, each mapping Sean's styles onto the three slot types (clean, crunch, high gain):

| Preset | Slot 1 | Slot 2 | Slot 3 | Character notes |
|---|---|---|---|---|
| Modern Prog | Bright modern clean | Crunch | High gain | Heavy pre compression, chorused delay, ducking |
| Math Rock | Bright modern clean | Edge-of-breakup | Crunch | Compressed sparkle, dotted-eighth and dual delays, chorus |
| Tech Death | Tight high gain | High gain lead | Clean | Boost into the amp, linked gates with adaptive release |
| Metal | High gain | Crunch | Clean | Boost, gates, less extreme than Tech Death |
| Midwest Emo | Vintage American clean (warmer, chimey) | Crunch | Edge-of-breakup | Light compression, more room mic, analog or tape delay |

Specific captures are chosen by Sean by ear; the table defines roles, not models. (Modern Prog and Math Rock were named after bands until 2026-10-03.) Since the built-in captures (2026-10-03) the slots are fixed by the heads, so each preset's scenes pick the slot whose role fits: slot 1 Glass (clean), slot 2 Ember (crunch), slot 3 Monolith (high gain). Tech Death and Metal start on slot 3 (ASSUMPTIONS DS50).

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

**Input level.** Per-slot input trim, plus input calibration from model metadata (built): a capture's `input_level_dbu` says what analog level reached 0 dBFS on the rig it was trained with, and a global setting says the same for our interface (default +12 dBu, the Scarlett Solo 4th Gen instrument input at minimum gain). The model's input gets interfaceDbu - captureDbu dB, so a guitar drives the capture as hard as it would the real amp. Captures without the field are unchanged. The loudness measurement uses the calibrated input, so normalization still holds, and changing the setting reloads the captures once it has settled for 300 ms. It's a global setting, never part of a preset.

**Loudness matching.** Built as planned: at load, 4 s of the reference guitar DI is rendered through the model, its BS.1770 integrated loudness measured, and the slot normalized to -18 LUFS, for every model, preferred over the file's own loudness field. The per-slot output trim is the manual adjustment. The reference DI is a deterministic Karplus-Strong riff (`ampsim::referenceGuitarDI`) until Sean records a real one.

**Differential testing.** See above. Synthetic random-weight models from the trainer, as a trainer-vs-core fuzzer, are still a good upstream contribution.

**CPU.** Measured (see "Amp switching").

**Ownership.** Claude writes the integration. Sean's hand-written WaveNet is an optional side track (see CLAUDE.md).

#### Amp gain (2026-10-04)

Sean's play test: "the gain generally just makes things louder", "the clipping is really high". Measured: Gain was the slot's input trim into one capture, so on Glass it was mostly a volume knob (-32 to -9 dBFS RMS from trim -12 to +24 dB), and Ember and Monolith were already saturated at -12 dB (crest factor 2.6 to 3.2 dB throughout), with nothing left for Gain to do. A capture is a snapshot of one amp at one gain setting; driving it past the input range it was trained on is extrapolation (ASSUMPTIONS AG1).

**Gain sets.** Each built-in amp is now several captures across its own gain knob: five steps at 0, 2.5, 5, 7.5, and 10, trained from the gray-box source at those positions of the channel's gain knob, with each channel's taper re-voiced so the steps are evenly spread in distortion (AG2, AG3): Glass clean to the edge of breakup, Ember light to heavy crunch, Monolith tight high gain to a saturated lead. A set is a folder with a `gainset.json` (`src/dsp/GainSet.h`) that the app, the presets, and the state refer to.

**The Gain knob with a set.** The same parameter, `amp*_input_trim` (stored in dB, -24 to +24, shown 0 to 10 as before; AG4), chooses a position across the steps. On a step, that step's model plays alone; between steps i and i + 1 the two blend: y = c(a) ((1 - a) m_i(x) + a m_{i+1}(x)), a linear crossfade because neighbouring steps are highly correlated, with c(a) = 1 / sqrt((1 - a)^2 + a^2 + 2a(1 - a) rho) restoring the power a linear fade of two signals with correlation rho loses mid-blend (rho measured per pair at load; AG5). Every step is loudness-normalized, so Gain changes the saturation and Master stays the volume.

**Scheduling.** A slot runs only what its blend needs: one model on a step, two between steps. A step the knob is heading for starts running hidden and joins the blend only after it has run on live input for its receptive field (the model's prewarm length, 85 ms for a standard WaveNet): NAM's WaveNet has finite memory, so from then on its output is exactly what it would have been running all along, and nothing clicks. The position moves toward the knob at 25 positions per second at most (0 to 10 in 0.4 s), which gives the next step time to warm while the current segment is crossed, and waits at the last warm step if not. At most three models run per slot (AG6, AG7). Measured on the full rig with the built-in sets (128-sample buffers, normal-priority test thread): defaults 20.3% of the deadline (p99 24.0%), every block on 26.3%, the heaviest settings 32.2% (all at Gain 5, one model per slot, as before the sets: 19.9 / 25.5 / 31.0%), the heaviest settings with every slot between two steps 50.3% (p99 55.7%, worst 64.8%), and with all three Gains sweeping at once 49.8% (p99 62.4%, worst 75.4%), no buffer over. Smaller step architectures weren't needed.

**Blending only where it's heard (2026-10-05; superseded 2026-10-07 by "Amp switching": an amp nobody hears doesn't run at all).** The three slots still all ran then, but only the selected slot blends (`NamAmp::Blend`, set by `AmpSection` per buffer). Every other slot plays the step nearest its Gain, alone: one model (ties to the lower step, with a quarter-position hysteresis around each midpoint; AG16). When that nearest step changes on a slot nobody hears (a scene, a preset, automation), the new step warms hidden and the position crosses to it at the Gain's slew rate under the blend law, one step at a time, two models at most (AG17). On a switch, the newly selected slot plays its nearest step while its second step warms (the 85 ms receptive field, in whole buffers), then crosses to the exact blend at the Gain's own slew, 25 positions per second, so 50 ms at most from half a step away (AG15); from then on it's bit-identical to a slot that blended all along (measured 107 to 125 ms after the switch, Gains 0.75 to 1 position from a step). The slot switched away from holds exactly what it plays through its 20 ms fade (`Blend::hold`: a model still warming stops, nothing audible changes), then drops to its nearest step the same way, unheard (AG18). Models running across the three slots, measured: 4 with every Gain between steps (was 6), at most 5 while switching (rapid switches 13 to 32 ms apart included), 6 with both unheard slots' Gains changing at once, 7 with all three Gains sweeping. Full rig, heaviest settings, every Gain between steps: 36.4% of the deadline on average (was 47.8%), p99 40.3% (52.4%); the same with the slot switching every 0.25 s: 38.7%, p99 46.7%; all three Gains sweeping: 47.3% (47.6%), unchanged, because the unheard slots spend most of that run crossing between neighbouring steps with two models each.

**A single capture** (a plain .nam): Gain stays its input trim, -24 to +24 dB, but loudness-compensated: at load, on the loader thread, the capture's loudness is measured at every 3 dB of trim on the reference DI, and the output is turned by the inverse, interpolated and following the same smoothed position as the trim, so Gain changes the drive and not the volume. A single capture still can't clean up or gain up beyond what it was trained on (AG8).

**Factory presets** set every amp knob of every slot they play, and scenes hold an amp's Gain (and Middle) where two scenes share an amp (AG12).

#### More built-in amps (2026-10-07)

tone_bench Round 1 (TONE_MATCH.md) found 45% of the matcher's error is amp and cab coverage: the oracle's Gain sat at +24 dB in 17 of 50 cases, it picked Ember at full gain for most high-gain and lead records, and the amp-only swap error was 0.29 on high gain and lead against 0.11 on clean. So five more built-in amps, each a five-step gain set trained exactly like the first three (`make_default_captures.py`; AG20 to AG29):

| Amp | Voicing | Range (NL, dB) | How it differs |
|---|---|---|---|
| **Forge** | Tight modern high gain | -11 to -4.9 | A 4th-order low cut (100 and 180 Hz) and +7 dB at 1.4 kHz before any clipping; one soft stage, then two hard-clipping ones (algebraic sigmoid, k = 8); an active contour (a 500 Hz cut) after; a stiff power amp, no sag |
| **Basalt** | Fat high gain with sag | -11 to -5.1 | Only a 30 Hz high-pass and +5 dB below 140 Hz into the clippers, 20 Hz couplings; three biased arctangent stages (the smoothest clipper); a cathode follower squashing the positive swing into its own FMV stack; 20 ms power-amp sag; a 70 Hz resonance |
| **Comet** | Saturated lead, mids forward | -9 to -4.5 | +6 dB at 850 Hz and a 5.5 kHz roll-off before the gain; three biased soft stages with an envelope squash before the last; a mid-forward active stack; the power amp driven into a two-section supply sag (1 ms and 30 ms) for a softer attack and the sustain |
| **Quartz** | Saturated lead, scooped and bright | -9 to -4.7 | The tone stack first (its own FMV values, a steep low cut), then a 6 dB cut at 550 Hz and a +5 dB treble shelf, all before the gain; three SYMMETRIC cubic clippers (odd harmonics only); a scoop and a low lift after; 10 ms sag |
| **Lantern** | Power-amp breakup, edge to crunch | -27 to -9 | One nearly clean preamp stage; the Gain drives the power section: a phase inverter that clips late, then a class AB output pair modelled per half (softplus conduction, saturating, 8% mismatched) under a 15 ms rail sag; an output transformer's band limits |

The sources are `prototypes/amp_voicings.py` (amp_sim.py's three channels are untouched). Every voicing differs from the built-ins (1 to 3 non-inverting tanh stages, the FMV stack after them, a plain tanh power amp) and from tone_bench's hidden ones (inverting triode stages with blocking, their own FMV values or a Baxandall, one push-pull model with a crossover term and a single sag). Checked by `prototypes/tone_bench/distinct.py`: each step of each built-in against every hidden gray-box voicing driven to the same NL, by the shape of the long-term spectrum and the harmonic profile of a sine (AG22).

**Tapers and training.** Each set's taper (the drive at Gain 0, 2.5, 5, 7.5, 10, found by `--voice` so NL steps evenly): Forge -33.8, -31.9, -29.3, -25.1, -10.3 dB; Basalt -26.4, -23.5, -19.9, -14.4, +1.7; Comet -30.0, -27.9, -24.8, -19.6, +1.1; Quartz -25.1, -23.2, -20.7, -16.4, -1.8 (first stage); Lantern -8.8, -4.3, -0.4, +4.0, +10.1 (power section). The high-gain and lead tops stop just under each voicing's NL ceiling: every voicing, Monolith too, flattens out at -4.4 to -5.1 dB on the tests' DI (AG23), so they reach that region by other kinds of distortion, not by more of it. Standard WaveNet, the pinned trainer, latency pinned per amp (Forge 91, Basalt 88, Comet 90, Quartz 91, Lantern 89; AG24), 100 epochs, 200 for the four high-gain and lead tops (AG25). Validation ESR per step (gain 0 / 2.5 / 5 / 7.5 / 10), then held out (the tests' DI through our engine against the gray-box source):

| Amp | Validation ESR | Held-out ESR |
|---|---|---|
| Forge | 0.0003 / 0.0003 / 0.0004 / 0.0007 / 0.0008 | 0.0006 / 0.0006 / 0.0007 / 0.0010 / 0.0027 |
| Basalt | 0.0004 / 0.0006 / 0.0007 / 0.0011 / 0.0022 | 0.0012 / 0.0017 / 0.0029 / 0.0051 / 0.0100 |
| Comet | 0.0007 / 0.0008 / 0.0010 / 0.0013 / 0.0028 | 0.0009 / 0.0013 / 0.0017 / 0.0028 / 0.0085 |
| Quartz | 0.0006 / 0.0006 / 0.0005 / 0.0006 / 0.0009 | 0.0013 / 0.0013 / 0.0024 / 0.0038 / 0.0077 |
| Lantern | 0.0002 / 0.0004 / 0.0003 / 0.0003 / 0.0004 | 0.0005 / 0.0012 / 0.0013 / 0.0011 / 0.0017 |

In the app's engine (`make_default_captures.py --measure`, loudness-normalized, the tests' 8 s DI; THD of a 110 Hz sine at -12 dBFS), Gain 0 / 5 / 10: Forge NL -11.0 / -8.0 / -4.9 dB, crest 7.1 / 6.0 / 4.5 dB, THD 0.9 / 1.9 / 28.4% (its 4th-order low cut takes most of a 110 Hz sine away before the clippers); Basalt NL -10.9 / -8.0 / -5.1, crest 8.5 / 6.8 / 6.0, THD 29.8 / 33.5 / 36.7%; Comet NL -8.9 / -6.7 / -4.4, crest 5.5 / 5.0 / 5.1 (the sag holds it nearly flat), THD 12.9 / 17.6 / 14.2%; Quartz NL -8.9 / -6.7 / -4.6, crest 5.3 / 4.8 / 4.4, THD 5.0 / 16.6 / 40.7%; Lantern NL -28.2 / -17.9 / -9.1, crest 15.0 / 12.1 / 7.8, THD 2.2 / 7.0 / 31.7%. Monolith for comparison: NL -9.1 / -6.7 / -4.4, crest 7.0 / 4.3 / 3.1. Every set holds its loudness within 0.09 LU across Gain, neighbouring steps correlate 0.938 to 0.999, and the live sweep matches the always-running blend within -134.8 dB of the peak (`AmpGainTests`). The five sets add 7.3 MB to the bundle (about 1.46 MB each); `content/models` is 11.7 MB in all.

**In the app (since 2026-10-07).** Each is an amp of its own on the Amp page's shelf, with its own knobs and head ("Amp switching"). Before that, **in the app** they weren't slot defaults: the three slots keep Glass, Ember, and Monolith and their heads. The capture menu (right-click at the grille or the model's name) has a **Built-in amps** submenu listing every built-in set, the one playing ticked; choosing one loads it into that slot, which keeps its head and shows the set's name in the info row ("Forge (built in, gain set)"). The list is read from the content folder (`presets::builtInGainSets`), slot defaults first, then by tone type and name, so a set added to `content/models` appears without a code change. Tone match's search is unchanged (another round makes its amp list come from the content folder); tone_bench's oracle can use them (`run.py --amps all`).

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

**Built (Phase 6, first part).** The engine, the Boost, and the Overdrive's Mid Drive and Distortion modes, as specified, with these details settled in the building:
- *Oversampling.* Polyphase IIR halfbands after HIIR: 8 coefficients (transition 0.04) for 48 to 96 kHz, 4 (0.25) for 96 to 192 kHz, 3 (0.375) for 192 to 384 kHz, all better than -99 dB. The up-and-down chain is an allpass whose only cost is 4.2 samples (87 us) of group delay at 4x, so the blocks report zero latency.
- *Engine* (`DriveEngine`). The input is upsampled once; the dry path gets its own downsampler on that same signal, so a parallel blend lines up with the wet path exactly instead of comb-filtering. The tight pre-high-pass is a 12 dB/oct Butterworth on the wet path at the oversampled rate. Circuits work in volts, scaled by the interface's full-scale voltage (+12 dBu, the NAM calibration). A mode switch runs the new circuit unheard for 80 ms from rest, then crossfades over 20 ms with the amp slots' sin law; a 4x/8x change fades out for 5 ms and back in.
- *Circuits* (`DriveCircuits`), per Yeh's decomposition but with the nonlinear stages kept as circuit equations solved per sample (trapezoidal rule, Newton) instead of the plan's static curves, which were a stretch goal. Mid Drive is the TS808 (ElectroSmash; Yeh, Abel, Smith, DAFx-07): input high-passes, the clipping amplifier as the one-state ODE of its feedback network, Yeh's tone stage as a TPT biquad, and the output network. Distortion is the RAT (ElectroSmash's parts list) with the LM308 as a macromodel (a tanh input pair into its 30 pF compensation: 0.3 V/us slew rate, 0.92 MHz GBW, 300 V/mV, output 1.5 V short of the rails), then the diode node and tone filter solved together. Transistor buffers are ideal; diodes are Shockley 1N914s.
- *Validation.* `prototypes/circuits.py`, a SPICE-style simulator, stands in for ngspice (see the decision log). The models at 4x track its 16x renders of the full schematics to -63 dB or better; small-signal responses match its AC analysis within 0.04 dB to 5 kHz.
- *Boost.* Clean: a 1 kHz high shelf (Q 0.5) of the tilt turned down by half of it, so lows and highs move by -/+ half the tilt; bit-transparent at 0 dB. Tight: the tight high-pass (default 150 Hz) and an 800 Hz bell (Q 0.7, default +6 dB), where the Tube Screamer's own response peaks. Screamer: the Mid Drive circuit at drive 0 and tone noon.
- *Controls.* Drive and Tone follow the pots' tapers (audio taper as 10% at half rotation, the TS tone pot linear, the RAT's Filter reversed so tone 1 is brightest); Level is a gain after the circuit with its volume pot at maximum; Mix is linear with Level after it.
- *Aliasing* rises with the input's frequency, because at full drive the clippers flip in less than one 192 kHz sample. Measured at 4x with -12 dBFS tones: -52 to -88 dB up to 1.3 kHz (the top of a guitar's range, the test's limit is -50 dB), -16 to -26 dB at 10 kHz; 8x improves every tone by 6 dB or more. Antiderivative antialiasing for stateful systems (Holters, DAFx-19) is the next step if Sean hears it.

**Built (Phase 6, second part).** The Overdrive's Transparent and Fuzz modes, appended to the mode list (`od_mode` indices 2 and 3), on the same engine:
- *Transparent* is the Klon Centaur as ElectroSmash draws it, stage by stage, each stage driven by an op-amp output so they're solved one after another: the input buffer; the front network (C3, R6 || C5 into the gain stage, and feed-forward network 1, R7 / C16 / R19, the clean 106 Hz low-passed path); the gain stage, a TL072 on 9 V with up to 40 dB at 1 kHz, which at high gain clips on its rails, so it's a static macromodel (finite gain, infinite bandwidth, clamps at +-3 V) rather than an ideal op-amp; the clipper and feed-forward network 2 as one ladder (C9 + R13 into 1N34A germanium diodes with their 7 ohm series resistance, then C10 / R16 and C11 / R15, with the Gain pot's second gang turning the clean path down as the first turns the gain up), eliminated to one diode node per sample; the summing amplifier (R20 || C13, 495 Hz); the active treble shelf (unity below 400 Hz, +17 to -7.5 dB above); and the output with the bypass line's 42 dB-down bleed through the anti-pop resistor. The summing and treble op-amps (U2, on the charge pump's +16.2 / -8.6 V) stay ideal: they peak at 4.5 V in the fixtures. Every capacitor is a trapezoidal companion with its physical state, so knob moves don't jolt it.
- *Fuzz* is the Big Muff Pi, American V3 (1977) as ElectroSmash draws it (10k collector resistors, a real V3 variant), on its real 9 V bias with 2N5088s in Ebers-Moll form and 1N914 pairs. The plan's per-stage decomposition didn't survive measurement: the stages load each other (in the fixtures each base swings 0.5 to 7% of the collector before it, the output stage's up to 120%), so treating them as buffered would get every coupling current wrong by that much. The stages keep their own elements as companions, the networks between them (Sustain, C13 + R12, the tone stack) become 2-ports, and all four stages are solved together each sample, Newton with pnjlim on both junctions of every transistor; the emitter and diode-node rows are eliminated per stage and the rest is a chain of 2 x 2 blocks, so a step costs little more than four separate stages. The bias is the same Newton with the capacitors open (within 1e-12 V of the simulator's). A linear predictor and a 1 uV step tolerance bring it to 3.1 iterations per sample, 4.6% of the deadline at 4x.
- *Validation.* The simulator gained an Ebers-Moll transistor, the TL072 macromodel, and diode series resistance, each checked against hand calculations. Against its 16x renders at 4x: Transparent -71.5 to -107.2 dB, Fuzz -70.9 to -87.2 dB except its brightest chord (-58.7 dB; its limit is -55, see the test), harmonics within 0.009 dB; small-signal responses within 0.035 dB (Transparent) and 0.063 dB (Fuzz, whose steep top end makes its warping larger; it matches its own analog response at the warped frequency within 0.014 dB). Aliasing is now measured over 50 Hz to 20 kHz, because the downsampler's transition band folds harmonics just above 24 kHz to just below it (the Fuzz is bright enough to show it); Fuzz -54.2 dB up to 1.3 kHz at 4x, Transparent -48.3 dB (its limit is -46: a fast op-amp clipping on its rails, ASSUMPTIONS V20), and 8x takes both below -73 dB.

### Gates

Design settled in design review round 4. Sean plays a lot of legato, tapping, and sweeps in heavy parts, so the defaults are tuned for quiet notes surviving, not just tight stops.

**Structure.** Two gates, each with its own detector, knobs, Learn button, and meters, linked by default. When linked, Gate B follows Gate A's detector and gain envelope (one gating decision applied at two points), which prevents two independent releases from compounding and chopping note tails, or stuttering when thresholds differ. Unlinking makes them fully independent. Gate A sits in the pre-FX section (reorderable like any pre-FX block) and cleans up noise going into the amp. Gate B is a fixed stage between the amp and the cab and kills the amp's amplified hiss.

**Detection.** Each gate has a detector source setting, defaulting to the clean DI for both, because the DI separates picking from the noise floor far better than an amplified signal. Gate A can also detect from its own input. Each detector has a sidechain high-pass (default about 100 Hz) so 60 Hz mains hum doesn't hold it open; low notes, even in drop A, still trigger through their harmonics. Peak-based detection, so pick attacks register instantly.

**Envelope.** Attack about 0.5 ms (no lookahead, zero latency). Hold to bridge gaps between tremolo-picked notes. Hysteresis: separate open and close thresholds to prevent chatter, with a wider default gap than a typical metal gate (around 8 dB) so quiet legato and tapped notes don't close it. Release exponential in dB. Range (closed attenuation) defaults to full mute.

**Release modes.** Adaptive release by default, chosen because of Sean's legato and tapping. Classic fixed release is an option. Adaptive: a fast and a slow envelope follower are compared, so a sudden drop (a deliberate stop) closes quickly while a slow decay (a ringing or legato note) closes slowly.

**Learn.** With strings muted, Learn measures the noise floor for 2 seconds and sets the threshold a few dB above it.

**Metering.** Detector level against both thresholds, plus live gain reduction, via atomics to the GUI.

**Out of scope for now.** Hum removal while playing (a comb of notch filters at 60 Hz harmonics), since a gate can only act in gaps. Easy to add later if needed.

**Built (Phase 6).** `src/dsp/Gate.*` is one gate; `src/dsp/LinkedGates.*` is the pair as the chain runs them. Detection: the DI (or own input) through a 24 dB/oct high-pass at 100 Hz, then the peak over a sliding 10 ms window (20 chunks of 0.5 ms). Decision: opens at the threshold, closes below threshold minus hysteresis (8 dB) after the hold (10 ms). Envelope: a raised-cosine attack (0.5 ms) and a release that falls a constant number of dB per second (the knob is the time to fall 60 dB). Adaptive release: the slow follower falls at most 100 dB/s, and the gap between it and the 10 ms peak picks the release, the knob (250 ms) at 8 dB or less, 20 ms at 20 dB or more, log-interpolated between; natural decays measure 0.9 to 3.8 dB of gap and stops 25 to 46 dB. Learn: 2 s of the detector level, 95th percentile, close threshold 6 dB above it. Range -90 dB is a true mute. Gate A, in the pre section, keeps detecting while it's switched off (it handles its own bypass and runs the gate on a copy), so a linked Gate B always has this buffer's curve and Learn works with it off. Gate B, fixed between amp and cab, applies Gate A's curve when linked (the default) and runs its own gate when unlinked; switching fades over 10 ms. Through the whole processor a linked Gate B applies exactly Gate A's gain, bit for bit. Both start off. Parameter IDs: `gate_a_*`, `gate_b_*` (`on`, `threshold`, `hysteresis`, `hold`, `attack`, `release`, `release_mode`, `range`, `detector`, `sc_hpf`, `sc_freq`) and `gate_link`. Block name in saved orders: "gate".

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

**Built (Phase 4).** As specified, with these details settled in the building: the attack and release knobs are time constants of a branching one-pole on the gain reduction in dB (63% of a step in the knob's time); auto release runs a fast smoother (60 ms release) beside a slow one (400 ms attack, 1.5 s release) and takes the larger reduction; auto makeup is the static curve's reduction at -12 dBFS; Pedal mode reads its previous output with a fixed 2 ms attack and a gain computer of slope (R - 1), which settles on ratio R; RMS uses a 10 ms window; the sidechain high-pass is 12 dB/oct. Both instances start switched off. Parameter IDs: `comp_pre_*` and `comp_post_*` (`on`, `mode`, `detector`, `threshold`, `ratio`, `knee`, `attack`, `release`, `auto_release`, `makeup`, `auto_makeup`, `mix`, `sc_hpf`, `sc_freq`).

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

**Built (Phase 4).** Graphic bands are bells at Q 0.9 on exact octaves (62.5 Hz to 16 kHz), designed by the accurate cascade method with two passes and the midpoint targets weighted 0.5 against the centres: in `prototypes/graphic_eq.py`, no Q met +-1 dB with equal weights (best 1.05 dB), and with the weighting the centres land within 0.36 dB. Parametric bands are +-18 dB, Q 0.1 to 18, defaulting to a 100 Hz low shelf, peaks at 400 Hz, 1 kHz, and 3 kHz, and an 8 kHz high shelf. Switching modes crossfades the two banks over 10 ms; a band type change or cut slope change dips to the unprocessed signal for 10 ms each way. Both instances start on and flat (bit-transparent). Parameter IDs: `eq_pre_*` and `eq_post_*` (`on`, `mode`, `g1`..`g9`, `b1_type`, `b1_freq`, `b1_gain`, `b1_q` .. `b5_*`, `lowcut_on/freq/slope`, `highcut_on/freq/slope`).

**GUI (Phase 11).** A visual EQ with a draggable response curve over a live analyzer. The audio thread copies output samples into a lock-free ring buffer; the GUI thread runs the FFT and draws. The analyzer never runs FFTs on the audio thread.

**Testing.**
- **Magnitude response.** Measured against the analytic target at many frequencies for every band type.
- **Graphic accuracy.** Random slider settings land within ±1 dB of the sliders at every band center.
- **Fast-sweep stability.** Rapid frequency sweeps under noise don't blow up or click.
- **Zipper noise.** Gain changes produce no audible stepping.

### Cab

The cab section is an IR system, not a set of modelled cabs. Commercial products' IRs (Neural DSP's among them) are proprietary and not ours to ship, so the app loads user-provided IR `.wav` files, plus the free CC0 IRs bundled in `content/`. Design settled in design review round 3.

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

### Match curve

Added 2026-10-07 (tone_bench Round 1: a high-resolution match curve beats the 5-band match EQ, -0.015 on the matcher's pick, -0.04 once the amp is right, and it absorbs about 40% of the cab error; `docs/TONE_MATCH.md`). `src/dsp/MatchCurve.*`, an `ampsim::Block` like any other.

**What it is.** A smooth magnitude correction of the amp-plus-cab sound, the way a mix engineer's EQ would correct it, given as (frequency, dB) points (tone match fits them). What it realizes (`MatchCurve::targetDb`): the points interpolated in dB on log frequency, clamped to +-15 dB, smoothed by a Gaussian of 1/12 octave (sigma) floored at fs / N = 11.7 Hz (constant-Q above 203 Hz, constant in Hz below: implemented as a fixed Gaussian on a warped axis whose unit is the local sigma), tapered to 0 dB below 25 Hz and above 18 kHz (raised cosines to 12.5 Hz and 22 kHz), then times the amount.

**The filter.** The minimum-phase FIR with that magnitude: the folded real cepstrum (`MinimumPhase.h`, Oppenheim and Schafer), in double precision on a 32768-point grid (8x the FIR, so the cepstrum doesn't wrap), 4096 taps (85 ms) with a raised-cosine fade over the last quarter to exactly 0. Minimum phase puts the energy first (99% within 0.3 to 0.7 ms on the test curves), so there's no added latency and no pre-ringing; linear phase would add 2048 samples. The length is the low-frequency resolution: with the smoothing floor at fs / N, the realized magnitude is within 0.07 dB of the target on the worst curves prototyped (+-15 dB every 1/12 octave), 2.7 dB at half that floor; 8192 taps would move the constant-Q corner to 100 Hz for twice the CPU. Measured in the tests: within 0.17 dB from 50 Hz to 16 kHz on five curves, minimum phase to -127 dB (its own magnitude's minimum-phase rebuild), 1.8 ms to design on the loader thread.

**Running it.** JUCE's zero-latency uniformly partitioned convolution, the cab's engine (brute-force convolution to -127 dB at buffers 1 to 512, 6.5 us per stereo 128-sample buffer, 0.24% of the deadline). A new curve or amount is designed on the loader thread, handed over through `Handoff`, and JUCE crossfades from the old FIR over 50 ms, as for an IR change. No FIR yet: a plain wire.

**Parameters and data.** `match_curve_on` (off) and `match_curve_amount` (0 to 100%, default 100). The amount scales the curve's dB exactly: the processor's timer rebuilds the FIR at the knob's amount, at most every 40 ms (as a moving mic re-morphs), so the knob isn't a `SmoothedValue`; the crossfades between successive FIRs are its smoothing (ASSUMPTIONS MC3). Off, amount 0, or no curve bypass the block through the chain (bit for bit). The curve itself is data: the state's `matchCurve` property and the presets' optional `"match_curve"` key, `[[hz, dB], ...]`, within format 2.

**Position.** A fixed slot right after the cab, before the post section: it corrects what the amp and cab make, as tone match renders it (`ToneMatcher::renderTone`: amp, cab, then the post EQ; the curve goes between the cab and the post EQ). Not under `post_fx_on` or `cab_bypass`.

**For tone match and the renderer.** `AmpSimProcessor::setMatchCurve (MatchCurve::Curve::fromPoints (points))` (message thread; the FIR follows on the loader, covered by `isLoading()`), `getMatchCurve()`; offline, `MatchCurve::designFir (curve, amount)` is exactly what the block convolves with, so any exact convolution renders the same. `ampsim_render --match-curve <file> [--match-amount <%>]`.

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

**Built (Phase 8).** `ampsim::Bloom` holds `Bitcrush`, `Phaser`, and `Flanger`, each with its own on/off (a 10 ms fade of its mix), in any order, with a linear container mix (100% by default) and its own bypass. Bypassed, it keeps its effects running (0.6% of the deadline with all three on, 0.03% with them off) and fades only its output, so coming back on is a pure crossfade with no restarted delay lines or LFOs. A reorder dips the effects to the dry over 10 ms, swaps at the bottom, and fades back, and the dip scales every effect's input as well, not just the first's: without that an effect stores the spliced input (the flanger in its delay line) and plays it back a few milliseconds later, once the wet is fading back in (measured 12 to 15 times the steady level above 5 kHz; with it, at most 1.0 times). Bloom's 0-to-1 fades follow an S-curve (smoothstep, no corner at either end). The bitcrusher quantizes to 2^bits two's-complement levels (step 2^(1 - bits), clipped to -1 and 1 - step, continuous bits 1 to 16), holds on a fractional phase accumulator (100 Hz up to the host rate, no anti-aliasing), adds optional TPDF dither of +-1 step, and has a 12 dB/oct tone low-pass from 1 to 20 kHz that leaves the path at 20 kHz. The phaser is TPT one-pole allpasses with the delay-free feedback loop solved exactly and the wet scaled by sqrt(1 - fb^2) to hold the level: Classic is four stages swept linearly in Hz from 160 Hz to 1.6 kHz by a triangle (a JFET's conductance is linear in its gate voltage), with the later version's feedback as a loop gain of 0.35; Modern is 2, 4, 6, 8, or 12 stages on an exponential sweep (default 100 Hz to 4 kHz at the corner) with resonance from 0 to 0.9; Vibe is four stages at the ratios of the original's capacitors swept by a lamp-and-photocell model (`prototypes/vibe.py`: 25 ms filament lag, light ~ P^(3.4/1.55), CdS conductance ~ light^0.8 rising with 4 ms and falling with 60 ms), so the sweep rises in 43% of the cycle at 1 Hz and 31% at 4 Hz. The flanger is one single-voice `ModulatedDelay` per channel (the engine gained an optional high-pass in its feedback loop): Manual 0.5 to 10 ms swept proportionally (0.1 to 1.9 times Manual at full depth), feedback 0 to 0.95 through the engine's tanh with a 150 Hz loop high-pass, its sign following the polarity switch, and the wet scaled by sqrt(1 - fb^2). Through-zero delays the dry by exactly 240 samples, has no feedback, and makes Bloom report 240 samples of latency whenever the setting is on (whether or not the flanger or Bloom is), switching at the bottom of a 20 ms dip to silence. The phaser's feedback is positive only and the flanger's sign follows its polarity because in this topology an opposite-signed loop flattens the response completely at |fb| = 0.71 (decision log). Parameter IDs are the integrator's (`bloom_*` suggested).

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

**Built (Phase 7).** The study in `prototypes/pitch_detection.py` chose the McLeod Pitch Method over YIN for the coarse, octave-safe stage, plus a fine stage for the readout (synthetic tones; Sean's real DI recordings are still to come, task 7.R). Coarse (`src/dsp/PitchDetector.*`): the DI low-passed by an 8th-order Butterworth at 3 kHz and decimated 4x to 12 kHz; the NSDF with each lag compared over max(lag, W_min) of the newest samples (35 ms for the tuner, 3 ms for the harmonizer); McLeod and Wyvill's key maxima with k = 0.93; parabolic interpolation; a reading needs clarity 0.9. Fine (`TunerAnalysis`): two Q 3 SVF band-passes around the coarse estimate, then interpolated upward zero crossings over max(0.25 s, 12 periods); a fresh pluck (energy doubling over a period) restarts the window. Display: the median of the last 5 readings, note hysteresis (the shown note changes 60 cents away from it), an 80 ms needle smoothing, strobe speed 0.25 periods per second per cent, a -60 dBFS gate (closing at -66 once a note is on), and the last reading held after the note decays. Hosting (`TunerThread`): a 65536-sample lock-free ring from the audio thread, 60 analyses a second on a normal-priority thread, readings to the GUI through a seqlock of atomics. In the app: `tuner_on` engages it (a global switch, MIDI-learnable for a footswitch, never restored engaged from a saved session), `tuner_mute` (on by default) mutes the output with a 20 ms fade, `tuner_a4` sets A4 from 430 to 450 Hz; all three are global settings. The display covers the tabs while the tuner is engaged, in needle or strobe mode (remembered).

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

**Built (Phase 9).** `src/dsp/PitchShifter.*` and `src/dsp/Psola.*`, with the design studies in `prototypes/pitch_shifter.py --study granular|psola`. Both engines read one shared `PitchShifterInput` (a `ModulatedDelay` line for the voices' Hermite reads, a contiguous history and a 12 kHz copy for the correlation searches), so any number of voices share one input.
- **Granular: two heads with aligned splices, not three or four overlapping heads.** Measured: four always-overlapping Hann heads are a static comb (a sine's level varies by 35 dB across frequency at an octave up); two heads that only overlap while crossfading hold every frequency. A plain sawtooth splice moves every partial off pitch by up to 1/(2 x the splice interval) (112.5 Hz up an octave lands 32 cents flat with 20 ms grains), so each splice's jump is searched (WSOLA-style normalized cross-correlation, coarse at 12 kHz then to the sample with a parabola) over 32 ms for a whole number of periods: sines come out within 0.0044 cents at -24 to +24 semitones, power chords within 1.4 cents and major triads within 11, while add9 and minor 7th voicings keep offsets of up to about 60 cents (the known weakness, which no splice offset can fix). A head splices only when it nears the edge of its 37 ms band, so near unison splices are seconds apart and at r = 1 it's a plain delay (bit-exact). Crossfades are correlation-matched (gains divided by sqrt(1 + 2 rho sin cos), Fink, Holters and Zolzer, DAFx 2016) with rho measured on a window the search never saw (the search's own is biased up and dipped noise 1 dB): sine ripple at most 0.37 dB, noise power within 0.36 dB around splices. Trail: about 10 ms up, 20 ms down on single notes (an early splice for rising heads), up to 26 ms down on chords, 19.5 ms at unison. Transients, inherent to any bounded-delay shifter: an octave down loses 26 of 60 short attacks (it plays half the input), an octave up plays every attack twice, 9 ms apart. `GranularShifter` is the self-contained mono version for the shimmer (no assumptions about its input, so it can sit in a feedback loop).
- **PSOLA: one analysis, any number of voices.** `PsolaAnalysis` runs the shared McLeod detector on the DI every 64 samples (two agreeing confident readings confirm a pitch, two unsure ones end it), puts pitch marks on the signal being shifted (the first on the largest sample of the latest period, each next where the period ending there best matches the last one, +-1/8 period, parabola), and exposes a confidence (2 ms ramp), the note count, and the period. `PsolaVoice` lays Hann grains from the mark nearest to (synthesis time - lag) at P/r spacing: two periods long for r <= 1, two synthesis periods for r > 1 (two analysis periods silence a sine an octave up), each short grain minus its windowed mean (otherwise a DC as big as the fundamental), and a closed-form level law for r < 1. Each voice has a log-domain ratio glide, a 2 ms fade on `active`, and its own lag. Measured with the real detector: within 0.32 cents at -24 to +24, energy off the shifted harmonics -38.7 to -47.9 dB, the guitar DI's notes tracked within 0.9 cents median, detection 9 ms (high E) to 25 ms (low E) and trail 1.4 to 1.8 periods (5.2 ms on the high E, 13.1 ms on the A). PSOLA keeps the input's spectral envelope (the amp and cab's colour stays put: a 1.2 kHz resonance stays at 1176 Hz an octave down, where granular moves it to 588 Hz), so up-shifts of bright tones come out a little quieter (1.6 dB at +12, 5.8 dB at +24 on a 1/k tone).
- **For the harmonizer (Phase 10):** prepare a `PsolaAnalysis` with the 110 Hz floor, run up to 4 `PsolaVoice`s off it, gate them with `getConfidence()` / `isConfident()`, and use `getNoteCount()` to catch new notes and `getPeriod()` for the detected pitch.

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

**Built (Phase 9).** `src/dsp/Multivoicer.*`, a stereo post-FX block. The wet is the mono sum of the input on one shared pitch-shifter input; each of up to 8 voices has an interval (semitones and cents, -24 to +24), a delay (0 to 50 ms on top of the engine's trail), pan, level (-60 dB is off), and drift (up to +-3 cents and 0 to 2 ms on smoothed random LFOs). Pans are constant power scaled by sqrt(2) (a centred voice is unity per side, as the cab's mics are), Spread scales them, the wet bus is divided by sqrt(sum of the voices' squared levels) so it holds the input's level at any voice count (band-limited noise within 0.05 dB for 1 to 8 voices), then the optional 12 dB/oct wet high-pass (off, 100 Hz) and the equal-power mix (mix 0 is bit-exact). Mono runs a PSOLA twin of every voice off one analysis whose detector reads the DI (80 Hz floor, so the low E works) and crossfades each voice to its granular twin over 20 ms whenever the analysis isn't sure: on chords, Mono measures exactly like Poly (same level and spectrum), and moving between them doesn't click (largest step 1.011 x steady playing). Starting points: Unison double (+8 and -10 cents, 0 and 7 ms, hard left and right, drift on), Octave stack (-12 and +12), Fifths stack (+7, and +19 at -6 dB), Double + Octaves (the default: the unison double plus -12 and +12 at -6 dB). Every knob is smoothed; voice-count, engine, interval, delay, pan, level, spread, mix, and high-pass changes measured at most 1.033 x the steady-state step. CPU at 128 samples: the default 0.9%, 8 voices Poly 2.1%, 8 voices Mono 2.3% of the deadline; 0 allocations, frees, or locks while every setting changes. Not wired into the processor or the chain yet (no parameter IDs).

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

**Built (Phase 10).** `src/dsp/Harmonizer.*`, in the post section between the compressor and the multivoicer. The PSOLA analysis runs the shared McLeod detector on the DI every 64 samples (one analysis per lowest-note setting: 110, 80, 60 Hz), the NoteTracker turns its readings into notes, `harmony::shiftFor` gives each voice's shift in the key, and PSOLA voices make the harmonies from the processed signal; the dry passes untouched. A voice sounds while the tracker has a note and the analysis tracks it (2 ms fades); a voice starting from silence takes its ratio at once, a sounding one glides to a re-evaluated interval over the glide setting (10 ms default). A4 is the tuner's. Key presets switch with scenes (add the key and scale to what scenes hold). Parameters `harm_*`: `on`, `root`, `scale`, `custom_mask` (12 bits above the root), `out_of_key`, `glide`, `floor`, `level`, and per voice `harm_v1_...` `on`, `mode`, `steps`, `semitones`, `octave`, `level`, `pan`, `humanize`.

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

Built so far (Phase 3; nine amps since 2026-10-07): Program Change 0 to 8 (labeled 1 to 9 on most footswitches) selects amp 1 to 9 (the shelf's order, the player's capture last) immediately on the audio thread (it's heard after the warm-up), read from the raw MIDI bytes so nothing allocates. The slot parameter catches up from a 20 Hz timer, because setting a parameter from the audio thread notifies listeners, which can lock. Mappings, stored in presets: Program Change or a CC selects the amp slot, CCs toggle any block's bypass, a CC toggles the tuner, a CC acts as tap tempo for the delay, and an expression pedal can drive any continuous parameter (volume, wah-style sweeps, harmonizer mix). Mapping is done with MIDI learn: click a control, press a switch, done. Until the GUI exists, mappings live in a config file.

Built (Phase 6, `src/MidiMap.*`): a mapping ties a CC to a parameter as a toggle (every press, a value of 64 or more, flips it; releases are ignored, so switches that send 127 then 0 and switches that send only 127 both work), momentary (follows the switch: on at 64 or more, off below; also the setting for a latching controller that alternates 127 and 0 itself), or continuous (0 to 127 sweeps the parameter's plain value from a min to a max: an expression pedal). One mapping per CC. The audio thread only forwards controller events through a lock-free FIFO (256 events; when full, new ones are dropped and counted, never waited on), and the 50 Hz timer applies them on the message thread, because setting a parameter notifies listeners, which can lock. A mapped switch therefore acts within 20 ms; program changes, tap tempo, and freeze still act on the audio thread at once. MIDI learn: the next controller event maps to the chosen parameter (a toggle for an on/off parameter, continuous over the full range otherwise), and the learning press itself changes nothing. Mappings are saved in the app's state and in presets (the "midi" list); a preset without one leaves the current mappings alone. In the panel, right-click (or ctrl-click) any control: MIDI learn, then for each mapping on it, switch a footswitch between toggle and follow-the-switch, or forget it; the "MIDI..." button lists every mapping and the built-in ones. The controls ignore right-clicks themselves, since a JUCE button would otherwise flip and a linear slider jump.

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

**Built (Phase 5).** As specified. Mix is equal power (the repeats are decorrelated from the dry). Digital time changes crossfade two read heads over 50 ms; analog and tape glide with a 150 ms one-pole limited to 0.25 samples per sample (about 5 semitones at most). The loop has a soft limiter (digital, linear below 0.7) or tanh saturation (analog drive 1.5 with a 3.5 kHz low-pass, tape 1.2 with 5 kHz, wow 0.5 Hz +-1 ms, flutter 6 Hz +-0.08 ms). Ducking reaches its full depth once the input's envelope is at -20 dBFS (5 ms attack, 100 ms release). Bypassed, feedback is capped at 99% so the spillover always dies away. Tempo is a global `tempo_bpm` (saved in presets); tap tempo comes from the GUI or a footswitch CC (`midi_tap_cc`, default 80). Parameter IDs: `delay_*`.

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
| Granular pitch shifter (multivoicer Poly mode, shimmer) | Sweeping | 2 read heads driven by a sawtooth, splices aligned by correlation (Phase 9: 3 to 4 overlapping heads comb) | None |
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

**Built (Phase 5).** On the shared `ModulatedDelay` engine (one per channel, up to 4 voices, Hermite reads, optional bounded feedback; built for the flanger and granular shifter to reuse). Classic: 11 ms swept up to +-4 ms, right LFO inverted. Dimension: an antiphase pair at 7 ms (+-1 ms) mixed as their difference, the SDD-320's matrix: no pitch wobble (0.1 cent), pure width, so its wet cancels in mono by design. Tri: three voices 120 degrees apart, panned left, centre, right. Low-end protection is a crossover (24 dB/oct high-pass into the lines, 12 dB/oct low-pass carried around them) that keeps the lows at unity. Analog: tanh into the lines and a 7 kHz low-pass on the wet; noise optional. Mix is equal power, with the lows at unity. Parameter IDs: `chorus_*`, with tempo sync (`chorus_sync`, `chorus_note`).

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

**Shimmer.** An octave-up pitch shifter in the FDN feedback loop for ambient pads. Built after the pitch shifter exists (Phase 9), not in Phase 5. Built (Phase 9): a granular shifter inside the Room and Hall networks (not the plate). Each sample the line outputs' component along a balanced +-1 unit vector is crossfaded with its shifted version (g up to 0.7), with the shifter's input low-passed at 5 kHz and the new component's power held at most 0.9 of what it replaces by a 50 ms power match, so the shimmer can move energy up in pitch but never add any. Parameters `reverb_shimmer` (0 to 100%, off by default) and `reverb_shimmer_interval` (+12, +7, +19, +24).

**Safety.** Reverb tails are the main denormal risk in the project, so this block carries the decay-to-silence CPU test. Spillover on bypass.

**Built (Phase 5).** Room and Hall are 8- and 16-line FDNs with prime, coprime lengths from 30 to 100 ms times the Size scale 2^(3 size - 2); the Householder matrix is followed by a one-line rotation (still orthogonal and O(N)), because plain Householder at N = 16 leaves 0.875 on the diagonal, which flutters. Per-line decay shelves at 300 Hz and 4 kHz. 10 early reflection taps per side feed 4 lattice diffusers. 4 lines modulated (+-0.5 ms, random). The plate is Dattorro's Fig. 1 with the paper's coefficients scaled to 48 kHz, its one-pole damping replaced by the same low and high shelves (exact at the paper's settings). Freeze mutes the input and makes the tank lossless. Mix is equal power; Size glides limited to 0.25 samples per sample; every engine sits at about the input's level at a 2 s decay. Parameter IDs: `reverb_*`, with pre-delay tempo sync and a freeze footswitch CC (`midi_freeze_cc`, default 81).

**Testing.**
- **Decay accuracy.** T60 measured from the impulse response's energy decay curve (Schroeder backward integration) is within 10% of the setting for low and high bands.
- **Stability.** Matrix orthogonality checked; energy bounded at maximum decay.
- **Freeze.** Energy holds constant for 60 seconds.
- **Stereo.** Low left/right cross-correlation at full width.
- **Denormals.** No CPU spike as a tail decays to silence.
- **Plate.** Matches a Python reference built from the paper's coefficients.

## Global features

Input and output level meters, a CPU meter, and correct latency reporting to the host. Oversampling is per block, not global (see Foundation decisions).

### Output safety limiter

Added 2026-10-04 after Sean's play test ("the clipping is also really high"). The last block in the chain, after the output level (`src/dsp/OutputLimiter.*`): stereo-linked, zero latency, on by default with a -1 dBFS ceiling. Its static curve is a soft clipper (identity up to a knee 2 dB under the ceiling, then a tanh into the ceiling), applied as a gain with an instant attack and a 60 ms release, so only the first quarter cycle of a new peak is shaped and the cycles after it are turned down whole; the output can never reach above the ceiling, and a signal staying under the knee passes bit for bit. Parameters `output_limit_on` and `output_limit_ceiling` (-12 to 0 dBFS) are global settings: presets and scenes never touch them. The Out meter shows it working (an ink cap and "limit" under the bar, held 1.5 s; no warning colours, UH5), and its switch and ceiling are on the Output page. Tests: "Output limiter".

### Gain staging (play test, 2026-10-04)

Sean: "when combined they seem to get too chaotic", "the clipping is also really high with the gain". The "Gain staging" test is the audit: the whole app on the built-in captures and a bundled cab, the test DI at -12 and -6 dBFS peaks, each block switched on alone at its defaults (loudness and peak change, at the amp's input and at the output) and the factory presets scene by scene. It found three level faults, fixed as below (the amp's own Gain and the built-in captures' levels are a separate piece of work):
- The pre compressor's auto makeup restored a -12 dBFS signal, but a DI sits around -21 dBFS between its peaks, so at its defaults it raised the DI's loudness by 4.6 to 6.4 dB and drove every amp after it harder (Modern Prog's amp input peaked at +4.5 dBFS). The pre instance now restores -21 dBFS (`Compressor::preAmpMakeupReferenceDb`): -0.5 to +1.0 dB. The post instance keeps -12.
- The overdrive's four circuits at Level 0 dB changed the loudness by -1.8 (Mid Drive) to +12.3 dB (Transparent), the boost's Screamer by -2.0. Each now carries a fixed unity trim inside its mode crossfade (`Overdrive::unityTrimDb`, `Boost::screamerUnityTrimDb`), measured on the DI at -9 dBFS peaks, so Level 0 dB is as loud as bypassed in every mode (within 0.3 dB at -9; clipping compresses, so +-1.5 to 3 dB at -12 and -6). The renderer and the circuit tests use the bare circuits.
- Bloom's phaser and flanger lost 4 and 3 dB at their defaults (a 50% mix of a signal with a phase-turned copy keeps (1 - a)^2 + a^2 of its power). Their mix is now scaled by 1 / sqrt((1 - a)^2 + a^2) (`mixLevelHold`, Fade.h): -1.0 and -0.1 dB.
Every other block was already within about 1 dB at its defaults; the audit now holds them there.

### CPU (2026-10-08)

Sean: "do a full check on cpu usage and minimize it while still keeping the realtime playability. I want this to be able to run on worse machines while still having the 48khz and 128 sample size smoothness on asio."

**How it's measured.** `AmpSimProcessor::CpuProfile` and `Chain::Profile` time every stage of the callback and every slot of the chain, buffer by buffer, when a benchmark attaches them (the app never does; detached they cost a pointer test per stage). `tests/CpuProfile.*` runs the processor on 10 s of the tests' guitar DI in 128-sample buffers at 48 kHz and reports each block's mean, p99, and worst buffer, plus what isn't a block: the parameter reads and setters (MIDI, the amp choice, every block's knobs), the tuner's and analyzer's taps, the chain's own work (the DI snapshot, the stereo copy, the reorder dips; bypass fades and keep-running copies are counted in their block's row), and the meters, fades, and CPU meter after the chain. The "Full rig" tests print the tables and assert the budget; `ampsim_tests --bench` prints the same on any machine. Rigs: **defaults** (every parameter at its default, Glass, close mic 1 on the longest bundled IR, 1 s), **typical** (each factory preset as it loads), and **heaviest** (every block on at its heaviest: 8x drive with the Fuzz, the Screamer, the multivoicer Mono with 8 voices, four harmonies, Hall with shimmer, both EQs and compressors, both cuts, three mics on 1 s IRs with a stereo room, and the Gain between two steps: two models). Timing was taken with another agent's tone_bench benchmark running on the machine the whole time (it never stopped long enough), so before and after ran interleaved, back to back (main's binary, this branch's, main's, this branch's; the two passes agreed within 0.2% of the deadline), and the absolute numbers are a little high; on a quiet machine they were the same or lower.

**The profile, before and after** (mean us per 128-sample buffer, and % of the 2.67 ms deadline; M5 Pro):

| Block | Defaults | Tech Death | Midwest Emo | Heaviest |
|---|---|---|---|---|
| amp (NAM + tone) | 146 -> 72 | 162 -> 72 | 282 -> 144 (two models: Gain between steps) | 278 -> 145 (two models) |
| overdrive | 52 -> 51 (off: it keeps running) | 51 -> 51 (off) | 52 -> 51 (off) | 180 -> 187 (Fuzz at 8x) |
| boost | 0.3 (off, Clean) | 52 -> 52 (Screamer) | 0.3 (off) | 99 -> 95 (Screamer at 8x) |
| cab | 15 -> 15 (1 s IR) | 16 -> 16 (1 s IR, both cuts) | 5 -> 5 (183 ms IR) | 47 -> 46 (p99 79; three mics, 1 s each) |
| reverb | off | 12 -> 12 | 12 -> 12 | 25 -> 25 (p99 45) |
| multivoicer | off | off | off | 38 -> 38 (p99 93; Mono, 8 voices) |
| harmonizer | off | off | off | 12 -> 12 (p99 20; 4 voices) |
| Bloom (keeps running while off) | 0.9 | 0.9 | 0.9 | 10 -> 10 |
| delay, compressors, EQs, gates, limiter | 4 -> 4 | 4 -> 4 | 12 -> 12 | 21 -> 21 |
| everything that isn't a block | 1.4 -> 1.4 | 1.4 -> 1.4 | 1.4 -> 1.4 | 1.5 -> 1.5 |
| **total, mean** | **219 -> 146 us, 8.2 -> 5.5%** | **300 -> 211 us, 11.3 -> 7.9%** | **367 -> 228 us, 13.8 -> 8.6%** | **716 -> 586 us, 26.9 -> 22.0%** |
| **total, p99** | **9.6 -> 5.9%** | **12.4 -> 8.5%** | **16.8 -> 9.0%** | **30.1 -> 25.0%** |

The other factory presets: Modern Prog 8.7 / 10.3% -> 6.1 / 6.6% (mean / p99), Math Rock 8.2 / 9.8% -> 5.7 / 5.9%, Metal 9.5 / 10.4% -> 6.1 / 6.4%. No buffer over the deadline in any run.

What isn't a block costs almost nothing: the parameter stage 0.9 to 1.1 us (0.04%), the taps 0.0 (nothing while the tuner and analyzer are off), the chain's own work 0.4 us, the meters and fades 0.1 us. The sample-rate guard is one atomic read on the audio thread and a once-a-second check on the message thread.

**The message thread** (`ampsim_tests --bench-gui`: the editor on screen at 1280 x 760, the defaults playing in real time on another thread; the share of one core over 5 s per page): input 1.1%, Pre FX 1.5%, Amp 3.8% (the output spectrum), EQ 1.6%, Cab 1.8%, Post FX 1.7%, Output 0.7%, the tuner engaged 2.3%, Tone match 1.6% of one core; the other threads (JUCE's timers, the loader, the tuner's analysis) 1.0 to 2.6%. The meters, the analyzer, the tuner and the timers are light; on a 4x slower core the busiest page would take about 15% of one core, which isn't the audio thread's core. Not measured on Windows, where JUCE 8 draws with Direct2D (ASSUMPTIONS CPU6).

**What changed** (output kept within a tolerance a test asserts; zero added latency; the real-time test still 0 allocations, 0 frees, 0 locks):

- **The NAM tanh, vectorised** (`src/dsp/NamTanh.*`). `sample` showed `tanhf` as the hottest function in the app: NAM core's `ActivationTanh` calls `std::tanh` per element, 30720 times per buffer for a standard WaveNet. The app now puts Eigen's packet tanh (a [9/8] rational minimax approximation, 4 or 8 floats at a time) into NAM core's activation registry, without touching the submodule. Within 5 ulp of libm's tanhf (3.0e-7) over every float in [-12, 12], identical on 94.7%. A standard model: 145 -> 72 us per buffer, and its p99 181 -> 75 us (libm's tanh has slow paths that made spikes). Output: the example WaveNets within -112.4 dB of NAM core's render tool (the golden limit is -100 dB; the stock tanh is still bit-exact on 7 of 9 models, 14 of 18 renders, checked in the same test), every built-in step within -134.5 dB of the stock tanh (1.2e-6 at most per sample, about -118 dBFS), the tone_bench-style renders within -135 dB (ampsim_render on main against this branch, five rigs: max 2.1e-7). Far below audibility (ASSUMPTIONS CPU4); NAM_UPSTREAM.md 3 is the PR this should become.

- Tried and reverted: the room mic on uniform partitioning instead of its 512-sample head and tail (JUCE's NonUniform). The tail's work every fourth buffer is the cab's p99 in the heaviest rig (79 us against a 46 us mean), but uniform cost more on average and at the p99 too (interleaved: the cab 55 -> 85 us mean, 121 -> 164 us p99 under load).

**Checked and left alone** (no lossless saving worth having):

- Blocks that are off cost nothing after their fade: the chain skips them, and the delay and reverb skip themselves once their tails have died. The exceptions are the blocks that keep running while off on purpose: Gate A (0.1 to 1.1 us), Bloom (0.9 us), and the boost and overdrive, whose circuits keep their capacitors charged (decision log 2026-10-02). The overdrive at its default Mid Drive costs 51 us while off, in every rig where it's off; cutting that changes the sound of switching it on (the options).
- Coefficients: every knob-dependent filter already recomputes from smoothed values every 32 samples, never per sample (EQ, amp tone, drive tone and tight, the boost's filters); the amp tone and the EQs cost 1 to 3 us.
- The drives: the circuits' Newton solves are 90% of their time, the halfband resamplers 7%; the solvers are converged to 1e-9 and any shortcut changes the output.
- The convolutions: JUCE's uniform partitioned convolution at 128 samples uses a 512-point FFT (4x the block), already its cheapest; a 1 s close mic is 10 to 15 us.
- Denormals: `juce::ScopedNoDenormals` around every callback (FTZ and DAZ on x86, FZ on arm64); the reverb decay test holds it.
- The stereo path: one copy at the cab; blocks after it process both channels in place.
- Compiler flags (ASSUMPTIONS CPU7): arm64 -O3 with LTO for our code, NAM core and demucs.cpp -O3; Windows MSVC /O2 /Ob2, JUCE's /Ox, /arch:AVX, /fp:precise. /fp:fast and -ffast-math stay off (they reorder float math: no longer bit-identical between builds, and they can break the NaN and denormal handling). Forcing Eigen's lazy small products (EIGEN_GEMM_TO_COEFFBASED_THRESHOLD) made a model 2.4x slower.

**Options that change the sound** (measured, not applied; Sean decides):

| Option | Saves (this Mac, per 128-sample buffer) | Costs (measured) |
|---|---|---|
| **Lite captures for the built-in amps** (NAM's lite WaveNet: 12 and 6 channels, fewer layers' worth of work) | A model 80 -> 56 us (30%; 0.9% of the deadline per model, 1.8% between steps). On a 5x laptop, 4.5% of the deadline per model | Monolith gain 5 (200 epochs, like the standard): validation ESR 0.00072 (standard 0.00051), held out on the tests' DI 0.0089 against the gray-box source (standard 0.0031), 0.0071 against the standard capture (-21.5 dB). Glass gain 5: validation 0.00014 (standard 0.00023), held out 0.0003 (the same as the standard), 0.0004 against the standard (-34 dB). Clean amps lose nothing measurable; the high-gain ones roughly triple their error. Retraining the 40 steps: about 7 h of the trainer on this Mac's GPU |
| **Feather captures** (NAM's feather: 8 and 4 channels) | A model 80 -> 33 us (59%; 1.8% of the deadline per model) | Monolith: validation 0.00089, held out 0.0109 (3.5x the standard's), 0.0090 against the standard (-20.4 dB). Glass: validation 0.00029, held out 0.0006 (2x), -32.5 dB against the standard |
| **The drives at 2x instead of 4x** (a third choice for `drive_oversampling`, or the default) | Per active drive at Drive and Tone noon: Mid Drive 56 -> 28 us, Distortion 91 -> 56, Transparent 101 -> 56, Fuzz 125 -> 80, the Screamer about half (1.0 to 1.7% of the deadline each) | Aliasing of full-drive tones up to 1.3 kHz: Mid Drive -47.4 dB (4x: -65.4), Distortion -34.7 (-52.3), Transparent -38.4 (-48.3), Fuzz -35.9 (-54.2); at 3 to 5 kHz 10 to 17 dB worse than 4x (-16 to -30 dB). The long-term spectrum on the DI stays within 0.23 to 0.53 dB of 4x in every octave. The plan's limit at 4x is -40 dB up to 1.3 kHz, so Distortion, Transparent, and Fuzz at 2x would fail it |
| **Stop a drive while it's off**, and warm it up unheard on the live input when it's switched on (like an amp switch) | Everything an off drive costs: the overdrive's default Mid Drive 51 us (1.9% of the deadline) in every rig where it's off, which is every factory preset; about 140 us for an off Fuzz at 4x; an off Screamer boost 52 us | The switch lands a warm-up later, and the circuit's output differs from one that ran all along until its capacitors have caught up. Largest difference after switch-on (through the 10 ms fade), re. its peak, warm-ups 0 / 20 / 50 / 100 / 200 ms: Mid Drive -27 / -34 / -47 / -54 / -72 dB; Transparent -31 / -36 / -49 / -53 / -72; the Screamer -28 / -35 / -47 / -54 / -72; Distortion -7 / -8 / -16 / -31 / -60; Fuzz -4 / -5 / -9 / -18 / -28 (its bias network settles slowest). 200 ms makes it inaudible for every mode but the Fuzz |
| **The Gain on the nearest step only** (no blend between two models) | Between steps, one model instead of two: 72 us (2.7% of the deadline); Midwest Emo's Glass at Gain 6.25 is the factory case | The Gain moves in five steps (0, 2.5, 5, 7.5, 10) instead of continuously, or each step's model takes the in-between Gains as an input trim (a single capture's compensated trim, AG8), which is extrapolation |
| **A CPU saver mode** (a global setting, never in presets): the three above that keep the knobs' behaviour closest (drives stopped while off with a 200 ms warm-up, the drives at 2x, the nearest step) | Defaults 146 -> about 95 us (5.5% -> 3.6%); Midwest Emo 228 -> about 105 us (8.6% -> 4.0%); Tech Death 211 -> about 135 us (7.9% -> 5.0%); the heaviest rig 586 -> about 340 us (22.0% -> 12.8%: its 8x drives at 2x, one model). On a 5x laptop the factory presets' p99 would go from about 45% to about 25% of the deadline | The three costs together, only while it's on. The sums are of the measured block costs, not a measured mode (it isn't built) |
| **The harmonizer and the multivoicer** (already measured; no change proposed) | On the defaults: the harmonizer 12 us (1 to 4 voices cost the same, the pitch tracking dominates; p99 20 us); the multivoicer Poly with its 4 voices 25 us (p99 68), 8 voices 31 (p99 72), Mono 4 voices 28 (p99 75), Mono 8 voices 38 (p99 86) | Their p99 is 2 to 3 times their mean (the analysis runs in bursts). Fewer multivoicer voices saves about 1.5 us each |
| **Windows built for AVX2 with FMA** (`/arch:AVX2`; Eigen then uses FMA) | Not measured (no x86 machine here). Eigen's float GEMM gains most from FMA; NAM's matrix products are now the bulk of a model | The app would crash at launch on CPUs without AVX2: Intel before Haswell (2013) and AMD before Excavator (2015). A safer variant is two builds or a runtime choice of NAM core, which is real work |
| **256-sample buffers on a weak machine** (no code) | The deadline doubles to 5.33 ms; the per-buffer cost a little less than doubles | 2.7 ms more latency each way |

**How the buffers are fed (2026-10-08, after the merge).** On the merged main, on an idle Mac, the budget's p99s failed every time (defaults 10.6 to 11.0% against 10, Midwest Emo 16.2 against 12) while the means matched: the profile ran flat out on an ordinary test thread, and an idle Mac stretches 1 to 2% of such a thread's buffers by up to 1.8x (every block's p99 the same multiple of its mean: the whole thread slower, a scheduling or clock effect, not the work). Fed like a device instead, on a real-time thread (JUCE's `startRealtimeThread`: on macOS a time-constraint thread with the 2.67 ms period) sleeping between buffers, the same rigs cost 3 to 4 times as much on an idle Mac (defaults 20%, the heaviest 47 to 59%): the scheduler ran the bursts on a low clock. Core Audio's IO thread belongs to the device's audio workgroup, which tells the scheduler each cycle's deadline, so the profile now does the same (`AudioWorkIntervalCreate`, `os_workgroup_join`, `os_workgroup_interval_start` and `finish` around each buffer; `cpu::Pacing::device`). Even then the OS picks the clock to meet the deadline with the least power, so the share of the deadline depends on how busy the machine is, not on how close it is to dropping out. Three Full rig runs each (mean / p99, the range over the runs):

| Rig | Flat out, quiet | Flat out, loaded | Device, quiet | Device, loaded |
|---|---|---|---|---|
| Defaults | 5.6-5.9 / 9.1-10.5 | 6.1-6.7 / 8.4-10.3 | 7.9-8.5 / 13.5-17.3 | 6.3-6.7 / 7.9-8.9 |
| Tech Death | 8.0-8.3 / 13.7-14.2 | 8.6-8.9 / 11.0-11.8 | 10.6-12.9 / 14.8-20.3 | 8.7-9.3 / 10.4-11.3 |
| Midwest Emo | 8.7-9.0 / 13.8-15.2 | 9.6-10.0 / 12.0-13.7 | 12.2-13.1 / 18.4-19.6 | 9.2-9.4 / 10.4-10.7 |
| Heaviest | 22.1-22.9 / 29.0-29.2 | 25.6-28.9 / 33.5-44.5 | 29.8-30.9 / 41.7-46.3 | 24.7-27.1 / 29.5-34.8 |

("Loaded": six busy loops on the 15 cores, load average 8 to 10. "Flat out" from here on is the highest-priority ordinary thread, the user-interactive QoS class, which keeps it on a performance core.) No p99 was stable to better than about 1.5x between quiet and loaded runs, on either kind of thread, so the p99 can't carry a tight limit on this Mac. The means are stable to about 15% flat out, and the flat-out mean is the work at full clock, which is what a slower machine scales. So the budget is asserted flat out, on the means, with p99 limits set from the worst of the six flat-out runs plus a margin, to catch a regression in the tail without failing on the scheduler; the device-paced defaults and heaviest rig are printed beside them (not asserted), and `--bench` prints both.

**The budget** (`tests/CpuProfile.h`, asserted by "Full rig", times `cpuBudgetScale()`; ASSUMPTIONS CPU1, CPU2, CPU9). "Worse machines" means a 4-core x64 laptop from about 2017, taken as 3 to 5 times slower per core than the M5 Pro for this code (Sean's Windows PC measured 1.75x). An ASIO callback at 128 samples shouldn't use more than about half of its 2.67 ms on average, so the driver, Windows' DPC latency spikes, and the slower buffers keep their share. Targets on this Mac, flat out, mean / p99 of the deadline:

| Rig | Budget | Measured (the six runs above) | The mean at 5x slower |
|---|---|---|---|
| Defaults | 8 / 13% | 5.6-6.7 / 8.4-10.5% | 30 to 34% |
| Every factory preset (typical) | 11 / 18% | 5.9-10.0 / 6.9-16.5% | 30 to 50% |
| Heaviest | 30 / 45% | 22.1-28.9 / 29.0-44.5% | 110 to 145%: not on the old laptop |

The heaviest rig is for faster machines: at 2x (Sean's PC) its mean is about half the deadline; on the old laptop it needs 256 samples or a lighter rig.

**The benchmark on another machine.** In a Release build of the tests on the machine to check (Windows: `cmake -B build -G "Visual Studio 17 2022" -A x64`, `cmake --build build --config Release --target ampsim_tests`):

```
build\ampsim_tests_artefacts\Release\ampsim_tests.exe --bench          (macOS: build/ampsim_tests_artefacts/Release/ampsim_tests --bench)
build\ampsim_tests_artefacts\Release\ampsim_tests.exe --bench 30       (30 s of DI per rig instead of 10)
build\ampsim_tests_artefacts\Release\ampsim_tests.exe --bench-gui      (opens the editor; the message thread per page)
```

It prints the CPU, the instruction set the build targets, a per-block table for the defaults, each factory preset, and the heaviest rig, and a summary of each rig's mean and p99 against the 128-sample deadline. Each rig runs twice: flat out on a high-priority thread (the per-block table and the summary's first pair) and paced like a device on a real-time thread (the summary's second pair; on Windows a time-critical thread, as ASIO's, without a work interval). Read the flat-out summary: a mean under 50% and a p99 under 60% of the deadline mean that rig plays at ASIO 128 on that machine with room to spare. The paced pair shows what the OS's power management makes of it, and should stay well under 100%.

### Presets and scenes

Design settled in design review round 15.

**Preset vs global.** A preset holds every parameter, each amp's capture, cab IRs and mic positions, pre and post chain order, Bloom order, MIDI mappings, harmonizer key presets, custom scales, tempo, and its scenes. Global settings (never changed by loading a preset): input calibration, audio and MIDI devices, A4 reference, tuner mute preference, CPU fallback mode, window size and UI scale.

**Format.** One JSON file per preset with a `format_version` field, stored in `~/Library/Application Support/BellyDSP/presets/` (`AmpSim` before the rename; the first launch copies it over).

**Evolution.** Missing parameter IDs take defaults; unknown IDs are ignored with a warning; structural changes bump `format_version` with one small, pure, tested migration function per step. A set of golden preset files lives in the test suite and must load identically on every build.

**Missing files.** Model and IR references store a path relative to the `models/` or `irs/` root plus a content hash. A broken path is relinked by searching the library for the same hash; a truly missing file loads as a "missing" slot without failing the preset.

**Loading.** The background thread parses, loads, and prewarms everything, then the audio thread swaps it in with a short fade (between songs, not seamless).

**Scenes.** 8 per preset, switched instantly from the footswitch since everything is already loaded. A scene snapshots the amp that plays (amp_model; the slot until 2026-10-07), every block's bypass state, and a chosen set of parameter values (such as delay mix or reverb level). One stomp moves a song from verse to chorus.

**Editing.** Undo/redo and A/B compare, both entirely on the GUI thread.

**Factory presets.** The five style presets (see Amps).

### GUI

Design settled in design review round 15. Sean directs the visual design; the agent builds it. The goal is the most polished result, not toolkit learning.

**Toolkit.** JUCE's own Component system throughout, from the basic panel to the final UI (it replaced egui when the project moved to C++). JUCE is the most widely used audio plugin GUI framework, so an agent builds it reliably, and its custom painting (Graphics, paths, images, LookAndFeel) gives full visual control. Polish comes from custom-drawn widgets and a consistent design system, not from the toolkit's defaults. JUCE also has a web-view UI option (HTML/CSS) if more visual freedom is ever worth it.

**Design system.** The UI handoff in `docs/ui/amp-ui-handoff/` is the design (adopted 2026-10-03; `docs/UI_DESIGN.md` lists where this build differs, with the ASSUMPTIONS UH entries). Everything is drawn in code; no artwork or trade dress copied from other products.

**Layout.** A fixed 1280 x 760 canvas scaled to the window and letterboxed.
- **Top bar.** The brand, the preset's name and tag with previous and next arrows (a click on the name opens the browser), Save, the Tuner button, and the In and Out meters.
- **Main area.** One page at a time: Input, Pre FX (tabs, drag to reorder), Amp (the shelf of mini heads, one per amp, where the amp tabs were; the head of the playing amp, the info row, the output spectrum, and the shared strip), EQ, Cab (the library, the speaker with two draggable mics, the mic panel), Post FX, Output (level, A/B, tempo, CPU, scenes, the footswitch's controllers), and the tuner.
- **Signal chain.** Along the bottom: Input, Pre FX, Amp, EQ, Cab, Post FX, Output; a click opens a page, a block's dot bypasses it.

Dark theme, crisp on Retina, resizable (the canvas scales). Knobs: vertical drag, Shift for fine, double-click to reset, scroll wheel and arrow keys, right-click for MIDI learn. Undo and redo from the keyboard.

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

### Tone match

Added 2026-10-04 (Sean's spec; `docs/TONE_MATCH.md` has the method, the math with sources, the measured accuracy, and how to use it). "Take in a song and recreate the tone of the lead guitar." A song holds only the rig's output mixed with everything else, so the real amp can't be recovered: tone match finds the closest fingerprint with BellyDSP's own controls, and the UI says so.

- **What it sets.** An amp (since 2026-10-07: the built-in amp whose set matched, or the player's capture), its Gain (input trim) and five tone knobs, one of the 21 built-in cabs in close mic 1, and a match EQ (the post EQ's five parametric bands, fitted to what's left, capped at +-12 dB). Apply writes them as normal parameters in one undo step, so presets save the result, and switches off the pre effects that color the tone before the amp (pre compressor, boost, overdrive, pre EQ; the gates stay).
- **How.** The chain is linear after the amp model, so only (slot, Gain) is rendered: a 6 dB grid, then 3 and 1.5 dB steps for the best two slots, through private `AmpSection` instances on worker threads. Cabs are screened on predicted spectra and the best four per point convolved; the tone knobs are fitted by ridge least squares and Nelder-Mead; the score is a perceptually weighted long-term (or DTW-aligned) spectral error plus four distortion features.
- **Modes.** Same part (the player played the target's part: chroma DTW, frame by frame) and Anything (long-term statistics).
- **Stages.** A: `prototypes/tone_match.py` and its synthetic study (the gate: it had to tell the amps apart and beat the defaults; it did). B: `src/tonematch/` (golden-tested against the prototype), the DI recorder on the audio thread (lock-free, preallocated, in the real-time safety test), the session, and the page. C: separation with Demucs v4 (htdemucs_6s) through demucs.cpp (`third_party/demucs.cpp`), the weights downloaded on first use.
- **Rules kept.** Nothing new on the audio thread but the recorder's wait-free copy; matching and separation run on their own threads, cancelable, with progress; no recorded music in the repo.

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
2026-10-01: Amps stay at three always-running slots; Sean's five styles (Polyphia, CHON, Tech Death, Metal, Midwest Emo) become five style presets that load captures into the slots (the presets are now Modern Prog, Math Rock, Tech Death, Metal, Midwest Emo: see 2026-10-03).
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
2026-10-02: Phase 5 built: delay, chorus, reverb, and basic presets as specified (details under each). Mix laws are equal power for all three (each wet is decorrelated from its dry); the chorus's was changed from linear when integrating, because linear tilted the tone 3 dB toward the bass at 50%. The chorus's Dimension mode is pure width (its wet cancels in mono, as on the original hardware). The plate's decay runs 6 to 20% long at short settings (its 725 ms loop), held to 25% instead of the plan's 10% until it's calibrated. Basic presets store absolute paths; loading one while playing fades out, applies, and fades back in.
2026-10-01: Phase 4 built: compressor and EQ as specified (details under each). Graphic EQ: Q 0.9 with centre-weighted least squares, because the plain accurate cascade design missed the +-1 dB target. Effects start switched off except the EQs, which start flat; the plan implied the pre compressor starts on, which would push every amp harder in a fresh session (recorded in ASSUMPTIONS.md). Reordering a section dips it to its own input for 10 ms each way, because a true crossfade between two orders would need a second copy of every block.
2026-10-01: Input calibration from `input_level_dbu`, on by default with the interface at +12 dBu (the Solo's instrument input at minimum gain); the gain lives with each loaded model, and normalization measures the calibrated input.
2026-10-01: Movable mics built as planned (cab packs, position map, minimum-phase morphing, 40 ms re-morph limit), with packs placed by `cabpack.json`, file names, or file order, and inverse-distance weights for scattered points. IR loudness matching switched from measuring 4 s of white noise to computing its exact expected value from the IR's K-weighted energy: within 0.034 dB of the measurement and 500x faster, which re-morphing every 40 ms needs.
2026-10-02: Phase 10 built: the harmonizer as specified, on the PSOLA engine and the NoteTracker. The dry is never delayed; voices fade in once a note is confirmed, which puts the harmony 1 to 3 ms behind the plan's per-string budgets (high E 9.3 ms against 6 to 8; low E 28 ms against 25 to 30). Key presets are scenes holding the key and scale rather than a separate list.
2026-10-02: Phase 9 in the chain: the multivoicer sits in the post section between the compressor and Bloom (`mv_*`, its starting points written into the parameters from a menu), and the shimmer lives in the FDN. The shimmer's first design assumed the shifted signal was uncorrelated with what it replaced and energy-neutral; measured, a frozen tail drained 50 dB a second with all lines scaled, then grew 24 dB once only the projection was crossfaded, because the cascade correlates with itself and the shifter overshoots near Nyquist. The bound is now enforced: a 5 kHz low-pass on the shifter's input and a power match with a 0.9 margin (a ratio of averages is biased high).
2026-10-02: Phase 11 editing and factory presets: undo/redo through the parameter tree's UndoManager (a step per mouse press, cleared by a preset load), A/B of the settings (not the files), and the five style presets as JSON in `presets/factory`, embedded in the app, with notes saying which captures to load.
2026-10-02: Presets format version 2 (Phase 11): captures and IRs are { path, hash, size }, the path relative to the library root (~/Library/Application Support/AmpSim/models or /irs) when the file is inside it; a moved file is found again by size, then a 64-bit FNV-1a content hash; a missing one loads as an empty slot with a warning. Version 1 migrates with a pure function. Scenes: 8 per preset, each the amp slot, every block switch, and a chosen set of knobs; recalled from the footswitch with `midi_scene_cc` (default 70, value 0 to 7) through the timer, so within 20 ms.
2026-10-02: Bloom in the chain (Phase 8): the post section is now EQ, compressor, Bloom, chorus, delay, reverb (the harmonizer and multivoicer will go between the compressor and Bloom). Parameter IDs `bloom_*` (`on`, `mix`, `crush_*`, `phaser_*`, `flanger_*`, with tempo sync for the phaser and flanger rates); the order inside Bloom is saved by effect name in the state and in presets ("order": { "bloom": [...] }); the processor's timer passes Bloom's latency (240 samples while through-zero is on) to the host.
2026-10-02: Reorders fade every block's input as well as the section's output (an S-curve of the dip, w^2 (3 - 2 w)), so a delay or reverb never stores the splice between the two orders' signals. Found by the Bloom agent inside Bloom and confirmed in the chain: swapping chorus and delay left a click in the delay's repeats at 131x the steady curvature; now 0.27x. Cost: the processed path dips by up to about 2.6 dB with one block running, 6.6 dB with five, for about 20 ms, under the crossfade to the dry input.
2026-10-02: Tuner built (Phase 7). McLeod Pitch Method for the coarse stage, chosen over YIN by the prototype study on synthetic tones; a band-pass zero-crossing fine stage for the readout (worst 0.083 cent on 52 synthetic tones from 30 Hz to 1.3 kHz). Each lag of the NSDF is judged on max(lag, W_min) of the newest samples, which lets the harmonizer use a 3 ms W_min and read the high E in about 6.5 ms. In the app the tuner is engaged by a global `tuner_on` switch that a session never restores, mutes the output with a 20 ms fade (switchable), and takes A4 from 430 to 450 Hz.
2026-10-02: Boost and overdrive in the chain (pre section: gate, compressor, boost, overdrive, EQ). They keep running while switched off, because a circuit switched on cold differs from a running one by -8 dB re. its peak for the first 30 ms (measured), which the 10 ms bypass fade doesn't hide; the cost is their CPU while off. The circuits' volts follow the interface level setting (`input_level_dbu`), the same number the NAM calibration uses. Oversampling (`drive_oversampling`, 4x or 8x) is a global setting, not part of presets. Parameter IDs: `boost_*` (`on`, `mode`, `level`, `tilt`, `tight_freq`, `mid`) and `od_*` (`on`, `mode`, `drive`, `tone`, `level`, `mix`, `tight`, `tight_freq`); block names "boost" and "overdrive". The `od_mode` list only grows at the end.
2026-10-02: Gates built as specified (details under Gates). Learn's margin sits on the close threshold, since that's the one noise must stay under. Gate A detects even while switched off, through the Block's own-bypass hook, so a linked Gate B never applies a stale curve. Both gates start off like every effect; the default threshold (-55 dBFS) suits a quiet DI and Learn is the intended way to set it. Saved section orders that predate a block get it where it breaks the fewest pairs of the default order, so a Phase 5 "eq, comp" becomes "gate, eq, comp".
2026-10-02: MIDI mappings (Phase 6): toggles flip on every press and ignore releases, momentary follows the switch (which also covers latching controllers), continuous sweeps a plain-value range; mapped CCs are applied by the 50 Hz timer (within 20 ms) through a lock-free FIFO, while program change, tap tempo, and freeze stay sample-accurate on the audio thread. Presets carry their mappings; a preset without any leaves the current ones alone.
2026-10-02: Pitch shifter (Phase 9): the granular engine uses two read heads with one crossfade at a time instead of 3 to 4 overlapping ones (overlapping heads measured as a 35 dB static comb), and every splice's jump is chosen by normalized cross-correlation over a 32 ms range so splices are phase-continuous (a plain sawtooth puts partials up to 1/(2H) Hz off pitch). The range is the trade between chords and trail: +-16 ms keeps power chords and major triads in tune with about 10 ms of trail up and 20 ms down (+-12 ms left triads 56 cents off; +-20 ms trails 34 ms). Crossfades are correlation-matched (Fink, Holters, Zolzer). PSOLA grains are two synthesis periods long for upward shifts (two analysis periods silence a sine an octave up), minus their windowed mean (or they add a DC as big as the fundamental). Recorded in ASSUMPTIONS S1 to S5.
2026-10-02: Multivoicer (Phase 9): the wet is normalized by sqrt(sum of squared voice levels) so the equal-power mix holds the level at any voice count; Mono falls back to each voice's granular twin (20 ms crossfade) whenever the PSOLA analysis isn't confident, so chords sound like Poly instead of garbage (the harmonizer will mute instead); Mono's detector floor is 80 Hz so the low E works. Recorded in ASSUMPTIONS S6 to S13.
2026-10-01: Cab mixing: constant-power panning scaled by sqrt(2), so a centred mic is unity on each side, matching the cab's passthrough and the chain's own mono-to-stereo copy when the cab is bypassed; hard-panned is +3 dB on that side. A mic whose first IR is loading stays out of the mix until JUCE's engine is in plus a 60 ms settle, then fades in over 20 ms, because JUCE starts each convolver from an identity engine that would pass raw amp fizz. The room uses JUCE's non-uniform (head/tail) convolution with a 512-sample head, still zero latency. Cuts are SVF Butterworth sections (one for 12 dB/oct, two with Q 0.541 and 1.307 for 24) with a 10 ms on/off crossfade.
2026-10-02: Drive validation uses `prototypes/circuits.py`, a SPICE-style simulator written for the purpose (MNA, trapezoidal companions, Newton with SPICE3's junction limiting, Shockley diodes, ideal op-amps, an LM308 macromodel), instead of ngspice, which isn't installed (ASSUMPTIONS W3). It's checked on circuits with known answers before rendering the golden fixtures, and installing ngspice later would only add a cross-check.
2026-10-02: The drive circuits' nonlinear stages are solved per sample (trapezoidal rule, a few Newton iterations at 4x) rather than as static curves: the capacitors around the diodes make clipping frequency dependent, and at 1.8 to 3.2% of the deadline it's affordable. Linear stages stay analytic transfer functions from the schematic, as planned.
2026-10-02: The RAT's LM308 is a macromodel with finite bandwidth, not an ideal op-amp plus a slew limit: a tanh input pair into the compensation capacitor gives the 0.3 V/us slew rate and the 0.92 MHz gain-bandwidth from one mechanism. At full gain the closed loop rolls off above about 400 Hz, which is a large part of the pedal's sound; an ideal op-amp would leave it 15.9 kHz wide.
2026-10-02: Oversampler stages fixed at 8 / 4 / 3 coefficients (48-96, 96-192, 192-384 kHz), 4.2 samples of group delay at 4x. The drive blocks' dry path for Mix is aligned to it, so the blend doesn't comb-filter; the blocks report zero latency.
2026-10-02: Drive mode switches warm the incoming circuit up unheard for 80 ms before the 20 ms crossfade; started cold, a circuit clips around the wrong bias for tens of milliseconds (measured: -5.7 dB from an ideal crossfade without the warm-up, -33 dB with it).
2026-10-02: Overdrive Transparent (Klon Centaur) and Fuzz (Big Muff Pi V3) built. The Klon's gain stage is a TL072 on 9 V that clips on its rails, so it's a macromodel with clamps, not an ideal op-amp. The Big Muff's four transistor stages are solved jointly each sample instead of decomposed per stage as planned, because they measurably load each other; on its real bias, with Ebers-Moll 2N5088s. Drive aliasing is measured over 50 Hz to 20 kHz, and the Transparent's 4x aliasing limit is -46 dB (ASSUMPTIONS V20).
2026-10-02: Phase 8 built: Bloom as specified (details under Bloom). Bypassed, Bloom keeps its effects running so coming back on is a pure crossfade; a reorder or a through-zero switch fades every effect's input, because an effect with memory otherwise replays the splice; Bloom's fades follow an S-curve. The phaser's feedback is a 0 to 0.9 resonance and the flanger's feedback takes its sign from the polarity switch, because a loop of the opposite sign to the wet flattens the response at |fb| = 0.71 (measured 0.00 dB of ripple, against 15 to 27 dB with matched signs). Through-zero latency depends only on its own setting, so footswitching the flanger or Bloom never changes it; switching the mode dips the output for 20 ms.
2026-10-02: Phase 11 GUI built from UI_DESIGN.md on JUCE's Component system (`src/ui/`): the design tokens in `Theme.h`, one LookAndFeel, parameter-bound knobs, fields, faders, and switches, a two-row chain strip whose drag-to-reorder calls `setSectionOrder()` like the old order strip (ASSUMPTIONS U9), a page per block in cards, the tuner overlay, and a UI scale that resizes the window around the same layout (U12). The meters and the CPU meter are atomics and the EQ analyzer a lock-free ring the GUI reads 30 times a second, all inside the real-time safety test. The pre EQ's analyzer reads the guitar as it arrives rather than the amp's input, because tapping inside the chain would change `src/dsp` (U11).
2026-10-03: The UI handoff (`docs/ui/amp-ui-handoff/`) adopted as the GUI's design source of truth, replacing the first UI_DESIGN proposal: a fixed 1280 x 760 canvas, the top bar, the bottom signal chain, the amp page with three head materials, the cab and tuner pages, and the real output spectrum where the mock drew a fake tone curve. Undesigned pages keep their contents, restyled (ASSUMPTIONS UH1 to UH19).
2026-10-03: New parameter IDs `amp_bypass` (the amp crossfades out to its input; its captures keep running underneath), `pre_fx_on` and `post_fx_on` (each block runs when its own switch and its section's are on, through the block's own crossfade). Gate A publishes an open flag (its gain above -6 dB) for the strip's light.
2026-10-03: Per-amp-slot cab assignment with "Follow amp choice": a cab picked on the cab page is assigned to the playing slot, and while following, a slot change loads that slot's cab into close mic 1. Saved in the state and in presets as the optional keys `cab_assign` (FileRefs) and `cab_follow`; the preset format stays at version 2.
2026-10-03: Fonts bundled: Geist (300, 400, 500, 600) and a Fraunces SemiBold Italic instance, both SIL OFL, embedded as BinaryData; text drawn without macOS font smoothing to match the reference.
2026-10-03: Distribution (docs/RELEASING.md, ASSUMPTIONS DS1 to DS21). Releases are signed with a free self-signed code-signing certificate, not a paid Developer ID: every build gets the same designated requirement (the certificate pinned), so the microphone permission survives updates and Sparkle accepts them; friends approve the app once with Open Anyway. No hardened runtime with it (dyld refuses Sparkle.framework across "different Team IDs"). A Developer ID path (hardened runtime, entitlements, notarization) is written behind AMPSIM_SIGN_IDENTITY, untested.
2026-10-03: Releases go to a separate public, releases-only GitHub repo (`RELEASES_REPO` in tools/release/release.conf, assumed SeanSnaider/amp-sim-releases); the source repo stays private. The apps' feeds are its "latest release" asset URLs (`.../releases/latest/download/appcast.xml`, `appcast-windows.xml`), so publishing a release is the whole deployment. Uploads use the GitHub REST API with curl and a token from the login keychain.
2026-10-03: Updates: Sparkle 2.10.0 on macOS (silent background download, installs on quit, daily checks, Ed25519-verified; sandbox-only XPC services left out) and WinSparkle 0.9.4 on Windows (daily checks, an "install?" dialog, then a silent per-user Inno Setup install that reopens the app). One Ed25519 key signs both. Both pinned by SHA-256 in tools/deps.conf. Sparkle 2.10 needs macOS 12, so builds with the updater target macOS 12; dev builds stay at 11.
2026-10-03: Release builds are universal (arm64 + x86_64, AVX baseline for the Intel slice) in their own build-release/ directory, made from scratch by tools/release/release.sh, which also runs the full test suite and refuses to release on a failure. Dev builds stay native, without the updater.
2026-10-03: Bundled content mechanism: content/ ships inside the app (Resources/content; next to the exe on Windows), presets refer to it as "factory:models/..." and "factory:irs/..." (format stays version 2), and every bundled file needs a licence entry in content/manifest.json, which generates its part of THIRD_PARTY_NOTICES.txt. The notices are generated at build time from the actual licence files and shown under the brand menu's About / licenses.
2026-10-03: Sean confirmed: release builds need macOS 12 or later (Sparkle 2.10.0; DS5), and Windows ships without ASIO (WASAPI, including exclusive mode, only; AMPSIM_WITH_ASIO stays off; DS13).
2026-10-03: The amp captures bundled with the app will be Sean's own, made with ampsim_capture and NAM's official trainer (docs/CAPTURING.md). Sean's decision: the GPL-3 "pelennor" capture set was rejected over its provenance, and TONE3000's captures over its terms.
2026-10-03: The built-in cabs are two CC0 IR packs by Bastian Karschewski (Jester Dyne Productions), "Emerald" (a vintage 4x12, 6 IRs) and "Brutal" (a modern closed 4x12, 15 IRs), 48 kHz files renamed without brands (ASSUMPTIONS DS22 to DS27). This replaces 2026-09-27's "No bundled IRs"; the cab section is still a loader for the user's own IRs and packs too.
2026-10-03: Capture tooling: ampsim_capture (plays NAM's v3.0.0 input file and records the return in the same callback), tools/fetch_nam_input.sh (the input file, fetch-only, pinned by SHA-256), and tools/train_capture.sh with neural-amp-modeler pinned at 0.12.3 (the last version with the standard/lite/feather/nano choice; 0.13.0 trains only its packed slimmable model, ASSUMPTIONS DS30).
2026-10-03: Renamed BellyDSP (was "Amp Sim"; the UI's brand placeholder "rig" is now "BellyDSP"). Only what a user or a repo visitor sees changes: the app, bundle, window, brand, data folder (~/Library/Application Support/BellyDSP, %APPDATA%\BellyDSP), settings file, artifacts, installer, and docs. The code keeps its codename (namespace ampsim, AmpSimProcessor, the ampsim_* tools and targets, source file names, the state's "AmpSim" root tag), and the first launch copies the old data folder and settings to the new names, never moving or deleting them (ASSUMPTIONS DS36, DS37).
2026-10-03: Licensing (Sean's decision): BellyDSP is free, open source software under AGPL-3.0-or-later (required in practice because JUCE is used under the AGPLv3; JUCE_LICENCE=AGPLv3), "Copyright (C) 2026 Sean Snaider", with SPDX headers on every source file we own. Sean's own NAM captures are CC BY 4.0, attribution "Sean Snaider" (replacing the DS32 placeholder). Third-party content keeps its own licence (the bundled IRs are CC0).
2026-10-03: Two repos (Sean's decision): this private repo stays the dev and test environment (remote origin); the public SeanSnaider/BellyDSP is "prod" (remote public) and only gets main and version tags. Releases (binaries and appcasts) are published on the public repo itself; the separate amp-sim-releases repo is dropped. The rule, enforced by release.sh: every binary given to anyone is built from a commit that is on public/main and tagged there, and the tag is pushed to public before any file is uploaded (the AGPL's Corresponding Source). CI runs on push and pull requests only on the public repo (ASSUMPTIONS DS40).
2026-10-03: Bundle ID com.seansnaider.bellydsp (was com.seansnaider.ampsim; free to change because no release had shipped), with a new Inno Setup AppId and plugin code Bdsp for the same reason. From the first release on, none of the three may change.
2026-10-03: Factory presets renamed (Sean's decision, no band names in the app): "Polyphia" is "Modern Prog" (ModernProg.json) and "CHON" is "Math Rock" (MathRock.json). A saved state whose top bar remembers a factory preset by an old name restores it under the new one; a user's own preset of that name is left alone (ASSUMPTIONS DS51).
2026-10-03: Built-in default captures (Sean's request): each amp slot comes with a capture, so nobody has to load one. Slot 1 Glass (clean), slot 2 Ember (crunch), slot 3 Monolith (high gain), in content/models, CC BY 4.0 with the attribution "Sean Snaider". They're stand-ins trained with NAM's trainer (standard WaveNet) from the project's own gray-box amp (prototypes/amp_sim.py, amp only; Monolith through the app's Screamer boost first) by tools/content/make_default_captures.py, and Sean's real captures replace them under the same names (docs/CAPTURING.md). A fresh start (and a restored state with no entry for a slot) loads them on the loader thread; a slot cleared on purpose stays empty; the factory presets use them; the capture menu has "Use the built-in capture"; the info row shows "Model Glass (built in)" (ASSUMPTIONS DS44 to DS53).
2026-10-04: Tone match (Sean's spec, docs/TONE_MATCH.md; branch tone-match). Stage A's synthetic study passed its gate (the right amp in 17 of 18 dry cases, every match better than the best default), so B and C were built. The search renders only the nonlinear part (slot and Gain) and fits the rest on spectra; distortion is judged by level spread, level flux, brightness movement, and crest factor (spectral flatness was tried and rejected: it measured how many notes were sounding). The match EQ is the post EQ's parametric bands; Apply is one undo step.
2026-10-04: demucs.cpp (sevagh/demucs.cpp, MIT, commit f1206e9) added as a pinned submodule for tone match's guitar separation, over ONNX Runtime: it's MIT, runs on Eigen like NAM core (built against NAM's copy, renamed to its own namespace so it can use Accelerate's BLAS on macOS), and matches Python's Demucs within Python's own run-to-run spread (20.8 dB SDR on the golden clip; Python against itself 19.0 to 31.3 dB). Only its src/ is built; never modified.
2026-10-04: Apply switches off the pre compressor, boost, overdrive, and pre EQ, in the same undo step (Sean: "switch them off"; it used to leave them). The noise gates stay as they are: they mute between notes rather than color the tone (ASSUMPTIONS TM19).
2026-10-04: Separation workers are sized from the memory, 2.2 GB each within 40% of physical memory and what's available now, 1 to 4 (Sean: "fix the memory as you see fit"; it was (RAM - 6 GB) / 3 GB). The model download checks the HTTP status, retries 5 times with resume, and logs to separation-log.txt (ASSUMPTIONS TM20, TM21).
2026-10-04: Gain staging after Sean's play test ("too chaotic", "the clipping is really high"): measured with the new "Gain staging" audit, the pre compressor's auto makeup now restores -21 dBFS (where a DI sits between its peaks) instead of -12, every overdrive mode and the Screamer boost carry a unity trim so Level 0 dB is as loud as bypassed, and Bloom's phaser and flanger hold their level through the mix (1 / sqrt((1 - a)^2 + a^2)). Blocks at their defaults now change the level by about 1 dB or less; drive pedals at unity by at most 3 dB across -12 to -6 dBFS (ASSUMPTIONS PT1 to PT4).
2026-10-04: Output safety limiter, last in the chain, on by default at -1 dBFS: a soft-clip static curve with an instant attack and a 60 ms release, zero latency, bit-transparent 2 dB under the ceiling. New global parameters `output_limit_on` and `output_limit_ceiling` (never in presets or scenes); the Out meter shows "limit" and an ink cap while it works (ASSUMPTIONS PT5 to PT7).
2026-10-04: Multivoicer mix mode (Sean said yes): new parameter `mv_mix_mode`, Blend (the old equal-power mix, the default) or Add (the dry at exactly unity, the voices on top at sin(mix pi/2), the same wet gain as Blend). Each voice gets an interval picker (octave down, unison, minor and major third, fourth, fifth, octave up; cents to 0), and four one-click shapes (Octave up, Octave down, Power fifth, Octave stack) set the voices and switch to Add in one undo step (ASSUMPTIONS PT8 to PT10).
2026-10-04: The Amp page's Gate group gets Gate A's switch. Its Threshold and Release are Gate A's, which starts off, and the strip had no switch, so the knobs did nothing (verified bit for bit). The gate light now only lights while the gate is on. Measured on a noisy pickup through Monolith: at the default threshold (-55 dBFS) the gate never closes once hum and hiss reach -70 dBFS RMS (the detector reads the noise at -61 dBFS peak, over the -63 close threshold); after Learn it closes 99.96% of the gaps at every level tested (ASSUMPTIONS PT11, PT12).
2026-10-04: The Demucs weights (htdemucs_6s, 55 MB) are not bundled or re-hosted: they're downloaded on first use from the author's Hugging Face repository (adefossez/HTDemucs-6s, pinned commit, SHA-256 checked) and converted locally. The Demucs code is MIT, but no licence covers redistributing the weights (and their training data includes MUSDB18, academic use only), so the spec's "if not, download from the upstream source" applies (ASSUMPTIONS TM14).
2026-10-04: The standalone app keeps its audio device at 48 kHz (src/platform/SampleRateGuard.h): once a second, if the device runs at another rate but supports 48 kHz, it switches it back and saves that as the chosen setup; it gives up (leaving the mute and warning) if the device can't do 48 kHz or something switches it away more than 5 times a minute. Found when Apple Music left Sean's interface at 44.1 kHz and BellyDSP went silent.
2026-10-04: Gain sets (lead's decision after Sean's play test: "the gain just makes things louder", "the clipping is really high"). A capture is one amp at one gain setting, so each built-in amp is now five captures across its own gain knob (Gain 0, 2.5, 5, 7.5, 10), trained from the gray-box source with each channel's gain taper re-voiced to even distortion steps, and stored as a folder with a `gainset.json` that slots, presets, and the state refer to. The old single `Glass.nam`, `Ember.nam`, `Monolith.nam` are gone from the bundle; references to them load the sets (ASSUMPTIONS AG1 to AG3, AG10, AG11, AG13).
2026-10-04: The head's Gain with a gain set chooses a position across the steps, keeping its parameter ID (`amp*_input_trim`, stored in dB, shown 0 to 10). Between two steps both run and blend linearly, corrected for their measured correlation so the level holds; every step is loudness-normalized, so Gain changes saturation and Master stays the volume. A slot runs one model on a step, two between steps, and up to three while its Gain moves: a model the knob is heading for warms hidden for its receptive field first, so the scheduled blend is bit-identical to one with every model always running (no click), and the position moves at most 25 positions per second (AG4 to AG7, AG9).
2026-10-04: A single capture's Gain stays its input trim, now loudness-compensated: its loudness is measured at every 3 dB of trim at load and the output turned by the inverse, so Gain changes drive, not volume. It can't clean up or gain up past what was captured; CAPTURING.md now explains gain sets, and `tools/train_gain_set.py` trains and packages one in one command (AG8, AG14).
2026-10-04: Factory presets with real amp settings: every slot a preset plays has its seven amp knobs set, scenes hold an amp's Gain where two scenes share it (Math Rock's Breakup and Crunch both on Ember, Midwest Emo's Clean and Breakup on Glass, Tech Death's Rhythm and Lead on Monolith), each slot's Master keeps its scenes' peaks at or below -1.6 dBFS on the -6 dBFS test DI with the limiter off, and the sound-fixes level recommendations are in (AG12).
2026-10-04: Website (Sean's decisions): bellydsp.com, served by GitHub Pages from the public repo SeanSnaider/BellyDSP through a GitHub Actions workflow (.github/workflows/pages.yml); plain HTML and CSS in site/, no framework, Geist self-hosted, no external requests. Sean buys the domain; DNS and the one-time settings are in docs/WEBSITE.md (ASSUMPTIONS WEB1 to WEB19).
2026-10-04: Counting (Sean's decision): only GitHub's own anonymous per-file download counters. No cookies, no trackers, no third-party scripts, no page-view counter. Downloads are the installer counts; the daily copies-in-use estimate is the day-to-day growth of the appcasts' counts, summed over all releases; a daily workflow (.github/workflows/stats.yml) keeps the history in downloads.csv on a separate `stats` branch, and the site's data is generated at deploy time (tools/site/build_site_data.py), never fetched from GitHub by visitors' browsers.
2026-10-04: Windows updates download their own file, BellyDSP-<version>-windows-update.exe (the setup's bytes under another name, signed with the same Ed25519 key), so GitHub's counts tell updates from first installs. add_windows.sh, release.sh --windows-dir, and the Windows workflow make and upload it; make_appcast.py refuses a Windows appcast pointing anywhere else (ASSUMPTIONS WEB14).
2026-10-04: Website copy confirmed by Sean ("use your picks"): the stand-in captures are disclosed, the CPU claim stays (re-measured: 26.3% of a 128-sample buffer with every block on, so "about a quarter" holds), the Claude Code credit line stays as written, the download button stays light ink, and the site is dark only.
2026-10-04: Noise gate defaults (Sean chose both): the default threshold goes from -55 to -45 dBFS, and the first time the gate is switched on it runs Learn. Measured before: at -55 dBFS the gate never closed on simulated pickup noise of -70 to -60 dBFS RMS (ASSUMPTIONS PT12).
2026-10-04: Tone match's A/B comparison (Sean: "hard to compare"). The page plays the target (what was matched), the match (the DI through what Apply would set), and the current settings (the same DI through what's set now), looped, through a preview player that is a chain block at the very end, after the output level and before the output limiter, idle and bit-transparent unless comparing. The two renders run on a worker through `ToneMatcher::renderTone` (the chain's AmpSection, close mic 1's IR, the post EQ), never the audio thread, and are handed over with a Handoff. The sources are level-matched by BS.1770 to the Current render (a toggle, on), switched and looped with 20 ms equal-power crossfades, and in same-part mode lined up through the matcher's DTW path. The live guitar is muted while comparing ("Mute my guitar while comparing", on). Zero added latency; the player is in the real-time safety test (ASSUMPTIONS TM24 to TM31).
2026-10-05: Only the selected amp slot blends a gain set (Sean approved; BUILD_PLAN "Amp gain", ASSUMPTIONS AG15 to AG19). The other two always-running slots play their nearest step alone; the newly selected slot warms its second step (85 ms) while playing its nearest one, then crosses to the exact blend at the Gain's slew (50 ms at most); the slot switched away from holds its blend through its fade, then drops to its nearest step unheard. Why: with every slot between steps, the six models measured 113% of the 128-sample deadline on GitHub's Windows runner (4 vCPU x64, `tests/FullRigTests.cpp` "between steps"). Measured here: 4 models instead of 6, 36.4% of the deadline instead of 47.8% on that run.
2026-10-05: Status lines are ordered by request. Each load, clear, or status-only change of an amp slot or cab mic starts a numbered request on the message thread and shows its status at once; a loader job's result lands only if its request is still the newest for that slot or mic, so an older request finishing late never overwrites a newer status, whatever the threads' timing. Found by CI's slow runner ("Empty" shown instead of "Saved model is missing"); with the loader held 150 ms, the old code also let an IR load in flight overwrite "Saved IR is missing" (ASSUMPTIONS DS56).
2026-10-05: Windows exclusive mode and the mono input (Sean chose the automatic reopen over shared mode or ASIO only). JUCE's WASAPI backend opens "highest active input + 1" channels, so the mono-in app asks an exclusive-mode endpoint for one channel, and the Scarlett's only offers two ("Couldn't open the input device!"); JUCE's Options dialog lets a mono-in processor have exactly one input. On Windows, when exclusive mode refuses Input 1 alone, the standalone's once-a-second device guard reopens with Inputs 1 and 2 on the last devices that opened (else the type's defaults); the processor hears Input 1 only (tested bit-identical against a tone on Input 2). src/platform/ExclusiveModeInput.h.
2026-10-05: Windows ships with ASIO (Sean: "asio needs to be there"; DS13, reversing 2026-10-03). AMPSIM_WITH_ASIO defaults on and the Windows workflow always sets it, under the ASIO SDK's GPLv3 option (no Steinberg agreement). Measured on his Scarlett Solo: WASAPI exclusive drops 10 to 40% of callbacks below 224 samples and crackled at 256 in the app; Focusrite USB ASIO ran 64, 128, and 256 with every callback on time, and at 128 Sean found it clean and tight, the heaviest presets included. WASAPI stays for interfaces without an ASIO driver.
2026-10-05: CI is cut to fit (Sean: it would never pass in time). The full suite took 42 minutes on the macOS runner and 17+ on Windows (plus 12 to 15 of compiling), and the macOS job hit its 90-minute limit. The five slowest groups (Amp gain, Gain staging, Tone match, Cab normalization study, Full rig: about 33 of the 42 minutes) are skipped on CI through AMPSIM_SKIP_TESTS, the rest (3102 checks, the real-time safety test among them) still runs; macOS CI is manual only. The full suite stays the gate: on Sean's Mac before every push, and on his Windows PC before every release (RELEASING.md, step 0).
2026-10-06: App icon (Sean's pick of three): a white "B" in Geist SemiBold with the emerald brand dot as its period, on the app's near-black, in macOS's icon grid. Drawn by tools/content/make_icon.py into resources/icon/BellyDSP_icon_1024.png; JUCE makes the .icns and .ico (ICON_BIG).
2026-10-06: Tone match plays the target, and you can play along (Sean: "playback of the original audio when not only inputting it, but recording your section of it, so that you can play along", then "also add a count-in"). The Target card's Play loops the section through the A/B's preview player (two more sources: the song section and, after a separated match of it, its guitar), with your guitar not muted. With a target loaded, Record is a play-along take: a click count-in (on, 4 or 2 beats, at the app's tempo until set on the page, with Tap and a measured suggestion), the song one beat after the last click to the sample, the DI recorder started on the song's first sample in the same callback, the take lined up by the device's reported input + output latency plus an offset, and Same part chosen; such a take is matched with DTW in a 0.5 s band (a fixed offset measured worse in the prototype; the band changed no synthetic result). ASSUMPTIONS TM32 to TM40; docs/TONE_MATCH.md, "Play along". How it feels and whether the reported latencies line takes up on Sean's interfaces is unverified (PROGRESS TM.7).
2026-10-06: Learning a capture from the song stays a prototype (Sean's brief: tone match "doesn't feel right"; train a small NAM model from the play-along take to the record, judged against the current matcher on synthetic ground truth, stop if it doesn't clearly win on feel without losing on spectrum). It failed its gate, 3 of 12 cases (TONE_MATCH.md), so no Learn tone button; Save take collects real takes for a benchmark instead (ASSUMPTIONS TM41 to TM49). The informed harmonic mask won on every synthetic case and is the candidate to port next, once real takes say it holds up.
2026-10-06: Tone match cleans up the target with the take (Sean: "build the cleanup"). The prototype's informed harmonic mask is ported to C++ (`src/tonematch/InformedMask.*`, golden-tested against `learn_tone.py`) and runs before the match in Same part mode, between the separation and the analysis, on the match's worker: "Clean up with my take" in the Target card, on by default, disabled in Anything (the notes don't line up). Improved over the prototype by a 5-frame time smoothing of the mask (measured +0.02 to +0.08 dB SI-SDR). The A/B's Target plays the cleaned target and a new Raw source (the preview player's source 5) the target before; Guitar only and stem.wav stay Demucs's stem. End to end the matcher's distance to the hidden rig fell from 8.18 to 3.57 dB (no separation) and 5.89 to 3.62 dB (separated). Open question for Sean (ASSUMPTIONS TM55): Anything on the cleaned target measured closer still.
2026-10-07: The match curve (Round 2 item 5 of tone_bench, built ahead of the matcher's use of it). A fixed stereo block after the cab: a smooth (frequency, dB) curve, clamped to +-15 dB, smoothed to 1/12 octave (floored at 11.7 Hz), tapered at the band edges, as a 4096-tap minimum-phase FIR through the cab's zero-latency convolution ("Match curve" above). The amount rebuilds the FIR at the knob's amount (exact dB scaling) rather than crossfading the identity and the full FIR, which would miss by up to 3.2 dB at 25 to 75% on a +-12 dB curve (ASSUMPTIONS MC3). New permanent IDs `match_curve_on`, `match_curve_amount`; the curve in the state and the presets' optional `match_curve` (format 2). UI on the Cab page under the speaker. The matcher's fit and Apply are left to the tone match work.
2026-10-07: Tone match Round 2 (tone_bench; docs/TONE_MATCH.md, "tone_bench Round 2"). After a play-along take the mode stays Anything and Clean up with my take is Auto (only with separation and measured bleed); the winner is chosen by the take-aware score (the take's notes paired with the target's); the search tries a pedal in front of the best amp (the overdrive's modes, the clean boost past Gain +24, the pre compressor) and the post compressor; the match curve replaces the 5-band match EQ at 50%; the amps searched are every gain set in the content folder plus the slots' captures. Apply now sets the pre compressor, boost, and overdrive as the match has them (on or off) instead of switching them off, sets the post compressor, loads a matched gain set into the playing slot, and sets the match curve with the post EQ off, all in one undo step. Benchmark medians: DEV 0.677 (the old default) to 0.422, TEST 0.653 to 0.512.
2026-10-07: An amp picker on the Amp page (Sean: "I can't find the new amps"; they were only in the grille's right-click menu). Right of the slot tabs, in the preset selector's style: "Amp", previous and next arrows that step through every built-in amp (wrapping; from a capture that isn't built in, next starts at the first), and the playing amp's name, which opens a menu of all of them (the slot defaults, a line, the other five, then "Load a capture file..."). A pick loads into the playing slot, as the capture menu does. The slot keeps its head's material, but the badge and the tab now name the amp it plays ("Comet" in the Glass lettering); a single capture that isn't a gain set keeps the material's name. Test: Built-in captures, "the amp picker" (menu, arrows, wrap, badge, other slot, a non-built-in capture), snapshots default_captures/editor_amp_picker_*.png.
2026-10-08: Full amp heads for the five more built-in amps and a user's capture (Sean: heads like Glass and Monolith). Six more materials, Forge, Basalt, Comet, Quartz, Lantern, and Custom, each a complete look like the first three (body, panel, grille, badge, knob skin, mini), procedural and cached like them, from the existing bundled fonts only (condensed, widened, and slanted Geist as path transforms; Fraunces for Lantern's script). `materialForAmp` maps the eight built-in names to their materials and anything else to Custom, whose badge is the capture's name, elided when long. The art only: which material the head wears is the Amp page's call (ASSUMPTIONS UH20 to UH25).
2026-10-08: Engine: only the selected amp runs (Sean chose it on 2026-10-07 over keeping three always-running slots). A switch keeps the outgoing amp playing while the incoming one warms up unheard for its receptive field (NAM's prewarm, 4096 samples at 128: 85.3 ms), then crossfades over 20 ms with the sin law, and the outgoing amp stops: click-free, about 0.1 s late. A switch during a warm-up retargets to the newest. Steady state is one model (two between gain steps); measured on the built-in amps, the full rig with every block on fell from 27.5% to 14.7% of the 128-sample deadline. All eight built-in sets stay loaded (about 200 MB), so a switch waits only for the warm-up. ASSUMPTIONS AS4 to AS7, AS12.
2026-10-08: Amp page: one amp, picked from eight (Sean's choice of 2026-10-07). The slot tabs (and the amp picker of the same day) are gone; a shelf of mini heads, one per built-in amp in `builtInGainSets` order (Glass, Ember, Monolith, Lantern, Basalt, Comet, Forge, Quartz), then the player's own capture once loaded, picks the amp, and each amp remembers its own knobs. Parameter IDs kept: amp1_* to amp3_* are Glass's, Ember's, Monolith's; amp4_* to amp8_* the five more; amp9_* your capture; new `amp_model` chooses (amp_slot stays registered, unused). Old states, presets (format 2 to 3), scenes, MIDI mappings, and cab assignments migrate slot by slot to the amp each slot's capture was. ASSUMPTIONS AS1 to AS3, AS8 to AS11, AS13 to AS17.
2026-10-08: Tone match with eight amps: the search stays the Round 2 search; the wide one is built and off. With eight amps, the Round 2 search cut S's winner before S saw it in 7 of 30 DEV cases. The cut was by the old score's shortlist, and Gain refinement and pedals went only to its best amps. A wide search (every amp refined, the shortlist and the pedal amps also chosen by S_pre, a cheap S) fixes that: 1 of 28 cases worse in S than three amps. But an exhaustive search shows S's optimum over eight amps is no closer to the hidden rigs (DEV median 0.463, the wide search 0.455, the Round 2 search 0.465, three amps 0.422; Wilcoxon p 0.44 to 0.75). The score, fitted on three amps, doesn't rank the new ones, and the wide search takes twice as long. Next: refit S on pools over all eight gain sets, then gate the wide search (docs/TONE_MATCH.md, "Round 2: eight amps"; ASSUMPTIONS TM81 to TM85).
2026-10-08: CPU pass (Sean: "run on worse machines ... 48khz and 128 sample size smoothness on asio"; BUILD_PLAN "CPU", ASSUMPTIONS CPU1 to CPU8). NAM core's tanh is replaced, through its own activation registry and without touching the submodule, by Eigen's vectorised one: a standard model 145 -> 72 us per buffer, output within -112.4 dB of NAM core's render tool (limit -100 dB) and -134.5 dB of the stock tanh on every built-in step, so it counts as transparent, not a sound option. Defaults 8.2 -> 5.5% of the deadline, the factory presets 8.2-13.8% -> 5.7-8.6%, the heaviest rig 26.9 -> 22.0%. A budget for a 2017-class 4-core x64 laptop (3 to 5x slower per core) is asserted by Full rig: defaults 7 / 10%, every factory preset 10 / 12%, the heaviest 25 / 35% (mean / p99). Options that change the sound are measured and left for Sean: lite or feather captures, 2x drive oversampling, stopping switched-off drives, the Gain on the nearest step, a CPU saver mode combining them, AVX2 on Windows. `ampsim_tests --bench` and `--bench-gui` check another machine.
2026-10-08: Tone match, the score with eight amps: no refit, and the Round 2 weights stay. Pools over every built-in amp, with S's terms, show today's S is as good as this family of scores gets on DEV: no refit beat it by leave-one-out (S 0.471 on the eight-amp pools, 0.437 on the three-amp part). The new forms didn't help either: the notes' hold after the attack, and their level against the player's picking. S picks Gain and cab within the right amp as well as with three amps, but picks the right amp in 5 of 30 cases (21 of 30 with three). So the gate failed at the pools, and the matcher, TEST, and the C++ are unchanged. Next, if wanted: voicing-sensitive per-note measures, or fewer amps in the search by default, which is Sean's call (docs/TONE_MATCH.md, "Round 3: the score with eight amps"; ASSUMPTIONS TM86 to TM88).
2026-10-08: Tone match keeps searching all eight built-in amps (Sean's call, made twice after the case against it). The case against, from Round 3 (docs/TONE_MATCH.md): DEV any_fx_curve 0.465 with eight against 0.422 with three; on DEV's pools the take-aware score picks the best amp in 5 of 30 cases with eight against 21 of 30 with three, and a wrong amp costs 0.068 against 0.019; no refit of the score beats today's weights. For it: TEST is better in 13 of 20 cases with eight (mean 0.538 against 0.574, median 0.528 against 0.512), and the best match the eight can reach is closer (pool best 0.376 against 0.395). Proposed and declined: the three by default with an "Include all eight amps" checkbox.
2026-10-08: A CPU saver switch, opt-in and off by default (Sean chose it from the CPU pass's measured options; BUILD_PLAN "CPU"): stops switched-off drives with a warm-up when one is switched on, runs the drives at 2x oversampling, and holds the Gain on the nearest step. Declined: always stopping an off drive outside the saver, lite models for the clean amps.
2026-10-08: The CPU budget's measurement (the coordinator: on the merged main the p99s failed on an idle Mac). The profile now runs on a thread of its own: flat out on a user-interactive thread for the budget, or paced like a device on a real-time thread in an audio work interval (`cpu::Pacing`). Three runs each, quiet and loaded: no p99 is stable to better than about 1.5x on this Mac on either thread (the scheduler and the clock), the flat-out means to about 15%; device pacing on an idle Mac runs the bursts on a lower clock (the defaults 8% instead of 6%). So the budget holds the flat-out means (defaults 8%, every factory preset 11%, the heaviest 30%) and p99 limits from the worst of six runs with a margin (13, 18, 45%); the device-paced figures are printed, not asserted (ASSUMPTIONS CPU2, CPU5, CPU9).

# Building the auto-wah: a guided walkthrough of adding a block

This guide walks you through adding an auto-wah to BellyDSP yourself, from the math to the tests to the docs. The
point is the second feature, not this one: by the end you should be able to add any new block without a guide.
The general checklist is at the end ("Adding any block").

How to use it:

- **Work in order.** Each step has the concept, what you write, a **Done when** line, and the test that proves it.
  Don't skip the prototype or the tests: in this project a feature isn't done until a test shows it works.
- **Write the code yourself.** The guide shows APIs and pseudocode, not the finished block. When you're stuck,
  ask Claude for a hint: you'll get the concept first, then pseudocode, then a pointer at the exact line, and it
  asks before going further. Ask for a review whenever you finish a step.
- **Work on a branch:** `git switch -c autowah`. Build with `cmake --build build -j 4` (the Mac has 24 GB; more
  jobs than that has swapped it to death before).
- **The house rules apply to you too** (CLAUDE.md): no allocation, locks, I/O or logging in `process()`;
  every user-facing parameter smoothed; parameter IDs permanent once they ship; nontrivial DSP gets comments
  saying where the math comes from; zero added latency; 48 kHz only.

**Since the redesign (2026-10-15):** Steps 1 to 7 and 9 to 11 are yours; Step 8 (the page) is replaced by the
pedal on the board and its inspector, which get built in the app's design system after your Step 7. Until then,
the test that every parameter has a control on screen will fail for the `wah_*` IDs. That's expected. Step 1's
decisions are made and recorded in `docs/BUILD_PLAN.md` "Auto-wah".

Line numbers below were right on 2026-10-11 and will drift. Search for the function or identifier named next to
them.

---

## Step 0. The map: where a block lives

A feature touches seven places. You'll visit each one:

| Layer | Where | What it does |
|---|---|---|
| The DSP block | `src/dsp/AutoWah.h`, `.cpp` (new) | The algorithm. Knows nothing about parameters, the UI or presets. |
| The chain | `src/dsp/Chain.h`, `Chain.cpp` | Owns every block as a typed member in signal order, runs them, crossfades bypass, reorders sections. |
| Parameters | `src/BlockParameters.h`, `.cpp` | The block's parameter IDs, ranges, defaults, and a `read()` that turns them into the block's settings. |
| Processor glue | `src/PluginProcessor.cpp` | Creates the parameters, and once per buffer reads them and hands them to the block. |
| The UI | `src/ui/Blocks.*`, `src/ui/Pages.*`, `src/PluginEditor.cpp` | The block's entry in the Pre FX tabs and its page of knobs. |
| Presets, scenes, A/B, MIDI learn | (automatic) | Every non-global parameter is saved and recalled; any `*_on` switch is a scene switch; every knob can be MIDI-learned. You change nothing here. |
| Tests and docs | `tests/`, `prototypes/`, `docs/` | The proof, and the record of the decisions. |

Read these before you start, in this order. They're the patterns you'll copy:

1. `src/dsp/Block.h` (60 lines): the interface every block implements.
2. `src/dsp/Svf.h`: the filter you'll sweep. Read its header comment; it's the theory for Step 2.
3. `src/dsp/Boost.h`, `Boost.cpp`: the cleanest pre-FX block. It smooths parameters and redesigns its filters
   every 32 samples (`coefficientInterval`, `processChunk`, `updateFilters`).
4. `src/dsp/Compressor.h`, `Compressor.cpp`: an envelope detector with attack and release (`coefficient()`,
   the branching one-pole in `process`).
5. `tests/CompressorTests.cpp` and `prototypes/compressor.py`: a golden test against a Python prototype.

---

## Step 1. Design it before you write it

Decide these, write them down, and put them in `docs/BUILD_PLAN.md` as a new `### Auto-wah` section (look at
`### Compressor` for the style) with a line in the decision log at the bottom. Changing your mind later is fine;
changing it silently isn't.

**What an auto-wah is.** A wah pedal is a resonant band-pass filter whose centre frequency your foot moves,
roughly 350 Hz (heel) to 2 kHz (toe). An auto-wah replaces your foot with an envelope follower: pick harder and
the filter opens up, let the note decay and it closes. Two parts, then: something that measures how loud you're
playing, and a filter that moves with it.

**Placement.** Pre FX, before the amp, like a real wah. Suggested default position: right after Gate A, so the
gate removes hiss before the envelope follower can react to it. The Pre FX order is draggable, so players can
move it anyway.

**What the detector listens to.** Two options:

- *Its own input:* simple, but if someone drags it after the compressor, the compressor has flattened the
  dynamics and the wah barely moves.
- *The clean DI snapshot* (`BlockContext::di`): the guitar as played, wherever the wah sits. That's the project's
  rule for detectors (CLAUDE.md: the gates and the harmonizer read the DI). **Recommended.** The catch: in unit
  tests the context is often empty (`{}`), so fall back to the block's own input when `context.di` is null.

**Parameters.** Each ID is permanent once it ships, so pick names you can live with. A reasonable first set:

| ID | Name | Range | Default | Why |
|---|---|---|---|---|
| `wah_on` | On | bool | off | Every effect starts off |
| `wah_sensitivity` | Sensitivity | 0 to 100 % | 50 % | How hard you have to pick to open it |
| `wah_freq_low` | Low | 150 to 800 Hz | 350 Hz | Where the sweep starts (heel) |
| `wah_freq_high` | High | 800 Hz to 4 kHz | 2.2 kHz | Where it ends (toe) |
| `wah_q` | Resonance | 1 to 12 | 4 | How vocal and narrow the peak is |
| `wah_attack` | Attack | 1 to 50 ms | 5 ms | How fast it opens |
| `wah_release` | Release | 20 to 500 ms | 150 ms | How fast it closes |
| `wah_direction` | Direction | Up / Down | Up | Down closes on the pick: a backwards wah |
| `wah_mix` | Mix | 0 to 100 % | 100 % | Blend with the dry guitar |

Frequencies and Q want a skewed range (see `skewedRange` in `BlockParameters.cpp`) so the knob's middle is a
musical middle, not the arithmetic one.

**Filter shape.** Start with the SVF's band-pass (`Svf::Type::bandpass`: 0 dB at the centre, falling either
side). It's the textbook wah. It also thins the sound and drops the level, which is why there's a Mix. A resonant
low-pass sounds fuller; that's a possible second mode later, not now.

**Done when:** the BUILD_PLAN section exists with these decisions and the reasons, plus a decision-log line.

---

## Step 2. The math

Write this up in your own words in the BUILD_PLAN section, and put the key equations in comments in the code
later. Sources: `docs/DSP_REFERENCE.md` has the glossary; the envelope follower is the one in Giannoulis,
Massberg and Reiss, "Digital Dynamic Range Compressor Design: A Tutorial and Analysis" (JAES, 2012), which the
compressor already follows; the filter is Simper's trapezoidal SVF and Zavalishin's *The Art of VA Filter
Design*, chapter 4, which `Svf.h` already cites.

### 2a. The envelope follower

Rectify, then smooth with a one-pole low-pass whose speed depends on whether the level is rising or falling:

```
r[n] = |x[n]|                                  rectified input
a    = attack coefficient  if r[n] > e[n-1]
       release coefficient otherwise
e[n] = a * e[n-1] + (1 - a) * r[n]
```

Why the coefficient is `a = exp(-1 / (tau * fs))`: feed the smoother a step from 0 to 1. Its output is
`1 - a^n`. You want that to reach `1 - 1/e` (63 %) after `tau` seconds, which is `n = tau * fs` samples. So
`a^(tau * fs) = 1/e`, which gives `a = exp(-1 / (tau * fs))`. The compressor's `coefficient()` does exactly this;
reuse the idea, not the function.

Compute the coefficients when attack or release change (in `setSettings`, as the compressor's
`updateTimeConstants` does), never per sample: `exp` isn't free.

### 2b. From level to a control between 0 and 1

Guitar dynamics are logarithmic. A soft note might be -40 dBFS and a hard pick -10 dBFS. A linear mapping would
only react to the loudest attacks, so work in decibels:

```
level_db = 20 * log10(e + 1e-9)                the tiny offset keeps log10 away from 0
c        = clamp((level_db - floor_db) / span_db, 0, 1)
```

Sensitivity sets `floor_db`: at high sensitivity the floor is low (quiet playing already opens the filter), at low
sensitivity it's high. One reasonable mapping: `floor_db = -20 - 0.4 * sensitivity_percent` (so -20 dB at 0 % and
-60 dB at 100 %) with a fixed `span_db = 30`. Try others in the prototype and keep what feels right. With Direction
set to Down, use `1 - c`.

`log10` per sample is fine at this rate, but you only need `c` when you redesign the filter (2d), so compute it
there.

### 2c. From control to frequency

Musical pitch is exponential, so sweep the frequency exponentially:

```
fc = f_low * (f_high / f_low)^c
```

At `c = 0.5` you're at the geometric mean, `sqrt(f_low * f_high)`, which is halfway in octaves (and what it
should sound like). A linear sweep would race through the low end and crawl through the top.

### 2d. The filter, and how often to move it

`Svf::design (Svf::Type::bandpass, fc, q, 0.0, sampleRate)` gives you the coefficients, `setCoefficients` applies
them, and `processSample` runs one sample. Why this filter and not a textbook biquad: its state variables are the
integrators' contents, so you can move `fc` every few samples without clicks or instability (read the comment at
the top of `Svf.h`). A direct-form biquad swept this fast can pop or even blow up.

`design` calls `std::tan`, so don't call it every sample. Redesign every 16 samples (a third of a millisecond at
48 kHz). The envelope moves smoothly, so nobody hears steps that small. That's the Boost's pattern: process the
buffer in chunks, redesign at the start of each chunk.

### 2e. Smoothing the knobs

Every user-facing parameter is smoothed (`juce::SmoothedValue`, 20 ms like the other blocks):

- `f_low`, `f_high`, `q`: `SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative>`, because they're
  musical ratios.
- `mix`: a linear `SmoothedValue<double>`.
- `sensitivity`: linear.
- Attack, release and direction aren't smoothed. Changing them only changes how the envelope moves, not the
  signal directly.

In each chunk, `skip (chunkLength)` the smoothers and use `getCurrentValue()` for the redesign, as
`Boost::processChunk` does. For `mix`, use `getNextValue()` per sample; it's cheap.

**Done when:** you can explain, without notes, why the coefficient is `exp(-1 / (tau * fs))`, why the mapping is
in dB, why the sweep is exponential, and why the SVF can be moved every 16 samples.

---

## Step 3. The Python prototype

`prototypes/autowah.py`. Write it straight from the equations in Step 2, not from C++ you haven't written yet. It
does three jobs: lets you hear and tune the design before it's in the app, gives you plots to sanity-check it, and
becomes the trusted reference for the golden test.

What it needs (copy the shape of `prototypes/compressor.py`):

1. **The SVF band-pass,** sample by sample in double precision, mirroring `Svf.h`'s equations.
   `compressor.py` has a high-pass version you can follow; the band-pass is the same loop with a different
   output mix.
2. **The envelope follower, the dB mapping, the exponential sweep,** with the filter redesigned every 16
   samples, exactly as the C++ will do it. The golden test compares sample by sample, so the prototype has to
   do the same thing in the same order.
3. **A listening mode:** render a DI file through it and write a WAV. Use one of your own DI recordings.
4. **A plot mode:** the input level, the envelope, and `fc` over time (matplotlib), so you can see that a pick
   opens the filter and the decay closes it.
5. **`--golden <dir>`:** write `cases.json` (a few settings: the defaults, Down, high Q with a fast attack, a
   narrow range), `input_mono.wav` (a few seconds of a synthetic guitar-like signal, or a plucked tone with
   decays, float32), and `expected_<case>.wav`. Look at `write_golden()` in `compressor.py`.

Run it with:

```
uv run --with numpy --with scipy --with matplotlib python prototypes/autowah.py <your_di.wav>
uv run --with numpy --with scipy python prototypes/autowah.py --golden tests/fixtures/autowah
```

**Done when:**
- you've listened to your DI through it and like the defaults;
- the plot shows the envelope rising on picks with the attack you set and falling with the release;
- `tests/fixtures/autowah/` holds the golden files.

Commit the prototype and the fixtures.

---

## Step 4. The C++ block

`src/dsp/AutoWah.h` and `AutoWah.cpp`, in `namespace ampsim`, implementing `Block`:

```cpp
class AutoWah final : public Block
{
public:
    struct Settings { /* one field per parameter, with the defaults */ };
    void setSettings (const Settings&);        // audio thread, once per buffer, before process()

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    // latencySamples() and isStereo() keep Block's defaults: 0 and false (a mono pre-FX block)
    ...
};
```

That's the API shape. The insides are yours. The pieces, as pseudocode:

```
setSettings(s):
    set the smoothers' targets (clamped to the ranges)
    if attack or release changed: recompute the two coefficients

prepare(fs, maxBlock):
    remember fs; reset each smoother with (fs, 0.020); compute the coefficients; reset()

reset():
    envelope = 0; filter.reset(); snap the smoothers to their targets; redesign the filter

process(block, context):
    x = block channel 0;  detector = context.di if it isn't null, else x
    for each chunk of 16 samples (the last one may be shorter):
        skip the smoothers by the chunk length
        compute c from the envelope (2b), fc from c (2c), redesign the filter (2d)
        for each sample in the chunk:
            update the envelope from |detector[i]| (2a)
            wet = filter.processSample(x[i])
            m = mix.getNextValue()
            x[i] = (1 - m) * x[i] + m * wet
```

Things that bite:

- **Real time.** No `new`, no `std::vector` that grows, no `DBG`, no locks. Everything is sized in `prepare`. The
  real-time safety test (Step 7) will catch you if you slip.
- **Doubles inside, floats at the edges.** The SVF state is double. Keep the envelope in double too. The buffer
  is float.
- **The math comments** are required: the envelope coefficient's derivation, the dB mapping, the exponential
  sweep, why 16 samples, and the sources. That's a review item.
- **`fc` limits.** `Svf::design` clamps to `[1 Hz, 0.49 fs]`, but keep `f_low < f_high` sensible: if someone sets
  Low above High, the formula still works and the sweep just runs backwards. Decide whether that's a bug or a
  feature, and write the decision down.

**Done when:** it compiles (Step 6 adds it to CMake) and the unit tests in Step 5 pass.

---

## Step 5. The unit tests

`tests/AutoWahTests.cpp`. Copy the skeleton from `tests/CompressorTests.cpp`: an anonymous namespace,
`using namespace testing;`, a `juce::UnitTest` subclass named `"Auto-wah"` in category `"ampsim"`, a static
instance at the end. Drive the block directly, as the compressor tests do:

```cpp
AutoWah w;
w.setSettings (s);
w.prepare (fs, blockSize);
w.process (juce::dsp::AudioBlock<float> (channels, 1, numSamples), {});   // {}: no DI, so it uses its input
```

The helpers you'll want are in `tests/TestHelpers.h`: `fs`, `blockSize`, `sine`, `whiteNoise`, `guitarDI`, `rms`,
`toDb`, `relativeErrorDb`, `maxStep` (a click detector), `runInBlocks`, `proofDir`, `writeWav`. `Plot.h` draws PNGs.
Log every measured number with `logMessage ("  -> ...")`. Sean reads them, and "it passed" without numbers proves
nothing.

The tests, each its own `beginTest`:

1. **Golden.** Every case in `tests/fixtures/autowah/cases.json` rendered through the C++ block matches the
   prototype's WAV with `relativeErrorDb < -100` (the compressor's bar). This proves the C++ does what you
   designed and listened to. If it fails by a lot, the order of operations differs (the chunk boundaries, when
   the envelope updates relative to the redesign). If it fails by a little, check float vs double.
2. **The sweep endpoints.** Make the mapping from control to frequency a small static function so you can test it
   alone: `c = 0` gives `f_low`, `c = 1` gives `f_high`, `c = 0.5` gives `sqrt(f_low * f_high)`, within 1e-9.
3. **The filter's response.** At a fixed `fc` and `q`, use `Svf::responseAt` to check the peak is 0 dB at `fc` and
   the -3 dB points sit about `fc / q` apart.
4. **Envelope timing.** Feed silence, then a constant-level sine burst, then silence again. Record the envelope.
   Expose it read-only for tests, the way the compressor exposes its meters. Check that it reaches 63 % of the
   step `attack` milliseconds after the burst starts, and falls to 37 % `release` milliseconds after it ends, each
   within 5 %. Log the measured times.
5. **It actually wahs.** A soft and a hard version of the same note: the hard one's output should have a higher
   spectral centroid with Direction Up and a lower one with Down. Log both centroids. Plot the output's spectrum
   for both and save the PNG to `proofDir()`.
6. **Mix 0 is the dry signal,** bit for bit (`std::memcmp` or exact equality on every sample).
7. **Nothing blows up.** Q at its maximum, the fastest attack, full-scale white noise, then digital silence: no
   NaN or inf, output bounded, and silence in eventually gives exactly silence out. Also check that `reset()`
   then the same input gives the same output.
8. **No clicks from the knobs.** Sweep `f_low`, `f_high`, `q` and `mix` across their ranges in a few hundred
   milliseconds while a note plays. `maxStep` of the output shouldn't exceed what the same note gives with the
   knobs still (allow a small factor; the other blocks use about 1.5x). This is what the smoothing is for.
9. **A listening render.** Your DI (or `guitarDI`) through the defaults, written with `writeWav` to
   `proofDir()/autowah_defaults.wav`. Tests can't judge the sound, but this gives you something to listen to on
   every run.

Run them alone while you work:

```
cmake --build build -j 4 --target ampsim_tests
build/ampsim_tests_artefacts/Release/ampsim_tests --proof-dir build/proof --only "Auto-wah"
```

(`--only` matches the test's name, here "Auto-wah", not the `beginTest` text.)

**Done when:** all nine pass, and you've looked at the plot and listened to the WAV.

---

## Step 6. Wire it into the build and the chain

**CMake** (`CMakeLists.txt`; files are listed by hand, there's no glob):

- add `src/dsp/AutoWah.cpp` to `set(AMPSIM_DSP_SOURCES ...)` (around line 176). The app, `ampsim_render` and the
  tests all share that list;
- add `tests/AutoWahTests.cpp` to `target_sources(ampsim_tests PRIVATE ...)` (around line 516).

Configure again if CMake doesn't notice the new files: `cmake -B build -DCMAKE_BUILD_TYPE=Release`.

**The chain** (`src/dsp/Chain.h`, `Chain.cpp`), five edits:

1. `#include "AutoWah.h"` with the other block includes.
2. A typed member, `AutoWah wah;`, among the pre-FX members in signal order (after `gateA` if that's your
   default position).
3. A new enumerator in `enum class Slot`, among the pre slots, in signal order. `prepare` walks the slots in enum
   order and asserts that no mono block comes after a stereo one, so it must sit before `amp`.
4. Add it to the pre section's default order: `static const std::vector<Slot> pre { ... }` in `Chain.cpp`
   (around line 62).
5. A `case` in the `blockFor` switch (around line 123) that returns `&wah`.

Everything else in the chain (prepare, reset, latency, bypass, reordering) loops over the slots and just works.
The pre section is mono, so your block gets a one-channel `AudioBlock`; the copy to stereo happens later, at the
cab.

**The order's name.** Saved orders store blocks by name. Add your slot to `AmpSimProcessor::blockName`
(`PluginProcessor.cpp`, around line 580) with a permanent name: `"wah"`. A preset saved before the wah existed
has no `"wah"` in its order, and the processor inserts it by its "fewest broken pairs" rule, so old presets keep
working.

**Tests that hard-code the pre order** will now fail, correctly, because the order has a new member. Update
them: `tests/ProcessorTests.cpp` (search for `Slot::preEq` near the order checks, around lines 733, 1694 to 1727,
2114 to 2129), `tests/ChainTests.cpp` (around 216), `tests/RealtimeSafetyTests.cpp` (around 502).

**Done when:** it builds, and those tests pass with the new order.

---

## Step 7. Parameters and the processor

**Declare the parameters** in `src/BlockParameters.h` and `.cpp`. Copy `CompressorParameters` as the pattern:

```cpp
struct AutoWahParameters
{
    static void addTo (Layout&, const juce::String& prefix, const juce::String& name);   // creates them
    void bind (State&, const juce::String& prefix);                                       // finds them again
    bool isOn() const noexcept { return on.on(); }
    ampsim::AutoWah::Settings read() const noexcept;                                      // the block's settings
    Raw on, sensitivity, freqLow, freqHigh, q, attack, release, direction, mix;
};
```

In `addTo`, each parameter is a `juce::AudioParameterFloat`, `Bool` or `Choice` with
`juce::ParameterID { prefix + "_q", 1 }`, a range (`skewedRange (lo, hi, centre, interval)` for frequencies and
Q), the default taken from `AutoWah::Settings{}` so there's one source of truth, and a label (`hertz()`,
`milliseconds()`, `percent()`). The helpers are at the top of `BlockParameters.cpp`.

**Register them** in `src/PluginProcessor.cpp`:

- `createParameterLayout()`: `params::AutoWahParameters::addTo (layout, "wah", "Auto-wah");` next to the
  pre comp's line (around 269);
- the constructor: `wahParams.bind (parameters, "wah");` next to `preCompParams.bind` (around 66), with a
  `params::AutoWahParameters wahParams;` member in `PluginProcessor.h` (around 570);
- `applyEffectParameters()` (called once per buffer; around 465 to 530), next to the pre comp:

```cpp
chain.setBypassed (Slot::wah, pre (wahParams.isOn()));
chain.wah.setSettings (wahParams.read());
```

`pre (on)` also folds in the Pre FX section's own switch. The chain crossfades the bypass over 10 ms and calls
`reset()` when the block comes back from fully off.

**Tone match's Apply** sets every pre effect so the match plays the same rig it measured. Add the wah to
`ToneMatchSession::preEffectsApplyTurnsOff()` (`src/ToneMatchSession.cpp`, around 776) and set `wah_on` to 0 in
`apply()` (around 664 to 690), then update the two lists in `tests/ToneMatchAppTests.cpp` (around lines 65 and
236). Otherwise a match made with the wah on would play back wrong.

**Presets, scenes, A/B, MIDI:** nothing to do. Presets save every non-global parameter, so an old preset loads
with the wah at its defaults (off). Scenes switch anything ending in `_on`. A/B snapshots every parameter. MIDI
learn works on any control tagged with its parameter, which `addKnob` does for you.

**Done when:** the app builds and the parameter IDs appear in a saved preset's JSON.

---

## Step 8. The page

1. **The block registry**, `src/ui/Blocks.h` and `Blocks.cpp`: a `BlockId::wah` in the enum, among the pre
   entries, and its row in the `blocks` array:
   `{ BlockId::wah, "Auto-wah", "wah", Section::pre, "wah_on", false, "wah" }`. The last field must equal the
   `blockName` from Step 6. That's all it takes for the tab to appear on the Pre FX page; the tabs are built from
   the section's order.
2. **The page class:** declare `class AutoWahPage final : public BlockPage` in `src/ui/Pages.h` next to
   `BoostPage`, and implement it in `Pages.cpp`. Copy `BoostPage` (around line 542 of `Pages.cpp`): the
   constructor makes the controls with `addKnob (id, caption, suffix)`, `addSwitch` and `addCombo`, and
   `layoutContent` places them in cards with `layoutCards`. Give each knob a tooltip saying what it does
   (`setTooltip`), and a formatter where the raw value reads badly (`setFormatter`).
3. **Register the page** in `src/PluginEditor.cpp` (around 109 to 127):
   `add (std::make_unique<ui::AutoWahPage> (p), B::wah);`

**Tests that cover the page for free:**
- `tests/ProcessorTests.cpp` saves `editor_wah.png` for every block (look at it in `build/proof`);
- it also fails if any parameter has no control tagged on its page, which is MIDI learn's safety net.

**Done when:** the Pre FX page has an Auto-wah tab, the snapshot looks like the other pages, and every knob works
in the app.

---

## Step 9. The whole-app tests

Every new block joins these:

1. **Real-time safety** (`tests/RealtimeSafetyTests.cpp`): 14 s of guitar through the whole processor while a
   list of events fires at given buffer numbers. Add events at free numbers in the pre-FX range (1310, 1330 and
   1390 were free): switch `wah_on` on, sweep `wah_freq_high` and `wah_q`, change `wah_attack`, flip
   `wah_direction`, and drag the wah to a new place in the pre order with `p.setSectionOrder`. It must still read
   0 allocations, 0 frees, 0 locks.
2. **Gain staging** (`tests/GainStagingTests.cpp`, the `blockAudit` list around line 195): add
   `{ "Auto-wah", { { "wah_on", 1 } }, true }` so the level audit covers it.
3. **The CPU profile** (`tests/CpuProfile.cpp`):
   - a `case Slot::wah: return "auto-wah";` in `slotName`;
   - `wah_on` in `setHeaviest`'s list;
   - the Full rig's "everything on" list (`tests/FullRigTests.cpp`, around line 230).

   Then run `--only "Full rig"` and read your block's line: a mono SVF and an envelope should cost a few
   microseconds per buffer. If it's much more, you're redesigning too often or calling `log10` per sample.
4. **The full suite,** once, at the end:
   `build/ampsim_tests_artefacts/Release/ampsim_tests --proof-dir build/proof`. Every test must pass.

**Done when:** the full suite passes, with the real-time line still reading 0/0/0.

---

## Step 10. The docs

- `docs/BUILD_PLAN.md`:
  - your `### Auto-wah` section, updated with what you actually built and measured;
  - the wah added to the Pre FX table and the default signal-chain diagram near the top;
  - the decision-log line.
- `docs/ASSUMPTIONS.md`: a new `## Auto-wah` section with the usual table (`| # | Assumption | Why | To change
  it |`), IDs `AW1`, `AW2`, and so on, for each judgment call: the detector reading the DI, the dB mapping and
  span, 16-sample updates, the default position, Low above High, the parameter ranges.
- `docs/PROGRESS.md`: a log line, and a "Sean checks" row (Step 11).
- `docs/DSP_REFERENCE.md`: the auto-wah's sources in the "Sources by block" table.

---

## Step 11. Listen

Tests prove it does what you designed. Only you can say if the design is good. Play through it and note:

- Does it open on the pick and close as the note decays, at a speed that suits fast playing and slow bends?
- Is the Sensitivity range useful at your interface's level, or does it sit fully open or fully closed?
- Is the sweep range vocal ("wow"), or too thin or too boomy at the ends?
- At high Resonance, does it whistle or get harsh into a high-gain amp?
- Does Down do something musical?
- Before Gate A versus after it, and before or after the compressor: which do you prefer?

Change the defaults if they're wrong. That's a decision: update the BUILD_PLAN and the decision log, re-run the
prototype's `--golden`, and the tests follow.

Then commit, push the branch to `origin`, and ask Claude for a review.

---

## Adding any block: the checklist

The same steps for any future block (a phaser in pre FX, a pitch pedal, a new drive):

1. Design: placement, what any detector listens to, the parameters (IDs are permanent), the defaults. BUILD_PLAN
   section and decision log.
2. The math, with sources, in your own words.
3. A Python prototype from the equations: listen, plot, `--golden` fixtures.
4. The block: `src/dsp/<Name>.h`, `.cpp`, implementing `Block`. Smoothed parameters, coefficients recomputed per
   chunk, real-time rules, math comments.
5. Unit tests: golden against the prototype, the math's invariants (endpoints, responses, time constants), the
   bypass or dry path, stability at the extremes, click-free knob moves, a listening render.
6. CMake: both lists.
7. The chain: include, member, `Slot`, default order, `blockFor`, and a `blockName`. Fix the tests that pin the
   order.
8. Parameters: `XParameters` in `BlockParameters`, `addTo` in the layout, `bind` in the constructor, `read` and
   `setBypassed` in `applyEffectParameters`. Tone match's lists if it's a pre effect.
9. The UI: `BlockId`, the `blocks` row (its order name equals `blockName`), the page class, registered in the
   editor.
10. The whole-app tests: real-time safety events, gain staging, the CPU profile's lists, then the full suite.
11. Docs: BUILD_PLAN, ASSUMPTIONS, PROGRESS, DSP_REFERENCE.
12. Listen, adjust, commit, review.

A post-FX block differs in two places: it's stereo (`isStereo()` returns true, so it gets two channels), and it
joins the post order instead of the pre one.

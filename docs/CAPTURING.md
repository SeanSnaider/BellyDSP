# Capturing your own gear

How to turn your own amp, pedal, or preamp into a NAM capture that plays in BellyDSP's amp slots, and how
it then ships to friends. Two tools do the work:

- **`ampsim_capture`** (built with everything else by `cmake --build build -j`) plays NAM's standard test
  file out of the Solo into your gear and records what comes back.
- **`tools/train_capture.sh`** runs NAM's official trainer on that recording and writes the `.nam` file.

Nothing here has been tried on real gear yet: the tools were tested against simulated devices, a
simulated pedal, and the Solo's own loopback (see "What's been verified" at the end).

## What a capture is, and which kind to make

A capture is a small neural network (a WaveNet) trained to do to the guitar signal exactly what your gear
does. NAM learns it from one recording: a 3-minute test file goes in, your gear's response comes out, and
the trainer fits the network to the pair.

The app already has its own cab section (the two bundled cab packs, your IRs, mic placement, a room mic),
so **amp only** captures are the ideal kind: the amp's sound without a speaker or mic in it.

| Setup | What you need | In the app | `--gear-type` |
|---|---|---|---|
| A. Amp through a load box, line out | a load box (a resistive or reactive load with a line out) | Cab on: pick any cab | `amp` |
| B. Mic'd amp, the whole rig | a mic and stand, a quiet-ish room, hearing protection | Cab **off** (the cab is in the capture) | `amp_cab` |
| C. A pedal or preamp straight in | nothing extra | Cab on for a preamp; for a drive pedal, see below | `pedal` or `preamp` |

A drive pedal captured alone is "the pedal", not "the pedal into an amp": in an amp slot it replaces the
amp. To use it as a pedal, capture it in front of the amp (setup A or B with the pedal in the chain,
`--gear-type pedal_amp`), or use the app's own drive blocks.

## Safety first

- **A tube amp must always have a load.** Never switch a tube amp on, or out of standby, without a speaker
  cab or a load box connected to the speaker output that matches its impedance (8 ohm into 8 ohm, and so
  on). Without a load, the output transformer can fail in seconds. Solid-state amps are more forgiving,
  but use a load anyway.
- **Never connect an amp's speaker output to the Solo, or to anything but a speaker or a load box.** It
  carries tens of volts and will destroy the interface. Only the load box's **line out** goes to the Solo.
- **Volume.** The test file is loud, harsh noise and sweeps. Turn the monitors and headphones down
  before every run; the Solo's headphones play outputs 1 and 2, so you hear what's sent to the gear. For
  setup B the amp is at playing volume for 3 minutes: wear hearing protection and keep the mic still.
- `ampsim_capture` asks you to press Enter before it plays anything, and never plays above the
  `--output-level-db` you give it (default -12 dBFS).

## The Solo's connections

`ampsim_capture --list` prints them. On the Scarlett Solo 4th Gen they come up as:

| Option | Channel | What it is |
|---|---|---|
| `--in-channel 1` | Input 1 | the 1/4" instrument input, where the guitar usually goes. Returns from a load box, pedal, or preamp come back here (unplug the guitar first) |
| `--in-channel 2` | Input 2 | the mic input (XLR), for setup B |
| `--out-channel 1` | Output 1 | the left rear line output (1/4" TRS). The test file goes out here |

The rear outputs are **line level**, far hotter than a guitar. An amp's guitar input expects guitar level,
so between Output 1 and an amp (or a pedal) goes a **reamp box** (it turns the line signal into a
guitar-like one at the right impedance), or, without one, a low `--output-level-db` (start at -20 dBFS,
see "Levels"). Which of the Solo's knobs and switches apply (the Input 1 gain knob, an Inst/Line setting
in Focusrite Control 2) is from memory of the Solo, not checked: look at the Solo's manual if a label
differs.

## Wiring

**A. Amp and load box (amp only, the recommended one)**

```
Solo Output 1 (rear, 1/4") --> reamp box --> amp's guitar INPUT
amp's SPEAKER OUT --> load box SPEAKER IN        (never to the Solo)
load box LINE OUT --> Solo Input 1 (front, 1/4"), its gain knob turned right down
```

**B. Mic'd amp (the whole rig, cab included)**

```
Solo Output 1 (rear, 1/4") --> reamp box --> amp's guitar INPUT
amp's SPEAKER OUT --> speaker cab (as normal)
mic on the cab --> XLR --> Solo Input 2
```

**C. Pedal or preamp**

```
Solo Output 1 (rear, 1/4") --> reamp box (or a plain cable and a low level) --> pedal INPUT
pedal OUTPUT --> Solo Input 1 (front, 1/4"), gain knob right down
```

## The commands, in order

Run these from the repo folder (`cd ~/Desktop/bitchless_neural_net/amp_sim_priv`).

**1. Build the tools** (once, and after pulling changes):

```
cmake --build build -j
```

`ampsim_capture` ends up in `build/ampsim_capture_artefacts/Release/`. To save typing:
`alias ampsim_capture=build/ampsim_capture_artefacts/Release/ampsim_capture`.

**2. Fetch NAM's input file** (once):

```
tools/fetch_nam_input.sh
```

It downloads `input.wav` (NAM's v3.0.0 test file, 190 s, 26 MB) from the link NAM's own trainer offers into
`build-deps/nam/`, and checks its SHA-256 (pinned in `tools/deps.conf`) and the MD5 the trainer itself uses
to recognise it. It's not in git: no licence is stated for the file, so each machine downloads its own.

**3. Check the connections**:

```
ampsim_capture --list
```

**4. Level check** (17 s, saves nothing; repeat until it says "Good"):

```
ampsim_capture --level-check --in-channel 1 --output-level-db -20
```

It plays the first 17 s of the file (the validation audio, the calibration blips, sweeps, noise) and
reports the return's peak and RMS in dBFS, whether anything clipped, the peak to expect over the whole
file, and the round-trip latency, measured from the blips the way NAM's trainer does (the trainer handles
latency itself; it's reported so you can see the blips came back). Aim for **peaks between -6 and -3
dBFS**:

- too hot or clipping: turn the Solo's gain knob for that input down (or the gear's master volume);
- too quiet: turn the Solo's input gain up;
- "almost nothing came back": a cable, the gear's standby switch, or the wrong `--in-channel`.

Change `--output-level-db` only to change **how hard the gear is driven** (that changes the tone, like
turning up a guitar's volume knob); the Solo's input gain only changes how loud the recording is.

**5. Capture** (190 s; don't touch anything while it runs):

```
mkdir -p captures
ampsim_capture --in-channel 1 --output-level-db -20 --output "captures/Crunch amp.wav"
```

It writes a 24-bit, 48 kHz WAV with exactly the input file's length, the same moment as sample 0. It
refuses to run unless the Solo is at 48 kHz (set it in Audio MIDI Setup if it isn't). If the interface
reports a dropout it says so: run it again with `--buffer 512` and nothing else open. Keep the amp's
settings written down (a photo works): the capture is only of that setting.

**6. Train**:

```
tools/train_capture.sh --input build-deps/nam/input.wav --output "captures/Crunch amp.wav" \
    --name "Crunch, amp only" --tone-type crunch --gear-type amp --out-dir trained
```

What `tools/train_capture.sh` is: a small wrapper that runs NAM's own trainer (the `neural-amp-modeler`
Python package, at the version pinned in `tools/deps.conf`, 0.12.3) through `uv`, which builds a private
Python environment for it the first time (about 1 GB, mostly PyTorch) and reuses it after that. It
checks the files, removes the latency, checks that the two copies of the validation audio match (if they
don't, the gear changed during the capture), trains, and writes `trained/Crunch, amp only.nam`, a plot of
the model against the gear, and a `training.json` with the numbers.

Options worth knowing: `--arch standard` (the default and the most accurate) or `lite`, `feather`, `nano`
(smaller, cheaper to run, less accurate); `--epochs 100` (NAM's default); `--tone-type` is one of clean,
overdrive, crunch, hi_gain, fuzz (the app shows it; the amp head's look follows it); `--device cpu` to
skip the GPU. On this Mac (M5 Pro) it trains on the GPU through Apple's MPS:

| Architecture | Time per epoch | 100 epochs | Synthetic capture's validation ESR |
|---|---|---|---|
| standard | 3.7 s | about 6 min | 0.0043 after 100 epochs |
| feather | 2.8 s | about 5 min | 0.0162 after 40 |
| nano | 2.4 s | about 4 min | 0.0298 after 40 |
| standard on the CPU (`--device cpu`) | 231 s | about 6.5 hours | |

(measured on the synthetic capture, `tools/synthetic_capture_check.py`; a real one is the same length, so the same time. A real amp is harder to model than that pedal circuit, so expect higher ESRs.)

The metadata: `--name` is what the capture is ("Crunch, amp only"), `--modeled-by` defaults to Sean
Snaider, `--tone-type` and `--gear-type` as above. **Never put a brand or model name in any of them**: the
file travels with the app, and the app never shows gear names (the script warns if it spots one).

**7. Listen.** Open the app, load the `.nam` into an amp slot (setup B: switch the Cab off with its dot in
the chain strip), and compare with the real amp at the same settings, same guitar, same volume. That's
the real test; the ESR below is a guide.

**8. Bundle it** (to ship it in the app):

1. Copy it into `content/models/`, for example `content/models/Crunch, amp only.nam`.
2. Add its entry to `content/manifest.json` (the build fails without one):
   ```json
   {
     "path": "models/Crunch, amp only.nam",
     "title": "Crunch, amp only",
     "author": "Sean Snaider",
     "source": "Captured by Sean Snaider for BellyDSP (https://github.com/SeanSnaider/BellyDSP)",
     "license": "CC BY 4.0",
     "license_file": "licenses/CC-BY-4.0.txt",
     "notes": "Amp through a load box; trained with neural-amp-modeler 0.12.3, standard, ESR 0.0xx"
   }
   ```
   Your own captures are CC BY 4.0 with the attribution "Sean Snaider" (decided 2026-10-03, replacing the
   DS32 placeholder; `content/licenses/CC-BY-4.0.txt` is the official legal code). Anyone may share and
   adapt them, commercially too, as long as they credit you. The app's code stays AGPL-3.0-or-later.
3. Put it in a factory preset: open the app, load the preset, load the bundled capture into a slot from
   the app's own copy (`build/BellyDSP_artefacts/Release/Standalone/BellyDSP.app/Contents/Resources/content/models`,
   after a build), Save the preset, and copy its `"amps"` entry (which now reads
   `"factory:models/Crunch, amp only.nam"` with a hash) into `presets/factory/<preset>.json`.
4. `cmake --build build -j && ctest --test-dir build --output-on-failure`: the tests check every factory
   preset loads its files with no warnings.

## Judging a capture

The trainer prints the **ESR** (error-to-signal ratio) on the validation audio, which the model never
trained on: the energy of the difference between the model and the gear, over the energy of the gear. 0
is perfect. NAM's own verdicts:

| ESR | NAM says | In practice |
|---|---|---|
| below 0.01 | "Great!" | usually indistinguishable; typical for clean and crunch amps with `standard` |
| 0.01 to 0.035 | "Not bad!" | high gain often lands here; listen closely to the pick attack and the noise floor |
| 0.035 to 0.1 | "might sound ok" | try more epochs, `standard`, or a quieter, more careful capture |
| above 0.1 | something's wrong | check the level check, the cables, and the "Replicate ESR" line |

It also prints **"Replicate ESR"**: how well the validation audio's two copies (at the start and the end of
the file) match each other. Near 0 means the gear stayed the same for 3 minutes. If it's high, something
changed (a knob, the tubes warming up, noise, a hum loop), and no model can beat it: fix that and capture
again. Warm a tube amp up for 15 minutes first.

Then look at the plot (`trained/<name> training/<name>.png`): the model's line should sit on the gear's.

## How a capture reaches friends

Once it's bundled (step 8) and committed, the next release carries it: write the release notes ("New:
a crunch capture of ..."), run `tools/release/release.sh <version>` (docs/RELEASING.md), and every
friend's copy updates itself within a day. Their presets refer to it as `factory:models/...`, so it's
found wherever the app is installed.

## Input level metadata (optional, for the feel)

The app's input calibration (docs/ASSUMPTIONS.md C9) makes a capture react to the guitar the way the
gear did, if the capture's metadata says what analog level 0 dBFS of the input file reached the gear at
(`--input-level-dbu`). Without it the capture still works; it just hears the guitar at whatever level the
Solo records it, so it may feel a little more or less driven than the real thing.

To find the number: run `ampsim_capture --sine-1k` with the **same `--output-level-db` as the capture**
(say `--output-level-db -20`) and measure the AC voltage V where the gear's input would be (after the
reamp box, if you use one) with a multimeter. That sine is the input file's full scale as the gear gets
it, so

    input_level_dbu = 20 log10 (V / 0.7746)

(0.7746 V RMS is 0 dBu; a meter on AC volts reads RMS). Pass it to
`tools/train_capture.sh --input-level-dbu <that number>`. For comparison, the Solo's instrument input
reaches 0 dBFS at about +12 dBu at minimum gain (C9): a capture whose number is close to that reacts to
your guitar at the Solo's minimum gain just as the gear would.

## When something fails

| Message | What to do |
|---|---|
| "Couldn't open it" | Plug the Solo in, quit apps using it exclusively, check `--list`. |
| "It's running at 44100 Hz" | Audio MIDI Setup > Scarlett Solo > Format: 48,000 Hz. |
| "Round trip: not detected" | The blips didn't come back: level, cables, `--in-channel`. |
| The trainer's "Failed checks!" | Usually the validation copies differ (Replicate ESR): the gear changed; capture again. |
| "Your recording differs in length" | Don't edit the recording; use ampsim_capture's file as is. |
| A dropout warning | `--buffer 512`, close other apps, capture again. |

## What's been verified (and what hasn't)

- The play-and-record engine against simulated devices (tests/CaptureTests.cpp): recordings bit-exact
  against the input shifted by the round trip, exactly the input's length, for buffer sizes 64 to 333 and
  delays 0 to 1999 samples; the latency measurement exact; no allocation in the callback.
- On the real Solo, with nothing loud: a level check at -60 dBFS out of Output 1, recording the Solo's
  Loopback 1 input (a digital copy of what it plays): the recording matched the input file shifted by 314
  samples at a 128-sample buffer and 570 at 256, at exactly the gain sent, with a residual at the level of
  24-bit rounding. That's the same callback path a real capture uses.
- The whole workflow on a synthetic capture (`tools/synthetic_capture_check.py`): the app's Distortion
  circuit as the "gear", trained with NAM's trainer, played in our engine, compared on a guitar DI the
  trainer never saw (numbers in docs/PROGRESS.md).
- **Not verified**: real gear, a reamp box, a load box, a mic, the Solo's knob and switch names, and the
  levels suggested here. The first real capture is the test.

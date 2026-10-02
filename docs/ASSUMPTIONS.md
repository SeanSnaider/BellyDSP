# Assumptions to review

Sean asked on 2026-10-01 for the whole build to run to completion without stopping for questions, with every assumption written down here so we can go through them once the program is done. Each entry says what was assumed, why, and what changing it would take. Nothing here is final: the decision log in `BUILD_PLAN.md` records what's been settled, and this file records what Claude decided on Sean's behalf.

Status key: **Open** (waiting for Sean), **Confirmed** (Sean agreed), **Changed** (Sean chose differently; the change is noted).

## Workflow

| # | Assumption | Why | To change it |
|---|---|---|---|
| W1 | Phases stay on stacked branches (`phase-4` starts from `phase-3`, and so on), and nothing merges into `main` until Sean has played it. | That was the agreed rule ("merges into main after Sean has played it"). Running to completion means phases can't wait for playing sessions, so they stack instead. | Merge the branches in order once each is played; or merge everything at once. |
| W2 | Things only Sean can do are recorded as tasks in `PROGRESS.md` and skipped, not blocked on: live playing checks, listening, real DI recordings, community captures, real IR packs, tone choices. | Running without questions. | Nothing to change; work through the list. |
| W3 | No new system tools are installed (no Homebrew, ngspice, PyTorch). Python prototypes run through `uv` with numpy/scipy, which CLAUDE.md already sanctions. | Installing software is a bigger step than running code, and Sean declined some installs earlier. | Approve an install, and the matching follow-up in `PROGRESS.md` gets done. |

## Phase 3: cab packs (movable mics)

| # | Assumption | Why | To change it |
|---|---|---|---|
| C1 | File-name keywords place IRs across the speaker: Cap / Center 0, CapEdge 0.25, CapCone 0.4, Cone 0.6, Edge 1.0. Distances read from `1in`, `2.5cm`, `10mm` and are scaled so the closest capture is 0 and the farthest 1. | The common naming conventions in IR packs; the exact numbers are a guess at how far apart those positions are. | Edit the keyword table in `CabPack.cpp`, or put a `cabpack.json` in the pack folder, which overrides names entirely. |
| C2 | If names don't say where every mic was (or two files land on one spot), the files are laid out left to right in name order, as a 1D slider, and the status says so. | Better than refusing the pack. | Refuse such packs instead. |
| C3 | A pack uses each IR's first (left) channel; the close mic's left/right setting applies only to single IR files. The room mic doesn't take packs. | Packs are close-mic captures; a stereo room with positions isn't in the plan. | Per-pack channel choice is easy to add. |
| C4 | All IRs in a pack must share one sample rate; a pack that mixes rates is refused with a message. | Morphing needs a common frequency grid. Resampling at load is possible but wasn't worth it without real packs to test. | Resample at load. |
| C5 | Minimum-phase reconstruction floors the spectrum 120 dB below its peak and gives the DC and Nyquist bins their neighbours' values. | Real cab IRs have near-exact zeros at DC and Nyquist, which otherwise leave a -60 dB error spread across the morphed IR; with the fix it's -77 dB. Neither change is audible (0 Hz, 24 kHz, -120 dB). | Nothing to decide unless a real pack misbehaves. |
| C6 | IR loudness matching is now computed exactly from the IR's K-weighted energy instead of measured with 4 s of white noise. It agrees with the measurement within 0.034 dB and is 500 times faster. | A moving mic re-matches every 40 ms, and the measurement took 70 ms. Recorded in the decision log. | Nothing to decide; the measured version stays in the tests as the reference. |
| C7 | The position pad shows the cap on the left, the cone's edge on the right, the closest capture at the top and the farthest at the bottom. A new mic starts at the cap, closest (0, 0). | Matches how most movable-mic UIs read; the real UI is Phase 11. | Flip the axes in `MicPositionPad`, change the parameter defaults. |
| C9 | Input calibration is on by default, with the interface's full scale at +12 dBu: the Solo 4th Gen instrument input's maximum level at minimum gain, from Focusrite's spec as I remember it (not checked against the manual). With the gain knob turned up, the real figure is lower by the knob's gain. | Calibration is what makes captures react to the guitar the way the real amp would; NAM's own plugin uses the same idea. | Turn it off by default, or change the default level once the Solo's manual (or a measurement) gives the real number. |
| C10 | A calibration change reloads every loaded capture once the setting has been still for 300 ms. | A reload is the only way to re-measure loudness with the new input level; reloading per knob step would flood the loader. | Shorter or longer settle time. |
| C8 | The processor's housekeeping timer now runs at 50 Hz (was 20 Hz) so a dragged mic re-morphs every 40 ms. It's a few microseconds of message-thread work per tick. | The plan's 40 ms rate needs ticks at least every 20 ms. | Lower it and accept slower re-morphing. |

## Phase 4: compressor and EQ

| # | Assumption | Why | To change it |
|---|---|---|---|
| E1 | Both compressors start switched off; the EQs start on but flat (which is bit-transparent). The plan implies the pre compressor starts on. | With its default auto makeup (+9 dB at 4:1, -24 dB threshold) a compressor that's on in a fresh session pushes every amp noticeably harder before you've chosen to use it. The style presets (Phase 5) switch it on where the style wants it. | Flip the `comp_pre_on` default in `PluginProcessor.cpp`. |
| E2 | Auto makeup compensates the reduction at a -12 dBFS input (about where a DI's and the post-cab signal's peaks sit). | The plan says "auto-makeup" without a reference level; at 0 dBFS it would add far too much gain for guitar levels. | Change `autoMakeupReferenceDb`. |
| E3 | Auto release: a 60 ms release and a slow smoother (400 ms attack, 1.5 s release) side by side, the larger reduction winning. | A standard program-dependent design: short peaks recover fast, long squashing recovers slowly without pumping. The numbers are starting points. | The constants in `Compressor.h`. |
| E4 | Pedal mode ignores the attack knob (fixed 2 ms), uses the other knobs as they are, and keeps the mix knob. | "Fast fixed attack and heavy squash" in the plan; heavy squash is up to the ratio knob. | Fix the ratio too, or give Pedal its own defaults. |
| E5 | The RMS detector averages over 10 ms, and the sidechain high-pass is 12 dB/oct. | Common choices; the plan names neither. | Constants in `Compressor.h`. |
| E6 | Graphic EQ bands: Q 0.9, exact octaves (62.5 Hz shown as 63), midpoints weighted 0.5 in the design. | The only way found to meet the plan's +-1 dB (the plain method reached 1.05 dB at best). | `prototypes/graphic_eq.py` has the study; change `graphicQ` and the weight together. |
| E7 | Parametric bands: +-18 dB gain, Q 0.1 to 18, defaults 100 Hz low shelf, 400 Hz / 1 kHz / 3 kHz peaks, 8 kHz high shelf. Cuts: low 20 Hz to 1 kHz (default 80 Hz), high 1 to 20 kHz (default 12 kHz). | Usual parametric EQ ranges; the plan doesn't give them. | `BlockParameters.cpp` and `Equalizer.h`. |
| E8 | Reordering a section, changing a band's type, and changing a cut's slope dip to the unprocessed signal for 10 ms each way rather than crossfading. Switching EQ modes does crossfade (both banks run during the fade). | A true crossfade between two orders or two filter types needs a second copy of the blocks; the dip is click-free, but a heavily processed sound is briefly unprocessed. | Duplicate the blocks and crossfade, at twice the CPU during changes. |
| E9 | Effect order is changed in the basic panel with < and > buttons. | Drag to reorder is the Phase 11 GUI. | Phase 11. |

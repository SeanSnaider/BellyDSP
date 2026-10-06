# Tone match

"Take in a song and recreate the tone of the lead guitar" (Sean's spec, 2026-10-04). This file says what tone match does, how (with the math and where it comes from), what it can't do, how accurate it measured on synthetic cases, and how to use it.

## What it can and can't do

A song holds only the output of a rig (the amp, its cab, the mic, the studio's EQ and compression), mixed with drums, bass, vocals, and a mastering chain. Nothing in that recording says which amp made it, so **the real amp can't be recovered**. Tone match finds the closest *fingerprint* with what BellyDSP already has:

- an amp slot (whichever capture each slot holds; the built-ins are Glass, Ember, Monolith),
- that slot's Gain (its input trim) and five tone knobs (Depth, Bass, Mid, Treble, Presence),
- one of the 21 built-in cabs in close mic 1,
- and a **match EQ**: the post EQ's five parametric bands, fitted to whatever difference is left, capped at +-12 dB.

It compares your playing (a DI recording) through those settings with the target and reports a closeness score. The score measures how close two sets of measurements came, not how it sounds. Only your ears can judge that.

What it can't do, as measured below: pin the Gain down within a few dB (a cranked crunch and a barely driven high-gain amp measure alike), pick the exact cab (it finds one of the same family), separate the lead from a rhythm guitar (Demucs gives every guitar), or work on a whole mix without separation (it fails: the drums and bass swamp the measurements).

## How to use it

1. Open it from the brand menu (top left, "BellyDSP"): **Match tone...**
2. **Target.** Choose a file (WAV, AIFF, FLAC, MP3, M4A/AAC, and anything else macOS decodes; on Windows, what Media Foundation decodes) or drop one on the page. Drag in the waveform to choose 3 to 60 s where the lead guitar dominates (drag the edges or the middle to adjust; a click places 30 s). **Play** plays that section, looped, with your guitar on top, so you can hear (and learn) what you're about to match; see "Play along" below. If it's a whole song, switch on **Separate the guitar first**; the first time, it downloads the separation model (55 MB) into the BellyDSP data folder. If anything in the download or the separation fails, the Match card says what (the HTTP status, no response, a damaged download, out of memory) and `~/Library/Application Support/BellyDSP/separation-log.txt` has the details.
3. **Your DI.** With a target loaded, **Record** is a play-along take: a count-in, then the section plays once while you play along and your clean DI records, lined up with the section, and it stops by itself at the section's end ("Play along" below). Without a target, Record records your DI on its own (up to a minute) until **Stop**. Or choose a DI file. Then the mode (Same part is chosen for you after a play-along take):
   - **Same part**: you played the same part as the target (learn the lick, play it). The two are lined up in time and compared moment by moment. The more precise mode, on a dry or isolated target.
   - **Anything**: you played anything. Play the same kind of part (a lead for a lead, in a similar register), because the long-term spectrum depends on which notes are played. Use this one on a separated stem.
4. **Match.** It takes a few seconds (plus about a minute per minute of audio when separating). Cancel stops it.
5. **Result.** The amp, Gain, tone, cab, match EQ (bands low to high), the curve the EQ was fitted to, and the score. **Apply** sets them as normal settings in one undo step (Cmd-Z puts everything back, the cab and the pre effects included), so a preset saves them. **Discard** throws the result away.

6. **Compare.** Once the result is in, the Match card plays three things, looped: **Target** (what was matched), **Match** (your DI through the result) and **Current** (your DI through what's set now). See "Comparing" below.

What Apply changes (ASSUMPTIONS TM8, TM19): the amp slot, its Gain and tone, the cab (close mic 1, with mic 2 and the room muted, cuts off), the post EQ (on, parametric, the five bands, cuts off), and it switches off the pre effects that color the tone before the amp, the pre compressor, boost, overdrive, and pre EQ, so the match is heard as it was made. The noise gate stays as it is. The page says this above Apply, with which of them are on right now.

From the command line: `prototypes/tone_match.py match TARGET DI --mode same|anything` (the prototype, through `ampsim_render`), and `ampsim_separate song.wav guitar.wav` (the separation, with its timing and memory).

## Comparing (A/B)

Sean's feedback on the first version: "hard to compare". So the page plays the song's guitar and your matched tone side by side, looped, without leaving it (ASSUMPTIONS TM24 to TM31).

**The sources.**

- **Target**: what was matched, the separated guitar stem when "Separate the guitar first" was on, otherwise the selected section of the file.
- **Match**: the DI the match used, rendered through what Apply would set (the slot, its Gain and tone, the cab in close mic 1, the match EQ as the post EQ, the pre effects off). The values are the ones the parameters will hold, so the render equals what Apply produces bit for bit (tested: `renderTone` on the settings read back from the processor after Apply gives the identical 432,000 samples).
- **Current**: the same DI through what's set now. It re-renders by itself when the slot, its knobs, its capture, close mic 1's IR, the amp or cab bypass, or the post EQ change and then stay still for 300 ms. After Apply it equals Match (tested).

Both renders run on the session's worker through `ToneMatcher::renderTone`: the chain's own `AmpSection`, close mic 1's IR as the cab plays it (the cab's own loading and loudness match), FFT convolution, and the chain's own `Equalizer`. They take about 3.6 s for a 9 s DI on this Mac, while the page stays usable, and Cancel stops them (bounded by a capture's load, which can't be interrupted: 1.2 s measured with a gain set). Not in them: the pre effects (Apply switches the coloring ones off, and Current leaves them out too), close mic 2, the room, the other post effects, the cab mic's level and pan, and the output level.

**Playing.** A preview player sits at the very end of the chain, after the output level and before the output limiter (so nothing it plays can clip the output). It's a chain block that's idle unless comparing, and idle it returns without touching the audio, so the live path is bit-identical to before (tested over 3 s of the riff, before a preview and from 50 ms after it). It adds no latency. The audio thread only reads buffers that were built and filled elsewhere and handed over with a `Handoff` (old ones handed back to be freed off the audio thread), and atomics for play, the source, the loops, and the levels. It's in the real-time safety test, which stays at 0 allocations, 0 frees, 0 blocking locks: material handed over, played for 408 buffers through its loop's wraps, switched three times, its loops moved, new material swapped in mid-play, stopped.

- **Play/Stop** (Space), **Target / Match / Current** (1, 2, 3), from anywhere on the page. Leaving the page stops it.
- **The loop** is the strip under the buttons: the matched section, the loop on it, and the playhead. Drag to set it, drag its edges or middle, double-click for the whole section (the default).
- **Mute my guitar while comparing** (on): the live guitar fades out over 20 ms while previewing and back after Stop, so the comparison isn't polluted. Off, you can play along.
- **Level match** (on): every source brought to the same BS.1770 loudness (that of the Current render, the level your rig plays at), so the comparison is about tone, not volume. Each source's own loudness is shown under its name. Measured through the processor, one loop of each: -19.29, -19.36, and -19.37 LUFS, a spread of 0.076 LU.
- **Level**: the comparison's own volume, -30 to +6 dB.

**Switching and looping.** A switch fades the old source out and the new one in over 20 ms with an equal-power crossfade: gains sin(theta) and cos(theta) of the same angle, whose squares sum to 1. That holds the level for two different, uncorrelated signals (measured: within 0.03 dB of the sines' level through a switch), where the chain's linear bypass fade, made for two versions of one signal, would dip 3 dB. The loop wraps the same way: 20 ms before its end the playing stream fades out, reaching the end as its gain reaches 0, while the same source fades in from the loop's start. Measured on two sines (220 and 331 Hz, -6 dBFS): the largest sample-to-sample step through a switch is 0.026 and through a wrap 0.014, under the 0.036 two such sines can make on their own; a hard switch would have jumped 0.014 here and a hard wrap 0.375.

**Keeping the place.** Match and Current share the DI's clock, so a switch between them continues at the exact next sample (tested). Between the target and the DI:

- *Same part*: the two are lined up through the DTW path the matcher already found (against the winning slot's render): each analysis frame's mean partner on the other clock, at the frames' centres, interpolated linearly. The DI's loop is the target's loop mapped the same way (measured on the fixtures: the target's 2.0 to 6.0 s is the DI's 1.957 to 5.829 s, and a switch 1.5 s in continued at 0.402 of the DI's loop against the target's 0.375 plus 0.1 s). The path has the 43 ms hop's resolution.
- *Anything*: nothing corresponds, so the target loops the chosen range and the DI its whole take, each from its own start, and a switch keeps the offset into the loop. The page says so under the progress bar.

**The spectra.** The result's curve also shows the target's and the match render's long-term spectra as thin lines (the target in ink, the match in emerald), third-octave smoothed, level-matched to each other with the fit's weights, on the EQ's own dB scale with the target's loudest counted band at +9 dB, so the gap between them reads against the EQ curve.

## Play along

Sean's request (2026-10-06): "Allow for playback of the original audio when not only inputting it, but recording your section of it, so that you can play along to it rather than guess where you are", and then "also add a count-in for starting if playing with the audio" (ASSUMPTIONS TM32 to TM40).

**Hearing the target.** The Target card's **Play** plays the selected section through the output, looped, after a count-in if "Count-in before Play" is on (off by default). Your guitar is not muted: you hear your rig with the song, so you can learn the part. **Song level** (-30 to +6 dB) sets the song's level, here and while recording. Once a separated match of exactly this section has made a guitar stem, **Full song / Guitar only** chooses between the section and its stem (they share a clock, so a switch keeps the exact place); another range takes Guitar only away, because the stem was of the old one. It plays through the same preview player as the A/B (sources 3 and 4 of the same Material, the same handoff, the same 20 ms equal-power crossfades at start, stop, and the loop's wrap), and starting one stops the other.

**The count-in.** Clicks before the song: on before Record (default) and, separately, before Play (default off); **4 beats** (default) or **2**. The click is a short sine burst (1 ms rise, 8 ms decay, 40 ms, -6 dBFS peak), 1760 Hz on beat 1 and 1320 Hz on the others, rendered once on the message thread into the Material, so the audio thread only reads it; **Click** sets its level (default -6 dB), apart from the song's. Click k sounds round(k B) samples after the start, where B = 60 x 48000 / BPM samples is the beat (not necessarily whole), and the song's first sample sounds at round(N B), one beat after the last click: the song's voice is spawned at the start with that delay, so the timing is the audio thread's own sample count. Measured in uneven buffers: 133 BPM, 4 beats: clicks at 0, 21654, 43308, 64962 and the song at 86617; 97.3 BPM, 2 beats: 0, 29599, song at 59198 (each exactly round(k B)). The page shows the beats left, 4 3 2 1, large on the waveform, and "Count-in: 3" under Record.

**The tempo.** The count-in follows the app's tempo (`tempo_bpm`, with its Tap on the Output page) until one is set on the page: **Tempo** (drag, scroll, or double-click to type; 30 to 300 BPM) and **Tap**, with TapTempo's rules. The page also measures the section's tempo and offers it as **Song 120** (a click uses it): onset autocorrelation after Ellis (2007), on a worker 250 ms after the range stops moving (23 ms for a minute of audio), so it never holds anything up (`src/tonematch/TempoEstimate.*`, ported from `prototypes/tempo_estimate.py` and golden-tested against it: the same BPM to two decimals on five fixtures):

1. onset strength: the spectral flux of log power (2048-point Hann, 10 ms hop, 30 Hz to 8 kHz), each bin's rise since the last frame summed, falls ignored;
2. minus its 1 s moving average, negatives to 0, then its mean removed (without that, the mean squared dominated the autocorrelation and white noise looked periodic at 0.34);
3. its unbiased autocorrelation r at lags 0.25 to 1.5 s, weighted by Ellis's preference W(tau) = exp(-0.5 (log2(tau / 0.5 s) / 1.4)^2), the peak refined by a parabola;
4. offered only if r there is at least 0.15 r(0).

Measured (`tempo_estimate.py --study`): exact on the synthetic band (120.0 for 120) and on two of the solo leads (120.0, 96.0); half or double on a riff and a lead (216 for 108, 69 for 138: still in time for a count-in); 4:3 off on lead_a's runs (154 for 116, 159 for 120: not); white noise gets no suggestion (0.095). So it's a suggestion with its number shown, nothing more: on a solo guitar line check it, or Tap.

**The take.** Record with a target loaded: the recorder is armed, the song (or its guitar) is set to play once from the section's start, and the count-in begins. The processor starts the recorder on the sample where the song's first sample sounds, in the same callback: the player reports that sample's index in the buffer (`songStartedAt()`), and `DiRecorder::beginArmed` starts there (an atomic idle/armed/recording state with a compare-exchange, so a Stop racing the start can't leave it recording). So recording sample 0 is the song's first sample, tested in uneven buffers (128, 100, 37, 1, ... samples): with the count-in on and off, the recording's sample 0 is exactly the input sample at which the song started, and all 192,000 samples follow in order. The song plays once and fades out over its last 20 ms; your guitar plays through your rig as always, not muted: the output is the live path plus the song (tested: with the song playing, output minus the same processor without the take is the section at -6 dB within 2.3e-8; with the song silent and no count-in, 5 s of the riff with the gate on is identical, sample for sample, to the processor without the take). The page shows the playhead on the waveform, the recorded part shaded, and "Recording... (Stop)"; the section can't be moved during a take. The take ends by itself once it covers the section; Stop earlier keeps what lines up with the section so far (3.5 s of a 5 s section, tested), if that's at least 3 s. Then the mode switches to Same part (you can switch back).

**Lining it up.** The song's sample p leaves the callback at t0 + p (t0: the song's first sample), reaches your ears the device's output latency later, and what you play against it reaches the callback the input latency after that: at t0 + p + L, with L = input latency + output latency (+ the **Offset**, -50 to +200 ms, for when the reported numbers are off). The recording starts at t0, so its sample p + L is what you played against song sample p: the reference is the take from L on, the section's length, and the recorder runs L past the section's end to catch the last notes. The latencies are the ones the device reports (JUCE's `getInputLatencyInSamples()` and `getOutputLatencyInSamples()` through the standalone's StandalonePluginHolder, `platform/DeviceLatency.h`; CoreAudio's include the device latency, its safety offset, the stream latency, and a buffer each way; ASIO's are the driver's). Tested with a fake device: reported 137 + 211 samples shift the take by exactly 348 (7.25 ms), the offset adds exactly its samples (2 ms: 444), and a loopback "player" with a 348-sample round trip (the input is the output 348 samples earlier) lines up with the section at lag 0 by cross-correlation; reported as 0, it lags by the whole 348. Outside the standalone app (a plugin later) the latency isn't known, so only the offset applies, and the page says so.

**Matching a take.** In same-part mode a take of the section selected now is already lined up (its sample 0 is the section's), so DTW runs only within a Sakoe-Chiba band of 0.5 s (12 frames) either side of that diagonal (`align(..., bandFrames)`, prototyped first in `tone_match.py` and golden-tested: the banded path's length, checksum, and mean cost equal the prototype's). A fixed offset (no DTW) was tried in the prototype and didn't hold up: on six synthetic play-along cases (the C++ search test's three settings, a take at the target's tempo with 12 ms timing jitter, lined up and 80 ms late) a near-fixed alignment (a one-frame band) scored worse in four (Glass 2.83 against 2.45, and all three 80 ms late ones), so DTW stays. Measured (`play along, measured`, ToneMatchTests):

| Take | Case | Full DTW: off the truth (frames, mean / max) | Band 0.5 s | Matched (both) | Combined vs best default |
|---|---|---|---|---|---|
| lined up | Glass +9 | 1.17 / 9 | 1.17 / 9 | Glass +9.0, Vintage 4x12 upper var. 1 | 2.25 vs 5.19 |
| lined up | Ember -7 | 0.09 / 1 | 0.09 / 1 | Ember -6.0, the true cab | 2.60 vs 7.34 |
| lined up | Monolith +3 | 0.35 / 3 | 0.35 / 3 | Monolith -6.0, the true cab | 2.97 vs 7.67 |
| 80 ms late | Glass +9 | 1.39 / 10 | 1.39 / 10 | Glass +7.5 | 2.82 vs 5.18 |
| 80 ms late | Ember -7 | 0.40 / 2 | 0.40 / 2 | **Glass** +21.0 | 2.85 vs 7.43 |
| 80 ms late | Monolith +3 | 1.28 / 14 | 1.28 / 14 | Monolith +6.0, the true cab | 3.28 vs 7.71 |

(Off the truth: |j - (i + shift)| over the target's playing frames, a frame being 43 ms.) On these cases the band changed nothing: unconstrained DTW already finds a take at the same tempo, and the band's matches, paths, and scores are identical. What the band buys is a bound: the path can't wander further than 0.5 s from where the take was recorded, which a repetitive riff or a missed bar could otherwise tempt it to; the synthetic leads don't test that. A take recorded against another range (or another file) isn't lined up with what's selected now, so it gets the unbanded DTW.

**Not verified (only Sean can).** Whether a take lines up on his interface: the reported latencies at 128 samples on the Scarlett Solo (macOS) and over ASIO on Windows, and whether the offset is needed; how the count-in and the song feel to play along with, the click's sound and level; and whether the tempo suggestion is right on his songs (PROGRESS TM.7).

## How it works

### The split: render the nonlinear part, fit the linear part

The chain a match configures is

```
DI -> Gain -> amp model (nonlinear) -> tone bands -> cab IR -> post EQ
```

Everything after the amp model is linear and time-invariant. For a fixed slot and Gain, the output's long-term power spectrum is the amp output's times |H(f)|^2 of the linear part, so the tone knobs and the EQ can be scored on spectra without rendering anything. Only (slot, Gain) needs real renders:

1. every slot at Gain -18, -12, ..., +18 dB (21 renders, in parallel) through a private copy of the chain's amp block (`AmpSection`; bit-exact against the whole chain with effects off and no cab, tested);
2. for each render, all 21 cabs screened on the predicted spectrum (the amp's spectrum times each IR's |H|^2), and the best four convolved with the render (FFT overlap-add: the cab block's output up to its loudness normalization, within -132 dB in the test) and scored;
3. Gain refined for the best two slots: +-3 dB, then +-1.5 dB around the best, with each slot's four best cabs;
4. the best candidate's tone knobs polished, then the match EQ fitted to what's left.

### The spectral error

Spectra come from short-time Fourier transforms (8192-point periodic Hann frames every 2048 samples at 48 kHz, 5.9 Hz bins), averaged into sixth-octave bands centred on 80 x 2^(i/6) Hz, i = 0 .. 43 (80 Hz to 11.5 kHz), about the ear's resolution for timbre. A frame counts as playing if its level is within 30 dB of the 95th percentile; the level is taken with each band relative to its own median over the signal, so no filter changes which frames count.

The long-term average spectrum (LTAS) is the mean band power over the playing frames, in dB. The residual r is target minus candidate, smoothed across bands with [1/4, 1/2, 1/4] (about a third of an octave, so a fit follows the tone's shape rather than the notes played). The error is the weighted RMS once the weighted mean is removed (levels are matched separately):

```
E = sqrt( sum_b w_b (r_b - rbar)^2 / sum_b w_b ),   rbar = sum_b w_b r_b / sum_b w_b
```

w_b is the A-weighting curve (IEC 61672-1) at the band's centre as a factor, floored at 0.25, times a confidence: 1 within 20 dB of the target's loudest band, falling linearly to 0.05 at 35 dB below. The confidence came from the study: two different leads through identical settings differed by 16 dB at 300 to 450 Hz, below the target's lowest note, where the target simply has nothing.

### Distortion features

Spectra alone can't tell a clean amp with the treble up from a distorted one. Four features, measured on each candidate's actual output (after its cab), describe distortion and compression. They use each frame's level relative to the long-term spectrum (each band minus its LTAS value, power-averaged), which no linear filter changes:

- **level spread**: the 90th minus the 10th percentile of that level over the playing frames (distortion compresses, so it shrinks);
- **level flux**: the median change from one frame to the next (clean notes rise and decay; distorted ones hold);
- **brightness movement**: the standard deviation of the whitened 2 to 8 kHz level minus the 150 to 800 Hz level (how much the brightness moves);
- **crest factor**: the median peak over RMS of the playing frames, in dB (clipping lowers it).

The distortion distance is D = sum_k |f_k(target) - f_k(candidate)| / s_k with scales s = (2, 0.4, 1.5, 0.5), about twice each feature's spread across different performances at the same settings. A candidate scores E + 0.5 D.

The feature study (`tone_match.py features`: five lead performances through nine settings) chose these by the ratio of how much a feature moves between settings to how much it moves between performances: crest factor 10.5, level spread 6.2, level flux 5.4, brightness movement 4.1. Spectral flatness of whitened frames, the first idea, scored 0.31: it measured how many notes were sounding.

### Same part: dynamic time warping

When the player played the target's part, the two can be compared moment by moment once they're lined up. Each frame gets a chroma vector (Fujishima 1999; Mueller 2015, ch. 3): the magnitude of every bin from 60 Hz to 2.1 kHz added to its nearest pitch class, log-compressed as log(1 + 100 c), and scaled to unit length (a frame 100 dB below the loudest is silence and gets the all-equal vector). Distortion and cabs change timbre far more than pitch classes. Matching target frame i with candidate frame j costs the cosine distance C(i, j) = 1 - c_i . c_j, and dynamic time warping (Sakoe and Chiba 1978) finds the cheapest monotone path with steps (1,1), (1,0), (0,1), both ends anchored:

```
D(i, j) = C(i, j) + min( D(i-1, j-1), D(i-1, j), D(i, j-1) )
```

computed row by row with a running min-plus scan, once per slot against its render at Gain 0. Along the path, the per-band mean of the dB differences is what the linear part must fit, and the RMS of the aligned level-envelope difference (mean removed) adds to the distortion distance (1 dB per unit).

### The fits

**Tone knobs.** The five bands' responses are evaluated exactly: the TPT state-variable filter's response equals its analog prototype's at Omega = tan(pi f / fs) / tan(pi fc / fs) (Simper 2013; Zavalishin 2012; `Svf::responseAt`). First a linearized least-squares fit (each band's dB response per dB of gain, taken at +6 dB) with a ridge penalty of 0.01 dB^2 per dB^2 of knob, so 10 dB on one knob costs as much as 1 dB of error and the knobs don't chase note-by-note differences; then Nelder-Mead (Nelder and Mead 1965, standard coefficients, bounds by clamping) on the exact responses.

**Match EQ.** The residual after the tone knobs is scaled toward 0 dB by each band's confidence (no evidence, no correction), capped at +-12 dB, and the post EQ's five bands (a low shelf, three peaks with Q 0.3 to 3, a high shelf) are fitted to it by Nelder-Mead on log frequency, gain, and log Q with a small ridge on band gain, then again with a smaller simplex.

**Closeness.** 100 exp(-(E after the EQ + 0.5 D) / 6 dB): 100 is identical; good synthetic matches land around 50 to 70, poor ones 20 to 40.

### Separation (Stage C)

Demucs v4, the hybrid transformer with six sources (htdemucs_6s: drums, bass, other, vocals, guitar, piano; Rouard, Massa, and Defossez 2023), run by demucs.cpp (sevagh/demucs.cpp, MIT) on Eigen. The target is resampled to 44.1 kHz stereo, normalized as Demucs does, split into 10 s parts with 0.75 s of context on each side (demucs.cpp's time grows faster than the length: one 60 s part took 532 s), run by up to four workers, crossfaded back with triangular weights, and the guitar source kept.

A part that is digital silence isn't run: Demucs normalizes each input by its standard deviation over time, `(x - mean) / std`, and silence has none, so the division made every output NaN. The crossfade carried that into the next part and the resampler's IIR filter to the end of the stem: before the fix, 30 s of a song with a 12.5 s silent intro came back as 1,439,998 NaN samples out of 1,440,000, and the matcher then said the target had too little playing in it. Now the silent part's stem is silence, and any non-finite output sample is zeroed and logged.

#### Memory: how many workers

Each worker holds a part's activations while it runs. Peak memory of `ampsim_separate` on a 60 s, 44.1 kHz MP3 section on this Mac (M5 Pro, 24 GB, 15 cores), macOS's peak memory footprint (another build was running at the same time, so the times are slower than an idle machine's):

| Workers | Peak memory | Time per minute of audio |
|---|---|---|
| 1 | 1.82 GB | 152 s |
| 2 | 3.34 GB | 68 s |
| 3 | 4.92 GB | 53 s |
| 4 | 5.66 GB | 50 s |

About 1.5 GB per worker on top of the model, measured. The earlier measurement of four workers peaked at 8.9 GB (its resident size, on a different run), and one short part has measured 3.0 GB resident, so the rule budgets **2.2 GB per worker**: workers = floor(min(40% of physical memory, memory available now - 1.5 GB) / 2.2 GB), at least 1, at most 4 and half the cores ("available" is free plus inactive, purgeable, and file-cache pages, from the kernel's VM statistics). That gives 4 on a 24 GB Mac with most of it free, 2 on 16 GB, 1 on 8 GB, and fewer when other apps hold the memory. If a worker still runs out of memory, the separation is tried again with one, and if that fails too, the page says "not enough memory to separate" instead of a generic error. The progress text says how many parts there are and how many run at a time.

#### Installing the model

The download goes through JUCE's URL stream (NSURLSession on macOS), which follows Hugging Face's 302 to its CDN (a different host, with a signed query string of about 1 KB) untouched. Each try checks the HTTP status (a 404 or 503 page is never saved as the model), allows 60 s without a byte (it was 20 s, which also counted the time to the first byte), and a dropped connection resumes where it stopped with a Range request; up to 5 tries, 2, 4, 8, and 16 s apart. Then the size, the SHA-256, the conversion, and the move into place, each with its own message. A model file that doesn't load is deleted so the next try downloads it again. Every step is logged with its details (HTTP status, bytes, times, worker count, peak memory) to `separation-log.txt` in the BellyDSP data folder, capped at 256 KB with one older file kept.

End to end on this Mac, through the page's own code path from an empty model folder (the network test below), a 30 s section (37.3 to 67.3 s) of a synthetic 44.1 kHz stereo MP3 song: download 1.6 s (HTTP 200 after 0.5 s), installed in 2.2 s, separated in 43.7 s with 3 workers (3 parts), the whole match done in 60.4 s, the process's peak memory 4.3 GB.

The weights are downloaded on first use from the Demucs author's Hugging Face repository at a pinned commit, checked against a SHA-256, and converted to demucs.cpp's format (the same f16 tensors, squeezed, in its container). They aren't re-hosted (docs/RELEASING.md, "Tone match's separation model").

## Licences (checked 2026-10-04 against the primary sources)

- **Demucs code**: MIT, "Copyright (c) Meta Platforms, Inc. and affiliates" (the LICENSE of github.com/facebookresearch/demucs, and its README: "Demucs is released under the MIT license"; the maintained fork github.com/adefossez/demucs says the same).
- **Demucs weights**: no licence is stated anywhere. The original checkpoints are fetched by the MIT code from dl.fbaipublicfiles.com; the fork's Hugging Face repository (huggingface.co/adefossez/HTDemucs-6s, the file this app downloads) carries no licence. The models were trained on MUSDB18 plus 800 songs (the README), and MUSDB18's tracks "can only be used for academic purposes" (sigsep.github.io/datasets/musdb.html; some are CC BY-NC-SA). So they're downloaded from upstream and never redistributed.
- **demucs.cpp**: MIT, "Copyright (c) 2023 Sevag H" (its LICENSE). In the notices, with Demucs's MIT notice.

## Measured accuracy (Stage A, synthetic ground truth)

Every signal is synthetic: Karplus-Strong plucked-string DIs (Karplus and Strong 1983) with a pickup resonance, rendered through the real chain (`ampsim_render`) with known settings; then optionally a synthetic room (exponentially decaying noise, RT60 1.4 s, -12 dB), a synthetic band (drums and bass at the guitar's level), or that band separated by Python's Demucs. Anything mode matches a different lead performance (another melody, another seed); same-part mode matches the same melody played as a different take (other plucks, 4% slower, 8 ms timing jitter). The reference DI is 17 s (anything) or 9 s (same part).

The errors come from rendering the recovered settings through the real chain and comparing with the **clean** guitar target (for the reverb, mix, and separated cases: the guitar before the room and the band), scored as the matcher scores: the spectral error after the match EQ (dB), the distortion distance, and combined = spectral + 0.5 x distortion. "Best default" is the best of the three amps at Gain 0, tone flat, the factory presets' usual cab, no EQ: what you'd have without tone match. Bold amps are wrong.

| # | Mode | Target | True: amp, Gain, tone D/B/M/T/P, cab, EQ | Recovered: amp, Gain, tone, cab | Spectral error (dB) | Distortion distance | Combined | Best default |
|---|---|---|---|---|---|---|---|---|
| 1 | anything | dry | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass +3.0, +0/-0/+0/+4/-0, Vintage 4x12, dynamic, lower | 1.82 | 3.20 | 3.42 | 5.13 |
| 2 | anything | dry | Glass -6, +2/-1/+2/-3/+0, Vintage 4x12, supercardioid, upper, pk 2500 Hz +4.0 | Glass -12.0, +1/-1/+2/+1/+1, Vintage 4x12, dynamic, lower | 1.72 | 3.45 | 3.44 | 5.23 |
| 3 | anything | dry | Glass +18, +0/+0/+0/+0/+0, Vintage 4x12, dynamic, upper, var. 1, none | Glass +12.0, -1/-2/+1/+2/-1, Vintage 4x12, dynamic, lower | 1.91 | 1.75 | 2.79 | 7.51 |
| 4 | anything | dry | Ember +2, +1/+2/-2/+0/+3, Modern 4x12, dynamic, 75 W, var. 3, none | Ember -6.0, +2/+3/+3/+4/+2, Modern 4x12, dynamic, 75 W, var. 1 | 2.04 | 2.66 | 3.38 | 6.05 |
| 5 | anything | dry | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | Ember -12.0, +1/+2/+4/+2/+3, Modern 4x12, dynamic, dark 60 W | 2.03 | 1.63 | 2.84 | 9.29 |
| 6 | anything | dry | Ember +14, +0/-3/+2/+0/-2, Vintage 4x12, dynamic, lower, none | Ember +12.0, -0/-1/+8/+2/+1, Vintage 4x12, dynamic, lower | 1.74 | 2.88 | 3.18 | 6.83 |
| 7 | anything | dry | Monolith +3, +4/-2/-5/+2/+1, Modern 4x12, dynamic, dark 60 W, none | Monolith -7.5, +2/+2/+0/+4/+3, Modern 4x12, dynamic, dark 60 W | 1.85 | 1.32 | 2.51 | 7.93 |
| 8 | anything | dry | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith -10.5, +1/+2/+6/+1/+2, Modern 4x12, dynamic, bright 60 W, var. 3 | 1.68 | 2.04 | 2.70 | 7.16 |
| 9 | anything | dry | Monolith +15, +0/+0/-6/+2/+0, Modern 4x12, vocal mic, bright 60 W, none | Monolith -7.5, +2/+3/+1/+3/+5, Vintage 4x12, dynamic, upper, var. 1 | 1.87 | 7.20 | 5.47 | 9.20 |
| 10 | same part | dry | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass +9.0, +1/+1/-0/+5/+2, Vintage 4x12, dynamic, upper, var. 1 | 0.92 | 9.51 | 5.67 | 9.98 |
| 11 | same part | dry | Glass -6, +2/-1/+2/-3/+0, Vintage 4x12, supercardioid, upper, pk 2500 Hz +4.0 | Glass -4.5, +2/+2/+0/+3/+5, Vintage 4x12, dynamic, upper, var. 2 | 0.99 | 10.88 | 6.43 | 7.66 |
| 12 | same part | dry | Glass +18, +0/+0/+0/+0/+0, Vintage 4x12, dynamic, upper, var. 1, none | Glass +16.5, -0/-0/-1/+2/+1, Vintage 4x12, dynamic, upper, var. 2 | 0.93 | 6.70 | 4.28 | 8.93 |
| 13 | same part | dry | Ember +2, +1/+2/-2/+0/+3, Modern 4x12, dynamic, 75 W, var. 3, none | Ember -3.0, +1/+2/-2/+1/+1, Modern 4x12, dynamic, 75 W, var. 2 | 1.04 | 5.60 | 3.84 | 8.18 |
| 14 | same part | dry | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | Ember -10.5, -0/+0/-1/+1/+2, Modern 4x12, dynamic, dark 60 W | 0.70 | 6.44 | 3.91 | 10.42 |
| 15 | same part | dry | Ember +14, +0/-3/+2/+0/-2, Vintage 4x12, dynamic, lower, none | **Monolith** -22.5, -1/-1/-2/-0/-1, Vintage 4x12, dynamic, lower | 0.67 | 4.41 | 2.87 | 5.22 |
| 16 | same part | dry | Monolith +3, +4/-2/-5/+2/+1, Modern 4x12, dynamic, dark 60 W, none | Monolith -9.0, +4/+6/-6/+2/+1, Modern 4x12, dynamic, dark 60 W | 0.62 | 2.95 | 2.10 | 10.93 |
| 17 | same part | dry | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith -9.0, +1/+2/+0/-0/+1, Modern 4x12, dynamic, bright 60 W, var. 3 | 0.69 | 2.93 | 2.16 | 8.02 |
| 18 | same part | dry | Monolith +15, +0/+0/-6/+2/+0, Modern 4x12, vocal mic, bright 60 W, none | Monolith -6.0, +2/+4/-8/+2/+1, Modern 4x12, blend, dark + bright 60 W, 1 | 0.63 | 2.86 | 2.06 | 10.02 |
| 19 | anything | reverb | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass +0.0, +0/-0/+1/+5/-1, Vintage 4x12, dynamic, lower | 1.85 | 3.70 | 3.70 | 5.13 |
| 20 | anything | reverb | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | Ember -16.5, +1/+2/+4/+2/+2, Modern 4x12, dynamic, dark 60 W | 2.12 | 3.71 | 3.97 | 9.29 |
| 21 | anything | reverb | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith -10.5, +1/+1/+6/+1/+2, Modern 4x12, dynamic, bright 60 W, var. 3 | 1.74 | 2.15 | 2.81 | 7.16 |
| 22 | same part | reverb | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass +3.0, +1/+2/-0/+5/-1, Vintage 4x12, dynamic, lower | 3.95 | 9.27 | 8.59 | 9.98 |
| 23 | same part | reverb | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | **Glass** +12.0, -0/-0/+2/-3/-2, Modern 4x12, blend, dark 60 W + 75 W | 2.19 | 6.94 | 5.66 | 10.42 |
| 24 | same part | reverb | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith -15.0, +1/+2/-0/-2/-1, Modern 4x12, dynamic, bright 60 W, var. 3 | 1.17 | 3.74 | 3.04 | 8.02 |
| 25 | anything | mix | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass +4.5, +12/+12/-12/-0/-12, Modern 4x12, supercardioid, dark 60 W | 7.71 | 9.82 | 12.62 | 5.13 |
| 26 | anything | mix | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | **Glass** +9.0, +12/+12/-9/-7/-12, Modern 4x12, supercardioid, dark 60 W | 7.07 | 6.99 | 10.57 | 9.29 |
| 27 | anything | mix | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith +6.0, +12/+12/-6/-1/-3, Modern 4x12, supercardioid, dark 60 W | 7.33 | 5.78 | 10.22 | 7.16 |
| 28 | same part | mix | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | **Monolith** -6.0, +4/+11/-12/-9/-12, Modern 4x12, supercardioid, dark 60 W | 9.50 | 22.85 | 20.93 | 9.98 |
| 29 | same part | mix | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | **Monolith** -13.5, +5/+12/-9/-11/-12, Modern 4x12, dynamic, dark 60 W | 8.18 | 14.94 | 15.65 | 10.42 |
| 30 | same part | mix | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith -3.0, +6/+12/-7/-6/-12, Modern 4x12, supercardioid, dark 60 W | 8.47 | 6.22 | 11.58 | 8.02 |
| 31 | anything | separated | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass +0.0, -2/-2/+2/+5/-2, Vintage 4x12, dynamic, upper, var. 1 | 2.47 | 4.34 | 4.64 | 5.13 |
| 32 | anything | separated | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | Ember -18.0, -1/-2/+7/+4/+2, Modern 4x12, dynamic, dark 60 W | 2.35 | 4.48 | 4.59 | 9.29 |
| 33 | anything | separated | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith +4.5, -1/-2/+8/+2/+3, Modern 4x12, dynamic, bright 60 W, var. 3 | 1.97 | 1.74 | 2.84 | 7.16 |
| 34 | same part | separated | Glass +10, +0/+2/-1/+3/+1, Vintage 4x12, dynamic, upper, var. 3, none | Glass -4.5, -1/-2/+5/+0/-6, Modern 4x12, supercardioid, dark 60 W | 8.33 | 12.56 | 14.61 | 9.98 |
| 35 | same part | separated | Ember -8, +0/+0/+0/+0/+0, Modern 4x12, blend, 75 W + bright 60 W, 2, hs 6000 Hz -4.0 | **Glass** +15.0, -0/-0/+3/-8/-8, Modern 4x12, supercardioid, dark 60 W | 5.40 | 6.27 | 8.54 | 10.42 |
| 36 | same part | separated | Monolith -12, -2/+3/+0/-2/+4, Modern 4x12, dynamic, bright 60 W, var. 1, ls 120 Hz +3.0 | Monolith +0.0, -0/-0/+2/-3/-2, Modern 4x12, dynamic, bright 60 W, var. 1 | 3.57 | 5.62 | 6.37 | 8.02 |

Summary of the test cases. The constants (lambda, the feature scales) were tuned on a separate set of six cases with other settings, melodies, and seeds, which scored the amp right 6/6 in both modes.

| Mode, target | Amp right | Exact cab | Median Gain error | Median spectral error | Median combined vs best default | Beat the best default |
|---|---|---|---|---|---|---|
| anything, dry | 9/9 | 2/9 | 6.0 dB | 1.85 dB | 3.18 vs 7.16 | 9/9 |
| same part, dry | 8/9 | 2/9 | 3.0 dB | 0.70 dB | 3.84 vs 8.93 | 9/9 |
| anything, reverb | 3/3 | 0/3 | 8.5 dB | 1.85 dB | 3.70 vs 7.16 | 3/3 |
| same part, reverb | 2/3 | 0/3 | 7.0 dB | 2.19 dB | 5.66 vs 9.98 | 3/3 |
| anything, whole mix | 2/3 | 0/3 | 17.0 dB | 7.33 dB | 10.57 vs 7.16 | 0/3 |
| same part, whole mix | 1/3 | 0/3 | 9.0 dB | 8.47 dB | 15.65 vs 9.98 | 0/3 |
| anything, separated | 3/3 | 0/3 | 10.0 dB | 2.35 dB | 4.59 vs 7.16 | 3/3 |
| same part, separated | 2/3 | 1/3 | 14.5 dB | 5.40 dB | 8.54 vs 9.98 | 2/3 |

What it says:

- On a clean or reverberant guitar, both modes find the right amp in nearly every case and land well under the defaults; same part fits the spectrum much more closely (0.70 dB against 1.85 dB) because the notes are the same.
- The Gain isn't recovered (3 to 10 dB off). Monolith saturates at any Gain and Glass stays clean over a wide range, so neighbouring Gains measure alike. The one dry miss (an Ember at +14 matched as a Monolith at -22.5) is the same thing: a cranked crunch against a barely driven high-gain amp.
- The exact cab is rarely found; one of the same family is, and the tone knobs and EQ absorb the rest.
- On a whole mix it fails: the knobs go to their limits fitting the drums and bass. Separation fixes Anything (2.35 dB, every amp right). Same part on a separated stem is worse: the stem's bleed and artefacts break the moment-by-moment comparison. On a separated target, use Anything.
- In anything mode the match EQ also fits part of the difference between the two performances (a dip near the reference's lowest notes); the confidence weighting and the ridge keep it from doing more.

Gate (the spec's): the synthetic results had to tell the three amps apart and beat leaving the settings at default. They did (17 of 18 dry cases right, every dry and reverb case under the best default), so Stages B and C were built.

Timing: the C++ matcher takes about 5 s for a 9 s target with a 9 s DI on this Mac (14 threads, 29 renders, 116 candidates). The renders dominate, so it grows with the DI's length. (The prototype's own times are with its render cache warm.)

Reproduce: `uv run --python 3.11 --with demucs --with numpy --with scipy python prototypes/tone_match.py study --separation` (and `study --tuning --quick` for the tuning set).

## The C++ port and its tests

`src/tonematch/` is a line-by-line port of the prototype, golden-tested against it on the same files (`tests/fixtures/tone_match`, written by the prototype's `golden` command, all synthetic): the bands, the weights, and Nelder-Mead on Rosenbrock's function are identical; the fixtures' long-term spectra and features agree within 0.00001 dB; DTW finds the same path (same length and checksum); a candidate's features, residual, tone fits, and match EQ curve agree within 0.001 dB; and the whole search finds the prototype's amp, Gain, and cab with the same score.

The comparison's tests are `tests/ToneMatchCompareTests.cpp` (snapshots `build/proof/tone_match/10_compare_rendering.png` to `14_compare_same_part.png`) and the real-time safety test. Play along's are `tests/ToneMatchPlayAlongTests.cpp` (the count-in, play once, the fake device's take, the latency compensation, the stop, the live path, the tempo estimate, Guitar only, and the page: snapshots `20_play_target.png`, `21_count_in.png`, `22_recording.png`, `23_take_done.png`, `24_play_guitar_only.png`), the band in `ToneMatchTests.cpp`, and the real-time safety test (the song and its guitar with a count-in, looped, switched, restarted, stopped, and a whole take through the session: 0 allocations, 0 frees, 0 blocking locks).

Other tests (`tests/ToneMatchTests.cpp`, `ToneMatchAppTests.cpp`, `ToneMatchSeparationTests.cpp`): the search on synthetic cases with known settings in C++ (every match better than the defaults); cancel; decoding WAV, AIFF, FLAC, M4A, and MP3; the DI recorder (sample-exact, and inside the real-time safety test, which stays at 0 allocations, 0 frees, 0 blocking locks); Apply as one undo step, with redo and Discard; the page, with snapshots in `build/proof/tone_match/`; the separation installer's size and SHA-256 checks and cancel; the C++ separation against Python's Demucs on the same 10 s clip (20.8 dB SDR; Python against its own reruns, which draw a different random shift, 19.0 to 31.3 dB); a silent stretch (finite, silent stem); simulated out of memory (retried with one worker, then reported); the worker rule; the log's cap; Apply switching the pre effects off and undo restoring each switch; and the result lines breaking only between items.

Two groups need a flag, so the default suite stays offline. `AMPSIM_HTTP_TESTS=1` runs the installer against `tests/model_server.py` (through uv), which redirects like Hugging Face, to another host name with a long percent-escaped query it refuses to accept changed, and misbehaves on purpose: a connection dropped at 20 MB, two 503s, a 25 s wait for the first byte, a 404, a flipped byte. Then the page's whole flow from an empty model folder on `tests/fixtures/tone_match/song_44k.mp3` (20 s of the synthetic song, 44.1 kHz stereo, LAME through lameenc), from 7.3 s. `AMPSIM_NETWORK_TESTS=1` runs the page's whole flow against the real URL (any song with `AMPSIM_SONG`, `AMPSIM_SONG_DI`, `AMPSIM_SONG_START`, `AMPSIM_SONG_SECONDS`).

## Not verified (only Sean can)

- How a match sounds, on anything. Every number here is a measurement on synthetic guitars.
- How the comparison sounds and feels: whether the switches and the loop are click-free to his ears, whether equal loudness by BS.1770 sounds equally loud on his material, and whether same part's alignment keeps a switch on the same note (PROGRESS TM.6).
- Real songs: Demucs on real mixes, real lead tones, real DIs (PROGRESS TM.1 to TM.3).

## Sources

- Fujishima, "Realtime chord recognition of musical sound: a system using Common Lisp Music", ICMC 1999 (chroma). Mueller, *Fundamentals of Music Processing*, Springer 2015, ch. 3 (chroma, DTW).
- Sakoe and Chiba, "Dynamic programming algorithm optimization for spoken word recognition", IEEE TASSP 26(1), 1978.
- Nelder and Mead, "A simplex method for function minimization", The Computer Journal 7(4), 1965.
- IEC 61672-1 (the A-weighting curve).
- Simper, "Linear Trapezoidal Integrated State Variable Filter With Low Noise Optimisation", Cytomic 2013; Zavalishin, *The Art of VA Filter Design*, 2012.
- Ellis, "Beat tracking by dynamic programming", Journal of New Music Research 36(1), 2007 (section 3.1, the tempo estimate).
- Karplus and Strong, "Digital synthesis of plucked-string and drum timbres", Computer Music Journal 7(2), 1983; Jaffe and Smith, "Extensions of the Karplus-Strong plucked-string algorithm", Computer Music Journal 7(2), 1983.
- Rouard, Massa, and Defossez, "Hybrid Transformers for Music Source Separation", ICASSP 2023.

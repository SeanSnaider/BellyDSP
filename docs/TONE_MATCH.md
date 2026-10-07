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
   - **Same part**: you played the same part as the target (learn the lick, play it). The two are lined up in time and compared moment by moment. The more precise mode, on a dry or isolated target. With **Clean up with my take** (on by default) the target is first cleaned up with your notes ("Cleaning up the target with your take" below).
   - **Anything**: you played anything. Play the same kind of part (a lead for a lead, in a similar register), because the long-term spectrum depends on which notes are played. Use this one on a separated stem.
4. **Match.** It takes a few seconds (plus about a minute per minute of audio when separating). Cancel stops it.
5. **Result.** The amp, Gain, tone, cab, match EQ (bands low to high), the curve the EQ was fitted to, and the score. **Apply** sets them as normal settings in one undo step (Cmd-Z puts everything back, the cab and the pre effects included), so a preset saves them. **Discard** throws the result away.

6. **Compare.** Once the result is in, the Match card plays three things, looped: **Target** (what was matched), **Match** (your DI through the result) and **Current** (your DI through what's set now). See "Comparing" below.

What Apply changes (ASSUMPTIONS TM8, TM19): the amp slot, its Gain and tone, the cab (close mic 1, with mic 2 and the room muted, cuts off), the post EQ (on, parametric, the five bands, cuts off), and it switches off the pre effects that color the tone before the amp, the pre compressor, boost, overdrive, and pre EQ, so the match is heard as it was made. The noise gate stays as it is. The page says this above Apply, with which of them are on right now.

From the command line: `prototypes/tone_match.py match TARGET DI --mode same|anything` (the prototype, through `ampsim_render`), and `ampsim_separate song.wav guitar.wav` (the separation, with its timing and memory).

## Comparing (A/B)

Sean's feedback on the first version: "hard to compare". So the page plays the song's guitar and your matched tone side by side, looped, without leaving it (ASSUMPTIONS TM24 to TM31).

**The sources.**

- **Target**: what was matched, the separated guitar stem when "Separate the guitar first" was on, otherwise the selected section of the file; cleaned up with your take when that was used.
- **Raw** (only when the cleanup was used): the target before the cleanup, on the target's clock (the player's source 5).
- **Match**: the DI the match used, rendered through what Apply would set (the slot, its Gain and tone, the cab in close mic 1, the match EQ as the post EQ, the pre effects off). The values are the ones the parameters will hold, so the render equals what Apply produces bit for bit (tested: `renderTone` on the settings read back from the processor after Apply gives the identical 432,000 samples).
- **Current**: the same DI through what's set now. It re-renders by itself when the slot, its knobs, its capture, close mic 1's IR, the amp or cab bypass, or the post EQ change and then stay still for 300 ms. After Apply it equals Match (tested).

Both renders run on the session's worker through `ToneMatcher::renderTone`: the chain's own `AmpSection`, close mic 1's IR as the cab plays it (the cab's own loading and loudness match), FFT convolution, and the chain's own `Equalizer`. They take about 3.6 s for a 9 s DI on this Mac, while the page stays usable, and Cancel stops them (bounded by a capture's load, which can't be interrupted: 1.2 s measured with a gain set). Not in them: the pre effects (Apply switches the coloring ones off, and Current leaves them out too), close mic 2, the room, the other post effects, the cab mic's level and pan, and the output level.

**Playing.** A preview player sits at the very end of the chain, after the output level and before the output limiter (so nothing it plays can clip the output). It's a chain block that's idle unless comparing, and idle it returns without touching the audio, so the live path is bit-identical to before (tested over 3 s of the riff, before a preview and from 50 ms after it). It adds no latency. The audio thread only reads buffers that were built and filled elsewhere and handed over with a `Handoff` (old ones handed back to be freed off the audio thread), and atomics for play, the source, the loops, and the levels. It's in the real-time safety test, which stays at 0 allocations, 0 frees, 0 blocking locks: material handed over, played for 408 buffers through its loop's wraps, switched three times, its loops moved, new material swapped in mid-play, stopped.

- **Play/Stop** (Space), **Target / Match / Current** (1, 2, 3), and **Raw** (4) after a cleaned-up match, from anywhere on the page. Leaving the page stops it.
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

## Cleaning up the target with your take

Sean, 2026-10-06: "build the cleanup", the informed mask from the learning prototype (below, "Informed separation (task 3)"), in the app (ASSUMPTIONS TM50 to TM56). When you played the target's part, your DI says which notes the lead plays and when. A single note through any amp, cab, and mic has its energy at the harmonics of its fundamental, h f0, so on the target's spectrogram the cleanup keeps what lies near those and turns everything else down 20 dB: the drums, the bass, another guitar between the lead's harmonics, Demucs's bleed. What sits right on top of a harmonic stays.

**Where it applies.** Only in **Same part**: a play-along take of the section (lined up by the round trip, so the notes are searched within the 0.5 s band) or any other same-part DI (unbanded DTW). In Anything mode your notes aren't the target's, so there's nothing to line up: the switch is disabled and its caption says so. It runs on the match's worker between the separation (or the section, with separation off) and the analysis, so everything the matcher measures comes from the cleaned target.

**The option.** **Clean up with my take**, in the Target card under the separation switch, on by default (whenever it applies). After a match the Match card says "Target is cleaned up with your take (35 notes); Raw is before." and the closeness line ends "target cleaned up". If your DI has no notes with a clear pitch, the match runs on the target as it was and the line says why. In the A/B, **Target** plays what was matched (the cleaned target), and a fourth source, **Raw** (key 4), plays the target before the cleanup, on the same clock, so a switch between the two keeps the exact sample. Guitar only, the play-along's stem, and Save take's `stem.wav` stay the separated stem as Demucs left it.

### How it works

1. **Your notes.** Onset strength in the DI: the spectral flux of a 1024-point (symmetric Hann) STFT every 120 samples (2.5 ms), each bin's rise in log(|X| + 1e-4 max |X|) since the frame before, positive parts summed from 100 Hz to 10 kHz (Bello et al. 2005). Peaks over a moving threshold (the median over +-100 ms plus a tenth of the flux's 99th percentile), at least 45 ms apart, each refined to the sample where a 1 ms RMS envelope rises fastest (e[n] - e[n - 48], within +-10 ms).
2. **Their pitches.** McLeod's method (`PitchDetector`, the harmonizer's settings: the DI low-passed at 3 kHz and decimated to 12 kHz, 110 Hz to 1.4 kHz) every 10 ms from 20 ms after each onset to the next one, the median of the readings with clarity 0.9 or more.
3. **Their places in the target.** Chroma DTW between the target and tanh(10 x) of the DI (tone match's own `align`; the plain distortion makes the DI's partials look like a distorted guitar's) maps each onset to the target's clock; then the note's own onset is found within +-60 ms: the flux over the bins within 3% (plus 0.6 bins) of its first 12 harmonics, times a 30 ms Gaussian around the DTW estimate, minus the flux's own lag measured on the DI (-336 samples on the fixture: a frame's time is its centre, so the flux peaks early). A note spans from its onset to the next one in the target.
4. **The pitch as the record plays it.** The DI's f0 times 2^(c/1200) for c = -60 to +60 cents in 5-cent steps, the c whose first 8 harmonics hold the most magnitude in the note's 16384-point spectrum: the record may be tuned differently from your guitar.
5. **The mask**, on a 4096-point STFT (11.7 Hz bins, so the harmonics of a low E separate) with hop 512:

   ```
   M(k, f) = max( 0.1, max over the notes n sounding in frame k, and h = 1 .. 40 with h f0_n <= 0.45 fs, of
                  exp(-1/2 ((f - h f0_n) / sigma_h)^2) ),      sigma_h = max(h f0_n (2^(35/1200) - 1), 1.5 bins)
   ```

   The 35 cents are the prototype's (ASSUMPTIONS TM47); the 1.5-bin minimum keeps a low partial's lobe from being narrower than the Hann window's own main lobe can resolve. A note sounds in frame k when the frame's 4096-sample window overlaps its span. The floor of 0.1 (-20 dB) is there because a distorted guitar has real energy between and below its harmonics: with no floor, the mask applied to the clean record alone moved its long-term spectrum by 11 to 23 dB in the prototype's study.
6. **Smooth by construction, and a little more.** Musical noise, the warbling tones of spectral subtraction and Wiener masks, comes from per-bin, per-frame gain decisions that jump at random. This mask makes none: in frequency it's a sum of smooth Gaussian lobes, and in time it's constant over each note, changing only at note boundaries, where the union over the window's overlap and the 85 ms window itself cross-fade it. On top of that, each bin is averaged over 5 frames (53 ms) and floored again: measured in the prototype (`learn_tone.py mask-study`), +0.02 to +0.08 dB SI-SDR on all four study cases and an unchanged spectral distance, so it's in.
7. **The inverse STFT.** Each frame (periodic Hann w) times its mask, inverse FFT, windowed by w again and overlap-added, divided by the sum of the squared windows (the least-squares inverse STFT, Griffin and Lim 1984; scipy's `istft`). At hop N/8 the squared periodic Hann windows add to a constant (3, the constant-overlap-add condition for w^2, which holds for any hop of N/4 or less), so an unmasked frame comes back exactly; dividing by the actual sum keeps the first and last samples exact too. Measured: an all-ones mask returns the input at 337 dB SNR (double precision), the floor alone 0.1 x the input at 317 dB.

`src/tonematch/InformedMask.*` is the port of `prototypes/learn_tone.py` (`align_notes`, `informed_mask`), written to make the same floating-point decisions (scipy's frame times, numpy's symmetric Hann, medfilt's zero padding, find_peaks' plateau and distance rules). It runs on the double-precision FFT (`FftDouble`).

### Measured

Golden test against the prototype (`tests/InformedMaskTests.cpp` on `tests/fixtures/informed_mask`, written by `uv run prototypes/learn_tone.py informed-golden`: 6 s of the crunch rig's record in the band with drums, bass, and a rhythm guitar, no separation, and the take of the same notes, 1% slower, 15 ms jitter, other velocities, a darker pick):

| What | Tolerance | Result |
|---|---|---|
| The take's flux peaks | identical | 37 of 37 identical |
| Onsets (refined to the sample) | within 1 sample | 37 of 37 sample-exact (36 within 10 ms of the 36 true ones; one extra) |
| The flux lag | identical | -336 samples |
| Each note's pitch from the DI | 0.5 cent | 0.0000 cents worst |
| DTW path (0.5 s band) | same length and checksum | 147 steps, same checksum |
| Target onsets, notes | within 1 sample | all sample-exact, 35 notes |
| Pitches refined in the target | 0.5 cent | 0.0000 cents worst |
| The mask: every frame's sum over the 2049 bins | 1e-5 relative | 1e-8 (raw and smoothed) |
| The mask: 11 whole frames, bin by bin | 1e-5 | 1e-7 |
| The output against the prototype's | 60 dB SNR | 83.2 dB (the prototype's file is 16-bit) |

Against the unmixed record, on that fixture: SI-SDR -0.73 dB for the mix, 8.47 dB cleaned (the prototype's 8.47), the spectral distance 7.48 dB to 2.98. Time: 0.11 to 0.14 s for 6 s, 1.14 s for a minute (364 notes) on this Mac; Cancel returned 0.7 ms after the flag in the standalone test (a minute, cancelled 100 ms in) and 13 to 15 ms after it through the session; the only stretch that can't be interrupted is the target's analysis for DTW (tone match's `Analysis::of`).

**End to end** (the test's last group): the matcher as the app runs it (Same part, the 0.5 s band, the built-in gain sets, the 21 cabs), its result rendered on the take and compared with the hidden rig on the take (the prototype's spectral distance; lower is closer):

| Target | Cleanup off | Cleanup on | For information: Anything on the cleaned target |
|---|---|---|---|
| The record in the band, no separation (the fixture) | 8.18 dB (Monolith +22.5, Modern 4x12 blend) | **3.57 dB** (Glass +22.5, Modern 4x12 supercardioid) | 3.15 dB |
| The study's high-gain case separated by Demucs (its first 6 s) | 5.89 dB (Monolith +22.5) | **3.62 dB** (Ember +16.5) | 1.97 dB |

The cleanup lowered the distance in both cases (the test requires it). The prototype's whole-study numbers (four rigs, 24 s each): SI-SDR from 1.4 to 3.0 dB (Demucs alone) to 4.8 to 8.8 dB, the spectral distance to the record from 4.7 to 5.5 dB to 1.4 to 2.2, and the matcher's Same part mode from 6.7 to 9.5 dB to 3.1 to 5.0. Unbanded DTW (a same-part DI that isn't a take) found the same notes on three of the four and cost 0.6 dB SI-SDR on the one with a delay and a room (4.16 against 4.79).

One thing the numbers say that the brief didn't ask: on a cleaned target, the matcher's **Anything** mode lands closer than Same part (3.15 against 3.57, 1.97 against 3.62 here; 1.9 to 2.1 against 3.1 to 5.0 in the study). A likely reason (not tested): Same part's frame-by-frame comparison still trips on what's left of the bleed, and the long-term statistics average it out. The cleanup needs the notes to line up; the matcher doesn't. Matching a take in Anything mode with the cleanup still on (through the take's alignment) would be a small change in the session if Sean wants it (ASSUMPTIONS TM55); as built, Anything means "my notes aren't the target's", and the cleanup stays off there.

**Not verified (only Sean can).** How a cleaned target sounds (Raw against Target in the A/B), and whether matching it sounds closer on a real song (PROGRESS TM.9). Real leads bend, slide, and add vibrato, which move the harmonics within a note; the mask follows one pitch per note (35 cents wide), so a wide bend is turned down where it leaves the lobe. A record tuned more than 60 cents off your guitar isn't caught. Ghost notes and fast runs the onset detector misses aren't kept.

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

The cleanup's are `tests/InformedMaskTests.cpp` (golden against the prototype, the STFT round trip, cancel, the end-to-end effect on the matcher) and `tests/ToneMatchCleanupTests.cpp` (the page: on, off, and disabled in Anything; Target is the cleanup's output sample for sample and Raw the section; level match over the four; cancel during the cleanup; snapshots `26_cleanup_on.png`, `27_cleanup_off.png`, `28_cleanup_disabled.png`), and the real-time safety test plays the Raw source too.

Other tests (`tests/ToneMatchTests.cpp`, `ToneMatchAppTests.cpp`, `ToneMatchSeparationTests.cpp`): the search on synthetic cases with known settings in C++ (every match better than the defaults); cancel; decoding WAV, AIFF, FLAC, M4A, and MP3; the DI recorder (sample-exact, and inside the real-time safety test, which stays at 0 allocations, 0 frees, 0 blocking locks); Apply as one undo step, with redo and Discard; the page, with snapshots in `build/proof/tone_match/`; the separation installer's size and SHA-256 checks and cancel; the C++ separation against Python's Demucs on the same 10 s clip (20.8 dB SDR; Python against its own reruns, which draw a different random shift, 19.0 to 31.3 dB); a silent stretch (finite, silent stem); simulated out of memory (retried with one worker, then reported); the worker rule; the log's cap; Apply switching the pre effects off and undo restoring each switch; and the result lines breaking only between items.

Two groups need a flag, so the default suite stays offline. `AMPSIM_HTTP_TESTS=1` runs the installer against `tests/model_server.py` (through uv), which redirects like Hugging Face, to another host name with a long percent-escaped query it refuses to accept changed, and misbehaves on purpose: a connection dropped at 20 MB, two 503s, a 25 s wait for the first byte, a 404, a flipped byte. Then the page's whole flow from an empty model folder on `tests/fixtures/tone_match/song_44k.mp3` (20 s of the synthetic song, 44.1 kHz stereo, LAME through lameenc), from 7.3 s. `AMPSIM_NETWORK_TESTS=1` runs the page's whole flow against the real URL (any song with `AMPSIM_SONG`, `AMPSIM_SONG_DI`, `AMPSIM_SONG_START`, `AMPSIM_SONG_SECONDS`).

## Learning a capture from the song (prototype)

Sean, 2026-10-06: tone match "doesn't feel right but also just feels off". The matcher above picks among three gain sets, a Gain, five tone bands, 21 cabs, and a match EQ, judged on long-term spectra and four distortion statistics. That fits brightness. It can't fit feel: how loudness, compression, and harmonics change with picking strength, the attack, the bloom. The idea prototyped here: train a small NAM model that turns Sean's play-along take into the record's guitar, so the dynamics come from the record. It's `prototypes/learn_tone.py` (ASSUMPTIONS TM41 to TM49). **It didn't pass its gate** (below), so there's no Learn tone button. What did come out of it: Save take, which collects real takes for a benchmark, and the informed mask, which helped every method on a mixed record.

### The design

The obstacle: the two performances are never sample-identical. Notes land 10 to 40 ms apart, at other velocities, with other picks, so NAM's ESR (sample by sample) compares unrelated waveforms. Three pieces:

1. **Per-note alignment.** Onsets are found in the take's clean DI (spectral flux, Bello et al. 2005, refined to the sample at the steepest rise of a 1 ms envelope: 0.02 to 0.04 ms median error, precision and recall 0.99 to 1.00 on every case). Each onset goes into the target's timeline through the chroma DTW path the matcher already computes (within the 0.5 s play-along band; unbanded, DTW on a mix put 10% of the notes over 0.7 s off), then moves to the strongest onset of that note in the target: the flux over the bins near the note's first 12 harmonics (its pitch from the DI, McLeod's method), times a 30 ms Gaussian prior, within +-60 ms, corrected by the flux's own lag measured on the DI. On a clean target the found onsets are off the truth by a constant (the cab IR's first arrival, which the model learns) plus 0.7 ms median and 1.3 to 2.7 ms at the 90th percentile; on a separated mix, 0.8 to 1.7 ms median but 43 to 116 ms at the 90th (a tenth of the notes find the wrong onset).
2. **A per-note loss** (the brief's option b). The model runs on the take as played (no warping: a 40 ms jitter on a 250 ms note would need a 16% stretch, which shifts the pitch or splices in the next attack). Each note's output, from 5 ms before its onset to the next onset in either performance, is compared with the same stretch of the target from that note's onset there, by NAM's bundled auraloss MRSTFT (Yamamoto et al. 2020; Steinmetz and Reiss 2020): spectral convergence plus log-magnitude distance at FFT sizes 256 to 4096, magnitudes only. No ESR term: between two performances it rewards outputting less.
3. **The model.** NAM's feather WaveNet (0.12.3's own table: 3,025 weights, receptive field 4,093 samples), built from `nam.models.wavenet`, trained by a small loop of our own (NAM 0.12.3's `LossConfig` has an `mrstft_weight`, but `core.train` only ever adds a 2e-4 pre-emphasized MRSTFT to its MSE, and it's built around NAM's test file: its latency blips and validation split), Adam 4e-3 decaying to 4e-4, 3000 steps of 16 notes, 15% of the notes held out to keep the best step. Exported with NAM's own exporter as an `amp_cab` capture: NAM core (through `ampsim_render`) and PyTorch agree on the held-out DI to an ESR of 1e-13. **Training time: 217 s for 3000 steps on this Mac's GPU (MPS)**, for a 25 s take of 86 notes.

Also tried, in the study: a "latent velocity" variant (one learnable input gain per note, penalized, so the model needn't learn the player's softer or harder picking as part of the rig; the regression-dilution argument is in the prototype's docstring), the take with no per-note alignment, a floored log-magnitude MRSTFT, a warm start from the matcher's sound (distilled by ESR on the take) with and without an anchor to it, and lite and standard WaveNets.

### How it was judged (synthetic, known rigs)

- **Four hidden rigs**, none of them a built-in amp: *high_gain* (the app's Screamer model at +3 dB into a three-stage gray-box channel on the clean channel's tone-stack voice (the built-in high gain uses the other), a 75 W cab, a mid scoop), *crunch* (two asymmetric stages, the power amp pushed, a bus compressor after the cab, 3:1 with a 10 ms attack), *edge_comp* (a pedal compressor into an edge-of-breakup channel), *high_gain_fx* (another high gain, with a 360 ms delay and a room on the record).
- **Three performances of one 24 s lead score** (86 notes, each with a picking strength from the music's own swell and accents plus noise, which sets the level and the pick's brightness): A, the record's guitarist; B, Sean's take of the same notes, 1% slower, with 10 to 40 ms jitter, his own velocities (0.8 of the swell, 2.5 dB noise), and a darker pick; C, held out, other melodies.
- **Targets:** the record unmixed (*stem*); the record in a band (drums, bass, and a palm-muted rhythm guitar 3 dB under the lead) separated by Demucs (*separated*); and that stem through the informed mask (*masked*).
- **Methods:** the current matcher (`tone_match.py`, the C++ matcher's golden reference, with the built-in gain sets as the app has them), in Same part (the app's default after a take) and Anything; the learned capture; the learned capture followed by a match EQ fitted the matcher's way; the oracle (the hidden rig).
- **Spectral distance:** the matcher's long-term spectral error on the held-out DI C, method against the hidden rig without its time effects.
- **The feel metric** (`feel_measures`, `feel_distance`): one pluck of E4 and an A power chord at input peaks of -30 to -6 dBFS in 3 dB steps (the takes' range), through the method and the rig. Four facets, in dB, each averaged over the two probes, and their mean, the feel score: *compression*, the RMS difference of the output-level curve (first 400 ms) with its mean removed; *harmonics*, the RMS difference of harmonics 2 to 8 relative to the fundamental (100 to 500 ms) over all levels; *attack*, the RMS difference of the first 60 ms envelope (2 ms RMS) relative to its peak; *decay*, the same from 60 ms to 1.2 s (20 ms RMS) relative to its start. For a sense of scale: the hidden high-gain rig itself with its drive 3 dB off scores 0.56 feel and 0.55 dB spectral; 6 dB off, 0.96 and 1.07; another cab, 2.07 and 2.72.
- **The gate:** feel at least 20% lower than the matcher's AND spectral distance no more than 0.25 dB higher, against the matcher's better mode on that target, in most cases.

No embedding distance (it was optional): I didn't find and check a small pretrained audio embedding whose weights' licence is clear, and the four facets answer the question asked.

### Results

Spectral distance and feel facets on the held-out DI against the hidden rig (the oracle is 0 on all of them). The last column is the note probe's compression slope (output dB per input dB) with the rig's own in brackets. From `uv run prototypes/learn_tone.py study --json out.json` (about two hours, renders cached), then `report out.json`.

| Case | Target | Method | Spectral (dB) | Compression | Harmonics | Attack | Decay | Feel | Slope (rig's) |
|---|---|---|---|---|---|---|---|---|---|
| high_gain | stem | matcher, same part (Monolith -22.5, Modern 4x12, dynamic, 75 W, var. 2) | 0.58 | 0.20 | 4.47 | 0.46 | 0.31 | **1.36** | 0.35 (0.33) |
| high_gain | stem | learned | 1.51 | 0.88 | 5.01 | 1.88 | 1.80 | **2.39** | 0.41 (0.33) |
| high_gain | stem | learned + match EQ | 1.46 | 0.91 | 4.89 | 1.86 | 1.77 | **2.36** | 0.41 (0.33) |
| high_gain | stem | learned, latent | 1.43 | 1.09 | 11.41 | 1.87 | 1.67 | **4.01** | 0.45 (0.33) |
| high_gain | stem | learned, no alignment | 2.72 | 0.69 | 10.92 | 1.37 | 1.35 | **3.58** | 0.20 (0.33) |
| high_gain | stem | upper bound: same performance, ESR | 2.55 | 0.25 | 2.63 | 0.94 | 0.43 | **1.06** | 0.31 (0.33) |
| high_gain | separated | matcher, same part (Monolith +22.5, Modern 4x12, dynamic, dark 60 W) | 6.73 | 2.15 | 17.27 | 3.57 | 6.67 | **7.41** | 0.05 (0.33) |
| high_gain | separated | matcher, anything (Ember +3.0, Modern 4x12, supercardioid, dark 60 W) | 4.50 | 3.85 | 18.03 | 2.43 | 3.38 | **6.92** | 0.85 (0.33) |
| high_gain | separated | learned | 2.96 | 1.02 | 9.89 | 1.80 | 1.32 | **3.51** | 0.21 (0.33) |
| high_gain | separated | learned + match EQ | 4.94 | 1.29 | 11.75 | 2.19 | 2.09 | **4.33** | 0.24 (0.33) |
| high_gain | masked | matcher, same part (Monolith +22.5, Modern 4x12, dynamic, dark 60 W) | 3.86 | 2.25 | 12.87 | 2.25 | 5.92 | **5.82** | 0.04 (0.33) |
| high_gain | masked | matcher, anything (Ember +15.0, Modern 4x12, blend, 75 W + bright 60 W, 2) | 2.08 | 2.61 | 8.23 | 1.13 | 3.45 | **3.85** | 0.69 (0.33) |
| high_gain | masked | learned | 1.82 | 0.76 | 10.02 | 1.73 | 1.10 | **3.40** | 0.23 (0.33) |
| high_gain | masked | learned + match EQ | 2.04 | 0.69 | 8.20 | 1.70 | 1.12 | **2.93** | 0.24 (0.33) |
| crunch | stem | matcher, same part (Ember -9.0, Vintage 4x12, supercardioid, lower) | 1.81 | 3.49 | 0.90 | 3.57 | 3.26 | **2.80** | 0.89 (0.44) |
| crunch | stem | learned | 4.14 | 2.36 | 8.96 | 3.55 | 2.64 | **4.38** | 0.75 (0.44) |
| crunch | stem | learned + match EQ | 2.14 | 2.34 | 8.44 | 3.58 | 2.59 | **4.24** | 0.75 (0.44) |
| crunch | stem | learned, latent | 3.03 | 3.15 | 9.38 | 3.71 | 3.48 | **4.93** | 0.91 (0.44) |
| crunch | stem | learned, no alignment | 7.20 | 1.23 | 8.03 | 4.44 | 1.59 | **3.82** | 0.60 (0.44) |
| crunch | stem | upper bound: same performance, ESR | 4.61 | 0.61 | 4.45 | 2.50 | 1.59 | **2.29** | 0.50 (0.44) |
| crunch | separated | matcher, same part (Monolith +22.5, Modern 4x12, dynamic, dark 60 W) | 9.46 | 2.88 | 15.17 | 4.81 | 7.53 | **7.60** | 0.06 (0.44) |
| crunch | separated | matcher, anything (Ember -16.5, Modern 4x12, blend, dark + bright 60 W, 1) | 4.54 | 3.69 | 14.58 | 3.14 | 3.86 | **6.32** | 0.91 (0.44) |
| crunch | separated | learned | 2.61 | 0.72 | 13.39 | 3.70 | 3.91 | **5.43** | 0.39 (0.44) |
| crunch | separated | learned + match EQ | 4.34 | 0.94 | 11.84 | 3.70 | 4.29 | **5.19** | 0.38 (0.44) |
| crunch | masked | matcher, same part (Ember +3.0, Modern 4x12, blend, dark + bright 60 W, 1) | 3.10 | 2.85 | 15.28 | 3.42 | 3.95 | **6.38** | 0.82 (0.44) |
| crunch | masked | matcher, anything (Ember -13.5, Modern 4x12, blend, dark + bright 60 W, 1) | 2.04 | 3.53 | 7.00 | 3.09 | 6.28 | **4.98** | 0.88 (0.44) |
| crunch | masked | learned | 2.90 | 0.94 | 10.02 | 3.58 | 3.20 | **4.43** | 0.52 (0.44) |
| crunch | masked | learned + match EQ | 2.45 | 1.13 | 9.98 | 3.40 | 3.26 | **4.44** | 0.55 (0.44) |
| edge_comp | stem | matcher, same part (Ember +21.0, Vintage 4x12, dynamic, upper, var. 2) | 1.32 | 1.22 | 1.60 | 0.76 | 0.92 | **1.12** | 0.56 (0.42) |
| edge_comp | stem | learned | 1.88 | 0.86 | 10.70 | 1.17 | 1.51 | **3.56** | 0.44 (0.42) |
| edge_comp | stem | learned + match EQ | 1.69 | 0.92 | 10.21 | 1.14 | 1.36 | **3.41** | 0.44 (0.42) |
| edge_comp | stem | learned, latent | 1.28 | 1.13 | 7.52 | 0.93 | 1.21 | **2.70** | 0.47 (0.42) |
| edge_comp | stem | learned, no alignment | 5.15 | 0.69 | 13.11 | 1.18 | 2.59 | **4.39** | 0.37 (0.42) |
| edge_comp | stem | upper bound: same performance, ESR | 3.04 | 0.52 | 1.37 | 0.48 | 0.73 | **0.78** | 0.46 (0.42) |
| edge_comp | separated | matcher, same part (Monolith +22.5, Modern 4x12, dynamic, dark 60 W) | 9.10 | 2.69 | 16.78 | 4.12 | 9.66 | **8.31** | 0.05 (0.42) |
| edge_comp | separated | matcher, anything (Ember +1.5, Modern 4x12, supercardioid, dark 60 W) | 5.53 | 3.38 | 20.31 | 2.76 | 2.72 | **7.29** | 0.86 (0.42) |
| edge_comp | separated | learned | 2.26 | 0.66 | 9.81 | 1.62 | 4.41 | **4.12** | 0.36 (0.42) |
| edge_comp | separated | learned + match EQ | 4.89 | 0.97 | 12.98 | 2.41 | 5.50 | **5.46** | 0.34 (0.42) |
| edge_comp | masked | matcher, same part (Monolith +18.0, Modern 4x12, dynamic, dark 60 W) | 4.98 | 2.35 | 12.43 | 2.82 | 7.92 | **6.38** | 0.10 (0.42) |
| edge_comp | masked | matcher, anything (Ember +6.0, Modern 4x12, blend, dark + bright 60 W, 2) | 1.95 | 2.85 | 9.39 | 1.28 | 3.22 | **4.19** | 0.77 (0.42) |
| edge_comp | masked | learned | 1.38 | 0.64 | 7.23 | 1.89 | 2.26 | **3.01** | 0.48 (0.42) |
| edge_comp | masked | learned + match EQ | 1.79 | 0.65 | 7.17 | 1.87 | 2.19 | **2.97** | 0.48 (0.42) |
| high_gain_fx | stem | matcher, same part (Ember -7.5, Modern 4x12, vocal mic, bright 60 W) | 1.27 | 0.89 | 3.97 | 0.80 | 0.98 | **1.66** | 0.86 (0.74) |
| high_gain_fx | stem | learned | 2.43 | 0.75 | 9.42 | 1.46 | 3.29 | **3.73** | 0.63 (0.74) |
| high_gain_fx | stem | learned + match EQ | 1.33 | 0.60 | 7.96 | 1.36 | 2.19 | **3.03** | 0.65 (0.74) |
| high_gain_fx | stem | learned, latent | 1.56 | 0.63 | 8.38 | 1.68 | 2.75 | **3.36** | 0.64 (0.74) |
| high_gain_fx | stem | learned, no alignment | 3.27 | 3.85 | 10.23 | 2.48 | 8.87 | **6.36** | 0.30 (0.74) |
| high_gain_fx | stem | upper bound: same performance, ESR | 2.40 | 0.21 | 4.68 | 0.48 | 0.29 | **1.41** | 0.73 (0.74) |
| high_gain_fx | separated | matcher, same part (Monolith +22.5, Modern 4x12, dynamic, dark 60 W) | 8.79 | 5.40 | 17.63 | 3.42 | 12.86 | **9.83** | 0.05 (0.74) |
| high_gain_fx | separated | matcher, anything (Ember -15.0, Modern 4x12, blend, dark + bright 60 W, 1) | 4.28 | 1.21 | 20.52 | 1.92 | 3.85 | **6.87** | 0.91 (0.74) |
| high_gain_fx | separated | learned | 3.05 | 3.58 | 9.27 | 2.52 | 7.90 | **5.82** | 0.25 (0.74) |
| high_gain_fx | separated | learned + match EQ | 4.47 | 3.68 | 16.38 | 3.06 | 8.66 | **7.94** | 0.22 (0.74) |
| high_gain_fx | masked | matcher, same part (Ember -12.0, Modern 4x12, blend, dark + bright 60 W, 1) | 3.29 | 1.06 | 19.27 | 1.27 | 2.67 | **6.06** | 0.89 (0.74) |
| high_gain_fx | masked | matcher, anything (Ember -12.0, Modern 4x12, blend, dark + bright 60 W, 1) | 1.86 | 1.03 | 11.10 | 0.96 | 1.52 | **3.65** | 0.88 (0.74) |
| high_gain_fx | masked | learned | 2.10 | 1.88 | 9.99 | 1.92 | 4.66 | **4.61** | 0.48 (0.74) |
| high_gain_fx | masked | learned + match EQ | 2.21 | 1.70 | 9.43 | 2.00 | 4.03 | **4.29** | 0.50 (0.74) |

Medians over the four rigs (the learned capture against the matcher's better mode): on the unmixed record, feel 3.64 against 1.51 and spectral 2.15 against 1.30 dB (the matcher wins); on the separated mix, feel 4.78 against 6.90 and spectral 2.78 against 4.52 dB (the learned capture wins); masked, 3.92 against 4.02 and 1.96 against 2.00 dB (even). By facet, the learned capture had the lower compression error in 9 of 12 cases and the lower harmonics error in 6, attack in 3, decay in 5.

What the take itself says (what's measurable on real data; the target's per-note compression slope, its note levels against the take's DI, and the spectral distance to the target on the take):

| Case | Target | Per-note slope: target, oracle, matcher, learned | Per-note level error (dB): matcher, learned | Spectral vs target on the take (dB): oracle, matcher, learned, learned + EQ |
|---|---|---|---|---|
| high_gain | stem | 0.13, 0.24, 0.29, 0.20 | 1.83, 1.27 | 0.98, 0.92, 0.95, 0.68 |
| high_gain | separated | 0.17, 0.24, 0.14, 0.09 | 2.17, 1.28 | 7.47, 5.50, 4.05, 1.69 |
| high_gain | masked | 0.20, 0.24, 0.10, 0.14 | 2.74, 2.00 | 2.51, 3.17, 2.04, 1.03 |
| crunch | stem | 0.20, 0.32, 0.77, 0.55 | 2.70, 2.12 | 0.96, 1.64, 3.03, 1.00 |
| crunch | separated | 0.27, 0.32, -0.01, 0.41 | 2.76, 1.60 | 5.42, 7.54, 4.46, 1.69 |
| crunch | masked | 0.32, 0.32, 0.66, 0.39 | 3.07, 2.25 | 1.73, 3.32, 1.55, 1.13 |
| edge_comp | stem | 0.11, 0.22, 0.34, 0.26 | 1.64, 1.51 | 1.26, 1.94, 1.33, 0.94 |
| edge_comp | separated | 0.15, 0.22, -0.05, 0.13 | 2.24, 1.51 | 7.38, 6.72, 4.67, 1.74 |
| edge_comp | masked | 0.18, 0.22, 0.04, 0.13 | 3.26, 2.13 | 2.59, 4.42, 1.88, 1.00 |
| high_gain_fx | stem | 0.45, 0.57, 0.73, 0.55 | 3.46, 3.05 | 0.88, 1.44, 2.22, 1.02 |
| high_gain_fx | separated | 0.31, 0.57, 0.03, 0.39 | 3.13, 1.59 | 8.85, 6.55, 4.84, 1.95 |
| high_gain_fx | masked | 0.37, 0.57, 0.74, 0.43 | 4.10, 2.67 | 3.49, 2.44, 1.90, 1.22 |

The target's per-note slope is flatter than the oracle's on the same take in every case (0.13 against 0.24 on high_gain): its note levels follow the original guitarist's picking, not Sean's. That's the regression dilution the latent-velocity variant was meant to undo.

### The gate: failed

**3 of 12 pass** (high_gain separated, edge_comp separated and masked), and 3 of 12 for the learned capture with a match EQ. Not "most cases", so per the brief: stop, and no in-app Learn tone. The honest summary:

- **On a clean or unmixed target the matcher is better**, on both measures (it fits the spectrum within 0.6 to 1.8 dB; the learned capture 1.5 to 4.1). The built-in gain sets are well-trained captures of amps related to the hidden ones, and the matcher's search can only produce real amps.
- **On a separated mix the learned capture is better** in all four rigs, on spectral distance and on feel (by 14 to 49%). The matcher's same-part mode breaks on Demucs's bleed (Monolith at +22.5 dB in all four, feel 7.4 to 9.8), and Anything is better but still over 6. The learned capture's per-note windows and harmonic onsets ignore most of what isn't the lead. With the mask, the matcher catches up.
- **The learned capture gets the compression right more often** (9 of 12) **and the harmonics wrong** (errors of 5 to 13 dB where the matcher, on a clean target, has 0.9 to 4.5).

Why, from the diagnostics (high_gain, run before the study):

1. **The loss doesn't single out the rig.** On the per-note MRSTFT, the hidden rig itself scores 0.910 against the target, the matcher 0.918, and the learned capture reached 0.855 to 0.89: lower than the truth. Perturbing the rig (drive +-6 dB, another cab, a 3 kHz boost, treble, master) raised it by only 1 to 18%. Between two performances, the best predictor of the target is not the rig: it's the rig plus whatever undoes Sean's playing (a darker pick, other velocities), and the extra harmonics are that.
2. **Even with the performance difference removed, MRSTFT alone trains poorly.** With the take the same performance as the record's except its timing (same plucks, same velocities, only jitter and tempo), the learned capture still had 11.8 dB of harmonics error and 2.74 dB spectral, and couldn't reach the oracle's loss (1.01 against 0.83, on the floored variant of the loss). A phase-blind loss from scratch is a hard optimization for a waveform model.
3. **Feather is small for these rigs.** The upper bound, the same performance sample-aligned and trained with ESR (6000 steps), reaches feel 0.78 to 2.29 (better than the matcher in all four) but 2.4 to 4.6 dB spectral: at a held-out ESR of 0.016 its error floor sits 20 to 30 dB under the signal, which is where the cab has rolled off (7 kHz up: +17 to +29 dB of fizz) and where the spectral distance's weights still count. On high_gain, lite reached 1.10 feel (ESR 0.011) and standard 0.74 (0.011), all at about 2.5 dB spectral.
4. Variants that didn't help: latent velocity (its gains correlated only 0.39 to 0.51 with the true velocity differences; feel worse in two cases, better in two), the floored log loss (feel 3.43 against 2.94), a warm start from the matcher (400 steps of distillation left ESR 0.99; 1500 got 0.048 but the fine-tuned results were worse than either), no per-note alignment (spectral worse in all four, 2.7 to 7.2 dB; feel worse in three).

What would be needed to try again:

- A loss whose minimum is the rig and not the rig plus the player: per-note statistics that don't depend on the pluck (band energies averaged over many notes at a similar level, the level curve fitted with the take's own velocities as the input, not per-note magnitudes), or a conditional model of the player's picking difference that's thrown away at export.
- A start that's already a good amp: fine-tune a real, well-trained capture (the matched gain set's own model, standard size) with a small learning rate and a spectral anchor, instead of training feather from nothing. That keeps the matcher's harmonics and lets the target move the compression, the one facet the learned capture got right.
- Longer and bigger: the upper bound needed 6000 steps and the standard size to beat the matcher on feel; 3000 feather steps is too little.
- Real takes to judge it on, which Save take now collects.

### Informed separation (task 3): a clear win

The take's pitches (McLeod's method on the DI, per note, then refined within +-60 cents against the target) make a harmonic mask on the target's STFT (4096 points): 35 cents (or the window's main lobe) around each harmonic of the notes sounding, everything else turned down 20 dB (a floor of 0.1; with no floor, the mask applied to the clean record alone moved its spectrum by 11 to 23 dB, because a distorted guitar has real energy between and below its harmonics). Against the unmixed record:

| Case | Onsets found (precision / recall, median error) | Target onsets, spread (median / 90th, ms): stem, separated, masked | Separation SI-SDR (dB): Demucs, + mask | Spectral distance to the record (dB): Demucs, + mask |
|---|---|---|---|---|
| high_gain | 1.00 / 1.00, 0.04 ms | 0.7 / 1.4, 1.7 / 64.0, 1.7 / 44.4 | 2.9, 8.8 | 4.73, 1.77 |
| crunch | 1.00 / 1.00, 0.02 ms | 0.7 / 1.3, 1.2 / 43.1, 1.1 / 33.2 | 3.0, 8.1 | 4.95, 2.20 |
| edge_comp | 1.00 / 1.00, 0.04 ms | 0.7 / 1.5, 0.8 / 91.0, 0.9 / 62.3 | 2.9, 7.8 | 5.47, 1.43 |
| high_gain_fx | 0.99 / 0.99, 0.04 ms | 0.9 / 2.7, 1.1 / 115.6, 1.0 / 72.1 | 1.4, 4.8 | 4.70, 1.89 |

(the 2nd column: precision and recall of the take's onsets; the 3rd: how far the target's onsets land from the truth, median and 90th percentile.) Separation quality rises by 3 to 6 dB SI-SDR and the spectral distance to the record falls from about 5 dB to about 2 in every case, and every method's feel was better on the masked stem than on the bare one (and its spectral distance, except the learned capture's on crunch): the matcher's Anything mode went from 4.3 to 5.5 dB spectral to 1.9 to 2.1, feel from 6.3 to 7.3 to 3.7 to 5.0. Applied straight to the mix, without Demucs, it reached 8.0 dB SI-SDR and 3.3 dB spectral on high_gain (the mix: -1.8 dB and 6.3).

Since 2026-10-06 it's in the app: "Cleaning up the target with your take" above. Unverified on real records: a real lead's bends and vibrato move the harmonics within a note, which a per-note pitch doesn't follow (a per-frame pitch track would), and a record tuned differently from Sean's guitar is caught only within +-60 cents.

### Save take, and a benchmark of real takes

After a play-along take of the section selected now, a quiet **Save take** button appears next to the mode in the Your DI card (it reads "Saved" until the next take; the status line says ", saved"). It writes `~/Library/Application Support/BellyDSP/ToneMatchTakes/<the target file's name>-<yyyymmdd-hhmmss>/` (a second save in the same second gets `-2`):

| File | What |
|---|---|
| `target.wav` | the selected section as decoded (48 kHz mono, 32-bit float) |
| `stem.wav` | its separated guitar, if a separated match of exactly this section made one |
| `di_raw.wav` | the take as recorded, from the song's first sample |
| `di.wav` | the take lined up with the section (`di_raw` from `align_samples` on): what Same part matches |
| `take.json` | `format` "bellydsp-tone-match-take", `version` 1, the app version and time, the target file and range, `section_samples`, `align_samples` and `latency_ms`, the device's reported latencies, the offset, the count-in, which source played, `band_seconds` 0.5, and `dtw` (the match's path, its frame size and hop, its slot) once a same-part match of exactly this take exists |

Writing happens on the message thread when the button is pressed; nothing touches the audio thread (the real-time safety test is unchanged and still passes). Tested (`tests/ToneMatchSaveTakeTests.cpp`, 70 checks): every file read back sample for sample against the session (with a DI that says where it was, `di.wav` starts exactly at input sample t0 + 352 for a reported 100 + 156 samples and a 2 ms offset), the JSON's fields, a second save in the same second, nothing to save before a take or for another range, the DTW path after a same-part match (167 steps, first and last pairs checked), `stem.wav` after a separated match, and the page's button (snapshot `build/proof/tone_match/25_take_saved.png`). Like the other tone match groups it's skipped on CI ("Tone match").

To build the benchmark (PROGRESS TM.8): on songs he knows, select the lead, record a take playing along, run Match (Same part, and with Separate the guitar first for a full mix, so `stem.wav` exists), Save take; then run the prototype on each folder and note, next to its numbers, which of the matcher's sound, the learned capture's, and the record he prefers.

### Running the prototype on a saved take

```
uv run prototypes/learn_tone.py "~/Library/Application Support/BellyDSP/ToneMatchTakes/<folder>"   [--mask] [--full-song] [--steps 3000]
```

uv reads the script's dependencies from its header (neural-amp-modeler 0.12.3, NumPy, SciPy, Demucs; about 1 GB the first time). It uses `stem.wav` if it's there (`--full-song`: the section instead), lines the notes up within the 0.5 s band, runs the current matcher on the same take (through `ampsim_render`, so the Release build has to exist), trains the capture (about 4 minutes), and writes into the folder `learned.nam` (an `amp_cab` capture: load it in a slot with the Cab off), `learned_take.wav` and `matcher_take.wav` (the take through each, to listen to), and `learn_tone_results.json`. It prints, per method: the spectral distance to the target, the per-note level error and spectrum error against the target's notes, and the per-note compression slope against the target's, plus the match EQ for the learned capture. There's no oracle on real data, and the target's slope is biased flat (above), so these say how close it got to the record on this take, not which is the better amp. Tried on a synthetic folder in the same format (the crunch rig in the band, masked): matcher 5.23 dB spectral, learned 2.53.

### What a "Learn tone" button would take

Inference is solved (the .nam loads in NAM core like any capture; tested). Training without Python is the hard part. The model is tiny (feather: 3,025 weights), the loss is a few FFTs, and one training is about 3000 steps of 16 notes.

| Option | What ships | Size | Licence | Cost and risk |
|---|---|---|---|---|
| A. libtorch in the app | PyTorch's C++ library, NAM's WaveNet and the loop ported to C++ | libtorch_cpu alone is hundreds of MB (the Python wheel's torch folder is 567 MB here); MPS from C++ is possible but not what NAM uses | BSD-3 (compatible with AGPL) | The biggest install by an order of magnitude, a second copy of NAM's model code to keep in step with NAM core and the trainer, a heavy CMake dependency on two OSes |
| B. A small hand-written trainer in C++ | forward and backward for the WaveNet layers (dilated conv, tanh, 1x1 mixes, the heads), Adam, the MRSTFT with JUCE's FFT, the .nam writer | tens of KB of code | ours (AGPL) | A few thousand lines with its own gradient bugs; it needs a golden test against PyTorch's gradients; CPU only. Rough cost: 3000 steps x 16 x 20,000 samples x ~3 x 3k multiply-adds per sample (forward and backward) is about 9e12 operations, a few minutes on all cores with SIMD. It's NAM-core-adjacent: a candidate upstream contribution (training in C++), which matches Sean's NAM goals |
| C. NAM's trainer through a local Python (uv) | the take folder (Save take already writes it) and a call to `uv run prototypes/learn_tone.py <folder>`; the app loads the .nam it writes | uv is 35 MB (could ship inside the app); the first run downloads Python and PyTorch, 1.1 GB here | uv MIT/Apache-2.0, PyTorch BSD, NAM trainer MIT | The cheapest to build (days), and it reuses exactly this prototype; but a 1 GB first-run download, a Python failure mode in a music app, and friends on Windows need it to work there too |
| D. Train on a server | an upload of the take and the section, a GPU job, a download of the .nam | nothing in the app | AGPL's network clause applies to a modified server (its source must be offered) | Running costs, accounts, and uploading excerpts of commercial recordings to a server Sean runs, which is a copyright and privacy question for a public service; the website is GitHub Pages (static), so it's new infrastructure |

### Not verified

How any of it sounds: the learned captures, the matcher's results here, the masked stems. Every number is a measurement on synthetic guitars through gray-box rigs. Whether real takes line up note by note as well as the synthetic ones (real leads have bends, slides, and ghost notes the onset detector and the per-note pitch don't model). Whether Save take's folder holds what Sean needs on his own songs (TM.8).

## tone_bench: a benchmark the matcher hasn't seen (Round 1)

Sean, 2026-10-06: tone matching "as powerful and accurate as possible", in gated rounds. Round 1 builds the benchmark and diagnoses the current matcher without changing it (ASSUMPTIONS TM57 to TM70). The earlier studies used hidden rigs made from the project's own gray-box models, which flatters the matcher; this one doesn't.

```
uv run prototypes/tone_bench/run.py --split dev|test --matcher current [--config same_clean,any_raw] [--cases id,...]
uv run prototypes/tone_bench/run.py --estimates --split dev|test     # the Round 2 what-ifs, then the report
```

It writes `build/tone_bench/<split>_current/report.md`, `results.json`, and one JSON per case (a rerun skips the done ones). Renders, records, and Demucs stems are cached in `build/tone_bench/cache`. A full split takes about 45 minutes (DEV) and 30 (TEST) with four cases in parallel on this Mac, the diagnosis included.

### What's in it

50 cases (`prototypes/tone_bench/cases.json`, seeded and calibrated, so they're reproducible): 10 per style (clean, edge of breakup, crunch, high gain, saturated lead), each style's ten split 3 dry, 3 produced, 4 mixed. DEV is 30 (for tuning) and TEST 20 (report only), with the same mix of styles and productions in both.

- **Amps the matcher never searches.** NAM core's example models (MIT, "Copyright (c) 2023 Steven Atkinson"; checked in `third_party/NeuralAmpModelerCore/LICENSE`): `lstm.nam` (nearly linear, a clean amp), `wavenet.nam` (edge to crunch with its input level), `A2.nam`, `slimmable_container.nam`, and `wavenet_a1_standard.nam` (high gain); the folder's generated test weights (outputs of +15 to +20 dBFS, crest near 0 dB) aren't used, and its files' provenance isn't documented. And ten new gray-box voicings (`tone_bench/rigs.py`), unlike the built-ins: 1 to 4 inverting triode stages with their own couplings, Miller filters, and bias shift (blocking), the tone stack between stages on some, three passive stacks with our own component values and an active one, a push-pull power amp with crossover, sag, presence, and resonance. 15 cases use a NAM model, 35 a gray-box. Each case's drive is set by bisection so the amp's own nonlinear energy ratio lands on a target drawn from its style's range (clean -40 to -28 dB, edge -26 to -17, crunch -16 to -9, high gain -9 to -5.5, lead -6 to -3.5).
- **Pedals, sometimes** (21 of 50): the pedal compressor, the clean boost, Mid Drive, Distortion, Transparent, and Fuzz, the app's own circuits (golden-tested against `circuits.py` and `compressor.py`) through new `ampsim_render` options (`--od`, `--pre-comp`, `--post-comp`, `--boost-tilt`), random settings.
- **Cabs the matcher can't pick.** Per split, a third of the 21 IRs (7, disjoint between splits; the factory cab stays searchable) is held out of the matcher's search, and the hidden cab is one of them, in 33 of 50 cases modified into a new IR: resonances moved (0.85 to 1.2 times), a mic-distance comb (0.4 to 2.5 ms), off-axis darkening, a small-speaker or a 4x12 low end, the minimum-phase version.
- **Recording and production.** Always a mic or channel EQ (a high-pass and 1 to 3 bands). Produced and mixed cases: a bus compressor (2 to 6 dB of reduction, 24 cases), a time effect at record mix levels (plate, room, slap, or a dotted-eighth delay, 24 cases), double tracking hard-panned on rhythm parts, drums and bass (and a rhythm guitar under a lead), a mastering limiter pushed 3 to 9 dB, and a lossy file (AAC through `afconvert` or MP3 through LAME, 128 to 256 kbps, at 44.1 kHz). Mixes are separated with Demucs and mixed to mono, as the app does.
- **Performances** (`tone_bench/performance.py`): scores per style with arpeggios that ring, strummed chords, power chords, palm mutes, gallops, runs with legato notes, bends, vibrato, and slides (a pitch curve read into the Karplus-Strong pluck), velocity setting level and pick brightness, a pickup resonance, a noise floor. A, the record's guitarist; B, the player's play-along take of the same notes (8 to 30 ms jitter on a slow drift of up to 40 ms, his own dynamics and attack, bends a little off, **his own guitar**); C, a held-out score on B's guitar, for scoring.

### How it scores

Every method's rig and the hidden rig play one evaluation signal (C, then probes), so the outputs are sample-aligned. The truth is the hidden rig's tone (pedal, amp, cab, mic EQ, bus compressor) with the player's guitar; the time effect is scored on its own. Facets (`tone_bench/metrics.py`, with the math): the loudness-weighted ERB long-term spectral distance (Glasberg and Moore's ERB scale, Zwicker's 0.23 power law over the PEAQ ear weighting and Terhardt's threshold), the same per note, the feel metric (learn_tone's compression, harmonics, attack, decay, probes 6 dB apart), the harmonic distribution on sine steps, intermodulation on a power chord's two tones, the crest-factor distribution, the time-effect tails, and a mel-cepstral distance (a check, not scored: no small embedding model with a clear licence installs without a large download). **Combined** is a weighted mean of the six scored facets, each divided by its median for a deliberately wrong rig on DEV: 0 is the hidden rig, about 1 a wrong rig. Weights: spectrum 0.5 (long-term 0.35, per note 0.15), feel 0.25, harmonics 0.1, crest 0.1, IMD 0.05.

Anchors on every case: the hidden rig scores exactly 0 (all facets 0.000 in both splits); a deliberately wrong rig (another style's case) 1.05 / 1.04 (DEV / TEST medians); "nothing matched" (each built-in at Gain 0, flat, the factory cab, no EQ) 0.93 / 0.92 on average, 0.65 / 0.68 for the best of the three.

### Baselines

The current matcher (`tone_match.py` with the gain sets: the C++ is its golden-tested port; the cleanup is `learn_tone.py`'s, which the C++ matches within 1e-7), the take as its DI, the 0.5 s play-along band in Same part, in four configurations: **same_clean** (the app's default after a take), same_raw, any_clean (TM55's proposal), and **any_raw**. Combined score, medians (lower is closer):

| Group | DEV same_clean | same_raw | any_clean | any_raw | TEST same_clean | same_raw | any_clean | any_raw |
|---|---|---|---|---|---|---|---|---|
| **all** (30 / 20) | 0.677 | 0.551 | 0.592 | **0.497** | 0.653 | 0.702 | 0.607 | **0.586** |
| clean | 0.636 | 0.650 | 0.498 | 0.541 | 0.760 | 0.663 | 0.602 | 0.562 |
| edge | 0.558 | 0.477 | 0.406 | 0.410 | 0.798 | 1.008 | 0.623 | 0.594 |
| crunch | 0.644 | 0.549 | 0.566 | 0.512 | 0.539 | 0.604 | 0.492 | 0.470 |
| high gain | 0.564 | 0.531 | 0.648 | 0.532 | 0.933 | 0.687 | 0.797 | 0.710 |
| lead | 0.743 | 0.605 | 0.631 | 0.545 | 0.713 | 0.780 | 0.646 | 0.663 |
| dry | 0.636 | 0.475 | 0.556 | 0.491 | 0.509 | 0.469 | 0.501 | 0.446 |
| produced | 0.604 | 0.445 | 0.599 | 0.451 | 0.861 | 0.648 | 0.611 | 0.568 |
| mixed, separated | 0.695 | 0.787 | 0.602 | 0.529 | 0.830 | 0.852 | 0.649 | 0.633 |
| no time effect | 0.688 | 0.637 | 0.594 | 0.513 | 0.556 | 0.598 | 0.592 | 0.519 |
| dotted-eighth delay | 0.497 | 0.532 | 0.638 | 0.498 | 0.882 | 0.856 | 0.743 | 0.746 |
| slap | 0.593 | 0.443 | 0.580 | 0.366 | 0.366 | 0.524 | 0.449 | 0.466 |
| plate | 0.661 | 0.479 | 0.548 | 0.525 | 1.065 | 1.157 | 0.652 | 0.743 |
| room | 0.582 | 0.622 | 0.427 | 0.419 | 0.811 | 0.702 | 0.682 | 0.546 |

(With 1 to 10 cases per cell, a single row means little; the "all" row and the paired tests below are what to read.) Beating the best of the three defaults (which needs the oracle to choose): same_clean 29 of 50, same_raw 33, any_clean 37, any_raw 43. The time-effect facet (not in the combined score) is 10.3 dB for the matcher on the 24 cases with a time effect and 2.2 dB if the true effect is added after its result.

What it says:

- **The app's default after a take is the worst of the four.** Same part with the cleanup lands at 0.665 (pooled median), no better than picking the best default by hand (0.656). Plain Anything without the cleanup is the best everywhere it counts: 0.516, better than same_clean in 38 of 50 cases (Wilcoxon signed-rank p = 2.5e-6) and than same_raw in 34 (p = 0.0003).
- **The cleanup hurts on a target that's already clean** and doesn't win on a separated mix either. On the clean record (the rig on A) of the 30 unmixed cases, cleaning moves the long-term spectrum by 1.1 (clean), 1.8 (edge), 1.9 (crunch), 3.6 (high gain), and 4.8 dB (lead) (the matcher's own measure, medians): chords and arpeggios that ring hold more than one pitch, bends and vibrato leave the 35-cent lobes, and a saturated amp has real energy between its harmonics. On the separated mixes it rescues Same part (0.695 against 0.789 on DEV) but Anything without it is as good or better (0.529 against 0.602 on DEV, 0.633 against 0.649 on TEST).

**TM55's verdict:** yes. After the cleanup, Anything beats Same part: 35 of 50 cases, median difference -0.032, Wilcoxon p = 0.0013 (DEV 20 of 30, p = 0.028; TEST 15 of 20, p = 0.022). But the question is overtaken: drop the cleanup too, and Anything is better still (any_raw beats any_clean in 33 of 50, p = 0.014).

### Diagnosis: where the error comes from

Per case, a ladder of matcher runs and oracle configurations (the oracle searches with the benchmark's own metric against the hidden rig on the evaluation signal; `tone_bench/oracle.py`):

- L0 the matcher on the real target; L1 on the dry target (the rig on A, no production or separation); L1b on the record's performance played on the player's guitar; L2 on the rig played by the player himself (no performance difference at all);
- L3 the oracle in the matcher's own search space (a built-in amp, Gain, tone, a searchable cab, match EQ); L4 the oracle allowed BellyDSP's pedal and post compressor too (the best of with and without);
- amp only, cab only, EQ only: the hidden rig with just that part replaced by BellyDSP's best, the linear parts refitted.

The oracle applies the linear part after a model (tone bands, cab, post EQ) by FFT convolution with its exact impulse response instead of re-rendering; against ampsim_render on every case it agrees within 64 to 86 dB SNR.

Means over all 50 cases (they add up to L0's mean):

| Part | same_clean | share | any_clean | share |
|---|---|---|---|---|
| L0 | 0.681 | | 0.599 | |
| production and separation (L0 - L1) | 0.057 | 8% | 0.030 | 5% |
| the player's guitar (L1 - L1b) | 0.018 | 3% | 0.060 | 10% |
| the playing: timing, dynamics, attack, alignment (L1b - L2) | 0.180 | 26% | 0.094 | 16% |
| objective and optimizer with a perfect target (L2 - L3) | 0.056 | 8% | 0.045 | 8% |
| pedal and compressor not searched (L3 - L4) | 0.064 | 9% | 0.064 | 11% |
| left at L4: amp and cab coverage | 0.306 | 45% | 0.306 | 51% |

| Oracle configuration | median | mean | notes |
|---|---|---|---|
| in-space (L3) | 0.347 | 0.370 | its Gain sits at the end of the range in 22 of 50 (17 at +24 dB: the built-ins don't reach the hidden rig's drive) |
| with pedal and compressor (L4) | 0.268 | 0.306 | on the 32 cases with a pedal or a bus compressor, 0.099 better; the compressor is in the winning variant in 23 of them, the pedal in 15; feel falls from about 2.5 to 1.3 dB |
| amp only | 0.171 | 0.201 | by style (mean): clean 0.109, edge 0.145, crunch 0.169, high gain 0.293, lead 0.288; NAM amps 0.25, gray-box 0.18 |
| cab only | 0.171 | 0.183 | modified IRs 0.202, exact held-out IRs 0.146 |
| EQ only (the 5-band match EQ for the mic EQ) | 0.031 | 0.040 | the post EQ can express a studio EQ |

**The objective is what fails, not the optimizer.** Scored by the matcher's own objective on its own target, the oracle's in-space configuration beat the matcher's pick in only 5 of 50 cases (same_clean; 3 of 50 for any_clean): the search finds what its score prefers. In the other 45 the score preferred a configuration that's further from the hidden rig. The ladder says why: given the rig played by the player himself (L2), the same matcher lands at 0.43, within 0.06 of the oracle; given another performance of the same notes (L1b) it lands at 0.61. The objective's statistics (long-term spectra and four distortion features, or the frame-by-frame comparison) move with how the notes were played, and the matcher fits the playing.

### Round 2 estimates

`run.py --estimates` (`tone_bench/estimates.py`), means over 50 cases:

| What-if | change in combined |
|---|---|
| the pick (any_raw) with a high-resolution minimum-phase match curve (1/12-octave, 8192 taps) instead of the 5-band EQ, fitted to the target without the oracle | -0.015 (better in 33 of 50) |
| the same curve on the oracle's in-space configuration | -0.042 (44 of 50) |
| the same curve on cab only (the true amp, the best searchable IR) | -0.075 (47 of 50): it absorbs about 40% of the cab coverage error |
| the true time effect after the pick (the time-effect facet only) | 10.3 to 2.2 dB on the 24 cases with one |

### The Round 2 plan this points to (for Sean's go)

In order, with what the numbers say each can close (combined units, means over 50; L0 is 0.68 for the app's default today):

1. **Make Anything the default after a take, and run the cleanup only when there's bleed to remove** (a separated mix, judged by the stem's energy between the take's harmonics). Measured now: 0.665 to 0.516 median (-0.15), no new DSP. The cheapest and biggest win.
2. **A performance-robust objective** (the largest closable share inside today's search space: from any_raw's 0.56 mean to the oracle's 0.37, up to -0.19, of which the playing difference alone is 0.09 to 0.18). Concretely: compare notes at matched input levels (bin the take's and the target's notes by their DI level, so a softer player isn't read as a cleaner amp), judge distortion on per-note harmonic and IMD profiles rather than whole-signal level statistics, use the benchmark's loudness-weighted ERB spectrum, and fit the player's guitar as a pre-amp EQ difference (the guitar is 3 to 10% of the error). Tune on DEV, report on TEST.
3. **New amp voicings as gain sets** (amp coverage: 0.20 mean, 0.29 on high gain and lead; the oracle's Gain at +24 dB in 17 of 50). Two high-gain sets (a tight four-stage one, a fat one with blocking), two saturated leads (with sag and compression), one power-amp-driven edge or crunch: five, each a five-step gain set from `make_default_captures.py` with the new voicings. Estimated: half of the amp share, about -0.10 at L4, and more of it on high gain and lead.
4. **The compressor and pedals in the search, the compressor first** (-0.10 on the 32 cases that have one, -0.06 over all; feel halves). A post-compressor fit (threshold, ratio, attack, release from the target's per-note level curve) before pedal selection; the pedal (Mid Drive, boost) as a discrete choice with drive on the 4 dB grid.
5. **A high-resolution minimum-phase match curve** instead of the 5-band EQ (-0.015 now, -0.04 once the amp is right, and 40% of the cab error). Cheap; worth more after 2 and 3.
6. **Cab coverage** last: the curve absorbs much of it; more IRs (small speakers, other mics) for the rest (cab only 0.18, modified IRs 0.20).
7. **A time-effect fit** (reverb decay, delay time and level, into the app's delay and reverb): outside the tone score, but the facet falls from 10.3 to 2.2 dB with a perfect fit.

The optimizer needs no work (5 misses in 50).

### Limits of this round

All of it is synthetic and judged by a metric we wrote: the facets and their weights are a reasoned guess at what matters, not a listening test. The gray-box voicings are still tanh-family models (if different ones), and only five NAM models are real captures of something. The guitars are Karplus-Strong. So the ranking of what limits the matcher is what to take from it, not the absolute numbers; whether the improvements it suggests sound closer is Sean's to judge.

## Not verified (only Sean can)

- How a match sounds, on anything. Every number here is a measurement on synthetic guitars.
- How the comparison sounds and feels: whether the switches and the loop are click-free to his ears, whether equal loudness by BS.1770 sounds equally loud on his material, and whether same part's alignment keeps a switch on the same note (PROGRESS TM.6).
- Real songs: Demucs on real mixes, real lead tones, real DIs (PROGRESS TM.1 to TM.3).
- The learning prototype and the informed mask on real takes (PROGRESS TM.8), and the cleanup in the app on a real song (TM.9).
- tone_bench's facets and weights as a stand-in for listening: whether a lower combined score sounds closer.

## Sources

- Fujishima, "Realtime chord recognition of musical sound: a system using Common Lisp Music", ICMC 1999 (chroma). Mueller, *Fundamentals of Music Processing*, Springer 2015, ch. 3 (chroma, DTW).
- Sakoe and Chiba, "Dynamic programming algorithm optimization for spoken word recognition", IEEE TASSP 26(1), 1978.
- Nelder and Mead, "A simplex method for function minimization", The Computer Journal 7(4), 1965.
- IEC 61672-1 (the A-weighting curve).
- Simper, "Linear Trapezoidal Integrated State Variable Filter With Low Noise Optimisation", Cytomic 2013; Zavalishin, *The Art of VA Filter Design*, 2012.
- Ellis, "Beat tracking by dynamic programming", Journal of New Music Research 36(1), 2007 (section 3.1, the tempo estimate).
- Karplus and Strong, "Digital synthesis of plucked-string and drum timbres", Computer Music Journal 7(2), 1983; Jaffe and Smith, "Extensions of the Karplus-Strong plucked-string algorithm", Computer Music Journal 7(2), 1983.
- Rouard, Massa, and Defossez, "Hybrid Transformers for Music Source Separation", ICASSP 2023.
- Bello, Daudet, Abdallah, Duxbury, Davies, Sandler, "A tutorial on onset detection in music signals", IEEE TSAP 13(5), 2005.
- Yamamoto, Song, Kim, "Parallel WaveGAN", ICASSP 2020 (the multi-resolution STFT loss); Steinmetz and Reiss, "auraloss: Audio focused loss functions in PyTorch", DMRN+15, 2020.
- McLeod and Wyvill, "A Smarter Way to Find Pitch", ICMC 2005.
- Le Roux, Wisdom, Erdogan, Hershey, "SDR - half-baked or well done?", ICASSP 2019 (SI-SDR).
- Griffin and Lim, "Signal estimation from modified short-time Fourier transform", IEEE TASSP 32(2), 1984 (the least-squares inverse STFT).
- Glasberg and Moore, "Derivation of auditory filter shapes from notched-noise data", Hearing Research 47, 1990 (the ERB scale). Zwicker and Fastl, *Psychoacoustics: Facts and Models*, Springer (the loudness power law). ITU-R BS.1387 (PEAQ: the outer and middle ear weighting). Terhardt, "Calculating virtual pitch", Hearing Research 1, 1979 (the threshold in quiet).
- Kubichek, "Mel-cepstral distance measure for objective speech quality assessment", IEEE PACRIM 1993.
- Grey, "Multidimensional perceptual scaling of musical timbres", JASA 61(5), 1977; McAdams, Winsberg, Donnadieu, De Soete, Krimphoff, "Perceptual scaling of synthesized musical timbres", Psychological Research 58, 1995.
- Pakarinen and Yeh, "A review of digital techniques for modeling vacuum-tube guitar amplifiers", Computer Music Journal 33(2), 2009. Yeh and Smith, DAFx 2006 (the three-knob tone stack's nodal analysis, the circuit model behind the gray-box stacks).
- Oppenheim and Schafer, *Discrete-Time Signal Processing* (the minimum-phase IR from the folded real cepstrum). Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients".

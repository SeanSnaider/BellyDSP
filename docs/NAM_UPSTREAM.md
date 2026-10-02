# NAM core: findings worth taking upstream

Sean wants to contribute to the NAM project. These are behaviours of NeuralAmpModelerCore (pinned at v0.6.0) found while building this app, each with how it showed up and what a fix or PR might look like. Nothing here is patched locally: submodules stay untouched, and changes go upstream as PRs.

## 1. LSTM keeps its state through Reset()

**Found:** 2026-10-01, input calibration tests.

`nam::DSP::Reset()` sets the sample rate and buffer size and then runs `prewarm()` (half a second of silence for an LSTM), but `nam::lstm::LSTM` doesn't reinitialize its cells' hidden and cell state. So whatever the model processed before a `Reset()` leaves a trace afterwards. Here, the app renders a reference DI through each capture at load to measure its loudness and then calls `Reset()`. Two copies of `lstm.nam` that rendered the DI at different levels came out of `Reset()` slightly different: their outputs then differed in the first 189 samples by at most -150 dBFS, and were bit-identical after that.

**Why it matters:** inaudible here, but `Reset()` reads as "start clean", and a host that resets a model to get a deterministic render won't get one for LSTMs. WaveNet models do reset fully in the same test.

**Possible PR:** an `LSTM::Reset` override (or a hook in `SetMaxBufferSize`) that restores each cell's initial hidden and cell state before prewarming, plus a test: process noise, reset, process an impulse, and compare against a fresh model.

## 2. slimmable_wavenet.nam differs from the render tool by -128.6 dB

**Found:** 2026-10-01, the differential test against NAM core's own `tools/render`.

Eight of the nine example models are bit-exact between this app's slot and NAM core's render tool; `slimmable_wavenet.nam` differs by -128.6 dB at every block size. That's about one part in a million, probably a different summation order somewhere in the slimmable container (the app processes in 128-sample blocks, the tool in its own block size). Worth a look to see whether the slimmable path is block-size dependent.

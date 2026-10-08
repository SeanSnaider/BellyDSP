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

## 3. ActivationTanh is half a WaveNet's time

**Found:** 2026-10-08, the CPU pass (BUILD_PLAN "CPU").

`nam::activations::ActivationTanh::apply` (NAM/activations.h) loops over the buffer calling `std::tanh` one float at a time. A standard A1 WaveNet calls it 240 times per sample (16 channels x 10 layers, then 8 x 10), 30720 times per 128-sample buffer; at about 3.5 ns per call on an M5 Pro that's roughly half of the model's whole time. `sample` showed `tanhf` as the hottest function in the app.

**What the app does now (no change to NAM core):** it registers its own "Tanh" in NAM's activation map (`src/dsp/NamTanh.*`, through a class derived from `Activation`, which may write the protected `_activations`): `Eigen::Map<Eigen::ArrayXf> (data, size) = ... .tanh()`, Eigen 5's vectorised rational approximation (GenericPacketMathFunctions.h, `ptanh_float`). Measured over every float in [-12, 12]: identical to libm's tanhf on 94.7%, at most 5 ulp (3.0e-7) apart, against NAM's own Fasttanh at 8.7e-4. A standard model went from 145 to 72 us per 128-sample buffer; its output stays within -112.4 dB of `tools/render` (the example models) and -134.5 dB of the stock tanh (the app's 40 built-in captures).

**Possible PR:** the same three lines in `ActivationTanh::apply` (and `ActivationSigmoid`, through `Eigen::ArrayXf::logistic()`), with a test that bounds the difference from `std::tanh` (5 ulp) and a benchmark. The LSTM (NAM/lstm.cpp) calls `tanhf` and the scalar sigmoid per element too; vectorising its gate loop over the hidden units is the same idea. A second, bigger one for later: Eigen's general GEMM (`gebp_kernel`, with its packing) is now the hottest code for 16-channel layers, where the matrices are tiny; NAM_ENABLE_A2_FAST already hand-optimises A2, and the same for A1 standard and lite would help every capture on TONE3000. (An experiment here: forcing Eigen's coefficient-based lazy products, EIGEN_GEMM_TO_COEFFBASED_THRESHOLD=1000, made a model 2.4x slower, so it needs real kernels, not a flag.)

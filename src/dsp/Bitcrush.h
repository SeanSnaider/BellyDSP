#pragma once

#include "Block.h"
#include "Svf.h"

#include <array>
#include <cmath>

namespace ampsim
{

/// The bitcrusher, one of Bloom's three effects (BUILD_PLAN "Bloom (modulation container)", design review
/// round 12): the one block where aliasing is the point. Stereo; zero latency.
///
/// 1. Sample-rate reduction: sample and hold driven by a fractional phase accumulator. Each sample the phase
///    advances by rate / fs; when it passes 1, the input is captured (and quantized) and held until the next
///    capture. Captures land on the host's sample grid, so for a ratio R = fs / rate the holds are floor(R) or
///    ceil(R) samples long, averaging exactly R (a fractional ratio alternates the two, like a Beatty
///    sequence). No oversampling and no anti-aliasing filter, deliberately: a tone at f comes back with images
///    at k rate +- f, folded around the host's Nyquist, shaped only by the hold's own response
///        |H(f)| = |sin(pi f R / fs) / (R sin(pi f / fs))|           (a zero-order hold, integer R)
///    so a 3 kHz tone held at 8 kHz carries an inharmonic 5 kHz image just 4.3 dB below it. Both channels
///    share one accumulator, like a stereo converter.
///
/// 2. Bit depth: quantization to 2^bits levels across full scale, two's complement style (mid-tread):
///        step = 2^(1 - bits),   y = clamp(step round(x / step), -1, 1 - step)
///    At integer bits that's exactly 2^bits levels, k step for k = -2^(bits-1) .. 2^(bits-1) - 1, including a
///    level at 0, so a decaying note rounds to silence once it's below half a step: the gated, sputtering
///    decay. The bits control is continuous: at fractional bits the step keeps the same formula (the full
///    scale holds 2^bits steps, a fractional count), and the levels are the grid's points inside
///    [-1, 1 - step] plus that range's ends where rounding is clipped. So the grid, the clip points, and the
///    quantization noise (step^2 / 12) all move smoothly as bits sweeps, and the level count only ever
///    rises, from 2^b at integer b to 2^(b+1) at the next (2.5 bits: 6 levels for a full-scale input).
///    Rounding is half away from zero, so the quantizer is odd-symmetric below the clip points. Bits run
///    1 to 16; at 1 bit the levels are -1 and 0, as in a 1-bit signed converter.
///    Quantizing happens at each capture: the virtual converter samples, quantizes, and holds.
///
/// 3. Dither (optional): triangular (TPDF) noise of +-1 step, the sum of two independent uniform values,
///    added before quantizing. TPDF makes the quantization error's mean and variance independent of the
///    signal (Lipshitz, Wannamaker, and Vanderkooy, "Quantization and Dither: A Theoretical Survey", JAES
///    1992): the error has mean 0 and variance step^2 / 4 (the dither's step^2 / 6 plus the rounding's
///    step^2 / 12) at every input level, and it's white, one new value per capture. Off, decays gate and
///    sputter; on, they sink into a steady hiss. Each channel has its own noise.
///
/// 4. Tone: a 2nd-order Butterworth low-pass (TPT SVF) on the crushed signal, 1 to 20 kHz; at 20 kHz it's
///    taken out of the path entirely (crossfaded over 20 ms), so the top of the knob is the raw crush.
///
/// 5. Mix: linear, out = (1 - mix) x + mix crushed, because the crushed signal is the dry plus an error and
///    stays correlated with it. Mix 0 is the dry exactly, mix 1 the crushed signal exactly.
///
/// Smoothing: bits and mix glide over 20 ms, the rate (multiplicatively) over 20 ms, the tone over 25 ms with
/// coefficients every 32 samples, and dither fades its noise in and out over 20 ms. On/off fades the
/// effect's mix over 10 ms along an S-curve (Fade.h); switched on from fully off, it starts from clean state.
class Bitcrush : public Block
{
public:
    static constexpr double minBits = 1.0, maxBits = 16.0;
    static constexpr double minRateHz = 100.0;
    static constexpr double minToneHz = 1000.0, maxToneHz = 20000.0;
    static constexpr double butterworthQ = 0.70710678118654752;
    static constexpr double smoothingSeconds = 0.020;
    static constexpr double toneSmoothingSeconds = 0.025;
    static constexpr double onFadeSeconds = 0.010;
    static constexpr int coefficientInterval = 32;

    struct Settings
    {
        bool on = false;
        float bits = 8.0f;        // 1 to 16, fractional
        float rateHz = 12000.0f;  // the held rate, 100 Hz up to the host rate (where nothing is held)
        bool dither = false;      // TPDF, +-1 step
        float toneHz = 20000.0f;  // the post low-pass, 1 to 20 kHz; 20 kHz is out of the path
        float mix = 1.0f;         // 0 dry to 1 crushed
    };

    /// The quantizer's step at a bit depth: full scale (2) over 2^bits levels.
    static double stepSize (double bits) noexcept { return std::exp2 (1.0 - bits); }

    /// The quantizer: y = clamp(step round(x / step), -1, 1 - step). Public so the tests can hold the audio to it.
    static double quantize (double x, double bits) noexcept;

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }

    /// Switched off and finished fading: process() leaves the audio untouched.
    bool isFullyOff() const noexcept { return onGain.getCurrentValue() <= 0.0 && ! onGain.isSmoothing(); }

private:
    void designTone (double frequency);
    void clearState();
    void jumpToTargets();

    double sampleRate = 48000.0;
    Settings settings;

    juce::SmoothedValue<double> bits { 8.0 }, mix { 1.0 }, dither { 0.0 }, toneOn { 0.0 }, onGain { 0.0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> rate { 12000.0 }, toneHz { 20000.0 };
    int samplesUntilUpdate = 0;
    bool wakePending = false;

    double phase = 1.0;                // the hold's accumulator: a capture whenever it passes 1
    std::array<double, 2> held {};      // the current held (quantized) value per channel
    std::array<Svf, 2> tone;
    std::array<juce::Random, 2> noise { juce::Random (0x5eed01), juce::Random (0x5eed02) };
};

} // namespace ampsim

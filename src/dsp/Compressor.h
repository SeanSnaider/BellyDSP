// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Svf.h"

#include <array>
#include <atomic>

namespace ampsim
{

/// The compressor (BUILD_PLAN "Compressor"): one block type, a mono instance in pre FX and a stereo,
/// stereo-linked instance in post FX. Two modes:
///
/// Studio: feed-forward, following D. Giannoulis, M. Massberg, J. Reiss, "Digital Dynamic Range
/// Compressor Design: A Tutorial and Analysis", JAES 60(6), 2012, in its recommended log-domain form:
///   1. Detector input: the compressor's own input through a sidechain high-pass (default 100 Hz), so
///      low notes don't pump everything. Stereo: the louder channel (linking).
///   2. Level x_L in dB: the instantaneous peak, or RMS over a 10 ms one-pole window.
///   3. Gain computer, a soft knee of width W around threshold T, ratio R (the paper's eq. 4):
///          y = x                                  for 2 (x - T) < -W
///          y = x + (1/R - 1) (x - T + W/2)^2 / 2W  for |2 (x - T)| <= W
///          y = T + (x - T) / R                    for 2 (x - T) > W
///   4. Gain reduction g = x_L - y (dB, >= 0), smoothed by a branching one-pole: attack coefficient
///      when g rises, release when it falls, alpha = exp(-1 / (tau fs)). Smoothing the reduction in dB
///      makes the knobs exact time constants: a step settles 63% of the way in tau.
///   5. Output gain 10^((makeup - g_smooth) / 20), then the dry/wet mix (parallel compression).
///
/// Pedal: feedback, in the style of OTA pedal compressors: the detector reads the compressor's own
/// previous output (before makeup and mix), with a fixed fast attack. A feedback loop sees
/// already-reduced levels, so its gain computer has slope (R - 1) instead of (1 - 1/R): in steady
/// state y = x - (R - 1)(y - T) gives y = T + (x - T)/R, the same static ratio as Studio. The knee is
/// the same quadratic, scaled. prototypes/compressor.py simulates both loops for the golden tests.
///
/// Auto release (program dependent): two smoothers in parallel, a fast one (the attack knob, 60 ms
/// release) and a slow one (400 ms attack, 1.5 s release), and the larger reduction wins. A short peak
/// barely charges the slow one, so the gain recovers in about 60 ms; seconds of heavy compression
/// charge it, so recovery slows down instead of pumping.
///
/// Auto makeup: the static curve's gain reduction at a reference level, so turning threshold or ratio keeps
/// the level roughly steady. The reference is where the signal's level typically sits, which depends on where
/// the compressor is: -12 dBFS after the cab (Settings' default, the post instance), and lower for the pre
/// instance, which compresses the DI itself (preAmpMakeupReferenceDb). The reference first was -12 dBFS for
/// both, taken from the DI's peaks; but between its peaks the DI sits far lower (a guitar DI peaking at -6
/// dBFS measures about -21 LUFS), and the detector follows that body, so the makeup of the reduction at
/// -12 dBFS lifted everything under the peaks: the pre instance at its defaults raised the DI's loudness by
/// 4.6 to 6.4 dB (the gain staging audit, 2026-10-04), which pushed every amp after it harder.
///
/// No lookahead: zero latency (foundation rules). Threshold, ratio, knee, makeup, and mix are smoothed
/// per sample.
class Compressor : public Block
{
public:
    enum class Mode
    {
        studio,
        pedal
    };

    enum class Detector
    {
        peak,
        rms
    };

    struct Settings
    {
        Mode mode = Mode::studio;
        Detector detector = Detector::peak;
        float thresholdDb = -24.0f;
        float ratio = 4.0f;
        float kneeDb = 6.0f;
        float attackMs = 8.0f;
        float releaseMs = 120.0f;
        bool autoRelease = true;
        float makeupDb = 0.0f;
        bool autoMakeup = true;
        float autoMakeupReferenceDb = -12.0f; // where auto makeup restores the level (see above)
        float mix = 0.7f; // 0 dry, 1 fully compressed
        bool sidechainHighPass = true;
        float sidechainHz = 100.0f;
    };

    static constexpr double rmsWindowMs = 10.0;
    static constexpr double pedalAttackMs = 2.0;
    static constexpr double autoFastReleaseMs = 60.0;
    static constexpr double autoSlowAttackMs = 400.0;
    static constexpr double autoSlowReleaseMs = 1500.0;
    static constexpr double autoMakeupReferenceDb = -12.0;   // the default (post-cab levels)
    static constexpr double preAmpMakeupReferenceDb = -21.0; // the pre instance: on the DI (the gain staging audit)
    static constexpr double smoothingSeconds = 0.020;

    explicit Compressor (bool isStereoBlock) : stereo (isStereoBlock) {}

    /// The feed-forward static curve: output level (dB) for input level x (dB).
    static double staticCurveDb (double x, double thresholdDb, double ratio, double kneeDb);

    /// The feedback gain computer: gain reduction (dB, >= 0) for detected output level y (dB).
    static double feedbackReductionDb (double y, double thresholdDb, double ratio, double kneeDb);

    /// The makeup gain auto makeup applies (dB): the static curve's reduction at the reference level.
    static double autoMakeupDb (double thresholdDb, double ratio, double kneeDb, double referenceDb = autoMakeupReferenceDb);

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    /// Meters, any thread: the largest gain reduction, input peak, and output peak of the last buffer (dB).
    float getGainReductionDb() const noexcept { return meterReduction.load (std::memory_order_relaxed); }
    float getInputPeakDb() const noexcept { return meterInput.load (std::memory_order_relaxed); }
    float getOutputPeakDb() const noexcept { return meterOutput.load (std::memory_order_relaxed); }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return stereo; }

private:
    double coefficient (double milliseconds) const;
    void updateTimeConstants();
    void updateSidechainFilter();

    const bool stereo;
    double sampleRate = 48000.0;
    Settings settings;

    juce::SmoothedValue<double> thresholdDb { -24.0 }, kneeDb { 6.0 }, makeupDb { 0.0 }, mix { 0.7 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> ratio { 4.0 };

    std::array<Svf, 2> sidechain; // per channel
    float sidechainDesignedHz = -1.0f;

    double attack = 0.0, release = 0.0, rmsCoefficient = 0.0;     // one-pole coefficients
    double slowAttack = 0.0, slowRelease = 0.0;
    double meanSquare = 0.0;           // RMS detector state
    double reduction = 0.0;            // smoothed gain reduction, dB
    double slowReduction = 0.0;        // auto release's slow branch, dB
    std::array<double, 2> lastOutput {}; // pedal mode: the previous output, before makeup and mix

    std::atomic<float> meterReduction { 0.0f }, meterInput { -100.0f }, meterOutput { -100.0f };
};

} // namespace ampsim

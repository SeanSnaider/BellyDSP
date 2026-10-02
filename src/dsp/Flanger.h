#pragma once

#include "Block.h"
#include "DelayLine.h"
#include "Lfo.h"
#include "ModulatedDelay.h"

#include <array>
#include <cmath>

namespace ampsim
{

/// The flanger, one of Bloom's three effects (BUILD_PLAN "Bloom (modulation container)"): a thin layer over
/// the shared modulated-delay engine (ModulatedDelay.h), one single-voice engine per channel. Stereo.
///
/// The comb. The wet is the input delayed by d, swept by the LFO, and the output mixes it with the dry:
///     out = (1 - mix) x + mix (+-wet)
/// With no feedback and the delay held (depth 0, the Manual control's static comb) that's
///     H(f) = (1 - mix) +- mix e^(-j 2 pi f d)
/// which at mix 1/2 is |cos(pi f d)| (positive polarity: notches at odd multiples of 1/2d, the warm "positive"
/// flange) or |sin(pi f d)| (negative: notches at multiples of 1/d from DC up, the thin "negative" flange).
/// Either way the notches are 1/d apart, and sweeping d moves them all, the high ones fastest.
///
/// Delay: Manual sets d from 0.5 to 10 ms, the centre of the sweep; depth swings it proportionally,
///     d(t) = manual (1 + 0.9 depth m(t))       m the LFO in [-1, 1]
/// so full depth sweeps from 0.1 to 1.9 times Manual: about 4 octaves of notch movement wherever Manual sits.
/// Reads use the engine's 4-point Hermite interpolation; LFO shapes are sine, triangle, or smoothed random;
/// the right channel's LFO leads by the stereo phase (0 to 180 degrees).
///
/// Feedback, -0.95 to 0.95: the engine writes x + tanh(fb HP(wet)) back into its line, so the loop's gain
/// stays below 1 and its level is bounded by the soft clip whatever the setting, and HP, a first-order
/// high-pass at 150 Hz in the loop, keeps the lows from recirculating into a boom (at 80 Hz the loop gain is
/// at most 0.45 even at 0.95). Positive feedback raises resonant peaks at multiples of 1/d, negative at the
/// odd multiples of 1/2d. Resonance adds power: a feedback comb's average power gain is 1 / (1 - fb^2), so
/// the wet is scaled by sqrt(1 - fb^2), which holds the level while feedback sharpens the peaks.
///
/// Through-zero (opt-in). Tape flanging used two machines; slowing one let its copy pass through the other's
/// timing, and at the crossing they cancel completely (with one inverted). Here the dry path is delayed by
/// exactly 5 ms (240 samples at 48 kHz) and the wet sweeps through it: Manual near 5 ms centres the sweep on
/// the dry, the relative delay tau = d - 5 ms goes negative (the wet ahead of the dry) and positive, and at
/// tau = 0 with negative polarity and mix 1/2 the output is exactly zero at every frequency. It adds 5 ms of
/// latency, reported by latencySamples() while the mode is on: the project's one documented latency
/// exception (BUILD_PLAN "Latency"). There's no feedback in this mode (tape flanging has none, and a loop
/// around a relative delay that passes through zero can't exist), so the feedback control is ignored.
/// The mode only changes when the owner calls setThroughZero(), or at reset() and prepare(): switching it
/// moves the dry path by 5 ms, which would click, so Bloom dips its output to silence around the switch.
///
/// Smoothing: Manual, depth, rate, and feedback glide over 100 ms inside the engines (a glide of the delay
/// bends pitch while it moves); mix over 20 ms; polarity flips through zero over 20 ms. A change of LFO shape
/// or stereo phase crossfades over 30 ms (an S-curve, Fade.h) to a second pair of engines that starts on the
/// first pair's LFO cycle (as the chorus does); every engine's line is written every sample, so the incoming
/// pair is current, and its feedback ramps in from 0 so the loop builds up instead of switching on.
/// On/off fades the effect's mix over 10 ms (S-curve). Switched off, the lines keep being written (and in
/// through-zero mode the output is still the 5 ms-delayed dry), so switching back on needs no reset: the glides
/// jump to their targets, so a knob moved while it was off doesn't sweep in, and the feedback ramps in.
class Flanger : public Block
{
public:
    static constexpr double minManualMs = 0.5, maxManualMs = 10.0;
    static constexpr double maxSwing = 0.9;
    static constexpr double maxDelayMs = 20.0; // 10 ms x 1.9, plus margin
    static constexpr double minRateHz = 0.05, maxRateHz = 10.0;
    static constexpr double maxFeedback = 0.95;
    static constexpr double loopHighPassHz = 150.0;
    static constexpr double throughZeroMs = 5.0;
    static constexpr double modulationSmoothingSeconds = 0.100;
    static constexpr double smoothingSeconds = 0.020;
    static constexpr double bankFadeSeconds = 0.030;
    static constexpr double onFadeSeconds = 0.010;

    struct Settings
    {
        bool on = false;
        float manualMs = 2.5f;                    // 0.5 to 10: the delay at the centre of the sweep
        float depth = 0.6f;                       // 0 (static comb) to 1 (0.1 to 1.9 x Manual)
        Lfo::Shape shape = Lfo::Shape::triangle;
        float rateHz = 0.2f;                      // 0.05 to 10
        float feedback = 0.5f;                    // -0.95 to 0.95 (none in through-zero mode)
        bool negative = false;                    // the wet's polarity in the mix
        float stereoPhase = 0.25f;                // the right LFO's lead in cycles, 0 to 0.5
        float mix = 0.5f;                         // 0 dry to 1 wet; 0.5 gives the deepest notches
        bool throughZero = false;                 // applied by setThroughZero(), reset(), or prepare()
    };

    /// One channel's engine settings: a single voice at Manual, swung by depth, the right one leading by the
    /// stereo phase, with the loop's feedback and high-pass. Public so the tests can hold the audio to it.
    static ModulatedDelay::Settings engineSettings (const Settings& settings, int channel, bool throughZero);

    /// The wet's level compensation for feedback fb: sqrt(1 - fb^2).
    static double feedbackCompensation (double fb) noexcept { return std::sqrt (1.0 - fb * fb); }

    /// Audio thread, once per buffer. Everything but throughZero.
    void setSettings (const Settings& settings);

    /// Audio thread: switches through-zero on or off at once. Only while the output is silent (Bloom calls it
    /// at the bottom of a dip): the dry path's delay jumps, and every glide jumps to its target.
    void setThroughZero (bool shouldBeOn);
    bool isThroughZero() const noexcept { return throughZero; }
    int getThroughZeroSamples() const noexcept { return throughZeroSamples; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    int latencySamples() const override { return throughZero ? throughZeroSamples : 0; }
    bool isStereo() const override { return true; }

    bool isFullyOff() const noexcept { return onGain.getCurrentValue() <= 0.0 && ! onGain.isSmoothing(); }

    /// For tests: the heard pair's delay (samples) on a channel at the last sample, and whether a pair
    /// crossfade is running.
    double getDelaySamples (int channel) const noexcept { return banks[(size_t) selected].engines[(size_t) channel].getVoiceDelay (0); }
    bool isSwitching() const noexcept;

private:
    struct Bank
    {
        Lfo::Shape shape = Lfo::Shape::triangle;
        double stereoPhase = 0.25;
        std::array<ModulatedDelay, 2> engines; // per channel
        juce::SmoothedValue<double> gain { 0.0 };
    };

    void configure (Bank& bank);
    void startBank (int index);
    void startEngines (Bank& bank, double phase);
    void select (int index);
    void applyBankRequest();
    void wake();
    double requestedStereoPhase() const noexcept;
    void writeLines (int channel, float x) noexcept;
    static bool audible (const Bank& bank) noexcept { return bank.gain.getCurrentValue() > 0.0 || bank.gain.isSmoothing(); }

    double sampleRate = 48000.0;
    Settings settings;
    bool throughZero = false;
    int throughZeroSamples = 240;
    bool wakePending = false;

    std::array<Bank, 2> banks;
    int selected = 0;
    std::array<DelayLine, 2> dryLines; // the through-zero dry path, per channel

    juce::SmoothedValue<double> mix { 0.5 }, polarity { 1.0 }, onGain { 0.0 };
};

} // namespace ampsim

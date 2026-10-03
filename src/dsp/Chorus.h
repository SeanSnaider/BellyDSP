// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Lfo.h"
#include "ModulatedDelay.h"
#include "Svf.h"

#include <array>

namespace ampsim
{

/// The chorus (BUILD_PLAN "Chorus", design review round 9): a stereo post-FX block on the shared
/// modulated-delay engine (ModulatedDelay.h), one engine per channel. Zero latency: the dry signal is never
/// delayed.
///
/// Modes. Every voice reads its channel's delay line at d = base + depth * LFO, and the depth knob scales
/// the mode's maximum sweep:
///   Classic    one voice per side at 11 ms, swept up to +-4 ms (7 to 15 ms at full depth): the CE-2 warble.
///              The right side's LFO is inverted, so its pitch rises while the left's falls.
///   Dimension  two voices in exact antiphase, A on the left line and B on the right, at 7 ms, swept up to
///              +-1 ms (shallow). The wet is their difference, (A - B)/sqrt(2) on the left and the opposite
///              on the right: the antiphase output matrix of the Roland SDD-320 Dimension D. The difference
///              of two opposite sweeps doesn't move in pitch: for a tone,
///                  e^{-jw(d0 + A m)} - e^{-jw(d0 - A m)} = -2j sin(w A m) e^{-jw d0},
///              which pulses in level at a fixed frequency (0.1 cent of residual wobble measured). The two
///              sides are opposite in polarity, so it's pure width: it cancels in mono and Width scales the
///              whole effect. It fades out by itself toward low frequencies, where sin(w A m) is small. The
///              hardware also high-passes the subtracted voice so the bass doesn't cancel; the crossover
///              below already keeps the bass out of the voices, and without that filter the shallow voice
///              doesn't comb with the dry in the low mids (deepest dip at 50% mix -6 dB instead of -15).
///   Tri        three voices, LFOs 120 degrees apart, at 12 ms, swept up to +-3 ms, panned left, centre,
///              right: the big studio rack chorus. Left hears voices 0 and 1, right hears 1 and 2; the centre
///              voice runs in both engines (same phase and random sequence) at 1/sqrt(2), and each side is
///              scaled by 1/sqrt(1.5), so each side holds one voice's power. Some voices rise while others
///              fall, so the pitch movement averages out across the stereo field. Three voices summed in
///              each channel average out the delay sweep too, and the dry then combs with a nearly fixed
///              delay (-14 dB dips at 50% mix, against -9.5 dB panned).
///
/// LFO: triangle (default), sine, or smoothed random, 0.05 to 10 Hz (tempo sync arrives as Hz). In random,
/// inverted partners share one sequence, so Classic's and Dimension's pairs stay in exact antiphase; Tri's
/// three voices wander independently.
///
/// Signal path, per channel:
///   1. Low-end protection (on by default, 150 Hz): only a 4th-order Butterworth high-pass of the input
///      (24 dB/oct, two SVF sections at Q 1.307 and 0.541) goes into the delay lines, and a 2nd-order
///      Butterworth low-pass at the same frequency (12 dB/oct, Q 0.707) carries the lows past them. The
///      low E's fundamental (82 Hz) reaches the lines 21 dB down, so low notes keep their pitch and level.
///   2. Analog character (on by default): light saturation tanh(v) on the way into the lines, BBD style
///      (transparent at low levels; a -6 dBFS sine loses 0.5 dB and gains a third harmonic 34 dB, about
///      2%, below its fundamental), and a gentle
///      12 dB/oct low-pass at 7 kHz on the wet, like a BBD's anti-aliasing and reconstruction filters, with
///      optional faint white noise (-80 dBFS RMS) ahead of it. Off is a pristine digital chorus. Not
///      oversampled: the tanh is light and the wet is post-cab, so its harmonics fold back far below the
///      signal, and the 7 kHz low-pass follows it.
///   3. Width: mid/side on the wet, the side scaled by width (0 to 1). The mono sum doesn't depend on it.
///   4. Mix, equal power: out = cos(mix pi/2) x + (1 - cos(mix pi/2)) low + sin(mix pi/2) wet. The chorused
///      band is uncorrelated with the dry, so its level holds at every mix, and the lows stay at unity.
///      Mix 0 is x exactly; mix 1 is the wet plus the protected lows (pure vibrato in Classic with the
///      high-pass off).
///
/// Why those two filters: above the low end the wet is uncorrelated with the dry, so it's power that adds
/// there, and |LP2|^2 + |HP4|^2 stays within 0.7 dB of 1 at every frequency (1/(1 + W^4) + W^8/(1 + W^8),
/// W the warped frequency over the cutoff), so even at 100% wet the level barely moves through the
/// crossover. The low-pass stays at 12 dB/oct because it's mixed with the unfiltered dry: a 4th-order
/// low-pass is 180 degrees out at the cutoff and dug an 8 dB hole there at 50% mix, where the 2nd-order
/// one is 90 degrees out and blends. The obvious alternative, "x minus its high-pass" as the low band, keeps
/// low notes at exactly unity but bulges +3 dB (50% mix) to +5.5 dB (100%) around the cutoff, and a 2nd-order
/// high-pass let the low E through only 11 dB down (5 cents of wobble and a 2 dB dip at rate 2 Hz instead of
/// 1.3 cents and 0.7 dB). The numbers behind every choice here are in prototypes/chorus.py's design study.
///
/// Smoothing: rate and depth glide over 100 ms inside the engines; mix, width, and the analog, noise, and
/// crossover on/off amounts over 20 ms; the crossover frequency over 25 ms with coefficients every 32
/// samples. A mode or shape change crossfades linearly over 30 ms to a second pair of engines that starts
/// where the first pair's LFO cycle is, so no voice ever jumps. Switching back mid-fade reverses the fade;
/// a third choice waits for the fade to end. Both pairs' delay lines are written every sample, so the
/// incoming pair never starts from an empty line.
class Chorus : public Block
{
public:
    enum class Mode
    {
        classic,
        dimension,
        tri
    };

    struct ModeSpec
    {
        const char* name;
        int voicesPerChannel;
        double baseDelayMs; // the centre of every voice's sweep
        double maxDepthMs;  // half the sweep at depth 1
    };

    static constexpr std::array<ModeSpec, 3> modeSpecs { {
        { "Classic", 1, 11.0, 4.0 },
        { "Dimension", 1, 7.0, 1.0 }, // A on the left, B on the right
        { "Tri", 2, 12.0, 3.0 },      // voices 0 and 1 on the left, 1 and 2 on the right
    } };

    static constexpr double minRateHz = 0.05, maxRateHz = 10.0;
    static constexpr double minHighPassHz = 40.0, maxHighPassHz = 1000.0;
    static constexpr double butterworthQ = 0.70710678118654752; // 2nd order: the low band, the analog low-pass
    static constexpr double dimensionGain = 0.70710678118654752;
    static constexpr double triSideLevel = 0.81649658092772603;   // 1/sqrt(1.5)
    static constexpr double triCentreLevel = 0.57735026918962576; // 1/sqrt(1.5) / sqrt(2)
    static constexpr double analogLowPassHz = 7000.0;
    static constexpr double noiseRmsDb = -80.0;
    static constexpr double maxDelayMs = 20.0;
    static constexpr double modulationSmoothingSeconds = 0.100;
    static constexpr double smoothingSeconds = 0.020;
    static constexpr double crossoverSmoothingSeconds = 0.025;
    static constexpr double modeFadeSeconds = 0.030;
    static constexpr int coefficientInterval = 32;

    struct Settings
    {
        Mode mode = Mode::classic;
        Lfo::Shape shape = Lfo::Shape::triangle;
        float rateHz = 0.8f;          // 0.05 to 10
        float depth = 0.5f;           // 0 to 1 of the mode's maximum sweep
        float mix = 0.5f;             // 0 dry to 1 wet
        float width = 1.0f;           // 0 (wet in mono) to 1 (full stereo)
        bool analog = true;           // saturation and the 7 kHz low-pass on the wet
        bool noise = false;           // faint noise, only with analog on
        bool wetHighPass = true;      // low-end protection
        float wetHighPassHz = 150.0f; // 40 to 1000
    };

    /// The engine settings for one channel of a mode: delays, depths, phases, polarities, random seeds, and
    /// levels, as described above. Public so the tests can hold the audio to it.
    static ModulatedDelay::Settings voiceSettings (Mode mode, Lfo::Shape shape, int channel, double rateHz, double depth);

    /// Q of section s (0 or 1) of the 4th-order Butterworth high-pass: 1 / (2 sin((2s + 1) pi / 8)), as in
    /// CutFilter: 1.307 and 0.541.
    static double highPassQ (int section);

    /// The mix law (equal power): out = dry x + (1 - dry) low + wet * (the chorused signal), with
    /// dry = cos(mix pi/2) and wet = sin(mix pi/2).
    struct MixGains
    {
        double dry, wet;
    };
    static MixGains mixGains (double mix) noexcept;

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }

    /// For tests: what's being heard (the selected pair of engines, once any mode fade has finished).
    Mode getActiveMode() const noexcept { return banks[(size_t) selected].mode; }
    bool isSwitching() const noexcept;
    double getVoiceDelaySamples (int channel, int voice) const noexcept;

private:
    struct Bank
    {
        Mode mode = Mode::classic;
        Lfo::Shape shape = Lfo::Shape::triangle;
        std::array<ModulatedDelay, 2> engines; // per channel
        juce::SmoothedValue<double> gain { 0.0 };
    };

    void configure (Bank& bank);
    void startBank (int index, Mode mode, Lfo::Shape shape);
    void select (int index);
    void applyModeRequest();
    void designCrossover (double frequency);
    void resetCrossover();
    static bool audible (const Bank& bank) noexcept { return bank.gain.getCurrentValue() > 0.0 || bank.gain.isSmoothing(); }

    double sampleRate = 48000.0;
    Settings settings;

    std::array<Bank, 2> banks;
    int selected = 0;

    juce::SmoothedValue<double> mix { 0.5 }, width { 1.0 }, analog { 1.0 }, noise { 0.0 }, crossoverOn { 1.0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> crossoverHz { 150.0 };
    int samplesUntilUpdate = 0;

    std::array<std::array<Svf, 2>, 2> wetHighPass; // [channel][section]: what feeds the lines
    std::array<Svf, 2> lowBand;                    // per channel: the lows carried past the lines
    std::array<Svf, 2> analogLowPass;              // per channel
    std::array<juce::Random, 2> noiseSources { juce::Random (101), juce::Random (202) };
};

} // namespace ampsim

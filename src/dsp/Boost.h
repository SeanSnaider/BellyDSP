#pragma once

#include "Block.h"
#include "DriveEngine.h"
#include "Svf.h"

#include <array>
#include <complex>

namespace ampsim
{

/// The boost (BUILD_PLAN "Boost and Overdrive"): a mono pre-FX block, mostly for pushing an amp.
///
/// Modes:
///   - Clean: flat gain with an optional tilt EQ, the lead boost. The tilt is a high shelf of `tiltDb` at
///     1 kHz (Q 0.5, gentle) with the whole thing turned down by tiltDb / 2, so lows move by -tilt/2,
///     highs by +tilt/2, and 1 kHz stays put. At 0 dB of level and tilt the block is bit-transparent.
///   - Tight: linear, no clipping. A 12 dB/oct Butterworth high-pass at the tight frequency (cuts the low
///     end that makes a high-gain amp flub) and a mid push, a bell at 800 Hz (Q 0.7), where the Tube
///     Screamer's own small-signal response peaks (806 Hz at drive 0, tone noon; tests/fixtures/drive).
///   - Screamer: the drive engine's Mid Drive circuit (the TS808) at minimal drive and the tone at noon:
///     the classic metal boost, with its slight compression and mid hump. It runs oversampled; Clean and
///     Tight are linear and don't need to.
/// Controls: Level in every mode, tilt in Clean, the tight frequency and mid amount in Tight.
///
/// Mode switches work like the engine's: a mode only runs while it's heard, one coming back runs unheard
/// for 80 ms from rest, then independent 20 ms ramps cross over with sin(pi/2 p) each. Knobs are smoothed
/// per sample, filter coefficients redesigned every 32 samples.
class Boost : public Block
{
public:
    enum class Mode
    {
        clean,
        tight,
        screamer
    };

    static constexpr int numModes = 3;
    static constexpr double tiltPivotHz = 1000.0, tiltQ = 0.5;
    static constexpr double midPushHz = 800.0, midPushQ = 0.7;
    static constexpr double screamerDrive = 0.0, screamerTone = 0.5;
    static constexpr double switchSeconds = DriveEngine::switchSeconds;
    static constexpr double smoothingSeconds = 0.020;
    static constexpr int coefficientInterval = 32;

    struct Settings
    {
        Mode mode = Mode::clean;
        float levelDb = 0.0f;  // -24 to +24, every mode
        float tiltDb = 0.0f;   // Clean: highs minus lows, -12 to +12
        float tightHz = 150.0f; // Tight: high-pass, 20 to 1000 Hz
        float midDb = 6.0f;    // Tight: mid push, 0 to 12 dB
        int oversampling = 4;  // Screamer
        double voltsAtFullScale = drive::defaultVoltsAtFullScale;
    };

    Boost();

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;

    DriveEngine& getScreamer() noexcept { return screamer; }

    /// The linear modes' exact responses (what the tests hold them to): the SVFs' analog prototypes at the
    /// bilinear-warped frequency.
    static std::complex<double> cleanResponse (double tiltDb, double frequency, double sampleRate);
    static std::complex<double> tightResponse (double tightHz, double midDb, double frequency, double sampleRate);

private:
    void processChunk (float* x, int n) noexcept;
    void updateFilters (bool force) noexcept;
    void runPath (int path, float* y, int n) noexcept;
    void startPath (int path) noexcept;
    void beginFade (int path) noexcept;
    void abandonWarmup() noexcept;

    double sampleRate = 48000.0;
    Settings settings;
    DriveEngine screamer;
    Svf tilt, tightHighPass, midPush;

    juce::SmoothedValue<double> tiltDb { 0.0 }, midDb { 6.0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> level { 1.0 }, tightHz { 150.0 };
    double appliedTilt = -1000.0, appliedTight = -1.0, appliedMid = -1000.0, tiltGain = 1.0;

    std::array<double, numModes> position {}, target {};
    std::array<bool, numModes> running {};
    double positionStep = 0.0;
    int warming = -1, warmupRemaining = 0, warmupSamples = 3840;

    std::array<float, coefficientInterval> input {}, pathOut {}, mixed {};
};

} // namespace ampsim

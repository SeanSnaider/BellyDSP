#pragma once

#include "DriveCircuits.h"
#include "Oversampler.h"
#include "Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>

namespace ampsim
{

/// The shared drive engine (BUILD_PLAN "Boost and Overdrive"): everything a pedal circuit needs around it,
/// the same for every mode of the Boost and the Overdrive. Per buffer of mono audio:
///
///     x ──► volts ──► up 4x (8x) ──┬──► down ─────────────────────────► dry (aligned)
///                                  └──► tight high-pass ──► circuit(s) ──► down ──► wet
///     out = level ((1 - mix) dry + mix wet) / volts-at-full-scale
///
/// Volts: the circuits work in real voltages (their diodes clip at ~0.6 V), so samples are scaled by the
/// interface's full-scale voltage on the way in and back on the way out: a circuit at unity gain is
/// unity in the chain.
///
/// Oversampling: the circuits run at 4x by default (8x optional, 1x and 2x for comparisons) through the
/// polyphase IIR halfbands in Oversampler.h, which add no latency beyond their own group delay (4.2
/// samples at 4x, 0.09 ms, at low frequencies).
///
/// Dry path: parallel blend (Mix) needs the dry to line up with the wet, or the oversampler's few samples
/// of group delay would comb-filter the blend (a first notch near 5 kHz). So the dry goes through the same
/// upsampler and its own downsampler with nothing in between: exactly the wet path's linear response.
///
/// Mode switches: every circuit has a position that ramps linearly to 1 (selected) or 0 over 20 ms and
/// contributes sin(pi/2 p) of its output, the equal-power law the amp slots use; a second switch mid-fade
/// just redirects the ramps. A circuit only runs while it's heard. One coming back starts from rest and
/// first runs unheard for 80 ms, so its coupling capacitors have settled on the signal before it fades in
/// (started cold, a circuit clips around the wrong bias for tens of milliseconds).
///
/// Knobs: drive, tone, and the tight frequency are smoothed, and the circuits' coefficients updated once
/// per 32 samples; level and mix are smoothed per sample. A change of oversampling factor fades the output
/// to silence over 5 ms, switches and resets, and fades back in.
class DriveEngine
{
public:
    static constexpr int maxCircuits = 4;
    static constexpr int coefficientInterval = 32;
    static constexpr int maxFactor = 8;
    static constexpr double switchSeconds = 0.020;
    static constexpr double warmupSeconds = 0.080;
    static constexpr double smoothingSeconds = 0.020;
    static constexpr double factorFadeSeconds = 0.005;
    static constexpr float tightOffHz = 20.0f;
    static constexpr float maxTightHz = 2000.0f;

    struct Settings
    {
        int circuit = 0;
        float drive = 0.5f;          // 0 to 1
        float tone = 0.5f;           // 0 to 1, 1 brightest
        float tightHz = tightOffHz;  // pre high-pass on the wet path; 20 Hz or less is off
        float mix = 1.0f;            // 0 dry, 1 wet
        float levelDb = 0.0f;        // -60 to +24
        int oversampling = 4;        // 1, 2, 4, 8
        double voltsAtFullScale = drive::defaultVoltsAtFullScale;
    };

    /// Construction time only (allocates).
    void addCircuit (std::unique_ptr<drive::Circuit> circuit);
    int getNumCircuits() const noexcept { return numCircuits; }
    drive::Circuit& getCircuit (int index) noexcept { return *circuits[(size_t) index]; }

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /// Audio thread, once per buffer. No allocation.
    void setSettings (const Settings& settings);

    /// Mono, in place, any length.
    void process (float* samples, int numSamples) noexcept;

    int getFactor() const noexcept { return factor; }
    bool isSwitching() const noexcept;
    /// The oversampling chain's group delay at low frequencies, in base-rate samples, for the current factor.
    double groupDelaySamples() const noexcept;

private:
    void processChunk (float* samples, int numSamples) noexcept;
    void applyFactor (int newFactor) noexcept;
    void updateControls (bool force) noexcept;
    void startCircuit (int index) noexcept;
    void beginFade (int index) noexcept;
    void abandonWarmup() noexcept;

    double sampleRate = 48000.0;
    Settings settings;
    std::array<std::unique_ptr<drive::Circuit>, maxCircuits> circuits;
    int numCircuits = 0;

    std::array<double, maxCircuits> position {}, target {};
    std::array<bool, maxCircuits> running {};
    double positionStep = 0.0; // per oversampled sample
    int warming = -1;          // a circuit running unheard before its fade-in
    int warmupRemaining = 0, warmupSamples = 3840;

    Upsampler upsampler;
    Downsampler wetDownsampler, dryDownsampler;
    Svf tight;
    bool tightOn = false;
    int factor = 4;
    double factorGain = 1.0, factorGainStep = 0.0;
    std::array<double, 4> groupDelays {}; // by stage count

    juce::SmoothedValue<double> drive { 0.5 }, tone { 0.5 }, mix { 1.0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> level { 1.0 }, tightHz { 20.0 };
    double appliedDrive = -1.0, appliedTone = -1.0, appliedTightHz = -1.0;

    std::array<double, coefficientInterval> volts {}, dry {}, wet {};
    std::array<double, coefficientInterval * maxFactor> high {}, scratch {}, wetHigh {};
};

} // namespace ampsim

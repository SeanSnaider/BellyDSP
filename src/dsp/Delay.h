#pragma once

#include "Block.h"
#include "DelayLine.h"
#include "Lfo.h"
#include "Svf.h"

#include <array>

namespace ampsim
{

/// The delay (BUILD_PLAN "Delay"): a stereo, preallocated 4 s delay with three characters, three
/// stereo layouts, a feedback loop that can self-oscillate, ducking, and spillover.
///
/// Per channel, each sample:
///     tap  = the line read `time` samples back (Hermite, between samples)
///     wet  = saturate (colour (low cut (high cut (tap))))     filters and saturation sit in the loop,
///     line <- input + feedback x wet                        so each repeat is thinner and darker
///     out  = dry x cos(mix pi/2) + wet x duck x sin(mix pi/2)
/// The mix is equal-power because the repeats are decorrelated from the dry (they're earlier sound).
///
/// Time changes. Digital: two read heads; a new time starts a 50 ms equal-power crossfade from the
/// old head to one at the new time, so the repeats never change pitch (a knob still turning queues the
/// newest time for when the fade ends). Analog and tape: one head whose position glides to the new
/// time (a one-pole, 150 ms), which bends the pitch of the repeats like a BBD or tape delay. The read
/// head moves at most 0.25 samples per sample, so the pitch never bends more than a ratio of 0.75 to
/// 1.25 (about -5 to +4 semitones): a big jump glides over longer instead of diving octaves.
///
/// Characters. Digital: a soft limiter that leaves normal levels untouched (linear below 0.7) and only
/// bounds runaway feedback. Analog (BBD): an extra 3.5 kHz low-pass per pass and tanh saturation, so
/// repeats darken and thicken. Tape: an extra 5 kHz low-pass, gentler saturation, and wow (0.5 Hz) and
/// flutter (6 Hz) on the read position.
///
/// Stereo layouts. Stereo: each side its own loop, the right one `offset` longer. Ping-pong: the input
/// summed to mono goes into the left line only, and each side feeds the other, so repeats alternate
/// left, right, left. Dual: each side its own time.
///
/// Feedback up to 110% for deliberate self-oscillation; the saturation keeps it bounded.
///
/// Ducking: an envelope follower on the input (5 ms attack, 100 ms release) pulls the wet down by up to
/// `duckDb` while you play: by duckDb x min(1, envelope / -20 dBFS), so fully at normal playing levels,
/// and fading out within a few hundred milliseconds of stopping, so the repeats bloom in the gaps.
///
/// Spillover: handles its own bypass. Bypassed, its input fades out over 10 ms and the dry comes up to
/// unity, while the repeats already in the lines keep going (feedback capped at 99% so even a
/// self-oscillating loop dies away). Zero latency: the dry is never delayed.
class Delay : public Block
{
public:
    enum class Mode
    {
        digital,
        analog,
        tape
    };

    enum class StereoMode
    {
        stereo,
        pingPong,
        dual
    };

    struct Settings
    {
        Mode mode = Mode::digital;
        StereoMode stereoMode = StereoMode::stereo;
        float timeMs = 375.0f;      // left (and both, except in dual)
        float rightTimeMs = 500.0f; // dual mode's right side
        float offsetMs = 0.0f;      // stereo mode: the right side this much longer
        float feedback = 0.35f;     // 0 to 1.1
        float lowCutHz = 120.0f;    // in the loop; 20 Hz or less switches it off
        float highCutHz = 6000.0f;  // in the loop; 20 kHz or more switches it off
        float modDepthMs = 0.0f;    // modulation of the repeats
        float modRateHz = 0.6f;
        float duckDb = 6.0f;        // 0 = no ducking
        float mix = 0.25f;          // 0 dry, 1 wet
    };

    static constexpr double maxSeconds = 4.0;
    static constexpr double minMs = 1.0;
    static constexpr double headFadeSeconds = 0.050;
    static constexpr double glideSeconds = 0.150;
    static constexpr double maxGlideRate = 0.25; // samples of read-head movement per sample
    static constexpr double duckReference = 0.1; // -20 dBFS: full ducking at or above it
    static constexpr double bypassFadeSeconds = 0.010;
    static constexpr double maxFeedback = 1.1;

    /// The soft limiter used in digital mode: identity below 0.7, then a tanh knee that never exceeds 1,
    /// with matching slope at the joint.
    static float softLimit (float x) noexcept;

    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }
    bool handlesOwnBypass() const override { return true; }
    void setBypassed (bool shouldBeBypassed) override;

    /// For tests and the processor: whether the block is currently doing work (false once bypassed and
    /// its repeats have died away).
    bool isActive() const noexcept { return ! bypassed || ! tailSilent; }

private:
    struct Head
    {
        double current = 0.0, next = 0.0, pending = 0.0; // read positions in samples
        int fadePosition = 0;
        bool fading = false;
        double glided = 0.0; // analog and tape
    };

    double timeSamples (float ms) const noexcept;
    float readHead (int channel, Head& head, double target, double modulation) noexcept;
    float colourAndSaturate (int channel, float x) noexcept;
    void designLoopFilters();
    void designColour();

    double sampleRate = 48000.0;
    Settings settings;
    std::array<DelayLine, 2> lines;
    std::array<Head, 2> heads;
    std::array<Svf, 2> lowCut, highCut, colour;
    Lfo modulation { 11 }, wow { 12 }, flutter { 13 };
    int headFadeLength = 2400;
    double glideCoefficient = 0.0;

    juce::SmoothedValue<float> feedback { 0.35f }, dryGain { 1.0f }, wetGain { 0.0f }, inputGain { 1.0f };
    double envelope = 0.0, envelopeAttack = 0.0, envelopeRelease = 0.0;
    juce::SmoothedValue<float> duckDb { 6.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> lowCutHz { 120.0f }, highCutHz { 6000.0f };
    int samplesUntilUpdate = 0;
    bool lowCutOff = false, highCutOff = false;

    bool bypassed = false;
    bool tailSilent = true;
    int silentSamples = 0;
};

} // namespace ampsim

#pragma once

#include "Block.h"
#include "CabIR.h"
#include "SwitchableDelay.h"
#include "Svf.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace ampsim
{

/// The cab section (BUILD_PLAN "Cab"): two close mics and a room mic, each an IR, mixed to stereo,
/// then a low cut and a high cut. This is the chain's mono-to-stereo point.
///
///   close mic 1 ─ delay ─ level, polarity, pan ─┐
///   close mic 2 ─ delay ─ level, polarity, pan ─┼─► sum ─► low cut ─► high cut ─► L, R
///   pre-delay ─ room mic (true stereo) ─ level ─┘
///
/// Panning is constant power: a mic at pan p in [-1, 1] sends sqrt(2) cos(theta) to the left and
/// sqrt(2) sin(theta) to the right, theta = (p + 1) pi/4, so its loudness summed over both sides
/// doesn't change as it moves. The sqrt(2) puts a centred mic at unity on each side, the same as the
/// cab's passthrough and the chain's own mono-to-stereo copy when the cab is bypassed, so none of
/// those switches change the level. Hard-panned, a mic is +3 dB on its side.
///
/// Auto alignment (on by default): when the close mics' IRs change, they're cross-correlated to find
/// the offset and polarity at which they line up best, and the earlier mic is delayed (and mic 2
/// flipped if needed) so they add instead of comb filtering. The room mic is left alone: its offset is
/// what gives depth.
///
/// A mic with no IR contributes nothing. With no IRs at all, the cab passes the amp straight through.
/// Every level, pan, polarity, delay, and on/off change ramps or crossfades, so nothing clicks.
class Cab : public Block
{
public:
    static constexpr int numCloseMics = 2;
    static constexpr int maxMicDelaySamples = 200;   // about 1.4 m of mic distance at 7 mm per sample
    static constexpr double maxRoomPreDelayMs = 100.0;
    static constexpr int alignmentMaxLag = 200;      // search +-200 samples when aligning
    static constexpr double fadeSeconds = 0.010;     // delay and on/off changes
    static constexpr double gainRampSeconds = 0.020; // level, pan, polarity, mute

    struct CloseMicSettings
    {
        float levelDb = 0.0f;
        float pan = 0.0f; // -1 left, 0 centre, +1 right
        bool invert = false;
        int delaySamples = 0;
        bool mute = false;
    };

    struct RoomSettings
    {
        float levelDb = 0.0f;
        float preDelayMs = 0.0f;
        bool mute = false;
    };

    enum class Slope
    {
        db12,
        db24
    };

    struct CutSettings
    {
        bool lowCutOn = false;
        float lowCutHz = 80.0f;
        Slope lowCutSlope = Slope::db12;
        bool highCutOn = false;
        float highCutHz = 8000.0f;
        Slope highCutSlope = Slope::db12;
    };

    /// The close mics' alignment, found by cross-correlating their IRs.
    struct Alignment
    {
        bool valid = false;  // false until both close mics have IRs
        int delayMic1 = 0;   // samples added to mic 1 (when mic 2 arrives later)
        int delayMic2 = 0;   // samples added to mic 2 (when mic 2 arrives earlier)
        bool invertMic2 = false;
        float correlation = 0.0f; // normalized peak correlation, 0 to 1
    };

    Cab() = default;

    /// Loader thread. Loads a close mic's IR, then re-aligns the close mics.
    CabIR::LoadResult loadCloseMic (int index, const juce::File& file, CabIR::Channel channel = CabIR::Channel::left);
    CabIR::LoadResult loadCloseMicSamples (int index, juce::AudioBuffer<float> samples, double sampleRate, const juce::String& name);
    CabIR::LoadResult loadRoom (const juce::File& file);
    CabIR::LoadResult loadRoomSamples (juce::AudioBuffer<float> samples, double sampleRate, const juce::String& name);

    /// Any thread: the alignment currently in effect.
    Alignment getAlignment() const;

    /// The offset (positive = h2 arrives later than h1) and polarity at which h2 best matches h1,
    /// searched within +-maxLag samples over the IRs' first 8192 samples. Normalized peak correlation
    /// in [0, 1] says how well they match.
    static Alignment computeAlignment (const std::vector<float>& h1, const std::vector<float>& h2, int maxLag);

    /// Any non-audio thread.
    void collectGarbage();

    // Audio thread, once per buffer.
    void setCloseMic (int index, const CloseMicSettings& settings);
    void setRoom (const RoomSettings& settings);
    void setAutoAlign (bool shouldAlign) noexcept { autoAlign = shouldAlign; }
    void setCuts (const CutSettings& settings);

    /// Audio thread, for tests.
    bool hasAnyImpulseResponse() const noexcept;
    CabIR& closeMic (int index) { return closeMics[(size_t) index]; }
    CabIR& roomMic() { return room; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }

private:
    struct MicState
    {
        juce::SmoothedValue<float> gainLeft, gainRight; // level x polarity x pan x mute, ramped linearly
        SwitchableDelay delay;
        juce::SmoothedValue<float> presence;            // fades a mic in once its first IR is really running
        int samplesSinceReady = 0;
    };

    /// One cut filter (low or high) on both channels: two SVF sections each (12 dB/oct uses one, 24
    /// uses both, as a 4th-order Butterworth), plus an on/off crossfade.
    struct CutFilter
    {
        std::array<std::array<Svf, 2>, 2> sections; // [channel][section]
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> frequency { 1000.0f };
        juce::SmoothedValue<float> wet { 0.0f };    // 0 off, 1 on
        Slope slope = Slope::db12;
        bool on = false;
        bool needsReset = false;
        bool coefficientsDirty = true;
        int samplesUntilUpdate = 0;
    };

    void updateCutCoefficients (CutFilter& cut, Svf::Type type);
    void processCut (CutFilter& cut, Svf::Type type, float* left, float* right, int numSamples);
    void realign();

    void applyTargets();
    void updatePresence (MicState& state, const CabIR& mic, int numSamples);

    std::array<CabIR, numCloseMics> closeMics;
    CabIR room { CabIR::Options { true, 512 } }; // true stereo, head/tail partitioning for long rooms

    std::array<MicState, numCloseMics> micStates;
    MicState roomState;                          // its gains, pre-delay, and presence
    std::array<CloseMicSettings, numCloseMics> micSettings;
    RoomSettings roomSettings;
    CutSettings cutSettings;
    CutFilter lowCut, highCut;
    juce::SmoothedValue<float> passthrough;      // 1 while no mic is playing: the amp goes straight through
    bool autoAlign = true;
    int settleSamples = 2880;                    // 60 ms: JUCE's 50 ms engine crossfade plus margin

    // The alignment, written by the loader thread and read by the audio thread, packed into one atomic
    // so the audio thread never sees half of an update: bits 0-15 mic 1 delay, 16-31 mic 2 delay,
    // bit 32 invert mic 2, bit 33 valid. The correlation is only for display.
    std::atomic<std::uint64_t> packedAlignment { 0 };
    std::atomic<float> alignedCorrelation { 0.0f };
    std::mutex realignMutex; // loader threads only: one realignment at a time

    juce::AudioBuffer<float> micBuffers;  // 2 channels per close mic, then 2 for the room
    juce::AudioBuffer<float> dryBuffer;   // for the cut filters' on/off crossfades
    double sampleRate = 48000.0;
};

} // namespace ampsim

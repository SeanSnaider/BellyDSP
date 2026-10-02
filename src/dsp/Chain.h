#pragma once

#include "AmpSection.h"
#include "Block.h"
#include "Cab.h"
#include "Gain.h"

#include <array>
#include <vector>

namespace ampsim
{

/// The signal chain. It owns the blocks, snapshots the DI, makes the mono-to-stereo copy, and
/// crossfades bypass. It never special-cases a block (BUILD_PLAN "Block design", decisions 2 to 6).
///
/// Blocks are typed members in signal-chain order (decision 6), so the processor can call
/// block-specific setters like inputGain.setGainDecibels(). The generic logic walks them through
/// inOrder() as Block pointers.
class Chain
{
public:
    Gain inputGain { false };
    AmpSection amp;
    Cab cab;
    Gain outputGain { true };

    enum class Slot : size_t
    {
        inputGain,
        amp,
        cab,
        outputGain
    };

    static constexpr size_t numSlots = 4;
    static constexpr double bypassFadeSeconds = 0.010;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /// Audio thread. On entry channel 0 holds the guitar. On return channels 0 and 1 hold the stereo
    /// output. The block needs at least two channels and at most maxBlockSize samples.
    void process (juce::dsp::AudioBlock<float> io);

    /// Audio thread, once per buffer before process(). Bypass crossfades over 10 ms (decision 4).
    void setBypassed (Slot slot, bool shouldBeBypassed);

    /// Audio thread, for tests: whether the slot is fully bypassed and being skipped.
    bool isFullyBypassed (Slot slot) const;

    int latencySamples() const;

    /// For tests: the DI snapshot taken at the start of the last process() call.
    const std::vector<float>& lastDISnapshot() const { return di; }

private:
    std::array<Block*, numSlots> inOrder() { return { &inputGain, &amp, &cab, &outputGain }; }
    std::array<const Block*, numSlots> inOrder() const { return { &inputGain, &amp, &cab, &outputGain }; }

    struct BypassState
    {
        bool bypassed = false;                   // the target: where the fade is heading
        juce::SmoothedValue<float> wet { 1.0f }; // the fade itself: 1 = block on, 0 = bypassed
        bool resetBeforeNextRun = false;

        bool fullyOff() const { return bypassed && ! wet.isSmoothing(); }
    };

    std::array<BypassState, numSlots> bypass;
    std::vector<float> di;        // decision 2: the clean guitar, snapshotted every buffer
    juce::AudioBuffer<float> dry; // a block's input, kept while its bypass crossfades
};

} // namespace ampsim

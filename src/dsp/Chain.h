#pragma once

#include "AmpSection.h"
#include "Block.h"
#include "Boost.h"
#include "Cab.h"
#include "Chorus.h"
#include "Compressor.h"
#include "Delay.h"
#include "Reverb.h"
#include "Equalizer.h"
#include "Gain.h"
#include "LinkedGates.h"
#include "Overdrive.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ampsim
{

/// The signal chain. It owns the blocks, snapshots the DI, makes the mono-to-stereo copy, crossfades
/// bypass, and runs the two reorderable sections. It never special-cases a block (BUILD_PLAN "Block
/// design", decisions 2 to 6).
///
///   input gain ─► PRE FX (mono, reorderable) ─► amp ─► Gate B ─► cab (mono to stereo) ─► POST FX
///   (stereo, reorderable) ─► output level
///
/// Blocks are typed members (decision 6), so the processor can call block-specific setters. The
/// generic logic reaches them through blockFor(). A section's order is an array of slots, changed by
/// a request from the message thread: the audio thread dips the section to its own input over 10 ms,
/// swaps the order at the bottom of the dip (where the swap can't be heard), and fades back, so a
/// reorder never clicks and never allocates (Foundation decisions, "Reorderable chain").
class Chain
{
public:
    Gain inputGain { false };
    Gate gateA;
    Compressor preCompressor { false };
    Boost boost;
    Overdrive overdrive;
    Equalizer preEq { false };
    AmpSection amp;
    GateB gateB { gateA }; // follows Gate A when linked (LinkedGates.h)
    Cab cab;
    Equalizer postEq { true };
    Compressor postCompressor { true };
    Chorus chorus;
    Delay delay;
    Reverb reverb;
    Gain outputGain { true };

    enum class Slot : size_t
    {
        inputGain,
        gateA,
        preCompressor,
        boost,
        overdrive,
        preEq,
        amp,
        gateB,
        cab,
        postEq,
        postCompressor,
        chorus,
        delay,
        reverb,
        outputGain,
        count
    };

    static constexpr size_t numSlots = (size_t) Slot::count;
    static constexpr double bypassFadeSeconds = 0.010;
    static constexpr double reorderFadeSeconds = 0.010;

    enum class Section
    {
        pre,
        post
    };

    static constexpr size_t maxSectionSize = 8;

    /// A section's blocks in their default order (BUILD_PLAN "Default signal chain"). Built once at
    /// startup, so the audio thread can read it without allocating.
    static const std::vector<Slot>& defaultOrder (Section section);

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /// Audio thread. On entry channel 0 holds the guitar. On return channels 0 and 1 hold the stereo
    /// output. The block needs at least two channels and at most maxBlockSize samples.
    void process (juce::dsp::AudioBlock<float> io);

    /// Audio thread, once per buffer before process(). Bypass crossfades over 10 ms (decision 4).
    /// Effects (the pre and post FX) start bypassed; the amp, cab, and gains start on.
    void setBypassed (Slot slot, bool shouldBeBypassed);

    /// Audio thread, for tests: whether the slot is fully bypassed and being skipped.
    bool isFullyBypassed (Slot slot) const;

    /// Any thread: asks for a new order. It must be a permutation of the section's default order;
    /// anything else is refused (returns false). The audio thread applies it with a dip.
    bool requestOrder (Section section, const std::vector<Slot>& order);

    /// Any thread: the most recently requested order.
    std::vector<Slot> getRequestedOrder (Section section) const;

    /// The thread that calls process(), for tests: the order in effect, and whether a reorder dip is running.
    std::vector<Slot> getAppliedOrder (Section section) const;
    bool isReordering (Section section) const;

    int latencySamples() const;

    /// For tests: the DI snapshot taken at the start of the last process() call.
    const std::vector<float>& lastDISnapshot() const { return di; }

    Block& blockFor (Slot slot);
    const Block& blockFor (Slot slot) const;

private:
    struct BypassState
    {
        bool bypassed = false;                   // the target: where the fade is heading
        juce::SmoothedValue<float> wet { 1.0f }; // the fade itself: 1 = block on, 0 = bypassed
        bool resetBeforeNextRun = false;

        // Keeps running while fully bypassed, on a copy of its input, so its state stays live: Gate A
        // (a linked Gate B follows its curve, and Learn works with it off) and the drive blocks (a
        // circuit switched on cold clips around the wrong bias while its coupling capacitors charge,
        // which a 10 ms fade doesn't hide). Costs the block's CPU while it's off; switching it on needs
        // no reset.
        bool keepRunning = false;

        bool fullyOff() const { return bypassed && ! wet.isSmoothing(); }
    };

    struct SectionState
    {
        std::array<Slot, maxSectionSize> order {};
        size_t size = 0;
        std::atomic<std::uint64_t> requested { 0 }; // packed: 4 bits per position, the index into defaultOrder()
        std::uint64_t applied = 0;
        bool swapPending = false;
        juce::SmoothedValue<float> wet { 1.0f };    // 1 = the section's output; dips to 0 (its input) to reorder
    };

    static std::uint64_t pack (Section section, const std::vector<Slot>& order);
    static void unpack (Section section, std::uint64_t packed, SectionState& state);

    void runBlock (Slot slot, juce::dsp::AudioBlock<float>& io, const BlockContext& context, bool& stereoCopied);
    void runSection (Section section, juce::dsp::AudioBlock<float>& io, const BlockContext& context, bool& stereoCopied);

    std::array<BypassState, numSlots> bypass;
    std::array<SectionState, 2> sections;
    std::vector<float> di;        // decision 2: the clean guitar, snapshotted every buffer
    juce::AudioBuffer<float> dry; // a block's input, kept while its bypass crossfades
    juce::AudioBuffer<float> sectionDry; // a section's input, kept while it reorders

public:
    Chain();
};

} // namespace ampsim

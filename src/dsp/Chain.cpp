#include "Chain.h"

#include <algorithm>

namespace ampsim
{

namespace
{
/// Decision 3: the one mono-to-stereo copy, made by the chain rather than by each block.
void copyLeftToRight (juce::dsp::AudioBlock<float>& io)
{
    io.getSingleChannelBlock (1).copyFrom (io.getSingleChannelBlock (0));
}

} // namespace

Chain::Chain()
{
    for (auto section : { Section::pre, Section::post })
    {
        auto& state = sections[(size_t) section];
        const auto packed = pack (section, defaultOrder (section));
        state.requested = packed;
        state.applied = packed;
        unpack (section, packed, state);

        // Effects start switched off, so a bare chain is just gain, amp, cab, and level; the
        // processor switches them on from their parameters.
        for (auto slot : defaultOrder (section))
            setBypassed (slot, true);
    }
}

const std::vector<Chain::Slot>& Chain::defaultOrder (Section section)
{
    // Built on first use, which is always the Chain constructor, never the audio thread; after that,
    // reading them never allocates.
    static const std::vector<Slot> pre { Slot::preCompressor, Slot::preEq };
    static const std::vector<Slot> post { Slot::postEq, Slot::postCompressor };
    return section == Section::pre ? pre : post;
}

std::uint64_t Chain::pack (Section section, const std::vector<Slot>& order)
{
    const auto& defaults = defaultOrder (section);
    std::uint64_t packed = 0;
    for (size_t i = 0; i < order.size(); ++i)
    {
        const auto index = (std::uint64_t) (std::find (defaults.begin(), defaults.end(), order[i]) - defaults.begin());
        packed |= index << (4 * i);
    }
    return packed;
}

void Chain::unpack (Section section, std::uint64_t packed, SectionState& state)
{
    const auto& defaults = defaultOrder (section);
    state.size = defaults.size();
    for (size_t i = 0; i < state.size; ++i)
        state.order[i] = defaults[(size_t) ((packed >> (4 * i)) & 0xf)];
}

bool Chain::requestOrder (Section section, const std::vector<Slot>& order)
{
    auto sorted = order, defaults = defaultOrder (section);
    std::sort (sorted.begin(), sorted.end());
    std::sort (defaults.begin(), defaults.end());
    if (sorted != defaults)
        return false; // not a permutation of this section's blocks

    sections[(size_t) section].requested.store (pack (section, order), std::memory_order_release);
    return true;
}

std::vector<Chain::Slot> Chain::getRequestedOrder (Section section) const
{
    SectionState copy;
    unpack (section, sections[(size_t) section].requested.load (std::memory_order_acquire), copy);
    return { copy.order.begin(), copy.order.begin() + (long) copy.size };
}

std::vector<Chain::Slot> Chain::getAppliedOrder (Section section) const
{
    const auto& state = sections[(size_t) section];
    return { state.order.begin(), state.order.begin() + (long) state.size };
}

bool Chain::isReordering (Section section) const
{
    const auto& state = sections[(size_t) section];
    return state.swapPending || state.wet.isSmoothing();
}

Block& Chain::blockFor (Slot slot)
{
    return const_cast<Block&> (std::as_const (*this).blockFor (slot));
}

const Block& Chain::blockFor (Slot slot) const
{
    switch (slot)
    {
        case Slot::inputGain:      return inputGain;
        case Slot::preCompressor:  return preCompressor;
        case Slot::preEq:          return preEq;
        case Slot::amp:            return amp;
        case Slot::cab:            return cab;
        case Slot::postEq:         return postEq;
        case Slot::postCompressor: return postCompressor;
        case Slot::outputGain:     return outputGain;
        case Slot::count:          break;
    }
    jassertfalse;
    return inputGain;
}

void Chain::prepare (double sampleRate, int maxBlockSize)
{
    di.assign ((size_t) maxBlockSize, 0.0f);
    dry.setSize (2, maxBlockSize);
    sectionDry.setSize (2, maxBlockSize);

    for (auto& state : bypass)
        state.wet.reset (sampleRate, bypassFadeSeconds); // also snaps to the current target

    // Start in the requested order with no dip: nothing is playing yet.
    for (auto section : { Section::pre, Section::post })
    {
        auto& state = sections[(size_t) section];
        state.applied = state.requested.load (std::memory_order_acquire);
        unpack (section, state.applied, state);
        state.swapPending = false;
        state.wet.reset (sampleRate, reorderFadeSeconds);
        state.wet.setCurrentAndTargetValue (1.0f);
    }

    // Prepare everything in signal order, checking decision 3: no mono block after a stereo one.
    bool seenStereo = false;
    for (size_t i = 0; i < numSlots; ++i)
    {
        auto& block = blockFor ((Slot) i);
        block.prepare (sampleRate, maxBlockSize);
        jassert (! (seenStereo && ! block.isStereo()));
        seenStereo = seenStereo || block.isStereo();
    }
}

void Chain::reset()
{
    for (size_t i = 0; i < numSlots; ++i)
        blockFor ((Slot) i).reset();

    for (auto& state : bypass)
    {
        state.wet.setCurrentAndTargetValue (state.bypassed ? 0.0f : 1.0f);
        state.resetBeforeNextRun = false;
    }
}

void Chain::runBlock (Slot slot, juce::dsp::AudioBlock<float>& io, const BlockContext& context, bool& stereoCopied)
{
    auto& block = blockFor (slot);
    auto& state = bypass[(size_t) slot];
    const auto numSamples = io.getNumSamples();

    // Fully bypassed: don't call the block at all, which also saves its CPU.
    if (state.fullyOff())
        return;

    if (block.isStereo() && ! stereoCopied)
    {
        copyLeftToRight (io);
        stereoCopied = true;
    }

    if (state.resetBeforeNextRun)
    {
        block.reset();
        state.resetBeforeNextRun = false;
    }

    const size_t numChannels = block.isStereo() ? 2 : 1;
    auto view = io.getSubsetChannelBlock (0, numChannels);

    if (! state.wet.isSmoothing())
    {
        block.process (view, context);
        return;
    }

    // Mid-crossfade: keep the dry input, run the block, then blend. The fade is linear because dry
    // and wet are the same signal with and without one block, so they're usually strongly
    // correlated, and for correlated signals a linear fade holds the level steady (an equal-power
    // fade would bump it by up to 3 dB in the middle).
    auto dryView = juce::dsp::AudioBlock<float> (dry).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    dryView.copyFrom (view);

    block.process (view, context);

    for (size_t n = 0; n < numSamples; ++n)
    {
        const auto w = state.wet.getNextValue(); // one fade position per sample, shared by both channels

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* out = view.getChannelPointer (ch);
            const auto d = dryView.getChannelPointer (ch)[n];
            out[n] = d + w * (out[n] - d);
        }
    }
}

void Chain::runSection (Section section, juce::dsp::AudioBlock<float>& io, const BlockContext& context, bool& stereoCopied)
{
    auto& state = sections[(size_t) section];
    const auto numSamples = io.getNumSamples();
    const size_t numChannels = section == Section::pre ? 1 : 2;

    // A new order: dip to the section's input, swap at the bottom, fade back.
    const auto requested = state.requested.load (std::memory_order_acquire);
    if (requested != state.applied && ! state.swapPending)
    {
        state.swapPending = true;
        state.wet.setTargetValue (0.0f);
    }
    if (state.swapPending && ! state.wet.isSmoothing())
    {
        state.applied = requested;
        unpack (section, requested, state);
        state.swapPending = false;
        state.wet.setTargetValue (1.0f);
    }

    const bool fading = state.wet.isSmoothing() || state.wet.getCurrentValue() < 1.0f;
    auto dryView = juce::dsp::AudioBlock<float> (sectionDry).getSubBlock (0, numSamples).getSubsetChannelBlock (0, numChannels);
    if (fading)
        dryView.copyFrom (io.getSubsetChannelBlock (0, numChannels));

    for (size_t i = 0; i < state.size; ++i)
        runBlock (state.order[i], io, context, stereoCopied);

    if (fading)
    {
        // Linear, like bypass: the section's input and output are the same signal through a few blocks.
        for (size_t n = 0; n < numSamples; ++n)
        {
            const auto w = state.wet.getNextValue();
            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                auto* out = io.getChannelPointer (ch);
                const auto d = dryView.getChannelPointer (ch)[n];
                out[n] = d + w * (out[n] - d);
            }
        }
    }
}

void Chain::process (juce::dsp::AudioBlock<float> io)
{
    jassert (io.getNumChannels() >= 2);
    const auto numSamples = io.getNumSamples();
    jassert (numSamples <= di.size());

    // Decision 2: snapshot the clean guitar before any block touches the buffer.
    std::copy (io.getChannelPointer (0), io.getChannelPointer (0) + numSamples, di.begin());
    const BlockContext context { di.data(), (int) numSamples };

    bool stereoCopied = false;
    runBlock (Slot::inputGain, io, context, stereoCopied);
    runSection (Section::pre, io, context, stereoCopied);
    runBlock (Slot::amp, io, context, stereoCopied);
    runBlock (Slot::cab, io, context, stereoCopied);

    // The cab is the mono-to-stereo point; if it's bypassed, the copy still happens before post FX.
    if (! stereoCopied)
    {
        copyLeftToRight (io);
        stereoCopied = true;
    }

    runSection (Section::post, io, context, stereoCopied);
    runBlock (Slot::outputGain, io, context, stereoCopied);
}

void Chain::setBypassed (Slot slot, bool shouldBeBypassed)
{
    // A block with spillover handles its own bypass, so its tail can ring out; the chain keeps
    // running it.
    if (auto& block = blockFor (slot); block.handlesOwnBypass())
    {
        block.setBypassed (shouldBeBypassed);
        return;
    }

    auto& state = bypass[(size_t) slot];

    if (state.bypassed == shouldBeBypassed)
        return;

    // Re-enabling a block that was fully off: its state is stale (filter memory or a tail from
    // whenever it was switched off), so clear it before it runs again (decision 4). A block that's
    // mid-fade never stopped running, so its state is still live and there's nothing to clear.
    if (! shouldBeBypassed && state.fullyOff())
        state.resetBeforeNextRun = true;

    state.bypassed = shouldBeBypassed;
    state.wet.setTargetValue (shouldBeBypassed ? 0.0f : 1.0f);
}

bool Chain::isFullyBypassed (Slot slot) const
{
    return bypass[(size_t) slot].fullyOff();
}

int Chain::latencySamples() const
{
    int total = 0;
    for (size_t i = 0; i < numSlots; ++i)
        total += blockFor ((Slot) i).latencySamples();
    return total;
}

} // namespace ampsim

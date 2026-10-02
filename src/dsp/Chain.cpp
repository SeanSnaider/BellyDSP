#include "Chain.h"

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

void Chain::prepare (double sampleRate, int maxBlockSize)
{
    di.assign ((size_t) maxBlockSize, 0.0f);
    dry.setSize (2, maxBlockSize);

    for (auto& state : bypass)
        state.wet.reset (sampleRate, bypassFadeSeconds); // also snaps to the current target

    bool seenStereo = false;

    for (auto* block : inOrder())
    {
        block->prepare (sampleRate, maxBlockSize);

        // Decision 3: no mono block after a stereo one. The order is fixed at compile time, so
        // this catches a bad edit to inOrder() in debug builds.
        jassert (! (seenStereo && ! block->isStereo()));
        seenStereo = seenStereo || block->isStereo();
    }
}

void Chain::reset()
{
    for (auto* block : inOrder())
        block->reset();

    for (auto& state : bypass)
    {
        state.wet.setCurrentAndTargetValue (state.bypassed ? 0.0f : 1.0f);
        state.resetBeforeNextRun = false;
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
    auto blocks = inOrder();

    for (size_t i = 0; i < numSlots; ++i)
    {
        auto& block = *blocks[i];
        auto& state = bypass[i];

        // Fully bypassed: don't call the block at all, which also saves its CPU.
        if (state.fullyOff())
            continue;

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
            continue;
        }

        // Mid-crossfade: keep the dry input, run the block, then blend. The fade is linear because
        // dry and wet are the same signal with and without one block, so they're usually strongly
        // correlated, and for correlated signals a linear fade holds the level steady (an
        // equal-power fade would bump it by up to 3 dB in the middle).
        auto dryView = juce::dsp::AudioBlock<float> (dry)
                           .getSubBlock (0, numSamples)
                           .getSubsetChannelBlock (0, numChannels);
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

    if (! stereoCopied)
        copyLeftToRight (io);
}

void Chain::setBypassed (Slot slot, bool shouldBeBypassed)
{
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

    for (const auto* block : inOrder())
        total += block->latencySamples();

    return total;
}

} // namespace ampsim

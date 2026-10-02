#include "LinkedGates.h"

#include <algorithm>

namespace ampsim
{

void GateA::prepare (double sampleRate, int maxBlockSize)
{
    gate.prepare (sampleRate, maxBlockSize);
    scratch.assign ((size_t) maxBlockSize, 0.0f);
    wet.reset (sampleRate, bypassFadeSeconds);
    wet.setCurrentAndTargetValue (bypassed ? 0.0f : 1.0f);
}

void GateA::reset()
{
    gate.reset();
    wet.setCurrentAndTargetValue (bypassed ? 0.0f : 1.0f);
}

void GateA::setBypassed (bool shouldBeBypassed)
{
    bypassed = shouldBeBypassed;
    wet.setTargetValue (shouldBeBypassed ? 0.0f : 1.0f);
}

void GateA::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    const auto numSamples = (int) block.getNumSamples();
    auto* audio = block.getChannelPointer (0);

    if (! wet.isSmoothing() && ! bypassed)
    {
        gate.process (block, context);
        return;
    }

    std::copy (audio, audio + numSamples, scratch.begin());

    if (! wet.isSmoothing())
    {
        // Off: the gate runs on the copy, and the audio is left exactly as it came in.
        float* channels[] = { scratch.data() };
        gate.process (juce::dsp::AudioBlock<float> (channels, 1, (size_t) numSamples), context);
        return;
    }

    // Fading: gate the audio, then blend back toward the input kept in the copy.
    gate.process (block, context);
    for (int n = 0; n < numSamples; ++n)
        audio[n] = scratch[(size_t) n] + wet.getNextValue() * (audio[n] - scratch[(size_t) n]);
}

void GateB::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    // Gate A runs every buffer (it detects even while off), so its curve is this buffer's. The length
    // check only guards against a caller that skipped it.
    if (linked && leader.getGainCurveLength() == (int) block.getNumSamples())
        gate.processWithGain (block, leader.getGainCurve());
    else
        gate.process (block, context);
}

} // namespace ampsim

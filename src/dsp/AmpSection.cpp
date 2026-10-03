// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AmpSection.h"

namespace ampsim
{

void AmpSection::selectSlot (int index)
{
    index = juce::jlimit (0, numSlots - 1, index);

    if (index == selected)
        return;

    selected = index;

    for (int s = 0; s < numSlots; ++s)
        position[(size_t) s].setTargetValue (s == selected ? 1.0f : 0.0f);
}

bool AmpSection::isSwitching() const noexcept
{
    for (const auto& p : position)
        if (p.isSmoothing())
            return true;

    return false;
}

bool AmpSection::isLoadingModel() const noexcept
{
    for (const auto& s : slots)
        if (s.model.isSwitching())
            return true;

    return false;
}

void AmpSection::prepare (double sampleRate, int maxBlockSize)
{
    slotOutputs.setSize (numSlots, maxBlockSize);

    for (int s = 0; s < numSlots; ++s)
    {
        auto& slot = slots[(size_t) s];
        slot.inputTrim.prepare (sampleRate, maxBlockSize);
        slot.model.prepare (sampleRate, maxBlockSize);
        slot.tone.prepare (sampleRate);
        slot.outputTrim.prepare (sampleRate, maxBlockSize);

        position[(size_t) s].reset (sampleRate, switchSeconds);
        position[(size_t) s].setCurrentAndTargetValue (s == selected ? 1.0f : 0.0f);
    }
}

void AmpSection::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    const auto numSamples = block.getNumSamples();
    const auto* input = block.getChannelPointer (0);
    auto outputs = juce::dsp::AudioBlock<float> (slotOutputs).getSubBlock (0, numSamples);

    // Every slot runs every buffer, selected or not, so its model's history stays current.
    for (size_t s = 0; s < (size_t) numSlots; ++s)
    {
        auto& slot = slots[s];
        auto channel = outputs.getSingleChannelBlock (s);
        std::copy (input, input + numSamples, channel.getChannelPointer (0));

        slot.inputTrim.process (channel, context);
        slot.model.process (channel, context);
        slot.tone.process (channel.getChannelPointer (0), (int) numSamples);
        slot.outputTrim.process (channel, context);
    }

    auto* out = block.getChannelPointer (0);

    if (! isSwitching())
    {
        std::copy (outputs.getChannelPointer ((size_t) selected),
                   outputs.getChannelPointer ((size_t) selected) + numSamples, out);
        return;
    }

    for (size_t n = 0; n < numSamples; ++n)
    {
        float sum = 0.0f;

        for (size_t s = 0; s < (size_t) numSlots; ++s)
        {
            const auto p = position[s].getNextValue();
            if (p > 0.0f)
                sum += std::sin (juce::MathConstants<float>::halfPi * p) * outputs.getChannelPointer (s)[n];
        }

        out[n] = sum;
    }
}

void AmpSection::reset()
{
    for (int s = 0; s < numSlots; ++s)
    {
        auto& slot = slots[(size_t) s];
        slot.inputTrim.reset();
        slot.model.reset();
        slot.tone.reset();
        slot.outputTrim.reset();
        position[(size_t) s].setCurrentAndTargetValue (s == selected ? 1.0f : 0.0f);
    }
}

} // namespace ampsim

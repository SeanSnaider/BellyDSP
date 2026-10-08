// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AmpSection.h"

namespace ampsim
{

void AmpSection::selectAmp (int index)
{
    selected = juce::jlimit (0, numAmps - 1, index);
}

bool AmpSection::isFading() const noexcept
{
    for (const auto& s : state)
        if (s.position.isSmoothing())
            return true;

    return false;
}

int AmpSection::getRunningAmps() const noexcept
{
    int n = 0;
    for (const auto& s : state)
        n += s.running ? 1 : 0;
    return n;
}

int AmpSection::getRunningModels() const noexcept
{
    int n = 0;
    for (int a = 0; a < numAmps; ++a)
        if (state[(size_t) a].running)
            n += amps[(size_t) a].model.getRunningSteps();
    return n;
}

bool AmpSection::isLoadingModel() const noexcept
{
    for (const auto& a : amps)
        if (a.model.isSwitching())
            return true;

    return false;
}

int AmpSection::warmupSamplesFor (int index, int blockSize) const noexcept
{
    const auto samples = amps[(size_t) juce::jlimit (0, numAmps - 1, index)].model.getWarmupSamples();
    const auto block = juce::jmax (1, blockSize);
    return (samples + block - 1) / block * block;
}

void AmpSection::prepare (double sampleRate, int maxBlockSize)
{
    ampOutputs.setSize (numAmps, maxBlockSize);
    ampOutputs.clear();

    // prepare() snaps every switch: the selected amp is heard at once, warm (its models were just prewarmed), and
    // nothing else runs.
    heard = selected;

    for (int a = 0; a < numAmps; ++a)
    {
        auto& amp = amps[(size_t) a];
        auto& s = state[(size_t) a];
        amp.model.setGain (amp.inputTrim.getPosition());
        amp.model.setBlend (NamAmp::Blend::exact);
        amp.model.prepare (sampleRate, maxBlockSize);
        amp.tone.prepare (sampleRate);
        amp.outputTrim.prepare (sampleRate, maxBlockSize);

        s.position.reset (sampleRate, switchSeconds);
        s.position.setCurrentAndTargetValue (a == heard ? 1.0f : 0.0f);
        s.running = a == heard;
        s.warmed = a == heard ? maxWarmed : 0;
    }
}

void AmpSection::start (int index) noexcept
{
    // An amp that hasn't been running: its models' history is stale, so it starts over on the live input from
    // here (NamAmp::restart: the Gain at the knob, the steps it needs running), with its tone filters cleared and
    // its knobs snapped (nobody hears it until the warm-up is done, so there's nothing to smooth).
    auto& amp = amps[(size_t) index];
    auto& s = state[(size_t) index];
    amp.model.setGain (amp.inputTrim.getPosition());
    amp.model.setBlend (nearestStepOnly ? NamAmp::Blend::nearest : NamAmp::Blend::exact);
    amp.model.restart();
    amp.tone.reset();
    amp.outputTrim.reset();
    s.running = true;
    s.warmed = 0;
}

void AmpSection::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    const auto numSamples = block.getNumSamples();
    const auto* input = block.getChannelPointer (0);
    auto outputs = juce::dsp::AudioBlock<float> (ampOutputs).getSubBlock (0, numSamples);

    // 1. A switch under way (BUILD_PLAN "Amp switching"). The selected amp starts if it isn't running, and once
    //    it has run for its warm-up the ramps head for it: it fades in over 20 ms, everything else out. Checked at
    //    the start of a buffer, so the warm-up is a whole number of buffers.
    if (selected != heard)
    {
        auto& incoming = state[(size_t) selected];
        if (! incoming.running)
            start (selected);

        if (incoming.warmed >= amps[(size_t) selected].model.getWarmupSamples())
        {
            heard = selected;
            for (int a = 0; a < numAmps; ++a)
                state[(size_t) a].position.setTargetValue (a == heard ? 1.0f : 0.0f);
        }
    }

    // 2. What runs: the amp heard, the one warming up (the newest request), and any still fading out. Anything
    //    else stops: an amp a newer switch overtook while it warmed, and one whose fade has reached 0.
    for (int a = 0; a < numAmps; ++a)
    {
        auto& s = state[(size_t) a];
        if (s.running && a != heard && a != selected && s.position.getCurrentValue() <= 0.0f && ! s.position.isSmoothing())
        {
            s.running = false;
            s.warmed = 0;
        }
    }

    // 3. Run them. The heard amp and the one warming blend their gain sets exactly; one fading out holds what it
    //    plays until it's silent (NamAmp::Blend::hold: nothing audible changes during its fade, and a model it
    //    was warming for a blend it hadn't reached stops).
    for (int a = 0; a < numAmps; ++a)
    {
        auto& s = state[(size_t) a];
        if (! s.running)
            continue;

        auto& amp = amps[(size_t) a];
        auto channel = outputs.getSingleChannelBlock ((size_t) a);
        std::copy (input, input + numSamples, channel.getChannelPointer (0));

        const auto live = nearestStepOnly ? NamAmp::Blend::nearest : NamAmp::Blend::exact;
        amp.model.setBlend (a == heard || a == selected ? live : NamAmp::Blend::hold);
        amp.model.setGain (amp.inputTrim.getPosition());
        amp.model.process (channel, context);
        amp.tone.process (channel.getChannelPointer (0), (int) numSamples);
        amp.outputTrim.process (channel, context);
        s.warmed = (int) juce::jmin ((juce::int64) maxWarmed, (juce::int64) s.warmed + (juce::int64) numSamples);
    }

    // 4. The mix. Settled: the heard amp's output as it is. Fading: sum sin(pi/2 p) of each amp still audible.
    auto* out = block.getChannelPointer (0);

    if (! isFading())
    {
        const auto* src = outputs.getChannelPointer ((size_t) heard);
        std::copy (src, src + numSamples, out);
        return;
    }

    for (size_t n = 0; n < numSamples; ++n)
    {
        float sum = 0.0f;

        for (size_t a = 0; a < (size_t) numAmps; ++a)
        {
            auto& s = state[a];
            if (! s.running)
                continue;
            const auto p = s.position.getNextValue();
            if (p > 0.0f)
                sum += std::sin (juce::MathConstants<float>::halfPi * p) * outputs.getChannelPointer (a)[n];
        }

        out[n] = sum;
    }
}

void AmpSection::reset()
{
    // Real-time safe: finishes any switch at once (the selected amp heard), clears every running amp's tone filters.
    heard = selected;
    for (int a = 0; a < numAmps; ++a)
    {
        auto& amp = amps[(size_t) a];
        auto& s = state[(size_t) a];
        if (s.running || a == heard)
        {
            amp.model.reset();
            amp.tone.reset();
            amp.outputTrim.reset();
        }
        if (a == heard && ! s.running)
            start (a);
        if (a != heard)
        {
            s.running = false;
            s.warmed = 0;
        }
        s.position.setCurrentAndTargetValue (a == heard ? 1.0f : 0.0f);
    }
}

} // namespace ampsim

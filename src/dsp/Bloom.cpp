// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Bloom.h"

#include "Fade.h"

namespace ampsim
{

bool Bloom::isValidOrder (const Order& order) noexcept
{
    std::array<bool, numEffects> seen {};
    for (auto effect : order)
    {
        const auto i = (int) effect;
        if (i < 0 || i >= numEffects || seen[(size_t) i])
            return false;
        seen[(size_t) i] = true;
    }
    return true;
}

const char* Bloom::effectName (Effect effect) noexcept
{
    switch (effect)
    {
        case Effect::bitcrush: return "bitcrush";
        case Effect::phaser:   return "phaser";
        case Effect::flanger:  return "flanger";
    }
    return "";
}

Block& Bloom::unit (Effect effect) noexcept
{
    switch (effect)
    {
        case Effect::bitcrush: return bitcrush;
        case Effect::phaser:   return phaser;
        case Effect::flanger:  return flanger;
    }
    return bitcrush;
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Bloom::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    bitcrush.setSettings (settings.bitcrush);
    phaser.setSettings (settings.phaser);
    flanger.setSettings (settings.flanger); // through-zero itself switches in applyRequests()
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));
    if (isValidOrder (settings.order))
        requested = settings.order;
}

void Bloom::setBypassed (bool shouldBeBypassed)
{
    // Only the output fades: the effects keep running, so coming back on is a pure crossfade.
    bypassed = shouldBeBypassed;
    bypassGain.setTargetValue (bypassed ? 0.0 : 1.0);
}

void Bloom::applyRequests()
{
    // Through-zero on or off: fade everything to silence, switch the flanger's dry path and ours at the bottom
    // (the latency changes there), and fade back. The latest request wins.
    if (settings.flanger.throughZero != throughZero && ! latencyPending)
    {
        latencyPending = true;
        latencyGain.setTargetValue (0.0);
    }
    if (latencyPending && ! latencyGain.isSmoothing())
    {
        throughZero = settings.flanger.throughZero;
        flanger.setThroughZero (throughZero);
        latency.store (throughZero ? throughZeroSamples : 0, std::memory_order_release);
        latencyPending = false;
        latencyGain.setTargetValue (1.0);
    }

    // A new order: dip the effects (and their input) to the dry, swap at the bottom, fade back.
    if (requested != applied && ! reorderPending)
    {
        reorderPending = true;
        sectionGain.setTargetValue (0.0);
    }
    if (reorderPending && ! sectionGain.isSmoothing())
    {
        applied = requested;
        reorderPending = false;
        sectionGain.setTargetValue (1.0);
    }
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Bloom::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    throughZeroSamples = juce::roundToInt (Flanger::throughZeroMs * sampleRate / 1000.0);

    input.setSize (2, maxBlockSize);
    for (auto* gains : { &inputGains, &wetGains, &outputGains })
        gains->assign ((size_t) maxBlockSize, 1.0);
    for (auto& line : dryLines)
        line.prepare (throughZeroSamples + 4);

    bitcrush.prepare (sampleRate, maxBlockSize);
    phaser.prepare (sampleRate, maxBlockSize);
    flanger.prepare (sampleRate, maxBlockSize);

    mix.reset (sampleRate, smoothingSeconds);
    bypassGain.reset (sampleRate, bypassFadeSeconds);
    sectionGain.reset (sampleRate, reorderFadeSeconds);
    latencyGain.reset (sampleRate, latencyFadeSeconds);
    reset();
}

void Bloom::reset()
{
    // Silent: everything requested takes effect at once, with no fades.
    bitcrush.reset();
    phaser.reset();
    flanger.reset(); // applies settings.flanger.throughZero

    throughZero = settings.flanger.throughZero;
    latency.store (throughZero ? throughZeroSamples : 0, std::memory_order_release);
    applied = requested;
    reorderPending = latencyPending = false;

    mix.setCurrentAndTargetValue (mix.getTargetValue());
    bypassGain.setCurrentAndTargetValue (bypassed ? 0.0 : 1.0);
    sectionGain.setCurrentAndTargetValue (1.0);
    latencyGain.setCurrentAndTargetValue (1.0);
    for (auto& line : dryLines)
        line.reset();
}

void Bloom::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    const auto numChannels = std::min (2, (int) block.getNumChannels());
    jassert (block.getNumSamples() <= inputGains.size()); // at most prepare()'s maxBlockSize
    const auto numSamples = std::min ((int) block.getNumSamples(), (int) inputGains.size());
    if (numChannels == 0 || numSamples == 0)
        return;

    applyRequests();
    float* const channels[2] = { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

    // This buffer's fades, sample by sample: v on every effect's input, g on their share of the output, z on all.
    bool dipping = false;
    for (int n = 0; n < numSamples; ++n)
    {
        const auto section = sCurve (sectionGain.getNextValue());
        const auto silence = sCurve (latencyGain.getNextValue());
        inputGains[(size_t) n] = section * silence;
        wetGains[(size_t) n] = mix.getNextValue() * sCurve (bypassGain.getNextValue()) * section;
        outputGains[(size_t) n] = silence;
        dipping = dipping || section < 1.0 || silence < 1.0;
    }

    // Keep the input for the dry path, then run the effects in order, in place. During a dip each effect's input is
    // scaled by v, the first's and every later one's: an effect with memory (the flanger's line and loop) keeps
    // playing its tail after its own input has gone, so only scaling every input makes each one exactly silent at
    // the swap, whatever comes before it in either order.
    for (int ch = 0; ch < numChannels; ++ch)
        input.copyFrom (ch, 0, channels[ch], numSamples);
    for (auto effect : applied)
    {
        if (dipping)
            for (int ch = 0; ch < numChannels; ++ch)
                for (int n = 0; n < numSamples; ++n)
                    channels[ch][n] *= (float) inputGains[(size_t) n];
        unit (effect).process (block.getSubBlock (0, (size_t) numSamples), context);
    }

    for (int n = 0; n < numSamples; ++n)
    {
        const auto g = wetGains[(size_t) n];
        const auto z = outputGains[(size_t) n];

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto x = input.getSample (ch, n);
            auto& line = dryLines[(size_t) ch];
            line.write (x);
            const auto dry = throughZero ? (double) line.readInteger (throughZeroSamples) : (double) x;

            // Linear: exactly the dry at g = 0, exactly the effects at g = 1.
            channels[ch][n] = (float) (z * ((1.0 - g) * dry + g * (double) channels[ch][n]));
        }
    }
}

} // namespace ampsim

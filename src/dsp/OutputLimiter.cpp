// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "OutputLimiter.h"

#include <cmath>

namespace ampsim
{

namespace
{
const double kneeRatio = std::pow (10.0, -OutputLimiter::kneeDb / 20.0); // k / c
} // namespace

double OutputLimiter::staticGain (double p, double c) noexcept
{
    const auto k = c * kneeRatio;
    if (p <= k)
        return 1.0;
    const auto span = c - k;
    return (k + span * std::tanh ((p - k) / span)) / p;
}

void OutputLimiter::setCeilingDb (float db) noexcept
{
    ceiling.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (minCeilingDb, maxCeilingDb, db)));
}

void OutputLimiter::prepare (double sampleRate, int)
{
    releaseCoefficient = std::exp (-1.0 / (releaseSeconds * sampleRate));
    ceiling.reset (sampleRate, ceilingSmoothingSeconds);
    reset();
}

void OutputLimiter::reset()
{
    ceiling.setCurrentAndTargetValue (ceiling.getTargetValue());
    gain = 1.0;
    reductionDb.store (0.0f, std::memory_order_relaxed);
}

void OutputLimiter::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numSamples = block.getNumSamples();
    auto* left = block.getChannelPointer (0);
    auto* right = block.getNumChannels() > 1 ? block.getChannelPointer (1) : nullptr;
    double lowest = gain;

    for (size_t n = 0; n < numSamples; ++n)
    {
        const auto c = (double) ceiling.getNextValue();
        const auto p = (double) juce::jmax (std::abs (left[n]), right != nullptr ? std::abs (right[n]) : 0.0f);

        // 2. Release toward 1, then take the static curve's gain if that's lower (instant attack).
        if (gain < 1.0)
        {
            gain = 1.0 - (1.0 - gain) * releaseCoefficient;
            if (1.0 - gain < 1.0e-6)
                gain = 1.0;
        }
        if (p > c * kneeRatio)
            gain = juce::jmin (gain, staticGain (p, c));

        // Under the knee with the gain back at 1: untouched (bit-transparent).
        if (gain < 1.0)
        {
            left[n] = (float) (left[n] * gain);
            if (right != nullptr)
                right[n] = (float) (right[n] * gain);
            lowest = juce::jmin (lowest, gain);
        }
    }

    reductionDb.store (lowest < 1.0 ? (float) (-20.0 * std::log10 (lowest)) : 0.0f, std::memory_order_relaxed);
}

} // namespace ampsim

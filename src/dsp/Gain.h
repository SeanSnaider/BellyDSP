// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"

namespace ampsim
{

/// A smoothed gain stage, used for the input trim (mono, top of the chain) and the output level
/// (stereo, end of the chain).
///
/// Smoothing: a knob change ramps over 25 ms instead of jumping, because a jump in gain is a step
/// in the waveform, which you hear as a click or "zipper noise". The ramp is multiplicative, which
/// means a constant ratio per sample:
///     g[n] = g0 * r^n,   r = (g1 / g0)^(1 / N)
/// In dB that's 20*log10(g[n]) = 20*log10(g0) + n * 20*log10(r), a straight line. Equal dB steps
/// per sample sound even, because loudness is perceived logarithmically. A linear ramp would rush
/// through the quiet end. Same math as nih-plug's Logarithmic smoother in the old Rust code.
class Gain : public Block
{
public:
    explicit Gain (bool isStereoBlock) : stereo (isStereoBlock) {}

    /// Audio thread, once per buffer, before process(). Setting the same value again doesn't
    /// restart the ramp, because SmoothedValue ignores unchanged targets.
    void setGainDecibels (float db)
    {
        // Multiplicative smoothing can't ramp to or from 0, so floor at -100 dB.
        gain.setTargetValue (juce::jmax (1.0e-5f, juce::Decibels::decibelsToGain (db)));
    }

    void prepare (double sampleRate, int /*maxBlockSize*/) override
    {
        gain.reset (sampleRate, rampSeconds); // also snaps the current value to the target
    }

    void process (juce::dsp::AudioBlock<float> block, const BlockContext&) override
    {
        const auto numChannels = (stereo && block.getNumChannels() > 1) ? size_t { 2 } : size_t { 1 };
        const auto numSamples = block.getNumSamples();

        if (! gain.isSmoothing())
        {
            // The common case: one constant gain for the whole buffer.
            const auto g = gain.getTargetValue();
            for (size_t ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::multiply (block.getChannelPointer (ch), g, (int) numSamples);
            return;
        }

        auto* left = block.getChannelPointer (0);
        auto* right = numChannels == 2 ? block.getChannelPointer (1) : nullptr;

        for (size_t i = 0; i < numSamples; ++i)
        {
            const auto g = gain.getNextValue(); // advance once per sample, shared by both channels
            left[i] *= g;
            if (right != nullptr)
                right[i] *= g;
        }
    }

    void reset() override { gain.setCurrentAndTargetValue (gain.getTargetValue()); }

    bool isStereo() const override { return stereo; }

private:
    static constexpr double rampSeconds = 0.025;

    const bool stereo;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f };
};

} // namespace ampsim

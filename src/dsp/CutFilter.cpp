#include "CutFilter.h"

#include <cmath>

namespace ampsim
{

double CutFilter::sectionQ (Slope slope, int section)
{
    // Section k = section + 1 of an order-N Butterworth: Q_k = 1 / (2 sin((2k - 1) pi / (2N))).
    const auto order = 2 * numSections (slope);
    return 1.0 / (2.0 * std::sin ((2.0 * section + 1.0) * juce::MathConstants<double>::pi / (2.0 * order)));
}

double CutFilter::responseDb (Kind kind, Slope slope, double cutoffHz, double f, double sampleRate)
{
    const auto order = 2.0 * numSections (slope); // N
    const auto w = std::tan (juce::MathConstants<double>::pi * f / sampleRate)
                   / std::tan (juce::MathConstants<double>::pi * cutoffHz / sampleRate);
    return -10.0 * std::log10 (1.0 + std::pow (w, kind == Kind::highCut ? 2.0 * order : -2.0 * order));
}

void CutFilter::prepare (double rate, int maxBlockSize)
{
    sampleRate = rate;
    dry.setSize (2, maxBlockSize);
    cutoff.reset (sampleRate, 0.025); // snaps to the target set before prepare()
    wet.reset (sampleRate, fadeSeconds);
    wet.setCurrentAndTargetValue (on ? 1.0f : 0.0f);
    slope = pendingSlope;
    slopeChangePending = false;
    reset();
}

void CutFilter::reset()
{
    for (auto& channel : sections)
        for (auto& section : channel)
            section.reset();

    needsReset = false;
    samplesUntilUpdate = 0;
    updateCoefficients();
}

void CutFilter::set (bool shouldBeOn, float cutoffHz, Slope newSlope)
{
    cutoff.setTargetValue (juce::jlimit (20.0f, 20000.0f, cutoffHz));
    const bool fullyOff = ! on && ! wet.isSmoothing() && wet.getCurrentValue() <= 0.0f && ! slopeChangePending;

    if (newSlope != pendingSlope)
    {
        pendingSlope = newSlope;

        if (fullyOff)
        {
            slope = newSlope; // silent: nothing is running
            needsReset = true;
        }
        else
        {
            slopeChangePending = true; // dip to the unfiltered signal, swap, fade back
            wet.setTargetValue (0.0f);
        }
    }

    if (shouldBeOn != on)
    {
        // Switching on from fully off: the filter state is stale and the cutoff may have moved
        // while nobody was listening, so start clean at the current setting.
        if (shouldBeOn && fullyOff)
        {
            needsReset = true;
            cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
        }

        on = shouldBeOn;
        if (! slopeChangePending)
            wet.setTargetValue (on ? 1.0f : 0.0f);
    }
}

void CutFilter::updateCoefficients()
{
    const auto type = kind == Kind::lowCut ? Svf::Type::highpass : Svf::Type::lowpass;
    const double f = cutoff.getCurrentValue();

    for (int s = 0; s < numSections (slope); ++s)
    {
        const auto c = Svf::design (type, f, sectionQ (slope, s), 0.0, sampleRate);
        for (auto& channel : sections)
            channel[(size_t) s].setCoefficients (c);
    }
}

void CutFilter::process (float* const* channels, int numChannels, int numSamples)
{
    // The dip for a slope change has reached the unfiltered signal: swap, then fade back in.
    if (slopeChangePending && ! wet.isSmoothing())
    {
        slope = pendingSlope;
        slopeChangePending = false;
        needsReset = true;
        wet.setTargetValue (on ? 1.0f : 0.0f);
    }

    if (! on && ! wet.isSmoothing() && wet.getCurrentValue() <= 0.0f)
        return; // fully off: skip

    if (needsReset)
        reset();

    const bool fading = wet.isSmoothing();
    if (fading)
        for (int ch = 0; ch < numChannels; ++ch)
            dry.copyFrom (ch, 0, channels[ch], numSamples);

    const auto count = numSections (slope);

    for (int n = 0; n < numSamples; ++n)
    {
        // Cutoff moves: redesign every 32 samples, as for every other knob-driven filter.
        if (samplesUntilUpdate-- <= 0)
        {
            samplesUntilUpdate = coefficientInterval - 1;
            if (cutoff.isSmoothing())
            {
                cutoff.skip (coefficientInterval);
                updateCoefficients();
            }
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto v = (double) channels[ch][n];
            for (int s = 0; s < count; ++s)
                v = sections[(size_t) ch][(size_t) s].processSample (v);
            channels[ch][n] = (float) v;
        }
    }

    if (fading)
    {
        // Linear crossfade with the unfiltered signal (the two are strongly correlated).
        for (int n = 0; n < numSamples; ++n)
        {
            const auto w = wet.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto d = dry.getSample (ch, n);
                channels[ch][n] = d + w * (channels[ch][n] - d);
            }
        }
    }
}

} // namespace ampsim

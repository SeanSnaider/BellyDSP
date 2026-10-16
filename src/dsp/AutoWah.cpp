// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AutoWah.h"

#include <algorithm>
#include <cmath>

namespace ampsim
{

double AutoWah::controlFor (double e, double sensitivityPercent, Direction direction) noexcept
{
    // In dB, with Sensitivity setting where "closed" ends (AutoWah.h, 2). The offset keeps log10 finite at 0.
    const auto levelDb = 20.0 * std::log10 (e + levelOffset);
    const auto floorDb = floorAtZeroDb - floorPerPercentDb * sensitivityPercent;
    const auto c = juce::jlimit (0.0, 1.0, (levelDb - floorDb) / spanDb);
    return direction == Direction::down ? 1.0 - c : c;
}

double AutoWah::centreFrequency (double c, double lowHz, double highHz) noexcept
{
    // Exponential (AutoWah.h, 3): c = 0 gives low, 1 gives high, 0.5 the geometric mean.
    return lowHz * std::pow (highHz / lowHz, c);
}

double AutoWah::wetGain (double q) noexcept
{
    // The level hold (AutoWah.h, 6): the band-pass's loss on a guitar goes as 1/sqrt(Q).
    return std::pow (10.0, makeupDb / 20.0) * std::sqrt (q / makeupReferenceQ);
}

double AutoWah::coefficient (double milliseconds) const noexcept
{
    // a = exp(-1 / (tau fs)): 63 % of a step in tau (AutoWah.h, 1).
    return std::exp (-1.0 / (milliseconds * 0.001 * sampleRate));
}

void AutoWah::updateTimeConstants() noexcept
{
    attack = coefficient (juce::jlimit (minAttackMs, maxAttackMs, (double) settings.attackMs));
    release = coefficient (juce::jlimit (minReleaseMs, maxReleaseMs, (double) settings.releaseMs));
}

void AutoWah::setSettings (const Settings& newSettings)
{
    const bool timesChanged = ! juce::exactlyEqual (newSettings.attackMs, settings.attackMs)
                              || ! juce::exactlyEqual (newSettings.releaseMs, settings.releaseMs);
    settings = newSettings;

    sensitivity.setTargetValue (juce::jlimit (minSensitivity, maxSensitivity, (double) settings.sensitivity));
    freqLow.setTargetValue (juce::jlimit (minLowHz, maxLowHz, (double) settings.freqLowHz));
    freqHigh.setTargetValue (juce::jlimit (minHighHz, maxHighHz, (double) settings.freqHighHz));
    q.setTargetValue (juce::jlimit (minQ, maxQ, (double) settings.q));
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));

    if (timesChanged)
        updateTimeConstants();
}

void AutoWah::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    for (auto* s : { &freqLow, &freqHigh, &q })
        s->reset (sampleRate, smoothingSeconds);
    for (auto* s : { &sensitivity, &mix })
        s->reset (sampleRate, smoothingSeconds);
    updateTimeConstants();
    reset();
}

void AutoWah::reset()
{
    // Start closed (the envelope at rest), on the current settings with no ramps.
    for (auto* s : { &freqLow, &freqHigh, &q })
        s->setCurrentAndTargetValue (s->getTargetValue());
    for (auto* s : { &sensitivity, &mix })
        s->setCurrentAndTargetValue (s->getTargetValue());
    envelope = 0.0;
    filter.reset();
    untilRedesign = 0; // the next sample starts a chunk, and redesigns
    redesign();
    meterControl.store ((float) control, std::memory_order_relaxed);
}

void AutoWah::setRedesignIntervalForStudy (int samples) noexcept
{
    interval = juce::jmax (1, samples);
    untilRedesign = juce::jmin (untilRedesign, interval);
}

void AutoWah::redesign() noexcept
{
    // fc from the envelope as it stands at the start of the chunk (AutoWah.h, 2 to 5).
    control = controlFor (envelope, sensitivity.getCurrentValue(), settings.direction);
    centre = centreFrequency (control, freqLow.getCurrentValue(), freqHigh.getCurrentValue());
    filter.setCoefficients (Svf::design (Svf::Type::bandpass, centre, q.getCurrentValue(), 0.0, sampleRate));
    gain = wetGain (q.getCurrentValue());
}

void AutoWah::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    auto* x = block.getChannelPointer (0);
    const auto numSamples = (int) block.getNumSamples();
    // The clean DI when the chain has one; otherwise (the block alone, in tests) its own input. Either way each
    // sample is read before x[i] is overwritten.
    const float* detector = context.di != nullptr ? context.di : x;

    for (int i = 0; i < numSamples;)
    {
        if (untilRedesign == 0)
        {
            // A new chunk: move the knob smoothers on by its length, then redesign once for the whole chunk.
            for (auto* s : { &freqLow, &freqHigh, &q })
                s->skip (interval);
            sensitivity.skip (interval);
            redesign();
            untilRedesign = interval;
        }

        const auto n = juce::jmin (untilRedesign, numSamples - i);
        const auto end = i + n;
        const bool dryOnly = ! mix.isSmoothing() && juce::exactlyEqual (mix.getTargetValue(), 0.0);
        for (; i < end; ++i)
        {
            // The envelope follower (1): attack while the level rises, release while it falls.
            const auto r = std::abs ((double) detector[i]);
            const auto a = r > envelope ? attack : release;
            envelope = a * envelope + (1.0 - a) * r;

            const auto dry = (double) x[i];
            const auto wet = gain * filter.processSample (dry);
            if (dryOnly)
                continue; // mix 0: the dry stays exactly as it came in
            const auto m = mix.getNextValue();
            x[i] = (float) ((1.0 - m) * dry + m * wet);
        }
        untilRedesign -= n;
    }

    meterControl.store ((float) control, std::memory_order_relaxed);
}

} // namespace ampsim

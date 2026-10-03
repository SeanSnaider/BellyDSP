// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Harmonizer.h"

#include "Multivoicer.h"

namespace ampsim
{

Harmonizer::Settings Harmonizer::defaults()
{
    Settings s;
    s.voices[0] = { true, { true, 2, 0 }, -3.0, 0.0, 0.0 };   // a third above
    s.voices[1] = { false, { true, 4, 0 }, -3.0, 0.0, 0.0 };  // a fifth above
    s.voices[2] = { false, { true, 0, -1 }, -6.0, -0.3, 0.0 }; // an octave below
    s.voices[3] = { false, { true, 0, 1 }, -6.0, 0.3, 0.0 };   // an octave above
    return s;
}

void Harmonizer::setSettings (const Settings& newSettings) noexcept
{
    settings = newSettings;
    settings.floor = juce::jlimit (0, (int) floors.size() - 1, settings.floor);

    auto trackerSettings = tracker.getSettings();
    trackerSettings.referenceA4 = juce::jlimit (430.0, 450.0, settings.referenceA4);
    tracker.setSettings (trackerSettings);

    level.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (minLevelDb, 6.0, settings.levelDb), minLevelDb));
    for (int v = 0; v < maxVoices; ++v)
    {
        const auto& voice = settings.voices[(size_t) v];
        const auto gain = voice.levelDb <= minLevelDb ? 0.0 : juce::Decibels::decibelsToGain (std::min (6.0, voice.levelDb));
        gains[(size_t) v].setTargetValue (gain);
        const auto [l, r] = Multivoicer::panGains (juce::jlimit (-1.0, 1.0, voice.pan));
        lefts[(size_t) v].setTargetValue (l);
        rights[(size_t) v].setTargetValue (r);
    }
}

void Harmonizer::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    input.prepare (sampleRate, maxHumanizeMs + 60.0, GranularVoice::searchMarginMs);
    for (size_t f = 0; f < floors.size(); ++f)
        analyses[f].prepare (sampleRate, floors[f]);
    tracker.prepare ((double) PsolaAnalysis::hop / sampleRate, tracker.getSettings());
    for (auto& voice : psola)
        voice.prepare (sampleRate);
    for (int v = 0; v < maxVoices; ++v)
        for (auto* s : { &gains[(size_t) v], &lefts[(size_t) v], &rights[(size_t) v] })
            s->reset (sampleRate, smoothingSeconds);
    level.reset (sampleRate, smoothingSeconds);
    setSettings (settings);
    reset();
}

void Harmonizer::reset()
{
    input.reset();
    for (auto& analysis : analyses)
        analysis.reset();
    tracker.reset();
    for (auto& voice : psola)
        voice.reset();
    for (int v = 0; v < maxVoices; ++v)
        for (auto* s : { &gains[(size_t) v], &lefts[(size_t) v], &rights[(size_t) v] })
            s->setCurrentAndTargetValue (s->getTargetValue());
    level.setCurrentAndTargetValue (level.getTargetValue());
    activeFloor = settings.floor;
    hopPhase = 0;
    shifts.fill (0);
}

void Harmonizer::onHop() noexcept
{
    // The tracker reads the analysis' latest McLeod estimate (on the DI).
    const auto estimate = analyses[(size_t) activeFloor].getLastEstimate();
    tracker.update (estimate.frequency, estimate.clarity);
    const auto sounding = tracker.hasNote() && analyses[(size_t) activeFloor].isConfident();

    for (int v = 0; v < maxVoices; ++v)
    {
        const auto& voice = settings.voices[(size_t) v];
        if (tracker.hasNote())
            shifts[(size_t) v] = harmony::shiftFor (tracker.getNote(), voice.interval, settings.key, settings.outOfScale);

        PsolaVoice::Settings s;
        s.ratio = std::exp2 ((double) shifts[(size_t) v] / 12.0);
        s.delayMs = juce::jlimit (0.0, maxHumanizeMs, voice.humanizeMs);
        // A voice starting from silence takes its ratio at once; a sounding one glides to a new interval.
        s.glideMs = psola[(size_t) v].getFade() <= 0.0 ? 0.0 : std::max (0.0, settings.glideMs);
        s.fadeMs = fadeMs;
        s.active = voice.on && voice.levelDb > minLevelDb && sounding;
        psola[(size_t) v].setSettings (s);
        shownShifts[(size_t) v].store (s.active ? shifts[(size_t) v] : shownSilent, std::memory_order_relaxed);
    }
    shownNote.store (sounding ? tracker.getNote() : -1, std::memory_order_relaxed);
}

void Harmonizer::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    const auto numChannels = std::min (2, (int) block.getNumChannels());
    const auto numSamples = (int) block.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    // A new detection floor: start that analysis (and the note tracking) from scratch.
    if (settings.floor != activeFloor)
    {
        activeFloor = settings.floor;
        analyses[(size_t) activeFloor].reset();
        tracker.reset();
        hopPhase = 0;
    }

    float* const left = block.getChannelPointer (0);
    float* const right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;
    const auto* di = context.di != nullptr && context.numSamples >= numSamples ? context.di : nullptr;
    auto& analysis = analyses[(size_t) activeFloor];

    for (int n = 0; n < numSamples; ++n)
    {
        const double x[2] = { (double) left[n], right != nullptr ? (double) right[n] : (double) left[n] };

        // The voices read the processed guitar before this sample is pushed (read before write).
        double wet[2] = { 0.0, 0.0 };
        for (int v = 0; v < maxVoices; ++v)
        {
            const auto g = gains[(size_t) v].getNextValue();
            const auto l = lefts[(size_t) v].getNextValue(), r = rights[(size_t) v].getNextValue();
            const auto y = (double) psola[(size_t) v].process (analysis, input);
            wet[0] += g * l * y;
            wet[1] += g * r * y;
        }
        input.push ((float) (0.5 * (x[0] + x[1])));
        analysis.process (di != nullptr ? di[n] : (float) x[0], input);
        if (++hopPhase == PsolaAnalysis::hop)
        {
            hopPhase = 0;
            onHop();
        }

        const auto overall = level.getNextValue();
        left[n] = (float) (x[0] + overall * wet[0]);
        if (right != nullptr)
            right[n] = (float) (x[1] + overall * wet[1]);
    }
}

} // namespace ampsim

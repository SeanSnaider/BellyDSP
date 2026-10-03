// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Svf.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace ampsim
{

/// An amp slot's tone controls: Depth, Bass, Mid, Treble, and Presence, as EQ on the model's output
/// (BUILD_PLAN "Amps"). A capture is a snapshot of one amp at one knob setting, so these shape the
/// captured tone rather than reproduce the real amp's interacting tone stack.
///
/// Five SVF bands in series. The frequencies are starting points, to be tuned by ear:
///   Depth     bell at 90 Hz, Q 1.2: the low resonance a power amp shows into a speaker
///   Bass      low shelf at 180 Hz
///   Mid       bell at 800 Hz, Q 0.7
///   Treble    high shelf at 2.8 kHz
///   Presence  bell at 5 kHz, Q 0.8: upper-mid bite, the range a power amp's presence control covers
/// Each knob is +-12 dB, smoothed per sample over 25 ms, with coefficients recomputed every 32
/// samples (foundation decisions). At 0 dB every band is bit-transparent.
class AmpTone
{
public:
    enum Band
    {
        depth,
        bass,
        mid,
        treble,
        presence,
        numBands
    };

    struct BandSpec
    {
        const char* name;
        Svf::Type type;
        double frequency;
        double q;
    };

    static constexpr std::array<BandSpec, numBands> bands { {
        { "Depth",    Svf::Type::peak,      90.0,   1.2 },
        { "Bass",     Svf::Type::lowShelf,  180.0,  0.7071 },
        { "Mid",      Svf::Type::peak,      800.0,  0.7 },
        { "Treble",   Svf::Type::highShelf, 2800.0, 0.7071 },
        { "Presence", Svf::Type::peak,      5000.0, 0.8 },
    } };

    static constexpr float rangeDb = 12.0f;
    static constexpr int coefficientInterval = 32;

    /// Audio thread, once per buffer.
    void setGainDb (Band band, float db) { gains[(size_t) band].setTargetValue (juce::jlimit (-rangeDb, rangeDb, db)); }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        for (size_t b = 0; b < numBands; ++b)
        {
            gains[b].reset (sampleRate, 0.025); // also snaps to the target
            updateBand (b, gains[b].getTargetValue());
            filters[b].reset();
        }
        samplesUntilUpdate = coefficientInterval;
    }

    void reset()
    {
        for (size_t b = 0; b < numBands; ++b)
        {
            gains[b].setCurrentAndTargetValue (gains[b].getTargetValue());
            updateBand (b, gains[b].getTargetValue());
            filters[b].reset();
        }
    }

    void process (float* samples, int numSamples) noexcept
    {
        int i = 0;

        while (i < numSamples)
        {
            if (samplesUntilUpdate == 0)
            {
                // Advance each smoother by one interval and redesign the bands that moved, using the
                // gain at the middle of the interval.
                for (size_t b = 0; b < numBands; ++b)
                {
                    if (gains[b].isSmoothing())
                    {
                        const auto mid = gains[b].skip (coefficientInterval / 2);
                        gains[b].skip (coefficientInterval / 2);

                        // If the ramp ended inside this interval, land exactly on the target.
                        updateBand (b, gains[b].isSmoothing() ? mid : gains[b].getTargetValue());
                    }
                }

                samplesUntilUpdate = coefficientInterval;
            }

            const auto len = std::min (samplesUntilUpdate, numSamples - i);

            for (int j = i; j < i + len; ++j)
            {
                auto v = (double) samples[j];
                for (auto& filter : filters)
                    v = filter.processSample (v);
                samples[j] = (float) v;
            }

            i += len;
            samplesUntilUpdate -= len;
        }
    }

    /// For tests and the GUI: the current overall response at frequency f, in dB.
    double responseDb (double f) const
    {
        std::complex<double> h = 1.0;
        for (const auto& filter : filters)
            h *= Svf::responseAt (filter.getCoefficients(), f, sampleRate);
        return 20.0 * std::log10 (std::abs (h));
    }

private:
    void updateBand (size_t b, float db)
    {
        const auto& spec = bands[b];
        filters[b].setCoefficients (Svf::design (spec.type, spec.frequency, spec.q, db, sampleRate));
    }

    std::array<Svf, numBands> filters;
    std::array<juce::SmoothedValue<float>, numBands> gains; // in dB, ramped linearly in dB
    double sampleRate = 48000.0;
    int samplesUntilUpdate = coefficientInterval;
};

} // namespace ampsim

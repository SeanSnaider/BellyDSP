// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Analyzer.h"

namespace ui
{

SpectrumAnalyzer::SpectrumAnalyzer()
    : history ((size_t) fftSize, 0.0f),
      fftData ((size_t) fftSize * 2, 0.0f),
      latest ((size_t) fftSize / 2 + 1, floorDb),
      smoothed ((size_t) fftSize / 2 + 1, floorDb),
      scratch (2048, 0.0f)
{
}

int SpectrumAnalyzer::pull (ampsim::SpscRing<float>& ring)
{
    int total = 0;
    for (int n; (n = ring.read (scratch.data(), (int) scratch.size())) > 0;)
    {
        push (scratch.data(), n);
        total += n;
    }
    return total;
}

void SpectrumAnalyzer::push (const float* samples, int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        history[(size_t) writePosition] = samples[i];
        writePosition = (writePosition + 1) & (fftSize - 1);
    }
    taken += numSamples;
}

void SpectrumAnalyzer::reset()
{
    std::fill (history.begin(), history.end(), 0.0f);
    std::fill (latest.begin(), latest.end(), floorDb);
    std::fill (smoothed.begin(), smoothed.end(), floorDb);
    writePosition = 0;
    taken = 0;
}

void SpectrumAnalyzer::compute()
{
    // Oldest to newest into the FFT's buffer (writePosition is the oldest sample), the upper half zero.
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = history[(size_t) ((writePosition + i) & (fftSize - 1))];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const auto scale = 4.0f / (float) fftSize;
    for (size_t k = 0; k < latest.size(); ++k)
    {
        const auto magnitude = fftData[k] * scale;
        latest[k] = magnitude > 1.0e-8f ? 20.0f * std::log10 (magnitude) : floorDb;
        smoothed[k] = latest[k] > smoothed[k] ? latest[k] : smoothed[k] + 0.2f * (latest[k] - smoothed[k]);
    }
}

double SpectrumAnalyzer::peakFrequency (double sampleRate) const
{
    size_t best = 1;
    for (size_t k = 2; k + 1 < latest.size(); ++k)
        if (latest[k] > latest[best])
            best = k;

    // The vertex of the parabola through the peak bin and its neighbours: delta = (a - c) / (2 (a - 2b + c)).
    const auto a = (double) latest[best - 1], b = (double) latest[best], c = (double) latest[best + 1];
    const auto denominator = a - 2.0 * b + c;
    const auto delta = std::abs (denominator) > 1.0e-12 ? 0.5 * (a - c) / denominator : 0.0;
    return ((double) best + juce::jlimit (-0.5, 0.5, delta)) * sampleRate / (double) fftSize;
}

float SpectrumAnalyzer::levelAt (double f, double ratio, double sampleRate) const
{
    const auto binWidth = sampleRate / (double) fftSize;
    const auto lo = f / binWidth, hi = f * ratio / binWidth;
    const auto last = (double) (smoothed.size() - 1);

    if ((int) std::floor (hi) <= (int) std::ceil (lo))
    {
        // Narrower than a bin: interpolate (in dB) between the two bins around f.
        const auto position = juce::jlimit (0.0, last - 1.0, lo);
        const auto k = (size_t) position;
        const auto t = (float) (position - (double) k);
        return smoothed[k] + t * (smoothed[k + 1] - smoothed[k]);
    }

    auto level = floorDb;
    for (auto k = (size_t) std::ceil (lo); k <= (size_t) juce::jmin (std::floor (hi), last); ++k)
        level = juce::jmax (level, smoothed[k]);
    return level;
}

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "dsp/SpscRing.h"

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace ui
{

/// The EQ page's live spectrum (UI_DESIGN "Layout", the EQ page): the newest 4096 samples the processor
/// tapped into its analyzer ring, Hann-windowed, through JUCE's FFT, in dBFS, smoothed over time. GUI
/// thread only; it's the ring's one reader. 4096 points at 48 kHz are 11.7 Hz bins, fine enough to tell
/// the low E's fundamental (82 Hz) from its octave.
class SpectrumAnalyzer
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder;

    SpectrumAnalyzer();

    /// Takes everything waiting in the ring, keeping the newest fftSize samples. Returns how many it took.
    int pull (ampsim::SpscRing<float>& ring);

    /// Adds samples directly (tests, and pull()).
    void push (const float* samples, int numSamples);

    /// Runs the FFT on the newest fftSize samples and updates the smoothed spectrum: a bin rises at once
    /// and falls by a fifth of the gap each frame (about 0.15 s at 30 frames a second).
    void compute();

    /// Forgets the history and the spectrum (when the page switches between the two EQs).
    void reset();

    /// fftSize / 2 + 1 bins in dBFS, where a sine centred on a bin reads its own peak level: through a
    /// Hann window (mean 1/2) its bin has magnitude A N / 4, so the level is 20 log10 (4 |X| / N).
    const std::vector<float>& getSpectrum() const noexcept { return smoothed; }
    const std::vector<float>& getLatest() const noexcept { return latest; }

    /// The loudest bin of the latest frame, refined by a parabola through it and its neighbours (in dB),
    /// which places a sine to a small fraction of a bin.
    double peakFrequency (double sampleRate) const;

    /// The smoothed level at f: the loudest bin between f and f * ratio (one column of the display), or
    /// interpolated between the two nearest bins when a bin is wider than that.
    float levelAt (double f, double ratio, double sampleRate) const;

    juce::int64 getSamplesTaken() const noexcept { return taken; }

    static constexpr float floorDb = -160.0f;

private:
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> history, fftData, latest, smoothed, scratch;
    int writePosition = 0;
    juce::int64 taken = 0;
};

} // namespace ui

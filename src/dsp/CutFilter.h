// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Svf.h"

#include <juce_dsp/juce_dsp.h>

#include <array>

namespace ampsim
{

/// A Butterworth low cut (high-pass) or high cut (low-pass) at 12, 24, or 48 dB per octave, on one or
/// two channels. Used by the cab and both EQs.
///
/// A Butterworth filter of order N has its poles evenly spaced on a half circle, which makes the
/// passband as flat as possible. Built as N/2 second-order SVF sections, section k (k = 1..N/2) has
///     Q_k = 1 / (2 sin((2k - 1) pi / (2N)))
/// so 12 dB/oct (N = 2) is one section at Q 0.7071, 24 (N = 4) is Q 1.3066 and 0.5412, and 48 (N = 8)
/// is Q 2.5629, 0.9000, 0.6013, 0.5098. The cascade is exactly the analog prototype,
/// |H|^2 = 1 / (1 + W^(2N)) for a high cut and 1 / (1 + W^(-2N)) for a low cut, with
/// W = tan(pi f / fs) / tan(pi fc / fs) the bilinear transform's warped frequency: -3.01 dB at the
/// cutoff for every slope.
///
/// The cutoff is smoothed (multiplicative, 25 ms) and the coefficients redesigned every 32 samples.
/// On/off crossfades with the unfiltered signal over 10 ms. A slope change can't be blended (old and
/// new filters have different state), so it dips to the unfiltered signal, swaps, and fades back in.
class CutFilter
{
public:
    enum class Kind
    {
        lowCut, // high-pass
        highCut // low-pass
    };

    enum class Slope
    {
        db12,
        db24,
        db48
    };

    static constexpr int maxSections = 4;
    static constexpr int coefficientInterval = 32;
    static constexpr double fadeSeconds = 0.010;

    explicit CutFilter (Kind filterKind) : kind (filterKind) {}

    static int numSections (Slope slope) { return slope == Slope::db12 ? 1 : (slope == Slope::db24 ? 2 : 4); }
    static double sectionQ (Slope slope, int section);

    /// The analog-prototype magnitude in dB at f, through the bilinear warping (what the tests hold
    /// the filter to).
    static double responseDb (Kind kind, Slope slope, double cutoffHz, double f, double sampleRate);

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /// Audio thread, once per buffer.
    void set (bool on, float cutoffHz, Slope slope);

    /// Audio thread. Up to two channels, in place.
    void process (float* const* channels, int numChannels, int numSamples);

    bool isOn() const noexcept { return on; }
    bool isActive() const noexcept { return on || wet.isSmoothing(); }

private:
    void updateCoefficients();

    const Kind kind;
    double sampleRate = 48000.0;
    std::array<std::array<Svf, maxSections>, 2> sections; // [channel][section]
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 1000.0f };
    juce::SmoothedValue<float> wet { 0.0f }; // 0 = unfiltered, 1 = filtered
    Slope slope = Slope::db12, pendingSlope = Slope::db12;
    bool on = false;
    bool slopeChangePending = false;
    bool needsReset = false;
    int samplesUntilUpdate = 0;
    juce::AudioBuffer<float> dry;
};

} // namespace ampsim

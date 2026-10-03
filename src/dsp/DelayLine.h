// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

namespace ampsim
{

/// 4-point, 3rd-order Hermite interpolation (Catmull-Rom), in the form from Olli Niemitalo, "Polynomial
/// Interpolators for High-Quality Resampling of Oversampled Audio" (2001). Given four consecutive
/// samples x[-1], x[0], x[1], x[2] and a position t in [0, 1) between x[0] and x[1]:
///     c0 = x0
///     c1 = (x1 - xm1) / 2
///     c2 = xm1 - 5/2 x0 + 2 x1 - x2 / 2
///     c3 = (x2 - xm1) / 2 + 3/2 (x0 - x1)
///     y  = ((c3 t + c2) t + c1) t + c0
/// The curve passes through every sample and its slope at each sample is the central difference, so
/// it's smooth (C1) across samples. Linear interpolation dulls the highs whenever the read position
/// moves (the BUILD_PLAN's reason for Hermite in every modulated delay); Hermite keeps them up to a few
/// kHz below Nyquist. It reproduces any polynomial up to degree 2 exactly.
inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const auto c1 = 0.5f * (x1 - xm1);
    const auto c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const auto c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

/// A mono circular delay line, allocated once for its maximum delay (prepare), then read at any
/// fractional delay with Hermite interpolation. The size is rounded up to a power of two so wrapping is
/// a bit mask. Write a sample, then read delays measured from it: read(0) is the sample just written,
/// read(d) the one d samples before. Fractional reads need one sample of headroom on each side, so the
/// largest usable delay is maxDelay (prepare() allocates maxDelay + 4).
class DelayLine
{
public:
    void prepare (int maxDelaySamples)
    {
        int size = 1;
        while (size < maxDelaySamples + 4)
            size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        writeIndex = 0;
        maxDelay = maxDelaySamples;
    }

    void reset() noexcept { std::fill (buffer.begin(), buffer.end(), 0.0f); }

    int getMaxDelay() const noexcept { return maxDelay; }

    void write (float x) noexcept
    {
        writeIndex = (writeIndex + 1) & mask;
        buffer[(size_t) writeIndex] = x;
    }

    /// The sample written `delay` writes ago (whole samples).
    float readInteger (int delay) const noexcept
    {
        return buffer[(size_t) ((writeIndex - delay) & mask)];
    }

    /// The signal `delay` samples ago, between samples by Hermite interpolation. 0 <= delay <= maxDelay.
    float read (double delay) const noexcept
    {
        const auto d = juce::jlimit (0.0, (double) maxDelay, delay);
        const auto whole = (int) d;
        const auto t = (float) (d - whole);

        // Going back in time from x0 = delay `whole`: x1 is one sample older, xm1 one sample newer.
        const auto x0 = readInteger (whole);
        const auto x1 = readInteger (whole + 1);
        const auto xm1 = whole > 0 ? readInteger (whole - 1) : x0;
        const auto x2 = readInteger (whole + 2);
        return hermite (xm1, x0, x1, x2, t);
    }

private:
    std::vector<float> buffer;
    int mask = 0, writeIndex = 0, maxDelay = 0;
};

} // namespace ampsim

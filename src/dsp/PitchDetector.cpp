// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PitchDetector.h"

#include <cmath>

namespace ampsim
{

namespace
{
/// sum a_i b_i in double precision. A float times a float is exact in a double (24 + 24 bits < 53), so
/// only the additions round. Four running sums let the CPU overlap the additions instead of waiting
/// for each one in turn.
double dot (const float* a, const float* b, int n) noexcept
{
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;

    for (; i + 4 <= n; i += 4)
    {
        s0 += (double) a[i] * (double) b[i];
        s1 += (double) a[i + 1] * (double) b[i + 1];
        s2 += (double) a[i + 2] * (double) b[i + 2];
        s3 += (double) a[i + 3] * (double) b[i + 3];
    }

    for (; i < n; ++i)
        s0 += (double) a[i] * (double) b[i];

    return (s0 + s1) + (s2 + s3);
}
} // namespace

void PitchDetector::prepare (double inputSampleRate, const Settings& newSettings)
{
    settings = newSettings;
    decimatedRate = inputSampleRate / decimation;

    // The longest lag is the period of minFrequency plus two samples of headroom, and each lag's window
    // is max(lag, W_min). The interpolation looks one lag further, so the frame holds the largest
    // window plus maxLag + 1 samples.
    maxLag = (int) (decimatedRate / settings.minFrequency) + 2;
    minWindow = std::max (1, juce::roundToInt (settings.minWindowSeconds * decimatedRate));
    frameLength = std::max (maxLag + 1, minWindow) + maxLag + 1;

    // Butterworth sections: Q_k = 1 / (2 sin((2k - 1) pi / (2N))) for N = 8, k = 4, 3, 2, 1
    // (0.510, 0.601, 0.900, 2.563): together a maximally flat 8th-order low-pass.
    for (int k = 4; k >= 1; --k)
    {
        const auto q = 1.0 / (2.0 * std::sin ((2 * k - 1) * juce::MathConstants<double>::pi / 16.0));
        lowpass[(size_t) (4 - k)].setCoefficients (Svf::design (Svf::Type::lowpass, lowpassHz, q, 0.0, inputSampleRate));
    }

    history.prepare (frameLength);
    nsdf.assign ((size_t) maxLag + 1, 0.0);
    energy.assign ((size_t) frameLength + 1, 0.0);
    reset();
}

void PitchDetector::reset() noexcept
{
    for (auto& section : lowpass)
        section.reset();

    phase = 0;
    history.reset();
}

double PitchDetector::nsdfAt (const float* x, int tau, int w) const noexcept
{
    // Newest w samples against the w samples tau earlier. m from the prefix energies:
    // sum over [a, b) of x^2 = energy[b] - energy[a].
    const auto L = frameLength;
    const auto r = dot (x + L - w, x + L - w - tau, w);
    const auto m = (energy[(size_t) L] - energy[(size_t) (L - w)]) + (energy[(size_t) (L - tau)] - energy[(size_t) (L - w - tau)]);
    return m > 0.0 ? 2.0 * r / m : 0.0;
}

template <typename Fn>
void PitchDetector::forEachKeyMaximum (Fn&& fn) const noexcept
{
    // McLeod and Wyvill: skip the positive lobe around lag 0, then report the highest point of every later
    // positive lobe (from a positive-going zero crossing to the next negative-going one). A lobe still open
    // at maxLag counts too. Ties go to the first sample, like numpy's argmax in the prototype.
    int tau = 1;
    while (tau <= maxLag && nsdf[(size_t) tau] > 0.0)
        ++tau;

    while (tau <= maxLag)
    {
        while (tau <= maxLag && nsdf[(size_t) tau] <= 0.0)
            ++tau;

        const auto start = tau;
        auto peak = tau;

        while (tau <= maxLag && nsdf[(size_t) tau] > 0.0)
        {
            if (nsdf[(size_t) tau] > nsdf[(size_t) peak])
                peak = tau;
            ++tau;
        }

        if (start < tau)
            fn (peak);
    }
}

PitchDetector::Estimate PitchDetector::detect() noexcept
{
    const auto* x = history.newest (frameLength);

    energy[0] = 0.0;
    for (int i = 0; i < frameLength; ++i)
        energy[(size_t) i + 1] = energy[(size_t) i] + (double) x[i] * (double) x[i];

    for (int tau = 0; tau <= maxLag; ++tau)
        nsdf[(size_t) tau] = nsdfAt (x, tau, std::max (tau, minWindow));

    // A key maximum only counts if its frequency is within the range (5% above maxFrequency allowed).
    const auto inRange = [this] (int tau) { return decimatedRate / tau <= settings.maxFrequency * 1.05; };

    double highest = -1.0;
    forEachKeyMaximum ([&] (int tau)
    {
        if (inRange (tau))
            highest = std::max (highest, nsdf[(size_t) tau]);
    });

    if (highest < 0.0)
        return {};

    int chosen = -1;
    forEachKeyMaximum ([&] (int tau)
    {
        if (chosen < 0 && inRange (tau) && nsdf[(size_t) tau] >= settings.threshold * highest)
            chosen = tau;
    });

    // Parabola through (-1, y0), (0, y1), (1, y2): vertex at delta = (y0 - y2) / (2 (y0 - 2 y1 + y2)),
    // height y1 - (y0 - y2) delta / 4. All three on the chosen lag's window, so they're comparable.
    const auto w = std::max (chosen, minWindow);
    const auto y0 = nsdfAt (x, chosen - 1, w);
    const auto y1 = nsdfAt (x, chosen, w);
    const auto y2 = nsdfAt (x, chosen + 1, w);
    const auto curvature = y0 - 2.0 * y1 + y2;
    const auto delta = curvature < 0.0 ? juce::jlimit (-1.0, 1.0, 0.5 * (y0 - y2) / curvature) : 0.0;

    Estimate e;
    e.frequency = decimatedRate / (chosen + delta);
    e.clarity = std::min (1.0, y1 - 0.25 * (y0 - y2) * delta);
    return e;
}

} // namespace ampsim

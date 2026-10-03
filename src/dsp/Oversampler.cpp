// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Oversampler.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace ampsim
{

namespace halfband
{
namespace
{
constexpr double pi = juce::MathConstants<double>::pi;

/// Elliptic modulus k and nome q for a normalized transition width (prototypes/halfband.py,
/// nome_from_transition). q = e + 2e^5 + 15e^9 + 150e^13 is the series of the nome in e.
void ellipticParameters (double transition, double& k, double& q)
{
    k = std::pow (std::tan ((1.0 - 2.0 * transition) * pi / 4.0), 2.0);
    const auto kk = std::sqrt (std::sqrt (1.0 - k * k));
    const auto e = 0.5 * (1.0 - kk) / (1.0 + kk);
    const auto e2 = e * e;
    const auto e4 = e2 * e2;
    q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
}

/// One allpass coefficient from the theta-function series (prototypes/halfband.py, coefficient()).
double coefficient (int index, double k, double q, int order)
{
    const auto c = (double) (index + 1);

    double numerator = 0.0;
    for (int i = 0; i <= 100; ++i)
    {
        const auto term = ((i % 2 == 0) ? 1.0 : -1.0) * std::pow (q, (double) (i * (i + 1))) * std::sin ((2 * i + 1) * c * pi / order);
        numerator += term;
        if (std::abs (term) < 1.0e-100)
            break;
    }

    double denominator = 0.0;
    for (int i = 1; i <= 101; ++i)
    {
        const auto term = ((i % 2 == 0) ? 1.0 : -1.0) * std::pow (q, (double) (i * i)) * std::cos (2.0 * i * c * pi / order);
        denominator += term;
        if (std::abs (term) < 1.0e-100)
            break;
    }
    denominator = 1.0 + 2.0 * denominator;

    const auto w = 2.0 * std::pow (q, 0.25) * numerator / denominator;
    const auto ww = w * w;
    const auto x = std::sqrt ((1.0 - ww * k) * (1.0 - ww / k)) / (1.0 + ww);
    return (1.0 - x) / (1.0 + x);
}
} // namespace

std::vector<double> design (int numCoefficients, double transition)
{
    jassert (numCoefficients >= 1 && numCoefficients <= maxCoefficients);
    jassert (transition > 0.0 && transition < 0.5);

    double k = 0.0, q = 0.0;
    ellipticParameters (transition, k, q);
    const int order = 2 * numCoefficients + 1;

    std::vector<double> coefficients ((size_t) numCoefficients);
    for (int i = 0; i < numCoefficients; ++i)
        coefficients[(size_t) i] = coefficient (i, k, q, order);
    return coefficients;
}

std::complex<double> response (const std::vector<double>& coefficients, double frequency)
{
    const auto z = std::polar (1.0, 2.0 * pi * frequency);
    const auto zInv2 = 1.0 / (z * z);
    std::complex<double> a0 (1.0, 0.0), a1 (1.0, 0.0);
    for (size_t i = 0; i < coefficients.size(); ++i)
    {
        const auto section = (coefficients[i] + zInv2) / (1.0 + coefficients[i] * zInv2);
        if (i % 2 == 0)
            a0 *= section;
        else
            a1 *= section;
    }
    return 0.5 * (a0 + a1 / z);
}
} // namespace halfband

// ---- HalfbandStage --------------------------------------------------------------------------

void HalfbandStage::setCoefficients (const std::vector<double>& coefficients)
{
    jassert ((int) coefficients.size() <= halfband::maxCoefficients);
    count = (int) coefficients.size();
    std::copy (coefficients.begin(), coefficients.end(), a.begin());
    reset();
}

void HalfbandStage::reset() noexcept
{
    x.fill (0.0);
    y.fill (0.0);
}

// ---- Up and down chains ---------------------------------------------------------------------

namespace
{
int stagesForFactor (int factor)
{
    jassert (factor == 1 || factor == 2 || factor == 4 || factor == 8);
    return factor >= 8 ? 3 : factor >= 4 ? 2 : factor >= 2 ? 1 : 0;
}

template <typename Stages>
void designStages (Stages& stages)
{
    for (size_t s = 0; s < stages.size(); ++s)
        stages[s].setCoefficients (halfband::design (oversampling::stageDesigns[s].numCoefficients, oversampling::stageDesigns[s].transition));
}
} // namespace

Upsampler::Upsampler() { designStages (stages); }

void Upsampler::setFactor (int factor) noexcept
{
    const auto n = stagesForFactor (factor);
    if (n != numStages)
    {
        numStages = n;
        reset();
    }
}

void Upsampler::reset() noexcept
{
    for (auto& s : stages)
        s.reset();
}

void Upsampler::process (const double* in, double* out, int numSamples) noexcept
{
    if (numStages == 0)
    {
        std::copy (in, in + numSamples, out);
        return;
    }

    const int factor = 1 << numStages;
    for (int n = 0; n < numSamples; ++n)
    {
        // Each stage doubles the samples in time order: ping-pong between two small buffers.
        double bufferA[8], bufferB[8];
        double* from = bufferA;
        double* to = bufferB;
        from[0] = in[n];
        int count = 1;
        for (int s = 0; s < numStages; ++s)
        {
            for (int i = 0; i < count; ++i)
                stages[(size_t) s].up (from[i], to[2 * i], to[2 * i + 1]);
            std::swap (from, to);
            count *= 2;
        }
        std::copy (from, from + factor, out + (size_t) n * (size_t) factor);
    }
}

Downsampler::Downsampler() { designStages (stages); }

void Downsampler::setFactor (int factor) noexcept
{
    const auto n = stagesForFactor (factor);
    if (n != numStages)
    {
        numStages = n;
        reset();
    }
}

void Downsampler::reset() noexcept
{
    for (auto& s : stages)
        s.reset();
}

void Downsampler::process (const double* in, double* out, int numSamples) noexcept
{
    if (numStages == 0)
    {
        std::copy (in, in + numSamples, out);
        return;
    }

    const int factor = 1 << numStages;
    for (int n = 0; n < numSamples; ++n)
    {
        // The highest-rate stage first: each stage halves the samples, in time order.
        double buffer[8];
        std::copy (in + (size_t) n * (size_t) factor, in + (size_t) (n + 1) * (size_t) factor, buffer);
        int count = factor;
        for (int s = numStages - 1; s >= 0; --s)
        {
            for (int i = 0; i < count / 2; ++i)
                buffer[i] = stages[(size_t) s].down (buffer[2 * i], buffer[2 * i + 1]);
            count /= 2;
        }
        out[n] = buffer[0];
    }
}

double oversampling::groupDelaySamples (int factor)
{
    const auto n = stagesForFactor (factor);
    double delay = 0.0;
    for (int s = 0; s < n; ++s)
    {
        const auto coefficients = halfband::design (stageDesigns[(size_t) s].numCoefficients, stageDesigns[(size_t) s].transition);
        double stageDelay = 0.0;
        for (const auto a : coefficients)
            stageDelay += (1.0 - a) / (1.0 + a);
        delay += stageDelay / (double) (1 << s); // stage s runs its pair at 2^s times the base rate
    }
    return delay;
}

} // namespace ampsim

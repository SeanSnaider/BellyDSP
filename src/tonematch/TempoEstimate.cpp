// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TempoEstimate.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ampsim::tonematch
{

namespace
{
constexpr int fftOrder = 11, fftSize = 1 << fftOrder, hopSamples = 480;
constexpr double lowHz = 30.0, highHz = 8000.0, detrendSeconds = 1.0;
constexpr double minBpm = 40.0, maxBpm = 240.0, preferredSeconds = 0.5, preferenceOctaves = 1.4, confidenceNeeded = 0.15;
} // namespace

Tempo estimateTempo (const float* x, int numSamples, double sampleRate, const std::atomic<bool>& cancel)
{
    Tempo out;
    if (numSamples < fftSize || sampleRate <= 0.0)
        return out;

    // 1. The onset strength: each frame's log power per bin, then the sum of the rises since the last frame.
    const auto frames = 1 + (numSamples - fftSize) / hopSamples;
    const auto firstBin = (int) std::ceil (lowHz * fftSize / sampleRate), endBin = (int) std::ceil (highHz * fftSize / sampleRate);
    const auto bins = endBin - firstBin;
    std::vector<float> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / fftSize)); // periodic Hann
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> buffer ((size_t) (2 * fftSize));
    std::vector<double> power ((size_t) frames * (size_t) bins);
    double top = 0.0;
    for (int t = 0; t < frames; ++t)
    {
        if ((t & 63) == 0 && cancel.load())
            return out;
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            buffer[(size_t) i] = x[(size_t) t * hopSamples + (size_t) i] * window[(size_t) i];
        fft.performRealOnlyForwardTransform (buffer.data(), true);
        for (int k = 0; k < bins; ++k)
        {
            const auto re = (double) buffer[(size_t) (2 * (firstBin + k))], im = (double) buffer[(size_t) (2 * (firstBin + k) + 1)];
            const auto p = re * re + im * im;
            power[(size_t) t * (size_t) bins + (size_t) k] = p;
            top = std::max (top, p);
        }
    }
    for (auto& p : power)
        p = std::log10 (1.0e-4 + p / (top + 1.0e-30));
    std::vector<double> onset ((size_t) frames, 0.0);
    for (int t = 1; t < frames; ++t)
        for (int k = 0; k < bins; ++k)
            onset[(size_t) t] += std::max (0.0, power[(size_t) t * (size_t) bins + (size_t) k] - power[(size_t) (t - 1) * (size_t) bins + (size_t) k]);

    // 2. The slow trend off (a 1 s centred moving average), negatives to 0, then the mean off.
    const auto half = (int) std::lround (detrendSeconds * sampleRate / hopSamples / 2.0);
    std::vector<double> sum ((size_t) frames + 1, 0.0), o ((size_t) frames);
    for (int t = 0; t < frames; ++t)
        sum[(size_t) t + 1] = sum[(size_t) t] + onset[(size_t) t];
    for (int t = 0; t < frames; ++t)
    {
        const auto a = std::max (0, t - half), b = std::min (frames, t + half + 1);
        o[(size_t) t] = std::max (0.0, onset[(size_t) t] - (sum[(size_t) b] - sum[(size_t) a]) / (b - a));
    }
    double mean = 0.0;
    for (auto v : o)
        mean += v;
    mean /= frames;
    bool any = false;
    for (auto& v : o)
    {
        v -= mean;
        any = any || v != 0.0;
    }

    // 3. The unbiased autocorrelation over the lags, weighted by the tempo preference; the peak, refined.
    const auto frameSeconds = (double) hopSamples / sampleRate;
    const auto lo = (int) std::floor (60.0 / maxBpm / frameSeconds), hi = (int) std::ceil (60.0 / minBpm / frameSeconds);
    if (frames < 2 * hi || ! any)
        return out;
    std::vector<double> r ((size_t) hi + 2, 0.0), score ((size_t) hi + 2, 0.0);
    for (int k = 0; k < hi + 2; ++k)
    {
        double acc = 0.0;
        for (int t = 0; t + k < frames; ++t)
            acc += o[(size_t) t] * o[(size_t) (t + k)];
        r[(size_t) k] = acc / (frames - k);
        if (k > 0)
        {
            const auto octaves = std::log2 (k * frameSeconds / preferredSeconds) / preferenceOctaves;
            score[(size_t) k] = std::exp (-0.5 * octaves * octaves) * r[(size_t) k];
        }
    }
    auto best = lo + 1;
    for (int k = lo + 1; k < hi; ++k)
        if (score[(size_t) k] > score[(size_t) best])
            best = k;
    const auto a = score[(size_t) best - 1], b = score[(size_t) best], c = score[(size_t) best + 1];
    const auto den = a - 2.0 * b + c;
    const auto delta = den < 0.0 ? 0.5 * (a - c) / den : 0.0;
    out.bpm = 60.0 / ((best + delta) * frameSeconds);
    out.confidence = r[0] > 0.0 ? r[(size_t) best] / r[0] : 0.0;
    out.confident = out.confidence >= confidenceNeeded;
    return out;
}

} // namespace ampsim::tonematch

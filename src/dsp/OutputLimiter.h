// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"

#include <atomic>

namespace ampsim
{

/// The output safety limiter (Sean's play test, 2026-10-04: "the clipping is really high"): the last block
/// in the chain, after the output level, so nothing BellyDSP sends to the interface goes over the ceiling
/// (default -1 dBFS). Stereo-linked, zero latency (no lookahead), and bit-transparent while the signal stays
/// below the knee, 2 dB under the ceiling.
///
/// Per sample, with p = max(|L|, |R|) (one gain for both sides, so the image never moves), c the ceiling
/// and k = c 10^(-2/20) the knee:
///
///  1. The static curve is a soft clipper, identity below the knee and a tanh into the ceiling above it:
///         s(p) = p                                    p <= k
///         s(p) = k + (c - k) tanh((p - k) / (c - k))   p >  k
///     Continuous with slope 1 at the knee (tanh'(0) = 1), and s(p) < c for every p, since tanh < 1.
///     The gain this sample needs is g_req = s(p) / p (1 below the knee).
///
///  2. Instant attack, smoothed release (a peak limiter without lookahead, so without latency):
///         g[n] = min(g_req[n], 1 - (1 - g[n-1]) a),   a = exp(-1 / (tau fs)),  tau = 60 ms
///     When a peak arrives the gain drops to g_req in that same sample, so the attacking samples follow the
///     soft clipper's curve (no flat tops, no overshoot); afterwards the gain recovers toward 1 with a 60 ms
///     time constant (gain reduction falls to 37% in 60 ms, 5% in 180 ms), so the cycles that follow a peak
///     are turned down whole instead of clipped. That is the standard zero-latency compromise: only the
///     first quarter cycle of a new peak is shaped, and it's shaped softly.
///
///  3. Guarantee: g[n] <= g_req[n] = s(p) / p, so |out| = g p <= s(p) < c. The output never reaches the
///     ceiling, whatever comes in (the "never exceeds the ceiling" test drives it 30 dB over).
///
/// Transparency: once the gain is back at exactly 1 (a reduction under 1e-6, -0.00001 dB, snaps to 1) and
/// the signal is under the knee, the samples aren't touched at all, so a signal 3 dB under the ceiling
/// passes bit for bit. The ceiling is smoothed over 20 ms (it moves the knee and the curve, never a gain
/// step). Gain reduction is published for the Out meter's limit light (the largest in the last buffer).
///
/// Sources: the soft-knee peak limiter's structure (gain computer, instant attack, exponential release) is
/// Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor Design: A Tutorial and Analysis", JAES
/// 60(6), 2012, with the ratio taken to infinity; the tanh knee is the usual smooth saturating curve (Zolzer,
/// "DAFX", 2nd ed., ch. 4, soft clipping).
class OutputLimiter : public Block
{
public:
    static constexpr float defaultCeilingDb = -1.0f, minCeilingDb = -12.0f, maxCeilingDb = 0.0f;
    static constexpr double kneeDb = 2.0, releaseSeconds = 0.060, ceilingSmoothingSeconds = 0.020;

    /// Audio thread, once per buffer.
    void setCeilingDb (float db) noexcept;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }

    /// Any thread: the largest gain reduction (dB, >= 0) in the last buffer processed.
    float getReductionDb() const noexcept { return reductionDb.load (std::memory_order_relaxed); }

    /// The static curve's gain for a peak p at ceiling c (linear), for tests.
    static double staticGain (double p, double c) noexcept;

private:
    juce::SmoothedValue<float> ceiling { 0.891250938f };
    double releaseCoefficient = 0.0;
    double gain = 1.0;
    std::atomic<float> reductionDb { 0.0f };
};

} // namespace ampsim

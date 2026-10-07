// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Handoff.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace ampsim
{

/// The match curve (BUILD_PLAN "Match curve"): a smooth, high-resolution magnitude correction after the
/// cab, applied as a minimum-phase FIR with zero added latency. Tone match fits it to what's left between
/// the target and the rig (tone_bench Round 1: it beats the 5-band match EQ), the way a mix engineer's EQ
/// corrects an amp-plus-cab sound.
///
/// The curve is a list of (frequency, dB) points. What the FIR realizes, targetDb(), is that curve
///   1. interpolated linearly in dB over log frequency (held flat past the first and last points),
///   2. clamped to +-maxDb,
///   3. smoothed by a Gaussian whose width is 1/12 octave (sigma), but never narrower than
///      minSmoothingHz (= fs / firLength, 11.7 Hz). The two meet at 203 Hz: above it the resolution is
///      constant-Q (1/12 octave), below it constant in Hz, which is all a 4096-tap FIR can realize (see
///      firLength),
///   4. tapered to 0 dB at the band edges (raised cosines from 25 Hz down to 12.5 Hz and from 18 kHz up to
///      22 kHz), so no boost lands on subsonics or ultrasonics,
///   5. scaled by the amount (0 to 1): dB times amount, exactly. A minimum-phase H with |H|^a has the same
///      shape at a fraction of the depth, so "50%" is half the correction, in dB, at every frequency.
///
/// The FIR is the minimum-phase filter with that magnitude, from the folded real cepstrum (MinimumPhase.h,
/// Oppenheim and Schafer), designed in double precision on a 32768-point grid (8 times the FIR, so the
/// cepstrum doesn't wrap), cut to firLength taps with a raised-cosine fade over the last quarter. Minimum
/// phase puts the filter's energy as early as possible, so it adds no delay (zero latency) and no
/// pre-ringing; a linear-phase FIR of the same magnitude would delay everything by half its length.
///
/// It runs through the same engine as the cab: JUCE's zero-latency uniformly partitioned convolution, with
/// its 50 ms crossfade when the FIR changes. A new curve or amount is designed on the loader thread and
/// handed to the audio thread through Handoff; the audio thread never allocates or frees.
class MatchCurve : public Block
{
public:
    /// The trade-off (prototyped in the scratch study, and measured by the tests): the FIR's frequency
    /// resolution is about fs / N. With the smoothing floor at fs / N, the realized magnitude is within
    /// 0.07 dB of the target for the worst curves tried (+-15 dB alternating every 1/12 octave); at half
    /// that floor the error grows to 2.7 dB. 4096 taps (85 ms) give 1/12 octave above 203 Hz, 1/6 octave at
    /// 100 Hz and 1/3 at 50 Hz. 8192 would move the corner down to 100 Hz for twice the convolution's CPU;
    /// guitar has little below 80 Hz, and tone match's own analysis bins are 5.9 Hz wide.
    static constexpr int firLength = 4096;
    static constexpr int designFftOrder = 15; // 32768 points: 8 x firLength
    static constexpr double designSampleRate = 48000.0;
    static constexpr double maxDb = 15.0;
    static constexpr double smoothingOctaves = 1.0 / 12.0;
    static constexpr double minSmoothingHz = designSampleRate / firLength;
    static constexpr double lowZeroHz = 12.5, lowEdgeHz = 25.0, highEdgeHz = 18000.0, highZeroHz = 22000.0;
    static constexpr int maxPoints = 2048;

    struct Point
    {
        double hz = 0.0, db = 0.0;
        bool operator== (const Point& other) const = default;
    };

    /// The data a match sets and a preset saves. Points are kept sorted by frequency, finite, above 0 Hz,
    /// at most maxPoints (fromPoints() and fromVar() see to that).
    struct Curve
    {
        std::vector<Point> points;

        /// Sorted, invalid points dropped (non-finite, frequency <= 0), points at the same frequency merged
        /// (the last wins), and at most maxPoints (every k-th point beyond that).
        static Curve fromPoints (std::vector<Point> points);

        /// As saved in the state and presets: [[hz, dB], [hz, dB], ...]. Anything else is no curve.
        juce::var toVar() const;
        static Curve fromVar (const juce::var& v);

        /// No points, or every point within 0.001 dB of 0: the FIR would be a plain wire.
        bool isFlat() const;

        bool operator== (const Curve& other) const = default;
    };

    /// What the FIR realizes at these frequencies (steps 1 to 5 above), in dB. Any thread.
    static std::vector<double> targetDb (const Curve& curve, double amount, const std::vector<double>& frequencies);
    static double targetDb (const Curve& curve, double amount, double hz);

    /// The minimum-phase FIR for this curve at this amount (0 to 1), firLength taps at 48 kHz; empty when
    /// the curve is flat or the amount is 0. The block's output is this FIR convolved with its input (the
    /// tests compare it with brute-force convolution), so the offline renderer and tone match can apply it
    /// with any exact convolution. Any thread but the audio thread (it allocates).
    static std::vector<float> designFir (const Curve& curve, double amount);

    MatchCurve();

    /// Loader thread: designs the FIR for this curve and amount and queues it for the audio thread, which
    /// crossfades to it over 50 ms. A flat curve (or amount 0) queues a one-tap identity, so even taking
    /// the curve away crossfades.
    void setCurve (const Curve& curve, double amount);

    /// Any non-audio thread: the last curve and amount given to setCurve(), and the FIR it designed.
    Curve getCurve() const;
    double getAmount() const;
    std::vector<float> getFir() const;

    /// Any non-audio thread. Frees FIR buffers the audio thread has finished with.
    void collectGarbage() { handoff.collect(); }

    /// Audio thread, for tests: whether any FIR has been installed (false: the block is a plain wire, bit
    /// for bit), and whether JUCE has swapped in the engine for the latest one.
    bool hasFir() const noexcept { return installed; }
    bool isEngineReady() const noexcept { return installed && convolution.getCurrentIRSize() == installedLength; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override { convolution.reset(); }
    bool isStereo() const override { return true; }

private:
    struct PendingFir
    {
        juce::AudioBuffer<float> taps;
    };

    void installPending() noexcept;

    Handoff<PendingFir> handoff;
    PendingFir* toRetire = nullptr;
    juce::dsp::Convolution convolution; // zero latency, uniformly partitioned (JUCE's default)
    bool installed = false;
    int installedLength = 0;

    mutable std::mutex designedMutex; // loader and message threads only
    Curve designedCurve;
    double designedAmount = 1.0;
    std::vector<float> designedFir;
};

} // namespace ampsim

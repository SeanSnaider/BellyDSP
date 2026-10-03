// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Svf.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ampsim
{

/// The newest samples of a stream, always readable as one contiguous array: each sample is written twice,
/// at its ring position and one capacity further on, so any stretch of up to `capacity` newest samples
/// sits in memory in time order with no wrap to handle. Allocated in prepare(); push() never allocates.
class ContiguousHistory
{
public:
    void prepare (int minimumCapacity)
    {
        capacity = 1;
        while (capacity < minimumCapacity)
            capacity <<= 1;
        buffer.assign ((size_t) (2 * capacity), 0.0f);
        reset();
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        position = 0;
        total = 0;
    }

    void push (float x) noexcept
    {
        buffer[(size_t) position] = x;
        buffer[(size_t) (position + capacity)] = x;
        position = (position + 1) & (capacity - 1);
        ++total;
    }

    /// The newest `length` samples (length <= capacity), oldest first. Before `length` samples have been
    /// pushed, the missing ones read as zeros.
    const float* newest (int length) const noexcept { return buffer.data() + position + capacity - length; }

    int getCapacity() const noexcept { return capacity; }

    /// Samples pushed since the last reset.
    int64_t getTotal() const noexcept { return total; }

private:
    std::vector<float> buffer;
    int capacity = 0, position = 0;
    int64_t total = 0;
};

/// Coarse, octave-safe pitch detection with the McLeod Pitch Method (Philip McLeod and Geoff Wyvill, "A
/// Smarter Way to Find Pitch", ICMC 2005). The tuner runs it on its analysis thread to find the note and
/// steer its fine stage (TunerAnalysis); the harmonizer will run it on the audio thread every 64 samples
/// (BUILD_PLAN "Harmonizer"). It allocates only in prepare(), so push() and detect() are real-time safe.
/// prototypes/pitch_detection.py is the reference, and the study there is why it's built this way.
///
/// 1. Decimation. The input is low-passed by an 8th-order Butterworth at 3 kHz (four SVF sections,
///    Q_k = 1 / (2 sin((2k - 1) pi / 16))) and every 4th sample is kept: 12 kHz at a 48 kHz input. Guitar
///    fundamentals stay below about 1.3 kHz, so this loses nothing the detector needs and cuts the work
///    16x (4x fewer samples times 4x fewer lags). Phase doesn't matter for pitch, so a cheap IIR is fine.
///    Aliases are down 48 dB at 6 kHz and 76 dB at 9 kHz. The 3 kHz cutoff also drops a stiff string's
///    stretched upper partials, which pull the waveform's periodicity sharp.
///
/// 2. The normalized square difference function (NSDF), at every lag tau up to the longest period:
///        n(tau) = 2 r(tau) / m(tau),   r = sum_j x_j x_{j-tau},   m = sum_j (x_j^2 + x_{j-tau}^2)
///    over j in the newest W(tau) samples. n is in [-1, 1] (because 2ab <= a^2 + b^2), and n = 1 means
///    the signal repeats exactly after tau samples. Unlike plain autocorrelation it doesn't favour short
///    lags, and unlike YIN's difference function it needs no cumulative normalization.
///    The window is W(tau) = max(tau, W_min): each candidate period is judged on two of its own periods
///    (at least W_min). With W_min longer than every lag (the tuner: 35 ms) it's the classic fixed window,
///    anchored at the newest sample. With a short W_min (the harmonizer: 3 ms) a high note is found after
///    two of its own periods instead of two periods of the lowest note: the high E reads in about 6 ms
///    instead of 12, the G in 10 instead of 14 (prototype latency study, and the "Tuner" tests). The cost
///    is more variance at short lags: noise reaches a clarity of 0.72 instead of 0.46 (still under 0.9).
///
/// 3. Peak picking (McLeod and Wyvill, section 5). Skip the lobe around lag 0; in every later positive
///    lobe take its highest point (a "key maximum"); choose the first key maximum that reaches k = 0.93
///    times the highest one. Taking the first good one rather than the best one is what avoids octave
///    errors: a period of 2T fits a signal of period T just as well, and a strong 2nd harmonic makes a
///    peak at T/2 that doesn't reach the threshold.
///
/// 4. Parabolic interpolation through the chosen lag and its neighbours (all three on the chosen lag's
///    window) gives the period to a fraction of a sample; the parabola's peak height is the clarity, the
///    confidence measure in [0, 1]. At 12 kHz a 1.3 kHz period is only 9 samples, which is why the tuner
///    adds a fine stage; the coarse estimate is within about 10 cents (MPM median 1.4 cents in the study).
///
/// Cost: about sum over tau of W(tau) multiply-adds per detect(): 181k for the tuner, 6.9k for the
/// harmonizer preset. Measured in the "Tuner" tests.
class PitchDetector
{
public:
    struct Settings
    {
        double minFrequency = 28.0;       ///< Hz. The longest period searched is fs_d / minFrequency + 2 samples.
        double maxFrequency = 1400.0;     ///< Hz. Key maxima above 1.05 x this are ignored.
        double minWindowSeconds = 0.035;  ///< W_min. Each lag compares max(lag, W_min) newest samples.
        double threshold = 0.93;          ///< MPM's k: the first key maximum above k x the highest wins.

        /// The tuner: down to 28 Hz (a B0 string 50 cents flat still reads), fixed 35 ms window, so the
        /// whole analysis spans two periods of 30 Hz (72 ms at 12 kHz).
        static Settings tuner() noexcept { return {}; }

        /// The harmonizer: a 110 Hz floor (open A, BUILD_PLAN default) and a 3 ms minimum window, the knee
        /// between speed and noise (prototype study: 2 ms is no faster on the open strings, 4 ms costs the
        /// high E a millisecond).
        static Settings harmonizer() noexcept { return { 110.0, 1400.0, 0.003, 0.93 }; }
    };

    struct Estimate
    {
        double frequency = 0.0; ///< Hz, 0 when no pitch was found.
        double clarity = 0.0;   ///< Height of the chosen NSDF peak: 1 is perfectly periodic, noise stays below about 0.9.
    };

    static constexpr int decimation = 4;
    static constexpr double lowpassHz = 3000.0;

    /// Allocates. inputSampleRate is the rate push() receives (48 kHz in the app).
    void prepare (double inputSampleRate, const Settings& newSettings);

    /// Clears the filters and the history (zeros). Real-time safe.
    void reset() noexcept;

    /// Low-passes and decimates input into the history. Real-time safe; any block size.
    void push (const float* input, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
            pushSample (input[i]);
    }

    void pushSample (float x) noexcept
    {
        auto y = (double) x;
        for (auto& section : lowpass)
            y = section.processSample (y);

        if (++phase == decimation) // keep the 4th, 8th, ... sample
        {
            phase = 0;
            history.push ((float) y);
        }
    }

    /// Runs MPM on the newest frame. Real-time safe (no allocation). Call as often as needed: the
    /// harmonizer every 64 input samples, the tuner once per display update.
    Estimate detect() noexcept;

    const Settings& getSettings() const noexcept { return settings; }
    double getDecimatedRate() const noexcept { return decimatedRate; }
    int getMaxLag() const noexcept { return maxLag; }
    int getMinWindow() const noexcept { return minWindow; }

    /// Decimated samples the analysis looks at: max(maxLag + 1, W_min) + maxLag + 1.
    int getFrameLength() const noexcept { return frameLength; }

    /// The same in seconds of input.
    double getFrameSeconds() const noexcept { return frameLength / decimatedRate; }

private:
    /// n(tau) on the current frame (x, its prefix energies) with window w.
    double nsdfAt (const float* x, int tau, int w) const noexcept;

    template <typename Fn>
    void forEachKeyMaximum (Fn&& fn) const noexcept;

    Settings settings;
    double decimatedRate = 12000.0;
    int maxLag = 0, minWindow = 1, frameLength = 0;

    std::array<Svf, 4> lowpass;
    int phase = 0;
    ContiguousHistory history;

    std::vector<double> nsdf;   // n(tau), tau = 0..maxLag
    std::vector<double> energy; // energy[i] = sum of x_j^2 for j < i over the frame (prefix sums)
};

} // namespace ampsim

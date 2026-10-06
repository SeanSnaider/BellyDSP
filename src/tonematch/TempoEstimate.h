// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <atomic>

namespace ampsim::tonematch
{

/// The section's tempo, for the play-along count-in's suggestion (docs/TONE_MATCH.md, "Play along"). A port
/// of prototypes/tempo_estimate.py (golden-tested against it), after Ellis, "Beat tracking by dynamic
/// programming", J. New Music Research 36(1), 2007, section 3.1:
///   1. onset strength: the spectral flux of log power (2048-point Hann, 10 ms hop, 30 Hz to 8 kHz; each
///      bin's rise since the last frame, falls ignored), log10(1e-4 + |X|^2 / max |X|^2);
///   2. minus its 1 s centred moving average, negatives set to 0, then its mean removed;
///   3. its unbiased autocorrelation r at lags 0.25 to 1.5 s (240 to 40 BPM), weighted by a log-Gaussian
///      preference around 0.5 s (120 BPM) with 1.4 octaves' deviation, the largest picked and refined by a
///      parabola through its neighbours;
///   4. confident when r there is at least 0.15 r(0).
/// On a solo guitar line it can land on half or double the beat (still in time for a count-in) or on a
/// dotted grouping (4:3, not); on a band it was exact. Runs on any thread but the audio thread; cancelable.
struct Tempo
{
    double bpm = 0.0;
    double confidence = 0.0; ///< r at the chosen lag over r(0)
    bool confident = false;
};

Tempo estimateTempo (const float* x, int numSamples, double sampleRate, const std::atomic<bool>& cancel);

} // namespace ampsim::tonematch

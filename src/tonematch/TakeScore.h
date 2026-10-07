// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "InformedMask.h"
#include "ToneMatchAnalysis.h"

#include <array>
#include <vector>

/// Tone match's take-aware score (docs/TONE_MATCH.md, "Round 2: the objective"; a port of prototypes/tone_match.py,
/// take_score, its golden reference: tests/ToneMatchTakeScoreTests.cpp holds this to its numbers).
///
/// tone_bench Round 1 found the old score prefers the wrong configuration: given another performance of the same
/// notes it fits the playing as much as the rig, because its distortion statistics are taken over the whole signal
/// and move with the player's dynamics. A play-along take plays the target's notes, so the two are paired note by
/// note (informed::alignNotes) and compared on the benchmark's own perceptual scale. The score, every term in dB:
///
///     S = E + 7.5 |d flux| + 4 |d crest| + 14 A + 2.5 |d spread| + 6.5 D_erb
///
///   E        the spectral error after the linear tone fit (the old Candidate's spectral)
///   flux     the median frame-to-frame level change, crest the median crest factor (Analysis::features 1 and 3)
///   A        per note pair, the share of the note's energy (its first 300 ms, or to the next note) in its first
///            15 ms, dB: the absolute difference between the target's note and the take's through the amp, weighted
///            by each target note's energy^0.3. How a note's onset stands out over what follows is what compression
///            changes; the cab's few-ms IR barely moves it.
///   spread   the standard deviation of the paired notes' levels: the target's against the candidate's
///   D_erb    the loudness-weighted long-term spectral distance on the ERB scale (one band per ERB, 50 Hz to 15 kHz,
///            Glasberg and Moore 1990; each band weighted by its share of the target's specific loudness, Zwicker's
///            0.23 power law over the PEAQ outer- and middle-ear weighting of ITU-R BS.1387 and Terhardt's 1979
///            threshold, the loudest band at 85 dB SPL), after the candidate's whole linear part
/// The weights were fitted on tone_bench's DEV candidate pools and rounded (prototypes/tone_bench/pool.py).
/// Offline (worker threads); allocates freely.
namespace ampsim::tonematch::take
{

constexpr int erbFftOrder = 12;
constexpr int erbFftSize = 1 << erbFftOrder;   // 4096
constexpr int erbBins = erbFftSize / 2 + 1;
constexpr int erbHop = 1024;
constexpr int noteMaxSamples = 14400;          // 300 ms
constexpr int noteMinSamples = 2048;
constexpr int attackSamples = 720;             // 15 ms
constexpr int minNotes = 8;
constexpr int shortlist = 16;                  // candidates fitted completely and scored this way

struct Weights
{
    double flux = 7.5, crest = 4.0, attack = 14.0, spread = 2.5, erb = 6.5;
};

/// The ERB bands: centres, the contiguous bins [first, last) of the 4096-point spectrum within half an ERB of
/// each centre (the nearest bin if none), and the ear's weighting and threshold in quiet at each centre.
struct ErbBands
{
    std::vector<double> centre, outerEarDb, thresholdDb;
    std::vector<int> first, last;
    int size() const noexcept { return (int) centre.size(); }
};
const ErbBands& erbBands();

/// The long-term power per bin of the 4096-point STFT (symmetric Hann, hop 1024, frames from the start while
/// they fit; a shorter signal is zero-padded to one frame), over the frames whose level (dB of the frame's
/// total power) is within 30 dB of its 95th percentile.
std::vector<double> ltasBins (const float* x, int numSamples);
inline std::vector<double> ltasBins (const std::vector<float>& x) { return ltasBins (x.data(), (int) x.size()); }

/// Band levels (dB) of per-bin power, each bin times filterPower[k] if given.
std::vector<double> bandsDb (const std::vector<double>& bins, const std::vector<double>* filterPower = nullptr);

/// Each band's share of the specific loudness (sum 1): N'_b = max(0, 10^(0.023 (L_b + A_b + P - T_b)) - 1),
/// P putting the loudest band (with A) at 85 dB SPL.
std::vector<double> loudnessWeights (const std::vector<double>& bandDb);

/// sqrt(sum_b w_b (d_b - dbar)^2), d = ref - m, dbar = sum_b w_b d_b (sum w = 1).
double loudnessDistance (const std::vector<double>& ref, const std::vector<double>& m, const std::vector<double>& w);

/// |H|^2 of the amp's tone bands and the match EQ's bands on the 4096-point spectrum's bins.
std::vector<double> linearPower (const std::array<double, 5>& tone, const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq);

/// Per note: the attack (10 log10 of the first 15 ms's energy over the note's, + 1e-6) and the level (dB).
struct NoteMeasures
{
    std::vector<double> attack, level;
};
NoteMeasures noteMeasures (const std::vector<float>& x, const std::vector<int>& starts, const std::vector<int>& lengths);

/// The paired notes (notes from informed::alignNotes; each kept with min(length, 300 ms) >= 2048 samples) and the
/// target's measures.
struct TakeNotes
{
    std::vector<int> takeOnsets, targetOnsets, lengths;
    NoteMeasures target;
    std::vector<double> noteWeights, erbDb, erbWeights;
    bool usable() const noexcept { return (int) takeOnsets.size() >= minNotes; }
    static TakeNotes of (const std::vector<float>& target, const std::vector<informed::Note>& notes);
};

struct Score
{
    double total = 0.0, spectral = 0.0, flux = 0.0, crest = 0.0, attack = 0.0, spread = 0.0, erb = 0.0;
};

/// S for one candidate: ampRender the take through the amp (no cab), candidate the same through the cab, tone and
/// eq its linear part after the cab (eq may be null), spectral its old spectral error.
Score score (const TakeNotes& notes, const Analysis& target, const std::vector<float>& ampRender, const std::vector<float>& candidate,
             const std::array<double, 5>& tone, const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq, double spectral,
             const Weights& weights = {});

} // namespace ampsim::tonematch::take

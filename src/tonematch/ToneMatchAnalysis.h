// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "../dsp/Equalizer.h"

#include <array>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

/// Tone match, the analysis half (docs/TONE_MATCH.md): spectra, bands, the distortion features, chroma
/// and DTW for same-part alignment, the knob fits, and the optimizer. A line-by-line port of the Python
/// prototype (prototypes/tone_match.py), which is the golden reference: tests/ToneMatchTests.cpp holds
/// this code to the prototype's numbers on the same inputs (tests/fixtures/tone_match).
///
/// Everything here is offline and runs on a worker thread: it allocates freely and never touches the
/// audio thread.
namespace ampsim::tonematch
{

constexpr double sampleRate = 48000.0;
constexpr int fftOrder = 13;
constexpr int fftSize = 1 << fftOrder;      // 8192: 5.9 Hz bins, so the 80 Hz sixth-octave band still gets one
constexpr int numBins = fftSize / 2 + 1;
constexpr int hop = 2048;                   // 43 ms
constexpr double activeRangeDb = 30.0;      // frames within 30 dB of the 95th percentile count as playing
constexpr double coarseLo = 80.0, coarseHi = 12000.0, coarseStep = 1.0 / 6.0; // sixth-octave bands
constexpr int numBands = 44;                // 80 Hz * 2^(i/6), i = 0 .. 43 (up to 11.5 kHz)
constexpr double toneRangeDb = 12.0;        // AmpTone::rangeDb
constexpr double eqCapDb = 12.0;            // the match EQ is capped at +-12 dB
constexpr double toneRidge = 0.01;          // dB^2 of error per dB^2 of knob
constexpr double eqRidge = 0.005;
constexpr double confidentDb = 20.0, ignoredDb = 35.0, minConfidence = 0.05;
constexpr double lambda = 0.5;              // weight of the distortion features against the spectral error
constexpr double envelopeScaleDb = 1.0;     // same part: 1 dB of envelope difference is one feature unit
constexpr double scoreScaleDb = 6.0;        // closeness = 100 exp(-E / 6 dB)
constexpr int numFeatures = 4;
constexpr std::array<double, numFeatures> featureScales { 2.0, 0.4, 1.5, 0.5 };

/// The sixth-octave bands: centres, the contiguous bin range [first, last) each one averages (a band too
/// narrow for a bin takes the bin nearest its centre), and the perceptual weights.
struct Bands
{
    std::array<double, numBands> centre {};
    std::array<int, numBands> first {}, last {};
    std::array<double, numBands> weight {};
};
const Bands& bands();

/// The A-weighting curve (IEC 61672) in dB, mapped to a factor and floored at 0.25: the weight of a
/// band's dB error. A rough stand-in for loudness-weighted error.
double perceptualWeight (double f);

/// [1/4, 1/2, 1/4] across neighbouring bands (the ends keep their value): about third-octave smoothing.
/// Every spectral comparison goes through it, so fits follow the tone rather than the notes played.
std::vector<double> smoothBands (const std::vector<double>& r);

/// A band's error weight for a target with this long-term spectrum: the perceptual weight times a
/// confidence, 1 within 20 dB of the loudest band, 5% at 35 dB or more below it, linear between (a lead
/// line has nothing below its lowest note; a difference there says what was played, not the rig).
std::vector<double> bandWeights (const std::vector<double>& ltas);

double weightedMean (const std::vector<double>& v, const std::vector<double>& w);
/// sqrt(sum w (r - mean_w r)^2 / sum w): the error left once the levels are matched, dB.
double weightedRmsCentred (const std::vector<double>& r, const std::vector<double>& w);

/// numpy's default percentile (linear interpolation between the sorted samples), p in [0, 100].
double percentile (std::vector<double> v, double p);

/// Everything the matcher needs from one signal (48 kHz mono). See prototypes/tone_match.py, Analysis.
///   ltasBins  long-term power per FFT bin (mean over playing frames)
///   ltas      the same in the sixth-octave bands, dB
///   frameDb   every frame's band levels, dB (numFrames x numBands, row-major)
///   level     every frame's level whitened by the long-term spectrum, dB (no filter changes it)
///   features  level spread (90th - 10th percentile of level), level flux (median frame-to-frame
///             change), brightness movement (std of the whitened 2-8 kHz minus 150-800 Hz level), crest
///             factor (median peak over RMS of the playing frames, dB)
///   chroma    12 pitch-class values per frame, unit length (same-part alignment)
struct Analysis
{
    int numFrames = 0;
    std::vector<double> ltasBins, ltas, weights, level, frameDb, chroma;
    std::vector<uint8_t> active;
    std::array<double, numFeatures> features {};
    bool ok = false; ///< false when there's too little playing to analyse (fewer than 4 frames)

    static Analysis of (const float* x, int numSamples);
    static Analysis of (const std::vector<float>& x) { return of (x.data(), (int) x.size()); }

    const double* frame (int t) const { return frameDb.data() + (size_t) t * numBands; }
};

/// sum |ft - fc| / scale over the four features.
double nonlinearDistance (const std::array<double, numFeatures>& target, const std::array<double, numFeatures>& candidate);

/// Dynamic time warping (Sakoe and Chiba 1978) on the cosine distance between chroma frames: steps
/// (1,1), (1,0), (0,1), both ends anchored, ties going diagonal, then up, then left.
struct Alignment
{
    std::vector<std::pair<int, int>> path; ///< (target frame, candidate frame) from the start to the end
    double meanCost = 0.0;
};
/// bandFrames > 0: only within a Sakoe-Chiba band (Sakoe and Chiba 1978, section IV) of that many frames
/// around the straight line from the first frames to the last (bandLimits), for a play-along take, which
/// is recorded lined up with the target (docs/TONE_MATCH.md, "Play along"). 0: unconstrained.
Alignment align (const Analysis& target, const Analysis& candidate, int bandFrames = 0);

/// The band's columns for each of n rows against m columns: |j - i (m-1)/(n-1)| <= w, where w is
/// bandFrames widened to at least the line's slope (and 1) so the steps can always follow the line;
/// the first row starts at column 0 and the last ends at m-1. bandFrames <= 0: every column.
std::pair<std::vector<int>, std::vector<int>> bandLimits (int n, int m, int bandFrames);

/// A play-along take's band (prototypes/tone_match.py, PLAY_ALONG_BAND_SECONDS): half a second either
/// side of the recorded alignment.
inline constexpr double playAlongBandSeconds = 0.5;

/// Same part: the frame-wise comparison along an alignment, over pairs where both frames are playing.
///   residual   smoothed mean of the band dB differences (what the linear part must fit)
///   envelope   RMS of the whitened level difference with its mean removed, dB
struct AlignedDifference
{
    std::vector<double> residual;
    double envelope = 0.0;
    int pairs = 0;
};
AlignedDifference alignedDifference (const Analysis& target, const Analysis& candidate, const Alignment& alignment);

// ---- The chain's linear blocks, exactly (Svf.h) -----------------------------------------------------

/// dB response of the amp's five tone bands (AmpTone::bands) at these gains, at the band centres.
std::array<double, numBands> toneDb (const std::array<double, 5>& gains);

/// dB response of the post EQ's parametric bands at the band centres (no cuts).
std::array<double, numBands> eqDb (const std::array<Equalizer::Band, Equalizer::numParametricBands>& eq);

// ---- Fits ----------------------------------------------------------------------------------------------

/// Nelder and Mead (1965) with the standard coefficients, bounds by clamping. Deterministic.
std::pair<std::vector<double>, double> nelderMead (const std::function<double (const std::vector<double>&)>& fn,
                                                   std::vector<double> x0, const std::vector<double>& step,
                                                   const std::vector<double>& lo, const std::vector<double>& hi,
                                                   int iterations, double tolerance);

/// sqrt(weighted mean square of the level-matched residual after the tone + toneRidge |theta|^2), dB.
double toneError (const std::vector<double>& r, const std::array<double, 5>& theta, const std::vector<double>& w);

/// The five tone knobs for the dB residual r (target minus candidate): ridge least squares on the
/// linearized bands, clamped to +-12 dB, and if polish, Nelder-Mead on the exact responses.
/// Returns the knobs and toneError.
std::pair<std::array<double, 5>, double> fitTone (const std::vector<double>& r, const std::vector<double>& w, bool polish);

/// The match EQ: the post EQ's five parametric bands (low shelf, three peaks, high shelf) fitted to the
/// (smoothed) residual, scaled toward 0 dB by each band's confidence and capped at +-12 dB. Returns the bands and the capped curve they were fitted to.
std::pair<std::array<Equalizer::Band, Equalizer::numParametricBands>, std::vector<double>> fitMatchEq (const std::vector<double>& residual,
                                                                                                         const std::vector<double>& w);

} // namespace ampsim::tonematch

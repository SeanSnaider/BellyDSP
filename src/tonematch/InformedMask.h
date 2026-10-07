// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Progress.h"
#include "ToneMatchAnalysis.h"

#include <atomic>
#include <cstdint>
#include <vector>

/// Cleaning up the target with the player's take (docs/TONE_MATCH.md, "Cleaning up the target with your
/// take"): an informed harmonic mask. When the player played the target's part (a play-along take, or a
/// same-part DI), the take says which notes the lead plays and when. A monophonic note through any amp has
/// its energy at the harmonics of its fundamental, so on the target's spectrogram this keeps what lies near
/// h f0 of the notes sounding and turns everything else down 20 dB: the drums, the bass, and another guitar
/// between the lead's harmonics. What sits on top of a harmonic stays.
///
/// A port of prototypes/learn_tone.py (align_notes, informed_mask; the prototype is the golden reference:
/// tests/InformedMaskTests.cpp holds this code to its numbers on tests/fixtures/informed_mask). Steps:
///   1. The take's onsets: spectral flux (Bello et al. 2005) over a moving-median threshold, each refined to
///      the sample at the steepest rise of a 1 ms envelope (detectOnsets).
///   2. Each note's pitch from the DI: McLeod's method (PitchDetector, the harmonizer's settings) every 10 ms,
///      the median of the confident readings (noteF0).
///   3. Each note into the target's time: chroma DTW between the target and the take through a plain
///      distortion (within the play-along band for a take), then the note's own onset in the target, the flux
///      over the bins near its first 12 harmonics, within +-60 ms (alignNotes).
///   4. Each note's pitch refined against the target within +-60 cents (refinePitchInTarget), the mask built
///      (harmonicMask) and applied on a 4096-point STFT, then the inverse STFT (applyMask).
///
/// Offline: allocates freely, runs on a worker (the session's), never the audio thread. Cancelable.
namespace ampsim::tonematch::informed
{

// The take's onsets: 1024-point frames every 120 samples (2.5 ms).
constexpr int onsetFftOrder = 10;
constexpr int onsetFftSize = 1 << onsetFftOrder;
constexpr int onsetBins = onsetFftSize / 2 + 1;
constexpr int onsetHop = 120;

// The mask's STFT: 4096 points (11.7 Hz bins, so neighbouring harmonics of a low E separate), hop 512.
constexpr int maskFftOrder = 12;
constexpr int maskFftSize = 1 << maskFftOrder;
constexpr int maskBins = maskFftSize / 2 + 1;
constexpr int maskHop = 512;

struct MaskSettings
{
    int harmonics = 40;      ///< h = 1 .. 40, and never above 0.45 fs
    double cents = 35.0;     ///< each harmonic's lobe: a Gaussian with sigma = h f0 (2^(cents/1200) - 1)
    double floor = 0.1;      ///< what isn't near a harmonic: turned down 20 dB, not removed
    /// The mask also smoothed along time, a moving average over 2 timeSmooth + 1 frames (the app's 2, 53 ms;
    /// 0: the mask exactly as the prototype's study ran it). Measured in the prototype (mask_study): +0.02 to
    /// +0.08 dB SI-SDR on all four cases, the spectral distance unchanged.
    int timeSmooth = 2;
};

/// One note of the take and its partner in the target (samples). o: the take's onset; oNext: its next
/// onset (or the end); t, tNext: the same in the target; length: what both have before either one's next
/// note; f0: the pitch from the DI (0 if none); f0Target: refined against the target (harmonicMask).
struct Note
{
    int o = 0, oNext = 0, t = 0, tNext = 0, length = 0;
    double f0 = 0.0, f0Target = 0.0;
};

// ---- The pieces (public for the golden tests) ---------------------------------------------------------

/// Onset strength: the positive part of each bin's log-magnitude rise since the previous frame, summed over
/// the bins (100 Hz to 10 kHz, or the given ones: onsetBins flags), per 2.5 ms frame. Frame t covers samples
/// [t hop - 512, t hop + 512) (zeros outside x), a symmetric Hann window (numpy's hanning). The log is
/// log(|X| + 1e-4 max |X|), the maximum over every frame and bin of this x, so the flux is level-free.
/// cancel: checked every 256 frames (an empty result if set).
std::vector<double> spectralFlux (const float* x, int numSamples, const std::vector<uint8_t>* bins = nullptr,
                                  const std::atomic<bool>* cancel = nullptr);

/// The flux's peaks over a moving-median threshold (the median of +-40 frames, zeros beyond the ends, as
/// scipy's medfilt, plus 10% of the flux's 99th percentile; Bello et al. 2005, section III), at least 45 ms
/// apart (scipy's find_peaks: the higher peak wins), as sample positions (frame times).
std::vector<int> fluxPeaks (const std::vector<float>& di, const std::atomic<bool>* cancel = nullptr);

/// The sample near a guess where the energy rises fastest: a 1 ms (48-sample) RMS envelope e, the largest
/// e[n] - e[n - 48] within +-radius, moved back half a window. A pluck's burst starts there.
int refineOnset (const std::vector<float>& x, int guess, int radius = 480);

/// The take's note onsets: the flux peaks, each refined to the sample.
std::vector<int> detectOnsets (const std::vector<float>& di, const std::vector<int>& peaks);

/// How far a flux peak sits from its sample-exact onset (the median over the peaks, samples; negative: the
/// flux peaks early, since a frame's time is its centre). The target's flux peaks are corrected by it.
int fluxLag (const std::vector<int>& peaks, const std::vector<int>& onsets);

/// Each note's pitch from the DI: McLeod's method (PitchDetector::Settings::harmonizer(), on the 3 kHz
/// low-passed, 12 kHz signal) every 10 ms from 20 ms after the onset to the note's end, the median of the
/// readings with clarity 0.9 or more. 0 if none qualify.
std::vector<double> noteF0 (const std::vector<float>& di, const std::vector<int>& onsets, const std::vector<int>& ends,
                            const std::atomic<bool>* cancel = nullptr);

/// The onset flux's bins near the first `harmonics` harmonics of f0: |f - h f0| <= tolerance h f0 + 0.6 bins.
/// f0 <= 0: 100 Hz to 10 kHz.
std::vector<uint8_t> harmonicBins (double f0, int harmonics = 12, double tolerance = 0.03);

/// What alignNotes found on the way (for the tests and the result's text).
struct AlignmentDetail
{
    std::vector<int> peaks, onsets, ends, targetOnsets, targetNext;
    std::vector<double> f0, estimates;
    int lag = 0;
    Alignment path;
};

/// The take's notes and their partners in the target (prototypes/learn_tone.py, align_notes). bandFrames:
/// DTW's Sakoe-Chiba band in analysis frames (a play-along take: ceil(0.5 s / hop) = 12; 0: unbanded).
/// Empty if the take has no onsets (or cancelled).
std::vector<Note> alignNotes (const std::vector<float>& di, const std::vector<float>& target, int bandFrames,
                              const std::atomic<bool>& cancel, AlignmentDetail* detail = nullptr);

/// The note's pitch as the target plays it: f0 2^(c/1200) for c = -60 .. +60 cents in 5-cent steps, the c
/// whose first 8 harmonics hold the most magnitude in the target's note (a 16384-point spectrum from 20 ms
/// after its onset). The record may be tuned a little differently from the player's guitar.
double refinePitchInTarget (const std::vector<float>& target, const Note& note);

/// The mask, frame-major (frames x maskBins). Frame k is centred on sample k hop.
struct Mask
{
    int frames = 0;
    std::vector<float> values;
    float at (int frame, int bin) const noexcept { return values[(size_t) frame * maskBins + (size_t) bin]; }
    static int framesFor (int numSamples) { return (numSamples + maskHop - 1) / maskHop + 1; }
};

/// M(k, f) = max over the notes whose span [t, tNext) overlaps frame k's window and their harmonics h of
///     exp(-1/2 ((f - h f0) / sigma_h)^2),   sigma_h = max(h f0 (2^(cents/1200) - 1), 1.5 bins),
/// at least the floor, then smoothed along time (settings.timeSmooth) and floored again. Fills each note's
/// f0Target. Notes without a pitch add nothing.
Mask harmonicMask (const std::vector<float>& target, std::vector<Note>& notes, const MaskSettings& settings, const std::atomic<bool>& cancel);

/// The target through the mask: STFT (periodic Hann, 4096 points, hop 512, zeros beyond the ends), each
/// frame times its mask, inverse FFT, windowed again and overlap-added, divided by the sum of the squared
/// windows (the least-squares inverse STFT, Griffin and Lim 1984; scipy's istft). Empty if cancelled.
std::vector<float> applyMask (const std::vector<float>& target, const Mask& mask, const std::atomic<bool>& cancel);

// ---- The whole cleanup ----------------------------------------------------------------------------------

struct Result
{
    bool ok = false, cancelled = false;
    juce::String error;
    std::vector<float> output; ///< the cleaned target, the target's length
    int onsets = 0, notes = 0, pitched = 0;
    double keptDb = 0.0;       ///< the output's energy relative to the target's (dB; how much was turned down)
    double seconds = 0.0;
};

/// The target cleaned up with the take (both 48 kHz mono; the take lined up with the target: a play-along
/// take as trimmed, or a same-part DI). bandSeconds > 0: DTW only that far either side of the diagonal (a
/// play-along take); 0: unbanded. Fails (ok false, error said) if the take has no notes with a pitch.
Result cleanUp (const std::vector<float>& target, const std::vector<float>& take, double bandSeconds, const std::atomic<bool>& cancel,
                const ProgressFn& progress = {}, const MaskSettings& settings = {});

} // namespace ampsim::tonematch::informed

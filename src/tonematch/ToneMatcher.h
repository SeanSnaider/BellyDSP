// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "ToneMatchAnalysis.h"

#include "../dsp/NamAmp.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

/// Tone match, the search (docs/TONE_MATCH.md; a port of prototypes/tone_match.py, match()).
///
/// Finds the amp slot, Gain (the slot's input trim), five tone knobs, built-in cab, and a match EQ (the
/// post EQ's parametric bands) that make the player's DI sound like a target. Only the slot and Gain
/// need renders, because everything after the amp model is linear and time-invariant: each candidate
/// (slot, Gain) is rendered through a private amp section (the chain's own AmpSection block, with the
/// capture in its first slot, exactly as the app runs it), the cabs are applied by FFT convolution, and
/// the tone and EQ are fitted on spectra. Steps:
///   1. every slot at Gain -18 .. +18 dB in 6 dB steps, rendered in parallel;
///   2. per (slot, Gain), the cabs screened on predicted spectra and the best four scored for real;
///   3. Gain refined (3 dB, then 1.5 dB steps) for the best two slots;
///   4. the best candidate's tone knobs polished, then the match EQ fitted to what's left.
/// The score of a candidate is its spectral error (dB) plus lambda times its distortion distance.
///
/// Runs on the calling thread plus a pool of workers it starts and joins; never the audio thread, never
/// the live processor. Cancelable at any point through the flag; progress goes to the callback (on
/// whichever thread is working, so a GUI must hop to the message thread itself).
namespace ampsim::tonematch
{

enum class Mode
{
    anything, ///< the player played anything: long-term statistics only
    samePart  ///< the player played the same part: DTW-aligned, frame by frame
};

struct MatchSettings
{
    Mode mode = Mode::anything;
    std::array<juce::File, 3> models;   ///< the slots' captures; an empty File leaves that slot out
    std::vector<juce::File> cabs;       ///< the cab IRs to choose from (the built-in 21 in the app)
    NamAmp::Calibration calibration;    ///< as the app loads its captures
    int threads = 0;                    ///< 0: the machine's cores minus one
    std::array<double, 7> gainGrid { -18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0 };
    std::array<double, 2> refineSteps { 3.0, 1.5 };
    double gainMin = -24.0, gainMax = 24.0; ///< amp*_input_trim's range
    int cabsPerAmp = 4;
    int refineSlots = 2;
};

struct MatchResult
{
    bool ok = false, cancelled = false;
    juce::String error;
    Mode mode = Mode::anything;

    int slot = 0;
    double gainDb = 0.0;
    std::array<double, 5> tone {};
    juce::File cab;
    std::array<Equalizer::Band, Equalizer::numParametricBands> eq {};

    double spectralErrorDb = 0.0;        ///< after the tone, before the match EQ
    double spectralErrorAfterEqDb = 0.0; ///< after the match EQ
    double distortion = 0.0;             ///< distortion distance (feature units)
    double closeness = 0.0;              ///< 100 exp(-(spectral after EQ + lambda distortion) / 6 dB)
    std::array<double, numFeatures> targetFeatures {}, resultFeatures {};
    std::vector<double> residual;        ///< per band, after the tone: what the match EQ fits
    std::vector<double> eqTarget;        ///< the capped curve the EQ was fitted to
    double runtimeSeconds = 0.0;
    int renders = 0, candidates = 0;

    struct RunnerUp
    {
        int slot;
        double gainDb;
        juce::File cab;
        double score;
    };
    std::vector<RunnerUp> runnersUp;
};

using ProgressFn = std::function<void (double fraction, const juce::String& stage)>;

class ToneMatcher
{
public:
    /// target and reference: 48 kHz mono. Blocks until done or cancelled.
    static MatchResult match (const std::vector<float>& target, const std::vector<float>& reference, const MatchSettings& settings,
                              const std::atomic<bool>& cancel, const ProgressFn& progress = {});

    // ---- The pieces, public for the tests ----------------------------------------------------------

    /// The DI through a private AmpSection with this capture in slot 1 at this Gain, tone flat: what the
    /// chain plays with every effect off and no cab (and what ampsim_render --model --trim writes).
    /// Returns an empty vector if the capture won't load or the render was cancelled.
    static std::vector<float> renderAmp (const juce::File& model, const NamAmp::Calibration& calibration,
                                         const std::vector<float>& di, double gainDb, const std::atomic<bool>& cancel);

    /// A cab IR as the cab block takes it for a close mic: the left channel, at most 1 s (CabIR::maxIRSeconds).
    static std::vector<float> loadIR (const juce::File& file);

    /// x convolved with ir, the first x.size() samples (FFT overlap-add).
    static std::vector<float> convolve (const std::vector<float>& x, const std::vector<float>& ir);

    /// The cab's power response sampled on the analysis bins (for the quick screen).
    static std::vector<double> cabPowerOnBins (const std::vector<float>& ir);

    /// A candidate (one slot, Gain, and cab), scored against the target as the search does.
    struct Candidate
    {
        int slot = 0;
        double gainDb = 0.0;
        int cab = 0;
        Analysis analysis;
        std::vector<double> residual;
        std::array<double, 5> tone {};
        double spectral = 0.0, distortion = 0.0;
        double total() const { return spectral + lambda * distortion; }
    };
    static Candidate score (int slot, double gainDb, int cab, const std::vector<float>& ampOutput, const std::vector<float>& ir,
                            const Analysis& target, Mode mode, const Alignment* alignment);
};

} // namespace ampsim::tonematch

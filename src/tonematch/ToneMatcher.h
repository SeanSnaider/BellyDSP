// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Progress.h"
#include "TakeScore.h"
#include "ToneMatchAnalysis.h"

#include "../dsp/Boost.h"
#include "../dsp/Compressor.h"
#include "../dsp/MatchCurve.h"
#include "../dsp/NamAmp.h"
#include "../dsp/Overdrive.h"

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

/// A pedal in front of the amp (Round 2: the search tries them; docs/TONE_MATCH.md, "Round 2: pedals"): the boost,
/// the overdrive, or the pre compressor, with the settings the chain's block takes (as BoostParameters,
/// OverdriveParameters, CompressorParameters read them).
struct Pedal
{
    enum class Kind
    {
        none,
        boost,
        overdrive,
        compressor
    };
    Kind kind = Kind::none;
    Boost::Settings boost;
    Overdrive::Settings overdrive;
    Compressor::Settings compressor;
};

/// The post compressor (after the cab and the post EQ), when a match uses it.
struct PostCompressor
{
    bool on = false;
    Compressor::Settings settings;
};

struct MatchSettings
{
    Mode mode = Mode::anything;
    /// The amps to choose from (gain sets or captures; the app: every gain set in the content folder, then the slots'
    /// own captures). An empty File is skipped. MatchResult::slot indexes this list.
    std::vector<juce::File> models;
    std::vector<juce::File> cabs;       ///< the cab IRs to choose from (the built-in 21 in the app)
    NamAmp::Calibration calibration;    ///< as the app loads its captures
    int threads = 0;                    ///< 0: the machine's cores minus one
    std::array<double, 7> gainGrid { -18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0 };
    std::array<double, 2> refineSteps { 3.0, 1.5 };
    double gainMin = -24.0, gainMax = 24.0; ///< amp*_input_trim's range
    int cabsPerAmp = 4;
    int refineSlots = 2;                ///< the slots (best by the old score) whose Gain is refined; with a take, see below
    /// With a take (the take-aware score decides; docs/TONE_MATCH.md, "Round 2: eight amps"): how far the search looks,
    /// so it scales with the number of amps. The old score ranks the amps differently from S, so with eight amps the
    /// three-amp search's winner was often never refined, never had the pedals tried in front of it, or ranked below
    /// the old score's 16 best (prototypes/tone_match.py, TAKE_REFINE_ALL, PEDAL_SLOTS_PRE, TAKE_PRE_SHORTLIST).
    /// The defaults are the Round 2 search: the wider one finds a lower S but, over eight amps, isn't closer to the
    /// hidden rigs (the score, not the search, decides which new amp wins), so it waits for the score's refit.
    bool takeRefinesEverySlot = false;  ///< every slot's Gain refined, not refineSlots (the wide search: true)
    int pedalSlots = 1;                 ///< the pedals in front of this many amps, the best by the old score
    int takePedalSlotsByPre = 0;        ///< and in front of this many more, the best by S_pre (the wide search: 1)
    int takePreShortlist = 0;           ///< the shortlist: the old score's take::shortlist best and S_pre's this many best (16)
    /// Same part: > 0 when the DI is a play-along take, recorded lined up with the target (the session
    /// trims it so its sample 0 is the target's), so DTW only searches this far either side of that
    /// alignment (align's band; playAlongBandSeconds). 0: unconstrained, as for a take played on its own.
    double alignmentBandSeconds = 0.0;
    /// The reference is a play-along take of the target, lined up with it (either mode; Round 2): its notes are
    /// paired with the target's (informed::alignNotes in the 0.5 s band) and the winner is chosen by the take-aware
    /// score (TakeScore.h) among the old score's take::shortlist best, each fitted completely. False, or fewer than
    /// take::minNotes pairs: the old score decides.
    bool takeIsLinedUp = false;
    /// Also try the app's pedals in front of the best amp and its post compressor (Round 2; ToneMatcher::pedalVariants,
    /// searchPostCompressor). The post compressor needs the take-aware score (a take).
    bool searchPedals = false;
    /// The match curve (src/dsp/MatchCurve.h) instead of the 5-band match EQ (Round 2, item 5: tone_bench DEV 0.435 ->
    /// 0.426 median, TEST measured in docs/TONE_MATCH.md), fitted to the winner (fitMatchCurve below) and played at
    /// matchCurveAmountPercent. Needs the take-aware score (it was measured with it).
    bool fitCurve = false;
    double matchCurveAmountPercent = 50.0;
    int pedalOversampling = 4;                                   ///< as the processor sets the drive blocks
    double voltsAtFullScale = drive::defaultVoltsAtFullScale;    ///< the interface's calibration, as the processor's
};

struct MatchResult
{
    bool ok = false, cancelled = false;
    juce::String error;
    Mode mode = Mode::anything;

    int slot = 0;                       ///< the matched amp: an index into MatchSettings::models
    juce::File model;                   ///< that amp's file
    double gainDb = 0.0;
    std::array<double, 5> tone {};
    juce::File cab;
    std::array<Equalizer::Band, Equalizer::numParametricBands> eq {};
    Pedal pedal;                        ///< in front of the amp (none unless the pedal search found one better)
    PostCompressor postCompressor;      ///< after the cab and the EQ
    /// The match curve when the search fitted one (MatchSettings::fitCurve): then it replaces the match EQ, whose
    /// bands are flat and which Apply switches off; empty otherwise.
    MatchCurve::Curve matchCurve;
    double matchCurveAmountPercent = 0.0;
    bool usesMatchEq() const noexcept { return matchCurve.points.empty(); }
    std::vector<double> postCompressorScores; ///< the take-aware score of the finished match without it and with each tried

    double spectralErrorDb = 0.0;        ///< after the tone, before the match EQ
    double spectralErrorAfterEqDb = 0.0; ///< after the match EQ
    double distortion = 0.0;             ///< distortion distance (feature units)
    double closeness = 0.0;              ///< 100 exp(-(spectral after EQ + lambda distortion) / 6 dB)
    std::array<double, numFeatures> targetFeatures {}, resultFeatures {};
    std::vector<double> residual;        ///< per band, after the tone: what the match EQ fits
    std::vector<double> eqTarget;        ///< the capped curve the EQ was fitted to
    std::vector<double> weights;         ///< each band's weight in the fit (perceptual times confidence)
    double runtimeSeconds = 0.0;
    int renders = 0, candidates = 0;
    bool takeScored = false;            ///< the take-aware score chose the winner
    int notePairs = 0;                  ///< the note pairs it compared
    int shortlisted = 0;                ///< the candidates it scored (fitted completely)
    take::Score takeScore;              ///< the winner's terms

    struct RunnerUp
    {
        int slot;
        double gainDb;
        juce::File cab;
        double score;
    };
    std::vector<RunnerUp> runnersUp;

    /// Same part: the DTW path (target frame, DI frame) between the target and the winning slot's render at
    /// Gain 0, as the search used it. The A/B player lines the two up in time with it. Empty in anything mode.
    std::vector<std::pair<int, int>> alignmentPath;
};

/// The settings a tone match render plays the DI through (the A/B comparison: what Apply would set, or
/// what's set now). The values are as the parameters store them, so a render of the settings read back
/// from the processor equals one of the settings that wrote them.
struct ToneSettings
{
    juce::File model;                   ///< the slot's capture or gain set; empty: no amp
    NamAmp::Calibration calibration;    ///< as the app loads its captures
    std::vector<Pedal> pedals;          ///< in front of the amp, in chain order (the pre compressor, the boost, the overdrive)
    bool matchCurveOn = false;          ///< the match curve, after the cab, before the post EQ
    MatchCurve::Curve matchCurve;
    double matchCurveAmount = 1.0;      ///< 0 to 1
    PostCompressor postCompressor;      ///< after the post EQ
    bool ampOn = true;                  ///< amp_bypass off
    float gainDb = 0.0f;                ///< the slot's Gain (amp*_input_trim)
    float masterDb = 0.0f;              ///< the slot's Master (amp*_output_trim)
    std::array<float, 5> tone {};       ///< Depth, Bass, Mid, Treble, Presence (dB)
    std::vector<float> cabIR;           ///< close mic 1's IR as the cab plays it (loudness-matched); empty: no cab
    bool postEqOn = false;              ///< post_fx_on and eq_post_on
    Equalizer::Settings postEq;
};


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
                                         const std::vector<float>& di, double gainDb, const std::atomic<bool>& cancel,
                                         const Pedal& pedal = {});

    /// The DI through a pedal alone (the chain's own block, mono, settings in before prepare()). The DI as it was
    /// for Kind::none.
    static std::vector<float> renderPedal (const Pedal& pedal, const std::vector<float>& di, const std::atomic<bool>& cancel);

    /// The post compressor (the chain's block, settings in before prepare()) on x, in place. False if cancelled.
    static bool compress (const Compressor::Settings& settings, std::vector<float>& x, const std::atomic<bool>& cancel);

    /// The playing level of x: the 90th percentile of a 50 ms RMS (the mean square over [n - 1200, n + 1200)) over the
    /// samples where it's above 1e-6, dB (-120 if none). Pedal and compressor thresholds are set from it.
    static double playingLevelDb (const std::vector<float>& x);

    /// The match curve's fit (prototypes/tone_match.py, fit_match_curve): the target's long-term power per analysis
    /// bin against the candidate's (candidateBins: its ltasBins times its linear part's |H|^2), the dB difference
    /// smoothed by a Gaussian of 1/12 octave (sigma, +-3 sigma) on log frequency, scaled toward 0 dB by the target's
    /// confidence (full within 20 dB of its loudest, 5% at 35 dB down; 0 outside 60 Hz to 14 kHz), the confidence-
    /// weighted mean removed, capped at +-12 dB; sampled every 1/48 octave from 40 Hz to 16 kHz (415 points).
    static MatchCurve::Curve fitMatchCurve (const Analysis& target, const std::vector<double>& candidateBins);

    /// |H|^2 of the tone bands and the EQ bands (eq may be null) on the analysis bins (k fs / 8192).
    static std::vector<double> linearPowerOnBins (const std::array<double, 5>& tone, const std::array<Equalizer::Band, Equalizer::numParametricBands>* eq);

    /// The pedals tried in front of the best amp at its best Gain (prototypes/tone_match.py, pedal_variants): the
    /// overdrive's four modes at Drive 0.3 and 0.7 (Tone 0.5, Level 0 dB, Tight off) each at the Gain and 6 dB under
    /// it; the clean boost at +6 and +12 dB into Gain +24 when the Gain is +18 or more; the pre compressor (pedal mode,
    /// peak, 4:1, 2 ms, 200 ms) 6 and 12 dB under the DI's playing level with makeup for three quarters of that.
    static std::vector<std::pair<Pedal, double>> pedalVariants (const std::vector<float>& di, double gainDb, const MatchSettings& settings);

    /// The DI through `settings`: the chain's own amp block (AmpSection, the capture in its first slot, with
    /// the Gain, tone, and Master), then the cab IR by FFT convolution, then the post EQ block (Equalizer).
    /// Mono, 48 kHz, the DI's length. What the chain plays with these settings, the pre effects, mic 2, the
    /// room, the other post effects, the cab mic's level and pan, and the output level aside. Empty if the
    /// capture won't load or the render was cancelled. Any thread but the audio thread.
    static std::vector<float> renderTone (const ToneSettings& settings, const std::vector<float>& di, const std::atomic<bool>& cancel);

    /// The settings a result plays the DI through as Apply would set them (the matched model, its Gain and tone,
    /// the cab as close mic 1 plays it, the match EQ as the parametric post EQ, no cuts), for renderTone. Reads
    /// the cab file. The values aren't snapped to the parameters' steps (ToneMatchSession::matchedSettings is).
    static ToneSettings settingsFor (const MatchResult& result, const MatchSettings& settings);

    /// An IR file as a close mic plays it: read, capped, faded, and loudness-matched by the cab's own code
    /// (CabIR::loadFile), the first channel. Empty if it won't load.
    static std::vector<float> irAsPlayed (const juce::File& file);

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
        int pedal = -1; ///< an index into the search's pedal variants; -1: none
        Analysis analysis;
        std::vector<double> residual;
        std::array<double, 5> tone {};
        double spectral = 0.0, distortion = 0.0;
        double total() const { return spectral + lambda * distortion; }
        /// With a take: take::ltasBins of the render through the cab (for S), and S_pre, the take-aware score with the
        /// linear tone fit and no match EQ (the search's prefilter).
        std::vector<double> takeLtasBins;
        double pre = 0.0;
    };
    /// takeLtasBins: also keep take::ltasBins of the render through the cab in the candidate.
    static Candidate score (int slot, double gainDb, int cab, const std::vector<float>& ampOutput, const std::vector<float>& ir,
                            const Analysis& target, Mode mode, const Alignment* alignment, bool takeLtasBins = false);
};

} // namespace ampsim::tonematch

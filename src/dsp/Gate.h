// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Svf.h"

#include <array>
#include <atomic>
#include <vector>

namespace ampsim
{

/// The noise gate (BUILD_PLAN "Gates", design review round 4 and the conflict review). Mono, zero
/// latency. Two instances in the chain: Gate A in pre FX and Gate B between the amp and the cab, linked
/// by default (one gating decision applied at two points; see "Linking" below).
///
/// Sources: the gate's structure (two thresholds for hysteresis, a hold counter, attack and release
/// on the gain) is the classic noise gate in U. Zolzer (ed.), "DAFX: Digital Audio Effects", 2nd ed.
/// (2011), chapter 4, and J. Reiss and A. McPherson, "Audio Effects: Theory, Implementation and
/// Application" (2014), the dynamics chapter. The split into detector, decision, and gain smoothing
/// follows Giannoulis, Massberg and Reiss (JAES 2012), as Compressor.h does. The sliding-window peak,
/// the adaptive release, and Learn are this project's own, settled by prototypes/gate.py --study.
///
/// Per sample (prototypes/gate.py simulates exactly this, and the golden test holds the block to it):
///
///  1. Detector input: the clean DI (BlockContext::di, the default) or the gate's own input, through a
///     sidechain high-pass, 24 dB/oct Butterworth at 100 Hz by default: two TPT SVF sections with
///     Q 1.3066 and 0.5412 (see CutFilter.h for the Butterworth Q formula). 60 Hz hum comes out 17.8 dB
///     down, so it can't hold the gate open, while low notes, even drop A (55 Hz), trigger through
///     their harmonics, which pass.
///
///  2. Peak level: D = the largest |x| in the last 10 ms, kept as 20 chunk maxima of 0.5 ms plus the
///     chunk being filled (a window of 10.0 to 10.5 ms). It takes the first sample of a pick attack
///     at once, and lets go of a stop as soon as the last loud peak leaves the window. 10 ms spans
///     half a period down to 50 Hz, so |x| of anything the high-pass lets through peaks at least once
///     per window: the level ripples about 1 dB on drop A and low E, 0 dB from 110 Hz up
///     (prototypes/gate.py --study). L = 20 log10 D, in dBFS.
///
///  3. Decision, with hysteresis. Closed (or closing) -> opening when L >= T_open (the threshold).
///     Open stays open while L >= T_close = T_open - hysteresis; once L drops below T_close the hold
///     counts down, and when it runs out the release starts. A level between the two thresholds
///     changes nothing, which is what stops a note hovering at the threshold from chattering. The
///     hold adds its own length to the gaps the detector already rides through (its 10 ms window,
///     plus a few ms of the sidechain high-pass ringing down on low notes): with the default 10 ms,
///     a choked tremolo note may go silent for 20 ms (E4) or 26 ms (A2) before the next pick
///     without the gate starting to close (the "hold" test).
///
///  4. Envelope, on an "openness" e in [0, 1]:
///       attack:  a raised cosine from wherever e is to 1, over the attack time (0.5 ms default).
///                Zero slope at both ends, so its spectrum falls 18 dB/oct instead of a step's 6,
///                which is what keeps the opening click-free without any lookahead.
///       release: e *= a every sample: an exponential decay, a straight line in dB, like a string's
///                own decay. The release time is the time to fall 60 dB: a = 10^(-3 / (t fs)).
///       e below 1e-6 (-120 dB) snaps to 0.
///     The gain is 1 - (1 - floor)(1 - e): exactly 1 when open (bit-transparent), the range's floor
///     when closed. Range at -90 dB, the end of its scale, is a true mute (floor 0).
///
///  5. Adaptive release (the default). Two envelope followers are compared. The fast one is D itself
///     (it drops 10 ms after a stop); the slow one, S = max(D, S k), falls at most 100 dB/s. Any note
///     ringing out decays slower than that (natural decays measure 10 to 80 dB/s), so S tracks it
///     exactly and the gap d = 20 log10(S / D) stays near 0 dB; a deliberate stop falls faster than S
///     can follow and opens the gap at (rate - 100) dB/s. The largest gap since L fell below T_close
///     picks the release:
///         d <= 8 dB:   the release knob (slow: a ringing or legato note fades out naturally)
///         d >= 20 dB:  20 ms, or the knob if it's shorter (fast: a stop is silent at once)
///         in between:  log-interpolated (equal steps of gap are equal ratios of time).
///     The study (prototypes/gate.py --study) measured natural decays at 0.9 to 3.8 dB, Gaussian
///     hiss alone at up to 5 dB, and stops (300 to 3000 dB/s) and palm mutes at 25 to 46 dB, so both
///     thresholds sit well clear. The gap builds up for as long as the fall lasts, so what decides is
///     how fast and how far the level fell: for a note dying from -15 dBFS (the "adaptive release"
///     test) every decay up to 90 dB/s keeps the knob's 250 ms, 120 dB/s gets 110 ms, and 150 dB/s
///     and faster (palm mutes) and hard stops get 20 ms; a hard stop on a quiet note, only 24 dB over
///     the close threshold, still gets 20.7 ms. Classic mode always uses the knob.
///
/// Learn: with the strings muted, startLearn() makes the audio thread histogram the detector level L
/// for 2 s (0.25 dB bins). The noise floor is its 95th percentile (so up to 100 ms of a stray glitch
/// doesn't count), and the close threshold goes 6 dB above that: the close threshold is the one that
/// decides whether noise can hold the gate open. The learned threshold (the open one) is therefore
/// noise + 6 dB + hysteresis. The message thread reads it (getLearnedThresholdDb(), with
/// getLearnCount() telling a new result from an old one) and writes it into the parameter.
///
/// Linking (Gate B follows Gate A): the gain curve is computed separately from applying it.
///   process()          computes this buffer's curve from the detector, applies it, and keeps it
///                      (getGainCurve(), valid until the next call).
///   processWithGain()  applies a curve computed elsewhere: a linked Gate B applying Gate A's.
///   computeGainCurve() computes a curve from a given detector signal without applying it: Gate A,
///                      switched off while Gate B is linked to it, still runs its settings on the DI.
/// Per buffer, call exactly one of process() and computeGainCurve() on a gate (each advances its
/// detector and envelope by the buffer). Switching a gate between its own curve and an external one
/// fades from the last gain it applied over 10 ms, and resuming its own detection picks the envelope
/// up from that gain, so linking and unlinking never jump. reset() leaves a gate whose curve
/// computeGainCurve() kept current while the chain skipped it, so Gate A switching back on doesn't
/// disturb a Gate B that was following it. (If computeGainCurve() stopped being called during the
/// skip, the gate resumes from that older state instead of starting fresh; the chain's 10 ms bypass
/// fade covers it. Calling computeGainCurve() every buffer the chain skips Gate A, linked or not,
/// about 1 us, avoids the case entirely.)
///
/// After prepare() or reset() the gate is open, and closes through its release if nothing is
/// playing: re-enabling it in the chain's 10 ms bypass crossfade then starts from exactly the dry
/// signal. Threshold, hysteresis, range, the sidechain frequency, and both switches (detector source,
/// sidechain on/off) are smoothed or crossfaded over 20 ms; times only change rates, so nothing a
/// setting does can make the gain jump. Nothing allocates or locks after prepare().
class Gate : public Block
{
public:
    enum class ReleaseMode
    {
        adaptive,
        classic
    };

    enum class DetectorSource
    {
        di,      // the clean DI snapshot (BlockContext::di)
        ownInput // the signal arriving at this gate
    };

    struct Settings
    {
        float thresholdDb = -55.0f;  // open threshold, dBFS of the detector's peak level (-100 to 0)
        float hysteresisDb = 8.0f;   // the close threshold sits this far below it (0 to 24)
        float holdMs = 10.0f;        // after the level drops below the close threshold (0 to 500)
        float attackMs = 0.5f;       // raised-cosine opening (0.05 to 50)
        float releaseMs = 250.0f;    // time to fall 60 dB; adaptive: the slow (natural decay) side (5 to 2000)
        ReleaseMode releaseMode = ReleaseMode::adaptive;
        float rangeDb = -90.0f;      // closed attenuation (-90 to 0); -90 is a true mute
        DetectorSource detector = DetectorSource::di;
        bool sidechainHighPass = true;
        float sidechainHz = 100.0f;  // 24 dB/oct (20 to 500)
    };

    // Knob ranges, applied by setSettings().
    static constexpr float minThresholdDb = -100.0f, maxThresholdDb = 0.0f;
    static constexpr float maxHysteresisDb = 24.0f;
    static constexpr float maxHoldMs = 500.0f;
    static constexpr float minAttackMs = 0.05f, maxAttackMs = 50.0f;
    static constexpr float minReleaseMs = 5.0f, maxReleaseMs = 2000.0f;
    static constexpr float muteDb = -90.0f; // range at or below this is a true mute
    static constexpr float minSidechainHz = 20.0f, maxSidechainHz = 500.0f;

    // Detector and envelope constants (the design study in prototypes/gate.py).
    static constexpr double chunkSeconds = 0.0005;          // 24 samples at 48 kHz
    static constexpr int numChunks = 20;                    // window 10.0 to 10.5 ms
    static constexpr double slowFallDbPerSecond = 100.0;    // the slow follower's fastest fall
    static constexpr double adaptiveSlowGapDb = 8.0;        // gap at or below: the knob's release
    static constexpr double adaptiveFastGapDb = 20.0;       // gap at or above: the fast release
    static constexpr double adaptiveFastReleaseMs = 20.0;   // a stop's release (time to fall 60 dB)
    static constexpr double releaseRangeDb = 60.0;          // the release time is the time to fall this far
    static constexpr double snapToClosed = 1.0e-6;          // openness below this is 0 (-120 dB)
    static constexpr double minimumLevel = 1.0e-9;          // -180 dB: keeps log10 finite in silence

    // Learn.
    static constexpr double learnSeconds = 2.0;
    static constexpr double learnPercentile = 0.95;
    static constexpr double learnMarginDb = 6.0;            // close threshold above the noise floor
    static constexpr double learnBinDb = 0.25;
    static constexpr double learnFloorDb = -160.0;
    static constexpr int learnBins = 640;                   // -160 to 0 dB in 0.25 dB bins

    static constexpr double smoothingSeconds = 0.020;       // threshold, hysteresis, range, switches
    static constexpr double linkFadeSeconds = 0.010;        // own curve <-> external curve
    static constexpr int coefficientInterval = 32;          // sidechain redesign while its frequency glides

    /// The adaptive mapping: release time (ms, to fall 60 dB) for the largest gap seen since the level
    /// fell below the close threshold.
    static double adaptiveReleaseMs (double gapDb, double knobMs);

    /// Per-sample decay factor that falls 60 dB in `ms` milliseconds.
    static double releaseCoefficient (double ms, double sampleRate);

    /// The Learn rule: the threshold (open) that puts the close threshold learnMarginDb above a noise
    /// floor, clamped to the threshold's range.
    static float thresholdForNoiseFloor (double noiseFloorDb, double hysteresisDb);

    /// Audio thread, once per buffer. Ranges are clamped.
    void setSettings (const Settings& settings);
    const Settings& getSettings() const noexcept { return settings; }

    void prepare (double sampleRate, int maxBlockSize) override;

    /// Detects from the DI or this gate's input (per the settings), computes the curve, applies it to
    /// channel 0. If context.di is null the gate detects from its own input.
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;

    void reset() override;

    // ---- Linking ---------------------------------------------------------------------------------

    /// Audio thread: runs this gate's detector and envelope on `detector` (the DI for a switched-off
    /// Gate A) and keeps the curve, without touching any audio. numSamples <= maxBlockSize.
    void computeGainCurve (const float* detector, int numSamples) noexcept;

    /// Audio thread: applies `gain` (one value per sample of the block, usually another gate's
    /// getGainCurve()) to channel 0 instead of this gate's own curve. This gate's own detector doesn't
    /// run, and doesn't learn, while it's applied this way.
    void processWithGain (juce::dsp::AudioBlock<float> block, const float* gain) noexcept;

    /// The gain curve of the last process() or computeGainCurve() call, getGainCurveLength() values.
    const float* getGainCurve() const noexcept { return gainCurve.data(); }
    int getGainCurveLength() const noexcept { return curveLength; }

    // ---- Learn -----------------------------------------------------------------------------------

    /// Any thread: start (or restart) measuring the noise floor. The audio thread begins at its next buffer.
    void startLearn() noexcept;

    /// Any thread: true from startLearn() until the result is ready.
    bool isLearning() const noexcept;

    /// Any thread: 0 to 1 through the current measurement.
    float getLearnProgress() const noexcept { return learnProgress.load (std::memory_order_relaxed); }

    /// Any thread: the last finished measurement. Read getLearnCount() first (it goes up by one each
    /// time a measurement finishes, after both values are stored), then the values.
    float getLearnedThresholdDb() const noexcept { return learnedThreshold.load (std::memory_order_relaxed); }
    float getLearnedNoiseFloorDb() const noexcept { return learnedNoiseFloor.load (std::memory_order_relaxed); }
    int getLearnCount() const noexcept { return learnCount.load (std::memory_order_acquire); }

    // ---- Meters (any thread, updated once per buffer) ----------------------------------------------

    /// The loudest detector level (dBFS) in the last buffer: what the thresholds are compared with.
    float getDetectorLevelDb() const noexcept { return meterDetector.load (std::memory_order_relaxed); }
    float getOpenThresholdDb() const noexcept { return meterOpen.load (std::memory_order_relaxed); }
    float getCloseThresholdDb() const noexcept { return meterClose.load (std::memory_order_relaxed); }

    /// The largest gain reduction (dB, 0 to 100; a mute reads 100) applied in the last buffer.
    float getGainReductionDb() const noexcept { return meterReduction.load (std::memory_order_relaxed); }

    /// Whether the gate's own decision is open (opening, open, or holding) at the end of the last buffer.
    bool isOpen() const noexcept { return meterOpenState.load (std::memory_order_relaxed); }

private:
    enum class State
    {
        opening,
        open, // includes the hold countdown
        releasing // includes fully closed
    };

    enum class CurveSource
    {
        none, // nothing applied since reset()
        own,
        external
    };

    static double floorFor (float rangeDb);
    void updateTimes();
    void resetState();
    void resetDetector();
    void designSidechain (double hz);
    void computeCurve (const float* di, const float* own, int numSamples) noexcept;
    double detectorSample (double di, double own) noexcept;
    double windowedPeak (double magnitude) noexcept;
    void learnSample (double levelDb) noexcept;
    void finishLearn() noexcept;
    void applyCurve (float* audio, const float* gain, int numSamples, CurveSource source) noexcept;
    void resumeOwnEnvelope() noexcept;

    double sampleRate = 48000.0;
    Settings settings;

    // Smoothed knobs and crossfaded switches.
    juce::SmoothedValue<double> thresholdDb { -55.0 }, hysteresisDb { 8.0 }, floorGain { 0.0 };
    juce::SmoothedValue<double> sidechainMix { 1.0 }; // 0 raw, 1 high-passed
    juce::SmoothedValue<double> sourceMix { 0.0 };    // 0 DI, 1 own input
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> sidechainHz { 100.0 };
    int samplesSinceDesign = 0;

    // Detector.
    std::array<Svf, 2> sidechain;
    std::array<double, numChunks> chunks {}; // the last numChunks chunk maxima
    int chunkLength = 24, chunkFill = 0, chunkPosition = 0;
    double chunkMax = 0.0, chunksMax = 0.0;
    double slowFollower = 0.0, slowFall = 1.0;

    // Envelope.
    State state = State::open;
    double openness = 1.0, attackStart = 1.0, attackPhase = 0.0, attackStep = 1.0;
    int holdSamples = 0, holdLeft = 0;
    bool belowClose = false;
    double gapPeak = 0.0, coefficientGap = -1.0, releaseFactor = 1.0;
    bool releaseDirty = true;

    // The curve and how it's applied.
    std::vector<float> gainCurve;
    int curveLength = 0;
    CurveSource lastSource = CurveSource::none;
    float lastApplied = 1.0f, fadeFrom = 1.0f;
    int fadeLength = 480, fadeLeft = 0;
    bool ownEnvelopeStale = false; // an external curve was applied since the envelope last ran
    bool keptWarm = false;         // computeGainCurve() ran since the last process()

    // Learn: requested by any thread, measured on the audio thread.
    std::atomic<bool> learnRequested { false }, learning { false };
    std::atomic<float> learnProgress { 0.0f }, learnedThreshold { -55.0f }, learnedNoiseFloor { -100.0f };
    std::atomic<int> learnCount { 0 };
    std::array<int, learnBins> learnHistogram {};
    int learnTotal = 96000, learnLeft = 0;

    std::atomic<float> meterDetector { -180.0f }, meterOpen { -55.0f }, meterClose { -63.0f }, meterReduction { 0.0f };
    std::atomic<bool> meterOpenState { true };
};

} // namespace ampsim

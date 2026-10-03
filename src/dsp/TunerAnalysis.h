// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PitchDetector.h"

#include <array>
#include <cstdint>

namespace ampsim
{

/// What the tuner shows (BUILD_PLAN "Tuner", Display). The analysis thread publishes it to the GUI
/// through TunerReadout.
struct TunerReading
{
    bool hasReading = false;     ///< A note has been read since the tuner was engaged (otherwise show "--").
    bool live = false;           ///< A note is sounding and these values are current; false: holding the last reading.
    double frequency = 0.0;      ///< Hz: the median of the latest fine readings (held after the note decays).
    int midiNote = -1;           ///< The note shown, MIDI numbering (69 = A4), with hysteresis.
    double cents = 0.0;          ///< The needle: smoothed offset from midiNote. Display to 0.1 cent.
    double rawCents = 0.0;       ///< The same offset, unsmoothed.
    double clarity = 0.0;        ///< The coarse detector's clarity in the latest analysis, 0 to 1.
    double strobePhase = 0.0;    ///< Strobe pattern offset in pattern periods, in [0, 1).
    double strobeVelocity = 0.0; ///< Pattern periods per second, proportional to rawCents; 0 while holding.
    double levelDb = -300.0;     ///< Input RMS over the last 50 ms, dBFS.
    double referenceA4 = 440.0;  ///< The A4 the note and cents are measured against.
};

/// The tuner's analysis, without threads: push() the DI, call analyse() 30 to 60 times a second, read
/// the result. TunerThread hosts it on its own thread; the tests drive it directly. Everything is
/// allocated in prepare(), so push() and analyse() never allocate or lock either.
///
/// Each analyse():
///  1. Coarse: the shared McLeod Pitch Method detector (PitchDetector, tuner preset: 28 Hz to 1.4 kHz on a
///     72 ms frame at 12 kHz) finds the note, octave-safe, within about 10 cents. A reading needs
///     clarity >= 0.9 and an input level above the gate.
///  2. Fine (prototypes/pitch_detection.py, refine): band-pass the 48 kHz DI around the coarse estimate
///     with two SVF band-pass stages (Q 3, unit gain at the centre), which leaves mostly the first partial,
///     then time its upward zero crossings, each placed by linear interpolation between the two samples
///     around it. frequency = (crossings - 1) / (last crossing - first crossing). The filters start from
///     rest and settle for max(0.15 s, 10 time constants), where one band-pass stage's envelope time
///     constant is Q / (pi f); the crossings are counted over max(0.25 s, 12 periods). That reads every
///     tone in the study within 0.5 cent of the first partial, which is the pitch a stiff string is heard
///     at (its upper partials run sharp: n f0 sqrt(1 + B n^2)). A just-plucked note uses what it has,
///     split between settling and counting in the same proportion (settling at least 6 time constants),
///     once the coarse frame is past the pluck and 4 periods fit.
///  3. Note tracking: a coarse jump of more than 60 cents is a new note. A pluck (the energy of the
///     newest period more than doubling, 3 dB, over the period before) restarts the fine window after it:
///     a re-plucked string comes back with a new phase, and a window spanning that jump reads tens of
///     cents off. A new note clears the median filter.
///  4. Robustness and display (BUILD_PLAN "Tuner"): the median of the last 5 fine readings (an outlier
///     can't move it), note hysteresis (the shown note only changes once the pitch is 60 cents from it, so
///     a string sitting at a quarter tone doesn't flicker between neighbours), a smoothed needle, and a
///     strobe whose pattern drifts at 0.25 periods per second per cent (stationary in tune; at A4 that's
///     the drift a physical strobe disc shows, 1 cent being 0.25 Hz there). When the note decays into the
///     noise the reading holds its last value and `live` goes false.
class TunerAnalysis
{
public:
    struct Settings
    {
        double referenceA4 = 440.0;                  ///< Hz, 430 to 450.
        double gateDb = -60.0;                       ///< Input RMS (50 ms) below this reads as silence; 6 dB lower once a note is on.
        double clarityThreshold = 0.9;               ///< Coarse clarity a reading needs.
        double newNoteCents = 60.0;                  ///< A coarse jump larger than this starts a new note.
        double hysteresisCents = 10.0;               ///< The shown note changes at 50 + this many cents.
        double needleSeconds = 0.08;                 ///< Needle smoothing time constant.
        double strobePeriodsPerSecondPerCent = 0.25; ///< Strobe drift speed.
        int medianLength = 5;                        ///< Fine readings in the median filter (1 to 9).
        double onsetRatio = 2.0;                     ///< Energy jump over one period that counts as a pluck.
    };

    static constexpr double minReferenceA4 = 430.0, maxReferenceA4 = 450.0;
    static constexpr double fineQ = 3.0;
    static constexpr int minFinePeriods = 4;

    /// Allocates (about 1.4 s of history at 48 kHz).
    void prepare (double newSampleRate, const Settings& newSettings);
    void prepare (double newSampleRate) { prepare (newSampleRate, Settings {}); }

    /// Forgets everything (history, note, reading). Keeps the settings and the reference.
    void reset() noexcept;

    void setReferenceA4 (double hz) noexcept { settings.referenceA4 = juce::jlimit (minReferenceA4, maxReferenceA4, hz); }
    double getReferenceA4() const noexcept { return settings.referenceA4; }
    const Settings& getSettings() const noexcept { return settings; }

    /// Appends DI samples. Never allocates.
    void push (const float* input, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            history.push (input[i]);
            detector.pushSample (input[i]);
        }
    }

    /// Samples went missing between pushes (the ring overflowed): start the note over after the gap.
    void markDiscontinuity() noexcept;

    /// Analyses everything pushed so far and updates the reading. Never allocates.
    const TunerReading& analyse() noexcept;

    const TunerReading& getReading() const noexcept { return reading; }

    /// The latest coarse estimate and fine reading (0 when there was none), for tests and diagnostics.
    PitchDetector::Estimate getCoarse() const noexcept { return coarse; }
    double getFine() const noexcept { return fine; }
    int64_t getSamplesPushed() const noexcept { return history.getTotal(); }

    // ---- The fine stage itself (stateless; the golden test calls these directly) ------------------

    struct FineSpan
    {
        int settle = 0, window = 0; ///< Samples: filter settling, then crossing counting.
    };

    /// Full size: settle max(0.15 s, 10 Q / (pi f)), window max(0.25 s, 12 periods). With less than that
    /// available, the available samples are split in the same proportion, but with at least 6 Q / (pi f)
    /// of settling.
    static FineSpan fineSpan (double coarseHz, int64_t available, double sampleRate) noexcept;

    /// Runs two band-pass SVFs (Q 3) centred on coarseHz over x[0..length) from rest and times the upward
    /// zero crossings after the first `settle` samples. Returns 0 with fewer than 5 crossings.
    static double refineFrequency (const float* x, int length, int settle, double coarseHz, double sampleRate) noexcept;

private:
    void scanOnsets (int64_t now) noexcept;
    void pushMedian (double log2Frequency) noexcept;
    double median() const noexcept;
    void updateDisplay (bool live, double dt, double levelDb) noexcept;

    Settings settings;
    double sampleRate = 48000.0;
    PitchDetector detector;
    ContiguousHistory history;

    int levelSamples = 2400, onsetStep = 240, minimumFineSamples = 0;
    int64_t associationSamples = 7200, graceSamples = 4800;

    // Note tracking.
    int64_t lastAnalysis = 0, nextOnsetCheck = 0, lastOnset = -1, lastConfident = 0, noteStart = 0;
    bool noteActive = false, noteHasFine = false;
    double trackedCoarse = 0.0;

    // Median filter over log2 frequencies: a small ring.
    std::array<double, 9> medianRing {};
    int medianCount = 0, medianNext = 0;

    // Display.
    double heldFrequency = 0.0, needle = 0.0, strobePhase = 0.0;
    int shownNote = -1;
    bool snapNeedle = true;

    PitchDetector::Estimate coarse;
    double fine = 0.0;
    TunerReading reading;
};

} // namespace ampsim

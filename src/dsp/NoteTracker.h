#pragma once

#include <array>
#include <cstdint>

namespace ampsim
{

/// Follows the played note for the harmonizer (BUILD_PLAN "Harmonizer": Note transitions, Onsets,
/// Confidence gating), from the shared pitch detector's estimates on the clean DI, one per hop (64 samples
/// at 48 kHz, 1.33 ms). It decides which note the harmony intervals are computed from, and when that
/// changes; the pitch shifter follows the played pitch itself, so between changes bends, vibrato, and
/// slides move the harmony in parallel. No allocation: safe on the audio thread.
///
/// The rules, with pitch p in fractional MIDI notes (69 + 12 log2 (f / A4), so it follows the tuner's A4):
///   Onset.     Two confident estimates in a row that agree within agreeCents start a note, at the
///              nearest semitone. One alone isn't enough: just after a pluck the detector can give a
///              confident reading of neither note, or of the previous one (up to 2.7 ms; the "Tuner"
///              tests), and a voice must never play a harmony for the wrong note.
///   Jump.      A move of more than jumpCents within jumpWindowSeconds (hammer-ons, pull-offs, taps) that
///              has landed on a different semitone re-evaluates at once. Landed means the last steadyCount
///              estimates (consecutive hops) span no more than steadyCents: while the detector's window
///              straddles both notes it reads a slowly moving blend (a pull-off from B3 to A3 read 57.81,
///              57.75, 57.70 at a clarity of 0.98, about 5 cents a hop near A#3), and only the settled
///              reading converges tighter than that (57.03, 57.000, 57.000). Vibrato moves about 1.5 cents
///              a hop at most (6 Hz, +-30 cents), well inside. In practice the jump comes two hops after the
///              detector first reads the new note.
///   Slide.     Otherwise a change of semitone (a slide, or a bend that's held) re-evaluates once the pitch
///              has stayed within settleCents of the new semitone for settleSeconds. A bend or vibrato that
///              doesn't stay on a new note changes nothing.
///   Release.   No confident estimate for loseSeconds (chords, noise, heavy muting) ends the note, so the
///              voices fade out instead of producing garbage; the next confident onset brings them back.
class NoteTracker
{
public:
    struct Settings
    {
        double referenceA4 = 440.0;
        double confidence = 0.9;          ///< Clarity a confident estimate needs (the detector's noise stays under 0.72).
        double agreeCents = 35.0;         ///< Two estimates this close are the same note (onsets).
        double steadyCents = 6.0;         ///< The last steadyCount estimates within this: a jump has landed.
        int steadyCount = 3;
        double jumpCents = 70.0;
        double jumpWindowSeconds = 0.010;
        double settleCents = 25.0;
        double settleSeconds = 0.030;
        double loseSeconds = 0.005;
    };

    enum class Change
    {
        none,
        onset,
        jump,
        slide,
        release
    };

    void prepare (double hopSeconds, const Settings& newSettings);
    void setSettings (const Settings& newSettings) noexcept { settings = newSettings; }
    const Settings& getSettings() const noexcept { return settings; }

    /// Forgets the note and the history.
    void reset() noexcept;

    /// One hop's estimate (frequency 0 = no pitch found). Returns the change it caused, if any.
    Change update (double frequency, double clarity) noexcept;

    bool hasNote() const noexcept { return note >= 0; }

    /// The note the intervals are computed from (MIDI number), -1 without one.
    int getNote() const noexcept { return note; }

    /// The latest confident pitch, fractional MIDI notes.
    double getPitch() const noexcept { return pitch; }

    /// Goes up by one at every change (onset, jump, slide, release).
    std::uint32_t getChangeCount() const noexcept { return changes; }
    Change getLastChange() const noexcept { return lastChange; }

    static double midiFromFrequency (double frequency, double referenceA4) noexcept;

private:
    struct Entry
    {
        double time = 0.0, pitch = 0.0;
    };

    Change setNote (int newNote, Change why) noexcept;

    Settings settings;
    double hop = 64.0 / 48000.0, time = 0.0, lastConfident = -1.0e9;
    int note = -1;
    double pitch = 0.0;
    std::uint32_t changes = 0;
    Change lastChange = Change::none;

    // Recent confident estimates (enough for the jump window), oldest overwritten first.
    static constexpr int historySize = 64;
    std::array<Entry, historySize> history {};
    int historyCount = 0, historyNext = 0;

    bool candidate = false; // an onset waiting for a second, agreeing estimate
    double candidatePitch = 0.0;
    int settleNote = -1;    // the semitone a slide is holding, and since when
    double settleStart = 0.0;
};

} // namespace ampsim

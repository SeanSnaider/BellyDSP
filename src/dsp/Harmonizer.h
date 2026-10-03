// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "NoteTracker.h"
#include "PitchShifter.h"
#include "Psola.h"
#include "Scales.h"

#include <array>

namespace ampsim
{

/// The harmonizer (BUILD_PLAN "Harmonizer"): up to four voices, each a key-aware interval from the note
/// being played, made by the shared pitch shifter's PSOLA engine. Stereo, in the post section.
///
/// Where things come from (BUILD_PLAN "Default signal chain", the deliberate choices):
///   - Detection reads the clean DI (BlockContext::di): the PSOLA analysis runs the shared McLeod detector
///     on it every 64 samples (harmonizer preset: 3 ms minimum window, the chosen floor), and the
///     NoteTracker turns those readings into notes (onsets, legato jumps, slides, releases; NoteTracker.h).
///   - The harmonies are made from this block's input, the guitar after the amp and cab, so two shifted
///     notes never go through the amp together (intermodulation). PSOLA places its pitch marks on that
///     signal and builds each voice from grains of it.
///   - The dry signal passes untouched and undelayed: the block reports zero latency. The voices trail it
///     by the detection time plus PSOLA's lag, like a tight second player.
///
/// Per hop (64 samples): the tracker's note and each voice's interval give the voice's shift,
/// harmony::shiftFor (note, interval, key, rule), and its ratio 2^(shift / 12). A voice sounds while the
/// tracker has a note and the analysis is tracking it (confidence gating: chords, noise, and muting fade
/// the voices out over 2 ms instead of producing garbage). The ratio follows the played pitch between
/// re-evaluations, so bends and vibrato move the harmony in parallel; a re-evaluated interval glides over
/// glideMs, except when the voice starts from silence, where it takes the new ratio at once (never the
/// previous note's harmony).
///
/// The detection floor is a choice of three (110 Hz, the open A, by default; 80 Hz for the low E; 60 Hz
/// for drop tunings). Each needs its own detector buffers, so all three analyses are prepared and only the
/// chosen one runs; changing the floor restarts the tracking. Out: the dry plus every voice at its level
/// and pan (constant power, sqrt 2-scaled like the multivoicer's, so a centred voice is unity per side),
/// times the overall level.
class Harmonizer : public Block
{
public:
    static constexpr int maxVoices = 4;
    static constexpr std::array<double, 3> floors { 110.0, 80.0, 60.0 };
    static constexpr double maxHumanizeMs = 30.0;
    static constexpr double fadeMs = 2.0;
    static constexpr double minLevelDb = -60.0; // a voice at -60 dB is off
    static constexpr double smoothingSeconds = 0.020;

    struct Voice
    {
        bool on = false;
        harmony::Interval interval; // diatonic steps (default +2, a third) or chromatic semitones, plus octaves
        double levelDb = -3.0;
        double pan = 0.0;           // -1 left .. +1 right
        double humanizeMs = 0.0;    // 0 .. 30: a small extra delay
    };

    struct Settings
    {
        std::array<Voice, maxVoices> voices;
        harmony::Key key;
        harmony::OutOfScale outOfScale = harmony::OutOfScale::parallel;
        double glideMs = 10.0;      // a re-evaluated interval's glide
        int floor = 0;              // index into floors
        double referenceA4 = 440.0; // the tuner's A4
        double levelDb = 0.0;       // every voice together
    };

    /// Voice 1 a diatonic third above, voice 2 a fifth above, voices 3 and 4 an octave below and above;
    /// only voice 1 on.
    static Settings defaults();

    Harmonizer() { settings = defaults(); }

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings) noexcept;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }
    int latencySamples() const override { return 0; }

    // For tests, meters, and the panel.
    const NoteTracker& getTracker() const noexcept { return tracker; }
    const PsolaAnalysis& getAnalysis() const noexcept { return analyses[(size_t) activeFloor]; }
    int getShift (int voice) const noexcept { return shifts[(size_t) voice]; }

    /// Any thread, for the panel: the note the intervals come from (-1 for none) and each voice's shift
    /// while it sounds (shownSilent when it doesn't).
    static constexpr int shownSilent = -1000;
    int getShownNote() const noexcept { return shownNote.load (std::memory_order_relaxed); }
    int getShownShift (int voice) const noexcept { return shownShifts[(size_t) voice].load (std::memory_order_relaxed); }
    double getVoiceFade (int voice) const noexcept { return psola[(size_t) voice].getFade(); }

private:
    void onHop() noexcept;

    Settings settings;
    double sampleRate = 48000.0;
    PitchShifterInput input;
    std::array<PsolaAnalysis, floors.size()> analyses;
    int activeFloor = 0, hopPhase = 0;
    NoteTracker tracker;
    std::array<PsolaVoice, maxVoices> psola;
    std::array<int, maxVoices> shifts {};
    std::array<juce::SmoothedValue<double>, maxVoices> gains, lefts, rights;
    juce::SmoothedValue<double> level { 1.0 };
    std::atomic<int> shownNote { -1 };
    std::array<std::atomic<int>, maxVoices> shownShifts {};
};

} // namespace ampsim

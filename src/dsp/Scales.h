// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <array>
#include <cstdint>

namespace ampsim
{

/// Keys, scales, and the harmonizer's "smart" intervals (BUILD_PLAN "Harmonizer", Key and scale, How
/// "smart" works). Pure functions of MIDI note numbers, no audio, so the rules are tested exactly.
///
/// A scale is a 12-bit mask of pitch classes above its root (bit k set = k semitones above the root is in
/// the scale). A diatonic interval counts scale steps: in a 7-note scale +2 is a third, +4 a fifth, +7 an
/// octave, so a diatonic third is 3 or 4 semitones depending on where the played note sits in the key.
namespace harmony
{
enum class Scale
{
    major,              // Ionian:          0 2 4 5 7 9 11
    naturalMinor,       // Aeolian:         0 2 3 5 7 8 10
    harmonicMinor,      //                  0 2 3 5 7 8 11
    melodicMinor,       // (ascending):     0 2 3 5 7 9 11
    dorian,             //                  0 2 3 5 7 9 10
    phrygian,           //                  0 1 3 5 7 8 10
    lydian,             //                  0 2 4 6 7 9 11
    mixolydian,         //                  0 2 4 5 7 9 10
    locrian,            //                  0 1 3 5 6 8 10
    phrygianDominant,   // (5th mode of harmonic minor): 0 1 4 5 7 8 10
    wholeHalfDiminished, // 8 notes, whole then half steps: 0 2 3 5 6 8 9 11
    custom              // the key's own mask
};

constexpr int numScales = 12;

/// The UI's names, in Scale order.
extern const std::array<const char*, numScales> scaleNames;

/// A scale's mask (custom: 0, use Key::customMask).
std::uint16_t maskOf (Scale scale) noexcept;

struct Key
{
    int root = 0;                     ///< Pitch class of the tonic: 0 = C, 1 = C#, ... 11 = B.
    Scale scale = Scale::major;
    std::uint16_t customMask = 0xfff; ///< For Scale::custom. An empty mask counts as all 12 notes.

    /// The pitch classes in the key, relative to the root (bit 0 always set unless the custom mask clears it).
    std::uint16_t mask() const noexcept;
};

/// One harmony voice's interval.
struct Interval
{
    bool diatonic = true; ///< Diatonic: `steps` scale steps. Chromatic: `steps` semitones, whatever the key.
    int steps = 2;        ///< Diatonic -7 to +7 (+2 = a third); chromatic -24 to +24.
    int octaves = 0;      ///< -2 to +2, added on top (octave and unison stacking).
};

/// What a note outside the key gets (BUILD_PLAN: Parallel by default, Snap as the alternative).
enum class OutOfScale
{
    parallel, ///< The same shift as the nearest in-key note, so chromatic passing tones move smoothly.
    snap      ///< The harmony of the nearest in-key note itself, so the harmony stays in the key.
};

/// The nearest note in the key (MIDI number). Ties go to the lower one. Returns `note` if it's in the key.
int nearestInKey (int note, const Key& key) noexcept;

/// The harmony note for an in-key note: `steps` scale steps away (octaves wrap). For a note outside the
/// key this is computed from the nearest in-key note.
int diatonicTarget (int note, int steps, const Key& key) noexcept;

/// The shift in semitones for a voice when `note` (MIDI number) is played: the heart of the harmonizer.
///   chromatic:            steps + 12 octaves
///   diatonic, in key:     diatonicTarget (note) - note + 12 octaves
///   diatonic, out of key: parallel: the shift its nearest in-key note would get
///                         snap:     diatonicTarget (nearest) - note + 12 octaves (lands on a key note)
int shiftFor (int note, const Interval& interval, const Key& key, OutOfScale rule) noexcept;
} // namespace harmony

} // namespace ampsim

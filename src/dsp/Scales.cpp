#include "Scales.h"

#include <initializer_list>

namespace ampsim::harmony
{

namespace
{
constexpr std::uint16_t maskFrom (std::initializer_list<int> semitones)
{
    std::uint16_t mask = 0;
    for (const auto s : semitones)
        mask = (std::uint16_t) (mask | (1u << s));
    return mask;
}

/// Floor division and modulo for negative MIDI distances (C++'s / and % round toward zero).
constexpr int floorDiv (int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
constexpr int floorMod (int a, int b) { return a - b * floorDiv (a, b); }

bool inMask (std::uint16_t mask, int pitchClass) noexcept { return (mask >> pitchClass) & 1u; }
} // namespace

const std::array<const char*, numScales> scaleNames { "Major", "Natural minor", "Harmonic minor", "Melodic minor", "Dorian", "Phrygian",
                                                      "Lydian", "Mixolydian", "Locrian", "Phrygian dominant", "Whole-half diminished", "Custom" };

std::uint16_t maskOf (Scale scale) noexcept
{
    switch (scale)
    {
        case Scale::major:               return maskFrom ({ 0, 2, 4, 5, 7, 9, 11 });
        case Scale::naturalMinor:        return maskFrom ({ 0, 2, 3, 5, 7, 8, 10 });
        case Scale::harmonicMinor:       return maskFrom ({ 0, 2, 3, 5, 7, 8, 11 });
        case Scale::melodicMinor:        return maskFrom ({ 0, 2, 3, 5, 7, 9, 11 });
        case Scale::dorian:              return maskFrom ({ 0, 2, 3, 5, 7, 9, 10 });
        case Scale::phrygian:            return maskFrom ({ 0, 1, 3, 5, 7, 8, 10 });
        case Scale::lydian:              return maskFrom ({ 0, 2, 4, 6, 7, 9, 11 });
        case Scale::mixolydian:          return maskFrom ({ 0, 2, 4, 5, 7, 9, 10 });
        case Scale::locrian:             return maskFrom ({ 0, 1, 3, 5, 6, 8, 10 });
        case Scale::phrygianDominant:    return maskFrom ({ 0, 1, 4, 5, 7, 8, 10 });
        case Scale::wholeHalfDiminished: return maskFrom ({ 0, 2, 3, 5, 6, 8, 9, 11 });
        case Scale::custom:              break;
    }
    return 0;
}

std::uint16_t Key::mask() const noexcept
{
    const auto m = scale == Scale::custom ? (std::uint16_t) (customMask & 0xfff) : maskOf (scale);
    return m == 0 ? (std::uint16_t) 0xfff : m;
}

int nearestInKey (int note, const Key& key) noexcept
{
    const auto mask = key.mask();
    // Search outwards, lower side first, so a note halfway between two key notes takes the lower one.
    for (int distance = 0; distance < 12; ++distance)
    {
        if (inMask (mask, floorMod (note - distance - key.root, 12)))
            return note - distance;
        if (inMask (mask, floorMod (note + distance - key.root, 12)))
            return note + distance;
    }
    return note; // unreachable: the mask always has a note
}

int diatonicTarget (int note, int steps, const Key& key) noexcept
{
    const auto mask = key.mask();
    std::array<int, 12> degrees {}; // the key's pitch classes above the root, ascending
    int count = 0;
    for (int pc = 0; pc < 12; ++pc)
        if (inMask (mask, pc))
            degrees[(size_t) count++] = pc;

    // Where the (nearest in-key) note sits: which octave above the root, which degree within it.
    const auto anchor = nearestInKey (note, key);
    const auto fromRoot = anchor - key.root;
    const auto octave = floorDiv (fromRoot, 12);
    int degree = 0;
    while (degrees[(size_t) degree] != floorMod (fromRoot, 12))
        ++degree;

    // Move by scale steps, carrying whole trips around the scale into octaves.
    const auto moved = degree + steps;
    return key.root + 12 * (octave + floorDiv (moved, count)) + degrees[(size_t) floorMod (moved, count)];
}

int shiftFor (int note, const Interval& interval, const Key& key, OutOfScale rule) noexcept
{
    if (! interval.diatonic)
        return interval.steps + 12 * interval.octaves;

    const auto anchor = nearestInKey (note, key);
    const auto target = diatonicTarget (anchor, interval.steps, key);

    // In key, anchor == note and both rules agree. Out of key: parallel keeps the anchor's shift (the
    // harmony moves with the passing tone), snap lands on the anchor's harmony note.
    const auto shift = rule == OutOfScale::parallel ? target - anchor : target - note;
    return shift + 12 * interval.octaves;
}

} // namespace ampsim::harmony

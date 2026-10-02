#pragma once

#include <array>

namespace ampsim
{

/// Note lengths for tempo-synced times (BUILD_PLAN "Delay": whole through 1/16, dotted and triplet).
namespace tempo
{
enum class Division
{
    whole,
    half,
    quarter,
    eighth,
    sixteenth
};

enum class Feel
{
    straight,
    dotted, // one and a half times as long
    triplet // two thirds as long: three in the time of two
};

/// A note's length in beats (quarter notes): whole 4, half 2, quarter 1, eighth 1/2, sixteenth 1/4,
/// times 3/2 dotted or 2/3 triplet.
double beats (Division division, Feel feel) noexcept;

/// A note's length in milliseconds at a tempo: beats x 60000 / bpm. A dotted eighth at 120 BPM is
/// 0.75 x 500 = 375 ms.
double milliseconds (Division division, Feel feel, double bpm) noexcept;

/// The divisions as the delay's "sync" parameter lists them, in order (index -> division and feel).
struct Note
{
    Division division;
    Feel feel;
    const char* name;
};
extern const std::array<Note, 15> notes;
} // namespace tempo

/// Tap tempo (BUILD_PLAN "Delay"): the tempo is the average of the last few intervals between taps,
/// an interval that's way off the average is ignored as a stray tap, and a gap of more than 2 s starts
/// a new tempo. Two stray taps in a row that agree with each other are taken as a deliberate tempo
/// change. No allocation, so the audio thread can feed it footswitch taps.
class TapTempo
{
public:
    static constexpr int maxIntervals = 4;
    static constexpr double resetSeconds = 2.0;
    static constexpr double outlierTolerance = 0.25; // more than 25% off the average is a stray tap
    static constexpr double minBpm = 30.0, maxBpm = 300.0;

    /// A tap at a time in seconds (any steadily increasing clock). Returns true when the tempo changed.
    bool tap (double timeSeconds) noexcept;

    bool hasTempo() const noexcept { return count > 0; }
    double getBpm() const noexcept { return bpm; }
    void reset() noexcept;

private:
    std::array<double, maxIntervals> intervals {};
    int count = 0;
    double lastTap = -1.0e9;
    double strayInterval = 0.0; // the last rejected interval, waiting for a second that agrees
    double bpm = 0.0;
};

} // namespace ampsim

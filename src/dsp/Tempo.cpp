#include "Tempo.h"

#include <cmath>

namespace ampsim
{

namespace tempo
{
double beats (Division division, Feel feel) noexcept
{
    static constexpr double straight[] = { 4.0, 2.0, 1.0, 0.5, 0.25 };
    const auto b = straight[(int) division];
    return feel == Feel::dotted ? 1.5 * b : (feel == Feel::triplet ? b * 2.0 / 3.0 : b);
}

double milliseconds (Division division, Feel feel, double bpm) noexcept
{
    return beats (division, feel) * 60000.0 / bpm;
}

const std::array<Note, 15> notes { {
    { Division::whole, Feel::straight, "1/1" },     { Division::whole, Feel::dotted, "1/1 dotted" },     { Division::whole, Feel::triplet, "1/1 triplet" },
    { Division::half, Feel::straight, "1/2" },      { Division::half, Feel::dotted, "1/2 dotted" },      { Division::half, Feel::triplet, "1/2 triplet" },
    { Division::quarter, Feel::straight, "1/4" },   { Division::quarter, Feel::dotted, "1/4 dotted" },   { Division::quarter, Feel::triplet, "1/4 triplet" },
    { Division::eighth, Feel::straight, "1/8" },    { Division::eighth, Feel::dotted, "1/8 dotted" },    { Division::eighth, Feel::triplet, "1/8 triplet" },
    { Division::sixteenth, Feel::straight, "1/16" }, { Division::sixteenth, Feel::dotted, "1/16 dotted" }, { Division::sixteenth, Feel::triplet, "1/16 triplet" },
} };
} // namespace tempo

void TapTempo::reset() noexcept
{
    count = 0;
    lastTap = -1.0e9;
    strayInterval = 0.0;
}

bool TapTempo::tap (double now) noexcept
{
    const auto interval = now - lastTap;
    lastTap = now;

    // The first tap, or one after a long gap, starts a new tempo: there's no interval yet.
    if (interval > resetSeconds || interval <= 0.0)
    {
        count = 0;
        strayInterval = 0.0;
        return false;
    }

    const auto fastest = 60.0 / maxBpm, slowest = 60.0 / minBpm;
    if (interval < fastest || interval > slowest)
        return false; // a double trigger or an implausibly slow tap

    if (count > 0)
    {
        double average = 0.0;
        for (int i = 0; i < count; ++i)
            average += intervals[(size_t) i];
        average /= count;

        if (std::abs (interval - average) > outlierTolerance * average)
        {
            // A stray tap, unless it agrees with the previous stray one: then the player has changed
            // tempo, so start over from those two intervals.
            if (strayInterval > 0.0 && std::abs (interval - strayInterval) <= outlierTolerance * strayInterval)
            {
                intervals[0] = strayInterval;
                intervals[1] = interval;
                count = 2;
                strayInterval = 0.0;
                bpm = 60.0 / (0.5 * (intervals[0] + intervals[1]));
                return true;
            }
            strayInterval = interval;
            return false;
        }
    }

    strayInterval = 0.0;

    // Keep the most recent intervals, oldest first.
    if (count == maxIntervals)
    {
        for (int i = 1; i < maxIntervals; ++i)
            intervals[(size_t) i - 1] = intervals[(size_t) i];
        --count;
    }
    intervals[(size_t) count++] = interval;

    double sum = 0.0;
    for (int i = 0; i < count; ++i)
        sum += intervals[(size_t) i];
    bpm = 60.0 / (sum / count);
    return true;
}

} // namespace ampsim

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <algorithm>
#include <vector>

namespace ampsim
{

/// A whole-sample delay line whose delay can change without a click. Jumping the read position
/// straight to a new delay makes the output jump between two unrelated points of the waveform, which
/// is heard as a click. Instead, a change reads the old and new positions together for a short time
/// and fades linearly from one to the other. Used for the cab's mic delays and room pre-delay.
class SwitchableDelay
{
public:
    /// Not real-time: allocates.
    void prepare (int maxDelaySamples, int fadeSamples)
    {
        buffer.assign ((size_t) maxDelaySamples + 1, 0.0f);
        fadeLength = std::max (1, fadeSamples);
        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        oldDelay = delay;
        fadePosition = fadeLength; // no fade in progress
    }

    /// Audio thread. Takes effect with a fade. A change mid-fade starts a new fade from where the
    /// output currently is.
    void setDelay (int samples) noexcept
    {
        samples = std::clamp (samples, 0, (int) buffer.size() - 1);

        if (samples == target)
            return;

        target = samples;

        if (fadePosition < fadeLength)
            return; // finish the current fade first; process() picks up the new target after it

        oldDelay = delay;
        delay = target;
        fadePosition = 0;
    }

    /// Jumps straight to a delay with no fade. Only for when the output is silent anyway (prepare).
    void setDelayImmediately (int samples) noexcept
    {
        delay = target = oldDelay = std::clamp (samples, 0, (int) buffer.size() - 1);
        fadePosition = fadeLength;
    }

    int getDelay() const noexcept { return target; }

    float processSample (float x) noexcept
    {
        const auto size = (int) buffer.size();
        buffer[(size_t) writeIndex] = x;

        auto y = buffer[(size_t) ((writeIndex - delay + size) % size)];

        if (fadePosition < fadeLength)
        {
            const auto t = (float) (fadePosition + 1) / (float) fadeLength;
            const auto old = buffer[(size_t) ((writeIndex - oldDelay + size) % size)];
            y = old + t * (y - old);

            if (++fadePosition == fadeLength && target != delay)
            {
                oldDelay = delay; // a change arrived mid-fade: start the next fade now
                delay = target;
                fadePosition = 0;
            }
        }

        writeIndex = (writeIndex + 1) % size;
        return y;
    }

private:
    std::vector<float> buffer;
    int writeIndex = 0;
    int delay = 0, target = 0, oldDelay = 0;
    int fadePosition = 1, fadeLength = 1;
};

} // namespace ampsim

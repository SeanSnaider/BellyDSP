// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "../dsp/SpscRing.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace ampsim::tonematch
{

/// Records the player's DI for tone match (docs/TONE_MATCH.md): the clean DI snapshot the chain takes
/// at the start of every buffer (BUILD_PLAN "Block design", decision 2), up to a minute of it.
///
/// The audio thread only ever copies into a preallocated lock-free ring (SpscRing, as the analyzer's
/// tap does) while recording is on: no allocation, no lock, no waiting. One other thread (the tone match
/// page's timer, on the message thread) starts and stops it and drains the ring into its own buffer.
/// The ring holds the whole minute (2^22 samples, 87 s at 48 kHz), so even a stalled reader loses
/// nothing; the consumer stops at maxSeconds.
class DiRecorder
{
public:
    static constexpr double maxSeconds = 60.0;
    static constexpr double sampleRate = 48000.0;
    static constexpr int maxSamples = (int) (maxSeconds * sampleRate);

    /// Allocates the ring (16 MB). Call once, before the audio thread runs (the processor's constructor).
    void prepare() { ring.prepare (maxSamples); }

    // ---- Audio thread ------------------------------------------------------------------------------

    /// Copies the buffer's DI in while recording is on. Wait-free.
    void push (const float* di, int numSamples) noexcept
    {
        if (recording.load (std::memory_order_acquire))
            ring.write (di, numSamples);
    }

    // ---- The one consumer thread --------------------------------------------------------------------

    /// Starts a new recording: the consumer's buffer is cleared, anything left in the ring dropped.
    void start()
    {
        recorded.clear();
        recorded.reserve ((size_t) maxSamples);
        ring.discardAll();
        recording.store (true, std::memory_order_release);
    }

    /// Stops recording, then takes whatever the audio thread had already written. A buffer the audio
    /// thread was in the middle of can still land after this; the next drain() or start() deals with it.
    void stop()
    {
        recording.store (false, std::memory_order_release);
        drain();
    }

    /// Moves what's waiting in the ring into the recording (up to maxSamples, then stops). Returns how
    /// many samples it took.
    int drain()
    {
        const auto ready = ring.getNumReady();
        if (ready <= 0)
            return 0;
        const auto room = maxSamples - (int) recorded.size();
        const auto old = recorded.size();
        recorded.resize (old + (size_t) std::max (0, std::min (ready, room)));
        const auto n = ring.read (recorded.data() + old, (int) (recorded.size() - old));
        if (room <= ready)
        {
            recording.store (false, std::memory_order_release);
            ring.discardAll();
        }
        return n;
    }

    bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }
    double recordedSeconds() const noexcept { return (double) recorded.size() / sampleRate; }
    const std::vector<float>& getRecording() const noexcept { return recorded; }

    /// For tests: samples the audio thread couldn't fit (never, unless the reader stalls for 87 s).
    uint64_t takeDroppedCount() noexcept { return ring.takeDroppedCount(); }

private:
    SpscRing<float> ring;
    std::atomic<bool> recording { false };
    std::vector<float> recorded; // consumer thread only
};

} // namespace ampsim::tonematch

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "../dsp/SpscRing.h"

#include <juce_core/juce_core.h>

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
/// nothing; the consumer stops at its limit.
///
/// Two ways to start. start(): from the next buffer on (Record without a target). arm() then, on the
/// audio thread, beginArmed(k): from sample k of the buffer the audio thread is in (play along: the
/// preview player reports the sample where the song's first sample sounded, and the processor starts the
/// recorder on it, so recording sample 0 is that sample). One atomic state (idle, armed, recording) with
/// a compare-exchange from armed to recording, so a Stop racing the start can't leave it recording.
class DiRecorder
{
public:
    static constexpr double maxSeconds = 60.0;
    static constexpr double sampleRate = 48000.0;
    static constexpr int maxSamples = (int) (maxSeconds * sampleRate);
    /// A play-along take may run past the minute by the device's round trip (and the user offset).
    static constexpr int takeHeadroomSamples = (int) (2.0 * sampleRate);

    /// Allocates the ring (16 MB). Call once, before the audio thread runs (the processor's constructor).
    void prepare() { ring.prepare (maxSamples + takeHeadroomSamples); }

    // ---- Audio thread ------------------------------------------------------------------------------

    /// Starts an armed recording at sample `offset` of the buffer being processed (the next push() takes
    /// its samples from there). Does nothing unless armed. Wait-free.
    void beginArmed (int offset) noexcept
    {
        auto expected = (int) State::armed;
        if (state.compare_exchange_strong (expected, (int) State::recording, std::memory_order_acq_rel))
            startOffset = std::max (0, offset);
    }

    /// Copies the buffer's DI in while recording is on. Wait-free.
    void push (const float* di, int numSamples) noexcept
    {
        if (state.load (std::memory_order_acquire) != (int) State::recording)
            return;
        const auto from = std::min (startOffset, numSamples);
        startOffset = 0;
        ring.write (di + from, numSamples - from);
    }

    // ---- The one consumer thread --------------------------------------------------------------------

    /// Starts a new recording from the next buffer: the consumer's buffer is cleared, anything left in the
    /// ring dropped.
    void start()
    {
        reset (maxSamples);
        state.store ((int) State::recording, std::memory_order_release);
    }

    /// Prepares a recording the audio thread starts (beginArmed), up to `limit` samples (at most a minute
    /// plus the take's headroom).
    void arm (int limit)
    {
        reset (juce::jlimit (1, maxSamples + takeHeadroomSamples, limit));
        state.store ((int) State::armed, std::memory_order_release);
    }

    /// Stops recording (or disarms), then takes whatever the audio thread had already written. A buffer the
    /// audio thread was in the middle of can still land after this; the next drain() or start() deals with it.
    void stop()
    {
        state.store ((int) State::idle, std::memory_order_release);
        drain();
    }

    /// Moves what's waiting in the ring into the recording (up to the limit, then stops). Returns how
    /// many samples it took.
    int drain()
    {
        const auto ready = ring.getNumReady();
        if (ready <= 0)
            return 0;
        const auto room = limitSamples - (int) recorded.size();
        const auto old = recorded.size();
        recorded.resize (old + (size_t) std::max (0, std::min (ready, room)));
        const auto n = ring.read (recorded.data() + old, (int) (recorded.size() - old));
        if (room <= ready)
        {
            state.store ((int) State::idle, std::memory_order_release);
            ring.discardAll();
        }
        return n;
    }

    bool isRecording() const noexcept { return state.load (std::memory_order_acquire) == (int) State::recording; }
    bool isArmed() const noexcept { return state.load (std::memory_order_acquire) == (int) State::armed; }
    double recordedSeconds() const noexcept { return (double) recorded.size() / sampleRate; }
    const std::vector<float>& getRecording() const noexcept { return recorded; }

    /// For tests: samples the audio thread couldn't fit (never, unless the reader stalls for 87 s).
    uint64_t takeDroppedCount() noexcept { return ring.takeDroppedCount(); }

private:
    enum class State
    {
        idle,
        armed,
        recording
    };

    void reset (int limit)
    {
        state.store ((int) State::idle, std::memory_order_release);
        limitSamples = limit;
        recorded.clear();
        recorded.reserve ((size_t) limit);
        ring.discardAll();
    }

    SpscRing<float> ring;
    std::atomic<int> state { (int) State::idle };
    int startOffset = 0;                // audio thread
    int limitSamples = maxSamples;      // consumer thread
    std::vector<float> recorded;        // consumer thread only
};

} // namespace ampsim::tonematch

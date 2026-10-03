// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Handoff.h"

#include <mutex>

namespace ampsim
{

/// One cab mic: convolution with a user-supplied impulse response (IR) file. The Cab block combines
/// three of these (two close mics and a room mic); on its own it's a complete one-mic cab.
///
/// A cab and mic form a linear, time-invariant system, so the IR describes them completely and
/// convolving the amp's output with it reproduces them:
///     y[n] = sum_k h[k] x[n-k]
/// JUCE's Convolution does this with FFTs and zero added latency: uniformly partitioned by default,
/// or with a non-uniform head and tail for long IRs (the Gardner 1995 scheme: short partitions first
/// so output starts immediately, longer ones for the tail where they're cheaper).
class CabIR : public Block
{
public:
    /// Longer IRs are cut to this and faded out (BUILD_PLAN "IR loading").
    static constexpr double maxIRSeconds = 1.0;

    struct Options
    {
        bool stereo = false;        // true: a two-channel IR feeds left and right separately (the room mic)
        int nonUniformHeadSize = 0; // > 0: head/tail partitioning, for long IRs
    };

    /// Which channel of a stereo file a mono mic uses. Close mics take the left by default.
    enum class Channel
    {
        left,
        right
    };

    struct LoadResult
    {
        bool ok = false;
        juce::String message;
        int numSamples = 0;
        double sampleRate = 0.0;
        double gain = 1.0; // linear gain applied to the file's samples by the loudness matching
        int numChannels = 0;
    };

    CabIR();
    explicit CabIR (Options options);

    /// Any non-audio thread. Reads the file, picks the channel(s), caps the length, loudness-matches
    /// it, and queues it for the audio thread.
    LoadResult loadFile (const juce::File& file, Channel channel = Channel::left);

    /// Same as loadFile() but from samples already in memory. A two-channel buffer is kept as stereo
    /// only for a stereo mic; otherwise the first channel is used.
    LoadResult loadSamples (juce::AudioBuffer<float> samples, double sampleRate, const juce::String& name);

    /// Any non-audio thread: removes the IR, so the mic goes back to passthrough. (Abrupt: the processor
    /// only does it inside a preset change, while the output is muted.)
    void clear();

    /// Any non-audio thread: the most recently loaded IR's first channel, after loudness matching.
    /// The cab uses it to align its close mics.
    std::vector<float> getLoadedIR() const;

    /// Any non-audio thread. Frees IR buffers the audio thread has finished with.
    void collectGarbage() { handoff.collect(); }

    /// Audio thread, for tests: whether an IR has been installed (false = passthrough).
    bool hasImpulseResponse() const noexcept { return hasIR; }

    /// Audio thread: installs a newly loaded IR if one is waiting. process() does this itself; a
    /// container that skips process() for a mic with no IR yet must call it every buffer, or the
    /// first IR would never arrive.
    void pollPendingIR() noexcept { installPendingIR(); }

    /// Audio thread: whether JUCE has swapped in the engine for the IR (it starts with a one-sample
    /// identity engine and crossfades to the real one over 50 ms once its background thread builds it).
    bool isEngineReady() const noexcept { return hasIR && (convolution->getCurrentIRSize() > 1 || expectedLength <= 1); }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override { convolution->reset(); }
    bool isStereo() const override { return true; }

private:
    struct PendingIR
    {
        juce::AudioBuffer<float> samples;
        double sampleRate = 0.0;
    };

    void installPendingIR();

    const Options options;
    Handoff<PendingIR> handoff;
    PendingIR* toRetire = nullptr;
    std::unique_ptr<juce::dsp::Convolution> convolution;
    bool hasIR = false;
    int expectedLength = 0; // the installed IR's length, before any resampling

    mutable std::mutex loadedMutex; // loader and message threads only
    std::vector<float> loadedIR;
};

} // namespace ampsim

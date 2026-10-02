#pragma once

#include "Block.h"
#include "Handoff.h"

namespace ampsim
{

/// The cab: convolution with a user-supplied impulse response (IR) .wav file. One mic for now;
/// the three-mic cab with movable mics comes later (BUILD_PLAN "Cab").
///
/// A cab and its mic form a linear, time-invariant system, so the IR describes it completely and
/// convolving the amp's output with the IR reproduces it:
///     y[n] = sum_k h[k] x[n-k]
/// JUCE's Convolution does this in the frequency domain with uniformly partitioned FFT convolution
/// and zero added latency.
///
/// This is the mono-to-stereo point of the chain: it convolves channel 0, then copies the result to
/// channel 1 (one mic means both sides are identical).
class CabIR : public Block
{
public:
    /// Longer IRs are cut to this and faded out (BUILD_PLAN "IR loading").
    static constexpr double maxIRSeconds = 1.0;

    struct LoadResult
    {
        bool ok = false;
        juce::String message;
        int numSamples = 0;
        double sampleRate = 0.0;
    };

    /// Message thread (or any non-audio thread). Reads the file, keeps the left channel, caps the
    /// length, normalizes it, and queues it for the audio thread.
    LoadResult loadFile (const juce::File& file);

    /// Same as loadFile() but from samples already in memory. Used by the tests.
    LoadResult loadSamples (juce::AudioBuffer<float> samples, double sampleRate, const juce::String& name);

    /// Any non-audio thread. Frees IR buffers the audio thread has finished with.
    void collectGarbage() { handoff.collect(); }

    /// Audio thread, for tests: whether an IR has been installed (false = passthrough).
    bool hasImpulseResponse() const noexcept { return hasIR; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override { convolution.reset(); }
    bool isStereo() const override { return true; }

private:
    struct PendingIR
    {
        juce::AudioBuffer<float> samples;
        double sampleRate = 0.0;
    };

    void installPendingIR();

    Handoff<PendingIR> handoff;
    PendingIR* toRetire = nullptr;
    juce::dsp::Convolution convolution; // default constructor = zero latency, uniform partitions
    bool hasIR = false;
};

} // namespace ampsim

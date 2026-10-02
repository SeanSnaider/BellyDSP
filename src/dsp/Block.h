#pragma once

#include <juce_dsp/juce_dsp.h>

namespace ampsim
{

/// Read-only extras that every block's process() receives (BUILD_PLAN "Block design", decision 2).
struct BlockContext
{
    /// Snapshot of the clean guitar (DI) for this buffer, copied before any block runs.
    /// Gates and the harmonizer's pitch detector read this, never the processed signal.
    /// It's const so no block can modify it.
    const float* di = nullptr;
    int numSamples = 0;
};

/// The interface every DSP unit implements: the C++ version of the Rust Block trait from the
/// old repo. "= 0" makes a method pure virtual (like a Java interface method), and the ones with
/// bodies are defaults a block can override.
class Block
{
public:
    virtual ~Block() = default;

    /// Allocate everything for the worst case here (decision 1). Never runs on the audio thread.
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;

    /// Runs on the audio thread: no allocation, locks, file I/O, or logging.
    /// Mono blocks read and write channel 0 only; stereo blocks use channels 0 and 1.
    virtual void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) = 0;

    /// Clears internal state (filter memory, delay lines) without allocating, so it's safe
    /// on the audio thread. The chain calls it right before re-enabling a fully bypassed block.
    virtual void reset() = 0;

    /// Delay this block adds, in samples. The whole chain must stay at zero.
    virtual int latencySamples() const { return 0; }

    /// False for mono blocks. The chain copies channel 0 to channel 1 once, right before
    /// the first stereo block (decision 3).
    virtual bool isStereo() const { return false; }
};

} // namespace ampsim

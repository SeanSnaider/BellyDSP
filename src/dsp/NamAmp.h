#pragma once

#include "Block.h"
#include "Handoff.h"

#include <NAM/dsp.h>

#include <atomic>
#include <memory>
#include <vector>

namespace ampsim
{

/// One amp slot: a .nam capture running on NeuralAmpModelerCore, the official NAM engine.
///
/// Threads:
///   loadModel() runs on a background thread. It parses the file, builds the network, allocates
///   its buffers, and prewarms it, then queues the finished model for the audio thread.
///   process() runs on the audio thread. When a new model is waiting, the old and new outputs are
///   crossfaded over 20 ms (both run during the fade), and the old model goes back through the
///   Handoff to be freed off the audio thread.
///
/// With no model loaded, the slot passes its input through unchanged.
class NamAmp : public Block
{
public:
    /// NAM captures are trained at 48 kHz, and v1 only runs at that rate (BUILD_PLAN "Sample rate").
    static constexpr double requiredSampleRate = 48000.0;

    /// Every model is normalized so the reference guitar DI comes out at this loudness (BS.1770 LUFS).
    static constexpr double targetLoudnessLufs = -18.0;

    /// Seconds of the reference guitar DI rendered through each model to measure its loudness.
    static constexpr double loudnessProbeSeconds = 4.0;

    static constexpr double switchFadeSeconds = 0.020;

    struct LoadResult
    {
        bool ok = false;
        juce::String message;          // why it failed, or a short description of what loaded
        double normalizationDb = 0.0;  // gain applied to the model's output
        double measuredLufs = 0.0;     // the model's output loudness on the reference DI, before normalizing
    };

    NamAmp() = default;
    ~NamAmp() override;

    /// Background thread; takes tens to hundreds of milliseconds. Pass normalize = false to hear
    /// (or test) the raw model level.
    LoadResult loadModel (const juce::File& file, bool normalize = true);

    /// Any non-audio thread. Frees models the audio thread has finished with.
    void collectGarbage() { handoff.collect(); }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;

    /// Real-time safe, so it can't clear the model's history: NAM's Reset() allocates. It only
    /// finishes a switch that's in progress.
    void reset() override;

    /// Audio thread, for tests and meters: true while a model switch is crossfading.
    bool isSwitching() const noexcept { return fading; }

    /// Audio thread, for tests: whether a model is running (false = passthrough).
    bool hasModel() const noexcept { return current != nullptr; }

private:
    struct Model
    {
        std::unique_ptr<nam::DSP> dsp;
        float normalizationGain = 1.0f; // linear
    };

    void pickUpNewModel();
    void finishSwitchIfDone();
    static void render (Model* model, float* input, float* output, int numSamples);

    Handoff<Model> handoff;

    // Audio thread state.
    Model* current = nullptr;   // nullptr = passthrough
    Model* fadingOut = nullptr; // the outgoing model (nullptr when fading in from passthrough)
    Model* toRetire = nullptr;  // done with, waiting for the retire slot to free up
    bool fading = false;
    int fadePosition = 0;
    int fadeLength = 960;

    std::vector<float> inputCopy, outgoing;

    // Written by prepare(), read by the loader thread.
    std::atomic<int> loaderMaxBlockSize { 4096 };
};

} // namespace ampsim

#pragma once

#include "dsp/Chain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <mutex>

/// The plugin: parameters, state, file loading, and the glue between JUCE's audio callback and the
/// DSP chain. All the actual DSP lives in src/dsp/. This class only reads knobs and calls the chain.
class AmpSimProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    AmpSimProcessor();
    ~AmpSimProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Amp Sim"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return ampsim::CabIR::maxIRSeconds; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /// Message thread. Loading happens on a background thread; watch getStatus() for the result.
    void loadModel (const juce::File& file);
    void loadImpulseResponse (const juce::File& file);

    /// True while a model or IR load is still running. Used by the tests.
    bool isLoading() const { return loadsInFlight.load() > 0; }

    struct Status
    {
        juce::String model { "No amp model loaded, so the clean DI passes through" };
        bool modelError = false;
        juce::String cab { "No cab IR loaded" };
        bool cabError = false;
        juce::String warning;
    };

    /// GUI thread. A copy of the current status lines.
    Status getStatus() const;

    /// For tests: the DSP chain. Only touch it from the thread that calls processBlock().
    ampsim::Chain& getChain() noexcept { return chain; }

    juce::AudioProcessorValueTreeState parameters;

    // Keys for the non-parameter state saved alongside the knobs.
    static inline const juce::Identifier modelPathKey { "modelPath" };
    static inline const juce::Identifier irPathKey { "irPath" };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;
    void setModelStatus (const juce::String& text, bool isError);
    void setCabStatus (const juce::String& text, bool isError);

    ampsim::Chain chain;

    std::atomic<float>* inputGainDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* cabBypass = nullptr;

    std::atomic<bool> sampleRateOk { true };
    std::atomic<double> deviceSampleRate { 0.0 };
    int preparedBlockSize = 0;

    mutable std::mutex statusMutex; // shared by the GUI and loader threads, never the audio thread
    Status status;
    std::atomic<int> loadsInFlight { 0 };

    // Declared last so it's destroyed first: its jobs use the chain and the status above.
    juce::ThreadPool loader { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimProcessor)
};

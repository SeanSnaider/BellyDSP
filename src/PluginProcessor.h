#pragma once

#include "dsp/Chain.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <mutex>

/// The plugin: parameters, state, file loading, MIDI, and the glue between JUCE's audio callback and
/// the DSP chain. All the actual DSP lives in src/dsp/. This class reads knobs and drives the chain.
class AmpSimProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    static constexpr int numAmpSlots = ampsim::AmpSection::numSlots;

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
    void loadModel (int slot, const juce::File& file);
    void loadImpulseResponse (const juce::File& file);

    /// True while a model or IR load is still running. Used by the tests.
    bool isLoading() const { return loadsInFlight.load() > 0; }

    struct Status
    {
        std::array<juce::String, numAmpSlots> model { "Empty", "Empty", "Empty" };
        std::array<bool, numAmpSlots> modelError {};
        juce::String cab { "No cab IR loaded" };
        bool cabError = false;
        juce::String warning;
    };

    /// GUI thread. A copy of the current status lines.
    Status getStatus() const;

    /// For tests: the DSP chain. Only touch it from the thread that calls processBlock().
    ampsim::Chain& getChain() noexcept { return chain; }

    /// For tests: what the timer does, run on demand (frees retired objects, syncs MIDI slot changes
    /// to the slot parameter).
    void runHousekeeping() { timerCallback(); }

    juce::AudioProcessorValueTreeState parameters;

    // Parameter IDs. Permanent once presets exist: never rename one, add a new ID instead.
    static juce::String ampParamId (int slot, const juce::String& name) { return "amp" + juce::String (slot + 1) + "_" + name; }
    static inline const juce::String slotParamId { "amp_slot" };

    // Keys for the non-parameter state saved alongside the knobs.
    static juce::Identifier modelPathKey (int slot) { return "amp" + juce::String (slot + 1) + "ModelPath"; }
    static inline const juce::Identifier legacyModelPathKey { "modelPath" }; // milestone 1's single slot
    static inline const juce::Identifier irPathKey { "irPath" };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;
    void setModelStatus (int slot, const juce::String& text, bool isError);
    void setCabStatus (const juce::String& text, bool isError);
    void handleMidi (const juce::MidiBuffer& midi);

    ampsim::Chain chain;

    struct SlotParameters
    {
        std::atomic<float>* inputTrim = nullptr;
        std::atomic<float>* outputTrim = nullptr;
        std::array<std::atomic<float>*, ampsim::AmpTone::numBands> tone {};
    };

    std::atomic<float>* inputGainDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* cabBypass = nullptr;
    std::atomic<float>* ampSlot = nullptr;
    std::array<SlotParameters, numAmpSlots> slotParameters;

    int lastSlotParameter = -1;              // audio thread: the slot parameter value last acted on
    std::atomic<int> midiSlotRequest { -1 }; // audio thread to timer: a footswitch picked this slot

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

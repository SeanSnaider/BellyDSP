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
    static constexpr int numCabMics = ampsim::Cab::numCloseMics + 1; // close mic 1, close mic 2, room
    static constexpr int roomMic = ampsim::Cab::numCloseMics;

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
    void loadCabIR (int mic, const juce::File& file); // mic 0, 1: close mics; 2: room

    /// True while a model or IR load is still running. Used by the tests.
    bool isLoading() const { return loadsInFlight.load() > 0; }

    struct Status
    {
        std::array<juce::String, numAmpSlots> model { "Empty", "Empty", "Empty" };
        std::array<bool, numAmpSlots> modelError {};
        std::array<juce::String, numCabMics> cab { "No IR", "No IR", "No IR" };
        std::array<bool, numCabMics> cabError {};
        juce::String alignment;
        juce::String warning;
    };

    /// GUI thread. A copy of the current status lines.
    Status getStatus() const;

    /// For tests: the DSP chain. Only touch it from the thread that calls processBlock().
    ampsim::Chain& getChain() noexcept { return chain; }

    /// For tests: what the timer does, run on demand (frees retired objects, syncs MIDI slot changes
    /// to the slot parameter, reloads an IR whose channel choice changed).
    void runHousekeeping() { timerCallback(); }

    juce::AudioProcessorValueTreeState parameters;

    // Parameter IDs. Permanent once presets exist: never rename one, add a new ID instead.
    static juce::String ampParamId (int slot, const juce::String& name) { return "amp" + juce::String (slot + 1) + "_" + name; }
    static juce::String cabParamId (int mic, const juce::String& name)
    {
        return "cab_" + (mic == roomMic ? juce::String ("room") : "mic" + juce::String (mic + 1)) + "_" + name;
    }
    static inline const juce::String slotParamId { "amp_slot" };

    // Keys for the non-parameter state saved alongside the knobs.
    static juce::Identifier modelPathKey (int slot) { return "amp" + juce::String (slot + 1) + "ModelPath"; }
    static juce::Identifier cabPathKey (int mic)
    {
        return mic == roomMic ? juce::Identifier ("cabRoomPath") : juce::Identifier ("cabMic" + juce::String (mic + 1) + "Path");
    }
    static inline const juce::Identifier legacyModelPathKey { "modelPath" }; // milestone 1's single slot
    static inline const juce::Identifier legacyIRPathKey { "irPath" };       // milestone 1's single cab IR

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;
    void setModelStatus (int slot, const juce::String& text, bool isError);
    void setCabStatus (int mic, const juce::String& text, bool isError);
    void handleMidi (const juce::MidiBuffer& midi);
    void applyCabParameters();
    std::atomic<float>* raw (const juce::String& id) const { return parameters.getRawParameterValue (id); }

    ampsim::Chain chain;

    struct SlotParameters
    {
        std::atomic<float>* inputTrim = nullptr;
        std::atomic<float>* outputTrim = nullptr;
        std::array<std::atomic<float>*, ampsim::AmpTone::numBands> tone {};
    };

    struct MicParameters
    {
        std::atomic<float>* level = nullptr;
        std::atomic<float>* pan = nullptr;
        std::atomic<float>* invert = nullptr;
        std::atomic<float>* delay = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* channel = nullptr;
    };

    std::atomic<float>* inputGainDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* cabBypass = nullptr;
    std::atomic<float>* ampSlot = nullptr;
    std::array<SlotParameters, numAmpSlots> slotParameters;
    std::array<MicParameters, ampsim::Cab::numCloseMics> micParameters;
    std::atomic<float>* roomLevel = nullptr;
    std::atomic<float>* roomPreDelay = nullptr;
    std::atomic<float>* roomMute = nullptr;
    std::atomic<float>* cabAlign = nullptr;
    std::atomic<float>* lowCutOn = nullptr;
    std::atomic<float>* lowCutFreq = nullptr;
    std::atomic<float>* lowCutSlope = nullptr;
    std::atomic<float>* highCutOn = nullptr;
    std::atomic<float>* highCutFreq = nullptr;
    std::atomic<float>* highCutSlope = nullptr;

    int lastSlotParameter = -1;              // audio thread: the slot parameter value last acted on
    std::atomic<int> midiSlotRequest { -1 }; // audio thread to timer: a footswitch picked this slot
    std::array<int, ampsim::Cab::numCloseMics> loadedChannel { 0, 0 }; // message thread: channel each close mic was read with

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

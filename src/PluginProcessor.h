#pragma once

#include "BlockParameters.h"
#include "MidiMap.h"
#include "Presets.h"
#include "dsp/Chain.h"
#include "dsp/Tempo.h"
#include "dsp/TunerThread.h"

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
    void releaseResources() override { tuner.release(); }
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
    void loadCabIR (int mic, const juce::File& file); // mic 0, 1: close mics; 2: room. A folder is a cab pack.

    /// Message thread: empty an amp slot or a cab mic (queued behind any load already running).
    void clearModel (int slot);
    void clearCabIR (int mic);

    /// Message thread: a close mic's cab pack positions (empty without a pack), for the position pad.
    std::vector<ampsim::CabPack::Point> getCabPackPoints (int mic) const { return chain.cab.getPackPoints (mic); }

    /// A moving mic is re-morphed at most this often (BUILD_PLAN "Movable mics": every ~40 ms).
    static constexpr double morphIntervalMs = 40.0;

    /// For tests: how many times a moving mic's IR has been re-morphed.
    int getMorphCount() const { return morphCount.load(); }

    /// A change to the input calibration reloads the captures once the setting has been still this
    /// long, so dragging the knob doesn't queue a reload per step.
    static constexpr double calibrationSettleMs = 300.0;

    /// For tests: how many times a calibration change has reloaded the captures.
    int getCalibrationReloadCount() const { return calibrationReloads; }

    /// Effect sections. Blocks have permanent names ("gate", "comp", "eq", ...) used in saved state, so
    /// the order survives blocks being added later. Message thread. Unknown names are ignored, and a
    /// missing block goes where it breaks the fewest pairs of the default order (the gate first).
    void setSectionOrder (ampsim::Chain::Section section, const juce::StringArray& names);
    juce::StringArray getSectionOrder (ampsim::Chain::Section section) const;
    static juce::String blockName (ampsim::Chain::Slot slot);
    static juce::Identifier orderKey (ampsim::Chain::Section section) { return section == ampsim::Chain::Section::pre ? "preOrder" : "postOrder"; }

    /// Message thread: loads a preset between songs (BUILD_PLAN "Presets and scenes", Loading): the
    /// output fades to silence over 20 ms, the preset is applied, and the output fades back in once its
    /// captures and IRs have loaded. A short gap, never a click. Returns the validation result; problems
    /// found while applying (missing files, unknown IDs) arrive in getPresetWarnings().
    presets::ApplyResult loadPreset (const juce::var& preset);
    juce::var capturePreset (const juce::String& name) { return presets::capture (*this, name); }
    bool isChangingPreset() const { return presetStage != PresetStage::idle; }
    juce::StringArray getPresetWarnings() const { return presetWarnings; }
    juce::String getPresetName() const { return parameters.state.getProperty ("presetName").toString(); }

    /// Message thread: the MIDI mappings (footswitch toggles, expression pedals), and MIDI learn: the
    /// next controller that moves maps to the parameter.
    MidiMap& getMidiMap() noexcept { return midiMap; }
    void midiLearn (const juce::String& parameterId) { midiMap.startLearn (parameterId); }
    static inline const juce::Identifier midiMapKey { "midiMap" };

    /// Message thread: a tap on the GUI's tap tempo button. Taps from the GUI and the footswitch (the
    /// CC set by midi_tap_cc, value 64 or more) both reach the audio thread's TapTempo; the resulting
    /// tempo is used right away and written to tempo_bpm by the timer.
    void tapTempo() { ++guiTaps; }

    /// Any thread: the tempo in effect (BPM).
    double getTempo() const { return tapPending.load() ? tappedBpm.load() : (double) tempoBpm->load(); }

    /// Any thread: Learn for the gates (BUILD_PLAN "Gates"): with the strings muted, measures the noise
    /// floor for 2 s and sets the threshold above it. Gate A always learns (it detects even while off);
    /// Gate B too when it's on and unlinked (linked, it uses Gate A's decision). The timer writes the
    /// results into the threshold knobs.
    void learnGates();
    bool isLearningGates() const;
    bool isGateBOnItsOwn() const;
    float getGateLearnProgress() const { return chain.gateA.getLearnProgress(); }

    /// Any thread: gate meters, for Gate A (b = false) or Gate B.
    struct GateMeter
    {
        float detectorDb = -180.0f, openDb = 0.0f, closeDb = 0.0f, reductionDb = 0.0f;
        bool open = true;
    };
    GateMeter getGateMeter (bool b) const
    {
        const auto& g = b ? chain.gateB.gate : chain.gateA;
        return { g.getDetectorLevelDb(), g.getOpenThresholdDb(), g.getCloseThresholdDb(), g.getGainReductionDb(), g.isOpen() };
    }

    /// The tuner (BUILD_PLAN "Tuner"): engaged by `tuner_on` (a global switch, never restored from a saved
    /// state, so the app can't start muted), it reads the DI on its own thread; the output mutes while it's
    /// engaged unless `tuner_mute` is off. A4 is `tuner_a4`, 430 to 450 Hz.
    ampsim::TunerReading getTunerReading() const { return tuner.getReading(); }
    bool isTunerEngaged() const { return tuner.isEngaged(); }
    uint32_t getTunerUpdateCount() const { return tuner.getUpdateCount(); }

    /// Any thread: compressor gain reduction meters (dB).
    float getCompressorReduction (bool post) const { return post ? chain.postCompressor.getGainReductionDb() : chain.preCompressor.getGainReductionDb(); }

    /// True while a model or IR load (or a moving mic's re-morph) is still running. Used by the tests.
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
        std::atomic<float>* positionX = nullptr;
        std::atomic<float>* positionY = nullptr;
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

    // Moving mics. packActive (the mic has a pack, so its position matters) is cleared by the message
    // thread when a load starts and set by the loader when it ends; morphInFlight is cleared by the
    // loader when a morph is done. The rest is message thread only.
    std::array<std::atomic<bool>, ampsim::Cab::numCloseMics> packActive {};
    std::array<std::atomic<bool>, ampsim::Cab::numCloseMics> morphInFlight {};
    std::array<juce::Point<float>, ampsim::Cab::numCloseMics> morphedPosition; // the position last sent to the loader
    std::array<double, ampsim::Cab::numCloseMics> lastMorphStartMs {};

    // Input calibration (message thread): what the loaded captures were calibrated with, and a newer
    // setting waiting to settle.
    ampsim::NamAmp::Calibration currentCalibration() const;
    ampsim::NamAmp::Calibration appliedCalibration, pendingCalibration;
    double pendingSinceMs = 0.0;
    int calibrationReloads = 0;
    std::atomic<float>* calibrateInput = nullptr;
    std::atomic<float>* interfaceInputDbu = nullptr;

    void applyEffectParameters();
    params::GateParameters gateAParams, gateBParams;
    params::BoostParameters boostParams;
    params::OverdriveParameters overdriveParams;
    params::Raw driveOversampling;
    std::atomic<float>* gateLink = nullptr;
    int gateALearnSeen = 0, gateBLearnSeen = 0;
    params::CompressorParameters preCompParams, postCompParams;
    params::EqualizerParameters preEqParams, postEqParams;
    params::DelayParameters delayParams;
    params::ChorusParameters chorusParams;
    params::ReverbParameters reverbParams;

    // Reverb freeze from the footswitch (the CC set by midi_freeze_cc): toggled on the audio thread at
    // once (freezeOverride, -1 when the switch itself is in charge), and written into reverb_freeze by
    // the timer (freezeRequest).
    std::atomic<float>* freezeCc = nullptr;
    int freezeOverride = -1;
    std::atomic<int> freezeRequest { -1 };

    // Every other controller: forwarded by the audio thread, mapped by the timer.
    CcFifo ccFifo;
    MidiMap midiMap; // message thread

    // Tempo and tap tempo. The TapTempo and the sample clock belong to the audio thread.
    void registerTap (double timeSeconds);
    std::atomic<float>* tempoBpm = nullptr;
    std::atomic<float>* tapCc = nullptr;
    ampsim::TapTempo tap;
    juce::int64 samplesProcessed = 0;
    std::atomic<int> guiTaps { 0 };
    int guiTapsSeen = 0;
    std::atomic<double> tappedBpm { 120.0 };
    std::atomic<bool> tapPending { false };

    // Preset changes (message thread drives the stages; the audio thread does the fade).
    enum class PresetStage { idle, fadingOut, loading };
    static constexpr double presetFadeSeconds = 0.020, presetSettleMs = 150.0, presetTimeoutMs = 5000.0;
    PresetStage presetStage = PresetStage::idle;
    juce::var pendingPreset;
    double presetStageMs = 0.0;
    juce::StringArray presetWarnings;
    std::atomic<bool> presetMute { false }, presetSilent { false };
    std::atomic<juce::uint32> lastProcessMs { 0 };
    juce::SmoothedValue<float> presetGain { 1.0f }; // audio thread
    juce::SmoothedValue<float> tunerGain { 1.0f };  // audio thread: the tuner's mute, same fade

    ampsim::TunerThread tuner;
    std::atomic<float>* tunerOn = nullptr;
    std::atomic<float>* tunerMute = nullptr;
    std::atomic<float>* tunerA4 = nullptr;
    bool tunerMuting() const noexcept
    {
        return tunerOn->load (std::memory_order_relaxed) >= 0.5f && tunerMute->load (std::memory_order_relaxed) >= 0.5f;
    }
    std::atomic<int> morphCount { 0 };

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

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "BlockParameters.h"
#include "MidiMap.h"
#include "Presets.h"
#include "Scenes.h"
#include "dsp/Chain.h"
#include "dsp/SpscRing.h"
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

    /// Message thread: "Follow amp choice" (the cab page). Each amp slot can have a cab assigned to it: the
    /// IR file or cab pack last picked for close mic 1 while that slot was playing. While following,
    /// switching slots (from the GUI, a footswitch, a scene, or the host) loads the new slot's cab into
    /// close mic 1. Both are saved in the state and in presets (Presets.h: "cab_assign", "cab_follow").
    bool isCabFollowing() const { return (bool) parameters.state.getProperty (cabFollowKey, true); }
    void setCabFollow (bool shouldFollow);
    juce::File getCabAssignment (int slot) const;
    void setCabAssignment (int slot, const juce::File& fileOrPack);

    /// Message thread: a cab picked on the cab page (from the library list, a dropped file, or the file
    /// chooser): loads it into close mic 1 and assigns it to the slot that's playing.
    void pickCab (const juce::File& fileOrPack);

    /// Message thread: the slot change in effect now has been dealt with (a preset or a restored state
    /// brings its own cab, which following mustn't replace).
    void markCabFollowed() { lastFollowedSlot = juce::roundToInt (ampSlot->load()); }

    static inline const juce::Identifier cabFollowKey { "cabFollow" };
    static juce::Identifier cabAssignKey (int slot) { return "cabAssign" + juce::String (slot + 1); }

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

    /// The order inside Bloom, by effect name ("bitcrush", "phaser", "flanger"), saved like the sections'.
    /// Message thread. Unknown names are ignored and missing effects keep their default places at the end.
    void setBloomOrder (const juce::StringArray& names);
    juce::StringArray getBloomOrder() const;
    static inline const juce::Identifier bloomOrderKey { "bloomOrder" };

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

    /// Message thread: the preset's scenes (Scenes.h). A footswitch picks one with the scene CC
    /// (midi_scene_cc, default 70): value 0 is scene 1, ... 7 is scene 8.
    Scenes& getScenes() noexcept { return scenes; }
    bool recallScene (int index) { return scenes.recall (index, parameters); }
    void storeScene (int index) { scenes.store (index, parameters); }
    static inline const juce::Identifier scenesKey { "scenes" };
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

    /// Any thread, for the panel: the note the harmonizer hears (-1 for none) and a voice's interval.
    int getHarmonizerNote() const { return chain.harmonizer.getShownNote(); }
    int getHarmonizerShift (int voice) const { return chain.harmonizer.getShownShift (voice); }

    /// Any thread: the shared strip's gate light. True while Gate A lets the guitar through: its gain
    /// (0 dB when it's switched off, or its section is) above -6 dB, that is, its largest gain reduction
    /// in the last buffer under 6 dB.
    bool isGateOpen() const noexcept { return gateOpen.load (std::memory_order_relaxed); }
    static constexpr float gateOpenBelowReductionDb = 6.0f;

    /// Any thread: whether the device runs at 48 kHz (otherwise the output is muted), and its rate.
    bool isSampleRateOk() const noexcept { return sampleRateOk.load(); }
    double getDeviceSampleRate() const noexcept { return deviceSampleRate.load(); }

    /// Any thread, for compressor gain reduction meters (dB).
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

    // ---- Meters, the CPU meter, and the analyzer (BUILD_PLAN "GUI", Thread communication: audio to GUI
    // through atomics and a ring buffer; the audio thread never waits for the GUI) ----------------------

    /// GUI thread: the highest input (DI) sample and the highest output samples, left and right, as linear
    /// magnitudes, since the last call. Each call starts the next measurement.
    struct Peaks
    {
        float input = 0.0f, left = 0.0f, right = 0.0f;
    };
    Peaks takePeaks() noexcept;

    /// Any thread: the audio callback's time against its deadline (the buffer's duration), in percent,
    /// averaged over about 300 ms (ASSUMPTIONS U5).
    float getCpuLoad() const noexcept { return cpuLoad.load (std::memory_order_relaxed); }
    static constexpr double cpuAverageSeconds = 0.3;

    /// What the EQ page's analyzer reads (ASSUMPTIONS U6, U11): nothing (the audio thread then writes
    /// nothing), the guitar as it arrives (before the input gain; the pre EQ's page adds the gain), or
    /// the chain's output before the preset and tuner fades (the post section's output times the output
    /// level, which the post EQ's page takes back out).
    enum class AnalyzerTap
    {
        off,
        preSection,
        postSection
    };
    void setAnalyzerTap (AnalyzerTap newTap) noexcept { analyzerTap.store ((int) newTap, std::memory_order_relaxed); }
    AnalyzerTap getAnalyzerTap() const noexcept { return (AnalyzerTap) analyzerTap.load (std::memory_order_relaxed); }

    /// GUI thread (the ring's one reader): the tapped samples, mono, oldest first. When the GUI falls
    /// behind, the ring fills and the audio thread drops the newest samples and counts them
    /// (SpscRing::takeDroppedCount); it never waits.
    ampsim::SpscRing<float>& getAnalyzerRing() noexcept { return analyzerRing; }
    static constexpr int analyzerRingSize = 16384;

    /// The editor's UI scale (75 to 150%) and window size in UI points: kept in the app's state, never in
    /// presets (BUILD_PLAN "Presets and scenes": window size and UI scale are global settings).
    static inline const juce::Identifier uiScaleKey { "uiScale" }, uiWidthKey { "uiWidth" }, uiHeightKey { "uiHeight" };

    /// For tests: the DSP chain. Only touch it from the thread that calls processBlock().
    ampsim::Chain& getChain() noexcept { return chain; }

    /// For tests: what the timer does, run on demand (frees retired objects, syncs MIDI slot changes
    /// to the slot parameter, reloads an IR whose channel choice changed).
    void runHousekeeping() { timerCallback(); }

    /// Undo and redo (BUILD_PLAN "Presets and scenes", Editing), entirely on the GUI thread: the parameter
    /// tree records every parameter change into it, and the editor starts a new transaction at each mouse
    /// press, so one knob drag is one step. Loading a preset clears it. (Declared before `parameters`,
    /// which is built with it.)
    juce::UndoManager undoManager;

    juce::AudioProcessorValueTreeState parameters;

    /// A/B compare: two snapshots of the sound's settings (every parameter but the global ones, and the
    /// three orders), switched on the GUI thread. Switching saves the current settings into the slot being
    /// left and applies the other one; the first switch starts B as a copy of A. Captures and IRs are
    /// whatever is loaded, in both. Loading a preset starts again from A.
    void abSwitch();
    void abCopyToOther();
    bool isOnB() const noexcept { return abOnB; }
    void resetAB() { abSlots = {}; abOnB = false; }

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
    std::atomic<float>* ampBypass = nullptr;
    std::atomic<float>* preFxOn = nullptr;
    std::atomic<float>* postFxOn = nullptr;
    std::atomic<bool> gateOpen { true };
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
    int lastFollowedSlot = -1;               // message thread: the slot whose cab "Follow amp choice" last applied
    void applyCabAssignment (int slot);
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
    params::BloomParameters bloomParams;
    params::MultivoicerParameters multivoicerParams;
    params::HarmonizerParameters harmonizerParams;
    std::atomic<int> bloomOrderCode { 0 + 3 * 1 + 9 * 2 }; // the order as three base-3 digits, first effect lowest
    ampsim::Bloom::Order bloomOrder() const noexcept;
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
    Scenes scenes;   // message thread

    struct Snapshot
    {
        bool stored = false;
        std::map<juce::String, float> values; // normalized
        juce::StringArray pre, post, bloom;
    };
    Snapshot captureSnapshot() const;
    void applySnapshot (const Snapshot& snapshot);
    std::array<Snapshot, 2> abSlots;
    bool abOnB = false;
    std::atomic<float>* sceneCc = nullptr;

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

    // Meters (the audio thread raises them, the GUI takes them), the CPU meter, and the analyzer's tap.
    static void raisePeak (std::atomic<float>& peak, float value) noexcept;
    void measureCpu (juce::int64 startTicks, int numSamples) noexcept;
    void tapOutput (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    std::atomic<float> inputPeak { 0.0f }, outputPeakLeft { 0.0f }, outputPeakRight { 0.0f };
    std::atomic<float> cpuLoad { 0.0f };
    float cpuSmoothed = 0.0f;       // audio thread
    double cpuSampleRate = 48000.0; // set in prepareToPlay, read by the audio thread
    std::atomic<int> analyzerTap { 0 };
    ampsim::SpscRing<float> analyzerRing;
    std::vector<float> analyzerScratch; // audio thread: the output's mono sum, allocated in the constructor

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

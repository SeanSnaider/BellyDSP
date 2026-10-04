// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "platform/DataMigration.h"

AmpSimProcessor::AmpSimProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, &undoManager, "AmpSim", createParameterLayout()) // the state's root tag: saved state and sessions carry it, so it keeps the codename
{
    // Give every parameter its default through the path a preset or a host uses (normalized value to
    // plain value, snapped to its step). Constructed defaults on skewed ranges are off in their last
    // bits (an 8 kHz default reads 7999.9995 Hz), so without this a fresh processor and the same sound
    // loaded from a preset would differ by about 1e-5.
    for (auto* parameter : getParameters())
        parameter->setValueNotifyingHost (parameter->getDefaultValue());

    inputGainDb = raw ("input_gain");
    outputGainDb = raw ("output_gain");
    limiterOn = raw ("output_limit_on");
    limiterCeiling = raw ("output_limit_ceiling");
    cabBypass = raw ("cab_bypass");
    ampSlot = raw (slotParamId);
    ampBypass = raw ("amp_bypass");
    preFxOn = raw ("pre_fx_on");
    postFxOn = raw ("post_fx_on");

    for (int s = 0; s < numAmpSlots; ++s)
    {
        auto& p = slotParameters[(size_t) s];
        p.inputTrim = raw (ampParamId (s, "input_trim"));
        p.outputTrim = raw (ampParamId (s, "output_trim"));

        for (int b = 0; b < ampsim::AmpTone::numBands; ++b)
            p.tone[(size_t) b] = raw (ampParamId (s, juce::String (ampsim::AmpTone::bands[(size_t) b].name).toLowerCase()));
    }

    for (int m = 0; m < ampsim::Cab::numCloseMics; ++m)
    {
        auto& p = micParameters[(size_t) m];
        p.level = raw (cabParamId (m, "level"));
        p.pan = raw (cabParamId (m, "pan"));
        p.invert = raw (cabParamId (m, "invert"));
        p.delay = raw (cabParamId (m, "delay"));
        p.mute = raw (cabParamId (m, "mute"));
        p.channel = raw (cabParamId (m, "channel"));
        p.positionX = raw (cabParamId (m, "pos_x"));
        p.positionY = raw (cabParamId (m, "pos_y"));
    }

    roomLevel = raw (cabParamId (roomMic, "level"));
    roomPreDelay = raw (cabParamId (roomMic, "predelay"));
    roomMute = raw (cabParamId (roomMic, "mute"));
    cabAlign = raw ("cab_align");
    gateAParams.bind (parameters, "gate_a");
    gateBParams.bind (parameters, "gate_b");
    gateLink = raw ("gate_link");
    boostParams.bind (parameters);
    overdriveParams.bind (parameters);
    driveOversampling.bind (parameters, "drive_oversampling");
    preCompParams.bind (parameters, "comp_pre");
    postCompParams.bind (parameters, "comp_post");
    preEqParams.bind (parameters, "eq_pre");
    postEqParams.bind (parameters, "eq_post");
    delayParams.bind (parameters);
    chorusParams.bind (parameters);
    bloomParams.bind (parameters);
    multivoicerParams.bind (parameters);
    harmonizerParams.bind (parameters);
    reverbParams.bind (parameters);
    tempoBpm = raw ("tempo_bpm");
    tapCc = raw ("midi_tap_cc");
    freezeCc = raw ("midi_freeze_cc");
    sceneCc = raw ("midi_scene_cc");
    calibrateInput = raw ("input_calibrate");
    interfaceInputDbu = raw ("input_level_dbu");
    tunerOn = raw ("tuner_on");
    tunerMute = raw ("tuner_mute");
    tunerA4 = raw ("tuner_a4");
    appliedCalibration = pendingCalibration = currentCalibration();
    lowCutOn = raw ("cab_lowcut_on");
    lowCutFreq = raw ("cab_lowcut_freq");
    lowCutSlope = raw ("cab_lowcut_slope");
    highCutOn = raw ("cab_highcut_on");
    highCutFreq = raw ("cab_highcut_freq");
    highCutSlope = raw ("cab_highcut_slope");

    markCabFollowed();

    // A fresh start: the amp slots start on their built-in captures, loaded on the loader thread.
    fillFreshSlots();

    // The analyzer's ring and scratch are allocated once, here, never while audio runs.
    analyzerRing.prepare (analyzerRingSize);
    analyzerScratch.assign (2048, 0.0f);
    diRecorder.prepare(); // tone match's DI recorder: its minute of ring, allocated once, here

    // Frees models and IRs the audio thread has handed back, syncs footswitch slot changes, re-reads an
    // IR whose channel choice changed, and re-morphs moving mics. 50 Hz, so a mic being dragged is
    // re-morphed every 40 ms (two ticks), the plan's rate.
    startTimerHz (50);
}

AmpSimProcessor::~AmpSimProcessor()
{
    stopTimer();
    loader.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout AmpSimProcessor::createParameterLayout()
{
    // Parameter IDs are permanent once presets exist. Never rename one; add a new ID instead.
    using Float = juce::AudioParameterFloat;
    using Bool = juce::AudioParameterBool;
    using Choice = juce::AudioParameterChoice;
    using Int = juce::AudioParameterInt;

    const auto levelRange = juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f);
    const auto toneRange = juce::NormalisableRange<float> (-ampsim::AmpTone::rangeDb, ampsim::AmpTone::rangeDb, 0.1f);
    const auto micLevelRange = juce::NormalisableRange<float> (-40.0f, 12.0f, 0.1f);
    const auto dB = juce::AudioParameterFloatAttributes().withLabel ("dB");
    const auto hz = juce::AudioParameterFloatAttributes().withLabel ("Hz");
    const auto ms = juce::AudioParameterFloatAttributes().withLabel ("ms");
    const auto skewed = [] (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> range (lo, hi, 1.0f);
        range.setSkewForCentre (centre);
        return range;
    };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<Float> (juce::ParameterID { "input_gain", 1 }, "Input Gain", levelRange, 0.0f, dB));
    layout.add (std::make_unique<Float> (juce::ParameterID { "output_gain", 1 }, "Output Level", levelRange, 0.0f, dB));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "cab_bypass", 1 }, "Cab Bypass", false));

    // The output safety limiter (OutputLimiter.h): global settings, never part of a preset or a scene, so no
    // preset can switch the protection off. On by default, ceiling -1 dBFS.
    layout.add (std::make_unique<Bool> (juce::ParameterID { "output_limit_on", 1 }, "Output Limiter", true));
    layout.add (std::make_unique<Float> (juce::ParameterID { "output_limit_ceiling", 1 }, "Output Limiter Ceiling",
                                         juce::NormalisableRange<float> (ampsim::OutputLimiter::minCeilingDb, ampsim::OutputLimiter::maxCeilingDb, 0.1f),
                                         ampsim::OutputLimiter::defaultCeilingDb, dB));
    layout.add (std::make_unique<Choice> (juce::ParameterID { slotParamId, 1 }, "Amp Slot", juce::StringArray { "Amp 1", "Amp 2", "Amp 3" }, 0));

    // The signal chain's bypass dots (the UI handoff's bottom chain): the whole amp, and each effect
    // section as a unit. A section switch never touches its blocks' own switches: a block runs when its
    // own switch and its section's are both on (BUILD_PLAN decision log, 2026-10-03).
    layout.add (std::make_unique<Bool> (juce::ParameterID { "amp_bypass", 1 }, "Amp Bypass", false));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "pre_fx_on", 1 }, "Pre FX On", true));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "post_fx_on", 1 }, "Post FX On", true));

    // Input calibration (global settings, not part of presets): the analog level that reaches 0 dBFS on
    // this interface. +12 dBu is the Scarlett Solo 4th Gen's instrument input at minimum gain.
    layout.add (std::make_unique<Bool> (juce::ParameterID { "input_calibrate", 1 }, "Calibrate Input to Captures", true));
    layout.add (std::make_unique<Float> (juce::ParameterID { "input_level_dbu", 1 }, "Interface Input Level",
                                         juce::NormalisableRange<float> (-10.0f, 30.0f, 0.1f), 12.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("dBu")));

    // The tuner (global settings, not part of presets): engaged or not, mute while tuning, A4.
    layout.add (std::make_unique<Bool> (juce::ParameterID { "tuner_on", 1 }, "Tuner", false));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "tuner_mute", 1 }, "Mute While Tuning", true));
    layout.add (std::make_unique<Float> (juce::ParameterID { "tuner_a4", 1 }, "Tuner A4", juce::NormalisableRange<float> (430.0f, 450.0f, 0.1f), 440.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    for (int s = 0; s < numAmpSlots; ++s)
    {
        const auto prefix = "Amp " + juce::String (s + 1) + " ";
        layout.add (std::make_unique<Float> (juce::ParameterID { ampParamId (s, "input_trim"), 1 }, prefix + "Input Trim", levelRange, 0.0f, dB));
        layout.add (std::make_unique<Float> (juce::ParameterID { ampParamId (s, "output_trim"), 1 }, prefix + "Output Trim", levelRange, 0.0f, dB));

        for (const auto& band : ampsim::AmpTone::bands)
            layout.add (std::make_unique<Float> (juce::ParameterID { ampParamId (s, juce::String (band.name).toLowerCase()), 1 },
                                                 prefix + band.name, toneRange, 0.0f, dB));
    }

    for (int m = 0; m < ampsim::Cab::numCloseMics; ++m)
    {
        const auto prefix = "Cab Mic " + juce::String (m + 1) + " ";
        layout.add (std::make_unique<Float> (juce::ParameterID { cabParamId (m, "level"), 1 }, prefix + "Level", micLevelRange, 0.0f, dB));
        // Pan reads C, L 50, R 70, and accepts the same when typed (or a plain number from -1 to 1).
        const auto panAttributes = juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                const auto percent = juce::roundToInt (std::abs (v) * 100.0f);
                return percent == 0 ? juce::String ("C") : (v < 0.0f ? "L " : "R ") + juce::String (percent);
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                const auto t = text.trim().toUpperCase();
                if (t.startsWith ("C")) return 0.0f;
                if (t.startsWith ("L")) return -juce::jlimit (0.0f, 100.0f, t.substring (1).trim().getFloatValue()) / 100.0f;
                if (t.startsWith ("R")) return juce::jlimit (0.0f, 100.0f, t.substring (1).trim().getFloatValue()) / 100.0f;
                return juce::jlimit (-1.0f, 1.0f, t.getFloatValue());
            });
        layout.add (std::make_unique<Float> (juce::ParameterID { cabParamId (m, "pan"), 1 }, prefix + "Pan",
                                             juce::NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f, panAttributes));
        layout.add (std::make_unique<Bool> (juce::ParameterID { cabParamId (m, "invert"), 1 }, prefix + "Invert", false));
        layout.add (std::make_unique<Int> (juce::ParameterID { cabParamId (m, "delay"), 1 }, prefix + "Delay", 0, ampsim::Cab::maxMicDelaySamples, 0,
                                           juce::AudioParameterIntAttributes().withLabel ("samples")));
        layout.add (std::make_unique<Bool> (juce::ParameterID { cabParamId (m, "mute"), 1 }, prefix + "Mute", false));
        layout.add (std::make_unique<Choice> (juce::ParameterID { cabParamId (m, "channel"), 1 }, prefix + "IR Channel",
                                              juce::StringArray { "Left", "Right" }, 0));
        // Where the mic sits on a cab pack's map (only used once a pack is loaded): x across the
        // speaker, 0 at the dust cap to 1 at the cone's edge; y the distance, 0 the closest capture.
        layout.add (std::make_unique<Float> (juce::ParameterID { cabParamId (m, "pos_x"), 1 }, prefix + "Position",
                                             juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        layout.add (std::make_unique<Float> (juce::ParameterID { cabParamId (m, "pos_y"), 1 }, prefix + "Distance",
                                             juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    }

    layout.add (std::make_unique<Float> (juce::ParameterID { cabParamId (roomMic, "level"), 1 }, "Cab Room Level", micLevelRange, -6.0f, dB));
    layout.add (std::make_unique<Float> (juce::ParameterID { cabParamId (roomMic, "predelay"), 1 }, "Cab Room Pre-delay",
                                         juce::NormalisableRange<float> (0.0f, (float) ampsim::Cab::maxRoomPreDelayMs, 0.1f), 0.0f, ms));
    layout.add (std::make_unique<Bool> (juce::ParameterID { cabParamId (roomMic, "mute"), 1 }, "Cab Room Mute", false));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "cab_align", 1 }, "Cab Auto Align", true));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "cab_lowcut_on", 1 }, "Cab Low Cut", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { "cab_lowcut_freq", 1 }, "Cab Low Cut Frequency", skewed (20.0f, 500.0f, 100.0f), 80.0f, hz));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "cab_lowcut_slope", 1 }, "Cab Low Cut Slope", juce::StringArray { "12 dB/oct", "24 dB/oct" }, 0));
    layout.add (std::make_unique<Bool> (juce::ParameterID { "cab_highcut_on", 1 }, "Cab High Cut", false));
    layout.add (std::make_unique<Float> (juce::ParameterID { "cab_highcut_freq", 1 }, "Cab High Cut Frequency", skewed (2000.0f, 20000.0f, 8000.0f), 8000.0f, hz));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "cab_highcut_slope", 1 }, "Cab High Cut Slope", juce::StringArray { "12 dB/oct", "24 dB/oct" }, 0));

    // Effects. Compressors start off (style presets switch them on); EQs start on, and flat they
    // pass the signal through bit for bit.
    params::GateParameters::addTo (layout, "gate_a", "Gate A");
    params::GateParameters::addTo (layout, "gate_b", "Gate B");
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "gate_link", 1 }, "Gate Link", true));
    params::BoostParameters::addTo (layout);
    params::OverdriveParameters::addTo (layout);
    // Global (not in presets): 8x costs about 1.5 to 1.9x the CPU of 4x (BUILD_PLAN "Boost and Overdrive").
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "drive_oversampling", 1 }, "Drive Oversampling",
                                                              juce::StringArray { "4x", "8x" }, 0));
    params::CompressorParameters::addTo (layout, "comp_pre", "Pre Comp", false);
    params::EqualizerParameters::addTo (layout, "eq_pre", "Pre EQ", true);
    params::EqualizerParameters::addTo (layout, "eq_post", "Post EQ", true);
    params::CompressorParameters::addTo (layout, "comp_post", "Post Comp", false);
    params::HarmonizerParameters::addTo (layout);
    params::MultivoicerParameters::addTo (layout);
    params::BloomParameters::addTo (layout);
    params::ChorusParameters::addTo (layout);
    params::DelayParameters::addTo (layout);
    params::ReverbParameters::addTo (layout);

    // Tempo (saved with presets) and the footswitch CC that taps it.
    layout.add (std::make_unique<Float> (juce::ParameterID { "tempo_bpm", 1 }, "Tempo", juce::NormalisableRange<float> (30.0f, 300.0f, 0.1f), 120.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("BPM")));
    layout.add (std::make_unique<Int> (juce::ParameterID { "midi_tap_cc", 1 }, "Tap Tempo CC", 0, 127, 80));
    layout.add (std::make_unique<Int> (juce::ParameterID { "midi_freeze_cc", 1 }, "Reverb Freeze CC", 0, 127, 81));
    layout.add (std::make_unique<Int> (juce::ParameterID { "midi_scene_cc", 1 }, "Scene CC", 0, 127, 70));
    return layout;
}

void AmpSimProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    deviceSampleRate = sampleRate;
    cpuSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    sampleRateOk = std::abs (sampleRate - ampsim::NamAmp::requiredSampleRate) < 0.5;
    preparedBlockSize = juce::jmax (1, samplesPerBlock);

    // Start on the saved slot and cab settings without ramps: prepare() snaps fades to the targets.
    lastSlotParameter = juce::roundToInt (ampSlot->load());
    chain.amp.selectSlot (lastSlotParameter);
    applyCabParameters();
    applyEffectParameters();
    chain.setBypassed (ampsim::Chain::Slot::limiter, limiterOn->load() < 0.5f);
    chain.limiter.setCeilingDb (limiterCeiling->load());

    chain.prepare (sampleRate, preparedBlockSize);
    setLatencySamples (chain.latencySamples());

    presetGain.reset (sampleRate, presetFadeSeconds);
    presetGain.setCurrentAndTargetValue (presetMute.load() ? 0.0f : 1.0f);
    tunerGain.reset (sampleRate, presetFadeSeconds);
    tunerGain.setCurrentAndTargetValue (tunerMuting() ? 0.0f : 1.0f);

    // Allocates the tuner's ring and analysis, and (re)starts its thread.
    tuner.setReferenceA4 ((double) tunerA4->load());
    tuner.setEngaged (tunerOn->load() >= 0.5f);
    tuner.prepare (sampleRate, preparedBlockSize);
}

bool AmpSimProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Mono guitar in, stereo out. In the standalone app a mono input bus takes the first enabled
    // input channel, which on the Solo is input 1, the guitar (channel 0).
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
           && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void AmpSimProcessor::handleMidi (const juce::MidiBuffer& midi)
{
    // Program change 0, 1, 2 (shown as 1, 2, 3 on most footswitches) selects amp slot 1, 2, 3. The
    // switch happens here, on the audio thread, right away. The slot parameter catches up from the
    // timer, because setting a parameter notifies listeners, which can lock and allocate.
    // Read the raw bytes rather than building MidiMessage objects, which keeps this path trivially
    // allocation-free.
    const auto tapController = juce::roundToInt (tapCc->load (std::memory_order_relaxed));
    const auto freezeController = juce::roundToInt (freezeCc->load (std::memory_order_relaxed));

    for (const auto metadata : midi)
    {
        const auto* data = metadata.data;

        if (metadata.numBytes >= 2 && (data[0] & 0xf0) == 0xc0 && data[1] < numAmpSlots)
        {
            chain.amp.selectSlot (data[1]);
            midiSlotRequest.store (data[1]);
        }

        // Tap tempo from the footswitch: the tap CC pressed (value 64 or more), timed to the sample.
        if (metadata.numBytes >= 3 && (data[0] & 0xf0) == 0xb0 && data[1] == tapController && data[2] >= 64)
            registerTap ((double) (samplesProcessed + metadata.samplePosition) / ampsim::NamAmp::requiredSampleRate);

        // Reverb freeze from the footswitch: each press toggles it, right away.
        if (metadata.numBytes >= 3 && (data[0] & 0xf0) == 0xb0 && data[1] == freezeController && data[2] >= 64)
        {
            const auto frozen = freezeOverride >= 0 ? freezeOverride == 1 : reverbParams.isFrozen();
            freezeOverride = frozen ? 0 : 1;
            freezeRequest.store (freezeOverride, std::memory_order_relaxed);
        }

        // Any other controller goes to the mappings (applied by the timer).
        if (metadata.numBytes >= 3 && (data[0] & 0xf0) == 0xb0 && data[1] != tapController && data[1] != freezeController)
            ccFifo.push (data[1], data[2]);
    }

    // Taps from the GUI's button, timed to this buffer.
    for (const auto taps = guiTaps.load (std::memory_order_relaxed); guiTapsSeen != taps; ++guiTapsSeen)
        registerTap ((double) samplesProcessed / ampsim::NamAmp::requiredSampleRate);
}

void AmpSimProcessor::applyCabParameters()
{
    const auto on = [] (const std::atomic<float>* p) { return p->load (std::memory_order_relaxed) >= 0.5f; };
    const auto value = [] (const std::atomic<float>* p) { return p->load (std::memory_order_relaxed); };

    for (int m = 0; m < ampsim::Cab::numCloseMics; ++m)
    {
        const auto& p = micParameters[(size_t) m];
        ampsim::Cab::CloseMicSettings settings;
        settings.levelDb = value (p.level);
        settings.pan = value (p.pan);
        settings.invert = on (p.invert);
        settings.delaySamples = juce::roundToInt (value (p.delay));
        settings.mute = on (p.mute);
        chain.cab.setCloseMic (m, settings);
    }

    ampsim::Cab::RoomSettings room;
    room.levelDb = value (roomLevel);
    room.preDelayMs = value (roomPreDelay);
    room.mute = on (roomMute);
    chain.cab.setRoom (room);
    chain.cab.setAutoAlign (on (cabAlign));

    ampsim::Cab::CutSettings cuts;
    cuts.lowCutOn = on (lowCutOn);
    cuts.lowCutHz = value (lowCutFreq);
    cuts.lowCutSlope = value (lowCutSlope) >= 0.5f ? ampsim::Cab::Slope::db24 : ampsim::Cab::Slope::db12;
    cuts.highCutOn = on (highCutOn);
    cuts.highCutHz = value (highCutFreq);
    cuts.highCutSlope = value (highCutSlope) >= 0.5f ? ampsim::Cab::Slope::db24 : ampsim::Cab::Slope::db12;
    chain.cab.setCuts (cuts);
}

void AmpSimProcessor::applyEffectParameters()
{
    using Slot = ampsim::Chain::Slot;

    // A block runs when its own switch is on and so is its section's (pre_fx_on, post_fx_on). The section
    // switch goes through each block's own bypass crossfade, so switching a section is as click-free as
    // switching its blocks one by one, and the blocks' parameters are never touched.
    const auto preOn = preFxOn->load (std::memory_order_relaxed) >= 0.5f;
    const auto postOn = postFxOn->load (std::memory_order_relaxed) >= 0.5f;
    const auto pre = [preOn] (bool on) { return ! (on && preOn); };   // bypassed?
    const auto post = [postOn] (bool on) { return ! (on && postOn); };

    chain.setBypassed (Slot::gateA, pre (gateAParams.isOn())); // off, it keeps detecting (Chain: keepRunning)
    chain.gateA.setSettings (gateAParams.read());
    chain.setBypassed (Slot::gateB, ! gateBParams.isOn());
    chain.gateB.gate.setSettings (gateBParams.read());
    chain.gateB.setLinked (gateLink->load (std::memory_order_relaxed) >= 0.5f);

    // The drive circuits run on volts: 0 dBFS is the interface's full scale (as for the NAM calibration).
    const auto oversampling = params::oversamplingFactor (driveOversampling);
    const auto volts = params::voltsAtFullScale ((double) interfaceInputDbu->load (std::memory_order_relaxed));
    chain.setBypassed (Slot::boost, pre (boostParams.isOn())); // off, both keep running (Chain: keepRunning)
    chain.boost.setSettings (boostParams.read (oversampling, volts));
    chain.setBypassed (Slot::overdrive, pre (overdriveParams.isOn()));
    chain.overdrive.setSettings (overdriveParams.read (oversampling, volts));
    chain.setBypassed (Slot::preCompressor, pre (preCompParams.isOn()));
    {
        // The pre instance compresses the DI, whose level sits far below its peaks: its auto makeup restores
        // the level there (Compressor.h, the gain staging audit).
        auto preSettings = preCompParams.read();
        preSettings.autoMakeupReferenceDb = (float) ampsim::Compressor::preAmpMakeupReferenceDb;
        chain.preCompressor.setSettings (preSettings);
    }
    chain.setBypassed (Slot::preEq, pre (preEqParams.isOn()));
    chain.preEq.setSettings (preEqParams.read());
    chain.setBypassed (Slot::postEq, post (postEqParams.isOn()));
    chain.postEq.setSettings (postEqParams.read());
    chain.setBypassed (Slot::postCompressor, post (postCompParams.isOn()));
    chain.postCompressor.setSettings (postCompParams.read());
    // The harmonizer maps notes with the tuner's A4 (BUILD_PLAN "Harmonizer", Key and scale).
    chain.setBypassed (Slot::harmonizer, post (harmonizerParams.isOn()));
    chain.harmonizer.setSettings (harmonizerParams.read ((double) tunerA4->load (std::memory_order_relaxed)));
    chain.setBypassed (Slot::multivoicer, post (multivoicerParams.isOn()));
    chain.multivoicer.setSettings (multivoicerParams.read());
    chain.setBypassed (Slot::bloom, post (bloomParams.isOn())); // Bloom takes this as its own bypass and keeps running
    chain.bloom.setSettings (bloomParams.read (getTempo(), bloomOrder()));
    chain.setBypassed (Slot::chorus, post (chorusParams.isOn()));
    chain.chorus.setSettings (chorusParams.read (getTempo()));
    chain.setBypassed (Slot::delay, post (delayParams.isOn())); // the delay and reverb take this as spillover
    chain.delay.setSettings (delayParams.read (getTempo()));

    // Once the timer has written a footswitch's freeze into the switch, the switch is in charge again.
    if (freezeOverride >= 0 && freezeRequest.load (std::memory_order_relaxed) < 0 && (reverbParams.isFrozen() ? 1 : 0) == freezeOverride)
        freezeOverride = -1;
    chain.setBypassed (Slot::reverb, post (reverbParams.isOn()));
    chain.reverb.setSettings (reverbParams.read (getTempo(), freezeOverride));
}

void AmpSimProcessor::registerTap (double timeSeconds)
{
    if (tap.tap (timeSeconds))
    {
        tappedBpm = juce::jlimit (30.0, 300.0, tap.getBpm());
        tapPending = true;
    }
}

void AmpSimProcessor::learnGates()
{
    chain.gateA.startLearn();
    if (isGateBOnItsOwn())
        chain.gateB.gate.startLearn();
}

bool AmpSimProcessor::isGateBOnItsOwn() const
{
    return gateBParams.isOn() && gateLink->load() < 0.5f;
}

bool AmpSimProcessor::isLearningGates() const
{
    // A Gate B switched off or relinked mid-measurement stops detecting, so it only counts while it runs.
    return chain.gateA.isLearning() || (chain.gateB.gate.isLearning() && isGateBOnItsOwn());
}

juce::String AmpSimProcessor::blockName (ampsim::Chain::Slot slot)
{
    using Slot = ampsim::Chain::Slot;
    if (slot == Slot::gateA)
        return "gate";
    if (slot == Slot::boost)
        return "boost";
    if (slot == Slot::bloom)
        return "bloom";
    if (slot == Slot::multivoicer)
        return "multivoicer";
    if (slot == Slot::harmonizer)
        return "harmonizer";
    if (slot == Slot::overdrive)
        return "overdrive";
    if (slot == Slot::preCompressor || slot == Slot::postCompressor)
        return "comp";
    if (slot == Slot::preEq || slot == Slot::postEq)
        return "eq";
    if (slot == Slot::chorus)
        return "chorus";
    if (slot == Slot::delay)
        return "delay";
    if (slot == Slot::reverb)
        return "reverb";
    return {}; // fixed blocks have no section name
}

void AmpSimProcessor::setSectionOrder (ampsim::Chain::Section section, const juce::StringArray& names)
{
    // Names to slots, skipping unknown and repeated names. A block the names leave out (one added since
    // they were saved, like the gate in a Phase 5 preset) goes where it breaks the fewest of the default
    // order's pairs with the blocks already placed, the latest such place on a tie: first for the gate
    // in "eq, comp", after both for the chorus in "comp, eq".
    const auto defaults = ampsim::Chain::defaultOrder (section);
    const auto rank = [&defaults] (ampsim::Chain::Slot slot) { return (size_t) (std::find (defaults.begin(), defaults.end(), slot) - defaults.begin()); };
    std::vector<ampsim::Chain::Slot> order;
    for (const auto& name : names)
        for (auto slot : defaults)
            if (blockName (slot) == name.trim() && std::find (order.begin(), order.end(), slot) == order.end())
                order.push_back (slot);
    for (size_t i = 0; i < defaults.size(); ++i)
    {
        if (std::find (order.begin(), order.end(), defaults[i]) != order.end())
            continue;
        size_t best = 0, fewest = defaults.size() + 1;
        for (size_t k = 0; k <= order.size(); ++k)
        {
            size_t broken = 0; // placed blocks on the wrong side of position k
            for (size_t m = 0; m < order.size(); ++m)
                broken += (m < k) != (rank (order[m]) < i) ? 1 : 0;
            if (broken <= fewest)
            {
                fewest = broken;
                best = k;
            }
        }
        order.insert (order.begin() + (long) best, defaults[i]);
    }

    chain.requestOrder (section, order);
    parameters.state.setProperty (orderKey (section), getSectionOrder (section).joinIntoString (","), nullptr);
}

ampsim::Bloom::Order AmpSimProcessor::bloomOrder() const noexcept
{
    const auto code = bloomOrderCode.load (std::memory_order_relaxed);
    return { (ampsim::Bloom::Effect) (code % 3), (ampsim::Bloom::Effect) (code / 3 % 3), (ampsim::Bloom::Effect) (code / 9 % 3) };
}

void AmpSimProcessor::setBloomOrder (const juce::StringArray& names)
{
    // Names to effects, skipping unknown and repeated names, then any effect not named, in default order.
    std::vector<ampsim::Bloom::Effect> order;
    for (const auto& name : names)
        for (const auto effect : ampsim::Bloom::defaultOrder)
            if (name.trim() == ampsim::Bloom::effectName (effect) && std::find (order.begin(), order.end(), effect) == order.end())
                order.push_back (effect);
    for (const auto effect : ampsim::Bloom::defaultOrder)
        if (std::find (order.begin(), order.end(), effect) == order.end())
            order.push_back (effect);

    bloomOrderCode.store ((int) order[0] + 3 * (int) order[1] + 9 * (int) order[2], std::memory_order_relaxed);
    parameters.state.setProperty (bloomOrderKey, getBloomOrder().joinIntoString (","), nullptr);
}

juce::StringArray AmpSimProcessor::getBloomOrder() const
{
    juce::StringArray names;
    for (const auto effect : bloomOrder())
        names.add (ampsim::Bloom::effectName (effect));
    return names;
}

AmpSimProcessor::Snapshot AmpSimProcessor::captureSnapshot() const
{
    Snapshot s;
    s.stored = true;
    for (auto* parameter : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter); ranged != nullptr && ! presets::isGlobal (ranged->paramID))
            s.values[ranged->paramID] = ranged->getValue();
    s.pre = getSectionOrder (ampsim::Chain::Section::pre);
    s.post = getSectionOrder (ampsim::Chain::Section::post);
    s.bloom = getBloomOrder();
    return s;
}

void AmpSimProcessor::applySnapshot (const Snapshot& s)
{
    for (const auto& [id, value] : s.values)
        if (auto* parameter = parameters.getParameter (id); parameter != nullptr && std::abs (parameter->getValue() - value) > 1.0e-9f)
            parameter->setValueNotifyingHost (value);
    setSectionOrder (ampsim::Chain::Section::pre, s.pre);
    setSectionOrder (ampsim::Chain::Section::post, s.post);
    setBloomOrder (s.bloom);
}

void AmpSimProcessor::abSwitch()
{
    undoManager.beginNewTransaction ("A/B");
    auto& leaving = abSlots[abOnB ? 1 : 0];
    auto& arriving = abSlots[abOnB ? 0 : 1];
    leaving = captureSnapshot();
    if (! arriving.stored)
        arriving = leaving; // B starts as a copy of A
    applySnapshot (arriving);
    abOnB = ! abOnB;
}

void AmpSimProcessor::abCopyToOther()
{
    abSlots[abOnB ? 0 : 1] = captureSnapshot();
}

juce::StringArray AmpSimProcessor::getSectionOrder (ampsim::Chain::Section section) const
{
    juce::StringArray names;
    for (auto slot : chain.getRequestedOrder (section))
        names.add (blockName (slot));
    return names;
}

void AmpSimProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // The CPU meter times the whole callback against its deadline.
    const auto startTicks = juce::Time::getHighResolutionTicks();

    // Denormals (tiny floats near zero) are very slow on some CPUs, and a decaying tail produces
    // them. This flushes them to zero for the duration of the callback (BUILD_PLAN "Denormals").
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples = buffer.getNumSamples();

    // The input meter reads the guitar as it arrives, before any gain: what decides whether the
    // interface clips.
    if (buffer.getNumChannels() > 0 && numSamples > 0)
        raisePeak (inputPeak, buffer.getMagnitude (0, 0, numSamples));

    // Output channels with no matching input start out holding garbage.
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (! sampleRateOk.load (std::memory_order_relaxed) || preparedBlockSize == 0
        || buffer.getNumChannels() < 2)
    {
        buffer.clear(); // the editor shows why
        measureCpu (startTicks, numSamples);
        return;
    }

    // The slot parameter changed (GUI, host, or the timer catching up with a footswitch).
    const auto slotParameter = juce::roundToInt (ampSlot->load (std::memory_order_relaxed));

    if (slotParameter != lastSlotParameter)
    {
        lastSlotParameter = slotParameter;
        chain.amp.selectSlot (slotParameter);
    }

    handleMidi (midi);

    chain.inputGain.setGainDecibels (inputGainDb->load (std::memory_order_relaxed));
    chain.outputGain.setGainDecibels (outputGainDb->load (std::memory_order_relaxed));
    chain.setBypassed (ampsim::Chain::Slot::limiter, limiterOn->load (std::memory_order_relaxed) < 0.5f);
    chain.limiter.setCeilingDb (limiterCeiling->load (std::memory_order_relaxed));
    chain.setBypassed (ampsim::Chain::Slot::cab, cabBypass->load (std::memory_order_relaxed) >= 0.5f);

    // The amp's bypass crossfades the amp section out to its own input with the chain's 10 ms bypass
    // fade. The three captures keep running on a copy of that input meanwhile (Chain: keepRunning), so
    // their history is current and switching the amp back on is as seamless as switching slots.
    chain.setBypassed (ampsim::Chain::Slot::amp, ampBypass->load (std::memory_order_relaxed) >= 0.5f);

    for (int s = 0; s < numAmpSlots; ++s)
    {
        auto& slot = chain.amp.slot (s);
        const auto& p = slotParameters[(size_t) s];
        slot.inputTrim.setGainDecibels (p.inputTrim->load (std::memory_order_relaxed));
        slot.outputTrim.setGainDecibels (p.outputTrim->load (std::memory_order_relaxed));

        // To the nearest 0.01 dB, so a centred knob (which reads 3.6e-7 dB after float snapping) is
        // exactly flat, and flat is bit-transparent.
        for (int b = 0; b < ampsim::AmpTone::numBands; ++b)
            slot.tone.setGainDb ((ampsim::AmpTone::Band) b, std::round (p.tone[(size_t) b]->load (std::memory_order_relaxed) * 100.0f) / 100.0f);
    }

    applyCabParameters();
    applyEffectParameters();

    // The tuner reads the clean guitar, before any block touches it (wait-free; nothing while disengaged).
    tuner.setEngaged (tunerOn->load (std::memory_order_relaxed) >= 0.5f);
    tuner.setReferenceA4 ((double) tunerA4->load (std::memory_order_relaxed));
    tuner.pushAudio (buffer.getReadPointer (0), numSamples);

    // The analyzer's pre tap: the guitar going into the chain (the GUI adds the input gain).
    const auto analyzerSource = (AnalyzerTap) analyzerTap.load (std::memory_order_relaxed);
    if (analyzerSource == AnalyzerTap::preSection)
        analyzerRing.write (buffer.getReadPointer (0), numSamples);

    // Hosts may occasionally send more samples than promised, so feed the chain in pieces that fit.
    auto io = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, 2);
    const auto maxChunk = (size_t) preparedBlockSize;

    for (size_t start = 0; start < (size_t) numSamples; start += maxChunk)
    {
        const auto len = juce::jmin (maxChunk, (size_t) numSamples - start);
        chain.process (io.getSubBlock (start, len));

        // Tone match's recorder takes the chain's own DI snapshot (wait-free; a no-op unless recording).
        diRecorder.push (chain.lastDISnapshot().data(), (int) len);
    }

    samplesProcessed += numSamples;

    // The Out meter's limit light: the limiter's largest gain reduction in this buffer (0 while it's off).
    if (! chain.isFullyBypassed (ampsim::Chain::Slot::limiter))
        raisePeak (limiterReduction, chain.limiter.getReductionDb());

    // The strip's gate light: whether Gate A is letting the guitar through. Its gain is the gate's (0 dB
    // while it's off or its section is), and the light is on while that gain is above -6 dB (the gate's
    // largest reduction in this buffer under 6 dB), so it goes out as the gate closes, not at the end of
    // the release.
    {
        const auto gateRunning = gateAParams.isOn() && preFxOn->load (std::memory_order_relaxed) >= 0.5f;
        gateOpen.store (! gateRunning || chain.gateA.getGainReductionDb() < gateOpenBelowReductionDb, std::memory_order_relaxed);
    }

    // The analyzer's post tap: the post section's output (times the output level, which the GUI takes out).
    if (analyzerSource == AnalyzerTap::postSection)
        tapOutput (buffer, numSamples);

    // Preset changes fade the whole output out, and back in once the new sound is ready; the tuner mutes
    // it while it's engaged (unless set to tune while hearing yourself). Both fade over 20 ms.
    presetGain.setTargetValue (presetMute.load (std::memory_order_relaxed) ? 0.0f : 1.0f);
    tunerGain.setTargetValue (tunerMuting() ? 0.0f : 1.0f);
    if (presetGain.isSmoothing() || presetGain.getCurrentValue() < 1.0f || tunerGain.isSmoothing() || tunerGain.getCurrentValue() < 1.0f)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);
        for (int n = 0; n < numSamples; ++n)
        {
            const auto g = presetGain.getNextValue() * tunerGain.getNextValue();
            left[n] *= g;
            right[n] *= g;
        }
    }
    presetSilent.store (presetMute.load (std::memory_order_relaxed) && ! presetGain.isSmoothing() && presetGain.getCurrentValue() <= 0.0f);
    lastProcessMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);

    // The output meters read what leaves the app.
    raisePeak (outputPeakLeft, buffer.getMagnitude (0, 0, numSamples));
    raisePeak (outputPeakRight, buffer.getMagnitude (1, 0, numSamples));
    measureCpu (startTicks, numSamples);
}

void AmpSimProcessor::raisePeak (std::atomic<float>& peak, float value) noexcept
{
    // A lock-free maximum: the compare-exchange only retries if the GUI took the peak in between (it
    // swaps in 0), so the loop runs once or twice and never waits on anything.
    auto current = peak.load (std::memory_order_relaxed);
    while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed))
    {
    }
}

AmpSimProcessor::Peaks AmpSimProcessor::takePeaks() noexcept
{
    return { inputPeak.exchange (0.0f, std::memory_order_relaxed), outputPeakLeft.exchange (0.0f, std::memory_order_relaxed),
             outputPeakRight.exchange (0.0f, std::memory_order_relaxed), limiterReduction.exchange (0.0f, std::memory_order_relaxed) };
}

void AmpSimProcessor::measureCpu (juce::int64 startTicks, int numSamples) noexcept
{
    // The callback's share of its deadline (the time the buffer takes to play), smoothed by a one-pole
    // average with a 300 ms time constant whatever the buffer size: for a buffer lasting T seconds the
    // coefficient is 1 - exp(-T / 0.3), so a step settles to 63% after 300 ms of audio.
    if (numSamples <= 0)
        return;
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks);
    const auto deadline = (double) numSamples / cpuSampleRate;
    const auto load = (float) (100.0 * seconds / deadline);
    const auto alpha = (float) (1.0 - std::exp (-deadline / cpuAverageSeconds));
    cpuSmoothed += alpha * (load - cpuSmoothed);
    cpuLoad.store (cpuSmoothed, std::memory_order_relaxed);
}

void AmpSimProcessor::tapOutput (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    // The mono sum of the stereo output, in pieces that fit the preallocated scratch buffer. A full
    // ring drops what doesn't fit (and counts it) rather than waiting for the GUI.
    const auto* left = buffer.getReadPointer (0);
    const auto* right = buffer.getReadPointer (1);
    const auto capacity = (int) analyzerScratch.size();
    for (int start = 0; start < numSamples; start += capacity)
    {
        const auto n = juce::jmin (capacity, numSamples - start);
        for (int i = 0; i < n; ++i)
            analyzerScratch[(size_t) i] = 0.5f * (left[start + i] + right[start + i]);
        analyzerRing.write (analyzerScratch.data(), n);
    }
}

juce::AudioProcessorEditor* AmpSimProcessor::createEditor()
{
    return new AmpSimEditor (*this);
}

void AmpSimProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty (midiMapKey, juce::JSON::toString (midiMap.toVar(), true), nullptr);
    state.setProperty (scenesKey, juce::JSON::toString (scenes.toVar(), true), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AmpSimProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    parameters.replaceState (juce::ValueTree::fromXml (*xml));
    auto& state = parameters.state;

    // The tuner never comes back engaged: a session saved while tuning would otherwise open muted.
    if (auto* tunerSwitch = parameters.getParameter ("tuner_on"))
        tunerSwitch->setValueNotifyingHost (0.0f);

    // A restored session brings its own cab: following starts from the slot it was saved on.
    markCabFollowed();

    // MIDI mappings (states saved before they existed have none).
    midiMap = MidiMap::fromVar (juce::JSON::parse (state.getProperty (midiMapKey).toString()), parameters);
    scenes = Scenes::fromVar (juce::JSON::parse (state.getProperty (scenesKey).toString()), parameters);

    // The captures below load with the restored calibration, so it's already applied.
    appliedCalibration = pendingCalibration = currentCalibration();

    // Effect order: saved by block name (older states have none, so the default order applies).
    for (auto section : { ampsim::Chain::Section::pre, ampsim::Chain::Section::post })
        setSectionOrder (section, juce::StringArray::fromTokens (state.getProperty (orderKey (section)).toString(), ",", ""));
    setBloomOrder (juce::StringArray::fromTokens (state.getProperty (bloomOrderKey).toString(), ",", ""));

    // Milestone 1 had one amp slot ("modelPath") and one cab IR ("irPath"): they become slot 1 and
    // close mic 1.
    if (state.hasProperty (legacyModelPathKey) && ! state.hasProperty (modelPathKey (0)))
        state.setProperty (modelPathKey (0), state.getProperty (legacyModelPathKey), nullptr);
    if (state.hasProperty (legacyIRPathKey) && ! state.hasProperty (cabPathKey (0)))
        state.setProperty (cabPathKey (0), state.getProperty (legacyIRPathKey), nullptr);
    state.removeProperty (legacyModelPathKey, nullptr);
    state.removeProperty (legacyIRPathKey, nullptr);

    // Reload the captures and IRs this state was saved with.
    for (int s = 0; s < numAmpSlots; ++s)
    {
        if (! state.hasProperty (modelPathKey (s)))
            continue; // no entry: fillFreshSlots() below

        const auto path = state.getProperty (modelPathKey (s)).toString();
        const auto moved = juce::File::isAbsolutePath (path) ? presets::bundledElsewhere (juce::File (path)) : juce::File();

        if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
            loadModel (s, juce::File (path));
        else if (moved.existsAsFile())
            loadModel (s, moved); // a built-in capture saved by a copy of the app installed somewhere else
        else if (juce::File::isAbsolutePath (path))
            unloadModel (s, "Saved model is missing: " + path, true); // the saved path stays in the state
        else if (requestedModel[(size_t) s].file != juce::File())
            unloadModel (s, "Empty", false); // cleared on purpose: stop the built-in a fresh start began loading
    }

    // Slots this state has no entry for (it was saved before the built-in captures existed) start on
    // their built-ins, as on a fresh start. An empty entry is a slot cleared on purpose, and stays empty.
    fillFreshSlots();

    // The top bar remembers the last factory preset by name: a factory preset that was renamed (no band
    // names, 2026-10-03) is shown, and stepped from, under its new name.
    if (state.getProperty (presetSourceKey).toString() == "factory")
        state.setProperty ("presetName", presets::currentFactoryPresetName (state.getProperty ("presetName").toString()), nullptr);

    for (int m = 0; m < numCabMics; ++m)
    {
        const auto path = state.getProperty (cabPathKey (m)).toString();

        if (! juce::File::isAbsolutePath (path))
            continue;

        if (juce::File (path).existsAsFile() || juce::File (path).isDirectory())
            loadCabIR (m, juce::File (path));
        else if (const auto moved = presets::bundledElsewhere (juce::File (path)); moved.exists())
            loadCabIR (m, moved); // a built-in IR saved by a copy of the app installed somewhere else
        else
            setCabStatus (m, "Saved IR is missing: " + path, true);
    }
}

void AmpSimProcessor::loadModel (int slot, const juce::File& file)
{
    slot = juce::jlimit (0, numAmpSlots - 1, slot);
    parameters.state.setProperty (modelPathKey (slot), file.getFullPathName(), nullptr);
    setModelStatus (slot, "Loading " + file.getFileName() + "...", false);

    const auto calibration = currentCalibration();
    requestedModel[(size_t) slot] = { file, calibration };

    ++loadsInFlight;
    loader.addJob ([this, slot, file, calibration]
    {
        const auto result = chain.amp.slot (slot).model.loadModel (file, true, calibration);
        setModelStatus (slot, result.message, ! result.ok);
        --loadsInFlight;
    });
}

void AmpSimProcessor::loadCabIR (int mic, const juce::File& file)
{
    mic = juce::jlimit (0, numCabMics - 1, mic);
    parameters.state.setProperty (cabPathKey (mic), file.getFullPathName(), nullptr);
    setCabStatus (mic, "Loading " + file.getFileName() + "...", false);

    auto channel = ampsim::CabIR::Channel::left;
    if (mic != roomMic)
    {
        const auto useRight = micParameters[(size_t) mic].channel->load() >= 0.5f;
        loadedChannel[(size_t) mic] = useRight ? 1 : 0;
        channel = useRight ? ampsim::CabIR::Channel::right : ampsim::CabIR::Channel::left;

        // Whatever loads next replaces the pack, so stop morphing against the old one now.
        packActive[(size_t) mic] = false;
    }

    if (file.isDirectory())
    {
        if (mic == roomMic)
        {
            setCabStatus (mic, "Cab packs are for the close mics; the room mic takes one IR file", true);
            return;
        }

        const juce::Point<float> position { micParameters[(size_t) mic].positionX->load(), micParameters[(size_t) mic].positionY->load() };
        morphedPosition[(size_t) mic] = position;

        ++loadsInFlight;
        loader.addJob ([this, mic, file, position]
        {
            const auto result = chain.cab.loadCloseMicPack (mic, file, position.x, position.y);
            setCabStatus (mic, result.message, ! result.ok);
            packActive[(size_t) mic] = chain.cab.hasPack (mic); // a failed load keeps the previous pack
            --loadsInFlight;
        });
        return;
    }

    ++loadsInFlight;
    loader.addJob ([this, mic, file, channel]
    {
        const auto result = mic == roomMic ? chain.cab.loadRoom (file) : chain.cab.loadCloseMic (mic, file, channel);
        setCabStatus (mic, result.message, ! result.ok);
        if (mic != roomMic)
            packActive[(size_t) mic] = chain.cab.hasPack (mic); // a file replaces the pack; a failed load keeps it
        --loadsInFlight;
    });
}

void AmpSimProcessor::setCabFollow (bool shouldFollow)
{
    parameters.state.setProperty (cabFollowKey, shouldFollow, nullptr);
    markCabFollowed();
    if (shouldFollow)
        applyCabAssignment (lastFollowedSlot);
}

juce::File AmpSimProcessor::getCabAssignment (int slot) const
{
    const auto path = parameters.state.getProperty (cabAssignKey (juce::jlimit (0, numAmpSlots - 1, slot))).toString();
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

void AmpSimProcessor::setCabAssignment (int slot, const juce::File& fileOrPack)
{
    const auto key = cabAssignKey (juce::jlimit (0, numAmpSlots - 1, slot));
    if (fileOrPack == juce::File())
        parameters.state.removeProperty (key, nullptr);
    else
        parameters.state.setProperty (key, fileOrPack.getFullPathName(), nullptr);
}

void AmpSimProcessor::pickCab (const juce::File& fileOrPack)
{
    loadCabIR (0, fileOrPack);
    setCabAssignment (juce::roundToInt (ampSlot->load()), fileOrPack);
}

void AmpSimProcessor::applyCabAssignment (int slot)
{
    // Only a slot with a cab assigned changes anything, and only if close mic 1 holds something else.
    const auto assigned = getCabAssignment (slot);
    if (assigned == juce::File() || ! assigned.exists())
        return;
    if (parameters.state.getProperty (cabPathKey (0)).toString() != assigned.getFullPathName())
        loadCabIR (0, assigned);
}

ampsim::NamAmp::Calibration AmpSimProcessor::currentCalibration() const
{
    return { calibrateInput->load() >= 0.5f, (double) interfaceInputDbu->load() };
}

presets::ApplyResult AmpSimProcessor::loadPreset (const juce::var& preset)
{
    auto check = presets::validate (preset);
    if (! check.ok)
        return check;

    pendingPreset = preset;
    presetWarnings.clear();
    presetStage = PresetStage::fadingOut;
    presetMute = true;
    timerCallback(); // applies straight away if no audio is running to fade
    return check;
}

void AmpSimProcessor::clearModel (int slot)
{
    slot = juce::jlimit (0, numAmpSlots - 1, slot);
    parameters.state.setProperty (modelPathKey (slot), juce::String(), nullptr); // empty, not absent: cleared on purpose
    unloadModel (slot, "Empty", false);
}

void AmpSimProcessor::unloadModel (int slot, const juce::String& statusAfter, bool isError)
{
    requestedModel[(size_t) slot] = {};
    ++loadsInFlight;
    loader.addJob ([this, slot, statusAfter, isError]
    {
        chain.amp.slot (slot).model.clearModel();
        setModelStatus (slot, statusAfter, isError);
        --loadsInFlight;
    });
}

void AmpSimProcessor::useBuiltInCapture (int slot)
{
    slot = juce::jlimit (0, numAmpSlots - 1, slot);
    if (const auto file = presets::builtInCapture (slot); file.existsAsFile())
        loadModel (slot, file);
    else
        setModelStatus (slot, "The built-in capture is missing: " + file.getFullPathName(), true);
}

void AmpSimProcessor::fillFreshSlots()
{
    if (! builtInCapturesForFreshSlots)
        return;

    for (int s = 0; s < numAmpSlots; ++s)
    {
        if (parameters.state.hasProperty (modelPathKey (s)))
            continue; // a capture of its own, or cleared on purpose (an empty path)

        // A copy of the app without its content folder (a bare build) just starts empty, without a warning.
        const auto file = presets::builtInCapture (s);
        if (! file.existsAsFile())
            continue;

        const auto& requested = requestedModel[(size_t) s];
        const auto calibration = currentCalibration();
        if (requested.file == file && requested.calibration.enabled == calibration.enabled
            && std::abs (requested.calibration.interfaceInputDbu - calibration.interfaceInputDbu) < 1.0e-4)
            parameters.state.setProperty (modelPathKey (s), file.getFullPathName(), nullptr); // already on its way
        else
            loadModel (s, file);
    }
}

void AmpSimProcessor::clearCabIR (int mic)
{
    mic = juce::jlimit (0, numCabMics - 1, mic);
    parameters.state.removeProperty (cabPathKey (mic), nullptr);
    if (mic != roomMic)
        packActive[(size_t) mic] = false;
    ++loadsInFlight;
    loader.addJob ([this, mic]
    {
        if (mic == roomMic)
            chain.cab.clearRoom();
        else
            chain.cab.clearCloseMic (mic);
        setCabStatus (mic, "No IR", false);
        --loadsInFlight;
    });
}

AmpSimProcessor::Status AmpSimProcessor::getStatus() const
{
    const std::scoped_lock lock (statusMutex);
    auto copy = status;

    const auto alignment = chain.cab.getAlignment();
    if (! alignment.valid)
        copy.alignment = "Alignment needs IRs in both close mics";
    else if (alignment.delayMic1 == 0 && alignment.delayMic2 == 0 && ! alignment.invertMic2)
        copy.alignment = "Close mics already line up (match " + juce::String (alignment.correlation, 2) + ")";
    else
        copy.alignment = "Close mics aligned: mic " + juce::String (alignment.delayMic1 > 0 ? 1 : 2) + " delayed "
                         + juce::String (juce::jmax (alignment.delayMic1, alignment.delayMic2)) + " samples"
                         + (alignment.invertMic2 ? ", mic 2 inverted" : "") + " (match " + juce::String (alignment.correlation, 2) + ")";

    if (! sampleRateOk.load())
        copy.warning = "Output muted: the audio device runs at " + juce::String (juce::roundToInt (deviceSampleRate.load()))
                       + " Hz and NAM models need 48000 Hz. Open Options and set the sample rate to 48000.";

    return copy;
}

void AmpSimProcessor::setModelStatus (int slot, const juce::String& text, bool isError)
{
    const std::scoped_lock lock (statusMutex);
    status.model[(size_t) slot] = text;
    status.modelError[(size_t) slot] = isError;
}

void AmpSimProcessor::setCabStatus (int mic, const juce::String& text, bool isError)
{
    const std::scoped_lock lock (statusMutex);
    status.cab[(size_t) mic] = text;
    status.cabError[(size_t) mic] = isError;
}

void AmpSimProcessor::timerCallback()
{
    for (int s = 0; s < numAmpSlots; ++s)
        chain.amp.slot (s).model.collectGarbage();

    chain.cab.collectGarbage();

    // Footswitches and pedals mapped to parameters (and MIDI learn).
    // The scene CC picks a scene by its value (0 is scene 1); it takes precedence over any mapping.
    const auto sceneController = juce::roundToInt (sceneCc->load());
    ccFifo.drain ([this, sceneController] (int cc, int value)
    {
        if (cc == sceneController)
        {
            if (value < Scenes::count)
                scenes.recall (value, parameters);
        }
        else
        {
            midiMap.handle (cc, value, parameters);
        }
    });

    // A footswitch toggled the reverb's freeze: write it into the switch (the audio thread already has).
    if (auto freeze = freezeRequest.load(); freeze >= 0)
    {
        if (auto* param = parameters.getParameter ("reverb_freeze"))
            param->setValueNotifyingHost ((float) freeze);
        freezeRequest.compare_exchange_strong (freeze, -1); // a newer press keeps its request for the next tick
    }

    // Gate Learn finished: write each measured threshold into its knob. (The count goes up after the
    // result is stored, so reading it first means the threshold read after it is the new one.)
    const auto writeLearned = [this] (const ampsim::Gate& gate, int& seen, const char* id)
    {
        if (const auto count = gate.getLearnCount(); count != seen)
        {
            seen = count;
            if (auto* param = parameters.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (gate.getLearnedThresholdDb()));
        }
    };
    writeLearned (chain.gateA, gateALearnSeen, "gate_a_threshold");
    writeLearned (chain.gateB.gate, gateBLearnSeen, "gate_b_threshold");

    // Bloom's through-zero flanger is the one latency the chain ever has (5 ms, while it's on): tell the host.
    if (const auto latency = chain.latencySamples(); latency != getLatencySamples())
        setLatencySamples (latency);

    // A tapped tempo: write it into the tempo knob (the audio thread already uses it).
    if (tapPending.load())
    {
        if (auto* param = parameters.getParameter ("tempo_bpm"))
            param->setValueNotifyingHost (param->convertTo0to1 ((float) tappedBpm.load()));
        tapPending = false;
    }

    // A footswitch changed the slot on the audio thread; make the parameter (and the GUI) agree.
    if (const auto slot = midiSlotRequest.exchange (-1); slot >= 0)
        if (auto* param = parameters.getParameter (slotParamId))
            param->setValueNotifyingHost (param->convertTo0to1 ((float) slot));

    // Follow amp choice: a new slot brings its own cab into close mic 1.
    if (const auto slot = juce::roundToInt (ampSlot->load()); slot != lastFollowedSlot)
    {
        lastFollowedSlot = slot;
        if (isCabFollowing())
            applyCabAssignment (slot);
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();

    // A preset change, stage by stage: once the fade has reached silence (or no audio is running),
    // apply it; once its loads are done and the new sound has settled (JUCE's 50 ms engine swaps, the
    // cab's 60 ms fade-ins, 25 ms knob ramps), fade back in.
    const bool audioRunning = juce::Time::getMillisecondCounter() - lastProcessMs.load() < 200;
    if (presetStage == PresetStage::fadingOut && (presetSilent.load() || ! audioRunning))
    {
        presetWarnings = presets::apply (*this, pendingPreset).warnings;
        pendingPreset = juce::var();
        presetStage = PresetStage::loading;
        presetStageMs = now;
    }
    else if (presetStage == PresetStage::loading
             && ((! isLoading() && now - presetStageMs >= presetSettleMs) || now - presetStageMs >= presetTimeoutMs))
    {
        presetMute = false;
        presetStage = PresetStage::idle;
    }

    // The input calibration changed: once it has settled, reload every capture with it (a reload
    // crossfades like any capture change).
    const auto same = [] (const ampsim::NamAmp::Calibration& a, const ampsim::NamAmp::Calibration& b)
    { return a.enabled == b.enabled && std::abs (a.interfaceInputDbu - b.interfaceInputDbu) < 1.0e-4; };

    if (const auto calibration = currentCalibration(); ! same (calibration, pendingCalibration))
    {
        pendingCalibration = calibration;
        pendingSinceMs = now;
    }
    else if (! same (pendingCalibration, appliedCalibration) && now - pendingSinceMs >= calibrationSettleMs)
    {
        appliedCalibration = pendingCalibration;
        ++calibrationReloads;

        for (int s = 0; s < numAmpSlots; ++s)
            if (const auto path = parameters.state.getProperty (modelPathKey (s)).toString();
                juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
                loadModel (s, juce::File (path));
    }

    for (int m = 0; m < ampsim::Cab::numCloseMics; ++m)
    {
        // A close mic's left/right choice changed: read its file again with the other channel.
        const auto wanted = micParameters[(size_t) m].channel->load() >= 0.5f ? 1 : 0;
        const auto path = parameters.state.getProperty (cabPathKey (m)).toString();

        if (wanted != loadedChannel[(size_t) m] && juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
            loadCabIR (m, juce::File (path));

        // A moving mic: once its position has changed, the last morph has finished, and 40 ms have
        // passed since it started, morph for the newest position. Positions in between are skipped,
        // never queued, so the mic always heads for where it is now.
        const juce::Point<float> position { micParameters[(size_t) m].positionX->load(), micParameters[(size_t) m].positionY->load() };

        if (packActive[(size_t) m] && ! morphInFlight[(size_t) m] && position != morphedPosition[(size_t) m]
            && now - lastMorphStartMs[(size_t) m] >= morphIntervalMs)
        {
            morphedPosition[(size_t) m] = position;
            lastMorphStartMs[(size_t) m] = now;
            morphInFlight[(size_t) m] = true;
            ++morphCount;
            ++loadsInFlight;

            loader.addJob ([this, m, position]
            {
                // A file loaded since this was queued has replaced the pack: nothing to move. (Only
                // loader jobs change packs, and there's one loader thread, so this can't go stale.)
                if (chain.cab.hasPack (m))
                {
                    const auto result = chain.cab.moveCloseMic (m, position.x, position.y);
                    setCabStatus (m, result.message, ! result.ok);
                }
                morphInFlight[(size_t) m] = false;
                --loadsInFlight;
            });
        }
    }
}

// JUCE's plugin wrappers (here, the standalone app) call this to create the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AmpSimProcessor();
}

#if defined(JucePlugin_Build_Standalone) && JucePlugin_Build_Standalone
namespace
{
// The app was called "Amp Sim" until 2026-10-03; copy its data folder and settings file to BellyDSP's
// names once (src/platform/DataMigration.h). This has to happen before JUCE's standalone wrapper opens
// its settings file, which it does in the application object's constructor and createPluginHolder(),
// before createPluginFilter() is ever called, so it runs as a static initializer: on the main thread
// (the one that becomes the message thread), before main(), long before any audio thread exists. Only in
// the standalone app; the tests run migrate() on temporary folders instead.
[[maybe_unused]] const bool legacyDataMigrated = []
{
    platform::migration::runAtStartup();
    return true;
}();
} // namespace
#endif

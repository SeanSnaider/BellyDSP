#include "PluginProcessor.h"
#include "PluginEditor.h"

AmpSimProcessor::AmpSimProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "AmpSim", createParameterLayout())
{
    // Give every parameter its default through the path a preset or a host uses (normalized value to
    // plain value, snapped to its step). Constructed defaults on skewed ranges are off in their last
    // bits (an 8 kHz default reads 7999.9995 Hz), so without this a fresh processor and the same sound
    // loaded from a preset would differ by about 1e-5.
    for (auto* parameter : getParameters())
        parameter->setValueNotifyingHost (parameter->getDefaultValue());

    inputGainDb = raw ("input_gain");
    outputGainDb = raw ("output_gain");
    cabBypass = raw ("cab_bypass");
    ampSlot = raw (slotParamId);

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
    preCompParams.bind (parameters, "comp_pre");
    postCompParams.bind (parameters, "comp_post");
    preEqParams.bind (parameters, "eq_pre");
    postEqParams.bind (parameters, "eq_post");
    delayParams.bind (parameters);
    tempoBpm = raw ("tempo_bpm");
    tapCc = raw ("midi_tap_cc");
    calibrateInput = raw ("input_calibrate");
    interfaceInputDbu = raw ("input_level_dbu");
    appliedCalibration = pendingCalibration = currentCalibration();
    lowCutOn = raw ("cab_lowcut_on");
    lowCutFreq = raw ("cab_lowcut_freq");
    lowCutSlope = raw ("cab_lowcut_slope");
    highCutOn = raw ("cab_highcut_on");
    highCutFreq = raw ("cab_highcut_freq");
    highCutSlope = raw ("cab_highcut_slope");

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
    layout.add (std::make_unique<Choice> (juce::ParameterID { slotParamId, 1 }, "Amp Slot", juce::StringArray { "Amp 1", "Amp 2", "Amp 3" }, 0));

    // Input calibration (global settings, not part of presets): the analog level that reaches 0 dBFS on
    // this interface. +12 dBu is the Scarlett Solo 4th Gen's instrument input at minimum gain.
    layout.add (std::make_unique<Bool> (juce::ParameterID { "input_calibrate", 1 }, "Calibrate Input to Captures", true));
    layout.add (std::make_unique<Float> (juce::ParameterID { "input_level_dbu", 1 }, "Interface Input Level",
                                         juce::NormalisableRange<float> (-10.0f, 30.0f, 0.1f), 12.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("dBu")));

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
    params::CompressorParameters::addTo (layout, "comp_pre", "Pre Comp", false);
    params::EqualizerParameters::addTo (layout, "eq_pre", "Pre EQ", true);
    params::EqualizerParameters::addTo (layout, "eq_post", "Post EQ", true);
    params::CompressorParameters::addTo (layout, "comp_post", "Post Comp", false);
    params::DelayParameters::addTo (layout);

    // Tempo (saved with presets) and the footswitch CC that taps it.
    layout.add (std::make_unique<Float> (juce::ParameterID { "tempo_bpm", 1 }, "Tempo", juce::NormalisableRange<float> (30.0f, 300.0f, 0.1f), 120.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("BPM")));
    layout.add (std::make_unique<Int> (juce::ParameterID { "midi_tap_cc", 1 }, "Tap Tempo CC", 0, 127, 80));
    return layout;
}

void AmpSimProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    deviceSampleRate = sampleRate;
    sampleRateOk = std::abs (sampleRate - ampsim::NamAmp::requiredSampleRate) < 0.5;
    preparedBlockSize = juce::jmax (1, samplesPerBlock);

    // Start on the saved slot and cab settings without ramps: prepare() snaps fades to the targets.
    lastSlotParameter = juce::roundToInt (ampSlot->load());
    chain.amp.selectSlot (lastSlotParameter);
    applyCabParameters();
    applyEffectParameters();

    chain.prepare (sampleRate, preparedBlockSize);
    setLatencySamples (chain.latencySamples());

    presetGain.reset (sampleRate, presetFadeSeconds);
    presetGain.setCurrentAndTargetValue (presetMute.load() ? 0.0f : 1.0f);
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
    chain.setBypassed (Slot::preCompressor, ! preCompParams.isOn());
    chain.preCompressor.setSettings (preCompParams.read());
    chain.setBypassed (Slot::preEq, ! preEqParams.isOn());
    chain.preEq.setSettings (preEqParams.read());
    chain.setBypassed (Slot::postEq, ! postEqParams.isOn());
    chain.postEq.setSettings (postEqParams.read());
    chain.setBypassed (Slot::postCompressor, ! postCompParams.isOn());
    chain.postCompressor.setSettings (postCompParams.read());
    chain.setBypassed (Slot::delay, ! delayParams.isOn()); // the delay takes this as spillover
    chain.delay.setSettings (delayParams.read (getTempo()));
}

void AmpSimProcessor::registerTap (double timeSeconds)
{
    if (tap.tap (timeSeconds))
    {
        tappedBpm = juce::jlimit (30.0, 300.0, tap.getBpm());
        tapPending = true;
    }
}

juce::String AmpSimProcessor::blockName (ampsim::Chain::Slot slot)
{
    using Slot = ampsim::Chain::Slot;
    if (slot == Slot::preCompressor || slot == Slot::postCompressor)
        return "comp";
    if (slot == Slot::preEq || slot == Slot::postEq)
        return "eq";
    if (slot == Slot::delay)
        return "delay";
    return {}; // fixed blocks have no section name
}

void AmpSimProcessor::setSectionOrder (ampsim::Chain::Section section, const juce::StringArray& names)
{
    // Names to slots, skipping unknown and repeated names, then any block not named, in default order.
    const auto defaults = ampsim::Chain::defaultOrder (section);
    std::vector<ampsim::Chain::Slot> order;
    for (const auto& name : names)
        for (auto slot : defaults)
            if (blockName (slot) == name.trim() && std::find (order.begin(), order.end(), slot) == order.end())
                order.push_back (slot);
    for (auto slot : defaults)
        if (std::find (order.begin(), order.end(), slot) == order.end())
            order.push_back (slot);

    chain.requestOrder (section, order);
    parameters.state.setProperty (orderKey (section), getSectionOrder (section).joinIntoString (","), nullptr);
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
    // Denormals (tiny floats near zero) are very slow on some CPUs, and a decaying tail produces
    // them. This flushes them to zero for the duration of the callback (BUILD_PLAN "Denormals").
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples = buffer.getNumSamples();

    // Output channels with no matching input start out holding garbage.
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (! sampleRateOk.load (std::memory_order_relaxed) || preparedBlockSize == 0
        || buffer.getNumChannels() < 2)
    {
        buffer.clear(); // the editor shows why
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
    chain.setBypassed (ampsim::Chain::Slot::cab, cabBypass->load (std::memory_order_relaxed) >= 0.5f);

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

    // Hosts may occasionally send more samples than promised, so feed the chain in pieces that fit.
    auto io = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, 2);
    const auto maxChunk = (size_t) preparedBlockSize;

    for (size_t start = 0; start < (size_t) numSamples; start += maxChunk)
        chain.process (io.getSubBlock (start, juce::jmin (maxChunk, (size_t) numSamples - start)));

    samplesProcessed += numSamples;

    // Preset changes: fade the whole output out, and back in once the new sound is ready.
    presetGain.setTargetValue (presetMute.load (std::memory_order_relaxed) ? 0.0f : 1.0f);
    if (presetGain.isSmoothing() || presetGain.getCurrentValue() < 1.0f)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);
        for (int n = 0; n < numSamples; ++n)
        {
            const auto g = presetGain.getNextValue();
            left[n] *= g;
            right[n] *= g;
        }
    }
    presetSilent.store (presetMute.load (std::memory_order_relaxed) && ! presetGain.isSmoothing() && presetGain.getCurrentValue() <= 0.0f);
    lastProcessMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* AmpSimProcessor::createEditor()
{
    return new AmpSimEditor (*this);
}

void AmpSimProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AmpSimProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    parameters.replaceState (juce::ValueTree::fromXml (*xml));
    auto& state = parameters.state;

    // The captures below load with the restored calibration, so it's already applied.
    appliedCalibration = pendingCalibration = currentCalibration();

    // Effect order: saved by block name (older states have none, so the default order applies).
    for (auto section : { ampsim::Chain::Section::pre, ampsim::Chain::Section::post })
        setSectionOrder (section, juce::StringArray::fromTokens (state.getProperty (orderKey (section)).toString(), ",", ""));

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
        const auto path = state.getProperty (modelPathKey (s)).toString();

        if (! juce::File::isAbsolutePath (path))
            continue;

        if (juce::File (path).existsAsFile())
            loadModel (s, juce::File (path));
        else
            setModelStatus (s, "Saved model is missing: " + path, true);
    }

    for (int m = 0; m < numCabMics; ++m)
    {
        const auto path = state.getProperty (cabPathKey (m)).toString();

        if (! juce::File::isAbsolutePath (path))
            continue;

        if (juce::File (path).existsAsFile() || juce::File (path).isDirectory())
            loadCabIR (m, juce::File (path));
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
    parameters.state.removeProperty (modelPathKey (slot), nullptr);
    ++loadsInFlight;
    loader.addJob ([this, slot]
    {
        chain.amp.slot (slot).model.clearModel();
        setModelStatus (slot, "Empty", false);
        --loadsInFlight;
    });
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

#include "PluginProcessor.h"
#include "PluginEditor.h"

AmpSimProcessor::AmpSimProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "AmpSim", createParameterLayout())
{
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

    chain.prepare (sampleRate, preparedBlockSize);
    setLatencySamples (chain.latencySamples());
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
    for (const auto metadata : midi)
    {
        const auto* data = metadata.data;

        if (metadata.numBytes >= 2 && (data[0] & 0xf0) == 0xc0 && data[1] < numAmpSlots)
        {
            chain.amp.selectSlot (data[1]);
            midiSlotRequest.store (data[1]);
        }
    }
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

        for (int b = 0; b < ampsim::AmpTone::numBands; ++b)
            slot.tone.setGainDb ((ampsim::AmpTone::Band) b, p.tone[(size_t) b]->load (std::memory_order_relaxed));
    }

    applyCabParameters();

    // Hosts may occasionally send more samples than promised, so feed the chain in pieces that fit.
    auto io = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, 2);
    const auto maxChunk = (size_t) preparedBlockSize;

    for (size_t start = 0; start < (size_t) numSamples; start += maxChunk)
        chain.process (io.getSubBlock (start, juce::jmin (maxChunk, (size_t) numSamples - start)));
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

    // A footswitch changed the slot on the audio thread; make the parameter (and the GUI) agree.
    if (const auto slot = midiSlotRequest.exchange (-1); slot >= 0)
        if (auto* param = parameters.getParameter (slotParamId))
            param->setValueNotifyingHost (param->convertTo0to1 ((float) slot));

    const auto now = juce::Time::getMillisecondCounterHiRes();

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

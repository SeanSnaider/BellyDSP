#include "PluginProcessor.h"
#include "PluginEditor.h"

AmpSimProcessor::AmpSimProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "AmpSim", createParameterLayout())
{
    inputGainDb = parameters.getRawParameterValue ("input_gain");
    outputGainDb = parameters.getRawParameterValue ("output_gain");
    cabBypass = parameters.getRawParameterValue ("cab_bypass");
    ampSlot = parameters.getRawParameterValue (slotParamId);

    for (int s = 0; s < numAmpSlots; ++s)
    {
        auto& p = slotParameters[(size_t) s];
        p.inputTrim = parameters.getRawParameterValue (ampParamId (s, "input_trim"));
        p.outputTrim = parameters.getRawParameterValue (ampParamId (s, "output_trim"));

        for (int b = 0; b < ampsim::AmpTone::numBands; ++b)
            p.tone[(size_t) b] = parameters.getRawParameterValue (ampParamId (s, juce::String (ampsim::AmpTone::bands[(size_t) b].name).toLowerCase()));
    }

    // Frees models and IRs the audio thread has handed back, and syncs footswitch slot changes.
    startTimerHz (20);
}

AmpSimProcessor::~AmpSimProcessor()
{
    stopTimer();
    loader.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout AmpSimProcessor::createParameterLayout()
{
    // Parameter IDs are permanent once presets exist. Never rename one; add a new ID instead.
    const auto levelRange = juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f);
    const auto toneRange = juce::NormalisableRange<float> (-ampsim::AmpTone::rangeDb, ampsim::AmpTone::rangeDb, 0.1f);
    const auto dB = juce::AudioParameterFloatAttributes().withLabel ("dB");

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "input_gain", 1 }, "Input Gain", levelRange, 0.0f, dB));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output_gain", 1 }, "Output Level", levelRange, 0.0f, dB));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "cab_bypass", 1 }, "Cab Bypass", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { slotParamId, 1 }, "Amp Slot",
                                                              juce::StringArray { "Amp 1", "Amp 2", "Amp 3" }, 0));

    for (int s = 0; s < numAmpSlots; ++s)
    {
        const auto prefix = "Amp " + juce::String (s + 1) + " ";
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ampParamId (s, "input_trim"), 1 },
                                                                 prefix + "Input Trim", levelRange, 0.0f, dB));
        layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ampParamId (s, "output_trim"), 1 },
                                                                 prefix + "Output Trim", levelRange, 0.0f, dB));

        for (const auto& band : ampsim::AmpTone::bands)
            layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { ampParamId (s, juce::String (band.name).toLowerCase()), 1 },
                                                                     prefix + band.name, toneRange, 0.0f, dB));
    }

    return layout;
}

void AmpSimProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    deviceSampleRate = sampleRate;
    sampleRateOk = std::abs (sampleRate - ampsim::NamAmp::requiredSampleRate) < 0.5;
    preparedBlockSize = juce::jmax (1, samplesPerBlock);

    // Start on the saved slot without a crossfade: prepare() snaps the fade to the selection.
    lastSlotParameter = juce::roundToInt (ampSlot->load());
    chain.amp.selectSlot (lastSlotParameter);

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

    // Milestone 1 had one slot, saved as "modelPath". It becomes slot 1.
    if (state.hasProperty (legacyModelPathKey) && ! state.hasProperty (modelPathKey (0)))
        state.setProperty (modelPathKey (0), state.getProperty (legacyModelPathKey), nullptr);
    state.removeProperty (legacyModelPathKey, nullptr);

    // Reload the captures and IR this state was saved with.
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

    const auto irPath = state.getProperty (irPathKey).toString();

    if (juce::File::isAbsolutePath (irPath))
    {
        if (juce::File (irPath).existsAsFile())
            loadImpulseResponse (juce::File (irPath));
        else
            setCabStatus ("Saved IR is missing: " + irPath, true);
    }
}

void AmpSimProcessor::loadModel (int slot, const juce::File& file)
{
    slot = juce::jlimit (0, numAmpSlots - 1, slot);
    parameters.state.setProperty (modelPathKey (slot), file.getFullPathName(), nullptr);
    setModelStatus (slot, "Loading " + file.getFileName() + "...", false);

    ++loadsInFlight;
    loader.addJob ([this, slot, file]
    {
        const auto result = chain.amp.slot (slot).model.loadModel (file);
        setModelStatus (slot, result.message, ! result.ok);
        --loadsInFlight;
    });
}

void AmpSimProcessor::loadImpulseResponse (const juce::File& file)
{
    parameters.state.setProperty (irPathKey, file.getFullPathName(), nullptr);
    setCabStatus ("Loading " + file.getFileName() + "...", false);

    ++loadsInFlight;
    loader.addJob ([this, file]
    {
        const auto result = chain.cab.loadFile (file);
        setCabStatus (result.message, ! result.ok);
        --loadsInFlight;
    });
}

AmpSimProcessor::Status AmpSimProcessor::getStatus() const
{
    const std::scoped_lock lock (statusMutex);
    auto copy = status;

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

void AmpSimProcessor::setCabStatus (const juce::String& text, bool isError)
{
    const std::scoped_lock lock (statusMutex);
    status.cab = text;
    status.cabError = isError;
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
}

// JUCE's plugin wrappers (here, the standalone app) call this to create the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AmpSimProcessor();
}

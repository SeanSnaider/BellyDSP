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

    // Frees models and IRs the audio thread has handed back.
    startTimerHz (10);
}

AmpSimProcessor::~AmpSimProcessor()
{
    stopTimer();
    loader.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout AmpSimProcessor::createParameterLayout()
{
    // Parameter IDs are permanent once presets exist. Never rename one; add a new ID instead.
    const auto gainRange = juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f);
    const auto dB = juce::AudioParameterFloatAttributes().withLabel ("dB");

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "input_gain", 1 },
                                                             "Input Gain", gainRange, 0.0f, dB));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output_gain", 1 },
                                                             "Output Level", gainRange, 0.0f, dB));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "cab_bypass", 1 },
                                                            "Cab Bypass", false));
    return layout;
}

void AmpSimProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    deviceSampleRate = sampleRate;
    sampleRateOk = std::abs (sampleRate - ampsim::NamAmp::requiredSampleRate) < 0.5;
    preparedBlockSize = juce::jmax (1, samplesPerBlock);

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

void AmpSimProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
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

    chain.inputGain.setGainDecibels (inputGainDb->load (std::memory_order_relaxed));
    chain.outputGain.setGainDecibels (outputGainDb->load (std::memory_order_relaxed));
    chain.setBypassed (ampsim::Chain::Slot::cab, cabBypass->load (std::memory_order_relaxed) >= 0.5f);

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

    // Reload the capture and IR this state was saved with.
    const auto modelPath = parameters.state.getProperty (modelPathKey).toString();
    const auto irPath = parameters.state.getProperty (irPathKey).toString();

    if (juce::File::isAbsolutePath (modelPath))
    {
        if (juce::File (modelPath).existsAsFile())
            loadModel (juce::File (modelPath));
        else
            setModelStatus ("Saved model is missing: " + modelPath, true);
    }

    if (juce::File::isAbsolutePath (irPath))
    {
        if (juce::File (irPath).existsAsFile())
            loadImpulseResponse (juce::File (irPath));
        else
            setCabStatus ("Saved IR is missing: " + irPath, true);
    }
}

void AmpSimProcessor::loadModel (const juce::File& file)
{
    parameters.state.setProperty (modelPathKey, file.getFullPathName(), nullptr);
    setModelStatus ("Loading " + file.getFileName() + "...", false);

    ++loadsInFlight;
    loader.addJob ([this, file]
    {
        const auto result = chain.amp.loadModel (file);
        setModelStatus (result.message, ! result.ok);
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

void AmpSimProcessor::setModelStatus (const juce::String& text, bool isError)
{
    const std::scoped_lock lock (statusMutex);
    status.model = text;
    status.modelError = isError;
}

void AmpSimProcessor::setCabStatus (const juce::String& text, bool isError)
{
    const std::scoped_lock lock (statusMutex);
    status.cab = text;
    status.cabError = isError;
}

void AmpSimProcessor::timerCallback()
{
    chain.amp.collectGarbage();
    chain.cab.collectGarbage();
}

// JUCE's plugin wrappers (here, the standalone app) call this to create the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AmpSimProcessor();
}

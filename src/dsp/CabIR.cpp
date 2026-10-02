#include "CabIR.h"
#include "Loudness.h"
#include "ReferenceSignals.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace ampsim
{

CabIR::CabIR() : CabIR (Options {}) {}

CabIR::CabIR (Options opts)
    : options (opts),
      convolution (opts.nonUniformHeadSize > 0
                       ? std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { opts.nonUniformHeadSize })
                       : std::make_unique<juce::dsp::Convolution>())
{
}

CabIR::LoadResult CabIR::loadFile (const juce::File& file, Channel channel)
{
    if (! file.existsAsFile())
        return { false, file.getFullPathName() + " doesn't exist" };

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return { false, "Couldn't read " + file.getFileName() + " as audio" };

    if (reader->lengthInSamples <= 0)
        return { false, file.getFileName() + " is empty" };

    // Read at most one sample past the cap, which is enough for loadSamples() to see that the
    // file was longer and fade the cut.
    const auto cap = (juce::int64) std::ceil (reader->sampleRate * maxIRSeconds);
    const auto toRead = (int) juce::jmin (reader->lengthInSamples, cap + 1);
    const bool fileIsStereo = reader->numChannels > 1;

    if (options.stereo && fileIsStereo)
    {
        juce::AudioBuffer<float> samples (2, toRead);
        reader->read (&samples, 0, toRead, 0, true, true);
        return loadSamples (std::move (samples), reader->sampleRate, file.getFileNameWithoutExtension());
    }

    // A mono mic: one channel of the file (BUILD_PLAN "IR loading": close mics take the left by
    // default, with the right as an option).
    juce::AudioBuffer<float> samples (1, toRead);
    const bool useRight = fileIsStereo && channel == Channel::right;
    reader->read (&samples, 0, toRead, 0, ! useRight, useRight);

    auto name = file.getFileNameWithoutExtension();
    if (fileIsStereo)
        name << (useRight ? " [R]" : " [L]");

    return loadSamples (std::move (samples), reader->sampleRate, name);
}

CabIR::LoadResult CabIR::loadSamples (juce::AudioBuffer<float> samples, double sampleRate,
                                      const juce::String& name)
{
    if (samples.getNumChannels() < 1 || samples.getNumSamples() < 1 || sampleRate <= 0.0)
        return { false, name + " has no samples" };

    const auto numChannels = (options.stereo && samples.getNumChannels() > 1) ? 2 : 1;
    if (samples.getNumChannels() != numChannels)
        samples.setSize (numChannels, samples.getNumSamples(), true); // keep the first channel(s)

    const auto cap = (int) std::ceil (sampleRate * maxIRSeconds);
    const bool cut = samples.getNumSamples() > cap;

    if (cut)
    {
        samples.setSize (numChannels, cap, true);

        // Cutting an IR off mid-tail leaves a step at its end, which would ring as a click after
        // every note. A 10 ms linear fade-out removes the step.
        const auto fadeSamples = juce::jmin (cap, juce::roundToInt (0.010 * sampleRate));
        for (int ch = 0; ch < numChannels; ++ch)
            samples.applyGainRamp (ch, cap - fadeSamples, fadeSamples, 1.0f, 0.0f);
    }

    double energy = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < samples.getNumSamples(); ++i)
            energy += (double) samples.getSample (ch, i) * (double) samples.getSample (ch, i);

    if (energy < 1.0e-12)
        return { false, name + " is silent" };

    // Loudness matching (BUILD_PLAN "Cab: Normalization"): measure white noise with the same BS.1770
    // method the amps use, before and after the IR, and scale the IR by the difference, so every cab
    // changes white noise's loudness by exactly 0 LU. A stereo IR is measured as a stereo signal
    // against the noise in both channels. The plan said pink noise; measured on clean and distorted
    // guitar through very different cabs, white noise keeps swaps within 2.1 LU and pink within only
    // 6.6 LU (see referenceWhiteNoise and the "Cab normalization study" test).
    const auto noise = referenceWhiteNoise ((int) (4.0 * sampleRate));
    std::vector<std::vector<float>> through;
    std::vector<const float*> beforeChannels, afterChannels;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        through.push_back (loudness::fftConvolve (noise, samples.getReadPointer (ch), samples.getNumSamples()));
        beforeChannels.push_back (noise.data());
    }
    for (const auto& t : through)
        afterChannels.push_back (t.data());

    const auto before = loudness::integrated (beforeChannels, (int) noise.size(), sampleRate);
    const auto after = loudness::integrated (afterChannels, (int) noise.size(), sampleRate);

    if (! std::isfinite (after))
        return { false, name + " is effectively silent" };

    const auto gain = std::pow (10.0, (before - after) / 20.0);
    samples.applyGain ((float) gain);

    {
        const std::scoped_lock lock (loadedMutex);
        loadedIR.assign (samples.getReadPointer (0), samples.getReadPointer (0) + samples.getNumSamples());
    }

    const auto numSamples = samples.getNumSamples();
    auto pending = std::make_unique<PendingIR>();
    pending->samples = std::move (samples);
    pending->sampleRate = sampleRate;
    handoff.publish (std::move (pending));

    LoadResult result { true, name, numSamples, sampleRate, gain, numChannels };
    result.message << " (" << juce::String (juce::roundToInt (1000.0 * numSamples / sampleRate)) << " ms";
    if (numChannels == 2)
        result.message << ", stereo";
    if (cut)
        result.message << ", cut to 1 s";
    result.message << ", loudness matched " << juce::String (juce::Decibels::gainToDecibels (gain), 1) << " dB)";
    return result;
}

std::vector<float> CabIR::getLoadedIR() const
{
    const std::scoped_lock lock (loadedMutex);
    return loadedIR;
}

void CabIR::prepare (double sampleRate, int maxBlockSize)
{
    // Install a waiting IR before prepare(): JUCE then builds it right away, so it's active on the
    // very first process() call. Safe because prepare() never overlaps process().
    installPendingIR();
    convolution->prepare ({ sampleRate, (juce::uint32) maxBlockSize, options.stereo ? 2u : 1u });

    delete toRetire;
    toRetire = nullptr;
    handoff.collect();
}

void CabIR::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    installPendingIR();

    auto left = block.getSingleChannelBlock (0);

    if (options.stereo && block.getNumChannels() > 1)
    {
        // The mic hears the mono amp signal on both sides; a stereo IR then gives each side its own
        // response (a mono IR gives both the same one).
        auto stereo = block.getSubsetChannelBlock (0, 2);
        stereo.getSingleChannelBlock (1).copyFrom (left);

        if (hasIR)
            convolution->process (juce::dsp::ProcessContextReplacing<float> (stereo));

        return;
    }

    if (hasIR)
        convolution->process (juce::dsp::ProcessContextReplacing<float> (left));

    // A mono mic: both sides are the same.
    if (block.getNumChannels() > 1)
        block.getSingleChannelBlock (1).copyFrom (left);
}

void CabIR::installPendingIR()
{
    if (toRetire != nullptr && handoff.retire (toRetire))
        toRetire = nullptr;

    if (toRetire != nullptr)
        return; // the last one hasn't been collected yet, so try again next buffer

    if (auto* pending = handoff.take())
    {
        // Wait-free, and moving the buffer in means nothing gets allocated or copied here. JUCE
        // builds the new FFT engine on its own background thread and crossfades to it.
        const auto stereo = pending->samples.getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes
                                                                  : juce::dsp::Convolution::Stereo::no;
        expectedLength = pending->samples.getNumSamples();
        convolution->loadImpulseResponse (std::move (pending->samples), pending->sampleRate, stereo,
                                          juce::dsp::Convolution::Trim::no,
                                          juce::dsp::Convolution::Normalise::no);
        hasIR = true;

        if (! handoff.retire (pending))
            toRetire = pending;
    }
}

} // namespace ampsim

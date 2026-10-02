#include "CabIR.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace ampsim
{

CabIR::LoadResult CabIR::loadFile (const juce::File& file)
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

    juce::AudioBuffer<float> samples (1, toRead);
    // Close mics use the left channel of a stereo IR (BUILD_PLAN "IR loading").
    reader->read (&samples, 0, toRead, 0, true, false);

    return loadSamples (std::move (samples), reader->sampleRate, file.getFileNameWithoutExtension());
}

CabIR::LoadResult CabIR::loadSamples (juce::AudioBuffer<float> samples, double sampleRate,
                                      const juce::String& name)
{
    if (samples.getNumChannels() < 1 || samples.getNumSamples() < 1 || sampleRate <= 0.0)
        return { false, name + " has no samples" };

    if (samples.getNumChannels() > 1)
        samples.setSize (1, samples.getNumSamples(), true); // keep the left channel

    const auto cap = (int) std::ceil (sampleRate * maxIRSeconds);
    const bool cut = samples.getNumSamples() > cap;

    if (cut)
    {
        samples.setSize (1, cap, true);

        // Cutting an IR off mid-tail leaves a step at its end, which would ring as a click after
        // every note. A 10 ms linear fade-out removes the step.
        const auto fadeSamples = juce::jmin (cap, juce::roundToInt (0.010 * sampleRate));
        samples.applyGainRamp (0, cap - fadeSamples, fadeSamples, 1.0f, 0.0f);
    }

    // Normalize to unit energy: sum of h[n]^2 = 1. By Parseval's theorem the energy of h equals
    // its power gain averaged over all frequencies, so white noise leaves every cab at the level it
    // went in, and swapping IRs doesn't jump in volume. A stand-in for the planned pink-noise
    // loudness measurement.
    const auto* h = samples.getReadPointer (0);
    double energy = 0.0;

    for (int i = 0; i < samples.getNumSamples(); ++i)
        energy += (double) h[i] * (double) h[i];

    if (energy < 1.0e-12)
        return { false, name + " is silent" };

    samples.applyGain ((float) (1.0 / std::sqrt (energy)));

    const auto numSamples = samples.getNumSamples();
    auto pending = std::make_unique<PendingIR>();
    pending->samples = std::move (samples);
    pending->sampleRate = sampleRate;
    handoff.publish (std::move (pending));

    LoadResult result { true, name, numSamples, sampleRate };
    result.message << " (" << juce::String (juce::roundToInt (1000.0 * numSamples / sampleRate)) << " ms";
    if (cut)
        result.message << ", cut to 1 s";
    result.message << ")";
    return result;
}

void CabIR::prepare (double sampleRate, int maxBlockSize)
{
    // Install a waiting IR before prepare(): JUCE then builds it right away, so it's active on the
    // very first process() call. Safe because prepare() never overlaps process().
    installPendingIR();
    convolution.prepare ({ sampleRate, (juce::uint32) maxBlockSize, 1 });

    delete toRetire;
    toRetire = nullptr;
    handoff.collect();
}

void CabIR::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    installPendingIR();

    auto mono = block.getSingleChannelBlock (0);

    if (hasIR)
        convolution.process (juce::dsp::ProcessContextReplacing<float> (mono));

    // One mic, so both sides are the same. The three-mic cab with panning makes this real stereo.
    if (block.getNumChannels() > 1)
        block.getSingleChannelBlock (1).copyFrom (mono);
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
        convolution.loadImpulseResponse (std::move (pending->samples), pending->sampleRate,
                                         juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no,
                                         juce::dsp::Convolution::Normalise::no);
        hasIR = true;

        if (! handoff.retire (pending))
            toRetire = pending;
    }
}

} // namespace ampsim

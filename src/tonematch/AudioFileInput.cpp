// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AudioFileInput.h"

namespace ampsim::tonematch
{

namespace
{
/// A manager with every format this platform has (a fresh one per call: they're cheap, and then any
/// thread can read a file without sharing one).
std::unique_ptr<juce::AudioFormatManager> formats()
{
    auto f = std::make_unique<juce::AudioFormatManager>();
    f->registerBasicFormats();
    return f;
}
} // namespace

juce::String AudioFileInput::wildcard()
{
    return formats()->getWildcardForAllFormats();
}

std::vector<float> AudioFileInput::resampleTo48k (const std::vector<float>& x, double sampleRate)
{
    if (std::abs (sampleRate - 48000.0) < 0.5 || x.empty())
        return x;

    juce::AudioBuffer<float> in (1, (int) x.size());
    in.copyFrom (0, 0, x.data(), (int) x.size());
    juce::MemoryAudioSource memory (in, false);
    juce::ResamplingAudioSource resampler (&memory, false, 1);
    const auto ratio = sampleRate / 48000.0; // source samples per output sample
    resampler.setResamplingRatio (ratio);
    const auto outLength = (int) std::floor ((double) x.size() / ratio);
    const int block = 4096;
    resampler.prepareToPlay (block, 48000.0);

    std::vector<float> out ((size_t) outLength);
    juce::AudioBuffer<float> buffer (1, block);
    for (int start = 0; start < outLength; start += block)
    {
        const auto len = juce::jmin (block, outLength - start);
        juce::AudioSourceChannelInfo info (&buffer, 0, len);
        resampler.getNextAudioBlock (info);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + len, out.begin() + start);
    }
    resampler.releaseResources();
    return out;
}

AudioFileInput AudioFileInput::read (const juce::File& file, double maxSeconds)
{
    AudioFileInput r;
    if (! file.existsAsFile())
    {
        r.error = "File not found: " + file.getFullPathName();
        return r;
    }

    const auto manager = formats();
    std::unique_ptr<juce::AudioFormatReader> reader (manager->createReaderFor (file));
    if (reader == nullptr)
    {
        r.error = "Can't read " + file.getFileName() + " (not an audio format this computer can decode)";
        return r;
    }

    r.formatName = reader->getFormatName();
    r.sourceSampleRate = reader->sampleRate;
    r.sourceChannels = (int) reader->numChannels;
    const auto length = (int) std::min (reader->lengthInSamples, (juce::int64) (maxSeconds * reader->sampleRate));
    if (length <= 0 || reader->sampleRate <= 0.0)
    {
        r.error = file.getFileName() + " is empty";
        return r;
    }

    juce::AudioBuffer<float> buffer;
    try
    {
        buffer.setSize ((int) reader->numChannels, length);
    }
    catch (const std::bad_alloc&)
    {
        r.error = "Not enough memory to read " + file.getFileName();
        return r;
    }
    if (! reader->read (&buffer, 0, length, 0, true, true))
    {
        r.error = "Couldn't decode " + file.getFileName() + " (" + r.formatName + "; the file may be damaged)";
        return r;
    }

    std::vector<float> mono ((size_t) length, 0.0f);
    const auto gain = 1.0f / (float) buffer.getNumChannels();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += gain * buffer.getSample (ch, i);

    r.samples = resampleTo48k (mono, reader->sampleRate);
    r.ok = true;
    return r;
}

} // namespace ampsim::tonematch

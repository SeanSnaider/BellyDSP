// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// ampsim_separate: tone match's guitar separation (docs/TONE_MATCH.md, Stage C) from the command line.
// Any audio file the app reads in, the guitar stem out (48 kHz mono, 32-bit float WAV), with the time it
// took per minute of audio and the process's peak memory. Installs the model on first use, as the app does.

#include "tonematch/AudioFileInput.h"
#include "tonematch/GuitarSeparator.h"

#include <iostream>
#include <sys/resource.h>

namespace
{
void usage()
{
    std::cerr << "Usage: ampsim_separate [options] <input> <guitar.wav>\n"
                 "  --model-dir <dir>   where the converted model lives (default: the BellyDSP data folder's Separation)\n"
                 "  --url <url>         where to download the weights from if they aren't installed (default: the pinned upstream URL)\n"
                 "  --threads <n>       parts separated in parallel (default: half the cores, at most 8)\n"
                 "  --seconds <s>       separate only the first s seconds\n";
}

double peakMemoryMB()
{
    rusage r {};
    getrusage (RUSAGE_SELF, &r);
   #if JUCE_MAC
    return (double) r.ru_maxrss / (1024.0 * 1024.0); // bytes on macOS
   #else
    return (double) r.ru_maxrss / 1024.0; // kilobytes on Linux
   #endif
}
} // namespace

int main (int argc, char* argv[])
{
    juce::File modelDir = ampsim::tonematch::GuitarSeparator::defaultFolder();
    juce::String url = ampsim::tonematch::GuitarSeparator::weightsUrl;
    int threads = 0;
    double seconds = 0.0;
    std::vector<juce::String> positional;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const bool hasValue = i + 1 < argc;
        if (a == "--model-dir" && hasValue)   modelDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else if (a == "--url" && hasValue)    url = argv[++i];
        else if (a == "--threads" && hasValue) threads = juce::String (argv[++i]).getIntValue();
        else if (a == "--seconds" && hasValue) seconds = juce::String (argv[++i]).getDoubleValue();
        else if (a.startsWith ("--"))         { usage(); return 1; }
        else                                  positional.push_back (a);
    }
    if (positional.size() != 2)
    {
        usage();
        return 1;
    }

    auto in = ampsim::tonematch::AudioFileInput::read (juce::File::getCurrentWorkingDirectory().getChildFile (positional[0]));
    if (! in.ok)
    {
        std::cerr << in.error << "\n";
        return 1;
    }
    if (seconds > 0.0 && in.samples.size() > (size_t) (seconds * 48000.0))
        in.samples.resize ((size_t) (seconds * 48000.0));

    ampsim::tonematch::GuitarSeparator separator (modelDir);
    const std::atomic<bool> noCancel { false };
    if (! separator.isInstalled())
    {
        std::cout << "Installing the model from " << url << " (" << ampsim::tonematch::GuitarSeparator::weightsBytes / 1000000 << " MB)\n";
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto e = separator.install (url, noCancel, {});
        if (e.isNotEmpty())
        {
            std::cerr << e << "\n";
            return 1;
        }
        std::cout << "  installed in " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) << " s: "
                  << separator.modelFile().getFullPathName() << "\n";
    }

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    if (const auto e = separator.load(); e.isNotEmpty())
    {
        std::cerr << e << "\n";
        return 1;
    }
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    juce::String error;
    const auto guitar = separator.separate (in.samples, noCancel, {}, error, threads);
    const auto t2 = juce::Time::getMillisecondCounterHiRes();
    if (guitar.empty())
    {
        std::cerr << error << "\n";
        return 1;
    }

    const auto audioSeconds = in.seconds();
    const auto runSeconds = (t2 - t1) / 1000.0;
    std::cout << "Input: " << in.formatName << ", " << juce::String (audioSeconds, 1) << " s\n"
              << "Model loaded in " << juce::String ((t1 - t0) / 1000.0, 2) << " s\n"
              << "Separated in " << juce::String (runSeconds, 1) << " s: " << juce::String (60.0 * runSeconds / audioSeconds, 1)
              << " s per minute of audio (" << juce::String (audioSeconds / runSeconds, 2) << "x real time)\n"
              << "Peak memory: " << juce::String (peakMemoryMB(), 0) << " MB\n";

    const auto outFile = juce::File::getCurrentWorkingDirectory().getChildFile (positional[1]);
    outFile.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (outFile);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0).withNumChannels (1)
                                                                      .withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    juce::AudioBuffer<float> b (1, (int) guitar.size());
    b.copyFrom (0, 0, guitar.data(), (int) guitar.size());
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()))
    {
        std::cerr << "Couldn't write " << outFile.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "Wrote " << outFile.getFullPathName() << "\n";
    return 0;
}

// ampsim_render: runs a WAV file through the same DSP chain the app uses, offline.
//
// Use it to listen to a capture/IR combination without the interface, to compare against
// NeuralAmpModelerCore's own render tool (--compare), and to measure CPU per audio block (--slots N
// runs N amp slots at once, like the planned three always-running slots).

#include "dsp/Chain.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

namespace
{
struct Options
{
    juce::File model, ir, input, output, compare;
    float inputGainDb = 0.0f, outputGainDb = 0.0f;
    int blockSize = 128;
    int slots = 1;
    bool normalize = true;
};

void printUsage()
{
    std::cerr << "Usage: ampsim_render [options] <input.wav> <output.wav>\n"
                 "  --model <file.nam>     amp capture (omit for passthrough)\n"
                 "  --ir <file.wav>        cab impulse response (omit for no cab)\n"
                 "  --input-gain <dB>      input trim (default 0)\n"
                 "  --output-gain <dB>     output level (default 0)\n"
                 "  --block <samples>      block size (default 128)\n"
                 "  --slots <n>            run n amp slots at once to measure CPU (default 1)\n"
                 "  --no-normalize         skip loudness normalization of the model\n"
                 "  --compare <ref.wav>    report the difference between the output and a reference\n";
}

juce::File fileArg (const char* arg)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (arg));
}

bool parse (int argc, char* argv[], Options& o)
{
    std::vector<juce::File> positional;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const bool hasValue = i + 1 < argc;

        if (a == "--model" && hasValue)              o.model = fileArg (argv[++i]);
        else if (a == "--ir" && hasValue)            o.ir = fileArg (argv[++i]);
        else if (a == "--compare" && hasValue)       o.compare = fileArg (argv[++i]);
        else if (a == "--input-gain" && hasValue)    o.inputGainDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--output-gain" && hasValue)   o.outputGainDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--block" && hasValue)         o.blockSize = juce::String (argv[++i]).getIntValue();
        else if (a == "--slots" && hasValue)         o.slots = juce::String (argv[++i]).getIntValue();
        else if (a == "--no-normalize")              o.normalize = false;
        else if (a.startsWith ("--"))                return false;
        else                                         positional.push_back (fileArg (argv[i]));
    }

    if (positional.size() != 2 || o.blockSize < 1 || o.slots < 1)
        return false;

    o.input = positional[0];
    o.output = positional[1];
    return true;
}

juce::AudioBuffer<float> readMono (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return {};

    juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false); // left channel
    sampleRate = reader->sampleRate;
    return buffer;
}

bool writeFloatWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
        return false;

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (buffer.getNumChannels())
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

double toDb (double linear)
{
    return linear > 0.0 ? 20.0 * std::log10 (linear) : -400.0;
}

void printLevels (const char* name, const juce::AudioBuffer<float>& buffer)
{
    const auto n = buffer.getNumSamples();
    double sumSquares = 0.0;
    int nonFinite = 0;

    for (int i = 0; i < n; ++i)
    {
        const auto x = buffer.getSample (0, i);
        nonFinite += std::isfinite (x) ? 0 : 1;
        sumSquares += (double) x * x;
    }

    std::cout << "  " << name << ": peak " << juce::String (toDb (buffer.getMagnitude (0, 0, n)), 1)
              << " dBFS, RMS " << juce::String (toDb (std::sqrt (sumSquares / juce::jmax (1, n))), 1)
              << " dBFS, non-finite samples: " << nonFinite << "\n";
}
} // namespace

int main (int argc, char* argv[])
{
    Options o;

    if (! parse (argc, argv, o))
    {
        printUsage();
        return 1;
    }

    double sampleRate = 0.0;
    const auto input = readMono (o.input, sampleRate);

    if (input.getNumSamples() == 0)
    {
        std::cerr << "Couldn't read " << o.input.getFullPathName() << "\n";
        return 1;
    }

    if (std::abs (sampleRate - ampsim::NamAmp::requiredSampleRate) > 0.5)
    {
        std::cerr << "The input is " << sampleRate << " Hz; the chain runs at 48000 Hz only.\n";
        return 1;
    }

    ampsim::Chain chain;
    std::vector<std::unique_ptr<ampsim::NamAmp>> extraSlots;

    if (o.model != juce::File())
    {
        for (int s = 0; s < o.slots; ++s)
        {
            auto& amp = s == 0 ? chain.amp : *extraSlots.emplace_back (std::make_unique<ampsim::NamAmp>());
            const auto result = amp.loadModel (o.model, o.normalize);

            if (! result.ok)
            {
                std::cerr << result.message << "\n";
                return 1;
            }

            if (s == 0)
                std::cout << "Model: " << result.message << "\n";
        }
    }

    if (o.ir != juce::File())
    {
        const auto result = chain.cab.loadFile (o.ir);

        if (! result.ok)
        {
            std::cerr << result.message << "\n";
            return 1;
        }

        std::cout << "IR: " << result.message << "\n";
    }

    // Set the gains before prepare(), which snaps them, so the render doesn't start with a ramp.
    chain.inputGain.setGainDecibels (o.inputGainDb);
    chain.outputGain.setGainDecibels (o.outputGainDb);
    chain.prepare (sampleRate, o.blockSize); // also installs the model and IR with no fade

    for (auto& slot : extraSlots)
        slot->prepare (sampleRate, o.blockSize);

    const auto numSamples = input.getNumSamples();
    juce::AudioBuffer<float> output (2, numSamples), block (2, o.blockSize), slotScratch (1, o.blockSize);
    std::vector<double> blockMicros;

    for (int start = 0; start < numSamples; start += o.blockSize)
    {
        const auto len = juce::jmin (o.blockSize, numSamples - start);
        block.copyFrom (0, 0, input, 0, start, len);
        block.clear (1, 0, len);

        const auto t0 = std::chrono::steady_clock::now();

        chain.process (juce::dsp::AudioBlock<float> (block).getSubBlock (0, (size_t) len));

        for (auto& slot : extraSlots)
        {
            // The other always-running slots: same input, output thrown away.
            slotScratch.copyFrom (0, 0, input, 0, start, len);
            const ampsim::BlockContext context { input.getReadPointer (0, start), len };
            slot->process (juce::dsp::AudioBlock<float> (slotScratch).getSubBlock (0, (size_t) len), context);
        }

        const auto t1 = std::chrono::steady_clock::now();
        blockMicros.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());

        output.copyFrom (0, start, block, 0, 0, len);
        output.copyFrom (1, start, block, 1, 0, len);
    }

    std::cout << "Levels:\n";
    printLevels ("input ", input);
    printLevels ("output", output);

    // CPU: time per block against the real-time deadline (the block's duration).
    auto sorted = blockMicros;
    std::sort (sorted.begin(), sorted.end());
    const auto deadline = 1.0e6 * o.blockSize / sampleRate;
    const auto mean = std::accumulate (sorted.begin(), sorted.end(), 0.0) / (double) sorted.size();
    const auto p99 = sorted[(size_t) (0.99 * (double) (sorted.size() - 1))];
    const auto worst = sorted.back();

    std::cout << "CPU (" << o.slots << " amp slot" << (o.slots == 1 ? "" : "s") << ", " << o.blockSize
              << "-sample blocks, deadline " << juce::String (deadline, 0) << " us):\n"
              << "  mean " << juce::String (mean, 1) << " us (" << juce::String (100.0 * mean / deadline, 1)
              << "% of deadline), p99 " << juce::String (p99, 1) << " us, worst " << juce::String (worst, 1)
              << " us\n"
              << "  real-time factor " << juce::String (deadline / mean, 1) << "x\n";

    if (! writeFloatWav (o.output, output, sampleRate))
    {
        std::cerr << "Couldn't write " << o.output.getFullPathName() << "\n";
        return 1;
    }

    std::cout << "Wrote " << o.output.getFullPathName() << " (32-bit float, stereo)\n";

    if (o.compare != juce::File())
    {
        double refRate = 0.0;
        const auto reference = readMono (o.compare, refRate);
        const auto n = juce::jmin (reference.getNumSamples(), numSamples);

        if (n == 0)
        {
            std::cerr << "Couldn't read " << o.compare.getFullPathName() << "\n";
            return 1;
        }

        double errorSquares = 0.0, refSquares = 0.0, maxError = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double e = (double) output.getSample (0, i) - (double) reference.getSample (0, i);
            errorSquares += e * e;
            refSquares += (double) reference.getSample (0, i) * reference.getSample (0, i);
            maxError = juce::jmax (maxError, std::abs (e));
        }

        std::cout << "Compared with " << o.compare.getFileName() << " over " << n << " samples:\n"
                  << "  max abs difference " << juce::String (maxError, 9) << " ("
                  << juce::String (toDb (maxError), 1) << " dBFS)\n"
                  << "  difference relative to reference RMS: "
                  << juce::String (toDb (std::sqrt (errorSquares / refSquares)), 1) << " dB\n";
    }

    return 0;
}

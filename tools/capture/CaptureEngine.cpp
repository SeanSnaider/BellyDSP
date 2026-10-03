#include "CaptureEngine.h"

#include <algorithm>
#include <cmath>

namespace capture
{

void Engine::prepare (const std::vector<float>& stimulus, float outputGain, int samplesToPlay)
{
    total = samplesToPlay > 0 ? std::min (samplesToPlay, (int) stimulus.size()) : (int) stimulus.size();
    // The output level is applied here, once, so the callback only copies.
    played.assign (stimulus.begin(), stimulus.begin() + total);
    for (auto& x : played)
        x *= outputGain;
    recording.assign ((size_t) total, 0.0f);
    position.store (0);
    runningPeak.store (0.0f);
    clipped.store (0);
}

void Engine::process (const float* input, float* output, int numSamples) noexcept
{
    const auto start = position.load (std::memory_order_relaxed);
    const auto n = std::max (0, std::min (numSamples, total - start));

    // Sample start + i goes out now, and sample start + i of the return is what arrives in this same
    // callback: the two files line up at sample 0, with the round trip as a delay inside the recording.
    if (output != nullptr)
    {
        if (n > 0)
            std::copy (played.data() + start, played.data() + start + n, output);
        std::fill (output + n, output + numSamples, 0.0f);
    }

    if (n == 0)
        return;

    float peak = 0.0f;
    int over = 0;
    if (input != nullptr)
    {
        std::copy (input, input + n, recording.data() + start);
        for (int i = 0; i < n; ++i)
        {
            const auto a = std::abs (input[i]);
            peak = std::max (peak, a);
            over += a >= 0.99997f ? 1 : 0;
        }
    }

    // The peak for the progress display: a lock-free running maximum.
    for (auto old = runningPeak.load (std::memory_order_relaxed); peak > old && ! runningPeak.compare_exchange_weak (old, peak);)
    {
    }
    if (over > 0)
        clipped.fetch_add (over);
    position.store (start + n, std::memory_order_release);
}

int simulate (Engine& engine, int blockSize, const std::function<void (const float*, float*, int)>& device)
{
    std::vector<float> out ((size_t) blockSize), in ((size_t) blockSize);
    int callbacks = 0;
    while (! engine.isDone())
    {
        // A real duplex callback hands over the input and asks for the output at once. The gear's answer
        // to this callback's output can only arrive in a later callback's input, so the simulated device
        // gets what was played in the previous callback: one block of extra round trip, as in reality.
        engine.process (in.data(), out.data(), blockSize);
        device (out.data(), in.data(), blockSize);
        ++callbacks;
    }
    return callbacks;
}

Levels measure (const float* samples, int numSamples)
{
    Levels l;
    double sumSquares = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto a = std::abs (samples[i]);
        peak = std::max (peak, a);
        sumSquares += (double) a * a;
        l.clippedSamples += a >= 0.99997f ? 1 : 0;
    }
    const auto toDb = [] (double x) { return x > 1.0e-10 ? (float) (20.0 * std::log10 (x)) : -200.0f; };
    l.peakDb = toDb (peak);
    l.rmsDb = toDb (numSamples > 0 ? std::sqrt (sumSquares / numSamples) : 0.0);
    return l;
}

int measureLatency (const std::vector<float>& y)
{
    constexpr int lookahead = 1000, lookback = 10000;
    constexpr double absThreshold = 0.0003, relThreshold = 0.001; // NAM's _DELAY_CALIBRATION_*_THRESHOLD
    if ((int) y.size() < v3::blips[1] + lookback)
        return -1;

    double background = 0.0;
    for (int i = v3::noiseStart; i < v3::noiseEnd; ++i)
        background = std::max (background, (double) std::abs (y[(size_t) i]));
    const auto trigger = std::max (background + absThreshold, (1.0 + relThreshold) * background);

    for (int k = 0; k < lookahead + lookback; ++k)
    {
        double average = 0.0;
        for (const auto blip : v3::blips)
            average += y[(size_t) (blip - lookahead + k)];
        average /= (double) std::size (v3::blips);
        if (std::abs (average) > trigger)
            return k - lookahead;
    }
    return -1;
}

std::vector<float> readStimulus (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Couldn't read " + file.getFullPathName() + " as audio";
        return {};
    }
    if (! juce::exactlyEqual (reader->sampleRate, sampleRate))
    {
        error = file.getFileName() + " is " + juce::String (reader->sampleRate) + " Hz; captures are 48 kHz only";
        return {};
    }
    if (reader->numChannels != 1)
    {
        error = file.getFileName() + " has " + juce::String (reader->numChannels) + " channels; NAM's input file is mono";
        return {};
    }
    juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false);
    return { buffer.getReadPointer (0), buffer.getReadPointer (0) + buffer.getNumSamples() };
}

bool writeRecording (const juce::File& file, const std::vector<float>& samples, juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
    {
        error = "Couldn't write " + file.getFullPathName();
        return false;
    }
    const auto options = juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);
    if (writer == nullptr)
    {
        error = "Couldn't create a WAV writer for " + file.getFullPathName();
        return false;
    }
    const float* channels[] { samples.data() };
    if (! writer->writeFromFloatArrays (channels, 1, (int) samples.size()))
    {
        error = "Writing " + file.getFullPathName() + " failed";
        return false;
    }
    return true;
}

} // namespace capture

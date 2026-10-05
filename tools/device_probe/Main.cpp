// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// ampsim_device_probe: opens the audio interface through JUCE's CoreAudio backend, the same code
// path the standalone app uses, and reports what actually happens. It outputs silence.
//
//   ampsim_device_probe [--device "Scarlett Solo 4th Gen"] [--seconds 5] [--buffer 128]
//                       [--type "Windows Audio (Exclusive Mode)"] [--input <name>] [--output <name>]
//
// Checks the problems that needed a patched nih-plug in the Rust version: a 4-input device, input
// and output on the same device (the old deadlock), and callbacks arriving on time. It also prints
// each input channel's level, so strumming during the probe shows which channel the guitar is on.

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cmath>
#include <iostream>

namespace
{
constexpr int maxChannels = 8;

class Probe final : public juce::AudioIODeviceCallback
{
public:
    std::atomic<int> callbacks { 0 };
    std::atomic<int> numInputs { 0 }, numOutputs { 0 }, lastBlockSize { 0 };
    double sumSquares[maxChannels] {};
    float peak[maxChannels] {};
    juce::int64 samplesPerChannel = 0;
    double minInterval = 1.0e9, maxInterval = 0.0, sumInterval = 0.0;
    int intervals = 0;

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numIn, float* const* outputs, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (lastCallbackMs > 0.0)
        {
            const auto interval = now - lastCallbackMs;
            minInterval = std::min (minInterval, interval);
            maxInterval = std::max (maxInterval, interval);
            sumInterval += interval;
            ++intervals;
        }

        lastCallbackMs = now;

        for (int ch = 0; ch < std::min (numIn, maxChannels); ++ch)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const auto x = inputs[ch][i];
                sumSquares[ch] += (double) x * x;
                peak[ch] = std::max (peak[ch], std::abs (x));
            }
        }

        for (int ch = 0; ch < numOut; ++ch)
            juce::FloatVectorOperations::clear (outputs[ch], numSamples); // silence, always

        samplesPerChannel += numSamples;
        numInputs = numIn;
        numOutputs = numOut;
        lastBlockSize = numSamples;
        ++callbacks;
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    double lastCallbackMs = 0.0;
};

juce::String levelDb (double linear)
{
    return linear > 0.0 ? juce::String (20.0 * std::log10 (linear), 1) + " dBFS" : juce::String ("-inf (digital silence)");
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::String deviceName = "Scarlett Solo 4th Gen";
    juce::String inputName, outputName, typeName;
    double seconds = 5.0;
    int bufferSize = 128;

    // --input/--output (default: --device) are for Windows, where WASAPI lists a device's input and output as
    // separate endpoints with different names; --type picks a backend, e.g. "Windows Audio (Exclusive Mode)".
    for (int i = 1; i + 1 < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--device")       deviceName = argv[++i];
        else if (arg == "--input")   inputName = argv[++i];
        else if (arg == "--output")  outputName = argv[++i];
        else if (arg == "--type")    typeName = argv[++i];
        else if (arg == "--seconds") seconds = juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--buffer")  bufferSize = juce::String (argv[++i]).getIntValue();
    }

    if (inputName.isEmpty())
        inputName = deviceName;
    if (outputName.isEmpty())
        outputName = deviceName;

    juce::AudioDeviceManager manager;
    std::cout << "Audio devices JUCE can see:\n";

    for (auto* type : manager.getAvailableDeviceTypes())
    {
        // The default devices are what the standalone app opens before anything is chosen in its Options.
        type->scanForDevices();
        const auto inputs = type->getDeviceNames (true), outputs = type->getDeviceNames (false);
        for (int i = 0; i < inputs.size(); ++i)
            std::cout << "  [" << type->getTypeName() << "] input:  " << inputs[i] << (i == type->getDefaultDeviceIndex (true) ? "  (default)" : "") << "\n";
        for (int i = 0; i < outputs.size(); ++i)
            std::cout << "  [" << type->getTypeName() << "] output: " << outputs[i] << (i == type->getDefaultDeviceIndex (false) ? "  (default)" : "") << "\n";
    }

    if (typeName.isNotEmpty())
        manager.setCurrentAudioDeviceType (typeName, false);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputName;
    setup.outputDeviceName = outputName;
    setup.sampleRate = 48000.0;
    setup.bufferSize = bufferSize;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.setRange (0, maxChannels, true); // every input the device has
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.setRange (0, 2, true);

    const auto error = manager.initialise (maxChannels, 2, nullptr, false, {}, &setup);
    auto* device = manager.getCurrentAudioDevice();

    if (error.isNotEmpty() || device == nullptr || device->getName() != outputName)
    {
        std::cout << "\nCouldn't open \"" << inputName << "\" / \"" << outputName << "\": "
                  << (error.isNotEmpty() ? error : juce::String ("not found")) << "\n";
        return 1;
    }

    std::cout << "\nOpened \"" << device->getName() << "\" (" << device->getTypeName() << ") at "
              << device->getCurrentSampleRate() << " Hz, buffer " << device->getCurrentBufferSizeSamples() << " samples\n"
              << "  input channels:  " << device->getInputChannelNames().joinIntoString (", ") << "\n"
              << "  output channels: " << device->getOutputChannelNames().joinIntoString (", ") << "\n"
              << "  reported latency: input " << device->getInputLatencyInSamples() << " + output "
              << device->getOutputLatencyInSamples() << " samples\n"
              << "  buffer sizes it offers: " << [device] { juce::StringArray s; for (auto b : device->getAvailableBufferSizes()) s.add (juce::String (b)); return s.joinIntoString (", "); }() << "\n"
              << "Running for " << seconds << " s with silent output. Strum the guitar now to see its channel.\n";

    Probe probe;
    manager.addAudioCallback (&probe);
    juce::Thread::sleep ((int) (seconds * 1000.0));
    manager.removeAudioCallback (&probe);

    const auto rate = device->getCurrentSampleRate();
    const auto expectedCallbacks = seconds * rate / device->getCurrentBufferSizeSamples();
    const auto inputNames = device->getInputChannelNames();

    std::cout << "\nResults:\n"
              << "  callbacks: " << probe.callbacks.load() << " (expected about " << juce::roundToInt (expectedCallbacks)
              << " for " << seconds << " s), " << probe.numInputs.load() << " inputs and " << probe.numOutputs.load()
              << " outputs per callback, " << probe.lastBlockSize.load() << " samples each\n";

    if (probe.intervals > 0)
        std::cout << "  time between callbacks: mean " << juce::String (probe.sumInterval / probe.intervals, 3) << " ms, min "
                  << juce::String (probe.minInterval, 3) << " ms, max " << juce::String (probe.maxInterval, 3)
                  << " ms (one buffer is " << juce::String (1000.0 * device->getCurrentBufferSizeSamples() / rate, 3) << " ms)\n";

    std::cout << "  input levels:\n";

    for (int ch = 0; ch < std::min (probe.numInputs.load(), maxChannels); ++ch)
    {
        const auto rms = std::sqrt (probe.sumSquares[ch] / (double) std::max<juce::int64> (1, probe.samplesPerChannel));
        std::cout << "    channel " << ch << " (" << inputNames[ch] << "): peak " << levelDb (probe.peak[ch]) << ", RMS "
                  << levelDb (rms) << "\n";
    }

    manager.closeAudioDevice();
    return probe.callbacks.load() > 0 ? 0 : 1;
}

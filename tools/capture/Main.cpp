// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// ampsim_capture: records a NAM capture of your own gear through the audio interface (docs/CAPTURING.md).
//
// It plays NAM's standard training file (input.wav) out of one output of the interface, into the gear,
// and records what comes back on one input, sample for sample in the same audio callback, into a WAV with
// exactly the input file's length: the output.wav that NAM's trainer (tools/train_capture.sh) wants.
//
//   ampsim_capture --level-check                 play 17 s and report the return's level and the latency
//   ampsim_capture --output my_amp.wav           the full 190 s capture
//   ampsim_capture --simulate drive --output x.wav   no hardware: a simulated device (for testing)
//
// Run with --help for every option. It opens the interface through JUCE's CoreAudio backend, like the app
// and ampsim_device_probe.

#include "CaptureEngine.h"
#include "dsp/Overdrive.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
struct Options
{
    juce::String device = "Scarlett Solo 4th Gen";
    juce::File input, output;
    int outChannel = 1, inChannel = 0; // numbered as on the interface (Output 1, Input 1); 0: not chosen
    float outputLevelDb = -12.0f;
    int buffer = 256;
    bool levelCheck = false, yes = false, list = false, sine = false;
    juce::String simulate; // "", "delay", or "drive"
    int simulatedDelay = 300;
};

void printHelp()
{
    std::cout << R"(ampsim_capture: capture your own amp, pedal, or preamp for NAM (docs/CAPTURING.md)

Usage:
  ampsim_capture --level-check [options]     play 17 s, report the return level and latency, write nothing
  ampsim_capture --output <file.wav> [opts]  the full 190 s capture, then train it with tools/train_capture.sh

Options:
  --device <name>          the interface (default "Scarlett Solo 4th Gen")
  --input <file.wav>       NAM's input file (default build-deps/nam/input.wav, from tools/fetch_nam_input.sh)
  --output <file.wav>      where the recording goes (24-bit, 48 kHz, the input's exact length)
  --out-channel <n>        the output that feeds the gear, numbered as on the interface (default 1: Output 1)
  --in-channel <n>         the input the gear comes back on, numbered as on the interface (required):
                           1 for a 1/4" cable from a load box, pedal, or preamp (Input 1, unplug the guitar),
                           2 for a microphone on a cab (Input 2). See docs/CAPTURING.md
  --output-level-db <dB>   the input file's level at the output (default -12). An amp's input expects
                           guitar level: use a reamp box, or keep this low (see CAPTURING.md)
  --level-check            (also --dry-run) the first 17 s only: nothing is saved
  --sine-1k                10 s of a 1 kHz sine at --output-level-db, to measure the output's voltage
                           with a multimeter (docs/CAPTURING.md, "Input level metadata")
  --buffer <samples>       the interface's buffer size (default 256; the capture doesn't need low latency)
  --yes                    start without asking for Enter first
  --list                   list the audio devices and their channels, then quit
  --simulate delay|drive   no interface: a simulated device. "delay" returns the output 300 samples later at
                           -6 dB; "drive" runs it through the app's Distortion pedal circuit (Drive 60%, +6 dB level) first. For tests
                           and for trying the whole workflow without gear.
  --simulated-delay <n>    the simulated round trip in samples (default 300)
)";
}

bool parse (int argc, char* argv[], Options& o)
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    o.input = cwd.getChildFile ("build-deps/nam/input.wav");
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const auto hasValue = i + 1 < argc;
        if (a == "--help" || a == "-h")                        { printHelp(); std::exit (0); }
        else if (a == "--device" && hasValue)                  o.device = argv[++i];
        else if (a == "--input" && hasValue)                   o.input = cwd.getChildFile (argv[++i]);
        else if (a == "--output" && hasValue)                  o.output = cwd.getChildFile (argv[++i]);
        else if (a == "--out-channel" && hasValue)             o.outChannel = juce::String (argv[++i]).getIntValue();
        else if (a == "--in-channel" && hasValue)              o.inChannel = juce::String (argv[++i]).getIntValue();
        else if (a == "--output-level-db" && hasValue)         o.outputLevelDb = juce::String (argv[++i]).getFloatValue();
        else if (a == "--buffer" && hasValue)                  o.buffer = juce::String (argv[++i]).getIntValue();
        else if (a == "--simulate" && hasValue)                o.simulate = argv[++i];
        else if (a == "--simulated-delay" && hasValue)         o.simulatedDelay = juce::String (argv[++i]).getIntValue();
        else if (a == "--level-check" || a == "--dry-run")     o.levelCheck = true;
        else if (a == "--sine-1k")                             o.sine = true;
        else if (a == "--yes")                                 o.yes = true;
        else if (a == "--list")                                o.list = true;
        else
        {
            std::cout << "Don't know the option \"" << a << "\". Run with --help to see them all.\n";
            return false;
        }
    }
    if (o.outputLevelDb > 0.0f)
    {
        std::cout << "--output-level-db must be 0 or below (it's a level in dBFS).\n";
        return false;
    }
    if (o.simulate.isNotEmpty() && o.simulate != "delay" && o.simulate != "drive")
    {
        std::cout << "--simulate takes \"delay\" or \"drive\".\n";
        return false;
    }
    if (! o.list && o.simulate.isEmpty() && ! o.sine && o.inChannel < 1)
    {
        std::cout << "Say which input the gear comes back on with --in-channel: 1 (Input 1, a 1/4\" cable from a load box,\n"
                     "pedal, or preamp) or 2 (Input 2, a microphone). docs/CAPTURING.md has a command for each setup.\n";
        return false;
    }
    if (! o.levelCheck && ! o.list && ! o.sine && o.output == juce::File())
    {
        std::cout << "Say where the recording goes with --output <file.wav>, or run --level-check first.\n";
        return false;
    }
    return true;
}

juce::String db (float value)
{
    return value <= -199.0f ? juce::String ("-inf") : juce::String (value, 1);
}

/// The audio callback: the engine on one input and one output channel; every other output silent.
class DeviceCallback final : public juce::AudioIODeviceCallback
{
public:
    DeviceCallback (capture::Engine& e, int in, int out) : engine (e), inIndex (in), outIndex (out) {}

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numIn, float* const* outputs, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOut; ++ch)
            if (ch != outIndex && outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);
        engine.process (inIndex < numIn ? inputs[inIndex] : nullptr, outIndex < numOut ? outputs[outIndex] : nullptr, numSamples);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    capture::Engine& engine;
    const int inIndex, outIndex;
};

/// What the return's level means, in plain words.
void adviseOnLevel (const capture::Levels& l, float predictedPeakDb, float outputLevelDb)
{
    std::cout << "\nWhat that means:\n";
    if (l.peakDb < -40.0f)
        std::cout << "  Almost nothing came back. Check the cables, that the gear is on, and that --in-channel is the input\n"
                     "  the gear is plugged into (ampsim_capture --list shows the channels). Turn the Solo's gain knob for\n"
                     "  that input up if the gear's output is quiet.\n";
    else if (l.clippedSamples > 0 || predictedPeakDb > -3.0f)
        std::cout << "  Too hot: the return clips or will come within 3 dB of clipping in the full run. Turn the Solo's\n"
                     "  gain knob for that input down (or the gear's output level), then check again.\n";
    else if (predictedPeakDb < -6.0f)
        std::cout << "  A little quiet: aim for peaks between -6 and -3 dBFS. Turn the Solo's gain knob for that input up\n"
                     "  a bit (not --output-level-db: that changes how hard the gear is driven, which changes the tone).\n";
    else
        std::cout << "  Good: the full run should peak between -6 and -3 dBFS.\n";
    std::cout << "  (--output-level-db " << db (outputLevelDb) << " sets how hard the gear is driven; the Solo's input gain only sets\n"
                 "  how loud the recording is. Set the drive first, by what you want to capture, then the input gain.)\n";
}

void reportLatency (int latency)
{
    if (latency >= 0)
        std::cout << "  Round trip: " << latency << " samples (" << juce::String (1000.0 * latency / capture::sampleRate, 2)
                  << " ms), measured from the input file's two calibration blips the way NAM's trainer does.\n"
                     "  Nothing to do with it: the trainer measures and removes it itself.\n";
    else
        std::cout << "  Round trip: not detected (the blips at 10.5 and 11.5 s didn't come back above the noise). With the\n"
                     "  output turned right down that's expected; on a real capture the trainer would complain too, so check\n"
                     "  the level and the wiring.\n";
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Options o;
    if (! parse (argc, argv, o))
        return 2;

    juce::AudioDeviceManager manager;
    if (o.list)
    {
        for (auto* type : manager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            for (const auto& name : type->getDeviceNames (true))
                std::cout << "[" << type->getTypeName() << "] " << name << "\n";
        }
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = setup.outputDeviceName = o.device;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange (0, 8, true);
        setup.outputChannels.setRange (0, 8, true);
        if (manager.initialise (8, 8, nullptr, false, {}, &setup).isEmpty() && manager.getCurrentAudioDevice() != nullptr)
        {
            auto* d = manager.getCurrentAudioDevice();
            std::cout << "\n" << d->getName() << ":\n";
            const auto ins = d->getInputChannelNames(), outs = d->getOutputChannelNames();
            for (int i = 0; i < ins.size(); ++i)
                std::cout << "  --in-channel " << (i + 1) << "   " << ins[i] << "\n";
            for (int i = 0; i < outs.size(); ++i)
                std::cout << "  --out-channel " << (i + 1) << "  " << outs[i] << "\n";
        }
        return 0;
    }

    juce::String error;
    std::vector<float> stimulus;
    if (o.sine)
    {
        // A steady 1 kHz sine at full scale (the output level scales it), for a multimeter: an AC meter
        // reads RMS volts, and dBu = 20 log10 (V / 0.7746).
        std::cout << "Step 1: a 1 kHz sine, 10 s\n";
        stimulus.resize (10 * 48'000);
        for (size_t i = 0; i < stimulus.size(); ++i)
            stimulus[i] = (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / capture::sampleRate);
        if (o.inChannel < 1)
            o.inChannel = 1; // nothing is analysed; any input will do
    }
    else
    {
        std::cout << "Step 1: reading NAM's input file " << o.input.getFullPathName() << "\n";
        stimulus = capture::readStimulus (o.input, error);
    }
    if (stimulus.empty())
    {
        std::cout << "  " << error << "\n  Get it with tools/fetch_nam_input.sh (it checks the download's SHA-256).\n";
        return 1;
    }
    std::cout << "  " << stimulus.size() << " samples (" << juce::String ((double) stimulus.size() / capture::sampleRate, 1) << " s) at 48 kHz";
    if (! o.sine && (int) stimulus.size() != capture::v3::length)
        std::cout << "\n  WARNING: that isn't the length of NAM's v3.0.0 input file (" << capture::v3::length
                  << " samples). The trainer may not recognise it, and the latency measurement here assumes v3.";
    std::cout << "\n";

    const auto gain = juce::Decibels::decibelsToGain (o.outputLevelDb);
    const auto toPlay = o.levelCheck ? capture::v3::levelCheckLength : 0;
    capture::Engine engine;
    engine.prepare (stimulus, gain, toPlay);
    const auto seconds = engine.getLength() / capture::sampleRate;

    if (o.simulate.isNotEmpty())
    {
        // No interface: the same engine, with a simulated device in the callback's place.
        std::cout << "Step 2: simulating the device (" << o.simulate << ", " << o.simulatedDelay << " samples of round trip plus one "
                  << o.buffer << "-sample buffer, as a real duplex callback adds)\n";
        std::vector<float> line ((size_t) o.simulatedDelay + 1, 0.0f);
        size_t writeAt = 0;
        ampsim::Overdrive drive;
        ampsim::Overdrive::Settings settings;
        settings.mode = ampsim::Overdrive::Mode::distortion;
        settings.drive = 0.6f;
        settings.tone = 0.5f;
        settings.levelDb = 6.0f;  // a return that peaks around -6 dBFS, like a well-set capture
        drive.prepare (capture::sampleRate, o.buffer);
        std::vector<float> scratch ((size_t) o.buffer);
        const auto useDrive = o.simulate == "drive";
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto callbacks = capture::simulate (engine, o.buffer, [&] (const float* played, float* returned, int n)
        {
            std::copy (played, played + n, scratch.begin());
            if (useDrive)
            {
                drive.setSettings (settings);
                float* channels[] { scratch.data() };
                drive.process (juce::dsp::AudioBlock<float> (channels, 1, (size_t) n), {});
            }
            for (int i = 0; i < n; ++i)
            {
                line[writeAt] = useDrive ? scratch[(size_t) i] : 0.5f * scratch[(size_t) i]; // -6 dB for "delay"
                writeAt = (writeAt + 1) % line.size();
                returned[i] = line[writeAt]; // written o.simulatedDelay samples ago
            }
        });
        std::cout << "  " << callbacks << " callbacks in " << juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) << " s\n";
    }
    else
    {
        std::cout << "Step 2: opening \"" << o.device << "\" at 48 kHz\n";
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = setup.outputDeviceName = o.device;
        setup.sampleRate = capture::sampleRate;
        setup.bufferSize = o.buffer;
        setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange (0, 8, true);   // every input, so channel n arrives as inputs[n]
        setup.outputChannels.setRange (0, 8, true);
        const auto openError = manager.initialise (8, 8, nullptr, false, {}, &setup);
        auto* device = manager.getCurrentAudioDevice();
        if (openError.isNotEmpty() || device == nullptr || device->getName() != o.device)
        {
            std::cout << "  Couldn't open it: " << (openError.isNotEmpty() ? openError : juce::String ("not found")) << "\n"
                      << "  Is it plugged in? ampsim_capture --list shows what's connected.\n";
            return 1;
        }
        if (! juce::exactlyEqual (device->getCurrentSampleRate(), capture::sampleRate))
        {
            std::cout << "  It's running at " << device->getCurrentSampleRate() << " Hz and wouldn't switch to 48 kHz. Captures are 48 kHz only:\n"
                         "  set it to 48000 Hz in Audio MIDI Setup (Applications > Utilities) and run this again.\n";
            return 1;
        }
        const auto ins = device->getInputChannelNames(), outs = device->getOutputChannelNames();
        if (o.inChannel < 1 || o.inChannel > ins.size() || o.outChannel < 1 || o.outChannel > outs.size())
        {
            std::cout << "  It has inputs 1 to " << ins.size() << " and outputs 1 to " << outs.size() << "; --in-channel " << o.inChannel
                      << " or --out-channel " << o.outChannel << " isn't one of them.\n";
            return 1;
        }
        std::cout << "  Opened at 48 kHz, " << device->getCurrentBufferSizeSamples() << "-sample buffers. Playing out of "
                  << outs[o.outChannel - 1] << ", recording " << ins[o.inChannel - 1] << ".\n";

        std::cout << "\nStep 3: " << (o.sine ? "sine" : o.levelCheck ? "level check" : "capture") << ": " << juce::String (seconds, 1) << " s of NAM's input file at "
                  << db (o.outputLevelDb) << " dBFS out of " << outs[o.outChannel - 1] << (o.sine ? " (a 1 kHz sine)" : "") << ".\n";
        if (o.outputLevelDb > -20.0f)
            std::cout << "  Heads up: whatever is on " << outs[o.outChannel - 1] << " (and the headphones, which mirror outputs 1 and 2) gets\n"
                         "  test tones and noise at this level. Reamping into an amp's input wants guitar level: use a reamp box,\n"
                         "  or keep this around -20 dBFS or lower. Turn monitors and headphones down.\n";
        if (! o.yes)
        {
            std::cout << "  Press Enter to start (Ctrl-C to cancel)... " << std::flush;
            std::string ignored;
            std::getline (std::cin, ignored);
        }

        DeviceCallback callback (engine, o.inChannel - 1, o.outChannel - 1);
        const auto xrunsBefore = device->getXRunCount();
        manager.addAudioCallback (&callback);
        while (! engine.isDone())
        {
            juce::Thread::sleep (1000);
            std::cout << "  " << juce::roundToInt (engine.getPosition() / capture::sampleRate) << " of " << juce::roundToInt (seconds)
                      << " s, return peak " << db (juce::Decibels::gainToDecibels (engine.takePeak(), -200.0f)) << " dBFS\n" << std::flush;
        }
        manager.removeAudioCallback (&callback);
        const auto xruns = device->getXRunCount() - xrunsBefore;
        manager.closeAudioDevice();
        if (xruns > 0)
            std::cout << "  WARNING: the interface reported " << xruns << " dropout(s). The recording stays aligned (output and input skip\n"
                         "  together), but a dropout can put a glitch in it: run again with --buffer 512 and nothing else open.\n";
    }

    if (o.sine)
    {
        std::cout << "\nDone. If a meter (AC volts) read V at the gear's input, a capture made at --output-level-db " << db (o.outputLevelDb)
                  << " has\n  --input-level-dbu = 20 log10 (V / 0.7746)   (docs/CAPTURING.md, \"Input level metadata\")\n";
        return 0;
    }

    const auto& recording = engine.getRecording();
    const auto levels = capture::measure (recording.data(), (int) recording.size());
    // How much louder the full file gets than what was just played (the training audio peaks a little
    // higher than the first 17 s), to predict the full run's peak from a level check.
    // The blips (two single-sample clicks near full scale, 10.5 and 11.5 s) are left out of this
    // comparison: they'd make the check look as loud as the training audio when it isn't.
    const auto peakOutsideBlips = [] (const std::vector<float>& x, int length)
    {
        const auto blipsStart = 480'000, blipsEnd = std::min (576'000, length);
        auto peak = capture::measure (x.data(), std::min (blipsStart, length)).peakDb;
        if (length > blipsEnd)
            peak = std::max (peak, capture::measure (x.data() + blipsEnd, length - blipsEnd).peakDb);
        return peak;
    };
    const auto playedPeak = peakOutsideBlips (stimulus, engine.getLength());
    const auto filePeak = peakOutsideBlips (stimulus, (int) stimulus.size());
    const auto predicted = peakOutsideBlips (recording, (int) recording.size()) + (filePeak - playedPeak);
    std::cout << "\nResults (" << recording.size() << " samples recorded, the same as played):\n"
              << "  Return: peak " << db (levels.peakDb) << " dBFS, RMS " << db (levels.rmsDb) << " dBFS, " << levels.clippedSamples
              << " samples at full scale (clipped)\n";
    if (o.levelCheck)
        std::cout << "  Leaving out the two calibration clicks, the whole file peaks " << juce::String (filePeak - playedPeak, 1)
                  << " dB higher than this check, so the full run should peak around "
                  << db (predicted) << " dBFS (if the gear is roughly linear in level).\n";
    reportLatency (capture::measureLatency (recording));
    adviseOnLevel (levels, o.levelCheck ? predicted : levels.peakDb, o.outputLevelDb);

    if (o.output != juce::File())
    {
        if (! capture::writeRecording (o.output, recording, error))
        {
            std::cout << "\n" << error << "\n";
            return 1;
        }
        std::cout << "\nWrote " << o.output.getFullPathName() << " (24-bit, 48 kHz, " << recording.size() << " samples).\n";
        if (! o.levelCheck)
            std::cout << "Next: train it (docs/CAPTURING.md, step 5):\n  tools/train_capture.sh --input " << o.input.getFullPathName() << " --output "
                      << o.output.getFullPathName() << " --name \"My capture\" --tone-type hi_gain --gear-type amp\n";
    }
    return levels.clippedSamples > 0 ? 3 : 0;
}

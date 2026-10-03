// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TestHelpers.h"
#include "dsp/Loudness.h"
#include "dsp/NamAmp.h"
#include "dsp/ReferenceSignals.h"

#include <NAM/get_dsp.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
using namespace testing;

std::vector<float> runAmp (ampsim::NamAmp& amp, const std::vector<float>& input,
                           const std::function<void (size_t start)>& beforeBlock = {})
{
    const ampsim::BlockContext context;
    return runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
    {
        if (beforeBlock)
            beforeBlock (start);

        amp.process (block.getSubsetChannelBlock (0, 1), context);
    }).left;
}

bool renderWithOfficialTool (const juce::File& model, const juce::File& input, const juce::File& output)
{
    juce::ChildProcess process;
    const juce::StringArray args { namRenderTool().getFullPathName(), model.getFullPathName(),
                                   input.getFullPathName(), output.getFullPathName() };

    if (! process.start (args))
        return false;

    process.readAllProcessOutput();
    return process.waitForProcessToFinish (60000) && process.getExitCode() == 0;
}

struct Timing
{
    double mean = 0.0, p99 = 0.0, worst = 0.0;
};

Timing summarize (std::vector<double> micros)
{
    std::sort (micros.begin(), micros.end());
    Timing t;
    t.mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
    t.p99 = micros[(size_t) (0.99 * (double) (micros.size() - 1))];
    t.worst = micros.back();
    return t;
}

juce::String percentOfDeadline (double micros) { return juce::String (100.0 * micros / deadlineMicros, 1) + "%"; }

juce::Array<juce::File> exampleModels()
{
    auto files = namDir().getChildFile ("example_models").findChildFiles (juce::File::findFiles, false, "*.nam");
    files.sort();
    return files;
}

class NamAmpTests final : public juce::UnitTest
{
public:
    NamAmpTests() : juce::UnitTest ("NAM amp slot", "ampsim") {}

    void runTest() override
    {
        const auto input = guitarDI ((int) (2.0 * fs));
        const auto a1 = exampleModel ("wavenet_a1_standard.nam");
        const auto lstm = exampleModel ("lstm.nam");

        beginTest ("passes the input through untouched until a model is loaded");
        {
            ampsim::NamAmp amp;
            amp.prepare (fs, blockSize);
            const auto out = runAmp (amp, input);
            expect (! amp.hasModel());
            expectEquals (maxAbsDifference (out, input), 0.0);
            logMessage ("  -> no model: output equals input, max difference " + juce::String (maxAbsDifference (out, input)));
        }

        beginTest ("loads every example model NeuralAmpModelerCore ships, or says why not");
        {
            juce::StringArray loaded;

            for (const auto& file : exampleModels())
            {
                ampsim::NamAmp amp;
                const auto result = amp.loadModel (file, false);

                if (! result.ok)
                {
                    logMessage ("  -> " + file.getFileName() + ": rejected: " + result.message);
                    continue;
                }

                amp.prepare (fs, blockSize);
                const auto out = runAmp (amp, input);
                const bool finite = std::all_of (out.begin(), out.end(), [] (float v) { return std::isfinite (v); });

                expect (finite, file.getFileName() + " produced NaN or Inf");
                expectGreaterThan (rms (out), 1.0e-6, file.getFileName() + " produced silence");
                loaded.add (file.getFileName());
                logMessage ("  -> " + file.getFileName() + ": loaded, output RMS " + juce::String (toDb (rms (out)), 1)
                            + " dBFS, all samples finite: " + (finite ? "yes" : "NO"));
            }

            for (auto name : { "wavenet_a1_standard.nam", "lstm.nam", "wavenet.nam", "A2.nam" })
                expect (loaded.contains (name), juce::String (name) + " must load");
        }

        beginTest ("matches NeuralAmpModelerCore's own render tool on two stimuli (limit -100 dB)");
        {
            expect (namRenderTool().existsAsFile(), "nam_render wasn't built");

            // Stimulus 1: NAM core's own example input (1 s silence, 1 s test tone). Stimulus 2: the
            // broadband one (guitar DI, a 20 Hz to 20 kHz sweep, noise, silence), which exercises the
            // whole network instead of one frequency.
            const auto richFile = tempDir().getChildFile ("rich_stimulus.wav");
            expect (writeWav (richFile, richStimulus()));

            for (const auto& file : exampleModels())
            {
                juce::StringArray results;

                for (const auto& stimulus : { exampleInputFile(), richFile })
                {
                    ampsim::NamAmp amp;
                    if (! amp.loadModel (file, false).ok)
                        continue;

                    const auto reference = tempDir().getChildFile ("ref_" + file.getFileNameWithoutExtension() + "_"
                                                                   + stimulus.getFileNameWithoutExtension() + ".wav");
                    if (! renderWithOfficialTool (file, stimulus, reference))
                    {
                        results.add (stimulus.getFileName() + ": official tool refused it");
                        continue;
                    }

                    const auto refBuffer = readWav (reference);
                    const std::vector<float> expected (refBuffer.getReadPointer (0), refBuffer.getReadPointer (0) + refBuffer.getNumSamples());
                    const auto stimulusBuffer = readWav (stimulus);
                    const std::vector<float> in (stimulusBuffer.getReadPointer (0), stimulusBuffer.getReadPointer (0) + stimulusBuffer.getNumSamples());

                    amp.prepare (fs, blockSize);
                    const auto ours = runAmp (amp, in);
                    const auto error = relativeErrorDb (ours, expected);

                    expectEquals ((int) expected.size(), (int) ours.size());
                    expectLessThan (error, -100.0, file.getFileName() + " on " + stimulus.getFileName());
                    results.add ((stimulus == richFile ? juce::String ("rich stimulus ") : juce::String ("NAM example input "))
                                 + dB (error) + " (max difference " + juce::String (maxAbsDifference (ours, expected), 9) + ")");
                }

                logMessage ("  -> " + file.getFileName() + " vs. official: " + results.joinIntoString ("; "));
            }
        }

        beginTest ("normalizes every model to -18 LUFS on the reference DI, measured rather than taken from the file");
        {
            const auto reference = ampsim::referenceGuitarDI ((int) (ampsim::NamAmp::loudnessProbeSeconds * fs));
            const auto otherTake = ampsim::referenceGuitarDI ((int) (6.0 * fs), fs, 7); // different plucks, not used for measuring
            juce::StringArray results;
            std::vector<double> otherLoudness;

            for (auto name : { "wavenet_a1_standard.nam", "lstm.nam", "wavenet.nam", "A2.nam" })
            {
                ampsim::NamAmp amp;
                const auto result = amp.loadModel (exampleModel (name), true);
                amp.prepare (fs, blockSize);
                const auto out = runAmp (amp, reference);
                const auto onReference = ampsim::loudness::integratedMono (out.data(), (int) out.size(), fs);

                ampsim::NamAmp again;
                again.loadModel (exampleModel (name), true);
                again.prepare (fs, blockSize);
                const auto outOther = runAmp (again, otherTake);
                const auto onOther = ampsim::loudness::integratedMono (outOther.data(), (int) outOther.size(), fs);
                otherLoudness.push_back (onOther);

                expectWithinAbsoluteError (onReference, ampsim::NamAmp::targetLoudnessLufs, 0.01, name);

                juce::String metadata = "none";
                auto json = juce::JSON::parse (exampleModel (name));
                if (auto* metadataObject = json.getProperty ("metadata", {}).getDynamicObject())
                    if (metadataObject->hasProperty ("loudness"))
                        metadata = juce::String ((double) metadataObject->getProperty ("loudness"), 1) + " dB";

                results.add (juce::String (name) + ": measured " + juce::String (result.measuredLufs, 1) + " LUFS, gain "
                             + juce::String (result.normalizationDb, 1) + " dB, now " + juce::String (onReference, 2)
                             + " LUFS (file's own loudness field: " + metadata + ")");
            }

            const auto spread = *std::max_element (otherLoudness.begin(), otherLoudness.end())
                                - *std::min_element (otherLoudness.begin(), otherLoudness.end());
            expectLessThan (spread, 1.0);

            for (const auto& r : results)
                logMessage ("  -> " + r);
            logMessage ("  -> on a different take of the riff (not the one measured), the four normalized models span "
                        + juce::String (spread, 2) + " LU");
        }

        beginTest ("switching models crossfades over 20 ms at equal power instead of jumping");
        {
            const auto signal = sine (110.0, 0.25, (int) fs);
            const size_t switchAt = 100 * blockSize;
            const int fadeLength = juce::roundToInt (fs * ampsim::NamAmp::switchFadeSeconds);

            ampsim::NamAmp amp, refA, refB;
            amp.loadModel (a1, false);
            amp.prepare (fs, blockSize);
            refA.loadModel (a1, false);
            refA.prepare (fs, blockSize);
            refB.loadModel (lstm, false);
            refB.prepare (fs, blockSize);

            bool sawSwitching = false;
            const ampsim::BlockContext context;
            auto out = runInBlocks (signal, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
            {
                if (start == switchAt)
                    amp.loadModel (lstm, false); // as if the loader thread finished right now

                amp.process (block.getSubsetChannelBlock (0, 1), context);
                sawSwitching = sawSwitching || amp.isSwitching();
            }).left;

            const auto a = runAmp (refA, signal);
            const auto bTail = runAmp (refB, std::vector<float> (signal.begin() + (long) switchAt, signal.end()));

            // Build the expected output: A, then sin/cos crossfade, then B.
            std::vector<float> expected (signal.size());
            for (size_t n = 0; n < signal.size(); ++n)
            {
                if (n < switchAt)
                {
                    expected[n] = a[n];
                    continue;
                }

                const auto b = bTail[n - switchAt];
                const auto t = std::min (1.0f, (float) (n - switchAt) / (float) fadeLength);
                const auto angle = juce::MathConstants<float>::halfPi * t;
                expected[n] = n < switchAt + 8 * blockSize ? b * std::sin (angle) + a[n] * std::cos (angle) : b;
            }

            const auto deviation = maxAbsDifference (out, expected);
            const auto steady = std::max (maxStep (a, 0, switchAt), maxStep (out, switchAt + 2 * (size_t) fadeLength));
            const auto during = maxStep (out, switchAt - 1, switchAt + (size_t) fadeLength + 1);
            const auto hardJump = std::abs (bTail[0] - a[switchAt - 1]);

            expect (sawSwitching);
            expect (! amp.isSwitching(), "the switch must be finished by the end");
            expectLessThan (deviation, 1.0e-5);
            expectLessThan (during, steady * 1.5);
            logMessage ("  -> output follows A, then sin/cos crossfade over " + juce::String (fadeLength)
                        + " samples (20 ms), then B: max deviation from that formula " + juce::String (deviation, 8));
            logMessage ("  -> largest sample-to-sample step during the switch " + juce::String (during, 5)
                        + " vs. " + juce::String (steady, 5) + " in steady state; a hard switch would have jumped "
                        + juce::String (hardJump, 5));

            writeWav (proofDir().getChildFile ("model_switch_a1_to_lstm.wav"), out);
        }

        beginTest ("loading models while audio runs never stalls the audio thread");
        {
            ampsim::NamAmp amp;
            amp.loadModel (a1, false);
            amp.prepare (fs, blockSize);

            std::atomic<bool> loading { true };
            std::atomic<int> loadsDone { 0 };
            std::atomic<double> shortestA1LoadMicros { 1.0e12 };
            std::thread loader ([&]
            {
                for (int i = 0; i < 6; ++i)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    amp.loadModel (i % 2 == 0 ? lstm : a1, false);
                    const auto took = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();

                    if (i % 2 == 1)
                        shortestA1LoadMicros = std::min (shortestA1LoadMicros.load(), took);

                    ++loadsDone;
                    std::this_thread::sleep_for (std::chrono::milliseconds (60));
                }
                loading = false;
            });

            const auto signal = guitarDI ((int) (2.0 * fs));
            juce::AudioBuffer<float> buffer (1, blockSize);
            const ampsim::BlockContext context;
            std::vector<double> blockTimes;
            int switches = 0;
            bool wasSwitching = false;

            for (size_t start = 0; loading || amp.isSwitching(); start = (start + blockSize) % (signal.size() - blockSize))
            {
                buffer.copyFrom (0, 0, signal.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                amp.process (juce::dsp::AudioBlock<float> (buffer), context);
                blockTimes.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());

                switches += (amp.isSwitching() && ! wasSwitching) ? 1 : 0;
                wasSwitching = amp.isSwitching();
            }

            loader.join();
            const auto timing = summarize (blockTimes);

            // If the audio thread ever did (or waited on) the loading, every one of the 3 A1 loads
            // would leave a block about as slow as the load itself. Counting those catches that,
            // while tolerating a stray OS scheduling hiccup, which a test thread (unlike the real
            // audio thread) isn't protected from.
            const auto slowThreshold = 0.5 * shortestA1LoadMicros.load();
            const auto slowBlocks = std::count_if (blockTimes.begin(), blockTimes.end(),
                                                   [&] (double t) { return t > slowThreshold; });

            expectGreaterThan (switches, 0);
            expectLessThan ((int) slowBlocks, 3);
            logMessage ("  -> " + juce::String (loadsDone.load()) + " loads on another thread (an A1 load takes at least "
                        + micros (shortestA1LoadMicros.load()) + "), " + juce::String (switches) + " switches picked up, over "
                        + juce::String ((int) blockTimes.size()) + " audio blocks: " + juce::String ((int) slowBlocks)
                        + " blocks as slow as half a load; worst block " + micros (timing.worst) + " ("
                        + percentOfDeadline (timing.worst) + " of the 2.67 ms deadline), mean " + micros (timing.mean));
        }

        beginTest ("CPU: one vs. three always-running slots, 128-sample buffers");
        {
            const auto longInput = guitarDI ((int) (10.0 * fs));
            const auto numBlocks = longInput.size() / blockSize;
            bool threeA1Fits = false;

            for (const auto& file : { a1, exampleModel ("A2.nam") })
            {
                for (int slots : { 1, 3 })
                {
                    std::vector<std::unique_ptr<ampsim::NamAmp>> amps;
                    for (int s = 0; s < slots; ++s)
                    {
                        amps.push_back (std::make_unique<ampsim::NamAmp>());
                        amps.back()->loadModel (file, false);
                        amps.back()->prepare (fs, blockSize);
                    }

                    juce::AudioBuffer<float> buffer (1, blockSize);
                    const ampsim::BlockContext context;
                    std::vector<double> blockTimes;

                    for (size_t b = 0; b < numBlocks; ++b)
                    {
                        const auto t0 = std::chrono::steady_clock::now();

                        for (auto& amp : amps)
                        {
                            buffer.copyFrom (0, 0, longInput.data() + b * blockSize, blockSize);
                            amp->process (juce::dsp::AudioBlock<float> (buffer), context);
                        }

                        blockTimes.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
                    }

                    const auto t = summarize (blockTimes);
                    if (file == a1 && slots == 3)
                        threeA1Fits = t.mean < 0.5 * deadlineMicros;

                    logMessage ("  -> " + file.getFileName() + " x" + juce::String (slots) + ": mean " + micros (t.mean) + " ("
                                + percentOfDeadline (t.mean) + " of deadline), p99 " + micros (t.p99) + ", worst "
                                + micros (t.worst) + " over " + juce::String ((int) numBlocks) + " blocks (10 s)");
                }
            }

            expect (threeA1Fits, "three A1 standard slots must fit in half the deadline (BUILD_PLAN CPU budget)");
        }

        beginTest ("bad files are rejected with a reason, not loaded");
        {
            const auto notJson = tempDir().getChildFile ("garbage.nam");
            notJson.replaceWithText ("{ this is not json");

            // A copy of wavenet.nam that claims a 44.1 kHz training rate.
            auto json = juce::JSON::parse (exampleModel ("wavenet.nam"));
            json.getDynamicObject()->setProperty ("sample_rate", 44100);
            const auto wrongRate = tempDir().getChildFile ("trained_at_44k.nam");
            wrongRate.replaceWithText (juce::JSON::toString (json));

            ampsim::NamAmp amp;
            const auto missing = amp.loadModel (juce::File ("/nonexistent/amp.nam"));
            const auto garbage = amp.loadModel (notJson);
            const auto rate = amp.loadModel (wrongRate);

            for (const auto& r : { missing, garbage, rate })
            {
                expect (! r.ok);
                expect (r.message.isNotEmpty());
                logMessage ("  -> \"" + r.message + "\"");
            }

            expect (rate.message.contains ("44100"));
            amp.prepare (fs, blockSize);
            expect (! amp.hasModel(), "nothing bad should have reached the audio thread");
        }

        beginTest ("input calibration: a capture's recorded input level sets the gain into the model");
        {
            const auto calibrationDI = guitarDI ((int) (2.0 * fs));
            const auto run = [&] (const juce::File& model, const ampsim::NamAmp::Calibration& calibration, float inputGain,
                                  ampsim::NamAmp::LoadResult& result)
            {
                ampsim::NamAmp slot;
                result = slot.loadModel (model, false, calibration);
                slot.prepare (fs, blockSize);
                auto scaled = calibrationDI;
                for (auto& v : scaled)
                    v *= inputGain;
                return runAmp (slot, scaled);
            };

            // lstm.nam was captured with +18.3 dBu reaching 0 dBFS. An interface whose 0 dBFS is
            // +12 dBu delivers the same guitar 6.3 dB hotter, so the model's input comes down 6.3 dB.
            ampsim::NamAmp::LoadResult calibrated, plain;
            const auto out = run (lstm, { true, 12.0 }, 1.0f, calibrated);
            const auto calibrationGain = juce::Decibels::decibelsToGain ((float) calibrated.calibrationDb, -1000.0f);
            const auto reference = run (lstm, {}, calibrationGain, plain); // uncalibrated, fed the scaled DI by hand
            expect (calibrated.hasInputLevel);
            expectWithinAbsoluteError (calibrated.captureInputDbu, 18.3, 1.0e-5); // NAM core keeps the level as a float
            expectWithinAbsoluteError (calibrated.calibrationDb, -6.3, 1.0e-5);

            // Identical once the model's start-up state has washed out. An LSTM keeps its hidden state
            // through NAM core's Reset() (only the half second of silence that prewarms it follows), so
            // the loudness render at load leaves a trace that depends on what was rendered: here the
            // calibrated and plain slots rendered the reference DI at different levels.
            size_t lastDifference = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs (out[i] - reference[i]) > 0.0f)
                    lastDifference = i;
            const auto startupDifference = maxAbsDifference (out, reference);
            expectLessThan (startupDifference, 1.0e-6);
            expectLessThan ((int) lastDifference, 500);

            // A capture that says it was made at +6 dBu: the input goes up 6 dB.
            auto json = juce::JSON::parse (exampleModel ("wavenet_a1_standard.nam"));
            if (auto* metadata = json.getProperty ("metadata", {}).getDynamicObject())
                metadata->setProperty ("input_level_dbu", 6.0);
            else
            {
                auto* fresh = new juce::DynamicObject();
                fresh->setProperty ("input_level_dbu", 6.0);
                json.getDynamicObject()->setProperty ("metadata", juce::var (fresh));
            }
            const auto lowLevel = tempDir().getChildFile ("captured_at_6dbu.nam");
            lowLevel.replaceWithText (juce::JSON::toString (json));
            ampsim::NamAmp::LoadResult up, upPlain;
            const auto upOut = run (lowLevel, { true, 12.0 }, 1.0f, up);
            const auto upReference = run (lowLevel, {}, juce::Decibels::decibelsToGain (6.0f, -1000.0f), upPlain);
            expectWithinAbsoluteError (up.calibrationDb, 6.0, 1.0e-9);
            expectEquals (maxAbsDifference (upOut, upReference), 0.0);

            // No recorded level, or calibration off: nothing changes.
            ampsim::NamAmp::LoadResult noLevel, off, offPlain, noLevelPlain;
            const auto noLevelOut = run (a1, { true, 12.0 }, 1.0f, noLevel);
            const auto offOut = run (lstm, { false, 12.0 }, 1.0f, off);
            expect (! noLevel.hasInputLevel && noLevel.calibrationDb == 0.0 && off.calibrationDb == 0.0);
            expectEquals (maxAbsDifference (noLevelOut, run (a1, {}, 1.0f, noLevelPlain)), 0.0);
            expectEquals (maxAbsDifference (offOut, run (lstm, {}, 1.0f, offPlain)), 0.0);

            // Normalization measures the calibrated input, so the slot still lands at -18 LUFS.
            const auto probe = ampsim::referenceGuitarDI ((int) (ampsim::NamAmp::loudnessProbeSeconds * fs));
            juce::StringArray loudness, crest;
            for (auto interfaceDbu : { 0.0, 12.0, 24.0 })
            {
                ampsim::NamAmp slot;
                const auto r = slot.loadModel (lstm, true, { true, interfaceDbu });
                slot.prepare (fs, blockSize);
                const auto rendered = runAmp (slot, probe);
                const auto lufs = ampsim::loudness::integratedMono (rendered.data(), (int) rendered.size(), fs);
                expectWithinAbsoluteError (lufs, ampsim::NamAmp::targetLoudnessLufs, 0.02);
                const auto peak = std::abs (*std::max_element (rendered.begin(), rendered.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }));
                loudness.add ("+" + juce::String (interfaceDbu, 0) + " dBu (" + juce::String (r.calibrationDb, 1) + " dB in): "
                              + juce::String (lufs, 2) + " LUFS");
                crest.add (juce::String (toDb (peak / rms (rendered)), 1) + " dB");
            }

            logMessage ("  -> lstm.nam (captured at +" + juce::String (calibrated.captureInputDbu, 1) + " dBu) on a +12 dBu interface: input "
                        + juce::String (calibrated.calibrationDb, 1) + " dB, identical to the uncalibrated model fed the DI 6.3 dB down from sample "
                        + juce::String ((int) lastDifference + 1) + " on (before that, NAM core's LSTM start-up state differs by at most "
                        + juce::String (toDb (startupDifference), 0) + " dBFS)");
            logMessage ("  -> a capture made at +6 dBu: input +" + juce::String (up.calibrationDb, 1) + " dB, also identical; a capture "
                        "without a level, or calibration off: unchanged, bit for bit");
            logMessage ("  -> normalized on the calibrated input: " + loudness.joinIntoString ("; "));
            logMessage ("  -> the hotter the interface's full scale, the harder the same guitar drives the capture: output crest factor "
                        + crest.joinIntoString (", ") + " at +0, +12, +24 dBu");
        }
    }
};

NamAmpTests namAmpTests;
} // namespace

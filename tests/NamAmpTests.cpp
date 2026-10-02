#include "TestHelpers.h"
#include "dsp/NamAmp.h"

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

        beginTest ("normalizes each model's loudness to -18 dB from its metadata");
        {
            const auto file = exampleModel ("wavenet.nam"); // metadata loudness: -20.02 dB
            ampsim::NamAmp raw, normalized;
            expect (raw.loadModel (file, false).ok);
            const auto result = normalized.loadModel (file, true);
            raw.prepare (fs, blockSize);
            normalized.prepare (fs, blockSize);

            const auto rawOut = runAmp (raw, input);
            const auto normOut = runAmp (normalized, input);
            const auto gain = std::pow (10.0, result.normalizationDb / 20.0);

            double worst = 0.0;
            for (size_t i = 0; i < rawOut.size(); ++i)
                worst = std::max (worst, std::abs (normOut[i] - rawOut[i] * gain));

            expectWithinAbsoluteError (result.normalizationDb, -18.0 - (-20.020729064941406), 1.0e-4);
            expectLessThan (worst, 1.0e-5);
            logMessage ("  -> metadata loudness -20.02 dB, so gain " + juce::String (result.normalizationDb, 3)
                        + " dB; measured output change " + juce::String (toDb (rms (normOut) / rms (rawOut)), 3) + " dB");
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
    }
};

NamAmpTests namAmpTests;
} // namespace

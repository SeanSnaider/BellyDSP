#include "TestHelpers.h"
#include "dsp/CabIR.h"
#include "dsp/Loudness.h"
#include "dsp/NamAmp.h"
#include "dsp/ReferenceSignals.h"

namespace
{
using namespace testing;

std::vector<float> runCab (ampsim::CabIR& cab, const std::vector<float>& input, int bufferSize, std::vector<float>* right = nullptr)
{
    const ampsim::BlockContext context;
    auto out = runInBlocks (input, bufferSize, [&] (juce::dsp::AudioBlock<float>& block, size_t)
    {
        cab.process (block, context);
    });

    if (right != nullptr)
        *right = out.right;

    return out.left;
}

class CabIRTests final : public juce::UnitTest
{
public:
    CabIRTests() : juce::UnitTest ("Cab IR", "ampsim") {}

    void runTest() override
    {
        const auto ir = syntheticCabIR (4096);
        const auto scaled = [] (std::vector<double> h, double gain) { for (auto& v : h) v *= gain; return h; };

        beginTest ("matches brute-force direct convolution (BUILD_PLAN target: error below -100 dB)");
        {
            ampsim::CabIR cab;
            const auto loaded = cab.loadSamples (toBuffer (ir), fs, "synthetic");
            expect (loaded.ok);
            cab.prepare (fs, blockSize);

            const auto input = whiteNoise ((int) fs * 2, 0.5f, 1);
            std::vector<float> right;
            const auto out = runCab (cab, input, blockSize, &right);
            const auto expected = directConvolution (input, scaled (ir, loaded.gain));
            const auto error = relativeErrorDb (out, expected);

            expectLessThan (error, -100.0);
            expectEquals (maxAbsDifference (out, right), 0.0, "one mic: left and right must be identical");
            logMessage ("  -> 2 s of noise through a 4096-tap IR: error vs. direct convolution " + dB (error)
                        + " (limit -100 dB); left == right");
        }

        beginTest ("zero latency: an impulse at sample 0 produces the IR starting at sample 0");
        {
            ampsim::CabIR cab;
            const auto loaded = cab.loadSamples (toBuffer (ir), fs, "synthetic");
            cab.prepare (fs, blockSize);

            std::vector<float> impulse (8192, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runCab (cab, impulse, blockSize);
            const auto expected = scaled (ir, loaded.gain);

            size_t firstNonZero = 0;
            while (firstNonZero < out.size() && std::abs (out[firstNonZero]) < 1.0e-9f)
                ++firstNonZero;

            expectEquals ((int) firstNonZero, 0);
            expectWithinAbsoluteError ((double) out[0], expected[0], 1.0e-6);
            expectLessThan (relativeErrorDb (std::vector<float> (out.begin(), out.begin() + 4096), expected), -100.0);
            logMessage ("  -> first output sample index " + juce::String ((int) firstNonZero) + " (0 = no latency); h[0] = "
                        + juce::String (out[0], 6) + " vs expected " + juce::String (expected[0], 6));
        }

        beginTest ("IRs are loudness-matched: white noise keeps its loudness through any cab, and guitar stays close");
        {
            // Three deliberately different cabs: the stock synthetic one, a dark one, and a bright one.
            const std::vector<std::pair<juce::String, std::vector<double>>> cabs {
                { "stock",  syntheticCabIR (4096) },
                { "dark",   syntheticCabIR (4096, 0.0, 2500.0) },
                { "bright", syntheticCabIR (4096, 8.0, 8000.0) },
            };

            // A distorted guitar: the reference DI through the A1 capture, as the cab would really get it.
            ampsim::NamAmp amp;
            amp.loadModel (exampleModel ("wavenet_a1_standard.nam"), true);
            amp.prepare (fs, blockSize);
            const ampsim::BlockContext context;
            const auto take = ampsim::referenceGuitarDI ((int) (6.0 * fs), fs, 7);
            const auto distorted = runInBlocks (take, blockSize, [&] (juce::dsp::AudioBlock<float>& b, size_t)
                                                { amp.process (b.getSubsetChannelBlock (0, 1), context); }).left;

            const auto noise = whiteNoise ((int) (4.0 * fs), 0.3f, 99); // a different seed than the loader's
            const auto lufs = [] (const std::vector<float>& x) { return ampsim::loudness::integratedMono (x.data(), (int) x.size(), fs); };
            const auto noiseLoudness = lufs (noise);
            std::vector<double> distortedLevels, cleanLevels;
            double worstNoiseChange = 0.0;

            for (const auto& [name, h] : cabs)
            {
                const auto loaded = ampsim::CabIR().loadSamples (toBuffer (h), fs, name);
                const auto matched = scaled (h, loaded.gain);
                const std::vector<float> hm (matched.begin(), matched.end());

                worstNoiseChange = std::max (worstNoiseChange, std::abs (lufs (ampsim::loudness::fftConvolve (noise, hm.data(), (int) hm.size())) - noiseLoudness));
                distortedLevels.push_back (lufs (ampsim::loudness::fftConvolve (distorted, hm.data(), (int) hm.size())));
                cleanLevels.push_back (lufs (ampsim::loudness::fftConvolve (take, hm.data(), (int) hm.size())));
            }

            const auto spread = [] (const std::vector<double>& v) { return *std::max_element (v.begin(), v.end()) - *std::min_element (v.begin(), v.end()); };

            // No single reference gets every tone within 1 LU through cabs this different (see the
            // study test); 2.5 LU is the bound white noise achieves on the study's signals.
            expectLessThan (worstNoiseChange, 0.05);
            expectLessThan (spread (distortedLevels), 2.5);
            expectLessThan (spread (cleanLevels), 2.5);
            logMessage ("  -> white noise changes loudness by at most " + juce::String (worstNoiseChange, 3) + " LU through any of the three cabs");
            logMessage ("  -> across stock, dark, and bright cabs: distorted guitar spans " + juce::String (spread (distortedLevels), 2)
                        + " LU, clean guitar " + juce::String (spread (cleanLevels), 2) + " LU");
        }

        beginTest ("any buffer size from 1 to 512 gives the same result");
        {
            const auto input = whiteNoise ((int) fs, 0.5f, 2);
            const auto gain = ampsim::CabIR().loadSamples (toBuffer (ir), fs, "probe").gain; // the same IR always gets the same gain
            const auto expected = directConvolution (input, scaled (ir, gain));
            juce::StringArray results;

            for (auto size : { 1, 7, 64, 128, 512 })
            {
                ampsim::CabIR cab;
                cab.loadSamples (toBuffer (ir), fs, "synthetic");
                cab.prepare (fs, 512);
                const auto error = relativeErrorDb (runCab (cab, input, size), expected);
                expectLessThan (error, -100.0, "buffer size " + juce::String (size));
                results.add (juce::String (size) + ": " + dB (error));
            }

            logMessage ("  -> error vs. direct convolution by buffer size: " + results.joinIntoString (", "));
        }

        beginTest ("IRs longer than 1 s are cut to 1 s and faded, so the cut doesn't click");
        {
            ampsim::CabIR cab;
            const auto longIR = whiteNoise ((int) (2.0 * fs), 0.5f, 3);
            const auto result = cab.loadSamples (toBuffer (longIR), fs, "two_seconds");
            expect (result.ok);
            expectEquals (result.numSamples, (int) fs);
            expect (result.message.contains ("cut to 1 s"));
            cab.prepare (fs, blockSize);

            std::vector<float> impulse ((size_t) (1.5 * fs), 0.0f);
            impulse[0] = 1.0f;
            const auto out = runCab (cab, impulse, blockSize);

            const auto before = rms (out.data() + 40000, 2000);      // well before the fade
            const auto lastMs = rms (out.data() + 47952, 48);        // the final 1 ms of the fade
            const auto after = rms (out.data() + 48000, 24000);      // past the end of the IR
            expectLessThan (lastMs, before * 0.15);
            expectLessThan (after, 1.0e-6); // FFT round-off only
            logMessage ("  -> 2 s IR loaded as " + juce::String (result.numSamples) + " samples (\"" + result.message
                        + "\"); last 1 ms is " + dB (toDb (lastMs / before)) + " relative to the body; output past 1 s is "
                        + dB (toDb (after)) + " (silence)");
        }

        beginTest ("a stereo IR file uses its left channel");
        {
            const auto left = syntheticCabIR (2048);
            auto right = whiteNoise (2048, 0.3f, 4);
            juce::AudioBuffer<float> stereoIR (2, 2048);
            for (int i = 0; i < 2048; ++i)
            {
                stereoIR.setSample (0, i, (float) left[(size_t) i]);
                stereoIR.setSample (1, i, right[(size_t) i]);
            }

            const auto file = tempDir().getChildFile ("stereo_ir.wav");
            expect (writeWav (file, stereoIR));

            ampsim::CabIR cab;
            const auto result = cab.loadFile (file);
            expect (result.ok, result.message);
            cab.prepare (fs, blockSize);

            std::vector<float> impulse (4096, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runCab (cab, impulse, blockSize);
            const auto expected = scaled (left, result.gain);
            const auto error = relativeErrorDb (std::vector<float> (out.begin(), out.begin() + 2048), expected);
            expectLessThan (error, -100.0);
            logMessage ("  -> measured IR vs. the file's left channel: " + dB (error));
        }

        beginTest ("bad files are rejected with a reason, not loaded");
        {
            ampsim::CabIR cab;
            const auto notAudio = tempDir().getChildFile ("not_audio.wav");
            notAudio.replaceWithText ("this is not a wav file");

            const auto missing = cab.loadFile (juce::File ("/nonexistent/cab.wav"));
            const auto garbage = cab.loadFile (notAudio);
            juce::AudioBuffer<float> silence (1, 1000);
            silence.clear(); // JUCE doesn't zero new buffers
            const auto silent = cab.loadSamples (std::move (silence), fs, "silent");

            for (const auto& r : { missing, garbage, silent })
            {
                expect (! r.ok);
                expect (r.message.isNotEmpty());
            }

            logMessage ("  -> \"" + missing.message + "\" / \"" + garbage.message + "\" / \"" + silent.message + "\"");
        }

        beginTest ("loading a new IR while audio runs crossfades to it, with no click");
        {
            ampsim::CabIR cab;
            cab.loadSamples (toBuffer (ir), fs, "first");
            cab.prepare (fs, blockSize);

            const auto secondRaw = syntheticCabIR (1024); // shorter, so a different response
            const auto secondGain = ampsim::CabIR().loadSamples (toBuffer (secondRaw), fs, "probe").gain;
            const auto secondIR = scaled (secondRaw, secondGain);
            const auto input = sine (220.0, 0.5, (int) (2.0 * fs));
            const ampsim::BlockContext context;
            const size_t switchAt = (size_t) (0.5 * fs) / blockSize * blockSize;

            auto out = runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
            {
                if (start == switchAt)
                    cab.loadSamples (toBuffer (secondRaw), fs, "second"); // normalized the same way as the probe

                cab.process (block, context);

                // Give JUCE's loader thread time to build the new engine, as real time would.
                if (start >= switchAt && start < switchAt + 20 * blockSize)
                    juce::Thread::sleep (2);
            }).left;

            const auto expectedAfter = directConvolution (input, secondIR);
            const auto tailStart = (size_t) (1.5 * fs);
            const auto settledError = relativeErrorDb (std::vector<float> (out.begin() + (long) tailStart, out.end()),
                                                       std::vector<double> (expectedAfter.begin() + (long) tailStart, expectedAfter.end()));

            const auto steadyStep = std::max (maxStep (out, 0, switchAt), maxStep (out, tailStart));
            const auto switchStep = maxStep (out, switchAt, switchAt + (size_t) (0.3 * fs));

            expectLessThan (settledError, -100.0);
            expectLessThan (switchStep, steadyStep * 1.5);
            logMessage ("  -> after the swap the output matches the new IR to " + dB (settledError)
                        + "; largest sample-to-sample step during the swap " + juce::String (switchStep, 5)
                        + " vs. " + juce::String (steadyStep, 5) + " in steady state (no click)");
        }
    }
};

CabIRTests cabIRTests;
} // namespace

// ampsim_capture's play-and-record engine (tools/capture), run against simulated devices: the recording
// must line up with what was played sample for sample, have exactly the input's length, survive the WAV
// round trip, and the latency measurement must find the simulated round trip. No hardware needed.

#include "AllocationTracking.h"
#include "CaptureEngine.h"
#include "TestHelpers.h"

namespace
{
using namespace testing;

/// A stand-in for NAM's v3 input file, laid out the same way where it matters (silence from 9 to 10.5 s,
/// single-sample blips at 10.5 and 11.5 s) but shorter: noise everywhere else, at -12 dBFS RMS.
std::vector<float> fakeInputFile (int length)
{
    auto x = whiteNoise (length, 0.25f, 7);
    for (int i = 432'000; i < 576'000 && i < length; ++i)
        x[(size_t) i] = 0.0f;
    for (const auto blip : capture::v3::blips)
        if (blip < length)
            x[(size_t) blip] = 0.99f;
    return x;
}

/// The simplest gear: a delay of `delay` samples and a gain, through a ring buffer, as the device would
/// return it.
struct DelayGainDevice
{
    std::vector<float> line;
    size_t at = 0;
    float gain;
    DelayGainDevice (int delay, float g) : line ((size_t) delay + 1, 0.0f), gain (g) {}
    void operator() (const float* played, float* returned, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            line[at] = gain * played[i];
            at = (at + 1) % line.size();
            returned[i] = line[at];
        }
    }
};

class CaptureTests final : public juce::UnitTest
{
public:
    CaptureTests() : juce::UnitTest ("Capture tool (ampsim_capture)", "ampsim") {}

    void runTest() override
    {
        beginTest ("against a simulated delay-and-gain device, the recording is the played file shifted by the round trip, sample for sample, and exactly as long");
        {
            const auto stimulus = fakeInputFile (700'000);
            const auto outputGain = juce::Decibels::decibelsToGain (-12.0f);
            juce::StringArray rows;
            for (const auto block : { 64, 128, 256, 333 }) // 333: the last callback is a partial one
            {
                for (const auto delay : { 0, 37, 300, 1999 })
                {
                    capture::Engine engine;
                    engine.prepare (stimulus, outputGain);
                    DelayGainDevice device (delay, 0.5f);
                    const auto callbacks = capture::simulate (engine, block, std::ref (device));
                    const auto& rec = engine.getRecording();
                    expectEquals ((int) rec.size(), (int) stimulus.size());

                    // The simulated round trip is the device's delay plus one buffer (its answer to this
                    // callback's output arrives with the next callback's input, as with real hardware).
                    const auto roundTrip = delay + block;
                    int mismatches = 0;
                    for (size_t n = 0; n < rec.size(); ++n)
                    {
                        const auto expected = n < (size_t) roundTrip ? 0.0f : 0.5f * (outputGain * stimulus[n - (size_t) roundTrip]);
                        mismatches += juce::exactlyEqual (rec[n], expected) ? 0 : 1; // bit-exact: the same float operations
                    }
                    expectEquals (mismatches, 0);
                    const auto measured = capture::measureLatency (rec);
                    expectEquals (measured, roundTrip);
                    expectEquals (callbacks, ((int) stimulus.size() + block - 1) / block);
                    if (delay == 300)
                        rows.add ("buffer " + juce::String (block) + ": round trip " + juce::String (roundTrip) + ", measured " + juce::String (measured)
                                  + ", " + juce::String (callbacks) + " callbacks, " + juce::String (mismatches) + " samples differ");
                }
            }
            logMessage ("  -> 16 runs (buffers 64, 128, 256, 333; delays 0, 37, 300, 1999): every recording " + juce::String ((int) stimulus.size())
                        + " samples, bit-exact against the shifted input, latency measured exactly. With 300 samples of delay:");
            for (const auto& r : rows)
                logMessage ("       " + r);
        }

        beginTest ("the level check plays only the first 17 s, then silence; nothing is allocated, freed, or locked in the callback");
        {
            const auto stimulus = fakeInputFile (capture::v3::levelCheckLength + 100'000);
            capture::Engine engine;
            engine.prepare (stimulus, 1.0f, capture::v3::levelCheckLength);
            expectEquals (engine.getLength(), capture::v3::levelCheckLength);
            std::vector<float> in ((size_t) blockSize, 0.25f), out ((size_t) blockSize, 1.0f);
            rtcheck::Counts counts;
            int callbacks = 0;
            float afterEnd = 0.0f;
            while (callbacks < capture::v3::levelCheckLength / blockSize + 50)
            {
                const auto wasDone = engine.isDone();
                rtcheck::begin();
                engine.process (in.data(), out.data(), blockSize);
                counts += rtcheck::end();
                if (wasDone) // every callback after the last played sample must be silent
                    for (const auto x : out)
                        afterEnd = std::max (afterEnd, std::abs (x));
                ++callbacks;
            }
            expect (engine.isDone());
            expectEquals (afterEnd, 0.0f);
            expectEquals ((int) engine.getRecording().size(), capture::v3::levelCheckLength);
            expectEquals (engine.getClippedSamples(), 0);
            expectEquals (counts.allocations, 0L);
            expectEquals (counts.frees, 0L);
            expectEquals (counts.blockingLocks, 0L);
            logMessage ("  -> " + juce::String (engine.getLength()) + " samples (17.0 s) played and recorded, silence after; " + juce::String (callbacks)
                        + " callbacks: " + juce::String (counts.allocations) + " allocations, " + juce::String (counts.frees) + " frees, "
                        + juce::String (counts.blockingLocks) + " blocking locks");
        }

        beginTest ("clipping, levels, and no answer: counted, measured, and reported as not detected");
        {
            const auto stimulus = fakeInputFile (600'000);
            capture::Engine engine;
            engine.prepare (stimulus, 1.0f);
            DelayGainDevice hot (100, 8.0f); // 18 dB too hot: the noise clips (a real converter would)
            capture::simulate (engine, 128, [&hot] (const float* p, float* r, int n)
            {
                hot (p, r, n);
                for (int i = 0; i < n; ++i)
                    r[i] = juce::jlimit (-1.0f, 1.0f, r[i]);
            });
            const auto levels = capture::measure (engine.getRecording().data(), (int) engine.getRecording().size());
            expect (engine.getClippedSamples() > 1000 && levels.clippedSamples == engine.getClippedSamples());
            expectWithinAbsoluteError (levels.peakDb, 0.0f, 0.01f);

            capture::Engine quiet;
            quiet.prepare (stimulus, 1.0f);
            capture::simulate (quiet, 128, [] (const float*, float* r, int n) { std::fill (r, r + n, 0.0f); });
            expectEquals (capture::measureLatency (quiet.getRecording()), -1);
            expectEquals (capture::measureLatency (std::vector<float> (1000, 0.0f)), -1); // too short to have the blips
            logMessage ("  -> 18 dB too hot: " + juce::String (levels.clippedSamples) + " clipped samples counted, peak " + juce::String (levels.peakDb, 2)
                        + " dBFS; nothing returned: latency not detected (-1)");
        }

        beginTest ("the recording's WAV is 24-bit, 48 kHz, the exact length, within one 24-bit step; anything but 48 kHz mono is refused");
        {
            const auto stimulus = fakeInputFile (200'000);
            const auto file = tempDir().getChildFile ("capture_roundtrip.wav");
            juce::String error;
            expect (capture::writeRecording (file, stimulus, error), error);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
            expect (reader != nullptr && reader->bitsPerSample == 24 && reader->sampleRate == 48000.0 && reader->numChannels == 1);
            const auto back = capture::readStimulus (file, error);
            expectEquals ((int) back.size(), (int) stimulus.size());
            const auto err = maxAbsDifference (back, stimulus);
            expectLessThan (err, 1.0 / 8388608.0 + 1.0e-9); // one 24-bit step (2^-23)
            reader.reset();

            const auto wrongRate = tempDir().getChildFile ("capture_44k.wav");
            writeWav (wrongRate, stimulus, 44100.0);
            expect (capture::readStimulus (wrongRate, error).empty() && error.contains ("48 kHz"), error);
            juce::String stereoError;
            const auto stereo = tempDir().getChildFile ("capture_stereo.wav");
            juce::AudioBuffer<float> two (2, 1000);
            two.clear();
            writeWav (stereo, two);
            expect (capture::readStimulus (stereo, stereoError).empty() && stereoError.contains ("mono"), stereoError);
            file.deleteFile();
            logMessage ("  -> written and read back: " + juce::String ((int) back.size()) + " samples, largest error " + juce::String (err * 8388608.0, 2)
                        + " of a 24-bit step; refused: \"" + error + "\", \"" + stereoError + "\"");
        }

        beginTest ("NAM's real input file (when tools/fetch_nam_input.sh has fetched it): the level check's 17 s and the latency on it");
        {
            const auto real = juce::File (AMPSIM_SOURCE_DIR).getChildFile ("build-deps/nam/input.wav");
            if (! real.existsAsFile())
            {
                logMessage ("  -> not fetched in this checkout (build-deps/nam/input.wav): skipped");
                expect (true);
                return;
            }
            juce::String error;
            const auto stimulus = capture::readStimulus (real, error);
            expectEquals ((int) stimulus.size(), capture::v3::length);
            for (const auto blip : capture::v3::blips)
                expectWithinAbsoluteError (stimulus[(size_t) blip], 0.99f, 1.0e-6f);
            capture::Engine engine;
            engine.prepare (stimulus, juce::Decibels::decibelsToGain (-12.0f), capture::v3::levelCheckLength);
            DelayGainDevice device (300, 0.5f);
            capture::simulate (engine, 256, std::ref (device));
            const auto latency = capture::measureLatency (engine.getRecording());
            expectEquals (latency, 300 + 256);
            const auto levels = capture::measure (engine.getRecording().data(), engine.getLength());
            const auto played = capture::measure (stimulus.data(), capture::v3::levelCheckLength), whole = capture::measure (stimulus.data(), (int) stimulus.size());
            logMessage ("  -> " + real.getFileName() + ": " + juce::String ((int) stimulus.size()) + " samples, blips of 0.99 at samples 504000 and 552000; "
                        "through 300 samples of delay at -6 dB plus a 256-sample buffer: latency " + juce::String (latency) + ", return peak "
                        + juce::String (levels.peakDb, 1) + " dBFS; the first 17 s peak at " + juce::String (played.peakDb, 1) + " dBFS, the whole file at "
                        + juce::String (whole.peakDb, 1) + " dBFS");
        }
    }
};

CaptureTests captureTests;
} // namespace

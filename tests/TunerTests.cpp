// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/PitchDetector.h"
#include "dsp/SpscRing.h"
#include "dsp/TunerAnalysis.h"
#include "dsp/TunerThread.h"

#include <atomic>
#include <chrono>
#include <complex>
#include <numeric>
#include <thread>

namespace
{
using namespace testing;
using ampsim::PitchDetector;
using ampsim::SpscRing;
using ampsim::TunerAnalysis;
using ampsim::TunerReading;

constexpr double twoPi = juce::MathConstants<double>::twoPi;

double cents (double measured, double reference) { return 1200.0 * std::log2 (measured / reference); }

int nearestNote (double hz, double a4 = 440.0) { return juce::roundToInt (12.0 * std::log2 (hz / a4) + 69.0); }

/// The shown note is right if the pitch is within 50 cents of it plus the 10 cent hysteresis (a pitch at a
/// quarter tone may be shown as either neighbour, whichever it arrived as).
bool noteIsRight (int shown, double hz, double a4 = 440.0)
{
    return std::abs (12.0 * std::log2 (hz / a4) + 69.0 - shown) <= 0.5 + TunerAnalysis::Settings {}.hysteresisCents / 100.0 + 1.0e-9;
}

juce::String str (double value, int decimals) { return juce::String (value, decimals); }

double median (std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    return v.size() % 2 == 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

double percentile (std::vector<double> v, double p)
{
    std::sort (v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min (v.size() - 1, (size_t) (p / 100.0 * (double) v.size()))];
}

// ---- Test tones: prototypes/pitch_detection.py's, in C++ --------------------------------------

/// Adds amplitude sin(2 pi f t + phase) exp(-t / decaySeconds) to x from sample `start` on (no decay for
/// decaySeconds <= 0), by turning a complex phasor one sample at a time.
void addPartial (std::vector<double>& x, size_t start, double frequency, double amplitude, double phase, double decaySeconds)
{
    const auto damping = decaySeconds > 0.0 ? std::exp (-1.0 / (decaySeconds * fs)) : 1.0;
    const auto step = std::polar (damping, twoPi * frequency / fs);
    auto z = std::polar (amplitude, phase);

    for (size_t i = start; i < x.size(); ++i)
    {
        x[i] += z.imag();
        z *= step;
    }
}

void normalisePeak (std::vector<double>& x, double peak)
{
    double largest = 1.0e-30;
    for (auto v : x)
        largest = std::max (largest, std::abs (v));
    for (auto& v : x)
        v *= peak / largest;
}

struct Tone
{
    std::vector<double> x;
    double pitch = 0.0; // the first partial: what a tuner should show
};

/// Partial n at n f0 sqrt(1 + B n^2) with amplitude 1/n (the 2nd raised by secondBoostDb), a random phase,
/// and decay time constant decayScale / (0.6 + 0.15 n) seconds (decayScale 0: sustained). Peak 0.5.
Tone stiffString (double f0, double B, double seconds, juce::int64 seed, double secondBoostDb = 0.0, double decayScale = 1.0)
{
    juce::Random random (seed);
    Tone t;
    t.x.assign ((size_t) (seconds * fs), 0.0);

    for (int n = 1; n <= 30; ++n)
    {
        const auto fn = n * f0 * std::sqrt (1.0 + B * n * n);
        if (fn > 0.45 * fs)
            break;
        const auto amplitude = (n == 2 ? std::pow (10.0, secondBoostDb / 20.0) : 1.0) / n;
        addPartial (t.x, 0, fn, amplitude, twoPi * random.nextDouble(), decayScale > 0.0 ? decayScale / (0.6 + 0.15 * n) : 0.0);
    }

    normalisePeak (t.x, 0.5);
    t.pitch = f0 * std::sqrt (1.0 + B);
    return t;
}

Tone sawtooth (double f0, double seconds)
{
    Tone t;
    t.x.assign ((size_t) (seconds * fs), 0.0);
    for (int n = 1; n * f0 < 0.45 * fs; ++n)
        addPartial (t.x, 0, n * f0, 0.3 / n, 0.0, 0.0);
    t.pitch = f0;
    return t;
}

Tone sineTone (double f0, double seconds)
{
    Tone t;
    t.x.assign ((size_t) (seconds * fs), 0.0);
    addPartial (t.x, 0, f0, 0.5, 0.0, 0.0);
    t.pitch = f0;
    return t;
}

std::vector<double> gaussian (size_t n, double rms, juce::int64 seed)
{
    juce::Random random (seed);
    std::vector<double> out (n);
    for (size_t i = 0; i < n; i += 2) // Box-Muller
    {
        const auto radius = rms * std::sqrt (-2.0 * std::log (std::max (1.0e-300, random.nextDouble())));
        const auto angle = twoPi * random.nextDouble();
        out[i] = radius * std::cos (angle);
        if (i + 1 < n)
            out[i + 1] = radius * std::sin (angle);
    }
    return out;
}

std::vector<float> withNoise (const std::vector<double>& x, double noiseRms, juce::int64 seed)
{
    const auto noise = gaussian (x.size(), noiseRms, seed);
    std::vector<float> out (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        out[i] = (float) (x[i] + noise[i]);
    return out;
}

/// The prototype's pluck: a stiff string (B 1e-4) with a 0.5 ms attack and a 2 ms burst of pick noise.
Tone pluck (double f0, double seconds, juce::int64 seed)
{
    auto t = stiffString (f0, 1.0e-4, seconds, seed);
    const auto burst = gaussian ((size_t) (0.002 * fs), 0.05, seed + 1000);
    for (size_t i = 0; i < t.x.size(); ++i)
    {
        t.x[i] *= 1.0 - std::exp (-(double) i / (0.0005 * fs));
        if (i < burst.size())
            t.x[i] += burst[i] * (1.0 - (double) i / (double) burst.size());
    }
    return t;
}

std::vector<double> concat (std::initializer_list<std::vector<double>> parts)
{
    std::vector<double> out;
    for (const auto& p : parts)
        out.insert (out.end(), p.begin(), p.end());
    return out;
}

std::vector<double> silence (double seconds) { return std::vector<double> ((size_t) (seconds * fs), 0.0); }

// ---- Driving the tuner --------------------------------------------------------------------------

struct Frame
{
    double time = 0.0; // seconds of signal analysed
    TunerReading reading;
    PitchDetector::Estimate coarse;
    double fine = 0.0;
};

/// The hosting path without the thread: the signal goes into an SpscRing in 128-sample audio blocks (what
/// the audio thread does), and every 6 blocks (16 ms, 62.5 updates a second) the ring is drained into the
/// analysis and analysed (what the analysis thread does).
std::vector<Frame> runTuner (const std::vector<float>& x, TunerAnalysis& analysis)
{
    SpscRing<float> ring;
    ring.prepare (65536);
    std::vector<float> scratch (4096);
    std::vector<Frame> frames;
    int blocks = 0;

    for (size_t start = 0; start < x.size(); start += (size_t) blockSize)
    {
        const auto n = (int) std::min ((size_t) blockSize, x.size() - start);
        ring.write (x.data() + start, n);

        if (++blocks % 6 == 0 || start + (size_t) n >= x.size())
        {
            for (int got; (got = ring.read (scratch.data(), (int) scratch.size())) > 0;)
                analysis.push (scratch.data(), got);

            Frame f;
            f.reading = analysis.analyse();
            f.time = (double) analysis.getSamplesPushed() / fs;
            f.coarse = analysis.getCoarse();
            f.fine = analysis.getFine();
            frames.push_back (f);
        }
    }

    return frames;
}

std::unique_ptr<TunerAnalysis> makeTuner (double referenceA4 = 440.0)
{
    auto t = std::make_unique<TunerAnalysis>();
    TunerAnalysis::Settings s;
    s.referenceA4 = referenceA4;
    t->prepare (fs, s);
    return t;
}

int countNoteChanges (const std::vector<Frame>& frames, double from = 0.0)
{
    int changes = 0, last = -1;
    for (const auto& f : frames)
    {
        if (f.time < from || ! f.reading.hasReading)
            continue;
        if (last >= 0 && f.reading.midiNote != last)
            ++changes;
        last = f.reading.midiNote;
    }
    return changes;
}

// ---- Golden fixture -----------------------------------------------------------------------------

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tuner").getChildFile (name);
}

class TunerTests final : public juce::UnitTest
{
public:
    TunerTests() : juce::UnitTest ("Tuner", "ampsim") {}

    void runTest() override
    {
        testRing();
        testGolden();
        testAccuracy();
        testOctaves();
        testHold();
        testSteadyNoise();
        testHysteresisAndReference();
        testLatency();
        testThread();
        testCostAndRealtime();
        plotNeedle();
    }

private:
    void testRing()
    {
        beginTest ("ring buffer: 2 M samples cross between two threads in order, none lost; a full ring drops and counts");
        {
            SpscRing<float> ring;
            ring.prepare (1024);
            constexpr int total = 2'000'000;
            int outOfOrder = 0, received = 0, partialWrites = 0;

            std::thread producer ([&]
            {
                juce::Random random (1);
                std::vector<float> chunk (300);
                int next = 0;
                while (next < total)
                {
                    const auto n = std::min (1 + random.nextInt (299), total - next);
                    for (int i = 0; i < n; ++i)
                        chunk[(size_t) i] = (float) ((next + i) % (1 << 24)); // exact in a float
                    int done = 0;
                    while (done < n) // the test's producer retries; the audio thread never would
                    {
                        const auto w = ring.write (chunk.data() + done, n - done);
                        if (w < n - done)
                            ++partialWrites;
                        done += w;
                        if (done < n)
                            std::this_thread::yield();
                    }
                    next += n;
                }
            });

            std::vector<float> got (257);
            juce::Random random (2);
            while (received < total)
            {
                const auto n = ring.read (got.data(), 1 + random.nextInt (256));
                for (int i = 0; i < n; ++i)
                    if (! juce::exactlyEqual (got[(size_t) i], (float) ((received + i) % (1 << 24))))
                        ++outOfOrder;
                received += n;
                if (n == 0)
                    std::this_thread::yield();
            }
            producer.join();
            const auto droppedWhileRetrying = ring.takeDroppedCount(); // partial writes count what didn't fit

            expectEquals (outOfOrder, 0);
            expectEquals (received, total);

            SpscRing<float> full;
            full.prepare (1000); // rounds up to 1024
            std::vector<float> block (128, 1.0f);
            int accepted = 0;
            for (int i = 0; i < 10; ++i)
                accepted += full.write (block.data(), 128);
            const auto dropped = full.takeDroppedCount();
            expectEquals (full.getCapacity(), 1024);
            expectEquals (accepted, 1024);
            expectEquals ((int) dropped, 1280 - 1024);
            expectEquals ((int) full.takeDroppedCount(), 0);

            logMessage ("  -> " + juce::String (total) + " samples through a 1024-sample ring in random chunks: " + juce::String (received)
                        + " received, " + juce::String (outOfOrder) + " out of order (" + juce::String (partialWrites)
                        + " writes found the ring full and were retried; " + juce::String ((juce::int64) droppedWhileRetrying)
                        + " samples refused along the way)");
            logMessage ("  -> 10 blocks of 128 into an unread 1024-sample ring: " + juce::String (accepted) + " accepted, "
                        + juce::String ((juce::int64) dropped) + " dropped and counted");
        }
    }

    void testGolden()
    {
        beginTest ("golden: the C++ detector and fine stage match prototypes/pitch_detection.py on tests/fixtures/tuner");
        {
            const auto input = readWav (fixture ("input.wav"));
            juce::StringArray lines;
            fixture ("expected.csv").readLines (lines);
            expect (input.getNumSamples() > 0 && lines.size() > 100, "fixture missing: run the prototype with --golden");
            const auto* x = input.getReadPointer (0);

            for (const auto* config : { "tuner", "harmonizer" })
            {
                PitchDetector d;
                d.prepare (fs, juce::String (config) == "tuner" ? PitchDetector::Settings::tuner() : PitchDetector::Settings::harmonizer());
                int pushed = 0, frames = 0, pitched = 0, disagreements = 0;
                double worstCents = 0.0, worstClarity = 0.0;

                for (const auto& line : lines)
                {
                    const auto f = juce::StringArray::fromTokens (line, ",", "");
                    if (f.size() < 8 || f[0] != "coarse" || f[1] != config)
                        continue;
                    const auto end = f[2].getIntValue();
                    d.push (x + pushed, end - pushed);
                    pushed = end;
                    const auto e = d.detect();
                    const auto expectedHz = f[6].getDoubleValue(), expectedClarity = f[7].getDoubleValue();
                    ++frames;
                    if ((e.frequency > 0.0) != (expectedHz > 0.0))
                    {
                        ++disagreements;
                        continue;
                    }
                    if (expectedHz > 0.0)
                    {
                        ++pitched;
                        worstCents = std::max (worstCents, std::abs (cents (e.frequency, expectedHz)));
                    }
                    worstClarity = std::max (worstClarity, std::abs (e.clarity - expectedClarity));
                }

                expectGreaterThan (frames, 250);
                expectEquals (disagreements, 0);
                expectLessThan (worstCents, 1.0e-4);
                expectLessThan (worstClarity, 1.0e-6);
                logMessage ("  -> coarse MPM, " + juce::String (config) + " preset (" + juce::String (d.getSettings().minFrequency, 0) + " Hz floor, W_min "
                            + juce::String (d.getMinWindow()) + " samples, frame " + juce::String (d.getFrameLength()) + "): " + juce::String (frames)
                            + " frames every 10 ms, " + juce::String (pitched) + " with a pitch, " + juce::String (disagreements)
                            + " pitch/no-pitch disagreements; worst frequency difference " + juce::String (worstCents, 9)
                            + " cents, worst clarity difference " + juce::String (worstClarity, 12));
            }

            int fineRows = 0, spanMismatches = 0;
            double worstFine = 0.0;
            for (const auto& line : lines)
            {
                const auto f = juce::StringArray::fromTokens (line, ",", "");
                if (f.size() < 8 || f[0] != "fine")
                    continue;
                const auto end = f[2].getIntValue(), noteStart = f[3].getIntValue(), settle = f[4].getIntValue();
                const auto coarse = f[5].getDoubleValue(), expectedHz = f[6].getDoubleValue();
                const auto span = TunerAnalysis::fineSpan (coarse, end - noteStart, fs);
                if (span.settle != settle)
                    ++spanMismatches;
                const auto length = span.settle + span.window;
                const auto hz = TunerAnalysis::refineFrequency (x + end - length, length, span.settle, coarse, fs);
                worstFine = std::max (worstFine, std::abs (cents (hz, expectedHz)));
                ++fineRows;
            }
            expectEquals (fineRows, 16);
            expectEquals (spanMismatches, 0);
            expectLessThan (worstFine, 1.0e-6);
            logMessage ("  -> fine stage (band-pass + zero crossings), " + juce::String (fineRows) + " points across 8 tones, young and full windows: "
                        + juce::String (spanMismatches) + " settle/window mismatches, worst difference " + juce::String (worstFine, 12) + " cents");
        }
    }

    void testAccuracy()
    {
        beginTest ("synthetic accuracy: sines, saws, and stiff strings from 30 Hz to 1.3 kHz read within 0.5 cent of the first partial");
        {
            const std::vector<double> freqs { 30.0, 41.2, 55.0, 82.4, 110.0, 146.8, 196.0, 246.9, 329.6, 440.0, 659.3, 880.0, 1318.5 };
            std::vector<double> shownErrors, fineErrors;
            double worstShown = 0.0, worstFine = 0.0;
            juce::String worstShownCase, worstFineCase;
            int wrongNotes = 0, cases = 0;
            juce::int64 seed = 1;

            for (const auto f : freqs)
            {
                const std::vector<std::pair<juce::String, Tone>> tones {
                    { "sine", sineTone (f, 1.25) },
                    { "saw", sawtooth (f, 1.25) },
                    { "stiff B 5e-5", stiffString (f, 5.0e-5, 1.25, seed++) },
                    { "stiff B 3e-4", stiffString (f, 3.0e-4, 1.25, seed++) },
                };

                for (const auto& [name, tone] : tones)
                {
                    ++cases;
                    auto tuner = makeTuner();
                    const auto x = withNoise (concat ({ silence (0.05), tone.x }), 0.003, seed++); // -50 dB re the tone
                    const auto frames = runTuner (x, *tuner);

                    // Judge every reading once the fine stage has had its full settle and window (plus the
                    // detector's frame and the pluck), i.e. the last 0.3 s for the lowest notes.
                    for (const auto& fr : frames)
                    {
                        if (fr.time < 1.0 || ! fr.reading.live)
                            continue;
                        const auto shown = std::abs (cents (fr.reading.frequency, tone.pitch));
                        shownErrors.push_back (shown);
                        if (shown > worstShown)
                        {
                            worstShown = shown;
                            worstShownCase = name + " at " + str (tone.pitch, 1) + " Hz";
                        }
                        if (fr.fine > 0.0)
                        {
                            const auto e = std::abs (cents (fr.fine, tone.pitch));
                            fineErrors.push_back (e);
                            if (e > worstFine)
                            {
                                worstFine = e;
                                worstFineCase = name + " at " + str (tone.pitch, 1) + " Hz";
                            }
                        }
                        if (! noteIsRight (fr.reading.midiNote, tone.pitch))
                            ++wrongNotes;
                    }
                    expect (frames.back().reading.live, name + " at " + str (f, 1) + " Hz has no live reading at the end");
                }
            }

            expectLessThan (worstShown, 0.5);
            expectLessThan (worstFine, 0.5);
            expectEquals (wrongNotes, 0);
            logMessage ("  -> " + juce::String (cases) + " tones (13 frequencies x sine, saw, stiff B 5e-5, stiff B 3e-4; noise -50 dB), "
                        + juce::String ((int) shownErrors.size()) + " readings from 1.0 to 1.3 s through the ring and the analysis at 62.5 Hz");
            logMessage ("  -> shown reading (median of 5): median error " + str (median (shownErrors), 4) + " cents, worst " + str (worstShown, 4)
                        + " cents (" + worstShownCase + ")");
            logMessage ("  -> each fine reading: median error " + str (median (fineErrors), 4) + " cents, worst " + str (worstFine, 4) + " cents ("
                        + worstFineCase + "); wrong notes shown: " + juce::String (wrongNotes));
        }
    }

    void testOctaves()
    {
        beginTest ("octave errors: none on low strings whose 2nd harmonic is 6 dB above the fundamental");
        {
            const std::vector<double> freqs { 30.0, 41.2, 55.0, 82.4, 110.0, 146.8, 196.0 };
            int coarseChecked = 0, coarseOctaves = 0, shownChecked = 0, shownWrong = 0;
            double worstCoarse = 0.0;
            juce::int64 seed = 100;

            for (const auto f : freqs)
            {
                for (int take = 0; take < 3; ++take)
                {
                    const auto tone = stiffString (f, 1.0e-4, 1.25, seed++, 6.0);
                    auto tuner = makeTuner();
                    const auto frames = runTuner (withNoise (concat ({ silence (0.05), tone.x }), 0.003, seed++), *tuner);

                    for (const auto& fr : frames)
                    {
                        if (fr.coarse.frequency > 0.0 && fr.coarse.clarity >= 0.9)
                        {
                            ++coarseChecked;
                            const auto c = std::abs (cents (fr.coarse.frequency, tone.pitch));
                            if (c > 600.0)
                                ++coarseOctaves;
                            else
                                worstCoarse = std::max (worstCoarse, c);
                        }
                        if (fr.reading.live)
                        {
                            ++shownChecked;
                            if (! noteIsRight (fr.reading.midiNote, tone.pitch))
                                ++shownWrong;
                        }
                    }
                }
            }

            expectGreaterThan (coarseChecked, 500);
            expectEquals (coarseOctaves, 0);
            expectEquals (shownWrong, 0);
            logMessage ("  -> 7 low notes (30 to 196 Hz) x 3 plucks, 2nd harmonic +6 dB: " + juce::String (coarseChecked)
                        + " confident coarse estimates, " + juce::String (coarseOctaves) + " octave errors (worst coarse error otherwise "
                        + str (worstCoarse, 2) + " cents); " + juce::String (shownChecked) + " live readings, " + juce::String (shownWrong)
                        + " showing the wrong note");
        }
    }

    void testHold()
    {
        beginTest ("hold: after a plucked note decays into the noise the reading holds its last value");
        {
            // A2 plucked after 0.2 s of a -80 dBFS noise floor, decaying about 60 dB in 3 s.
            const auto tone = stiffString (110.0, 1.0e-4, 4.8, 21, 0.0, 0.35);
            const auto x = withNoise (concat ({ silence (0.2), tone.x }), 1.0e-4, 22);
            auto tuner = makeTuner();
            const auto frames = runTuner (x, *tuner);

            double lastLiveTime = -1.0, liveLevel = 0.0, firstReading = -1.0, worstLive = 0.0, worstTime = 0.0, worstLevel = 0.0;
            TunerReading lastLive;
            for (const auto& f : frames)
            {
                if (f.reading.live)
                {
                    lastLiveTime = f.time;
                    lastLive = f.reading;
                    liveLevel = f.reading.levelDb;
                    const auto error = std::abs (cents (f.reading.frequency, tone.pitch));
                    if (error > worstLive)
                    {
                        worstLive = error;
                        worstTime = f.time - 0.2;
                        worstLevel = f.reading.levelDb;
                    }
                    if (firstReading < 0.0)
                        firstReading = f.time - 0.2;
                }
            }

            int heldFrames = 0, changedWhileHeld = 0, liveAgain = 0;
            for (const auto& f : frames)
            {
                if (f.time <= lastLiveTime)
                    continue;
                ++heldFrames;
                if (f.reading.live)
                    ++liveAgain;
                // Bit-identical: a held reading isn't recomputed from anything new.
                if (! f.reading.hasReading || ! juce::exactlyEqual (f.reading.frequency, lastLive.frequency) || f.reading.midiNote != lastLive.midiNote
                    || ! juce::exactlyEqual (f.reading.cents, lastLive.rawCents) || ! juce::exactlyEqual (f.reading.strobeVelocity, 0.0)
                    || ! juce::exactlyEqual (f.reading.strobePhase, frames.back().reading.strobePhase))
                    ++changedWhileHeld;
            }

            expect (lastLiveTime > 1.0 && lastLiveTime < 4.5, "the note should stop reading somewhere in its decay");
            expectGreaterThan (heldFrames, 20);
            expectEquals (changedWhileHeld, 0);
            expectEquals (liveAgain, 0);
            expectEquals (countNoteChanges (frames), 0);
            expectLessThan (worstLive, 0.5);
            expectLessThan (std::abs (cents (lastLive.frequency, tone.pitch)), 0.5);
            logMessage ("  -> first reading " + str (1000.0 * firstReading, 0) + " ms after the pluck; live until " + str (lastLiveTime - 0.2, 2)
                        + " s after it (input " + str (liveLevel, 1) + " dBFS; gate -60, closing at -66 once a note is on); every live reading within "
                        + str (worstLive, 3) + " cents (worst " + str (worstTime, 2) + " s after the pluck, input " + str (worstLevel, 1)
                        + " dBFS); note changes during the decay: " + juce::String (countNoteChanges (frames)));
            logMessage ("  -> then " + juce::String (heldFrames) + " updates (" + str (heldFrames * 768.0 / fs, 2) + " s) of noise: the reading holds A2 at "
                        + str (lastLive.frequency, 4) + " Hz (" + str (cents (lastLive.frequency, tone.pitch), 3) + " cents from the true "
                        + str (tone.pitch, 4) + " Hz), " + juce::String (changedWhileHeld) + " changes, strobe frozen, live flag back on "
                        + juce::String (liveAgain) + " times");
        }
    }

    void testSteadyNoise()
    {
        beginTest ("no flicker: a sustained tone in noise keeps its note; the needle wanders less than one 0.1 cent display step at 40 dB SNR, two at 20 dB");
        {
            for (const auto snrDb : { 40.0, 20.0 })
            {
                // E2, sustained, 6 s; noise snrDb below the tone's RMS.
                auto tone = stiffString (82.41, 1.0e-4, 6.0, 31, 0.0, 0.0);
                const auto toneRms = std::sqrt (std::inner_product (tone.x.begin(), tone.x.end(), tone.x.begin(), 0.0) / (double) tone.x.size());
                auto tuner = makeTuner();
                const auto frames = runTuner (withNoise (tone.x, toneRms * std::pow (10.0, -snrDb / 20.0), 32), *tuner);

                std::vector<double> needle, raw;
                int dropouts = 0;
                for (const auto& f : frames)
                {
                    if (f.time < 2.0)
                        continue;
                    if (! f.reading.live)
                        ++dropouts;
                    needle.push_back (f.reading.cents);
                    raw.push_back (f.reading.rawCents);
                }
                const auto [nLo, nHi] = std::minmax_element (needle.begin(), needle.end());
                const auto [rLo, rHi] = std::minmax_element (raw.begin(), raw.end());
                const auto mean = std::accumulate (needle.begin(), needle.end(), 0.0) / (double) needle.size();
                double variance = 0.0;
                for (auto v : needle)
                    variance += (v - mean) * (v - mean);
                const auto stdDev = std::sqrt (variance / (double) needle.size());
                const auto expected = cents (tone.pitch, 440.0 * std::pow (2.0, (40 - 69) / 12.0));
                const auto changes = countNoteChanges (frames);

                expectEquals (changes, 0);
                expectEquals (dropouts, 0);
                expectLessThan (*nHi - *nLo, snrDb >= 40.0 ? 0.1 : 0.25);
                expectLessThan (std::abs (mean - expected), 0.5);
                logMessage ("  -> E2 sustained, SNR " + str (snrDb, 0) + " dB, 4 s (" + juce::String ((int) needle.size()) + " updates): note changes "
                            + juce::String (changes) + ", dropouts " + juce::String (dropouts) + "; needle mean " + str (mean, 3) + " cents (true "
                            + str (expected, 3) + "), standard deviation " + str (stdDev, 4) + ", peak to peak " + str (*nHi - *nLo, 4)
                            + " cents (unsmoothed reading: " + str (*rHi - *rLo, 4) + ")");
            }
        }
    }

    void testHysteresisAndReference()
    {
        beginTest ("note hysteresis: a slide across a note boundary changes the note once, and a quarter tone doesn't flicker");
        {
            // E2 - 30 cents up to E2 + 80 (= F2 - 20) over 3 s, hold 1 s, back down over 3 s. Three harmonics,
            // continuous phase, noise 40 dB down.
            const auto e2 = 440.0 * std::pow (2.0, (40 - 69) / 12.0);
            std::vector<double> x, trueCents;
            double phase = 0.0;
            const auto total = (size_t) (7.5 * fs);
            for (size_t i = 0; i < total; ++i)
            {
                const auto t = (double) i / fs - 0.25;
                const auto c = t < 0.0 ? -30.0 : t < 3.0 ? -30.0 + 110.0 * t / 3.0 : t < 4.0 ? 80.0 : t < 7.0 ? 80.0 - 110.0 * (t - 4.0) / 3.0 : -30.0;
                phase += twoPi * e2 * std::pow (2.0, c / 1200.0) / fs;
                x.push_back (0.25 * std::sin (phase) + 0.12 * std::sin (2.0 * phase) + 0.06 * std::sin (3.0 * phase));
                trueCents.push_back (c);
            }
            auto tuner = makeTuner();
            const auto frames = runTuner (withNoise (x, 0.003, 41), *tuner);

            juce::StringArray switches;
            int last = -1, changes = 0;
            for (const auto& f : frames)
            {
                if (! f.reading.hasReading)
                    continue;
                if (last >= 0 && f.reading.midiNote != last)
                {
                    ++changes;
                    const auto shownBefore = 100.0 * (12.0 * std::log2 (f.reading.frequency / 440.0) + 69.0 - last);
                    const auto i = std::min (trueCents.size() - 1, (size_t) (f.time * fs) - 1);
                    switches.add ((last == 40 ? "E2 -> F2" : "F2 -> E2") + juce::String (" at a reading of E2 ")
                                  + (shownBefore >= 0 ? "+" : "") + str (last == 40 ? shownBefore : shownBefore + 100.0, 1) + " cents (true pitch E2 "
                                  + (trueCents[i] >= 0 ? "+" : "") + str (trueCents[i], 1) + ")");
                }
                last = f.reading.midiNote;
            }
            expectEquals (changes, 2);
            logMessage ("  -> slide E2 -30 -> +80 -> -30 cents (3 s each way): " + juce::String (changes) + " note changes, one each way: "
                        + switches.joinIntoString ("; "));

            // A string sitting exactly on the quarter tone between E2 and F2, in noise.
            auto quarter = stiffString (e2 * std::pow (2.0, 50.0 / 1200.0) / std::sqrt (1.0 + 1.0e-4), 1.0e-4, 5.0, 42, 0.0, 0.0);
            auto tuner2 = makeTuner();
            const auto frames2 = runTuner (withNoise (quarter.x, 0.003, 43), *tuner2);
            std::vector<double> shown;
            for (const auto& f : frames2)
                if (f.reading.live)
                    shown.push_back (100.0 * (12.0 * std::log2 (f.reading.frequency / 440.0) + 69.0 - 40.0));
            const auto [lo, hi] = std::minmax_element (shown.begin(), shown.end());
            const auto quarterChanges = countNoteChanges (frames2);
            expectEquals (quarterChanges, 0);
            logMessage ("  -> a string held at E2 +50 cents (the quarter tone) for 5 s: readings from E2 +" + str (*lo, 3) + " to +" + str (*hi, 3)
                        + " cents, note changes " + juce::String (quarterChanges) + " (shown as " + juce::MidiMessage::getMidiNoteName (frames2.back().reading.midiNote, true, true, 4)
                        + " " + (frames2.back().reading.rawCents >= 0 ? "+" : "") + str (frames2.back().reading.rawCents, 1) + ")");
        }

        beginTest ("A4 reference: at 432 Hz a 440 Hz string reads A4 +31.77 cents, a 432 Hz one A4 +0.00; the range is 430 to 450 Hz");
        {
            juce::StringArray results;
            double worst = 0.0;
            for (const auto& [reference, frequency] : std::vector<std::pair<double, double>> { { 432.0, 440.0 }, { 432.0, 432.0 }, { 440.0, 440.0 }, { 450.0, 440.0 }, { 430.0, 82.41 } })
            {
                auto tuner = makeTuner (reference);
                const auto tone = sawtooth (frequency, 1.2);
                const auto frames = runTuner (withNoise (tone.x, 0.003, 51), *tuner);
                const auto& r = frames.back().reading;
                const auto expectedNote = nearestNote (frequency, reference);
                const auto expectedCents = 100.0 * (12.0 * std::log2 (frequency / reference) + 69.0 - expectedNote);
                expectEquals (r.midiNote, expectedNote);
                worst = std::max (worst, std::abs (r.rawCents - expectedCents));
                results.add (str (frequency, 2) + " Hz at A4 = " + str (reference, 0) + ": " + juce::MidiMessage::getMidiNoteName (r.midiNote, true, true, 4)
                             + " " + (r.rawCents >= 0 ? "+" : "") + str (r.rawCents, 2) + " (expected " + (expectedCents >= 0 ? "+" : "") + str (expectedCents, 2) + ")");
            }
            expectLessThan (worst, 0.05);

            TunerAnalysis clamp;
            clamp.prepare (fs);
            clamp.setReferenceA4 (400.0);
            const auto low = clamp.getReferenceA4();
            clamp.setReferenceA4 (500.0);
            const auto high = clamp.getReferenceA4();
            expectEquals (low, 430.0);
            expectEquals (high, 450.0);

            // Changing the reference while a reading is held re-labels the held reading.
            auto tuner = makeTuner (440.0);
            const auto tone = stiffString (110.0, 1.0e-4, 1.5, 52, 0.0, 0.2);
            runTuner (withNoise (concat ({ tone.x, silence (1.0) }), 1.0e-4, 53), *tuner);
            const auto before = tuner->getReading();
            tuner->setReferenceA4 (432.0);
            const auto after = tuner->analyse();
            expect (! before.live && ! after.live);
            expectWithinAbsoluteError (after.rawCents - before.rawCents, cents (440.0, 432.0), 1.0e-9);
            logMessage ("  -> " + results.joinIntoString ("; ") + "; worst error " + str (worst, 4) + " cents");
            logMessage ("  -> setReferenceA4 (400) gives " + str (low, 0) + ", (500) gives " + str (high, 0) + "; a held A2 re-read at 432 Hz moves by "
                        + str (after.rawCents - before.rawCents, 3) + " cents");
        }

        beginTest ("strobe: the pattern drifts at 0.25 periods per second per cent, and stands still in tune");
        {
            juce::StringArray results;
            double worst = 0.0;
            for (const auto offset : { -5.0, -1.0, 0.0, 0.3, 2.0 })
            {
                const auto tone = sawtooth (440.0 * std::pow (2.0, offset / 1200.0), 2.5);
                auto tuner = makeTuner();
                const auto frames = runTuner (withNoise (tone.x, 0.003, 91), *tuner);

                // Unwrap the phase over the last 1.5 s: each step is the shortest way round the circle.
                double travelled = 0.0, start = -1.0, previous = 0.0;
                for (const auto& f : frames)
                {
                    if (f.time < 1.0)
                        continue;
                    if (start < 0.0)
                        start = f.time;
                    else
                    {
                        auto step = f.reading.strobePhase - previous;
                        step -= std::round (step);
                        travelled += step;
                    }
                    previous = f.reading.strobePhase;
                }
                const auto speed = travelled / (frames.back().time - start);
                const auto expected = TunerAnalysis::Settings {}.strobePeriodsPerSecondPerCent * offset;
                worst = std::max (worst, std::abs (speed - expected));
                results.add ((offset >= 0 ? "+" : "") + str (offset, 1) + " cents: " + str (speed, 4) + " periods/s (expected " + str (expected, 4) + ")");
            }
            expectLessThan (worst, 0.005);
            logMessage ("  -> A4 offset by: " + results.joinIntoString ("; ") + "; worst difference " + str (worst, 5) + " periods/s");
        }
    }

    struct Latency
    {
        std::vector<double> ms;
        int misses = 0, transitional = 0;
        double staleMs = 0.0;
        juce::String lastWrong;
    };

    /// Harmonizer use: PitchDetector with the harmonizer preset (floor floorHz), detect() every 64 samples.
    /// A pluck after silence or after another note (cut off over 1 ms); 6 pluck timings each.
    Latency measureLatency (double f, double floorHz, const std::vector<double>& previousNotes)
    {
        auto settings = PitchDetector::Settings::harmonizer();
        settings.minFrequency = floorHz;
        PitchDetector d;
        d.prepare (fs, settings);
        Latency result;
        constexpr int hop = 64;

        for (const auto previous : previousNotes)
        {
            for (int seed = 0; seed < 6; ++seed)
            {
                d.reset();
                const auto lead = (size_t) (0.3 * fs) + (size_t) (11 * seed % hop);
                std::vector<double> before (lead, 0.0);
                double previousPitch = 0.0;
                if (previous > 0.0)
                {
                    auto p = pluck (previous, (double) lead / fs + 0.01, 500 + seed);
                    std::copy (p.x.begin(), p.x.begin() + (std::ptrdiff_t) lead, before.begin());
                    for (size_t i = 0; i < 48; ++i)
                        before[lead - 48 + i] *= 1.0 - (double) i / 48.0;
                    previousPitch = p.pitch;
                }
                const auto note = pluck (f, 0.1, seed);
                const auto x = withNoise (concat ({ before, note.x }), 0.0005, 600 + seed);

                double found = -1.0;
                for (size_t end = hop; end <= x.size(); end += hop)
                {
                    d.push (x.data() + end - hop, hop);
                    if (end <= lead || found >= 0.0)
                        continue;
                    const auto e = d.detect();
                    const auto ms = 1000.0 * (double) (end - lead) / fs;
                    if (e.frequency <= 0.0 || e.clarity < 0.9)
                        continue;
                    if (std::abs (cents (e.frequency, note.pitch)) < 50.0)
                        found = ms;
                    else if (previousPitch > 0.0 && std::abs (cents (e.frequency, previousPitch)) < 50.0)
                        result.staleMs = std::max (result.staleMs, ms);
                    else
                    {
                        ++result.transitional;
                        result.lastWrong = str (e.frequency, 1) + " Hz at " + str (ms, 2) + " ms";
                    }
                }
                if (found < 0.0)
                    ++result.misses;
                else
                    result.ms.push_back (found);
            }
        }
        return result;
    }

    void testLatency()
    {
        beginTest ("detection latency for the harmonizer: pluck to a confident coarse reading at the 110 Hz floor, within the BUILD_PLAN budgets");
        {
            struct Case { const char* name; double hz, floor, budget; };
            for (const auto& s : { Case { "high E", 329.63, 110.0, 8.0 }, Case { "G", 196.0, 110.0, 12.0 },
                                   Case { "A", 110.0, 110.0, 20.0 }, Case { "low E", 82.41, 80.0, 30.0 } })
            {
                const auto r = measureLatency (s.hz, s.floor, { 0.0, s.hz * std::pow (2.0, -5.0 / 12.0), s.hz * std::pow (2.0, 7.0 / 12.0) });
                const auto mean = std::accumulate (r.ms.begin(), r.ms.end(), 0.0) / (double) std::max<size_t> (1, r.ms.size());
                const auto worst = r.ms.empty() ? 1.0e9 : *std::max_element (r.ms.begin(), r.ms.end());
                expectEquals (r.misses, 0);
                expectLessThan (worst, s.budget);
                logMessage ("  -> " + juce::String (s.name) + " (" + str (s.hz, 2) + " Hz, floor " + str (s.floor, 0) + " Hz): mean " + str (mean, 2)
                            + " ms, worst " + str (worst, 2) + " ms (budget " + str (s.budget, 0) + " ms) over 18 plucks after silence, a fourth below, a fifth above; "
                            + "the previous note still read up to " + str (r.staleMs, 2) + " ms after the pluck; " + juce::String (r.transitional)
                            + " confident readings of neither note" + (r.transitional > 0 ? " (last: " + r.lastWrong + ")" : juce::String()));
            }

            // The low E with the default floor: out of range, so it should give no confident reading of anything.
            const auto r = measureLatency (82.41, 110.0, { 0.0 });
            logMessage ("  -> low E at the default 110 Hz floor: " + juce::String (r.misses) + " of 6 plucks never read as E2 within 60 ms, "
                        + juce::String (r.transitional) + " confident readings of another pitch" + (r.transitional > 0 ? " (last: " + r.lastWrong + ")" : juce::String())
                        + "; the floor has to come down to about 80 Hz to play the low E (BUILD_PLAN: \"if the floor is lowered to reach it\")");
        }
    }

    void testThread()
    {
        beginTest ("TunerThread: real-time pace from a fake audio thread, readings arrive at about 60 Hz, never torn, cleared on disengage");
        {
            ampsim::TunerThread tuner;
            tuner.prepare (fs, blockSize);
            tuner.setEngaged (true);
            const auto tone = stiffString (110.0, 1.0e-4, 2.0, 61, 0.0, 0.0);
            const auto x = withNoise (tone.x, 0.0005, 62);
            std::atomic<bool> done { false };
            const auto startCount = tuner.getUpdateCount();
            const auto t0 = std::chrono::steady_clock::now();

            std::thread audio ([&]
            {
                auto next = std::chrono::steady_clock::now();
                for (size_t start = 0; start + (size_t) blockSize <= x.size(); start += (size_t) blockSize)
                {
                    tuner.pushAudio (x.data() + start, blockSize);
                    next += std::chrono::microseconds ((int) (1.0e6 * blockSize / fs));
                    std::this_thread::sleep_until (next);
                }
                done = true;
            });

            int snapshots = 0, torn = 0;
            double firstLive = -1.0;
            TunerReading last;
            while (! done)
            {
                const auto r = tuner.getReading();
                ++snapshots;
                if (r.hasReading)
                {
                    // Every field comes from one analysis, so the frequency, note, and cents must agree exactly.
                    const auto note = 12.0 * std::log2 (r.frequency / r.referenceA4) + 69.0;
                    if (std::abs (note - (r.midiNote + r.rawCents / 100.0)) > 1.0e-9)
                        ++torn;
                }
                if (r.live && firstLive < 0.0)
                    firstLive = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
                last = r;
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            audio.join();
            const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            const auto updates = tuner.getUpdateCount() - startCount;

            tuner.setEngaged (false);
            std::this_thread::sleep_for (std::chrono::milliseconds (150));
            const auto afterDisengage = tuner.getReading();

            expect (last.live);
            expectEquals (last.midiNote, 45);
            expectLessThan (std::abs (cents (last.frequency, tone.pitch)), 0.5);
            expectEquals (torn, 0);
            expect (updates / seconds > 40.0 && updates / seconds < 70.0, "expected about 60 updates a second");
            expect (! afterDisengage.hasReading);
            logMessage ("  -> 2 s of A2 pushed in real time: " + juce::String ((int) updates) + " updates in " + str (seconds, 2) + " s ("
                        + str (updates / seconds, 1) + " per second); first live reading after " + str (1000.0 * firstLive, 0) + " ms; last reading "
                        + juce::MidiMessage::getMidiNoteName (last.midiNote, true, true, 4) + " " + str (last.rawCents, 3) + " cents (true "
                        + str (cents (tone.pitch, 110.0), 3) + "); " + juce::String (snapshots) + " GUI snapshots, " + juce::String (torn)
                        + " torn; after disengaging: hasReading " + (afterDisengage.hasReading ? "true" : "false"));
        }
    }

    void testCostAndRealtime()
    {
        beginTest ("cost: the analysis thread stays light, and the harmonizer's detector is cheap on the audio thread");
        {
            const auto di = guitarDI ((int) (10.0 * fs));
            const auto low = withNoise (sawtooth (30.0, 10.0).x, 0.003, 71);
            juce::StringArray lines;

            for (const auto& [name, signal] : std::vector<std::pair<juce::String, const std::vector<float>*>> { { "guitar DI", &di }, { "30 Hz saw (longest fine span)", &low } })
            {
                TunerAnalysis tuner;
                tuner.prepare (fs);
                std::vector<double> times;
                for (size_t start = 0; start + 768 <= signal->size(); start += 768)
                {
                    tuner.push (signal->data() + start, 768);
                    const auto t0 = std::chrono::steady_clock::now();
                    tuner.analyse();
                    times.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
                }
                const auto mean = std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
                expectLessThan (mean, 1000.0 * cpuBudgetScale());
                lines.add ("tuner analyse() on " + name + ": mean " + micros (mean) + ", p99 " + micros (percentile (times, 99.0)) + ", worst "
                           + micros (*std::max_element (times.begin(), times.end())) + "; at 62.5 updates a second that's "
                           + str (100.0 * mean * 62.5 / 1.0e6, 2) + "% of one core");
            }

            PitchDetector d;
            d.prepare (fs, PitchDetector::Settings::harmonizer());
            std::vector<double> times;
            for (size_t start = 0; start + 64 <= di.size(); start += 64)
            {
                const auto t0 = std::chrono::steady_clock::now();
                d.push (di.data() + start, 64);
                d.detect();
                times.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            const auto mean = std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
            expectLessThan (2.0 * mean, 0.02 * deadlineMicros * cpuBudgetScale());
            lines.add ("harmonizer preset, push (64) + detect() on the guitar DI: mean " + micros (mean) + ", p99 " + micros (percentile (times, 99.0))
                       + "; two per 128-sample buffer = " + str (100.0 * 2.0 * mean / deadlineMicros, 2) + "% of the 2.67 ms deadline");
            for (const auto& l : lines)
                logMessage ("  -> " + l);
        }

        beginTest ("real-time safety: pushing into the ring, and the detector's audio-thread use, allocate and lock nothing");
        {
            const auto di = guitarDI ((int) (10.0 * fs));
            auto describe = [] (const rtcheck::Counts& c)
            {
                return juce::String (c.allocations) + " allocations, " + juce::String (c.frees) + " frees, " + juce::String (c.blockingLocks) + " locks";
            };

            // The audio thread's side of the real TunerThread (its analysis thread running meanwhile),
            // engaged and disengaged, including a full ring (10 s pushed far faster than real time).
            ampsim::TunerThread tuner;
            tuner.prepare (fs, blockSize);
            rtcheck::Counts push;
            int blocks = 0;
            for (size_t start = 0; start + (size_t) blockSize <= di.size(); start += (size_t) blockSize, ++blocks)
            {
                if (blocks == 100)
                    tuner.setEngaged (true);
                if (blocks == 3500)
                    tuner.setEngaged (false);
                if (blocks == 3600)
                    tuner.setEngaged (true);
                rtcheck::begin();
                tuner.pushAudio (di.data() + start, blockSize);
                push += rtcheck::end();
            }
            expectEquals (push.allocations, 0L);
            expectEquals (push.frees, 0L);
            expectEquals (push.blockingLocks, 0L);

            // The harmonizer's use: push 64 samples, detect, on the audio thread.
            PitchDetector d;
            d.prepare (fs, PitchDetector::Settings::harmonizer());
            rtcheck::Counts detect;
            for (size_t start = 0; start + 64 <= di.size(); start += 64)
            {
                rtcheck::begin();
                d.push (di.data() + start, 64);
                d.detect();
                detect += rtcheck::end();
            }
            expectEquals (detect.allocations, 0L);
            expectEquals (detect.frees, 0L);
            expectEquals (detect.blockingLocks, 0L);

            // The analysis thread's side too, though it's allowed to block: nothing allocates per update.
            TunerAnalysis analysis;
            analysis.prepare (fs);
            rtcheck::Counts analyse;
            for (size_t start = 0; start + 768 <= di.size(); start += 768)
            {
                rtcheck::begin();
                analysis.push (di.data() + start, 768);
                analysis.analyse();
                analyse += rtcheck::end();
            }
            expectEquals (analyse.allocations, 0L);
            expectEquals (analyse.frees, 0L);

            logMessage ("  -> TunerThread::pushAudio, " + juce::String (blocks) + " blocks (engaged, disengaged, engaged; ring full part of the time): " + describe (push));
            logMessage ("  -> PitchDetector (harmonizer preset), push (64) + detect() x " + juce::String ((int) (di.size() / 64)) + ": " + describe (detect));
            logMessage ("  -> TunerAnalysis push + analyse() x " + juce::String ((int) (di.size() / 768)) + " (analysis thread): " + describe (analyse));
        }
    }

    void plotNeedle()
    {
        beginTest ("proof plot: the needle for a plucked, decaying E2 tuned 3 cents sharp, re-plucked at 2 s, then held");
        {
            const auto e2 = 440.0 * std::pow (2.0, (40 - 69) / 12.0);
            const auto f0 = e2 * std::pow (2.0, 3.0 / 1200.0);
            auto first = stiffString (f0, 1.0e-4, 1.9, 81, 0.0, 0.5);
            auto second = stiffString (f0, 1.0e-4, 4.0, 82, 0.0, 0.25);
            for (size_t i = 0; i < 96; ++i) // the pick stops the string before it plucks again
                first.x[first.x.size() - 96 + i] *= 1.0 - (double) i / 96.0;
            const auto x = withNoise (concat ({ silence (0.1), first.x, second.x }), 1.0e-4, 83);
            auto tuner = makeTuner();
            const auto frames = runTuner (x, *tuner);
            const auto trueCents = cents (first.pitch, e2);

            PlotSeries needle { "needle (smoothed cents)", {}, {}, plotColour (0), 2.5f };
            PlotSeries raw { "median reading, unsmoothed", {}, {}, plotColour (1), 1.2f };
            PlotSeries truth { "true first partial", {}, {}, plotColour (6), 1.5f, true };
            PlotSeries live { "live (at +2.0) / holding (at +1.75)", {}, {}, plotColour (2), 1.5f };
            double worstAfterSettle = 0.0;
            for (const auto& f : frames)
            {
                const auto t = f.time;
                needle.x.push_back (t);
                needle.y.push_back (f.reading.hasReading ? f.reading.cents : std::nan (""));
                raw.x.push_back (t);
                raw.y.push_back (f.reading.hasReading ? f.reading.rawCents : std::nan (""));
                truth.x.push_back (t);
                truth.y.push_back (trueCents);
                live.x.push_back (t);
                live.y.push_back (f.reading.live ? 2.0 : 1.75);
                if (f.reading.live && ((t > 0.6 && t < 2.0) || t > 2.5))
                    worstAfterSettle = std::max (worstAfterSettle, std::abs (f.reading.cents - trueCents));
            }

            PlotOptions o;
            o.title = "Tuner needle: E2 plucked 3 cents sharp, re-plucked at 2 s, decaying into the noise and held";
            o.xLabel = "Time (s)";
            o.yLabel = "Cents from E2";
            o.xMin = 0.0; o.xMax = 6.0; o.yMin = 1.5; o.yMax = 4.5;
            const auto png = proofDir().getChildFile ("tuner_needle.png");
            double holdFrom = -1.0;
            for (const auto& f : frames)
                holdFrom = f.reading.live ? -1.0 : (holdFrom < 0.0 ? f.time : holdFrom);
            expect (savePlot (png, o, { truth, raw, needle, live }));
            expectLessThan (worstAfterSettle, 0.5);
            expect (holdFrom > 3.0 && holdFrom < 5.9, "the second pluck should decay into a hold before the end");
            logMessage ("  -> needle within " + str (worstAfterSettle, 3) + " cents of the true +" + str (trueCents, 3)
                        + " from 0.5 s after each pluck while live; holding " + str (frames.back().reading.cents, 3) + " cents from "
                        + str (holdFrom, 2) + " s on: " + png.getFullPathName());
        }
    }
};

TunerTests tunerTests;
} // namespace

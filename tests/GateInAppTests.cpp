// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The noise gate in the app (Sean's play test, 2026-10-04: "all the individual parts seem to function as
// intended, other than the noise gate"). Two questions: does the Amp page's Gate group do anything, and how
// does Gate A at its defaults behave on a realistically noisy guitar (single-coil hum and hiss) through the
// high-gain capture?

#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"

#include <cmath>
#include <random>

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

/// A pickup's noise floor at `rmsDb` dBFS: half its power mains buzz (60 Hz and its harmonics to 900 Hz at
/// 1/k amplitudes, random phases: a single coil's hum is rich in harmonics) and half hiss (white).
std::vector<float> pickupNoise (int numSamples, double rmsDb, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> phase (0.0, juce::MathConstants<double>::twoPi);
    std::normal_distribution<double> gauss (0.0, 1.0);
    std::vector<double> hum ((size_t) numSamples, 0.0), hiss ((size_t) numSamples);
    for (int k = 1; k <= 15; ++k)
    {
        const auto ph = phase (rng);
        for (int n = 0; n < numSamples; ++n)
            hum[(size_t) n] += std::sin (juce::MathConstants<double>::twoPi * 60.0 * k * n / fs + ph) / k;
    }
    for (auto& h : hiss)
        h = gauss (rng);
    const auto power = [] (const std::vector<double>& x)
    {
        double s = 0.0;
        for (auto v : x)
            s += v * v;
        return s / (double) x.size();
    };
    const auto target = std::pow (10.0, rmsDb / 20.0);
    const auto gh = target * std::sqrt (0.5 / power (hum)), gs = target * std::sqrt (0.5 / power (hiss));
    std::vector<float> out ((size_t) numSamples);
    for (size_t n = 0; n < out.size(); ++n)
        out[n] = (float) (gh * hum[n] + gs * hiss[n]);
    return out;
}

struct Run
{
    std::vector<float> left, gain; // the output, and Gate A's gain per sample
    std::vector<float> detectorDb; // Gate A's detector level per buffer
};

Run play (AmpSimProcessor& p, const std::vector<float>& input)
{
    Run r;
    r.left.resize (input.size());
    r.gain.assign (input.size(), 1.0f);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, r.left.begin() + (long) start);
        const auto& gate = p.getChain().gateA;
        if (gate.getGainCurveLength() == blockSize)
            std::copy (gate.getGainCurve(), gate.getGainCurve() + blockSize, r.gain.begin() + (long) start);
        r.detectorDb.push_back (p.getGateMeter (false).detectorDb);
    }
    return r;
}

double rmsDb (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
        s += (double) x[i] * x[i];
    return 10.0 * std::log10 (s / (double) (to - from) + 1.0e-30);
}
} // namespace

class GateInAppTests final : public juce::UnitTest
{
public:
    GateInAppTests() : juce::UnitTest ("Gate in the app", "ampsim") {}

    void runTest() override
    {
        beginTest ("the Amp page's Gate knobs are Gate A's, which starts off: turned while it's off they change nothing; switched on, they act");
        {
            const auto input = [&]
            {
                auto x = guitarDI ((int) (3.0 * fs));
                const auto noise = pickupNoise ((int) x.size(), -62.0, 1);
                for (size_t i = (size_t) (2.0 * fs); i < x.size(); ++i)
                    x[i] = 0.0f; // the last second: strings muted
                for (size_t i = 0; i < x.size(); ++i)
                    x[i] += noise[i];
                return x;
            }();
            const auto render = [&] (bool on, float thresholdDb, float releaseMs)
            {
                AmpSimProcessor p; // empty slots, no cab: the gate's own effect, plainly
                setParam (p, "gate_a_on", on ? 1.0f : 0.0f);
                setParam (p, "gate_a_threshold", thresholdDb);
                setParam (p, "gate_a_release", releaseMs);
                p.prepareToPlay (fs, blockSize);
                return play (p, input).left;
            };
            AmpSimProcessor fresh;
            const auto defaultOn = fresh.parameters.getRawParameterValue ("gate_a_on")->load();
            const auto offDefault = render (false, -55.0f, 250.0f), offTurned = render (false, -30.0f, 2000.0f);
            const auto onDefault = render (true, -55.0f, 250.0f), onTurned = render (true, -30.0f, 2000.0f);
            expectEquals (defaultOn, 0.0f);
            expect (offDefault == offTurned);
            expect (onDefault != onTurned);
            const auto gapFrom = (size_t) (2.5 * fs), gapTo = input.size();
            logMessage ("  -> gate_a_on starts at " + juce::String (defaultOn, 0) + " (off). Off: Threshold -55 -> -30 dB and Release 250 -> 2000 ms give the same output bit for bit. "
                        "On: the muted second measures " + juce::String (rmsDb (onDefault, gapFrom, gapTo), 1) + " dBFS at the defaults and "
                        + juce::String (rmsDb (onTurned, gapFrom, gapTo), 1) + " dBFS turned (" + juce::String (rmsDb (offDefault, gapFrom, gapTo), 1)
                        + " dBFS off). So until now the strip's knobs did nothing unless Gate A was switched on from the Pre FX page");
        }

        beginTest ("Gate A at its defaults on a noisy pickup (hum and hiss at -70, -65, -60 dBFS RMS) through Monolith: gaps, chatter, playing, and after Learn");
        {
            std::unique_ptr<AmpSimProcessor> owner;
            {
                WithBuiltInCaptures builtIns;
                owner = std::make_unique<AmpSimProcessor>();
            }
            auto& p = *owner;
            const auto cab = presets::resolve (presets::FileRef::fromVar (presets::factoryPresets()[2]["cab"]["mic1"]), "irs").file;
            p.loadCabIR (0, cab);
            waitForLoads (p);
            setParam (p, AmpSimProcessor::slotParamId, 2.0f);

            // Three times: the 2 s riff (peaks -6 dBFS), then 1.5 s of muted strings; the noise under all of it.
            const auto phrase = (size_t) (2.0 * fs), gap = (size_t) (1.5 * fs), cycle = phrase + gap;
            const auto riff = guitarDI ((int) phrase);
            juce::StringArray rows;
            for (const auto noiseDb : { -70.0, -65.0, -60.0 })
            {
                std::vector<float> input (3 * cycle, 0.0f);
                const auto noise = pickupNoise ((int) input.size(), noiseDb, 7);
                for (size_t c = 0; c < 3; ++c)
                    std::copy (riff.begin(), riff.end(), input.begin() + (long) (c * cycle));
                for (size_t i = 0; i < input.size(); ++i)
                    input[i] += noise[i];

                const auto measure = [&] (const juce::String& label, const Run& on, const Run& off)
                {
                    // In the gaps (from 300 ms after each stop, so the release is over): how often the gate is
                    // closed, how often it opens (chatter), and the noise left at the output against the gate off.
                    size_t closed = 0, total = 0, opens = 0, cutWhilePlaying = 0, playing = 0;
                    double gapOn = 0.0, gapOff = 0.0;
                    for (size_t c = 0; c < 3; ++c)
                    {
                        const auto from = c * cycle + phrase + (size_t) (0.3 * fs), to = (c + 1) * cycle;
                        for (size_t i = from; i < to; ++i)
                        {
                            closed += on.gain[i] < 0.01f ? 1 : 0;
                            opens += (on.gain[i] >= 0.5f && on.gain[i - 1] < 0.5f) ? 1 : 0;
                            gapOn += (double) on.left[i] * on.left[i];
                            gapOff += (double) off.left[i] * off.left[i];
                            ++total;
                        }
                        // While playing (the riff, up to 30 ms before its stop): samples the gate turned down by 6 dB or more.
                        for (size_t i = c * cycle + (size_t) (0.05 * fs); i < c * cycle + phrase - (size_t) (0.03 * fs); ++i)
                        {
                            cutWhilePlaying += on.gain[i] < 0.5f ? 1 : 0;
                            ++playing;
                        }
                    }
                    std::vector<float> detector;
                    for (size_t b = 0; b < on.detectorDb.size(); ++b)
                    {
                        const auto i = b * blockSize;
                        if (i % cycle > phrase + (size_t) (0.3 * fs))
                            detector.push_back (on.detectorDb[b]);
                    }
                    std::sort (detector.begin(), detector.end());
                    const auto median = detector.empty() ? -200.0f : detector[detector.size() / 2];
                    const auto p95 = detector.empty() ? -200.0f : detector[(size_t) (0.95 * (double) (detector.size() - 1))];
                    const auto closedShare = (double) closed / (double) juce::jmax ((size_t) 1, total);
                    rows.add ("noise " + juce::String (noiseDb, 0) + " dBFS, " + label + ": detector in the gaps median " + juce::String (median, 1) + " / 95% "
                              + juce::String (p95, 1) + " dBFS; gate closed " + juce::String (100.0 * closedShare, 0) + "% of the gaps, opened "
                              + juce::String (opens) + " times in them; output noise in the gaps " + juce::String (10.0 * std::log10 (gapOn / (double) total + 1.0e-30), 1)
                              + " dBFS vs " + juce::String (10.0 * std::log10 (gapOff / (double) total + 1.0e-30), 1) + " off; turned down while playing "
                              + juce::String (100.0 * (double) cutWhilePlaying / (double) playing, 2) + "% of the time");
                    return closedShare;
                };

                const auto runWith = [&] (bool gateOn)
                {
                    setParam (p, "gate_a_on", gateOn ? 1.0f : 0.0f);
                    p.runHousekeeping();
                    p.prepareToPlay (fs, blockSize);
                    return play (p, input);
                };
                setParam (p, "gate_a_threshold", -55.0f);
                const auto off = runWith (false);
                const auto atDefault = measure ("default threshold -55", runWith (true), off);

                // Learn: 2 s of the muted strings, then the threshold it wrote.
                setParam (p, "gate_a_on", 1.0f);
                p.prepareToPlay (fs, blockSize);
                p.learnGates();
                const auto muted = std::vector<float> (noise.begin(), noise.begin() + (long) (2.2 * fs));
                play (p, muted);
                p.runHousekeeping();
                const auto learned = p.parameters.getRawParameterValue ("gate_a_threshold")->load();
                const auto afterLearn = measure ("after Learn (threshold " + juce::String (learned, 1) + ")", runWith (true), off);
                expectGreaterThan (afterLearn, 0.9);
                juce::ignoreUnused (atDefault); // reported, not held: at the default threshold it never closes (see the log)
            }
            setParam (p, "gate_a_threshold", -55.0f);
            logMessage ("  -> " + rows.joinIntoString ("\n  -> "));
        }
    }
};

static GateInAppTests gateInAppTests;

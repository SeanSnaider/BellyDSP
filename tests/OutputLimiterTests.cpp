// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/OutputLimiter.h"

#include <cmath>

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

/// The limiter on a stereo signal (left and right given), in 128-sample buffers; optionally changing the
/// ceiling at a sample index.
Stereo runLimiter (ampsim::OutputLimiter& limiter, const std::vector<float>& left, const std::vector<float>& right,
                   std::vector<float>* reduction = nullptr)
{
    Stereo out { left, right };
    const ampsim::BlockContext context;
    for (size_t start = 0; start < left.size(); start += blockSize)
    {
        const auto n = std::min ((size_t) blockSize, left.size() - start);
        float* channels[] = { out.left.data() + start, out.right.data() + start };
        juce::dsp::AudioBlock<float> block (channels, 2, n);
        limiter.process (block, context);
        if (reduction != nullptr)
            reduction->push_back (limiter.getReductionDb());
    }
    return out;
}

float peakOf (const Stereo& s, size_t from = 0)
{
    float peak = 0.0f;
    for (size_t i = from; i < s.left.size(); ++i)
        peak = juce::jmax (peak, std::abs (s.left[i]), std::abs (s.right[i]));
    return peak;
}

std::vector<float> scaledTo (std::vector<float> x, float peakDb)
{
    float peak = 0.0f;
    for (auto v : x)
        peak = juce::jmax (peak, std::abs (v));
    const auto g = juce::Decibels::decibelsToGain (peakDb) / peak;
    for (auto& v : x)
        v *= g;
    return x;
}
} // namespace

class OutputLimiterTests final : public juce::UnitTest
{
public:
    OutputLimiterTests() : juce::UnitTest ("Output limiter", "ampsim") {}

    void runTest() override
    {
        beginTest ("the static curve: identity below the knee (2 dB under the ceiling), continuous with slope 1 there, always under the ceiling");
        {
            const auto c = 0.891250938; // -1 dBFS
            const auto k = c * std::pow (10.0, -ampsim::OutputLimiter::kneeDb / 20.0);
            expectEquals (ampsim::OutputLimiter::staticGain (k, c), 1.0);
            double worstSlope = 0.0, highest = 0.0;
            for (double p = k; p < 1000.0 * c; p *= 1.001)
            {
                const auto y = p * ampsim::OutputLimiter::staticGain (p, c);
                highest = juce::jmax (highest, y);
            }
            expectLessOrEqual (highest, c * (1.0 + 1.0e-12));
            // Slope just above the knee: (s(k + d) - k) / d -> 1.
            const auto d = 1.0e-7;
            worstSlope = ((k + d) * ampsim::OutputLimiter::staticGain (k + d, c) - k) / d;
            expectWithinAbsoluteError (worstSlope, 1.0, 1.0e-5);
            logMessage ("  -> knee at " + juce::String (20.0 * std::log10 (k), 2) + " dBFS for a -1 dBFS ceiling; slope just above it " + juce::String (worstSlope, 7)
                        + "; the curve's highest output for peaks up to +60 dB over the ceiling: " + juce::String (20.0 * std::log10 (highest), 4) + " dBFS");
        }

        beginTest ("never exceeds the ceiling: sines, square waves, noise, and the guitar DI driven up to 30 dB over, at ceilings 0, -1, and -6 dBFS, and while the ceiling moves");
        {
            juce::StringArray lines;
            for (const auto ceilingDb : { 0.0f, -1.0f, -6.0f })
            {
                const auto ceiling = juce::Decibels::decibelsToGain (ceilingDb);
                float worst = 0.0f;
                for (const auto overDb : { 0.0f, 3.0f, 10.0f, 30.0f })
                {
                    const auto n = (int) fs;
                    std::vector<std::vector<float>> signals { scaledTo (sine (1000.0, 1.0, n), ceilingDb + overDb), scaledTo (sine (61.0, 1.0, n), ceilingDb + overDb),
                                                              scaledTo (whiteNoise (n, 1.0f, 3), ceilingDb + overDb), scaledTo (guitarDI (n), ceilingDb + overDb) };
                    std::vector<float> square (n);
                    for (int i = 0; i < n; ++i)
                        square[(size_t) i] = (i / 120) % 2 == 0 ? 1.0f : -1.0f;
                    signals.push_back (scaledTo (square, ceilingDb + overDb));
                    for (const auto& x : signals)
                    {
                        ampsim::OutputLimiter limiter;
                        limiter.setCeilingDb (ceilingDb);
                        limiter.prepare (fs, blockSize);
                        auto right = x;
                        std::reverse (right.begin(), right.end()); // a different signal on each side: linked by the louder
                        const auto out = runLimiter (limiter, x, right);
                        worst = juce::jmax (worst, peakOf (out));
                    }
                }
                expectLessOrEqual (worst, ceiling);
                lines.add (juce::String (ceilingDb, 0) + " dBFS: highest output " + juce::String (juce::Decibels::gainToDecibels (worst), 3) + " dBFS");
            }

            // The ceiling moving from -1 to -12 and back while the DI runs 20 dB over: never above where the
            // smoothed ceiling is (it moves over 20 ms), and never above the higher of the two.
            ampsim::OutputLimiter limiter;
            limiter.setCeilingDb (-1.0f);
            limiter.prepare (fs, blockSize);
            const auto x = scaledTo (guitarDI ((int) (2 * fs)), 19.0f);
            Stereo out { x, x };
            const ampsim::BlockContext context;
            for (size_t start = 0; start < x.size(); start += blockSize)
            {
                limiter.setCeilingDb ((double) start < fs / 2 ? -1.0f : (double) start < fs ? -12.0f : -1.0f);
                float* channels[] = { out.left.data() + start, out.right.data() + start };
                juce::dsp::AudioBlock<float> block (channels, 2, (size_t) blockSize);
                limiter.process (block, context);
            }
            const auto moving = peakOf (out);
            const auto lowCeilingPeak = [&]
            {
                float peak = 0.0f;
                for (size_t i = (size_t) (0.55 * fs); i < (size_t) fs; ++i)
                    peak = juce::jmax (peak, std::abs (out.left[i]));
                return peak;
            }();
            expectLessOrEqual (moving, juce::Decibels::decibelsToGain (-1.0f));
            expectLessOrEqual (lowCeilingPeak, juce::Decibels::decibelsToGain (-12.0f));
            logMessage ("  -> " + lines.joinIntoString ("; ") + " (sines at 61 Hz and 1 kHz, noise, a square wave, the DI, each 0 to 30 dB over). Ceiling moved -1 to -12 to -1 "
                        "dBFS under the DI 20 dB over: highest " + juce::String (juce::Decibels::gainToDecibels (moving), 2) + " dBFS, and "
                        + juce::String (juce::Decibels::gainToDecibels (lowCeilingPeak), 2) + " dBFS once at -12");
        }

        beginTest ("bit-transparent while the signal stays 3 dB under the ceiling, and again once it recovers after limiting; zero latency");
        {
            ampsim::OutputLimiter limiter;
            limiter.prepare (fs, blockSize);
            const auto quiet = scaledTo (guitarDI ((int) (4 * fs)), -4.0f); // 3 dB under the -1 dBFS ceiling
            auto right = quiet;
            std::reverse (right.begin(), right.end());
            const auto out = runLimiter (limiter, quiet, right);
            expect (out.left == quiet && out.right == right);

            // A burst 10 dB over, then the quiet DI: within 0.01 dB after a few release times, then bit-exact.
            auto x = scaledTo (guitarDI ((int) (0.5 * fs)), 9.0f);
            x.insert (x.end(), quiet.begin(), quiet.end());
            std::vector<float> reductions;
            const auto after = runLimiter (limiter, x, x, &reductions);
            size_t exactFrom = x.size();
            for (size_t i = x.size(); i-- > 0;)
                if (! juce::exactlyEqual (after.left[i], x[i]))
                {
                    exactFrom = i + 1;
                    break;
                }
            const auto exactAfterMs = 1000.0 * (double) (exactFrom - (size_t) (0.5 * fs)) / fs;
            double worstAt300 = 0.0;
            for (size_t i = (size_t) (1.0 * fs); i < x.size(); ++i)
                if (std::abs (x[i]) > 1.0e-3f)
                    worstAt300 = juce::jmax (worstAt300, std::abs (juce::Decibels::gainToDecibels ((double) after.left[i] / x[i])));
            expectLessThan (worstAt300, 0.01);
            expectLessThan (exactAfterMs, 2000.0);

            // Zero latency: an impulse under the knee comes out at sample 0, unchanged.
            ampsim::OutputLimiter fresh;
            fresh.prepare (fs, blockSize);
            std::vector<float> impulse (256, 0.0f);
            impulse[0] = 0.5f;
            const auto i = runLimiter (fresh, impulse, impulse);
            expect (i.left == impulse);
            expectEquals (fresh.latencySamples(), 0);
            logMessage ("  -> the DI peaking at -4 dBFS (3 dB under a -1 dBFS ceiling): bit for bit, both sides. After a burst 10 dB over: within "
                        + juce::String (worstAt300, 4) + " dB from 500 ms after it on, bit-exact again " + juce::String (exactAfterMs, 0)
                        + " ms after the burst. An impulse comes out at sample 0, unchanged; latency 0");
        }

        beginTest ("what it does to a sustained overload: the release keeps it a gain change, not a clipper (1 kHz sine 6 dB over)");
        {
            ampsim::OutputLimiter limiter;
            limiter.prepare (fs, blockSize);
            const auto x = sine (997.0, juce::Decibels::decibelsToGain (5.0f), (int) fs);
            const auto out = runLimiter (limiter, x, x);
            // The steady state, after 0.5 s: the level, and the energy off the fundamental (a fitted sine removed).
            const auto from = (size_t) (0.5 * fs);
            std::vector<float> tail (out.left.begin() + (long) from, out.left.end());
            double c = 0.0, s = 0.0;
            for (size_t n = 0; n < tail.size(); ++n)
            {
                const auto phase = juce::MathConstants<double>::twoPi * 997.0 * (double) (n + from) / fs;
                c += tail[n] * std::cos (phase);
                s += tail[n] * std::sin (phase);
            }
            c *= 2.0 / (double) tail.size();
            s *= 2.0 / (double) tail.size();
            double residual = 0.0, total = 0.0;
            for (size_t n = 0; n < tail.size(); ++n)
            {
                const auto phase = juce::MathConstants<double>::twoPi * 997.0 * (double) (n + from) / fs;
                const auto e = tail[n] - (c * std::cos (phase) + s * std::sin (phase));
                residual += e * e;
                total += (double) tail[n] * tail[n];
            }
            const auto thdDb = 10.0 * std::log10 (residual / total);
            const auto peakDb = juce::Decibels::gainToDecibels (peakOf ({ tail, tail }));
            expectLessThan (thdDb, -30.0);
            logMessage ("  -> steady state: peaks at " + juce::String (peakDb, 2) + " dBFS (ceiling -1), distortion (everything but the fundamental) "
                        + juce::String (thdDb, 1) + " dB: the gain dips at each peak and recovers 60 ms-slowly between them");
        }

        beginTest ("in the app: on by default with a -1 dBFS ceiling, global (presets and scenes never touch it), the Out meter sees it, nothing allocates");
        {
            AmpSimProcessor p;
            expect (p.parameters.getRawParameterValue ("output_limit_on")->load() >= 0.5f);
            expectWithinAbsoluteError (p.parameters.getRawParameterValue ("output_limit_ceiling")->load(), -1.0f, 1.0e-4f);
            expect (presets::isGlobal ("output_limit_on") && presets::isGlobal ("output_limit_ceiling"));
            expect (! Scenes::isSwitch ("output_limit_on"));
            p.prepareToPlay (fs, blockSize);

            // Empty slots and no cab: the DI straight through, +24 dB of output level on a -6 dBFS DI.
            setParam (p, "output_gain", 24.0f);
            const auto input = guitarDI ((int) (2 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            float peak = 0.0f;
            rtcheck::Counts counts;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                rtcheck::begin();
                p.processBlock (buffer, midi);
                const auto c = rtcheck::end();
                counts.allocations += c.allocations;
                counts.frees += c.frees;
                counts.blockingLocks += c.blockingLocks;
                if (start > (size_t) (0.1 * fs))
                    peak = juce::jmax (peak, buffer.getMagnitude (0, 0, blockSize), buffer.getMagnitude (1, 0, blockSize));
            }
            const auto meter = p.takePeaks();
            expectLessOrEqual (peak, juce::Decibels::decibelsToGain (-1.0f));
            expectGreaterThan (meter.limiterDb, 10.0f);
            expectEquals (counts.allocations + counts.frees + counts.blockingLocks, 0L);

            // Switched off: the same +24 dB goes out unprotected (the reason it's on by default).
            setParam (p, "output_limit_on", 0.0f);
            float unprotected = 0.0f;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                p.processBlock (buffer, midi);
                if (start == (size_t) (0.1 * fs) / blockSize * blockSize)
                    p.takePeaks(); // past the 10 ms bypass fade
                if (start > (size_t) (0.1 * fs))
                    unprotected = juce::jmax (unprotected, buffer.getMagnitude (0, 0, blockSize));
            }
            const auto offMeter = p.takePeaks();
            expectGreaterThan (unprotected, 4.0f);
            expectEquals (offMeter.limiterDb, 0.0f);

            // A preset can't switch it off: loading one with "output_limit_on": 0 leaves it on.
            setParam (p, "output_limit_on", 1.0f);
            auto preset = p.capturePreset ("x");
            preset["parameters"].getDynamicObject()->setProperty ("output_limit_on", 0.0);
            expect (presets::apply (p, preset).ok);
            expect (p.parameters.getRawParameterValue ("output_limit_on")->load() >= 0.5f);

            logMessage ("  -> the DI at +24 dB output level: the output peaks at " + juce::String (juce::Decibels::gainToDecibels (peak), 2) + " dBFS with the limiter (reduction "
                        + juce::String (meter.limiterDb, 1) + " dB reported to the Out meter), " + juce::String (juce::Decibels::gainToDecibels (unprotected), 1)
                        + " dBFS without; 0 allocations, frees, or locks; a preset saying off leaves it on");
        }
    }
};

static OutputLimiterTests outputLimiterTests;

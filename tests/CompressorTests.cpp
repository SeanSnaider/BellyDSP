// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Compressor.h"

namespace
{
using namespace testing;
using ampsim::Compressor;

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/compressor").getChildFile (name);
}

/// A square wave: |x| is constant, so a peak or RMS detector sees one steady level.
std::vector<float> square (const std::vector<std::pair<double, double>>& segments) // {seconds, dBFS}
{
    std::vector<float> x;
    for (const auto& [seconds, db] : segments)
    {
        const auto a = (float) std::pow (10.0, db / 20.0);
        for (int n = 0; n < (int) (seconds * fs); ++n)
            x.push_back (((x.size() / 240) % 2 == 0) ? a : -a); // 100 Hz
    }
    return x;
}

Compressor::Settings plain()
{
    Compressor::Settings s;
    s.thresholdDb = -30.0f;
    s.ratio = 4.0f;
    s.kneeDb = 0.0f;
    s.attackMs = 10.0f;
    s.releaseMs = 200.0f;
    s.autoRelease = false;
    s.autoMakeup = false;
    s.makeupDb = 0.0f;
    s.mix = 1.0f;
    s.sidechainHighPass = false;
    return s;
}

Stereo runCompressor (Compressor& c, const std::vector<float>& left, const std::vector<float>* right = nullptr)
{
    Stereo out { left, right != nullptr ? *right : left };
    for (size_t start = 0; start < left.size(); start += blockSize)
    {
        const auto len = std::min ((size_t) blockSize, left.size() - start);
        float* channels[2] = { out.left.data() + start, out.right.data() + start };
        c.process (juce::dsp::AudioBlock<float> (channels, c.isStereo() ? 2 : 1, len), {});
    }
    return out;
}

/// Gain reduction (dB) applied at each sample, from output over input.
std::vector<double> reductionTrace (const std::vector<float>& in, const std::vector<float>& out)
{
    std::vector<double> r (in.size());
    for (size_t n = 0; n < in.size(); ++n)
        r[n] = 20.0 * std::log10 (std::abs ((double) in[n]) / std::abs ((double) out[n]));
    return r;
}

Compressor::Settings fromJson (const juce::var& v)
{
    Compressor::Settings s;
    s.mode = v["mode"].toString() == "pedal" ? Compressor::Mode::pedal : Compressor::Mode::studio;
    s.detector = v["detector"].toString() == "rms" ? Compressor::Detector::rms : Compressor::Detector::peak;
    s.thresholdDb = (float) (double) v["threshold_db"];
    s.ratio = (float) (double) v["ratio"];
    s.kneeDb = (float) (double) v["knee_db"];
    s.attackMs = (float) (double) v["attack_ms"];
    s.releaseMs = (float) (double) v["release_ms"];
    s.autoRelease = (bool) v["auto_release"];
    s.makeupDb = (float) (double) v["makeup_db"];
    s.autoMakeup = (bool) v["auto_makeup"];
    s.mix = (float) (double) v["mix"];
    s.sidechainHighPass = (bool) v["sidechain_high_pass"];
    s.sidechainHz = (float) (double) v["sidechain_hz"];
    return s;
}

class CompressorTests final : public juce::UnitTest
{
public:
    CompressorTests() : juce::UnitTest ("Compressor", "ampsim") {}

    void runTest() override
    {
        beginTest ("matches the Python reference simulation of both loops (golden renders, limit -100 dB)");
        {
            const auto cases = juce::JSON::parse (fixture ("cases.json"));
            const auto mono = readWav (fixture ("input_mono.wav"));
            const auto stereo = readWav (fixture ("input_stereo.wav"));
            juce::StringArray results;

            for (const auto& name : { "studio_peak", "studio_rms_auto", "pedal", "studio_stereo_linked" })
            {
                const bool isStereo = juce::String (name).contains ("stereo");
                const auto& input = isStereo ? stereo : mono;
                const auto expected = readWav (fixture ("expected_" + juce::String (name) + ".wav"));

                Compressor c (isStereo);
                c.setSettings (fromJson (cases[name]));
                c.prepare (fs, blockSize);

                std::vector<float> left (input.getReadPointer (0), input.getReadPointer (0) + input.getNumSamples());
                std::vector<float> right (input.getReadPointer (input.getNumChannels() - 1),
                                          input.getReadPointer (input.getNumChannels() - 1) + input.getNumSamples());
                const auto out = runCompressor (c, left, &right);

                auto worst = -400.0;
                for (int ch = 0; ch < (isStereo ? 2 : 1); ++ch)
                {
                    std::vector<float> e (expected.getReadPointer (ch), expected.getReadPointer (ch) + expected.getNumSamples());
                    worst = std::max (worst, relativeErrorDb (ch == 0 ? out.left : out.right, e));
                }
                expectLessThan (worst, -100.0);
                results.add (juce::String (name) + " " + juce::String (worst, 1) + " dB");
            }
            logMessage ("  -> C++ vs Python (tests/fixtures/compressor, from prototypes/compressor.py): " + results.joinIntoString ("; "));
        }

        beginTest ("static curve: measured steady-state levels follow the threshold, ratio, and knee");
        {
            std::vector<PlotSeries> series;
            juce::StringArray results;
            double worst = 0.0;
            int colour = 0;

            for (const auto& [ratio, knee] : std::vector<std::pair<float, float>> { { 2.0f, 0.0f }, { 4.0f, 6.0f }, { 10.0f, 12.0f } })
            {
                PlotSeries analytic { "R " + juce::String (ratio, 0) + ":1, knee " + juce::String (knee, 0) + " dB", {}, {}, plotColour (colour), 2.0f };
                PlotSeries measured { "", {}, {}, plotColour (colour++), 5.0f };
                for (double x = -60.0; x <= 0.0; x += 1.0)
                {
                    analytic.x.push_back (x);
                    analytic.y.push_back (Compressor::staticCurveDb (x, -30.0, ratio, knee));
                }
                for (double x = -54.0; x <= 0.0; x += 3.0)
                {
                    auto s = plain();
                    s.ratio = ratio;
                    s.kneeDb = knee;
                    Compressor c (false);
                    c.setSettings (s);
                    c.prepare (fs, blockSize);
                    const auto in = square ({ { 0.6, x } });
                    const auto out = runCompressor (c, in).left;
                    const auto y = toDb (std::abs (out.back()));
                    const auto expected = Compressor::staticCurveDb (x, -30.0, ratio, knee);
                    worst = std::max (worst, std::abs (y - expected));
                    measured.x.push_back (x);
                    measured.y.push_back (y);
                }
                series.push_back (analytic);
                series.push_back (measured);
            }
            expectLessThan (worst, 0.01);

            PlotOptions o;
            o.title = "Compressor static curves, threshold -30 dB (lines: formula; dots: measured)";
            o.xLabel = "Input level (dBFS)";
            o.yLabel = "Output level (dBFS)";
            o.xMin = -60.0; o.xMax = 0.0; o.yMin = -60.0; o.yMax = 0.0;
            const auto png = proofDir().getChildFile ("compressor_static_curves.png");
            // Draw the measured points as short strokes: duplicate each point slightly to the right.
            for (auto& s : series)
                if (s.name.isEmpty())
                {
                    PlotSeries dots { "", {}, {}, s.colour, 6.0f };
                    for (size_t i = 0; i < s.x.size(); ++i)
                    {
                        dots.x.insert (dots.x.end(), { s.x[i] - 0.25, s.x[i] + 0.25, std::nan ("") });
                        dots.y.insert (dots.y.end(), { s.y[i], s.y[i], std::nan ("") });
                    }
                    s = dots;
                }
            expect (savePlot (png, o, series));
            logMessage ("  -> 3 curves x 19 levels (square waves, 100 Hz): every steady-state output within " + juce::String (worst, 4)
                        + " dB of the formula");
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("the knee joins both straight lines with no jump or kink");
        {
            double worstValue = 0.0, worstSlope = 0.0;
            for (const auto& [t, r, w] : std::vector<std::tuple<double, double, double>> { { -30, 4, 6 }, { -20, 10, 12 }, { -40, 2, 3 } })
            {
                const auto e = 1.0e-7;
                for (auto edge : { t - w / 2, t + w / 2 })
                {
                    const auto below = Compressor::staticCurveDb (edge - e, t, r, w), above = Compressor::staticCurveDb (edge + e, t, r, w);
                    worstValue = std::max (worstValue, std::abs (above - below));
                    const auto slopeBelow = (Compressor::staticCurveDb (edge - e, t, r, w) - Compressor::staticCurveDb (edge - 2 * e, t, r, w)) / e;
                    const auto slopeAbove = (Compressor::staticCurveDb (edge + 2 * e, t, r, w) - Compressor::staticCurveDb (edge + e, t, r, w)) / e;
                    worstSlope = std::max (worstSlope, std::abs (slopeAbove - slopeBelow));
                }
            }
            expectLessThan (worstValue, 1.0e-6);
            expectLessThan (worstSlope, 1.0e-4);
            logMessage ("  -> at both knee edges of 3 settings: largest jump " + juce::String (worstValue, 9) + " dB, largest slope change "
                        + juce::String (worstSlope, 6));
        }

        beginTest ("attack and release are exact time constants (63% of a step)");
        {
            auto s = plain();
            s.ratio = 100.0f;
            Compressor c (false);
            c.setSettings (s);
            c.prepare (fs, blockSize);
            const auto in = square ({ { 0.5, -40.0 }, { 1.0, -10.0 }, { 1.5, -40.0 } });
            const auto out = runCompressor (c, in).left;
            const auto r = reductionTrace (in, out);
            const auto full = -10.0 - Compressor::staticCurveDb (-10.0, -30.0, 100.0, 0.0);

            const auto upStart = (size_t) (0.5 * fs), downStart = (size_t) (1.5 * fs);
            size_t attackAt = upStart, releaseAt = downStart;
            while (r[attackAt] < (1.0 - std::exp (-1.0)) * full)
                ++attackAt;
            while (r[releaseAt] > std::exp (-1.0) * full)
                ++releaseAt;
            const auto attackMs = 1000.0 * (double) (attackAt - upStart) / fs;
            const auto releaseMs = 1000.0 * (double) (releaseAt - downStart) / fs;
            expectWithinAbsoluteError (attackMs, 10.0, 0.05);
            expectWithinAbsoluteError (releaseMs, 200.0, 0.05);

            PlotOptions o;
            o.title = "Gain reduction for a -40 to -10 to -40 dB step (attack 10 ms, release 200 ms)";
            o.xLabel = "Time (ms)";
            o.yLabel = "Gain reduction (dB)";
            o.xMin = 400.0; o.xMax = 2400.0; o.yMin = 0.0; o.yMax = 22.0;
            PlotSeries measured { "measured", {}, {}, plotColour (0), 2.5f }, ideal { "1 - e^(-t/tau), e^(-t/tau)", {}, {}, plotColour (6), 1.5f, true };
            for (size_t n = (size_t) (0.4 * fs); n < (size_t) (2.4 * fs); n += 24)
            {
                const auto ms = 1000.0 * (double) n / fs;
                measured.x.push_back (ms);
                measured.y.push_back (r[n]);
                ideal.x.push_back (ms);
                ideal.y.push_back (n < upStart ? 0.0 : n < downStart ? full * (1.0 - std::exp (-(ms - 500.0) / 10.0)) : full * std::exp (-(ms - 1500.0) / 200.0));
            }
            const auto png = proofDir().getChildFile ("compressor_attack_release.png");
            expect (savePlot (png, o, { measured, ideal }));
            logMessage ("  -> 63% of the " + juce::String (full, 2) + " dB step after " + juce::String (attackMs, 3) + " ms (attack knob 10 ms); back to 37% after "
                        + juce::String (releaseMs, 3) + " ms (release knob 200 ms)");
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("auto release: a short peak recovers fast, seconds of heavy compression recover slowly");
        {
            const auto recovery = [&] (double burstSeconds)
            {
                auto s = plain();
                s.ratio = 10.0f;
                s.attackMs = 5.0f;
                s.autoRelease = true;
                Compressor c (false);
                c.setSettings (s);
                c.prepare (fs, blockSize);
                const auto in = square ({ { 0.2, -50.0 }, { burstSeconds, -10.0 }, { 4.0, -50.0 } });
                const auto r = reductionTrace (in, runCompressor (c, in).left);
                const auto end = (size_t) ((0.2 + burstSeconds) * fs);
                const auto atEnd = r[end - 1];
                size_t n = end;
                while (n < r.size() && r[n] > std::exp (-1.0) * atEnd)
                    ++n;
                return std::pair<double, double> { 1000.0 * (double) (n - end) / fs, atEnd };
            };
            const auto [shortMs, shortReduction] = recovery (0.03);
            const auto [longMs, longReduction] = recovery (3.0);
            expectLessThan (shortMs, 100.0);
            expectGreaterThan (longMs, 1000.0);
            logMessage ("  -> after a 30 ms peak (" + juce::String (shortReduction, 1) + " dB of reduction) the gain recovers to 37% in "
                        + juce::String (shortMs, 0) + " ms; after 3 s of it (" + juce::String (longReduction, 1) + " dB) in " + juce::String (longMs, 0) + " ms");
        }

        beginTest ("parallel mix and auto makeup land where they should");
        {
            auto s = plain();
            s.thresholdDb = -24.0f;
            s.kneeDb = 6.0f;
            s.autoMakeup = true;
            Compressor c (false);
            c.setSettings (s);
            c.prepare (fs, blockSize);
            const auto in = square ({ { 0.5, Compressor::autoMakeupReferenceDb } });
            const auto net = toDb (std::abs (runCompressor (c, in).left.back())) - Compressor::autoMakeupReferenceDb;
            expectWithinAbsoluteError (net, 0.0, 0.001);

            auto half = plain();
            half.mix = 0.5f;
            half.makeupDb = 3.0f;
            Compressor p (false);
            p.setSettings (half);
            p.prepare (fs, blockSize);
            const auto x = square ({ { 0.5, -10.0 } });
            const auto y = std::abs (runCompressor (p, x).left.back());
            const auto g = std::pow (10.0, (Compressor::staticCurveDb (-10.0, -30.0, 4.0, 0.0) + 10.0 + 3.0) / 20.0);
            const auto expected = std::abs ((double) x.back()) * (0.5 + 0.5 * g);
            expectWithinAbsoluteError (y / expected, 1.0, 1.0e-4);
            logMessage ("  -> auto makeup (" + juce::String (Compressor::autoMakeupDb (-24.0, 4.0, 6.0), 2) + " dB) brings a -12 dBFS signal out at "
                        + juce::String (net, 4) + " dB net; mix 50% with +3 dB makeup: output / (0.5 dry + 0.5 compressed) = " + juce::String (y / expected, 6));
        }

        beginTest ("stereo linking: a loud left channel compresses the right equally");
        {
            Compressor c (true);
            c.setSettings (plain());
            c.prepare (fs, blockSize);
            const auto left = square ({ { 0.5, -10.0 } });
            const auto right = square ({ { 0.5, -40.0 } });
            const auto out = runCompressor (c, left, &right);
            const auto gainLeft = toDb (std::abs (out.left.back() / left.back()));
            const auto gainRight = toDb (std::abs (out.right.back() / right.back()));
            expectWithinAbsoluteError (gainRight, gainLeft, 1.0e-4);
            expectLessThan (gainRight, -10.0);
            logMessage ("  -> left at -10 dB, right at -40 dB (below threshold): both get " + juce::String (gainLeft, 2) + " / " + juce::String (gainRight, 2) + " dB");
        }

        beginTest ("pedal mode (feedback) settles on the same static ratio");
        {
            juce::StringArray results;
            double worst = 0.0;
            for (double x = -40.0; x <= 0.0; x += 10.0)
            {
                auto s = plain();
                s.mode = Compressor::Mode::pedal;
                Compressor c (false);
                c.setSettings (s);
                c.prepare (fs, blockSize);
                const auto in = square ({ { 1.0, x } });
                const auto y = toDb (std::abs (runCompressor (c, in).left.back()));
                const auto expected = Compressor::staticCurveDb (x, -30.0, 4.0, 0.0);
                worst = std::max (worst, std::abs (y - expected));
                results.add (juce::String (x, 0) + " -> " + juce::String (y, 2));
            }
            expectLessThan (worst, 0.01);
            logMessage ("  -> threshold -30 dB, 4:1, feedback: " + results.joinIntoString (", ") + " dB (largest error " + juce::String (worst, 4) + " dB)");
        }

        beginTest ("meters report gain reduction and levels; a threshold sweep doesn't step");
        {
            Compressor c (false);
            c.setSettings (plain());
            c.prepare (fs, blockSize);
            runCompressor (c, square ({ { 0.5, -10.0 } }));
            const auto expectedReduction = -10.0 - Compressor::staticCurveDb (-10.0, -30.0, 4.0, 0.0);
            expectWithinAbsoluteError ((double) c.getGainReductionDb(), expectedReduction, 0.01);
            expectWithinAbsoluteError ((double) c.getInputPeakDb(), -10.0, 0.01);
            expectWithinAbsoluteError ((double) c.getOutputPeakDb(), -10.0 - expectedReduction, 0.01);

            // A 1 kHz tone while the threshold moves from -30 to -6 dB: the gain glides over 20 ms.
            Compressor z (false);
            z.setSettings (plain());
            z.prepare (fs, blockSize);
            const auto tone = sine (1000.0, 0.3, (int) fs);
            auto out = tone;
            for (size_t start = 0; start < tone.size(); start += blockSize)
            {
                if (start == (size_t) (0.5 * fs) / blockSize * blockSize)
                {
                    auto s = plain();
                    s.thresholdDb = -6.0f;
                    z.setSettings (s);
                }
                float* channels[1] = { out.data() + start };
                z.process (juce::dsp::AudioBlock<float> (channels, 1, std::min ((size_t) blockSize, tone.size() - start)), {});
            }
            const auto steady = maxStep (out, (size_t) (0.3 * fs), (size_t) (0.45 * fs));
            const auto during = maxStep (out, (size_t) (0.5 * fs), (size_t) (0.6 * fs));
            const auto settled = maxStep (out, (size_t) (0.8 * fs), (size_t) (0.95 * fs));
            expectLessThan (during, settled * 1.01);
            logMessage ("  -> meters: reduction " + juce::String (c.getGainReductionDb(), 2) + " dB (expected " + juce::String (expectedReduction, 2)
                        + "), input " + juce::String (c.getInputPeakDb(), 2) + " dBFS, output " + juce::String (c.getOutputPeakDb(), 2) + " dBFS");
            logMessage ("  -> threshold -30 -> -6 dB under a tone: largest sample step " + juce::String (during, 4) + " during the glide, "
                        + juce::String (steady, 4) + " before, " + juce::String (settled, 4) + " after (no step beyond the louder tone's own)");
        }
    }
};

CompressorTests compressorTests;
} // namespace

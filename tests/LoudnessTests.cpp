// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TestHelpers.h"
#include "dsp/Loudness.h"
#include "dsp/ReferenceSignals.h"
#include "dsp/Svf.h"

namespace
{
using namespace testing;

/// A 1 kHz sine made of segments: {seconds, level in dBFS (peak)}. The EBU Tech 3341 test signals.
std::vector<float> toneSegments (std::initializer_list<std::pair<double, double>> segments)
{
    std::vector<float> x;
    double phase = 0.0;

    for (const auto& [seconds, levelDb] : segments)
    {
        const auto amplitude = std::pow (10.0, levelDb / 20.0);
        for (int n = 0; n < (int) (seconds * fs); ++n)
        {
            x.push_back ((float) (amplitude * std::sin (phase)));
            phase += juce::MathConstants<double>::twoPi * 1000.0 / fs;
        }
    }

    return x;
}

double stereoLoudness (const std::vector<float>& x)
{
    return ampsim::loudness::integrated ({ x.data(), x.data() }, (int) x.size(), fs);
}

class LoudnessTests final : public juce::UnitTest
{
public:
    LoudnessTests() : juce::UnitTest ("Loudness (BS.1770)", "ampsim") {}

    void runTest() override
    {
        beginTest ("K-weighting coefficients at 48 kHz equal the values published in BS.1770-4");
        {
            const auto pre = ampsim::loudness::preFilter (48000.0);
            const auto rlb = ampsim::loudness::rlbFilter (48000.0);
            const double published[] = { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585,
                                         1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
            const double ours[] = { pre.b0, pre.b1, pre.b2, pre.a1, pre.a2, rlb.b0, rlb.b1, rlb.b2, rlb.a1, rlb.a2 };

            double worst = 0.0;
            for (int i = 0; i < 10; ++i)
                worst = std::max (worst, std::abs (ours[i] - published[i]));

            expectLessThan (worst, 1.0e-10);
            logMessage ("  -> largest difference from the standard's 10 coefficients: " + juce::String (worst, 14));
        }

        beginTest ("EBU Tech 3341 test signals read their specified loudness (tolerance +-0.1 LU)");
        {
            struct Case { const char* name; std::vector<float> signal; double expected; };
            const Case cases[] = {
                { "case 1: 1 kHz at -23 dBFS",                     toneSegments ({ { 20.0, -23.0 } }), -23.0 },
                { "case 2: 1 kHz at -33 dBFS",                     toneSegments ({ { 20.0, -33.0 } }), -33.0 },
                { "case 3: -36/-23/-36 dBFS (relative gate)",      toneSegments ({ { 10.0, -36.0 }, { 60.0, -23.0 }, { 10.0, -36.0 } }), -23.0 },
                { "case 4: -72/-36/-23/-36/-72 (both gates)",      toneSegments ({ { 10.0, -72.0 }, { 10.0, -36.0 }, { 60.0, -23.0 }, { 10.0, -36.0 }, { 10.0, -72.0 } }), -23.0 },
                { "case 5: -26/-20/-26 dBFS",                      toneSegments ({ { 20.0, -26.0 }, { 20.1, -20.0 }, { 20.0, -26.0 } }), -23.0 },
            };

            juce::StringArray results;
            for (const auto& c : cases)
            {
                const auto measured = stereoLoudness (c.signal);
                expectWithinAbsoluteError (measured, c.expected, 0.1, c.name);
                results.add (juce::String (c.name) + " = " + juce::String (measured, 2) + " LUFS (spec " + juce::String (c.expected, 1) + ")");
            }

            logMessage ("  -> " + results.joinIntoString ("; "));
        }

        beginTest ("mono is one channel's power (3 LU below the same tone in both stereo channels), and silence reads -inf");
        {
            const auto tone = toneSegments ({ { 10.0, -23.0 } });
            const auto mono = ampsim::loudness::integratedMono (tone.data(), (int) tone.size(), fs);
            const std::vector<float> quiet ((size_t) fs * 2, 0.0f);
            const auto silent = ampsim::loudness::integratedMono (quiet.data(), (int) quiet.size(), fs);

            expectWithinAbsoluteError (mono, -26.01, 0.1);
            expect (! std::isfinite (silent) && silent < 0.0);
            logMessage ("  -> mono 1 kHz at -23 dBFS: " + juce::String (mono, 2) + " LUFS; silence: " + juce::String (silent));
        }

        beginTest ("FFT convolution equals direct convolution");
        {
            const auto x = whiteNoise (20000, 0.5f, 31);
            const auto h = whiteNoise (777, 0.3f, 32);
            const auto fast = ampsim::loudness::fftConvolve (x, h.data(), (int) h.size());
            const auto direct = directConvolution (x, std::vector<double> (h.begin(), h.end()));
            const auto error = relativeErrorDb (fast, direct);

            expectLessThan (error, -100.0);
            logMessage ("  -> 20000 samples through a 777-tap filter: " + dB (error) + " relative error");
        }

        beginTest ("the reference signals are what they claim");
        {
            const auto di = ampsim::referenceGuitarDI ((int) (4.0 * fs));
            const auto peak = toDb (std::abs (*std::max_element (di.begin(), di.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); })));
            const auto diLoudness = ampsim::loudness::integratedMono (di.data(), (int) di.size(), fs);

            // Pink noise (used for comparisons): equal power per octave, so two octave bands carry about
            // the same energy.
            const auto noise = pinkNoise ((int) (10.0 * fs));
            const auto bandPower = [&] (double lo, double hi)
            {
                ampsim::Svf highpass, lowpass;
                highpass.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, lo, 0.7071, 0.0, fs));
                lowpass.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::lowpass, hi, 0.7071, 0.0, fs));
                double sum = 0.0;
                for (auto v : noise)
                {
                    const auto y = lowpass.processSample (highpass.processSample (v));
                    sum += y * y;
                }
                return 10.0 * std::log10 (sum / (double) noise.size());
            };
            const auto lowOctave = bandPower (250.0, 500.0);
            const auto highOctave = bandPower (4000.0, 8000.0);

            expectWithinAbsoluteError (peak, -6.02, 0.01);
            expectWithinAbsoluteError (lowOctave - highOctave, 0.0, 0.5);
            logMessage ("  -> reference guitar DI: peak " + juce::String (peak, 2) + " dBFS, loudness " + juce::String (diLoudness, 1)
                        + " LUFS; pink noise: 250-500 Hz octave vs. 4-8 kHz octave differ by " + juce::String (lowOctave - highOctave, 2) + " dB");
        }

        beginTest ("the IR loudness match computed from the IR's energy equals measuring real white noise through it");
        {
            // The reference: the original method, 4 s of white noise measured with the BS.1770 meter
            // before and after the IR (a stereo IR against noise in both channels).
            const auto measured = [] (const std::vector<std::vector<float>>& channels)
            {
                const auto noise = ampsim::referenceWhiteNoise ((int) (4.0 * fs));
                std::vector<std::vector<float>> through;
                std::vector<const float*> before, after;
                for (const auto& h : channels)
                {
                    through.push_back (ampsim::loudness::fftConvolve (noise, h.data(), (int) h.size()));
                    before.push_back (noise.data());
                }
                for (const auto& t : through)
                    after.push_back (t.data());
                return ampsim::loudness::integrated (before, (int) noise.size(), fs) - ampsim::loudness::integrated (after, (int) noise.size(), fs);
            };
            const auto toFloat = [] (const std::vector<double>& h) { return std::vector<float> (h.begin(), h.end()); };
            const auto decayingNoise = [] (int length, double tau, juce::int64 seed)
            {
                auto h = whiteNoise (length, 1.0f, seed);
                for (size_t n = 0; n < h.size(); ++n)
                    h[n] *= (float) std::exp (-(double) n / (tau * fs));
                return h;
            };

            const std::vector<std::pair<juce::String, std::vector<std::vector<float>>>> irs {
                { "stock cab", { toFloat (syntheticCabIR (4096)) } },
                { "dark cab", { toFloat (syntheticCabIR (4096, -4.0, 2500.0)) } },
                { "bright cab", { toFloat (syntheticCabIR (4096, 10.0, 9000.0)) } },
                { "quiet cab (-30 dB)", { [&] { auto h = toFloat (syntheticCabIR (2048)); for (auto& v : h) v *= 0.0316f; return h; }() } },
                { "1 s stereo room", { decayingNoise (48000, 0.15, 5), decayingNoise (48000, 0.15, 6) } },
            };

            juce::StringArray results;
            double worst = 0.0;
            for (const auto& [name, channels] : irs)
            {
                std::vector<const float*> pointers;
                for (const auto& h : channels)
                    pointers.push_back (h.data());
                const auto analyticDb = 20.0 * std::log10 (ampsim::loudness::whiteNoiseMatchingGain (pointers, (int) channels[0].size(), fs));
                const auto measuredDb = measured (channels);
                worst = std::max (worst, std::abs (analyticDb - measuredDb));
                results.add (name + " " + juce::String (analyticDb, 2) + " vs " + juce::String (measuredDb, 2) + " dB");
            }
            expectLessThan (worst, 0.1);

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto room = irs.back().second;
            ampsim::loudness::whiteNoiseMatchingGain ({ room[0].data(), room[1].data() }, 48000, fs);
            const auto analyticMs = juce::Time::getMillisecondCounterHiRes() - t0;
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            measured (room);
            const auto measuredMs = juce::Time::getMillisecondCounterHiRes() - t1;

            logMessage ("  -> computed vs measured gain: " + results.joinIntoString ("; ") + " (largest difference " + juce::String (worst, 3) + " dB)");
            logMessage ("  -> 1 s stereo room: computed in " + juce::String (analyticMs, 2) + " ms, measured in " + juce::String (measuredMs, 1) + " ms");
        }
    }
};

LoudnessTests loudnessTests;
} // namespace

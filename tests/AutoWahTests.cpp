// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/AutoWah.h"
#include "dsp/FftDouble.h"
#include "dsp/Svf.h"

#include <complex>
#include <cstring>
#include <map>

namespace
{
using namespace testing;
using ampsim::AutoWah;
using ampsim::Svf;

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/autowah").getChildFile (name);
}

/// Feeds a mono signal through the block in fixed-size buffers, with no DI (so it detects its own input) or with
/// `di` as the context's DI snapshot.
std::vector<float> runWah (AutoWah& w, const std::vector<float>& input, int bufferSize = blockSize, const std::vector<float>* di = nullptr)
{
    auto out = input;
    for (size_t start = 0; start < input.size(); start += (size_t) bufferSize)
    {
        const auto len = std::min ((size_t) bufferSize, input.size() - start);
        float* channels[1] = { out.data() + start };
        ampsim::BlockContext context;
        if (di != nullptr)
        {
            context.di = di->data() + start;
            context.numSamples = (int) len;
        }
        w.process (juce::dsp::AudioBlock<float> (channels, 1, len), context);
    }
    return out;
}

std::vector<float> render (const AutoWah::Settings& s, const std::vector<float>& input, int bufferSize = blockSize)
{
    AutoWah w;
    w.setSettings (s);
    w.prepare (fs, blockSize);
    return runWah (w, input, bufferSize);
}

/// A guitar-like note: harmonics 1 to `harmonics` at 1/k, decaying with time constant `decay` seconds (0: sustained).
std::vector<float> note (double f0, double levelDb, double seconds, double decay, int harmonics = 16)
{
    std::vector<float> x ((size_t) (seconds * fs));
    const auto a = std::pow (10.0, levelDb / 20.0) / 2.0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const auto t = (double) n / fs;
        double v = 0.0;
        for (int k = 1; k <= harmonics && k * f0 < 0.45 * fs; ++k)
            v += std::sin (juce::MathConstants<double>::twoPi * f0 * k * t + 0.7 * k) / k;
        x[n] = (float) (a * v * (decay > 0.0 ? std::exp (-t / decay) : 1.0));
    }
    return x;
}

/// The power spectrum (Hann window, zero-padded to a power of two): bin k is at k fs / N.
std::vector<double> powerSpectrum (const std::vector<float>& x, size_t start, size_t length, int& order)
{
    order = 1;
    while ((size_t) 1 << order < length)
        ++order;
    ampsim::FftDouble fft (order);
    std::vector<std::complex<double>> data ((size_t) fft.size());
    for (size_t n = 0; n < length; ++n)
    {
        const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) n / (double) length);
        data[n] = window * (double) x[start + n];
    }
    fft.forward (data);
    std::vector<double> power ((size_t) fft.size() / 2 + 1);
    for (size_t k = 0; k < power.size(); ++k)
        power[k] = std::norm (data[k]);
    return power;
}

double binHz (int order) { return fs / (double) ((size_t) 1 << order); }

/// The spectral centroid (Hz) of x[start, start + length): the power-weighted mean frequency.
double centroid (const std::vector<float>& x, size_t start, size_t length)
{
    int order = 0;
    const auto p = powerSpectrum (x, start, length, order);
    double moment = 0.0, total = 0.0;
    for (size_t k = 1; k < p.size(); ++k)
    {
        moment += p[k] * (double) k * binHz (order);
        total += p[k];
    }
    return moment / total;
}

/// The energy above `hz`, in dB relative to the whole signal's.
double energyAboveDb (const std::vector<float>& x, double hz)
{
    int order = 0;
    const auto p = powerSpectrum (x, 0, x.size(), order);
    double above = 0.0, total = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
    {
        total += p[k];
        if ((double) k * binHz (order) > hz)
            above += p[k];
    }
    return 10.0 * std::log10 (above / total);
}

double energyAbove (const std::vector<float>& x, double hz)
{
    int order = 0;
    const auto p = powerSpectrum (x, 0, x.size(), order);
    double above = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
        if ((double) k * binHz (order) > hz)
            above += p[k];
    return above;
}

AutoWah::Settings fromJson (const juce::var& v)
{
    AutoWah::Settings s;
    s.sensitivity = (float) (double) v["sensitivity"];
    s.freqLowHz = (float) (double) v["freq_low"];
    s.freqHighHz = (float) (double) v["freq_high"];
    s.q = (float) (double) v["q"];
    s.attackMs = (float) (double) v["attack_ms"];
    s.releaseMs = (float) (double) v["release_ms"];
    s.direction = v["direction"].toString() == "down" ? AutoWah::Direction::down : AutoWah::Direction::up;
    s.mix = (float) (double) v["mix"];
    return s;
}

/// A square wave at 100 Hz: |x| is constant, so the envelope follower sees one exact step.
std::vector<float> squareSteps (const std::vector<std::pair<double, double>>& segments) // {seconds, amplitude}
{
    std::vector<float> x;
    for (const auto& [seconds, amplitude] : segments)
        for (int n = 0; n < (int) (seconds * fs); ++n)
            x.push_back (((x.size() / 240) % 2 == 0) ? (float) amplitude : (float) -amplitude);
    return x;
}

class AutoWahTests final : public juce::UnitTest
{
public:
    AutoWahTests() : juce::UnitTest ("Auto-wah", "ampsim") {}

    void runTest() override
    {
        beginTest ("matches the Python reference sample by sample (golden renders, limit -100 dB)");
        {
            const auto json = juce::JSON::parse (fixture ("cases.json"));
            expectEquals ((int) json["interval"], AutoWah::redesignInterval, "the fixtures were rendered with another redesign interval");
            const auto cases = json["cases"];
            const auto input = readWav (fixture ("input_mono.wav"));
            const std::vector<float> x (input.getReadPointer (0), input.getReadPointer (0) + input.getNumSamples());
            juce::StringArray results;
            int count = 0;

            for (const auto* name : { "defaults", "down", "high_q_fast", "narrow", "low_sensitivity" })
            {
                expect (cases.hasProperty (name), juce::String ("missing case ") + name);
                const auto expected = readWav (fixture ("expected_" + juce::String (name) + ".wav"));
                const std::vector<float> e (expected.getReadPointer (0), expected.getReadPointer (0) + expected.getNumSamples());
                const auto y = render (fromJson (cases[name]), x);
                const auto error = relativeErrorDb (y, e);
                expectLessThan (error, -100.0, name);
                results.add (juce::String (name) + " " + juce::String (error, 1) + " dB");
                ++count;
            }
            expectEquals (count, 5);
            logMessage ("  -> C++ vs Python (tests/fixtures/autowah, " + juce::String ((double) x.size() / fs, 1) + " s, interval "
                        + juce::String (AutoWah::redesignInterval) + "): " + results.joinIntoString ("; "));
        }

        beginTest ("the sweep: c = 0 is Low, c = 1 is High, c = 0.5 the geometric mean; the dB mapping's ends");
        {
            double worst = 0.0;
            for (const auto& [lo, hi] : std::vector<std::pair<double, double>> { { 350.0, 2200.0 }, { 150.0, 4000.0 }, { 800.0, 800.0 }, { 600.0, 900.0 } })
            {
                worst = std::max (worst, std::abs (AutoWah::centreFrequency (0.0, lo, hi) / lo - 1.0));
                worst = std::max (worst, std::abs (AutoWah::centreFrequency (1.0, lo, hi) / hi - 1.0));
                worst = std::max (worst, std::abs (AutoWah::centreFrequency (0.5, lo, hi) / std::sqrt (lo * hi) - 1.0));
            }
            expectLessThan (worst, 1.0e-9);
            // Equal steps of c are equal intervals: a quarter of the way is a quarter of the octaves.
            const auto octaves = std::log2 (2200.0 / 350.0);
            const auto quarter = std::log2 (AutoWah::centreFrequency (0.25, 350.0, 2200.0) / 350.0) / octaves;
            expectWithinAbsoluteError (quarter, 0.25, 1.0e-12);
            // Low above High (only reachable by calling the formula directly): it sweeps downwards, no sorting.
            const auto backwards = AutoWah::centreFrequency (1.0, 900.0, 600.0);
            expectWithinAbsoluteError (backwards, 600.0, 1.0e-9);

            // The level mapping: closed at the floor (-40 dBFS at 50 %), open 30 dB above it, Down inverted.
            const auto at = [] (double db) { return std::pow (10.0, db / 20.0) - AutoWah::levelOffset; };
            const auto closed = AutoWah::controlFor (at (-40.0), 50.0, AutoWah::Direction::up);
            const auto open = AutoWah::controlFor (at (-10.0), 50.0, AutoWah::Direction::up);
            const auto middle = AutoWah::controlFor (at (-25.0), 50.0, AutoWah::Direction::up);
            const auto downMiddle = AutoWah::controlFor (at (-31.0), 50.0, AutoWah::Direction::down);
            const auto silence = AutoWah::controlFor (0.0, 100.0, AutoWah::Direction::up);
            const auto floorAtZero = AutoWah::controlFor (at (-20.0), 0.0, AutoWah::Direction::up);
            const auto floorAtFull = AutoWah::controlFor (at (-60.0), 100.0, AutoWah::Direction::up);
            expectWithinAbsoluteError (closed, 0.0, 1.0e-9);
            expectWithinAbsoluteError (open, 1.0, 1.0e-9);
            expectWithinAbsoluteError (middle, 0.5, 1.0e-9);
            expectWithinAbsoluteError (downMiddle, 1.0 - 0.3, 1.0e-9);
            expectEquals (silence, 0.0);
            expectWithinAbsoluteError (floorAtZero, 0.0, 1.0e-9);
            expectWithinAbsoluteError (floorAtFull, 0.0, 1.0e-9);
            logMessage ("  -> 4 Low/High pairs: c = 0, 1, 0.5 within " + juce::String (worst, 17) + " (relative) of Low, High, sqrt(Low High); c = 0.25 is "
                        + juce::String (quarter, 12) + " of the octaves; Low 900 > High 600 sweeps down to " + juce::String (backwards, 6) + " Hz");
            logMessage ("  -> sensitivity 50 %: -40 dBFS gives c = " + juce::String (closed, 9) + ", -25 gives " + juce::String (middle, 9) + ", -10 gives "
                        + juce::String (open, 9) + "; Down at -31 gives " + juce::String (downMiddle, 9) + "; the floor sits at -20 dBFS at 0 % (c = "
                        + juce::String (floorAtZero, 9) + ") and -60 at 100 % (c = " + juce::String (floorAtFull, 9) + "); silence gives " + juce::String (silence));
        }

        beginTest ("the band-pass: 0 dB at fc, -3 dB points fc/Q apart (bilinear-warped); the block applies it with its level hold on top");
        {
            juce::StringArray results;
            const auto halfPowerDb = 10.0 * std::log10 (0.5);
            double worstPeak = 0.0, worstEdge = 0.0, worstWidth = 0.0, worstWarped = 0.0;
            for (const auto fc : { 350.0, 1000.0, 2200.0, 4000.0 })
                for (const auto q : { 1.0, 4.0, 12.0 })
                {
                    const auto c = Svf::design (Svf::Type::bandpass, fc, q, 0.0, fs);
                    const auto gainDb = [&] (double f) { return 20.0 * std::log10 (std::abs (Svf::responseAt (c, f, fs))); };
                    worstPeak = std::max (worstPeak, std::abs (gainDb (fc)));

                    // The -3 dB points by bisection on each side of fc.
                    const auto edge = [&] (double lo, double hi)
                    {
                        const bool rising = gainDb (lo) < gainDb (hi);
                        for (int i = 0; i < 200; ++i)
                        {
                            const auto mid = 0.5 * (lo + hi);
                            ((gainDb (mid) < halfPowerDb) == rising ? lo : hi) = mid;
                        }
                        return 0.5 * (lo + hi);
                    };
                    const auto f1 = edge (1.0, fc), f2 = edge (fc, 0.4999 * fs);
                    worstEdge = std::max ({ worstEdge, std::abs (gainDb (f1) - halfPowerDb), std::abs (gainDb (f2) - halfPowerDb) });

                    // The analog prototype's -3 dB points: Omega2 - Omega1 = 1/Q and Omega1 Omega2 = 1, mapped back
                    // through the prewarp f = fs/pi atan(g Omega). So width fc/Q exactly in the analog domain.
                    const auto k = 1.0 / q;
                    const auto o2 = 0.5 * (k + std::sqrt (k * k + 4.0)), o1 = 1.0 / o2;
                    const auto warp = [&] (double omega) { return fs / juce::MathConstants<double>::pi * std::atan (c.g * omega); };
                    worstWarped = std::max ({ worstWarped, std::abs (f1 - warp (o1)), std::abs (f2 - warp (o2)) });
                    const auto widthError = (f2 - f1) / (fc / q) - 1.0;
                    worstWidth = std::max (worstWidth, std::abs (widthError));
                    if (q == 4.0)
                        results.add (juce::String (fc, 0) + " Hz: " + juce::String (f1, 1) + " to " + juce::String (f2, 1) + " Hz, width "
                                     + juce::String (f2 - f1, 1) + " (fc/Q " + juce::String (fc / q, 1) + ", " + juce::String (100.0 * widthError, 2) + " %)");
                }
            expectLessThan (worstPeak, 1.0e-9);
            expectLessThan (worstEdge, 1.0e-6);
            expectLessThan (worstWarped, 1.0e-6);
            expectLessThan (worstWidth, 0.10); // the bilinear warp narrows it near Nyquist: 4 kHz at Q 1 is the worst case

            // Through the block: Low = High = 800 Hz fixes fc whatever the envelope does, so sines measure the filter, raised
            // by the level hold (AutoWah.h, 6): makeupDb at Q 4.
            AutoWah::Settings s;
            s.freqLowHz = 800.0f;
            s.freqHighHz = 800.0f;
            s.q = 4.0f;
            const auto k = 0.25;
            const auto o2 = 0.5 * (k + std::sqrt (k * k + 4.0));
            const auto g = std::tan (juce::MathConstants<double>::pi * 800.0 / fs);
            const auto upper = fs / juce::MathConstants<double>::pi * std::atan (g * o2), lower = fs / juce::MathConstants<double>::pi * std::atan (g / o2);
            const auto measure = [&] (double f)
            {
                const auto x = sine (f, 0.25, (int) fs);
                const auto y = render (s, x);
                return toDb (rms (y.data() + 24000, 24000) / rms (x.data() + 24000, 24000));
            };
            const auto hold = 20.0 * std::log10 (AutoWah::wetGain (4.0));
            const auto atFc = measure (800.0) - hold, atLower = measure (lower) - hold, atUpper = measure (upper) - hold;
            expectWithinAbsoluteError (hold, AutoWah::makeupDb, 1.0e-9);
            expectWithinAbsoluteError (atFc, 0.0, 0.01);
            expectWithinAbsoluteError (atLower, halfPowerDb, 0.02);
            expectWithinAbsoluteError (atUpper, halfPowerDb, 0.02);
            // The hold rises as sqrt(Q): +10 log10(Q / 4) dB.
            const auto holdAt12 = 20.0 * std::log10 (AutoWah::wetGain (12.0)), holdAt1 = 20.0 * std::log10 (AutoWah::wetGain (1.0));
            expectWithinAbsoluteError (holdAt12 - hold, 10.0 * std::log10 (3.0), 1.0e-9);
            expectWithinAbsoluteError (holdAt1 - hold, 10.0 * std::log10 (0.25), 1.0e-9);

            logMessage ("  -> 4 centres x Q 1, 4, 12: peak within " + juce::String (worstPeak, 12) + " dB of 0; the -3 dB points within "
                        + juce::String (worstWarped, 9) + " Hz of the prewarped analog prediction; width vs fc/Q off by at most "
                        + juce::String (100.0 * worstWidth, 2) + " % (the warp near Nyquist)");
            logMessage ("  -> Q 4: " + results.joinIntoString ("; "));
            logMessage ("  -> through the block at 800 Hz, Q 4 (sines, 0.5 s settled), less the level hold (" + juce::String (hold, 2) + " dB; "
                        + juce::String (holdAt1, 2) + " at Q 1, " + juce::String (holdAt12, 2) + " at Q 12): " + juce::String (atFc, 4) + " dB at fc, "
                        + juce::String (atLower, 4) + " dB at " + juce::String (lower, 1) + " Hz, " + juce::String (atUpper, 4) + " dB at " + juce::String (upper, 1) + " Hz");
        }

        beginTest ("the envelope: 63 % of a step in Attack, back to 37 % in Release (within 5 %)");
        {
            juce::StringArray results;
            for (const auto& [attackMs, releaseMs] : std::vector<std::pair<float, float>> { { 5.0f, 150.0f }, { 1.0f, 20.0f }, { 50.0f, 500.0f }, { 12.0f, 80.0f } })
            {
                AutoWah::Settings s;
                s.attackMs = attackMs;
                s.releaseMs = releaseMs;
                AutoWah w;
                w.setSettings (s);
                w.prepare (fs, blockSize);
                const auto amplitude = 0.25;
                const auto x = squareSteps ({ { 0.1, 0.0 }, { 0.6, amplitude }, { 2.0, 0.0 } });
                std::vector<double> e (x.size());
                std::vector<float> one (1);
                for (size_t n = 0; n < x.size(); ++n)
                {
                    one[0] = x[n];
                    runWah (w, one, 1);
                    e[n] = w.getEnvelope();
                }
                const auto upStart = (size_t) (0.1 * fs), downStart = (size_t) (0.7 * fs);
                // Index n holds the envelope after sample n, so k samples of the step in are at upStart + k - 1.
                size_t up = upStart, down = downStart;
                while (e[up] < (1.0 - std::exp (-1.0)) * amplitude)
                    ++up;
                const auto atEnd = e[downStart - 1];
                while (e[down] > std::exp (-1.0) * atEnd)
                    ++down;
                const auto measuredAttack = 1000.0 * (double) (up - upStart + 1) / fs;
                const auto measuredRelease = 1000.0 * (double) (down - downStart + 1) / fs;
                expectWithinAbsoluteError (measuredAttack / attackMs, 1.0, 0.05);
                expectWithinAbsoluteError (measuredRelease / releaseMs, 1.0, 0.05);
                results.add ("attack " + juce::String (attackMs, 0) + " ms measured " + juce::String (measuredAttack, 3) + " ms, release " + juce::String (releaseMs, 0)
                             + " ms measured " + juce::String (measuredRelease, 3) + " ms");
            }
            logMessage ("  -> a -12 dBFS square step (|x| constant), the envelope read after every sample: " + results.joinIntoString ("; "));
        }

        beginTest ("it wahs: a hard pick sweeps brighter than a soft one with Up, darker with Down");
        {
            const auto soft = note (110.0, -36.0, 0.5, 0.3), hard = note (110.0, -6.0, 0.5, 0.3);
            const size_t start = 0, length = 16384; // the first 341 ms of the note
            juce::StringArray results;
            std::vector<PlotSeries> series;
            int colour = 0;
            double upSoft = 0.0, upHard = 0.0, downSoft = 0.0, downHard = 0.0;
            for (const auto direction : { AutoWah::Direction::up, AutoWah::Direction::down })
            {
                AutoWah::Settings s;
                s.direction = direction;
                const auto ySoft = render (s, soft), yHard = render (s, hard);
                const auto cSoft = centroid (ySoft, start, length), cHard = centroid (yHard, start, length);
                (direction == AutoWah::Direction::up ? upSoft : downSoft) = cSoft;
                (direction == AutoWah::Direction::up ? upHard : downHard) = cHard;

                for (const auto* y : { &ySoft, &yHard })
                {
                    int order = 0;
                    const auto p = powerSpectrum (*y, start, length, order);
                    const auto peak = *std::max_element (p.begin(), p.end());
                    PlotSeries line { juce::String (direction == AutoWah::Direction::up ? "Up, " : "Down, ") + (y == &ySoft ? "soft (-36 dBFS)" : "hard (-6 dBFS)"),
                                      {}, {}, plotColour (colour++), 1.8f, direction == AutoWah::Direction::down };
                    for (size_t k = 1; k < p.size(); ++k)
                    {
                        const auto f = (double) k * binHz (order);
                        if (f < 50.0 || f > 10000.0)
                            continue;
                        line.x.push_back (f);
                        line.y.push_back (10.0 * std::log10 (p[k] / peak + 1.0e-30));
                    }
                    series.push_back (line);
                }
            }
            const auto dry = centroid (hard, start, length);
            expectGreaterThan (upHard, upSoft * 1.5);
            expectLessThan (downHard, downSoft / 1.5);

            PlotOptions o;
            o.title = "Auto-wah output spectra, a 110 Hz note picked soft and hard (defaults; dashed: Down)";
            o.xLabel = "Frequency (Hz)";
            o.yLabel = "Level (dB re each one's peak)";
            o.logX = true;
            o.xMin = 50.0; o.xMax = 10000.0; o.yMin = -90.0; o.yMax = 5.0;
            const auto png = proofDir().getChildFile ("autowah_spectra.png");
            expect (savePlot (png, o, series));
            logMessage ("  -> spectral centroid of the first 341 ms (the dry note: " + juce::String (dry, 0) + " Hz): Up soft " + juce::String (upSoft, 0) + " Hz, hard "
                        + juce::String (upHard, 0) + " Hz; Down soft " + juce::String (downSoft, 0) + " Hz, hard " + juce::String (downHard, 0) + " Hz");
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("Mix 0 is the dry signal, bit for bit");
        {
            AutoWah::Settings s;
            s.mix = 0.0f;
            s.q = 12.0f;
            const auto x = guitarDI ((int) (2.0 * fs));
            const auto y = render (s, x);
            expect (std::memcmp (x.data(), y.data(), x.size() * sizeof (float)) == 0);
            size_t differing = 0;
            for (size_t n = 0; n < x.size(); ++n)
                differing += std::memcmp (&x[n], &y[n], sizeof (float)) != 0 ? 1 : 0;
            logMessage ("  -> 2 s of guitar at Mix 0, Q 12: " + juce::String ((int) differing) + " of " + juce::String ((int) x.size()) + " samples differ (memcmp)");
        }

        beginTest ("nothing blows up: Q 12, the fastest attack, full-scale noise, then silence; reset() and buffer sizes repeat exactly");
        {
            juce::StringArray results;
            for (const auto direction : { AutoWah::Direction::up, AutoWah::Direction::down })
            {
                AutoWah::Settings s;
                s.q = 12.0f;
                s.attackMs = 1.0f;
                s.releaseMs = 20.0f;
                s.sensitivity = 100.0f;
                s.freqLowHz = 150.0f;
                s.freqHighHz = 4000.0f;
                s.direction = direction;
                auto x = whiteNoise ((int) (2.0 * fs), 1.0f, 11);
                x.resize ((size_t) (5.0 * fs), 0.0f);
                const auto y = render (s, x);
                bool finite = true;
                double peak = 0.0;
                for (const auto v : y)
                {
                    finite = finite && std::isfinite (v);
                    peak = std::max (peak, (double) std::abs (v));
                }
                size_t lastNonZero = 0;
                for (size_t n = 0; n < y.size(); ++n)
                    if (y[n] != 0.0f)
                        lastNonZero = n;
                expect (finite);
                // Bounded: under twice the filter's peak gain (its level hold at Q 12) for a full-scale input.
                expectLessThan (peak, 2.0 * AutoWah::wetGain (12.0));
                expectLessThan ((double) lastNonZero / fs, 4.0); // exact zeros for the last second at least
                results.add (juce::String (direction == AutoWah::Direction::up ? "Up" : "Down") + ": peak " + juce::String (toDb (peak), 2) + " dBFS (limit "
                             + juce::String (toDb (2.0 * AutoWah::wetGain (12.0)), 1) + "), every sample finite, exactly 0 from "
                             + juce::String ((double) (lastNonZero + 1) / fs - 2.0, 2) + " s into the silence");
            }
            logMessage ("  -> full-scale white noise for 2 s, then 3 s of silence (Q 12, attack 1 ms, release 20 ms, 150 Hz to 4 kHz, sensitivity 100 %): "
                        + results.joinIntoString ("; "));

            AutoWah::Settings s;
            s.q = 8.0f;
            s.attackMs = 2.0f;
            const auto x = guitarDI ((int) (3.0 * fs));
            AutoWah w;
            w.setSettings (s);
            w.prepare (fs, blockSize);
            const auto first = runWah (w, x);
            w.reset();
            const auto second = runWah (w, x);
            const bool repeats = std::memcmp (first.data(), second.data(), x.size() * sizeof (float)) == 0;
            expect (repeats);
            juce::StringArray sizes;
            bool allSame = true;
            for (const auto size : { 1, 7, 64, 100, 512 })
            {
                const auto y = render (s, x, size);
                const bool same = std::memcmp (first.data(), y.data(), x.size() * sizeof (float)) == 0;
                allSame = allSame && same;
                sizes.add (juce::String (size) + (same ? " identical" : " DIFFERENT"));
            }
            expect (allSame);
            logMessage ("  -> 3 s of guitar, reset(), the same again: " + juce::String (repeats ? "bit-identical" : "DIFFERENT")
                        + "; against 128-sample buffers, buffers of " + sizes.joinIntoString (", ") + " (the redesign interval counts across buffers)");
        }

        beginTest ("no clicks from the knobs: Low, High and Q, then Mix, jumped and swept under a sustained note");
        {
            // A sustained note, so the envelope holds still and only the knobs move. Two runs: the filter's knobs with the
            // wet alone (Mix 100 %), then Mix with the filter at its defaults. Each knob jumps from one end of its range to
            // the other at 0.5 s and back at 1.0 s (the worst case for the smoothing), then glides up over 300 ms from 1.5 s.
            // The largest sample step while they move is held against the same note with the knobs still, at the low
            // end, the middle and the high end (the other blocks' bar: 1.5 x).
            const auto x = note (110.0, -12.0, 3.0, 0.0, 30);
            AutoWah::Settings lowFilter, highFilter, dry, wet;
            lowFilter.freqLowHz = 150.0f;
            lowFilter.freqHighHz = 800.0f;
            lowFilter.q = 1.0f;
            highFilter.freqLowHz = 800.0f;
            highFilter.freqHighHz = 4000.0f;
            highFilter.q = 12.0f;
            dry.mix = 0.0f;
            wet.mix = 1.0f;
            const auto lerp = [] (const AutoWah::Settings& a, const AutoWah::Settings& b, float t)
            {
                auto s = a;
                // Frequencies and Q glide in ratio, like their skewed knobs; mix linearly.
                s.freqLowHz = a.freqLowHz * std::pow (b.freqLowHz / a.freqLowHz, t);
                s.freqHighHz = a.freqHighHz * std::pow (b.freqHighHz / a.freqHighHz, t);
                s.q = a.q * std::pow (b.q / a.q, t);
                s.mix = a.mix + (b.mix - a.mix) * t;
                return s;
            };

            juce::StringArray results;
            for (const auto& [what, from, to] : std::vector<std::tuple<juce::String, AutoWah::Settings, AutoWah::Settings>> {
                     { "Low 150<->800, High 800<->4000, Q 1<->12 (Mix 100 %)", lowFilter, highFilter }, { "Mix 0<->100 %", dry, wet } })
            {
                double still = 0.0;
                juce::StringArray stills;
                for (const auto t : { 0.0f, 0.5f, 1.0f })
                {
                    const auto y = render (lerp (from, to, t), x);
                    const auto step = maxStep (y, (size_t) (0.2 * fs));
                    still = std::max (still, step);
                    stills.add (juce::String (step, 5));
                }

                AutoWah w;
                w.setSettings (from);
                w.prepare (fs, blockSize);
                auto y = x;
                for (size_t start = 0; start < x.size(); start += blockSize)
                {
                    const auto seconds = (double) start / fs;
                    auto s = from;
                    if (seconds >= 0.5 && seconds < 1.0)
                        s = to;
                    else if (seconds >= 1.5)
                        s = lerp (from, to, (float) juce::jlimit (0.0, 1.0, (seconds - 1.5) / 0.3));
                    w.setSettings (s);
                    float* channels[1] = { y.data() + start };
                    w.process (juce::dsp::AudioBlock<float> (channels, 1, std::min ((size_t) blockSize, x.size() - start)), {});
                }
                const auto jumps = maxStep (y, (size_t) (0.45 * fs), (size_t) (1.45 * fs));
                const auto glide = maxStep (y, (size_t) (1.45 * fs), (size_t) (2.0 * fs));
                expectLessThan (jumps, 1.5 * still, what);
                expectLessThan (glide, 1.5 * still, what);
                results.add (what + ": largest step " + juce::String (jumps, 5) + " through the jumps, " + juce::String (glide, 5) + " through the glide; still at the low end, middle, high end "
                             + stills.joinIntoString (", ") + " (limit " + juce::String (1.5 * still, 5) + ")");
            }
            logMessage ("  -> a sustained 110 Hz note (30 harmonics) at -12 dBFS. " + results.joinIntoString ("; "));
        }

        beginTest ("the redesign interval study: the largest interval with no measurable stepping (attack 1 ms, picked notes)");
        {
            // Redesigning every sample is the reference. Stepping (fc held for a chunk, then jumped) would show up as
            // larger sample steps and as extra energy above the notes' own top end. "Measurable" here: the output's
            // energy above 6 kHz more than 0.1 dB above the per-sample render's, or its largest step more than 1 % larger.
            // (Stepping adds energy; a slightly later sweep can also take a little away, which isn't stepping.) Two
            // signals: the golden fixture's picked notes (harmonics below 3 kHz, so only the onsets and a -80 dBFS
            // noise floor reach above 6 kHz, and nothing masks the stepping: the deciding case), and the synthetic
            // guitar, whose bright plucks fill that region.
            const auto fixtureInput = readWav (fixture ("input_mono.wav"));
            const std::vector<std::pair<juce::String, std::vector<float>>> signals {
                { "picked notes", std::vector<float> (fixtureInput.getReadPointer (0), fixtureInput.getReadPointer (0) + fixtureInput.getNumSamples()) },
                { "guitar", guitarDI ((int) (4.0 * fs)) }
            };
            const std::vector<int> intervals { 1, 2, 4, 8, 16, 32 };
            std::map<int, bool> passes;
            for (const auto& [signalName, x] : signals)
                for (const auto q : { 4.0f, 12.0f })
                {
                    AutoWah::Settings s;
                    s.attackMs = 1.0f;
                    s.q = q;
                    std::vector<std::vector<float>> outs;
                    for (const auto interval : intervals)
                    {
                        AutoWah w;
                        w.setSettings (s);
                        w.prepare (fs, blockSize);
                        w.setRedesignIntervalForStudy (interval);
                        outs.push_back (runWah (w, x));
                    }
                    const auto& reference = outs[0];
                    const auto refHf = energyAbove (reference, 6000.0);
                    const auto refStep = maxStep (reference);
                    juce::StringArray lines;
                    for (size_t i = 1; i < intervals.size(); ++i)
                    {
                        std::vector<float> difference (x.size());
                        for (size_t n = 0; n < x.size(); ++n)
                            difference[n] = outs[i][n] - reference[n];
                        const auto hfChange = 10.0 * std::log10 (energyAbove (outs[i], 6000.0) / refHf);
                        const auto steppingHf = 10.0 * std::log10 (energyAbove (difference, 6000.0) / refHf + 1.0e-40);
                        const auto stepRatio = maxStep (outs[i]) / refStep;
                        const bool ok = hfChange < 0.1 && stepRatio < 1.01;
                        passes[intervals[i]] = (passes.count (intervals[i]) == 0 || passes[intervals[i]]) && ok;
                        lines.add (juce::String (intervals[i]) + ": HF " + (hfChange >= 0.0 ? "+" : "") + juce::String (hfChange, 2) + " dB (the stepping alone "
                                   + juce::String (steppingHf, 1) + " dB), max step x" + juce::String (stepRatio, 4) + ", difference "
                                   + juce::String (relativeErrorDb (outs[i], reference), 1) + " dB");
                    }
                    logMessage ("  -> " + signalName + ", Q " + juce::String (q, 0) + " (above 6 kHz the per-sample render holds " + juce::String (energyAboveDb (reference, 6000.0), 1)
                                + " dB of its energy): " + lines.joinIntoString ("; "));
                }
            passes[1] = true;
            juce::StringArray verdict;
            for (const auto interval : intervals)
                verdict.add (juce::String (interval) + (passes[interval] ? " clean" : " steps"));
            expect (passes[AutoWah::redesignInterval], "the chosen interval steps measurably");
            expect (! passes[2 * AutoWah::redesignInterval], "twice the chosen interval is clean too: it isn't the largest");
            logMessage ("  -> " + verdict.joinIntoString (", ") + ": redesignInterval = " + juce::String (AutoWah::redesignInterval));
        }

        beginTest ("a listening render: guitar through the defaults");
        {
            const auto x = guitarDI ((int) (8.0 * fs));
            AutoWah w;
            w.setSettings ({});
            w.prepare (fs, blockSize);
            auto y = x;
            double lowest = 1.0e9, highest = 0.0, openTime = 0.0, closedTime = 0.0;
            for (size_t start = 0; start < x.size(); start += blockSize)
            {
                float* channels[1] = { y.data() + start };
                const auto len = std::min ((size_t) blockSize, x.size() - start);
                w.process (juce::dsp::AudioBlock<float> (channels, 1, len), {});
                lowest = std::min (lowest, w.getCentreFrequency());
                highest = std::max (highest, w.getCentreFrequency());
                openTime += w.getControl() > 0.9f ? (double) len / fs : 0.0;
                closedTime += w.getControl() < 0.1f ? (double) len / fs : 0.0;
            }
            const auto file = proofDir().getChildFile ("autowah_defaults.wav");
            const auto dryFile = proofDir().getChildFile ("autowah_dry.wav");
            expect (writeWav (file, y));
            expect (writeWav (dryFile, x));
            logMessage ("  -> 8 s of the synthetic guitar (peaks -6 dBFS) through the defaults: loudness " + juce::String (toDb (rms (y) / rms (x)), 1)
                        + " dB against the dry (RMS), fc " + juce::String (lowest, 0) + " to " + juce::String (highest, 0) + " Hz, more than 90 % open "
                        + juce::String (100.0 * openTime / 8.0, 0) + " % of the time and less than 10 % open " + juce::String (100.0 * closedTime / 8.0, 0) + " %");
            logMessage ("  -> " + file.getFullPathName() + " (and the dry: " + dryFile.getFileName() + ")");
        }
    }
};

AutoWahTests autoWahTests;
} // namespace

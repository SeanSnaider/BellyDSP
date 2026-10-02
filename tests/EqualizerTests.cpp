#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Equalizer.h"

#include <chrono>
#include <complex>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::Equalizer;
using ampsim::CutFilter;

double dftMagnitudeDb (const std::vector<float>& h, double f)
{
    std::complex<double> sum = 0.0;
    const auto w = -juce::MathConstants<double>::twoPi * f / fs;
    for (size_t n = 0; n < h.size(); ++n)
        sum += (double) h[n] * std::polar (1.0, w * (double) n);
    return 20.0 * std::log10 (std::abs (sum));
}

std::vector<double> logSpaced (double lo, double hi, int count)
{
    std::vector<double> f ((size_t) count);
    for (int i = 0; i < count; ++i)
        f[(size_t) i] = lo * std::pow (hi / lo, (double) i / (count - 1));
    return f;
}

/// The EQ's impulse response (mono instance), long enough for its lowest band to ring out.
std::vector<float> impulseResponse (const Equalizer::Settings& settings, int length = 32768)
{
    Equalizer eq (false);
    eq.setSettings (settings);
    eq.prepare (fs, blockSize);
    std::vector<float> x ((size_t) length, 0.0f);
    x[0] = 1.0f;
    return runInBlocks (x, blockSize, [&] (juce::dsp::AudioBlock<float>& b, size_t) { eq.process (b.getSubsetChannelBlock (0, 1), {}); }).left;
}

std::vector<float> runEq (Equalizer& eq, const std::vector<float>& x, const std::function<void (size_t)>& beforeBlock = {})
{
    return runInBlocks (x, blockSize, [&] (juce::dsp::AudioBlock<float>& b, size_t start)
    {
        if (beforeBlock)
            beforeBlock (start);
        eq.process (b.getSubsetChannelBlock (0, eq.isStereo() ? 2 : 1), {});
    }).left;
}

Equalizer::Settings parametric()
{
    Equalizer::Settings s;
    s.mode = Equalizer::Mode::parametric;
    return s;
}

class EqualizerTests final : public juce::UnitTest
{
public:
    EqualizerTests() : juce::UnitTest ("EQ", "ampsim") {}

    void runTest() override
    {
        beginTest ("the graphic design matches the Python prototype (golden values)");
        {
            const auto csv = juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/graphic_eq_design.csv");
            juce::StringArray lines;
            csv.readLines (lines);
            int rows = 0;
            double worst = 0.0;
            for (const auto& line : lines)
            {
                if (line.startsWith ("#") || line.trim().isEmpty())
                    continue;
                juce::StringArray values;
                values.addTokens (line, ",", "");
                expectEquals (values.size(), 18);
                std::array<double, 9> sliders {};
                for (int m = 0; m < 9; ++m)
                    sliders[(size_t) m] = values[m].getDoubleValue();
                const auto gains = Equalizer::designGraphic (sliders, fs);
                for (int m = 0; m < 9; ++m)
                    worst = std::max (worst, std::abs (gains[(size_t) m] - values[9 + m].getDoubleValue()));
                ++rows;
            }
            expectEquals (rows, 60);
            expectLessThan (worst, 1.0e-6);
            logMessage ("  -> " + juce::String (rows) + " slider settings from prototypes/graphic_eq.py: band gains agree to " + juce::String (worst, 12) + " dB");
        }

        beginTest ("graphic mode lands within +-1 dB of the sliders (2000 random settings)");
        {
            juce::Random random (11);
            double worst = 0.0, worstNaive = 0.0, worstMidpoint = 0.0;
            for (int trial = 0; trial < 2000; ++trial)
            {
                Equalizer::Settings s;
                for (auto& v : s.sliders)
                    v = trial == 0 ? 12.0f : trial == 1 ? -12.0f : (float) (random.nextDouble() * 24.0 - 12.0);
                if (trial == 2)
                    for (size_t m = 0; m < 9; ++m)
                        s.sliders[m] = m % 2 == 0 ? 12.0f : -12.0f;

                std::array<double, 9> sliders {};
                for (size_t m = 0; m < 9; ++m)
                    sliders[m] = s.sliders[m];
                for (size_t m = 0; m < 9; ++m)
                {
                    const auto fc = Equalizer::graphicCentres[m];
                    worst = std::max (worst, std::abs (Equalizer::responseDb (s, fc, fs) - sliders[m]));

                    // The naive design: band gain = slider.
                    double naive = 0.0;
                    for (size_t b = 0; b < 9; ++b)
                        naive += 20.0 * std::log10 (std::abs (ampsim::Svf::responseAt (ampsim::Svf::design (ampsim::Svf::Type::peak, Equalizer::graphicCentres[b], Equalizer::graphicQ, sliders[b], fs), fc, fs)));
                    worstNaive = std::max (worstNaive, std::abs (naive - sliders[m]));

                    if (m + 1 < 9)
                    {
                        const auto mid = std::sqrt (fc * Equalizer::graphicCentres[m + 1]);
                        worstMidpoint = std::max (worstMidpoint, std::abs (Equalizer::responseDb (s, mid, fs) - 0.5 * (sliders[m] + sliders[m + 1])));
                    }
                }
            }
            expectLessThan (worst, 1.0);

            // The real block, measured: its impulse response equals the formula.
            Equalizer::Settings zigzag;
            for (size_t m = 0; m < 9; ++m)
                zigzag.sliders[m] = m % 2 == 0 ? 12.0f : -12.0f;
            const auto h = impulseResponse (zigzag);
            double blockError = 0.0;
            for (auto fc : Equalizer::graphicCentres)
                blockError = std::max (blockError, std::abs (dftMagnitudeDb (h, fc) - Equalizer::responseDb (zigzag, fc, fs)));
            expectLessThan (blockError, 0.01);

            // Plot: alternating sliders, naive vs. compensated.
            const auto freqs = logSpaced (20.0, 20000.0, 400);
            PlotSeries designed { "this EQ", freqs, {}, plotColour (0), 2.5f }, naive { "naive (band gain = slider)", freqs, {}, plotColour (1), 1.5f };
            PlotSeries sliderMarks { "sliders", {}, {}, plotColour (6), 6.0f };
            for (auto f : freqs)
            {
                designed.y.push_back (dftMagnitudeDb (h, f));
                double n = 0.0;
                for (size_t b = 0; b < 9; ++b)
                    n += 20.0 * std::log10 (std::abs (ampsim::Svf::responseAt (ampsim::Svf::design (ampsim::Svf::Type::peak, Equalizer::graphicCentres[b], Equalizer::graphicQ, zigzag.sliders[b], fs), f, fs)));
                naive.y.push_back (n);
            }
            for (size_t m = 0; m < 9; ++m)
            {
                sliderMarks.x.insert (sliderMarks.x.end(), { Equalizer::graphicCentres[m] * 0.93, Equalizer::graphicCentres[m] * 1.07, std::nan ("") });
                sliderMarks.y.insert (sliderMarks.y.end(), { (double) zigzag.sliders[m], (double) zigzag.sliders[m], std::nan ("") });
            }
            PlotOptions o;
            o.title = "Graphic EQ, sliders alternating +12 / -12 dB";
            o.xLabel = "Frequency (Hz)";
            o.yLabel = "dB";
            o.logX = true;
            o.xMin = 20.0; o.xMax = 20000.0; o.yMin = -24.0; o.yMax = 24.0;
            const auto png = proofDir().getChildFile ("eq_graphic_accuracy.png");
            expect (savePlot (png, o, { naive, designed, sliderMarks }));

            logMessage ("  -> 2000 settings: worst error at a band centre " + juce::String (worst, 3) + " dB (naive design: " + juce::String (worstNaive, 2)
                        + " dB); between bands, within " + juce::String (worstMidpoint, 2) + " dB of a straight line between sliders");
            logMessage ("  -> the real block, alternating +-12 dB: impulse response matches the formula to " + juce::String (blockError, 4) + " dB at every centre");
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("parametric bands and cuts match their analytic responses");
        {
            struct Case
            {
                juce::String name;
                Equalizer::Settings settings;
            };
            std::vector<Case> cases;
            auto add = [&] (const juce::String& name, int band, Equalizer::BandType type, float f, float g, float q)
            {
                auto s = parametric();
                s.bands[(size_t) band] = { type, f, g, q };
                cases.push_back ({ name, s });
            };
            add ("peak +9 dB at 1 kHz, Q 2", 2, Equalizer::BandType::peak, 1000.0f, 9.0f, 2.0f);
            add ("low shelf -6 dB at 200 Hz", 0, Equalizer::BandType::lowShelf, 200.0f, -6.0f, 0.7071f);
            add ("high shelf +6 dB at 5 kHz", 4, Equalizer::BandType::highShelf, 5000.0f, 6.0f, 0.7071f);
            add ("notch at 3 kHz, Q 4", 3, Equalizer::BandType::notch, 3000.0f, 0.0f, 4.0f);
            for (auto slope : { CutFilter::Slope::db12, CutFilter::Slope::db24, CutFilter::Slope::db48 })
            {
                auto s = parametric();
                s.lowCut = { true, 100.0f, slope };
                s.highCut = { true, 8000.0f, slope };
                cases.push_back ({ "cuts 100 Hz / 8 kHz at " + juce::String (12 * CutFilter::numSections (slope)) + " dB/oct", s });
            }

            const auto freqs = logSpaced (20.0, 20000.0, 160);
            double worst = 0.0;
            std::vector<PlotSeries> series;
            int colour = 0;
            for (const auto& c : cases)
            {
                const auto h = impulseResponse (c.settings);
                PlotSeries line { c.name, {}, {}, plotColour (colour++), 2.0f };
                for (auto f : freqs)
                {
                    const auto expected = Equalizer::responseDb (c.settings, f, fs);
                    const auto measured = dftMagnitudeDb (h, f);
                    if (expected > -60.0)
                        worst = std::max (worst, std::abs (measured - expected));
                    line.x.push_back (f);
                    line.y.push_back (measured);
                }
                series.push_back (line);
            }
            expectLessThan (worst, 0.01);

            // Design targets.
            const auto peakAtFc = dftMagnitudeDb (impulseResponse (cases[0].settings), 1000.0);
            const auto notchDepth = dftMagnitudeDb (impulseResponse (cases[3].settings), 3000.0);
            const auto cutAtFc = Equalizer::responseDb (cases[6].settings, 100.0, fs) - CutFilter::responseDb (CutFilter::Kind::highCut, CutFilter::Slope::db48, 8000.0, 100.0, fs);
            const auto octaveBelow = CutFilter::responseDb (CutFilter::Kind::lowCut, CutFilter::Slope::db48, 100.0, 50.0, fs);
            expectWithinAbsoluteError (peakAtFc, 9.0, 0.001);
            expectLessThan (notchDepth, -60.0);
            expectWithinAbsoluteError (cutAtFc, -3.0103, 0.001);

            PlotOptions o;
            o.title = "Parametric band types and cuts (measured impulse responses)";
            o.xLabel = "Frequency (Hz)";
            o.yLabel = "dB";
            o.logX = true;
            o.xMin = 20.0; o.xMax = 20000.0; o.yMin = -48.0; o.yMax = 12.0;
            const auto png = proofDir().getChildFile ("eq_parametric_types.png");
            expect (savePlot (png, o, series));
            logMessage ("  -> 4 band types and 3 cut slopes, 160 frequencies each: measured within " + juce::String (worst, 4) + " dB of the formulas");
            logMessage ("  -> peak reaches " + juce::String (peakAtFc, 4) + " dB at fc; notch " + juce::String (notchDepth, 1) + " dB deep; 48 dB/oct cut "
                        + juce::String (cutAtFc, 4) + " dB at fc and " + juce::String (octaveBelow, 2) + " dB an octave out");
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("flat is bit-transparent in both modes, mono and stereo");
        {
            const auto x = guitarDI ((int) fs);
            double worst = 0.0;
            for (auto mode : { Equalizer::Mode::graphic, Equalizer::Mode::parametric })
            {
                for (bool stereo : { false, true })
                {
                    Equalizer eq (stereo);
                    Equalizer::Settings s;
                    s.mode = mode;
                    eq.setSettings (s);
                    eq.prepare (fs, blockSize);
                    const auto out = runInBlocks (x, blockSize, [&] (juce::dsp::AudioBlock<float>& b, size_t) { eq.process (b.getSubsetChannelBlock (0, stereo ? 2 : 1), {}); });
                    worst = std::max ({ worst, maxAbsDifference (out.left, x), stereo ? maxAbsDifference (out.right, x) : 0.0 });
                }
            }
            expectEquals (worst, 0.0);
            logMessage ("  -> graphic and parametric, mono and stereo, all controls at 0 dB: output == input, bit for bit");
        }

        beginTest ("fast frequency sweeps stay stable");
        {
            Equalizer eq (false);
            auto s = parametric();
            s.bands[2] = { Equalizer::BandType::peak, 1000.0f, 12.0f, 5.0f };
            eq.setSettings (s);
            eq.prepare (fs, blockSize);
            const auto noise = whiteNoise ((int) (3.0 * fs), 0.25f, 4);
            const auto out = runEq (eq, noise, [&] (size_t start)
            {
                // The band's frequency swept 40 Hz <-> 16 kHz five times a second.
                const auto t = (double) start / fs;
                s.bands[2].frequency = (float) (40.0 * std::pow (400.0, 0.5 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 5.0 * t)));
                eq.setSettings (s);
            });
            bool finite = true;
            float peak = 0.0f;
            for (auto v : out)
            {
                finite = finite && std::isfinite (v);
                peak = std::max (peak, std::abs (v));
            }
            expect (finite);
            expectLessThan (toDb (peak / 0.25f), 20.0);
            logMessage ("  -> +12 dB, Q 5 band swept 40 Hz to 16 kHz at 5 Hz for 3 s under white noise: output finite, peak "
                        + juce::String (toDb (peak / 0.25f), 1) + " dB relative to the noise's peak (a +12 dB band on its own)");
        }

        beginTest ("moving a slider doesn't zipper: 32-sample coefficient updates vs. per-sample ones");
        {
            // A 1 kHz tone while the 1 kHz slider moves 0 -> +12 dB. The reference redesigns every
            // sample from the same ramp of band gains.
            Equalizer eq (false);
            Equalizer::Settings s;
            eq.setSettings (s);
            eq.prepare (fs, blockSize);
            const auto tone = sine (1000.0, 0.2, (int) (0.5 * fs));
            const auto changeAt = (size_t) (0.1 * fs) / blockSize * blockSize;
            auto moved = s;
            moved.sliders[4] = 12.0f;
            const auto out = runEq (eq, tone, [&] (size_t start) { if (start == changeAt) eq.setSettings (moved); });

            std::array<double, 9> zero {}, twelve {};
            twelve[4] = 12.0;
            const auto before = Equalizer::designGraphic (zero, fs), after = Equalizer::designGraphic (twelve, fs);
            std::array<ampsim::Svf, 9> filters;
            std::array<juce::SmoothedValue<double>, 9> gains;
            for (size_t m = 0; m < 9; ++m)
            {
                gains[m].reset (fs, Equalizer::smoothingSeconds);
                gains[m].setCurrentAndTargetValue (before[m]);
            }
            std::vector<float> reference (tone.size());
            for (size_t n = 0; n < tone.size(); ++n)
            {
                if (n == changeAt)
                    for (size_t m = 0; m < 9; ++m)
                        gains[m].setTargetValue (after[m]);
                auto v = (double) tone[n];
                for (size_t m = 0; m < 9; ++m)
                {
                    filters[m].setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::peak, Equalizer::graphicCentres[m], Equalizer::graphicQ, gains[m].getNextValue(), fs));
                    v = filters[m].processSample (v);
                }
                reference[n] = (float) v;
            }
            std::vector<float> a (out.begin() + (long) changeAt, out.begin() + (long) changeAt + 2400);
            std::vector<float> b (reference.begin() + (long) changeAt, reference.begin() + (long) changeAt + 2400);
            const auto error = relativeErrorDb (a, b);
            expectLessThan (error, -40.0);
            logMessage ("  -> during the 25 ms ramp the output is " + juce::String (error, 1) + " dB from per-sample redesign");
        }

        beginTest ("switching modes and changing a band's type don't click");
        {
            Equalizer eq (false);
            Equalizer::Settings s;
            for (size_t m = 0; m < 9; ++m)
                s.sliders[m] = m % 2 == 0 ? 9.0f : -9.0f;
            s.bands[2] = { Equalizer::BandType::peak, 300.0f, 10.0f, 1.5f };
            eq.setSettings (s);
            eq.prepare (fs, blockSize);
            const auto tone = sine (300.0, 0.2, (int) (1.5 * fs));
            const auto modeAt = (size_t) (0.4 * fs) / blockSize * blockSize, typeAt = (size_t) (0.9 * fs) / blockSize * blockSize;
            const auto out = runEq (eq, tone, [&] (size_t start)
            {
                if (start == modeAt)
                    s.mode = Equalizer::Mode::parametric;
                if (start == typeAt)
                    s.bands[2].type = Equalizer::BandType::lowShelf;
                eq.setSettings (s);
            });
            const auto steadyBefore = maxStep (out, (size_t) (0.2 * fs), modeAt);
            const auto duringMode = maxStep (out, modeAt, modeAt + 2400);
            const auto steadyMiddle = maxStep (out, modeAt + 9600, typeAt);
            const auto duringType = maxStep (out, typeAt, typeAt + 2400);
            const auto steadyAfter = maxStep (out, typeAt + 9600, out.size());
            const auto bound = 1.05 * std::max ({ steadyBefore, steadyMiddle, steadyAfter });
            expectLessThan (duringMode, bound);
            expectLessThan (duringType, bound);
            logMessage ("  -> graphic -> parametric: largest step " + juce::String (duringMode, 4) + " (steady " + juce::String (steadyBefore, 4) + " / "
                        + juce::String (steadyMiddle, 4) + "); peak -> low shelf: " + juce::String (duringType, 4) + " (steady after " + juce::String (steadyAfter, 4) + ")");
        }

        beginTest ("CPU: a stereo graphic EQ with cuts, 128-sample buffers");
        {
            Equalizer eq (true);
            Equalizer::Settings s;
            for (size_t m = 0; m < 9; ++m)
                s.sliders[m] = (float) (m % 3) * 4.0f - 4.0f;
            s.lowCut = { true, 80.0f, CutFilter::Slope::db24 };
            s.highCut = { true, 9000.0f, CutFilter::Slope::db24 };
            eq.setSettings (s);
            eq.prepare (fs, blockSize);
            juce::AudioBuffer<float> buffer (2, blockSize);
            const auto noise = whiteNoise ((int) (10.0 * fs), 0.3f, 5);
            std::vector<double> micros;
            for (size_t start = 0; start + blockSize <= noise.size(); start += blockSize)
            {
                buffer.copyFrom (0, 0, noise.data() + start, blockSize);
                buffer.copyFrom (1, 0, noise.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                eq.process (juce::dsp::AudioBlock<float> (buffer), {});
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            expectLessThan (mean, 0.05 * deadlineMicros);
            logMessage ("  -> mean " + juce::String (mean, 1) + " us per buffer (" + juce::String (100.0 * mean / deadlineMicros, 2) + "% of the deadline)");
        }
    }
};

EqualizerTests equalizerTests;
} // namespace

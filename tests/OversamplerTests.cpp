#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/FftDouble.h"
#include "dsp/Oversampler.h"

#include <complex>

namespace
{
using namespace testing;
using ampsim::Downsampler;
using ampsim::Upsampler;

constexpr double pi = juce::MathConstants<double>::pi;

/// The upsampling chain's impulse response at the high rate: what an impulse at the base rate becomes.
/// Divided by the factor, its spectrum is the cascaded interpolation filter's response, unity in the
/// passband.
std::vector<double> upImpulseResponse (int factor, int baseLength)
{
    Upsampler up;
    up.setFactor (factor);
    std::vector<double> in ((size_t) baseLength, 0.0), out ((size_t) (baseLength * factor));
    in[0] = 1.0;
    up.process (in.data(), out.data(), baseLength);
    for (auto& v : out)
        v /= factor;
    return out;
}

/// Up then down with nothing in between, on an impulse: the chain's own linear response at the base rate.
std::vector<double> roundTripImpulseResponse (int factor, int length)
{
    Upsampler up;
    Downsampler down;
    up.setFactor (factor);
    down.setFactor (factor);
    std::vector<double> in ((size_t) length, 0.0), high ((size_t) (length * factor)), out ((size_t) length);
    in[0] = 1.0;
    up.process (in.data(), high.data(), length);
    down.process (high.data(), out.data(), length);
    return out;
}

std::complex<double> dtft (const std::vector<double>& h, double normalizedFrequency)
{
    std::complex<double> sum (0.0, 0.0);
    for (size_t n = 0; n < h.size(); ++n)
        sum += h[n] * std::polar (1.0, -2.0 * pi * normalizedFrequency * (double) n);
    return sum;
}

/// Group delay in samples at f (fraction of the rate), from the phase slope.
double groupDelay (const std::vector<double>& h, double f)
{
    const auto df = 1.0e-6;
    const auto a = std::arg (dtft (h, f - df)), b = std::arg (dtft (h, f + df));
    auto d = b - a;
    while (d > pi)
        d -= 2.0 * pi;
    while (d < -pi)
        d += 2.0 * pi;
    return -d / (2.0 * pi * 2.0 * df);
}

std::vector<double> magnitudeDb (const std::vector<double>& h, int order)
{
    ampsim::FftDouble fft (order);
    std::vector<std::complex<double>> data ((size_t) fft.size());
    for (size_t n = 0; n < std::min (h.size(), data.size()); ++n)
        data[n] = h[n];
    fft.forward (data);
    std::vector<double> db (data.size() / 2 + 1);
    for (size_t k = 0; k < db.size(); ++k)
        db[k] = 20.0 * std::log10 (std::max (std::abs (data[k]), 1.0e-30));
    return db;
}

class OversamplerTests final : public juce::UnitTest
{
public:
    OversamplerTests() : juce::UnitTest ("Oversampler", "ampsim") {}

    void runTest() override
    {
        beginTest ("halfband coefficients match the Python design (tests/fixtures/halfband_coefficients.csv)");
        {
            const auto csv = juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/halfband_coefficients.csv");
            juce::StringArray lines;
            csv.readLines (lines);
            double worst = 0.0;
            juce::StringArray designs;
            for (const auto& line : lines)
            {
                if (line.startsWith ("#") || line.trim().isEmpty())
                    continue;
                juce::StringArray fields;
                fields.addTokens (line, ",", "");
                const auto n = fields[0].getIntValue();
                const auto transition = fields[1].getDoubleValue();
                const auto coefficients = ampsim::halfband::design (n, transition);
                expectEquals ((int) coefficients.size(), fields.size() - 2);
                for (int i = 0; i < n; ++i)
                    worst = std::max (worst, std::abs (coefficients[(size_t) i] - fields[i + 2].getDoubleValue()));
                designs.add (juce::String (n) + " @ " + juce::String (transition));
            }
            expectEquals (designs.size(), 5);
            expectLessThan (worst, 1.0e-13);
            logMessage ("  -> " + juce::String (designs.size()) + " designs (" + designs.joinIntoString (", ") + "): largest difference from Python "
                        + juce::String (worst, 20));

            juce::StringArray stages;
            for (const auto& d : ampsim::oversampling::stageDesigns)
            {
                const auto c = ampsim::halfband::design (d.numCoefficients, d.transition);
                juce::StringArray values;
                for (const auto v : c)
                    values.add (juce::String (v, 6));
                stages.add (juce::String (d.numCoefficients) + " @ " + juce::String (d.transition) + " = {" + values.joinIntoString (", ") + "}");
            }
            logMessage ("  -> stages used: " + stages.joinIntoString ("; "));
        }

        beginTest ("2x, 4x, 8x chains: passband flatness and image/alias rejection");
        {
            std::vector<PlotSeries> series;
            int colour = 0;
            for (const auto factor : { 2, 4, 8 })
            {
                constexpr int order = 16; // 65536 points at the high rate
                const auto h = upImpulseResponse (factor, (1 << order) / factor);
                const auto db = magnitudeDb (h, order);
                const auto highRate = fs * factor;
                const auto binHz = highRate / (double) (1 << order);

                double ripple = 0.0, stop = -400.0, nearStop = -400.0;
                for (size_t k = 0; k < db.size(); ++k)
                {
                    const auto f = (double) k * binHz;
                    if (f <= 20000.0)
                        ripple = std::max (ripple, std::abs (db[k]));
                    if (f >= 28000.0) // images of 0-20 kHz, and what would alias into it
                        stop = std::max (stop, db[k]);
                    if (f >= 25920.0 && f <= 28000.0) // from stage 1's stopband edge
                        nearStop = std::max (nearStop, db[k]);
                }
                expectLessThan (ripple, 1.0e-4);
                expectLessThan (stop, -99.0);
                logMessage ("  -> " + juce::String (factor) + "x: passband 0-20 kHz flat within " + juce::String (ripple, 7) + " dB; everything from 28 kHz to "
                            + juce::String (highRate / 2000.0, 0) + " kHz (the images of 0-20 kHz) at most " + juce::String (stop, 1)
                            + " dB; 25.92-28 kHz at most " + juce::String (nearStop, 1) + " dB");

                PlotSeries s { juce::String (factor) + "x (" + juce::String (highRate / 1000.0, 0) + " kHz)", {}, {}, plotColour (colour++), 1.5f };
                for (size_t k = 1; k < db.size(); k += 8)
                {
                    s.x.push_back ((double) k * binHz);
                    s.y.push_back (std::max (-180.0, db[k]));
                }
                series.push_back (s);
            }

            PlotOptions o;
            o.title = "Interpolation (and decimation) filter of each chain, polyphase IIR halfbands";
            o.xLabel = "Frequency (Hz) at the oversampled rate";
            o.yLabel = "Magnitude (dB)";
            o.logX = true;
            o.xMin = 1000.0; o.xMax = 192000.0; o.yMin = -180.0; o.yMax = 10.0;
            const auto png = proofDir().getChildFile ("oversampler_response.png");
            expect (savePlot (png, o, series));
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("up then down: an allpass at the base rate, with the group delay it costs");
        {
            std::vector<PlotSeries> series;
            int colour = 0;
            for (const auto factor : { 2, 4, 8 })
            {
                const auto h = roundTripImpulseResponse (factor, 4096);
                double worstDb = 0.0, edgeDb = 0.0;
                for (double f = 10.0; f <= 23000.0; f *= 1.02)
                {
                    const auto db = std::abs (20.0 * std::log10 (std::abs (dtft (h, f / fs))));
                    auto& band = f <= 20000.0 ? worstDb : edgeDb;
                    band = std::max (band, db);
                }
                const auto predicted = ampsim::oversampling::groupDelaySamples (factor);
                const auto at100 = groupDelay (h, 100.0 / fs);
                juce::StringArray delays;
                PlotSeries s { juce::String (factor) + "x", {}, {}, plotColour (colour++), 2.0f };
                for (const auto f : { 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
                    delays.add (juce::String (f / 1000.0, 0) + " kHz " + juce::String (groupDelay (h, f / fs), 2));
                for (double f = 20.0; f <= 22000.0; f *= 1.05)
                {
                    s.x.push_back (f);
                    s.y.push_back (groupDelay (h, f / fs));
                }
                series.push_back (s);

                expectLessThan (worstDb, 1.0e-6);
                expectWithinAbsoluteError (at100, predicted, 0.01);
                logMessage ("  -> " + juce::String (factor) + "x round trip: magnitude within " + juce::String (worstDb, 9) + " dB of 0 from 10 Hz to 20 kHz ("
                            + juce::String (edgeDb, 6) + " dB at 20-23 kHz); group delay "
                            + juce::String (at100, 3) + " samples (" + juce::String (1.0e6 * at100 / fs, 1) + " us) at 100 Hz (predicted from the coefficients: "
                            + juce::String (predicted, 3) + "); " + delays.joinIntoString (", "));
            }

            PlotOptions o;
            o.title = "Group delay of the up-and-down chain (the only delay the oversampling adds)";
            o.xLabel = "Frequency (Hz)";
            o.yLabel = "Group delay (samples at 48 kHz)";
            o.logX = true;
            o.xMin = 20.0; o.xMax = 22000.0; o.yMin = 0.0; o.yMax = 12.0;
            const auto png = proofDir().getChildFile ("oversampler_group_delay.png");
            expect (savePlot (png, o, series));
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("a sine through up and down comes back as the same sine, shifted only by the allpass phase");
        {
            juce::StringArray results;
            double worst = -400.0;
            for (const auto factor : { 2, 4, 8 })
            {
                const auto h = roundTripImpulseResponse (factor, 4096);
                for (const auto f : { 1000.0, 10000.0 })
                {
                    constexpr int length = 24000;
                    std::vector<double> x ((size_t) length), high ((size_t) (length * factor)), y ((size_t) length);
                    for (int n = 0; n < length; ++n)
                        x[(size_t) n] = 0.5 * std::sin (2.0 * pi * f * n / fs);
                    Upsampler up;
                    Downsampler down;
                    up.setFactor (factor);
                    down.setFactor (factor);
                    up.process (x.data(), high.data(), length);
                    down.process (high.data(), y.data(), length);

                    // Steady state of an LTI system: the same sine times H(f).
                    const auto response = dtft (h, f / fs);
                    double errorSquares = 0.0, signalSquares = 0.0;
                    for (int n = 4000; n < length; ++n)
                    {
                        const auto expected = 0.5 * std::abs (response) * std::sin (2.0 * pi * f * n / fs + std::arg (response));
                        errorSquares += (y[(size_t) n] - expected) * (y[(size_t) n] - expected);
                        signalSquares += expected * expected;
                    }
                    const auto errorDb = 10.0 * std::log10 (errorSquares / signalSquares);
                    worst = std::max (worst, errorDb);
                    results.add (juce::String (factor) + "x " + juce::String (f / 1000.0, 0) + " kHz: gain " + juce::String (std::abs (response), 7) + ", error "
                                 + juce::String (errorDb, 1) + " dB");
                }
            }
            expectLessThan (worst, -120.0);
            logMessage ("  -> " + results.joinIntoString ("; "));
        }
    }
};

OversamplerTests oversamplerTests;
} // namespace

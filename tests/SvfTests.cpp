#include "TestHelpers.h"
#include "dsp/Svf.h"

#include <complex>

namespace
{
using namespace testing;
using ampsim::Svf;
using Type = Svf::Type;

/// The filter's measured complex response at frequency f: the DFT of its impulse response there.
std::complex<double> measuredResponse (const std::vector<double>& impulseResponse, double f)
{
    std::complex<double> sum = 0.0;
    const auto w = -juce::MathConstants<double>::twoPi * f / fs;

    for (size_t n = 0; n < impulseResponse.size(); ++n)
        sum += impulseResponse[n] * std::polar (1.0, w * (double) n);

    return sum;
}

std::vector<double> impulseResponse (const Svf::Coefficients& c, int length)
{
    Svf filter;
    filter.setCoefficients (c);
    std::vector<double> h ((size_t) length);

    for (int n = 0; n < length; ++n)
        h[(size_t) n] = filter.processSample (n == 0 ? 1.0 : 0.0);

    return h;
}

double magnitudeDb (std::complex<double> h) { return 20.0 * std::log10 (std::abs (h)); }

/// Direct form I biquad with RBJ cookbook peaking coefficients, for comparison under fast sweeps.
struct DirectFormBiquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void setPeak (double f0, double q, double gainDb)
    {
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto w0 = juce::MathConstants<double>::twoPi * f0 / fs;
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0; b1 = -2.0 * std::cos (w0) / a0; b2 = (1.0 - alpha * A) / a0;
        a1 = -2.0 * std::cos (w0) / a0; a2 = (1.0 - alpha / A) / a0;
    }

    double process (double x)
    {
        const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

class SvfTests final : public juce::UnitTest
{
public:
    SvfTests() : juce::UnitTest ("SVF filter", "ampsim") {}

    void runTest() override
    {
        struct Case { const char* name; Type type; double fc, q, gainDb; };
        const Case cases[] = {
            { "lowpass",          Type::lowpass,   1000.0, 0.7071, 0.0 },
            { "highpass",         Type::highpass,  1000.0, 0.7071, 0.0 },
            { "bandpass",         Type::bandpass,  1000.0, 2.0,    0.0 },
            { "notch",            Type::notch,     1000.0, 2.0,    0.0 },
            { "allpass",          Type::allpass,   1000.0, 0.7,    0.0 },
            { "peak +9 dB",       Type::peak,      1000.0, 1.5,    9.0 },
            { "peak -9 dB",       Type::peak,      1000.0, 1.5,   -9.0 },
            { "low shelf +6 dB",  Type::lowShelf,  200.0,  0.7071, 6.0 },
            { "high shelf -6 dB", Type::highShelf, 3000.0, 0.7071, -6.0 },
            { "lowpass at 40 Hz", Type::lowpass,   40.0,   0.7071, 0.0 },
        };

        beginTest ("every response type matches its analog prototype at 200 frequencies");
        {
            juce::StringArray results;

            for (const auto& c : cases)
            {
                const auto coeffs = Svf::design (c.type, c.fc, c.q, c.gainDb, fs);
                const auto h = impulseResponse (coeffs, 1 << 16);
                double worst = 0.0;

                for (int i = 0; i < 200; ++i)
                {
                    const auto f = 20.0 * std::pow (20000.0 / 20.0, i / 199.0); // 20 Hz to 20 kHz, log spaced
                    const auto expected = Svf::responseAt (coeffs, f, fs);
                    const auto error = std::abs (measuredResponse (h, f) - expected) / std::max (std::abs (expected), 1.0e-3);
                    worst = std::max (worst, error);
                }

                expectLessThan (worst, 1.0e-6, c.name);
                results.add (juce::String (c.name) + " " + juce::String (toDb (worst), 0) + " dB");
            }

            logMessage ("  -> worst complex error vs. the analog prototype at the prewarped frequency: "
                        + results.joinIntoString (", "));
        }

        beginTest ("design targets: gains land where they're asked to");
        {
            const auto at = [] (Type t, double fc, double q, double g, double f)
            { return magnitudeDb (Svf::responseAt (Svf::design (t, fc, q, g, fs), f, fs)); };

            const auto lp3dB = at (Type::lowpass, 1000.0, 0.7071, 0.0, 1000.0);
            const auto hp3dB = at (Type::highpass, 1000.0, 0.7071, 0.0, 1000.0);
            const auto peak = at (Type::peak, 1000.0, 1.5, 9.0, 1000.0);
            const auto shelfLow = at (Type::lowShelf, 200.0, 0.7071, 6.0, 1.0);
            const auto shelfMid = at (Type::lowShelf, 200.0, 0.7071, 6.0, 200.0);
            const auto shelfHigh = at (Type::lowShelf, 200.0, 0.7071, 6.0, 20000.0);
            const auto notch = at (Type::notch, 1000.0, 2.0, 0.0, 1000.0);

            expectWithinAbsoluteError (lp3dB, -3.0103, 0.001);
            expectWithinAbsoluteError (hp3dB, -3.0103, 0.001);
            expectWithinAbsoluteError (peak, 9.0, 1.0e-9);
            expectWithinAbsoluteError (shelfLow, 6.0, 0.001);
            expectWithinAbsoluteError (shelfMid, 3.0, 1.0e-9);
            expectWithinAbsoluteError (shelfHigh, 0.0, 0.05);
            expectLessThan (notch, -200.0);

            logMessage ("  -> lowpass and highpass at fc (Q 0.707): " + juce::String (lp3dB, 4) + " / " + juce::String (hp3dB, 4)
                        + " dB; +9 dB bell at fc: " + juce::String (peak, 6) + " dB; +6 dB low shelf at 1 Hz / fc / 20 kHz: "
                        + juce::String (shelfLow, 3) + " / " + juce::String (shelfMid, 6) + " / " + juce::String (shelfHigh, 3)
                        + " dB; notch at fc: " + juce::String (notch, 0) + " dB");
        }

        beginTest ("0 dB bells and shelves are bit-transparent");
        {
            const auto input = whiteNoise (48000, 0.5f, 11);
            double worst = 0.0;

            for (auto type : { Type::peak, Type::lowShelf, Type::highShelf })
            {
                Svf filter;
                filter.setCoefficients (Svf::design (type, 500.0, 0.7, 0.0, fs));
                for (auto x : input)
                    worst = std::max (worst, std::abs ((double) (float) filter.processSample (x) - (double) x));
            }

            expectEquals (worst, 0.0);
            logMessage ("  -> max difference between input and output at 0 dB: " + juce::String (worst));
        }

        beginTest ("fast sweeps under noise stay bounded (BUILD_PLAN EQ fast-sweep test)");
        {
            // A +12 dB, Q 4 bell swept between 40 Hz and 16 kHz twenty times a second, with new
            // coefficients every sample: far faster than any knob or expression pedal.
            const auto input = whiteNoise (96000, 0.5f, 12);
            Svf svf;
            DirectFormBiquad biquad;
            double svfPeak = 0.0, biquadPeak = 0.0;
            bool svfFinite = true;

            for (size_t n = 0; n < input.size(); ++n)
            {
                const auto lfo = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * 20.0 * (double) n / fs);
                const auto f = 40.0 * std::pow (16000.0 / 40.0, lfo);
                svf.setCoefficients (Svf::design (Type::peak, f, 4.0, 12.0, fs));
                biquad.setPeak (f, 4.0, 12.0);

                const auto ySvf = svf.processSample (input[n]);
                const auto yBiquad = biquad.process (input[n]);
                svfFinite = svfFinite && std::isfinite (ySvf);
                svfPeak = std::max (svfPeak, std::abs (ySvf));
                biquadPeak = std::max (biquadPeak, std::abs (yBiquad));
            }

            // Static gain is at most +12 dB (x4) on a 0.5 peak input, so 2.0 plus the noise's crest.
            expect (svfFinite);
            expectLessThan (svfPeak, 8.0);
            logMessage ("  -> peak output under a 20 Hz full-range sweep: SVF " + juce::String (svfPeak, 2)
                        + ", direct-form biquad with the same sweep " + juce::String (biquadPeak, 2)
                        + " (input peak 0.5, static gain up to x4)");
        }

        beginTest ("updating coefficients every 32 samples adds no audible stepping (zipper test)");
        {
            // Sweep a bell from -12 to +12 dB over 200 ms on a 1 kHz sine. Compare coefficients
            // updated every sample (ideal) with every 32 samples (what the EQ and tone stacks do).
            const auto input = sine (1000.0, 0.5, 19200);
            Svf ideal, stepped;
            std::vector<float> idealOut (input.size()), steppedOut (input.size());
            const auto gainAt = [&] (size_t n) { return -12.0 + 24.0 * std::min (1.0, (double) n / 9600.0); };

            for (size_t n = 0; n < input.size(); ++n)
            {
                ideal.setCoefficients (Svf::design (Type::peak, 1000.0, 1.0, gainAt (n), fs));
                if (n % 32 == 0)
                    stepped.setCoefficients (Svf::design (Type::peak, 1000.0, 1.0, gainAt (n + 16), fs)); // mid-interval value

                idealOut[n] = (float) ideal.processSample (input[n]);
                steppedOut[n] = (float) stepped.processSample (input[n]);
            }

            const auto error = relativeErrorDb (steppedOut, std::vector<double> (idealOut.begin(), idealOut.end()));
            expectLessThan (error, -40.0);
            logMessage ("  -> 24 dB sweep in 200 ms: 32-sample updates differ from per-sample updates by " + dB (error)
                        + " relative to the output");
        }
    }
};

SvfTests svfTests;
} // namespace

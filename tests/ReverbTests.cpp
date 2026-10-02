#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Reverb.h"

#include <chrono>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::DattorroPlate;
using ampsim::FeedbackDelayNetwork;
using ampsim::Reverb;
using Engine = Reverb::Engine;

constexpr std::array<Engine, 3> allEngines { Engine::room, Engine::hall, Engine::plate };

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/reverb").getChildFile (name);
}

juce::String nameOf (Engine e)
{
    return e == Engine::room ? "Room" : (e == Engine::hall ? "Hall" : "Plate");
}

/// Settings for measuring the reverb itself: all wet, no pre-delay, cuts off, late reverb only.
Reverb::Settings wetOnly (Engine engine)
{
    Reverb::Settings s;
    s.engine = engine;
    s.mix = 1.0f;
    s.preDelayMs = 0.0f;
    s.lowCutHz = 20.0f;
    s.highCutHz = 20000.0f;
    s.earlyLate = 1.0f;
    s.ducking = 0.0f;
    s.width = 1.0f;
    return s;
}

/// Runs a stereo signal through the reverb in 128-sample buffers. `before` runs ahead of each buffer
/// with its first sample's index (for settings changes).
Stereo run (Reverb& reverb, const std::vector<float>& left, const std::vector<float>& right,
            const std::function<void (size_t)>& before = {})
{
    Stereo out { left, right };
    for (size_t start = 0; start < left.size(); start += blockSize)
    {
        if (before)
            before (start);
        const auto len = std::min ((size_t) blockSize, left.size() - start);
        float* channels[2] = { out.left.data() + start, out.right.data() + start };
        reverb.process (juce::dsp::AudioBlock<float> (channels, 2, len), {});
    }
    return out;
}

Stereo run (Reverb& reverb, const std::vector<float>& mono, const std::function<void (size_t)>& before = {})
{
    return run (reverb, mono, mono, before);
}

std::vector<float> impulse (size_t length)
{
    std::vector<float> x (length, 0.0f);
    x[0] = 1.0f;
    return x;
}

std::vector<float> concat (std::vector<float> a, const std::vector<float>& b)
{
    a.insert (a.end(), b.begin(), b.end());
    return a;
}

double energy (const std::vector<float>& x, size_t start, size_t end)
{
    double sum = 0.0;
    for (size_t i = start; i < std::min (end, x.size()); ++i)
        sum += (double) x[i] * (double) x[i];
    return sum;
}

double energyDb (const Stereo& s, size_t start, size_t end)
{
    return 10.0 * std::log10 (std::max (1.0e-300, energy (s.left, start, end) + energy (s.right, start, end)));
}

// ---- Decay measurement ------------------------------------------------------------------------

/// One octave band of x: 4th-order Butterworth high-pass at lo and low-pass at hi (two SVF sections
/// each, Q 0.541 and 1.307), in double.
std::vector<double> band (const std::vector<float>& x, double lo, double hi)
{
    using ampsim::Svf;
    std::array<Svf, 4> f;
    f[0].setCoefficients (Svf::design (Svf::Type::highpass, lo, 0.5411961001461970, 0.0, fs));
    f[1].setCoefficients (Svf::design (Svf::Type::highpass, lo, 1.3065629648763766, 0.0, fs));
    f[2].setCoefficients (Svf::design (Svf::Type::lowpass, hi, 0.5411961001461970, 0.0, fs));
    f[3].setCoefficients (Svf::design (Svf::Type::lowpass, hi, 1.3065629648763766, 0.0, fs));
    std::vector<double> y (x.size());
    for (size_t n = 0; n < x.size(); ++n)
    {
        auto v = (double) x[n];
        for (auto& s : f)
            v = s.processSample (v);
        y[n] = v;
    }
    return y;
}

/// Schroeder's energy decay curve (M. R. Schroeder, "New Method of Measuring Reverberation Time",
/// JASA 37, 1965): EDC(t) = sum over tau >= t of h^2(tau), the backward integral of the squared impulse
/// response, which equals the ensemble average of the decay of many noise bursts. In dB re its start.
/// Both channels' energies are summed.
std::vector<double> energyDecayCurve (const std::vector<double>& left, const std::vector<double>& right)
{
    std::vector<double> edc (left.size());
    double sum = 0.0;
    for (size_t i = left.size(); i-- > 0;)
    {
        sum += left[i] * left[i] + right[i] * right[i];
        edc[i] = sum;
    }
    const auto total = edc[0];
    for (auto& v : edc)
        v = 10.0 * std::log10 (std::max (v / total, 1.0e-30));
    return edc;
}

/// T60 from a least-squares line through the EDC between -5 and -35 dB (the T30 method, extrapolated
/// to 60 dB): T60 = -60 / slope.
double t60FromEdc (const std::vector<double>& edcDb)
{
    size_t start = 0, end = 0;
    while (start < edcDb.size() && edcDb[start] > -5.0)
        ++start;
    end = start;
    while (end < edcDb.size() && edcDb[end] > -35.0)
        ++end;
    if (end >= edcDb.size() || end - start < 10)
        return 0.0;

    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const auto count = (double) (end - start);
    for (size_t i = start; i < end; ++i)
    {
        const auto t = (double) i / fs;
        sx += t;
        sy += edcDb[i];
        sxx += t * t;
        sxy += t * edcDb[i];
    }
    const auto slope = (count * sxy - sx * sy) / (count * sxx - sx * sx); // dB per second
    return -60.0 / slope;
}

struct Bands
{
    double low, mid, high;
};

constexpr double lowBandLo = 44.2, lowBandHi = 88.4;     // octave around 62.5 Hz
constexpr double midBandLo = 707.0, midBandHi = 1414.0;  // octave around 1 kHz
constexpr double highBandLo = 8000.0, highBandHi = 16000.0; // octave around 11.3 kHz

/// The T60s of a reverb's impulse response in the low, mid, and high bands, and the EDCs (for plots).
Bands measureT60 (const Reverb::Settings& settings, double seconds, std::array<std::vector<double>, 3>* edcs = nullptr)
{
    Reverb r;
    r.setSettings (settings);
    r.prepare (fs, blockSize);
    const auto out = run (r, impulse ((size_t) (seconds * fs)));

    const double edges[3][2] = { { lowBandLo, lowBandHi }, { midBandLo, midBandHi }, { highBandLo, highBandHi } };
    double t60[3];
    for (int b = 0; b < 3; ++b)
    {
        const auto edc = energyDecayCurve (band (out.left, edges[b][0], edges[b][1]), band (out.right, edges[b][0], edges[b][1]));
        t60[b] = t60FromEdc (edc);
        if (edcs != nullptr)
            (*edcs)[(size_t) b] = edc;
    }
    return { t60[0], t60[1], t60[2] };
}

// ---- Clicks -----------------------------------------------------------------------------------

/// The largest sample step in each 10 ms window, relative to the largest step a pure sine of frequency
/// f could make at the local peak level, 2 A sin(pi f / fs), with A the peak over the window and 20 ms
/// either side (so a level that's growing fast doesn't count as a step). About 1 for a smooth tone;
/// a discontinuity of 1% of the level adds about 0.8 at 100 Hz. A tone whose pitch moves (a Size glide
/// Doppler-shifts the tail on every trip around the network) raises it by the frequency ratio.
double worstStepRatio (const std::vector<float>& x, size_t start, size_t end, double f)
{
    const auto window = (size_t) (0.01 * fs), margin = (size_t) (0.02 * fs);
    const auto perUnit = 2.0 * std::sin (juce::MathConstants<double>::pi * f / fs);
    double worst = 0.0;
    for (size_t w = start; w + window <= std::min (end, x.size()); w += window)
    {
        double peak = 0.0;
        for (size_t i = w > margin ? w - margin : 0; i < std::min (x.size(), w + window + margin); ++i)
            peak = std::max (peak, (double) std::abs (x[i]));
        if (peak < 1.0e-4)
            continue;
        worst = std::max (worst, maxStep (x, w, w + window) / (perUnit * peak));
    }
    return worst;
}

double stepRatio (const Stereo& s, size_t start, size_t end, double f)
{
    return std::max (worstStepRatio (s.left, start, end, f), worstStepRatio (s.right, start, end, f));
}

/// The energy above 3 kHz (4th-order Butterworth high-pass) in the worst 10 ms window of [start, end),
/// in dB relative to the input tone's own energy over a window (amplitude A: A^2/2 per sample). Under a
/// 100 Hz tone there's essentially none (the high-pass alone takes 100 Hz down 118 dB), and a pitch
/// glide or a level change doesn't add any, but a discontinuity is broadband: a step of 1% of the tone's
/// amplitude lands around -60 dB here, a slope corner from a 30 ms linear fade around -85 dB.
double worstHighFrequencyDb (const std::vector<float>& x, size_t start, size_t end, double toneAmplitude)
{
    using ampsim::Svf;
    Svf a, b;
    a.setCoefficients (Svf::design (Svf::Type::highpass, 3000.0, 0.5411961001461970, 0.0, fs));
    b.setCoefficients (Svf::design (Svf::Type::highpass, 3000.0, 1.3065629648763766, 0.0, fs));
    std::vector<double> high (x.size());
    for (size_t n = 0; n < x.size(); ++n)
        high[n] = b.processSample (a.processSample ((double) x[n]));

    const auto window = (size_t) (0.01 * fs);
    const auto reference = 0.5 * toneAmplitude * toneAmplitude * (double) window;
    double worst = -400.0;
    for (size_t w = start; w + window <= std::min (end, x.size()); w += window)
    {
        double eh = 0.0;
        for (size_t i = w; i < w + window; ++i)
            eh += high[i] * high[i];
        worst = std::max (worst, 10.0 * std::log10 (std::max (eh, 1.0e-300) / reference));
    }
    return worst;
}

double highFrequencyDb (const Stereo& s, size_t start, size_t end, double toneAmplitude)
{
    return std::max (worstHighFrequencyDb (s.left, start, end, toneAmplitude), worstHighFrequencyDb (s.right, start, end, toneAmplitude));
}

/// A tone that fades in over 50 ms along a half cosine, so its own start adds nothing broadband.
std::vector<float> smoothTone (double f, double amplitude, int numSamples)
{
    auto x = sine (f, amplitude, numSamples);
    const auto fade = (int) (0.05 * fs);
    for (int n = 0; n < std::min (fade, numSamples); ++n)
        x[(size_t) n] *= (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * n / fade));
    return x;
}

// ---- Misc -------------------------------------------------------------------------------------

/// Normalized cross-correlation of left and right over [start, end), the largest |value| over lags
/// within +-maxLag samples.
double crossCorrelation (const Stereo& s, size_t start, size_t end, int maxLag, double* atZero = nullptr)
{
    double best = 0.0;
    const auto el = energy (s.left, start, end), er = energy (s.right, start, end);
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (size_t i = start; i < end; ++i)
        {
            const auto j = (long) i + lag;
            if (j >= (long) start && j < (long) end)
                sum += (double) s.left[i] * (double) s.right[(size_t) j];
        }
        const auto rho = sum / std::sqrt (el * er);
        if (lag == 0 && atZero != nullptr)
            *atZero = rho;
        best = std::max (best, std::abs (rho));
    }
    return best;
}

double percentile (std::vector<double> v, double p)
{
    std::sort (v.begin(), v.end());
    return v[(size_t) std::min ((double) v.size() - 1.0, p * (double) (v.size() - 1))];
}

juce::String describe (const rtcheck::Counts& c)
{
    return juce::String (c.allocations) + " allocations, " + juce::String (c.frees) + " frees, " + juce::String (c.blockingLocks)
           + " blocking locks";
}

DattorroPlate::Parameters plateFromJson (const juce::var& v)
{
    DattorroPlate::Parameters p;
    p.decay = (double) v["decay"];
    p.decayDiffusion1 = (double) v["decay_diffusion_1"];
    p.inputDiffusion1 = (double) v["input_diffusion_1"];
    p.inputDiffusion2 = (double) v["input_diffusion_2"];
    p.bandwidth = (double) v["bandwidth"];
    p.damping = (double) v["damping"];
    p.excursion = (double) v["excursion"];
    return p;
}

class ReverbTests final : public juce::UnitTest
{
public:
    ReverbTests() : juce::UnitTest ("Reverb", "ampsim") {}

    void runTest() override
    {
        plateGolden();
        matrixOrthogonality();
        lineLengths();
        decayAccuracy();
        plateDecay();
        levels();
        earlyReflections();
        stability();
        freeze();
        stereo();
        denormals();
        shimmer();
        latencyAndMixLaw();
        bufferSizes();
        spillover();
        clicks();
        ducking();
        realtime();
        cpu();
        renders();
    }

private:
    // ------------------------------------------------------------------------------------------
    void plateGolden()
    {
        beginTest ("the plate matches the Python reference built from Dattorro's paper (golden renders, limit -100 dB)");

        const auto cases = juce::JSON::parse (fixture ("cases.json"));
        const auto music = readWav (fixture ("input_music.wav"));
        expect (music.getNumSamples() > 0, "fixtures missing: run prototypes/plate_reverb.py --golden tests/fixtures/reverb");
        const std::vector<float> musicMono (music.getReadPointer (0), music.getReadPointer (0) + music.getNumSamples());
        juce::StringArray results;

        for (const auto* name : { "impulse", "modulated", "block" })
        {
            const auto c = cases[name];
            const auto input = c["input"].toString() == "impulse" ? impulse (musicMono.size()) : musicMono;
            const auto expected = readWav (fixture ("expected_" + juce::String (name) + ".wav"));
            Stereo out;

            if (c["kind"].toString() == "core")
            {
                // The engine on its own, at Table 1's values.
                DattorroPlate plate;
                plate.prepare (fs);
                auto p = plateFromJson (c["parameters"]);
                p.modRateHz = (double) c["mod_rate_hz"];
                plate.setParameters (p);
                plate.snapToTargets();
                plate.reset();

                out.left.resize (input.size());
                out.right.resize (input.size());
                for (size_t start = 0; start < input.size(); start += blockSize)
                {
                    const auto len = (int) std::min ((size_t) blockSize, input.size() - start);
                    plate.process (input.data() + start, out.left.data() + start, out.right.data() + start, len);
                }
            }
            else
            {
                // The whole block in plate mode: knob mapping, low shelf, wet only, nothing else in the way.
                const auto& s = c["settings"];
                auto settings = wetOnly (Engine::plate);
                settings.decaySeconds = (float) (double) s["decay_seconds"];
                settings.lowDecayMultiplier = (float) (double) s["low_multiplier"];
                settings.highDecayMultiplier = (float) (double) s["high_multiplier"];
                settings.diffusion = (float) (double) s["diffusion"];
                settings.modDepth = (float) (double) s["mod_depth"];
                settings.modRateHz = (float) (double) s["mod_rate_hz"];
                Reverb r;
                r.setSettings (settings);
                r.prepare (fs, blockSize);
                out = run (r, input);
                for (auto* ch : { &out.left, &out.right })
                    for (auto& v : *ch)
                        v = (float) (v / Reverb::plateLevel);
            }

            auto worst = -400.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                std::vector<float> e (expected.getReadPointer (ch), expected.getReadPointer (ch) + expected.getNumSamples());
                worst = std::max (worst, relativeErrorDb (ch == 0 ? out.left : out.right, e));
            }
            expectLessThan (worst, -100.0, name);
            results.add (juce::String (name) + " " + juce::String (worst, 1) + " dB");
        }

        // The mapped coefficients of the block case, for the record.
        const auto mapped = cases["block"]["mapped"];
        logMessage ("  -> C++ vs Python (tests/fixtures/reverb, from prototypes/plate_reverb.py), 1 s each, both channels: "
                    + results.joinIntoString ("; "));
        logMessage ("  -> cases: impulse = Table 1 with modulation off; modulated = Table 1 with EXCURSION 16 at 1 Hz on plucks; "
                    "block = Reverb in plate mode, 3 s, low x1.5, high x0.375 (mapped: decay "
                    + juce::String ((double) mapped["decay"], 4) + ", shelves " + juce::String ((double) mapped["low_shelf_db"], 2) + " dB low and "
                    + juce::String ((double) mapped["high_shelf_db"], 2) + " dB high per half, damping off)");
    }

    // ------------------------------------------------------------------------------------------
    void matrixOrthogonality()
    {
        beginTest ("the feedback matrix (Householder, then a rotation) is orthogonal: M M^T = I");
        juce::StringArray results;
        for (int n : { 8, 16 })
        {
            // Column j of M is M applied to the unit vector e_j.
            std::vector<std::vector<double>> m ((size_t) n, std::vector<double> ((size_t) n));
            for (int j = 0; j < n; ++j)
            {
                std::vector<double> e ((size_t) n, 0.0);
                e[(size_t) j] = 1.0;
                FeedbackDelayNetwork::feedbackMatrix (e.data(), n);
                for (int i = 0; i < n; ++i)
                    m[(size_t) i][(size_t) j] = e[(size_t) i];
            }
            double worst = 0.0, largestSelf = 0.0;
            for (int i = 0; i < n; ++i)
            {
                largestSelf = std::max (largestSelf, std::abs (m[(size_t) i][(size_t) i]));
                for (int k = 0; k < n; ++k)
                {
                    double dot = 0.0;
                    for (int j = 0; j < n; ++j)
                        dot += m[(size_t) i][(size_t) j] * m[(size_t) k][(size_t) j];
                    worst = std::max (worst, std::abs (dot - (i == k ? 1.0 : 0.0)));
                }
            }
            expectLessThan (worst, 1.0e-14);
            results.add ("N = " + juce::String (n) + ": max |M M^T - I| = " + juce::String (worst, 17) + ", largest diagonal entry "
                         + juce::String (largestSelf, 3) + " (plain Householder: " + juce::String (1.0 - 2.0 / n, 3) + ")");
        }
        logMessage ("  -> " + results.joinIntoString ("; "));
    }

    // ------------------------------------------------------------------------------------------
    void lineLengths()
    {
        beginTest ("line lengths: distinct primes (so pairwise coprime) spread over 30 to 100 ms at Size scale 1, scaled by Size");

        juce::StringArray results;
        for (const int count : { 8, 16 })
        {
            FeedbackDelayNetwork network (count);
            network.prepare (fs);
            bool allPrime = true, allCoprime = true;
            for (const double scale : { 0.25, 0.5, 0.707, 1.0, 1.32, 2.0 })
            {
                std::array<int, FeedbackDelayNetwork::maxLines> lengths {};
                network.lengthsFor (scale, lengths);
                for (int i = 0; i < count; ++i)
                {
                    for (int d = 2; d * d <= lengths[(size_t) i]; ++d)
                        allPrime = allPrime && lengths[(size_t) i] % d != 0;
                    for (int j = 0; j < i; ++j)
                        allCoprime = allCoprime && std::gcd (lengths[(size_t) i], lengths[(size_t) j]) == 1;
                }
                const auto wanted0 = 30.0 * scale * fs / 1000.0, wantedLast = 100.0 * scale * fs / 1000.0;
                expectWithinAbsoluteError ((double) lengths[0], wanted0, 0.02 * wanted0);
                expectWithinAbsoluteError ((double) lengths[(size_t) count - 1], wantedLast, 0.02 * wantedLast);
                if (std::abs (scale - 1.0) < 1.0e-9)
                {
                    juce::StringArray ms;
                    for (int i = 0; i < count; ++i)
                        ms.add (juce::String (lengths[(size_t) i]));
                    results.add (juce::String (count) + " lines at scale 1: " + ms.joinIntoString (", ") + " samples");
                }
            }
            expect (allPrime);
            expect (allCoprime);
        }
        logMessage ("  -> every length prime and every pair coprime at scales 0.25 to 2; " + results.joinIntoString ("; "));
    }

    // ------------------------------------------------------------------------------------------
    void decayAccuracy()
    {
        beginTest ("decay: T60 from the energy decay curve (Schroeder, fit -5 to -35 dB) is within 10% of the setting, low and high bands");

        struct Case
        {
            Engine engine;
            float size, decay, low, high, depth;
        };
        const std::vector<Case> cases {
            { Engine::room, 0.4f, 0.8f, 1.5f, 0.5f, 0.3f },
            { Engine::room, 0.6f, 1.6f, 1.0f, 0.35f, 0.3f },
            { Engine::hall, 0.5f, 2.5f, 1.3f, 0.6f, 0.3f },
            { Engine::hall, 0.8f, 5.0f, 0.8f, 0.4f, 0.3f },
            { Engine::hall, 0.6f, 3.0f, 2.0f, 1.0f, 0.0f },
        };

        std::array<std::vector<double>, 3> plotEdcs;
        Bands plotTargets {};
        double worstError = 0.0;

        for (size_t i = 0; i < cases.size(); ++i)
        {
            const auto& c = cases[i];
            auto s = wetOnly (c.engine);
            s.size = c.size;
            s.decaySeconds = c.decay;
            s.lowDecayMultiplier = c.low;
            s.highDecayMultiplier = c.high;
            s.modDepth = c.depth;
            const Bands target { c.decay * c.low, c.decay, c.decay * c.high };
            const auto seconds = 1.6 * std::max ({ target.low, target.mid, target.high }) + 1.0;

            std::array<std::vector<double>, 3> edcs;
            const auto m = measureT60 (s, seconds, &edcs);
            const auto error = [] (double measured, double wanted) { return 100.0 * (measured - wanted) / wanted; };
            const Bands e { error (m.low, target.low), error (m.mid, target.mid), error (m.high, target.high) };
            expectLessThan (std::abs (e.low), 10.0, "low band");
            expectLessThan (std::abs (e.high), 10.0, "high band");
            expectLessThan (std::abs (e.mid), 10.0, "mid band");
            worstError = std::max ({ worstError, std::abs (e.low), std::abs (e.mid), std::abs (e.high) });

            logMessage ("  -> " + nameOf (c.engine) + " size " + juce::String (c.size, 2) + " (lines " + juce::String (s.engine == Engine::room ? 8 : 16)
                        + ", scale " + juce::String (Reverb::sizeScale (c.size), 3) + "), decay " + juce::String (c.decay, 1) + " s, low x"
                        + juce::String (c.low, 2) + ", high x" + juce::String (c.high, 2) + ", modulation " + juce::String (c.depth, 1)
                        + ": low " + juce::String (m.low, 3) + " s (set " + juce::String (target.low, 3) + ", " + juce::String (e.low, 1)
                        + "%), mid " + juce::String (m.mid, 3) + " s (" + juce::String (e.mid, 1) + "%), high " + juce::String (m.high, 3)
                        + " s (set " + juce::String (target.high, 3) + ", " + juce::String (e.high, 1) + "%)");

            if (i == 2)
            {
                plotEdcs = edcs;
                plotTargets = target;
            }
        }

        // Plot: the Hall case's three EDCs against the ideal slopes.
        PlotOptions o;
        o.title = "Energy decay curves, Hall, decay 2.5 s, low x1.3, high x0.6 (dashed: the set T60s from -5 dB)";
        o.xLabel = "Time (s)";
        o.yLabel = "EDC (dB)";
        o.xMin = 0.0; o.xMax = 4.0; o.yMin = -70.0; o.yMax = 0.0;
        std::vector<PlotSeries> series;
        const char* labels[3] = { "low band (44-88 Hz)", "mid band (0.7-1.4 kHz)", "high band (8-16 kHz)" };
        const double targets[3] = { plotTargets.low, plotTargets.mid, plotTargets.high };
        for (int b = 0; b < 3; ++b)
        {
            PlotSeries measured { labels[b], {}, {}, plotColour (b), 2.0f };
            PlotSeries ideal { "", {}, {}, plotColour (b), 1.2f, true };
            const auto& edc = plotEdcs[(size_t) b];
            for (size_t n = 0; n < edc.size() && (double) n / fs <= o.xMax; n += 48)
            {
                measured.x.push_back ((double) n / fs);
                measured.y.push_back (edc[n]);
            }
            // The set T60's slope, through the point where the measured curve crosses -5 dB (where the fit starts).
            size_t crossing = 0;
            while (crossing < edc.size() && edc[crossing] > -5.0)
                ++crossing;
            const auto t5 = (double) crossing / fs;
            for (double t = t5; t <= o.xMax; t += 0.05)
            {
                ideal.x.push_back (t);
                ideal.y.push_back (-5.0 - 60.0 * (t - t5) / targets[b]);
            }
            series.push_back (measured);
            series.push_back (ideal);
        }
        const auto png = proofDir().getChildFile ("reverb_decay_edc.png");
        expect (savePlot (png, o, series));
        logMessage ("  -> largest error over 5 settings x 3 bands: " + juce::String (worstError, 1) + "%; plot " + png.getFullPathName());
    }

    // ------------------------------------------------------------------------------------------
    void plateDecay()
    {
        beginTest ("plate decay: the mapped coefficients land near the set T60s (its 725 ms loop makes short bands run long)");

        // The plate's T60 follows from its loop gain only on average: its lattices' group delay swings
        // between 0.2 and 5.7 times their length, the modes near the peaks decay slowest and dominate
        // the curve, and a short decay is mostly over within one 725 ms trip. Measured, not assumed.
        struct Case
        {
            float decay, low, high;
        };
        double worst = 0.0;
        for (const auto& c : std::vector<Case> { { 1.0f, 1.0f, 1.0f }, { 2.0f, 1.0f, 1.0f }, { 4.0f, 1.0f, 1.0f }, { 2.0f, 1.5f, 0.5f }, { 3.0f, 0.7f, 0.3f } })
        {
            auto s = wetOnly (Engine::plate);
            s.decaySeconds = c.decay;
            s.lowDecayMultiplier = c.low;
            s.highDecayMultiplier = c.high;
            const Bands target { c.decay * c.low, c.decay, c.decay * c.high };
            const auto m = measureT60 (s, 1.6 * std::max (target.low, target.mid) + 1.0);
            const auto error = [] (double measured, double wanted) { return 100.0 * (measured - wanted) / wanted; };
            const Bands e { error (m.low, target.low), error (m.mid, target.mid), error (m.high, target.high) };
            for (auto v : { e.low, e.mid, e.high })
            {
                expectLessThan (std::abs (v), 25.0);
                worst = std::max (worst, std::abs (v));
            }
            logMessage ("  -> Plate decay " + juce::String (c.decay, 1) + " s, low x" + juce::String (c.low, 1) + ", high x" + juce::String (c.high, 1)
                        + ": low " + juce::String (m.low, 3) + " s (" + juce::String (e.low, 1) + "%), mid " + juce::String (m.mid, 3) + " s ("
                        + juce::String (e.mid, 1) + "%), high " + juce::String (m.high, 3) + " s (" + juce::String (e.high, 1) + "%)");
        }
        logMessage ("  -> largest plate error " + juce::String (worst, 1) + "% (limit 25%; the FDN engines are held to 10%)");
    }

    // ------------------------------------------------------------------------------------------
    void levels()
    {
        beginTest ("levels: the late reverb sits near the input level at a 2 s decay on every engine, and Size doesn't change it");

        const auto measure = [] (Engine engine, float size, float decay, float balance)
        {
            auto s = wetOnly (engine);
            s.size = size;
            s.decaySeconds = decay;
            s.earlyLate = balance;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);
            const auto input = whiteNoise ((int) (8.0 * fs), 0.1f, 2);
            const auto out = run (r, input);
            const auto from = (size_t) (4.0 * fs);
            return energyDb (out, from, out.left.size()) - 10.0 * std::log10 (2.0 * energy (input, from, input.size()));
        };

        juce::StringArray results;
        for (const auto engine : allEngines)
        {
            const auto late = measure (engine, 0.5f, 2.0f, 1.0f);
            expectWithinAbsoluteError (late, 0.0, 1.5, nameOf (engine));
            juce::String line = nameOf (engine) + " late " + juce::String (late, 2) + " dB";
            if (engine != Engine::plate)
            {
                const auto small = measure (engine, 0.2f, 2.0f, 1.0f), large = measure (engine, 0.9f, 2.0f, 1.0f);
                const auto longer = measure (engine, 0.5f, 4.0f, 1.0f);
                const auto early = measure (engine, 0.5f, 2.0f, 0.0f);
                expectLessThan (std::max ({ std::abs (small - late), std::abs (large - late) }), 1.5, nameOf (engine));
                line << " (size 0.2: " << juce::String (small, 2) << ", 0.9: " << juce::String (large, 2) << "; decay 4 s: " << juce::String (longer, 2)
                     << "), early only " << juce::String (early, 2) << " dB";
            }
            else
            {
                line << " (decay 4 s: " << juce::String (measure (engine, 0.5f, 4.0f, 1.0f), 2) << ")";
            }
            results.add (line);
        }
        logMessage ("  -> steady white noise, wet re input: " + results.joinIntoString ("; "));
    }

    // ------------------------------------------------------------------------------------------
    void earlyReflections()
    {
        beginTest ("early reflections land where the pattern says, after the pre-delay, scaled by Size");

        juce::StringArray results;
        for (const auto& [size, preDelay] : std::vector<std::pair<float, float>> { { 0.2f, 0.0f }, { 2.0f / 3.0f, 0.0f }, { 1.0f, 25.0f } })
        {
            auto s = wetOnly (Engine::hall);
            s.earlyLate = 0.0f; // early only
            s.size = size;
            s.preDelayMs = preDelay;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);
            const auto out = run (r, impulse ((size_t) (0.5 * fs)));

            // The first tap of each side: 4.3 ms (left) and 5.9 ms (right) at scale 1, read with Hermite
            // interpolation, so the energy peaks on the sample nearest the exact time.
            const auto scale = Reverb::sizeScale (size);
            for (int side = 0; side < 2; ++side)
            {
                const auto& y = side == 0 ? out.left : out.right;
                size_t first = 1;
                while (first < y.size() && std::abs (y[first]) < 1.0e-3f)
                    ++first;
                size_t peak = first;
                for (size_t n = first; n < first + 4; ++n)
                    if (std::abs (y[n]) > std::abs (y[peak]))
                        peak = n;
                const auto expected = (preDelay + (side == 0 ? 4.3 : 5.9) * scale) * fs / 1000.0;
                expectWithinAbsoluteError ((double) peak, expected, 0.51);
                if (side == 0)
                    results.add ("size " + juce::String (size, 2) + " (x" + juce::String (scale, 3) + "), pre-delay " + juce::String (preDelay, 0) + " ms: first left tap at sample "
                                 + juce::String ((int) peak) + " (expected " + juce::String (expected, 1) + ")");
            }
        }
        logMessage ("  -> " + results.joinIntoString ("; "));
    }

    // ------------------------------------------------------------------------------------------
    void stability()
    {
        beginTest ("stability: at maximum decay (30 s, low x4, high x2, full modulation) the tail's energy never grows");

        for (const auto engine : allEngines)
        {
            for (const float size : { 0.0f, 1.0f })
            {
                if (engine == Engine::plate && size > 0.0f)
                    continue; // the plate has no Size
                auto s = wetOnly (engine);
                s.decaySeconds = 30.0f;
                s.lowDecayMultiplier = 4.0f;
                s.highDecayMultiplier = 2.0f;
                s.modDepth = 1.0f;
                s.modRateHz = 5.0f;
                s.diffusion = 1.0f;
                s.size = size;
                s.earlyLate = 0.5f;
                Reverb r;
                r.setSettings (s);
                r.prepare (fs, blockSize);

                // 3 s of noise, then 12 s of silence, in half-second windows.
                const auto input = concat (whiteNoise ((int) (3.0 * fs), 0.5f, 21), std::vector<float> ((size_t) (12.0 * fs), 0.0f));
                std::vector<double> stored;
                const auto window = (size_t) (0.5 * fs);
                const auto out = run (r, input, [&] (size_t start)
                {
                    if (start >= (size_t) (3.0 * fs) && start % window < (size_t) blockSize)
                        stored.push_back (r.storedEnergy());
                });

                std::vector<double> windows;
                for (size_t w = (size_t) (3.0 * fs); w + window <= out.left.size(); w += window)
                    windows.push_back (energyDb (out, w, w + window));
                double largestRise = -100.0, runningMax = windows.front(), largestOverMax = -100.0;
                for (size_t k = 1; k < windows.size(); ++k)
                {
                    largestRise = std::max (largestRise, windows[k] - windows[k - 1]);
                    largestOverMax = std::max (largestOverMax, windows[k] - runningMax);
                    runningMax = std::max (runningMax, windows[k]);
                }
                double storedRise = -100.0;
                for (size_t k = 1; k < stored.size(); ++k)
                    storedRise = std::max (storedRise, 10.0 * std::log10 (stored[k] / stored[k - 1]));

                const auto totalDrop = windows.back() - windows.front();
                expectLessThan (largestOverMax, 0.5, nameOf (engine));
                expectLessThan (totalDrop, -1.0, nameOf (engine));
                expect (std::isfinite (windows.back()));
                logMessage ("  -> " + nameOf (engine) + (engine == Engine::plate ? juce::String() : " size " + juce::String (size, 0)) + ": over 12 s of tail, "
                            "output energy fell " + juce::String (-totalDrop, 1) + " dB; largest half-second rise " + juce::String (largestRise, 2)
                            + " dB, largest above every earlier window " + juce::String (largestOverMax, 2) + " dB; stored energy's largest half-second rise "
                            + juce::String (storedRise, 3) + " dB");
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    void freeze()
    {
        beginTest ("freeze holds the tail's energy for 60 s while the input keeps playing (FDN and plate)");

        for (const auto engine : allEngines)
        {
            auto s = wetOnly (engine);
            s.decaySeconds = 2.0f;
            s.earlyLate = 0.5f;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);

            // 1 s of noise, then freeze, with noise still coming in for the whole 61 s that follow.
            const auto total = (size_t) (62.0 * fs);
            const auto input = whiteNoise ((int) total, 0.3f, 5);
            const auto freezeAt = (size_t) (1.0 * fs);
            std::vector<double> stored;
            const auto out = run (r, input, [&] (size_t start)
            {
                if (start == freezeAt)
                {
                    auto frozen = s;
                    frozen.freeze = true;
                    r.setSettings (frozen);
                }
                if (start >= freezeAt + (size_t) fs && (start - freezeAt) % (size_t) fs == 0)
                    stored.push_back (r.storedEnergy());
            });

            // From 1 s after the stomp (the decay glide and the diffusers have finished) to 61 s.
            const auto from = freezeAt + (size_t) fs;
            const auto window = (size_t) (5.0 * fs);
            std::vector<double> windows;
            for (size_t w = from; w + window <= out.left.size(); w += window)
                windows.push_back (energyDb (out, w, w + window));
            const auto [lo, hi] = std::minmax_element (windows.begin(), windows.end());
            double storedDrift = 0.0;
            for (auto e : stored)
                storedDrift = std::max (storedDrift, std::abs (10.0 * std::log10 (e / stored.front())));
            const auto storedEnd = 10.0 * std::log10 (stored.back() / stored.front());

            expectLessThan (storedDrift, 0.01, nameOf (engine));
            expectLessThan (*hi - *lo, 0.5, nameOf (engine));
            logMessage ("  -> " + nameOf (engine) + ": stored energy over 60 s (sampled every second): largest drift "
                        + juce::String::formatted ("%.2e", storedDrift) + " dB, end vs start " + juce::String::formatted ("%.2e", storedEnd)
                        + " dB (limit 0.01 dB); output energy in 5 s windows spans " + juce::String (*hi - *lo, 3)
                        + " dB (limit 0.5 dB), with noise playing into it the whole time");
        }
    }

    // ------------------------------------------------------------------------------------------
    void stereo()
    {
        beginTest ("stereo: left and right are decorrelated at full width and identical at zero width");
        for (const auto engine : allEngines)
        {
            juce::StringArray results;
            for (const float w : { 1.0f, 0.5f, 0.0f })
            {
                auto s = wetOnly (engine);
                s.width = w;
                s.earlyLate = 0.5f;
                Reverb r;
                r.setSettings (s);
                r.prepare (fs, blockSize);
                const auto input = concat (whiteNoise ((int) (0.2 * fs), 0.5f, 8), std::vector<float> ((size_t) (2.0 * fs), 0.0f));
                const auto out = run (r, input);
                double atZero = 0.0;
                const auto best = crossCorrelation (out, (size_t) (0.05 * fs), out.left.size(), 48, &atZero);
                if (w == 1.0f)
                    expectLessThan (best, 0.25, nameOf (engine));
                if (w == 0.0f)
                    expectGreaterThan (atZero, 0.9999, nameOf (engine));
                results.add ("width " + juce::String (w, 1) + ": " + juce::String (atZero, 3) + " (max over +-1 ms " + juce::String (best, 3) + ")");
            }
            logMessage ("  -> " + nameOf (engine) + ", L/R correlation of the wet from 50 ms to 2.2 s after a mono noise burst: " + results.joinIntoString (", "));
        }
    }

    // ------------------------------------------------------------------------------------------
    void denormals()
    {
        beginTest ("denormals: no CPU spike as a tail decays to silence (flush-to-zero on, as in the processor)");

        for (const auto engine : allEngines)
        {
            for (const bool flush : { true, false })
            {
                // Half a second of noise, then 20 s of silence: -60 dB a second takes the tail through
                // float's smallest normal numbers (-758 dB) around 13 s.
                const auto input = concat (whiteNoise ((int) (0.5 * fs), 0.5f, 3), std::vector<float> ((size_t) (20.0 * fs), 0.0f));
                const auto perSecond = (size_t) (fs / blockSize);

                // Median block time in each second of the tail, against the first second after the input.
                // Other processes can only make a run slower, so each second's time is the fastest of three
                // runs: a real denormal slowdown shows in all three, a burst of load elsewhere doesn't.
                std::vector<double> medians;
                Stereo out;
                for (int run = 0; run < 3; ++run)
                {
                    auto s = wetOnly (engine);
                    s.decaySeconds = 1.0f;
                    s.earlyLate = 0.5f;
                    Reverb r;
                    r.setSettings (s);
                    r.prepare (fs, blockSize);

                    std::vector<double> blockMicros;
                    out = Stereo { input, input };
                    {
                        std::unique_ptr<juce::ScopedNoDenormals> noDenormals;
                        if (flush)
                            noDenormals = std::make_unique<juce::ScopedNoDenormals>();
                        for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
                        {
                            float* channels[2] = { out.left.data() + start, out.right.data() + start };
                            const auto t0 = std::chrono::steady_clock::now();
                            r.process (juce::dsp::AudioBlock<float> (channels, 2, blockSize), {});
                            const auto t1 = std::chrono::steady_clock::now();
                            blockMicros.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
                        }
                    }

                    size_t second = 0;
                    for (size_t b = (size_t) (0.5 * fs) / blockSize; b + perSecond <= blockMicros.size(); b += perSecond, ++second)
                    {
                        const auto median = percentile (std::vector<double> (blockMicros.begin() + (long) b, blockMicros.begin() + (long) (b + perSecond)), 0.5);
                        if (run == 0)
                            medians.push_back (median);
                        else
                            medians[second] = std::min (medians[second], median);
                    }
                }
                const auto slowest = *std::max_element (medians.begin(), medians.end());
                const auto ratio = slowest / medians.front();
                const auto lastRatio = medians.back() / medians.front();

                // When did the output reach exact digital silence (and stay there)?
                size_t silentFrom = out.left.size();
                while (silentFrom > 0 && out.left[silentFrom - 1] == 0.0f && out.right[silentFrom - 1] == 0.0f)
                    --silentFrom;

                if (flush)
                    expectLessThan (ratio, 1.5, nameOf (engine));
                logMessage ("  -> " + nameOf (engine) + (flush ? ", flush-to-zero on: " : ", flush-to-zero OFF (for comparison): ")
                            + "median block " + juce::String (medians.front(), 2) + " us in the first second of the tail, "
                            + juce::String (medians.back(), 2) + " us in the last (x" + juce::String (lastRatio, 2) + "); slowest second x"
                            + juce::String (ratio, 2) + "; output exactly zero from " + juce::String ((double) silentFrom / fs - 0.5, 2) + " s after the input stopped");
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    void shimmer()
    {
        beginTest ("shimmer: an octave-up shifter in the feedback loop builds the octave into the tail, never adds energy (even frozen), and costs nothing when off");

        // The level of a pure tone in x[from, to), by correlation with a Hann-windowed complex exponential.
        const auto toneDb = [] (const std::vector<float>& x, size_t from, size_t to, double f)
        {
            double re = 0.0, im = 0.0, w = 0.0;
            const auto n = to - from;
            for (size_t i = 0; i < n; ++i)
            {
                const auto hann = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * (double) i / (double) n);
                const auto phase = 2.0 * juce::MathConstants<double>::pi * f * (double) (from + i) / fs;
                re += hann * x[from + i] * std::cos (phase);
                im += hann * x[from + i] * std::sin (phase);
                w += hann;
            }
            return toDb (2.0 * std::sqrt (re * re + im * im) / w + 1.0e-12);
        };
        const auto render = [] (float shimmerAmount, float semitones, const std::vector<float>& input, bool freeze = false, float decay = 6.0f)
        {
            auto s = wetOnly (Engine::hall);
            s.decaySeconds = decay;
            s.modDepth = 0.0f; // a clean tone: no line modulation
            s.lowCutHz = 20.0f;
            s.highCutHz = 20000.0f;
            s.shimmer = shimmerAmount;
            s.shimmerSemitones = semitones;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);
            if (freeze)
            {
                // Freeze after the input has gone in.
                Stereo out { input, input };
                for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
                {
                    if (start == (size_t) (0.6 * fs) / blockSize * blockSize)
                    {
                        s.freeze = true;
                        r.setSettings (s);
                    }
                    float* channels[] = { out.left.data() + start, out.right.data() + start };
                    r.process (juce::dsp::AudioBlock<float> (channels, 2, blockSize), {});
                }
                return out;
            }
            return run (r, input);
        };

        // A 330 Hz tone for 0.3 s, then the tail, measured from 1.5 to 2.5 s.
        auto tone = sine (330.0, 0.3, (int) (0.3 * fs));
        tone.resize ((size_t) (3.0 * fs), 0.0f);
        const auto from = (size_t) (1.5 * fs), to = (size_t) (2.5 * fs);
        const auto plain = render (0.0f, 12.0f, tone).left;
        const auto octave = render (1.0f, 12.0f, tone).left;
        const auto fifth = render (1.0f, 7.0f, tone).left;
        const auto plainOctave = toneDb (plain, from, to, 660.0) - toneDb (plain, from, to, 330.0);
        const auto shimmerOctave = toneDb (octave, from, to, 660.0) - toneDb (octave, from, to, 330.0);
        const auto shimmerTwo = toneDb (octave, from, to, 1320.0) - toneDb (octave, from, to, 330.0);
        const auto fifthUp = toneDb (fifth, from, to, 330.0 * std::exp2 (7.0 / 12.0)) - toneDb (fifth, from, to, 330.0);
        expectLessThan (plainOctave, -60.0);
        expectGreaterThan (shimmerOctave, -20.0);
        expectGreaterThan (fifthUp, -20.0);

        // Energy never builds: frozen with full shimmer for 30 s, and a 30 s decay with full shimmer.
        auto noise = whiteNoise ((int) (0.5 * fs), 0.3f, 11);
        noise.resize ((size_t) (30.5 * fs), 0.0f);
        const auto frozen = render (1.0f, 12.0f, noise, true);
        const auto longest = render (1.0f, 12.0f, noise, false, 30.0f);
        const auto secondDb = [] (const Stereo& o, double second)
        {
            const auto start = (size_t) (second * fs);
            return toDb (std::sqrt (0.5 * (std::pow (rms (o.left.data() + start, (size_t) fs), 2.0) + std::pow (rms (o.right.data() + start, (size_t) fs), 2.0))));
        };
        double frozenStart = secondDb (frozen, 1.0), frozenMax = -1000.0, frozenEnd = secondDb (frozen, 29.0);
        double longestStart = secondDb (longest, 1.0), longestMax = -1000.0;
        float peak = 0.0f;
        for (int second = 1; second < 30; ++second)
        {
            frozenMax = std::max (frozenMax, secondDb (frozen, second));
            longestMax = std::max (longestMax, secondDb (longest, second));
        }
        for (const auto& o : { frozen, longest })
            for (const auto v : o.left)
                peak = std::max (peak, std::abs (v));
        expectLessThan (frozenMax - frozenStart, 1.0);
        expectLessThan (longestMax - longestStart, 1.0);
        expect (std::isfinite (peak) && peak < 2.0f);

        // Cost: Hall with and without it.
        const auto cost = [] (float shimmerAmount)
        {
            auto s = wetOnly (Engine::hall);
            s.shimmer = shimmerAmount;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);
            const auto input = guitarDI ((int) (2.0 * fs));
            Stereo out { input, input };
            std::vector<double> micros;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                float* channels[] = { out.left.data() + start, out.right.data() + start };
                const auto t0 = std::chrono::steady_clock::now();
                r.process (juce::dsp::AudioBlock<float> (channels, 2, blockSize), {});
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            return percentile (micros, 0.5);
        };
        const auto withoutUs = cost (0.0f), withUs = cost (1.0f);

        logMessage ("  -> a 330 Hz tone into Hall (6 s): from 1.5 to 2.5 s the octave sits " + juce::String (plainOctave, 1) + " dB under the tone without "
                    "shimmer, " + juce::String (shimmerOctave, 1) + " dB with it (two octaves up " + juce::String (shimmerTwo, 1) + " dB); set to +7, the fifth "
                    + juce::String (fifthUp, 1) + " dB");
        logMessage ("  -> frozen with full shimmer for 30 s: level " + juce::String (frozenStart, 1) + " dBFS in the first second, at most "
                    + juce::String (frozenMax, 1) + ", " + juce::String (frozenEnd, 1) + " at the end; a 30 s decay with full shimmer: at most "
                    + juce::String (longestMax - longestStart, 2) + " dB over its first second; peak " + juce::String (peak, 3));
        logMessage ("  -> Hall per 128-sample stereo block: " + juce::String (withoutUs, 1) + " us without shimmer, " + juce::String (withUs, 1)
                    + " us with it (" + juce::String (100.0 * withUs / 2666.7, 2) + "% of the deadline)");
    }

    // ------------------------------------------------------------------------------------------
    void latencyAndMixLaw()
    {
        beginTest ("zero latency for the dry path, and the equal-power mix law's endpoints");

        juce::StringArray results;
        for (const auto engine : allEngines)
        {
            for (const float mix : { 0.0f, 0.25f, 0.5f, 1.0f })
            {
                auto s = wetOnly (engine);
                s.mix = mix;
                s.earlyLate = 0.5f;
                Reverb r;
                r.setSettings (s);
                r.prepare (fs, blockSize);
                expectEquals (r.latencySamples(), 0);

                const auto input = impulse ((size_t) fs);
                const auto out = run (r, input);
                const auto dry = std::cos (mix * juce::MathConstants<double>::halfPi);
                expectWithinAbsoluteError ((double) out.left[0], dry, 1.0e-6);
                expectWithinAbsoluteError ((double) out.right[0], dry, 1.0e-6);

                if (mix == 0.0f)
                {
                    // All dry: the input comes back bit for bit, on both channels, forever after.
                    expectEquals (maxAbsDifference (out.left, input), 0.0);
                    expectEquals (maxAbsDifference (out.right, input), 0.0);
                }
                if (engine == Engine::hall)
                    results.add ("mix " + juce::String (mix, 2) + ": out[0] = " + juce::String (out.left[0], 6) + " (cos = " + juce::String (dry, 6) + ")");
            }
        }

        // Equal power: dry^2 + wet^2 = 1 at every setting. An impulse gives both: sample 0 is the dry gain
        // (the wet starts later), and the tail against a fully wet reverb's tail is the wet gain.
        double worstPower = 0.0;
        for (const float mix : { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f })
        {
            auto s = wetOnly (Engine::hall);
            s.earlyLate = 0.5f;
            Reverb full, mixed;
            full.setSettings (s);
            s.mix = mix;
            mixed.setSettings (s);
            full.prepare (fs, blockSize);
            mixed.prepare (fs, blockSize);
            const auto input = impulse ((size_t) fs);
            const auto wet = run (full, input);
            const auto out = run (mixed, input);
            const auto wetGain = std::sqrt (energy (out.left, 1, out.left.size()) / energy (wet.left, 1, wet.left.size()));
            const auto dryGain = (double) out.left[0];
            worstPower = std::max (worstPower, std::abs (dryGain * dryGain + wetGain * wetGain - 1.0));
        }
        expectLessThan (worstPower, 1.0e-4);
        logMessage ("  -> reported latency 0; an impulse at sample 0 comes out at sample 0 scaled by cos(mix pi/2) on all three engines (Hall: "
                    + results.joinIntoString ("; ") + "); mix 0 returns the input bit for bit; measured dry^2 + wet^2 - 1 at mix 0.1..0.9: largest "
                    + juce::String (worstPower, 7));
    }

    // ------------------------------------------------------------------------------------------
    void bufferSizes()
    {
        beginTest ("any buffer size from 1 to 512 gives the same output, bit for bit");

        const auto input = concat (guitarDI ((int) (1.0 * fs)), std::vector<float> ((size_t) (0.5 * fs), 0.0f));
        juce::StringArray results;
        for (const auto engine : allEngines)
        {
            Reverb::Settings s;
            s.engine = engine;
            s.mix = 0.5f;
            const auto render = [&] (int bufferSize)
            {
                Reverb r;
                r.setSettings (s);
                r.prepare (fs, 512);
                return runInBlocks (input, bufferSize, [&] (juce::dsp::AudioBlock<float>& block, size_t) { r.process (block, {}); });
            };
            const auto reference = render (128);
            double worst = 0.0;
            for (const int size : { 1, 7, 64, 512 })
            {
                const auto out = render (size);
                worst = std::max ({ worst, maxAbsDifference (out.left, reference.left), maxAbsDifference (out.right, reference.right) });
            }
            expectEquals (worst, 0.0, nameOf (engine));
            results.add (nameOf (engine) + " " + juce::String (worst));
        }
        logMessage ("  -> 1.5 s of guitar at buffer sizes 1, 7, 64, and 512 against 128, largest difference: " + results.joinIntoString (", "));
    }

    // ------------------------------------------------------------------------------------------
    void spillover()
    {
        beginTest ("spillover: bypassed mid-tail, the tail keeps ringing, new input stays out, re-enabling never resets, silence goes idle");

        for (const auto engine : allEngines)
        {
            auto s = wetOnly (engine);
            s.mix = 0.5f;
            s.decaySeconds = 2.0f;
            s.earlyLate = 0.5f;

            // A: a burst, a 20 ms gap, bypass at the start of the gap, then a different signal B.
            // Reference: the same burst and then silence, never bypassed.
            const auto burst = whiteNoise ((int) (0.5 * fs), 0.3f, 11);
            const auto bypassAt = burst.size() / blockSize * blockSize;
            const auto gap = (size_t) (0.02 * fs);
            const auto b = whiteNoise ((int) (2.0 * fs), 0.3f, 12);
            auto input = std::vector<float> (burst.begin(), burst.begin() + (long) bypassAt);
            input.resize (bypassAt + gap, 0.0f);
            input.insert (input.end(), b.begin(), b.end());
            auto silentAfter = std::vector<float> (input.begin(), input.begin() + (long) bypassAt);
            silentAfter.resize (input.size(), 0.0f);

            Reverb r, reference;
            r.setSettings (s);
            reference.setSettings (s);
            r.prepare (fs, blockSize);
            reference.prepare (fs, blockSize);
            const auto out = run (r, input, [&] (size_t start) { r.setBypassed (start >= bypassAt); });
            const auto ref = run (reference, silentAfter);

            // After the gap: out = B (dry at unity) + the tail; ref = the tail alone.
            std::vector<float> tail, refTail;
            for (size_t n = bypassAt + gap; n < out.left.size(); ++n)
            {
                tail.push_back (out.left[n] - input[n]);
                refTail.push_back (ref.left[n]);
            }
            const auto error = relativeErrorDb (tail, refTail);
            const auto tailDb = toDb (rms (refTail));
            expectLessThan (error, -100.0, nameOf (engine));
            expectGreaterThan (tailDb, -60.0, nameOf (engine));

            // Re-enabling: bypass for 300 ms of silence mid-tail, then back on. Since nothing new comes in,
            // the output must equal a reverb that was never bypassed, sample for sample.
            Reverb toggled, untouched;
            toggled.setSettings (s);
            untouched.setSettings (s);
            toggled.prepare (fs, blockSize);
            untouched.prepare (fs, blockSize);
            const auto toggleInput = concat (whiteNoise ((int) (0.3 * fs), 0.3f, 13), std::vector<float> ((size_t) (1.5 * fs), 0.0f));
            const auto offAt = (size_t) (0.4 * fs) / blockSize * blockSize, onAt = (size_t) (0.7 * fs) / blockSize * blockSize;
            const auto t = run (toggled, toggleInput, [&] (size_t start) { toggled.setBypassed (start >= offAt && start < onAt); });
            const auto u = run (untouched, toggleInput);
            const auto toggleDifference = std::max (maxAbsDifference (t.left, u.left), maxAbsDifference (t.right, u.right));
            expectEquals (toggleDifference, 0.0, nameOf (engine));

            // Idle: bypassed with a dying tail (decay 0.4 s), the block stops running once the tail is
            // below -120 dBFS for a second, then passes the dry bit for bit; switched on, it reverbs again.
            auto quick = s;
            quick.decaySeconds = 0.4f;
            Reverb sleeper;
            sleeper.setSettings (quick);
            sleeper.prepare (fs, blockSize);
            sleeper.setBypassed (false);
            const auto sleepInput = whiteNoise ((int) (6.0 * fs), 0.3f, 14);
            const auto sleepBypass = (size_t) (0.5 * fs) / blockSize * blockSize;
            size_t idleFrom = 0;
            const auto slept = run (sleeper, sleepInput, [&] (size_t start)
            {
                sleeper.setBypassed (start >= sleepBypass);
                if (sleeper.isIdle() && idleFrom == 0)
                    idleFrom = start;
            });
            expect (sleeper.isIdle(), nameOf (engine) + ": never went idle");
            double idleDifference = 0.0;
            for (size_t n = idleFrom; n < slept.left.size(); ++n)
                idleDifference = std::max (idleDifference, (double) std::abs (slept.left[n] - sleepInput[n]));
            expectEquals (idleDifference, 0.0);

            // Woken: a new burst reverberates just as in a reverb that never slept (same wet energy).
            sleeper.setBypassed (false);
            Reverb fresh;
            fresh.setSettings (quick);
            fresh.prepare (fs, blockSize);
            const auto burst2 = concat (whiteNoise ((int) (0.2 * fs), 0.3f, 15), std::vector<float> ((size_t) (0.5 * fs), 0.0f));
            const auto woken = run (sleeper, burst2);
            const auto reference2 = run (fresh, burst2);
            const auto tailFrom = (size_t) (0.21 * fs); // after the burst: wet only
            const auto wokenTail = energyDb (woken, tailFrom, woken.left.size()) - energyDb (reference2, tailFrom, reference2.left.size());
            expect (! sleeper.isIdle());
            expectWithinAbsoluteError (wokenTail, 0.0, 1.0);

            logMessage ("  -> " + nameOf (engine) + ": after bypass, output minus the new dry vs the never-bypassed tail: " + juce::String (error, 1)
                        + " dB (tail at " + juce::String (tailDb, 1) + " dBFS RMS; new input absent); bypass off and on again mid-tail: max difference "
                        + juce::String (toggleDifference) + " (never reset); idle " + juce::String ((double) (idleFrom - sleepBypass) / fs, 2)
                        + " s after bypass (decay 0.4 s), then dry bit for bit (max difference " + juce::String (idleDifference)
                        + "); switched back on, a new burst's tail is " + juce::String (wokenTail, 2) + " dB from a fresh reverb's");
        }
    }

    // ------------------------------------------------------------------------------------------
    static constexpr double maxStepRatio = 2.0, maxHighFrequencyDb = -80.0;

    void clicks()
    {
        beginTest ("no clicks: sweeping size, decay, mix, pre-delay, and a cut, and switching engines, under a 100 Hz tone");

        // A 100 Hz tone, faded in smoothly and given 2 s to reach a steady state. Every change runs from
        // 2.0 to 3.0 s (the processor sets the knob once per buffer, as a player turning it would), and
        // the measurement covers the change and the 300 ms after it, against the 500 ms before.
        const auto f = 100.0, amplitude = 0.25;
        const auto tone = smoothTone (f, amplitude, (int) (4.5 * fs));
        const auto changeStart = (size_t) (2.0 * fs), changeEnd = (size_t) (3.0 * fs);
        const auto before = (size_t) (1.5 * fs), after = changeEnd + (size_t) (0.3 * fs);
        const auto sweep = [&] (size_t start) { return juce::jlimit (0.0, 1.0, ((double) start - (double) changeStart) / (double) (changeEnd - changeStart)); };

        // Positive control: the same tone with a step of 1% of its amplitude at a zero crossing (where it
        // adds to the tone's own steepest step), and a 30 ms linear fade to silence starting on a peak
        // (the worst case for a fade's corner).
        {
            const auto peak = changeStart + (size_t) (0.25 * fs / f);
            auto stepped = tone, faded = tone;
            for (size_t n = changeStart; n < stepped.size(); ++n)
                stepped[n] += (float) (0.01 * amplitude);
            for (size_t n = peak; n < faded.size(); ++n)
                faded[n] *= (float) std::max (0.0, 1.0 - (double) (n - peak) / (0.03 * fs));
            const Stereo a { stepped, stepped }, b { faded, faded };
            const auto stepRatioControl = stepRatio (a, changeStart - 480, changeStart + 960, f);
            const auto stepHighControl = highFrequencyDb (a, changeStart - 480, changeStart + 960, amplitude);
            const auto fadeHighControl = highFrequencyDb (b, changeStart - 480, changeStart + (size_t) (0.05 * fs), amplitude);
            expectGreaterThan (stepRatioControl, 1.5);
            expectGreaterThan (stepHighControl, maxHighFrequencyDb + 10.0);
            logMessage ("  -> positive control: a 1% step in the tone gives step ratio " + juce::String (stepRatioControl, 3) + " and energy above 3 kHz "
                        + juce::String (stepHighControl, 1) + " dB; a 30 ms linear fade to silence " + juce::String (fadeHighControl, 1) + " dB");
        }

        struct Sweep
        {
            juce::String name;
            Engine engine;
            std::function<void (Reverb::Settings&, double)> apply;
        };
        const std::vector<Sweep> sweeps {
            { "Hall size 0.3 -> 0.9", Engine::hall, [] (Reverb::Settings& s, double t) { s.size = (float) (0.3 + 0.6 * t); } },
            { "Room size 0.9 -> 0.2", Engine::room, [] (Reverb::Settings& s, double t) { s.size = (float) (0.9 - 0.7 * t); } },
            { "Hall decay 0.5 -> 10 s", Engine::hall, [] (Reverb::Settings& s, double t) { s.decaySeconds = (float) (0.5 * std::pow (20.0, t)); } },
            { "Plate decay 10 -> 0.5 s", Engine::plate, [] (Reverb::Settings& s, double t) { s.decaySeconds = (float) (10.0 * std::pow (0.05, t)); } },
            { "Room high multiplier 0.2 -> 2", Engine::room, [] (Reverb::Settings& s, double t) { s.highDecayMultiplier = (float) (0.2 * std::pow (10.0, t)); } },
            { "Hall mix 0 -> 1", Engine::hall, [] (Reverb::Settings& s, double t) { s.mix = (float) t; } },
            { "Plate mix 1 -> 0", Engine::plate, [] (Reverb::Settings& s, double t) { s.mix = (float) (1.0 - t); } },
            { "Hall width 1 -> 0", Engine::hall, [] (Reverb::Settings& s, double t) { s.width = (float) (1.0 - t); } },
            { "Hall diffusion 0 -> 1", Engine::hall, [] (Reverb::Settings& s, double t) { s.diffusion = (float) t; } },
            { "Plate modulation 0 -> 1", Engine::plate, [] (Reverb::Settings& s, double t) { s.modDepth = (float) t; } },
            { "Hall pre-delay 0 -> 200 ms", Engine::hall, [] (Reverb::Settings& s, double t) { s.preDelayMs = (float) (200.0 * t); } },
            { "Room low cut 20 Hz -> 1 kHz", Engine::room, [] (Reverb::Settings& s, double t) { s.lowCutHz = (float) (20.0 * std::pow (50.0, t)); } },
        };

        for (const auto& sw : sweeps)
        {
            auto s = wetOnly (sw.engine);
            s.earlyLate = 0.5f;
            sw.apply (s, 0.0);
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);
            const auto out = run (r, tone, [&] (size_t start)
            {
                auto now = s;
                sw.apply (now, sweep (start));
                r.setSettings (now);
            });
            const auto steady = stepRatio (out, before, changeStart, f);
            const auto during = stepRatio (out, changeStart, after, f);
            const auto steadyHigh = highFrequencyDb (out, before, changeStart, amplitude);
            const auto duringHigh = highFrequencyDb (out, changeStart, after, amplitude);
            expectLessThan (during, maxStepRatio, sw.name);
            expectLessThan (duringHigh, maxHighFrequencyDb, sw.name);
            logMessage ("  -> " + sw.name + " over 1 s: largest step / a clean tone's " + juce::String (during, 3) + " (" + juce::String (steady, 3)
                        + " before); energy above 3 kHz " + juce::String (duringHigh, 1) + " dB (" + juce::String (steadyHigh, 1) + " dB before)");
        }

        // Engine switches, each a 30 ms equal-power crossfade while the tone plays.
        auto s = wetOnly (Engine::room);
        s.earlyLate = 0.5f;
        Reverb r;
        r.setSettings (s);
        r.prepare (fs, blockSize);
        const Engine order[] = { Engine::hall, Engine::plate, Engine::room, Engine::plate };
        const auto spacing = (size_t) (0.6 * fs);
        std::vector<size_t> switchAt;
        const auto out = run (r, tone, [&] (size_t start)
        {
            if (start < changeStart)
                return;
            const auto k = (start - changeStart) / spacing;
            if (k < 4 && (start - changeStart) % spacing < (size_t) blockSize)
            {
                auto now = s;
                now.engine = order[k];
                r.setSettings (now);
                switchAt.push_back (start);
            }
        });
        double worst = 0.0, worstHigh = -400.0;
        for (auto at : switchAt)
        {
            worst = std::max (worst, stepRatio (out, at, at + (size_t) (0.1 * fs), f));
            worstHigh = std::max (worstHigh, highFrequencyDb (out, at, at + (size_t) (0.1 * fs), amplitude));
        }
        const auto steady = stepRatio (out, before, changeStart, f);
        const auto steadyHigh = highFrequencyDb (out, before, changeStart, amplitude);
        expectLessThan (worst, maxStepRatio);
        expectLessThan (worstHigh, maxHighFrequencyDb);
        expectEquals ((int) switchAt.size(), 4);
        // Once its fade is done, an engine that isn't selected stops running (and costs nothing).
        expect (r.isEngineRunning (Engine::plate) && ! r.isEngineRunning (Engine::room) && ! r.isEngineRunning (Engine::hall));
        logMessage ("  -> engine switches Room -> Hall -> Plate -> Room -> Plate under the tone: largest step / a clean tone's " + juce::String (worst, 3)
                    + " in the 100 ms after each switch (" + juce::String (steady, 3) + " before); energy above 3 kHz " + juce::String (worstHigh, 1)
                    + " dB (" + juce::String (steadyHigh, 1) + " dB before)");
        logMessage ("  -> limits: step ratio " + juce::String (maxStepRatio, 1) + ", energy above 3 kHz " + juce::String (maxHighFrequencyDb, 0)
                    + " dB re the tone; a Size glide legitimately raises the step ratio, because the gliding lines Doppler-shift the tail on every trip");
    }

    // ------------------------------------------------------------------------------------------
    void ducking()
    {
        beginTest ("ducking lowers the wet while playing and lets it bloom in the gaps");

        auto s = wetOnly (Engine::hall);
        s.earlyLate = 0.5f;
        Reverb plain, ducked;
        plain.setSettings (s);
        s.ducking = 1.0f;
        ducked.setSettings (s);
        plain.prepare (fs, blockSize);
        ducked.prepare (fs, blockSize);
        const auto input = concat (whiteNoise ((int) (1.0 * fs), 0.1f, 9), std::vector<float> ((size_t) (2.0 * fs), 0.0f));
        const auto a = run (plain, input), b = run (ducked, input);
        const auto during = energyDb (b, (size_t) (0.5 * fs), (size_t) fs) - energyDb (a, (size_t) (0.5 * fs), (size_t) fs);
        const auto gap = energyDb (b, (size_t) (2.2 * fs), (size_t) (2.7 * fs)) - energyDb (a, (size_t) (2.2 * fs), (size_t) (2.7 * fs));
        expectWithinAbsoluteError (during, -Reverb::duckRangeDb, 0.5);
        expectWithinAbsoluteError (gap, 0.0, 0.5);
        logMessage ("  -> ducking 100%, noise peaking at -20 dBFS: the wet is " + juce::String (during, 2) + " dB while playing (full range -18 dB), "
                    + juce::String (gap, 2) + " dB 1.2 to 1.7 s after the input stops; off by default (bit-identical wet when 0)");
    }

    // ------------------------------------------------------------------------------------------
    void realtime()
    {
        beginTest ("real time: every setting moving, freeze, bypass, idle and wake, engine switches: nothing allocated, freed, or locked");

        Reverb r;
        Reverb::Settings s;
        r.setSettings (s);
        r.prepare (fs, blockSize);

        const auto input = concat (guitarDI ((int) (6.0 * fs)), concat (std::vector<float> ((size_t) (4.0 * fs), 0.0f), guitarDI ((int) (2.0 * fs))));
        juce::AudioBuffer<float> buffer (2, blockSize);
        rtcheck::Counts total;
        int blocks = 0;
        bool sawIdle = false, sawFrozen = false;
        juce::Random random (17);

        for (size_t start = 0; start + blockSize <= input.size(); start += blockSize, ++blocks)
        {
            buffer.copyFrom (0, 0, input.data() + start, blockSize);
            buffer.copyFrom (1, 0, input.data() + start, blockSize);

            // Settings to apply this block (built outside the measurement; applied inside it).
            auto next = s;
            bool bypass = false;
            if (blocks % 10 == 0)
            {
                next.mix = random.nextFloat();
                next.preDelayMs = 500.0f * random.nextFloat();
                next.size = random.nextFloat();
                next.decaySeconds = 0.2f + 8.0f * random.nextFloat();
                next.lowDecayMultiplier = 0.25f + 3.0f * random.nextFloat();
                next.highDecayMultiplier = 0.1f + 1.5f * random.nextFloat();
                next.diffusion = random.nextFloat();
                next.modDepth = random.nextFloat();
                next.modRateHz = 0.05f + 4.0f * random.nextFloat();
                next.width = random.nextFloat();
                next.earlyLate = random.nextFloat();
                next.lowCutHz = 20.0f + 500.0f * random.nextFloat();
                next.highCutHz = 2000.0f + 18000.0f * random.nextFloat();
                next.ducking = random.nextFloat();
            }
            next.freeze = (blocks / 120) % 3 == 1;
            // Blocks 2300..3600 fall in the 4 s of silence: bypassed with a short decay and no engine
            // switches (each one restarts the wait for silence), so the block goes idle, then wakes
            // when it's switched back on.
            if (blocks >= 2300 && blocks < 3600)
            {
                next.decaySeconds = 0.3f;
                next.freeze = false;
                bypass = blocks >= 2320 && blocks < 3500;
            }
            else if (blocks % 75 == 0)
            {
                next.engine = (Engine) ((blocks / 75) % 3);
            }
            else
            {
                bypass = (blocks / 50) % 4 == 3;
            }
            s = next;

            rtcheck::begin();
            r.setSettings (s);
            r.setBypassed (bypass);
            r.process (juce::dsp::AudioBlock<float> (buffer), {});
            total += rtcheck::end();

            sawIdle = sawIdle || r.isIdle();
            sawFrozen = sawFrozen || s.freeze;
        }

        expect (sawIdle, "the run must include the idle state");
        expect (sawFrozen);
        expectEquals (total.allocations, 0L);
        expectEquals (total.frees, 0L);
        expectEquals (total.blockingLocks, 0L);
        logMessage ("  -> " + juce::String (blocks) + " blocks (" + juce::String (blocks * blockSize / fs, 1) + " s): every setting re-randomized every 10 blocks, "
                    "an engine switch every 75, freeze on a third of the time, bypass toggling, and an idle sleep and wake: " + describe (total));
    }

    // ------------------------------------------------------------------------------------------
    void cpu()
    {
        beginTest ("CPU per 128-sample stereo block, each engine (target: well under 5% of the 2.67 ms deadline)");
        const juce::ScopedNoDenormals noDenormals;
        const auto input = guitarDI ((int) (10.0 * fs));

        for (const auto engine : allEngines)
        {
            Reverb::Settings s;
            s.engine = engine;
            s.mix = 0.3f;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            std::vector<double> times;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                buffer.copyFrom (1, 0, input.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                r.process (juce::dsp::AudioBlock<float> (buffer), {});
                const auto t1 = std::chrono::steady_clock::now();
                times.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
            }
            const auto mean = std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
            const auto p99 = percentile (times, 0.99);
            expectLessThan (100.0 * mean / deadlineMicros, 5.0, nameOf (engine));
            logMessage ("  -> " + nameOf (engine) + " (defaults, mix 0.3, guitar DI, 10 s): mean " + micros (mean) + " = "
                        + juce::String (100.0 * mean / deadlineMicros, 2) + "% of the deadline, p99 " + micros (p99) + ", worst "
                        + micros (*std::max_element (times.begin(), times.end())));
        }
    }

    // ------------------------------------------------------------------------------------------
    void renders()
    {
        beginTest ("renders for listening: the guitar DI through each engine, each impulse response, and a freeze");

        const auto save = [this] (const juce::String& name, const Stereo& s)
        {
            juce::AudioBuffer<float> buffer (2, (int) s.left.size());
            buffer.copyFrom (0, 0, s.left.data(), (int) s.left.size());
            buffer.copyFrom (1, 0, s.right.data(), (int) s.right.size());
            const auto file = proofDir().getChildFile (name);
            expect (writeWav (file, buffer));
            return name;
        };

        juce::StringArray files;
        const auto di = concat (guitarDI ((int) (4.0 * fs)), std::vector<float> ((size_t) (3.0 * fs), 0.0f));
        for (const auto engine : allEngines)
        {
            Reverb::Settings s; // the defaults, a little wetter
            s.engine = engine;
            s.mix = 0.35f;
            Reverb r;
            r.setSettings (s);
            r.prepare (fs, blockSize);
            files.add (save ("reverb_" + nameOf (engine).toLowerCase() + "_guitar.wav", run (r, di)));

            auto w = wetOnly (engine);
            w.earlyLate = 0.5f;
            Reverb ir;
            ir.setSettings (w);
            ir.prepare (fs, blockSize);
            files.add (save ("reverb_" + nameOf (engine).toLowerCase() + "_impulse.wav", run (ir, impulse ((size_t) (4.0 * fs)))));
        }

        // Freeze: a riff into the Hall, frozen at 1.5 s, the riff keeps playing dry over the held pad
        // until 5.5 s, then the freeze is released and the pad decays.
        Reverb::Settings s;
        s.mix = 0.45f;
        Reverb frozen;
        frozen.setSettings (s);
        frozen.prepare (fs, blockSize);
        const auto riff = concat (guitarDI ((int) (5.5 * fs)), std::vector<float> ((size_t) (3.0 * fs), 0.0f));
        files.add (save ("reverb_hall_freeze.wav", run (frozen, riff, [&] (size_t start)
        {
            auto now = s;
            now.freeze = start >= (size_t) (1.5 * fs) && start < (size_t) (5.5 * fs);
            frozen.setSettings (now);
        })));

        logMessage ("  -> " + proofDir().getFullPathName() + ": " + files.joinIntoString (", ")
                    + " (unverified: Sean listens for metallic ringing, flutter, the stereo image, and the freeze)");
    }
};

ReverbTests reverbTests;
} // namespace

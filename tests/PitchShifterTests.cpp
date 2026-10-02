#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/PitchShifter.h"
#include "dsp/Psola.h"

#include <chrono>
#include <complex>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::GranularShifter;
using ampsim::GranularVoice;
using ampsim::PitchShifterInput;

constexpr double twoPi = juce::MathConstants<double>::twoPi;

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/pitch_shifter").getChildFile (name);
}

std::vector<float> channel (const juce::AudioBuffer<float>& b, int ch)
{
    return { b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples() };
}

double semitonesToRatio (double semitones) { return std::pow (2.0, semitones / 12.0); }
double cents (double f, double reference) { return 1200.0 * std::log2 (f / reference); }
juce::String str (double v, int places = 2) { return juce::String (v, places); }

/// Upward zero crossings of y in [start, end), located by linear interpolation (very accurate on a sine,
/// whose curvature vanishes at the crossing).
std::vector<double> crossings (const std::vector<float>& y, size_t start, size_t end)
{
    std::vector<double> c;
    for (size_t n = std::max<size_t> (start, 1); n < std::min (end, y.size()); ++n)
        if (y[n - 1] < 0.0f && y[n] >= 0.0f)
            c.push_back ((double) (n - 1) - (double) y[n - 1] / ((double) y[n] - (double) y[n - 1]));
    return c;
}

/// The long-run frequency between the first and last upward crossing in [start, end): whole cycles over
/// the time they took. Phase slips at splices would show up here as an offset.
double meanFrequency (const std::vector<float>& y, size_t start, size_t end)
{
    const auto c = crossings (y, start, end);
    if (c.size() < 3)
        return 0.0;
    return fs * (double) (c.size() - 1) / (c.back() - c.front());
}

/// The largest deviation of any single cycle's frequency from f0, in cents.
double worstCycleCents (const std::vector<float>& y, double f0, size_t start, size_t end)
{
    const auto c = crossings (y, start, end);
    double worst = 0.0;
    for (size_t i = 1; i < c.size(); ++i)
        worst = std::max (worst, std::abs (cents (fs / (c[i] - c[i - 1]), f0)));
    return worst;
}

/// The amplitude of each half cycle (largest |y| between crossings of zero), as an envelope.
std::vector<double> cycleAmplitudes (const std::vector<float>& y, size_t start, size_t end)
{
    std::vector<double> amps;
    double peak = 0.0;
    for (size_t n = std::max<size_t> (start, 1); n < std::min (end, y.size()); ++n)
    {
        peak = std::max (peak, (double) std::abs (y[n]));
        if ((y[n - 1] < 0.0f) != (y[n] < 0.0f))
        {
            amps.push_back (peak);
            peak = 0.0;
        }
    }
    if (amps.size() > 2)
        amps = { amps.begin() + 1, amps.end() - 1 }; // drop the partial half cycles at the ends
    return amps;
}

double rippleDb (const std::vector<double>& amps)
{
    if (amps.empty())
        return 0.0;
    const auto [lo, hi] = std::minmax_element (amps.begin(), amps.end());
    return *lo > 0.0 ? 20.0 * std::log10 (*hi / *lo) : 400.0;
}

std::vector<float> sineWave (double f, double amplitude, size_t n, double phase = 0.0)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = (float) (amplitude * std::sin (twoPi * f * (double) i / fs + phase));
    return x;
}

/// A steady chord of harmonic tones (k-th partial at 1/k^1.2, random phases), normalized to peak 0.5.
std::vector<float> chord (const std::vector<double>& roots, size_t n, int harmonics = 8, juce::int64 seed = 3)
{
    juce::Random random (seed);
    std::vector<double> x (n, 0.0);
    for (auto f0 : roots)
        for (int k = 1; k <= harmonics; ++k)
        {
            const auto phase = random.nextDouble() * twoPi;
            const auto a = 1.0 / std::pow ((double) k, 1.2);
            for (size_t i = 0; i < n; ++i)
                x[i] += a * std::sin (twoPi * f0 * k * (double) i / fs + phase);
        }
    double peak = 1.0e-9;
    for (auto v : x)
        peak = std::max (peak, std::abs (v));
    std::vector<float> out (n);
    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (0.5 * x[i] / peak);
    return out;
}

/// Magnitude spectrum of x[start, start + 2^order) with a Hann window.
std::vector<double> magnitudeSpectrum (const std::vector<float>& x, size_t start, int order)
{
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> frame ((size_t) (2 * size), 0.0f);
    for (int i = 0; i < size; ++i)
        frame[(size_t) i] = x[start + (size_t) i] * (float) (0.5 - 0.5 * std::cos (twoPi * i / size));
    fft.performFrequencyOnlyForwardTransform (frame.data(), true);
    return { frame.begin(), frame.begin() + size / 2 + 1 };
}

/// The frequency of the largest spectral peak within +-searchCents of f, refined by a parabola through
/// the log magnitudes (a Hann window's peak is close to a parabola in dB).
double peakFrequency (const std::vector<double>& mag, int order, double f, double searchCents = 40.0)
{
    const auto binHz = fs / (double) (1 << order);
    const auto lo = std::max<size_t> (1, (size_t) std::floor (f * std::pow (2.0, -searchCents / 1200.0) / binHz));
    const auto hi = std::min (mag.size() - 2, (size_t) std::ceil (f * std::pow (2.0, searchCents / 1200.0) / binHz));
    auto k = lo;
    for (auto i = lo; i <= hi; ++i)
        if (mag[i] > mag[k])
            k = i;
    const auto a = std::log (mag[k - 1] + 1.0e-30), b = std::log (mag[k] + 1.0e-30), c = std::log (mag[k + 1] + 1.0e-30);
    const auto denominator = a - 2.0 * b + c;
    const auto p = denominator < 0.0 ? 0.5 * (a - c) / denominator : 0.0;
    return ((double) k + p) * binHz;
}

/// Runs a GranularShifter over x, applying `before` ahead of each 128-sample buffer; records each splice's
/// starting sample in `spliceStarts` if given.
std::vector<float> runShifter (GranularShifter& s, const std::vector<float>& x, const std::function<void (size_t)>& before = {},
                               std::vector<size_t>* spliceStarts = nullptr)
{
    std::vector<float> y (x.size());
    int seen = s.getVoice().getSpliceCount();
    for (size_t start = 0; start < x.size(); start += (size_t) blockSize)
    {
        if (before)
            before (start);
        const auto end = std::min (x.size(), start + (size_t) blockSize);
        for (auto n = start; n < end; ++n)
        {
            y[n] = s.processSample (x[n]);
            if (spliceStarts != nullptr && s.getVoice().getSpliceCount() != seen)
            {
                seen = s.getVoice().getSpliceCount();
                spliceStarts->push_back (n);
            }
        }
    }
    return y;
}

GranularVoice::Settings granular (double ratio, double delayMs = 0.0, double glideMs = 30.0)
{
    GranularVoice::Settings s;
    s.ratio = ratio;
    s.delayMs = delayMs;
    s.glideMs = glideMs;
    return s;
}

class PitchShifterTests final : public juce::UnitTest
{
public:
    PitchShifterTests() : juce::UnitTest ("Pitch shifter", "ampsim") {}

    void runTest() override
    {
        granularGolden();
        granularPitch();
        granularRipple();
        granularChords();
        granularUnityAndTrail();
        granularGlides();
        granularSmear();
        granularRealtimeAndCpu();
        psolaGolden();
        psolaPitch();
        psolaTracking();
        psolaTrail();
        psolaCpu();
    }

private:
    // ---- PSOLA ----------------------------------------------------------------------------------

    /// One PSOLA voice with its own input and analysis, sample by sample. `period` (if given) feeds the
    /// analysis a fixed period track instead of its detector: (block start, period) pairs.
    struct PsolaRun
    {
        std::vector<float> y;
        std::vector<double> confidence, delays;
        int notes = 0, grains = 0;
    };

    static PsolaRun runPsola (const std::vector<float>& x, const std::vector<float>& di, ampsim::PsolaVoice::Settings settings,
                              const std::vector<std::pair<size_t, double>>& periodTrack = {},
                              const std::vector<std::pair<size_t, ampsim::PsolaVoice::Settings>>& changes = {}, double maxDelayMs = 40.0,
                              double minFrequency = ampsim::PsolaAnalysis::defaultMinFrequency)
    {
        ampsim::PitchShifterInput input;
        input.prepare (fs, maxDelayMs, GranularVoice::searchMarginMs);
        ampsim::PsolaAnalysis analysis;
        analysis.prepare (fs, minFrequency);
        ampsim::PsolaVoice voice;
        voice.setSettings (settings);
        voice.prepare (fs);

        PsolaRun r;
        r.y.resize (x.size());
        r.confidence.resize (x.size());
        r.delays.resize (x.size());
        for (size_t n = 0; n < x.size(); ++n)
        {
            if (n % (size_t) blockSize == 0)
            {
                for (const auto& [at, p] : periodTrack)
                    if (at == n)
                        analysis.setPeriodOverride (p);
                for (const auto& [at, s] : changes)
                    if (at == n)
                        voice.setSettings (s);
            }
            r.y[n] = voice.process (analysis, input);
            input.push (x[n]);
            analysis.process (di[n], input);
            r.confidence[n] = analysis.getConfidence();
            r.delays[n] = voice.getLastGrainDelay();
        }
        r.notes = analysis.getNoteCount();
        r.grains = voice.getGrainCount();
        return r;
    }

    static ampsim::PsolaVoice::Settings psola (double ratio, double delayMs = 0.0, double glideMs = 30.0)
    {
        ampsim::PsolaVoice::Settings s;
        s.ratio = ratio;
        s.delayMs = delayMs;
        s.glideMs = glideMs;
        return s;
    }

    void psolaGolden()
    {
        beginTest ("PSOLA: matches prototypes/pitch_shifter.py sample by sample on a fixed period track (golden renders, limit -100 dB)");

        const auto spec = juce::JSON::parse (fixture ("cases.json"))["psola"];
        const auto input = channel (readWav (fixture ("input_notes.wav")), 0);
        std::vector<std::pair<size_t, double>> track;
        for (const auto& t : *spec["track"].getArray())
            track.push_back ({ (size_t) (int) t[0], (double) t[1] });
        juce::StringArray results;

        for (const auto* name : { "down_octave", "up_fifth", "up_two_octaves", "glide" })
        {
            const auto c = spec["cases"][name];
            const auto expected = channel (readWav (fixture ("expected_psola_" + juce::String (name) + ".wav")), 0);
            std::vector<std::pair<size_t, ampsim::PsolaVoice::Settings>> changes;
            for (const auto& ch : *c["changes"].getArray())
                changes.push_back ({ (size_t) (int) ch[0], psola ((double) ch[1], (double) ch[2], (double) ch[3]) });
            const auto r = runPsola (input, input, psola ((double) c["ratio"], (double) c["delay_ms"], (double) c["glide_ms"]), track, changes,
                                     (double) spec["max_delay_ms"]);
            const auto error = relativeErrorDb (r.y, expected);
            expectLessThan (error, -100.0);
            if (error >= -100.0)
                writeWav (proofDir().getChildFile ("pitch_shifter_golden_psola_" + juce::String (name) + "_cpp.wav"), r.y);
            results.add (juce::String (name) + " " + dB (error) + " (" + juce::String (r.notes) + " tracks, " + juce::String (r.grains) + " grains)");
        }
        logMessage ("  -> C++ vs prototypes/pitch_shifter.py (tests/fixtures/pitch_shifter/input_notes.wav: G3, D3, B3, A2, 1 s, a fixed period track "
                    "standing in for the detector): " + results.joinIntoString ("; "));
    }

    /// A harmonic tone: partial k at 1/k, random phases, normalized to peak 0.5.
    static std::vector<float> harmonicTone (double f0, size_t n, int harmonics = 10, juce::int64 seed = 9)
    {
        return chord ({ f0 }, n, harmonics, seed);
    }

    /// Energy off the harmonics of f (outside +-3 Hz of each multiple), in dB relative to the total.
    static double offHarmonicDb (const std::vector<double>& mag, int order, double f)
    {
        const auto binHz = fs / (double) (1 << order);
        double on = 0.0, total = 0.0;
        for (size_t k = 1; k < mag.size(); ++k)
        {
            const auto freq = (double) k * binHz;
            const auto e = mag[k] * mag[k];
            total += e;
            const auto h = std::round (freq / f);
            if (h >= 1.0 && std::abs (freq - h * f) <= 3.0)
                on += e;
        }
        return 10.0 * std::log10 (std::max (1.0e-30, total - on) / total);
    }

    void psolaPitch()
    {
        beginTest ("PSOLA: a shifted tone comes out within 1 cent of the target, with the real detector on the DI");

        // Harmonic tones (10 partials at 1/k) on the A, G, and high E strings, and sines. PSOLA keeps the
        // input's spectral envelope, so a sine shifted up keeps its energy near the original frequency: it
        // comes out 6 dB down an octave up and 14 dB down two octaves up (the grain is shorter than the
        // sine's period there), and for r < 1 its strongest component stays at f with the new fundamental r f
        // 6 dB below it. Both still repeat at exactly r f; the peak at r f is what's measured.
        constexpr int order = 16;
        const auto n = (size_t) (3.0 * fs);
        const std::vector<std::pair<juce::String, double>> shifts { { "-24", -24.0 }, { "-12", -12.0 }, { "-5", -5.0 }, { "-0.1", -0.1 },
                                                                    { "-7 cents", -0.07 }, { "+7 cents", 0.07 }, { "+0.1", 0.1 },
                                                                    { "+7", 7.0 }, { "+12", 12.0 }, { "+24", 24.0 } };
        juce::StringArray harmonicRows, sineRows, distortionRows, levelRows;
        double worstHarmonic = 0.0, worstSine = 0.0;
        for (const auto& [name, semitones] : shifts)
        {
            const auto r = semitonesToRatio (semitones);
            double rowHarmonic = 0.0, rowSine = 0.0, rowDistortion = -400.0, rowLevelLo = 100.0, rowLevelHi = -100.0;
            for (const auto f0 : { 110.0, 196.0, 329.63 })
            {
                for (const auto isSine : { false, true })
                {
                    const auto x = isSine ? sineWave (f0, 0.5, n) : harmonicTone (f0, n);
                    const auto run = runPsola (x, x, psola (r));
                    const auto mag = magnitudeSpectrum (run.y, (size_t) (1.2 * fs), order);
                    const auto error = cents (peakFrequency (mag, order, f0 * r, 20.0), f0 * r);
                    expectLessThan (std::abs (error), 1.0);
                    if (isSine)
                    {
                        rowSine = std::max (rowSine, std::abs (error));
                    }
                    else
                    {
                        rowHarmonic = std::max (rowHarmonic, std::abs (error));
                        rowDistortion = std::max (rowDistortion, offHarmonicDb (mag, order, f0 * r));
                        const auto level = toDb (rms (run.y.data() + (size_t) (1.0 * fs), (size_t) (1.5 * fs)) / rms (x.data() + (size_t) (1.0 * fs), (size_t) (1.5 * fs)));
                        rowLevelLo = std::min (rowLevelLo, level);
                        rowLevelHi = std::max (rowLevelHi, level);
                    }
                }
            }
            worstHarmonic = std::max (worstHarmonic, rowHarmonic);
            worstSine = std::max (worstSine, rowSine);
            harmonicRows.add (name + " " + str (rowHarmonic, 3));
            sineRows.add (name + " " + str (rowSine, 3));
            distortionRows.add (name + " " + str (rowDistortion, 1));
            levelRows.add (name + " " + str (rowLevelLo, 1) + ".." + str (rowLevelHi, 1));
            // Down and near unison the level compensation holds the level; up, PSOLA keeps the input's spectral
            // envelope, so a tone whose partials fall as 1/k is quieter where its new harmonics land on the
            // falling part (1.6 dB at +12, 5.8 dB at +24 for these tones).
            expectGreaterThan (rowLevelLo, semitones > 1.0 ? -7.0 : -1.0);
            expectLessThan (rowLevelHi, 1.0);
        }
        logMessage ("  -> harmonic tones on 110, 196, 330 Hz, worst error per shift (cents, limit 1): " + harmonicRows.joinIntoString (", "));
        logMessage ("  -> sines, worst error per shift (cents, limit 1): " + sineRows.joinIntoString (", "));
        logMessage ("  -> distortion: energy off the harmonics of the shifted pitch, worst of the three tones (dB): " + distortionRows.joinIntoString (", "));
        logMessage ("  -> level of the harmonic tones against the input (dB; limit +-1 down to unison, -7 up, where PSOLA keeps the input's falling "
                    "spectral envelope): " + levelRows.joinIntoString (", "));
        logMessage ("  -> worst " + str (worstHarmonic, 3) + " cents (harmonic tones), " + str (worstSine, 3) + " cents (sines)");
    }

    /// Pitch every 10 ms with the McLeod detector (tuner preset): {time of the frame's centre, Hz, clarity}.
    struct PitchPoint
    {
        double seconds, hz, clarity;
    };

    static std::vector<PitchPoint> pitchTrack (const std::vector<float>& x)
    {
        ampsim::PitchDetector d;
        d.prepare (fs, ampsim::PitchDetector::Settings::tuner());
        std::vector<PitchPoint> points;
        const auto every = (size_t) (0.010 * fs);
        const auto centreLag = 0.5 * d.getFrameSeconds();
        for (size_t start = 0; start + every <= x.size(); start += every)
        {
            d.push (x.data() + start, (int) every);
            const auto e = d.detect();
            points.push_back ({ (double) (start + every) / fs - centreLag, e.frequency, e.clarity });
        }
        return points;
    }

    void psolaTracking()
    {
        beginTest ("PSOLA: the output pitch tracks input x ratio through a sweep and on the guitar DI's single notes");

        // A harmonic tone sweeping 110 -> 440 Hz (two octaves in 3 s, log), and the reference guitar DI. The
        // output's pitch (McLeod, 10 ms steps) against the input's pitch at the same moment minus the voice's
        // trail, times the ratio. Frames the detector isn't sure of (clarity < 0.9, the input's or the
        // output's) are left out and counted.
        const auto n = (size_t) (3.5 * fs);
        std::vector<double> phase (n);
        std::vector<float> sweep (n);
        {
            double ph = 0.0;
            juce::Random random (4);
            std::vector<double> offsets (10);
            for (auto& o : offsets)
                o = random.nextDouble() * twoPi;
            for (size_t i = 0; i < n; ++i)
            {
                const auto t = juce::jlimit (0.0, 3.0, (double) i / fs - 0.25);
                const auto f = 110.0 * std::pow (4.0, t / 3.0);
                ph += twoPi * f / fs;
                double v = 0.0;
                for (int k = 1; k <= 10; ++k)
                    v += std::sin (k * ph + offsets[(size_t) k - 1]) / k;
                sweep[i] = (float) (0.3 * v);
            }
        }

        struct Case
        {
            juce::String name;
            const std::vector<float>* x;
            double semitones;
        };
        const auto di = guitarDI ((int) (4.0 * fs));
        std::vector<PitchPoint> plotIn, plotOut;
        double plotRatio = 1.0;
        juce::StringArray rows;
        for (const auto& c : { Case { "sweep 110-440 Hz, -12", &sweep, -12.0 }, Case { "sweep 110-440 Hz, +7", &sweep, 7.0 },
                               Case { "guitar DI, -12", &di, -12.0 }, Case { "guitar DI, +12", &di, 12.0 } })
        {
            const auto r = semitonesToRatio (c.semitones);
            const auto run = runPsola (*c.x, *c.x, psola (r));
            const auto in = pitchTrack (*c.x);
            const auto out = pitchTrack (run.y);
            std::vector<double> errors;
            int skipped = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                const auto t = out[i].seconds;
                const auto sample = (size_t) juce::jlimit (0.0, (double) c.x->size() - 1.0, t * fs);
                if (run.confidence[sample] < 1.0 || out[i].clarity < 0.9 || out[i].hz <= 0.0)
                {
                    ++skipped;
                    continue;
                }
                // The input's pitch at t minus the trail, and half a period more: a grain repeats at the spacing of
                // its mark from the previous one, the period over the half period before the mark on average.
                const auto j0 = (size_t) juce::jlimit (0.0, (double) in.size() - 1.0, std::floor ((t - in.front().seconds) / 0.010));
                const auto halfPeriod = in[j0].hz > 0.0 ? 0.5 / in[j0].hz : 0.0;
                const auto source = t - run.delays[sample] / fs - halfPeriod;
                const auto j = (size_t) std::floor ((source - in.front().seconds) / 0.010);
                if (j + 1 >= in.size() || in[j].clarity < 0.9 || in[j + 1].clarity < 0.9 || in[j].hz <= 0.0 || in[j + 1].hz <= 0.0)
                {
                    ++skipped;
                    continue;
                }
                const auto frac = (source - in[j].seconds) / 0.010;
                const auto expected = r * in[j].hz * std::pow (in[j + 1].hz / in[j].hz, frac);
                errors.push_back (std::abs (cents (out[i].hz, expected)));
            }
            std::sort (errors.begin(), errors.end());
            const auto median = errors.empty() ? 0.0 : errors[errors.size() / 2];
            const auto p95 = errors.empty() ? 0.0 : errors[(size_t) (0.95 * (double) errors.size())];
            expectGreaterThan ((int) errors.size(), 50);
            expectLessThan (median, 5.0);
            rows.add (c.name + ": median " + str (median, 2) + " cents, 95th percentile " + str (p95, 2) + " over " + juce::String ((int) errors.size())
                      + " frames (" + juce::String (skipped) + " left out)");
            if (c.name == "sweep 110-440 Hz, -12")
            {
                plotIn = in;
                plotOut = out;
                plotRatio = r;
            }
        }
        logMessage ("  -> output pitch against input x ratio (McLeod, 10 ms frames; the detector's own error is about 1-2 cents; limit: median under 5 cents): "
                    + rows.joinIntoString ("; "));

        PlotSeries expected { "input pitch x 1/2", {}, {}, plotColour (0), 2.0f };
        PlotSeries measured { "PSOLA output, down an octave", {}, {}, plotColour (1), 1.5f, true };
        for (const auto& p : plotIn)
            if (p.clarity >= 0.9 && p.hz > 0.0)
            {
                expected.x.push_back (p.seconds);
                expected.y.push_back (p.hz * plotRatio);
            }
        for (const auto& p : plotOut)
            if (p.clarity >= 0.9 && p.hz > 0.0)
            {
                measured.x.push_back (p.seconds);
                measured.y.push_back (p.hz);
            }
        PlotOptions o;
        o.title = "PSOLA pitch track: a 110-440 Hz sweep shifted down an octave";
        o.xLabel = "s";
        o.yLabel = "Hz";
        o.xMin = 0.0;
        o.xMax = 3.5;
        o.yMin = 40.0;
        o.yMax = 240.0;
        const auto png = proofDir().getChildFile ("pitch_shifter_psola_sweep.png");
        expect (savePlot (png, o, { expected, measured }));
        logMessage ("  -> " + png.getFullPathName());
    }

    void psolaTrail()
    {
        beginTest ("PSOLA: detection time and trail per string (a pluck after silence, the 80 Hz detector floor)");

        juce::StringArray rows;
        for (const auto& [name, f0] : { std::pair<const char*, double> { "low E", 82.41 }, { "A", 110.0 }, { "D", 146.83 }, { "G", 196.0 }, { "B", 246.94 },
                                        { "high E", 329.63 } })
        {
            // A Karplus-Strong pluck at 0.3 s after faint noise, as the tuner's latency tests use.
            const auto n = (size_t) (1.2 * fs);
            std::vector<float> x (n, 0.0f);
            juce::Random random (17);
            const auto start = (size_t) (0.3 * fs);
            const auto period = std::max (2, juce::roundToInt (fs / f0));
            std::vector<double> loop ((size_t) period);
            for (auto& v : loop)
                v = 2.0 * random.nextDouble() - 1.0;
            for (size_t i = start; i < n; ++i)
            {
                const auto k = (i - start) % (size_t) period;
                x[i] = (float) (0.4 * loop[k]);
                loop[k] = 0.996 * 0.5 * (loop[k] + loop[(k + 1) % (size_t) period]);
            }
            for (auto& v : x)
                v += 0.0005f * (2.0f * random.nextFloat() - 1.0f);

            const auto run = runPsola (x, x, psola (0.5));
            size_t detected = 0;
            for (size_t i = start; i < n; ++i)
                if (run.confidence[i] >= 0.5)
                {
                    detected = i;
                    break;
                }
            double delaySum = 0.0;
            int count = 0;
            for (size_t i = start + (size_t) (0.1 * fs); i < n; ++i)
                if (run.confidence[i] >= 1.0)
                {
                    delaySum += run.delays[i];
                    ++count;
                }
            const auto detectionMs = detected > 0 ? 1000.0 * (double) (detected - start) / fs : -1.0;
            const auto trailMs = count > 0 ? 1000.0 * delaySum / count / fs : -1.0;
            expectGreaterThan (detectionMs, 0.0);
            expectLessThan (detectionMs, 40.0);
            rows.add (juce::String (name) + " (" + str (f0, 1) + " Hz): detected " + str (detectionMs, 1) + " ms after the pluck, trail " + str (trailMs, 1)
                      + " ms (" + str (trailMs * f0 / 1000.0, 2) + " periods)");
        }
        logMessage ("  -> an octave down, a pluck after silence: " + rows.joinIntoString ("; ")
                    + ". The shifted note starts about detection + trail after the pluck; the dry is never delayed.");
    }

    void psolaCpu()
    {
        beginTest ("PSOLA: CPU for the analysis and one voice");

        const auto di = guitarDI ((int) (10.0 * fs));
        ampsim::PitchShifterInput input;
        input.prepare (fs, 40.0, GranularVoice::searchMarginMs);
        ampsim::PsolaAnalysis analysis;
        analysis.prepare (fs);
        ampsim::PsolaVoice voice;
        voice.setSettings (psola (0.5));
        voice.prepare (fs);
        std::vector<double> micros;
        micros.reserve (di.size() / (size_t) blockSize + 1);
        float sink = 0.0f;
        rtcheck::begin();
        for (size_t start = 0; start + (size_t) blockSize <= di.size(); start += (size_t) blockSize)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = start; i < start + (size_t) blockSize; ++i)
            {
                sink += voice.process (analysis, input);
                input.push (di[i]);
                analysis.process (di[i], input);
            }
            micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
        }
        const auto counts = rtcheck::end();
        juce::ignoreUnused (sink);
        expectEquals (counts.allocations, 0L);
        expectEquals (counts.frees, 0L);
        expectEquals (counts.blockingLocks, 0L);
        std::sort (micros.begin(), micros.end());
        const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
        logMessage ("  -> 10 s of guitar DI, one voice down an octave with its own input and analysis, per 128-sample buffer: mean " + str (mean, 2) + " us ("
                    + str (100.0 * mean / deadlineMicros, 2) + "% of the deadline), p99 " + str (micros[(size_t) (0.99 * (double) micros.size())], 1)
                    + " us, worst " + str (micros.back(), 1) + " us; " + juce::String (counts.allocations) + " allocations, " + juce::String (counts.frees)
                    + " frees, " + juce::String (counts.blockingLocks) + " blocking locks");
    }

    // ---- Granular -------------------------------------------------------------------------------

    void granularGolden()
    {
        beginTest ("granular: matches prototypes/pitch_shifter.py sample by sample (golden renders, limit -100 dB)");

        const auto cases = juce::JSON::parse (fixture ("cases.json"))["granular"];
        const auto input = channel (readWav (fixture ("input.wav")), 0);
        expectEquals ((int) input.size(), 48000);
        juce::StringArray results;

        for (const auto* name : { "up_fifth", "down_octave", "up_two_octaves", "detune", "glide_and_delay" })
        {
            const auto c = cases[name];
            const auto expected = channel (readWav (fixture ("expected_granular_" + juce::String (name) + ".wav")), 0);
            GranularShifter s;
            s.setSettings (granular ((double) c["ratio"], (double) c["delay_ms"], (double) c["glide_ms"]));
            s.prepare (fs, (double) c["max_voice_delay_ms"]);
            const auto* changes = c["changes"].getArray();
            std::vector<size_t> starts;
            juce::StringArray log;
            const auto y = runShifter (s, input, [&] (size_t start)
            {
                for (const auto& change : *changes)
                    if ((size_t) (int) change[0] == start)
                        s.setSettings (granular ((double) change[1], (double) change[2], (double) change[3]));
            }, &starts);
            const auto error = relativeErrorDb (y, expected);
            expectLessThan (error, -100.0);
            if (error >= -100.0) // keep the C++ render and its splices for comparison
            {
                writeWav (proofDir().getChildFile ("pitch_shifter_golden_granular_" + juce::String (name) + "_cpp.wav"), y);
                GranularShifter again;
                again.setSettings (granular ((double) c["ratio"], (double) c["delay_ms"], (double) c["glide_ms"]));
                again.prepare (fs, (double) c["max_voice_delay_ms"]);
                int seen = 0;
                for (size_t n = 0; n < input.size(); ++n)
                {
                    const auto before = again.getVoice().getDelaySamples();
                    again.processSample (input[n]);
                    if (again.getVoice().getSpliceCount() != seen)
                    {
                        seen = again.getVoice().getSpliceCount();
                        const auto sp = again.getVoice().getLastSplice();
                        log.add (juce::String ((int) n) + "," + juce::String (before, 9) + "," + juce::String (sp.jump, 9) + "," + juce::String (sp.correlation, 12));
                    }
                }
                proofDir().getChildFile ("pitch_shifter_golden_granular_" + juce::String (name) + "_splices.csv").replaceWithText (log.joinIntoString ("\n"));
            }
            results.add (juce::String (name) + " " + dB (error) + " (" + juce::String (s.getVoice().getSpliceCount()) + " splices)");
        }
        logMessage ("  -> C++ vs prototypes/pitch_shifter.py (tests/fixtures/pitch_shifter, 1 s of plucks, a power chord, noise, silence): "
                    + results.joinIntoString ("; "));
    }

    void granularPitch()
    {
        beginTest ("granular: a shifted sine comes out within 1 cent of the target (long-run frequency), at every shift");

        juce::StringArray rows;
        double worst = 0.0, worstCycle = 0.0;
        juce::String worstCase;
        const auto n = (size_t) (2.6 * fs);

        // +-0.1 semitone is +-10 cents; +-7 cents is a typical double.
        const std::vector<std::pair<juce::String, double>> shifts { { "-24", -24.0 }, { "-12", -12.0 }, { "-5", -5.0 }, { "-0.1", -0.1 },
                                                                    { "-7 cents", -0.07 }, { "+7 cents", 0.07 }, { "+0.1", 0.1 },
                                                                    { "+7", 7.0 }, { "+12", 12.0 }, { "+24", 24.0 } };
        for (const auto& [name, semitones] : shifts)
        {
            const auto ratio = semitonesToRatio (semitones);
            double rowWorst = 0.0;
            for (const auto f : { 110.0, 196.0, 440.0 })
            {
                GranularShifter s;
                s.setSettings (granular (ratio));
                s.prepare (fs);
                const auto y = runShifter (s, sineWave (f, 0.5, n));
                const auto target = f * ratio;
                const auto error = cents (meanFrequency (y, (size_t) (0.3 * fs), n), target);
                const auto cycle = worstCycleCents (y, target, (size_t) (0.3 * fs), n);
                expectLessThan (std::abs (error), 1.0);
                rowWorst = std::max (rowWorst, std::abs (error));
                worstCycle = std::max (worstCycle, cycle);
                if (std::abs (error) >= worst)
                {
                    worst = std::abs (error);
                    worstCase = name + " on " + str (f, 0) + " Hz";
                }
            }
            rows.add (name + " " + str (rowWorst, 4));
        }
        logMessage ("  -> worst long-run error per shift over 110, 196, 440 Hz sines (cents, limit 1): " + rows.joinIntoString (", "));
        logMessage ("  -> worst overall " + str (worst, 4) + " cents (" + worstCase + "); the worst single cycle anywhere is "
                    + str (worstCycle, 2) + " cents off (a splice's crossfade nudges one cycle; the long run stays exact)");
    }

    /// The output's power around each splice, averaged over all splices (event-locked), in dB relative to the
    /// average away from splices. With enough splices the input's own randomness averages out and what is
    /// left is the crossfade's systematic dip or bump.
    struct LockedEnvelope
    {
        std::vector<double> db; // from `pre` samples before a splice to `post` after
        int splices = 0;
    };

    static LockedEnvelope lockedPower (const std::vector<float>& y, const std::vector<size_t>& starts, size_t from, int pre, int post, int smooth)
    {
        LockedEnvelope e;
        std::vector<double> sum ((size_t) (pre + post), 0.0);
        for (auto s : starts)
        {
            if (s < from + (size_t) (pre + smooth) || s + (size_t) (post + smooth) >= y.size())
                continue;
            for (int i = -pre; i < post; ++i)
            {
                double p = 0.0; // power over `smooth` samples centred on s + i
                for (int k = -smooth / 2; k < smooth / 2; ++k)
                    p += (double) y[s + (size_t) (i + k)] * (double) y[s + (size_t) (i + k)];
                sum[(size_t) (i + pre)] += p / smooth;
            }
            ++e.splices;
        }
        // Reference: the average over the stretch before the fades start (the first `pre` samples).
        double reference = 0.0;
        for (int i = 0; i < pre / 2; ++i)
            reference += sum[(size_t) i];
        reference /= (double) (pre / 2);
        for (auto v : sum)
            e.db.push_back (10.0 * std::log10 (std::max (v, 1.0e-30) / std::max (reference, 1.0e-30)));
        return e;
    }

    void granularRipple()
    {
        beginTest ("granular: grain ripple (the level wobble at splices) on sines and on noise");

        // Limits: amplitude modulation of a tone is just detectable at about 2% depth (0.35 dB peak to peak) at
        // slow rates (Zwicker 1952), and of noise at about 5% (0.9 dB; Viemeister 1979). Splices are brief
        // single events, not a steady modulation, so holding sines to 0.5 dB and noise to 0.5 dB keeps every
        // splice at or below the point where a steady wobble would start to be heard.
        const auto n = (size_t) (3.0 * fs);
        juce::StringArray sineRows;
        double worstSine = 0.0;
        juce::String worstSineCase;
        std::vector<double> plotEnvelope;
        for (const auto semitones : { -24.0, -12.0, -5.0, 7.0, 12.0, 24.0 })
        {
            double rowWorst = 0.0, rowSum = 0.0;
            int count = 0;
            for (const auto f : { 98.0, 147.0, 220.0, 330.0, 494.0, 740.0, 1109.0 })
            {
                GranularShifter s;
                s.setSettings (granular (semitonesToRatio (semitones)));
                s.prepare (fs);
                const auto y = runShifter (s, sineWave (f, 0.5, n));
                const auto amps = cycleAmplitudes (y, (size_t) (0.3 * fs), n);
                const auto ripple = rippleDb (amps);
                rowWorst = std::max (rowWorst, ripple);
                rowSum += ripple;
                ++count;
                if (ripple >= worstSine)
                {
                    worstSine = ripple;
                    worstSineCase = str (semitones, 0) + " st on " + str (f, 0) + " Hz";
                }
                if (juce::exactlyEqual (semitones, 12.0) && juce::exactlyEqual (f, 220.0))
                    plotEnvelope = amps;
            }
            expectLessThan (rowWorst, 0.5);
            sineRows.add (str (semitones, 0) + " st: mean " + str (rowSum / count, 3) + ", worst " + str (rowWorst, 3));
        }
        logMessage ("  -> sines (98 Hz to 1.1 kHz, 7 each), peak-to-peak level wobble over 2.7 s in dB (limit 0.5): " + sineRows.joinIntoString ("; "));
        logMessage ("  -> worst " + str (worstSine, 3) + " dB (" + worstSineCase + ")");

        // Noise: white noise low-passed at 6 kHz (a post-cab guitar's band; full-band white noise also measures the
        // Hermite read's own high-frequency loss, which changes with the fractional delay, up to 0.9 dB at 15 kHz+).
        juce::StringArray noiseRows;
        double worstNoise = 0.0;
        LockedEnvelope plotNoise;
        auto noise = whiteNoise ((int) (12.0 * fs), 0.5f, 11);
        {
            ampsim::Svf lp1, lp2;
            lp1.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::lowpass, 6000.0, 1.30656, 0.0, fs));
            lp2.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::lowpass, 6000.0, 0.541196, 0.0, fs));
            for (auto& v : noise)
                v = (float) lp2.processSample (lp1.processSample ((double) v));
        }
        for (const auto semitones : { -12.0, -5.0, 7.0, 12.0, 24.0 })
        {
            GranularShifter s;
            s.setSettings (granular (semitonesToRatio (semitones)));
            s.prepare (fs);
            std::vector<size_t> starts;
            const auto y = runShifter (s, noise, {}, &starts);
            const auto e = lockedPower (y, starts, (size_t) (0.2 * fs), 240, 480, 48);
            double worst = 0.0;
            for (auto v : e.db)
                worst = std::max (worst, std::abs (v));
            worstNoise = std::max (worstNoise, worst);
            expectLessThan (worst, 0.5);
            noiseRows.add (str (semitones, 0) + " st " + str (worst, 3) + " dB over " + juce::String (e.splices) + " splices");
            if (juce::exactlyEqual (semitones, 12.0))
                plotNoise = e;
        }
        logMessage ("  -> 6 kHz band-limited noise, largest deviation of the splice-locked power (5 ms before to 10 ms after each splice, "
                    "1 ms windows) from the level before the fade (limit 0.5 dB): " + noiseRows.joinIntoString ("; "));

        // Plot: the sine's half-cycle amplitudes, and the splice-locked noise power.
        {
            PlotSeries sine { "220 Hz sine up 12: each half cycle's peak (dB)", {}, {}, plotColour (0) };
            for (size_t i = 0; i < plotEnvelope.size(); ++i)
            {
                sine.x.push_back (0.3 + (double) i / (2.0 * 440.0));
                sine.y.push_back (toDb (plotEnvelope[i] / 0.5));
            }
            PlotOptions o;
            o.title = "Grain ripple: a 220 Hz sine shifted up an octave";
            o.xLabel = "time (s)";
            o.yLabel = "level (dB)";
            o.xMin = 0.3;
            o.xMax = 3.0;
            o.yMin = -1.0;
            o.yMax = 1.0;
            const auto png = proofDir().getChildFile ("pitch_shifter_ripple_sine.png");
            expect (savePlot (png, o, { sine }));

            PlotSeries locked { "band-limited noise up 12, power locked to splices", {}, {}, plotColour (1) };
            for (size_t i = 0; i < plotNoise.db.size(); ++i)
            {
                locked.x.push_back (((double) i - 240.0) / fs * 1000.0);
                locked.y.push_back (plotNoise.db[i]);
            }
            PlotOptions o2;
            o2.title = "Grain ripple on noise: power around a splice, averaged over " + juce::String (plotNoise.splices) + " splices";
            o2.xLabel = "ms from the splice";
            o2.yLabel = "dB";
            o2.xMin = -5.0;
            o2.xMax = 10.0;
            o2.yMin = -1.0;
            o2.yMax = 1.0;
            const auto png2 = proofDir().getChildFile ("pitch_shifter_ripple_noise.png");
            expect (savePlot (png2, o2, { locked }));
            logMessage ("  -> " + png.getFullPathName() + ", " + png2.getFullPathName());
        }
    }

    void granularChords()
    {
        beginTest ("granular: a chord's spectral peaks all move by the ratio");

        // Power chords have a common period of at most 24 ms (the low E with its fifth: 2 x 12.1 ms), inside the
        // splice search's range, so every note stays in tune (limit 2 cents). A triad or an add9 voicing has
        // no short common period, so no single splice offset keeps every note's phase: the leftover phase step
        // repeats at every splice and pulls notes off pitch. That is the granular engine's known weakness; it
        // is measured and reported here, held only to staying nearer its own note than the next (75 cents).
        struct Chord
        {
            const char* name;
            std::vector<double> roots;
            double limit;
        };
        const std::vector<Chord> chords { { "E5 (E2 B2 E3)", { 82.41, 123.47, 164.81 }, 2.0 },
                                          { "A5 (A2 E3 A3)", { 110.0, 164.81, 220.0 }, 2.0 },
                                          { "C major (C3 E3 G3)", { 130.81, 164.81, 196.0 }, 75.0 },
                                          { "Gadd9 open (G2 B2 D3 A3 D4)", { 98.0, 123.47, 146.83, 220.0, 293.66 }, 75.0 } };
        constexpr int order = 16; // 1.37 s, 0.73 Hz bins
        const auto n = (size_t) (3.0 * fs);
        const auto binHz = fs / (double) (1 << order);

        for (const auto& c : chords)
        {
            const auto x = chord (c.roots, n);
            juce::StringArray perShift;
            double worst = 0.0;
            for (const auto semitones : { -12.0, 7.0, 12.0 })
            {
                const auto r = semitonesToRatio (semitones);
                GranularShifter s;
                s.setSettings (granular (r));
                s.prepare (fs);
                const auto y = runShifter (s, x);
                const auto mag = magnitudeSpectrum (y, (size_t) (1.2 * fs), order);
                juce::StringArray errors;
                for (const auto f : c.roots)
                {
                    const auto error = cents (peakFrequency (mag, order, f * r), f * r);
                    errors.add (str (error, 2));
                    worst = std::max (worst, std::abs (error));
                    expectLessThan (std::abs (error), c.limit);
                }

                // Spectral purity: the share of the output's energy within 3 Hz of the shifted partials.
                double on = 0.0, total = 0.0;
                for (size_t k = 1; k < mag.size(); ++k)
                {
                    const auto f = (double) k * binHz;
                    const auto e = mag[k] * mag[k];
                    total += e;
                    for (const auto root : c.roots)
                    {
                        const auto h = std::round (f / (root * r));
                        if (h >= 1.0 && h <= 8.0 && std::abs (f - h * root * r) <= 3.0)
                        {
                            on += e;
                            break;
                        }
                    }
                }
                perShift.add (str (semitones, 0) + " st [" + errors.joinIntoString (" ") + "], energy off the partials " + str (toDb ((total - on) / total) / 2.0, 1)
                              + " dB");

                if (juce::String (c.name).startsWith ("E5") && juce::exactlyEqual (semitones, 12.0))
                    plotChord (x, y, c.roots, r);
            }
            logMessage ("  -> " + juce::String (c.name) + ", each note's fundamental, error in cents (limit " + str (c.limit, 0) + "): "
                        + perShift.joinIntoString ("; ") + "; worst " + str (worst, 2) + " cents");
        }
    }

    void plotChord (const std::vector<float>& x, const std::vector<float>& y, const std::vector<double>& roots, double r)
    {
        constexpr int order = 15;
        const auto before = magnitudeSpectrum (x, (size_t) (1.2 * fs), order);
        const auto after = magnitudeSpectrum (y, (size_t) (1.2 * fs), order);
        const auto binHz = fs / (double) (1 << order);
        double peak = 1.0e-30;
        for (auto v : before)
            peak = std::max (peak, v);
        PlotSeries in { "input: E5 power chord", {}, {}, plotColour (0), 1.5f };
        PlotSeries out { "granular, up 12 semitones", {}, {}, plotColour (1), 1.5f };
        for (size_t k = 1; k < before.size(); ++k)
        {
            const auto f = (double) k * binHz;
            if (f < 50.0 || f > 4000.0)
                continue;
            in.x.push_back (f);
            in.y.push_back (std::max (-100.0, toDb (before[k] / peak)));
            out.x.push_back (f);
            out.y.push_back (std::max (-100.0, toDb (after[k] / peak)));
        }
        std::vector<PlotSeries> series { in, out };
        for (auto f : roots)
            for (int h = 1; h <= 2; ++h)
                series.push_back ({ series.size() == 2 ? juce::String ("expected: the input's partials x 2") : juce::String(), { f * h * r, f * h * r },
                                    { -100.0, 0.0 }, plotColour (2).withAlpha (0.5f), 1.0f, true });
        PlotOptions o;
        o.title = "Chord spectra: E5 before and after a granular octave up";
        o.xLabel = "Hz";
        o.yLabel = "dB";
        o.logX = true;
        o.xMin = 50.0;
        o.xMax = 4000.0;
        o.yMin = -100.0;
        o.yMax = 5.0;
        const auto png = proofDir().getChildFile ("pitch_shifter_chord_spectra.png");
        expect (savePlot (png, o, series));
        logMessage ("  -> " + png.getFullPathName());
    }

    void granularUnityAndTrail()
    {
        beginTest ("granular: at r = 1 it is a plain delay; the trail per shift");

        const auto di = guitarDI ((int) (4.0 * fs));
        {
            GranularShifter s;
            s.setSettings (granular (1.0));
            s.prepare (fs);
            const auto y = runShifter (s, di);
            const auto d = (size_t) juce::roundToInt (s.getVoice().getDelaySamples());
            double worst = 0.0;
            for (size_t i = d; i < di.size(); ++i)
                worst = std::max (worst, (double) std::abs (y[i] - di[i - d]));
            expectEquals (worst, 0.0);
            expectEquals (s.getVoice().getSpliceCount(), 0);
            logMessage ("  -> r = 1: the output is the input " + juce::String ((int) d) + " samples (" + str (1000.0 * (double) d / fs, 2)
                        + " ms) late, largest difference " + juce::String (worst) + " (bit-exact), " + juce::String (s.getVoice().getSpliceCount()) + " splices");
        }

        juce::StringArray rows;
        for (const auto semitones : { -24.0, -12.0, -5.0, -0.1, 0.1, 7.0, 12.0, 24.0 })
        {
            GranularShifter s;
            s.setSettings (granular (semitonesToRatio (semitones)));
            s.prepare (fs);
            double sum = 0.0, lo = 1.0e9, hi = 0.0;
            for (size_t i = 0; i < di.size(); ++i)
            {
                s.processSample (di[i]);
                const auto d = s.getVoice().getDelaySamples();
                sum += d;
                lo = std::min (lo, d);
                hi = std::max (hi, d);
            }
            const auto mean = 1000.0 * sum / (double) di.size() / fs;
            rows.add (str (semitones, 1) + " st " + str (mean, 1) + " ms (" + str (1000.0 * lo / fs, 1) + " to " + str (1000.0 * hi / fs, 1) + ", "
                      + juce::String (s.getVoice().getSpliceCount() / 4) + " splices/s)");
            expectLessThan (mean, 30.0);
        }
        logMessage ("  -> trail (the read head's mean delay over 4 s of guitar DI; the dry is never delayed): " + rows.joinIntoString ("; "));
    }

    void granularGlides()
    {
        beginTest ("granular: ratio glides and delay changes don't click, and the pitch follows a glide exactly");

        // Steps between ratios with a 30 ms glide, on a 220 Hz sine: the largest step in the 60 ms after each
        // change against the steady-state steps either side.
        const auto x = sineWave (220.0, 0.5, (size_t) (3.5 * fs));
        const std::vector<double> sequence { 12.0, -12.0, 7.0, 0.0, -24.0, 24.0 };
        GranularShifter s;
        s.setSettings (granular (semitonesToRatio (sequence[0])));
        s.prepare (fs);
        const auto every = (size_t) (0.5 * fs);
        const auto y = runShifter (s, x, [&] (size_t start)
        {
            const auto k = start / every;
            if (start % every < (size_t) blockSize && k > 0 && k < sequence.size())
                s.setSettings (granular (semitonesToRatio (sequence[k])));
        });
        double worstRatio = 0.0;
        juce::StringArray rows;
        for (size_t k = 1; k < sequence.size(); ++k)
        {
            const auto change = k * every - (k * every) % (size_t) blockSize;
            const auto before = maxStep (y, change - (size_t) (0.25 * fs), change);
            const auto after = maxStep (y, change + (size_t) (0.15 * fs), change + (size_t) (0.45 * fs));
            const auto during = maxStep (y, change, change + (size_t) (0.06 * fs));
            const auto ratio = during / std::max (before, after);
            worstRatio = std::max (worstRatio, ratio);
            rows.add (str (sequence[k - 1], 0) + " -> " + str (sequence[k], 0) + ": " + str (ratio, 3));
        }
        expectLessThan (worstRatio, 1.05);
        logMessage ("  -> largest step in the 60 ms after a ratio change over the larger steady state either side (limit 1.05): "
                    + rows.joinIntoString (", "));

        // Delay changes re-place the head with one crossfade.
        {
            GranularShifter d;
            d.setSettings (granular (semitonesToRatio (7.0), 0.0));
            d.prepare (fs, 50.0);
            const std::vector<double> delays { 0.0, 15.0, 3.0, 40.0, 40.5, 10.0 };
            const auto z = runShifter (d, x, [&] (size_t start)
            {
                const auto k = start / every;
                if (start % every < (size_t) blockSize && k > 0 && k < delays.size())
                    d.setSettings (granular (semitonesToRatio (7.0), delays[k]));
            });
            const auto steady = maxStep (z, (size_t) (0.2 * fs), every);
            double worst = 0.0;
            for (size_t k = 1; k < delays.size(); ++k)
                worst = std::max (worst, maxStep (z, k * every - 64, k * every + (size_t) (0.05 * fs)) / steady);
            expectLessThan (worst, 1.05);
            logMessage ("  -> voice delay 0 -> 15 -> 3 -> 40 -> 40.5 -> 10 ms at +7: largest step after a change " + str (worst, 3)
                        + " x the steady state (limit 1.05)");
        }

        // A slow glide (300 ms, +0 -> +7 semitones): each cycle's pitch against the glide's ratio at that time.
        {
            GranularShifter g;
            g.setSettings (granular (1.0, 0.0, 300.0));
            g.prepare (fs);
            const auto start = (size_t) (0.5 * fs) - (size_t) (0.5 * fs) % (size_t) blockSize;
            const auto z = runShifter (g, x, [&] (size_t s0)
            {
                if (s0 == start)
                    g.setSettings (granular (semitonesToRatio (7.0), 0.0, 300.0));
            });
            const auto c = crossings (z, start, start + (size_t) (0.3 * fs));
            double worst = 0.0;
            for (size_t i = 1; i < c.size(); ++i)
            {
                const auto mid = 0.5 * (c[i] + c[i - 1]);
                const auto progress = juce::jlimit (0.0, 1.0, (mid - (double) start) / (0.3 * fs));
                const auto expected = 220.0 * std::pow (2.0, 7.0 / 12.0 * progress);
                worst = std::max (worst, std::abs (cents (fs / (c[i] - c[i - 1]), expected)));
            }
            expectLessThan (worst, 1.0);
            logMessage ("  -> a 300 ms glide from 0 to +7 semitones on 220 Hz: every cycle within " + str (worst, 3)
                        + " cents of the log-linear glide (limit 1)");
        }
    }

    void granularSmear()
    {
        beginTest ("granular: transient smear on fast playing (the other side of the 32 ms search range)");

        // 16th notes at 120 BPM: a 2 ms burst of a 2 kHz tone every 125 ms. A splice repeats a stretch of input
        // (r > 1) or skips one (r < 1), so an attack can come out twice, or quieter, or not at all. For each
        // burst: the output's main arrival (the loudest 2 ms within 60 ms), how loud it is against the burst,
        // and the energy anywhere else in those 60 ms (repeats) against the main arrival.
        const auto n = (size_t) (8.0 * fs);
        const auto every = (size_t) (0.125 * fs);
        const auto burst = (size_t) (0.002 * fs);
        std::vector<float> x (n, 0.0f);
        for (size_t start = (size_t) (0.25 * fs); start + burst < n; start += every)
            for (size_t i = 0; i < burst; ++i)
                x[start + i] = (float) (0.5 * std::sin (twoPi * 2000.0 * (double) i / fs) * (0.5 - 0.5 * std::cos (twoPi * (double) i / (double) burst)));
        const auto burstEnergy = (double) burst * 0.25 * 0.375 * 0.5 * 4.0 / 4.0; // sum of x^2: 0.5^2 * mean(hann^2) * mean(sin^2) * length

        juce::ignoreUnused (burstEnergy);
        juce::StringArray rows;
        for (const auto semitones : { -12.0, 7.0, 12.0 })
        {
            GranularShifter s;
            s.setSettings (granular (semitonesToRatio (semitones)));
            s.prepare (fs);
            const auto y = runShifter (s, x);

            // Each burst's arrivals: energy in 1 ms steps over the 60 ms after it; the loudest is the main
            // arrival, the loudest at least 4 ms away from it a repeat.
            int lost = 0, doubled = 0, total = 0;
            std::vector<double> mainLevels, spacings;
            const auto step = (size_t) (0.001 * fs);
            for (size_t start = (size_t) (0.25 * fs) + every; start + every < n; start += every, ++total)
            {
                std::vector<double> e;
                for (size_t t = start; t + step <= start + (size_t) (0.060 * fs); t += step)
                {
                    double sum = 0.0;
                    for (size_t i = 0; i < step; ++i)
                        sum += (double) y[t + i] * (double) y[t + i];
                    e.push_back (sum);
                }
                const auto main = (size_t) (std::max_element (e.begin(), e.end()) - e.begin());
                double inputPeak = 0.0;
                for (size_t t = start; t + step <= start + burst + step; t += step)
                {
                    double sum = 0.0;
                    for (size_t i = 0; i < step; ++i)
                        sum += (double) x[t + i] * (double) x[t + i];
                    inputPeak = std::max (inputPeak, sum);
                }
                const auto mainDb = 10.0 * std::log10 (std::max (1.0e-30, e[main]) / inputPeak);
                if (mainDb < -10.0)
                {
                    ++lost;
                    continue;
                }
                mainLevels.push_back (mainDb);
                double second = 0.0;
                size_t secondAt = main;
                for (size_t k = 0; k < e.size(); ++k)
                    if ((k + 4 <= main || k >= main + 4) && e[k] > second)
                    {
                        second = e[k];
                        secondAt = k;
                    }
                if (second > 0.25 * e[main]) // a second arrival within 6 dB
                {
                    ++doubled;
                    spacings.push_back (std::abs ((double) secondAt - (double) main));
                }
            }
            std::sort (mainLevels.begin(), mainLevels.end());
            std::sort (spacings.begin(), spacings.end());
            rows.add (str (semitones, 0) + " st: " + juce::String (lost) + " of " + juce::String (total) + " attacks lost (below -10 dB), "
                      + juce::String (doubled) + " heard twice (a second arrival within 6 dB"
                      + (spacings.empty() ? juce::String (")") : ", " + str (spacings[spacings.size() / 2], 0) + " ms apart in the median, up to " + str (spacings.back(), 0) + " ms)")
                      + ", the rest at a median " + str (mainLevels.empty() ? 0.0 : mainLevels[mainLevels.size() / 2], 1) + " dB");
        }
        logMessage ("  -> 2 ms bursts every 125 ms (16ths at 120 BPM): " + rows.joinIntoString ("; ")
                    + ". Time-domain shifting can't avoid it: an octave down plays half the input, so half of all short attacks fall in skipped stretches; "
                    "an octave up plays every moment twice. The search range sets how far apart the two arrivals are.");
    }

    void granularRealtimeAndCpu()
    {
        beginTest ("granular: nothing allocates or locks; CPU per voice");

        const auto di = guitarDI ((int) (10.0 * fs));
        {
            PitchShifterInput input;
            input.prepare (fs, GranularVoice::maxDelayMs (50.0, 2.0), GranularVoice::searchMarginMs);
            std::array<GranularVoice, 8> voices;
            for (auto& v : voices)
                v.prepare (fs);
            GranularShifter single;
            single.prepare (fs, 20.0);
            int changes = 0;

            rtcheck::begin();
            for (size_t start = 0; start + (size_t) blockSize <= di.size(); start += (size_t) blockSize)
            {
                const auto block = start / (size_t) blockSize;
                if (block % 37 == 0)
                {
                    for (size_t v = 0; v < voices.size(); ++v)
                        voices[v].setSettings (granular (semitonesToRatio (-24.0 + (double) ((block / 37 + v * 5) % 49)), (double) ((block + v) % 50),
                                                         (double) (block % 3) * 40.0));
                    single.setSettings (granular (semitonesToRatio ((double) (block % 25) - 12.0), (double) (block % 20)));
                    ++changes;
                }
                for (size_t i = start; i < start + (size_t) blockSize; ++i)
                {
                    float sum = 0.0f;
                    for (size_t v = 0; v < voices.size(); ++v)
                        sum += voices[v].process (input, 1.0 + 0.001 * std::sin ((double) i * 1.0e-4 * (double) (v + 1)), 48.0 * std::sin ((double) i * 2.0e-5));
                    input.push (di[i]);
                    sum += single.processSample (di[i]);
                    juce::ignoreUnused (sum);
                }
            }
            const auto counts = rtcheck::end();
            expectEquals (counts.allocations, 0L);
            expectEquals (counts.frees, 0L);
            expectEquals (counts.blockingLocks, 0L);
            logMessage ("  -> 10 s, 8 voices on one input plus a standalone shifter, " + juce::String (changes) + " changes of ratio, delay, and glide, "
                        + "drift on every voice: " + juce::String (counts.allocations) + " allocations, " + juce::String (counts.frees) + " frees, "
                        + juce::String (counts.blockingLocks) + " blocking locks");
        }

        juce::StringArray rows;
        for (const auto semitones : { -12.0, 7.0, 12.0, 24.0 })
        {
            GranularShifter s;
            s.setSettings (granular (semitonesToRatio (semitones)));
            s.prepare (fs);
            std::vector<double> micros;
            std::vector<float> out ((size_t) blockSize);
            for (size_t start = 0; start + (size_t) blockSize <= di.size(); start += (size_t) blockSize)
            {
                const auto t0 = std::chrono::steady_clock::now();
                s.process (di.data() + start, out.data(), blockSize);
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            std::sort (micros.begin(), micros.end());
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            rows.add (str (semitones, 0) + " st mean " + str (mean, 2) + " us (" + str (100.0 * mean / deadlineMicros, 2) + "%), p99 "
                      + str (micros[(size_t) (0.99 * (double) micros.size())], 2) + " us, worst " + str (micros.back(), 1) + " us");
        }
        logMessage ("  -> one mono voice per 128-sample buffer on 10 s of guitar DI: " + rows.joinIntoString ("; "));
    }
};

PitchShifterTests pitchShifterTests;
} // namespace

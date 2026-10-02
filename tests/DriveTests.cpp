#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Boost.h"
#include "dsp/FftDouble.h"
#include "dsp/Overdrive.h"

#include <chrono>
#include <complex>
#include <map>
#include <numeric>

// Tests for the drive engine, the Overdrive (Mid Drive, Distortion, Transparent), and the Boost (Clean,
// Tight, Screamer). The references are tests/fixtures/drive, rendered by prototypes/circuits.py: AC
// analyses and 16x transient simulations of the full schematics.

namespace
{
using namespace testing;
using ampsim::Boost;
using ampsim::DriveEngine;
using ampsim::Overdrive;

constexpr double pi = juce::MathConstants<double>::pi;
constexpr double circuitToleranceDb = -60.0;  // waveform error, model at 4x vs the 16x circuit simulation
constexpr double harmonicToleranceDb = 0.1;   // every harmonic above -80 dB
constexpr double aliasingLimitDb = -50.0;     // inharmonic energy at 4x, full drive, tones up to 1.3 kHz

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/drive").getChildFile (name);
}

std::vector<float> readMono (const juce::File& file)
{
    const auto b = readWav (file);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

std::vector<double> toDouble (const std::vector<float>& x) { return std::vector<double> (x.begin(), x.end()); }

/// Runs a block over a mono signal in fixed-size buffers, with an optional callback before each buffer.
template <typename BlockType>
std::vector<float> run (BlockType& block, const std::vector<float>& x, int bufferSize = blockSize,
                        const std::function<void (size_t)>& beforeBuffer = {})
{
    auto y = x;
    for (size_t start = 0; start < y.size(); start += (size_t) bufferSize)
    {
        if (beforeBuffer)
            beforeBuffer (start);
        const auto len = std::min ((size_t) bufferSize, y.size() - start);
        float* channels[1] = { y.data() + start };
        block.process (juce::dsp::AudioBlock<float> (channels, 1, len), {});
    }
    return y;
}

Overdrive::Settings overdriveSettings (Overdrive::Mode mode, float drive, float tone, int factor = 4)
{
    Overdrive::Settings s;
    s.mode = mode;
    s.drive = drive;
    s.tone = tone;
    s.levelDb = 0.0f;
    s.mix = 1.0f;
    s.tightHz = DriveEngine::tightOffHz;
    s.oversampling = factor;
    return s;
}

std::vector<float> renderOverdrive (const Overdrive::Settings& s, const std::vector<float>& x, int bufferSize = blockSize)
{
    Overdrive o;
    o.setSettings (s);
    o.prepare (fs, bufferSize);
    return run (o, x, bufferSize);
}

std::vector<float> renderBoost (const Boost::Settings& s, const std::vector<float>& x, int bufferSize = blockSize)
{
    Boost b;
    b.setSettings (s);
    b.prepare (fs, bufferSize);
    return run (b, x, bufferSize);
}

/// Every Overdrive mode, in Mode order, with its UI name and the fixtures' circuit name.
struct ModeInfo
{
    Overdrive::Mode mode;
    const char* name;
    const char* tag;
};

const std::vector<ModeInfo>& overdriveModes()
{
    static const std::vector<ModeInfo> modes { { Overdrive::Mode::midDrive, "Mid Drive", "mid_drive" },
                                               { Overdrive::Mode::distortion, "Distortion", "distortion" },
                                               { Overdrive::Mode::transparent, "Transparent", "transparent" } };
    return modes;
}

const ModeInfo& infoOf (Overdrive::Mode m) { return overdriveModes()[(size_t) m]; }
juce::String modeName (Overdrive::Mode m) { return infoOf (m).name; }
juce::String modeTag (Overdrive::Mode m) { return infoOf (m).tag; }

Overdrive::Mode modeOfTag (const juce::String& tag)
{
    for (const auto& m : overdriveModes())
        if (tag == m.tag)
            return m.mode;
    jassertfalse;
    return Overdrive::Mode::midDrive;
}

/// Waveform tolerance against the 16x circuit simulation, per mode.
double circuitTolerance (Overdrive::Mode) { return circuitToleranceDb; }

/// How closely each model's own analytic small-signal response must match AC analysis of the netlist.
double analyticTolerance (Overdrive::Mode) { return 1.0e-6; }

/// Aliasing limit at 4x for full-drive tones up to 1.3 kHz at -12 dBFS. The Transparent's gain stage is a
/// fast op-amp (a TL072, 13 V/us) that clips on its 9 V rails as a near-square wave at full gain: its
/// corners take a fraction of a microsecond, far under one 192 kHz sample, so at 4x its hottest high
/// note aliases at -48 dB (the RAT's LM308 slews at 0.3 V/us, which rounds its corners). 8x clears it
/// with 20 dB to spare; antiderivative antialiasing would too (ASSUMPTIONS V20).
double aliasingLimit (Overdrive::Mode mode) { return mode == Overdrive::Mode::transparent ? -46.0 : aliasingLimitDb; }

/// A plot range that holds the data: +-(max |v| x 1.1), rounded up to 0.5.
double plotRange (const std::vector<double>& a, const std::vector<double>& b, double scale)
{
    double peak = 0.0;
    for (const auto* v : { &a, &b })
        for (const auto x : *v)
            peak = std::max (peak, std::abs (x) * scale);
    return std::ceil (peak * 1.1 / 0.5) * 0.5;
}

// ---- Oversampling-aware comparison -------------------------------------------------------------

/// Up then down with nothing between: the oversampling chain's own linear response.
std::vector<double> chainResponse (int factor, int length = 512)
{
    ampsim::Upsampler up;
    ampsim::Downsampler down;
    up.setFactor (factor);
    down.setFactor (factor);
    std::vector<double> in ((size_t) length, 0.0), high ((size_t) (length * factor)), out ((size_t) length);
    in[0] = 1.0;
    up.process (in.data(), high.data(), length);
    down.process (high.data(), out.data(), length);
    return out;
}

std::vector<double> convolve (const std::vector<float>& x, const std::vector<double>& h)
{
    std::vector<double> y (x.size(), 0.0);
    for (size_t n = 0; n < x.size(); ++n)
        for (size_t k = 0; k < h.size() && k <= n; ++k)
            y[n] += h[k] * (double) x[n - k];
    return y;
}

std::complex<double> dtft (const std::vector<double>& h, double frequency)
{
    // Phasor by recurrence, renormalized now and then: fast and accurate over long responses.
    const auto step = std::polar (1.0, -2.0 * pi * frequency / fs);
    std::complex<double> w (1.0, 0.0), sum (0.0, 0.0);
    for (size_t n = 0; n < h.size(); ++n)
    {
        sum += h[n] * w;
        w *= step;
        if ((n & 1023) == 1023)
            w /= std::abs (w);
    }
    return sum;
}

/// The upsampling chain's group delay at low frequencies, in base-rate samples.
double upsamplerDelay (int factor)
{
    ampsim::Upsampler up;
    up.setFactor (factor);
    std::vector<double> in (512, 0.0), out ((size_t) (512 * factor));
    in[0] = 1.0;
    up.process (in.data(), out.data(), 512);
    const auto phaseAt = [&] (double f) // cycles per high-rate sample
    {
        std::complex<double> s (0.0, 0.0);
        for (size_t m = 0; m < out.size(); ++m)
            s += out[m] * std::polar (1.0, -2.0 * pi * f * (double) m);
        return std::arg (s);
    };
    const auto f = 100.0 / (fs * factor), df = 1.0e-7;
    return -(phaseAt (f + df) - phaseAt (f - df)) / (2.0 * pi * 2.0 * df) / factor;
}

/// Puts a band-limited 48 kHz reference through what the model does to its own output: the input's
/// trip through the upsampler (at the input's low frequencies, a delay) and the model's real downsampler.
/// The reference is interpolated to the oversampled rate, delayed, with a Kaiser-windowed sinc (linear
/// phase, flat to 20 kHz), then decimated by ampsim::Downsampler.
///
/// Why not just apply the whole up-and-down response to the reference: that charges the upsampler's
/// phase at high frequencies to the output's harmonics, which in the model never pass through the
/// upsampler (they're made after it). Measured that way, hard clipping (Distortion) "misses" by -33 to
/// -43 dB at 4x and 8x alike, which is the alignment, not the model.
std::vector<double> alignReference (const std::vector<float>& reference, int factor)
{
    const auto delay = upsamplerDelay (factor);
    constexpr int half = 48; // base-rate samples each side
    constexpr double beta = 10.0;
    const auto fc = 22500.0 / fs;
    const auto bessel = [] (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    const auto kernel = [&] (double t)
    {
        if (std::abs (t) >= half)
            return 0.0;
        const auto sinc = std::abs (t) < 1.0e-12 ? 2.0 * fc : std::sin (2.0 * pi * fc * t) / (pi * t);
        const auto r = t / half;
        return sinc * bessel (beta * std::sqrt (1.0 - r * r)) / bessel (beta);
    };

    const auto n = reference.size();
    std::vector<double> high (n * (size_t) factor, 0.0);
    for (size_t m = 0; m < high.size(); ++m)
    {
        const auto t = (double) m / factor - delay;
        const auto centre = (long) std::floor (t);
        double sum = 0.0;
        for (long k = centre - half; k <= centre + half + 1; ++k)
            if (k >= 0 && k < (long) n)
                sum += (double) reference[(size_t) k] * kernel (t - (double) k);
        high[m] = sum;
    }

    ampsim::Downsampler down;
    down.setFactor (factor);
    std::vector<double> out (n);
    down.process (high.data(), out.data(), (int) n);
    return out;
}

/// A zero-phase low-pass at 20 kHz (Blackman-windowed sinc, 255 taps): comparisons ignore 20-24 kHz,
/// where the reference's resampling filter and the oversampler's band edge differ.
std::vector<double> bandLimit (const std::vector<double>& x)
{
    constexpr int half = 127;
    std::vector<double> h (2 * half + 1);
    const auto fc = 20000.0 / fs;
    double sum = 0.0;
    for (int k = -half; k <= half; ++k)
    {
        const auto sinc = k == 0 ? 2.0 * fc : std::sin (2.0 * pi * fc * k) / (pi * k);
        const auto w = 0.42 + 0.5 * std::cos (pi * k / (half + 1)) + 0.08 * std::cos (2.0 * pi * k / (half + 1));
        h[(size_t) (k + half)] = sinc * w;
        sum += sinc * w;
    }
    for (auto& v : h)
        v /= sum;
    std::vector<double> y (x.size(), 0.0);
    for (size_t n = 0; n < x.size(); ++n)
        for (int k = -half; k <= half; ++k)
        {
            const auto i = (long) n - k;
            if (i >= 0 && i < (long) x.size())
                y[n] += h[(size_t) (k + half)] * x[(size_t) i];
        }
    return y;
}

double errorDb (const std::vector<double>& actual, const std::vector<double>& expected, size_t from, size_t to)
{
    double e = 0.0, s = 0.0;
    for (size_t n = from; n < std::min (to, expected.size()); ++n)
    {
        e += (actual[n] - expected[n]) * (actual[n] - expected[n]);
        s += expected[n] * expected[n];
    }
    return 10.0 * std::log10 (std::max (e, 1.0e-300) / s);
}

// ---- Spectra ------------------------------------------------------------------------------------

/// Power spectrum (|X|^2 per bin) of the last 2^order samples, 4-term Blackman-Harris window.
std::vector<double> powerSpectrum (const std::vector<double>& x, int order, size_t endOffset = 64)
{
    ampsim::FftDouble fft (order);
    const auto n = (size_t) fft.size();
    std::vector<std::complex<double>> data (n);
    const auto start = x.size() - n - endOffset;
    for (size_t i = 0; i < n; ++i)
    {
        const auto t = 2.0 * pi * (double) i / (double) n;
        const auto w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2.0 * t) - 0.01168 * std::cos (3.0 * t);
        data[i] = w * x[start + i];
    }
    fft.forward (data);
    std::vector<double> p (n / 2);
    for (size_t k = 0; k < p.size(); ++k)
        p[k] = std::norm (data[k]);
    return p;
}

/// Harmonic levels in dB relative to the fundamental (peak bin within +-3 of each k f0).
std::vector<double> harmonics (const std::vector<double>& x, double f0, int count, int order = 13)
{
    const auto p = powerSpectrum (x, order);
    const auto n = (double) (2 * p.size());
    std::vector<double> levels;
    for (int k = 1; k <= count; ++k)
    {
        const auto bin = k * f0 * n / fs;
        double peak = 1.0e-60;
        for (auto b = (long) std::floor (bin) - 3; b <= (long) std::ceil (bin) + 3; ++b)
            if (b > 0 && b < (long) p.size())
                peak = std::max (peak, p[(size_t) b]);
        levels.push_back (10.0 * std::log10 (peak));
    }
    const auto fundamental = levels[0];
    for (auto& l : levels)
        l -= fundamental;
    return levels;
}

/// Energy outside the harmonics of f0 (and outside DC to 50 Hz) relative to the total, in dB: aliasing.
double inharmonicDb (const std::vector<double>& x, double f0, int order = 13)
{
    const auto p = powerSpectrum (x, order);
    const auto binHz = fs / (double) (2 * p.size());
    double harmonic = 0.0, other = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
    {
        const auto f = (double) k * binHz;
        if (f < 50.0)
            continue;
        const auto nearest = std::round (f / f0);
        const bool onHarmonic = nearest >= 1.0 && std::abs (f - nearest * f0) <= 4.5 * binHz;
        (onHarmonic ? harmonic : other) += p[k];
    }
    return 10.0 * std::log10 (std::max (other, 1.0e-300) / (harmonic + other));
}

// ---- Fixtures -----------------------------------------------------------------------------------

struct CircuitCase
{
    juce::String name, circuit, input, expected;
    float drive = 0.0f, tone = 0.0f;
};

std::vector<CircuitCase> circuitCases()
{
    const auto json = juce::JSON::parse (fixture ("cases.json"));
    std::vector<CircuitCase> cases;
    if (auto* object = json["cases"].getDynamicObject())
        for (const auto& p : object->getProperties())
            cases.push_back ({ p.name.toString(), p.value["circuit"].toString(), p.value["input"].toString(), p.value["expected"].toString(),
                               (float) (double) p.value["drive"], (float) (double) p.value["tone"] });
    return cases;
}

struct AcTable
{
    std::vector<double> frequencies;
    std::map<juce::String, std::vector<std::complex<double>>> responses; // "d0.5_t1.0" -> H
};

AcTable readAc (const juce::String& circuit)
{
    AcTable t;
    juce::StringArray lines;
    fixture ("ac_" + circuit + ".csv").readLines (lines);
    juce::StringArray header;
    for (const auto& line : lines)
    {
        if (line.startsWith ("#") || line.trim().isEmpty())
            continue;
        juce::StringArray fields;
        fields.addTokens (line, ",", "");
        if (header.isEmpty())
        {
            header = fields;
            continue;
        }
        t.frequencies.push_back (fields[0].getDoubleValue());
        for (int c = 1; c + 1 < fields.size(); c += 2)
        {
            const auto key = header[c].upToLastOccurrenceOf ("_db", false, false);
            const auto mag = std::pow (10.0, fields[c].getDoubleValue() / 20.0);
            t.responses[key].push_back (std::polar (mag, fields[c + 1].getDoubleValue() * pi / 180.0));
        }
    }
    return t;
}

juce::String acKey (double drive, double tone) { return "d" + juce::String (drive, 1) + "_t" + juce::String (tone, 1); }

// ---- Small-signal measurement -------------------------------------------------------------------

/// The model's response to a tiny impulse (-160 dBFS, so every diode stays linear), divided by the
/// oversampling chain's own response: what's left is the circuit as discretized at 4x.
template <typename Process>
std::vector<std::complex<double>> smallSignal (const std::vector<double>& frequencies, Process&& render, int factor = 4)
{
    constexpr double amplitude = 1.0e-8;
    std::vector<float> x ((size_t) 1 << 17, 0.0f);
    x[0] = (float) amplitude;
    const auto y = render (x);
    std::vector<double> h (y.size());
    for (size_t n = 0; n < y.size(); ++n)
        h[n] = (double) y[n] / amplitude;
    const auto chain = chainResponse (factor);
    std::vector<std::complex<double>> out;
    for (const auto f : frequencies)
        out.push_back (dtft (h, f) / dtft (chain, f));
    return out;
}

double dbOf (std::complex<double> h) { return 20.0 * std::log10 (std::abs (h)); }

struct Deviation
{
    double low = 0.0, high = 0.0, phase = 0.0; // worst |dB| to 5 kHz, worst |dB| 5-20 kHz, worst degrees to 5 kHz
};

Deviation compare (const std::vector<double>& f, const std::vector<std::complex<double>>& got, const std::vector<std::complex<double>>& want)
{
    Deviation d;
    for (size_t i = 0; i < f.size(); ++i)
    {
        const auto db = std::abs (dbOf (got[i]) - dbOf (want[i]));
        if (f[i] <= 5000.0)
        {
            d.low = std::max (d.low, db);
            d.phase = std::max (d.phase, std::abs (std::arg (got[i] / want[i])) * 180.0 / pi);
        }
        else
        {
            d.high = std::max (d.high, db);
        }
    }
    return d;
}

std::vector<float> sineWave (double frequency, double amplitude, int numSamples, double fadeSeconds = 0.01)
{
    auto x = sine (frequency, amplitude, numSamples);
    const auto fade = (int) (fadeSeconds * fs);
    for (int n = 0; n < std::min (fade, numSamples); ++n)
        x[(size_t) n] *= (float) n / (float) fade;
    return x;
}

class DriveTests final : public juce::UnitTest
{
public:
    DriveTests() : juce::UnitTest ("Drive", "ampsim") {}

    void runTest() override
    {
        circuitMatch();
        linearBehaviour();
        aliasing();
        noDc();
        transparency();
        mixAndLevel();
        noClicks();
        modeSwitching();
        realTime();
        cpu();
    }

private:
    void circuitMatch()
    {
        beginTest ("circuit match: models at 4x vs 16x circuit simulations of the schematics (limits: waveform "
                   + juce::String (circuitToleranceDb, 0) + " dB, harmonics " + juce::String (harmonicToleranceDb) + " dB)");

        juce::StringArray lines;
        double worstError = -400.0, worstHarmonic = 0.0;
        for (const auto& c : circuitCases())
        {
            const auto mode = modeOfTag (c.circuit);
            const auto x = readMono (fixture (c.input));
            const auto reference = readMono (fixture (c.expected));
            const auto end = reference.size() - 200; // the reference's resampling filter rings at the very end

            double errors[2] {};
            std::vector<double> expected4, actual4;
            for (const auto factor : { 4, 8 })
            {
                const auto expected = bandLimit (alignReference (reference, factor));
                const auto actual = bandLimit (toDouble (renderOverdrive (overdriveSettings (mode, c.drive, c.tone, factor), x)));
                errors[factor == 4 ? 0 : 1] = errorDb (actual, expected, 0, end);
                if (factor == 4)
                {
                    expected4 = expected;
                    actual4 = actual;
                }
            }
            expectLessThan (errors[0], circuitTolerance (mode), c.name);
            worstError = std::max (worstError, errors[0]);

            juce::String harmonicText;
            if (c.input.contains ("sine"))
            {
                const auto f0 = c.input.contains ("220") ? 220.0 : 110.0;
                const auto ref = harmonics (expected4, f0, 15), got = harmonics (actual4, f0, 15);
                double worst = 0.0;
                int counted = 0;
                for (size_t k = 1; k < ref.size(); ++k)
                    if (ref[k] > -80.0)
                    {
                        worst = std::max (worst, std::abs (got[k] - ref[k]));
                        ++counted;
                    }
                expectLessThan (worst, harmonicToleranceDb, c.name);
                worstHarmonic = std::max (worstHarmonic, worst);
                harmonicText = "; " + juce::String (counted) + " harmonics above -80 dB within " + juce::String (worst, 3) + " dB (H2 "
                               + juce::String (ref[1], 1) + ", H3 " + juce::String (ref[2], 1) + ", H5 " + juce::String (ref[4], 1) + " dB)";
                if (c.name.endsWith ("sine110_d100"))
                    plotCircuitCase (mode, x, reference, expected4, actual4, ref, got);
            }
            lines.add (c.name + ": " + juce::String (errors[0], 1) + " dB at 4x (" + juce::String (errors[1], 1) + " dB at 8x)" + harmonicText);
        }
        for (const auto& l : lines)
            logMessage ("  -> " + l);

        // The Boost's Screamer is the same circuit at drive 0, tone noon.
        {
            const auto x = readMono (fixture ("input_sine220.wav"));
            const auto expected = bandLimit (alignReference (readMono (fixture ("expected_mid_drive_sine220_d0.wav")), 4));
            Boost::Settings s;
            s.mode = Boost::Mode::screamer;
            const auto actual = bandLimit (toDouble (renderBoost (s, x)));
            const auto err = errorDb (actual, expected, 0, expected.size() - 200);
            expectLessThan (err, circuitToleranceDb);
            logMessage ("  -> Boost Screamer vs the Mid Drive circuit at drive 0, tone noon (220 Hz, 200 mV): " + juce::String (err, 1) + " dB");
        }

        // Newton iterations per solve over a heavily driven run.
        for (const auto& info : overdriveModes())
        {
            const auto mode = info.mode;
            Overdrive o;
            o.setSettings (overdriveSettings (mode, 1.0f, 0.5f));
            o.prepare (fs, blockSize);
            run (o, guitarDI ((int) fs));
            logMessage ("  -> " + modeName (mode) + " at full drive on the guitar DI: " + juce::String (o.getEngine().getCircuit ((int) mode).meanIterations(), 2)
                        + " Newton iterations per solve on average");
        }
        logMessage ("  -> worst waveform error " + juce::String (worstError, 1) + " dB, worst harmonic difference " + juce::String (worstHarmonic, 3) + " dB");
    }

    void plotCircuitCase (Overdrive::Mode mode, const std::vector<float>& input, const std::vector<float>& reference, const std::vector<double>& expected,
                          const std::vector<double>& actual, const std::vector<double>& refHarmonics, const std::vector<double>& gotHarmonics)
    {
        const auto tag = modeTag (mode);
        const auto volts = ampsim::drive::defaultVoltsAtFullScale;
        const auto period = (size_t) std::round (fs / 110.0);
        const auto start = reference.size() - 400 - 2 * period;
        const std::vector<double> shown (expected.begin() + (long) start, expected.begin() + (long) (start + 2 * period));
        const auto range = plotRange (shown, {}, volts);

        // Waveforms over two periods.
        PlotSeries circuit { "circuit (16x simulation)", {}, {}, plotColour (6), 4.0f }, model { "model (4x)", {}, {}, plotColour (1), 1.5f };
        for (size_t n = start; n < start + 2 * period; ++n)
        {
            const auto ms = 1000.0 * (double) (n - start) / fs;
            circuit.x.push_back (ms);
            circuit.y.push_back (expected[n] * volts);
            model.x.push_back (ms);
            model.y.push_back (actual[n] * volts);
        }
        PlotOptions o;
        o.title = modeName (mode) + ", full drive, 110 Hz at 1 V: output, circuit vs model";
        o.xLabel = "Time (ms)";
        o.yLabel = "Output (V)";
        o.xMin = 0.0; o.xMax = 2000.0 * (double) period / fs; o.yMin = -range; o.yMax = range;
        auto png = proofDir().getChildFile ("drive_" + tag + "_waveform.png");
        expect (savePlot (png, o, { circuit, model }));
        logMessage ("  -> " + png.getFullPathName());

        // Transfer curve: output against input over one period.
        PlotSeries curveCircuit { "circuit", {}, {}, plotColour (6), 4.0f }, curveModel { "model", {}, {}, plotColour (1), 1.5f };
        const auto delay = (size_t) std::round (ampsim::oversampling::groupDelaySamples (4)); // both outputs carry it
        for (size_t n = start; n < start + period; ++n)
        {
            curveCircuit.x.push_back ((double) input[n - delay] * volts);
            curveCircuit.y.push_back (expected[n] * volts);
            curveModel.x.push_back ((double) input[n - delay] * volts);
            curveModel.y.push_back (actual[n] * volts);
        }
        o.title = modeName (mode) + ", full drive: transfer curve over one 110 Hz period (filters make it a loop)";
        o.xLabel = "Input (V)";
        o.yLabel = "Output (V)";
        o.xMin = -1.1; o.xMax = 1.1; o.yMin = -range; o.yMax = range;
        png = proofDir().getChildFile ("drive_" + tag + "_transfer.png");
        expect (savePlot (png, o, { curveCircuit, curveModel }));
        logMessage ("  -> " + png.getFullPathName());

        // Harmonic levels.
        PlotSeries hc { "circuit", {}, {}, plotColour (6), 5.0f }, hm { "model", {}, {}, plotColour (1), 2.0f, true };
        for (size_t k = 0; k < refHarmonics.size(); ++k)
        {
            hc.x.push_back ((double) k + 1.0);
            hc.y.push_back (std::max (-120.0, refHarmonics[k]));
            hm.x.push_back ((double) k + 1.0);
            hm.y.push_back (std::max (-120.0, gotHarmonics[k]));
        }
        o.title = modeName (mode) + ", full drive, 110 Hz at 1 V: harmonics relative to the fundamental";
        o.xLabel = "Harmonic";
        o.yLabel = "Level (dB)";
        o.xMin = 1.0; o.xMax = (double) refHarmonics.size(); o.yMin = -120.0; o.yMax = 5.0;
        png = proofDir().getChildFile ("drive_" + tag + "_harmonics.png");
        expect (savePlot (png, o, { hc, hm }));
        logMessage ("  -> " + png.getFullPathName());
    }

    void linearBehaviour()
    {
        beginTest ("linear behaviour: at -160 dBFS the response matches AC analysis of the full circuit (every drive and tone setting)");
        for (const auto& info : overdriveModes())
        {
            const auto mode = info.mode;
            const auto name = juce::String (info.name), tag = juce::String (info.tag);
            const auto table = readAc (tag);
            Deviation worst;
            double lowest = 1000.0, highest = -1000.0;
            double analyticWorst = 0.0;
            std::vector<PlotSeries> series;
            int colour = 0;
            for (const auto drive : { 0.0, 0.5, 1.0 })
                for (const auto tone : { 0.0, 0.5, 1.0 })
                {
                    const auto settings = overdriveSettings (mode, (float) drive, (float) tone);
                    const auto measured = smallSignal (table.frequencies, [&] (const std::vector<float>& x) { return renderOverdrive (settings, x); });
                    const auto& ac = table.responses.at (acKey (drive, tone));
                    const auto d = compare (table.frequencies, measured, ac);
                    worst.low = std::max (worst.low, d.low);
                    worst.high = std::max (worst.high, d.high);
                    worst.phase = std::max (worst.phase, d.phase);

                    // The C++ analytic small-signal response (from the same transfer functions as the filters).
                    Overdrive o;
                    o.setSettings (settings);
                    o.prepare (fs, blockSize);
                    for (size_t i = 0; i < table.frequencies.size(); ++i)
                        analyticWorst = std::max (analyticWorst,
                                                  std::abs (dbOf (o.getEngine().getCircuit ((int) mode).smallSignalResponse (table.frequencies[i])) - dbOf (ac[i])));

                    if (juce::exactlyEqual (drive, 0.5))
                    {
                        PlotSeries a { "circuit, tone " + juce::String (tone, 1), {}, {}, plotColour (colour), 4.0f };
                        PlotSeries m { "model", {}, {}, plotColour (colour + 3), 1.5f, true };
                        for (size_t i = 0; i < table.frequencies.size(); ++i)
                        {
                            a.x.push_back (table.frequencies[i]);
                            a.y.push_back (dbOf (ac[i]));
                            m.x.push_back (table.frequencies[i]);
                            m.y.push_back (dbOf (measured[i]));
                            lowest = std::min (lowest, dbOf (ac[i]));
                            highest = std::max (highest, dbOf (ac[i]));
                        }
                        series.push_back (a);
                        series.push_back (m);
                        ++colour;
                    }
                }
            expectLessThan (worst.low, 0.05, name);
            expectLessThan (worst.high, 1.0, name);
            expectLessThan (worst.phase, 0.5, name);
            expectLessThan (analyticWorst, analyticTolerance (mode), name);

            // The 5-20 kHz residual is the bilinear transform's frequency warping at 192 kHz, which a
            // circuit discretized at 8x has a quarter of.
            const auto& acHigh = table.responses.at (acKey (1.0, 1.0));
            const auto at4 = compare (table.frequencies, smallSignal (table.frequencies, [&] (const std::vector<float>& x) { return renderOverdrive (overdriveSettings (mode, 1.0f, 1.0f, 4), x); }), acHigh);
            const auto at8 = compare (table.frequencies, smallSignal (table.frequencies, [&] (const std::vector<float>& x) { return renderOverdrive (overdriveSettings (mode, 1.0f, 1.0f, 8), x); }, 8), acHigh);
            expectLessThan (at8.high, 0.5 * at4.high);
            logMessage ("  -> " + modeName (mode) + ", 9 settings x 120 frequencies: model within " + juce::String (worst.low, 4) + " dB and "
                        + juce::String (worst.phase, 3) + " degrees of AC analysis to 5 kHz, " + juce::String (worst.high, 3)
                        + " dB from 5 to 20 kHz (bilinear warping at 192 kHz: at full drive and tone, " + juce::String (at4.high, 3) + " dB at 4x, " + juce::String (at8.high, 3)
                        + " dB at 8x); the C++ analytic response agrees with AC analysis to " + juce::String (analyticWorst, 9) + " dB");

            PlotOptions o;
            o.title = modeName (mode) + " small-signal response at drive 0.5, tone 0 / 0.5 / 1 (thick: circuit AC analysis, dashed: model)";
            o.xLabel = "Frequency (Hz)";
            o.yLabel = "Gain (dB)";
            o.logX = true;
            o.xMin = 20.0; o.xMax = 20000.0;
            o.yMin = std::floor (lowest / 10.0) * 10.0;
            o.yMax = std::ceil (highest / 10.0) * 10.0 + 10.0;
            const auto png = proofDir().getChildFile ("drive_" + tag + "_linear.png");
            expect (savePlot (png, o, series));
            logMessage ("  -> " + png.getFullPathName());
        }

        // Boost: Screamer against the same AC table, the linear modes against their exact responses.
        {
            const auto table = readAc ("mid_drive");
            Boost::Settings s;
            s.mode = Boost::Mode::screamer;
            const auto measured = smallSignal (table.frequencies, [&] (const std::vector<float>& x) { return renderBoost (s, x); });
            const auto d = compare (table.frequencies, measured, table.responses.at (acKey (0.0, 0.5)));
            expectLessThan (d.low, 0.05);
            expectLessThan (d.high, 1.0);
            logMessage ("  -> Boost Screamer: within " + juce::String (d.low, 4) + " dB of the Mid Drive circuit at drive 0, tone noon to 5 kHz, "
                        + juce::String (d.high, 3) + " dB to 20 kHz");

            double cleanWorst = 0.0, tightWorst = 0.0;
            for (const auto tilt : { -12.0f, -6.0f, 6.0f, 12.0f })
            {
                Boost::Settings c;
                c.mode = Boost::Mode::clean;
                c.tiltDb = tilt;
                const auto got = smallSignal (table.frequencies, [&] (const std::vector<float>& x) { return renderBoost (c, x); }, 1);
                for (size_t i = 0; i < table.frequencies.size(); ++i)
                    cleanWorst = std::max (cleanWorst, std::abs (dbOf (got[i]) - dbOf (Boost::cleanResponse (tilt, table.frequencies[i], fs))));
            }
            for (const auto& [hz, mid] : std::vector<std::pair<float, float>> { { 80.0f, 0.0f }, { 150.0f, 6.0f }, { 400.0f, 12.0f } })
            {
                Boost::Settings t;
                t.mode = Boost::Mode::tight;
                t.tightHz = hz;
                t.midDb = mid;
                const auto got = smallSignal (table.frequencies, [&] (const std::vector<float>& x) { return renderBoost (t, x); }, 1);
                for (size_t i = 0; i < table.frequencies.size(); ++i)
                    tightWorst = std::max (tightWorst, std::abs (dbOf (got[i]) - dbOf (Boost::tightResponse (hz, mid, table.frequencies[i], fs))));
            }
            expectLessThan (cleanWorst, 1.0e-3);
            expectLessThan (tightWorst, 1.0e-3);
            const auto atLow = dbOf (Boost::cleanResponse (12.0, 20.0, fs)), atHigh = dbOf (Boost::cleanResponse (12.0, 20000.0, fs));
            logMessage ("  -> Boost Clean tilt (-12, -6, +6, +12 dB) and Tight (80 Hz flat, 150 Hz +6 dB mid, 400 Hz +12 dB): within " + juce::String (cleanWorst, 6)
                        + " and " + juce::String (tightWorst, 6) + " dB of their exact responses; +12 dB of tilt is " + juce::String (atLow, 2) + " dB at 20 Hz, "
                        + juce::String (dbOf (Boost::cleanResponse (12.0, 1000.0, fs)), 2) + " dB at 1 kHz, " + juce::String (atHigh, 2) + " dB at 20 kHz");
        }
    }

    void aliasing()
    {
        beginTest ("aliasing: a stepped sweep of tones at full drive, inharmonic energy at 1x / 4x / 8x (limit at 4x: "
                   + juce::String (aliasingLimitDb, 0) + " dB up to 1.3 kHz, the top of a 24-fret guitar)");
        // Tones on odd FFT bins (8192 points), so no alias can land on a harmonic: a tone that divides the
        // oversampled rate (3 kHz, 12 kHz) would fold every harmonic back onto another and hide it.
        const auto binHz = fs / 8192.0;
        std::vector<double> tones;
        for (const auto f : { 500.0, 1000.0, 1300.0, 2000.0, 3000.0, 5000.0, 7000.0, 10000.0 })
        {
            auto k = (int) std::round (f / binHz);
            if (k % 2 == 0)
                ++k;
            tones.push_back (k * binHz);
        }

        struct Subject
        {
            juce::String name, tag;
            double limit;
            std::function<std::vector<float> (const std::vector<float>&, int)> render;
        };
        std::vector<Subject> subjects;
        for (const auto& info : overdriveModes())
        {
            const auto mode = info.mode;
            subjects.push_back ({ info.name, info.tag, aliasingLimit (mode),
                                  [mode] (const std::vector<float>& x, int f) { return renderOverdrive (overdriveSettings (mode, 1.0f, 0.5f, f), x); } });
        }
        subjects.push_back ({ "Boost Screamer", "screamer", aliasingLimitDb, [] (const std::vector<float>& x, int f)
                              {
                                  Boost::Settings s;
                                  s.mode = Boost::Mode::screamer;
                                  s.oversampling = f;
                                  return renderBoost (s, x);
                              } });

        std::vector<PlotSeries> curves;
        int colour = 0;
        for (const auto& subject : subjects)
        {
            std::map<int, std::vector<double>> levels; // factor -> per tone
            for (const auto factor : { 1, 4, 8 })
                for (const auto f : tones)
                    levels[factor].push_back (inharmonicDb (toDouble (subject.render (sineWave (f, 0.25, (int) (0.6 * fs)), factor)), f));

            juce::String table;
            double worstGuitarRange = -400.0, leastGain4 = 400.0, leastGain8 = 400.0;
            for (size_t i = 0; i < tones.size(); ++i)
            {
                const auto a1 = levels[1][i], a4 = levels[4][i], a8 = levels[8][i];
                if (tones[i] <= 1310.0)
                    worstGuitarRange = std::max (worstGuitarRange, a4);
                if (tones[i] >= 990.0)
                {
                    leastGain4 = std::min (leastGain4, a1 - a4);
                    leastGain8 = std::min (leastGain8, a4 - a8);
                }
                table << juce::String (tones[i] / 1000.0, 2) << " kHz " << juce::String (a1, 1) << " / " << juce::String (a4, 1) << " / " << juce::String (a8, 1)
                      << (i + 1 < tones.size() ? "; " : "");
            }
            expectLessThan (worstGuitarRange, subject.limit, subject.name);
            expectGreaterThan (leastGain4, 10.0, subject.name);
            expectGreaterThan (leastGain8, 3.0, subject.name);
            logMessage ("  -> " + subject.name + " (1x / 4x / 8x dB): " + table);
            logMessage ("  -> " + subject.name + ": worst at 4x up to 1.3 kHz " + juce::String (worstGuitarRange, 1) + " dB (limit " + juce::String (subject.limit, 0)
                        + " dB); from 1 kHz up, 4x is at least "
                        + juce::String (leastGain4, 1) + " dB below 1x and 8x at least " + juce::String (leastGain8, 1) + " dB below 4x");

            for (const auto factor : { 4, 8 })
            {
                PlotSeries s { subject.name + " " + juce::String (factor) + "x", {}, {}, plotColour (colour), factor == 4 ? 2.5f : 1.5f, factor == 8 };
                for (size_t i = 0; i < tones.size(); ++i)
                {
                    s.x.push_back (tones[i]);
                    s.y.push_back (levels[factor][i]);
                }
                curves.push_back (s);
            }
            ++colour;

            // One tone's spectrum at each factor.
            if (subject.tag != "screamer")
            {
                std::vector<PlotSeries> series;
                int c = 0;
                const auto f = tones[6];
                for (const auto factor : { 1, 4, 8 })
                {
                    const auto p = powerSpectrum (toDouble (subject.render (sineWave (f, 0.25, (int) (0.6 * fs)), factor)), 13);
                    double peak = 0.0;
                    for (const auto v : p)
                        peak = std::max (peak, v);
                    PlotSeries s { juce::String (factor) + "x", {}, {}, plotColour (c++), factor == 4 ? 2.0f : 1.2f };
                    for (size_t k = 1; k < p.size(); ++k)
                    {
                        s.x.push_back ((double) k * binHz);
                        s.y.push_back (std::max (-160.0, 10.0 * std::log10 (p[k] / peak)));
                    }
                    series.push_back (s);
                }
                PlotOptions o;
                o.title = subject.name + ", full drive, " + juce::String (f / 1000.0, 2) + " kHz at -12 dBFS: everything between the harmonics is aliasing";
                o.xLabel = "Frequency (Hz)";
                o.yLabel = "Level (dB re. fundamental)";
                o.xMin = 0.0; o.xMax = 24000.0; o.yMin = -160.0; o.yMax = 5.0;
                const auto png = proofDir().getChildFile ("drive_" + subject.tag + "_aliasing.png");
                expect (savePlot (png, o, series));
                logMessage ("  -> " + png.getFullPathName());
            }
        }

        PlotOptions o;
        o.title = "Aliasing (inharmonic energy) for full-drive tones at -12 dBFS, solid 4x, dashed 8x";
        o.xLabel = "Tone frequency (Hz)";
        o.yLabel = "Inharmonic energy (dB re. total)";
        o.logX = true;
        o.xMin = 400.0; o.xMax = 12000.0; o.yMin = -120.0; o.yMax = 0.0;
        const auto png = proofDir().getChildFile ("drive_aliasing_vs_frequency.png");
        expect (savePlot (png, o, curves));
        logMessage ("  -> " + png.getFullPathName());
    }

    void noDc()
    {
        beginTest ("DC: an asymmetric input with a DC offset leaves no DC at the output");
        // 100 Hz + 200 Hz in phase (lopsided peaks, which clipping rectifies) plus a 0.05 DC offset.
        std::vector<float> x ((size_t) (3.0 * fs));
        for (size_t n = 0; n < x.size(); ++n)
        {
            const auto t = (double) n / fs;
            x[n] = (float) (0.05 + 0.25 * std::sin (2.0 * pi * 100.0 * t) + 0.2 * std::sin (2.0 * pi * 200.0 * t));
        }
        const auto meanOfLastSecond = [] (const std::vector<float>& y)
        {
            double sum = 0.0;
            for (size_t n = y.size() - (size_t) fs; n < y.size(); ++n)
                sum += y[n];
            return sum / fs;
        };

        juce::StringArray results;
        double worst = 0.0;
        for (const auto& info : overdriveModes())
            for (const auto drive : { 0.0f, 1.0f })
            {
                const auto m = meanOfLastSecond (renderOverdrive (overdriveSettings (info.mode, drive, 0.5f), x));
                worst = std::max (worst, std::abs (m));
                results.add (juce::String (info.name) + " at drive " + juce::String ((int) drive) + " " + juce::String (toDb (std::abs (m)), 1) + " dBFS");
            }
        for (const auto mode : { Boost::Mode::screamer, Boost::Mode::tight })
        {
            Boost::Settings s;
            s.mode = mode;
            const auto m = meanOfLastSecond (renderBoost (s, x));
            worst = std::max (worst, std::abs (m));
            results.add (juce::String (mode == Boost::Mode::screamer ? "Boost Screamer " : "Boost Tight ") + juce::String (toDb (std::abs (m)), 1) + " dBFS");
        }
        expectLessThan (worst, 1.0e-5);
        logMessage ("  -> mean of the last second, input mean +0.05 (-26 dBFS of DC): " + results.joinIntoString (", ")
                    + ". (Boost Clean passes its input untouched, DC included, by design.)");
    }

    void transparency()
    {
        beginTest ("transparency: Boost Clean at unity gain is bit-transparent, at any buffer size and after moving the knobs back");
        auto x = guitarDI ((int) fs);
        const auto noise = whiteNoise ((int) fs, 0.5f, 7);
        x.insert (x.end(), noise.begin(), noise.end());

        int exactRuns = 0;
        for (const auto bufferSize : { 1, 7, 64, 128, 512 })
        {
            const auto y = renderBoost (Boost::Settings {}, x, bufferSize);
            exactRuns += y == x ? 1 : 0;
        }
        expectEquals (exactRuns, 5);

        // Turn level and tilt away, switch to Tight and back, return everything: bit-exact again once settled.
        Boost b;
        Boost::Settings s;
        b.setSettings (s);
        b.prepare (fs, blockSize);
        const auto y = run (b, x, blockSize, [&] (size_t start)
        {
            const auto t = (double) start / fs;
            s = Boost::Settings {};
            if (t >= 0.1 && t < 0.3) { s.levelDb = 6.0f; s.tiltDb = 5.0f; }
            if (t >= 0.3 && t < 0.5) s.mode = Boost::Mode::tight;
            b.setSettings (s);
        });
        const auto settledFrom = (size_t) (0.65 * fs);
        const bool settledExact = std::equal (y.begin() + (long) settledFrom, y.end(), x.begin() + (long) settledFrom);
        expect (settledExact);
        logMessage ("  -> 2 s (guitar DI and white noise) at buffer sizes 1, 7, 64, 128, 512: " + juce::String (exactRuns)
                    + " of 5 bit-identical; after +6 dB level, +5 dB tilt, and a trip to Tight mode and back: "
                    + (settledExact ? "bit-identical again once the switch back (80 ms warm-up, 20 ms fade) is done" : "NOT bit-identical"));
    }

    void mixAndLevel()
    {
        beginTest ("mix and level: dry, wet, and blend land exactly where they should in every mode, and the dry is phase-aligned");
        const auto x = guitarDI ((int) fs);
        const auto expectedDry = convolve (x, chainResponse (4));
        const auto gain = std::pow (10.0, 6.0 / 20.0);
        for (const auto& info : overdriveModes())
        {
            auto s = overdriveSettings (info.mode, 0.7f, 0.5f);
            s.mix = 0.0f;
            const auto dry = renderOverdrive (s, x);
            const auto dryError = relativeErrorDb (dry, expectedDry);

            s.mix = 1.0f;
            const auto wet = renderOverdrive (s, x);
            s.mix = 0.5f;
            const auto half = renderOverdrive (s, x);
            std::vector<double> blend (x.size());
            for (size_t n = 0; n < x.size(); ++n)
                blend[n] = 0.5 * (double) dry[n] + 0.5 * (double) wet[n];
            const auto blendError = relativeErrorDb (half, blend);

            s.mix = 1.0f;
            s.levelDb = 6.0f;
            const auto louder = renderOverdrive (s, x);
            double ratioError = 0.0;
            for (size_t n = 0; n < x.size(); ++n)
                ratioError = std::max (ratioError, std::abs ((double) louder[n] - gain * (double) wet[n]));

            expectLessThan (dryError, -120.0, info.name);
            expectLessThan (blendError, -120.0, info.name);
            expectLessThan (ratioError, 1.0e-6, info.name);
            logMessage ("  -> " + juce::String (info.name) + " (drive 0.7, tone noon): mix 0 = input through the oversampler's own response to " + dB (dryError)
                        + "; mix 0.5 = (dry + wet) / 2 to " + dB (blendError) + "; +6 dB of level = x" + juce::String (gain, 4) + " to " + juce::String (ratioError, 9)
                        + "; the circuit alone is " + juce::String (toDb (rms (wet) / rms (x)), 1) + " dB re. the input on the guitar DI");
        }

        // Why the dry is aligned: at drive 0 and tiny levels the circuit is nearly linear, so a 50% blend's
        // response should be smooth. With an unaligned (zero-delay) dry it would comb-filter.
        const std::vector<double> freqs { 1000.0, 2000.0, 4000.0, 5000.0, 6000.0, 8000.0, 10000.0, 15000.0, 20000.0 };
        auto lin = overdriveSettings (Overdrive::Mode::midDrive, 0.0f, 0.5f);
        lin.mix = 0.5f;
        lin.levelDb = 0.0f;
        const auto blended = smallSignal (freqs, [&] (const std::vector<float>& in) { return renderOverdrive (lin, in); });
        lin.mix = 1.0f;
        const auto wetOnly = smallSignal (freqs, [&] (const std::vector<float>& in) { return renderOverdrive (lin, in); });
        const auto chain = chainResponse (4);
        double alignedError = 0.0, worstNotch = 0.0;
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const auto expected = 0.5 * (wetOnly[i] + 1.0); // both relative to the chain: dry is exactly the chain
            alignedError = std::max (alignedError, std::abs (dbOf (blended[i]) - dbOf (expected)));
            const auto unaligned = 0.5 * (wetOnly[i] * dtft (chain, freqs[i]) + 1.0);
            worstNotch = std::min (worstNotch, dbOf (unaligned) - dbOf (expected));
        }
        expectLessThan (alignedError, 0.01);
        logMessage ("  -> 50% blend at drive 0 (Mid Drive): matches (wet + dry) / 2 within " + juce::String (alignedError, 4) + " dB from 1 to 20 kHz; an unaligned dry would dip by "
                    + juce::String (-worstNotch, 1) + " dB in that range");
    }

    void noClicks()
    {
        beginTest ("no clicks: mode switches, drive and tone sweeps, and oversampling changes");
        const auto x = guitarDI ((int) (2.0 * fs));
        const auto steady = [&] (const std::vector<float>& y, double from, double to) { return maxStep (y, (size_t) (from * fs), (size_t) (to * fs)); };

        // Mode switch, against an ideal crossfade of two circuits that ran the whole time.
        {
            auto s = overdriveSettings (Overdrive::Mode::midDrive, 0.6f, 0.5f);
            const auto a = renderOverdrive (s, x);
            s.mode = Overdrive::Mode::distortion;
            const auto bOut = renderOverdrive (s, x);
            const auto switchAt = (size_t) (1.0 * fs) / blockSize * blockSize;

            Overdrive o;
            auto live = overdriveSettings (Overdrive::Mode::midDrive, 0.6f, 0.5f);
            o.setSettings (live);
            o.prepare (fs, blockSize);
            const auto y = run (o, x, blockSize, [&] (size_t start)
            {
                if (start == switchAt)
                {
                    live.mode = Overdrive::Mode::distortion;
                    o.setSettings (live);
                }
            });

            const auto fade = (size_t) (DriveEngine::switchSeconds * fs);
            const auto fadeAt = switchAt + (size_t) std::round (DriveEngine::warmupSeconds * fs); // after the warm-up
            double deviation = 0.0, peak = 0.0;
            for (size_t n = switchAt; n < fadeAt + fade + 2400; ++n)
            {
                const auto p = n < fadeAt ? 0.0 : std::min (1.0, (double) (n - fadeAt) / (double) fade);
                const auto ideal = std::cos (0.5 * pi * p) * a[n] + std::sin (0.5 * pi * p) * bOut[n];
                deviation = std::max (deviation, std::abs (y[n] - ideal));
                peak = std::max (peak, std::abs (ideal));
            }
            const auto during = maxStep (y, switchAt, fadeAt + fade + 480);
            const auto limit = std::max (steady (a, 0.5, 1.0), steady (bOut, 1.2, 1.7));
            expectLessThan (during, limit * 1.05);
            expectLessThan (toDb (deviation / peak), -30.0);
            logMessage ("  -> Mid Drive -> Distortion while playing (80 ms warm-up, then a 20 ms crossfade): largest step " + juce::String (during, 4)
                        + " (steady playing: up to " + juce::String (limit, 4) + "); differs from an ideal crossfade of two always-running circuits by at most "
                        + juce::String (toDb (deviation / peak), 1) + " dB re. the peak");
        }

        // Drive and tone sweeps.
        for (const auto& info : overdriveModes())
        {
            const auto mode = info.mode;
            Overdrive o;
            auto s = overdriveSettings (mode, 0.0f, 0.5f);
            o.setSettings (s);
            o.prepare (fs, blockSize);
            const auto y = run (o, x, blockSize, [&] (size_t start)
            {
                const auto t = (double) start / fs;
                s.drive = (float) juce::jlimit (0.0, 1.0, (t - 0.5) / 0.1);  // 0 -> 1 in 100 ms
                s.tone = (float) (t < 1.2 ? 0.5 : juce::jlimit (0.0, 1.0, 0.5 - (t - 1.2) / 0.1)); // then tone 0.5 -> 0
                o.setSettings (s);
            });
            // The yardstick is the larger of the sweep's two ends playing the same passage: the steps a
            // knob sweep may make are the ones either setting makes on its own. (Full drive alone isn't
            // enough: the Transparent at drive 0 is a bright clean boost, +14 dB, whose attacks step
            // further than its clipped, low-passed full-drive sound.)
            const auto settledFull = renderOverdrive (overdriveSettings (mode, 1.0f, 0.5f), x);
            const auto settledZero = renderOverdrive (overdriveSettings (mode, 0.0f, 0.5f), x);
            const auto during = steady (y, 0.5, 0.7), toneSweep = steady (y, 1.2, 1.4);
            const auto limit = std::max (steady (settledFull, 0.5, 1.5), steady (settledZero, 0.5, 0.7));
            expectLessThan (during, limit * 1.05, info.name);
            expectLessThan (toneSweep, limit * 1.05, info.name);
            logMessage ("  -> " + modeName (mode) + ": drive 0 -> 1 in 100 ms, largest step " + juce::String (during, 4) + "; tone 0.5 -> 0 in 100 ms, "
                        + juce::String (toneSweep, 4) + " (steady: " + juce::String (steady (settledFull, 0.5, 1.5), 4) + " at full drive, "
                        + juce::String (steady (settledZero, 0.5, 0.7), 4) + " at drive 0 over the same passage)");
        }

        // Oversampling 4x -> 8x -> 4x while playing: a 5 ms dip to silence and back.
        {
            Overdrive o;
            auto s = overdriveSettings (Overdrive::Mode::distortion, 0.6f, 0.5f);
            o.setSettings (s);
            o.prepare (fs, blockSize);
            const auto y = run (o, x, blockSize, [&] (size_t start)
            {
                const auto t = (double) start / fs;
                s.oversampling = (t >= 0.6 && t < 1.2) ? 8 : 4;
                o.setSettings (s);
            });
            const auto around = std::max (steady (y, 0.6, 0.65), steady (y, 1.2, 1.25));
            const auto playing = steady (y, 0.3, 0.6);
            expectLessThan (around, playing * 1.05);
            logMessage ("  -> 4x -> 8x -> 4x while playing: largest step around the changes " + juce::String (around, 4) + " (steady " + juce::String (playing, 4) + ")");
        }

        // Boost: Clean -> Tight -> Screamer -> Clean.
        {
            Boost b;
            Boost::Settings s;
            s.levelDb = 3.0f;
            b.setSettings (s);
            b.prepare (fs, blockSize);
            const auto y = run (b, x, blockSize, [&] (size_t start)
            {
                const auto t = (double) start / fs;
                s.mode = t < 0.5 ? Boost::Mode::clean : t < 1.0 ? Boost::Mode::tight : t < 1.5 ? Boost::Mode::screamer : Boost::Mode::clean;
                b.setSettings (s);
            });
            // The ideal: each mode rendered on its own the whole time, crossfaded with the block's timing
            // (requests at block starts, 80 ms warm-up, then 20 ms ramps with sin(pi/2 p) each).
            std::array<std::vector<float>, Boost::numModes> alone;
            for (int m = 0; m < Boost::numModes; ++m)
            {
                auto only = s;
                only.mode = (Boost::Mode) m;
                alone[(size_t) m] = renderBoost (only, x);
            }
            const auto warmup = (size_t) std::round (DriveEngine::warmupSeconds * fs), fade = (size_t) std::round (Boost::switchSeconds * fs);
            const auto blockStart = [] (double t) { return (size_t) std::ceil (t * fs / blockSize) * (size_t) blockSize; };
            const std::vector<std::pair<size_t, int>> switches { { blockStart (0.5) + warmup, 1 }, { blockStart (1.0) + warmup, 2 }, { blockStart (1.5) + warmup, 0 } };
            std::vector<double> ideal (x.size());
            std::array<double, Boost::numModes> p { 1.0, 0.0, 0.0 };
            int heading = 0;
            size_t next = 0;
            for (size_t n = 0; n < x.size(); ++n)
            {
                if (next < switches.size() && n == switches[next].first)
                    heading = switches[next++].second;
                double sum = 0.0;
                for (int m = 0; m < Boost::numModes; ++m)
                {
                    auto& pm = p[(size_t) m];
                    pm = m == heading ? std::min (1.0, pm + 1.0 / (double) fade) : std::max (0.0, pm - 1.0 / (double) fade);
                    sum += std::sin (0.5 * pi * pm) * alone[(size_t) m][n];
                }
                ideal[n] = sum;
            }
            double deviation = 0.0, peak = 0.0, worstRatio = 0.0;
            for (size_t n = 0; n < x.size(); ++n)
            {
                deviation = std::max (deviation, std::abs ((double) y[n] - ideal[n]));
                peak = std::max (peak, std::abs (ideal[n]));
            }
            for (const auto& [fadeAt, mode] : switches)
            {
                const auto from = fadeAt - warmup, to = fadeAt + fade + 480;
                double idealStep = 0.0;
                for (size_t n = from + 1; n < to; ++n)
                    idealStep = std::max (idealStep, std::abs (ideal[n] - ideal[n - 1]));
                worstRatio = std::max (worstRatio, maxStep (y, from, to) / idealStep);
            }
            expectLessThan (worstRatio, 1.05);
            expectLessThan (toDb (deviation / peak), -30.0);
            logMessage ("  -> Boost Clean -> Tight -> Screamer -> Clean: within " + juce::String (toDb (deviation / peak), 1)
                        + " dB (re. the peak) of an ideal crossfade of the modes running on their own; largest step in each switch at most "
                        + juce::String (worstRatio, 3) + " x the ideal's");
        }
    }

    /// A walk through the modes that switches into and out of each newer mode (Transparent, Fuzz) from
    /// and to every other mode: each ordered pair that involves one of them appears once.
    static std::vector<Overdrive::Mode> switchPath()
    {
        using M = Overdrive::Mode;
        return { M::midDrive, M::transparent, M::distortion, M::transparent, M::midDrive };
    }

    void modeSwitching()
    {
        beginTest ("mode switching: into and out of each newer mode from and to every other one while playing, against an ideal crossfade of "
                   "the modes running on their own");
        const auto path = switchPath();
        constexpr double segment = 0.3;
        const auto x = guitarDI ((int) ((segment * (double) path.size() + 0.2) * fs));

        // Each mode alone, the whole time.
        std::map<int, std::vector<float>> alone;
        for (const auto m : path)
            if (alone.count ((int) m) == 0)
                alone[(int) m] = renderOverdrive (overdriveSettings (m, 0.6f, 0.5f), x);

        // The live run: each switch requested at the first block start of its segment.
        const auto blockStart = [] (double t) { return (size_t) std::ceil (t * fs / blockSize) * (size_t) blockSize; };
        std::vector<size_t> requests;
        for (size_t k = 1; k < path.size(); ++k)
            requests.push_back (blockStart (segment * (double) k));
        Overdrive o;
        auto live = overdriveSettings (path[0], 0.6f, 0.5f);
        o.setSettings (live);
        o.prepare (fs, blockSize);
        const auto y = run (o, x, blockSize, [&] (size_t start)
        {
            for (size_t k = 0; k < requests.size(); ++k)
                if (start == requests[k])
                {
                    live.mode = path[k + 1];
                    o.setSettings (live);
                }
        });

        // The ideal, with the block's timing: 80 ms of warm-up after each request, then 20 ms ramps
        // contributing sin(pi/2 p) each.
        const auto warmup = (size_t) std::round (DriveEngine::warmupSeconds * fs);
        const auto step = 1.0 / (DriveEngine::switchSeconds * fs);
        std::map<int, double> position;
        for (const auto& [m, signal] : alone)
            position[m] = m == (int) path[0] ? 1.0 : 0.0;
        int heading = (int) path[0];
        size_t next = 0;
        std::vector<double> ideal (x.size());
        for (size_t n = 0; n < x.size(); ++n)
        {
            if (next < requests.size() && n == requests[next] + warmup)
                heading = (int) path[++next];
            double sum = 0.0;
            for (auto& [m, p] : position)
            {
                p = m == heading ? std::min (1.0, p + step) : std::max (0.0, p - step);
                sum += std::sin (0.5 * pi * p) * alone[m][n];
            }
            ideal[n] = sum;
        }

        double peak = 0.0;
        for (const auto v : ideal)
            peak = std::max (peak, std::abs (v));
        juce::StringArray lines;
        double worstDeviation = -400.0, worstRatio = 0.0;
        for (size_t k = 0; k < requests.size(); ++k)
        {
            const auto from = requests[k], to = requests[k] + warmup + (size_t) (DriveEngine::switchSeconds * fs) + 480;
            double deviation = 0.0, idealStep = 0.0;
            for (size_t n = from; n < to; ++n)
            {
                deviation = std::max (deviation, std::abs ((double) y[n] - ideal[n]));
                if (n > from)
                    idealStep = std::max (idealStep, std::abs (ideal[n] - ideal[n - 1]));
            }
            const auto ratio = maxStep (y, from, to) / idealStep;
            worstDeviation = std::max (worstDeviation, toDb (deviation / peak));
            worstRatio = std::max (worstRatio, ratio);
            lines.add (modeName (path[k]) + " -> " + modeName (path[k + 1]) + " " + juce::String (toDb (deviation / peak), 1) + " dB, step x" + juce::String (ratio, 3));
        }
        expectLessThan (worstDeviation, -30.0);
        expectLessThan (worstRatio, 1.05);
        logMessage ("  -> each switch's largest difference from the ideal crossfade (dB re. the peak) and largest step against the ideal's: " + lines.joinIntoString ("; "));
        logMessage ("  -> worst " + juce::String (worstDeviation, 1) + " dB, largest step x" + juce::String (worstRatio, 3) + " the ideal's");
    }

    void realTime()
    {
        beginTest ("real time: every setting, mode, and oversampling change allocates and locks nothing");
        const auto x = guitarDI ((int) (6.0 * fs));
        rtcheck::Counts total;
        int blocks = 0;

        Overdrive o;
        auto os = overdriveSettings (Overdrive::Mode::midDrive, 0.5f, 0.5f);
        o.setSettings (os);
        o.prepare (fs, blockSize);
        Boost b;
        Boost::Settings bs;
        b.setSettings (bs);
        b.prepare (fs, blockSize);

        std::vector<float> buffer ((size_t) blockSize), buffer2 ((size_t) blockSize);
        for (size_t start = 0; start + blockSize <= x.size(); start += blockSize, ++blocks)
        {
            switch (blocks % 400)
            {
                case 20:  os.mode = Overdrive::Mode::distortion; bs.mode = Boost::Mode::tight; break;
                case 40:  os.drive = 1.0f; os.tone = 0.1f; bs.tightHz = 400.0f; bs.midDb = 12.0f; break;
                case 60:  os.mix = 0.4f; os.levelDb = -6.0f; bs.mode = Boost::Mode::screamer; break;
                case 80:  os.tightHz = 300.0f; os.oversampling = 8; bs.oversampling = 8; break;
                case 100: os.mode = Overdrive::Mode::transparent; break;
                case 110: os.drive = 0.3f; os.tone = 0.9f; break;
                case 120: os.mode = Overdrive::Mode::midDrive; os.drive = 0.2f; bs.mode = Boost::Mode::clean; bs.tiltDb = 6.0f; break;
                case 140: os.mode = Overdrive::Mode::distortion; os.oversampling = 4; bs.oversampling = 4; bs.levelDb = 10.0f; break;
                case 160: os.mode = Overdrive::Mode::midDrive; os.mix = 1.0f; os.tightHz = 20.0f; bs.mode = Boost::Mode::screamer; break;
                case 180: os.mode = Overdrive::Mode::transparent; break;
                case 190: os.drive = 1.0f; os.tone = 0.0f; break;
                case 200: os = overdriveSettings (Overdrive::Mode::midDrive, 0.5f, 0.5f); bs = Boost::Settings {}; break;
                default: break;
            }
            std::copy (x.begin() + (long) start, x.begin() + (long) start + blockSize, buffer.begin());
            std::copy (x.begin() + (long) start, x.begin() + (long) start + blockSize, buffer2.begin());
            float* c1[1] = { buffer.data() };
            float* c2[1] = { buffer2.data() };
            juce::ScopedNoDenormals noDenormals;
            rtcheck::begin();
            o.setSettings (os);
            o.process (juce::dsp::AudioBlock<float> (c1, 1, (size_t) blockSize), {});
            b.setSettings (bs);
            b.process (juce::dsp::AudioBlock<float> (c2, 1, (size_t) blockSize), {});
            total += rtcheck::end();
        }
        expectEquals (total.allocations, 0L);
        expectEquals (total.frees, 0L);
        expectEquals (total.blockingLocks, 0L);
        logMessage ("  -> " + juce::String (blocks) + " blocks through both blocks with every mode, knob, mix, tight, and 4x/8x changing: "
                    + juce::String (total.allocations) + " allocations, " + juce::String (total.frees) + " frees, " + juce::String (total.blockingLocks) + " locks");
    }

    void cpu()
    {
        beginTest ("CPU: each mode at 4x and 8x, 128-sample buffers of the guitar DI");
        const auto x = guitarDI ((int) (4.0 * fs));
        const auto measure = [&] (auto& block)
        {
            std::vector<float> buffer ((size_t) blockSize);
            std::vector<double> micros;
            for (size_t start = 0; start + blockSize <= x.size(); start += blockSize)
            {
                std::copy (x.begin() + (long) start, x.begin() + (long) start + blockSize, buffer.begin());
                float* c[1] = { buffer.data() };
                juce::ScopedNoDenormals noDenormals;
                const auto t0 = std::chrono::steady_clock::now();
                block.process (juce::dsp::AudioBlock<float> (c, 1, (size_t) blockSize), {});
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            std::sort (micros.begin(), micros.end());
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            return std::pair<double, double> { mean, micros[(size_t) (0.99 * (double) micros.size())] };
        };

        juce::StringArray results;
        double worstMean = 0.0;
        for (const auto& info : overdriveModes())
            for (const auto factor : { 4, 8 })
            {
                Overdrive o;
                o.setSettings (overdriveSettings (info.mode, 0.8f, 0.5f, factor));
                o.prepare (fs, blockSize);
                const auto [mean, p99] = measure (o);
                worstMean = std::max (worstMean, factor == 4 ? mean : 0.0);
                results.add (juce::String (info.name) + " " + juce::String (factor) + "x: " + juce::String (mean, 1) + " us (" + juce::String (100.0 * mean / deadlineMicros, 2)
                             + "%, p99 " + juce::String (p99, 1) + " us)");
            }
        for (const auto& [mode, factor] : std::vector<std::pair<Boost::Mode, int>> { { Boost::Mode::clean, 4 }, { Boost::Mode::tight, 4 }, { Boost::Mode::screamer, 4 }, { Boost::Mode::screamer, 8 } })
        {
            Boost b;
            Boost::Settings s;
            s.mode = mode;
            s.tiltDb = 3.0f;
            s.oversampling = factor;
            b.setSettings (s);
            b.prepare (fs, blockSize);
            const auto [mean, p99] = measure (b);
            const auto name = mode == Boost::Mode::clean ? "Boost Clean" : mode == Boost::Mode::tight ? "Boost Tight" : "Boost Screamer";
            results.add (juce::String (name) + (mode == Boost::Mode::screamer ? " " + juce::String (factor) + "x" : juce::String()) + ": " + juce::String (mean, 1) + " us ("
                         + juce::String (100.0 * mean / deadlineMicros, 2) + "%)");
        }
        expectLessThan (worstMean, 0.05 * deadlineMicros);
        logMessage ("  -> mean per 128-sample block (% of the 2.67 ms deadline): " + results.joinIntoString ("; "));
    }
};

DriveTests driveTests;
} // namespace

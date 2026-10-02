#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Bitcrush.h"
#include "dsp/Bloom.h"
#include "dsp/Flanger.h"
#include "dsp/Phaser.h"
#include "dsp/Svf.h"

#include <chrono>
#include <complex>
#include <set>

namespace
{
using namespace testing;
using ampsim::Bitcrush;
using ampsim::Bloom;
using ampsim::Flanger;
using ampsim::Lfo;
using ampsim::Phaser;

constexpr double pi = juce::MathConstants<double>::pi;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/bloom").getChildFile (name);
}

std::vector<float> channel (const juce::AudioBuffer<float>& b, int ch)
{
    return { b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples() };
}

/// Runs a stereo signal through a block in fixed-size buffers. `before` runs ahead of each buffer with its
/// first sample's index, to change settings mid-stream.
Stereo run (ampsim::Block& block, const std::vector<float>& left, const std::vector<float>& right, int bufferSize = blockSize,
            const std::function<void (size_t)>& before = {})
{
    Stereo out { left, right };
    for (size_t start = 0; start < left.size(); start += (size_t) bufferSize)
    {
        if (before)
            before (start);
        const auto len = std::min ((size_t) bufferSize, left.size() - start);
        float* channels[2] = { out.left.data() + start, out.right.data() + start };
        block.process (juce::dsp::AudioBlock<float> (channels, 2, len), {});
    }
    return out;
}

Stereo run (ampsim::Block& block, const std::vector<float>& mono, int bufferSize = blockSize, const std::function<void (size_t)>& before = {})
{
    return run (block, mono, mono, bufferSize, before);
}

/// The block's response (left channel) to an impulse of the given amplitude, divided by it.
std::vector<double> impulseResponse (ampsim::Block& block, int length, double amplitude = 1.0)
{
    std::vector<float> x ((size_t) length, 0.0f);
    x[0] = (float) amplitude;
    const auto y = run (block, x).left;
    std::vector<double> h ((size_t) length);
    for (size_t n = 0; n < h.size(); ++n)
        h[n] = (double) y[n] / amplitude;
    return h;
}

/// H(f) of an impulse response, by direct DTFT at any frequency (a recursive phasor, re-anchored every 1024
/// samples so it doesn't drift).
std::complex<double> dtft (const std::vector<double>& h, double f)
{
    const auto w = -twoPi * f / fs;
    const auto step = std::polar (1.0, w);
    std::complex<double> sum = 0.0, z = 1.0;
    for (size_t n = 0; n < h.size(); ++n)
    {
        if ((n & 1023) == 0)
            z = std::polar (1.0, w * (double) n);
        sum += h[n] * z;
        z *= step;
    }
    return sum;
}

/// |H| on the grid of a 2^order-point FFT (zero-padded), bins 0 to N/2.
std::vector<double> fftMagnitude (const std::vector<double>& h, int order)
{
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) (2 * size), 0.0f);
    for (size_t n = 0; n < std::min (h.size(), (size_t) size); ++n)
        data[n] = (float) h[n];
    fft.performRealOnlyForwardTransform (data.data(), true);
    std::vector<double> magnitude ((size_t) (size / 2 + 1));
    for (int k = 0; k <= size / 2; ++k)
        magnitude[(size_t) k] = std::hypot ((double) data[(size_t) (2 * k)], (double) data[(size_t) (2 * k + 1)]);
    return magnitude;
}

/// The frequency of the minimum of |H| between a and b, by golden-section search on the DTFT.
double refineMinimum (const std::vector<double>& h, double a, double b)
{
    const auto ratio = 0.5 * (std::sqrt (5.0) - 1.0);
    auto c = b - ratio * (b - a), d = a + ratio * (b - a);
    auto fc = std::abs (dtft (h, c)), fd = std::abs (dtft (h, d));
    for (int i = 0; i < 60; ++i)
    {
        if (fc < fd)
        {
            b = d; d = c; fd = fc;
            c = b - ratio * (b - a);
            fc = std::abs (dtft (h, c));
        }
        else
        {
            a = c; c = d; fc = fd;
            d = a + ratio * (b - a);
            fd = std::abs (dtft (h, d));
        }
    }
    return 0.5 * (a + b);
}

/// Every local minimum of |H| deeper than belowDb between fMin and fMax: found on a 131072-point FFT grid
/// (0.37 Hz bins) and refined by golden-section search. A narrow notch can sit 30 dB above its true depth at
/// the nearest bin, so the grid threshold is loose; the refined depth is what gets reported.
std::vector<double> findNotches (const std::vector<double>& h, double belowDb, double fMin, double fMax)
{
    constexpr int order = 17;
    const auto magnitude = fftMagnitude (h, order);
    const auto binHz = fs / (double) (1 << order);
    const auto threshold = std::pow (10.0, belowDb / 20.0);
    std::vector<double> notches;
    for (size_t k = 1; k + 1 < magnitude.size(); ++k)
    {
        const auto f = (double) k * binHz;
        if (f < fMin || f > fMax)
            continue;
        if (magnitude[k] < threshold && magnitude[k] < magnitude[k - 1] && magnitude[k] <= magnitude[k + 1])
            notches.push_back (refineMinimum (h, f - binHz, f + binHz));
    }
    return notches;
}

/// The amplitude of y's component at f over `length` samples from `start` (a whole number of f's cycles).
double toneAmplitude (const std::vector<float>& y, double f, size_t start, size_t length)
{
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < length; ++n)
        sum += (double) y[start + n] * std::polar (1.0, -twoPi * f * (double) n / fs);
    return 2.0 * std::abs (sum) / (double) length;
}

/// Welch-averaged power spectrum: 8192-point Hann frames with 50% overlap, bins 0 to 4096.
std::vector<double> powerSpectrum (const std::vector<float>& x)
{
    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), frame ((size_t) (2 * size));
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (twoPi * i / size));

    std::vector<double> power ((size_t) (size / 2 + 1), 0.0);
    int frames = 0;
    for (size_t start = 0; start + (size_t) size <= x.size(); start += (size_t) size / 2, ++frames)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = x[start + (size_t) i] * window[(size_t) i];
        fft.performRealOnlyForwardTransform (frame.data(), true);
        for (int k = 0; k <= size / 2; ++k)
        {
            const auto re = (double) frame[(size_t) (2 * k)], im = (double) frame[(size_t) (2 * k + 1)];
            power[(size_t) k] += re * re + im * im;
        }
    }
    for (auto& p : power)
        p /= std::max (1, frames);
    return power;
}

/// A 4th-order Butterworth high-pass at 5 kHz: what a click leaves behind on a low tone.
std::vector<float> highPassed (const std::vector<float>& y)
{
    std::array<ampsim::Svf, 2> highPass;
    for (int k = 0; k < 2; ++k)
    {
        const auto q = 1.0 / (2.0 * std::sin ((2.0 * k + 1.0) * pi / 8.0));
        highPass[(size_t) k].setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, 5000.0, q, 0.0, fs));
    }
    std::vector<float> residual (y.size());
    for (size_t n = 0; n < y.size(); ++n)
        residual[n] = (float) highPass[1].processSample (highPass[0].processSample (y[n]));
    return residual;
}

double peakAbs (const std::vector<float>& x, size_t from, size_t to)
{
    double p = 0.0;
    for (size_t n = from; n < std::min (to, x.size()); ++n)
        p = std::max (p, (double) std::abs (x[n]));
    return p;
}

// ---- Settings builders ------------------------------------------------------------------------

Bitcrush::Settings crush (float bits, float rateHz, bool dither = false, float toneHz = 20000.0f, float mix = 1.0f)
{
    Bitcrush::Settings s;
    s.on = true;
    s.bits = bits;
    s.rateHz = rateHz;
    s.dither = dither;
    s.toneHz = toneHz;
    s.mix = mix;
    return s;
}

/// Modern with its range collapsed to one corner: a fixed allpass chain.
Phaser::Settings staticPhaser (int stages, float cornerHz, float feedback, float mix)
{
    Phaser::Settings s;
    s.on = true;
    s.mode = Phaser::Mode::modern;
    s.stages = stages;
    s.lowHz = s.highHz = cornerHz;
    s.depth = 0.0f;
    s.feedback = feedback;
    s.stereoOffset = 0.0f;
    s.mix = mix;
    return s;
}

Flanger::Settings staticFlanger (float manualMs, bool negative, float feedback, float mix = 0.5f)
{
    Flanger::Settings s;
    s.on = true;
    s.manualMs = manualMs;
    s.depth = 0.0f;
    s.feedback = feedback;
    s.negative = negative;
    s.stereoPhase = 0.0f;
    s.mix = mix;
    return s;
}

Phaser::Settings phaserFromJson (const juce::var& v)
{
    Phaser::Settings s;
    s.on = true;
    const auto mode = v["mode"].toString();
    s.mode = mode == "vibe" ? Phaser::Mode::vibe : (mode == "classic" ? Phaser::Mode::classic : Phaser::Mode::modern);
    s.stages = (int) v["stages"];
    s.rateHz = (float) (double) v["rateHz"];
    s.depth = (float) (double) v["depth"];
    s.shape = v["shape"].toString() == "triangle" ? Lfo::Shape::triangle : Lfo::Shape::sine;
    s.lowHz = (float) (double) v["lowHz"];
    s.highHz = (float) (double) v["highHz"];
    s.feedback = (float) (double) v["feedback"];
    s.classicFeedback = (bool) v["classicFeedback"];
    s.stereoOffset = (float) (double) v["stereoOffset"];
    s.mix = (float) (double) v["mix"];
    return s;
}

/// The shape of a sweep over its last LFO cycle, in octaves: prototypes/vibe.py's sweep_metrics, ported.
struct SweepMetrics
{
    double riseFraction, riseMs, fallMs, octaves, lowHz, highHz;
};

SweepMetrics sweepMetrics (const std::vector<double>& hz, double rate)
{
    const auto period = fs / rate;
    const auto whole = (int) std::lround (period);
    std::vector<double> cycle;
    for (auto it = hz.end() - whole; it != hz.end(); ++it)
        cycle.push_back (std::log2 (*it));
    const auto lo = (int) (std::min_element (cycle.begin(), cycle.end()) - cycle.begin());
    const auto hi = (int) (std::max_element (cycle.begin(), cycle.end()) - cycle.begin());
    const auto low = cycle[(size_t) lo], high = cycle[(size_t) hi];

    const auto crossing = [&] (int start, bool rising, double level)
    {
        for (int k = 1; k < whole; ++k)
        {
            const auto a = cycle[(size_t) ((start + k - 1) % whole)], b = cycle[(size_t) ((start + k) % whole)];
            if ((rising && a < level && level <= b) || (! rising && a > level && level >= b))
                return k - 1 + (level - a) / (b - a);
        }
        return std::nan ("");
    };
    const auto ten = low + 0.1 * (high - low), ninety = low + 0.9 * (high - low);
    const auto rise = crossing (lo, true, ninety) - crossing (lo, true, ten);
    const auto fall = crossing (hi, false, ten) - crossing (hi, false, ninety);
    return { (double) (((hi - lo) % whole + whole) % whole) / period, 1000.0 * rise / fs, 1000.0 * fall / fs, high - low,
             std::exp2 (low), std::exp2 (high) };
}

class BloomTests final : public juce::UnitTest
{
public:
    BloomTests() : juce::UnitTest ("Bloom", "ampsim") {}

    void runTest() override
    {
        bitcrushLevels();
        bitcrushHold();
        bitcrushAliasing();
        bitcrushDither();
        phaserGolden();
        vibeSweep();
        phaserNotches();
        phaserSweepTracking();
        phaserAllpass();
        phaserStability();
        flangerComb();
        flangerFeedback();
        throughZero();
        containerMix();
        containerClicks();
        realtime();
        cpu();
        renders();
    }

private:
    // ---- Bitcrush ---------------------------------------------------------------------------------

    void bitcrushLevels()
    {
        beginTest ("bitcrush: exactly 2^bits output levels at integer bits, and fractional bits move the grid smoothly");

        // A slow ramp past full scale through the block: no hold (rate = fs), no dither, tone open, mix 1.
        constexpr int length = 400000;
        std::vector<float> ramp ((size_t) length);
        for (int n = 0; n < length; ++n)
            ramp[(size_t) n] = (float) (-1.25 + 2.5 * n / (length - 1));

        juce::StringArray integerCounts;
        bool exactCounts = true, onGrid = true, matchesFunction = true;
        for (int b = 1; b <= 16; ++b)
        {
            Bitcrush c;
            c.setSettings (crush ((float) b, (float) fs));
            c.prepare (fs, blockSize);
            const auto out = run (c, ramp).left;
            const std::set<float> levels (out.begin(), out.end());
            const auto step = std::exp2 (1.0 - b);
            exactCounts = exactCounts && levels.size() == ((size_t) 1 << b);
            onGrid = onGrid && juce::exactlyEqual (*levels.begin(), -1.0f) && juce::exactlyEqual (*levels.rbegin(), (float) (1.0 - step));
            for (auto v : levels)
                onGrid = onGrid && juce::exactlyEqual (std::floor ((double) v / step), (double) v / step); // a whole number of steps
            for (size_t n = 0; n < out.size(); ++n)
                matchesFunction = matchesFunction && juce::exactlyEqual (out[n], (float) Bitcrush::quantize ((double) ramp[n], (double) b));
            if (b <= 4 || b == 8 || b == 12 || b == 16)
                integerCounts.add (juce::String (b) + " bits " + juce::String ((int) levels.size()));
        }
        expect (exactCounts);
        expect (onGrid);
        expect (matchesFunction);

        // Fractional bits through the block, and the count over a fine sweep of the bits control.
        juce::StringArray fractionalCounts;
        bool between = true;
        for (const auto b : { 1.5f, 2.5f, 3.25f, 3.5f, 4.75f, 6.3f })
        {
            Bitcrush c;
            c.setSettings (crush (b, (float) fs));
            c.prepare (fs, blockSize);
            const auto out = run (c, ramp).left;
            const auto count = std::set<float> (out.begin(), out.end()).size();
            between = between && count >= ((size_t) 1 << (int) std::floor (b)) && count <= ((size_t) 1 << (int) std::ceil (b));
            fractionalCounts.add (juce::String (b, 2) + " bits " + juce::String ((int) count) + " (2^b = " + juce::String (std::exp2 ((double) b), 2) + ")");
        }
        expect (between);

        bool monotone = true;
        size_t previous = 0;
        std::vector<double> coarse (100001);
        for (size_t n = 0; n < coarse.size(); ++n)
            coarse[n] = -1.2 + 2.4 * (double) n / (double) (coarse.size() - 1);
        for (int k = 100; k <= 600; ++k)
        {
            std::set<double> levels;
            for (auto x : coarse)
                levels.insert (Bitcrush::quantize (x, k / 100.0));
            monotone = monotone && levels.size() >= previous;
            previous = levels.size();
        }
        expect (monotone);

        // Quantization noise against step^2 / 12, across integer and fractional bits: three sines at
        // unrelated frequencies (peaks below every clip point from 3 bits up), 1 s each.
        std::vector<float> busy ((size_t) fs);
        for (size_t n = 0; n < busy.size(); ++n)
        {
            const auto t = (double) n / fs;
            busy[n] = (float) (0.25 * std::sin (twoPi * 1234.567 * t) + 0.25 * std::sin (twoPi * 371.29 * t + 1.0) + 0.24 * std::sin (twoPi * 5021.3 * t + 2.0));
        }
        juce::StringArray noiseRows;
        double worstNoise = 0.0;
        PlotSeries measured { "measured error RMS", {}, {}, plotColour (0), 2.5f };
        PlotSeries theory { "step / sqrt(12)", {}, {}, plotColour (6), 1.2f, true };
        for (int quarter = 4; quarter <= 64; ++quarter)
        {
            const auto b = quarter / 4.0;
            Bitcrush c;
            c.setSettings (crush ((float) b, (float) fs));
            c.prepare (fs, blockSize);
            const auto out = run (c, busy).left;
            double sum = 0.0;
            for (size_t n = 0; n < out.size(); ++n)
                sum += ((double) out[n] - (double) busy[n]) * ((double) out[n] - (double) busy[n]);
            const auto errorRms = std::sqrt (sum / (double) out.size());
            const auto expected = Bitcrush::stepSize (b) / std::sqrt (12.0);
            measured.x.push_back (b);
            measured.y.push_back (toDb (errorRms));
            theory.x.push_back (b);
            theory.y.push_back (toDb (expected));
            if (b >= 4.0)
                worstNoise = std::max (worstNoise, std::abs (errorRms / expected - 1.0));
            if (quarter % 6 == 2 || quarter == 17 || quarter == 18)
                noiseRows.add (juce::String (b, 2) + " bits " + juce::String (errorRms / expected, 3));
        }
        expectLessThan (worstNoise, 0.03);

        PlotOptions o;
        o.title = "Bitcrush quantization noise vs bits (three sines, no hold, no dither; dashed: step / sqrt(12))";
        o.xLabel = "Bits";
        o.yLabel = "Error RMS (dBFS)";
        o.xMin = 1.0; o.xMax = 16.0; o.yMin = -110.0; o.yMax = 0.0;
        const auto png = proofDir().getChildFile ("bloom_bitcrush_noise.png");
        expect (savePlot (png, o, { measured, theory }));

        logMessage ("  -> distinct output levels of a ramp past full scale, integer bits (all 1 to 16 exact, every level a whole number of steps, "
                    "the block bit-exact with Bitcrush::quantize): " + integerCounts.joinIntoString (", "));
        logMessage ("  -> fractional bits through the block, each between 2^floor and 2^ceil: " + fractionalCounts.joinIntoString (", ")
                    + "; over bits 1.00 to 6.00 in 0.01 steps the count never falls: " + (monotone ? "true" : "false"));
        logMessage ("  -> error RMS / (step / sqrt(12)) on three sines, largest deviation from 4 bits up " + juce::String (100.0 * worstNoise, 2)
                    + "% (limit 3%): " + noiseRows.joinIntoString (", "));
        logMessage ("  -> " + png.getFullPathName());
    }

    void bitcrushHold()
    {
        beginTest ("bitcrush: the hold lasts fs / rate samples on average, alternating floor and ceil for fractional ratios");

        // A ramp whose every sample quantizes differently at 16 bits (1.23 steps apart), so each constant run in
        // the output is exactly one hold.
        std::vector<float> ramp ((size_t) fs);
        for (size_t n = 0; n < ramp.size(); ++n)
            ramp[n] = (float) (-0.9 + 1.8 * (double) n / fs);

        juce::StringArray rows;
        bool lengthsOk = true, valuesOk = true, meansOk = true;
        for (const auto rate : { 8000.0f, 7000.0f, 4410.0f, 1234.5f, 32000.0f, 100.0f })
        {
            const auto ratio = fs / (double) rate;
            Bitcrush c;
            c.setSettings (crush (16.0f, rate));
            c.prepare (fs, blockSize);
            const auto out = run (c, ramp).left;

            std::vector<int> lengths;
            size_t start = 0;
            for (size_t n = 1; n <= out.size(); ++n)
            {
                if (n == out.size() || ! juce::exactlyEqual (out[n], out[start]))
                {
                    if (n < out.size()) // the last run may be cut short
                    {
                        lengths.push_back ((int) (n - start));
                        valuesOk = valuesOk && juce::exactlyEqual (out[start], (float) Bitcrush::quantize ((double) ramp[start], 16.0));
                    }
                    start = n;
                }
            }
            const auto lo = (int) std::floor (ratio), hi = (int) std::ceil (ratio);
            int longer = 0;
            double total = 0.0;
            for (auto l : lengths)
            {
                lengthsOk = lengthsOk && (l == lo || l == hi);
                longer += l == hi && hi != lo ? 1 : 0;
                total += l;
            }
            const auto mean = total / (double) lengths.size();
            meansOk = meansOk && std::abs (mean - ratio) <= 1.0 / (double) lengths.size() + 1.0e-9;
            rows.add (juce::String (rate, 1) + " Hz (R " + juce::String (ratio, 4) + "): " + juce::String ((int) lengths.size()) + " holds of "
                      + (lo == hi ? juce::String (lo) : juce::String (lo) + " or " + juce::String (hi)) + " samples, mean " + juce::String (mean, 5)
                      + (lo == hi ? juce::String() : ", " + juce::String (100.0 * longer / (double) lengths.size(), 1) + "% long (frac(R) "
                                                         + juce::String (100.0 * (ratio - lo), 1) + "%)"));
        }
        expect (lengthsOk);
        expect (valuesOk);
        expect (meansOk);
        logMessage ("  -> 1 s ramp at 16 bits, every hold measured: " + rows.joinIntoString ("; ")
                    + "; every hold starts with the input at its first sample, quantized: " + (valuesOk ? "true" : "false"));
    }

    void bitcrushAliasing()
    {
        beginTest ("bitcrush: aliasing is there by design, at the zero-order hold's predicted levels");

        // A 3 kHz sine held at 8 kHz (R = 6), 16 bits. The held sequence is the sine sampled every 6 samples,
        // so its images sit at m 8 kHz +- 3 kHz (folded into 0 to 24 kHz), each at (A / 6) |sin(3 w) / sin(w / 2)|,
        // the 6-sample hold's response at its own frequency w = 2 pi f / fs.
        const auto tone = sine (3000.0, 0.5, (int) (2.0 * fs));
        Bitcrush c;
        c.setSettings (crush (16.0f, 8000.0f));
        c.prepare (fs, blockSize);
        const auto out = run (c, tone).left;

        const auto predicted = [] (double f)
        {
            const auto w = twoPi * f / fs;
            return 0.5 / 6.0 * std::abs (std::sin (3.0 * w) / std::sin (0.5 * w));
        };
        juce::StringArray rows;
        double worst = 0.0;
        for (const auto f : { 3000.0, 5000.0, 11000.0, 13000.0, 19000.0, 21000.0 })
        {
            const auto a = toneAmplitude (out, f, (size_t) (0.5 * fs), (size_t) fs);
            const auto error = toDb (a / predicted (f));
            worst = std::max (worst, std::abs (error));
            rows.add (juce::String (f / 1000.0, 0) + " kHz " + juce::String (toDb (a / 0.5), 2) + " dB (predicted " + juce::String (toDb (predicted (f) / 0.5), 2) + ")");
        }
        expectLessThan (worst, 0.05);
        const auto aliasDb = toDb (toneAmplitude (out, 5000.0, (size_t) (0.5 * fs), (size_t) fs) / toneAmplitude (out, 3000.0, (size_t) (0.5 * fs), (size_t) fs));
        expectGreaterThan (aliasDb, -6.0);

        // A fractional ratio (7 kHz, R = 6.857): holds of 6 and 7 samples, and the first image at 7 - 3 = 4 kHz.
        Bitcrush f;
        f.setSettings (crush (16.0f, 7000.0f));
        f.prepare (fs, blockSize);
        const auto fractional = run (f, tone).left;
        const auto fractionalAlias = toDb (toneAmplitude (fractional, 4000.0, (size_t) (0.5 * fs), (size_t) fs)
                                           / toneAmplitude (fractional, 3000.0, (size_t) (0.5 * fs), (size_t) fs));
        expectGreaterThan (fractionalAlias, -10.0);

        // Plot the 8 kHz case's spectrum against the predicted image levels.
        const auto power = powerSpectrum (std::vector<float> (out.begin() + (long) (0.25 * fs), out.end()));
        const auto reference = power[(size_t) std::lround (3000.0 * 8192.0 / fs)];
        PlotSeries spectrum { "3 kHz sine held at 8 kHz (relative to the 3 kHz component)", {}, {}, plotColour (0), 1.5f };
        for (size_t k = 1; k < power.size(); ++k)
        {
            spectrum.x.push_back ((double) k * fs / 8192.0);
            spectrum.y.push_back (10.0 * std::log10 (std::max (power[k], 1.0e-30) / reference));
        }
        std::vector<PlotSeries> series { spectrum };
        for (const auto image : { 5000.0, 11000.0, 13000.0, 19000.0, 21000.0 })
        {
            const auto level = toDb (predicted (image) / predicted (3000.0));
            series.push_back ({ image < 6000.0 ? juce::String ("predicted image levels") : juce::String(), { image - 400.0, image + 400.0 }, { level, level }, plotColour (6), 2.5f });
        }
        PlotOptions o;
        o.title = "Bitcrush aliasing: no anti-aliasing, images at m 8 kHz +- 3 kHz";
        o.xLabel = "Frequency (Hz)";
        o.yLabel = "Level (dB)";
        o.xMin = 0.0; o.xMax = 24000.0; o.yMin = -120.0; o.yMax = 5.0;
        const auto png = proofDir().getChildFile ("bloom_bitcrush_aliasing.png");
        expect (savePlot (png, o, series));

        logMessage ("  -> 3 kHz sine (-6 dBFS) held at 8 kHz, each component's level vs the hold's prediction (largest error " + juce::String (worst, 3)
                    + " dB, limit 0.05): " + rows.joinIntoString (", ") + "; the inharmonic 5 kHz image sits " + juce::String (aliasDb, 2) + " dB from the 3 kHz tone");
        logMessage ("  -> held at 7 kHz (fractional ratio 6.857): the 4 kHz image at " + juce::String (fractionalAlias, 2) + " dB from the tone");
        logMessage ("  -> " + png.getFullPathName());
    }

    void bitcrushDither()
    {
        beginTest ("bitcrush: TPDF dither makes the error's mean 0 and variance step^2 / 4 at every level, white, and turns gated decays into hiss");

        // DC inputs at 4 bits (step 1/8), 2 s each, no hold: the error y - x with and without dither.
        const auto step = Bitcrush::stepSize (4.0);
        juce::StringArray rows;
        double worstMean = 0.0, worstVariance = 0.0;
        for (const auto fraction : { 0.0, 0.1, 0.25, 0.5, 0.73 })
        {
            const auto level = (float) ((3.0 + fraction) * step);
            const std::vector<float> dc ((size_t) (2.0 * fs), level);
            double stats[2][2] {}; // [dither][mean, variance]
            for (int d = 0; d < 2; ++d)
            {
                Bitcrush c;
                c.setSettings (crush (4.0f, (float) fs, d == 1));
                c.prepare (fs, blockSize);
                const auto out = run (c, dc).left;
                double sum = 0.0, squares = 0.0;
                for (auto y : out)
                {
                    const auto e = (double) y - (double) level;
                    sum += e;
                    squares += e * e;
                }
                const auto mean = sum / (double) out.size();
                stats[d][0] = mean;
                stats[d][1] = squares / (double) out.size() - mean * mean;
            }
            worstMean = std::max (worstMean, std::abs (stats[1][0]) / step);
            worstVariance = std::max (worstVariance, std::abs (stats[1][1] / (step * step / 4.0) - 1.0));
            rows.add ("x = " + juce::String (3.0 + fraction, 2) + " steps: dither mean " + juce::String (stats[1][0] / step, 4) + " steps, variance "
                      + juce::String (stats[1][1] / (step * step), 4) + " steps^2; without " + juce::String (stats[0][0] / step, 2) + " and "
                      + juce::String (stats[0][1] / (step * step), 4));
        }
        expectLessThan (worstMean, 0.01);
        expectLessThan (worstVariance, 0.03);

        // The spectrum of dithered silence (the output is the error): 10 s, 12 bands of 2 kHz.
        const std::vector<float> silence ((size_t) (10.0 * fs), 0.0f);
        Bitcrush hiss;
        hiss.setSettings (crush (4.0f, (float) fs, true));
        hiss.prepare (fs, blockSize);
        const auto power = powerSpectrum (run (hiss, silence).left);
        std::vector<double> bands (12, 0.0);
        for (size_t k = 1; k < power.size() - 1; ++k)
            bands[std::min ((size_t) 11, (size_t) ((double) k * fs / 8192.0 / 2000.0))] += power[k];
        double meanBand = 0.0;
        for (auto b : bands)
            meanBand += b / 12.0;
        double flatness = 0.0;
        for (auto b : bands)
            flatness = std::max (flatness, std::abs (10.0 * std::log10 (b / meanBand)));
        expectLessThan (flatness, 0.5);

        // A decaying 440 Hz note at 6 bits: without dither it gates to digital silence; with it, hiss remains.
        std::vector<float> note ((size_t) (3.0 * fs));
        for (size_t n = 0; n < note.size(); ++n)
            note[n] = (float) (0.5 * std::exp (-(double) n / fs / 0.3) * std::sin (twoPi * 440.0 * (double) n / fs));
        double gateSeconds = 0.0, tailRms[2] {};
        for (int d = 0; d < 2; ++d)
        {
            Bitcrush c;
            c.setSettings (crush (6.0f, (float) fs, d == 1));
            c.prepare (fs, blockSize);
            const auto out = run (c, note).left;
            if (d == 0)
            {
                size_t last = 0;
                for (size_t n = 0; n < out.size(); ++n)
                    if (out[n] != 0.0f)
                        last = n;
                gateSeconds = (double) last / fs;
            }
            tailRms[d] = rms (out.data() + (size_t) (2.5 * fs), (size_t) (0.5 * fs));
        }
        const auto sixBitStep = Bitcrush::stepSize (6.0);
        expectEquals (tailRms[0], 0.0);
        expectWithinAbsoluteError (tailRms[1] / (0.5 * sixBitStep), 1.0, 0.05);

        logMessage ("  -> 4 bits, 2 s of DC each (TPDF predicts mean 0 and variance 0.25 steps^2 at every level; largest mean " + juce::String (worstMean, 4)
                    + " steps, variance off by " + juce::String (100.0 * worstVariance, 2) + "%): " + rows.joinIntoString ("; "));
        logMessage ("  -> dithered silence, 10 s: the 12 bands of 2 kHz are within " + juce::String (flatness, 3) + " dB of their mean (white; limit 0.5 dB)");
        logMessage ("  -> a 440 Hz note decaying from -6 dBFS (tau 0.3 s) at 6 bits: without dither it gates to exact silence at " + juce::String (gateSeconds, 3)
                    + " s (" + juce::String (toDb (0.5 * std::exp (-gateSeconds / 0.3)), 1) + " dBFS, half a step is " + juce::String (toDb (0.5 * sixBitStep), 1)
                    + " dBFS); with dither the last 0.5 s is hiss at " + juce::String (toDb (tailRms[1]), 2) + " dBFS RMS (predicted step / 2 = "
                    + juce::String (toDb (0.5 * sixBitStep), 2) + ")");
    }

    // ---- Phaser -----------------------------------------------------------------------------------

    void phaserGolden()
    {
        beginTest ("phaser: matches prototypes/vibe.py sample by sample in every mode (golden renders, limit -100 dB)");

        const auto cases = juce::JSON::parse (fixture ("phaser_cases.json"));
        const auto input = readWav (fixture ("phaser_input.wav"));
        expectEquals (input.getNumChannels(), 2);
        const auto left = channel (input, 0), right = channel (input, 1);
        juce::StringArray results;
        for (const auto* name : { "vibe", "vibe_fast_wet", "classic_block", "modern_6_negative", "modern_12_resonant" })
        {
            const auto expected = readWav (fixture ("expected_phaser_" + juce::String (name) + ".wav"));
            Phaser p;
            p.setSettings (phaserFromJson (cases["phaser"][name]));
            p.prepare (fs, blockSize);
            const auto out = run (p, left, right);
            const auto worst = std::max (relativeErrorDb (out.left, channel (expected, 0)), relativeErrorDb (out.right, channel (expected, 1)));
            expectLessThan (worst, -100.0);
            results.add (juce::String (name) + " " + dB (worst));
        }
        logMessage ("  -> C++ vs prototypes/vibe.py (tests/fixtures/bloom, 1 s stereo guitar-like input): " + results.joinIntoString ("; "));
    }

    void vibeSweep()
    {
        beginTest ("vibe: the lamp-and-photocell sweep and its asymmetry match the Python model (golden)");

        const auto cases = juce::JSON::parse (fixture ("phaser_cases.json"));
        juce::StringArray rows;
        double worstCents = 0.0, worstFraction = 0.0, worstMs = 0.0, worstOctaves = 0.0;
        std::vector<PlotSeries> series;
        int colour = 0;
        for (const auto* name : { "vibe_sweep_1hz", "vibe_sweep_4hz" })
        {
            const auto spec = cases["sweeps"][name];
            const auto rate = (double) spec["rateHz"];
            Phaser::Settings s;
            s.on = true;
            s.mode = Phaser::Mode::vibe;
            s.rateHz = (float) rate;
            s.depth = (float) (double) spec["depth"];
            s.stereoOffset = 0.0f;
            Phaser p;
            p.setSettings (s);
            p.prepare (fs, blockSize);

            // One sample at a time with silence going in, recording the 15 nF stage's corner.
            const auto expected = channel (readWav (fixture ("expected_" + juce::String (name) + ".wav")), 0);
            std::vector<double> corner (expected.size()), reference (expected.size());
            float l = 0.0f, r = 0.0f;
            for (size_t n = 0; n < expected.size(); ++n)
            {
                l = r = 0.0f;
                float* channels[2] = { &l, &r };
                p.process (juce::dsp::AudioBlock<float> (channels, 2, 1), {});
                corner[n] = p.getCornerHz (0, 0);
                reference[n] = 1000.0 * (double) expected[n];
                worstCents = std::max (worstCents, std::abs (1200.0 * std::log2 (corner[n] / reference[n])));
            }

            const auto m = sweepMetrics (corner, rate);
            const auto& py = spec["metrics"];
            worstFraction = std::max (worstFraction, std::abs (m.riseFraction - (double) py["rise_fraction"]));
            worstMs = std::max ({ worstMs, std::abs (m.riseMs - (double) py["rise_ms"]), std::abs (m.fallMs - (double) py["fall_ms"]) });
            worstOctaves = std::max (worstOctaves, std::abs (m.octaves - (double) py["octaves"]));
            rows.add (juce::String (rate, 0) + " Hz: rises in " + juce::String (100.0 * m.riseFraction, 2) + "% of the cycle (Python "
                      + juce::String (100.0 * (double) py["rise_fraction"], 2) + "%), 10-90% rise " + juce::String (m.riseMs, 2) + " ms vs fall "
                      + juce::String (m.fallMs, 2) + " ms (Python " + juce::String ((double) py["rise_ms"], 2) + " / " + juce::String ((double) py["fall_ms"], 2)
                      + "), swing " + juce::String (m.lowHz, 0) + " to " + juce::String (m.highHz, 0) + " Hz (" + juce::String (m.octaves, 4) + " octaves, Python "
                      + juce::String ((double) py["octaves"], 4) + ")");

            // The last two cycles, against the LFO (drawn as the corner a lag-free, symmetric lamp would give).
            const auto cycle = fs / rate;
            const auto from = corner.size() - (size_t) (2.0 * cycle);
            PlotSeries cpp { juce::String (rate, 0) + " Hz: C++", {}, {}, plotColour (colour), 2.5f };
            PlotSeries python { juce::String (rate, 0) + " Hz: Python", {}, {}, plotColour (6), 1.0f, true };
            PlotSeries lfo { juce::String (rate, 0) + " Hz: LFO (scaled)", {}, {}, plotColour (colour + 2), 1.0f, true };
            for (size_t n = from; n < corner.size(); n += 24)
            {
                const auto t = (double) (n - from) / cycle;
                cpp.x.push_back (t);
                cpp.y.push_back (corner[n]);
                python.x.push_back (t);
                python.y.push_back (reference[n]);
                lfo.x.push_back (t);
                lfo.y.push_back (Phaser::vibeReferenceHz (0.5 + 0.5 * std::sin (twoPi * rate * (double) n / fs)));
            }
            series.push_back (lfo);
            series.push_back (cpp);
            series.push_back (python);
            colour += 1;
        }
        expectLessThan (worstCents, 0.001);
        expectLessThan (worstFraction, 1.0e-4);
        expectLessThan (worstMs, 0.05);
        expectLessThan (worstOctaves, 1.0e-4);

        PlotOptions o;
        o.title = "Vibe sweep: the 15 nF stage's corner over two LFO cycles (fast rise, slow fall; dashed: Python, LFO)";
        o.xLabel = "Time (LFO cycles)";
        o.yLabel = "Corner (Hz)";
        o.xMin = 0.0; o.xMax = 2.0; o.yMin = 0.0; o.yMax = 3700.0;
        const auto png = proofDir().getChildFile ("bloom_vibe_sweep.png");
        expect (savePlot (png, o, series));

        logMessage ("  -> the corner sample by sample vs Python: largest difference " + juce::String (worstCents, 6) + " cents (limit 0.001; the fixture is float32)");
        logMessage ("  -> sweep asymmetry, C++ (limits vs Python: 0.01% of the cycle, 0.05 ms, 1e-4 octaves): " + rows.joinIntoString ("; "));
        logMessage ("  -> " + png.getFullPathName());
    }

    void phaserNotches()
    {
        beginTest ("phaser: stages / 2 notches, where the sweep mapping and the allpass formula put them");

        juce::StringArray rows;
        bool countsOk = true;
        double worstPosition = 0.0, shallowest = -400.0;
        for (const auto stages : { 2, 4, 6, 8, 12 })
        {
            juce::StringArray perCorner;
            for (const auto corner : { 100.0f, 400.0f, 1000.0f, 4000.0f })
            {
                Phaser p;
                p.setSettings (staticPhaser (stages, corner, 0.0f, 0.5f));
                p.prepare (fs, blockSize);
                const auto h = impulseResponse (p, 65536);
                const auto notches = findNotches (h, -20.0, 5.0, 23900.0);
                countsOk = countsOk && (int) notches.size() == stages / 2;
                for (size_t k = 0; k < std::min (notches.size(), (size_t) (stages / 2)); ++k)
                {
                    const auto predicted = Phaser::notchFrequency (stages, (int) k, corner, fs);
                    worstPosition = std::max (worstPosition, std::abs (notches[k] / predicted - 1.0));
                    shallowest = std::max (shallowest, toDb (std::abs (dtft (h, notches[k]))));
                }
                juce::StringArray positions;
                for (auto f : notches)
                    positions.add (juce::String (f, 1));
                if (corner == 1000.0f)
                    perCorner.add ("at 1 kHz: " + positions.joinIntoString (", ") + " Hz");
                else
                    perCorner.add (juce::String (corner, 0) + " Hz: " + juce::String ((int) notches.size()));
            }
            rows.add (juce::String (stages) + " stages, " + perCorner.joinIntoString ("; "));
        }
        expect (countsOk);
        expectLessThan (worstPosition, 1.0e-6);
        expectLessThan (shallowest, -60.0);

        // The sweep mapping: the corner that the running block uses, sample by sample, against the formulas.
        double worstModern = 0.0, worstClassic = 0.0;
        for (auto mode : { Phaser::Mode::modern, Phaser::Mode::classic })
        {
            Phaser::Settings s;
            s.on = true;
            s.mode = mode;
            s.stages = 6;
            s.rateHz = 1.7f;
            s.depth = 0.8f;
            s.shape = Lfo::Shape::sine;
            s.lowHz = 120.0f;
            s.highHz = 3500.0f;
            s.stereoOffset = 0.0f;
            Phaser p;
            p.setSettings (s);
            p.prepare (fs, blockSize);
            const auto rate = (double) s.rateHz;
            double phase = 0.0;
            float l = 0.0f, r = 0.0f;
            for (int n = 0; n < (int) (2.0 * fs); ++n)
            {
                l = r = 0.1f * (float) std::sin (0.01 * n);
                float* channels[2] = { &l, &r };
                p.process (juce::dsp::AudioBlock<float> (channels, 2, 1), {});
                const auto shape = mode == Phaser::Mode::modern ? Lfo::Shape::sine : Lfo::Shape::triangle;
                const auto m = (double) Lfo::shapeAt (shape, phase);
                const auto expected = mode == Phaser::Mode::modern ? Phaser::modernCorner (120.0, 3500.0, (double) s.depth, m)
                                                                   : Phaser::classicCorner ((double) s.depth, m);
                auto& worst = mode == Phaser::Mode::modern ? worstModern : worstClassic;
                for (int stage = 0; stage < (mode == Phaser::Mode::modern ? 6 : 4); ++stage)
                    worst = std::max (worst, std::abs (p.getCornerHz (0, stage) / expected - 1.0));
                phase += rate / fs;
                phase -= std::floor (phase);
            }
        }
        expectLessThan (std::max (worstModern, worstClassic), 1.0e-12);

        // Plot: the responses at a 1 kHz corner, mix 50%, plus 4 stages with feedback.
        std::vector<PlotSeries> series;
        int colour = 0;
        const auto addResponse = [&] (const Phaser::Settings& s, const juce::String& name, bool dashed)
        {
            Phaser p;
            p.setSettings (s);
            p.prepare (fs, blockSize);
            const auto magnitude = fftMagnitude (impulseResponse (p, 65536), 16);
            PlotSeries line { name, {}, {}, plotColour (colour++), dashed ? 1.5f : 2.0f, dashed };
            for (double f = 20.0; f < 20000.0; f *= 1.004)
            {
                line.x.push_back (f);
                line.y.push_back (toDb (magnitude[(size_t) std::lround (f * 65536.0 / fs)]));
            }
            series.push_back (line);
        };
        for (const auto stages : { 2, 4, 6, 8, 12 })
            addResponse (staticPhaser (stages, 1000.0f, 0.0f, 0.5f), juce::String (stages) + " stages", false);
        addResponse (staticPhaser (4, 1000.0f, 0.7f, 0.5f), "4 stages, feedback +0.7", true);
        addResponse (staticPhaser (4, 1000.0f, -0.7f, 0.5f), "4 stages, feedback -0.7", true);
        PlotOptions o;
        o.title = "Phaser at a 1 kHz corner, mix 50%: stages / 2 notches (dashed: with feedback)";
        o.xLabel = "Frequency (Hz)";
        o.yLabel = "Magnitude (dB)";
        o.logX = true;
        o.xMin = 20.0; o.xMax = 20000.0; o.yMin = -60.0; o.yMax = 12.0;
        const auto png = proofDir().getChildFile ("bloom_phaser_responses.png");
        expect (savePlot (png, o, series));

        logMessage ("  -> notches (minima below -20 dB on the grid, refined) of the fixed chain at corners 100 Hz, 400 Hz, 1 kHz, 4 kHz, mix 50%, no feedback: "
                    + rows.joinIntoString ("; "));
        logMessage ("  -> every count is stages / 2; positions vs (fs / pi) atan(g tan((2k + 1) pi / 2N)): largest relative error " + juce::String (worstPosition, 9)
                    + " (limit 1e-6); the shallowest notch is " + juce::String (shallowest, 1) + " dB (total, limit -60)");
        logMessage ("  -> the running sweep's corner vs the mapping, sample by sample over 2 s: Modern (exponential) " + juce::String (worstModern, 14)
                    + ", Classic (linear in Hz) " + juce::String (worstClassic, 14) + " relative (limit 1e-12)");
        logMessage ("  -> " + png.getFullPathName());
    }

    void phaserSweepTracking()
    {
        beginTest ("phaser: the moving notch, measured on white noise, follows the sweep mapping");

        // Two stages (one notch, exactly at the corner), sweeping 300 Hz to 3 kHz at depth 50% and 0.05 Hz, so a
        // 171 ms window sees the notch nearly still. Each window: the transfer estimate Sxy / Sxx over eight
        // 1024-sample Hann frames, its minimum, refined by a parabola through the three bins around it.
        Phaser::Settings s;
        s.on = true;
        s.mode = Phaser::Mode::modern;
        s.stages = 2;
        s.rateHz = 0.05f;
        s.depth = 0.5f;
        s.shape = Lfo::Shape::sine;
        s.lowHz = 300.0f;
        s.highHz = 3000.0f;
        s.stereoOffset = 0.0f;
        s.mix = 0.5f;
        Phaser p;
        p.setSettings (s);
        p.prepare (fs, blockSize);
        const auto noise = whiteNoise ((int) (20.0 * fs), 0.3f, 17);
        const auto out = run (p, noise).left;

        constexpr int order = 10, size = 1 << order, frames = 8;
        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) size), bufX ((size_t) (2 * size)), bufY ((size_t) (2 * size));
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (twoPi * i / size));

        PlotSeries measured { "measured notch (40 windows)", {}, {}, plotColour (0), 3.0f };
        PlotSeries predicted { "the mapping: 300 (3000 / 300)^(1/2 + depth m / 2)", {}, {}, plotColour (6), 1.5f, true };
        double worst = 0.0;
        for (int w = 0; w < 40; ++w)
        {
            const auto start = (size_t) (0.25 * fs + w * 0.5 * fs);
            std::vector<std::complex<double>> sxy ((size_t) (size / 2 + 1));
            std::vector<double> sxx ((size_t) (size / 2 + 1));
            for (int f = 0; f < frames; ++f)
            {
                std::fill (bufX.begin(), bufX.end(), 0.0f);
                std::fill (bufY.begin(), bufY.end(), 0.0f);
                for (int i = 0; i < size; ++i)
                {
                    bufX[(size_t) i] = noise[start + (size_t) (f * size + i)] * window[(size_t) i];
                    bufY[(size_t) i] = out[start + (size_t) (f * size + i)] * window[(size_t) i];
                }
                fft.performRealOnlyForwardTransform (bufX.data(), true);
                fft.performRealOnlyForwardTransform (bufY.data(), true);
                for (int k = 0; k <= size / 2; ++k)
                {
                    const std::complex<double> X (bufX[(size_t) (2 * k)], bufX[(size_t) (2 * k + 1)]), Y (bufY[(size_t) (2 * k)], bufY[(size_t) (2 * k + 1)]);
                    sxy[(size_t) k] += Y * std::conj (X);
                    sxx[(size_t) k] += std::norm (X);
                }
            }
            const auto binHz = fs / size;
            size_t best = 0;
            double bestPower = 1.0e30;
            for (size_t k = (size_t) (200.0 / binHz); k < (size_t) (4000.0 / binHz); ++k)
            {
                const auto power = std::norm (sxy[k] / sxx[k]);
                if (power < bestPower)
                {
                    bestPower = power;
                    best = k;
                }
            }
            const auto a = std::norm (sxy[best - 1] / sxx[best - 1]), b = bestPower, c = std::norm (sxy[best + 1] / sxx[best + 1]);
            const auto offset = 0.5 * (a - c) / (a - 2.0 * b + c);
            const auto notch = ((double) best + offset) * binHz;

            const auto centre = (double) start / fs + (double) (frames * size) / (2.0 * fs);
            const auto m = std::sin (twoPi * 0.05 * centre);
            const auto expected = Phaser::modernCorner (300.0, 3000.0, 0.5, m);
            worst = std::max (worst, std::abs (notch / expected - 1.0));
            measured.x.push_back (centre);
            measured.y.push_back (notch);
        }
        for (double t = 0.0; t < 20.5; t += 0.01)
        {
            predicted.x.push_back (t);
            predicted.y.push_back (Phaser::modernCorner (300.0, 3000.0, 0.5, std::sin (twoPi * 0.05 * t)));
        }
        expectLessThan (worst, 0.02);

        PlotOptions o;
        o.title = "Phaser sweep: the notch of 2 stages measured on white noise vs the exponential mapping";
        o.xLabel = "Time (s)";
        o.yLabel = "Notch (Hz)";
        o.xMin = 0.0; o.xMax = 20.5; o.yMin = 400.0; o.yMax = 1800.0;
        const auto png = proofDir().getChildFile ("bloom_phaser_sweep.png");
        expect (savePlot (png, o, { predicted, measured }));
        logMessage ("  -> 40 windows of 171 ms over one 20 s sweep (533 Hz to 1.69 kHz): the measured notch is within " + juce::String (100.0 * worst, 3)
                    + "% of the mapping at each window's centre (limit 2%)");
        logMessage ("  -> " + png.getFullPathName());
    }

    void phaserAllpass()
    {
        beginTest ("phaser: the wet alone (mix 100%, no feedback) has unity magnitude at every frequency; with feedback it's the solved loop exactly");

        double worst = 0.0;
        int configurations = 0;
        for (const auto stages : { 2, 4, 6, 8, 12 })
        {
            for (const auto corner : { 100.0f, 1000.0f, 6000.0f })
            {
                Phaser p;
                p.setSettings (staticPhaser (stages, corner, 0.0f, 1.0f));
                p.prepare (fs, blockSize);
                const auto magnitude = fftMagnitude (impulseResponse (p, 65536), 16);
                for (size_t k = 1; k < magnitude.size(); ++k)
                    worst = std::max (worst, std::abs (toDb (magnitude[k])));
                ++configurations;
            }
        }
        // The Vibe's mismatched stages, held still (depth 0: the lamp sits at its bias, steady from its warm start).
        {
            Phaser::Settings s;
            s.on = true;
            s.mode = Phaser::Mode::vibe;
            s.depth = 0.0f;
            s.mix = 1.0f;
            s.stereoOffset = 0.0f;
            Phaser p;
            p.setSettings (s);
            p.prepare (fs, blockSize);
            const auto magnitude = fftMagnitude (impulseResponse (p, 65536), 16);
            for (size_t k = 1; k < magnitude.size(); ++k)
                worst = std::max (worst, std::abs (toDb (magnitude[k])));
            ++configurations;
        }
        expectLessThan (worst, 0.001);

        // Feedback: H = sqrt(1 - fb^2) A / (1 - fb A), A = ((1 - jW) / (1 + jW))^N with W = tan(pi f / fs) / g.
        double worstLoop = 0.0;
        for (const auto fb : { 0.7f, -0.7f, 0.9f })
        {
            Phaser p;
            p.setSettings (staticPhaser (4, 1000.0f, fb, 1.0f));
            p.prepare (fs, blockSize);
            const auto h = impulseResponse (p, 65536);
            const auto g = std::tan (pi * 1000.0 / fs);
            for (double f = 30.0; f < 20000.0; f *= 1.05)
            {
                const std::complex<double> stage (1.0, -std::tan (pi * f / fs) / g), a = std::pow (std::conj (stage) / stage, 4);
                const auto expected = std::sqrt (1.0 - (double) fb * fb) * a / (1.0 - (double) fb * a);
                worstLoop = std::max (worstLoop, std::abs (toDb (std::abs (dtft (h, f)) / std::abs (expected))));
            }
        }
        expectLessThan (worstLoop, 0.001);

        logMessage ("  -> " + juce::String (configurations) + " fixed chains (2 to 12 stages at 100 Hz, 1 kHz, 6 kHz, and the Vibe's mismatched four), wet only: "
                    "largest deviation from 0 dB over every FFT bin " + juce::String (worst, 6) + " dB (limit 0.001)");
        logMessage ("  -> with feedback +0.7, -0.7, +0.9 (4 stages, 1 kHz): the measured wet vs sqrt(1 - fb^2) A / (1 - fb A) from 30 Hz to 20 kHz, largest error "
                    + juce::String (worstLoop, 6) + " dB (limit 0.001): the delay-free loop is solved exactly");
    }

    void phaserStability()
    {
        beginTest ("phaser: stable at maximum feedback, however fast the sweep");

        struct Case
        {
            const char* name;
            Phaser::Settings settings;
        };
        const auto modern = [] (int stages, float fb)
        {
            Phaser::Settings s;
            s.on = true;
            s.mode = Phaser::Mode::modern;
            s.stages = stages;
            s.rateHz = 10.0f;
            s.depth = 1.0f;
            s.lowHz = 20.0f;
            s.highHz = 20000.0f;
            s.feedback = fb;
            s.mix = 0.5f;
            return s;
        };
        Phaser::Settings classic;
        classic.on = true;
        classic.classicFeedback = true;
        classic.rateHz = 10.0f;
        Phaser::Settings vibe;
        vibe.on = true;
        vibe.mode = Phaser::Mode::vibe;
        vibe.rateHz = 10.0f;
        const std::vector<Case> cases { { "Modern 12 stages +0.9", modern (12, 0.9f) }, { "Modern 12 stages -0.9", modern (12, -0.9f) },
                                        { "Modern 2 stages +0.9", modern (2, 0.9f) },   { "Modern 6 stages -0.9", modern (6, -0.9f) },
                                        { "Classic with feedback", classic },           { "Vibe", vibe } };

        // 10 s of full-scale white noise with a full-scale impulse every second, then 2 s of silence.
        auto input = whiteNoise ((int) (10.0 * fs), 1.0f, 23);
        for (size_t n = 0; n < input.size(); n += (size_t) fs)
            input[n] = 1.0f;
        input.resize ((size_t) (12.0 * fs), 0.0f);

        juce::StringArray rows;
        bool finite = true;
        double worstPeak = 0.0, worstTail = 0.0;
        for (const auto& c : cases)
        {
            Phaser p;
            p.setSettings (c.settings);
            p.prepare (fs, blockSize);
            const auto out = run (p, input);
            for (const auto* y : { &out.left, &out.right })
                for (auto v : *y)
                    finite = finite && std::isfinite (v);
            const auto peak = std::max (peakAbs (out.left, 0, (size_t) (10.0 * fs)), peakAbs (out.right, 0, (size_t) (10.0 * fs)));
            const auto tail = rms (out.left.data() + (size_t) (11.5 * fs), (size_t) (0.5 * fs));
            worstPeak = std::max (worstPeak, peak);
            worstTail = std::max (worstTail, tail);
            rows.add (juce::String (c.name) + ": peak " + juce::String (peak, 2) + ", RMS gain " + juce::String (toDb (rms (out.left.data(), (size_t) (10.0 * fs)) / rms (input.data(), (size_t) (10.0 * fs))), 2)
                      + " dB, tail " + juce::String (toDb (tail), 0) + " dBFS");
        }
        expect (finite);
        expectLessThan (worstPeak, 10.0);
        expectLessThan (worstTail, 1.0e-6);
        logMessage ("  -> 10 s of full-scale noise and impulses, sweeping 20 Hz to 20 kHz at 10 Hz, then 2 s of silence (all finite, peak limit 10, tail limit -120 dBFS): "
                    + rows.joinIntoString ("; "));
    }

    // ---- Flanger ----------------------------------------------------------------------------------

    void flangerComb()
    {
        beginTest ("flanger: the comb's notches are 1/d apart at the Manual delay d, and feedback gives exactly the predicted response");

        juce::StringArray rows;
        double worstSpacing = 0.0, worstFractional = 0.0, worstPosition = 0.0;
        for (const auto manual : { 1.0f, 2.5f, 7.0f, 1.37f })
        {
            for (const bool negative : { false, true })
            {
                Flanger f;
                f.setSettings (staticFlanger (manual, negative, 0.0f));
                f.prepare (fs, blockSize);
                const auto h = impulseResponse (f, 8192);
                const auto d = (double) manual * fs / 1000.0;
                const auto spacing = fs / d;
                const bool fractional = ! juce::exactlyEqual (d, std::floor (d));
                const auto top = fractional ? 5000.0 : 15000.0;
                const auto notches = findNotches (h, -30.0, 10.0, top);

                // Positive: notches at (k + 1/2) / d; negative: k / d from k = 1 (DC is a notch too).
                double sumK = 0.0, sumF = 0.0, sumKK = 0.0, sumKF = 0.0;
                for (size_t i = 0; i < notches.size(); ++i)
                {
                    const auto k = (double) i + (negative ? 1.0 : 0.5);
                    sumK += k; sumF += notches[i]; sumKK += k * k; sumKF += k * notches[i];
                    if (! fractional)
                        worstPosition = std::max (worstPosition, std::abs (notches[i] / (k * spacing) - 1.0));
                }
                const auto count = (double) notches.size();
                const auto fitted = (count * sumKF - sumK * sumF) / (count * sumKK - sumK * sumK);
                const auto error = std::abs (fitted / spacing - 1.0);
                (fractional ? worstFractional : worstSpacing) = std::max (fractional ? worstFractional : worstSpacing, error);
                if (! negative || fractional)
                    rows.add (juce::String (manual, 2) + " ms " + (negative ? "negative" : "positive") + ": " + juce::String ((int) notches.size())
                              + " notches below " + juce::String (top / 1000.0, 0) + " kHz, " + juce::String (fitted, 3) + " Hz apart (1/d = " + juce::String (spacing, 3) + ")");
            }
        }
        expectLessThan (worstSpacing, 1.0e-6);
        expectLessThan (worstPosition, 1.0e-6);
        expectLessThan (worstFractional, 0.005);

        // Feedback +-0.7 at 2 ms, with the matching polarity, against
        //   H = (1 - mix) + mix sign sqrt(1 - fb^2) e^(-jwd) / (1 - fb HP e^(-jwd)),  HP = jW / (1 + jW), W = tan(pi f / fs) / tan(pi 150 / fs)
        // measured with a small impulse, where the loop's tanh is linear.
        double worstLoop = 0.0;
        std::vector<PlotSeries> series;
        int colour = 0;
        const auto d = 2.0 * fs / 1000.0;
        for (const auto fb : { 0.0f, 0.7f, -0.7f })
        {
            for (const bool negative : { false, true })
            {
                if (fb != 0.0f && negative != (fb < 0.0f))
                    continue;
                Flanger f;
                f.setSettings (staticFlanger (2.0f, negative, fb));
                f.prepare (fs, blockSize);
                const auto h = impulseResponse (f, 32768, 1.0e-3);
                const auto g = std::tan (pi * Flanger::loopHighPassHz / fs);
                const auto expectedAt = [&] (double freq)
                {
                    const auto delay = std::polar (1.0, -twoPi * freq * d / fs);
                    const std::complex<double> jw (0.0, std::tan (pi * freq / fs) / g);
                    const auto hp = jw / (1.0 + jw);
                    const auto sign = negative ? -1.0 : 1.0;
                    return 0.5 + 0.5 * sign * Flanger::feedbackCompensation (fb) * delay / (1.0 - (double) fb * hp * delay);
                };
                for (double freq = 50.0; freq < 15000.0; freq *= 1.01)
                {
                    const auto expected = std::abs (expectedAt (freq));
                    if (expected > 1.0e-3) // away from exact zeros
                        worstLoop = std::max (worstLoop, std::abs (toDb (std::abs (dtft (h, freq)) / expected)));
                }
                const auto magnitude = fftMagnitude (h, 16);
                PlotSeries line { juce::String (negative ? "negative" : "positive") + (fb == 0.0f ? juce::String (", no feedback") : ", feedback " + juce::String (fb, 1)),
                                  {}, {}, plotColour (colour), 2.0f };
                PlotSeries theory { fb == 0.0f ? juce::String() : "predicted", {}, {}, plotColour (6), 1.0f, true };
                for (double freq = 20.0; freq < 20000.0; freq *= 1.003)
                {
                    line.x.push_back (freq);
                    line.y.push_back (toDb (magnitude[(size_t) std::lround (freq * 65536.0 / fs)]));
                    if (fb != 0.0f)
                    {
                        theory.x.push_back (freq);
                        theory.y.push_back (toDb (std::abs (expectedAt (freq))));
                    }
                }
                series.push_back (line);
                if (fb != 0.0f)
                    series.push_back (theory);
                ++colour;
            }
        }
        expectLessThan (worstLoop, 0.01);

        PlotOptions o;
        o.title = "Flanger comb at Manual 2 ms, mix 50% (dashed: predicted with feedback, 150 Hz loop high-pass)";
        o.xLabel = "Frequency (Hz)";
        o.yLabel = "Magnitude (dB)";
        o.logX = true;
        o.xMin = 20.0; o.xMax = 20000.0; o.yMin = -50.0; o.yMax = 12.0;
        const auto png = proofDir().getChildFile ("bloom_flanger_responses.png");
        expect (savePlot (png, o, series));

        logMessage ("  -> static comb (depth 0, no feedback, mix 50%), notch spacing from a straight-line fit: " + rows.joinIntoString ("; "));
        logMessage ("  -> whole-sample delays: spacing within " + juce::String (worstSpacing, 9) + " of 1/d and every notch within " + juce::String (worstPosition, 9)
                    + " of (k + 1/2)/d or k/d (limits 1e-6); 1.37 ms (65.76 samples, Hermite): spacing within " + juce::String (100.0 * worstFractional, 3) + "% (limit 0.5%)");
        logMessage ("  -> feedback +0.7 and -0.7: the measured comb vs the loop's formula (soft clip in its linear range, 150 Hz high-pass), 50 Hz to 15 kHz, largest error "
                    + juce::String (worstLoop, 5) + " dB (limit 0.01)");
        logMessage ("  -> " + png.getFullPathName());
    }

    void flangerFeedback()
    {
        beginTest ("flanger: feedback stays bounded at the maximum, on noise and on a resonance");

        // Manual 0.5 ms (24 samples), swept, feedback +-0.95. The line holds less than max|x| + 1 (the tanh), a
        // Hermite read overshoots by at most 1.25, and the wet is scaled by sqrt(1 - fb^2), so
        //   |out| <= (1 - mix) max|x| + mix sqrt(1 - fb^2) 1.25 (max|x| + 1)
        const auto noise = whiteNoise ((int) (4.0 * fs), 1.0f, 31);
        const auto resonance = sine (fs / 24.0, 1.0, (int) (4.0 * fs)); // 2 kHz: a multiple of 1/d
        juce::StringArray rows;
        bool finite = true, bounded = true;
        for (const auto fb : { 0.95f, -0.95f })
        {
            for (const auto* input : { &noise, &resonance })
            {
                Flanger::Settings s;
                s.on = true;
                s.manualMs = 0.5f;
                s.depth = 0.5f;
                s.rateHz = 0.3f;
                s.feedback = fb;
                s.mix = 0.5f;
                Flanger f;
                f.setSettings (s);
                f.prepare (fs, blockSize);
                const auto out = run (f, *input);
                double peak = 0.0;
                for (const auto* y : { &out.left, &out.right })
                    for (auto v : *y)
                    {
                        finite = finite && std::isfinite (v);
                        peak = std::max (peak, (double) std::abs (v));
                    }
                const auto bound = 0.5 * 1.0 + 0.5 * Flanger::feedbackCompensation (fb) * 1.25 * 2.0;
                bounded = bounded && peak < bound;
                rows.add (juce::String (fb, 2) + (input == &noise ? " on noise" : " on a 2 kHz resonance") + ": peak " + juce::String (peak, 3) + " (bound "
                          + juce::String (bound, 3) + ")");
            }
        }
        expect (finite);
        expect (bounded);
        logMessage ("  -> full-scale input, Manual 0.5 ms swept at 50% depth, mix 50%, all finite: " + rows.joinIntoString ("; "));
    }

    void throughZero()
    {
        beginTest ("through-zero: total cancellation at the crossing, and the reported latency is the measured one");

        const auto di = guitarDI ((int) (2.0 * fs));

        // 1. The crossing held still: Manual 5 ms = the dry's delay, depth 0, negative, mix 50%: nothing at all.
        auto s = staticFlanger (5.0f, true, 0.7f); // the feedback setting is ignored in this mode
        s.throughZero = true;
        Flanger still;
        still.setSettings (s);
        still.prepare (fs, blockSize);
        const auto cancelled = run (still, di);
        const auto residual = std::max (peakAbs (cancelled.left, 0, di.size()), peakAbs (cancelled.right, 0, di.size()));
        expectEquals (residual, 0.0);

        // Positive polarity at the crossing: the dry, exactly 240 samples late.
        s.negative = false;
        Flanger doubled;
        doubled.setSettings (s);
        doubled.prepare (fs, blockSize);
        const auto twice = run (doubled, di).left;
        bool delayedExactly = true;
        for (size_t n = 0; n < di.size(); ++n)
            delayedExactly = delayedExactly && juce::exactlyEqual (twice[n], n >= 240 ? di[n - 240] : 0.0f);
        expect (delayedExactly);

        // 2. Swept through zero: Manual 5 ms, depth 50% (2.75 to 7.25 ms), sine at 0.25 Hz, negative, on a 300 Hz
        // tone. With tau = d - 5 ms, out = A sin(pi f tau) cos(...): the level follows |sin(pi f tau)| and vanishes
        // where tau crosses zero (every 2 s).
        Flanger::Settings sweep;
        sweep.on = true;
        sweep.manualMs = 5.0f;
        sweep.depth = 0.5f;
        sweep.shape = Lfo::Shape::sine;
        sweep.rateHz = 0.25f;
        sweep.negative = true;
        sweep.stereoPhase = 0.0f;
        sweep.mix = 0.5f;
        sweep.throughZero = true;
        Flanger swept;
        swept.setSettings (sweep);
        swept.prepare (fs, blockSize);
        const auto tone = sine (300.0, 0.5, (int) (8.0 * fs));
        std::vector<float> out (tone.size());
        std::vector<double> tau (tone.size());
        for (size_t n = 0; n < tone.size(); ++n)
        {
            float l = tone[n], r = tone[n];
            float* channels[2] = { &l, &r };
            swept.process (juce::dsp::AudioBlock<float> (channels, 2, 1), {});
            out[n] = l;
            tau[n] = (swept.getDelaySamples (0) - 240.0) / fs;
        }
        const size_t window = 160; // one cycle of 300 Hz
        double worstTrack = 0.0, worstCrossing = -400.0;
        PlotSeries level { "measured (one-cycle RMS)", {}, {}, plotColour (0), 2.0f };
        PlotSeries theory { "A |sin(pi f tau)| / sqrt(2)", {}, {}, plotColour (6), 1.2f, true };
        for (size_t start = (size_t) (0.5 * fs); start + window < out.size(); start += 40)
        {
            const auto centre = start + window / 2;
            const auto measured = rms (out.data() + start, window);
            const auto expected = 0.5 / std::sqrt (2.0) * std::abs (std::sin (pi * 300.0 * tau[centre]));
            if (expected > 0.5 / std::sqrt (2.0) * 0.03) // above -30 dB
                worstTrack = std::max (worstTrack, std::abs (toDb (measured / expected)));
            level.x.push_back ((double) centre / fs);
            level.y.push_back (toDb (measured / (0.5 / std::sqrt (2.0))));
            theory.x.push_back ((double) centre / fs);
            theory.y.push_back (toDb (expected / (0.5 / std::sqrt (2.0))));
        }
        juce::StringArray crossings;
        for (const auto seconds : { 2.0, 4.0, 6.0 })
        {
            // The sample where tau changes sign, and the one-cycle RMS centred on it.
            auto at = (size_t) (seconds * fs) - 2000;
            while (at < out.size() - 1 && ! ((tau[at] <= 0.0) != (tau[at + 1] <= 0.0)))
                ++at;
            const auto depthDb = toDb (rms (out.data() + at - window / 2, window) / (0.5 / std::sqrt (2.0)));
            worstCrossing = std::max (worstCrossing, depthDb);
            crossings.add (juce::String ((double) at / fs, 4) + " s " + juce::String (depthDb, 1) + " dB");
        }
        expectLessThan (worstTrack, 0.5);
        expectLessThan (worstCrossing, -40.0);

        PlotOptions o;
        o.title = "Through-zero flanging: a 300 Hz tone as the wet sweeps through the 5 ms dry (negative, mix 50%)";
        o.xLabel = "Time (s)";
        o.yLabel = "Level re the input (dB)";
        o.xMin = 0.5; o.xMax = 8.0; o.yMin = -70.0; o.yMax = 3.0;
        const auto png = proofDir().getChildFile ("bloom_flanger_through_zero.png");
        expect (savePlot (png, o, { level, theory }));

        // 3. Latency: reported vs measured, for the flanger and for Bloom.
        Flanger off;
        expectEquals (off.latencySamples(), 0);
        expectEquals (still.latencySamples(), 240);

        std::vector<float> impulse ((size_t) 4096, 0.0f);
        impulse[100] = 1.0f;
        auto dryOnly = s;
        dryOnly.mix = 0.0f;
        Flanger probe;
        probe.setSettings (dryOnly);
        probe.prepare (fs, blockSize);
        const auto probeOut = run (probe, impulse).left;
        const auto measuredFlanger = (int) (std::max_element (probeOut.begin(), probeOut.end()) - probeOut.begin()) - 100;
        expectEquals (measuredFlanger, probe.latencySamples());

        // Bloom with everything on and the flanger in through-zero mode at Manual 2.5 ms: the cross-correlation
        // of output and input peaks at the latency (the 5 ms-delayed dry dominates).
        Bloom::Settings b;
        b.bitcrush = crush (12.0f, 24000.0f, false, 12000.0f, 0.5f);
        b.phaser.on = true;
        b.phaser.mix = 0.3f;
        b.flanger.on = true;
        b.flanger.manualMs = 2.5f;
        b.flanger.throughZero = true;
        b.flanger.mix = 0.4f;
        Bloom bloom;
        bloom.setSettings (b);
        bloom.prepare (fs, blockSize);
        const auto noise = whiteNoise ((int) fs, 0.3f, 41);
        const auto bloomOut = run (bloom, noise).left;
        int bestLag = 0;
        double bestCorrelation = -1.0e9;
        for (int lag = 0; lag < 1000; ++lag)
        {
            double sum = 0.0;
            for (size_t n = 2000; n + (size_t) lag < bloomOut.size(); ++n)
                sum += (double) bloomOut[n + (size_t) lag] * (double) noise[n];
            if (sum > bestCorrelation)
            {
                bestCorrelation = sum;
                bestLag = lag;
            }
        }
        expectEquals (bestLag, bloom.latencySamples());

        // Switching the mode off while running: after the 20 ms dip the latency is 0 and the dry is undelayed.
        b.flanger.throughZero = false;
        b.mix = 0.0f;
        bloom.setSettings (b);
        const auto after = run (bloom, noise).left;
        expectEquals (bloom.latencySamples(), 0);
        bool undelayed = true;
        for (size_t n = (size_t) (0.1 * fs); n < noise.size(); ++n)
            undelayed = undelayed && juce::exactlyEqual (after[n], noise[n]);
        expect (undelayed);

        logMessage ("  -> held at the crossing (Manual 5 ms, depth 0, negative, mix 50%): largest output sample " + juce::String (residual)
                    + " over 2 s of guitar DI (total cancellation, bit-exact); positive polarity there gives the DI exactly 240 samples late: "
                    + (delayedExactly ? "true" : "false"));
        logMessage ("  -> swept through zero (300 Hz tone, tau between -2.25 and +2.25 ms at 0.25 Hz): the one-cycle level follows |sin(pi f tau)| within "
                    + juce::String (worstTrack, 3) + " dB wherever it's above -30 dB (limit 0.5); at each crossing " + crossings.joinIntoString (", ")
                    + " (limit -40 dB)");
        logMessage ("  -> latency: the flanger reports " + juce::String (probe.latencySamples()) + " samples (" + juce::String (1000.0 * probe.latencySamples() / fs, 3)
                    + " ms) in through-zero mode and 0 otherwise, and an impulse's dry arrives " + juce::String (measuredFlanger)
                    + " samples late; Bloom with all three on reports " + juce::String (240) + " and its output correlates best with the input at lag "
                    + juce::String (bestLag) + "; switched off mid-stream it reports " + juce::String (bloom.latencySamples()) + " and its dry is undelayed again: "
                    + (undelayed ? "true" : "false"));
        logMessage ("  -> " + png.getFullPathName());
    }

    // ---- Container --------------------------------------------------------------------------------

    static Bloom::Settings everythingOn()
    {
        Bloom::Settings s;
        s.bitcrush = crush (6.5f, 11025.0f, true, 7000.0f, 0.8f);
        s.phaser.on = true;
        s.phaser.mode = Phaser::Mode::modern;
        s.phaser.stages = 6;
        s.phaser.feedback = 0.5f;
        s.phaser.rateHz = 0.7f;
        s.flanger.on = true;
        s.flanger.feedback = 0.6f;
        s.flanger.shape = Lfo::Shape::sine;
        s.flanger.rateHz = 0.3f;
        return s;
    }

    void containerMix()
    {
        beginTest ("container: 0% mix is bit-exact, and the mix law is linear");

        const auto di = guitarDI ((int) (3.0 * fs));

        // 0%, everything on (dither too), through-zero off and on.
        auto s = everythingOn();
        s.mix = 0.0f;
        Bloom zero;
        zero.setSettings (s);
        zero.prepare (fs, blockSize);
        const auto passthrough = run (zero, di);
        const auto worstPassthrough = std::max (maxAbsDifference (passthrough.left, di), maxAbsDifference (passthrough.right, di));
        expectEquals (worstPassthrough, 0.0);

        s.flanger.throughZero = true;
        Bloom delayed;
        delayed.setSettings (s);
        delayed.prepare (fs, blockSize);
        const auto late = run (delayed, di).left;
        bool delayedExactly = true;
        for (size_t n = 0; n < di.size(); ++n)
            delayedExactly = delayedExactly && juce::exactlyEqual (late[n], n >= 240 ? di[n - 240] : 0.0f);
        expect (delayedExactly);

        // The law: out = (1 - mix) dry + mix effects, against the 100% render (dither off so runs repeat exactly).
        auto base = everythingOn();
        base.bitcrush.dither = false;
        base.mix = 1.0f;
        Bloom full;
        full.setSettings (base);
        full.prepare (fs, blockSize);
        const auto wet = run (full, di);
        juce::StringArray rows;
        double worst = 0.0;
        for (const auto mix : { 0.25f, 0.5f, 0.75f })
        {
            auto m = base;
            m.mix = mix;
            Bloom b;
            b.setSettings (m);
            b.prepare (fs, blockSize);
            const auto out = run (b, di);
            double error = 0.0;
            for (size_t n = 0; n < di.size(); ++n)
            {
                const auto expectedL = (1.0 - (double) mix) * di[n] + (double) mix * wet.left[n];
                const auto expectedR = (1.0 - (double) mix) * di[n] + (double) mix * wet.right[n];
                error = std::max ({ error, std::abs (out.left[n] - expectedL), std::abs (out.right[n] - expectedR) });
            }
            worst = std::max (worst, error);
            rows.add (juce::String ((int) (100.0f * mix)) + "% " + juce::String (error, 9));
        }
        expectLessThan (worst, 1.0e-6);

        logMessage ("  -> mix 0% with all three on (dither, feedback, 6-stage phaser): largest difference from the input " + juce::String (worstPassthrough)
                    + " over 3 s (bit-exact); with through-zero on, the input exactly 240 samples late: " + (delayedExactly ? "true" : "false"));
        logMessage ("  -> linear law, out = (1 - mix) dry + mix effects, largest difference from it: " + rows.joinIntoString (", ")
                    + " (limit 1e-6, float rounding)");
    }

    void containerClicks()
    {
        beginTest ("container: order changes, every on/off, bypass, through-zero, and mode, stage, and knob changes don't click");

        // A low two-note chord (220 + 330 Hz) through all three, set so the steady output has little above 5 kHz
        // (the crush has no hold and its tone at 1 kHz), while the order still matters (the crush is nonlinear).
        // A click is broadband and stands out above 5 kHz. Both LFOs run at 2 Hz and changes come 1.5 s apart,
        // so the steady stretch on each side of a change spans whole LFO cycles and holds their loudest moment.
        Bloom::Settings base;
        base.bitcrush = crush (6.0f, (float) fs, false, 1000.0f, 0.5f);
        base.phaser.on = true;
        base.phaser.rateHz = 2.0f;
        base.flanger.on = true;
        base.flanger.manualMs = 2.0f;
        base.flanger.depth = 0.4f;
        base.flanger.rateHz = 2.0f;
        base.flanger.feedback = 0.3f;

        using E = Bloom::Effect;
        struct Change
        {
            const char* name;
            std::function<void (Bloom::Settings&, bool&)> apply;
        };
        const std::vector<Change> changes {
            { "order phaser, crush, flanger", [] (Bloom::Settings& s, bool&) { s.order = { E::phaser, E::bitcrush, E::flanger }; } },
            { "order flanger, phaser, crush", [] (Bloom::Settings& s, bool&) { s.order = { E::flanger, E::phaser, E::bitcrush }; } },
            { "order crush, flanger, phaser", [] (Bloom::Settings& s, bool&) { s.order = { E::bitcrush, E::flanger, E::phaser }; } },
            { "order phaser, flanger, crush", [] (Bloom::Settings& s, bool&) { s.order = { E::phaser, E::flanger, E::bitcrush }; } },
            { "order flanger, crush, phaser", [] (Bloom::Settings& s, bool&) { s.order = { E::flanger, E::bitcrush, E::phaser }; } },
            { "order back to default", [] (Bloom::Settings& s, bool&) { s.order = Bloom::defaultOrder; } },
            { "bitcrush off", [] (Bloom::Settings& s, bool&) { s.bitcrush.on = false; } },
            { "bitcrush on", [] (Bloom::Settings& s, bool&) { s.bitcrush.on = true; } },
            { "phaser off", [] (Bloom::Settings& s, bool&) { s.phaser.on = false; } },
            { "phaser on", [] (Bloom::Settings& s, bool&) { s.phaser.on = true; } },
            { "flanger off", [] (Bloom::Settings& s, bool&) { s.flanger.on = false; } },
            { "flanger on", [] (Bloom::Settings& s, bool&) { s.flanger.on = true; } },
            { "Bloom bypassed", [] (Bloom::Settings&, bool& bypass) { bypass = true; } },
            { "Bloom on", [] (Bloom::Settings&, bool& bypass) { bypass = false; } },
            { "mix 100 -> 30% at once", [] (Bloom::Settings& s, bool&) { s.mix = 0.3f; } },
            { "mix back to 100%", [] (Bloom::Settings& s, bool&) { s.mix = 1.0f; } },
            { "through-zero on", [] (Bloom::Settings& s, bool&) { s.flanger.throughZero = true; } },
            { "through-zero off", [] (Bloom::Settings& s, bool&) { s.flanger.throughZero = false; } },
            { "phaser Classic -> Modern 12 stages", [] (Bloom::Settings& s, bool&) { s.phaser.mode = Phaser::Mode::modern; s.phaser.stages = 12; } },
            { "phaser 12 -> 2 stages", [] (Bloom::Settings& s, bool&) { s.phaser.stages = 2; } },
            { "phaser -> Vibe", [] (Bloom::Settings& s, bool&) { s.phaser.mode = Phaser::Mode::vibe; } },
            { "phaser -> Classic with feedback", [] (Bloom::Settings& s, bool&) { s.phaser.mode = Phaser::Mode::classic; s.phaser.classicFeedback = true; } },
            { "flanger triangle -> sine", [] (Bloom::Settings& s, bool&) { s.flanger.shape = Lfo::Shape::sine; } },
            { "flanger stereo phase 90 -> 180 degrees", [] (Bloom::Settings& s, bool&) { s.flanger.stereoPhase = 0.5f; } },
            { "flanger polarity negative", [] (Bloom::Settings& s, bool&) { s.flanger.negative = true; } },
            { "flanger Manual 2 -> 6 ms at once", [] (Bloom::Settings& s, bool&) { s.flanger.manualMs = 6.0f; } },
            { "flanger feedback 0.3 -> -0.8 at once", [] (Bloom::Settings& s, bool&) { s.flanger.feedback = -0.8f; } },
            { "bits 6 -> 4 at once", [] (Bloom::Settings& s, bool&) { s.bitcrush.bits = 4.0f; } },
            { "crush tone 1 -> 3 kHz at once", [] (Bloom::Settings& s, bool&) { s.bitcrush.toneHz = 3000.0f; } },
            { "phaser rate 2 -> 4 Hz, depth 1 -> 0.3 at once", [] (Bloom::Settings& s, bool&) { s.phaser.rateHz = 4.0f; s.phaser.depth = 0.3f; } },
        };

        const auto spacing = 1.5, first = 1.0;
        const auto seconds = first + spacing * (double) changes.size() + 1.5;
        std::vector<float> chord ((size_t) (seconds * fs));
        for (size_t n = 0; n < chord.size(); ++n)
            chord[n] = (float) (0.35 * std::sin (twoPi * 220.0 * (double) n / fs) + 0.2 * std::sin (twoPi * 330.0 * (double) n / fs + 0.5));

        Bloom bloom;
        auto s = base;
        bool bypassed = false;
        bloom.setSettings (s);
        bloom.prepare (fs, blockSize);
        size_t next = 0;
        const auto out = run (bloom, chord, blockSize, [&] (size_t start)
        {
            while (next < changes.size() && (double) start >= (first + spacing * (double) next) * fs)
                changes[next++].apply (s, bypassed);
            bloom.setSettings (s);
            bloom.setBypassed (bypassed);
        }).left;

        const auto residual = highPassed (out);
        juce::StringArray rows;
        double worst = 0.0;
        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto at = (size_t) std::ceil ((first + spacing * (double) i) * fs / blockSize) * blockSize; // the buffer that sees it
            const auto around = peakAbs (residual, at - 256, at + (size_t) (0.1 * fs));
            const auto steady = std::max (peakAbs (residual, at - (size_t) (0.75 * fs), at - 480), peakAbs (residual, at + (size_t) (0.2 * fs), at + (size_t) (1.45 * fs)));
            const auto ratio = around / steady;
            worst = std::max (worst, ratio);
            size_t peakAt = at - 256;
            for (size_t n = at - 256; n < at + (size_t) (0.1 * fs); ++n)
                if (std::abs (residual[n]) > std::abs (residual[peakAt]))
                    peakAt = n;
            rows.add (juce::String (changes[i].name) + " x" + juce::String (ratio, 2)
                      + (ratio > 1.0 ? " (peak " + juce::String (1000.0 * ((double) peakAt - (double) at) / fs, 1) + " ms after)" : juce::String()));
        }

        // Positive controls, in the same setting: (1) a reorder with no dip, spliced from two fixed-order renders
        // where they differ most in a steady stretch; (2) one sample dropped where the output is steepest.
        const auto render = [&] (Bloom::Order order)
        {
            auto r = base;
            r.order = order;
            Bloom b;
            b.setSettings (r);
            b.prepare (fs, blockSize);
            return run (b, std::vector<float> (chord.begin(), chord.begin() + (long) (3.0 * fs))).left;
        };
        const auto a = render (Bloom::defaultOrder), b = render ({ E::flanger, E::phaser, E::bitcrush });
        const auto controlRatio = [] (const std::vector<float>& y, size_t at)
        {
            const auto r = highPassed (y);
            return peakAbs (r, at - 256, at + 256) / std::max (peakAbs (r, (size_t) (1.0 * fs), at - 480), peakAbs (r, at + 4800, (size_t) (2.9 * fs)));
        };
        size_t splice = (size_t) (1.5 * fs);
        for (size_t n = (size_t) (1.5 * fs); n < (size_t) (2.0 * fs); ++n)
            if (std::abs (a[n] - b[n]) > std::abs (a[splice] - b[splice]))
                splice = n;
        auto spliced = a;
        std::copy (b.begin() + (long) splice, b.end(), spliced.begin() + (long) splice);
        const auto spliceRatio = controlRatio (spliced, splice);

        auto dropped = a;
        size_t steepest = (size_t) (1.7 * fs);
        for (size_t n = (size_t) (1.7 * fs); n < (size_t) (1.72 * fs); ++n)
            if (std::abs (a[n + 1] - a[n]) > std::abs (a[steepest + 1] - a[steepest]))
                steepest = n;
        dropped.erase (dropped.begin() + (long) steepest);
        const auto dropRatio = controlRatio (dropped, steepest);

        expectLessThan (worst, 1.2);
        expectGreaterThan (spliceRatio, 3.0);
        expectGreaterThan (dropRatio, 3.0);
        expect (bloom.getAppliedOrder() == Bloom::defaultOrder && ! bloom.isReordering() && ! bloom.isSwitchingLatency());

        logMessage ("  -> peak above 5 kHz in the 100 ms after each change, as a multiple of the steady stretches either side (whole LFO cycles; limit 1.2): "
                    + rows.joinIntoString ("; "));
        logMessage ("  -> positive controls in the same setting: a reorder with no dip (two fixed-order renders spliced) x" + juce::String (spliceRatio, 1)
                    + ", one sample dropped x" + juce::String (dropRatio, 1) + " (each must exceed 3)");
    }

    void realtime()
    {
        beginTest ("real time: nothing allocates, frees, or locks while every setting, mode, stage count, order, and through-zero changes");

        Bloom bloom;
        bloom.setSettings ({});
        bloom.prepare (fs, blockSize);
        const auto di = guitarDI ((int) (10.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        using E = Bloom::Effect;
        const std::array<Bloom::Order, 6> orders { { { E::bitcrush, E::phaser, E::flanger }, { E::bitcrush, E::flanger, E::phaser },
                                                     { E::phaser, E::bitcrush, E::flanger }, { E::phaser, E::flanger, E::bitcrush },
                                                     { E::flanger, E::bitcrush, E::phaser }, { E::flanger, E::phaser, E::bitcrush } } };
        const std::array<Lfo::Shape, 3> shapes { Lfo::Shape::triangle, Lfo::Shape::sine, Lfo::Shape::random };
        const std::array<Phaser::Mode, 3> modes { Phaser::Mode::classic, Phaser::Mode::modern, Phaser::Mode::vibe };
        Bloom::Settings s;
        int blocks = 0, changes = 0, tzSwitches = 0, bypassToggles = 0;
        bool bypass = false, lastTz = false;

        rtcheck::begin();
        for (size_t start = 0; start + blockSize <= di.size(); start += blockSize, ++blocks)
        {
            if (blocks % 12 == 0 || blocks % 89 == 1) // regularly, and sometimes right after (mid-fade)
            {
                const auto k = blocks / 12 + blocks % 5;
                s.bitcrush.on = k % 3 != 0;
                s.bitcrush.bits = 1.0f + (float) (k % 31) * 0.5f;
                s.bitcrush.rateHz = 200.0f + 3100.0f * (float) (k % 16);
                s.bitcrush.dither = k % 2 == 0;
                s.bitcrush.toneHz = 1000.0f + 2100.0f * (float) (k % 10);
                s.bitcrush.mix = (float) (k % 4) / 3.0f;
                s.phaser.on = k % 4 != 1;
                s.phaser.mode = modes[(size_t) (k % 3)];
                s.phaser.stages = Phaser::stageChoices[(size_t) (k % 5)];
                s.phaser.rateHz = 0.05f + (float) (k % 10);
                s.phaser.depth = (float) (k % 5) / 4.0f;
                s.phaser.shape = shapes[(size_t) (k % 2)];
                s.phaser.lowHz = 50.0f + 40.0f * (float) (k % 7);
                s.phaser.highHz = 1000.0f + 1500.0f * (float) (k % 6);
                s.phaser.feedback = -0.9f + 0.3f * (float) (k % 7);
                s.phaser.classicFeedback = k % 2 == 1;
                s.phaser.stereoOffset = (float) (k % 3) / 4.0f;
                s.phaser.mix = (float) (k % 5) / 4.0f;
                s.flanger.on = k % 5 != 2;
                s.flanger.manualMs = 0.5f + (float) (k % 20) * 0.5f;
                s.flanger.depth = (float) (k % 6) / 5.0f;
                s.flanger.shape = shapes[(size_t) (k % 3)];
                s.flanger.rateHz = 0.05f + (float) (k % 8);
                s.flanger.feedback = -0.95f + 0.19f * (float) (k % 11);
                s.flanger.negative = k % 2 == 0;
                s.flanger.stereoPhase = (float) (k % 3) / 4.0f;
                s.flanger.mix = (float) (k % 4) / 3.0f;
                s.flanger.throughZero = (k / 4) % 2 == 1;
                s.order = orders[(size_t) (k % 6)];
                s.mix = (float) (k % 5) / 4.0f;
                tzSwitches += s.flanger.throughZero != lastTz ? 1 : 0;
                lastTz = s.flanger.throughZero;
                ++changes;
            }
            if (blocks % 37 == 5)
            {
                bypass = ! bypass;
                ++bypassToggles;
            }
            bloom.setSettings (s);
            bloom.setBypassed (bypass);
            buffer.copyFrom (0, 0, di.data() + start, blockSize);
            buffer.copyFrom (1, 0, di.data() + start, blockSize);
            bloom.process (juce::dsp::AudioBlock<float> (buffer), {});
        }
        const auto counts = rtcheck::end();

        expectEquals (counts.allocations, 0L);
        expectEquals (counts.frees, 0L);
        expectEquals (counts.blockingLocks, 0L);
        logMessage ("  -> " + juce::String (blocks) + " buffers, " + juce::String (changes) + " changes of every setting (modes, stage counts, shapes, orders, some mid-fade), "
                    + juce::String (tzSwitches) + " through-zero switches, " + juce::String (bypassToggles) + " bypass toggles: " + juce::String (counts.allocations)
                    + " allocations, " + juce::String (counts.frees) + " frees, " + juce::String (counts.blockingLocks) + " blocking locks");
    }

    void cpu()
    {
        beginTest ("CPU: a 128-sample stereo buffer with all three on");

        const auto di = guitarDI ((int) (10.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::StringArray results;
        double worstMean = 0.0;

        const auto time = [&] (Bloom::Settings s, const juce::String& label, const std::function<void (Bloom::Settings&, int)>& each = {})
        {
            Bloom b;
            b.setSettings (s);
            b.prepare (fs, blockSize);
            std::vector<double> micros;
            int block = 0;
            for (size_t start = 0; start + blockSize <= di.size(); start += blockSize, ++block)
            {
                if (each)
                {
                    each (s, block);
                    b.setSettings (s);
                }
                buffer.copyFrom (0, 0, di.data() + start, blockSize);
                buffer.copyFrom (1, 0, di.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                b.process (juce::dsp::AudioBlock<float> (buffer), {});
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            std::sort (micros.begin(), micros.end());
            double mean = 0.0;
            for (auto m : micros)
                mean += m;
            mean /= (double) micros.size();
            worstMean = std::max (worstMean, mean);
            const auto p99 = micros[(size_t) (0.99 * (double) micros.size())];
            results.add (label + " mean " + juce::String (mean, 2) + " us (" + juce::String (100.0 * mean / deadlineMicros, 2) + "%), p99 " + juce::String (p99, 2) + " us");
        };

        auto all = everythingOn();
        all.phaser.stages = 12;
        all.phaser.feedback = 0.7f;
        all.flanger.feedback = 0.8f;
        time (all, "crush (dither, tone) + Modern 12 stages + flanger");

        auto vibe = all;
        vibe.phaser.mode = Phaser::Mode::vibe;
        time (vibe, "crush + Vibe + flanger");

        auto tz = all;
        tz.flanger.throughZero = true;
        time (tz, "crush + Modern 12 + through-zero flanger");

        // Worst case: the phaser crossfading banks (Modern 12 <-> Vibe) and the flanger crossfading pairs, all the time.
        time (all, "constant phaser and flanger crossfades", [] (Bloom::Settings& s, int block)
        {
            if (block % 12 == 0)
            {
                s.phaser.mode = s.phaser.mode == Phaser::Mode::vibe ? Phaser::Mode::modern : Phaser::Mode::vibe;
                s.flanger.shape = s.flanger.shape == Lfo::Shape::sine ? Lfo::Shape::triangle : Lfo::Shape::sine;
            }
        });

        expectLessThan (worstMean, 0.10 * deadlineMicros);
        logMessage ("  -> 10 s of guitar DI, of the 2.67 ms deadline: " + results.joinIntoString ("; "));
    }

    void renders()
    {
        beginTest ("renders for listening (proof directory)");

        const auto di = guitarDI ((int) (6.0 * fs));
        expect (writeWav (proofDir().getChildFile ("bloom_dry.wav"), di));

        struct Render
        {
            const char* file;
            Bloom::Settings settings;
        };
        Bloom::Settings crushOnly;
        crushOnly.bitcrush = crush (8.0f, 11025.0f);
        Bloom::Settings classic;
        classic.phaser.on = true;
        Bloom::Settings vibe;
        vibe.phaser.on = true;
        vibe.phaser.mode = Phaser::Mode::vibe;
        vibe.phaser.rateHz = 3.0f;
        Bloom::Settings flanger;
        flanger.flanger.on = true;
        Bloom::Settings jet;
        jet.flanger.on = true;
        jet.flanger.manualMs = 1.5f;
        jet.flanger.depth = 0.9f;
        jet.flanger.feedback = -0.8f;
        jet.flanger.negative = true;
        Bloom::Settings tape;
        tape.flanger.on = true;
        tape.flanger.manualMs = 5.0f;
        tape.flanger.depth = 0.5f;
        tape.flanger.shape = Lfo::Shape::sine;
        tape.flanger.negative = true;
        tape.flanger.throughZero = true;
        const std::vector<Render> renders { { "bloom_crush.wav", crushOnly },        { "bloom_phaser_classic.wav", classic },
                                            { "bloom_phaser_vibe.wav", vibe },       { "bloom_flanger.wav", flanger },
                                            { "bloom_flanger_negative_jet.wav", jet }, { "bloom_flanger_through_zero.wav", tape },
                                            { "bloom_all_three.wav", everythingOn() } };
        juce::StringArray files;
        for (const auto& r : renders)
        {
            Bloom b;
            b.setSettings (r.settings);
            b.prepare (fs, blockSize);
            const auto out = run (b, di);
            juce::AudioBuffer<float> stereo (2, (int) di.size());
            stereo.copyFrom (0, 0, out.left.data(), (int) di.size());
            stereo.copyFrom (1, 0, out.right.data(), (int) di.size());
            expect (writeWav (proofDir().getChildFile (r.file), stereo));
            files.add (r.file);
        }
        logMessage ("  -> the synthetic guitar DI through Bloom, stereo: " + files.joinIntoString (", ") + " (plus bloom_dry.wav) in " + proofDir().getFullPathName()
                    + ". Unverified by ear: Sean's to judge.");
    }
};

BloomTests bloomTests;
} // namespace

#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Chorus.h"
#include "dsp/ModulatedDelay.h"
#include "dsp/Svf.h"

#include <chrono>
#include <complex>

namespace
{
using namespace testing;
using ampsim::Chorus;
using ampsim::Lfo;
using ampsim::ModulatedDelay;

constexpr double twoPi = juce::MathConstants<double>::twoPi;

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/chorus").getChildFile (name);
}

const char* modeName (Chorus::Mode mode) { return Chorus::modeSpecs[(size_t) mode].name; }

/// Nothing but the wet: Classic, mix 1, analog and the high-pass off. Pure vibrato.
Chorus::Settings vibrato (Lfo::Shape shape, float rateHz, float depth)
{
    Chorus::Settings s;
    s.mode = Chorus::Mode::classic;
    s.shape = shape;
    s.rateHz = rateHz;
    s.depth = depth;
    s.mix = 1.0f;
    s.analog = false;
    s.wetHighPass = false;
    return s;
}

Chorus::Settings defaultsFor (Chorus::Mode mode)
{
    Chorus::Settings s;
    s.mode = mode;
    return s;
}

/// Runs a stereo signal through the chorus in fixed-size buffers. `before` runs ahead of each buffer with
/// its first sample's index, to change settings mid-stream.
Stereo run (Chorus& c, const std::vector<float>& left, const std::vector<float>& right, int bufferSize = blockSize,
            const std::function<void (size_t)>& before = {})
{
    Stereo out { left, right };
    for (size_t start = 0; start < left.size(); start += (size_t) bufferSize)
    {
        if (before)
            before (start);
        const auto len = std::min ((size_t) bufferSize, left.size() - start);
        float* channels[2] = { out.left.data() + start, out.right.data() + start };
        c.process (juce::dsp::AudioBlock<float> (channels, 2, len), {});
    }
    return out;
}

Stereo run (Chorus& c, const std::vector<float>& mono, int bufferSize = blockSize, const std::function<void (size_t)>& before = {})
{
    return run (c, mono, mono, bufferSize, before);
}

struct PitchPoint
{
    double seconds, cents;
};

/// The frequency of each cycle of y between upward zero crossings (located by linear interpolation, which
/// is very accurate on a sine because its curvature vanishes at the crossing), in cents relative to f0.
std::vector<PitchPoint> cyclePitch (const std::vector<float>& y, double f0, size_t start, size_t end = SIZE_MAX)
{
    std::vector<PitchPoint> points;
    double previous = -1.0;
    for (size_t n = std::max<size_t> (start, 1); n < std::min (end, y.size()); ++n)
    {
        if (y[n - 1] < 0.0f && y[n] >= 0.0f)
        {
            const auto crossing = (double) (n - 1) - (double) y[n - 1] / ((double) y[n] - (double) y[n - 1]);
            if (previous >= 0.0)
                points.push_back ({ 0.5 * (crossing + previous) / fs, 1200.0 * std::log2 (fs / (crossing - previous) / f0) });
            previous = crossing;
        }
    }
    return points;
}

std::pair<double, double> centsRange (const std::vector<PitchPoint>& points)
{
    double lo = 1.0e9, hi = -1.0e9;
    for (const auto& p : points)
    {
        lo = std::min (lo, p.cents);
        hi = std::max (hi, p.cents);
    }
    return { lo, hi };
}

/// The phase in degrees of x's component at `cyclesPerSample`, from the DFT over x's length (a whole number
/// of cycles). Only differences between traces are meaningful.
double phaseDegrees (const std::vector<double>& x, double cyclesPerSample)
{
    double mean = 0.0;
    for (auto v : x)
        mean += v;
    mean /= (double) x.size();

    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < x.size(); ++n)
        sum += (x[n] - mean) * std::polar (1.0, -twoPi * cyclesPerSample * (double) n);
    return std::arg (sum) * 180.0 / juce::MathConstants<double>::pi;
}

double wrapDegrees (double d)
{
    d = std::fmod (d, 360.0);
    return d < 0.0 ? d + 360.0 : d;
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

double binHz (size_t k) { return (double) k * fs / 8192.0; }

/// The amplitude of y's component at f over `length` samples from `start` (a whole number of f's cycles,
/// so the DFT bin is exact).
double toneAmplitude (const std::vector<float>& y, double f, size_t start, size_t length)
{
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < length; ++n)
        sum += (double) y[start + n] * std::polar (1.0, -twoPi * f * (double) n / fs);
    return 2.0 * std::abs (sum) / (double) length;
}

/// Fourier sine coefficient k of tanh(a sin(theta)): what light saturation does to a sine.
double tanhHarmonic (double a, int k)
{
    constexpr int steps = 48000;
    double sum = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const auto theta = twoPi * (i + 0.5) / steps;
        sum += std::tanh (a * std::sin (theta)) * std::sin (k * theta);
    }
    return 2.0 * sum / steps;
}

Chorus::Settings settingsFromJson (const juce::var& v)
{
    Chorus::Settings s;
    const auto mode = v["mode"].toString();
    s.mode = mode == "dimension" ? Chorus::Mode::dimension : (mode == "tri" ? Chorus::Mode::tri : Chorus::Mode::classic);
    s.shape = v["shape"].toString() == "sine" ? Lfo::Shape::sine : Lfo::Shape::triangle;
    s.rateHz = (float) (double) v["rateHz"];
    s.depth = (float) (double) v["depth"];
    s.mix = (float) (double) v["mix"];
    s.width = (float) (double) v["width"];
    s.analog = (bool) v["analog"];
    s.noise = (bool) v["noise"];
    s.wetHighPass = (bool) v["wetHighPass"];
    s.wetHighPassHz = (float) (double) v["wetHighPassHz"];
    return s;
}

ModulatedDelay::Settings engineFromJson (const juce::var& v)
{
    ModulatedDelay::Settings s;
    const auto* voices = v["voices"].getArray();
    s.numVoices = voices->size();
    for (int i = 0; i < voices->size(); ++i)
    {
        const auto& in = voices->getReference (i);
        auto& voice = s.voices[(size_t) i];
        voice.baseDelayMs = (double) in["base_ms"];
        voice.depthMs = (double) in["depth_ms"];
        voice.shape = in["shape"].toString() == "sine" ? Lfo::Shape::sine : Lfo::Shape::triangle;
        voice.rateHz = (double) in["rate"];
        voice.phase = (double) in["phase"];
        voice.inverted = (bool) in["inverted"];
        voice.level = (double) in["level"];
    }
    s.feedback = (double) v["feedback"];
    return s;
}

std::vector<float> channel (const juce::AudioBuffer<float>& b, int ch)
{
    return { b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples() };
}

class ChorusTests final : public juce::UnitTest
{
public:
    ChorusTests() : juce::UnitTest ("Chorus", "ampsim") {}

    void runTest() override
    {
        golden();
        depth();
        lfoPhases();
        wetHighPass();
        monoSum();
        interpolation();
        zipper();
        dryPath();
        modeSwitch();
        analogCharacter();
        engine();
        realtime();
        cpu();
        renders();
    }

private:
    void golden()
    {
        beginTest ("matches the Python reference sample by sample (golden renders, limit -100 dB)");

        const auto cases = juce::JSON::parse (fixture ("cases.json"));
        const auto input = readWav (fixture ("input_stereo.wav"));
        expectEquals (input.getNumChannels(), 2);
        const auto left = channel (input, 0), right = channel (input, 1);
        juce::StringArray results;

        for (const auto* name : { "classic", "dimension", "tri", "tri_analog" })
        {
            const auto expected = readWav (fixture ("expected_" + juce::String (name) + ".wav"));
            Chorus c;
            c.setSettings (settingsFromJson (cases["chorus"][name]));
            c.prepare (fs, blockSize);
            const auto out = run (c, left, right);
            const auto worst = std::max (relativeErrorDb (out.left, channel (expected, 0)), relativeErrorDb (out.right, channel (expected, 1)));
            expectLessThan (worst, -100.0);
            results.add (juce::String (name) + " " + dB (worst));
        }

        // The bare engine: three voices of different shapes, rates, offsets, and polarities, with feedback.
        {
            const auto spec = cases["engine"];
            const auto expected = channel (readWav (fixture ("expected_engine_feedback.wav")), 0);
            ModulatedDelay e;
            e.setSettings (engineFromJson (spec));
            e.prepare (fs, (double) spec["max_delay_ms"], 0.1);
            std::vector<float> wet (left.size());
            for (size_t n = 0; n < left.size(); ++n)
                wet[n] = e.processSample (left[n]);
            const auto error = relativeErrorDb (wet, expected);
            expectLessThan (error, -100.0);
            results.add ("engine with 0.6 feedback " + dB (error));
        }
        logMessage ("  -> C++ vs prototypes/chorus.py (tests/fixtures/chorus, 1 s stereo, analog off except tri_analog): "
                    + results.joinIntoString ("; "));
    }

    void depth()
    {
        beginTest ("depth: the pitch swing of a sine through the wet path matches the Doppler formula f (1 - d')");

        struct Case
        {
            Lfo::Shape shape;
            float rate, depth;
        };
        const auto tone = sine (1000.0, 0.5, (int) (4.5 * fs));
        juce::StringArray results;
        double worst = 0.0;
        std::vector<PlotSeries> series;
        int colour = 0;

        for (const auto& k : { Case { Lfo::Shape::triangle, 1.0f, 0.5f }, Case { Lfo::Shape::triangle, 3.0f, 1.0f },
                               Case { Lfo::Shape::sine, 0.5f, 1.0f }, Case { Lfo::Shape::sine, 2.0f, 0.3f } })
        {
            Chorus c;
            c.setSettings (vibrato (k.shape, k.rate, k.depth));
            c.prepare (fs, blockSize);
            const auto out = run (c, tone);

            // Depth in seconds (Classic sweeps up to +-4 ms), and the formula's swing.
            const auto a = (double) k.depth * Chorus::modeSpecs[0].maxDepthMs / 1000.0;
            const auto swing = (k.shape == Lfo::Shape::triangle ? 4.0 : twoPi) * (double) k.rate * a;
            const auto upTheory = 1200.0 * std::log2 (1.0 + swing), downTheory = 1200.0 * std::log2 (1.0 - swing);

            const auto points = cyclePitch (out.left, 1000.0, (size_t) (0.25 * fs));
            const auto [down, up] = centsRange (points);
            worst = std::max ({ worst, std::abs (up - upTheory), std::abs (down - downTheory) });
            results.add (juce::String (k.shape == Lfo::Shape::triangle ? "triangle " : "sine ") + juce::String (k.rate, 1) + " Hz "
                         + juce::String (a * 1000.0, 1) + " ms: +" + juce::String (up, 3) + "/" + juce::String (down, 3) + " cents (theory +"
                         + juce::String (upTheory, 3) + "/" + juce::String (downTheory, 3) + ")");

            if (k.rate < 1.5f)
            {
                PlotSeries measured { results[results.size() - 1].upToFirstOccurrenceOf (":", false, false) + " measured", {}, {}, plotColour (colour), 2.5f };
                PlotSeries theory { "formula", {}, {}, plotColour (6), 1.2f, true };
                for (const auto& p : points)
                {
                    measured.x.push_back (p.seconds);
                    measured.y.push_back (p.cents);
                }
                for (double t = 0.25; t < 4.5; t += 0.001)
                {
                    // d(t) = base + A * LFO(rate t); f_out / f = 1 - d'(t).
                    const auto phase = (double) k.rate * t;
                    const auto slope = k.shape == Lfo::Shape::triangle
                                           ? a * 4.0 * k.rate * ((phase - std::floor (phase) < 0.25 || phase - std::floor (phase) >= 0.75) ? 1.0 : -1.0)
                                           : a * twoPi * k.rate * std::cos (twoPi * phase);
                    theory.x.push_back (t);
                    theory.y.push_back (1200.0 * std::log2 (1.0 - slope));
                }
                series.push_back (measured);
                series.push_back (theory); // drawn on top, so the dashes show where the two coincide
                ++colour;
            }
        }
        expectLessThan (worst, 0.05);

        PlotOptions o;
        o.title = "Pitch of a 1 kHz sine through the wet path (dashed: f (1 - d'(t)))";
        o.xLabel = "Time (s)";
        o.yLabel = "Pitch (cents)";
        o.xMin = 0.25; o.xMax = 4.5; o.yMin = -32.0; o.yMax = 32.0;
        const auto png = proofDir().getChildFile ("chorus_pitch.png");
        expect (savePlot (png, o, series));
        logMessage ("  -> each cycle's frequency from zero crossings, largest error " + juce::String (worst, 4) + " cents (limit 0.05): "
                    + results.joinIntoString ("; "));
        logMessage ("  -> " + png.getFullPathName());
    }

    void lfoPhases()
    {
        beginTest ("LFO phases: Classic's right side inverted, Dimension's pair 180 degrees apart, Tri's voices 120 degrees apart");

        // Rate 2 Hz: a cycle is exactly 24000 samples. Record every voice's delay, sample by sample.
        constexpr int cycle = 24000;
        juce::StringArray results;
        double worstPhase = 0.0, worstSwing = 0.0;
        std::vector<PlotSeries> series;

        for (auto mode : { Chorus::Mode::classic, Chorus::Mode::dimension, Chorus::Mode::tri })
        {
            auto s = defaultsFor (mode);
            s.rateHz = 2.0f;
            s.depth = 1.0f;
            Chorus c;
            c.setSettings (s);
            c.prepare (fs, blockSize);

            const auto perChannel = Chorus::modeSpecs[(size_t) mode].voicesPerChannel;
            std::vector<std::vector<double>> traces ((size_t) (2 * perChannel));
            float l = 0.0f, r = 0.0f;
            for (int n = 0; n < 3 * cycle; ++n)
            {
                l = r = 0.1f * (float) std::sin (0.01 * n);
                float* channels[2] = { &l, &r };
                c.process (juce::dsp::AudioBlock<float> (channels, 2, 1), {});
                for (int ch = 0; ch < 2; ++ch)
                    for (int v = 0; v < perChannel; ++v)
                        traces[(size_t) (ch * perChannel + v)].push_back (c.getVoiceDelaySamples (ch, v));
            }

            // Phases relative to the left side's first voice; the expected layout per mode.
            std::vector<double> phases;
            for (auto& t : traces)
            {
                const std::vector<double> lastCycles (t.end() - 2 * cycle, t.end());
                phases.push_back (wrapDegrees (phaseDegrees (lastCycles, 1.0 / cycle) - phaseDegrees (std::vector<double> (traces[0].end() - 2 * cycle, traces[0].end()), 1.0 / cycle)));
                const auto [lo, hi] = std::minmax_element (lastCycles.begin(), lastCycles.end());
                const auto expectedSwing = 2.0 * Chorus::modeSpecs[(size_t) mode].maxDepthMs * fs / 1000.0;
                worstSwing = std::max (worstSwing, std::abs ((*hi - *lo) - expectedSwing));
            }

            std::vector<double> expected;
            juce::String layout;
            if (mode == Chorus::Mode::tri)
            {
                expected = { 0.0, 120.0, 120.0, 240.0 }; // left: voices 0 and 1; right: 1 and 2
                layout = "left " + juce::String (phases[0], 3) + "/" + juce::String (phases[1], 3) + ", right " + juce::String (phases[2], 3) + "/"
                         + juce::String (phases[3], 3) + " (expected 0/120, 120/240)";
            }
            else
            {
                expected = { 0.0, 180.0 };
                layout = "right " + juce::String (phases[1], 3) + " (expected 180)";
            }
            for (size_t i = 0; i < phases.size(); ++i)
            {
                auto error = std::abs (phases[i] - expected[i]);
                error = std::min (error, 360.0 - error);
                worstPhase = std::max (worstPhase, error);
            }
            results.add (juce::String (modeName (mode)) + ": " + layout);

            for (size_t i = 0; i < traces.size(); ++i)
            {
                // Tri: coloured by which of the three voices it is, so the shared centre voice shows as one.
                const auto right = i >= (size_t) perChannel;
                const auto colour = mode == Chorus::Mode::tri ? std::array<int, 3> { 4, 5, 7 }[(size_t) ((int) right + (int) i % perChannel)]
                                                              : (int) mode * 2 + (int) right;
                PlotSeries p { juce::String (modeName (mode)) + (right ? " R" : " L") + juce::String ((int) i % perChannel), {}, {},
                               plotColour (colour), 2.0f, right };
                for (int n = cycle; n < 3 * cycle; n += 48)
                {
                    p.x.push_back ((double) (n - cycle) / fs * 1000.0);
                    p.y.push_back (traces[i][(size_t) n] / fs * 1000.0);
                }
                series.push_back (p);
            }
        }
        expectLessThan (worstPhase, 1.0e-6);
        expectLessThan (worstSwing, 1.0e-6);

        // The audio really gets those delays: Classic, an impulse every 1000 samples, the centroid of each
        // echo against the reported delay. Hermite reproduces straight lines, so an echo's centroid sits
        // exactly at a constant delay; a moving one smears it by a few hundredths of a sample.
        double worstEcho = 0.0;
        int echoes = 0;
        {
            Chorus c;
            c.setSettings (vibrato (Lfo::Shape::triangle, 2.0f, 1.0f));
            c.prepare (fs, blockSize);
            std::vector<float> impulses ((size_t) (2 * cycle), 0.0f);
            for (size_t n = 0; n < impulses.size(); n += 1000)
                impulses[n] = 1.0f;

            std::vector<double> reportedL, reportedR;
            Stereo out { impulses, impulses };
            for (size_t n = 0; n < impulses.size(); ++n)
            {
                float* channels[2] = { out.left.data() + n, out.right.data() + n };
                c.process (juce::dsp::AudioBlock<float> (channels, 2, 1), {});
                reportedL.push_back (c.getVoiceDelaySamples (0, 0));
                reportedR.push_back (c.getVoiceDelaySamples (1, 0));
            }
            for (size_t start = 1000; start + 1000 <= impulses.size(); start += 1000)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto& y = ch == 0 ? out.left : out.right;
                    double weight = 0.0, moment = 0.0;
                    for (size_t n = start + 300; n < start + 800; ++n)
                    {
                        weight += y[n];
                        moment += (double) (n - start) * y[n];
                    }
                    const auto centroid = moment / weight;
                    const auto at = start + (size_t) std::lround (centroid);
                    worstEcho = std::max (worstEcho, std::abs (centroid - (ch == 0 ? reportedL : reportedR)[at]));
                    ++echoes;
                }
            }
        }
        expectLessThan (worstEcho, 0.1);

        PlotOptions o;
        o.title = "Every voice's delay over two LFO cycles at depth 100% (solid: left, dashed: right)";
        o.xLabel = "Time (ms)";
        o.yLabel = "Delay (ms)";
        o.xMin = 0.0; o.xMax = 1000.0; o.yMin = 5.0; o.yMax = 16.0;
        const auto png = proofDir().getChildFile ("chorus_voice_delays.png");
        expect (savePlot (png, o, series));
        logMessage ("  -> phase of each voice's delay sweep at the LFO rate, relative to the left's first voice (degrees): "
                    + results.joinIntoString ("; ") + "; largest error " + juce::String (worstPhase, 9) + " deg; every sweep spans 2 x the mode's maximum depth to "
                    + juce::String (worstSwing, 9) + " samples");
        logMessage ("  -> the audio gets the reported delay: " + juce::String (echoes) + " impulse echoes in Classic (both sides), centroid within "
                    + juce::String (worstEcho, 4) + " samples of the reported delay (limit 0.1)");
        logMessage ("  -> " + png.getFullPathName());
    }

    void wetHighPass()
    {
        beginTest ("wet high-pass: low notes pass at full level with their pitch steady");

        juce::StringArray results;
        for (const auto frequency : { 82.41, 61.74 }) // low E, and a 7-string's low B
        {
            const auto tone = sine (frequency, 0.5, (int) (5.0 * fs));
            double wobble[2] {}, level[2] {};
            for (int on = 0; on < 2; ++on)
            {
                auto s = defaultsFor (Chorus::Mode::classic);
                s.rateHz = 2.0f; // a strong chorus: 4 x 2 Hz x 2 ms = 1.6% (27.6 cents) on the wet
                s.analog = false;
                s.wetHighPass = on == 1;
                Chorus c;
                c.setSettings (s);
                c.prepare (fs, blockSize);
                const auto out = run (c, tone).left;
                const auto [lo, hi] = centsRange (cyclePitch (out, frequency, (size_t) fs));
                wobble[on] = std::max (std::abs (lo), std::abs (hi));
                level[on] = toDb (rms (out.data() + (size_t) fs, out.size() - (size_t) fs) / rms (tone));
            }
            expectLessThan (wobble[1], 0.15 * wobble[0]);
            expectLessThan (wobble[1], 2.0);
            expectWithinAbsoluteError (level[1], 0.0, 1.0);
            results.add (juce::String (frequency, 1) + " Hz: pitch swing +-" + juce::String (wobble[1], 2) + " cents and level " + juce::String (level[1], 2)
                         + " dB with the high-pass on; +-" + juce::String (wobble[0], 2) + " cents and " + juce::String (level[0], 2) + " dB with it off");
        }
        logMessage ("  -> Classic, mix 50%, depth 50% (2 ms), rate 2 Hz (the wet alone swings +-27.6 cents), high-pass at 150 Hz (limits: under 2 cents, "
                    "under 15% of the swing without it, level within 1 dB): " + results.joinIntoString ("; "));
    }

    void monoSum()
    {
        beginTest ("mono sum: summing left and right adds no deep cancellation");

        const auto noise = whiteNoise ((int) (20.0 * fs), 0.5f, 11);
        const auto dry = powerSpectrum (noise);
        juce::StringArray results;
        std::vector<PlotSeries> series;

        const auto measure = [&] (Chorus::Settings s, const juce::String& label, bool check, int colour)
        {
            Chorus c;
            c.setSettings (s);
            c.prepare (fs, blockSize);
            const auto out = run (c, noise);
            std::vector<float> mono (noise.size());
            for (size_t n = 0; n < mono.size(); ++n)
                mono[n] = 0.5f * (out.left[n] + out.right[n]);
            const auto pl = powerSpectrum (out.left), pr = powerSpectrum (out.right), pm = powerSpectrum (mono);

            double deepestMono = 0.0, deepestSide = 0.0, worstLoss = 0.0, monoHz = 0.0, lossHz = 0.0;
            double highMono = 0.0;
            int highBins = 0;
            PlotSeries curve { label, {}, {}, plotColour (colour), 1.6f };
            for (size_t k = 1; k < dry.size(); ++k)
            {
                const auto f = binHz (k);
                if (f < 100.0 || f > 10000.0)
                    continue;
                const auto monoDb = 10.0 * std::log10 (pm[k] / dry[k]);
                const auto sideDb = 10.0 * std::log10 (std::min (pl[k], pr[k]) / dry[k]);
                const auto lossDb = 10.0 * std::log10 (pm[k] / (0.5 * (pl[k] + pr[k])));
                if (monoDb < deepestMono) { deepestMono = monoDb; monoHz = f; }
                deepestSide = std::min (deepestSide, sideDb);
                if (lossDb < worstLoss) { worstLoss = lossDb; lossHz = f; }
                if (f >= 2000.0 && f < 8000.0) { highMono += pm[k] / dry[k]; ++highBins; }
                curve.x.push_back (f);
                curve.y.push_back (monoDb);
            }
            const auto highDb = 10.0 * std::log10 (highMono / highBins);
            if (check)
            {
                expectGreaterThan (worstLoss, -6.0);
                expectGreaterThan (deepestMono, -15.0);
                series.push_back (curve);
            }
            results.add (label + ": mono sum deepest " + juce::String (deepestMono, 1) + " dB at " + juce::String (monoHz, 0) + " Hz (one side alone "
                         + juce::String (deepestSide, 1) + " dB), summing adds at most " + juce::String (-worstLoss, 1) + " dB (at " + juce::String (lossHz, 0)
                         + " Hz), mono 2-8 kHz " + juce::String (highDb, 1) + " dB");
        };

        int colour = 0;
        for (auto mode : { Chorus::Mode::classic, Chorus::Mode::dimension, Chorus::Mode::tri })
        {
            auto s = defaultsFor (mode);
            s.analog = false;
            measure (s, juce::String (modeName (mode)) + " (defaults, mix 50%)", true, colour++);
        }
        {
            auto s = defaultsFor (Chorus::Mode::classic);
            s.analog = false;
            s.mix = 1.0f;
            measure (s, "Classic at 100% wet", true, colour++);
        }
        {
            auto s = defaultsFor (Chorus::Mode::dimension);
            s.analog = false;
            s.mix = 1.0f;
            measure (s, "Dimension at 100% wet (pure side, cancels in mono by design)", false, colour++);
        }

        PlotOptions o;
        o.title = "Mono sum (L+R)/2 against the dry input, white noise, analog off";
        o.xLabel = "Frequency (Hz)";
        o.yLabel = "Level (dB)";
        o.logX = true;
        o.xMin = 100.0; o.xMax = 10000.0; o.yMin = -24.0; o.yMax = 6.0;
        const auto png = proofDir().getChildFile ("chorus_mono_sum.png");
        expect (savePlot (png, o, series));
        logMessage ("  -> 20 s of white noise, 100 Hz to 10 kHz, 5.9 Hz bins (limits: summing adds less than 6 dB, mono never below -15 dB): "
                    + results.joinIntoString ("; "));
        logMessage ("  -> " + png.getFullPathName());
    }

    void interpolation()
    {
        beginTest ("interpolation: the swept Hermite read matches the exact band-limited delayed sine");

        // One voice, 10 ms swept +-3 ms by a 1.3 Hz sine, so the read position crosses every fraction. The
        // reference is the sine evaluated at n - d(n) exactly, which is what an ideal band-limited fractional
        // delay returns. Linear interpolation of the same positions for comparison.
        ModulatedDelay::Settings s;
        s.numVoices = 1;
        s.voices[0].baseDelayMs = 10.0;
        s.voices[0].depthMs = 3.0;
        s.voices[0].shape = Lfo::Shape::sine;
        s.voices[0].rateHz = 1.3;

        juce::StringArray results;
        PlotSeries hermite { "Hermite (the engine)", {}, {}, plotColour (0), 2.5f }, linear { "linear, same positions", {}, {}, plotColour (1), 2.0f, true };
        double at2k = 0.0, at4k = 0.0;

        for (const auto f : { 100.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 12000.0 })
        {
            ModulatedDelay e;
            e.setSettings (s);
            e.prepare (fs, 20.0, 0.1);
            ampsim::DelayLine line;
            line.prepare (1024);

            const auto n0 = (size_t) (0.1 * fs), length = (size_t) (2.0 * fs);
            std::vector<float> got, lin;
            std::vector<double> want;
            for (size_t n = 0; n < length; ++n)
            {
                const auto x = (float) (0.5 * std::sin (twoPi * f * (double) n / fs));
                const auto y = e.processSample (x);
                const auto d = e.getVoiceDelay (0);
                // Linear: the same position read before this sample is written, like the engine.
                const auto pos = d - 1.0;
                const auto whole = (int) pos;
                const auto t = pos - whole;
                const auto l = (1.0 - t) * line.readInteger (whole) + t * line.readInteger (whole + 1);
                line.write (x);
                if (n < n0)
                    continue;
                got.push_back (y);
                lin.push_back ((float) l);
                want.push_back (0.5 * std::sin (twoPi * f * ((double) n - d) / fs));
            }
            const auto errorDb = relativeErrorDb (got, want), linearDb = relativeErrorDb (lin, want);
            if (f == 2000.0) at2k = errorDb;
            if (f == 4000.0) at4k = errorDb;
            hermite.x.push_back (f);
            hermite.y.push_back (errorDb);
            linear.x.push_back (f);
            linear.y.push_back (linearDb);
            results.add (juce::String (f / 1000.0, f < 1000.0 ? 2 : 0) + " kHz " + juce::String (errorDb, 1) + " (linear " + juce::String (linearDb, 1) + ")");
        }
        expectLessThan (at2k, -70.0);
        expectLessThan (at4k, -50.0);

        PlotOptions o;
        o.title = "Error of a swept fractional delay against the exact delayed sine";
        o.xLabel = "Frequency (Hz)";
        o.yLabel = "Error relative to the signal (dB)";
        o.logX = true;
        o.xMin = 100.0; o.xMax = 12000.0; o.yMin = -160.0; o.yMax = 0.0;
        const auto png = proofDir().getChildFile ("chorus_interpolation.png");
        expect (savePlot (png, o, { hermite, linear }));
        logMessage ("  -> error in dB at each frequency (limits -70 dB at 2 kHz, -50 dB at 4 kHz; about 18 dB better per octave down): "
                    + results.joinIntoString (", "));
        logMessage ("  -> " + png.getFullPathName());
    }

    void zipper()
    {
        beginTest ("zipper noise: rate, depth, mix, and width moves and jumps don't step");

        // A 440 Hz tone through the defaults (analog off so every step is the signal's own). Steady for 2 s,
        // a move over 1 s (a new target every buffer, like a turned knob) or a jump in one buffer (a preset),
        // steady for 2 s. Both steady windows span more than an LFO cycle (0.8 Hz). A click or a zipper
        // step would stand out against the larger of the two steady states: a one-sample jump of the read
        // position doubles the step of a sine. The limit allows the move's own pitch bend: depth 0 to 100%
        // in the 100 ms glide moves the delay 4 ms, a 4% bend for 100 ms on half the signal.
        const auto tone = sine (440.0, 0.5, (int) (5.0 * fs));
        struct Move
        {
            const char* name;
            std::function<void (Chorus::Settings&, double)> apply; // 0 = start, 1 = end
            bool jump;
            double levelAllowance = 1.0;
        };
        // The mix is equal power (cos and sin of pi/2 mix), which holds the level of uncorrelated signals.
        // A steady tone's dry and wet are correlated, so halfway through a mix ramp they can add up to
        // sqrt(2) louder: a level swell, not a zipper step. Mix ramps are held to 1.05 x that bound.
        const auto equalPowerSwell = std::sqrt (2.0);
        const std::vector<Move> moves {
            { "rate 0.2 -> 6 Hz", [] (Chorus::Settings& s, double t) { s.rateHz = (float) (0.2 * std::pow (30.0, t)); }, false },
            { "depth 10 -> 100%", [] (Chorus::Settings& s, double t) { s.depth = (float) (0.1 + 0.9 * t); }, false },
            { "mix 0 -> 100%", [] (Chorus::Settings& s, double t) { s.mix = (float) t; }, false, equalPowerSwell },
            { "width 0 -> 100%", [] (Chorus::Settings& s, double t) { s.width = (float) t; }, false },
            { "depth 0 -> 100% at once", [] (Chorus::Settings& s, double t) { s.depth = (float) (t > 0.0 ? 1.0 : 0.0); }, true },
            { "mix 0 -> 100% at once", [] (Chorus::Settings& s, double t) { s.mix = (float) (t > 0.0 ? 1.0 : 0.0); }, true, equalPowerSwell },
            { "rate 0.2 -> 6 Hz at once", [] (Chorus::Settings& s, double t) { s.rateHz = t > 0.0 ? 6.0f : 0.2f; }, true },
        };

        juce::StringArray results;
        double worstRatio = 0.0;
        for (const auto& move : moves)
        {
            auto s = defaultsFor (Chorus::Mode::classic);
            s.analog = false;
            move.apply (s, 0.0);
            Chorus c;
            c.setSettings (s);
            c.prepare (fs, blockSize);
            const auto moveStart = (size_t) (2.0 * fs) / blockSize * blockSize;
            const auto moveEnd = move.jump ? moveStart + (size_t) (0.2 * fs) : moveStart + (size_t) fs;
            const auto out = run (c, tone, blockSize, [&] (size_t start)
            {
                if (start < moveStart)
                    return;
                const auto t = move.jump ? 1.0 : std::min (1.0, (double) (start - moveStart) / (double) (moveEnd - moveStart));
                auto now = s;
                move.apply (now, t);
                c.setSettings (now);
            }).left;
            const auto before = maxStep (out, (size_t) (0.5 * fs), moveStart);
            const auto during = maxStep (out, moveStart, moveEnd);
            const auto after = maxStep (out, moveEnd + (size_t) (0.2 * fs), out.size());
            const auto ratio = during / std::max (before, after);
            worstRatio = std::max (worstRatio, ratio / move.levelAllowance);
            results.add (juce::String (move.name) + ": " + juce::String (during, 4) + " during vs " + juce::String (before, 4) + " before, " + juce::String (after, 4)
                         + " after (x" + juce::String (ratio, 3) + ")");
        }
        expectLessThan (worstRatio, 1.05);
        logMessage ("  -> largest sample step of a 0.5 x 440 Hz tone (limit: 1.05 x the larger steady state, x sqrt(2) for the equal-power mix ramps): "
                    + results.joinIntoString ("; "));
    }

    void dryPath()
    {
        beginTest ("zero latency: the dry path is untouched, and 100% wet is pure vibrato with no dry in it");

        Chorus probe;
        expectEquals (probe.latencySamples(), 0);

        // Mix 0 with everything else on (analog, noise, high-pass): bit-exact passthrough in every mode.
        const auto di = guitarDI ((int) (3.0 * fs));
        double worstPassthrough = 0.0;
        for (auto mode : { Chorus::Mode::classic, Chorus::Mode::dimension, Chorus::Mode::tri })
        {
            auto s = defaultsFor (mode);
            s.mix = 0.0f;
            s.noise = true;
            Chorus c;
            c.setSettings (s);
            c.prepare (fs, blockSize);
            const auto out = run (c, di);
            worstPassthrough = std::max ({ worstPassthrough, maxAbsDifference (out.left, di), maxAbsDifference (out.right, di) });
        }
        expectEquals (worstPassthrough, 0.0);

        // An impulse at mix 50% (analog and high-pass off): cos(pi/4) of it comes out at once (the equal-power
        // dry gain), the rest only after the shortest delay the sweep reaches.
        std::vector<float> impulse ((size_t) fs, 0.0f);
        impulse[0] = 1.0f;
        auto half = vibrato (Lfo::Shape::triangle, 0.8f, 0.5f);
        half.mix = 0.5f;
        Chorus c;
        c.setSettings (half);
        c.prepare (fs, blockSize);
        const auto out = run (c, impulse).left;
        const auto shortest = (Chorus::modeSpecs[0].baseDelayMs - 0.5 * Chorus::modeSpecs[0].maxDepthMs) * fs / 1000.0;
        double before = 0.0;
        for (size_t n = 1; n + 2 < (size_t) shortest; ++n)
            before = std::max (before, (double) std::abs (out[n]));
        expectWithinAbsoluteError (out[0], (float) std::cos (juce::MathConstants<double>::pi / 4.0), 1.0e-7f);
        expectEquals (before, 0.0);

        // 100% wet: no dry at all, the same delay, and unity level for a sine.
        Chorus w;
        w.setSettings (vibrato (Lfo::Shape::triangle, 0.8f, 0.5f));
        w.prepare (fs, blockSize);
        const auto wet = run (w, impulse).left;
        double leak = 0.0;
        for (size_t n = 0; n + 2 < (size_t) shortest; ++n)
            leak = std::max (leak, (double) std::abs (wet[n]));
        expectEquals (leak, 0.0);

        const auto tone = sine (700.0, 0.5, (int) (3.0 * fs));
        Chorus v;
        v.setSettings (vibrato (Lfo::Shape::sine, 1.5f, 0.75f));
        v.prepare (fs, blockSize);
        const auto vib = run (v, tone).left;
        const auto levelDb = toDb (rms (vib.data() + (size_t) fs, vib.size() - (size_t) fs) / rms (tone.data() + (size_t) fs, tone.size() - (size_t) fs));
        const auto [lo, hi] = centsRange (cyclePitch (vib, 700.0, (size_t) fs));
        expectWithinAbsoluteError (levelDb, 0.0, 0.01);
        expectGreaterThan (hi, 20.0);

        logMessage ("  -> latency 0 samples; mix 0 with analog, noise, and high-pass on: largest difference from the input " + juce::String (worstPassthrough)
                    + " in all three modes (bit-exact); an impulse at mix 50%: " + juce::String (out[0]) + " at sample 0, then nothing until sample "
                    + juce::String ((int) shortest - 2) + " (the sweep's shortest delay is " + juce::String (shortest, 0) + ")");
        logMessage ("  -> 100% wet: largest output before the delay " + juce::String (leak) + " (no dry); a 700 Hz sine comes out at " + juce::String (levelDb, 4)
                    + " dB, swinging +" + juce::String (hi, 1) + "/" + juce::String (lo, 1) + " cents: vibrato");
    }

    struct Change
    {
        double seconds;
        Chorus::Mode mode;
        Lfo::Shape shape;
    };

    /// A 440 Hz tone through the defaults (analog off) with the given mode and shape changes.
    std::vector<float> switchRun (Chorus& c, const std::vector<Change>& changes, double seconds, float rate, int& fadesSeen)
    {
        auto s = defaultsFor (Chorus::Mode::classic);
        s.analog = false;
        s.rateHz = rate;
        c.setSettings (s);
        c.prepare (fs, blockSize);
        size_t next = 0;
        return run (c, sine (440.0, 0.5, (int) (seconds * fs)), blockSize, [&] (size_t start)
        {
            while (next < changes.size() && (double) start >= changes[next].seconds * fs)
            {
                s.mode = changes[next].mode;
                s.shape = changes[next].shape;
                c.setSettings (s);
                ++next;
            }
            fadesSeen += c.isSwitching() ? 1 : 0;
        }).left;
    }

    void modeSwitch()
    {
        beginTest ("mode and shape changes crossfade without a click, including changes mid-fade");

        // 1. Periodic LFOs at 1 Hz with a change every 1.5 s, so the steady stretch on each side spans a whole
        // LFO cycle and holds that mode's largest step (its voices and the dry line up once per cycle). Then a
        // reversal mid-fade, and three changes in a row (the third waits for the fade).
        const std::vector<Change> changes {
            { 1.5, Chorus::Mode::dimension, Lfo::Shape::triangle }, { 3.0, Chorus::Mode::tri, Lfo::Shape::triangle },
            { 4.5, Chorus::Mode::classic, Lfo::Shape::triangle },   { 6.0, Chorus::Mode::classic, Lfo::Shape::sine },
            { 7.5, Chorus::Mode::tri, Lfo::Shape::sine },           { 9.0, Chorus::Mode::dimension, Lfo::Shape::sine },
            { 9.01, Chorus::Mode::tri, Lfo::Shape::sine },          // back again while the fade runs
            { 10.5, Chorus::Mode::classic, Lfo::Shape::triangle },  { 10.503, Chorus::Mode::dimension, Lfo::Shape::sine },
            { 10.506, Chorus::Mode::tri, Lfo::Shape::triangle },
        };
        Chorus c;
        int fadesSeen = 0;
        const auto out = switchRun (c, changes, 13.5, 1.0f, fadesSeen);

        juce::StringArray results;
        double worstRatio = 0.0;
        const std::vector<double> marks { 1.5, 3.0, 4.5, 6.0, 7.5, 9.0, 10.5 };
        for (size_t i = 0; i < marks.size(); ++i)
        {
            const auto at = (size_t) (marks[i] * fs);
            const auto previous = i == 0 ? (size_t) (0.2 * fs) : (size_t) ((marks[i - 1] + 0.3) * fs);
            const auto following = i + 1 < marks.size() ? (size_t) (marks[i + 1] * fs) : out.size();
            const auto steady = std::max (maxStep (out, previous, at - 256), maxStep (out, at + (size_t) (0.3 * fs), following - 256));
            const auto around = maxStep (out, at - 256, at + (size_t) (0.3 * fs));
            worstRatio = std::max (worstRatio, around / steady);
            results.add (juce::String (marks[i], 1) + " s x" + juce::String (around / steady, 3));
        }
        expectLessThan (worstRatio, 1.05);
        expect (c.getActiveMode() == Chorus::Mode::tri && ! c.isSwitching());
        expectGreaterThan (fadesSeen, 0);

        // 2. Into and out of the random shape, whose wander never repeats, so no steady stretch is sure to
        // hold its largest step. A click detector that doesn't need one: everything a 440 Hz tone through a
        // chorus should contain sits near 440 Hz, so a 24 dB/oct high-pass at 4 kHz leaves almost nothing,
        // and a click (a jump in the waveform) is broadband and shows up there.
        const std::vector<Change> randomChanges {
            { 1.5, Chorus::Mode::tri, Lfo::Shape::random },     { 3.0, Chorus::Mode::dimension, Lfo::Shape::random },
            { 4.5, Chorus::Mode::classic, Lfo::Shape::random }, { 6.0, Chorus::Mode::classic, Lfo::Shape::triangle },
        };
        Chorus r;
        int randomFades = 0;
        const auto randomOut = switchRun (r, randomChanges, 7.5, 0.8f, randomFades);
        const auto highPassed = [] (const std::vector<float>& y)
        {
            std::array<ampsim::Svf, 2> highPass;
            for (int k = 0; k < 2; ++k)
                highPass[(size_t) k].setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, 4000.0, Chorus::highPassQ (k), 0.0, fs));
            std::vector<float> residual (y.size());
            for (size_t n = 0; n < y.size(); ++n)
                residual[n] = (float) highPass[1].processSample (highPass[0].processSample (y[n]));
            return residual;
        };
        const auto peak = [] (const std::vector<float>& residual, size_t from, size_t to)
        {
            double p = 0.0;
            for (size_t n = from; n < std::min (to, residual.size()); ++n)
                p = std::max (p, (double) std::abs (residual[n]));
            return p;
        };

        const auto residual = highPassed (randomOut);
        double steadyResidual = peak (residual, (size_t) (0.3 * fs), (size_t) (1.5 * fs) - 256), switchResidual = 0.0;
        for (const auto& change : randomChanges)
        {
            const auto at = (size_t) (change.seconds * fs);
            switchResidual = std::max (switchResidual, peak (residual, at - 256, at + (size_t) (0.1 * fs)));
            steadyResidual = std::max (steadyResidual, peak (residual, at + (size_t) (0.1 * fs), at + (size_t) (1.5 * fs) - 256));
        }

        // Positive control: the same output with the read position jumped by one sample (one sample
        // dropped) where the tone is steepest, in a steady stretch.
        auto jumped = randomOut;
        const auto from = (size_t) (2.0 * fs), to = (size_t) (2.5 * fs);
        size_t steepest = from;
        for (size_t n = from; n < to; ++n)
            if (std::abs (jumped[n + 1] - jumped[n]) > std::abs (jumped[steepest + 1] - jumped[steepest]))
                steepest = n;
        jumped.erase (jumped.begin() + (long) steepest);
        const auto control = peak (highPassed (jumped), from, to);

        expectGreaterThan (control, 20.0 * steadyResidual);
        expectLessThan (switchResidual, 0.1 * control);
        expect (r.getActiveMode() == Chorus::Mode::classic && ! r.isSwitching());

        logMessage ("  -> periodic LFOs: largest step in the 300 ms after each change, as a multiple of the steady states either side (limit 1.05): "
                    + results.joinIntoString (", ") + "; ends in Tri, fade finished");
        logMessage ("  -> into and out of random: peak above 4 kHz of a 0.5 x 440 Hz tone " + juce::String (switchResidual, 6) + " around the changes vs "
                    + juce::String (steadyResidual, 6) + " in steady stretches; positive control, one sample dropped in a steady stretch: "
                    + juce::String (control, 6) + " (limit for the changes: a tenth of that)");
    }

    void analogCharacter()
    {
        beginTest ("analog character: the wet's 7 kHz low-pass, light tanh saturation, faint noise, and switching it all without a click");

        // Depth 0 (a fixed 11 ms delay, a whole number of samples), mix 1, high-pass off: the output is the
        // delayed wet, so analog on over analog off is exactly the low-pass, measured on a quiet tone where
        // tanh is linear (tanh(0.01) / 0.01 = 1 - 3e-5).
        const auto wetOnly = [] (bool analog, bool noise)
        {
            auto s = vibrato (Lfo::Shape::triangle, 0.8f, 0.0f);
            s.analog = analog;
            s.noise = noise;
            return s;
        };
        const auto window = (size_t) (0.5 * fs), from = (size_t) (0.25 * fs);
        const auto lowPass = ampsim::Svf::design (ampsim::Svf::Type::lowpass, Chorus::analogLowPassHz, Chorus::butterworthQ, 0.0, fs);
        const auto lowPassDb = [&] (double f) { return toDb (std::abs (ampsim::Svf::responseAt (lowPass, f, fs))); };

        juce::StringArray filter;
        double worstFilter = 0.0;
        for (const auto f : { 1000.0, 3500.0, 7000.0, 14000.0 })
        {
            const auto tone = sine (f, 0.01, (int) fs);
            double amplitude[2] {};
            for (int on = 0; on < 2; ++on)
            {
                Chorus c;
                c.setSettings (wetOnly (on == 1, false));
                c.prepare (fs, blockSize);
                amplitude[on] = toneAmplitude (run (c, tone).left, f, from, window);
            }
            const auto measured = toDb (amplitude[1] / amplitude[0]);
            worstFilter = std::max (worstFilter, std::abs (measured - lowPassDb (f)));
            filter.add (juce::String (f / 1000.0, 1) + " kHz " + juce::String (measured, 2) + " dB");
        }
        expectLessThan (worstFilter, 0.01);

        // A -6 dBFS sine through the saturation: the fundamental and third harmonic against tanh's Fourier
        // series (through the low-pass).
        const auto loud = sine (1000.0, 0.5, (int) fs);
        Chorus s;
        s.setSettings (wetOnly (true, false));
        s.prepare (fs, blockSize);
        const auto saturated = run (s, loud).left;
        const auto h1 = toneAmplitude (saturated, 1000.0, from, window), h3 = toneAmplitude (saturated, 3000.0, from, window);
        const auto h1Theory = tanhHarmonic (0.5, 1) * std::pow (10.0, lowPassDb (1000.0) / 20.0);
        const auto h3Theory = std::abs (tanhHarmonic (0.5, 3)) * std::pow (10.0, lowPassDb (3000.0) / 20.0);
        expectWithinAbsoluteError (toDb (h1 / h1Theory), 0.0, 0.01);
        expectWithinAbsoluteError (toDb (h3 / h3Theory), 0.0, 0.05);

        // Noise: silence in. Off, the output is exactly silent; on, white noise at -80 dBFS RMS through the
        // low-pass, whose white-noise power gain is the energy of its impulse response.
        const std::vector<float> silence ((size_t) (2.0 * fs), 0.0f);
        double noiseDb[2] {};
        for (int on = 0; on < 2; ++on)
        {
            Chorus c;
            c.setSettings (wetOnly (true, on == 1));
            c.prepare (fs, blockSize);
            const auto out = run (c, silence).left;
            noiseDb[on] = toDb (rms (out.data() + (size_t) (0.5 * fs), out.size() - (size_t) (0.5 * fs)));
        }
        double energy = 0.0;
        {
            ampsim::Svf f;
            f.setCoefficients (lowPass);
            for (int n = 0; n < 4096; ++n)
            {
                const auto h = f.processSample (n == 0 ? 1.0 : 0.0);
                energy += h * h;
            }
        }
        const auto noiseTheory = Chorus::noiseRmsDb + 10.0 * std::log10 (energy);
        expect (noiseDb[0] <= -399.0);
        expectWithinAbsoluteError (noiseDb[1], noiseTheory, 0.2);

        // Switching analog, noise, and the high-pass while a tone plays, and turning the high-pass frequency
        // 150 -> 600 -> 150 Hz over half a second each way (defaults, LFO at 1 Hz so every steady stretch
        // spans a cycle): no step beyond the steady states either side. (A jump of the frequency straight
        // across the tone glides over 25 ms, and the crossover's phase at the tone turns fast enough then to
        // bend its pitch briefly, which the step metric reads; the click detector below checks that jump.)
        struct Toggle
        {
            double seconds, span;
            std::function<void (Chorus::Settings&, double)> apply; // t from 0 to 1 over the span
        };
        const std::vector<Toggle> toggles {
            { 1.5, 0.0, [] (Chorus::Settings& x, double) { x.analog = false; } },
            { 3.0, 0.0, [] (Chorus::Settings& x, double) { x.analog = true; } },
            { 4.5, 0.0, [] (Chorus::Settings& x, double) { x.noise = true; } },
            { 6.0, 0.0, [] (Chorus::Settings& x, double) { x.noise = false; } },
            { 7.5, 0.0, [] (Chorus::Settings& x, double) { x.wetHighPass = false; } },
            { 9.0, 0.0, [] (Chorus::Settings& x, double) { x.wetHighPass = true; } },
            { 10.5, 0.5, [] (Chorus::Settings& x, double t) { x.wetHighPassHz = (float) (150.0 * std::pow (4.0, t)); } },
            { 12.5, 0.5, [] (Chorus::Settings& x, double t) { x.wetHighPassHz = (float) (600.0 * std::pow (0.25, t)); } },
        };
        auto settings = defaultsFor (Chorus::Mode::classic);
        settings.rateHz = 1.0f;
        Chorus t;
        t.setSettings (settings);
        t.prepare (fs, blockSize);
        const auto out = run (t, sine (440.0, 0.5, (int) (14.5 * fs)), blockSize, [&] (size_t start)
        {
            const auto now = (double) start / fs;
            for (const auto& toggle : toggles)
                if (now >= toggle.seconds && now < toggle.seconds + toggle.span + 0.1)
                    toggle.apply (settings, toggle.span > 0.0 ? std::min (1.0, (now - toggle.seconds) / toggle.span) : 1.0);
            t.setSettings (settings);
        }).left;
        double worstRatio = 0.0;
        for (size_t i = 0; i < toggles.size(); ++i)
        {
            const auto at = (size_t) (toggles[i].seconds * fs);
            const auto settled = at + (size_t) ((toggles[i].span + 0.3) * fs);
            const auto previous = i == 0 ? (size_t) (0.2 * fs) : (size_t) ((toggles[i - 1].seconds + toggles[i - 1].span + 0.3) * fs);
            const auto following = i + 1 < toggles.size() ? (size_t) (toggles[i + 1].seconds * fs) : out.size();
            const auto steady = std::max (maxStep (out, previous, at - 256), maxStep (out, settled, following - 256));
            worstRatio = std::max (worstRatio, maxStep (out, at - 256, settled) / steady);
        }
        expectLessThan (worstRatio, 1.05);

        // The frequency jumped in one buffer, 150 -> 600 -> 150 Hz: the click detector of the mode-switch
        // test (the residual above 4 kHz of the tone), against a positive control made here: the same
        // output with one sample dropped in a steady stretch, where the tone is steepest.
        double jumpResidual = 0.0, jumpSteady = 0.0, jumpControl = 0.0;
        {
            auto j = defaultsFor (Chorus::Mode::classic);
            j.rateHz = 1.0f;
            Chorus c;
            c.setSettings (j);
            c.prepare (fs, blockSize);
            const auto y = run (c, sine (440.0, 0.5, (int) (4.5 * fs)), blockSize, [&] (size_t start)
            {
                j.wetHighPassHz = start >= (size_t) (1.5 * fs) && start < (size_t) (3.0 * fs) ? 600.0f : 150.0f;
                c.setSettings (j);
            }).left;

            const auto residualPeaks = [] (const std::vector<float>& signal, const std::function<bool (size_t)>& near, double& nearPeak, double& elsewhere)
            {
                std::array<ampsim::Svf, 2> highPass;
                for (int k = 0; k < 2; ++k)
                    highPass[(size_t) k].setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, 4000.0, Chorus::highPassQ (k), 0.0, fs));
                for (size_t n = 0; n < signal.size(); ++n)
                {
                    const auto r = std::abs (highPass[1].processSample (highPass[0].processSample (signal[n])));
                    if (n < (size_t) (0.3 * fs))
                        continue;
                    auto& peak = near (n) ? nearPeak : elsewhere;
                    peak = std::max (peak, r);
                }
            };
            residualPeaks (y, [] (size_t n) { return (n + 256 > (size_t) (1.5 * fs) && n < (size_t) (1.6 * fs)) || (n + 256 > (size_t) (3.0 * fs) && n < (size_t) (3.1 * fs)); },
                           jumpResidual, jumpSteady);

            auto dropped = y;
            size_t steepest = (size_t) (0.5 * fs);
            for (size_t n = steepest; n < (size_t) (1.0 * fs); ++n)
                if (std::abs (dropped[n + 1] - dropped[n]) > std::abs (dropped[steepest + 1] - dropped[steepest]))
                    steepest = n;
            dropped.erase (dropped.begin() + (long) steepest);
            double unused = 0.0;
            residualPeaks (dropped, [&] (size_t n) { return n >= steepest && n < steepest + 1000; }, jumpControl, unused);
        }
        expectLessThan (jumpResidual, 0.1 * jumpControl);

        logMessage ("  -> low-pass on the wet, analog on over off (theory: 2nd-order Butterworth at 7 kHz, largest error " + juce::String (worstFilter, 4)
                    + " dB): " + filter.joinIntoString (", "));
        logMessage ("  -> a -6 dBFS 1 kHz sine through tanh: fundamental " + juce::String (toDb (h1 / 0.5), 3) + " dB (theory " + juce::String (toDb (h1Theory / 0.5), 3)
                    + "), third harmonic " + juce::String (toDb (h3 / h1), 2) + " dB below it (theory " + juce::String (toDb (h3Theory / h1Theory), 2) + ")");
        logMessage ("  -> silence in: " + juce::String (noiseDb[0] <= -399.0 ? "exact silence" : juce::String (noiseDb[0], 1) + " dBFS") + " with noise off, "
                    + juce::String (noiseDb[1], 2) + " dBFS RMS with it on (theory " + juce::String (noiseTheory, 2) + "); switching analog, noise, the high-pass, "
                    "and turning its frequency (150 -> 600 -> 150 Hz) under a tone: largest step x" + juce::String (worstRatio, 3) + " of the steady states (limit 1.05); "
                    "the frequency jumped in one buffer: peak above 4 kHz " + juce::String (jumpResidual, 6) + " vs " + juce::String (jumpSteady, 6)
                    + " steady, and " + juce::String (jumpControl, 6) + " for one dropped sample (limit a tenth of that)");
    }

    void engine()
    {
        beginTest ("engine: feedback stays bounded at the maximum, and a change of voice count fades voices in step");

        // Feedback +-0.99 on a 2 ms delay with full-scale noise. The line holds less than max|x| + 1 = 2, and
        // a Hermite read can overshoot its samples by at most the sum of its weights' magnitudes, 1.25 (at
        // t = 0.5: 1/16 + 9/16 + 9/16 + 1/16), so the wet stays below 2.5.
        const auto noise = whiteNoise ((int) (2.0 * fs), 1.0f, 5);
        double peaks[2] {};
        bool finite = true;
        for (int sign = 0; sign < 2; ++sign)
        {
            ModulatedDelay e;
            ModulatedDelay::Settings s;
            s.voices[0].baseDelayMs = 2.0;
            s.voices[0].depthMs = 0.5;
            s.voices[0].rateHz = 0.3;
            s.feedback = sign == 0 ? 0.99 : -0.99;
            e.setSettings (s);
            e.prepare (fs, 20.0, 0.1);
            for (auto x : noise)
            {
                const auto y = (double) e.processSample (x);
                finite = finite && std::isfinite (y);
                peaks[sign] = std::max (peaks[sign], std::abs (y));
            }
        }
        expect (finite);
        expectLessThan (std::max (peaks[0], peaks[1]), 2.5);

        // One voice, then three (offsets 0, 1/3, 2/3), then one again, under a 440 Hz tone.
        const auto tone = sine (440.0, 0.5, (int) (4.0 * fs));
        ModulatedDelay e;
        ModulatedDelay::Settings s;
        for (int v = 0; v < 3; ++v)
        {
            auto& voice = s.voices[(size_t) v];
            voice.baseDelayMs = 12.0;
            voice.depthMs = 2.0;
            voice.rateHz = 2.0;
            voice.phase = v / 3.0;
            voice.level = 1.0 / 3.0;
        }
        s.numVoices = 1;
        e.setSettings (s);
        e.prepare (fs, 20.0, 0.1);

        std::vector<float> out (tone.size());
        std::vector<double> d0, d1, d2;
        for (size_t n = 0; n < tone.size(); ++n)
        {
            if (n == (size_t) (1.0 * fs) || n == (size_t) (3.0 * fs))
            {
                s.numVoices = n < (size_t) (2.0 * fs) ? 3 : 1;
                e.setSettings (s);
            }
            out[n] = e.processSample (tone[n]);
            if (n >= (size_t) (1.5 * fs) && n < (size_t) (2.5 * fs))
            {
                d0.push_back (e.getVoiceDelay (0));
                d1.push_back (e.getVoiceDelay (1));
                d2.push_back (e.getVoiceDelay (2));
            }
        }
        const auto steady = std::max (maxStep (out, (size_t) (0.2 * fs), (size_t) (0.95 * fs)), maxStep (out, (size_t) (1.2 * fs), (size_t) (2.9 * fs)));
        const auto around = std::max (maxStep (out, (size_t) (0.95 * fs), (size_t) (1.2 * fs)), maxStep (out, (size_t) (2.95 * fs), (size_t) (3.2 * fs)));
        const auto p1 = wrapDegrees (phaseDegrees (d1, 2.0 / fs) - phaseDegrees (d0, 2.0 / fs));
        const auto p2 = wrapDegrees (phaseDegrees (d2, 2.0 / fs) - phaseDegrees (d0, 2.0 / fs));
        expectLessThan (around, steady * 1.05);
        expectWithinAbsoluteError (p1, 120.0, 1.0e-6);
        expectWithinAbsoluteError (p2, 240.0, 1.0e-6);
        expect (! e.isVoiceRunning (1) && ! e.isVoiceRunning (2));
        logMessage ("  -> feedback +0.99 / -0.99 on full-scale noise: peak wet " + juce::String (peaks[0], 3) + " / " + juce::String (peaks[1], 3)
                    + " (bound 2.5), all finite; 1 -> 3 -> 1 voices: largest step around the changes " + juce::String (around, 4) + " vs " + juce::String (steady, 4)
                    + " steady; the voices that joined run " + juce::String (p1, 6) + " and " + juce::String (p2, 6) + " degrees after voice 0 (120, 240), and stop after fading out");
    }

    void realtime()
    {
        beginTest ("real time: nothing allocates, frees, or locks while every setting and mode changes");

        Chorus c;
        c.setSettings ({});
        c.prepare (fs, blockSize);
        const auto di = guitarDI ((int) (8.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        const std::array<Chorus::Mode, 3> modes { Chorus::Mode::classic, Chorus::Mode::dimension, Chorus::Mode::tri };
        const std::array<Lfo::Shape, 3> shapes { Lfo::Shape::triangle, Lfo::Shape::sine, Lfo::Shape::random };
        Chorus::Settings s;
        int blocks = 0, changes = 0;

        rtcheck::begin();
        for (size_t start = 0; start + blockSize <= di.size(); start += blockSize, ++blocks)
        {
            if (blocks % 20 == 0 || blocks % 97 == 1) // every 20 buffers, and sometimes the very next one (mid-fade)
            {
                const auto k = blocks / 20 + blocks % 7;
                s.mode = modes[(size_t) (k % 3)];
                s.shape = shapes[(size_t) ((k / 3) % 3)];
                s.rateHz = 0.1f + (float) (k % 11);
                s.depth = (float) (k % 5) / 4.0f;
                s.mix = (float) (k % 4) / 3.0f;
                s.width = (float) (k % 3) / 2.0f;
                s.analog = k % 2 == 0;
                s.noise = k % 4 < 2;
                s.wetHighPass = k % 5 != 0;
                s.wetHighPassHz = 60.0f + 70.0f * (float) (k % 9);
                ++changes;
            }
            c.setSettings (s);
            buffer.copyFrom (0, 0, di.data() + start, blockSize);
            buffer.copyFrom (1, 0, di.data() + start, blockSize);
            c.process (juce::dsp::AudioBlock<float> (buffer), {});
        }
        const auto counts = rtcheck::end();

        expectEquals (counts.allocations, 0L);
        expectEquals (counts.frees, 0L);
        expectEquals (counts.blockingLocks, 0L);
        logMessage ("  -> " + juce::String (blocks) + " buffers, " + juce::String (changes) + " changes of every setting (modes and shapes included, some mid-fade): "
                    + juce::String (counts.allocations) + " allocations, " + juce::String (counts.frees) + " frees, " + juce::String (counts.blockingLocks)
                    + " blocking locks");
    }

    void cpu()
    {
        beginTest ("CPU: a 128-sample stereo buffer per mode");

        const auto di = guitarDI ((int) (10.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::StringArray results;
        double worstMean = 0.0;

        const auto time = [&] (Chorus::Settings s, const juce::String& label, const std::function<void (Chorus&, int)>& each = {})
        {
            Chorus c;
            c.setSettings (s);
            c.prepare (fs, blockSize);
            std::vector<double> micros;
            int block = 0;
            for (size_t start = 0; start + blockSize <= di.size(); start += blockSize, ++block)
            {
                if (each)
                    each (c, block);
                buffer.copyFrom (0, 0, di.data() + start, blockSize);
                buffer.copyFrom (1, 0, di.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                c.process (juce::dsp::AudioBlock<float> (buffer), {});
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

        for (auto mode : { Chorus::Mode::classic, Chorus::Mode::dimension, Chorus::Mode::tri })
            time (defaultsFor (mode), modeName (mode));

        auto heavy = defaultsFor (Chorus::Mode::tri);
        heavy.noise = true;
        heavy.shape = Lfo::Shape::sine;
        time (heavy, "Tri with sine LFO and noise");

        // Worst case: two pairs of engines running, a fade from Tri to Dimension started every 30 ms.
        time (heavy, "constant mode fades (Tri <-> Dimension)", [heavy] (Chorus& c, int block) mutable
        {
            if (block % 12 == 0)
            {
                heavy.mode = heavy.mode == Chorus::Mode::tri ? Chorus::Mode::dimension : Chorus::Mode::tri;
                c.setSettings (heavy);
            }
        });

        expectLessThan (worstMean, 0.05 * deadlineMicros);
        logMessage ("  -> 10 s of guitar DI, analog on, of the 2.67 ms deadline: " + results.joinIntoString ("; "));
    }

    void renders()
    {
        beginTest ("renders for listening (proof directory)");

        const auto di = guitarDI ((int) (6.0 * fs));
        expect (writeWav (proofDir().getChildFile ("chorus_dry.wav"), di));
        juce::StringArray files;
        for (auto mode : { Chorus::Mode::classic, Chorus::Mode::dimension, Chorus::Mode::tri })
        {
            Chorus c;
            c.setSettings (defaultsFor (mode));
            c.prepare (fs, blockSize);
            const auto out = run (c, di);
            juce::AudioBuffer<float> stereo (2, (int) di.size());
            stereo.copyFrom (0, 0, out.left.data(), (int) di.size());
            stereo.copyFrom (1, 0, out.right.data(), (int) di.size());
            const auto file = proofDir().getChildFile ("chorus_" + juce::String (modeName (mode)).toLowerCase() + ".wav");
            expect (writeWav (file, stereo));
            files.add (file.getFileName());
        }
        logMessage ("  -> the synthetic guitar DI through each mode at its defaults, stereo: " + files.joinIntoString (", ")
                    + " (plus chorus_dry.wav) in " + proofDir().getFullPathName() + ". Unverified by ear: Sean's to judge.");
    }
};

ChorusTests chorusTests;
} // namespace

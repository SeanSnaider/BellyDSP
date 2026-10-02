#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Multivoicer.h"

#include <chrono>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::Multivoicer;

constexpr double twoPi = juce::MathConstants<double>::twoPi;

juce::String str (double v, int places = 2) { return juce::String (v, places); }
double cents (double f, double reference) { return 1200.0 * std::log2 (f / reference); }

std::vector<float> sineWave (double f, double amplitude, size_t n)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = (float) (amplitude * std::sin (twoPi * f * (double) i / fs));
    return x;
}

/// Runs a stereo signal through the multivoicer in 128-sample buffers; `before` runs ahead of each buffer
/// with its first sample's index, to change settings mid-stream. The DI is the left input.
Stereo run (Multivoicer& m, const std::vector<float>& left, const std::vector<float>& right, const std::function<void (size_t)>& before = {},
            int bufferSize = blockSize)
{
    Stereo out { left, right };
    for (size_t start = 0; start < left.size(); start += (size_t) bufferSize)
    {
        if (before)
            before (start);
        const auto len = std::min ((size_t) bufferSize, left.size() - start);
        float* channels[2] = { out.left.data() + start, out.right.data() + start };
        ampsim::BlockContext context { left.data() + start, (int) len };
        m.process (juce::dsp::AudioBlock<float> (channels, 2, len), context);
    }
    return out;
}

Stereo run (Multivoicer& m, const std::vector<float>& mono, const std::function<void (size_t)>& before = {})
{
    return run (m, mono, mono, before);
}

/// One voice, everything else silent: the given interval, pan, and delay, mix 1 (wet only).
Multivoicer::Settings single (double semitones, double cents = 0.0, double pan = 0.0, double delayMs = 0.0)
{
    auto s = Multivoicer::defaults();
    s.voiceCount = 1;
    s.voices[0] = {};
    s.voices[0].semitones = semitones;
    s.voices[0].cents = cents;
    s.voices[0].pan = pan;
    s.voices[0].delayMs = delayMs;
    s.mix = 1.0;
    return s;
}

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
    return ((double) k + (denominator < 0.0 ? 0.5 * (a - c) / denominator : 0.0)) * binHz;
}

/// The amplitude of y's component at f over [start, start + length) (a whole number of cycles of f).
double toneAmplitude (const std::vector<float>& y, double f, size_t start, size_t length)
{
    std::complex<double> sum = 0.0;
    for (size_t n = 0; n < length; ++n)
        sum += (double) y[start + n] * std::polar (1.0, -twoPi * f * (double) n / fs);
    return 2.0 * std::abs (sum) / (double) length;
}

class MultivoicerTests final : public juce::UnitTest
{
public:
    MultivoicerTests() : juce::UnitTest ("Multivoicer", "ampsim") {}

    void runTest() override
    {
        startingPoints();
        dryPath();
        voicePitch();
        panAndSpread();
        normalization();
        wetHighPass();
        drift();
        switching();
        realtime();
        monoSingleNotes();
        monoChords();
        engineSwitch();
        cpu();
        renders();
    }

private:
    void startingPoints()
    {
        beginTest ("starting points: Unison double, Octave stack, Fifths stack, Double + Octaves (the default)");

        using SP = Multivoicer::StartingPoint;
        const auto describe = [] (const Multivoicer::Settings& s)
        {
            juce::StringArray voices;
            for (int i = 0; i < s.voiceCount; ++i)
            {
                const auto& v = s.voices[(size_t) i];
                voices.add ((v.semitones >= 0.0 ? "+" : "") + str (v.semitones, 0) + " st " + (v.cents >= 0.0 ? "+" : "") + str (v.cents, 0) + " c, "
                            + str (v.delayMs, 0) + " ms, pan " + str (v.pan, 2) + ", " + str (v.levelDb, 0) + " dB" + (v.drift > 0.0 ? ", drift" : ""));
            }
            return voices.joinIntoString ("; ");
        };

        const auto unison = Multivoicer::startingPoint (SP::unisonDouble);
        expectEquals (unison.voiceCount, 2);
        expect (std::abs (unison.voices[0].cents) >= 7.0 && std::abs (unison.voices[0].cents) <= 12.0);
        expect (std::abs (unison.voices[1].cents) >= 7.0 && std::abs (unison.voices[1].cents) <= 12.0);
        expect (unison.voices[0].pan * unison.voices[1].pan < -0.5); // wide, opposite sides

        const auto octaves = Multivoicer::startingPoint (SP::octaveStack);
        expectEquals (octaves.voices[0].semitones, -12.0);
        expectEquals (octaves.voices[1].semitones, 12.0);

        const auto fifths = Multivoicer::startingPoint (SP::fifthsStack);
        expectEquals (fifths.voices[0].semitones, 7.0);

        const auto both = Multivoicer::startingPoint (SP::doubleOctaves);
        expectEquals (both.voiceCount, 4);
        expect (both.voices[2].levelDb < 0.0 && both.voices[3].levelDb < 0.0); // the octaves are quieter
        expectEquals (both.voices[2].semitones, -12.0);
        expectEquals (both.voices[3].semitones, 12.0);

        const auto d = Multivoicer::defaults();
        expect (d.engine == Multivoicer::Engine::poly);
        expect (! d.wetHighPass);
        expectEquals (d.voiceCount, both.voiceCount);

        logMessage ("  -> Unison double: " + describe (unison));
        logMessage ("  -> Octave stack: " + describe (octaves));
        logMessage ("  -> Fifths stack: " + describe (fifths));
        logMessage ("  -> Double + Octaves (default; Poly, mix 50%, wet high-pass off): " + describe (both));
    }

    void dryPath()
    {
        beginTest ("dry path: untouched and undelayed (zero latency); mix 0 is the input bit for bit");

        Multivoicer m;
        expectEquals (m.latencySamples(), 0);
        const auto left = guitarDI ((int) (2.0 * fs));
        auto right = left;
        for (size_t i = 0; i < right.size(); ++i)
            right[i] = 0.7f * left[i] + 0.1f * (float) std::sin ((double) i * 0.01); // a different right channel

        auto s = Multivoicer::defaults();
        s.mix = 0.0;
        m.setSettings (s);
        m.prepare (fs, blockSize);
        const auto passthrough = run (m, left, right);
        expectEquals (maxAbsDifference (passthrough.left, left), 0.0);
        expectEquals (maxAbsDifference (passthrough.right, right), 0.0);

        // At 50%: out = cos(pi/4) dry + sin(pi/4) wet. Subtracting sin(pi/4) x (a wet-only run) must leave
        // exactly cos(pi/4) x the input, sample for sample: the dry isn't filtered, shifted, or delayed.
        double worst = 0.0;
        for (const auto mixValue : { 0.25, 0.5, 0.8 })
        {
            Multivoicer a, b;
            auto sa = Multivoicer::defaults();
            sa.mix = mixValue;
            auto sb = sa;
            sb.mix = 1.0;
            a.setSettings (sa);
            b.setSettings (sb);
            a.prepare (fs, blockSize);
            b.prepare (fs, blockSize);
            const auto mixed = run (a, left, right);
            const auto wet = run (b, left, right);
            const auto g = Multivoicer::mixGains (mixValue);
            for (size_t i = 0; i < left.size(); ++i)
            {
                worst = std::max (worst, std::abs ((double) mixed.left[i] - g.wet * (double) wet.left[i] - g.dry * (double) left[i]));
                worst = std::max (worst, std::abs ((double) mixed.right[i] - g.wet * (double) wet.right[i] - g.dry * (double) right[i]));
            }
        }
        expectLessThan (worst, 1.0e-6);
        logMessage ("  -> latency 0 samples; mix 0: largest difference from the stereo input " + juce::String (maxAbsDifference (passthrough.left, left))
                    + " (bit-exact); at mix 25/50/80%, output minus sin(mix pi/2) x wet differs from cos(mix pi/2) x input by at most "
                    + juce::String (worst, 9) + " (float rounding): the dry is untouched and on time");
    }

    void voicePitch()
    {
        beginTest ("voices: every voice of the starting points comes out at its interval (220 Hz sine, FFT peaks)");

        const auto x = sineWave (220.0, 0.5, (size_t) (3.0 * fs));
        constexpr int order = 16;
        double worst = 0.0;
        juce::StringArray rows;
        for (const auto point : { Multivoicer::StartingPoint::octaveStack, Multivoicer::StartingPoint::fifthsStack, Multivoicer::StartingPoint::unisonDouble })
        {
            auto s = Multivoicer::startingPoint (point);
            s.mix = 1.0;
            for (auto& v : s.voices)
                v.drift = 0.0; // drift wanders on purpose; checked separately
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            const auto out = run (m, x);
            std::vector<float> sum (x.size());
            for (size_t i = 0; i < x.size(); ++i)
                sum[i] = out.left[i] + out.right[i];
            const auto mag = magnitudeSpectrum (sum, (size_t) (1.0 * fs), order);
            juce::StringArray errors;
            for (int v = 0; v < s.voiceCount; ++v)
            {
                const auto target = 220.0 * Multivoicer::ratioOf (s.voices[(size_t) v]);
                const auto error = cents (peakFrequency (mag, order, target, 4.0), target);
                worst = std::max (worst, std::abs (error));
                errors.add (str (error, 3));
            }
            rows.add ((point == Multivoicer::StartingPoint::octaveStack ? "octave stack" : point == Multivoicer::StartingPoint::fifthsStack ? "fifths stack" : "unison double")
                      + juce::String (" [") + errors.joinIntoString (" ") + "]");
        }
        expectLessThan (worst, 1.0);
        logMessage ("  -> each voice's spectral peak against its target, cents (limit 1): " + rows.joinIntoString ("; ") + "; worst " + str (worst, 3));
    }

    void panAndSpread()
    {
        beginTest ("pan: constant power scaled by sqrt(2) (centre is unity per side), spread scales every pan");

        double worstPower = 0.0;
        for (int i = 0; i <= 20; ++i)
        {
            const auto [l, r] = Multivoicer::panGains (-1.0 + 0.1 * i);
            worstPower = std::max (worstPower, std::abs (l * l + r * r - 2.0));
        }
        const auto centre = Multivoicer::panGains (0.0);
        const auto hardLeft = Multivoicer::panGains (-1.0);
        expectLessThan (worstPower, 1.0e-12);
        expectWithinAbsoluteError (centre.first, 1.0, 1.0e-12);
        expectWithinAbsoluteError (centre.second, 1.0, 1.0e-12);
        expectLessThan (std::abs (hardLeft.second), 1.0e-12);

        // Through the block: a voice hard left puts nothing on the right; spread 0 centres it.
        const auto x = sineWave (330.0, 0.5, (size_t) (1.0 * fs));
        Multivoicer m;
        m.setSettings (single (7.0, 0.0, -1.0));
        m.prepare (fs, blockSize);
        const auto hard = run (m, x);
        auto centred = single (7.0, 0.0, -1.0);
        centred.spread = 0.0;
        Multivoicer c;
        c.setSettings (centred);
        c.prepare (fs, blockSize);
        const auto mid = run (c, x);
        const auto rightLeak = rms (hard.right);
        const auto balance = toDb (rms (mid.left) / rms (mid.right));
        const auto level = toDb (rms (mid.left) / rms (x));
        expectEquals (rightLeak, 0.0);
        expectWithinAbsoluteError (balance, 0.0, 1.0e-9);
        expectWithinAbsoluteError (level, 0.0, 0.1);
        logMessage ("  -> L^2 + R^2 = 2 within " + juce::String (worstPower) + " over pans -1..1; centre gains " + str (centre.first, 6) + " / " + str (centre.second, 6)
                    + "; a voice hard left puts " + juce::String (rightLeak) + " on the right; with spread 0 it sits in the middle (L/R " + str (balance, 9)
                    + " dB) at " + str (level, 3) + " dB on each side");
    }

    void normalization()
    {
        beginTest ("wet level: divided by sqrt(sum of squared levels), the wet holds the input's level whatever the voice count");

        // Noise band-limited to 6 kHz (a post-cab guitar's band), so the Hermite reads' small loss in white noise's
        // top octave doesn't blur the result.
        auto noise = whiteNoise ((int) (4.0 * fs), 0.3f, 5);
        {
            ampsim::Svf lp1, lp2;
            lp1.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::lowpass, 6000.0, 1.30656, 0.0, fs));
            lp2.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::lowpass, 6000.0, 0.541196, 0.0, fs));
            for (auto& v : noise)
                v = (float) lp2.processSample (lp1.processSample ((double) v));
        }
        juce::StringArray rows;
        double worst = 0.0;
        for (const auto count : { 1, 2, 4, 8 })
        {
            auto s = Multivoicer::defaults();
            s.voiceCount = count;
            const double intervals[] = { 7.0, -12.0, 12.0, 5.0, -5.0, 3.0, -7.0, 19.0 };
            for (int v = 0; v < Multivoicer::maxVoices; ++v)
            {
                s.voices[(size_t) v] = {};
                s.voices[(size_t) v].semitones = intervals[v];
                s.voices[(size_t) v].pan = 0.0;
            }
            s.mix = 1.0;
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            const auto out = run (m, noise);
            const auto level = toDb (rms (out.left.data() + (size_t) (0.5 * fs), out.left.size() - (size_t) (0.5 * fs)) / rms (noise));
            worst = std::max (worst, std::abs (level));
            rows.add (juce::String (count) + " voices " + str (level, 2) + " dB");
        }
        expectLessThan (worst, 0.5);
        logMessage ("  -> 6 kHz band-limited noise, voices centred, wet only: wet level against the input per side (limit +-0.5 dB): " + rows.joinIntoString (", ")
                    + " (the voices are decorrelated, so their powers add and the normalization holds the sum at the input's level)");
    }

    void wetHighPass()
    {
        beginTest ("wet high-pass: off by default; on, it thins the wet's lows and leaves the dry alone");

        // An 82.4 Hz sine (low E) with one voice an octave down (41.2 Hz), mix 50%.
        const auto x = sineWave (82.41, 0.5, (size_t) (3.0 * fs));
        const auto render = [&] (bool enabled, double mixValue)
        {
            auto s = single (-12.0);
            s.mix = mixValue;
            s.wetHighPass = enabled;
            s.wetHighPassHz = 100.0;
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            return run (m, x);
        };
        const auto off = render (false, 0.5), on = render (true, 0.5), onWet = render (true, 1.0);
        const auto len = (size_t) (1.0 * fs);
        const auto cut = toDb (toneAmplitude (on.left, 82.41 / 2.0, (size_t) (1.5 * fs), len) / toneAmplitude (off.left, 82.41 / 2.0, (size_t) (1.5 * fs), len));
        const auto expected = toDb (std::abs (ampsim::Svf::responseAt (ampsim::Svf::design (ampsim::Svf::Type::highpass, 100.0, 0.70710678118654752, 0.0, fs), 41.205, fs)));

        // The dry is untouched with the high-pass on: output minus sin(pi/4) x the wet-only run is cos(pi/4) x input.
        const auto g = Multivoicer::mixGains (0.5);
        double dryError = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            dryError = std::max (dryError, std::abs ((double) on.left[i] - g.wet * (double) onWet.left[i] - g.dry * (double) x[i]));

        expectWithinAbsoluteError (cut, expected, 0.5);
        expectLessThan (dryError, 1.0e-6);
        expect (! Multivoicer::defaults().wetHighPass);
        logMessage ("  -> the octave-down voice of a low E (41.2 Hz) drops " + str (cut, 2) + " dB with the 100 Hz high-pass on (a 12 dB/oct Butterworth at 41.2 Hz: "
                    + str (expected, 2) + " dB); the dry is the input times cos(pi/4) to within " + juce::String (dryError, 9) + "; off by default");
    }

    void drift()
    {
        beginTest ("drift: a voice wanders within +-3 cents (and 0 to 2 ms) when on, and holds its pitch when off");

        const auto x = sineWave (220.0, 0.5, (size_t) (8.0 * fs));
        double ranges[2] = { 0.0, 0.0 };
        for (const auto amount : { 0.0, 1.0 })
        {
            auto s = single (0.0, 0.0);
            s.voices[0].drift = amount;
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            const auto out = run (m, x);
            // Each cycle's pitch from upward zero crossings.
            double lo = 1.0e9, hi = -1.0e9, previous = -1.0;
            for (size_t n = (size_t) (0.5 * fs); n < out.left.size(); ++n)
            {
                if (out.left[n - 1] < 0.0f && out.left[n] >= 0.0f)
                {
                    const auto crossing = (double) (n - 1) - (double) out.left[n - 1] / ((double) out.left[n] - (double) out.left[n - 1]);
                    if (previous > 0.0)
                    {
                        const auto c = cents (fs / (crossing - previous), 220.0);
                        lo = std::min (lo, c);
                        hi = std::max (hi, c);
                    }
                    previous = crossing;
                }
            }
            ranges[amount > 0.0 ? 1 : 0] = hi - lo;
            if (amount > 0.0)
            {
                expectLessThan (hi, 4.5);
                expectGreaterThan (lo, -4.5);
                expectGreaterThan (hi - lo, 2.0); // it does wander
            }
        }
        expectLessThan (ranges[0], 0.2);
        logMessage ("  -> 8 s of a 220 Hz sine through one unison voice: pitch range " + str (ranges[0], 3) + " cents with drift off, " + str (ranges[1], 2)
                    + " cents with drift on (limit +-4.5: 3 cents of pitch drift plus the timing drift's own small bend)");
    }

    void switching()
    {
        beginTest ("switching: voice counts, intervals, delays, pans, levels, spread, mix, and the high-pass change without clicks");

        const auto x = sineWave (196.0, 0.5, (size_t) (7.0 * fs));
        auto s = Multivoicer::defaults();
        for (auto& v : s.voices)
            v.drift = 0.0;
        Multivoicer m;
        m.setSettings (s);
        m.prepare (fs, blockSize);

        // A change every 250 ms; each one's largest step in the 60 ms after it, against the largest step anywhere
        // in steady stretches (a step bound for this sine's mix of voices).
        std::vector<size_t> changeAt;
        int k = 0;
        const auto out = run (m, x, [&] (size_t start)
        {
            if (start == 0 || start % (size_t) (0.25 * fs) >= (size_t) blockSize)
                return;
            ++k;
            switch (k % 9)
            {
                case 0: s.voiceCount = 8; break;
                case 1: s.voiceCount = 2; break;
                case 2: s.voices[0].semitones = 5.0; s.voices[1].semitones = -7.0; break;
                case 3: s.voices[0].delayMs = 30.0; s.voices[1].delayMs = 4.0; break;
                case 4: s.voices[0].pan = 1.0; s.voices[1].pan = -1.0; s.voiceCount = 5; break;
                case 5: s.voices[2].levelDb = 3.0; s.voices[3].levelDb = -60.0; break;
                case 6: s.spread = 0.2; s.mix = 0.9; break;
                case 7: s.wetHighPass = ! s.wetHighPass; s.wetHighPassHz = 300.0; break;
                default: s.mix = 0.3; s.spread = 1.0; s.voices[0].semitones = 12.0; s.voiceCount = 3; break;
            }
            m.setSettings (s);
            changeAt.push_back (start);
        });

        // The steady-state reference: the largest step over the whole render, outside the 60 ms after changes.
        std::vector<bool> nearChange (out.left.size(), false);
        for (auto c : changeAt)
            for (size_t i = c; i < std::min (out.left.size(), c + (size_t) (0.06 * fs)); ++i)
                nearChange[i] = true;
        double steady = 0.0, worstAfter = 0.0;
        for (size_t i = (size_t) (0.2 * fs); i < out.left.size(); ++i)
        {
            const auto step = std::max (std::abs ((double) out.left[i] - (double) out.left[i - 1]), std::abs ((double) out.right[i] - (double) out.right[i - 1]));
            (nearChange[i] ? worstAfter : steady) = std::max (nearChange[i] ? worstAfter : steady, step);
        }
        const auto ratio = worstAfter / steady;
        expectLessThan (ratio, 1.1);
        logMessage ("  -> " + juce::String ((int) changeAt.size()) + " changes on a 196 Hz sine (counts 2-8, intervals, delays 4-30 ms, hard pans, levels to off, "
                    + "spread, mix, high-pass on/off): largest step within 60 ms of a change " + str (ratio, 3) + " x the steady-state largest (limit 1.1)");
    }

    void realtime()
    {
        beginTest ("real time: nothing allocates, frees, or locks while every setting changes");

        Multivoicer m;
        auto s = Multivoicer::defaults();
        m.setSettings (s);
        m.prepare (fs, blockSize);
        const auto di = guitarDI ((int) (10.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        int blocks = 0, changes = 0;

        rtcheck::begin();
        for (size_t start = 0; start + (size_t) blockSize <= di.size(); start += (size_t) blockSize, ++blocks)
        {
            if (blocks % 23 == 0 || blocks % 97 == 1)
            {
                const auto j = blocks / 23 + blocks % 5;
                s.engine = j % 3 == 0 ? Multivoicer::Engine::mono : Multivoicer::Engine::poly;
                s.voiceCount = 1 + j % 8;
                for (int v = 0; v < Multivoicer::maxVoices; ++v)
                {
                    auto& voice = s.voices[(size_t) v];
                    voice.semitones = (double) ((j * 7 + v * 5) % 49) - 24.0;
                    voice.cents = (double) ((j + v) % 21) - 10.0;
                    voice.delayMs = (double) ((j * 3 + v * 11) % 51);
                    voice.pan = ((double) ((j + v) % 9) - 4.0) / 4.0;
                    voice.levelDb = (double) ((j + v) % 4) * -6.0;
                    voice.drift = (double) ((j + v) % 2);
                }
                s.spread = (double) (j % 5) / 4.0;
                s.mix = (double) (j % 6) / 5.0;
                s.wetHighPass = j % 4 == 1;
                s.wetHighPassHz = 60.0 + 37.0 * (double) (j % 11);
                m.setSettings (s);
                ++changes;
            }
            buffer.copyFrom (0, 0, di.data() + start, blockSize);
            buffer.copyFrom (1, 0, di.data() + start, blockSize);
            m.process (juce::dsp::AudioBlock<float> (buffer), { di.data() + start, blockSize });
        }
        const auto counts = rtcheck::end();
        expectEquals (counts.allocations, 0L);
        expectEquals (counts.frees, 0L);
        expectEquals (counts.blockingLocks, 0L);
        logMessage ("  -> " + juce::String (blocks) + " buffers, " + juce::String (changes) + " changes of every setting (engine, voice count, intervals, delays, "
                    + "pans, levels, drift, spread, mix, high-pass): " + juce::String (counts.allocations) + " allocations, " + juce::String (counts.frees)
                    + " frees, " + juce::String (counts.blockingLocks) + " blocking locks");
    }

    /// Sections joined by 5 ms crossfades: each a steady harmonic chord (or a single note), 8 partials at 1/k.
    static std::vector<float> sections (const std::vector<std::vector<double>>& notes, double secondsEach)
    {
        const auto each = (size_t) (secondsEach * fs);
        const auto fadeLength = (size_t) (0.005 * fs);
        std::vector<float> x (each * notes.size(), 0.0f);
        juce::Random random (21);
        for (size_t s = 0; s < notes.size(); ++s)
        {
            for (const auto f0 : notes[s])
            {
                for (int k = 1; k <= 8; ++k)
                {
                    const auto phase = random.nextDouble() * twoPi;
                    for (size_t i = 0; i < each + fadeLength && s * each + i < x.size(); ++i)
                    {
                        auto g = 1.0;
                        if (s > 0 && i < fadeLength)
                            g = (double) i / (double) fadeLength;
                        if (i >= each)
                            g = 1.0 - (double) (i - each) / (double) fadeLength;
                        x[s * each + i] += (float) (0.12 * g * std::sin (twoPi * f0 * k * (double) i / fs + phase) / k);
                    }
                }
            }
        }
        return x;
    }

    /// Energy off the partials (outside +-3 Hz of each harmonic of each note, times the ratio), in dB, over
    /// 2^order samples from start (order 16 resolves the window's own main lobe inside the 3 Hz).
    static double offPartialsDb (const std::vector<float>& y, size_t start, const std::vector<double>& notes, double ratio, int order = 15)
    {
        const auto mag = magnitudeSpectrum (y, start, order);
        const auto binHz = fs / (double) (1 << order);
        double on = 0.0, total = 0.0;
        for (size_t k = 1; k < mag.size(); ++k)
        {
            const auto f = (double) k * binHz;
            const auto e = mag[k] * mag[k];
            total += e;
            for (const auto f0 : notes)
            {
                const auto h = std::round (f / (f0 * ratio));
                if (h >= 1.0 && std::abs (f - h * f0 * ratio) <= 3.0)
                {
                    on += e;
                    break;
                }
            }
        }
        return 10.0 * std::log10 (std::max (1.0e-30, total - on) / total);
    }

    void monoSingleNotes()
    {
        beginTest ("Mono: single notes are PSOLA, and clean");

        // A single voice on three steady notes (A2, G3, high E), wet only, Mono against Poly.
        const std::vector<std::vector<double>> notes { { 110.0 }, { 196.0 }, { 329.63 } };
        const auto x = sections (notes, 2.5);
        juce::StringArray rows;
        for (const auto semitones : { -12.0, -24.0, 12.0, 7.0 })
        {
            double off[2] = { 0.0, 0.0 }, share = 0.0;
            for (const auto mono : { false, true })
            {
                auto s = single (semitones);
                s.engine = mono ? Multivoicer::Engine::mono : Multivoicer::Engine::poly;
                Multivoicer m;
                m.setSettings (s);
                m.prepare (fs, blockSize);
                std::vector<double> shares;
                const auto out = run (m, x, [&] (size_t start)
                {
                    if (start % (size_t) (0.5 * fs) < (size_t) blockSize && start % (size_t) (2.5 * fs) >= (size_t) (0.5 * fs))
                        shares.push_back (m.getPsolaShare());
                });
                double worst = -400.0;
                for (size_t k = 0; k < notes.size(); ++k)
                    worst = std::max (worst, offPartialsDb (out.left, k * (size_t) (2.5 * fs) + (size_t) (0.8 * fs), notes[k], Multivoicer::ratioOf (s.voices[0]), 16));
                off[mono ? 1 : 0] = worst;
                if (mono)
                    share = *std::min_element (shares.begin(), shares.end());
            }
            expectLessThan (off[1], -40.0);
            expectGreaterThan (share, 0.99);
            rows.add (str (semitones, 0) + " st: Poly " + str (off[0], 1) + " dB, Mono " + str (off[1], 1) + " dB (PSOLA share " + str (share, 3) + ")");
        }
        logMessage ("  -> energy off the shifted note's harmonics, worst of A2, G3, high E (steady, wet only; Mono limit -40 dB): " + rows.joinIntoString ("; ")
                    + ". On steady single notes both engines are clean: the granular engine's splices are aligned to whole periods too");

        // What does differ: PSOLA keeps the input's spectral envelope (its formants: on a guitar, the amp and
        // cab's colour), granular scales it with the pitch. A G3 whose partials peak at a 1.2 kHz resonance,
        // down an octave: where is the output's strongest partial?
        const auto n = (size_t) (3.0 * fs);
        std::vector<float> formant (n, 0.0f);
        for (int k = 1; k <= 40; ++k)
        {
            const auto f = 196.0 * k;
            const auto a = 0.05 / k + 1.0 / (1.0 + std::pow ((f - 1200.0) / 150.0, 2.0));
            for (size_t i = 0; i < n; ++i)
                formant[i] += (float) (0.03 * a * std::sin (twoPi * f * (double) i / fs + 0.7 * k));
        }
        double peakHz[2] = { 0.0, 0.0 };
        for (const auto mono : { false, true })
        {
            auto s = single (-12.0);
            s.engine = mono ? Multivoicer::Engine::mono : Multivoicer::Engine::poly;
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            const auto out = run (m, formant);
            constexpr int order = 16;
            const auto mag = magnitudeSpectrum (out.left, (size_t) (1.0 * fs), order);
            size_t best = 1;
            for (size_t k = 1; k < mag.size(); ++k)
                if (mag[k] > mag[best])
                    best = k;
            peakHz[mono ? 1 : 0] = (double) best * fs / (double) (1 << order);
        }
        expectWithinAbsoluteError (peakHz[1], 1176.0, 120.0); // the 12th partial of 98 Hz sits at the resonance
        expectWithinAbsoluteError (peakHz[0], 588.0, 120.0);  // granular moved the resonance down an octave
        logMessage ("  -> a G3 with a 1.2 kHz resonance, down an octave: the strongest output partial is at " + juce::String (juce::roundToInt (peakHz[1])) + " Hz in Mono (PSOLA keeps "
                    + "the resonance) and " + juce::String (juce::roundToInt (peakHz[0])) + " Hz in Poly (granular takes it down an octave with the pitch)");
    }

    void monoChords()
    {
        beginTest ("Mono: on chords it degrades to Poly (the granular fallback), with no clicks at the changes");

        // "Gracefully" here means: once the analysis can't find one period, each voice is its granular twin,
        // so Mono on a chord sounds and measures like Poly (level and spectrum), and the moves between PSOLA
        // and granular are 20 ms crossfades with no step beyond steady playing.
        const std::vector<std::vector<double>> notes { { 110.0 }, { 82.41, 123.47, 164.81 }, { 146.83 }, { 130.81, 164.81, 196.0 }, { 196.0 } };
        const auto each = 1.0;
        const auto x = sections (notes, each);
        auto s = single (-12.0);
        Stereo outs[2];
        std::vector<double> shareTrace;
        for (const auto mono : { false, true })
        {
            s.engine = mono ? Multivoicer::Engine::mono : Multivoicer::Engine::poly;
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            outs[mono ? 1 : 0] = run (m, x, [&] (size_t)
            {
                if (mono)
                    shareTrace.push_back (m.getPsolaShare());
            });
        }

        juce::StringArray rows;
        const auto blocksPerSection = (size_t) (each * fs) / (size_t) blockSize;
        for (size_t k = 0; k < notes.size(); ++k)
        {
            const auto start = k * (size_t) (each * fs) + (size_t) (0.3 * fs);
            const auto len = (size_t) (0.6 * fs);
            double meanShare = 0.0;
            const auto firstBlock = k * blocksPerSection + blocksPerSection * 3 / 10;
            for (auto b = firstBlock; b < (k + 1) * blocksPerSection; ++b)
                meanShare += shareTrace[b];
            meanShare /= (double) ((k + 1) * blocksPerSection - firstBlock);
            const auto levelDiff = toDb (rms (outs[1].left.data() + start, len) / rms (outs[0].left.data() + start, len));
            const auto offPoly = offPartialsDb (outs[0].left, start, notes[k], 0.5);
            const auto offMono = offPartialsDb (outs[1].left, start, notes[k], 0.5);
            const auto isChord = notes[k].size() > 1;
            if (isChord)
            {
                expectLessThan (meanShare, 0.05);
                expectWithinAbsoluteError (levelDiff, 0.0, 1.0);
                expectWithinAbsoluteError (offMono, offPoly, 2.0);
            }
            else
            {
                expectGreaterThan (meanShare, 0.9);
            }
            rows.add (juce::String (isChord ? "chord" : "note") + " " + juce::String ((int) k + 1) + ": PSOLA share " + str (meanShare, 3) + ", Mono vs Poly level "
                      + str (levelDiff, 2) + " dB, off-partial energy Poly " + str (offPoly, 1) + " / Mono " + str (offMono, 1) + " dB");
        }

        // Clicks: the largest step in the 120 ms around each change against the steady-state largest either side.
        double worst = 0.0;
        const auto& y = outs[1].left;
        for (size_t k = 1; k < notes.size(); ++k)
        {
            const auto change = k * (size_t) (each * fs);
            const auto steady = std::max (maxStep (y, change - (size_t) (0.5 * fs), change - (size_t) (0.05 * fs)),
                                          maxStep (y, change + (size_t) (0.4 * fs), change + (size_t) (0.9 * fs)));
            worst = std::max (worst, maxStep (y, change - (size_t) (0.02 * fs), change + (size_t) (0.1 * fs)) / steady);
        }
        expectLessThan (worst, 1.25);
        logMessage ("  -> one voice down an octave, A2 / E5 chord / D3 / C major chord / G3, 1 s each, from 0.3 s in: " + rows.joinIntoString ("; "));
        logMessage ("  -> largest step around each change in Mono, against the steady state either side: " + str (worst, 3)
                    + " (limit 1.25; a note change itself makes bigger steps than steady playing)");

        PlotSeries share { "PSOLA share of the voice (Mono)", {}, {}, plotColour (0) };
        for (size_t b = 0; b < shareTrace.size(); ++b)
        {
            share.x.push_back ((double) (b * (size_t) blockSize) / fs);
            share.y.push_back (shareTrace[b]);
        }
        PlotOptions o;
        o.title = "Mono on notes and chords: A2, E5, D3, C major, G3 (1 s each)";
        o.xLabel = "s";
        o.yLabel = "share";
        o.xMin = 0.0;
        o.xMax = 5.0;
        o.yMin = -0.05;
        o.yMax = 1.05;
        const auto png = proofDir().getChildFile ("multivoicer_mono_fallback.png");
        expect (savePlot (png, o, { share }));
        logMessage ("  -> " + png.getFullPathName());
    }

    void engineSwitch()
    {
        beginTest ("engine switch: Poly <-> Mono crossfades without a click");

        const auto x = sections ({ { 146.83 } }, 4.0);
        auto s = Multivoicer::startingPoint (Multivoicer::StartingPoint::octaveStack);
        Multivoicer m;
        m.setSettings (s);
        m.prepare (fs, blockSize);
        std::vector<size_t> switches;
        const auto out = run (m, x, [&] (size_t start)
        {
            if (start > (size_t) (0.5 * fs) && start % (size_t) (0.4 * fs) < (size_t) blockSize)
            {
                s.engine = s.engine == Multivoicer::Engine::poly ? Multivoicer::Engine::mono : Multivoicer::Engine::poly;
                m.setSettings (s);
                switches.push_back (start);
            }
        });
        std::vector<bool> near (out.left.size(), false);
        for (auto c : switches)
            for (size_t i = c; i < std::min (out.left.size(), c + (size_t) (0.05 * fs)); ++i)
                near[i] = true;
        double steady = 0.0, around = 0.0;
        for (size_t i = (size_t) (0.5 * fs); i < out.left.size(); ++i)
        {
            const auto step = std::max (std::abs ((double) out.left[i] - (double) out.left[i - 1]), std::abs ((double) out.right[i] - (double) out.right[i - 1]));
            if (near[i])
                around = std::max (around, step);
            else
                steady = std::max (steady, step);
        }
        expectLessThan (around / steady, 1.1);
        logMessage ("  -> " + juce::String ((int) switches.size()) + " switches on a sustained D3 (Octave stack): largest step within 50 ms of a switch "
                    + str (around / steady, 3) + " x the steady-state largest (limit 1.1)");
    }

    void cpu()
    {
        beginTest ("CPU: a 128-sample stereo buffer");

        const auto di = guitarDI ((int) (10.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::StringArray rows;
        const auto time = [&] (const Multivoicer::Settings& s, const juce::String& label)
        {
            Multivoicer m;
            m.setSettings (s);
            m.prepare (fs, blockSize);
            std::vector<double> micros;
            for (size_t start = 0; start + (size_t) blockSize <= di.size(); start += (size_t) blockSize)
            {
                buffer.copyFrom (0, 0, di.data() + start, blockSize);
                buffer.copyFrom (1, 0, di.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                m.process (juce::dsp::AudioBlock<float> (buffer), { di.data() + start, blockSize });
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            std::sort (micros.begin(), micros.end());
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            rows.add (label + " mean " + str (mean, 1) + " us (" + str (100.0 * mean / deadlineMicros, 2) + "%), p99 " + str (micros[(size_t) (0.99 * (double) micros.size())], 1)
                      + " us, worst " + str (micros.back(), 1) + " us");
            return mean;
        };

        time (Multivoicer::defaults(), "default (4 voices, Poly)");
        auto eight = Multivoicer::defaults();
        eight.voiceCount = 8;
        const double intervals[] = { 0.08, -0.1, -12.0, 12.0, 7.0, -5.0, 19.0, 24.0 };
        for (int v = 0; v < 8; ++v)
        {
            eight.voices[(size_t) v].semitones = intervals[v];
            eight.voices[(size_t) v].drift = 1.0;
            eight.voices[(size_t) v].levelDb = 0.0;
        }
        const auto poly = time (eight, "8 voices Poly (unison to +24, drift on)");
        auto eightMono = eight;
        eightMono.engine = Multivoicer::Engine::mono;
        const auto mono = time (eightMono, "8 voices Mono (PSOLA plus the granular fallback, one analysis)");
        expectLessThan (poly, 0.15 * deadlineMicros);
        expectLessThan (mono, 0.25 * deadlineMicros);
        logMessage ("  -> 10 s of guitar DI, of the 2.67 ms deadline: " + rows.joinIntoString ("; "));
    }

    void renders()
    {
        beginTest ("renders for listening (proof directory)");

        const auto di = guitarDI ((int) (6.0 * fs));
        juce::StringArray files;
        using SP = Multivoicer::StartingPoint;
        for (const auto engine : { Multivoicer::Engine::poly, Multivoicer::Engine::mono })
        {
            for (const auto& [point, name] : { std::pair { SP::unisonDouble, "unison_double" }, std::pair { SP::octaveStack, "octave_stack" },
                                               std::pair { SP::fifthsStack, "fifths_stack" }, std::pair { SP::doubleOctaves, "double_octaves" } })
            {
                auto s = Multivoicer::startingPoint (point);
                s.engine = engine;
                Multivoicer m;
                m.setSettings (s);
                m.prepare (fs, blockSize);
                const auto out = run (m, di);
                juce::AudioBuffer<float> stereo (2, (int) di.size());
                stereo.copyFrom (0, 0, out.left.data(), (int) di.size());
                stereo.copyFrom (1, 0, out.right.data(), (int) di.size());
                const auto file = proofDir().getChildFile ("multivoicer_" + juce::String (engine == Multivoicer::Engine::poly ? "poly_" : "mono_") + juce::String (name)
                                                           + ".wav");
                expect (writeWav (file, stereo));
                files.add (file.getFileName());
            }
        }
        expect (writeWav (proofDir().getChildFile ("multivoicer_dry.wav"), di));
        logMessage ("  -> the synthetic guitar DI through each starting point at mix 50%, Poly and Mono: " + files.joinIntoString (", ") + " (plus multivoicer_dry.wav) in "
                    + proofDir().getFullPathName() + ". Unverified by ear: Sean's to judge.");
    }
};

MultivoicerTests multivoicerTests;
} // namespace

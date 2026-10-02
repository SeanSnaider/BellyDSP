#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Gate.h"
#include "dsp/Svf.h"

#include <chrono>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::Gate;

juce::File fixture (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/gate").getChildFile (name);
}

std::vector<float> readMono (const juce::File& file)
{
    const auto b = readWav (file);
    return { b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples() };
}

size_t at (double seconds) { return (size_t) std::llround (seconds * fs); }

double ms (size_t samples) { return 1000.0 * (double) samples / fs; }

double gainDb (double g) { return g > 1.0e-12 ? 20.0 * std::log10 (g) : -240.0; }

/// Gaussian noise (Box-Muller on juce::Random), deterministic.
std::vector<double> gaussian (size_t n, double sigma, juce::int64 seed)
{
    juce::Random random (seed);
    std::vector<double> x (n);
    for (size_t i = 0; i < n; i += 2)
    {
        const auto u1 = std::max (1.0e-12, random.nextDouble()), u2 = random.nextDouble();
        const auto r = sigma * std::sqrt (-2.0 * std::log (u1));
        x[i] = r * std::cos (juce::MathConstants<double>::twoPi * u2);
        if (i + 1 < n)
            x[i + 1] = r * std::sin (juce::MathConstants<double>::twoPi * u2);
    }
    return x;
}

/// A plucked-string-like tone, the recipe in prototypes/gate.py: harmonics at 1/k with random phases,
/// harmonic k decaying at decay (1 + 0.15 (k - 1)) dB/s (highs die faster, as on a string), scaled to
/// peakDb. From stopAt on, everything also dies at stopDbPerSecond (a muted string). riseMs > 0 fades
/// the onset in (a hammer-on or tap has no pick attack).
std::vector<float> tone (double f0, double peakDb, double seconds, double decayDbPerSecond, juce::int64 seed,
                         double stopAt = -1.0, double stopDbPerSecond = 0.0, double riseMs = 0.0)
{
    juce::Random random (seed);
    const auto n = at (seconds);
    std::vector<double> x (n, 0.0);
    for (int k = 1; k <= 20 && f0 * k < 0.45 * fs; ++k)
    {
        const auto phase = random.nextDouble() * juce::MathConstants<double>::twoPi;
        const auto rate = decayDbPerSecond * (1.0 + 0.15 * (k - 1));
        for (size_t i = 0; i < n; ++i)
        {
            const auto t = (double) i / fs;
            x[i] += std::sin (juce::MathConstants<double>::twoPi * f0 * k * t + phase) / k * std::pow (10.0, -rate * t / 20.0);
        }
    }

    double peak = 1.0e-12;
    for (size_t i = 0; i < n; ++i)
    {
        const auto t = (double) i / fs;
        if (stopAt >= 0.0 && t > stopAt)
            x[i] *= std::pow (10.0, -(t - stopAt) * stopDbPerSecond / 20.0);
        if (riseMs > 0.0 && t * 1000.0 < riseMs)
            x[i] *= t * 1000.0 / riseMs;
        peak = std::max (peak, std::abs (x[i]));
    }

    std::vector<float> out (n);
    const auto scale = std::pow (10.0, peakDb / 20.0) / peak;
    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (x[i] * scale);
    return out;
}

/// A DI noise floor: 60 Hz hum with harmonics (120 Hz -6 dB, 180 Hz -9, 240 Hz -14, 300 Hz -16, as
/// single coils and ground loops make) peaking at humDb, plus Gaussian hiss at hissDb RMS.
std::vector<float> noiseFloor (double seconds, juce::int64 seed, double humDb = -66.0, double hissDb = -84.0)
{
    const auto n = at (seconds);
    const double relative[] = { 0.0, -6.0, -9.0, -14.0, -16.0 };
    std::vector<double> hum (n, 0.0);
    double peak = 1.0e-12;
    for (size_t i = 0; i < n; ++i)
    {
        for (int k = 1; k <= 5; ++k)
            hum[i] += std::pow (10.0, relative[k - 1] / 20.0) * std::sin (juce::MathConstants<double>::twoPi * 60.0 * k * (double) i / fs + 0.7 * k);
        peak = std::max (peak, std::abs (hum[i]));
    }

    const auto hiss = gaussian (n, std::pow (10.0, hissDb / 20.0), seed);
    std::vector<float> out (n);
    for (size_t i = 0; i < n; ++i)
        out[i] = (float) (hum[i] * std::pow (10.0, humDb / 20.0) / peak + hiss[i]);
    return out;
}

/// A Karplus-Strong pluck (the recipe in src/dsp/ReferenceSignals.cpp): a noise burst circulating in a
/// one-period loop through a two-point average and a decay factor. Like a real pick, it starts at full
/// swing on its first sample. Scaled to peakDb.
std::vector<float> pluck (double f0, double peakDb, double seconds, juce::int64 seed, double decay = 0.996)
{
    juce::Random random (seed);
    const auto period = (size_t) std::max (2, juce::roundToInt (fs / f0));
    std::vector<double> loop (period);
    for (auto& v : loop)
        v = 2.0 * random.nextDouble() - 1.0;

    std::vector<double> x (at (seconds));
    double peak = 1.0e-12;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const auto k = i % period;
        x[i] = loop[k];
        loop[k] = decay * 0.5 * (loop[k] + loop[(k + 1) % period]);
        peak = std::max (peak, std::abs (x[i]));
    }

    std::vector<float> out (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        out[i] = (float) (x[i] * std::pow (10.0, peakDb / 20.0) / peak);
    return out;
}

/// The first sample at or after `from` where `di`, through a 24 dB/oct Butterworth high-pass at `hz`
/// (the gate's sidechain, rebuilt here from two SVF sections), reaches `thresholdDb`: the earliest any
/// zero-latency gate detecting it could react. An independent reference for the opening tests.
size_t thresholdCrossing (const std::vector<float>& di, size_t from, double thresholdDb, double hz = 100.0)
{
    std::array<ampsim::Svf, 2> highPass;
    const double q[2] = { 1.3065629648763766, 0.5411961001461970 };
    for (size_t i = 0; i < 2; ++i)
        highPass[i].setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, hz, q[i], 0.0, fs));
    const auto level = std::pow (10.0, thresholdDb / 20.0);
    for (size_t n = 0; n < di.size(); ++n)
    {
        const auto v = highPass[1].processSample (highPass[0].processSample ((double) di[n]));
        if (n >= from && std::abs (v) >= level)
            return n;
    }
    return di.size();
}

juce::String whole (double value) { return juce::String (juce::roundToInt (value)); }

void place (std::vector<float>& base, const std::vector<float>& x, double atSeconds)
{
    const auto start = at (atSeconds);
    for (size_t i = 0; i < x.size() && start + i < base.size(); ++i)
        base[start + i] += x[i];
}

struct Run
{
    std::vector<float> out, gain;
    std::vector<float> detectorDb, openDb, closeDb; // the meters, once per buffer
};

/// Runs `audio` through the gate in 128-sample buffers, with `di` as the DI snapshot.
Run runGate (Gate& gate, const std::vector<float>& audio, const std::vector<float>& di,
             const std::function<void (size_t)>& beforeBlock = {})
{
    Run r;
    r.out = audio;
    r.gain.resize (audio.size());
    for (size_t start = 0; start < audio.size(); start += blockSize)
    {
        if (beforeBlock)
            beforeBlock (start);
        const auto len = std::min ((size_t) blockSize, audio.size() - start);
        float* channels[1] = { r.out.data() + start };
        const ampsim::BlockContext context { di.data() + start, (int) len };
        gate.process (juce::dsp::AudioBlock<float> (channels, 1, len), context);
        std::copy (gate.getGainCurve(), gate.getGainCurve() + len, r.gain.begin() + (long) start);
        r.detectorDb.push_back (gate.getDetectorLevelDb());
        r.openDb.push_back (gate.getOpenThresholdDb());
        r.closeDb.push_back (gate.getCloseThresholdDb());
    }
    return r;
}

Run runGate (Gate& gate, const std::vector<float>& di) { return runGate (gate, di, di); }

Gate::Settings fromJson (const juce::var& v)
{
    Gate::Settings s;
    s.thresholdDb = (float) (double) v["threshold_db"];
    s.hysteresisDb = (float) (double) v["hysteresis_db"];
    s.holdMs = (float) (double) v["hold_ms"];
    s.attackMs = (float) (double) v["attack_ms"];
    s.releaseMs = (float) (double) v["release_ms"];
    s.releaseMode = v["release_mode"].toString() == "classic" ? Gate::ReleaseMode::classic : Gate::ReleaseMode::adaptive;
    s.rangeDb = (float) (double) v["range_db"];
    s.detector = v["detector"].toString() == "own" ? Gate::DetectorSource::ownInput : Gate::DetectorSource::di;
    s.sidechainHighPass = (bool) v["sidechain_high_pass"];
    s.sidechainHz = (float) (double) v["sidechain_hz"];
    return s;
}

/// Opens and closes, read off the gain curve: the start of every rising stretch is an opening, the
/// start of every falling one a closing.
struct Transitions
{
    int opens = 0, closes = 0;
};

Transitions countTransitions (const std::vector<float>& gain, size_t from = 1, size_t to = SIZE_MAX)
{
    Transitions t;
    int direction = 0;
    for (size_t n = std::max<size_t> (from, 1); n < std::min (to, gain.size()); ++n)
    {
        const auto step = (double) gain[n] - (double) gain[n - 1];
        if (step > 0.0 && direction != 1)
        {
            ++t.opens;
            direction = 1;
        }
        else if (step < 0.0 && direction != -1)
        {
            ++t.closes;
            direction = -1;
        }
    }
    return t;
}

/// First index >= from where pred holds (or the size).
size_t findFrom (const std::vector<float>& x, size_t from, const std::function<bool (float)>& pred)
{
    for (size_t n = from; n < x.size(); ++n)
        if (pred (x[n]))
            return n;
    return x.size();
}

float minIn (const std::vector<float>& x, size_t a, size_t b) { return *std::min_element (x.begin() + (long) a, x.begin() + (long) b); }
float maxIn (const std::vector<float>& x, size_t a, size_t b) { return *std::max_element (x.begin() + (long) a, x.begin() + (long) b); }

/// An 8th-order Butterworth high-pass (four TPT SVF sections), double precision.
std::vector<double> highPass8 (const std::vector<float>& x, double cutoff)
{
    std::array<ampsim::Svf, 4> sections;
    for (int k = 1; k <= 4; ++k)
        sections[(size_t) k - 1].setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, cutoff,
                                                                     1.0 / (2.0 * std::sin ((2 * k - 1) * juce::MathConstants<double>::pi / 16.0)), 0.0, fs));
    std::vector<double> y (x.size());
    for (size_t n = 0; n < x.size(); ++n)
    {
        double v = x[n];
        for (auto& s : sections)
            v = s.processSample (v);
        y[n] = v;
    }
    return y;
}

/// The loudest 1 ms RMS of `hf` from 2 ms before to 40 ms after `centre`, in dB relative to refRms.
double burstDb (const std::vector<double>& hf, size_t centre, double refRms)
{
    const size_t window = 48, a = centre - at (0.002), b = centre + at (0.040);
    double sum = 0.0, worst = 0.0;
    for (size_t i = a; i < a + window; ++i)
        sum += hf[i] * hf[i];
    for (size_t k = a; k + window <= b; ++k)
    {
        worst = std::max (worst, std::sqrt (std::max (0.0, sum) / window));
        sum += hf[k + window] * hf[k + window] - hf[k] * hf[k];
    }
    return 20.0 * std::log10 (std::max (worst, 1.0e-12) / refRms);
}

juce::String joinDb (const std::vector<double>& values, int decimals = 1)
{
    juce::StringArray s;
    for (auto v : values)
        s.add (decimals == 0 ? whole (v) : juce::String (v, decimals)); // juce::String (x, 0) means "all digits"
    return s.joinIntoString (", ");
}

class GateTests final : public juce::UnitTest
{
public:
    GateTests() : juce::UnitTest ("Gate", "ampsim") {}

    void runTest() override
    {
        beginTest ("matches the Python reference simulation (golden renders, limit -100 dB); Learn lands on the same bin");
        {
            const auto cases = juce::JSON::parse (fixture ("cases.json"));
            const auto di = readMono (fixture ("input_di.wav"));
            const auto own = readMono (fixture ("input_own.wav"));
            juce::StringArray results;

            for (const auto* name : { "default", "classic_own_input", "adaptive_no_highpass" })
            {
                const auto expectedGain = readMono (fixture ("expected_gain_" + juce::String (name) + ".wav"));
                Gate g;
                g.setSettings (fromJson (cases[name]));
                g.prepare (fs, blockSize);
                const auto r = runGate (g, own, di);

                std::vector<float> expectedOut (own.size());
                for (size_t n = 0; n < own.size(); ++n)
                    expectedOut[n] = own[n] * expectedGain[n];

                const auto gainError = relativeErrorDb (r.gain, expectedGain);
                const auto outError = relativeErrorDb (r.out, expectedOut);
                const auto worstStep = maxAbsDifference (r.gain, expectedGain);
                const auto closed = std::count_if (expectedGain.begin(), expectedGain.end(), [] (float v) { return v < 0.5f; });
                expectLessThan (gainError, -100.0);
                expectLessThan (outError, -100.0);
                expectLessThan (worstStep, 1.0e-5);
                results.add (juce::String (name) + ": gain " + dB (gainError) + ", output " + dB (outError) + ", largest difference "
                             + juce::String (worstStep, 9) + " (" + whole (100.0 * (double) closed / (double) expectedGain.size()) + "% closed)");
            }
            logMessage ("  -> C++ vs Python (tests/fixtures/gate, from prototypes/gate.py), 3.5 s of DI with picks, a stop, legato, decays, chugs:");
            for (const auto& line : results)
                logMessage ("  ->   " + line);

            // Learn on 2.1 s of hum and hiss: the same histogram bin as the reference.
            const auto noise = readMono (fixture ("input_noise.wav"));
            Gate g;
            g.setSettings (fromJson (cases["learn"]));
            g.prepare (fs, blockSize);
            g.startLearn();
            expect (g.isLearning());
            runGate (g, noise);
            const auto expectedNoise = (double) cases["learn"]["expected_noise_floor_db"];
            const auto expectedThreshold = (double) cases["learn"]["expected_threshold_db"];
            expect (! g.isLearning());
            expectEquals (g.getLearnCount(), 1);
            expectWithinAbsoluteError ((double) g.getLearnedNoiseFloorDb(), expectedNoise, 1.0e-4);
            expectWithinAbsoluteError ((double) g.getLearnedThresholdDb(), expectedThreshold, 1.0e-4);
            logMessage ("  -> Learn: noise floor " + juce::String (g.getLearnedNoiseFloorDb(), 2) + " dBFS (Python " + juce::String (expectedNoise, 2)
                        + "), threshold " + juce::String (g.getLearnedThresholdDb(), 2) + " dB (Python " + juce::String (expectedThreshold, 2) + ")");
        }

        beginTest ("attenuation: between notes the gate reaches its range (mute is digital silence)");
        {
            // Gate B's job: the DI is the detector, the audio is a loud, hissy amp output.
            auto di = noiseFloor (3.5, 1, -75.0, -90.0);
            auto notesOnly = std::vector<float> (di.size(), 0.0f);
            const double onsets[] = { 0.6, 1.4, 2.2 };
            for (auto onset : onsets)
            {
                const auto note = tone (196.0, -15.0, 0.6, 20.0, 2, 0.3, 1500.0);
                place (di, note, onset);
                place (notesOnly, note, onset);
            }
            const auto hiss = gaussian (di.size(), std::pow (10.0, -35.0 / 20.0), 3);
            std::vector<float> amp (di.size());
            for (size_t n = 0; n < amp.size(); ++n)
                amp[n] = (float) (hiss[n] + 0.3 * std::tanh (20.0 * notesOnly[n]));

            juce::StringArray results;
            for (const auto range : { -90.0f, -60.0f, -40.0f, -20.0f })
            {
                Gate::Settings s;
                s.rangeDb = range;
                Gate g;
                g.setSettings (s);
                g.prepare (fs, blockSize);
                const auto r = runGate (g, amp, di);

                std::vector<double> attenuation, reachedAfter;
                bool silent = true;
                const auto floorGain = range <= Gate::muteDb ? 0.0f : (float) std::pow (10.0, range / 20.0);
                for (size_t i = 0; i < 3; ++i)
                {
                    const auto stop = at (onsets[i] + 0.3);
                    const auto a = stop + at (0.15), b = i + 1 < 3 ? at (onsets[i + 1]) - at (0.005) : at (3.45);
                    std::vector<float> in (amp.begin() + (long) a, amp.begin() + (long) b), out (r.out.begin() + (long) a, r.out.begin() + (long) b);
                    attenuation.push_back (gainDb (rms (out) / rms (in)));
                    silent = silent && std::all_of (out.begin(), out.end(), [] (float v) { return v == 0.0f; });
                    reachedAfter.push_back (ms (findFrom (r.gain, stop, [&] (float v) { return v <= floorGain * 1.0001f + 1.0e-9f; }) - stop));
                }

                if (range <= Gate::muteDb)
                {
                    expect (silent);
                    results.add ("range -90 (mute): every closed sample exactly 0 (digital silence), reached " + joinDb (reachedAfter, 0) + " ms after the stops");
                }
                else
                {
                    for (auto a : attenuation)
                        expectWithinAbsoluteError (a, (double) range, 0.05);
                    results.add ("range " + whole (range) + " dB: closed sections at " + joinDb (attenuation, 2) + " dB, reached "
                                 + joinDb (reachedAfter, 0) + " ms after the stops");
                }
            }
            logMessage ("  -> notes at -15 dBFS stopped hard (1500 dB/s), DI floor -75 dBFS hum + -90 dBFS hiss, gated signal: -35 dBFS hiss + driven notes");
            for (const auto& line : results)
                logMessage ("  ->   " + line);
        }

        beginTest ("opening speed: fully open within 1 ms of an onset");
        {
            // Two clocks: the note's first sample, and the first sample its high-passed signal reaches the
            // threshold (an independent reference, thresholdCrossing()). Picks start at full swing, so the
            // two coincide; a note that fades in reaches the threshold later, and no zero-latency gate can
            // open before that.
            struct Case
            {
                juce::String label;
                std::vector<float> note;
                bool sharpOnset;
            };
            const std::vector<Case> cases {
                { "picked note at -20 dBFS", pluck (196.0, -20.0, 0.4, 4), true },
                { "quiet pick at -45 dBFS (10 dB over the threshold)", pluck (196.0, -45.0, 0.4, 5), true },
                { "picked low E at -30 dBFS", pluck (82.41, -30.0, 0.4, 6), true },
                { "harmonic tone at -45 dBFS, random phases (starts near zero)", tone (196.0, -45.0, 0.4, 20.0, 5), false },
                { "tapped note at -40 dBFS, 3 ms fade-in", tone (196.0, -40.0, 0.4, 20.0, 8, -1.0, 0.0, 3.0), false },
            };
            for (const auto& c : cases)
            {
                auto di = noiseFloor (1.2, 7, -75.0, -90.0);
                const auto onset = at (0.8);
                place (di, c.note, 0.8);
                Gate g;
                g.setSettings ({});
                g.prepare (fs, blockSize);
                const auto r = runGate (g, di);
                expectEquals (r.gain[onset - 1], 0.0f); // closed before it
                const auto crossing = thresholdCrossing (di, onset, Gate::Settings {}.thresholdDb);
                const auto starts = findFrom (r.gain, onset - 1, [] (float v) { return v > 0.0f; });
                const auto half = findFrom (r.gain, onset, [] (float v) { return v >= 0.5f; });
                const auto full = findFrom (r.gain, onset, [] (float v) { return v >= 1.0f; });
                expectEquals ((int) starts, (int) crossing); // reacts on the very sample the signal gets there
                expectLessOrEqual (ms (full - crossing), 1.0);
                if (c.sharpOnset)
                    expectLessOrEqual (ms (full - onset), 1.0);
                logMessage ("  -> " + c.label + ": reaches the threshold at +" + juce::String (ms (crossing - onset), 2) + " ms; gain starts rising at +"
                            + juce::String (ms (starts - onset), 2) + " ms, -6 dB at +" + juce::String (ms (half - onset), 2) + ", 0 dB at +"
                            + juce::String (ms (full - onset), 2) + " ms (" + juce::String (ms (full - crossing), 2) + " ms after the crossing)");
            }
        }

        beginTest ("chatter: a note hovering at the threshold, with and without hysteresis");
        {
            // A 330 Hz sine whose level starts 4 dB over the threshold, decays 4 dB/s, and wobbles
            // +-2.5 dB at 6 Hz (beating strings), so it hovers around the threshold for a second.
            const auto n = at (3.5);
            std::vector<float> x (n);
            for (size_t i = 0; i < n; ++i)
            {
                const auto t = (double) i / fs;
                const auto levelDb = -55.0 + 4.0 - 4.0 * t + 2.5 * std::sin (juce::MathConstants<double>::twoPi * 6.0 * t);
                x[i] = (float) (std::pow (10.0, levelDb / 20.0) * std::sin (juce::MathConstants<double>::twoPi * 330.0 * t));
            }

            const auto transitions = [&] (float hysteresis)
            {
                Gate::Settings s;
                s.hysteresisDb = hysteresis;
                Gate g;
                g.setSettings (s);
                g.prepare (fs, blockSize);
                return countTransitions (runGate (g, x).gain);
            };
            const auto wide = transitions (8.0f), none = transitions (0.0f), narrow = transitions (3.0f);
            expectEquals (wide.closes, 1);
            expectEquals (wide.opens, 0);
            expectGreaterOrEqual (none.opens + none.closes, 10);
            logMessage ("  -> threshold -55 dB, level -51 dB falling 4 dB/s with a +-2.5 dB wobble: hysteresis 8 dB (default): " + juce::String (wide.closes)
                        + " close, " + juce::String (wide.opens) + " reopens; 3 dB: " + juce::String (narrow.closes) + " closes, " + juce::String (narrow.opens)
                        + " reopens; 0 dB: " + juce::String (none.closes) + " closes, " + juce::String (none.opens) + " reopens");
        }

        beginTest ("hold: bridges the gaps between choked tremolo-picked notes, on top of what the detector bridges itself");
        {
            // Two picks: the first rings 30 ms and is choked over 1 ms, the second comes `gap` ms after the
            // choke. Does the gate start a release in between? The longest gap it rides through is the
            // detector's own tail (the 10 ms window plus the sidechain high-pass ringing down) plus the hold.
            const auto floorSignal = noiseFloor (1.0, 150, -75.0, -90.0);
            const auto releasesInGap = [&] (double f0, float holdMs, int gapMs)
            {
                auto di = floorSignal;
                auto first = pluck (f0, -18.0, 0.031, 151);
                for (size_t i = at (0.030); i < first.size(); ++i)
                    first[i] *= (float) (1.0 - (double) (i - at (0.030)) / (double) at (0.001));
                place (di, first, 0.6);
                const auto second = at (0.631 + gapMs / 1000.0);
                place (di, pluck (f0, -18.0, 0.2, 152), (double) second / fs);
                Gate::Settings settings;
                settings.holdMs = holdMs;
                Gate g;
                g.setSettings (settings);
                g.prepare (fs, blockSize);
                return minIn (runGate (g, di).gain, at (0.601), second + 1) < 1.0f;
            };
            const auto longestBridged = [&] (double f0, float holdMs)
            {
                int gap = 1;
                while (gap < 150 && ! releasesInGap (f0, holdMs, gap))
                    ++gap;
                return gap - 1;
            };

            juce::StringArray results;
            for (const auto& [label, f0] : std::vector<std::pair<juce::String, double>> { { "A2 (110 Hz)", 110.0 }, { "E4 (330 Hz)", 330.0 } })
            {
                const auto none = longestBridged (f0, 0.0f), byDefault = longestBridged (f0, 10.0f), longer = longestBridged (f0, 30.0f);
                expectWithinAbsoluteError (byDefault - none, 10, 1);
                expectWithinAbsoluteError (longer - none, 30, 1);
                results.add (label + ": hold 0 rides through gaps up to " + juce::String (none) + " ms, hold 10 (default) " + juce::String (byDefault)
                             + " ms, hold 30 " + juce::String (longer) + " ms");
            }
            logMessage ("  -> longest choked gap without a release (1 ms steps): " + results.joinIntoString ("; "));
        }

        beginTest ("Learn: publishes its result with a count; a device restart mid-measurement starts it over");
        {
            Gate g;
            g.setSettings ({});
            g.prepare (fs, blockSize);
            g.startLearn();
            runGate (g, noiseFloor (1.0, 160));
            const auto halfway = g.getLearnProgress();
            g.prepare (fs, blockSize); // the device restarts
            expect (g.isLearning());
            runGate (g, noiseFloor (1.0, 161));
            const auto beforeEnd = g.getLearnCount();
            runGate (g, noiseFloor (1.2, 162));
            expectEquals (beforeEnd, 0);
            expectEquals (g.getLearnCount(), 1);
            expect (! g.isLearning());
            expectWithinAbsoluteError ((double) g.getLearnedThresholdDb(), (double) Gate::thresholdForNoiseFloor (g.getLearnedNoiseFloorDb(), 8.0), 1.0e-4);
            logMessage ("  -> progress " + juce::String (halfway, 2) + " when the device restarted; the measurement started over and finished 2 s later: noise floor "
                        + juce::String (g.getLearnedNoiseFloorDb(), 2) + " dBFS, threshold " + juce::String (g.getLearnedThresholdDb(), 2) + " dB (count "
                        + juce::String (g.getLearnCount()) + ")");
        }

        beginTest ("clicks: high-frequency energy at every open and close, against a hard gate");
        {
            // The gated signal is a pure 220 Hz sine, so anything above 4 kHz in the output is the gate's
            // own doing. The DI opens and closes it: 1 kHz bursts, 150 ms on, 250 ms off.
            const auto n = at (1.6);
            std::vector<float> audio (n), di (n, 0.0f);
            for (size_t i = 0; i < n; ++i)
                audio[i] = (float) (std::pow (10.0, -12.0 / 20.0) * std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) i / fs));
            const double onsets[] = { 0.4, 0.8, 1.2 };
            for (auto onset : onsets)
                for (size_t i = at (onset); i < at (onset + 0.15); ++i)
                    di[i] = (float) (0.1 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) (i - at (onset)) / fs));

            Gate g;
            g.setSettings ({});
            g.prepare (fs, blockSize);
            const auto r = runGate (g, audio, di);
            std::vector<float> hard (n);
            for (size_t i = 0; i < n; ++i)
                hard[i] = audio[i] * (r.gain[i] > 0.5f ? 1.0f : 0.0f);

            const auto ref = rms (audio);
            const auto hf = highPass8 (r.out, 4000.0), hardHf = highPass8 (hard, 4000.0);
            constexpr double limit = -45.0, belowHard = 25.0;
            std::vector<double> opens, closes, hardOpens, hardCloses;
            for (auto onset : onsets)
            {
                const auto openAt = at (onset);
                const auto closeAt = findFrom (r.gain, at (onset + 0.15), [] (float v) { return v < 1.0f; });
                opens.push_back (burstDb (hf, openAt, ref));
                closes.push_back (burstDb (hf, closeAt, ref));
                hardOpens.push_back (burstDb (hardHf, openAt, ref));
                hardCloses.push_back (burstDb (hardHf, closeAt, ref));
            }
            for (size_t i = 0; i < 3; ++i)
            {
                expectLessThan (opens[i], limit);
                expectLessThan (closes[i], limit);
                expectLessThan (opens[i], hardOpens[i] - belowHard);
                expectLessThan (closes[i], hardCloses[i] - belowHard);
            }
            logMessage ("  -> limit: the loudest 1 ms above 4 kHz within 40 ms of a transition stays " + whole (-limit)
                        + " dB under the note and " + whole (belowHard) + " dB under a hard gate (same decisions, gain stepped 0/1)");
            logMessage ("  -> opens (0.5 ms raised cosine): " + joinDb (opens) + " dB; hard gate " + joinDb (hardOpens) + " dB");
            logMessage ("  -> closes (adaptive: a stop, so the 20 ms release): " + joinDb (closes) + " dB; hard gate " + joinDb (hardCloses) + " dB");
        }

        beginTest ("adaptive release: a sharp stop closes fast, a natural decay slowly");
        {
            // A learned threshold over a realistic floor, then one note per run.
            const auto learn = [&]
            {
                Gate g;
                g.setSettings ({});
                g.prepare (fs, blockSize);
                g.startLearn();
                runGate (g, noiseFloor (2.1, 21));
                return g.getLearnedThresholdDb();
            };
            const auto threshold = learn();

            struct Case
            {
                juce::String name;
                Gate::ReleaseMode mode;
                double decay, stopAt, stopRate, peakDb = -15.0;
            };
            // A sweep of decay rates (the whole note dying at that rate, from -15 dBFS), then a hard stop,
            // then classic mode for comparison.
            std::vector<Case> cases;
            const std::vector<double> rates { 15.0, 30.0, 60.0, 90.0, 120.0, 150.0, 250.0 };
            for (auto rate : rates)
                cases.push_back ({ "decay " + whole (rate) + " dB/s", Gate::ReleaseMode::adaptive, rate, -1.0, 0.0 });
            cases.push_back ({ "stop (1500 dB/s)", Gate::ReleaseMode::adaptive, 30.0, 0.4, 1500.0 });
            cases.push_back ({ "classic: stop (1500 dB/s)", Gate::ReleaseMode::classic, 30.0, 0.4, 1500.0 });
            cases.push_back ({ "classic: decay 30 dB/s", Gate::ReleaseMode::classic, 30.0, -1.0, 0.0 });
            cases.push_back ({ "quiet note (-35 dBFS) stopped", Gate::ReleaseMode::adaptive, 30.0, 0.4, 1500.0, -35.0 });

            std::vector<double> lengths, afterStop;
            std::vector<Run> runs;
            std::vector<size_t> starts;
            for (const auto& k : cases)
            {
                auto di = noiseFloor (5.5, 22);
                const auto noteAt = 0.5;
                place (di, tone (196.0, k.peakDb, 5.0, k.decay, 23, k.stopAt, k.stopRate), noteAt);
                Gate::Settings s;
                s.thresholdDb = threshold;
                s.releaseMode = k.mode;
                Gate g;
                g.setSettings (s);
                g.prepare (fs, blockSize);
                runs.push_back (runGate (g, di));

                const auto& r = runs.back();
                const auto start = findFrom (r.gain, at (noteAt + 0.05), [] (float v) { return v < 1.0f; });
                const auto end = findFrom (r.gain, start, [] (float v) { return v <= 1.0e-3f; });
                starts.push_back (start);
                lengths.push_back (ms (end - start));
                afterStop.push_back (k.stopAt >= 0.0 ? ms (end - at (noteAt + k.stopAt)) : 0.0);
            }

            juce::StringArray sweep;
            for (size_t i = 0; i < rates.size(); ++i)
                sweep.add (whole (rates[i]) + " dB/s: " + juce::String (lengths[i], 1));
            const auto stop = rates.size(), classicStop = stop + 1, classicDecay = stop + 2, quietStop = stop + 3;
            logMessage ("  -> learned threshold " + juce::String (threshold, 2) + " dB, release knob 250 ms. Release length (0 to -60 dB) by how fast the note dies, adaptive:");
            logMessage ("  ->   " + sweep.joinIntoString ("; ") + " ms");
            logMessage ("  ->   hard stop (1500 dB/s): " + juce::String (lengths[stop], 1) + " ms, silent (-60 dB) " + whole (afterStop[stop])
                        + " ms after the stop; " + juce::String (lengths[1] / lengths[stop], 1) + "x faster than the 30 dB/s decay");
            logMessage ("  ->   the same stop on a quiet note (-35 dBFS, " + juce::String (-35.0 - (threshold - 8.0), 1) + " dB over the close threshold): "
                        + juce::String (lengths[quietStop], 1) + " ms, silent " + whole (afterStop[quietStop]) + " ms after the stop");
            logMessage ("  -> classic: the stop " + juce::String (lengths[classicStop], 1) + " ms (silent " + whole (afterStop[classicStop]) + " ms after it), the 30 dB/s decay "
                        + juce::String (lengths[classicDecay], 1) + " ms");

            expectWithinAbsoluteError (lengths[0], 250.0, 1.0);
            expectWithinAbsoluteError (lengths[1], 250.0, 1.0);
            for (size_t i = 1; i < rates.size(); ++i)
                expectLessOrEqual (lengths[i], lengths[i - 1] + 0.5); // faster decays never close slower
            expectLessThan (lengths[stop], 25.0);
            expectLessThan (lengths[quietStop], 40.0);
            expectWithinAbsoluteError (lengths[classicStop], 250.0, 1.0);
            expectWithinAbsoluteError (lengths[classicDecay], 250.0, 1.0);

            // The plot: the slow and fast ends, the sweep's case nearest the middle (70 ms), classic dashed.
            size_t middle = 0;
            for (size_t i = 0; i < rates.size(); ++i)
                if (std::abs (std::log (lengths[i] / 70.0)) < std::abs (std::log (lengths[middle] / 70.0)))
                    middle = i;
            std::vector<PlotSeries> series;
            int colour = 0;
            for (const auto i : { (size_t) 1, middle, stop, classicStop })
            {
                PlotSeries p { (cases[i].mode == Gate::ReleaseMode::adaptive ? "adaptive: " : "") + cases[i].name, {}, {}, plotColour (colour++), 2.0f,
                               cases[i].mode == Gate::ReleaseMode::classic };
                for (size_t n = starts[i] - at (0.02); n < std::min (runs[i].gain.size(), starts[i] + at (0.4)); n += 12)
                {
                    p.x.push_back (ms (n) - ms (starts[i]));
                    p.y.push_back (std::max (-100.0, gainDb (runs[i].gain[n])));
                }
                series.push_back (p);
            }

            PlotOptions o;
            o.title = "Gate release from where it starts: adaptive (solid) vs classic (dashed), release knob 250 ms";
            o.xLabel = "Time from the start of the release (ms)";
            o.yLabel = "Gain (dB)";
            o.xMin = -20.0; o.xMax = 400.0; o.yMin = -100.0; o.yMax = 5.0;
            const auto png = proofDir().getChildFile ("gate_adaptive_release.png");
            expect (savePlot (png, o, series));
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("legato: after Learn, notes 15 to 20 dB under the picking keep the gate open (plot)");
        {
            // Learn first, on 2.2 s of muted strings: hum at -66 dBFS with harmonics plus -84 dBFS hiss.
            Gate g;
            g.setSettings ({});
            g.prepare (fs, blockSize);
            g.startLearn();
            const auto muted = noiseFloor (2.2, 31);
            std::vector<double> progress;
            runGate (g, muted, muted, [&] (size_t start) { if (start % at (0.5) < (size_t) blockSize) progress.push_back (g.getLearnProgress()); });
            expectEquals (g.getLearnCount(), 1);
            const auto threshold = g.getLearnedThresholdDb(), noise = g.getLearnedNoiseFloorDb();
            Gate::Settings s;
            s.thresholdDb = threshold; // what the message thread does with the result
            g.setSettings (s);

            // Then the phrase: four picked notes at -12 dBFS, ten hammer-ons and taps alternating -27 and
            // -32 dBFS (15 and 20 dB quieter) with soft 3 ms onsets, a last tapped note left ringing, a
            // pause, then three palm-muted chugs and a hard stop.
            auto x = noiseFloor (5.0, 32);
            const auto t0 = 0.4, step = 0.125;
            for (int k = 0; k < 4; ++k)
                place (x, tone (110.0 * (1.0 + 0.06 * k), -12.0, step, 30.0, 40 + k), t0 + step * k);
            const auto legatoStart = t0 + 4 * step;
            for (int k = 0; k < 10; ++k)
                place (x, tone (220.0 * std::pow (2.0, k / 12.0), k % 2 == 0 ? -27.0 : -32.0, step, 35.0, 50 + k, -1.0, 0.0, 3.0), legatoStart + step * k);
            const auto legatoEnd = legatoStart + 10 * step;
            place (x, tone (392.0, -27.0, 1.6, 35.0, 60, -1.0, 0.0, 3.0), legatoEnd);
            const auto chugs = legatoEnd + 2.0;
            for (int k = 0; k < 3; ++k)
                place (x, tone (82.41, -14.0, 0.12, 250.0, 70 + k, k == 2 ? 0.06 : -1.0, 1500.0), chugs + 0.12 * k);

            const auto r = runGate (g, x);
            const auto a = at (legatoStart), b = at (legatoEnd);
            const auto lowest = minIn (r.gain, a, b);
            float lowestDetector = 0.0f;
            for (auto i = a / (size_t) blockSize; i < b / (size_t) blockSize; ++i)
                lowestDetector = std::min (lowestDetector, r.detectorDb[i]);
            expectEquals (lowest, 1.0f);

            // ...and it still closes in the gaps: after the ringing note dies away and after the stop.
            const auto closedAfterRing = maxIn (r.gain, at (chugs - 0.2), at (chugs - 0.01));
            const auto stopAt = at (chugs + 0.24 + 0.06);
            const auto silentAfter = findFrom (r.gain, stopAt, [] (float v) { return v == 0.0f; });
            expectEquals (closedAfterRing, 0.0f);
            expectLessThan (ms (silentAfter - stopAt), 120.0);

            // The same phrase through a by-ear "tight metal" setting (threshold turned up until the amp's
            // hiss is gone while playing) chops the legato.
            Gate::Settings tight;
            tight.thresholdDb = -30.0f;
            tight.hysteresisDb = 2.0f;
            tight.holdMs = 0.0f;
            tight.releaseMode = Gate::ReleaseMode::classic;
            tight.releaseMs = 20.0f;
            Gate t;
            t.setSettings (tight);
            t.prepare (fs, blockSize);
            const auto chopped = minIn (runGate (t, x).gain, a, b);
            expectLessThan (gainDb (chopped), -20.0);

            logMessage ("  -> Learn progress at 0.5 s steps: " + joinDb (progress, 2) + "; noise floor " + juce::String (noise, 2) + " dBFS -> threshold "
                        + juce::String (threshold, 2) + " dB, close threshold " + juce::String (threshold - s.hysteresisDb, 2) + " dB");
            logMessage ("  -> legato (10 notes, 15/20 dB under the picking): lowest gain " + juce::String (gainDb (lowest), 2) + " dB; the detector stayed "
                        + juce::String (lowestDetector - (threshold - s.hysteresisDb), 1) + " dB above the close threshold (per-buffer levels)");
            logMessage ("  -> it still closes: fully shut before the chugs (largest gain in the 190 ms before them " + juce::String (closedAfterRing, 3)
                        + "), and silent " + whole (ms (silentAfter - stopAt)) + " ms after the hard stop");
            logMessage ("  -> a by-ear 'tight metal' setting (-30 dB, 2 dB hysteresis, no hold, classic 20 ms) on the same phrase: lowest legato gain "
                        + whole (gainDb (chopped)) + " dB");

            // The plot: detector level against both thresholds, and the gain.
            PlotSeries level { "detector level (dBFS)", {}, {}, plotColour (0), 1.5f }, gain { "gain (dB)", {}, {}, plotColour (2), 2.5f };
            PlotSeries openLine { "open threshold", {}, {}, plotColour (1), 1.5f }, closeLine { "close threshold", {}, {}, plotColour (3), 1.5f, true };
            PlotSeries floorLine { "learned noise floor", {}, {}, plotColour (6), 1.0f, true };
            for (size_t i = 0; i < r.detectorDb.size(); ++i)
            {
                const auto time = (double) (i * (size_t) blockSize) / fs;
                level.x.push_back (time);
                level.y.push_back (std::max (-100.0f, r.detectorDb[i]));
            }
            for (size_t n = 0; n < r.gain.size(); n += 24)
            {
                gain.x.push_back ((double) n / fs);
                gain.y.push_back (std::max (-100.0, gainDb (r.gain[n])));
            }
            for (const auto time : { 0.0, (double) x.size() / fs })
            {
                openLine.x.push_back (time);
                openLine.y.push_back (threshold);
                closeLine.x.push_back (time);
                closeLine.y.push_back (threshold - s.hysteresisDb);
                floorLine.x.push_back (time);
                floorLine.y.push_back (noise);
            }
            PlotOptions o;
            o.title = "Gate after Learn: 4 picked notes, 10 legato notes 15-20 dB quieter, a ringing note, chugs with a hard stop";
            o.xLabel = "Time (s)";
            o.yLabel = "dB";
            o.xMin = 0.0; o.xMax = 5.0; o.yMin = -100.0; o.yMax = 5.0;
            const auto png = proofDir().getChildFile ("gate_note_sequence.png");
            expect (savePlot (png, o, { floorLine, closeLine, openLine, level, gain }));
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("hum: the sidechain high-pass keeps 60 Hz hum from holding the gate open; low E and drop A still open it");
        {
            juce::StringArray results;
            for (const auto& [label, f0] : std::vector<std::pair<juce::String, double>> { { "low E (82.4 Hz)", 82.41 }, { "drop A (55 Hz)", 55.0 } })
            {
                for (const bool highPass : { true, false })
                {
                    // Hum at -48 dBFS peak (7 dB over the -55 dB threshold), and at 1.5 s a soft low note at -40 dBFS.
                    auto di = sine (60.0, std::pow (10.0, -48.0 / 20.0), (int) at (2.2));
                    const auto onset = at (1.5);
                    place (di, tone (f0, -40.0, 0.6, 20.0, 80), 1.5);
                    Gate::Settings s;
                    s.sidechainHighPass = highPass;
                    Gate g;
                    g.setSettings (s);
                    g.prepare (fs, blockSize);
                    const auto r = runGate (g, di);
                    const auto humLevel = maxIn (r.detectorDb, at (0.7) / blockSize, at (1.45) / blockSize);
                    const auto humGain = maxIn (r.gain, at (0.7), at (1.45));
                    const auto noteLevel = maxIn (r.detectorDb, onset / blockSize + 2, onset / blockSize + 40);
                    const auto opened = findFrom (r.gain, onset, [] (float v) { return v >= 1.0f; });
                    const auto crossing = thresholdCrossing (di, onset, s.thresholdDb);

                    if (highPass)
                    {
                        expectEquals (humGain, 0.0f);
                        expectLessOrEqual (ms (opened - crossing), 1.0);
                        expectLessOrEqual (ms (opened - onset), 5.0);
                        results.add (label + ", high-pass on: hum reads " + juce::String (humLevel, 1) + " dB, gate shut (gain " + juce::String (humGain, 1)
                                     + "); the note reads " + juce::String (noteLevel, 1) + " dB and opens it fully " + juce::String (ms (opened - onset), 2)
                                     + " ms after it starts (" + juce::String (ms (opened - crossing), 2) + " ms after its high-passed signal reaches the threshold)");
                    }
                    else
                    {
                        expectEquals (humGain, 1.0f);
                        results.add (label + ", high-pass off: hum reads " + juce::String (humLevel, 1) + " dB and holds the gate open (gain " + juce::String (humGain, 1) + ")");
                    }
                }
            }
            for (const auto& line : results)
                logMessage ("  -> " + line);
        }

        beginTest ("linking: Gate B applies Gate A's curve exactly; the curve-only path reproduces process(); switching never steps");
        {
            auto di = noiseFloor (3.0, 90);
            place (di, tone (146.8, -12.0, 0.5, 20.0, 91, 0.3, 1500.0), 0.5);
            place (di, tone (220.0, -30.0, 1.2, 40.0, 92), 1.2);
            place (di, tone (82.41, -15.0, 0.1, 250.0, 93), 2.6);
            const auto hiss = gaussian (di.size(), 0.02, 94);
            std::vector<float> amp (di.size());
            for (size_t n = 0; n < amp.size(); ++n)
                amp[n] = (float) (hiss[n] + 0.4 * std::tanh (25.0 * di[n]));

            // A in pre FX (detecting from the DI, gating the guitar); B after the amp, linked. C runs A's
            // settings through the curve-only path.
            Gate a, b, c;
            for (auto* g : { &a, &b, &c })
            {
                g->setSettings ({});
                g->prepare (fs, blockSize);
            }
            auto outA = di, outB = amp;
            std::vector<float> curveA (di.size()), curveC (di.size());
            for (size_t start = 0; start < di.size(); start += blockSize)
            {
                const auto len = std::min ((size_t) blockSize, di.size() - start);
                const ampsim::BlockContext context { di.data() + start, (int) len };
                float* chA[1] = { outA.data() + start };
                float* chB[1] = { outB.data() + start };
                a.process (juce::dsp::AudioBlock<float> (chA, 1, len), context);
                b.processWithGain (juce::dsp::AudioBlock<float> (chB, 1, len), a.getGainCurve());
                c.computeGainCurve (di.data() + start, (int) len);
                std::copy (a.getGainCurve(), a.getGainCurve() + len, curveA.begin() + (long) start);
                std::copy (c.getGainCurve(), c.getGainCurve() + len, curveC.begin() + (long) start);
            }
            std::vector<float> expectedB (amp.size());
            for (size_t n = 0; n < amp.size(); ++n)
                expectedB[n] = amp[n] * curveA[n]; // its own statement, so the product is rounded to float like the gate's
            const auto worstB = maxAbsDifference (outB, expectedB);
            const auto transitions = countTransitions (curveA);
            expectEquals (worstB, 0.0);
            expectEquals (maxAbsDifference (curveA, curveC), 0.0);
            expectGreaterOrEqual (transitions.closes, 3);
            logMessage ("  -> linked B on a different signal (hissy amp output) vs its input x A's curve: largest difference " + juce::String (worstB)
                        + " over " + juce::String (transitions.opens) + " opens and " + juce::String (transitions.closes) + " closes");
            logMessage ("  -> computeGainCurve() on the DI vs the curve process() computed: largest difference " + juce::String (maxAbsDifference (curveA, curveC)));

            // Switching B between its own curve and A's. A is open on a soft note that B (threshold -40)
            // ignores, so at every switch the two curves disagree completely. B's input is a constant 0.5,
            // so its output is exactly its applied gain.
            auto soft = noiseFloor (3.0, 95);
            place (soft, tone (196.0, -50.0, 3.0, 0.0, 96), 0.2); // steady: A (close -63) holds open, B (close -48) shuts
            Gate a2, b2;
            Gate::Settings bSettings;
            bSettings.thresholdDb = -40.0f;
            a2.setSettings ({});
            b2.setSettings (bSettings);
            a2.prepare (fs, blockSize);
            b2.prepare (fs, blockSize);
            std::vector<float> applied (soft.size(), 0.5f);
            std::vector<size_t> switches;
            for (size_t start = 0, block = 0; start < soft.size(); start += blockSize, ++block)
            {
                const auto len = std::min ((size_t) blockSize, soft.size() - start);
                const ampsim::BlockContext context { soft.data() + start, (int) len };
                auto scratch = std::vector<float> (soft.begin() + (long) start, soft.begin() + (long) (start + len));
                float* chA[1] = { scratch.data() };
                a2.process (juce::dsp::AudioBlock<float> (chA, 1, len), context);
                const bool linked = (start >= at (1.0) && start < at (1.8)) || start >= at (2.5);
                if (start > 0 && linked != ((start - blockSize >= at (1.0) && start - blockSize < at (1.8)) || start - blockSize >= at (2.5)))
                    switches.push_back (start);
                float* chB[1] = { applied.data() + start };
                if (linked)
                    b2.processWithGain (juce::dsp::AudioBlock<float> (chB, 1, len), a2.getGainCurve());
                else
                    b2.process (juce::dsp::AudioBlock<float> (chB, 1, len), context);
            }
            std::vector<double> steps, before, after;
            for (auto sw : switches)
            {
                steps.push_back (2.0 * maxStep (applied, sw - 1, sw + at (0.012)));
                before.push_back (2.0 * applied[sw - 1]);
                after.push_back (2.0 * applied[sw + at (0.011)]);
            }
            for (auto st : steps)
                expectLessThan (st, 1.01 / (Gate::linkFadeSeconds * fs));
            logMessage ("  -> B switched own -> A's -> own -> A's at " + juce::String (switches.size()) + " points: gain " + joinDb (before, 3) + " before, "
                        + joinDb (after, 3) + " 11 ms after; largest step per sample " + joinDb (steps, 5) + " (a 10 ms fade is 0.00208)");

            // Gate A switched off (the chain skips it) while B follows: A's curve keeps coming from
            // computeGainCurve() on the DI, and re-enabling it (reset(), then process()) carries on with
            // exactly the curve an always-on A computes.
            Gate always, toggled;
            for (auto* g : { &always, &toggled })
            {
                g->setSettings ({});
                g->prepare (fs, blockSize);
            }
            double worstToggle = 0.0;
            for (size_t start = 0, block = 0; start < di.size(); start += blockSize, ++block)
            {
                const auto len = std::min ((size_t) blockSize, di.size() - start);
                const ampsim::BlockContext context { di.data() + start, (int) len };
                auto x1 = std::vector<float> (di.begin() + (long) start, di.begin() + (long) (start + len)), x2 = x1;
                float* ch1[1] = { x1.data() };
                float* ch2[1] = { x2.data() };
                always.process (juce::dsp::AudioBlock<float> (ch1, 1, len), context);
                const bool off = (block / 100) % 2 == 1; // off for 100 buffers at a time
                if (off)
                {
                    toggled.computeGainCurve (di.data() + start, (int) len);
                }
                else
                {
                    if (block > 0 && ((block - 1) / 100) % 2 == 1)
                        toggled.reset(); // what the chain does right before re-enabling a skipped block
                    toggled.process (juce::dsp::AudioBlock<float> (ch2, 1, len), context);
                }
                for (size_t i = 0; i < len; ++i)
                    worstToggle = std::max (worstToggle, (double) std::abs (always.getGainCurve()[i] - toggled.getGainCurve()[i]));
            }
            expectEquals (worstToggle, 0.0);
            logMessage ("  -> A off and on every 100 buffers (curve-only while off, reset() on re-enable): largest difference from an always-on A "
                        + juce::String (worstToggle));
        }

        beginTest ("zero latency: an open gate passes its input bit for bit");
        {
            Gate g;
            g.setSettings ({});
            g.prepare (fs, blockSize);
            expectEquals (g.latencySamples(), 0);
            expect (! g.isStereo());

            const auto x = sine (440.0, 0.1, (int) at (1.0));
            const auto r = runGate (g, x);
            expectEquals (maxAbsDifference (r.out, x), 0.0);

            // A guitar phrase: wherever the gate is fully open, the output is the input, sample for sample.
            Gate h;
            h.setSettings ({});
            h.prepare (fs, blockSize);
            auto di = guitarDI ((int) at (4.0));
            const auto hum = noiseFloor (4.0, 100, -75.0, -90.0);
            for (size_t n = 0; n < di.size(); ++n)
                di[n] += hum[n];
            const auto rg = runGate (h, di);
            size_t openSamples = 0;
            double worst = 0.0;
            for (size_t n = 0; n < di.size(); ++n)
                if (rg.gain[n] == 1.0f)
                {
                    ++openSamples;
                    worst = std::max (worst, (double) std::abs (rg.out[n] - di[n]));
                }
            expectEquals (worst, 0.0);
            logMessage ("  -> latencySamples() = 0; a -20 dBFS sine comes out bit-identical; on the guitar phrase the gate is fully open for "
                        + juce::String (100.0 * (double) openSamples / (double) di.size(), 1) + "% of samples, all of them bit-identical to the input");
        }

        beginTest ("detector source: the DI or the gate's own input");
        {
            auto notes = noiseFloor (2.0, 110, -75.0, -90.0);
            place (notes, tone (196.0, -20.0, 0.5, 20.0, 111), 1.0);
            const auto quiet = noiseFloor (2.0, 112, -75.0, -90.0);
            juce::StringArray results;
            for (const auto& [label, di, own] : std::vector<std::tuple<juce::String, std::vector<float>, std::vector<float>>> {
                     { "note on the DI only", notes, quiet }, { "note on the gate's input only", quiet, notes } })
            {
                for (const auto source : { Gate::DetectorSource::di, Gate::DetectorSource::ownInput })
                {
                    Gate::Settings s;
                    s.detector = source;
                    Gate g;
                    g.setSettings (s);
                    g.prepare (fs, blockSize);
                    const auto r = runGate (g, own, di);
                    const auto during = minIn (r.gain, at (1.01), at (1.3));
                    const auto noteOnDetector = (source == Gate::DetectorSource::di) == label.contains ("DI");
                    expectEquals (during, noteOnDetector ? 1.0f : 0.0f);
                    results.add (label + ", detecting from " + (source == Gate::DetectorSource::di ? "the DI" : "its own input") + ": gain during the note "
                                 + whole (gainDb (during)) + " dB");
                }
            }
            for (const auto& line : results)
                logMessage ("  -> " + line);
        }

        beginTest ("settings: every change moves the gain smoothly, never in a step");
        {
            // Closed on a quiet floor, then every setting changes in turn (none of them opening it);
            // then open on a loud tone and the same again; then the threshold jumps above the tone.
            auto x = noiseFloor (4.0, 120, -75.0, -90.0);
            const auto toneAt = 2.0;
            place (x, sine (330.0, 0.1, (int) at (2.0)), toneAt);
            Gate::Settings s;
            Gate g;
            g.setSettings (s);
            g.prepare (fs, blockSize);
            const auto change = [&] (size_t start, double seconds, const std::function<void (Gate::Settings&)>& f)
            {
                if (start == at (seconds) / blockSize * blockSize)
                {
                    f (s);
                    g.setSettings (s);
                }
            };
            const auto r = runGate (g, x, x, [&] (size_t start)
            {
                change (start, 0.6, [] (auto& v) { v.rangeDb = -20.0f; });
                change (start, 0.7, [] (auto& v) { v.hysteresisDb = 16.0f; v.thresholdDb = -50.0f; });
                change (start, 0.8, [] (auto& v) { v.sidechainHz = 300.0f; });
                change (start, 0.9, [] (auto& v) { v.sidechainHighPass = false; });
                change (start, 1.0, [] (auto& v) { v.detector = Gate::DetectorSource::ownInput; });
                change (start, 1.1, [] (auto& v) { v.releaseMode = Gate::ReleaseMode::classic; v.attackMs = 5.0f; v.releaseMs = 40.0f; v.holdMs = 100.0f; });
                change (start, 1.2, [] (auto& v) { v.rangeDb = -60.0f; v.sidechainHighPass = true; v.detector = Gate::DetectorSource::di; });
                change (start, 1.4, [] (auto& v) { v = Gate::Settings {}; });
                // Open on the tone (from 2.0 s): move everything again.
                change (start, 2.3, [] (auto& v) { v.rangeDb = -30.0f; v.hysteresisDb = 2.0f; v.sidechainHz = 400.0f; });
                change (start, 2.4, [] (auto& v) { v.releaseMode = Gate::ReleaseMode::classic; v.attackMs = 10.0f; v.holdMs = 0.0f; v.detector = Gate::DetectorSource::ownInput; });
                change (start, 2.5, [] (auto& v) { v = Gate::Settings {}; });
                // The threshold jumps 26 dB above the tone: the gate closes through its release.
                change (start, 3.0, [] (auto& v) { v.thresholdDb = 0.0f; });
            });
            const auto closedSteps = maxStep (r.gain, at (0.55), at (1.9));
            const auto openSteps = maxStep (r.gain, at (2.2), at (2.95));
            const auto openLowest = minIn (r.gain, at (2.2), at (2.95));
            const auto closingSteps = maxStep (r.gain, at (2.95), at (3.9));
            const auto closingReached = minIn (r.gain, at (3.5), at (3.9));
            // Range -90 -> -20 dB over 20 ms is the biggest move while closed: 0.1 over 960 samples.
            expectLessThan (closedSteps, 0.1 / 960.0 * 1.01);
            expectEquals (openLowest, 1.0f);
            expectLessThan (closingSteps, 0.0101); // the steepest a 250 ms release or a 20 ms fast one ever falls
            expectEquals (closingReached, 0.0f);
            logMessage ("  -> closed, changing range, thresholds, sidechain, source, mode, and times: largest gain step per sample "
                        + juce::String (closedSteps, 7) + " (the range's own 20 ms ramp: " + juce::String (0.1 / 960.0, 7) + ")");
            logMessage ("  -> open, changing everything again: gain never left " + juce::String (openLowest, 1) + " (largest step "
                        + juce::String (openSteps, 7) + ")");
            logMessage ("  -> threshold raised 26 dB over the tone: the gate closes through its release, largest step " + juce::String (closingSteps, 5)
                        + " per sample, reaching " + juce::String (closingReached, 1));
        }

        beginTest ("meters: detector level, both thresholds, gain reduction, state");
        {
            Gate::Settings s;
            s.rangeDb = -40.0f;
            Gate g;
            g.setSettings (s);
            g.prepare (fs, blockSize);
            runGate (g, sine (1000.0, 0.1, (int) at (0.5)));
            const auto openLevel = g.getDetectorLevelDb(), openReduction = g.getGainReductionDb();
            const auto openState = g.isOpen();
            runGate (g, noiseFloor (1.0, 130, -75.0, -90.0));
            const auto closedLevel = g.getDetectorLevelDb(), closedReduction = g.getGainReductionDb();
            const auto closedState = g.isOpen();

            Gate m;
            m.setSettings ({});
            m.prepare (fs, blockSize);
            runGate (m, noiseFloor (1.0, 131, -75.0, -90.0));

            expectWithinAbsoluteError ((double) openLevel, -20.0, 0.1);
            expectEquals (openReduction, 0.0f);
            expect (openState);
            expectWithinAbsoluteError ((double) closedReduction, 40.0, 1.0e-3);
            expect (! closedState);
            expectEquals (g.getOpenThresholdDb(), -55.0f);
            expectEquals (g.getCloseThresholdDb(), -63.0f);
            expectEquals (m.getGainReductionDb(), 100.0f);
            logMessage ("  -> 1 kHz at -20 dBFS: detector " + juce::String (openLevel, 2) + " dB, open, reduction " + juce::String (openReduction, 1)
                        + " dB; on the floor: detector " + juce::String (closedLevel, 1) + " dB against open/close thresholds " + whole (g.getOpenThresholdDb())
                        + "/" + whole (g.getCloseThresholdDb()) + " dB, closed, reduction " + juce::String (closedReduction, 2)
                        + " dB at range -40 and " + whole (m.getGainReductionDb()) + " dB (mute) at -90");
        }

        beginTest ("real time: every setting, Learn, linking, and the curve-only path allocate and lock nothing; CPU");
        {
            Gate a, b;
            Gate::Settings s;
            a.setSettings (s);
            b.setSettings (s);
            a.prepare (fs, blockSize);
            b.prepare (fs, blockSize);
            auto di = guitarDI ((int) at (10.0));
            const auto hum = noiseFloor (10.0, 140);
            for (size_t n = 0; n < di.size(); ++n)
                di[n] += hum[n];

            juce::AudioBuffer<float> bufferA (1, blockSize), bufferB (1, blockSize);
            rtcheck::Counts total;
            std::vector<double> micros, linkedMicros;
            int blocks = 0;
            for (size_t start = 0; start + blockSize <= di.size(); start += blockSize, ++blocks)
            {
                const bool timed = blocks < 1000; // the first 2.7 s run on default settings, timed
                switch (blocks % 400)
                {
                    case 50:  s.thresholdDb = -45.0f; s.hysteresisDb = 4.0f; break;
                    case 80:  s.rangeDb = -30.0f; s.holdMs = 50.0f; break;
                    case 110: s.attackMs = 3.0f; s.releaseMs = 80.0f; break;
                    case 140: s.releaseMode = Gate::ReleaseMode::classic; break;
                    case 170: s.detector = Gate::DetectorSource::ownInput; break;
                    case 200: s.sidechainHighPass = false; break;
                    case 230: s.sidechainHighPass = true; s.sidechainHz = 250.0f; break;
                    case 260: s = Gate::Settings {}; break;
                    default: break;
                }
                bufferA.copyFrom (0, 0, di.data() + start, blockSize);
                bufferB.copyFrom (0, 0, di.data() + start, blockSize);
                const ampsim::BlockContext context { di.data() + start, blockSize };
                juce::ScopedNoDenormals noDenormals;
                const auto t0 = std::chrono::steady_clock::now();
                rtcheck::begin();
                if (blocks == 1100 || blocks == 2500) // each takes 750 buffers
                    a.startLearn();
                if (! timed)
                {
                    a.setSettings (s);
                    b.setSettings (s);
                }
                a.process (juce::dsp::AudioBlock<float> (bufferA), context);
                const auto t1 = std::chrono::steady_clock::now();
                if (blocks % 500 < 250)
                    b.processWithGain (juce::dsp::AudioBlock<float> (bufferB), a.getGainCurve());
                else if (blocks % 500 < 400)
                    b.process (juce::dsp::AudioBlock<float> (bufferB), context);
                else
                    b.computeGainCurve (di.data() + start, blockSize);
                total += rtcheck::end();
                const auto t2 = std::chrono::steady_clock::now();
                if (timed)
                {
                    micros.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
                    if (blocks % 500 < 250)
                        linkedMicros.push_back (std::chrono::duration<double, std::micro> (t2 - t1).count());
                }
            }
            expectEquals (total.allocations, 0L);
            expectEquals (total.frees, 0L);
            expectEquals (total.blockingLocks, 0L);
            expectGreaterOrEqual (a.getLearnCount(), 1);
            auto sorted = micros;
            std::sort (sorted.begin(), sorted.end());
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            const auto p99 = sorted[(size_t) (0.99 * (double) (sorted.size() - 1))];
            const auto linkedMean = std::accumulate (linkedMicros.begin(), linkedMicros.end(), 0.0) / (double) linkedMicros.size();
            expectLessThan (mean, 0.02 * deadlineMicros);
            logMessage ("  -> " + juce::String (blocks) + " buffers changing every setting, " + juce::String (a.getLearnCount())
                        + " Learn runs, B linked / own / curve-only in turn: " + juce::String (total.allocations) + " allocations, " + juce::String (total.frees)
                        + " frees, " + juce::String (total.blockingLocks) + " locks");
            logMessage ("  -> CPU per 128-sample buffer (guitar DI, defaults): mean " + juce::String (mean, 2) + " us (" + juce::String (100.0 * mean / deadlineMicros, 2)
                        + "% of the deadline), p99 " + juce::String (p99, 2) + " us; a linked Gate B applying A's curve: " + juce::String (linkedMean, 2) + " us");
        }
    }
};

GateTests gateTests;
} // namespace

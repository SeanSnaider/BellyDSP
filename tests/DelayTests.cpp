#include "AllocationTracking.h"
#include "Plot.h"
#include "TestHelpers.h"
#include "dsp/Delay.h"
#include "dsp/Tempo.h"

#include <chrono>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::Delay;

/// Pristine digital settings: no loop filters, no ducking, wet only.
Delay::Settings pristine (float timeMs, float feedback = 0.0f)
{
    Delay::Settings s;
    s.timeMs = timeMs;
    s.feedback = feedback;
    s.lowCutHz = 20.0f;
    s.highCutHz = 20000.0f;
    s.duckDb = 0.0f;
    s.mix = 1.0f;
    return s;
}

Stereo runDelay (Delay& d, const std::vector<float>& left, const std::vector<float>& right,
                 const std::function<void (size_t)>& beforeBlock = {})
{
    Stereo out { left, right };
    for (size_t start = 0; start < left.size(); start += blockSize)
    {
        if (beforeBlock)
            beforeBlock (start);
        const auto len = std::min ((size_t) blockSize, left.size() - start);
        float* channels[2] = { out.left.data() + start, out.right.data() + start };
        d.process (juce::dsp::AudioBlock<float> (channels, 2, len), {});
    }
    return out;
}

Stereo runDelay (Delay& d, const std::vector<float>& mono, const std::function<void (size_t)>& beforeBlock = {})
{
    return runDelay (d, mono, mono, beforeBlock);
}

/// A half-scale impulse: at normal levels, below the digital soft limiter's knee (0.7).
std::vector<float> impulseAt (size_t length, size_t at = 0)
{
    std::vector<float> x (length, 0.0f);
    x[at] = 0.5f;
    return x;
}

size_t peakIndex (const std::vector<float>& x, size_t from, size_t to)
{
    size_t best = from;
    for (size_t n = from; n < std::min (to, x.size()); ++n)
        if (std::abs (x[n]) > std::abs (x[best]))
            best = n;
    return best;
}

/// A 10 ms Hann-windowed 1 kHz burst: band-limited, so a fractional delay is well defined.
std::vector<float> burst (size_t length)
{
    std::vector<float> x (length, 0.0f);
    for (size_t n = 0; n < 480; ++n)
        x[n] = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) n / fs)
                        * (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (double) n / 480.0)));
    return x;
}

double peakIn (const std::vector<float>& x, size_t from, size_t to)
{
    double p = 0.0;
    for (size_t n = from; n < std::min (to, x.size()); ++n)
        p = std::max (p, (double) std::abs (x[n]));
    return p;
}

/// Frequency from zero-crossing spacing over a window.
double frequencyIn (const std::vector<float>& x, size_t from, size_t to)
{
    std::vector<double> crossings;
    for (size_t n = from + 1; n < to; ++n)
        if (x[n - 1] < 0.0f && x[n] >= 0.0f)
            crossings.push_back ((double) (n - 1) + x[n - 1] / (x[n - 1] - x[n]));
    if (crossings.size() < 2)
        return 0.0;
    return fs * (double) (crossings.size() - 1) / (crossings.back() - crossings.front());
}

class DelayTests final : public juce::UnitTest
{
public:
    DelayTests() : juce::UnitTest ("Delay", "ampsim") {}

    void runTest() override
    {
        beginTest ("an echo arrives at exactly the expected sample, and synced times are exact");
        {
            juce::StringArray results;
            for (auto ms : { 1.0f, 10.0f, 375.0f, 1000.0f, 4000.0f })
            {
                Delay d;
                d.setSettings (pristine (ms));
                d.prepare (fs, blockSize);
                const auto expected = (size_t) std::lround (ms * 0.001 * fs);
                const auto out = runDelay (d, impulseAt (expected + 1000));
                expectEquals ((int) peakIndex (out.left, 1, out.left.size()), (int) expected);
                expectWithinAbsoluteError (out.left[expected], 0.5f, 1.0e-6f);
                results.add (juce::String (ms, 0) + " ms at sample " + juce::String ((int) peakIndex (out.left, 1, out.left.size())));
            }

            // A dotted eighth at 120 BPM, as the processor computes it: 375 ms, 18000 samples.
            Delay synced;
            synced.setSettings (pristine ((float) ampsim::tempo::milliseconds (ampsim::tempo::Division::eighth, ampsim::tempo::Feel::dotted, 120.0)));
            synced.prepare (fs, blockSize);
            const auto echo = runDelay (synced, impulseAt (19000));
            expectEquals ((int) peakIndex (echo.left, 1, echo.left.size()), 18000);

            // A fractional time: the burst's echo equals the burst's own formula evaluated 100.5 samples
            // later (the burst is a smooth function of time, so it has a value between samples).
            Delay frac;
            frac.setSettings (pristine ((float) (100.5 * 1000.0 / fs)));
            frac.prepare (fs, blockSize);
            const auto y = runDelay (frac, burst (2000)).left;
            const auto burstAt = [] (double t)
            {
                if (t < 0.0 || t >= 480.0)
                    return 0.0;
                return 0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t / fs) * (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * t / 480.0));
            };
            const auto errorAt = [&] (double delay)
            {
                std::vector<double> expected (y.size());
                for (size_t n = 0; n < y.size(); ++n)
                    expected[n] = burstAt ((double) n - delay);
                return relativeErrorDb (y, expected);
            };
            const auto atHalf = errorAt (100.5), atWhole = std::min (errorAt (100.0), errorAt (101.0));
            expectLessThan (atHalf, -50.0);
            expectLessThan (atHalf, atWhole - 20.0);
            logMessage ("  -> impulse echoes: " + results.joinIntoString (", ") + "; dotted eighth at 120 BPM: sample "
                        + juce::String ((int) peakIndex (echo.left, 1, echo.left.size())) + " (375.000 ms); 100.5-sample delay: the burst's echo is "
                        + juce::String (atHalf, 1) + " dB from the burst 100.5 samples late (" + juce::String (atWhole, 1) + " dB from it 100 or 101 samples late)");
        }

        beginTest ("feedback: each repeat is the set fraction of the last, and 110% stays bounded");
        {
            Delay d;
            d.setSettings (pristine (100.0f, 0.5f));
            d.prepare (fs, blockSize);
            const auto out = runDelay (d, burst (48000)).left;
            juce::StringArray ratios;
            double worst = 0.0;
            for (int k = 1; k < 8; ++k)
            {
                const auto a = peakIn (out, (size_t) (4800 * k), (size_t) (4800 * k + 600));
                const auto b = peakIn (out, (size_t) (4800 * (k + 1)), (size_t) (4800 * (k + 1) + 600));
                worst = std::max (worst, std::abs (b / a - 0.5));
                ratios.add (juce::String (b / a, 4));
            }
            expectLessThan (worst, 1.0e-3);

            // 110%: self-oscillates but never exceeds 1 (digital) or 1/1.5 (analog).
            juce::StringArray bounded;
            for (auto mode : { Delay::Mode::digital, Delay::Mode::analog, Delay::Mode::tape })
            {
                Delay hot;
                auto s = pristine (50.0f, 1.1f);
                s.mode = mode;
                s.lowCutHz = 120.0f;
                s.highCutHz = 6000.0f;
                hot.setSettings (s);
                hot.prepare (fs, blockSize);
                auto x = whiteNoise ((int) (20.0 * fs), 0.0f, 1);
                const auto kick = whiteNoise (4800, 0.5f, 2);
                std::copy (kick.begin(), kick.end(), x.begin());
                const auto y = runDelay (hot, x).left;
                const auto peak = peakIn (y, 0, y.size());
                const auto lastSecond = rms (y.data() + y.size() - 48000, 48000);
                expectLessThan (peak, 1.0001);
                expectGreaterThan (lastSecond, 0.01);
                bounded.add (juce::String (mode == Delay::Mode::digital ? "digital" : mode == Delay::Mode::analog ? "analog" : "tape") + " peak "
                             + juce::String (peak, 3) + ", still " + juce::String (toDb (lastSecond), 1) + " dB RMS after 20 s");
            }
            logMessage ("  -> feedback 50%, repeat-to-repeat ratios: " + ratios.joinIntoString (", ") + " (largest error " + juce::String (worst, 5) + ")");
            logMessage ("  -> feedback 110% for 20 s: " + bounded.joinIntoString ("; "));
        }

        beginTest ("digital time changes crossfade between heads: no click and no pitch change; analog and tape glide the pitch");
        {
            const auto tone = sine (220.0, 0.3, (int) (3.0 * fs));
            const auto changeAt = (size_t) (1.5 * fs) / blockSize * blockSize;
            const auto longTone = sine (1000.0, 0.2, (int) (4.5 * fs));

            Delay digital;
            auto s = pristine (300.0f);
            digital.setSettings (s);
            digital.prepare (fs, blockSize);
            // 300 -> 452.3 ms: not a whole number of the tone's periods, so jumping the read position
            // would put a step in the waveform.
            const auto out = runDelay (digital, tone, [&] (size_t start) { if (start == changeAt) { s.timeMs = 452.3f; digital.setSettings (s); } }).left;

            // A click is broadband: look above 3 kHz, where a 220 Hz tone (and a smooth crossfade of
            // it) has nothing. For comparison, the same change made by jumping the read position.
            const auto highBand = [] (const std::vector<float>& x)
            {
                std::array<ampsim::Svf, 2> hp;
                for (auto& f : hp)
                    f.setCoefficients (ampsim::Svf::design (ampsim::Svf::Type::highpass, 3000.0, 0.7071, 0.0, fs));
                std::vector<float> y (x.size());
                for (size_t n = 0; n < x.size(); ++n)
                    y[n] = (float) hp[1].processSample (hp[0].processSample (x[n]));
                return y;
            };
            const auto hf = highBand (out);
            const auto clickDuring = peakIn (hf, changeAt, changeAt + 4800);
            std::vector<float> jumped (out.size());
            for (size_t n = 0; n < out.size(); ++n)
                jumped[n] = tone[n >= changeAt ? (n >= 21710 ? n - 21710 : 0) : (n >= 14400 ? n - 14400 : 0)];
            const auto clickJump = peakIn (highBand (jumped), changeAt, changeAt + 4800);
            expectLessThan (clickDuring, 0.001);
            expectGreaterThan (clickJump, 10.0 * clickDuring);
            // Right after the 50 ms fade the repeats are back at exactly 220 Hz: the head jumped, it
            // didn't move. (During the fade itself the phase of the sum swings from one head's to the
            // other's, which reads as a brief frequency offset, not a pitch bend.)
            const auto pitchAfter = frequencyIn (out, changeAt + 2400, changeAt + 9600);
            expectWithinAbsoluteError (pitchAfter, 220.0, 0.05);

            // Analog: the read position glides, so the repeats' pitch bends while the time grows.
            Delay analog;
            auto a = pristine (300.0f);
            a.mode = Delay::Mode::analog;
            analog.setSettings (a);
            analog.prepare (fs, blockSize);
            const auto bend = runDelay (analog, longTone, [&] (size_t start) { if (start == changeAt) { a.timeMs = 450.0f; analog.setSettings (a); } }).left;
            const auto lowest = frequencyIn (bend, changeAt + 480, changeAt + 2400);
            const auto settled = frequencyIn (bend, changeAt + 120000, changeAt + 132000); // 2.5 s later
            const auto bendCents = 1200.0 * std::log2 (lowest / 1000.0);
            expectLessThan (bendCents, -50.0);
            expectGreaterThan (bendCents, 1200.0 * std::log2 (1.0 - Delay::maxGlideRate) - 5.0);
            expectWithinAbsoluteError (settled, 1000.0, 1.0);
            logMessage ("  -> digital 300 -> 452.3 ms under a 220 Hz tone: above 3 kHz the output peaks at " + juce::String (clickDuring, 6)
                        + " during the crossfade (jumping the read position instead: " + juce::String (clickJump, 4) + "); pitch right after it "
                        + juce::String (pitchAfter, 3) + " Hz");
            logMessage ("  -> analog 300 -> 450 ms under a 1 kHz tone: the repeats bend " + juce::String (bendCents, 0) + " cents, then settle at "
                        + juce::String (settled, 2) + " Hz");
        }

        beginTest ("stereo layouts: offset, dual times, and ping-pong alternating sides");
        {
            const auto echoTimes = [&] (Delay::Settings s, size_t length)
            {
                Delay d;
                d.setSettings (s);
                d.prepare (fs, blockSize);
                const auto x = impulseAt (length);
                return runDelay (d, x, x);
            };

            auto stereo = pristine (100.0f);
            stereo.offsetMs = 20.0f;
            const auto st = echoTimes (stereo, 10000);
            expectEquals ((int) peakIndex (st.left, 1, st.left.size()), 4800);
            expectEquals ((int) peakIndex (st.right, 1, st.right.size()), 5760);

            auto dual = pristine (100.0f);
            dual.stereoMode = Delay::StereoMode::dual;
            dual.rightTimeMs = 150.0f;
            const auto du = echoTimes (dual, 10000);
            expectEquals ((int) peakIndex (du.right, 1, du.right.size()), 7200);

            auto pong = pristine (100.0f, 0.8f);
            pong.stereoMode = Delay::StereoMode::pingPong;
            const auto pp = echoTimes (pong, 30000);
            juce::StringArray sides;
            bool alternates = true;
            for (int k = 1; k <= 5; ++k)
            {
                const auto at = (size_t) (4800 * k);
                const auto l = std::abs (pp.left[at]), r = std::abs (pp.right[at]);
                const bool leftSide = k % 2 == 1;
                alternates = alternates && (leftSide ? (l > 0.1f && r < 1.0e-6f) : (r > 0.1f && l < 1.0e-6f));
                sides.add (juce::String (k * 100) + " ms " + (l > r ? "L " : "R ") + juce::String (juce::jmax (l, r), 3));
            }
            expect (alternates);

            PlotOptions o;
            o.title = "Ping-pong, 100 ms, feedback 80%: an impulse's repeats";
            o.xLabel = "Time (ms)";
            o.yLabel = "Amplitude";
            o.xMin = 0.0; o.xMax = 600.0; o.yMin = -1.0; o.yMax = 1.0;
            PlotSeries l { "left", {}, {}, plotColour (0), 2.0f }, r { "right (drawn negative)", {}, {}, plotColour (1), 2.0f };
            for (size_t n = 0; n < (size_t) (0.6 * fs); n += 1)
            {
                l.x.push_back (1000.0 * (double) n / fs);
                l.y.push_back (pp.left[n]);
                r.x.push_back (1000.0 * (double) n / fs);
                r.y.push_back (-pp.right[n]);
            }
            const auto png = proofDir().getChildFile ("delay_ping_pong.png");
            expect (savePlot (png, o, { l, r }));
            logMessage ("  -> stereo with a 20 ms offset: left echo at sample " + juce::String ((int) peakIndex (st.left, 1, st.left.size())) + ", right at "
                        + juce::String ((int) peakIndex (st.right, 1, st.right.size())) + "; dual 100/150 ms: right at " + juce::String ((int) peakIndex (du.right, 1, du.right.size())));
            logMessage ("  -> ping-pong repeats: " + sides.joinIntoString (", "));
            logMessage ("  -> " + png.getFullPathName());
        }

        beginTest ("ducking pulls the repeats down by the set amount while you play, and lets them bloom in the gaps");
        {
            // A steady tone throughout, a long delay: the repeats are measured while the tone plays,
            // with and without ducking.
            const auto levelWith = [&] (float duckDb)
            {
                Delay d;
                auto s = pristine (50.0f, 0.0f);
                s.mix = 0.5f;
                s.duckDb = duckDb;
                d.setSettings (s);
                d.prepare (fs, blockSize);
                const auto tone = sine (300.0, 0.3, (int) fs);
                const auto out = runDelay (d, tone).left;
                // out = dry cos + wet sin duck; remove the known dry part to isolate the wet.
                std::vector<float> wet (out.size());
                const auto c = std::cos (0.5f * juce::MathConstants<float>::halfPi);
                for (size_t n = 0; n < out.size(); ++n)
                    wet[n] = out[n] - c * tone[n];
                return toDb (rms (wet.data() + 24000, 20000));
            };
            const auto reduction = levelWith (0.0f) - levelWith (6.0f);
            expectWithinAbsoluteError (reduction, 6.0, 0.05);

            // In a gap the repeats come back up: a 100 ms note, then silence; its echo 400 ms later,
            // with 12 dB of ducking, against the same echo with no ducking.
            const auto echoTail = [&] (float duckDb)
            {
                Delay d;
                auto s = pristine (400.0f, 0.0f);
                s.duckDb = duckDb;
                d.setSettings (s);
                d.prepare (fs, blockSize);
                auto x = sine (300.0, 0.3, (int) fs);
                std::fill (x.begin() + 4800, x.end(), 0.0f);
                const auto out = runDelay (d, x).left;
                return rms (out.data() + 19200 + 2400, 2400); // the echo's second half
            };
            const auto released = toDb (echoTail (12.0f) / echoTail (0.0f));
            expectGreaterThan (released, -1.0);
            logMessage ("  -> 6 dB of ducking under a steady tone: the repeats drop " + juce::String (reduction, 3) + " dB; a 100 ms note's echo 400 ms later, "
                        "after the note has stopped, is within " + juce::String (-released, 2) + " dB of the unducked echo (12 dB ducking, released)");
        }

        beginTest ("analog and tape repeats darken; tape wobbles");
        {
            juce::StringArray results;
            for (auto mode : { Delay::Mode::digital, Delay::Mode::analog, Delay::Mode::tape })
            {
                Delay d;
                auto s = pristine (100.0f, 0.7f);
                s.mode = mode;
                d.setSettings (s);
                d.prepare (fs, blockSize);
                // Two tones together: how much of each survives four repeats.
                std::vector<float> x ((size_t) (0.6 * fs), 0.0f);
                for (size_t n = 0; n < 2400; ++n)
                    x[n] = (float) ((0.15 * std::sin (juce::MathConstants<double>::twoPi * 500.0 * (double) n / fs)
                                     + 0.15 * std::sin (juce::MathConstants<double>::twoPi * 6000.0 * (double) n / fs))
                                    * std::sin (juce::MathConstants<double>::pi * (double) n / 2400.0));
                const auto y = runDelay (d, x).left;
                const auto bandLevel = [&] (double f, size_t from, size_t len)
                {
                    double re = 0.0, im = 0.0;
                    for (size_t n = from; n < from + len; ++n)
                    {
                        re += y[n] * std::cos (juce::MathConstants<double>::twoPi * f * (double) n / fs);
                        im += y[n] * std::sin (juce::MathConstants<double>::twoPi * f * (double) n / fs);
                    }
                    return std::hypot (re, im);
                };
                const auto first = (size_t) (0.1 * fs), fourth = (size_t) (0.4 * fs);
                const auto lowLoss = toDb (bandLevel (500.0, fourth, 2400) / bandLevel (500.0, first, 2400));
                const auto highLoss = toDb (bandLevel (6000.0, fourth, 2400) / bandLevel (6000.0, first, 2400));
                if (mode != Delay::Mode::digital)
                    expectLessThan (highLoss, lowLoss - 6.0);
                results.add (juce::String (mode == Delay::Mode::digital ? "digital" : mode == Delay::Mode::analog ? "analog" : "tape") + " "
                             + juce::String (lowLoss, 1) + " / " + juce::String (highLoss, 1) + " dB");
            }
            logMessage ("  -> from the first repeat to the fourth (feedback 70%), 500 Hz / 6 kHz change by: " + results.joinIntoString ("; "));
        }

        beginTest ("spillover: bypassed, the repeats ring out but new playing doesn't enter");
        {
            Delay d;
            d.setSettings (pristine (100.0f, 0.5f));
            d.prepare (fs, blockSize);
            const auto bypassAt = (size_t) (0.05 * fs) / blockSize * blockSize; // after the first burst, before its echo
            auto x = burst ((size_t) (1.0 * fs));
            const auto second = burst (480);
            const auto noteAt = (size_t) (0.35 * fs); // between the first burst's echoes
            std::copy (second.begin(), second.end(), x.begin() + (long) noteAt);
            const auto out = runDelay (d, x, [&] (size_t start) { if (start == bypassAt) d.setBypassed (true); }).left;

            const auto firstEcho = peakIn (out, 4800, 5400), secondEcho = peakIn (out, 9600, 10200);
            const auto bypassedNoteDry = peakIn (out, noteAt, noteAt + 600);
            const auto echoOfBypassedNote = peakIn (out, noteAt + 4800, noteAt + 5400); // nothing else repeats there
            expectGreaterThan (firstEcho, 0.4);
            expectWithinAbsoluteError (secondEcho / firstEcho, 0.5, 0.01);
            expectWithinAbsoluteError (bypassedNoteDry, 0.5, 0.01); // the dry note at unity
            expectLessThan (echoOfBypassedNote, 0.001);
            logMessage ("  -> bypassed before the first echo: echoes keep coming at " + juce::String (firstEcho, 3) + ", " + juce::String (secondEcho, 3)
                        + " (feedback 50%); a note played while bypassed passes dry at " + juce::String (bypassedNoteDry, 3) + " and adds nothing to the repeats");
        }

        beginTest ("real time: changing every setting, mode, layout, and bypass allocates and locks nothing; CPU");
        {
            Delay d;
            Delay::Settings s;
            d.setSettings (s);
            d.prepare (fs, blockSize);
            const auto x = guitarDI ((int) (6.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            rtcheck::Counts total;
            std::vector<double> micros;
            int blocks = 0;
            for (size_t start = 0; start + blockSize <= x.size(); start += blockSize, ++blocks)
            {
                switch (blocks % 300)
                {
                    case 20:  s.mode = Delay::Mode::analog; break;
                    case 40:  s.timeMs = 600.0f; break;
                    case 60:  s.mode = Delay::Mode::tape; s.modDepthMs = 2.0f; break;
                    case 80:  s.stereoMode = Delay::StereoMode::pingPong; break;
                    case 100: s.stereoMode = Delay::StereoMode::dual; s.rightTimeMs = 250.0f; break;
                    case 120: s.feedback = 1.1f; s.lowCutHz = 300.0f; s.highCutHz = 3000.0f; break;
                    case 140: s.mode = Delay::Mode::digital; s.timeMs = 200.0f; break;
                    case 160: d.setBypassed (true); break;
                    case 200: d.setBypassed (false); s.feedback = 0.4f; s.duckDb = 12.0f; break;
                    case 220: s.stereoMode = Delay::StereoMode::stereo; s.offsetMs = 15.0f; s.mix = 0.6f; break;
                    default: break;
                }
                buffer.copyFrom (0, 0, x.data() + start, blockSize);
                buffer.copyFrom (1, 0, x.data() + start, blockSize);
                juce::ScopedNoDenormals noDenormals;
                const auto t0 = std::chrono::steady_clock::now();
                rtcheck::begin();
                d.setSettings (s);
                d.process (juce::dsp::AudioBlock<float> (buffer), {});
                total += rtcheck::end();
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            expectEquals (total.allocations, 0L);
            expectEquals (total.frees, 0L);
            expectEquals (total.blockingLocks, 0L);
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            expectLessThan (mean, 0.05 * deadlineMicros);
            logMessage ("  -> " + juce::String (blocks) + " blocks through every mode, layout, feedback up to 110%, bypass and back: "
                        + juce::String (total.allocations) + " allocations, " + juce::String (total.frees) + " frees, " + juce::String (total.blockingLocks)
                        + " locks; mean " + juce::String (mean, 1) + " us per stereo block (" + juce::String (100.0 * mean / deadlineMicros, 2) + "% of the deadline)");
        }
    }
};

DelayTests delayTests;
} // namespace

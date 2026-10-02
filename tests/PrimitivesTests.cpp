#include "TestHelpers.h"
#include "dsp/DelayLine.h"
#include "dsp/Lfo.h"
#include "dsp/Tempo.h"

namespace
{
using namespace testing;

class PrimitivesTests final : public juce::UnitTest
{
public:
    PrimitivesTests() : juce::UnitTest ("Shared primitives (interpolation, delay line, LFO, tempo)", "ampsim") {}

    void runTest() override
    {
        beginTest ("Hermite interpolation passes through the samples and reproduces quadratics exactly");
        {
            const auto p = [] (double x) { return 0.3 - 1.7 * x + 0.45 * x * x; };
            double worst = 0.0;
            for (double t = 0.0; t < 1.0; t += 0.05)
                worst = std::max (worst, std::abs ((double) ampsim::hermite ((float) p (-1), (float) p (0), (float) p (1), (float) p (2), (float) t) - p (t)));
            expectLessThan (worst, 1.0e-6);
            expectEquals (ampsim::hermite (1.0f, 2.0f, 3.0f, 5.0f, 0.0f), 2.0f);
            logMessage ("  -> on a quadratic, largest error " + juce::String (worst, 9) + " (float rounding); t = 0 returns x[0] exactly");
        }

        beginTest ("the delay line: whole delays are exact, fractional ones keep the highs better than linear interpolation");
        {
            ampsim::DelayLine line;
            line.prepare (1000);
            const auto x = whiteNoise (4000, 0.5f, 3);
            std::vector<float> integer, fractional;
            for (auto v : x)
            {
                line.write (v);
                integer.push_back (line.readInteger (37));
            }
            double worstInteger = 0.0;
            for (size_t n = 37; n < x.size(); ++n)
                worstInteger = std::max (worstInteger, (double) std::abs (integer[n] - x[n - 37]));
            expectEquals (worstInteger, 0.0);

            // Half a sample is the worst case for interpolation. A sine delayed 10.5 samples, against
            // the exact delayed sine, and the same with linear interpolation.
            juce::StringArray results;
            for (auto f : { 1000.0, 5000.0, 10000.0 })
            {
                ampsim::DelayLine d;
                d.prepare (100);
                double gainHermite = 0.0, gainLinear = 0.0, reference = 0.0;
                for (int n = 0; n < 4800; ++n)
                {
                    d.write ((float) std::sin (juce::MathConstants<double>::twoPi * f * n / fs));
                    if (n < 200)
                        continue;
                    const auto h = d.read (10.5);
                    const auto l = 0.5f * (d.readInteger (10) + d.readInteger (11));
                    const auto exact = std::sin (juce::MathConstants<double>::twoPi * f * (n - 10.5) / fs);
                    gainHermite += h * h;
                    gainLinear += l * l;
                    reference += exact * exact;
                }
                const auto hermiteDb = 10.0 * std::log10 (gainHermite / reference), linearDb = 10.0 * std::log10 (gainLinear / reference);
                expectGreaterThan (hermiteDb, linearDb);
                results.add (juce::String (f / 1000.0, 0) + " kHz: Hermite " + juce::String (hermiteDb, 2) + " dB, linear " + juce::String (linearDb, 2) + " dB");
            }
            logMessage ("  -> delay 37 samples: exact, bit for bit; half-sample delay, level of a sine: " + results.joinIntoString ("; "));
        }

        beginTest ("the LFO: shapes, rate, and phase offsets");
        {
            using Shape = ampsim::Lfo::Shape;
            expectWithinAbsoluteError (ampsim::Lfo::shapeAt (Shape::triangle, 0.25), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (ampsim::Lfo::shapeAt (Shape::triangle, 0.75), -1.0f, 1.0e-6f);
            expectWithinAbsoluteError (ampsim::Lfo::shapeAt (Shape::triangle, 0.125), 0.5f, 1.0e-6f);
            expectWithinAbsoluteError (ampsim::Lfo::shapeAt (Shape::sine, 0.25), 1.0f, 1.0e-6f);

            // 2 Hz: a full cycle every 24000 samples; two LFOs a third of a cycle apart stay that way.
            ampsim::Lfo a, b;
            a.prepare (fs);
            b.prepare (fs);
            a.setRate (2.0);
            b.setRate (2.0);
            b.setPhase (1.0 / 3.0);
            std::vector<int> wraps;
            float worstOffset = 0.0f;
            for (int n = 0; n < 3 * 24000 + 10; ++n)
            {
                const auto before = a.getPhase();
                a.next();
                const auto vb = b.next();
                if (a.getPhase() < before)
                    wraps.push_back (n);
                const auto expected = (float) std::sin (juce::MathConstants<double>::twoPi * (2.0 * n / fs + 1.0 / 3.0));
                worstOffset = std::max (worstOffset, std::abs (vb - expected));
            }
            expectEquals ((int) wraps.size(), 3);
            for (size_t i = 1; i < wraps.size(); ++i)
                expectWithinAbsoluteError (wraps[i] - wraps[i - 1], 24000, 1);
            expectLessThan (worstOffset, 1.0e-4f);

            // Random: stays in [-1, 1] and moves smoothly (no step bigger than a cosine glide allows).
            ampsim::Lfo r (7);
            r.prepare (fs);
            r.setShape (Shape::random);
            r.setRate (5.0);
            float lo = 0.0f, hi = 0.0f, biggestStep = 0.0f, last = r.next();
            for (int n = 0; n < 5 * 48000; ++n)
            {
                const auto v = r.next();
                lo = std::min (lo, v);
                hi = std::max (hi, v);
                biggestStep = std::max (biggestStep, std::abs (v - last));
                last = v;
            }
            const auto stepBound = (float) (2.0 * juce::MathConstants<double>::pi / 2.0 * 5.0 / fs); // max slope of a 2-wide cosine glide
            expect (lo >= -1.0f && hi <= 1.0f);
            expectLessThan (biggestStep, stepBound * 1.01f);
            logMessage ("  -> triangle hits +1 at a quarter cycle and -1 at three quarters; 2 Hz: a cycle every " + juce::String (wraps[1] - wraps[0])
                        + " samples (24000 expected), a 120 degree offset held to " + juce::String (worstOffset, 6) + "; random: range " + juce::String (lo, 2) + " to " + juce::String (hi, 2)
                        + ", largest step " + juce::String (biggestStep, 6) + " (bound " + juce::String (stepBound, 6) + ")");
        }

        beginTest ("note lengths: a dotted eighth at 120 BPM is exactly 375 ms");
        {
            using ampsim::tempo::Division;
            using ampsim::tempo::Feel;
            expectWithinAbsoluteError (ampsim::tempo::milliseconds (Division::eighth, Feel::dotted, 120.0), 375.0, 1.0e-9);
            expectWithinAbsoluteError (ampsim::tempo::milliseconds (Division::quarter, Feel::straight, 120.0), 500.0, 1.0e-9);
            expectWithinAbsoluteError (ampsim::tempo::milliseconds (Division::quarter, Feel::triplet, 120.0), 1000.0 / 3.0, 1.0e-9);
            expectWithinAbsoluteError (ampsim::tempo::milliseconds (Division::whole, Feel::straight, 60.0), 4000.0, 1.0e-9);
            expectWithinAbsoluteError (ampsim::tempo::milliseconds (Division::sixteenth, Feel::straight, 150.0), 100.0, 1.0e-9);
            juce::StringArray all;
            for (const auto& note : ampsim::tempo::notes)
                all.add (juce::String (note.name) + " " + juce::String (ampsim::tempo::milliseconds (note.division, note.feel, 120.0), 1));
            logMessage ("  -> at 120 BPM (ms): " + all.joinIntoString (", "));
        }

        beginTest ("tap tempo: averages, ignores a stray tap, follows a deliberate change, and restarts after 2 s");
        {
            ampsim::TapTempo tap;
            for (int i = 0; i < 5; ++i)
                tap.tap (10.0 + 0.5 * i); // 120 BPM
            expectWithinAbsoluteError (tap.getBpm(), 120.0, 1.0e-9);

            // Slightly uneven taps average out.
            ampsim::TapTempo uneven;
            for (double t : { 0.0, 0.49, 1.01, 1.50, 2.00 })
                uneven.tap (t);
            expectWithinAbsoluteError (uneven.getBpm(), 120.0, 0.01);

            // One stray tap, 40% early, doesn't move the tempo.
            tap.tap (12.3);
            expectWithinAbsoluteError (tap.getBpm(), 120.0, 1.0e-9);

            // A double trigger (two taps 50 ms apart) is ignored too.
            ampsim::TapTempo bounce;
            bounce.tap (0.0);
            bounce.tap (0.5);
            bounce.tap (0.55);
            expectWithinAbsoluteError (bounce.getBpm(), 120.0, 1.0e-9);

            // Switching to 90 BPM: the first new interval is a stray, the second agrees with it.
            ampsim::TapTempo change;
            for (int i = 0; i < 4; ++i)
                change.tap (0.5 * i);
            const auto start = 1.5;
            change.tap (start + 2.0 / 3.0);
            const auto afterOne = change.getBpm();
            change.tap (start + 4.0 / 3.0);
            expectWithinAbsoluteError (afterOne, 120.0, 1.0e-9);
            expectWithinAbsoluteError (change.getBpm(), 90.0, 1.0e-6);

            // After a 3 s pause, two taps 0.6 s apart set 100 BPM, with no trace of the old tempo.
            change.tap (start + 4.0 / 3.0 + 3.0);
            change.tap (start + 4.0 / 3.0 + 3.6);
            expectWithinAbsoluteError (change.getBpm(), 100.0, 1.0e-6);
            logMessage ("  -> steady taps 0.5 s apart: " + juce::String (tap.getBpm(), 3) + " BPM; uneven 0.49/0.52/0.49/0.50 s: " + juce::String (uneven.getBpm(), 3)
                        + "; a stray tap and a 50 ms double trigger: ignored; 120 -> 90 BPM after two agreeing taps; after a 3 s gap: " + juce::String (change.getBpm(), 3));
        }
    }
};

PrimitivesTests primitivesTests;
} // namespace

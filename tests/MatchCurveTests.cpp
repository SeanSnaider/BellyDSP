// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The match curve (dsp/MatchCurve.h; BUILD_PLAN "Match curve"): the FIR against its target, minimum phase,
// zero latency, the block against brute-force convolution, curve changes and amount rebuilds without a
// click, off and amount 0 bit for bit, the state and presets, and its CPU.

#include "Plot.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"
#include "dsp/FftDouble.h"
#include "dsp/MatchCurve.h"
#include "dsp/MinimumPhase.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>

namespace
{
using namespace testing;
using ampsim::MatchCurve;

constexpr int analysisOrder = 17; // 131072 points: 0.37 Hz bins, far finer than anything the FIR can do

/// Points every 1/12 octave from 20 Hz to 20 kHz.
MatchCurve::Curve curveFrom (const std::function<double (double)>& db)
{
    std::vector<MatchCurve::Point> points;
    for (double f = 20.0; f <= 20000.0; f *= std::pow (2.0, 1.0 / 12.0))
        points.push_back ({ f, db (f) });
    return MatchCurve::Curve::fromPoints (points);
}

/// What a match leaves: broad bumps and dips, up to about +-9 dB.
MatchCurve::Curve matchLikeCurve()
{
    return curveFrom ([] (double f)
    {
        const auto o = std::log2 (f / 1000.0);
        return 5.0 * std::sin (2.0 * std::numbers::pi * o / 3.5) + 6.0 * std::exp (-0.5 * std::pow ((o - 1.6) / 0.25, 2.0))
               - 7.0 * std::exp (-0.5 * std::pow ((o + 1.3) / 0.2, 2.0));
    });
}

/// The worst kind: +-15 dB alternating every 1/12 octave (step 3 smooths it to what the FIR can realize).
MatchCurve::Curve alternatingCurve (double db = 15.0)
{
    std::vector<MatchCurve::Point> points;
    int i = 0;
    for (double f = 20.0; f <= 20000.0; f *= std::pow (2.0, 1.0 / 12.0), ++i)
        points.push_back ({ f, (i % 2 == 0 ? db : -db) });
    return MatchCurve::Curve::fromPoints (points);
}

/// Large, resolvable features: a -12 dB dip at 300 Hz (1/3 octave wide), +12 dB at 3 kHz, a +10 dB
/// shelf above 8 kHz, and +-15 dB alternating every half octave below 200 Hz.
MatchCurve::Curve bigCurve()
{
    return curveFrom ([] (double f)
    {
        const auto o = std::log2 (f / 1000.0);
        auto db = -12.0 * std::exp (-0.5 * std::pow ((o - std::log2 (0.3)) / 0.15, 2.0)) + 12.0 * std::exp (-0.5 * std::pow ((o - std::log2 (3.0)) / 0.2, 2.0))
                  + (f > 8000.0 ? 10.0 : 0.0);
        if (f < 200.0)
            db += ((int) std::floor (2.0 * std::log2 (f / 20.0)) % 2 == 0) ? 15.0 : -15.0;
        return db;
    });
}

/// A shelf beyond the clamp: +24 dB above 3 kHz, so steps 2 and 4 both act.
MatchCurve::Curve loudShelf()
{
    return curveFrom ([] (double f) { return f > 3000.0 ? 24.0 : 0.0; });
}

std::vector<std::complex<double>> spectrum (const std::vector<float>& h, int order)
{
    const ampsim::FftDouble fft (order);
    std::vector<std::complex<double>> x ((size_t) fft.size());
    for (size_t i = 0; i < h.size() && i < x.size(); ++i)
        x[i] = h[i];
    fft.forward (x);
    return x;
}

double binHz (size_t k, int order) { return (double) k * fs / (double) (1 << order); }

struct MagnitudeCheck
{
    double maxErrorDb = 0.0, atHz = 0.0;
};

/// |H| of the FIR against targetDb on every bin from lo to hi.
MagnitudeCheck checkMagnitude (const std::vector<float>& fir, const MatchCurve::Curve& curve, double amount, double lo = 50.0, double hi = 16000.0)
{
    const auto H = spectrum (fir, analysisOrder);
    std::vector<double> freqs;
    std::vector<size_t> bins;
    for (size_t k = 1; k < H.size() / 2; ++k)
        if (const auto f = binHz (k, analysisOrder); f >= lo && f <= hi)
        {
            freqs.push_back (f);
            bins.push_back (k);
        }
    const auto target = MatchCurve::targetDb (curve, amount, freqs);
    MagnitudeCheck c;
    for (size_t i = 0; i < bins.size(); ++i)
        if (const auto e = std::abs (20.0 * std::log10 (std::abs (H[bins[i]])) - target[i]); e > c.maxErrorDb)
        {
            c.maxErrorDb = e;
            c.atHz = freqs[i];
        }
    return c;
}

std::vector<float> runBlock (MatchCurve& block, const std::vector<float>& input, int bufferSize, std::vector<float>* right = nullptr,
                             const std::function<void (size_t)>& before = {})
{
    auto out = runInBlocks (input, bufferSize, [&] (juce::dsp::AudioBlock<float>& b, size_t start)
    {
        if (before)
            before (start);
        block.process (b, {});
    });
    if (right != nullptr)
        *right = out.right;
    return out.left;
}

std::vector<double> toDouble (const std::vector<float>& h) { return { h.begin(), h.end() }; }

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (2);
}

/// The processor on the input, freshly prepared, in 128-sample buffers: both output channels.
Stereo render (AmpSimProcessor& p, const std::vector<float>& input, const std::function<void (size_t block)>& between = {})
{
    p.prepareToPlay (fs, blockSize);
    Stereo out { std::vector<float> (input.size()), std::vector<float> (input.size()) };
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    size_t block = 0;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize, ++block)
    {
        if (between)
            between (block);
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, out.left.begin() + (std::ptrdiff_t) start);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize, out.right.begin() + (std::ptrdiff_t) start);
    }
    return out;
}

/// A processor whose output is the chain's signal at the match curve (no amp model, no IR, the post effects
/// off, the output limiter off), so what the match curve does is all that differs.
void plainRig (AmpSimProcessor& p)
{
    setParam (p, "output_limit_on", 0.0f);
    setParam (p, "post_fx_on", 0.0f);
}

std::vector<float> tail (const std::vector<float>& x, size_t from) { return { x.begin() + (std::ptrdiff_t) from, x.end() }; }
std::vector<double> tail (const std::vector<double>& x, size_t from, size_t size) { return { x.begin() + (std::ptrdiff_t) from, x.begin() + (std::ptrdiff_t) size }; }

class MatchCurveTests final : public juce::UnitTest
{
public:
    MatchCurveTests() : juce::UnitTest ("Match curve", "ampsim") {}

    void runTest() override
    {
        beginTest ("the FIR's magnitude lands on the target within 0.5 dB from 50 Hz to 16 kHz");
        {
            const std::vector<std::pair<juce::String, MatchCurve::Curve>> curves {
                { "a match-like curve (+-9 dB bumps)", matchLikeCurve() },
                { "+-15 dB alternating every 1/12 octave", alternatingCurve() },
                { "big features: -12 dB at 300 Hz, +12 at 3 kHz, a +10 shelf, +-15 every half octave below 200 Hz", bigCurve() },
                { "a +24 dB shelf above 3 kHz (clamped to +15)", loudShelf() },
                { "one point, -6 dB (a broadband cut)", MatchCurve::Curve::fromPoints ({ { 1000.0, -6.0 } }) } };

            for (const auto& [name, curve] : curves)
            {
                const auto fir = MatchCurve::designFir (curve, 1.0);
                expectEquals ((int) fir.size(), MatchCurve::firLength);
                const auto c = checkMagnitude (fir, curve, 1.0);
                expectLessThan (c.maxErrorDb, 0.5, name);
                logMessage ("  -> " + name + ": worst " + juce::String (c.maxErrorDb, 3) + " dB at " + juce::String (c.atHz, 0) + " Hz (limit 0.5)");
            }

            // The target itself: clamped to +-15 dB, flat at the band edges, and the smoothing's resolution.
            const auto shelf = loudShelf();
            expectWithinAbsoluteError (MatchCurve::targetDb (shelf, 1.0, 10000.0), 15.0, 1.0e-9);
            expectWithinAbsoluteError (MatchCurve::targetDb (shelf, 1.0, 23000.0), 0.0, 1.0e-12);
            expectWithinAbsoluteError (MatchCurve::targetDb (MatchCurve::Curve::fromPoints ({ { 1000.0, -6.0 } }), 1.0, 10.0), 0.0, 1.0e-12);

            // Proof plot: the match-like curve as given, the target the FIR aims at, and what it realizes.
            {
                const auto curve = matchLikeCurve();
                const auto fir = MatchCurve::designFir (curve, 1.0);
                const auto H = spectrum (fir, analysisOrder);
                PlotSeries given { "the curve as given", {}, {}, plotColour (2), 1.5f, true }, target { "target (smoothed, tapered)", {}, {}, plotColour (1), 2.5f },
                    realized { "the 4096-tap FIR", {}, {}, plotColour (0), 1.5f };
                for (const auto& p : curve.points)
                {
                    given.x.push_back (p.hz);
                    given.y.push_back (p.db);
                }
                for (double f = 10.0; f < 24000.0; f *= 1.01)
                {
                    target.x.push_back (f);
                    target.y.push_back (MatchCurve::targetDb (curve, 1.0, f));
                    const auto k = (size_t) std::lround (f / binHz (1, analysisOrder));
                    realized.x.push_back (f);
                    realized.y.push_back (20.0 * std::log10 (std::abs (H[k])));
                }
                savePlot (proofDir().getChildFile ("match_curve_response.png"),
                          { "Match curve: target and the minimum-phase FIR", "Hz", "dB", true, 10.0, 24000.0, -12.0, 12.0 }, { given, target, realized });
            }
        }

        beginTest ("the amount scales the curve's dB exactly (50% is half the correction at every frequency)");
        {
            const auto curve = bigCurve();
            double worst = 0.0, worstBlend = 0.0;
            for (const auto amount : { 0.25, 0.5, 0.75 })
            {
                const auto fir = MatchCurve::designFir (curve, amount);
                const auto c = checkMagnitude (fir, curve, amount);
                worst = std::max (worst, c.maxErrorDb);

                // Against amount x full curve directly (targetDb is linear in the amount).
                expectWithinAbsoluteError (MatchCurve::targetDb (curve, amount, 3000.0), amount * MatchCurve::targetDb (curve, 1.0, 3000.0), 1.0e-9);

                // The alternative: crossfading the identity and the full FIR, 1 + a (H - 1), for the record.
                const auto H = spectrum (MatchCurve::designFir (curve, 1.0), analysisOrder);
                for (size_t k = 1; k < H.size() / 2; ++k)
                    if (const auto f = binHz (k, analysisOrder); f >= 50.0 && f <= 16000.0)
                    {
                        const auto blend = 20.0 * std::log10 (std::abs (1.0 + amount * (H[k] - 1.0)));
                        worstBlend = std::max (worstBlend, std::abs (blend - amount * 20.0 * std::log10 (std::abs (H[k]))));
                    }
            }
            expectLessThan (worst, 0.5);
            logMessage ("  -> the big curve at 25, 50, 75%: rebuilt FIRs within " + juce::String (worst, 3)
                        + " dB of amount x curve; a dry/FIR crossfade would miss by up to " + juce::String (worstBlend, 2) + " dB");
            expect (MatchCurve::designFir (curve, 0.0).empty(), "amount 0 designs nothing");
            expect (MatchCurve::designFir ({}, 1.0).empty() && MatchCurve::designFir (curveFrom ([] (double) { return 0.0; }), 1.0).empty(),
                    "no curve, or a flat one, designs nothing");
        }

        beginTest ("the FIR is minimum phase: its phase is the Hilbert transform of its log magnitude");
        {
            for (const auto& curve : { matchLikeCurve(), bigCurve(), loudShelf() })
            {
                // Rebuild the minimum-phase filter from the FIR's own magnitude (the folded cepstrum: the
                // discrete Hilbert relation between ln|H| and arg H); a minimum-phase FIR comes back as itself.
                const auto fir = MatchCurve::designFir (curve, 1.0);
                const auto rebuilt = ampsim::minphase::minimumPhase (fir, analysisOrder);
                const auto error = relativeErrorDb (rebuilt, toDouble (fir));

                // And the phase itself, against the minimum phase of the same magnitude, 50 Hz to 16 kHz.
                const auto H = spectrum (fir, analysisOrder), R = spectrum (rebuilt, analysisOrder);
                double worstPhase = 0.0;
                for (size_t k = 1; k < H.size() / 2; ++k)
                    if (const auto f = binHz (k, analysisOrder); f >= 50.0 && f <= 16000.0)
                        worstPhase = std::max (worstPhase, std::abs (std::arg (H[k] / R[k])) * 180.0 / std::numbers::pi);

                // Energy as early as possible: 99% of it within the first few ms (a linear-phase FIR of the
                // same magnitude would centre it on 2048 samples, 43 ms).
                double total = 0.0, partial = 0.0;
                for (auto v : fir)
                    total += (double) v * v;
                int n99 = 0;
                for (; partial < 0.99 * total; ++n99)
                    partial += (double) fir[(size_t) n99] * fir[(size_t) n99];

                expectLessThan (error, -60.0);
                expectLessThan (worstPhase, 1.0);
                logMessage ("  -> min-phase rebuild of the FIR from its own magnitude: difference " + dB (error) + ", phase within "
                            + juce::String (worstPhase, 3) + " degrees; 99% of the energy in the first " + juce::String (n99) + " samples ("
                            + juce::String (1000.0 * n99 / fs, 2) + " ms)");
            }
        }

        beginTest ("zero latency: an impulse comes out as the FIR from sample 0");
        {
            MatchCurve block;
            block.setCurve (matchLikeCurve(), 1.0);
            block.prepare (fs, blockSize);
            std::vector<float> impulse ((size_t) MatchCurve::firLength + 1024, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runBlock (block, impulse, blockSize);
            const auto fir = block.getFir();
            expectEquals (block.latencySamples(), 0);
            expectWithinAbsoluteError (out[0], fir[0], 1.0e-6f);
            expectGreaterThan (std::abs (out[0]), 0.3f, "the FIR's first tap carries the direct sound");
            const auto error = relativeErrorDb (std::vector<float> (out.begin(), out.begin() + MatchCurve::firLength), toDouble (fir));
            expectLessThan (error, -100.0);
            logMessage ("  -> out[0] = " + juce::String (out[0], 6) + " (the FIR's first tap " + juce::String (fir[0], 6) + "); the impulse response is the FIR to "
                        + dB (error) + "; latencySamples() = 0");
        }

        beginTest ("the block's output is brute-force convolution with the FIR, both channels, at every buffer size");
        {
            const auto curve = matchLikeCurve();
            const auto input = whiteNoise ((int) fs, 0.5f, 7);
            const auto expected = directConvolution (input, toDouble (MatchCurve::designFir (curve, 1.0)));
            double worst = -1000.0;
            for (const auto bufferSize : { 1, 7, 64, 128, 512 })
            {
                MatchCurve block;
                block.setCurve (curve, 1.0);
                block.prepare (fs, bufferSize);
                std::vector<float> right;
                const auto out = runBlock (block, input, bufferSize, &right);
                const auto error = relativeErrorDb (out, expected);
                worst = std::max (worst, error);
                expectLessThan (error, -100.0, juce::String (bufferSize));
                expectEquals (maxAbsDifference (out, right), 0.0, "left and right go through the same FIR");
            }
            logMessage ("  -> 1 s of noise at buffers of 1, 7, 64, 128, 512: worst error against direct convolution " + dB (worst) + " (limit -100 dB); left == right");
        }

        beginTest ("no FIR yet: the block is a plain wire, bit for bit");
        {
            MatchCurve block;
            block.prepare (fs, blockSize);
            const auto input = guitarDI ((int) fs);
            const auto out = runBlock (block, input, blockSize);
            expectEquals (maxAbsDifference (out, input), 0.0);
            expect (! block.hasFir());
        }

        beginTest ("a new curve while audio runs crossfades to it, with no click");
        {
            const auto curveA = curveFrom ([] (double f) { return f < 400.0 ? 8.0 : -4.0; });
            const auto curveB = curveFrom ([] (double f) { return f < 400.0 ? -8.0 : 5.0; });
            MatchCurve block;
            block.setCurve (curveA, 1.0);
            block.prepare (fs, blockSize);

            auto input = sine (220.0, 0.25, (int) (2.0 * fs));
            const auto high = sine (2500.0, 0.1, (int) input.size());
            for (size_t i = 0; i < input.size(); ++i)
                input[i] += high[i];
            const size_t switchAt = (size_t) (0.5 * fs) / blockSize * blockSize;
            size_t readyAt = 0;
            const auto out = runBlock (block, input, blockSize, nullptr, [&] (size_t start)
            {
                if (start == switchAt)
                    block.setCurve (curveB, 1.0);
                // Real time gives JUCE's background thread 2.7 ms per buffer to build the engine.
                if (start >= switchAt && start < switchAt + 20 * blockSize)
                    juce::Thread::sleep (2);
                if (readyAt == 0 && start > switchAt && block.isEngineReady())
                    readyAt = start;
            });

            const auto expectedAfter = directConvolution (input, toDouble (MatchCurve::designFir (curveB, 1.0)));
            const auto settleAt = (size_t) (1.2 * fs);
            const auto settled = relativeErrorDb (tail (out, settleAt), tail (expectedAfter, settleAt, out.size()));
            const auto steadyStep = std::max (maxStep (out, blockSize * 40, switchAt), maxStep (out, settleAt));
            const auto switchStep = maxStep (out, switchAt, switchAt + (size_t) (0.3 * fs));
            expectLessThan (settled, -100.0);
            expectLessThan (switchStep, steadyStep * 1.25);
            logMessage ("  -> swapped at " + juce::String (switchAt / fs, 3) + " s; after the 50 ms crossfade the output is the new FIR's to " + dB (settled)
                        + "; largest sample-to-sample step during the swap " + juce::String (switchStep, 5) + " vs. " + juce::String (steadyStep, 5)
                        + " in steady state (limit 1.25 x)");
            writeWav (proofDir().getChildFile ("match_curve_swap.wav"), out);
        }

        beginTest ("off, amount 0, and no curve are bit-identical to the rig without a match curve");
        {
            const auto input = guitarDI (562 * blockSize) /* 1.5 s in whole buffers */;
            AmpSimProcessor reference;
            plainRig (reference);
            const auto ref = render (reference, input);

            const auto variant = [&] (bool on, float amount, bool withCurve, const std::function<void (AmpSimProcessor&, size_t)>& between = {})
            {
                AmpSimProcessor p;
                plainRig (p);
                if (withCurve)
                    p.setMatchCurve (matchLikeCurve());
                setParam (p, "match_curve_on", on ? 1.0f : 0.0f);
                setParam (p, "match_curve_amount", amount);
                waitForLoads (p);
                return render (p, input, between ? [&] (size_t b) { between (p, b); } : std::function<void (size_t)>());
            };

            const auto off = variant (false, 100.0f, true);
            const auto zero = variant (true, 0.0f, true);
            const auto none = variant (true, 100.0f, false);
            for (const auto* out : { &off, &zero, &none })
            {
                expectEquals (maxAbsDifference (out->left, ref.left), 0.0);
                expectEquals (maxAbsDifference (out->right, ref.right), 0.0);
            }

            // On at 100%, then the amount to 0 mid-stream: once the 10 ms bypass fade is over, bit for bit again.
            const size_t dropBlock = 150;
            const auto dropped = variant (true, 100.0f, true, [&] (AmpSimProcessor& p, size_t block)
            {
                if (block == dropBlock)
                    setParam (p, "match_curve_amount", 0.0f);
            });
            const auto from = (dropBlock + 8) * (size_t) blockSize; // 10 ms is 3.75 buffers
            expectGreaterThan (maxAbsDifference (std::vector<float> (dropped.left.begin(), dropped.left.begin() + (std::ptrdiff_t) (dropBlock * blockSize)),
                                                 std::vector<float> (ref.left.begin(), ref.left.begin() + (std::ptrdiff_t) (dropBlock * blockSize))), 1.0e-3,
                               "the curve must have been playing before the drop");
            expectEquals (maxAbsDifference (tail (dropped.left, from), tail (ref.left, from)), 0.0);
            expectEquals (maxAbsDifference (tail (dropped.right, from), tail (ref.right, from)), 0.0);
            logMessage ("  -> match_curve_on off, amount 0, and no curve: max difference 0 on both channels over 1.5 s; amount dropped to 0 mid-stream: "
                        "0 from 10 ms after the drop");
        }

        beginTest ("in the app, on at 100%: the output is the rig without it, convolved with the FIR (it sits after the cab)");
        {
            const auto input = guitarDI (562 * blockSize) /* 1.5 s in whole buffers */;
            AmpSimProcessor reference;
            plainRig (reference);
            const auto ref = render (reference, input);

            AmpSimProcessor p;
            plainRig (p);
            p.setMatchCurve (matchLikeCurve());
            setParam (p, "match_curve_on", 1.0f);
            waitForLoads (p);
            const auto out = render (p, input);
            const auto fir = toDouble (MatchCurve::designFir (matchLikeCurve(), 1.0));
            const auto expectedLeft = directConvolution (ref.left, fir), expectedRight = directConvolution (ref.right, fir);
            const auto from = (size_t) (0.2 * fs); // after the chain's 10 ms fade-in and the FIR's 85 ms
            const auto errorLeft = relativeErrorDb (tail (out.left, from), tail (expectedLeft, from, out.left.size()));
            const auto errorRight = relativeErrorDb (tail (out.right, from), tail (expectedRight, from, out.right.size()));
            expectLessThan (std::max (errorLeft, errorRight), -100.0);
            expect (p.getChain().matchCurve.hasFir());
            logMessage ("  -> against direct convolution of the rig's own output with the FIR: " + dB (errorLeft) + " (left), " + dB (errorRight) + " (right)");
        }

        beginTest ("moving the amount rebuilds the FIR at the new amount (rate-limited, the newest amount wins)");
        {
            AmpSimProcessor p;
            const auto curve = matchLikeCurve();
            p.setMatchCurve (curve);
            setParam (p, "match_curve_on", 1.0f);
            waitForLoads (p);
            const auto before = p.getMatchCurveBuildCount();
            expectWithinAbsoluteError (p.getChain().matchCurve.getAmount(), 1.0, 1.0e-12);

            // A drag: many values within one 40 ms window, then the timer ticks.
            for (const auto amount : { 90.0f, 75.0f, 60.0f, 50.0f })
            {
                setParam (p, "match_curve_amount", amount);
                p.runHousekeeping();
            }
            juce::Thread::sleep (60);
            p.runHousekeeping();
            waitForLoads (p);
            p.runHousekeeping();
            waitForLoads (p);
            const auto builds = p.getMatchCurveBuildCount() - before;
            expectWithinAbsoluteError (p.getChain().matchCurve.getAmount(), 0.5, 1.0e-6);
            expect (p.getChain().matchCurve.getFir() == MatchCurve::designFir (curve, 0.5), "the FIR must be the curve's at 50%");
            expectLessOrEqual (builds, 3, "a drag mustn't queue a design per step");
            expectGreaterThan (builds, 0);

            // At 0 the FIR is kept at 1% (bypassed anyway), so turning it up starts from almost nothing.
            setParam (p, "match_curve_amount", 0.0f);
            juce::Thread::sleep (60);
            p.runHousekeeping();
            waitForLoads (p);
            expectWithinAbsoluteError (p.getChain().matchCurve.getAmount(), 0.01, 1.0e-9);
            logMessage ("  -> 90, 75, 60, 50% within one window: " + juce::String (builds) + " designs, the last at 50%; amount 0 keeps a 1% FIR");
        }

        beginTest ("the curve and its parameters survive the state and presets; a preset without one has none");
        {
            const auto curve = matchLikeCurve();
            AmpSimProcessor a;
            a.setMatchCurve (curve);
            setParam (a, "match_curve_on", 1.0f);
            setParam (a, "match_curve_amount", 62.5f);
            waitForLoads (a);

            juce::MemoryBlock state;
            a.getStateInformation (state);
            AmpSimProcessor b;
            b.setStateInformation (state.getData(), (int) state.getSize());
            juce::Thread::sleep (60);
            b.runHousekeeping();
            waitForLoads (b);
            const auto& got = b.getMatchCurve().points;
            double worstPoint = 0.0;
            for (size_t i = 0; i < std::min (got.size(), curve.points.size()); ++i)
                worstPoint = std::max ({ worstPoint, std::abs (got[i].hz - curve.points[i].hz), std::abs (got[i].db - curve.points[i].db) });
            expectEquals ((int) got.size(), (int) curve.points.size());
            expectLessThan (worstPoint, 1.0e-9);
            expectEquals (b.parameters.getRawParameterValue ("match_curve_on")->load(), 1.0f);
            expectWithinAbsoluteError (b.parameters.getRawParameterValue ("match_curve_amount")->load(), 62.5f, 1.0e-4f);
            expect (b.getChain().matchCurve.getFir() == MatchCurve::designFir (b.getMatchCurve(), 0.625), "the restored FIR is the curve's at 62.5%");

            // Presets: through JSON text, as a file would be.
            const auto saved = juce::JSON::toString (presets::capture (a, "with curve"));
            AmpSimProcessor c;
            const auto applied = presets::apply (c, juce::JSON::parse (saved));
            expect (applied.ok, applied.error);
            waitForLoads (c);
            expectEquals ((int) c.getMatchCurve().points.size(), (int) curve.points.size());
            expectWithinAbsoluteError (c.getMatchCurve().points[40].db, curve.points[40].db, 1.0e-9);
            expectWithinAbsoluteError (c.parameters.getRawParameterValue ("match_curve_amount")->load(), 62.5f, 1.0e-4f);
            expect (applied.warnings.isEmpty(), applied.warnings.joinIntoString ("; "));

            // A preset saved before the match curve existed (the format 1 golden file): no curve, no warning about it.
            const auto golden = juce::JSON::parse (juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/presets/golden_v1.json"));
            expect (golden.isObject());
            const auto old = presets::apply (c, golden);
            waitForLoads (c);
            expect (old.ok, old.error);
            expect (c.getMatchCurve().points.empty(), "a preset without a match curve has none");
            expect (! c.parameters.state.hasProperty (AmpSimProcessor::matchCurveKey), "and the state then holds none");
            expectEquals (c.parameters.getRawParameterValue ("match_curve_on")->load(), 0.0f);
            logMessage ("  -> state: " + juce::String ((int) got.size()) + " points back within " + juce::String (worstPoint) + ", on and 62.5% restored, the FIR rebuilt; "
                        "preset: the same through JSON with no warnings; the v1 golden preset loads with no curve");
        }

        beginTest ("curves are cleaned up on the way in: sorted, invalid points dropped, duplicates merged, garbage refused");
        {
            const auto c = MatchCurve::Curve::fromPoints ({ { 2000.0, 1.0 }, { 100.0, 2.0 }, { std::nan (""), 3.0 }, { -5.0, 1.0 }, { 0.0, 1.0 },
                                                            { 100.0, 4.0 }, { 500.0, std::numeric_limits<double>::infinity() } });
            expectEquals ((int) c.points.size(), 2);
            expectEquals (c.points[0].hz, 100.0);
            expectEquals (c.points[0].db, 4.0, "the later of two points at one frequency wins");
            expectEquals (c.points[1].hz, 2000.0);
            expect (MatchCurve::Curve::fromVar (juce::var ("nonsense")).points.empty());
            expect (MatchCurve::Curve::fromVar (juce::JSON::parse ("[[100, 1], [200], \"x\", [300, 2]]")).points.size() == 2);
            std::vector<MatchCurve::Point> many;
            for (int i = 0; i < 10000; ++i)
                many.push_back ({ 20.0 + i, 0.5 });
            const auto thinned = MatchCurve::Curve::fromPoints (many);
            expectEquals ((int) thinned.points.size(), MatchCurve::maxPoints);
            expectEquals (thinned.points.back().hz, 20.0 + 9999.0);
            const auto round = MatchCurve::Curve::fromVar (juce::JSON::parse (juce::JSON::toString (matchLikeCurve().toVar())));
            expectEquals ((int) round.points.size(), (int) matchLikeCurve().points.size());
        }

        beginTest ("CPU: the block at 128-sample buffers, stereo");
        {
            MatchCurve block;
            block.setCurve (matchLikeCurve(), 1.0);
            block.prepare (fs, blockSize);
            const auto input = guitarDI ((int) (10.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            std::vector<double> micros;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                buffer.copyFrom (1, 0, input.data() + start, blockSize);
                const auto t0 = std::chrono::steady_clock::now();
                block.process (juce::dsp::AudioBlock<float> (buffer), {});
                micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
            }
            std::sort (micros.begin(), micros.end());
            const auto mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
            const auto p99 = micros[(size_t) (0.99 * (double) (micros.size() - 1))];
            expectLessThan (mean, 0.05 * deadlineMicros * cpuBudgetScale());

            const auto t0 = std::chrono::steady_clock::now();
            const auto fir = MatchCurve::designFir (matchLikeCurve(), 1.0);
            const auto designMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
            logMessage ("  -> mean " + juce::String (mean, 1) + " us per buffer (" + juce::String (100.0 * mean / deadlineMicros, 2)
                        + "% of the 2667 us deadline; limit 5%), p99 " + juce::String (p99, 1) + " us, worst " + juce::String (micros.back(), 1)
                        + " us; designing a FIR on the loader thread: " + juce::String (designMs, 1) + " ms");
        }
    }
};

MatchCurveTests matchCurveTests;
} // namespace

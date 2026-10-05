// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TestHelpers.h"
#include "dsp/Chain.h"

namespace
{
using namespace testing;

using Slot = ampsim::Chain::Slot;

Stereo runChain (ampsim::Chain& chain, const std::vector<float>& input,
                 const std::function<void (size_t start)>& beforeBlock = {})
{
    return runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
    {
        if (beforeBlock)
            beforeBlock (start);

        chain.process (block);
    });
}

// Chains are made on the heap: one is 147 KB, and MSVC gives every local in runTest() its own slot in the frame,
// which would outgrow Windows' 1 MB main-thread stack.
class ChainTests final : public juce::UnitTest
{
public:
    ChainTests() : juce::UnitTest ("Signal chain", "ampsim") {}

    void runTest() override
    {
        const auto ir = syntheticCabIR (4096);

        beginTest ("the DI snapshot is the untouched guitar, even after the input gain changes the buffer");
        {
            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            chain.inputGain.setGainDecibels (12.0f);
            chain.prepare (fs, blockSize);

            const auto input = guitarDI (blockSize * 4);
            std::vector<float> snapshot;
            runChain (chain, std::vector<float> (input.begin(), input.begin() + blockSize));
            snapshot = chain.lastDISnapshot();

            expectGreaterThan (rms (input.data(), blockSize), 0.01, "the snapshot must be compared on real signal");
            expectEquals (maxAbsDifference (std::vector<float> (snapshot.begin(), snapshot.begin() + blockSize),
                                            std::vector<float> (input.begin(), input.begin() + blockSize)), 0.0);
            logMessage ("  -> with +12 dB input gain, the DI snapshot still equals the raw input exactly");
        }

        beginTest ("zero latency end to end, and left == right for the one-mic chain");
        {
            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            const auto loaded = chain.cab.loadCloseMicSamples (0, toBuffer (ir), fs, "synthetic");
            chain.prepare (fs, blockSize);

            std::vector<float> impulse (8192, 0.0f);
            impulse[0] = 1.0f;
            const auto out = runChain (chain, impulse);

            expectEquals (chain.latencySamples(), 0);
            expectWithinAbsoluteError ((double) out.left[0], ir[0] * loaded.gain, 1.0e-6);
            expectEquals (maxAbsDifference (out.left, out.right), 0.0);
            logMessage ("  -> reported latency " + juce::String (chain.latencySamples()) + " samples; impulse in at sample 0 "
                        + "comes out at sample 0 (" + juce::String (out.left[0], 6) + " = h[0]); left/right difference "
                        + juce::String (maxAbsDifference (out.left, out.right)));
        }

        beginTest ("cab bypass crossfades linearly over 10 ms, then the cab is skipped entirely");
        {
            const auto signal = sine (220.0, 0.5, (int) fs);
            const size_t bypassAt = 50 * blockSize;
            const int fade = juce::roundToInt (fs * ampsim::Chain::bypassFadeSeconds); // 480

            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            auto wetRefOwner = std::make_unique<ampsim::Chain>();
            auto& wetRef = *wetRefOwner;
            auto dryRefOwner = std::make_unique<ampsim::Chain>();
            auto& dryRef = *dryRefOwner;
            chain.cab.loadCloseMicSamples (0, toBuffer (ir), fs, "synthetic");
            wetRef.cab.loadCloseMicSamples (0, toBuffer (ir), fs, "synthetic");
            for (auto* c : { &chain, &wetRef, &dryRef })
                c->prepare (fs, blockSize);

            bool skipped = false;
            const auto out = runChain (chain, signal, [&] (size_t start)
            {
                if (start == bypassAt)
                    chain.setBypassed (Slot::cab, true);

                skipped = skipped || chain.isFullyBypassed (Slot::cab);
            }).left;

            const auto wet = runChain (wetRef, signal).left; // cab always on
            const auto dry = runChain (dryRef, signal).left; // no IR, so the cab passes through

            // Expected: wet, then dry + w (wet - dry) with w stepping 1 -> 0 over 480 samples, then dry.
            std::vector<float> expected (signal.size());
            for (size_t n = 0; n < signal.size(); ++n)
            {
                const auto k = (int) n - (int) bypassAt;
                const auto w = k < 0 ? 1.0f : std::max (0.0f, 1.0f - (float) (k + 1) / (float) fade);
                expected[n] = dry[n] + w * (wet[n] - dry[n]);
            }

            const auto deviation = maxAbsDifference (out, expected);
            const auto steady = std::max (maxStep (wet), maxStep (dry));
            const auto during = maxStep (out, bypassAt - 1, bypassAt + (size_t) fade + 1);
            const auto hardJump = std::abs (dry[bypassAt] - wet[bypassAt - 1]);

            expect (skipped, "the cab should be skipped once fully bypassed");
            expectLessThan (deviation, 1.0e-5);
            expectLessThan (during, steady * 1.25);
            logMessage ("  -> follows cab-on, a linear 480-sample (10 ms) fade, then cab-off: max deviation "
                        + juce::String (deviation, 8) + "; largest step during the fade " + juce::String (during, 5)
                        + " vs. " + juce::String (steady, 5) + " steady (a hard switch would jump " + juce::String (hardJump, 5) + ")");

            writeWav (proofDir().getChildFile ("cab_bypass_toggle.wav"), out);
        }

        beginTest ("re-enabling a fully bypassed cab clears its stale tail first");
        {
            // A long, reverb-like IR: noise decaying with a 100 ms time constant, so it rings for
            // hundreds of milliseconds after the input stops.
            auto ringing = whiteNoise (16384, 1.0f, 6);
            std::vector<double> longIR (ringing.size());
            for (size_t n = 0; n < ringing.size(); ++n)
                longIR[n] = ringing[n] * std::exp (-(double) n / (0.1 * fs));

            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            auto controlOwner = std::make_unique<ampsim::Chain>();
            auto& control = *controlOwner;
            chain.cab.loadCloseMicSamples (0, toBuffer (longIR), fs, "ringing");
            control.cab.loadCloseMicSamples (0, toBuffer (longIR), fs, "ringing");
            chain.prepare (fs, blockSize);
            control.prepare (fs, blockSize);

            // 100 ms of loud noise, bypass the cab, 53 ms of silence, re-enable, ~200 ms of silence.
            auto signal = whiteNoise ((int) (0.1 * fs), 0.8f, 5);
            signal.resize ((size_t) (0.35 * fs), 0.0f);
            const size_t bypassAt = (size_t) (0.1 * fs) / blockSize * blockSize;
            const size_t enableAt = bypassAt + 20 * blockSize;

            const auto out = runChain (chain, signal, [&] (size_t start)
            {
                if (start == bypassAt) chain.setBypassed (Slot::cab, true);
                if (start == enableAt) chain.setBypassed (Slot::cab, false);
            }).left;

            // Control: the same cab, never bypassed, is still ringing at that point.
            const auto controlOut = runChain (control, signal).left;

            const auto afterEnable = rms (out.data() + enableAt, out.size() - enableAt);
            const auto controlTail = rms (controlOut.data() + enableAt, controlOut.size() - enableAt);

            expectLessThan (afterEnable, 1.0e-7);
            expectGreaterThan (controlTail, 1.0e-3);
            logMessage ("  -> after re-enabling into silence the output RMS is " + juce::String (afterEnable, 9)
                        + "; the same cab without the bypass is still ringing at " + juce::String (toDb (controlTail), 1)
                        + " dBFS, so the reset cleared a real tail");
        }

        beginTest ("toggling bypass back mid-fade reverses smoothly with no reset");
        {
            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            chain.cab.loadCloseMicSamples (0, toBuffer (ir), fs, "synthetic");
            chain.prepare (fs, blockSize);

            const auto signal = sine (220.0, 0.5, (int) (0.5 * fs));
            const size_t offAt = 40 * blockSize, onAt = offAt + 2 * blockSize; // 5 ms into a 10 ms fade
            bool everFullyOff = false;

            const auto out = runChain (chain, signal, [&] (size_t start)
            {
                if (start == offAt) chain.setBypassed (Slot::cab, true);
                if (start == onAt) chain.setBypassed (Slot::cab, false);
                everFullyOff = everFullyOff || chain.isFullyBypassed (Slot::cab);
            }).left;

            const auto steady = maxStep (out, 0, offAt);
            const auto during = maxStep (out, offAt, onAt + 6 * blockSize);

            expect (! everFullyOff);
            expectLessThan (during, steady * 1.25);
            logMessage ("  -> never fully bypassed; largest step " + juce::String (during, 5) + " vs. " + juce::String (steady, 5) + " steady");
        }

        beginTest ("reordering a section dips to its input, swaps, and fades back: no click, and it ends up as the new order");
        {
            // Compressor and EQ in the pre section, set so the order matters: a big low boost before a
            // heavy compressor isn't the same as after it.
            const auto setUp = [] (ampsim::Chain& chain)
            {
                ampsim::Compressor::Settings comp;
                comp.thresholdDb = -30.0f;
                comp.ratio = 8.0f;
                comp.mix = 1.0f;
                comp.autoRelease = false; // 120 ms release, so two differently started envelopes agree within 2 s
                chain.preCompressor.setSettings (comp);
                ampsim::Equalizer::Settings eq;
                eq.mode = ampsim::Equalizer::Mode::parametric;
                eq.bands[0] = { ampsim::Equalizer::BandType::lowShelf, 250.0f, 12.0f, 0.7071f };
                chain.preEq.setSettings (eq);
                chain.setBypassed (Slot::preCompressor, false);
                chain.setBypassed (Slot::preEq, false);
            };

            using Section = ampsim::Chain::Section;
            const std::vector<Slot> swapped { Slot::gateA, Slot::preEq, Slot::preCompressor, Slot::boost, Slot::overdrive };
            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            auto referenceOwner = std::make_unique<ampsim::Chain>();
            auto& reference = *referenceOwner;
            setUp (chain);
            setUp (reference);
            expect (reference.requestOrder (Section::pre, swapped));
            chain.prepare (fs, blockSize);
            reference.prepare (fs, blockSize);

            const auto input = guitarDI ((int) (4.0 * fs));
            const auto swapAt = (size_t) (1.0 * fs) / blockSize * blockSize;
            int reorderBlocks = 0;
            const auto out = runChain (chain, input, [&] (size_t start)
            {
                if (start == swapAt)
                    expect (chain.requestOrder (Section::pre, swapped));
                reorderBlocks += chain.isReordering (Section::pre) ? 1 : 0;
            }).left;
            const auto expected = runChain (reference, input).left;

            expect (chain.getAppliedOrder (Section::pre) == swapped);
            expectGreaterThan (reorderBlocks, 5);

            const auto steady = std::max (maxStep (out, (size_t) (0.5 * fs), swapAt), maxStep (out, swapAt + 4800, swapAt + 48000));
            const auto during = maxStep (out, swapAt, swapAt + 2400);
            expectLessThan (during, steady * 1.05);

            std::vector<float> tail (out.begin() + (long) (3.0 * fs), out.end()), expectedTail (expected.begin() + (long) (3.0 * fs), expected.end());
            const auto converged = relativeErrorDb (tail, expectedTail);
            expectLessThan (converged, -100.0);

            const auto before = maxAbsDifference (std::vector<float> (out.begin() + (long) (0.5 * fs), out.begin() + (long) swapAt),
                                                  std::vector<float> (expected.begin() + (long) (0.5 * fs), expected.begin() + (long) swapAt));

            // Only permutations of the section's own blocks are accepted.
            expect (! chain.requestOrder (Section::pre, { Slot::preEq, Slot::preEq }));
            expect (! chain.requestOrder (Section::pre, { Slot::preEq, Slot::postCompressor }));
            expect (! chain.requestOrder (Section::post, { Slot::postEq }));

            logMessage ("  -> pre FX compressor -> EQ swapped to EQ -> compressor at 1 s: " + juce::String (reorderBlocks) + " blocks of dip ("
                        + juce::String (reorderBlocks * blockSize * 1000.0 / fs, 1) + " ms); largest step " + juce::String (during, 4) + " vs. "
                        + juce::String (steady, 4) + " steady");
            logMessage ("  -> the two orders differ by up to " + juce::String (before, 3) + " before the swap; 2 s after it, the output is "
                        + juce::String (converged, 1) + " dB from a chain that started in the new order");
            logMessage ("  -> refused: a block twice, a block from the other section, a missing block");
        }

        beginTest ("a reorder doesn't leave a splice in a delay line: the repeats of the swap are as clean as the playing");
        {
            // Chorus then delay, swapped to delay then chorus while a pure tone plays. Without care, the
            // delay line stores the jump from the chorus's output to the dry input at the swap, and replays
            // it 300 ms later, after the section has faded back in, where no dip hides it.
            const auto setUp = [] (ampsim::Chain& chain)
            {
                ampsim::Chorus::Settings chorus;
                chorus.mix = 1.0f;
                chorus.analog = false;
                chain.chorus.setSettings (chorus);
                ampsim::Delay::Settings delay;
                delay.timeMs = 300.0f;
                delay.feedback = 0.5f;
                delay.mix = 0.5f;
                delay.duckDb = 0.0f;
                delay.lowCutHz = 20.0f;
                delay.highCutHz = 20000.0f;
                chain.delay.setSettings (delay);
                chain.setBypassed (Slot::chorus, false);
                chain.setBypassed (Slot::delay, false);
            };
            using Section = ampsim::Chain::Section;
            const std::vector<Slot> swapped { Slot::postEq, Slot::postCompressor, Slot::delay, Slot::chorus, Slot::reverb };

            auto chainOwner = std::make_unique<ampsim::Chain>();
            auto& chain = *chainOwner;
            setUp (chain);
            chain.prepare (fs, blockSize);
            auto tone = sine (220.0, 0.25, (int) (1.5 * fs));
            tone.resize ((size_t) (2.6 * fs), 0.0f); // then silence, so the repeats stand alone
            const auto swapAt = (size_t) (1.0 * fs) / blockSize * blockSize;
            const auto out = runChain (chain, tone, [&] (size_t start)
            {
                if (start == swapAt)
                    chain.requestOrder (Section::post, swapped);
            }).left;

            // A click shows as a burst in the second difference (a step or kink has a large one; a 220 Hz
            // tone and its chorus a small one).
            const auto curvature = [&out] (size_t from, size_t to)
            {
                double worst = 0.0;
                for (size_t n = std::max<size_t> (from, 2); n < std::min (to, out.size()); ++n)
                    worst = std::max (worst, std::abs ((double) out[n] - 2.0 * out[n - 1] + (double) out[n - 2]));
                return worst;
            };
            const auto delaySamples = (size_t) (0.3 * fs);
            const auto steady = curvature ((size_t) (0.5 * fs), (size_t) (0.95 * fs));
            const auto atSwap = curvature (swapAt - 480, swapAt + 2400);
            juce::StringArray repeats;
            double worstRepeat = 0.0;
            for (size_t r = 1; r <= 3; ++r)
            {
                const auto centre = swapAt + r * delaySamples;
                const auto ratio = curvature (centre - 960, centre + 2400) / steady;
                worstRepeat = std::max (worstRepeat, ratio);
                repeats.add ("x" + juce::String (ratio, 2));
            }
            expectLessThan (worstRepeat, 1.5);
            logMessage ("  -> chorus <-> delay swapped at 1 s on a 220 Hz tone (delay 300 ms, feedback 50%): largest second difference in the "
                        "repeats of the swap " + repeats.joinIntoString (", ") + " of steady playing; during the swap itself x"
                        + juce::String (atSwap / steady, 2));
        }
    }
};

ChainTests chainTests;
} // namespace

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

class ChainTests final : public juce::UnitTest
{
public:
    ChainTests() : juce::UnitTest ("Signal chain", "ampsim") {}

    void runTest() override
    {
        const auto ir = syntheticCabIR (4096);

        beginTest ("the DI snapshot is the untouched guitar, even after the input gain changes the buffer");
        {
            ampsim::Chain chain;
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
            ampsim::Chain chain;
            const auto loaded = chain.cab.loadSamples (toBuffer (ir), fs, "synthetic");
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

            ampsim::Chain chain, wetRef, dryRef;
            chain.cab.loadSamples (toBuffer (ir), fs, "synthetic");
            wetRef.cab.loadSamples (toBuffer (ir), fs, "synthetic");
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

            ampsim::Chain chain, control;
            chain.cab.loadSamples (toBuffer (longIR), fs, "ringing");
            control.cab.loadSamples (toBuffer (longIR), fs, "ringing");
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
            ampsim::Chain chain;
            chain.cab.loadSamples (toBuffer (ir), fs, "synthetic");
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
    }
};

ChainTests chainTests;
} // namespace

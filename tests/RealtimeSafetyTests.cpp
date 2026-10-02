#include "AllocationTracking.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

juce::String describe (const rtcheck::Counts& c)
{
    return juce::String (c.allocations) + " allocations, " + juce::String (c.frees) + " frees, "
           + juce::String (c.blockingLocks) + " blocking locks";
}

class RealtimeSafetyTests final : public juce::UnitTest
{
public:
    RealtimeSafetyTests() : juce::UnitTest ("Real-time safety", "ampsim") {}

    void runTest() override
    {
        beginTest ("positive control: the detector catches an allocation, a free, and a lock");
        {
            juce::CriticalSection mutex;
            rtcheck::begin();
            {
                std::vector<int> v (1000);
                juce::String s ("text");
                s << 12345;
                const juce::ScopedLock lock (mutex);
            }
            const auto c = rtcheck::end();

            expectGreaterThan (c.allocations, 0L);
            expectGreaterThan (c.frees, 0L);
            expectGreaterThan (c.blockingLocks, 0L);
            logMessage ("  -> deliberately bad code: " + describe (c) + " (detector works)");
        }

        beginTest ("10 s through the whole processor while switching models, swapping IRs, bypassing, and turning knobs: "
                   "nothing allocated, freed, or locked on the audio thread");
        {
            const auto irA = tempDir().getChildFile ("rt_ir_a.wav");
            const auto irB = tempDir().getChildFile ("rt_ir_b.wav");
            writeWav (irA, toBuffer (syntheticCabIR (4096)));
            writeWav (irB, toBuffer (syntheticCabIR (1024)));
            const auto a1 = exampleModel ("wavenet_a1_standard.nam");
            const auto lstm = exampleModel ("lstm.nam");

            AmpSimProcessor p;
            p.loadModel (a1);
            p.loadImpulseResponse (irA);
            while (p.isLoading())
                juce::Thread::sleep (5);
            p.prepareToPlay (fs, blockSize);

            const auto input = exampleInput ((int) (10.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            rtcheck::Counts total;
            int blocks = 0, switchingBlocks = 0;

            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize, ++blocks)
            {
                // What a player does mid-song. These calls are the message thread's side, outside the measurement.
                switch (blocks)
                {
                    case 200:  p.loadModel (lstm); break;                // model switch
                    case 600:  p.loadImpulseResponse (irB); break;      // IR swap
                    case 1000: setParam (p, "cab_bypass", 1.0f); break;  // cab off
                    case 1100: setParam (p, "cab_bypass", 0.0f); break;  // cab back on (reset + fade in)
                    case 1500: setParam (p, "input_gain", 6.0f); break;  // knob moves (gain ramps)
                    case 1600: setParam (p, "output_gain", -6.0f); break;
                    case 2000: p.loadModel (a1); break;                  // switch back
                    default: break;
                }

                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);

                rtcheck::begin();
                p.processBlock (buffer, midi); // the audio thread's side, measured
                total += rtcheck::end();

                switchingBlocks += p.getChain().amp.isSwitching() ? 1 : 0;

                // Real time: a block is 2.67 ms, so loader threads get time to finish their work.
                if (blocks % 8 == 0)
                    juce::Thread::sleep (1);
            }

            expectGreaterThan (switchingBlocks, 8, "both model switches must have run during the measurement");
            expect (p.getStatus().cab.contains ("rt_ir_b"));
            expectEquals (total.allocations, 0L);
            expectEquals (total.frees, 0L);
            expectEquals (total.blockingLocks, 0L);

            logMessage ("  -> " + juce::String (blocks) + " blocks (" + juce::String (blocks * blockSize / fs, 1)
                        + " s of audio): 2 model switches (" + juce::String (switchingBlocks) + " blocks mid-crossfade), 1 IR swap, "
                        "cab bypass off and on, 2 knob ramps");
            logMessage ("  -> audio thread: " + describe (total));
        }
    }
};

RealtimeSafetyTests realtimeSafetyTests;
} // namespace

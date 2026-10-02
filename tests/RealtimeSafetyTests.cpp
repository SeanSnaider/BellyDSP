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

        beginTest ("10 s through the whole processor while using everything: nothing allocated, freed, or locked on the audio thread");
        {
            const auto irA = tempDir().getChildFile ("rt_ir_a.wav");
            const auto irB = tempDir().getChildFile ("rt_ir_b.wav");
            const auto irC = tempDir().getChildFile ("rt_ir_c.wav");
            const auto roomIR = tempDir().getChildFile ("rt_room.wav");
            writeWav (irA, toBuffer (syntheticCabIR (4096)));
            writeWav (irB, toBuffer (syntheticCabIR (1024)));
            writeWav (irC, toBuffer (syntheticCabIR (2048, 8.0, 7000.0)));
            {
                juce::AudioBuffer<float> stereoRoom (2, 24000);
                juce::Random random (3);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < stereoRoom.getNumSamples(); ++i)
                        stereoRoom.setSample (ch, i, (2.0f * random.nextFloat() - 1.0f) * std::exp (-(float) i / 4800.0f));
                writeWav (roomIR, stereoRoom);
            }
            const auto a1 = exampleModel ("wavenet_a1_standard.nam");
            const auto lstm = exampleModel ("lstm.nam");
            const auto small = exampleModel ("wavenet.nam");

            AmpSimProcessor p;
            p.loadModel (0, a1);
            p.loadCabIR (0, irA);
            while (p.isLoading())
                juce::Thread::sleep (5);
            p.prepareToPlay (fs, blockSize);

            const auto input = guitarDI ((int) (10.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            rtcheck::Counts total;
            int blocks = 0, modelFadeBlocks = 0, slotSwitchBlocks = 0;
            juce::StringArray events;

            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize, ++blocks)
            {
                // What a player does mid-song. These are the message thread's side, outside the measurement.
                midi.clear();
                switch (blocks)
                {
                    case 200:  p.loadModel (0, lstm); break;                                // new capture in the playing slot
                    case 300:  p.loadModel (1, small); break;                               // capture into a slot that isn't playing
                    case 450:  setParam (p, AmpSimProcessor::slotParamId, 1.0f); break;     // slot 2 from the GUI
                    case 600:  p.loadCabIR (0, irB); break;                                 // IR swap on close mic 1
                    case 650:  p.loadCabIR (1, irC); break;                                 // close mic 2 (and auto alignment)
                    case 680:  p.loadCabIR (AmpSimProcessor::roomMic, roomIR); break;       // stereo room mic
                    case 720:  setParam (p, AmpSimProcessor::cabParamId (0, "pan"), -0.6f); break;
                    case 740:  setParam (p, AmpSimProcessor::cabParamId (1, "level"), -6.0f); break;
                    case 760:  setParam (p, "cab_align", 0.0f); break;                      // manual delay and polarity
                    case 770:  setParam (p, AmpSimProcessor::cabParamId (1, "delay"), 40.0f); break;
                    case 780:  setParam (p, AmpSimProcessor::cabParamId (1, "invert"), 1.0f); break;
                    case 790:  setParam (p, AmpSimProcessor::cabParamId (AmpSimProcessor::roomMic, "predelay"), 20.0f); break;
                    case 820:  setParam (p, "cab_lowcut_on", 1.0f); break;                  // cuts on, slope change, sweep
                    case 830:  setParam (p, "cab_highcut_on", 1.0f); break;
                    case 840:  setParam (p, "cab_lowcut_slope", 1.0f); break;
                    case 850:  setParam (p, "cab_highcut_freq", 4000.0f); break;
                    case 870:  setParam (p, AmpSimProcessor::cabParamId (0, "mute"), 1.0f); break;
                    case 890:  setParam (p, "cab_lowcut_on", 0.0f); break;
                    case 700:  setParam (p, AmpSimProcessor::ampParamId (1, "bass"), 6.0f); break;     // tone knobs
                    case 710:  setParam (p, AmpSimProcessor::ampParamId (1, "presence"), -4.0f); break;
                    case 800:  setParam (p, AmpSimProcessor::ampParamId (1, "output_trim"), -3.0f); break;
                    case 900:  midi.addEvent (juce::MidiMessage::programChange (1, 2), 0); break;      // footswitch: slot 3
                    case 1000: setParam (p, "cab_bypass", 1.0f); break;                     // cab off
                    case 1100: setParam (p, "cab_bypass", 0.0f); break;                     // cab back on
                    case 1200: midi.addEvent (juce::MidiMessage::programChange (1, 0), 0); break;      // footswitch: slot 1
                    case 1500: setParam (p, "input_gain", 6.0f); break;
                    case 1600: setParam (p, "output_gain", -6.0f); break;
                    case 2000: p.loadModel (0, a1); break;                                  // switch the capture back
                    default: break;
                }

                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);

                rtcheck::begin();
                p.processBlock (buffer, midi); // the audio thread's side, measured
                total += rtcheck::end();

                modelFadeBlocks += p.getChain().amp.isLoadingModel() ? 1 : 0;
                slotSwitchBlocks += p.getChain().amp.isSwitching() ? 1 : 0;

                // Real time: a block is 2.67 ms, so loader threads get time to finish their work,
                // and the message thread's housekeeping runs now and then.
                if (blocks % 8 == 0)
                {
                    juce::Thread::sleep (1);
                    p.runHousekeeping();
                }
            }

            expectGreaterThan (modelFadeBlocks, 16, "the model loads must have crossfaded during the measurement");
            expectGreaterThan (slotSwitchBlocks, 16, "the slot switches must have crossfaded during the measurement");
            expectEquals (p.getChain().amp.getSelectedSlot(), 0);
            expect (p.getStatus().cab[0].contains ("rt_ir_b"));
            expect (p.getStatus().cab[1].contains ("rt_ir_c") && p.getStatus().cab[2].contains ("rt_room"));
            expect (p.getChain().cab.closeMic (1).hasImpulseResponse() && p.getChain().cab.roomMic().hasImpulseResponse(),
                    "the IRs loaded during playback must have reached the audio thread");
            expectEquals (total.allocations, 0L);
            expectEquals (total.frees, 0L);
            expectEquals (total.blockingLocks, 0L);

            logMessage ("  -> " + juce::String (blocks) + " blocks (" + juce::String (blocks * blockSize / fs, 1)
                        + " s of audio): 3 capture loads (" + juce::String (modelFadeBlocks) + " blocks mid-crossfade), "
                        "3 slot switches from the GUI and the footswitch (" + juce::String (slotSwitchBlocks)
                        + " blocks mid-crossfade), 3 IR loads into the three cab mics plus an IR swap, auto alignment, 6 cab mic changes, cuts on, off, re-sloped and swept, cab bypass off and on, 5 knob ramps");
            logMessage ("  -> audio thread: " + describe (total));
        }
    }
};

RealtimeSafetyTests realtimeSafetyTests;
} // namespace

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "MidiMap.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "platform/AppInfo.h"

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
            if (rtcheck::lockCountingAvailable)
                expectGreaterThan (c.blockingLocks, 0L);
            else
                logMessage ("  -> NOTE: this platform's build can't count blocking locks; the macOS run is the real check");
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
            // A cab pack for a moving mic: four captures on a 2 x 2 grid (cap/edge, 1in/4in).
            const auto packFolder = tempDir().getChildFile ("rt_pack");
            packFolder.deleteRecursively();
            packFolder.createDirectory();
            writeWav (packFolder.getChildFile ("Cap_1in.wav"), toBuffer (syntheticCabIR (4096, 8.0, 7000.0)));
            writeWav (packFolder.getChildFile ("Edge_1in.wav"), toBuffer (syntheticCabIR (4096, 0.0, 3500.0)));
            writeWav (packFolder.getChildFile ("Cap_4in.wav"), toBuffer (syntheticCabIR (4096, 6.0, 6000.0)));
            writeWav (packFolder.getChildFile ("Edge_4in.wav"), toBuffer (syntheticCabIR (4096, -2.0, 3000.0)));

            // The longest IR bundled with the app (content/irs, copied next to this binary): 1 s.
            const auto bundledIR = platform::factoryContentFolder().getChildFile ("irs/Modern 4x12/Modern 4x12, dynamic, 75 W, var. 3.wav");
            expect (bundledIR.existsAsFile(), bundledIR.getFullPathName());

            const auto a1 = exampleModel ("wavenet_a1_standard.nam");
            const auto lstm = exampleModel ("lstm.nam");
            const auto small = exampleModel ("wavenet.nam");

            AmpSimProcessor p;
            p.getMidiMap().set ({ 82, MidiMapping::Action::toggle, "chorus_on" }); // a footswitch mapped to an effect
            p.getMidiMap().set ({ 11, MidiMapping::Action::continuous, "delay_mix", 0.0f, 100.0f }); // an expression pedal
            p.loadModel (0, a1);
            p.loadCabIR (0, irA);
            while (p.isLoading())
                juce::Thread::sleep (5);
            p.prepareToPlay (fs, blockSize);

            const auto input = guitarDI ((int) (10.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            rtcheck::Counts total;
            int blocks = 0, modelFadeBlocks = 0, slotSwitchBlocks = 0, morphsBefore = 0;
            bool ampWasBypassed = false, sectionsWereBypassed = false;
            juce::StringArray events;

            // The GUI's side of the meters and the analyzer (Phase 11): it takes the peaks and empties the
            // analyzer's ring now and then, outside the measurement.
            std::vector<float> analyzerSink (4096);
            juce::int64 analyzed = 0;
            const auto guiReads = [&]
            {
                p.takePeaks();
                for (int n; (n = p.getAnalyzerRing().read (analyzerSink.data(), (int) analyzerSink.size())) > 0;)
                    analyzed += n;
            };

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
                    case 630:  p.loadCabIR (0, bundledIR); break;                           // a built-in 1 s IR (content/irs)
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
                    case 905:  p.loadCabIR (1, packFolder); morphsBefore = p.getMorphCount(); break; // close mic 2 becomes movable
                    case 1000: setParam (p, "cab_bypass", 1.0f); break;                     // cab off
                    case 1100: setParam (p, "cab_bypass", 0.0f); break;                     // cab back on
                    case 1200: midi.addEvent (juce::MidiMessage::programChange (1, 0), 0); break;      // footswitch: slot 1
                    case 1250: setParam (p, "input_level_dbu", 15.0f); break;               // interface level: captures reload
                    // Phase 4 effects: on, re-moded, re-typed, re-sloped, and reordered while playing.
                    case 1300: setParam (p, "comp_pre_on", 1.0f); break;
                    case 1320: setParam (p, "eq_pre_g3", 6.0f); break;
                    case 1340: setParam (p, "eq_pre_mode", 1.0f); break;                    // graphic -> parametric crossfade
                    case 1360: setParam (p, "eq_pre_b2_type", 3.0f); break;                 // peak -> notch: dip and swap
                    case 1380: p.setSectionOrder (ampsim::Chain::Section::pre, { "eq", "comp" }); break;
                    case 1400: setParam (p, "comp_post_on", 1.0f); break;
                    case 1420: setParam (p, "eq_post_lowcut_on", 1.0f); break;
                    case 1440: setParam (p, "eq_post_lowcut_slope", 2.0f); break;           // 12 -> 48 dB/oct: dip and swap
                    case 1460: p.setSectionOrder (ampsim::Chain::Section::post, { "comp", "eq" }); break;
                    case 1480: setParam (p, "comp_pre_mode", 1.0f); break;                  // studio -> pedal
                    case 1520: setParam (p, "comp_post_detector", 1.0f); break;             // peak -> RMS
                    case 1540: setParam (p, "eq_post_b3_gain", -9.0f); break;
                    // The delay: on, retimed by footswitch taps, re-moded, re-laid-out, bypassed into spillover.
                    case 1560: setParam (p, "delay_on", 1.0f); break;
                    case 1610: midi.addEvent (juce::MidiMessage::controllerEvent (1, 80, 127), 17); break;
                    case 1800: midi.addEvent (juce::MidiMessage::controllerEvent (1, 80, 127), 5); break;
                    case 1990: midi.addEvent (juce::MidiMessage::controllerEvent (1, 80, 127), 90); break;
                    case 2100: setParam (p, "delay_mode", 1.0f); break;
                    case 2150: setParam (p, "delay_stereo", 1.0f); break;
                    case 2200: setParam (p, "delay_feedback", 105.0f); break;
                    case 2250: setParam (p, "delay_sync", 0.0f); break;
                    case 2300: setParam (p, "delay_time", 650.0f); break;
                    case 2400: setParam (p, "delay_on", 0.0f); break;           // spillover: the repeats ring on
                    case 2600: setParam (p, "delay_mode", 2.0f); setParam (p, "delay_on", 1.0f); break;
                    // Chorus and reverb: on, re-moded, synced, frozen from the footswitch, bypassed into spillover.
                    case 2650: setParam (p, "chorus_on", 1.0f); break;
                    case 2700: setParam (p, "chorus_mode", 2.0f); break;                    // Tri
                    case 2720: setParam (p, "chorus_shape", 2.0f); break;                   // random
                    case 2740: setParam (p, "chorus_sync", 1.0f); break;
                    case 2760: setParam (p, "reverb_on", 1.0f); break;
                    case 2800: setParam (p, "reverb_engine", 2.0f); break;                  // plate
                    case 2850: midi.addEvent (juce::MidiMessage::controllerEvent (1, 81, 127), 33); break; // freeze
                    case 2950: midi.addEvent (juce::MidiMessage::controllerEvent (1, 81, 127), 7); break;  // thaw
                    case 3000: setParam (p, "reverb_predelay_sync", 1.0f); setParam (p, "reverb_size", 80.0f); break;
                    case 3100: setParam (p, "reverb_on", 0.0f); setParam (p, "chorus_on", 0.0f); break;   // reverb spills over
                    case 3300: setParam (p, "reverb_on", 1.0f); setParam (p, "reverb_engine", 0.0f); setParam (p, "reverb_shimmer", 70.0f); break;
                    case 3320: setParam (p, "reverb_shimmer_interval", 1.0f); setParam (p, "mv_on", 1.0f); break;
                    // Phase 10 harmonizer: on with two voices, the key and scale changed, the floor moved, off again.
                    case 3331: setParam (p, "harm_on", 1.0f); setParam (p, "harm_v2_on", 1.0f); break;
                    case 3351: setParam (p, "harm_root", 4.0f); setParam (p, "harm_scale", 1.0f); setParam (p, "harm_floor", 1.0f); break;
                    case 3371: setParam (p, "harm_v2_mode", 1.0f); setParam (p, "harm_out_of_key", 1.0f); break;
                    case 3391: setParam (p, "harm_on", 0.0f); break;
                    case 3340: setParam (p, "mv_engine", 1.0f); setParam (p, "mv_voices", 8.0f); break;
                    case 3360: setParam (p, "mv_engine", 0.0f); setParam (p, "reverb_freeze", 1.0f); break;
                    case 3380: setParam (p, "reverb_freeze", 0.0f); setParam (p, "mv_on", 0.0f); break;
                    // Phase 7 tuner: engaged (muting), A4 moved, heard instead of muted, disengaged.
                    case 2620: setParam (p, "tuner_on", 1.0f); break;
                    case 2780: setParam (p, "tuner_a4", 432.0f); setParam (p, "tuner_mute", 0.0f); break;
                    case 2900: setParam (p, "tuner_on", 0.0f); break;
                    case 3400: midi.addEvent (juce::MidiMessage::controllerEvent (1, 82, 127), 3); break; // mapped toggle
                    case 3420: midi.addEvent (juce::MidiMessage::controllerEvent (1, 82, 0), 9); break;
                    case 3450:
                        for (int i = 0; i < 64; ++i)
                            midi.addEvent (juce::MidiMessage::controllerEvent (1, 11, i * 2), i * 2); // pedal sweep
                        break;
                    // Phase 6 gates: both on and linked, Learn, unlinked with Gate B on its own detector, re-moded,
                    // relinked, Gate A off while Gate B follows it, then Learn on both.
                    case 1650: setParam (p, "gate_a_on", 1.0f); setParam (p, "gate_b_on", 1.0f); break;
                    case 1700: p.learnGates(); break;                                        // 2 s: finishes at block 2450
                    case 1850: setParam (p, "gate_link", 0.0f); break;
                    case 1900: setParam (p, "gate_b_detector", 1.0f); setParam (p, "gate_b_threshold", -30.0f); break;
                    case 1950: setParam (p, "gate_a_release_mode", 1.0f); setParam (p, "gate_a_threshold", -45.0f); break;
                    case 2050: setParam (p, "gate_link", 1.0f); break;
                    case 2350: setParam (p, "gate_a_on", 0.0f); break;
                    case 3500: setParam (p, "gate_link", 0.0f); p.learnGates(); break;
                    // Phase 11 scenes: stored, then recalled from the footswitch (CC 70) while playing.
                    case 1750: p.storeScene (0); break;
                    case 2701: p.storeScene (1); break;
                    case 2901: midi.addEvent (juce::MidiMessage::controllerEvent (1, 70, 0), 17); break;
                    case 3101: midi.addEvent (juce::MidiMessage::controllerEvent (1, 70, 1), 5); break;
                    // Phase 6 drive: boost and overdrive on, re-moded, 8x and back, tight, off again.
                    case 2450: setParam (p, "boost_on", 1.0f); setParam (p, "boost_mode", 2.0f); break; // Screamer
                    case 2500: setParam (p, "od_on", 1.0f); setParam (p, "od_mode", 1.0f); setParam (p, "od_drive", 80.0f); break;
                    case 2550: setParam (p, "drive_oversampling", 1.0f); break;                        // 8x
                    // Phase 8 Bloom: on with all three, re-moded, reordered, through-zero on and off, bypassed.
                    case 3010: setParam (p, "bloom_on", 1.0f); setParam (p, "bloom_crush_on", 1.0f); setParam (p, "bloom_phaser_on", 1.0f);
                               setParam (p, "bloom_flanger_on", 1.0f); break;
                    case 3050: setParam (p, "bloom_phaser_mode", 2.0f); setParam (p, "bloom_flanger_shape", 2.0f); break;
                    case 3150: p.setBloomOrder ({ "flanger", "bitcrush", "phaser" }); break;
                    case 3200: setParam (p, "bloom_flanger_tz", 1.0f); break;
                    case 3250: setParam (p, "bloom_flanger_tz", 0.0f); setParam (p, "bloom_phaser_stages", 4.0f); setParam (p, "bloom_phaser_mode", 1.0f); break;
                    case 3350: setParam (p, "bloom_on", 0.0f); break;
                    case 3550: setParam (p, "od_tight", 1.0f); setParam (p, "od_mode", 0.0f); setParam (p, "boost_mode", 1.0f); break;
                    case 3600: setParam (p, "drive_oversampling", 0.0f); setParam (p, "od_on", 0.0f); setParam (p, "boost_on", 0.0f); break;
                    // Phase 11 GUI hooks: the analyzer taps the post section, then the pre section; from block
                    // 2401 to 2800 the GUI stops reading, so the ring fills and the audio thread drops instead of waiting.
                    case 52:   p.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::postSection); break;
                    case 2205: p.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::preSection); break;
                    case 3555: p.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::postSection); break;
                    // The UI handoff's bypass dots: the amp (its captures keep running), and each effect section.
                    case 1010: setParam (p, "amp_bypass", 1.0f); break;
                    case 1090: setParam (p, "amp_bypass", 0.0f); break;
                    case 3610: setParam (p, "pre_fx_on", 0.0f); break;
                    case 3630: setParam (p, "post_fx_on", 0.0f); break;
                    case 3650: setParam (p, "pre_fx_on", 1.0f); break;
                    case 3670: setParam (p, "post_fx_on", 1.0f); break;
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

                if (blocks == 1080)
                    ampWasBypassed = p.getChain().isFullyBypassed (ampsim::Chain::Slot::amp);
                if (blocks == 3640)
                    sectionsWereBypassed = p.getChain().isFullyBypassed (ampsim::Chain::Slot::postEq) && p.getChain().isFullyBypassed (ampsim::Chain::Slot::gateA);
                modelFadeBlocks += p.getChain().amp.isLoadingModel() ? 1 : 0;
                slotSwitchBlocks += p.getChain().amp.isSwitching() ? 1 : 0;

                // Blocks 950 to 1150: dragging close mic 2 around its pack, in real time (the 40 ms
                // re-morph limit is wall-clock time), with the timer's housekeeping running between.
                if (blocks >= 950 && blocks < 1150)
                {
                    if (blocks % 4 == 0)
                    {
                        const auto t = (blocks - 950) / 200.0;
                        setParam (p, AmpSimProcessor::cabParamId (1, "pos_x"), (float) (0.5 + 0.5 * std::sin (9.0 * t)));
                        setParam (p, AmpSimProcessor::cabParamId (1, "pos_y"), (float) t);
                    }
                    if (blocks % 2 == 0)
                    {
                        juce::Thread::sleep (3);
                        p.runHousekeeping();
                    }
                }
                // Otherwise faster than real time, but a block is 2.67 ms, so loader threads get time
                // to finish their work, and the message thread's housekeeping runs now and then.
                else if (blocks % 8 == 0)
                {
                    juce::Thread::sleep (1);
                    p.runHousekeeping();
                    if (blocks <= 2400 || blocks > 2800)
                        guiReads();
                }
            }

            expect (ampWasBypassed, "the amp bypass must have reached the audio thread");
            expect (sectionsWereBypassed, "the section switches must have reached the audio thread");
            expectGreaterThan (modelFadeBlocks, 16, "the model loads must have crossfaded during the measurement");
            expectGreaterThan (slotSwitchBlocks, 16, "the slot switches must have crossfaded during the measurement");
            expectEquals (p.getChain().amp.getSelectedSlot(), 0);
            expect (p.getStatus().cab[0].contains ("Modern 4x12, dynamic, 75 W, var. 3"), p.getStatus().cab[0]); // the built-in IR replaced rt_ir_b
            expectEquals (p.getCalibrationReloadCount(), 1, "the calibration change must have reloaded the captures during the measurement");
            expectEquals (p.getChain().gateA.getLearnCount(), 1, "the gate Learn must have finished during the measurement");
            using Slot = ampsim::Chain::Slot;
            expect (p.getChain().getAppliedOrder (ampsim::Chain::Section::pre) == std::vector<Slot> { Slot::gateA, Slot::preEq, Slot::preCompressor, Slot::boost, Slot::overdrive },
                    "the pre FX reorder must have reached the audio thread");
            const auto postOrder = p.getChain().getAppliedOrder (ampsim::Chain::Section::post);
            expect (postOrder.size() >= 2 && postOrder[0] == Slot::postCompressor && postOrder[1] == Slot::postEq,
                    "the post FX reorder must have reached the audio thread");
            expectGreaterThan (p.getCompressorReduction (false), 0.0f, "the pre compressor must have been working");
            expectGreaterThan (std::abs (p.getTempo() - 120.0), 1.0, "the footswitch taps must have changed the tempo");
            const auto morphs = p.getMorphCount() - morphsBefore;
            expectGreaterThan (morphs, 4, "the mic must have re-morphed while it was dragged");
            expect (p.getStatus().cab[1].startsWith ("rt_pack (4 IRs on a grid) at ") && p.getStatus().cab[2].contains ("rt_room"), p.getStatus().cab[1]);
            expect (p.getChain().cab.closeMic (1).hasImpulseResponse() && p.getChain().cab.roomMic().hasImpulseResponse(),
                    "the IRs loaded during playback must have reached the audio thread");
            const auto analyzerDropped = (juce::int64) p.getAnalyzerRing().takeDroppedCount();
            expectGreaterThan (analyzed, (juce::int64) 300000, "the GUI must have read the analyzer's ring during the measurement");
            expectGreaterThan (analyzerDropped, (juce::int64) 0, "the ring must have filled while the GUI stopped reading, and dropped");
            expectGreaterThan (p.getCpuLoad(), 0.0f, "the CPU meter must have measured the callbacks");
            expectEquals (total.allocations, 0L);
            expectEquals (total.frees, 0L);
            expectEquals (total.blockingLocks, 0L);

            logMessage ("  -> " + juce::String (blocks) + " blocks (" + juce::String (blocks * blockSize / fs, 1)
                        + " s of audio): 3 capture loads (" + juce::String (modelFadeBlocks) + " blocks mid-crossfade), "
                        "3 slot switches from the GUI and the footswitch (" + juce::String (slotSwitchBlocks)
                        + " blocks mid-crossfade), 3 IR loads into the three cab mics plus an IR swap and a built-in 1 s IR from the app's content folder, auto alignment, 6 cab mic changes, cuts on, off, re-sloped and swept, "
                        "a cab pack loaded into close mic 2 and dragged around (" + juce::String (morphs) + " re-morphs), cab bypass off and on, 5 knob ramps, "
                        "an interface-level change that recalibrated and reloaded every capture, "
                        "both compressors switched on (one to pedal mode, one to RMS), EQ sliders and bands moved, graphic -> parametric, "
                        "a band type change, a cut slope change, and both FX sections reordered, "
                        "the delay switched on, retimed by three footswitch taps, re-moded and re-laid-out with 105% feedback, then bypassed into spillover and back, "
                        "the chorus on with mode, shape, and sync changes, and the reverb on, re-engined twice, frozen and thawed from the footswitch, resized, "
                        "and bypassed into spillover; the amp bypassed and back (its captures running underneath), and both effect sections switched off and on");
            logMessage ("  -> the GUI hooks: input and output meters and the CPU meter every buffer, the analyzer tapping the post section, the pre section, "
                        "and the post section again; the GUI read " + juce::String (analyzed) + " samples from the ring and stopped for 400 buffers, "
                        "so the ring filled and the audio thread dropped " + juce::String (analyzerDropped) + " samples instead of waiting; CPU meter "
                        + juce::String (p.getCpuLoad(), 1) + "%");
            logMessage ("  -> audio thread: " + describe (total));
        }
    }
};

RealtimeSafetyTests realtimeSafetyTests;
} // namespace

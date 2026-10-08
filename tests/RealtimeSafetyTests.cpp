// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "BuiltInCaptures.h"
#include "MidiMap.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"
#include "ToneMatchSession.h"
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

        beginTest ("14 s through the whole processor while using everything: nothing allocated, freed, or locked on the audio thread");
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
            // As in the app: the processor starts on the eight built-in amps, all loaded, and amp 1 gets an example
            // capture on top before playing starts.
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            p.getMidiMap().set ({ 82, MidiMapping::Action::toggle, "chorus_on" }); // a footswitch mapped to an effect
            p.getMidiMap().set ({ 11, MidiMapping::Action::continuous, "delay_mix", 0.0f, 100.0f }); // an expression pedal
            p.loadModel (0, a1);
            p.loadCabIR (0, irA);
            while (p.isLoading())
                juce::Thread::sleep (5);
            p.prepareToPlay (fs, blockSize);

            const auto input = guitarDI ((int) (14.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            rtcheck::Counts total;
            int blocks = 0, modelFadeBlocks = 0, slotSwitchBlocks = 0, morphsBefore = 0, gainMovingBlocks = 0, maxSetModels = 0;
            int setSlotSwitches = 0, heldBlendBlocks = 0, maxTotalModels = 0, maxRunningAmps = 0, warmingBlocks = 0;
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
                p.getDiRecorder().drain(); // tone match's page drains its DI recording the same way
                p.getPreviewPlayer().collectGarbage(); // and frees the A/B material the audio thread handed back
            };

            // Tone match's A/B player (docs/TONE_MATCH.md, "Comparing"): material built on this (the message)
            // thread, handed over, played, switched between its sources (the raw target too), its loops moved, new material
            // swapped in while it plays, the live mute switched off, and stopped.
            auto& preview = p.getPreviewPlayer();
            const auto material = [] (double f)
            {
                auto m = std::make_unique<ampsim::PreviewPlayer::Material>();
                for (size_t s = 0; s < 3; ++s)
                    m->audio[s] = std::make_shared<const std::vector<float>> (sine (f * (double) (s + 2) / 2.0, 0.1, 96000));
                // The target before the cleanup with the take (source 5, the target's clock).
                m->audio[ampsim::PreviewPlayer::sourceTargetRaw] = std::make_shared<const std::vector<float>> (sine (f * 3.0, 0.1, 96000));
                return m;
            };
            int previewBlocks = 0;

            // Play along (docs/TONE_MATCH.md, "Play along"): the song and its guitar with a count-in, looped, switched
            // between, stopped (the player directly, as the Target card's Play drives it); then a whole take through
            // the session the page uses: Record with a target, the count-in, the section while the DI records from
            // the song's first sample, and its end by itself.
            ToneMatchSession session (p);
            session.setLatencySource ([] {
                platform::device::Latency l;
                l.known = true;
                l.inputSamples = 150;
                l.outputSamples = 190;
                return l;
            });
            std::vector<float> firstRecording;
            int countInBlocks = 0, takeBlocks = 0;
            const auto songMaterial = []
            {
                auto m = std::make_unique<ampsim::PreviewPlayer::Material>();
                m->audio[3] = std::make_shared<const std::vector<float>> (sine (196.0, 0.1, 72000));
                m->audio[4] = std::make_shared<const std::vector<float>> (sine (392.0, 0.1, 72000));
                m->clickAccent = std::make_shared<const std::vector<float>> (sine (1760.0, 0.2, 1920));
                m->click = std::make_shared<const std::vector<float>> (sine (1320.0, 0.2, 1920));
                return m;
            };

            // The match curve (dsp/MatchCurve.h): two curves as tone match would set them.
            const auto curveA = ampsim::MatchCurve::Curve::fromPoints ({ { 80.0, 3.0 }, { 400.0, -6.0 }, { 2500.0, 5.0 }, { 8000.0, -4.0 } });
            const auto curveB = ampsim::MatchCurve::Curve::fromPoints ({ { 120.0, -4.0 }, { 900.0, 4.0 }, { 5000.0, -8.0 }, { 12000.0, 2.0 } });
            const auto matchBuildsBefore = p.getMatchCurveBuildCount();
            int matchCurveBlocks = 0;

            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize, ++blocks)
            {
                // What a player does mid-song. These are the message thread's side, outside the measurement.
                midi.clear();
                switch (blocks)
                {
                    case 200:  p.loadModel (0, lstm); break;                                // new capture in the playing amp
                    case 300:  p.loadModel (1, small); break;                               // capture into an amp that isn't running
                    case 450:  setParam (p, AmpSimProcessor::ampModelParamId, 1.0f); break; // amp 2 from the shelf: it starts, warms, fades in
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
                    case 900:  midi.addEvent (juce::MidiMessage::programChange (1, 2), 0); break;      // footswitch: amp 3
                    case 905:  p.loadCabIR (1, packFolder); morphsBefore = p.getMorphCount(); break; // close mic 2 becomes movable
                    case 1000: setParam (p, "cab_bypass", 1.0f); break;                     // cab off
                    case 1100: setParam (p, "cab_bypass", 0.0f); break;                     // cab back on
                    case 1200: midi.addEvent (juce::MidiMessage::programChange (1, 0), 0); break;      // footswitch: amp 1
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
                    case 3325: setParam (p, "mv_mix_mode", 1.0f); break;                    // the multivoicer's Add mode
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
                    // The UI handoff's bypass dots: the amp (the playing amp keeps running), and each effect section.
                    case 1010: setParam (p, "amp_bypass", 1.0f); break;
                    case 1090: setParam (p, "amp_bypass", 0.0f); break;
                    case 3610: setParam (p, "pre_fx_on", 0.0f); break;
                    case 3630: setParam (p, "post_fx_on", 0.0f); break;
                    case 3650: setParam (p, "pre_fx_on", 1.0f); break;
                    case 3670: setParam (p, "post_fx_on", 1.0f); break;
                    case 1500: setParam (p, "input_gain", 6.0f); break;
                    case 1510: setParam (p, "output_gain", 18.0f); break;                   // the output limiter at work
                    case 1580: setParam (p, "output_limit_ceiling", -6.0f); break;
                    case 1590: setParam (p, "output_limit_on", 0.0f); break;                 // off and back on: its bypass fade
                    case 1595: setParam (p, "output_limit_on", 1.0f); break;
                    case 1600: setParam (p, "output_gain", -6.0f); break;
                    case 2000: p.loadModel (0, a1); break;                                  // switch the capture back
                    case 1150: p.loadModel (1, presets::builtInCapture (1)); break;         // Ember's own set back in amp 2 (not running)
                    case 3212: p.loadYourCapture (small); break;                            // a capture of your own: loads into amp 9 and plays
                    case 3262: setParam (p, AmpSimProcessor::ampModelParamId, 6.0f); break; // Forge from the shelf, its set never run yet
                    case 3290: setParam (p, AmpSimProcessor::ampModelParamId, 8.0f); break; // back to your capture
                    case 3700: setParam (p, AmpSimProcessor::ampModelParamId, 0.0f); p.clearModel (8); break; // Glass, and your capture removed
                    // Tone match: the DI recorder records from block 400 to block 2609 (the page's Record and Stop).
                    case 400:  p.getDiRecorder().start(); break;
                    case 2610: p.getDiRecorder().stop(); break;
                    default: break;
                }

                switch (blocks)
                {
                    case 3050: preview.setMaterial (material (220.0)); preview.setLoop (0, 0, 30000); preview.setLoop (1, 0, 40000); break;
                    case 3060: preview.setSource (0); preview.setPlaying (true); break;
                    case 3120: preview.setSource (1); break;
                    case 3180: preview.setSource (2); break;
                    case 3205: preview.setSource (ampsim::PreviewPlayer::sourceTargetRaw); break; // the raw target
                    case 3225: preview.setSource (0); break;                                     // and back, same clock
                    case 3240: preview.setLoop (0, 12000, 40000); preview.setLoop (1, 6000, 30000); break;
                    case 3300: preview.setMaterial (material (330.0)); break;                // swapped in mid-play
                    case 3360: preview.setSource (0); preview.setLevelDb (-6.0f); preview.setSourceGainDb (0, -3.0f); break;
                    case 3400: preview.setMuteLive (false); break;
                    case 3460: preview.setPlaying (false); break;
                    // The Target card's Play: two clicks at 240 BPM, then the section looped; to the guitar and back; stopped.
                    case 3480:
                        preview.setMaterial (songMaterial());
                        preview.setLoop (2, 0, 72000);
                        preview.setSource (3);
                        preview.setOnce (false);
                        preview.setMuteLive (false);
                        preview.setLevelDb (-3.0f);
                        preview.setCountIn (2, 12000.0);
                        preview.startFresh();
                        break;
                    case 3600: preview.setSource (4); break;
                    case 3640: preview.setSource (3); break;
                    case 3660: preview.setClickLevelDb (-12.0f); preview.startFresh(); break; // from the top again, with its count-in
                    case 3760: preview.setPlaying (false); break;
                    // A take: Record with a target loaded (a 3 s section, 2 clicks at 240 BPM).
                    case 3800:
                        session.setTargetSignal (sine (220.0, 0.1, (int) (4.0 * fs)), "rt song");
                        session.setRange (0.5, 3.5); // 3 s, the shortest section
                        session.setCountInForTake (true);
                        session.setCountInBeats (2);
                        session.setCountInBpm (240.0);
                        expect (session.startPlayAlong());
                        break;
                    case 4300: session.setSongLevelDb (-9.0f); session.setClickLevelDb (-3.0f); break; // a level moved mid-take
                    default: break;
                }
                if (blocks == 2611)
                    firstRecording = p.getDiRecorder().getRecording();

                // The match curve: a curve set (tone match's Apply) and switched on, its amount dragged (the timer
                // rebuilds the FIR), a new curve swapped in while it plays, off and on, amount 0 and back, the curve
                // cleared mid-stream and set again.
                switch (blocks)
                {
                    case 500: p.setMatchCurve (curveA); setParam (p, "match_curve_on", 1.0f); break;
                    case 760: p.setMatchCurve (curveB); break;
                    case 820: setParam (p, "match_curve_on", 0.0f); break;
                    case 850: setParam (p, "match_curve_on", 1.0f); break;
                    case 900: setParam (p, "match_curve_amount", 0.0f); break;
                    case 940: setParam (p, "match_curve_amount", 80.0f); break;
                    case 4500: p.clearMatchCurve(); break;
                    case 4600: p.setMatchCurve (curveA); break;
                    default: break;
                }
                if (blocks >= 520 && blocks < 700)
                    setParam (p, "match_curve_amount", 100.0f - 70.0f * (float) (blocks - 520) / 180.0f);

                // The Gain knobs (BUILD_PLAN "Amp gain"): a drag across every step of slot 3's gain set (Monolith),
                // a jump between steps as a scene makes, a drag down Ember's set, and a drag on slot 1's single
                // capture (its loudness-compensated trim, per sample while it moves).
                using GainKnob = ampsim::AmpSection::GainKnob;
                if (blocks >= 2000 && blocks < 2400)
                    setParam (p, AmpSimProcessor::ampParamId (2, "input_trim"), GainKnob::dbForPosition (10.0f * (float) (blocks - 2000) / 400.0f));
                if (blocks == 2450)
                    setParam (p, AmpSimProcessor::ampParamId (2, "input_trim"), GainKnob::dbForPosition (6.25f));
                if (blocks >= 2500 && blocks < 2700)
                    setParam (p, AmpSimProcessor::ampParamId (1, "input_trim"), GainKnob::dbForPosition (10.0f - 9.0f * (float) (blocks - 2500) / 200.0f));
                if (blocks >= 3000 && blocks < 3200)
                    setParam (p, AmpSimProcessor::ampParamId (0, "input_trim"), GainKnob::dbForPosition (5.0f + 4.0f * (float) (blocks - 3000) / 200.0f));

                // Only the selected amp runs (BUILD_PLAN "Amp switching"): amp 3 is selected from the footswitch for its
                // drag and its jump between steps; then the footswitch flips between amps 2 and 3 (both between steps,
                // Ember while it's dragged) and out to Comet, Lantern and Quartz every 8 to 40 buffers, mostly inside an
                // incoming amp's 85 ms warm-up (a retarget) or its 20 ms fade (a redirect), so amps start, warm, fade,
                // hold their blends, and stop; and back to amp 1.
                for (const auto& [at, slot] : std::initializer_list<std::pair<int, int>> {
                         { 1985, 2 }, { 2460, 1 }, { 2470, 2 }, { 2478, 1 }, { 2490, 2 }, { 2530, 1 }, { 2545, 2 }, { 2560, 1 }, { 2600, 2 }, { 2612, 1 },
                         { 2620, 2 }, { 2660, 5 }, { 2668, 3 }, { 2700, 7 }, { 2735, 1 }, { 2780, 0 } })
                    if (blocks == at)
                    {
                        midi.addEvent (juce::MidiMessage::programChange (1, slot), 11);
                        ++setSlotSwitches;
                    }

                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);

                rtcheck::begin();
                p.processBlock (buffer, midi); // the audio thread's side, measured
                total += rtcheck::end();
                gainMovingBlocks += p.getChain().amp.amp (2).model.isGainMoving() || p.getChain().amp.amp (1).model.isGainMoving()
                                    || p.getChain().amp.amp (0).model.isGainMoving() ? 1 : 0;
                maxSetModels = std::max (maxSetModels, p.getChain().amp.amp (2).model.getRunningSteps());
                {
                    const auto& section = p.getChain().amp;
                    for (int s = 0; s < AmpSimProcessor::numAmps; ++s)
                    {
                        const auto& m = section.amp (s).model;
                        heldBlendBlocks += section.isRunning (s) && m.getBlend() == ampsim::NamAmp::Blend::hold && m.getRunningSteps() == 2 ? 1 : 0;
                    }
                    maxTotalModels = std::max (maxTotalModels, section.getRunningModels());
                    maxRunningAmps = std::max (maxRunningAmps, section.getRunningAmps());
                    warmingBlocks += section.isWarmingUp() ? 1 : 0;
                }
                previewBlocks += preview.isActive() ? 1 : 0;
                matchCurveBlocks += p.getChain().isFullyBypassed (ampsim::Chain::Slot::matchCurve) ? 0 : 1;
                countInBlocks += preview.getCountInRemaining() > 0 ? 1 : 0;
                takeBlocks += session.isPlayingAlong() ? 1 : 0;

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
                    if (blocks > 3800)
                        session.poll(); // the page's timer: drains the take, ends it
                }
            }

            expect (ampWasBypassed, "the amp bypass must have reached the audio thread");
            expect (sectionsWereBypassed, "the section switches must have reached the audio thread");
            expectGreaterThan (modelFadeBlocks, 16, "the model loads must have crossfaded during the measurement");
            expectGreaterThan (slotSwitchBlocks, 16, "the amp switches must have warmed and crossfaded during the measurement");
            expectGreaterThan (warmingBlocks, 100, "incoming amps must have warmed up during the measurement");
            expectGreaterThan (gainMovingBlocks, 250, "the Gain drags must have moved the captures during the measurement");
            expectGreaterThan (previewBlocks, 395, "the A/B player must have played during the measurement");
            expect (! preview.isActive(), "the A/B player must have stopped and gone idle");
            expectEquals (maxSetModels, ampsim::NamAmp::maxRunningSteps, "the gain set's sweep must have run a model warming ahead");
            expectEquals (setSlotSwitches, 16);
            expectGreaterThan (heldBlendBlocks, 10, "amps fading out must have held their blends during the measurement");
            expectLessOrEqual (maxRunningAmps, 3);
            expectLessOrEqual (maxTotalModels, 3 * ampsim::NamAmp::maxRunningSteps);
            expectEquals (p.getChain().amp.getSelectedAmp(), 0);
            const auto ember = p.parameters.state.getProperty (AmpSimProcessor::modelPathKey (1)).toString() == presets::builtInCapture (1).getFullPathName();
            expect (ember, "amp 2 must be back on its built-in capture");
            expect (p.getStatus().model[8] == "Empty", "your capture must have been removed");
            expect (p.getStatus().cab[0].contains ("Modern 4x12, dynamic, 75 W, var. 3"), p.getStatus().cab[0]); // the built-in IR replaced rt_ir_b
            expectEquals (p.getCalibrationReloadCount(), 1, "the calibration change must have reloaded the captures during the measurement");
            expectEquals (p.getChain().gateA.getLearnCount(), 2, "both gate Learns must have finished during the measurement");
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
            // The recording is exactly the DI the chain took in blocks 400 to 2609, with nothing dropped.
            const auto& recording = firstRecording;
            const auto recordedFrom = (size_t) 400 * blockSize;
            const auto recordingExact = recording.size() == (size_t) 2210 * blockSize
                                        && std::equal (recording.begin(), recording.end(), input.begin() + (std::ptrdiff_t) recordedFrom);
            expect (recordingExact, "the DI recording must be the input of blocks 400 to 2609, sample for sample (got "
                                        + juce::String ((int) recording.size()) + " samples)");
            expectEquals ((juce::int64) p.getDiRecorder().takeDroppedCount(), (juce::int64) 0);
            // The take: complete, the section plus the round trip long, a run of the input from the song's first sample.
            const auto& take = session.getTake();
            expect (take.valid && take.complete, "the play-along take must have finished by itself during the measurement");
            expectEquals ((juce::int64) take.raw.size(), (juce::int64) (3 * fs + 340));
            const auto takeAt = take.raw.size() < 64 ? input.end() : std::search (input.begin(), input.end(), take.raw.begin(), take.raw.begin() + 64);
            const auto takeExact = takeAt != input.end() && (size_t) (input.end() - takeAt) >= take.raw.size() && std::equal (take.raw.begin(), take.raw.end(), takeAt);
            expect (takeExact, "the take must be the input from one sample on, in order");
            expectGreaterThan (countInBlocks, 150, "the count-ins must have played during the measurement");
            expect (! session.isPlayingAlong());
            for (int i = 0; i < 15000 && p.isLoading(); ++i) // the last design may still be on the loader, behind the calibration change's reload of all nine amps
                juce::Thread::sleep (2);
            const auto matchBuilds = p.getMatchCurveBuildCount() - matchBuildsBefore;
            expectGreaterThan (matchBuilds, 4, "the match curve must have been designed for its curves and amounts during the measurement");
            expectGreaterThan (matchCurveBlocks, 3000, "the match curve must have played during the measurement");
            expect (p.getChain().matchCurve.hasFir() && p.getChain().matchCurve.getCurve() == curveA, "the last curve must have reached the block");
            expectEquals (total.allocations, 0L);
            expectEquals (total.frees, 0L);
            expectEquals (total.blockingLocks, 0L);

            logMessage ("  -> " + juce::String (blocks) + " blocks (" + juce::String (blocks * blockSize / fs, 1)
                        + " s of audio), starting on the eight built-in amps: 5 capture loads, one of them into an amp that wasn't running, one Ember's own set put back, "
                        "and one a capture of your own (amp 9, selected, later removed) (" + juce::String (modelFadeBlocks) + " blocks mid-crossfade), "
                        "amp switches from the shelf and the footswitch, Forge's set run for the first time among them (" + juce::String (slotSwitchBlocks)
                        + " blocks warming up or crossfading), 3 IR loads into the three cab mics plus an IR swap and a built-in 1 s IR from the app's content folder, auto alignment, 6 cab mic changes, cuts on, off, re-sloped and swept, "
                        "a cab pack loaded into close mic 2 and dragged around (" + juce::String (morphs) + " re-morphs), cab bypass off and on, 5 knob ramps, "
                        "an interface-level change that recalibrated and reloaded every capture, "
                        "both compressors switched on (one to pedal mode, one to RMS), EQ sliders and bands moved, graphic -> parametric, "
                        "a band type change, a cut slope change, and both FX sections reordered, "
                        "the delay switched on, retimed by three footswitch taps, re-moded and re-laid-out with 105% feedback, then bypassed into spillover and back, "
                        "the chorus on with mode, shape, and sync changes, and the reverb on, re-engined twice, frozen and thawed from the footswitch, resized, "
                        "and bypassed into spillover; the amp bypassed and back (its captures running underneath), and both effect sections switched off and on; "
                        "tone match's DI recorder started and stopped mid-run (" + juce::String (p.getDiRecorder().recordedSeconds(), 2)
                        + " s recorded, sample-exact, nothing dropped); tone match's A/B player handed material, played for " + juce::String (previewBlocks)
                        + " blocks through its loops' wraps, switched source three times, its loops moved, new material swapped in mid-play, the live mute off, stopped; "
                        "play along: the song and its guitar with a two-click count-in, looped, switched to the guitar and back, restarted from the top with its count-in, stopped, "
                        "then a take through the session (" + juce::String (countInBlocks) + " blocks counting in across both, the take running for " + juce::String (takeBlocks)
                        + " blocks, a level moved mid-take, " + juce::String ((int) take.raw.size()) + " samples recorded from the song's first sample, ended by itself)");
            logMessage ("  -> the GUI hooks: input and output meters and the CPU meter every buffer, the analyzer tapping the post section, the pre section, "
                        "and the post section again; the GUI read " + juce::String (analyzed) + " samples from the ring and stopped for 400 buffers, "
                        "so the ring filled and the audio thread dropped " + juce::String (analyzerDropped) + " samples instead of waiting; CPU meter "
                        + juce::String (p.getCpuLoad(), 1) + "%");
            logMessage ("  -> amp switching: " + juce::String (setSlotSwitches) + " footswitch switches with the Gains between steps, 8 to 40 buffers apart (retargets "
                        "mid-warm-up and redirects mid-fade among them), " + juce::String (warmingBlocks) + " buffers with an incoming amp warming, "
                        + juce::String (heldBlendBlocks) + " amp-buffers holding a blend while fading out, at most " + juce::String (maxRunningAmps)
                        + " amps and " + juce::String (maxTotalModels) + " step models running in one buffer");
            logMessage ("  -> the match curve: two curves, swapped and cleared mid-stream, off and on, its amount dragged from 100 to 30% and to 0 and back ("
                        + juce::String (matchBuilds) + " FIR designs handed over, " + juce::String (matchCurveBlocks) + " blocks playing it)");
            logMessage ("  -> audio thread: " + describe (total));
        }
    }
};

RealtimeSafetyTests realtimeSafetyTests;
} // namespace

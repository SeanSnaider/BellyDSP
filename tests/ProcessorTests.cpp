#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"

namespace
{
using namespace testing;

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getParam (AmpSimProcessor& p, const juce::String& id)
{
    return p.parameters.getRawParameterValue (id)->load();
}

Stereo processAll (AmpSimProcessor& p, const std::vector<float>& input)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    Stereo out;
    out.left.resize (input.size());
    out.right.resize (input.size());

    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, out.left.begin() + (long) start);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize, out.right.begin() + (long) start);
    }

    return out;
}

juce::File writeSyntheticIR (const juce::String& name, int length)
{
    const auto file = tempDir().getChildFile (name + ".wav");
    writeWav (file, toBuffer (syntheticCabIR (length)));
    return file;
}

/// A brighter cab than the stock synthetic one, arriving 9 samples later, so the snapshot shows a real alignment.
std::vector<double> scaledBright()
{
    auto h = syntheticCabIR (2048, 8.0, 7000.0);
    h.insert (h.begin(), 9, 0.0);
    return h;
}

/// A cab pack folder: four captures on a 2 x 2 grid (dust cap and cone edge, 1 and 4 inches away).
juce::File writeTestPack (const juce::String& name)
{
    const auto folder = tempDir().getChildFile (name);
    folder.deleteRecursively();
    folder.createDirectory();
    writeWav (folder.getChildFile ("Cap_1in.wav"), toBuffer (syntheticCabIR (4096, 8.0, 7000.0)));
    writeWav (folder.getChildFile ("Edge_1in.wav"), toBuffer (syntheticCabIR (4096, 0.0, 3500.0)));
    writeWav (folder.getChildFile ("Cap_4in.wav"), toBuffer (syntheticCabIR (4096, 6.0, 6000.0)));
    writeWav (folder.getChildFile ("Edge_4in.wav"), toBuffer (syntheticCabIR (4096, -2.0, 3000.0)));
    return folder;
}

/// Whether an effect order begins with these blocks (later phases append more blocks to each section).
bool startsWith (const juce::StringArray& order, const juce::StringArray& prefix)
{
    if (order.size() < prefix.size())
        return false;
    for (int i = 0; i < prefix.size(); ++i)
        if (order[i] != prefix[i])
            return false;
    return true;
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

class ProcessorTests final : public juce::UnitTest
{
public:
    ProcessorTests() : juce::UnitTest ("Plugin processor and editor", "ampsim") {}

    void runTest() override
    {
        const auto a1 = exampleModel ("wavenet_a1_standard.nam");
        const auto irFile = writeSyntheticIR ("synthetic_4x12", 4096);

        beginTest ("bus layout: mono guitar in, stereo out, nothing else");
        {
            AmpSimProcessor p;
            const auto layout = [] (juce::AudioChannelSet in, juce::AudioChannelSet out)
            {
                juce::AudioProcessor::BusesLayout l;
                l.inputBuses.add (in);
                l.outputBuses.add (out);
                return l;
            };

            using Set = juce::AudioChannelSet;
            expect (p.isBusesLayoutSupported (layout (Set::mono(), Set::stereo())));
            expect (! p.isBusesLayoutSupported (layout (Set::stereo(), Set::stereo())));
            expect (! p.isBusesLayoutSupported (layout (Set::mono(), Set::mono())));
            expectEquals (p.getTotalNumInputChannels(), 1);
            expectEquals (p.getTotalNumOutputChannels(), 2);
            logMessage ("  -> 1 input channel (the guitar), 2 output channels; stereo-in and mono-out layouts rejected");
        }

        beginTest ("parameters have their permanent IDs, ranges, and defaults");
        {
            AmpSimProcessor p;
            for (auto id : { "input_gain", "output_gain" })
            {
                auto* param = dynamic_cast<juce::AudioParameterFloat*> (p.parameters.getParameter (id));
                expect (param != nullptr, id);
                expectEquals (param->range.start, -24.0f);
                expectEquals (param->range.end, 24.0f);
                // The 0.1 dB snapping is done in float, so the default lands within ~4e-7 dB of 0.
                expectWithinAbsoluteError (param->get(), 0.0f, 1.0e-5f);
            }

            auto* bypass = dynamic_cast<juce::AudioParameterBool*> (p.parameters.getParameter ("cab_bypass"));
            expect (bypass != nullptr && ! bypass->get());
            logMessage ("  -> input_gain and output_gain: -24 to +24 dB, default 0 dB; cab_bypass: default off");
        }

        beginTest ("the gain knobs change the level by exactly their dB values");
        {
            AmpSimProcessor p; // no model or IR: the chain is just the two gain stages
            p.prepareToPlay (fs, blockSize);
            setParam (p, "input_gain", 6.0f);
            setParam (p, "output_gain", -10.0f);

            const auto input = sine (440.0, 0.1, 48000);
            const auto out = processAll (p, input);
            const auto settledFrom = (size_t) (0.5 * fs);
            const auto change = toDb (rms (out.left.data() + settledFrom, input.size() - settledFrom)
                                      / rms (input.data() + settledFrom, input.size() - settledFrom));

            expectWithinAbsoluteError (change, -4.0, 0.01);
            expectEquals (maxAbsDifference (out.left, out.right), 0.0);
            logMessage ("  -> input +6 dB and output -10 dB: measured " + juce::String (change, 3) + " dB (expected -4.000); left == right");
        }

        beginTest ("refuses to run at 44.1 kHz and says why");
        {
            AmpSimProcessor p;
            p.prepareToPlay (44100.0, blockSize);
            const auto out = processAll (p, sine (440.0, 0.5, 4096));
            const auto warning = p.getStatus().warning;

            expectEquals (rms (out.left) + rms (out.right), 0.0);
            expect (warning.contains ("44100") && warning.contains ("48000"));
            logMessage ("  -> output is silent; warning shown: \"" + warning + "\"");
        }

        beginTest ("saving and restoring state brings back the knobs, the model, and the IR, and sounds identical");
        {
            AmpSimProcessor original;
            setParam (original, "input_gain", 3.5f);
            setParam (original, "output_gain", -2.0f);
            original.loadModel (0, a1);
            original.loadCabIR (0, irFile);
            waitForLoads (original);

            juce::MemoryBlock state;
            original.getStateInformation (state);

            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            waitForLoads (restored);

            for (auto id : { "input_gain", "output_gain", "cab_bypass" })
                expectEquals (getParam (restored, id), getParam (original, id), id);

            expectEquals (restored.getStatus().model[0], original.getStatus().model[0]);
            expectEquals (restored.getStatus().cab[0], original.getStatus().cab[0]);

            original.prepareToPlay (fs, blockSize);
            restored.prepareToPlay (fs, blockSize);
            const auto input = guitarDI ((int) (2.0 * fs));
            const auto a = processAll (original, input);
            const auto b = processAll (restored, input);
            const auto difference = maxAbsDifference (a.left, b.left);

            expectEquals (difference, 0.0);
            logMessage ("  -> state is " + juce::String ((int) state.getSize()) + " bytes; restored: input_gain "
                        + juce::String (getParam (restored, "input_gain"), 1) + " dB, output_gain "
                        + juce::String (getParam (restored, "output_gain"), 1) + " dB, model \"" + restored.getStatus().model[0]
                        + "\", cab \"" + restored.getStatus().cab[0] + "\"; 2 s through both: max difference " + juce::String (difference));
        }

        beginTest ("a cab pack and the mic's position survive saving and restoring, and sound identical");
        {
            const auto packFolder = writeTestPack ("state_pack");
            AmpSimProcessor original;
            setParam (original, AmpSimProcessor::cabParamId (0, "pos_x"), 0.3f);
            setParam (original, AmpSimProcessor::cabParamId (0, "pos_y"), 0.7f);
            original.loadCabIR (0, packFolder);
            waitForLoads (original);

            juce::MemoryBlock state;
            original.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            waitForLoads (restored);

            expect (original.getStatus().cab[0].contains ("state_pack (4 IRs on a grid), placed by file names; mic at 0.30, 0.70"), original.getStatus().cab[0]);
            expectEquals (restored.getStatus().cab[0], original.getStatus().cab[0]);
            expectEquals ((int) restored.getCabPackPoints (0).size(), 4);
            expectEquals (getParam (restored, AmpSimProcessor::cabParamId (0, "pos_x")), 0.3f);

            original.prepareToPlay (fs, blockSize);
            restored.prepareToPlay (fs, blockSize);
            const auto input = guitarDI ((int) fs);
            const auto difference = maxAbsDifference (processAll (original, input).left, processAll (restored, input).left);
            expectEquals (difference, 0.0);
            logMessage ("  -> restored: \"" + restored.getStatus().cab[0] + "\" with " + juce::String ((int) restored.getCabPackPoints (0).size())
                        + " pack positions; 1 s through both: max difference " + juce::String (difference));
        }

        beginTest ("changing the interface level reloads the captures once, after the setting settles");
        {
            AmpSimProcessor p;
            p.loadModel (0, exampleModel ("lstm.nam"));
            p.loadModel (1, a1); // no recorded input level
            waitForLoads (p);
            const auto before = p.getStatus().model[0];
            expect (before.contains ("input calibrated -6.3 dB"), before);
            expect (! p.getStatus().model[1].contains ("calibrated"));

            // Drag the setting from +12 to +18.3 dBu over 600 ms, as a mouse would.
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (auto now = t0; now - t0 < 600.0; now = juce::Time::getMillisecondCounterHiRes())
            {
                setParam (p, "input_level_dbu", (float) (12.0 + 6.3 * (now - t0) / 600.0));
                p.runHousekeeping();
                juce::Thread::sleep (10);
            }
            setParam (p, "input_level_dbu", 18.3f);
            const auto reloadsWhileDragging = p.getCalibrationReloadCount();

            for (int i = 0; i < 50; ++i)
            {
                p.runHousekeeping();
                juce::Thread::sleep (10);
            }
            waitForLoads (p);
            const auto after = p.getStatus().model[0];

            expectEquals (reloadsWhileDragging, 0);
            expectEquals (p.getCalibrationReloadCount(), 1);
            expect (after.contains ("input calibrated 0.0 dB"), after);

            setParam (p, "input_calibrate", 0.0f);
            for (int i = 0; i < 40; ++i)
            {
                p.runHousekeeping();
                juce::Thread::sleep (10);
            }
            waitForLoads (p);
            expect (! p.getStatus().model[0].contains ("calibrated"));
            expectEquals (p.getCalibrationReloadCount(), 2);

            logMessage ("  -> before: \"" + before + "\"");
            logMessage ("  -> dragged to +18.3 dBu over 600 ms: " + juce::String (reloadsWhileDragging) + " reloads during the drag, 1 after it "
                        "settled: \"" + after + "\"");
            logMessage ("  -> calibration off: \"" + p.getStatus().model[0] + "\"");
        }

        beginTest ("tap tempo from the footswitch sets the tempo, and a synced delay follows it to the sample");
        {
            AmpSimProcessor p;
            setParam (p, "delay_on", 1.0f);
            setParam (p, "delay_note", 6.0f); // 1/4
            setParam (p, "delay_mix", 100.0f);
            setParam (p, "delay_feedback", 0.0f);
            setParam (p, "delay_lowcut", 20.0f);
            setParam (p, "delay_highcut", 20000.0f);
            setParam (p, "delay_duck", 0.0f);
            p.prepareToPlay (fs, blockSize);

            // Four presses of the tap CC (80), 0.6 s apart (100 BPM), at exact sample positions.
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            const std::vector<juce::int64> taps { 1000, 1000 + 28800, 1000 + 57600, 1000 + 86400 };
            for (juce::int64 start = 0; start < 100000; start += blockSize)
            {
                midi.clear();
                for (auto t : taps)
                    if (t >= start && t < start + blockSize)
                        midi.addEvent (juce::MidiMessage::controllerEvent (1, 80, 127), (int) (t - start));
                buffer.clear();
                p.processBlock (buffer, midi);
            }
            const auto tapped = p.getTempo();
            p.runHousekeeping(); // the timer writes it into the tempo knob
            expectWithinAbsoluteError (tapped, 100.0, 1.0e-6);
            expectWithinAbsoluteError (getParam (p, "tempo_bpm"), 100.0f, 0.05f);

            // A quarter note at 100 BPM: an impulse's echo 600 ms later.
            std::vector<float> x ((size_t) (0.8 * fs), 0.0f);
            x[0] = 0.5f;
            const auto out = processAll (p, x);
            size_t peak = 1;
            for (size_t n = 1; n < out.left.size(); ++n)
                if (std::abs (out.left[n]) > std::abs (out.left[peak]))
                    peak = n;
            expectEquals ((int) peak, 28800);

            // The GUI's button, after a pause long enough to start a new tempo: taps every 188 buffers
            // (0.5013 s, since GUI taps are timed to the start of a buffer).
            for (int b = 0; b < 940; ++b) // 2.5 s
            {
                buffer.clear();
                midi.clear();
                p.processBlock (buffer, midi);
            }
            for (int i = 0; i < 4; ++i)
            {
                p.tapTempo();
                for (int b = 0; b < 188; ++b) // 188 buffers of 128 = 0.501 s
                {
                    buffer.clear();
                    midi.clear();
                    p.processBlock (buffer, midi);
                }
            }
            expectWithinAbsoluteError (p.getTempo(), 60.0 * fs / (188.0 * blockSize), 0.01);
            logMessage ("  -> 4 footswitch taps 0.6 s apart: " + juce::String (tapped, 3) + " BPM, tempo knob " + juce::String (getParam (p, "tempo_bpm"), 1)
                        + "; a synced quarter-note delay echoes at sample " + juce::String ((int) peak) + " (600.000 ms); 4 GUI taps every 188 buffers: "
                        + juce::String (p.getTempo(), 2) + " BPM");
        }

        beginTest ("a saved model that has gone missing is reported, not a crash");
        {
            AmpSimProcessor p;
            auto tree = p.parameters.copyState();
            tree.setProperty (AmpSimProcessor::modelPathKey (0), "/nonexistent/folder/gone.nam", nullptr);
            juce::MemoryBlock state;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), state);

            p.setStateInformation (state.getData(), (int) state.getSize());
            const auto status = p.getStatus();
            expect (status.modelError[0]);
            expect (status.model[0].contains ("missing"));
            logMessage ("  -> status: \"" + status.model[0] + "\"");
        }

        beginTest ("effects: compressors start off, EQs start on and flat, and their knobs reach the blocks");
        {
            AmpSimProcessor p;
            for (auto [id, expected] : std::initializer_list<std::pair<const char*, float>> {
                     { "comp_pre_on", 0.0f }, { "comp_post_on", 0.0f }, { "eq_pre_on", 1.0f }, { "eq_post_on", 1.0f },
                     { "comp_pre_threshold", -24.0f }, { "comp_pre_ratio", 4.0f }, { "comp_pre_attack", 8.0f }, { "comp_pre_mix", 70.0f },
                     { "eq_post_g5", 0.0f }, { "eq_pre_b1_freq", 100.0f }, { "eq_post_lowcut_freq", 80.0f } })
                expectWithinAbsoluteError (getParam (p, id), expected, 1.0e-3f, id);

            // Defaults pass the signal through untouched (flat EQs are bit-transparent, compressors off).
            p.prepareToPlay (fs, blockSize);
            const auto di = guitarDI ((int) fs);
            const auto untouched = processAll (p, di);
            expectEquals (maxAbsDifference (untouched.left, di), 0.0);

            // The post EQ's 1 kHz slider at +6 dB lifts a 1 kHz tone by about 6 dB.
            setParam (p, "eq_post_g5", 6.0f);
            const auto tone = sine (1000.0, 0.1, (int) fs);
            const auto lifted = processAll (p, tone);
            const auto lift = toDb (rms (lifted.left.data() + 24000, 20000) / rms (tone.data() + 24000, 20000));
            expectWithinAbsoluteError (lift, 6.0, 0.4);

            // The pre compressor, switched on, takes gain off a hot signal (its meter says how much).
            setParam (p, "eq_post_g5", 0.0f);
            setParam (p, "comp_pre_on", 1.0f);
            setParam (p, "comp_pre_auto_makeup", 0.0f);
            setParam (p, "comp_pre_mix", 100.0f);
            const auto hot = sine (500.0, 0.5, (int) fs);
            const auto squashed = processAll (p, hot);
            const auto change = toDb (rms (squashed.left.data() + 24000, 20000) / rms (hot.data() + 24000, 20000));
            expectLessThan (change, -6.0);
            expectGreaterThan (p.getCompressorReduction (false), 6.0f);
            logMessage ("  -> defaults leave the guitar untouched (bit for bit); post EQ 1 kHz slider +6 dB: a 1 kHz tone comes up "
                        + juce::String (lift, 2) + " dB; pre comp on (-24 dB, 4:1, no makeup): a -6 dBFS tone comes down " + juce::String (-change, 1)
                        + " dB, meter " + juce::String (p.getCompressorReduction (false), 1) + " dB");
        }

        beginTest ("the effect order is saved by block name and restored, and odd saved orders are repaired");
        {
            using Section = ampsim::Chain::Section;
            AmpSimProcessor p;
            expect (p.getSectionOrder (Section::pre) == juce::StringArray { "comp", "eq" });
            expect (startsWith (p.getSectionOrder (Section::post), { "eq", "comp" }));
            p.setSectionOrder (Section::pre, { "eq", "comp" });
            p.setSectionOrder (Section::post, { "comp", "eq" });

            juce::MemoryBlock state;
            p.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expect (restored.getSectionOrder (Section::pre) == juce::StringArray { "eq", "comp" });
            expect (startsWith (restored.getSectionOrder (Section::post), { "comp", "eq" }));
            expect (restored.getSectionOrder (Section::post) == p.getSectionOrder (Section::post));

            // A name from the future, a repeat, and a missing block: unknown and repeated names are
            // skipped, and the missing block keeps its default place at the end.
            AmpSimProcessor odd;
            odd.setSectionOrder (Section::pre, { "harmonizer", "eq", "eq" });
            expect (odd.getSectionOrder (Section::pre) == juce::StringArray { "eq", "comp" });

            // A state saved before effects existed loads with the default order.
            AmpSimProcessor legacy;
            auto tree = legacy.parameters.copyState();
            tree.removeProperty (AmpSimProcessor::orderKey (Section::pre), nullptr);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
            legacy.setStateInformation (old.getData(), (int) old.getSize());
            expect (legacy.getSectionOrder (Section::pre) == juce::StringArray { "comp", "eq" });

            logMessage ("  -> saved \"" + p.parameters.state.getProperty (AmpSimProcessor::orderKey (Section::pre)).toString() + "\" / \""
                        + p.parameters.state.getProperty (AmpSimProcessor::orderKey (Section::post)).toString() + "\", restored the same; "
                        "\"harmonizer, eq, eq\" becomes \"" + odd.getSectionOrder (Section::pre).joinIntoString (", ")
                        + "\"; a state without an order gets the default");
        }

        beginTest ("the editor draws (snapshots saved as proof)");
        {
            AmpSimProcessor p;
            p.loadModel (0, a1);
            p.loadModel (1, exampleModel ("lstm.nam"));
            p.loadCabIR (0, irFile);
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);

            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            const auto file = proofDir().getChildFile ("editor.png");
            expect (savePng (image, file));
            expectEquals (image.getWidth(), editor->getWidth() * 2);

            // The Cab tab, with IRs in all three mics.
            const auto room = tempDir().getChildFile ("snapshot_room.wav");
            {
                juce::AudioBuffer<float> stereoRoom (2, 24000);
                juce::Random random (9);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < stereoRoom.getNumSamples(); ++i)
                        stereoRoom.setSample (ch, i, (2.0f * random.nextFloat() - 1.0f) * std::exp (-(float) i / 4800.0f));
                writeWav (room, stereoRoom);
            }
            const auto mic2 = tempDir().getChildFile ("synthetic_bright.wav");
            {
                auto bright = scaledBright();
                writeWav (mic2, toBuffer (bright));
            }
            p.loadCabIR (1, mic2);
            p.loadCabIR (AmpSimProcessor::roomMic, room);
            waitForLoads (p);
            auto* ampSimEditor = dynamic_cast<AmpSimEditor*> (editor.get());
            expect (ampSimEditor != nullptr);
            ampSimEditor->showTab (1);
            ampSimEditor->resized();
            ampSimEditor->refresh(); // what the status timer would do
            const auto cabFile = proofDir().getChildFile ("editor_cab.png");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), cabFile));
            logMessage ("  -> " + cabFile.getFullPathName());

            // A cab pack in close mic 1, the mic moved between captures: the position pad.
            setParam (p, AmpSimProcessor::cabParamId (0, "pos_x"), 0.62f);
            setParam (p, AmpSimProcessor::cabParamId (0, "pos_y"), 0.35f);
            p.loadCabIR (0, writeTestPack ("snapshot_pack"));
            waitForLoads (p);
            ampSimEditor->refresh();
            const auto packFile = proofDir().getChildFile ("editor_cab_pack.png");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), packFile));
            logMessage ("  -> " + packFile.getFullPathName());

            // Pre FX with the compressor working (its meter showing), the order swapped, and a graphic
            // EQ curve; Post FX in parametric mode with cuts.
            setParam (p, "comp_pre_on", 1.0f);
            setParam (p, "comp_pre_threshold", -36.0f);
            p.setSectionOrder (ampsim::Chain::Section::pre, { "eq", "comp" });
            for (int m = 0; m < 9; ++m)
                setParam (p, "eq_pre_g" + juce::String (m + 1), (float) std::sin (m * 0.9) * 9.0f);
            setParam (p, "eq_post_mode", 1.0f);
            setParam (p, "eq_post_b2_gain", -6.0f);
            setParam (p, "eq_post_b3_gain", 4.5f);
            setParam (p, "eq_post_b5_gain", 3.0f);
            setParam (p, "eq_post_lowcut_on", 1.0f);
            setParam (p, "eq_post_highcut_on", 1.0f);
            setParam (p, "eq_post_highcut_slope", 2.0f);
            processAll (p, guitarDI ((int) fs));
            setParam (p, "delay_on", 1.0f);
            setParam (p, "delay_stereo", 2.0f);
            for (auto [tab, name] : std::initializer_list<std::pair<int, const char*>> { { 2, "editor_prefx.png" }, { 3, "editor_postfx.png" }, { 4, "editor_timefx.png" } })
            {
                ampSimEditor->showTab (tab);
                ampSimEditor->resized();
                ampSimEditor->refresh();
                juce::Thread::sleep (60); // the EQ curves redraw on their own 30 Hz timers...
                for (auto* child : ampSimEditor->getChildren())
                    child->repaint();
                const auto fxFile = proofDir().getChildFile (name);
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), fxFile));
                logMessage ("  -> " + fxFile.getFullPathName());
            }

            AmpSimProcessor wrongRate;
            wrongRate.prepareToPlay (44100.0, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> warningEditor (wrongRate.createEditor());
            const auto warningFile = proofDir().getChildFile ("editor_44k_warning.png");
            expect (savePng (warningEditor->createComponentSnapshot (warningEditor->getLocalBounds(), true, 2.0f), warningFile));

            logMessage ("  -> " + file.getFullPathName() + " (" + juce::String (image.getWidth()) + "x" + juce::String (image.getHeight()) + ")");
            logMessage ("  -> " + warningFile.getFullPathName());
        }

        beginTest ("listening renders: the synthetic guitar DI through the full chain");
        {
            const auto input = guitarDI ((int) (4.0 * fs));
            writeWav (proofDir().getChildFile ("render_0_synthetic_guitar_di.wav"), input);

            for (auto modelName : { "wavenet_a1_standard", "lstm" })
            {
                AmpSimProcessor p;
                p.loadModel (0, exampleModel (juce::String (modelName) + ".nam"));
                p.loadCabIR (0, irFile);
                waitForLoads (p);
                p.prepareToPlay (fs, blockSize);

                const auto out = processAll (p, input);
                juce::AudioBuffer<float> stereo (2, (int) out.left.size());
                stereo.copyFrom (0, 0, out.left.data(), (int) out.left.size());
                stereo.copyFrom (1, 0, out.right.data(), (int) out.right.size());

                const auto file = proofDir().getChildFile ("render_" + juce::String (modelName) + "_synthetic_cab.wav");
                expect (writeWav (file, stereo));
                logMessage ("  -> " + file.getFileName() + ": peak " + juce::String (toDb (stereo.getMagnitude (0, stereo.getNumSamples())), 1)
                            + " dBFS, RMS " + juce::String (toDb (rms (out.left)), 1) + " dBFS");
            }
        }
    }
};

ProcessorTests processorTests;
} // namespace

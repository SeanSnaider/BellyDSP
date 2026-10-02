#include "PluginEditor.h"
#include "BlockParameters.h"
#include "MidiMap.h"
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

        beginTest ("presets: saving and loading brings back the whole sound, bit for bit");
        {
            AmpSimProcessor a;
            for (auto [id, value] : std::initializer_list<std::pair<const char*, float>> {
                     { "input_gain", 1.5f }, { "amp_slot", 1.0f }, { "amp2_mid", 3.0f }, { "cab_mic1_pan", 0.3f }, { "cab_lowcut_on", 1.0f },
                     { "comp_pre_on", 1.0f }, { "comp_pre_threshold", -32.0f }, { "eq_pre_g3", 4.5f }, { "eq_post_mode", 1.0f },
                     { "eq_post_b2_type", 3.0f }, { "delay_on", 1.0f }, { "delay_mode", 1.0f }, { "delay_feedback", 55.0f }, { "tempo_bpm", 104.0f } })
                setParam (a, id, value);
            a.loadModel (0, a1);
            a.loadModel (1, exampleModel ("lstm.nam"));
            a.loadCabIR (0, irFile);
            a.loadCabIR (1, writeTestPack ("preset_pack"));
            a.setSectionOrder (ampsim::Chain::Section::post, { "delay", "eq", "comp" });
            waitForLoads (a);

            const auto file = tempDir().getChildFile ("round_trip.json");
            expect (presets::save (a.capturePreset ("Round trip"), file));
            juce::String error;
            const auto loaded = presets::load (file, error);
            expect (error.isEmpty(), error);

            AmpSimProcessor b;
            setParam (b, "delay_feedback", 90.0f); // anything set before must be overwritten
            setParam (b, "output_gain", -12.0f);
            expect (b.loadPreset (loaded).ok);
            waitForLoads (b);
            for (int i = 0; i < 50 && b.isChangingPreset(); ++i)
            {
                juce::Thread::sleep (10);
                b.runHousekeeping();
            }
            expect (! b.isChangingPreset());

            int compared = 0, mismatched = 0;
            for (auto* parameter : a.getParameters())
                if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                {
                    ++compared;
                    if (std::abs (ranged->getValue() - b.parameters.getParameter (ranged->paramID)->getValue()) > 1.0e-6f)
                        ++mismatched;
                }
            expectEquals (mismatched, 0);
            for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
                expectEquals (b.getStatus().model[(size_t) s], a.getStatus().model[(size_t) s]);
            for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
                expectEquals (b.getStatus().cab[(size_t) m], a.getStatus().cab[(size_t) m]);
            expect (b.getSectionOrder (ampsim::Chain::Section::post) == a.getSectionOrder (ampsim::Chain::Section::post));
            expectEquals (b.getPresetName(), juce::String ("Round trip"));

            a.prepareToPlay (fs, blockSize);
            b.prepareToPlay (fs, blockSize);
            const auto input = guitarDI ((int) (2.0 * fs));
            const auto difference = maxAbsDifference (processAll (a, input).left, processAll (b, input).left);
            expectEquals (difference, 0.0);
            logMessage ("  -> " + file.getFileName() + " (" + juce::String ((int) file.getSize()) + " bytes): " + juce::String (compared)
                        + " parameters, 2 captures, an IR and a cab pack, and the effect order all come back; 2 s through both: max difference "
                        + juce::String (difference));
        }

        beginTest ("presets: unmentioned parameters take their defaults, problems are reported, newer formats are refused, global settings stay put");
        {
            AmpSimProcessor p;
            setParam (p, "delay_feedback", 80.0f);
            setParam (p, "input_level_dbu", 18.0f); // global: a preset never touches it
            const auto sparse = juce::JSON::parse (R"({ "format_version": 1, "name": "Sparse", "parameters": { "delay_on": 1, "made_up_id": 3 },
                                                       "amps": [ "/nowhere/amp.nam", "", "" ], "cab": { "mic1": "/nowhere/cab.wav" }, "order": {} })");
            expect (p.loadPreset (sparse).ok);
            waitForLoads (p);
            expectWithinAbsoluteError (getParam (p, "delay_feedback"), 35.0f, 1.0e-3f);
            expectEquals (getParam (p, "delay_on"), 1.0f);
            expectWithinAbsoluteError (getParam (p, "input_level_dbu"), 18.0f, 1.0e-3f);
            const auto warnings = p.getPresetWarnings().joinIntoString ("; ");
            expect (warnings.contains ("made_up_id") && warnings.contains ("/nowhere/amp.nam") && warnings.contains ("/nowhere/cab.wav"), warnings);

            const auto future = p.loadPreset (juce::JSON::parse (R"({ "format_version": 2, "parameters": {} })"));
            const auto garbage = p.loadPreset (juce::var ("not a preset"));
            expect (! future.ok && future.error.contains ("newer"));
            expect (! garbage.ok);
            logMessage ("  -> a preset naming only delay_on: delay_feedback back to its default " + juce::String (getParam (p, "delay_feedback"), 1)
                        + "%, input level (global) still " + juce::String (getParam (p, "input_level_dbu"), 1) + " dBu");
            logMessage ("  -> warnings: " + warnings);
            logMessage ("  -> refused: \"" + future.error + "\"; \"" + garbage.error + "\"");
        }

        beginTest ("presets: the golden v1 file loads the same on every build");
        {
            AmpSimProcessor p;
            juce::String error;
            const auto golden = presets::load (juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/presets/golden_v1.json"), error);
            expect (error.isEmpty(), error);
            expect (p.loadPreset (golden).ok);
            waitForLoads (p);

            const std::vector<std::pair<const char*, float>> expected {
                { "input_gain", 2.5f }, { "output_gain", -3.0f }, { "amp_slot", 1.0f }, { "amp2_bass", 4.0f }, { "amp2_presence", -2.5f },
                { "cab_mic1_level", -3.0f }, { "cab_mic1_pan", -0.5f }, { "cab_align", 0.0f }, { "comp_pre_on", 1.0f }, { "comp_pre_mode", 1.0f },
                { "comp_pre_threshold", -30.0f }, { "comp_pre_ratio", 6.0f }, { "eq_post_mode", 1.0f }, { "eq_post_b3_freq", 2500.0f },
                { "eq_post_b3_gain", 3.5f }, { "delay_on", 1.0f }, { "delay_mode", 2.0f }, { "delay_note", 9.0f }, { "delay_feedback", 45.0f },
                { "tempo_bpm", 96.0f },
                // and some it doesn't mention, at their defaults
                { "comp_post_on", 0.0f }, { "eq_pre_on", 1.0f }, { "delay_mix", 25.0f }, { "amp1_bass", 0.0f } };
            int wrong = 0;
            for (const auto& [id, value] : expected)
                if (std::abs (getParam (p, id) - value) > 0.01f)
                {
                    ++wrong;
                    logMessage ("  !! " + juce::String (id) + " is " + juce::String (getParam (p, id)) + ", expected " + juce::String (value));
                }
            expectEquals (wrong, 0);
            expect (p.getSectionOrder (ampsim::Chain::Section::pre) == juce::StringArray { "gate", "eq", "comp" }); // saved before the gate existed
            expect (startsWith (p.getSectionOrder (ampsim::Chain::Section::post), { "delay", "eq", "comp" }));
            expect (p.getPresetWarnings().joinIntoString ("; ").contains ("some_future_parameter"));
            logMessage ("  -> tests/fixtures/presets/golden_v1.json: " + juce::String ((int) expected.size()) + " parameter values as expected, order \""
                        + p.getSectionOrder (ampsim::Chain::Section::pre).joinIntoString (", ") + "\" / \"" + p.getSectionOrder (ampsim::Chain::Section::post).joinIntoString (", ")
                        + "\", warning: " + p.getPresetWarnings().joinIntoString ("; "));
        }

        beginTest ("presets: loading one while playing fades out, swaps, and fades back in without a click");
        {
            AmpSimProcessor p;
            p.loadModel (0, a1);
            p.loadCabIR (0, irFile);
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            const auto other = juce::JSON::parse (R"({ "format_version": 1, "name": "Other", "parameters": { "amp_slot": 0, "output_gain": -4.0, "delay_on": 1 },
                                                     "amps": [ ")" + exampleModel ("lstm.nam").getFullPathName() + R"(", "", "" ], "cab": { "mic1": ")"
                                                     + writeSyntheticIR ("preset_other_ir", 2048).getFullPathName() + R"(" }, "order": {} })");

            const auto input = guitarDI ((int) (4.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            std::vector<float> out;
            size_t requestAt = 0, silentFrom = 0, silentTo = 0;
            for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
            {
                if (start == (size_t) (1.0 * fs) / blockSize * blockSize)
                {
                    requestAt = start;
                    expect (p.loadPreset (other).ok);
                }
                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                p.processBlock (buffer, midi);
                out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
                if (requestAt > 0 && silentFrom == 0 && buffer.getMagnitude (0, 0, blockSize) == 0.0f)
                    silentFrom = start;
                if (silentFrom > 0 && silentTo == 0 && buffer.getMagnitude (0, 0, blockSize) > 0.0f)
                    silentTo = start;
                if ((start / blockSize) % 4 == 0)
                {
                    juce::Thread::sleep (2); // let the loader work, as in real time
                    p.runHousekeeping();
                }
            }

            expect (silentFrom > requestAt && silentTo > silentFrom);
            expect (p.getStatus().model[0].startsWith ("lstm"));
            const auto steady = maxStep (out, (size_t) (0.5 * fs), requestAt);
            const auto fadeOut = maxStep (out, requestAt, silentFrom + blockSize);
            const auto fadeIn = maxStep (out, silentTo, silentTo + 2400);
            const auto after = maxStep (out, silentTo + 4800, out.size());
            expectLessThan (fadeOut, steady * 1.05);
            expectLessThan (fadeIn, std::max (steady, after) * 1.05);
            logMessage ("  -> preset requested at " + juce::String (1000.0 * (double) requestAt / fs, 0) + " ms: silent from "
                        + juce::String (1000.0 * (double) silentFrom / fs, 0) + " ms to " + juce::String (1000.0 * (double) silentTo / fs, 0)
                        + " ms while the new capture and IR load; largest step fading out " + juce::String (fadeOut, 4) + ", fading in "
                        + juce::String (fadeIn, 4) + " (steady playing " + juce::String (steady, 4) + " before, " + juce::String (after, 4) + " after)");
        }

        beginTest ("chorus and reverb: off by default, synced times follow the tempo, and the freeze footswitch acts at once");
        {
            AmpSimProcessor p;
            expectEquals (getParam (p, "chorus_on"), 0.0f);
            expectEquals (getParam (p, "reverb_on"), 0.0f);
            expect (p.getSectionOrder (ampsim::Chain::Section::post) == juce::StringArray { "eq", "comp", "chorus", "delay", "reverb" });

            // Synced: a quarter-note chorus cycle at 120 BPM is 2 Hz; a sixteenth-note pre-delay is 125 ms.
            setParam (p, "chorus_sync", 1.0f);
            setParam (p, "chorus_note", 6.0f); // 1/4
            setParam (p, "reverb_predelay_sync", 1.0f);
            setParam (p, "reverb_predelay_note", 12.0f); // 1/16
            params::ChorusParameters chorus;
            chorus.bind (p.parameters);
            expectWithinAbsoluteError ((double) chorus.read (120.0).rateHz, 2.0, 1.0e-5);
            setParam (p, "reverb_on", 1.0f);
            p.prepareToPlay (fs, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            const auto run = [&] (bool pressFreeze)
            {
                midi.clear();
                if (pressFreeze)
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 81, 127), 0);
                buffer.clear();
                p.processBlock (buffer, midi);
            };
            run (false);
            expectWithinAbsoluteError ((double) p.getChain().reverb.getSettings().preDelayMs, 125.0, 1.0e-3);

            // The footswitch's freeze: in effect in the same buffer, in the switch once the timer runs.
            run (true);
            const auto frozenAtOnce = p.getChain().reverb.getSettings().freeze;
            const auto switchBefore = getParam (p, "reverb_freeze");
            p.runHousekeeping();
            run (false);
            const auto switchAfter = getParam (p, "reverb_freeze");
            const auto stillFrozen = p.getChain().reverb.getSettings().freeze;
            run (true);
            const auto thawedAtOnce = ! p.getChain().reverb.getSettings().freeze;
            p.runHousekeeping();
            run (false);
            expect (frozenAtOnce && stillFrozen && thawedAtOnce);
            expectEquals (switchBefore, 0.0f);
            expectEquals (switchAfter, 1.0f);
            expectEquals (getParam (p, "reverb_freeze"), 0.0f);
            logMessage ("  -> chorus and reverb off by default; post order eq, comp, chorus, delay, reverb; a synced quarter-note chorus at 120 BPM runs at "
                        + juce::String (chorus.read (120.0).rateHz, 3) + " Hz, a synced sixteenth pre-delay is "
                        + juce::String (p.getChain().reverb.getSettings().preDelayMs, 1) + " ms");
            logMessage ("  -> freeze footswitch (CC 81): frozen in the same buffer, the switch catches up after the timer; a second press thaws at once");
        }

        beginTest ("MIDI mappings: toggles, momentary switches, and expression pedals, learned, saved, and in presets");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            juce::AudioBuffer<float> buffer (2, blockSize);
            const auto send = [&] (std::initializer_list<std::pair<int, int>> events)
            {
                juce::MidiBuffer midi;
                for (const auto& [cc, value] : events)
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, cc, value), 0);
                buffer.clear();
                p.processBlock (buffer, midi);
                p.runHousekeeping(); // the timer applies the mappings
            };

            auto& map = p.getMidiMap();
            map.set ({ 82, MidiMapping::Action::toggle, "delay_on" });
            map.set ({ 83, MidiMapping::Action::momentary, "reverb_freeze" });
            MidiMapping pedal;
            pedal.cc = 11;
            pedal.action = MidiMapping::Action::continuous;
            pedal.parameterId = "output_gain";
            pedal.minimum = -24.0f;
            pedal.maximum = 6.0f;
            map.set (pedal);

            send ({ { 82, 127 } });
            const auto afterPress = getParam (p, "delay_on");
            send ({ { 82, 0 } });
            const auto afterRelease = getParam (p, "delay_on");
            send ({ { 82, 127 } });
            const auto afterSecondPress = getParam (p, "delay_on");
            expect (afterPress == 1.0f && afterRelease == 1.0f && afterSecondPress == 0.0f);
            send ({ { 82, 127 } }); // a switch that sends 127 on every press and never a release
            const auto triggerOn = getParam (p, "delay_on");
            send ({ { 82, 127 } });
            expect (triggerOn == 1.0f && getParam (p, "delay_on") == 0.0f);

            send ({ { 83, 127 } });
            const auto held = getParam (p, "reverb_freeze");
            send ({ { 83, 0 } });
            expect (held == 1.0f && getParam (p, "reverb_freeze") == 0.0f);

            juce::StringArray sweep;
            for (int value : { 0, 64, 127 })
            {
                send ({ { 11, value } });
                sweep.add (juce::String (value) + " -> " + juce::String (getParam (p, "output_gain"), 1) + " dB");
            }
            expectWithinAbsoluteError (getParam (p, "output_gain"), 6.0f, 1.0e-3f);

            // Learn: the next controller maps to the parameter (the learning press itself changes nothing).
            p.midiLearn ("chorus_on");
            send ({ { 90, 127 }, { 90, 0 } });
            const auto learnedQuietly = getParam (p, "chorus_on") == 0.0f;
            send ({ { 90, 127 } });
            expect (learnedQuietly && getParam (p, "chorus_on") == 1.0f);
            p.midiLearn ("reverb_mix");
            send ({ { 91, 100 } });
            send ({ { 91, 127 } });
            expectWithinAbsoluteError (getParam (p, "reverb_mix"), 100.0f, 1.0e-3f);
            expectEquals ((int) map.getMappings().size(), 5);

            // Saved with the state, and carried by presets.
            juce::MemoryBlock state;
            p.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expectEquals ((int) restored.getMidiMap().getMappings().size(), 5);
            AmpSimProcessor fromPreset;
            expect (fromPreset.loadPreset (p.capturePreset ("With footswitch")).ok);
            expectEquals ((int) fromPreset.getMidiMap().getMappings().size(), 5);
            const auto old = juce::JSON::parse (R"({ "format_version": 1, "parameters": {} })"); // no "midi": keep the mappings
            expect (fromPreset.loadPreset (old).ok);
            expectEquals ((int) fromPreset.getMidiMap().getMappings().size(), 5);

            // A flood of controllers in one buffer: the FIFO fills and drops the rest, never blocking.
            juce::MidiBuffer flood;
            for (int i = 0; i < 300; ++i)
                flood.addEvent (juce::MidiMessage::controllerEvent (1, 20, i % 128), i % blockSize);
            buffer.clear();
            p.processBlock (buffer, flood);
            p.runHousekeeping();
            logMessage ("  -> toggle (CC 82): press on, release nothing, press off, and a switch sending only 127 flips every press; "
                        "momentary (CC 83): follows the switch; expression (CC 11 over -24..+6 dB): "
                        + sweep.joinIntoString (", "));
            logMessage ("  -> MIDI learn mapped CC 90 to chorus_on (a toggle) and CC 91 to reverb_mix (0-100%); 5 mappings survive saving the state "
                        "and loading a preset; a preset without mappings leaves them alone");
            logMessage ("  -> 300 controllers in one buffer: 255 queued, " + juce::String (300 - 255) + " dropped, the audio thread never waits");
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

        beginTest ("gates: linked, Gate B applies Gate A's decision after the amp; unlinked they're independent; Learn sets the thresholds");
        {
            // Three notes, each followed by 0.6 s of hiss alone (uniform, peak 0.0002: -79 dBFS RMS, a quiet
            // DI that the default threshold closes on).
            std::vector<float> input;
            for (int r = 0; r < 3; ++r)
            {
                const auto note = guitarDI ((int) (0.4 * fs));
                input.insert (input.end(), note.begin(), note.end());
                input.resize (input.size() + (size_t) (0.6 * fs), 0.0f);
            }
            const auto hiss = whiteNoise ((int) input.size(), 0.0002f, 7);
            for (size_t n = 0; n < input.size(); ++n)
                input[n] += hiss[n];

            // The reference: one lone gate with the same settings, detecting from the same DI.
            const auto referenceGain = [&] (const ampsim::Gate::Settings& settings)
            {
                ampsim::Gate gate;
                gate.prepare (fs, blockSize);
                std::vector<float> gain (input.size(), 1.0f), scratch ((size_t) blockSize);
                for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
                {
                    gate.setSettings (settings);
                    std::copy (input.begin() + (long) start, input.begin() + (long) start + blockSize, scratch.begin());
                    float* channels[] = { scratch.data() };
                    gate.process (juce::dsp::AudioBlock<float> (channels, 1, (size_t) blockSize), { input.data() + start, blockSize });
                    std::copy (gate.getGainCurve(), gate.getGainCurve() + blockSize, gain.begin() + (long) start);
                }
                return gain;
            };
            using Settings = std::initializer_list<std::pair<const char*, float>>;
            const auto run = [&] (Settings settings)
            {
                AmpSimProcessor p;
                for (const auto& [id, value] : settings)
                    setParam (p, id, value);
                p.prepareToPlay (fs, blockSize);
                return processAll (p, input).left;
            };
            // The reference reads its settings through the same parameters (skewed knobs round their
            // values a little, which shows once a gate starts closing).
            const auto settingsFor = [&] (Settings settings, const char* prefix)
            {
                AmpSimProcessor p;
                for (const auto& [id, value] : settings)
                    setParam (p, id, value);
                params::GateParameters gate;
                gate.bind (p.parameters, prefix);
                return gate.read();
            };
            // Error against x times the gain (twice over for gating at two points), multiplied in float as
            // the gates do, after the 10 ms switch-on fades.
            const auto errorAgainst = [&] (const std::vector<float>& out, const std::vector<float>& gain, int times)
            {
                double worst = 0.0;
                for (size_t n = 960; n < out.size() - blockSize; ++n)
                {
                    auto expected = input[n];
                    for (int t = 0; t < times; ++t)
                        expected *= gain[n];
                    worst = std::max (worst, std::abs ((double) out[n] - (double) expected));
                }
                return worst;
            };

            const auto gainA = referenceGain (settingsFor ({}, "gate_a"));
            const auto gainB = referenceGain (settingsFor ({ { "gate_b_threshold", -40.0f } }, "gate_b"));

            // Gate A off, Gate B on and linked (the default): A still decides.
            const auto followsA = errorAgainst (run ({ { "gate_b_on", 1.0f } }), gainA, 1);
            // Both on, linked: the same decision before and after the amp.
            const auto twice = errorAgainst (run ({ { "gate_a_on", 1.0f }, { "gate_b_on", 1.0f } }), gainA, 2);
            // Unlinked: Gate B on its own settings, Gate A off.
            const auto own = errorAgainst (run ({ { "gate_b_on", 1.0f }, { "gate_link", 0.0f }, { "gate_b_threshold", -40.0f } }), gainB, 1);
            expectEquals (followsA, 0.0);
            expectEquals (twice, 0.0);
            expectEquals (own, 0.0);

            double closedA = 1.0, closedB = 1.0;
            for (size_t n = (size_t) (0.7 * fs); n < (size_t) (0.95 * fs); ++n) // the first gap, after the release
            {
                closedA = std::min (closedA, (double) gainA[n]);
                closedB = std::min (closedB, (double) gainB[n]);
            }
            expectEquals (closedA, 0.0);
            expectEquals (closedB, 0.0);

            // Learn: 2 s of a louder hiss alone (peak 0.001, -65 dBFS RMS, where the default threshold
            // wouldn't close). Linked, only Gate A learns (Gate B uses its decision); with Gate B on and
            // unlinked, both do.
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            const auto quiet = whiteNoise ((int) (2.2 * fs), 0.001f, 8);
            setParam (p, "gate_b_on", 1.0f);
            p.learnGates();
            expect (p.isLearningGates());
            processAll (p, quiet);
            p.runHousekeeping();
            const auto learnedA = getParam (p, "gate_a_threshold");
            expect (! p.isLearningGates());
            expectWithinAbsoluteError (learnedA, p.getChain().gateA.gate.getLearnedThresholdDb(), 0.051f);
            expectEquals (getParam (p, "gate_b_threshold"), -55.0f);
            setParam (p, "gate_link", 0.0f);
            p.learnGates();
            processAll (p, quiet);
            p.runHousekeeping();
            expectWithinAbsoluteError (getParam (p, "gate_b_threshold"), p.getChain().gateB.gate.getLearnedThresholdDb(), 0.051f);
            expect (getParam (p, "gate_b_threshold") > -50.0f);
            expect (! p.isLearningGates());
            const auto noiseFloor = p.getChain().gateA.gate.getLearnedNoiseFloorDb();
            expect (noiseFloor > -66.0f && noiseFloor < -55.0f);

            logMessage ("  -> through the whole processor, bit for bit against a lone reference gate: linked Gate B with Gate A off applies Gate A's gain "
                        "(max difference " + juce::String (followsA) + "); both on and linked gate twice with one decision (" + juce::String (twice)
                        + "); unlinked, Gate B is its own gate at -40 dBFS (" + juce::String (own) + "); both mute the gaps in -79 dBFS RMS hiss "
                        "(min gain " + juce::String (closedA) + " and " + juce::String (closedB) + ")");
            logMessage ("  -> Learn on 2 s of -65 dBFS RMS hiss: noise floor (95th percentile of the 10 ms peak) " + juce::String (noiseFloor, 1)
                        + " dBFS, Gate A's threshold set to " + juce::String (learnedA, 1) + " dBFS (floor + 6 dB margin + 8 dB hysteresis); "
                        "Gate B learned only once unlinked: " + juce::String (getParam (p, "gate_b_threshold"), 1) + " dBFS");
        }

        beginTest ("the effect order is saved by block name and restored, and odd saved orders are repaired");
        {
            using Section = ampsim::Chain::Section;
            AmpSimProcessor p;
            expect (p.getSectionOrder (Section::pre) == juce::StringArray { "gate", "comp", "eq" });
            expect (startsWith (p.getSectionOrder (Section::post), { "eq", "comp" }));
            p.setSectionOrder (Section::pre, { "eq", "gate", "comp" });
            p.setSectionOrder (Section::post, { "comp", "eq" });
            expect (startsWith (p.getSectionOrder (Section::post), { "comp", "eq", "chorus" }));

            juce::MemoryBlock state;
            p.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expect (restored.getSectionOrder (Section::pre) == juce::StringArray { "eq", "gate", "comp" });
            expect (startsWith (restored.getSectionOrder (Section::post), { "comp", "eq" }));
            expect (restored.getSectionOrder (Section::post) == p.getSectionOrder (Section::post));

            // A name from the future, a repeat, and missing blocks: unknown and repeated names are
            // skipped, and a missing block goes where it breaks the fewest pairs of the default order.
            AmpSimProcessor odd;
            odd.setSectionOrder (Section::pre, { "harmonizer", "eq", "eq" });
            expect (odd.getSectionOrder (Section::pre) == juce::StringArray { "gate", "comp", "eq" });

            // An order saved before the gate existed (Phase 5): the gate goes first, the rest keep their order.
            AmpSimProcessor phase5;
            phase5.setSectionOrder (Section::pre, { "eq", "comp" });
            expect (phase5.getSectionOrder (Section::pre) == juce::StringArray { "gate", "eq", "comp" });

            // A state saved before effects existed loads with the default order.
            AmpSimProcessor legacy;
            auto tree = legacy.parameters.copyState();
            tree.removeProperty (AmpSimProcessor::orderKey (Section::pre), nullptr);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
            legacy.setStateInformation (old.getData(), (int) old.getSize());
            expect (legacy.getSectionOrder (Section::pre) == juce::StringArray { "gate", "comp", "eq" });

            logMessage ("  -> saved \"" + p.parameters.state.getProperty (AmpSimProcessor::orderKey (Section::pre)).toString() + "\" / \""
                        + p.parameters.state.getProperty (AmpSimProcessor::orderKey (Section::post)).toString() + "\", restored the same; "
                        "\"harmonizer, eq, eq\" becomes \"" + odd.getSectionOrder (Section::pre).joinIntoString (", ")
                        + "\"; a Phase 5 \"eq, comp\" becomes \"" + phase5.getSectionOrder (Section::pre).joinIntoString (", ")
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
            setParam (p, "chorus_on", 1.0f);
            setParam (p, "chorus_sync", 1.0f);
            setParam (p, "reverb_on", 1.0f);
            setParam (p, "reverb_engine", 2.0f);
            setParam (p, "gate_a_on", 1.0f);
            setParam (p, "gate_b_on", 1.0f);
            for (auto [tab, name] : std::initializer_list<std::pair<int, const char*>> {
                     { 2, "editor_gates.png" }, { 3, "editor_prefx.png" }, { 4, "editor_postfx.png" }, { 5, "editor_timefx.png" } })
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

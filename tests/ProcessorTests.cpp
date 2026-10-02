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
            expect (p.getSectionOrder (ampsim::Chain::Section::pre) == juce::StringArray { "gate", "eq", "comp", "boost", "overdrive" }); // saved before they existed
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

        beginTest ("MIDI learn from the panel: a right-click menu on any control learns, switches, and forgets mappings, and never moves the control");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto* ampSimEditor = dynamic_cast<AmpSimEditor*> (editor.get());
            expect (ampSimEditor != nullptr);

            // Every control attached to a parameter carries its ID, on every tab.
            juce::StringArray tagged;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                if (const auto id = c.getProperties()["parameterId"].toString(); id.isNotEmpty())
                    tagged.addIfNotAlreadyThere (id);
                for (auto* child : c.getChildren())
                    walk (*child);
            };
            for (int tab = 0; tab < 6; ++tab)
            {
                ampSimEditor->showTab (tab);
                walk (*editor);
            }
            for (const auto* id : { "delay_on", "reverb_mix", "gate_a_threshold", "gate_link", "eq_pre_g3", "cab_bypass", "comp_post_mode" })
                expect (tagged.contains (id), id);

            juce::AudioBuffer<float> buffer (2, blockSize);
            const auto send = [&] (int cc, int value)
            {
                juce::MidiBuffer midi;
                midi.addEvent (juce::MidiMessage::controllerEvent (1, cc, value), 0);
                buffer.clear();
                p.processBlock (buffer, midi);
                p.runHousekeeping();
            };
            const auto choose = [&] (const juce::String& id, const juce::String& itemStart)
            {
                const auto menu = ampSimEditor->midiMenuFor (id); // the iterator only holds a reference
                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                    if (it.getItem().text.startsWith (itemStart) && it.getItem().action)
                    {
                        it.getItem().action();
                        return true;
                    }
                return false;
            };
            const auto& mappings = p.getMidiMap().getMappings();

            // A switch: learn, then the footswitch's first press maps it.
            expect (choose ("delay_on", "MIDI learn"));
            expect (p.getMidiMap().isLearning());
            send (85, 127);
            send (85, 0);
            expect (mappings.size() == 1 && mappings[0].cc == 85 && mappings[0].action == MidiMapping::Action::toggle);
            expectEquals (getParam (p, "delay_on"), 0.0f); // learning doesn't flip it
            send (85, 127);
            expectEquals (getParam (p, "delay_on"), 1.0f);

            // The menu now offers following the switch instead (a latching footswitch), and back, and forgetting it.
            expect (choose ("delay_on", "Follow the switch"));
            expect (mappings[0].action == MidiMapping::Action::momentary);
            send (85, 0);
            expectEquals (getParam (p, "delay_on"), 0.0f);
            expect (choose ("delay_on", "Flip on each press"));
            expect (mappings[0].action == MidiMapping::Action::toggle);
            expect (choose ("delay_on", "Forget CC 85"));
            expect (mappings.empty());

            // A knob: a continuous mapping over its whole range. And a learn can be cancelled.
            expect (choose ("reverb_mix", "MIDI learn"));
            send (86, 127);
            expect (mappings.size() == 1 && mappings[0].action == MidiMapping::Action::continuous && mappings[0].minimum == 0.0f
                    && mappings[0].maximum == 100.0f);
            expect (choose ("chorus_on", "MIDI learn"));
            expect (choose ("chorus_on", "Cancel MIDI learn"));
            expect (! p.getMidiMap().isLearning());

            // Right-clicks (and ctrl-clicks) never reach the control itself: a JUCE button would flip and a
            // linear slider would jump. Left-clicks still do.
            struct Recorder : juce::Component
            {
                int downs = 0, drags = 0, ups = 0;
                void mouseDown (const juce::MouseEvent&) override { ++downs; }
                void mouseDrag (const juce::MouseEvent&) override { ++drags; }
                void mouseUp (const juce::MouseEvent&) override { ++ups; }
            };
            IgnoresRightClick<Recorder> control;
            const auto click = [&] (int modifiers)
            {
                const auto now = juce::Time::getCurrentTime();
                const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), {}, juce::ModifierKeys (modifiers),
                                          juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &control, &control, now, {}, now, 1, false);
                control.mouseDown (e);
                control.mouseDrag (e);
                control.mouseUp (e);
            };
            click (juce::ModifierKeys::rightButtonModifier);
            click (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier);
            const auto reachedOnRightClick = control.downs + control.drags + control.ups;
            click (juce::ModifierKeys::leftButtonModifier);
            expectEquals (reachedOnRightClick, 0);
            expect (control.downs == 1 && control.drags == 1 && control.ups == 1);

            logMessage ("  -> " + juce::String (tagged.size()) + " parameters reachable by right-click across the 6 tabs; the delay switch learned CC 85 "
                        "as a toggle (the learning press changed nothing), switched to follow the switch and back, and was forgotten; the reverb mix "
                        "learned CC 86 over 0-100%; a learn was cancelled; right- and ctrl-clicks reached the control 0 times, a left-click 3 (down, drag, up)");
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
            expectWithinAbsoluteError (learnedA, p.getChain().gateA.getLearnedThresholdDb(), 0.051f);
            expectEquals (getParam (p, "gate_b_threshold"), -55.0f);
            setParam (p, "gate_link", 0.0f);
            p.learnGates();
            processAll (p, quiet);
            p.runHousekeeping();
            expectWithinAbsoluteError (getParam (p, "gate_b_threshold"), p.getChain().gateB.gate.getLearnedThresholdDb(), 0.051f);
            expect (getParam (p, "gate_b_threshold") > -50.0f);
            expect (! p.isLearningGates());
            const auto noiseFloor = p.getChain().gateA.getLearnedNoiseFloorDb();
            expect (noiseFloor > -66.0f && noiseFloor < -55.0f);

            logMessage ("  -> through the whole processor, bit for bit against a lone reference gate: linked Gate B with Gate A off applies Gate A's gain "
                        "(max difference " + juce::String (followsA) + "); both on and linked gate twice with one decision (" + juce::String (twice)
                        + "); unlinked, Gate B is its own gate at -40 dBFS (" + juce::String (own) + "); both mute the gaps in -79 dBFS RMS hiss "
                        "(min gain " + juce::String (closedA) + " and " + juce::String (closedB) + ")");
            logMessage ("  -> Learn on 2 s of -65 dBFS RMS hiss: noise floor (95th percentile of the 10 ms peak) " + juce::String (noiseFloor, 1)
                        + " dBFS, Gate A's threshold set to " + juce::String (learnedA, 1) + " dBFS (floor + 6 dB margin + 8 dB hysteresis); "
                        "Gate B learned only once unlinked: " + juce::String (getParam (p, "gate_b_threshold"), 1) + " dBFS");
        }

        beginTest ("boost and overdrive: through the processor they're exactly the blocks, switched on mid-song they start warm, and the interface level and oversampling reach them");
        {
            const auto input = guitarDI ((int) (2.0 * fs));
            using Settings = std::initializer_list<std::pair<const char*, float>>;

            // The processor with these settings, from the start or switched on at `onAt`.
            const auto render = [&] (Settings settings, const char* onId = nullptr, size_t onAt = 0)
            {
                AmpSimProcessor p;
                for (const auto& [id, value] : settings)
                    setParam (p, id, value);
                p.prepareToPlay (fs, blockSize);
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                std::vector<float> out (input.size());
                for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
                {
                    if (onId != nullptr && start == onAt)
                        setParam (p, onId, 1.0f);
                    buffer.clear();
                    buffer.copyFrom (0, 0, input.data() + start, blockSize);
                    p.processBlock (buffer, midi);
                    std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, out.begin() + (long) start);
                }
                return out;
            };
            // A lone block with the same settings, read through the same parameters, set before prepare()
            // as the processor does (so it starts in its mode, with no ramps).
            const auto lone = [&] (auto& block, Settings settings, auto readSettings, size_t startAt = 0)
            {
                AmpSimProcessor p;
                for (const auto& [id, value] : settings)
                    setParam (p, id, value);
                const auto blockSettings = readSettings (p);
                block.setSettings (blockSettings);
                block.prepare (fs, blockSize);
                std::vector<float> out (input.size(), 0.0f), scratch ((size_t) blockSize);
                for (size_t start = startAt; start + blockSize <= input.size(); start += blockSize)
                {
                    block.setSettings (blockSettings);
                    std::copy (input.begin() + (long) start, input.begin() + (long) start + blockSize, scratch.begin());
                    float* channels[] = { scratch.data() };
                    block.process (juce::dsp::AudioBlock<float> (channels, 1, (size_t) blockSize), { input.data() + start, blockSize });
                    std::copy (scratch.begin(), scratch.end(), out.begin() + (long) start);
                }
                return out;
            };
            const auto overdriveSettings = [] (AmpSimProcessor& p)
            {
                params::OverdriveParameters od;
                od.bind (p.parameters);
                params::Raw os;
                os.bind (p.parameters, "drive_oversampling");
                return od.read (params::oversamplingFactor (os), params::voltsAtFullScale (getParam (p, "input_level_dbu")));
            };
            const auto boostSettings = [] (AmpSimProcessor& p)
            {
                params::BoostParameters boost;
                boost.bind (p.parameters);
                return boost.read (4, params::voltsAtFullScale (getParam (p, "input_level_dbu")));
            };
            const auto maxDifference = [] (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
            {
                double worst = 0.0;
                for (size_t n = from; n < to; ++n)
                    worst = std::max (worst, std::abs ((double) a[n] - (double) b[n]));
                return worst;
            };
            const auto afterFade = (size_t) (0.02 * fs), end = input.size() - blockSize;

            // From the start: exactly the lone blocks (after the 10 ms switch-on fade).
            const Settings rat { { "od_on", 1.0f }, { "od_mode", 1.0f }, { "od_drive", 70.0f }, { "od_tone", 40.0f }, { "od_level", -6.0f },
                                 { "od_tight", 1.0f } };
            ampsim::Overdrive od1;
            const auto odDifference = maxDifference (render (rat), lone (od1, rat, overdriveSettings), afterFade, end);
            const Settings screamer { { "boost_on", 1.0f }, { "boost_mode", 2.0f }, { "boost_level", 6.0f } };
            ampsim::Boost boost1;
            const auto boostDifference = maxDifference (render (screamer), lone (boost1, screamer, boostSettings), afterFade, end);
            expectEquals (odDifference, 0.0);
            expectEquals (boostDifference, 0.0);

            // Switched on at 1 s: the overdrive kept running while it was off, so 10 ms later the output is
            // exactly an always-on overdrive's. A circuit started cold at 1 s instead, for comparison.
            const auto onAt = (size_t) fs / blockSize * blockSize;
            const Settings ratOff { { "od_mode", 1.0f }, { "od_drive", 70.0f }, { "od_tone", 40.0f }, { "od_level", -6.0f }, { "od_tight", 1.0f } };
            const auto engaged = render (ratOff, "od_on", onAt);
            ampsim::Overdrive alwaysOn, cold;
            const auto always = lone (alwaysOn, rat, overdriveSettings);
            const auto coldStart = lone (cold, rat, overdriveSettings, onAt);
            const auto warmDifference = maxDifference (engaged, always, onAt + (size_t) (0.011 * fs), end);
            double peak = 0.0;
            for (size_t n = onAt; n < end; ++n)
                peak = std::max (peak, std::abs ((double) always[n]));
            const auto coldDb = [&] (double fromMs, double toMs)
            { return toDb (maxDifference (coldStart, always, onAt + (size_t) (fromMs * 0.001 * fs), onAt + (size_t) (toMs * 0.001 * fs)) / peak); };
            expectEquals (warmDifference, 0.0);

            // The interface level sets the circuits' volts: +18 dBu drives them 6 dB harder than +12.
            const Settings hotter { { "od_on", 1.0f }, { "od_mode", 1.0f }, { "od_drive", 70.0f }, { "od_tone", 40.0f }, { "od_level", -6.0f },
                                    { "od_tight", 1.0f }, { "input_level_dbu", 18.0f } };
            ampsim::Overdrive od18;
            const auto hot = render (hotter);
            expectEquals (maxDifference (hot, lone (od18, hotter, overdriveSettings), afterFade, end), 0.0);
            const auto levelChange = toDb (rms (hot.data() + afterFade, end - afterFade) / rms (always.data() + afterFade, end - afterFade));

            // 8x reaches the blocks, and isn't part of a preset.
            const Settings at8x { { "od_on", 1.0f }, { "od_mode", 1.0f }, { "od_drive", 70.0f }, { "od_tone", 40.0f }, { "od_level", -6.0f },
                                  { "od_tight", 1.0f }, { "drive_oversampling", 1.0f } };
            ampsim::Overdrive od8;
            expectEquals (maxDifference (render (at8x), lone (od8, at8x, overdriveSettings), afterFade, end), 0.0);
            AmpSimProcessor p;
            setParam (p, "drive_oversampling", 1.0f);
            const auto preset = p.capturePreset ("x");
            expect (! preset["parameters"].hasProperty ("drive_oversampling"));
            expect (preset["parameters"].hasProperty ("od_drive") && preset["parameters"].hasProperty ("boost_mode"));

            logMessage ("  -> through the whole processor, against lone blocks: Distortion (drive 70%, tone 40%, -6 dB, tight 150 Hz) differs by "
                        + juce::String (odDifference) + ", the Screamer boost (+6 dB) by " + juce::String (boostDifference) + "; at 8x by 0");
            logMessage ("  -> switched on at 1 s: 11 ms later the output is an always-on overdrive's exactly (difference " + juce::String (warmDifference)
                        + "); a circuit started cold at 1 s would differ by " + juce::String (coldDb (11.0, 30.0), 1) + " dB (11 to 30 ms), "
                        + juce::String (coldDb (30.0, 100.0), 1) + " dB (30 to 100 ms), " + juce::String (coldDb (100.0, 300.0), 1)
                        + " dB (0.1 to 0.3 s) re. its peak");
            logMessage ("  -> interface level +18 dBu instead of +12: the circuits see 6 dB more volts, output level " + juce::String (levelChange, 2)
                        + " dB (clipping); matches the lone block at +18 exactly. drive_oversampling stays out of presets");
        }

        beginTest ("tuner: reads the DI on its own thread while engaged, mutes the output (or not), follows A4, and never comes back engaged");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);

            // A2 as a plucked harmonic tone: partials n at 1/n, a slow decay, -12 dBFS peak or so.
            const auto pluck = [] (double f, double seconds)
            {
                std::vector<float> x ((size_t) (seconds * fs));
                for (size_t i = 0; i < x.size(); ++i)
                {
                    const auto t = (double) i / fs;
                    double v = 0.0;
                    for (int n = 1; n <= 8; ++n)
                        v += std::sin (juce::MathConstants<double>::twoPi * n * f * t) / n;
                    x[i] = (float) (0.12 * v * std::exp (-t / 4.0));
                }
                return x;
            };
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            // Played at about real time (a 128-sample buffer is 2.67 ms), so the analysis thread keeps up.
            const auto play = [&] (const std::vector<float>& x)
            {
                std::vector<float> out;
                for (size_t start = 0; start + blockSize <= x.size(); start += blockSize)
                {
                    buffer.clear();
                    buffer.copyFrom (0, 0, x.data() + start, blockSize);
                    p.processBlock (buffer, midi);
                    out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
                    juce::Thread::sleep (2);
                }
                return out;
            };

            const auto note = pluck (110.0, 4.0);
            const auto idleUpdates = p.getTunerUpdateCount();
            play (std::vector<float> (note.begin(), note.begin() + (long) (0.2 * fs)));
            const auto updatesWhileOff = p.getTunerUpdateCount() - idleUpdates;

            setParam (p, "tuner_on", 1.0f);
            const auto startUpdates = p.getTunerUpdateCount();
            const auto muted = play (std::vector<float> (note.begin(), note.begin() + (long) (1.5 * fs)));
            const auto a2 = p.getTunerReading();
            const auto updates = p.getTunerUpdateCount() - startUpdates;
            expect (p.isTunerEngaged());
            expect (a2.hasReading && a2.live && a2.midiNote == 45);
            expectWithinAbsoluteError (a2.cents, 0.0, 0.5);
            double mutedPeak = 0.0;
            for (size_t n = (size_t) (0.025 * fs); n < muted.size(); ++n)
                mutedPeak = std::max (mutedPeak, std::abs ((double) muted[n]));
            expectEquals (mutedPeak, 0.0);
            const auto fadeStep = maxStep (muted, 0, (size_t) (0.025 * fs));
            const auto playingStep = maxStep (note, 0, (size_t) (0.5 * fs));
            expectLessThan (fadeStep, playingStep * 1.01);

            // Tune while hearing yourself: the guitar comes back (the defaults pass it untouched).
            setParam (p, "tuner_mute", 0.0f);
            const auto heardFrom = (size_t) (1.5 * fs) / blockSize * blockSize;
            const auto heard = play (std::vector<float> (note.begin() + (long) heardFrom, note.begin() + (long) heardFrom + (long) (0.4 * fs)));
            double heardDifference = 0.0;
            for (size_t n = (size_t) (0.025 * fs); n < heard.size(); ++n)
                heardDifference = std::max (heardDifference, std::abs ((double) heard[n] - (double) note[heardFrom + n]));
            expectEquals (heardDifference, 0.0);

            // A4 = 432 Hz: the same string reads 31.77 cents sharp.
            setParam (p, "tuner_a4", 432.0f);
            const auto a4From = heardFrom + heard.size();
            play (std::vector<float> (note.begin() + (long) a4From, note.begin() + (long) a4From + (long) (0.6 * fs)));
            const auto at432 = p.getTunerReading();
            const auto expected432 = 1200.0 * std::log2 (440.0 / 432.0);
            expectWithinAbsoluteError (at432.referenceA4, 432.0, 0.05);
            expect (at432.midiNote == 45);
            expectWithinAbsoluteError (at432.cents, expected432, 0.5);

            // Never saved engaged, never in a preset.
            juce::MemoryBlock state;
            p.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expectEquals (getParam (restored, "tuner_on"), 0.0f);
            expectWithinAbsoluteError (getParam (restored, "tuner_a4"), 432.0f, 0.05f);
            const auto preset = p.capturePreset ("x");
            for (const auto* id : { "tuner_on", "tuner_mute", "tuner_a4" })
                expect (! preset["parameters"].hasProperty (id), id);

            setParam (p, "tuner_on", 0.0f);
            logMessage ("  -> off: " + juce::String ((int) updatesWhileOff) + " analyses in 0.2 s; engaged: " + juce::String ((int) updates) + " analyses in "
                        "1.5 s of a plucked 110 Hz tone, reading " + juce::MidiMessage::getMidiNoteName (a2.midiNote, true, true, 4) + " "
                        + juce::String (a2.cents, 3) + " cents (" + juce::String (a2.frequency, 4) + " Hz)");
            logMessage ("  -> muted while tuning: output exactly 0 after the 20 ms fade (largest step during it " + juce::String (fadeStep, 4)
                        + " vs. " + juce::String (playingStep, 4) + " in the note itself); with mute off the guitar passes untouched (difference "
                        + juce::String (heardDifference) + ")");
            logMessage ("  -> A4 = 432 Hz: the same string reads " + juce::String (at432.cents, 3) + " cents (expected " + juce::String (expected432, 3)
                        + "); a restored session starts with the tuner off; no tuner setting is in a preset");
        }

        beginTest ("the effect order is saved by block name and restored, and odd saved orders are repaired");
        {
            using Section = ampsim::Chain::Section;
            AmpSimProcessor p;
            expect (p.getSectionOrder (Section::pre) == juce::StringArray { "gate", "comp", "boost", "overdrive", "eq" });
            expect (startsWith (p.getSectionOrder (Section::post), { "eq", "comp" }));
            p.setSectionOrder (Section::pre, { "eq", "gate", "comp", "boost", "overdrive" });
            p.setSectionOrder (Section::post, { "comp", "eq" });
            expect (startsWith (p.getSectionOrder (Section::post), { "comp", "eq", "chorus" }));

            juce::MemoryBlock state;
            p.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expect (restored.getSectionOrder (Section::pre) == juce::StringArray { "eq", "gate", "comp", "boost", "overdrive" });
            expect (startsWith (restored.getSectionOrder (Section::post), { "comp", "eq" }));
            expect (restored.getSectionOrder (Section::post) == p.getSectionOrder (Section::post));

            // A name from the future, a repeat, and missing blocks: unknown and repeated names are
            // skipped, and a missing block goes where it breaks the fewest pairs of the default order.
            AmpSimProcessor odd;
            odd.setSectionOrder (Section::pre, { "harmonizer", "eq", "eq" });
            expect (odd.getSectionOrder (Section::pre) == juce::StringArray { "gate", "comp", "boost", "overdrive", "eq" });

            // An order saved before the gate and the drive blocks existed (Phase 5): the gate goes first, the
            // boost and overdrive after the compressor, and the player's own order stays.
            AmpSimProcessor phase5;
            phase5.setSectionOrder (Section::pre, { "eq", "comp" });
            expect (phase5.getSectionOrder (Section::pre) == juce::StringArray { "gate", "eq", "comp", "boost", "overdrive" });

            // A state saved before effects existed loads with the default order.
            AmpSimProcessor legacy;
            auto tree = legacy.parameters.copyState();
            tree.removeProperty (AmpSimProcessor::orderKey (Section::pre), nullptr);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
            legacy.setStateInformation (old.getData(), (int) old.getSize());
            expect (legacy.getSectionOrder (Section::pre) == juce::StringArray { "gate", "comp", "boost", "overdrive", "eq" });

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
            setParam (p, "boost_on", 1.0f);
            setParam (p, "boost_mode", 1.0f);
            setParam (p, "od_on", 1.0f);
            setParam (p, "od_mode", 1.0f);
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

            // The tuner over the tabs: needle, then strobe, with frozen readings.
            setParam (p, "tuner_on", 1.0f);
            ampSimEditor->refresh();
            auto& tunerView = ampSimEditor->getTunerView();
            expect (tunerView.isVisible());
            ampsim::TunerReading e2;
            e2.hasReading = e2.live = true;
            e2.midiNote = 40;
            e2.frequency = 82.56;
            e2.cents = 3.1;
            e2.levelDb = -21.0;
            tunerView.setStrobe (false);
            tunerView.freeze (e2);
            const auto needleFile = proofDir().getChildFile ("editor_tuner_needle.png");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), needleFile));
            auto flat = e2;
            flat.midiNote = 45;
            flat.frequency = 108.64;
            flat.cents = -21.6;
            flat.strobePhase = 0.3;
            tunerView.setStrobe (true);
            tunerView.freeze (flat);
            const auto strobeFile = proofDir().getChildFile ("editor_tuner_strobe.png");
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), strobeFile));
            tunerView.setStrobe (false);
            setParam (p, "tuner_on", 0.0f);
            ampSimEditor->refresh();
            expect (! tunerView.isVisible());
            logMessage ("  -> " + needleFile.getFullPathName());
            logMessage ("  -> " + strobeFile.getFullPathName());

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

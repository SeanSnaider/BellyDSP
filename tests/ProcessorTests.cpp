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

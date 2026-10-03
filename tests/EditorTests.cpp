// The GUI after the UI handoff (docs/ui/amp-ui-handoff/): the designed pages rendered at 2x next to the
// handoff's screenshots (build/proof/compare_<page>.png), and their behaviour.

#include "PluginEditor.h"
#include "TestHelpers.h"

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getParam (AmpSimProcessor& p, const juce::String& id)
{
    return p.parameters.getRawParameterValue (id)->load();
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

juce::File handoffScreenshot (const juce::String& name)
{
    return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("docs/ui/amp-ui-handoff/screenshots").getChildFile (name);
}

/// Ours on the left, the handoff's on the right, at the same size, with a thin gap.
bool writeComparison (const juce::Image& ours, const juce::String& screenshot, const juce::String& name)
{
    const auto theirs = juce::ImageFileFormat::loadFrom (handoffScreenshot (screenshot));
    if (! theirs.isValid())
        return false;
    const auto w = juce::jmax (ours.getWidth(), theirs.getWidth()), h = juce::jmax (ours.getHeight(), theirs.getHeight());
    juce::Image both (juce::Image::RGB, 2 * w + 16, h, true);
    juce::Graphics g (both);
    g.fillAll (juce::Colour (0xff3a3a3a));
    g.drawImageAt (ours, 0, 0);
    g.drawImageAt (theirs, w + 16, 0);
    return savePng (both, proofDir().getChildFile ("compare_" + name + ".png"));
}

/// A copy of an example capture whose metadata names its tone (and a make and model the UI must not show).
juce::File taggedCapture (const juce::String& source, const juce::String& toneType, const juce::String& fileName)
{
    auto json = juce::JSON::parse (exampleModel (source).loadFileAsString());
    auto* metadata = new juce::DynamicObject();
    metadata->setProperty ("tone_type", toneType);
    metadata->setProperty ("gear_make", "Brandname");
    metadata->setProperty ("gear_model", "Model X");
    json.getDynamicObject()->setProperty ("metadata", juce::var (metadata));
    const auto file = tempDir().getChildFile (fileName);
    file.replaceWithText (juce::JSON::toString (json, true));
    return file;
}

/// Plays guitar through the processor for `seconds`, in buffers, letting the GUI's analyzer read along.
void play (AmpSimProcessor& p, double seconds, std::function<void()> eachFrame = {})
{
    const auto input = guitarDI ((int) (seconds * fs));
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0, n = 0; start + blockSize <= input.size(); start += blockSize, ++n)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        if (eachFrame && n % 12 == 11)
            eachFrame();
    }
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> at, juce::Point<float> down, int modifiers = 0, bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | modifiers),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, down, now, 1, dragged);
}

class EditorTests final : public juce::UnitTest
{
public:
    EditorTests() : juce::UnitTest ("Editor after the UI handoff", "ampsim") {}

    void runTest() override
    {
        beginTest ("fonts: Geist and Fraunces load from the app's own data");
        {
            expect (ui::theme::bundledFontsLoaded());
            const auto geist = juce::Font (ui::theme::geist (ui::theme::Weight::medium, 14.0f));
            expect (geist.getTypefacePtr()->getName().startsWith ("Geist"), geist.getTypefacePtr()->getName());
            logMessage ("  -> bundled fonts loaded: " + juce::String (ui::theme::bundledFontsLoaded() ? "yes" : "no") + " (Geist 300/400/500/600, Fraunces SemiBold Italic)");
        }

        beginTest ("amp page: each slot wears its head, its own knobs, its capture's voice; the UI follows program changes; compared with the handoff");
        {
            AmpSimProcessor p;
            p.loadModel (0, taggedCapture ("wavenet_a1_standard.nam", "clean", "glass_clean.nam"));
            p.loadModel (1, taggedCapture ("lstm.nam", "crunch", "ember_crunch.nam"));
            p.loadModel (2, taggedCapture ("A2.nam", "hi_gain", "monolith_high_gain.nam"));
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            p.parameters.state.setProperty ("presetName", "Open string clean", nullptr);
            p.parameters.state.setProperty ("presetSource", "user", nullptr);
            setParam (p, AmpSimProcessor::ampParamId (0, "input_trim"), -7.2f);   // 3.5 on the knob
            setParam (p, AmpSimProcessor::ampParamId (0, "mid"), 2.4f);           // 6.0
            setParam (p, AmpSimProcessor::ampParamId (0, "treble"), 3.6f);        // 6.5
            setParam (p, AmpSimProcessor::ampParamId (2, "input_trim"), 12.0f);   // 7.5
            setParam (p, AmpSimProcessor::ampParamId (2, "bass"), -2.4f);         // 4.0

            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            auto& amp = ed.getAmpView();

            const std::array<const char*, 3> names { "glass", "ember", "monolith" };
            const std::array<const char*, 3> shots { "01_amp_glass.png", "02_amp_ember.png", "03_amp_monolith.png" };
            const std::array<const char*, 3> voices { "Clean", "Crunch", "High gain" };
            juce::StringArray files, seen;
            for (int s = 0; s < 3; ++s)
            {
                setParam (p, AmpSimProcessor::slotParamId, (float) s);
                play (p, 0.6, [&] { amp.getSpectrum().update(); });
                ed.refresh();
                ed.getTopBar().updateMeters (p.takePeaks(), 1.0 / 30.0); // what the meter timer does
                expectEquals (amp.getShownSlot(), s);
                expect (amp.getHead().getMaterial() == ui::materialFor (s));
                expect (amp.getKnob (s, 0).isVisible() && ! amp.getKnob ((s + 1) % 3, 0).isVisible());
                expect (amp.getJewel().isLit());
                expectEquals (amp.getVoiceText(), juce::String (voices[(size_t) s]));
                expect (! amp.getVoiceText().contains ("Brandname") && ! amp.getModelText().contains ("Model X"));
                expect (amp.getSpectrum().hasCurve());
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                const auto file = proofDir().getChildFile ("editor_amp_" + juce::String (names[(size_t) s]) + ".png");
                expect (savePng (image, file));
                expect (writeComparison (image, shots[(size_t) s], "amp_" + juce::String (names[(size_t) s])));
                files.add (file.getFileName());
                seen.add (amp.getVoiceText() + " / " + amp.getModelText() + " / " + amp.getRateText());
            }
            expectEquals (amp.getKnob (0, 0).getValueText(), juce::String ("3.5"));
            expectEquals (amp.getKnob (0, 3).getValueText(), juce::String ("6.5"));
            expectEquals (amp.getKnob (2, 0).getValueText(), juce::String ("7.5"));
            expectEquals (amp.getKnob (1, 6).getValueText(), juce::String ("5.0")); // Master at unity

            // The three heads' art alone, at 2x (proof of the procedural textures).
            for (int m = 0; m < 3; ++m)
                expect (savePng (ui::AmpHead::render ((ui::Material) m, 2.0f), proofDir().getChildFile ("amp_head_" + juce::String (names[(size_t) m]) + ".png")));

            // The head is drawn once per material at a scale, then only drawn from the cache.
            const auto renders = amp.getHead().getRenderCount();
            for (int i = 0; i < 5; ++i)
                editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            expectEquals (amp.getHead().getRenderCount(), renders);

            // A program change from the footswitch: the page follows (the audio thread switches, the timer
            // writes the slot, the page shows it at its next refresh).
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::programChange (1, 1), 0);
            p.processBlock (buffer, midi);
            p.runHousekeeping();
            ed.refresh();
            const auto followedPc = amp.getShownSlot() == 1 && amp.getTabs().getSelected() == 1;
            expect (followedPc);

            // A click on a tab switches the slot; the slot's knobs come back with their own values.
            auto& tab = *amp.getTabs().getTab (0);
            const auto middle = tab.getLocalBounds().getCentre().toFloat().withY (10.0f);
            tab.mouseDown (mouseEvent (tab, middle, middle));
            tab.mouseUp (mouseEvent (tab, middle, middle));
            ed.refresh();
            expectEquals ((int) getParam (p, AmpSimProcessor::slotParamId), 0);
            expectEquals (amp.getKnob (0, 0).getValueText(), juce::String ("3.5"));

            // An empty slot: a dark jewel and "No capture loaded".
            p.clearModel (2);
            waitForLoads (p);
            setParam (p, AmpSimProcessor::slotParamId, 2.0f);
            ed.refresh();
            expect (! amp.getJewel().isLit());
            expectEquals (amp.getVoiceText(), juce::String ("No capture loaded"));

            logMessage ("  -> " + files.joinIntoString (", ") + "; info rows: " + seen.joinIntoString ("; "));
            logMessage ("  -> knobs read 3.5 / 6.5 (Glass gain, treble), 7.5 (Monolith gain), 5.0 (Ember master at unity); the head art rendered "
                        + juce::String (renders) + " times for 3 materials and 8 snapshots; program change 2 moved the page to Ember: "
                        + (followedPc ? "yes" : "no") + "; an empty slot's jewel is dark");
        }

        beginTest ("a knob mid-drag shows its value underlined in emerald on an amp panel (compared with the handoff's crop)");
        {
            AmpSimProcessor p;
            p.loadModel (0, exampleModel ("wavenet_a1_standard.nam"));
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            auto& gain = ed.getAmpView().getKnob (0, 0);
            const auto centre = gain.getLocalBounds().getCentre().toFloat();
            gain.mouseDown (mouseEvent (gain, centre, centre));
            gain.mouseDrag (mouseEvent (gain, centre.translated (0.0f, -30.0f), centre, 0, true)); // 15% up from 5.0: 6.5
            const auto label = gain.getShownLabel();
            const auto full = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            gain.mouseUp (mouseEvent (gain, centre.translated (0.0f, -30.0f), centre, 0, true));
            expectEquals (label, juce::String ("6.5"));
            expect (savePng (full, proofDir().getChildFile ("editor_knob_dragging.png")));
            const auto crop = full.getClippedImage ({ 508, 307, 840, 300 });
            expect (savePng (crop, proofDir().getChildFile ("editor_knob_dragging_crop.png")));
            expect (writeComparison (crop, "06_knob_dragging.png", "knob_dragging"));
            logMessage ("  -> Gain dragged 30 points up from 5.0: the label reads \"" + label + "\" mid-drag; build/proof/editor_knob_dragging.png");
        }
    }
};

EditorTests editorTests;
} // namespace

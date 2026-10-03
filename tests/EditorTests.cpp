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

/// A cab pack folder: four captures on a 2 x 2 grid (dust cap and cone edge, 1 and 4 inches away).
juce::File writePack (const juce::File& folder)
{
    folder.deleteRecursively();
    folder.createDirectory();
    writeWav (folder.getChildFile ("Cap_1in.wav"), toBuffer (syntheticCabIR (4096, 8.0, 7000.0)));
    writeWav (folder.getChildFile ("Edge_1in.wav"), toBuffer (syntheticCabIR (4096, 0.0, 3500.0)));
    writeWav (folder.getChildFile ("Cap_4in.wav"), toBuffer (syntheticCabIR (4096, 6.0, 6000.0)));
    writeWav (folder.getChildFile ("Edge_4in.wav"), toBuffer (syntheticCabIR (4096, -2.0, 3000.0)));
    return folder;
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

        beginTest ("cab page: the library lists packs and IRs; a pick loads close mic 1, assigns it, and stops following; mics move on packs only; compared with the handoff");
        {
            // A library of three cabs: a pack and two IR files.
            const auto library = tempDir().getChildFile ("cab_library");
            library.deleteRecursively();
            library.createDirectory();
            writePack (library.getChildFile ("2x12 open back"));
            writeWav (library.getChildFile ("4x12 vintage.wav"), toBuffer (syntheticCabIR (4096, 3.0, 4500.0)));
            writeWav (library.getChildFile ("4x12 modern.wav"), toBuffer (syntheticCabIR (4096, 5.0, 6000.0)));
            presets::setLibraryRoot ("irs", library);

            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            p.parameters.state.setProperty ("presetName", "Open string clean", nullptr);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::cab);
            auto& cab = ed.getCabView();
            const auto& entries = cab.getEntries();
            expectEquals ((int) entries.size(), 3);
            const auto listedCount = (int) entries.size();
            const auto listed = entries.size() == 3 && entries[0].pack && entries[0].name == "2x12 open back" && entries[1].name == "4x12 modern";

            // Follow is on by default; a pick turns it off and assigns the cab to the playing slot.
            const auto followedBefore = p.isCabFollowing();
            cab.clickEntry (0);
            waitForLoads (p);
            p.loadCabIR (1, writePack (tempDir().getChildFile ("mic_b_pack")));
            waitForLoads (p);
            setParam (p, AmpSimProcessor::cabParamId (0, "pos_x"), 0.13f / 0.95f);
            setParam (p, AmpSimProcessor::cabParamId (1, "pos_x"), 0.52f / 0.95f);
            setParam (p, AmpSimProcessor::cabParamId (1, "pos_y"), 0.35f);
            ed.refresh();
            expect (followedBefore && ! p.isCabFollowing());
            expect (p.getCabAssignment (0) == entries[0].file);
            expectEquals (p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString(), entries[0].file.getFullPathName());
            expectEquals (cab.getReadout(), juce::String ("Mic A|Cap|Mic B|Cone"));
            expect (! cab.isMarkerDimmed (0) && ! cab.isMarkerDimmed (1));

            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            expect (savePng (image, proofDir().getChildFile ("editor_cab_page.png")));
            expect (writeComparison (image, "04_cab.png", "cab"));

            // Dragging mic A out to the cone's edge: pos_x follows the distance (clamped at 0.95 of the radius).
            const auto from = cab.markerPosition (0);
            cab.dragMarker (0, from, from + juce::Point<float> (400.0f, 0.0f));
            ed.refresh();
            const auto clamped = getParam (p, AmpSimProcessor::cabParamId (0, "pos_x"));
            expectWithinAbsoluteError (clamped, 1.0f, 1.0e-3f);
            const auto zoneAtEdge = cab.getReadout();

            // A plain IR in mic B: its marker dims and can't be dragged.
            p.loadCabIR (1, library.getChildFile ("4x12 vintage.wav"));
            waitForLoads (p);
            ed.refresh();
            const auto before = getParam (p, AmpSimProcessor::cabParamId (1, "pos_x"));
            cab.dragMarker (1, cab.markerPosition (1), cab.markerPosition (1) + juce::Point<float> (-60.0f, 0.0f));
            expect (cab.isMarkerDimmed (1));
            expectEquals (getParam (p, AmpSimProcessor::cabParamId (1, "pos_x")), before);
            const auto withIR = cab.getReadout();
            p.loadCabIR (0, library.getChildFile ("4x12 modern.wav"));
            waitForLoads (p);
            ed.refresh();
            expectEquals (cab.getReadout(), juce::String ("Load a pack to move mics"));
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("editor_cab_page_irs.png")));

            // Follow back on, then switching to a slot with a cab assigned loads it.
            p.setCabAssignment (1, library.getChildFile ("4x12 vintage.wav"));
            p.setCabFollow (true);
            setParam (p, AmpSimProcessor::slotParamId, 1.0f);
            p.runHousekeeping();
            ed.refresh();
            expectEquals (p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString(), library.getChildFile ("4x12 vintage.wav").getFullPathName());

            // An empty library says so.
            presets::setLibraryRoot ("irs", tempDir().getChildFile ("empty_library"));
            cab.rescan();
            expect (cab.getEntries().empty());
            presets::setLibraryRoot ("irs", {});

            expect (listed);
            logMessage ("  -> the library listed " + juce::String (listedCount) + " cabs (a pack, two IRs); a click loaded the pack into close mic 1, "
                        "assigned it to slot 1, and turned Follow off; readouts: \"" + juce::String ("Mic A Cap, Mic B Cone") + "\", after dragging A to the edge \""
                        + zoneAtEdge.replace ("|", " ") + "\" (pos_x " + juce::String (clamped, 3) + "), with an IR in mic B \"" + withIR.replace ("|", " ")
                        + "\" (its marker dimmed and fixed); with IRs in both: \"Load a pack to move mics\"");
        }

        beginTest ("tuner page: a real tone through the tuner thread, in tune and 13 cents sharp; strings, targeting, tunings; compared with the handoff");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            p.parameters.state.setProperty ("presetName", "Open string clean", nullptr);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::tuner);
            expectEquals (getParam (p, "tuner_on"), 1.0f); // opening the page engages the tuner
            auto& tuner = ed.getTunerPage();
            tuner.setTuning (0);

            // A plucked string (partials n at 1/n), played at about real time so the tuner's thread keeps up.
            const auto pluck = [&p, &tuner] (double f, double seconds)
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                const auto n = (int) (seconds * fs);
                for (int start = 0; start + blockSize <= n; start += blockSize)
                {
                    buffer.clear();
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const auto t = (double) (start + i) / fs;
                        double v = 0.0;
                        for (int k = 1; k <= 8; ++k)
                            v += std::sin (juce::MathConstants<double>::twoPi * k * f * t) / k;
                        buffer.setSample (0, i, (float) (0.12 * v * std::exp (-t / 4.0)));
                    }
                    p.processBlock (buffer, midi);
                    juce::Thread::sleep (2);
                }
                tuner.poll();
            };

            pluck (82.41, 1.5); // E2, in tune
            ed.refresh();
            const auto inTune = tuner.getShown();
            const auto barNote = ed.getTopBar().getTunerNote();
            expect (inTune.inTune && inTune.note == "E" && inTune.octave == "2" && inTune.currentString == 0, inTune.cents);
            expect (tuner.isStringDone (0));
            expectEquals (barNote, juce::String ("E"));
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("editor_tuner_in_tune.png")));

            pluck (82.41 * std::pow (2.0, 13.0 / 1200.0), 1.5); // 13 cents sharp
            const auto sharp = tuner.getShown();
            expect (! sharp.inTune && sharp.state == "Sharp" && std::abs (sharp.centsValue - 13.0) < 1.0, sharp.cents);
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            expect (savePng (image, proofDir().getChildFile ("editor_tuner_sharp.png")));
            expect (writeComparison (image, "05_tuner.png", "tuner"));

            // Targeting the A string: the same E reads 500 cents flat of it (off the scale); again lets go.
            tuner.clickString (1);
            const auto targeted = tuner.getShown();
            expect (targeted.note == "A" && targeted.currentString == 1 && targeted.centsValue < -480.0);
            tuner.clickString (1);
            expect (tuner.getTarget() < 0);

            // Drop D: the low string is D2; a new tuning starts with no strings done.
            tuner.setTuning (1);
            expect (! tuner.isStringDone (0));
            expectEquals ((int) p.parameters.state.getProperty ("tunerTuning"), 1);

            // Leaving the page disengages the tuner, and the button's note goes back to a dash.
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            expectEquals (getParam (p, "tuner_on"), 0.0f);
            expectEquals (ed.getTopBar().getTunerNote(), juce::String ("-"));
            logMessage ("  -> E2 at 82.41 Hz: " + inTune.note + inTune.octave + " " + inTune.hz + " Hz, " + inTune.cents + " cents, \"" + inTune.state
                        + "\", string 6 marked tuned, the top bar's Tuner shows \"" + barNote + "\"; 13 cents sharp: " + sharp.hz + " Hz, " + sharp.cents + " cents, \""
                        + sharp.state + "\"; targeting the A string read " + juce::String (targeted.centsValue, 1) + " cents; leaving the page disengaged the tuner");
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

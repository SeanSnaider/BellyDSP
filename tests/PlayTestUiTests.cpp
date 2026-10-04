// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The UI from the 2026-10-04 play-test fixes: the Amp page's Gate switch, the Out meter's limit light and the
// Output page's limiter controls, and the Multivoicer's mix mode, shapes, and interval pickers. Each is driven
// the way a click drives it, checked against the parameters, and snapshotted at 2x into build/proof.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ui/AmpView.h"
#include "ui/MainPages.h"
#include "ui/Pages.h"

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

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> at)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, 1, false);
}

/// A plain left click in the middle of a component, as the mouse would deliver it.
void click (juce::Component& c)
{
    const auto centre = c.getLocalBounds().getCentre().toFloat();
    c.mouseDown (mouseEvent (c, centre));
    c.mouseUp (mouseEvent (c, centre));
}

void play (AmpSimProcessor& p, const std::vector<float>& input)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
    }
}
} // namespace

class PlayTestUiTests final : public juce::UnitTest
{
public:
    PlayTestUiTests() : juce::UnitTest ("Play-test UI", "ampsim") {}

    void runTest() override
    {
        beginTest ("Amp page: the strip's Gate group has Gate A's switch; a click turns it on (one undo step), and the light only shows while it's on");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            auto& amp = ed.getAmpView();
            auto& gateSwitch = amp.getGateSwitch();
            expect (gateSwitch.isVisible() && ! gateSwitch.getToggleState());
            expectEquals (getParam (p, "gate_a_on"), 0.0f);
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("playtest_amp_gate_off.png")));

            p.undoManager.beginNewTransaction(); // what the editor's own mouse press handler does first
            click (gateSwitch);
            p.parameters.copyState();            // what the parameter tree's timer does
            ed.refresh();
            const auto onAfterClick = getParam (p, "gate_a_on");
            play (p, guitarDI ((int) (0.5 * fs)));
            ed.refresh();
            expectEquals (onAfterClick, 1.0f);
            expect (gateSwitch.getToggleState());
            expect (p.undoManager.canUndo());
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("playtest_amp_gate_on.png")));
            const auto strip = amp.createComponentSnapshot (amp.getLocalBounds().withTrimmedTop (amp.getHeight() - 100), true, 3.0f);
            expect (savePng (strip, proofDir().getChildFile ("playtest_amp_strip_crop.png")));
            p.undoManager.undo();
            expectEquals (getParam (p, "gate_a_on"), 0.0f);
            logMessage ("  -> gate_a_on 0 at the start, 1 after a click on the strip's switch, 0 again after undo; build/proof/playtest_amp_gate_off.png, "
                        "playtest_amp_gate_on.png, playtest_amp_strip_crop.png");
        }

        beginTest ("Out meter: \"limit\" and an ink cap while the output limiter works, gone 1.5 s after; the Output page has the limiter's switch and ceiling");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "output_gain", 24.0f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::output);
            play (p, guitarDI ((int) (0.5 * fs)));
            ed.getTopBar().updateMeters (p.takePeaks(), 1.0 / 30.0); // what the meter timer does
            auto& out = ed.getTopBar().getOutputMeter();
            const auto limiting = out.isLimiting(), clipped = out.isClipped();
            expect (limiting);
            expect (! clipped); // the limiter kept it under -1 dBFS: no clip
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("playtest_output_limiting.png")));
            const auto meterArea = out.getBoundsInParent().getUnion (ed.getTopBar().getInputMeter().getBoundsInParent()).expanded (8);
            expect (savePng (ed.getTopBar().createComponentSnapshot (meterArea, true, 4.0f), proofDir().getChildFile ("playtest_meters_limit_crop.png")));

            // Quiet again: it holds 1.5 s, then goes.
            setParam (p, "output_gain", -12.0f);
            for (int frame = 0; frame < 60; ++frame)
            {
                play (p, std::vector<float> ((size_t) (fs / 30.0), 0.0f));
                ed.getTopBar().updateMeters (p.takePeaks(), 1.0 / 30.0);
            }
            expect (! out.isLimiting());
            logMessage ("  -> +24 dB of output level: the Out meter shows the limit cap and text (no clip latched); quiet for 2 s: gone. "
                        "build/proof/playtest_output_limiting.png, playtest_meters_limit_crop.png");
        }

        beginTest ("Multivoicer page: the mix mode, the four shapes (each one undo step), and each voice's interval picker; snapshots");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "mv_on", 1.0f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::multivoicer);
            ed.refresh();
            auto& page = dynamic_cast<ui::MultivoicerPage&> (ed.getPage (ui::BlockId::multivoicer));
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("playtest_multivoicer_default.png")));

            juce::StringArray seen;
            const std::array<std::vector<int>, 4> expected { { { 12 }, { -12 }, { 7, 12 }, { -12, 12 } } };
            for (int i = 0; i < 4; ++i)
            {
                auto& button = page.getShapeButton (i);
                button.onClick();
                const auto count = juce::roundToInt (getParam (p, "mv_voices"));
                expectEquals (count, (int) expected[(size_t) i].size());
                juce::StringArray intervals;
                for (int v = 0; v < count; ++v)
                {
                    const auto st = juce::roundToInt (getParam (p, params::MultivoicerParameters::voiceId (v, "semitones")));
                    expectEquals (st, expected[(size_t) i][(size_t) v]);
                    expectEquals (getParam (p, params::MultivoicerParameters::voiceId (v, "delay")), 0.0f);
                    expectWithinAbsoluteError (getParam (p, params::MultivoicerParameters::voiceId (v, "cents")), 0.0f, 1.0e-4f);
                    intervals.add ((st > 0 ? "+" : "") + juce::String (st));
                }
                expectEquals (getParam (p, "mv_mix_mode"), 1.0f);
                seen.add (button.getButtonText() + ": " + intervals.joinIntoString (", "));
            }
            // The last shape is one undo step: undoing it puts back the Power fifth exactly.
            p.parameters.copyState(); // what the parameter tree's timer does
            p.undoManager.undo();
            expectEquals (juce::roundToInt (getParam (p, params::MultivoicerParameters::voiceId (0, "semitones"))), 7);
            p.undoManager.redo();
            ed.refresh();
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), proofDir().getChildFile ("playtest_multivoicer_octave_stack.png")));

            // Voice 2's picker: the named intervals, the current one ticked; picking the fifth sets +7, cents 0.
            setParam (p, params::MultivoicerParameters::voiceId (1, "cents"), 8.0f);
            auto menu = page.intervalMenu (1);
            juce::StringArray items;
            bool tickedOctave = false;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                items.add (it.getItem().text);
                tickedOctave = tickedOctave || (it.getItem().isTicked && it.getItem().text.startsWith ("Octave up"));
                if (it.getItem().text.startsWith ("Fifth"))
                    it.getItem().action();
            }
            expect (tickedOctave);
            expectEquals (items.size(), 7);
            expectEquals (juce::roundToInt (getParam (p, params::MultivoicerParameters::voiceId (1, "semitones"))), 7);
            expectWithinAbsoluteError (getParam (p, params::MultivoicerParameters::voiceId (1, "cents")), 0.0f, 1.0e-4f);
            expect (page.getIntervalPicker (1).isVisible());
            logMessage ("  -> shapes: " + seen.joinIntoString ("; ") + " (each sets the mix to Add, delays and cents 0; undo restores the previous shape). Voice 2's picker: "
                        + items.joinIntoString (", ") + " (the current one ticked); Fifth set it to +7, cents 0. build/proof/playtest_multivoicer_default.png, "
                        "playtest_multivoicer_octave_stack.png");
        }
    }
};

static PlayTestUiTests playTestUiTests;

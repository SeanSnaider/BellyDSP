// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The main screen (the app redesign, phase 1: .impeccable/surfaces/src-plugineditor-cpp.md): the theme, selecting a
// pedal (the inspector shows its controls), the footswitches (one undo step each), the keys, the stage's modes, the
// overlays, the scenes in the footer, and snapshots of it all in build/proof/redesign/.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "Presets.h"
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
    p.runHousekeeping();
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

juce::File redesignDir()
{
    return proofDir().getChildFile ("redesign");
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> at, int modifiers = juce::ModifierKeys::leftButtonModifier)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (modifiers), juce::MouseInputSource::defaultPressure, 0.0f,
                             0.0f, 0.0f, 0.0f, &c, &c, now, at, now, 1, false);
}

/// A click as the mouse makes it: down and up on the component at a point (its own coordinates).
void click (juce::Component& c, juce::Point<float> at)
{
    c.mouseDown (mouseEvent (c, at));
    c.mouseUp (mouseEvent (c, at));
}

/// Plays guitar through the processor for `seconds`, in buffers.
void play (AmpSimProcessor& p, double seconds)
{
    const auto input = guitarDI ((int) (seconds * fs));
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
    }
}

/// Every parameter ID tagged on a component or inside it.
juce::StringArray taggedIn (juce::Component& c)
{
    juce::StringArray ids;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& x)
    {
        if (const auto id = x.getProperties()[ui::parameterIdProperty].toString(); id.isNotEmpty())
            ids.addIfNotAlreadyThere (id);
        for (auto* child : x.getChildren())
            walk (*child);
    };
    walk (c);
    return ids;
}

/// The accent's lit pixels in an area of an image (device pixels): how many, and their mean x.
std::pair<int, double> litIn (const juce::Image& image, juce::Rectangle<int> area)
{
    area = area.getIntersection (image.getBounds());
    int count = 0;
    double sum = 0.0;
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
        {
            const auto c = image.getPixelAt (x, y);
            const auto hue = c.getHue() * 360.0f;
            if (hue > 15.0f && hue < 50.0f && c.getRed() > c.getBlue() + 80 && c.getBrightness() > 0.55f)
            {
                ++count;
                sum += x + 0.5;
            }
        }
    return { count, count > 0 ? sum / count : -1.0 };
}

class MainScreenTests final : public juce::UnitTest
{
public:
    MainScreenTests() : juce::UnitTest ("Main screen", "ampsim") {}

    void runTest() override
    {
        beginTest ("theme: the orange accent and the studio grounds, set once in Theme.h; red for clips only");
        {
            using namespace ui::theme;
            expect (accent == juce::Colour (0xffff8a00));
            expect (bg == juce::Colour (0xff121212) && panelBg == juce::Colour (0xff1a1a1a) && raised == juce::Colour (0xff1f1f1f));
            expect (line1 == juce::Colour (0xff2c2c2c) && ink == juce::Colour (0xffe8e8e8) && inkDim == juce::Colour (0xff9a9a9a));
            expect (knobSkin (Skin::chrome).track == line2);
            // One accent (RD4-8): no head's knob pointer is teal or emerald. Monolith's and Comet's are ink, Forge's the
            // accent; every pointer is neutral (its saturation under 0.15) or the accent itself.
            expect (knobSkin (Skin::monolith).pointer == ink && knobSkin (Skin::comet).pointer == ink);
            expect (knobSkin (Skin::forge).pointer == accent);
            for (int k = 0; k <= (int) Skin::custom; ++k)
            {
                const auto pointer = knobSkin ((Skin) k).pointer;
                const auto hue = pointer.getHue() * 360.0f;
                const auto tealOrGreen = pointer.getSaturation() > 0.2f && hue > 90.0f && hue < 200.0f;
                expect (! tealOrGreen && (pointer == accent || pointer.getSaturation() < 0.5f || hue < 40.0f), "skin " + juce::String (k) + " #" + pointer.toDisplayString (false));
            }

            // A latched clip is drawn red on the meter; nothing else is.
            ui::LevelMeter meter ("Out", 2);
            meter.setBounds (0, 0, meter.getPreferredWidth(), 14);
            const float hot[] { 1.0f, 1.0f };
            meter.push (hot, 0.02);
            const auto image = meter.createComponentSnapshot (meter.getLocalBounds(), true, 4.0f);
            const auto capPixel = image.getPixelAt (image.getWidth() - 4, image.getHeight() / 2);
            expect (meter.isClipped());
            expect (capPixel.getRed() > 180 && capPixel.getGreen() < 120, capPixel.toDisplayString (true));
            logMessage ("  -> accent #" + accent.toDisplayString (false) + ", ground #" + bg.toDisplayString (false) + ", panels #" + panelBg.toDisplayString (false)
                        + ", raised #" + raised.toDisplayString (false) + ", hairlines #" + line1.toDisplayString (false) + "; a clip's cap reads #"
                        + capPixel.toDisplayString (false));
        }

        beginTest ("layout: the top bar, the stage over the board, the inspector at the right, the footer; the amp selected by default");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            using namespace ui::theme;
            expect (ed.getTopBar().getBounds() == juce::Rectangle<int> (0, 0, canvasWidth, topBarHeight));
            expect (ed.getStage().getBounds() == juce::Rectangle<int> (0, topBarHeight, 980, stageHeight));
            expect (ed.getBoard().getBounds() == juce::Rectangle<int> (0, topBarHeight + stageHeight, 980, boardHeight));
            expect (ed.getInspector().getBounds() == juce::Rectangle<int> (980, topBarHeight, 300, canvasHeight - topBarHeight - footerHeight));
            expect (ed.getScenesBar().getBottom() <= canvasHeight && ed.getScenesBar().getY() >= canvasHeight - footerHeight);
            expect (ed.getSelectedBlock() == ui::BlockId::amp && ed.getStageMode() == ui::StageMode::amp);
            expect (ed.getInspector().getContent() == &ed.getAmpView().getPanel());
            expect (ed.getBoard().getSelected() == ui::BlockId::amp);

            // Every pedal on the board, in three tabs (RD5-1): the pre section; the amp, Gate B, and the cab; the post
            // section. Pre FX is open by default. Each tab's gear is on one row under the strip, left to right in chain
            // order, inside the board, at a scale larger than the two-row board's 0.665.
            auto& board = ed.getBoard();
            expect (board.getTab() == ui::BoardTab::pre, "Pre FX by default");
            int pedals = 0;
            for (int b = 0; b < ui::numBlocks; ++b)
                if (board.getPedal ((ui::BlockId) b) != nullptr)
                    ++pedals;
            expectEquals (pedals, 15);
            juce::StringArray rows;
            for (int t = 0; t < ui::numBoardTabs; ++t)
            {
                const auto tab = (ui::BoardTab) t;
                board.setTab (tab);
                const auto blocks = board.getTabBlocks (tab);
                juce::StringArray names;
                const juce::Component* previous = nullptr;
                for (auto id : blocks)
                {
                    auto* c = board.getPedal (id) != nullptr ? board.getPedal (id) : board.getMarker (id);
                    expect (c != nullptr && c->isVisible() && board.isShown (id), ui::info (id).name);
                    expect (board.getLocalBounds().contains (c->getBounds()), ui::info (id).name + juce::String (" inside the board"));
                    expect (c->getY() >= board.getTabStrip().getBottom(), "under the strip");
                    if (previous != nullptr)
                    {
                        expect (previous->getRight() <= c->getX(), "in chain order, not overlapping");
                        expectEquals (c->getY(), previous->getY(), "one row");
                    }
                    previous = c;
                    names.add (id == ui::BlockId::amp ? juce::String ("the amp") : id == ui::BlockId::cab ? juce::String ("the cab") : juce::String (ui::info (id).name));
                }
                expectGreaterThan (board.getPedalScale (tab), 0.75f, ui::boardTabLabel (tab));
                rows.add (juce::String (ui::boardTabLabel (tab)) + " (" + names.joinIntoString (", ") + ") at " + juce::String (board.getPedalScale (tab), 3));
            }
            expect (board.getTabBlocks (ui::BoardTab::ampCab) == std::vector<ui::BlockId> { ui::BlockId::amp, ui::BlockId::gateB, ui::BlockId::cab });
            board.setTab (ui::BoardTab::pre);
            logMessage ("  -> " + rows.joinIntoString ("; ") + " of their size; the strip " + board.getTabStrip().getBounds().toString());
        }

        beginTest ("selecting a pedal: a click on it shows its every control in the inspector and raises it; the amp's knobs by default");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();

            // The amp's inspector holds only the amp (RD4-9): its seven knobs (the playing amp's), its switch, and the
            // levels in and out of the chain round it. No Gate A: it's its own pedal, which still reaches it.
            const auto ampIds = taggedIn (ed.getInspector());
            for (const auto* id : { "amp1_input_trim", "amp1_output_trim", "amp1_bass", "amp1_mid", "amp1_treble", "amp1_presence", "amp1_depth", "input_gain",
                                    "output_gain", "amp_bypass" })
                expect (ampIds.contains (id), id);
            for (const auto& id : ampIds)
                expect (id.startsWith ("amp") || id == "input_gain" || id == "output_gain", "not the amp's: " + id);
            expectEquals (ampIds.size(), 7 * AmpSimProcessor::numAmps + 3, "the amps' seven knobs each (only the playing amp's shown), the switch, in and out");

            // A click on the delay pedal (above its footswitch), on the Post FX tab: its page in the inspector, nothing
            // switched.
            click (board.getTabStrip(), board.getTabArea (ui::BoardTab::post).getCentre());
            expect (board.isShown (ui::BlockId::delay));
            auto& delay = *board.getPedal (ui::BlockId::delay);
            const auto foot = board.getFootswitchArea (ui::BlockId::delay);
            click (delay, { foot.getCentreX(), foot.getY() - 30.0f });
            expect (ed.getSelectedBlock() == ui::BlockId::delay && board.getSelected() == ui::BlockId::delay);
            expect (ed.getInspector().getContent() == &ed.getPage (ui::BlockId::delay));
            expectEquals (getParam (p, "delay_on"), 0.0f);
            const auto delayIds = taggedIn (ed.getInspector());
            for (const auto* id : { "delay_on", "delay_mode", "delay_stereo", "delay_sync", "delay_time", "delay_feedback", "delay_mix", "tempo_bpm" })
                expect (delayIds.contains (id), id);
            expect (! delayIds.contains ("amp1_input_trim"), "only the selected block's controls");
            expectEquals (p.parameters.state.getProperty ("uiSelected").toString(), juce::String ("delay"));

            // Every pedal: its page holds a control for every parameter under its prefix.
            juce::StringArray counts;
            for (int b = 0; b < ui::numBlocks; ++b)
            {
                const auto id = (ui::BlockId) b;
                if (board.getPedal (id) == nullptr)
                    continue;
                ed.selectBlock (id);
                const auto ids = taggedIn (ed.getInspector());
                expect (ids.contains (ui::info (id).onParameter), ui::info (id).name);
                // The column fits the inspector's width, and the inspector scrolls when it's taller.
                auto& content = *ed.getInspector().getContent();
                expectEquals (content.getWidth(), ed.getInspector().getViewport().getWidth() - ui::Inspector::scrollBarWidth);
                counts.add (juce::String (ui::info (id).pageName) + " " + juce::String (ids.size()) + " (" + juce::String (content.getHeight()) + " px)");
            }
            expectEquals (ed.getPage (ui::BlockId::reverb).getGenericCount(), 0); // the pages cover their blocks themselves

            // The amp marker selects the amp, and the stage shows it; the cab marker the cab (on the Amp & Cab tab).
            click (board.getTabStrip(), board.getTabArea (ui::BoardTab::ampCab).getCentre());
            click (*board.getMarker (ui::BlockId::cab), { 30.0f, 40.0f });
            ed.finishAnimations();
            expect (ed.getSelectedBlock() == ui::BlockId::cab && ed.getStageMode() == ui::StageMode::cab);
            expect (ed.getInspector().getContent() == &ed.getCabView().getPanel());
            click (*board.getMarker (ui::BlockId::amp), { 30.0f, 40.0f });
            ed.finishAnimations();
            expect (ed.getSelectedBlock() == ui::BlockId::amp && ed.getStageMode() == ui::StageMode::amp);
            logMessage ("  -> a click on the delay pedal put its " + juce::String (delayIds.size()) + " controls in the inspector; per pedal (tagged controls, "
                        "column height): " + counts.joinIntoString (", ") + "; the cab and amp markers select them and move the stage");
        }

        beginTest ("footswitches: a click on a pedal's footswitch flips its switch as one undo step; its LED follows, live; a section's switch dims its row");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();
            auto& od = *board.getPedal (ui::BlockId::overdrive);
            const auto foot = board.getFootswitchArea (ui::BlockId::overdrive);
            expect (od.getLocalBounds().toFloat().contains (foot) && foot.getY() > (float) od.getHeight() * 0.5f, foot.toString());

            p.parameters.copyState();
            p.undoManager.clearUndoHistory();
            const auto selectedBefore = ed.getSelectedBlock();
            ed.mouseDown (mouseEvent (od, foot.getCentre())); // the editor hears the press too, and starts the step
            click (od, foot.getCentre());
            const auto on = getParam (p, "od_on");
            board.refresh();
            const auto lit = board.isLit (ui::BlockId::overdrive);
            expectEquals (on, 1.0f);
            expect (lit);
            expect (ed.getSelectedBlock() == selectedBefore, "a footswitch doesn't select");

            // One undo step: undo turns it off again, and the LED follows.
            p.parameters.copyState();
            expect (p.undoManager.undo());
            board.refresh();
            expectEquals (getParam (p, "od_on"), 0.0f);
            expect (! board.isLit (ui::BlockId::overdrive));
            expect (p.undoManager.redo());
            expectEquals (getParam (p, "od_on"), 1.0f);

            // The LEDs tell the truth whoever moves the switch: a scene, MIDI, a preset; refresh reads them.
            setParam (p, "reverb_on", 1.0f);
            board.refresh();
            expect (board.isLit (ui::BlockId::reverb));
            setParam (p, "reverb_on", 0.0f);
            board.refresh();
            expect (! board.isLit (ui::BlockId::reverb));

            // A bypassed pedal is dimmed (RD4-1), and lights up whole when it's on: the overdrive on, the reverb off.
            expect (! board.isDimmed (ui::BlockId::overdrive) && board.isDimmed (ui::BlockId::reverb));
            const auto brightness = [&] (ui::BlockId id)
            {
                // The pedal's mean brightness (HSB), away from its LED and its halo: the enclosure itself.
                auto& pedal = *board.getPedal (id);
                const auto image = pedal.createComponentSnapshot (pedal.getLocalBounds(), true, 2.0f);
                const auto fit = ui::pedalBounds (pedal.getLocalBounds().toFloat(), id);
                const auto k = fit.getWidth() / ui::pedalSize (id).x;
                const auto glow = ui::pedalLayout (id).ledGlow;
                const auto halo = juce::Rectangle<float> (fit.getX() + glow.getX() * k, fit.getY() + glow.getY() * k, glow.getWidth() * k, glow.getHeight() * k) * 2.0f;
                const auto body = ui::pedalLayout (id).body;
                const auto inside = (juce::Rectangle<float> (fit.getX() + body.getX() * k, fit.getY() + body.getY() * k, body.getWidth() * k, body.getHeight() * k) * 2.0f).reduced (6.0f);
                double sum = 0.0, saturation = 0.0;
                int n = 0;
                for (int y = (int) inside.getY(); y < (int) inside.getBottom(); ++y)
                    for (int x = (int) inside.getX(); x < (int) inside.getRight(); ++x)
                        if (! halo.contains ((float) x, (float) y))
                        {
                            const auto c = image.getPixelAt (x, y);
                            sum += c.getBrightness();
                            saturation += c.getSaturation();
                            ++n;
                        }
                return std::pair<double, double> { sum / juce::jmax (1, n), saturation / juce::jmax (1, n) };
            };
            const auto odOn = brightness (ui::BlockId::overdrive);
            setParam (p, "od_on", 0.0f);
            board.refresh();
            const auto odOff = brightness (ui::BlockId::overdrive);
            expect (board.isDimmed (ui::BlockId::overdrive) && ! board.isLit (ui::BlockId::overdrive));
            expectWithinAbsoluteError (odOff.first / odOn.first, (double) ui::dimBrightness, 0.06, "off at about 55% brightness");
            expectLessThan (odOff.second, odOn.second * 0.7, "off at lower saturation");
            setParam (p, "od_on", 1.0f);
            board.refresh();

            // The post section's switch dims its row the same way, and each pedal keeps its own LED: the chorus is on,
            // so it's lit and dimmed; the boost, before the amp, isn't touched.
            setParam (p, "chorus_on", 1.0f);
            setParam (p, "post_fx_on", 0.0f);
            board.refresh();
            expect (board.isDimmed (ui::BlockId::chorus) && board.isLit (ui::BlockId::chorus));
            expect (board.isDimmed (ui::BlockId::delay) && ! board.isDimmed (ui::BlockId::overdrive));
            expect (juce::exactlyEqual (board.getPedal (ui::BlockId::chorus)->getAlpha(), 1.0f), "dimmed by the art, not by fading over the tray");
            const auto chorusInSectionOff = brightness (ui::BlockId::chorus);
            setParam (p, "post_fx_on", 1.0f);
            board.refresh();
            const auto chorusOn = brightness (ui::BlockId::chorus);
            expectWithinAbsoluteError (chorusInSectionOff.first / chorusOn.first, (double) ui::dimBrightness, 0.06, "a section that's off: the same dimming");
            setParam (p, "chorus_on", 0.0f);
            board.refresh();
            logMessage ("  -> the overdrive's enclosure off: " + juce::String (odOff.first / odOn.first, 2) + " of its brightness on, saturation "
                        + juce::String (odOff.second, 2) + " from " + juce::String (odOn.second, 2) + "; the chorus in a section that's off: "
                        + juce::String (chorusInSectionOff.first / chorusOn.first, 2) + " of its brightness, its LED still lit");

            // The sections' switches are labelled toggles (RD4-3): ON or OFF beside the pill, under the row's name.
            for (auto section : { ui::Section::pre, ui::Section::post })
            {
                auto* toggle = board.getSectionSwitch (section);
                expect (toggle != nullptr && toggle->isLabelled() && toggle->getWidth() >= ui::PowerSwitch::labelledWidth, "a labelled toggle");
            }

            // A right-click on a pedal is MIDI learn for its switch (it's tagged with it), and never flips it.
            expectEquals (od.getProperties()[ui::parameterIdProperty].toString(), juce::String ("od_on"));
            od.mouseDown (mouseEvent (od, foot.getCentre(), juce::ModifierKeys::rightButtonModifier));
            od.mouseUp (mouseEvent (od, foot.getCentre(), juce::ModifierKeys::rightButtonModifier));
            expectEquals (getParam (p, "od_on"), 1.0f);

            // The board at its real size with a few pedals on and the delay selected (raised), for the eye.
            for (const auto* id : { "gate_a_on", "comp_post_on", "delay_on", "reverb_on", "chorus_on" })
                setParam (p, id, 1.0f);
            ed.selectBlock (ui::BlockId::delay);
            board.refresh();
            expect (savePng (board.createComponentSnapshot (board.getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("board.png")));
            const auto delayBox = board.getPedal (ui::BlockId::delay)->getBounds().expanded (12);
            expect (savePng (board.createComponentSnapshot (delayBox, true, 3.0f), redesignDir().getChildFile ("board_delay_crop.png")));
            logMessage ("  -> a click on the overdrive's footswitch (" + foot.toString() + " in the pedal) switched it on, LED lit, selection unchanged; one undo "
                        "switched it off; the LEDs followed the reverb's switch both ways; the post switch dimmed the bottom row; a right-click flipped nothing; "
                        "build/proof/redesign/board.png, board_delay_crop.png");
        }

        beginTest ("board tabs: a click on a tab shows its blocks and hides the others; the LED strips mirror every block's switch and its section's, open or not");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();
            auto& strip = board.getTabStrip();

            // Each tab by a click anywhere on it (its name, or its LEDs): its blocks show and every other tab's hide; the
            // pre and post sections' switches show on their own tabs only. The open tab is app state, not a parameter.
            const auto paramsBefore = p.getParameters().size();
            board.setTab (ui::BoardTab::post); // so that the first click (Pre FX) changes the tab
            juce::StringArray shown;
            for (int t = 0; t < ui::numBoardTabs; ++t)
            {
                const auto tab = (ui::BoardTab) t;
                const auto area = board.getTabArea (tab);
                const auto at = t == 1 ? board.getTabLedCentre (tab, 1) : juce::Point<float> (area.getX() + 20.0f, area.getCentreY());
                click (strip, at);
                expect (board.getTab() == tab, ui::boardTabLabel (tab));
                int visible = 0;
                for (int b = 0; b < ui::numBlocks; ++b)
                {
                    const auto id = (ui::BlockId) b;
                    if (board.getPedal (id) == nullptr && board.getMarker (id) == nullptr)
                        continue;
                    expect (board.isShown (id) == (ui::boardTabFor (id) == tab), juce::String (ui::boardTabLabel (tab)) + ": " + ui::info (id).name);
                    visible += board.isShown (id) ? 1 : 0;
                }
                expectEquals (visible, (int) board.getTabBlocks (tab).size());
                expect (board.getSectionSwitch (ui::Section::pre)->isVisible() == (tab == ui::BoardTab::pre));
                expect (board.getSectionSwitch (ui::Section::post)->isVisible() == (tab == ui::BoardTab::post));
                expectEquals (p.parameters.state.getProperty ("uiBoardTab").toString(), juce::String (ui::boardTabName (tab)));
                shown.add (juce::String (ui::boardTabLabel (tab)) + " " + juce::String (visible));
            }
            expectEquals (p.getParameters().size(), paramsBefore);
            // A right-click on the strip opens nothing.
            strip.mouseDown (mouseEvent (strip, board.getTabArea (ui::BoardTab::pre).getCentre(), juce::ModifierKeys::rightButtonModifier));
            strip.mouseUp (mouseEvent (strip, board.getTabArea (ui::BoardTab::pre).getCentre(), juce::ModifierKeys::rightButtonModifier));
            expect (board.getTab() == ui::BoardTab::post);

            // The LEDs, from the parameters, on every tab (the Post FX tab is open): the overdrive and the delay on, the
            // cab off, Gate B on; then each section switched off.
            for (const auto* id : { "gate_a_on", "comp_pre_on", "boost_on", "eq_pre_on", "eq_post_on", "comp_post_on", "harm_on", "mv_on", "bloom_on", "chorus_on",
                                    "reverb_on" })
                setParam (p, id, 0.0f);
            setParam (p, "od_on", 1.0f);
            setParam (p, "delay_on", 1.0f);
            setParam (p, "gate_b_on", 1.0f);
            setParam (p, "amp_bypass", 0.0f);
            setParam (p, "cab_bypass", 1.0f);
            board.refresh();
            const auto ledsSay = [&board] (ui::BoardTab t)
            {
                juce::String text;
                for (const auto& led : board.getTabLeds (t))
                    text << (led.dimmed ? (led.lit ? "o" : "_") : (led.lit ? "O" : "."));
                return text;
            };
            const auto check = [&] (const juce::String& when)
            {
                for (int t = 0; t < ui::numBoardTabs; ++t)
                {
                    const auto tab = (ui::BoardTab) t;
                    const auto leds = board.getTabLeds (tab);
                    const auto blocks = board.getTabBlocks (tab);
                    expectEquals ((int) leds.size(), (int) blocks.size());
                    for (size_t i = 0; i < leds.size(); ++i)
                    {
                        const auto id = blocks[i];
                        const auto& bi = ui::info (id);
                        const auto parameter = id == ui::BlockId::amp ? juce::String ("amp_bypass") : juce::String (bi.onParameter);
                        const auto inverted = id == ui::BlockId::amp || bi.switchInverted;
                        const auto on = (getParam (p, parameter) >= 0.5f) != inverted;
                        const auto sectionOff = (bi.section == ui::Section::pre && getParam (p, "pre_fx_on") < 0.5f)
                                                || (bi.section == ui::Section::post && getParam (p, "post_fx_on") < 0.5f);
                        expect (leds[i].id == id && leds[i].lit == on && leds[i].dimmed == sectionOff, when + ": " + bi.name);
                    }
                }
            };
            check ("both sections on");
            const auto allOn = ledsSay (ui::BoardTab::pre) + " | " + ledsSay (ui::BoardTab::ampCab) + " | " + ledsSay (ui::BoardTab::post);
            expectEquals (ledsSay (ui::BoardTab::ampCab), juce::String ("OO."));
            setParam (p, "pre_fx_on", 0.0f);
            board.refresh();
            check ("the pre section off");
            // Gate B isn't in the pre section (the chain bypasses it by gate_b_on alone): neither its LED nor its pedal dims.
            expect (! board.getTabLeds (ui::BoardTab::ampCab)[1].dimmed && ! board.isDimmed (ui::BlockId::gateB));
            expect (board.isDimmed (ui::BlockId::overdrive) && board.isLit (ui::BlockId::overdrive));
            setParam (p, "post_fx_on", 0.0f);
            board.refresh();
            check ("both sections off");
            const auto bothOff = ledsSay (ui::BoardTab::pre) + " | " + ledsSay (ui::BoardTab::ampCab) + " | " + ledsSay (ui::BoardTab::post);

            // As drawn: a lit LED is orange, an off one isn't, a dimmed lit one is a darker orange, on a tab that isn't
            // open (Pre FX, with the Post FX tab open).
            const auto image = strip.createComponentSnapshot (strip.getLocalBounds(), true, 2.0f);
            const auto pixel = [&] (ui::BoardTab t, int index) { return image.getPixelAt (juce::roundToInt (board.getTabLedCentre (t, index).x * 2.0f), juce::roundToInt (board.getTabLedCentre (t, index).y * 2.0f)); };
            const auto preOrder = board.getTabBlocks (ui::BoardTab::pre);
            const auto odIndex = (int) (std::find (preOrder.begin(), preOrder.end(), ui::BlockId::overdrive) - preOrder.begin());
            const auto boostIndex = (int) (std::find (preOrder.begin(), preOrder.end(), ui::BlockId::boost) - preOrder.begin());
            const auto litDimmed = pixel (ui::BoardTab::pre, odIndex), off = pixel (ui::BoardTab::pre, boostIndex);
            const auto litAmp = pixel (ui::BoardTab::ampCab, 0), offCab = pixel (ui::BoardTab::ampCab, 2);
            const auto orange = [] (juce::Colour c) { return c.getRed() > 90 && c.getRed() > c.getBlue() + 60 && c.getHue() * 360.0f > 15.0f && c.getHue() * 360.0f < 50.0f; };
            expect (orange (litAmp) && litAmp.getBrightness() > 0.85f, litAmp.toDisplayString (false));
            expect (orange (litDimmed) && litDimmed.getBrightness() < litAmp.getBrightness() * 0.8f, litDimmed.toDisplayString (false));
            expect (! orange (off) && ! orange (offCab) && off.getBrightness() < 0.3f, off.toDisplayString (false) + " " + offCab.toDisplayString (false));
            setParam (p, "pre_fx_on", 1.0f);
            setParam (p, "post_fx_on", 1.0f);
            board.refresh();
            logMessage ("  -> tabs clicked, blocks shown: " + shown.joinIntoString (", ") + " (no parameter added); the LEDs (O lit, . off, o lit and dimmed, _ off "
                        "and dimmed), Pre | Amp & Cab | Post: " + allOn + " with both sections on, " + bothOff + " with both off; drawn on the closed Pre FX tab: "
                        "the overdrive's (lit, section off) #" + litDimmed.toDisplayString (false) + ", the boost's (off) #" + off.toDisplayString (false)
                        + "; the amp's (lit) #" + litAmp.toDisplayString (false) + ", the cab's (off) #" + offCab.toDisplayString (false));
        }

        beginTest ("board tabs: a selection from outside the board opens its tab (the stage's Amp, Cab, and EQ, the EQ view's choice, code); the tab is "
                   "remembered when the editor reopens");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();
            auto& control = ed.getStage().getModeControl();
            juce::StringArray seen;
            const auto saw = [&] (const juce::String& what) { seen.add (what + ": " + ui::boardTabLabel (board.getTab())); };

            expect (board.getTab() == ui::BoardTab::pre && ed.getSelectedBlock() == ui::BlockId::amp, "first start: the amp selected, Pre FX open");
            click (control, control.optionArea (1).getCentre()); // the stage's Cab
            expect (ed.getSelectedBlock() == ui::BlockId::cab && board.getTab() == ui::BoardTab::ampCab);
            saw ("the stage's Cab");
            ed.selectBlock (ui::BlockId::delay);
            expect (board.getTab() == ui::BoardTab::post && board.isShown (ui::BlockId::delay));
            saw ("the delay selected in code");
            click (control, control.optionArea (0).getCentre()); // the stage's Amp
            expect (ed.getSelectedBlock() == ui::BlockId::amp && board.getTab() == ui::BoardTab::ampCab);
            saw ("the stage's Amp");
            // The stage's EQ selects the EQ pedal it shows (the post EQ first), and the board opens that pedal's tab
            // (RD5-5): the EQ view's choice moves it to the pre EQ's tab.
            click (control, control.optionArea (2).getCentre());
            expect (ed.getSelectedBlock() == ui::BlockId::postEq && board.getTab() == ui::BoardTab::post);
            saw ("the stage's EQ");
            auto& choice = ed.getEqStage().getChoice();
            click (choice, choice.optionArea (1).getCentre());
            expect (ed.getSelectedBlock() == ui::BlockId::preEq && board.getTab() == ui::BoardTab::pre, ui::boardTabLabel (board.getTab()));
            saw ("the EQ view's Pre");
            ed.selectBlock (ui::BlockId::gateB);
            expect (board.getTab() == ui::BoardTab::ampCab);
            saw ("Gate B selected in code");

            // A tab picked by hand stays until the selection moves, and the tab open when the editor closes is the one it
            // opens on, whatever is selected.
            click (board.getTabStrip(), board.getTabArea (ui::BoardTab::post).getCentre());
            expect (board.getTab() == ui::BoardTab::post && ed.getSelectedBlock() == ui::BlockId::gateB, "a tab picked by hand keeps the selection");
            editor.reset();
            std::unique_ptr<juce::AudioProcessorEditor> again (p.createEditor());
            auto& ed2 = dynamic_cast<AmpSimEditor&> (*again);
            expect (ed2.getBoard().getTab() == ui::BoardTab::post && ed2.getSelectedBlock() == ui::BlockId::gateB);
            logMessage ("  -> " + seen.joinIntoString ("; ") + "; Post FX picked by hand with Gate B selected, and a new editor opened on Post FX with Gate B "
                        "still selected");
        }

        beginTest ("board tabs: on Pre FX and Post FX a drag reorders the section, a footswitch toggles as one undo step with its LED's bloom, a right-click is MIDI learn");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();
            juce::StringArray log;

            for (const auto tab : { ui::BoardTab::pre, ui::BoardTab::post })
            {
                click (board.getTabStrip(), board.getTabArea (tab).getCentre());
                const auto section = tab == ui::BoardTab::pre ? ampsim::Chain::Section::pre : ampsim::Chain::Section::post;
                const auto order = board.getTabBlocks (tab);
                const auto first = order[0], second = order[1];

                // The first pedal dragged onto the second's place: the section's order changes (the chain's dip applies it).
                auto& pedal = *board.getPedal (first);
                expect (pedal.isVisible());
                const auto dx = (float) (board.getPedal (second)->getBounds().getCentreX() - pedal.getBounds().getCentreX());
                board.dragPedal (first, dx);
                const auto saved = p.getSectionOrder (section);
                expect (saved[0] == juce::String (ui::info (second).orderName) && saved[1] == juce::String (ui::info (first).orderName), saved.joinIntoString (", "));
                expect (board.getTabBlocks (tab)[1] == first && board.getPedal (first)->getX() > board.getPedal (second)->getX());
                play (p, 0.05);
                expect (p.getChain().getAppliedOrder (section).size() == order.size());

                // A footswitch click (the second pedal, now first): on, lit with its bloom, one undo step; the tab's LED
                // follows.
                const auto id = second;
                const auto parameter = juce::String (ui::info (id).onParameter);
                setParam (p, parameter, 0.0f);
                board.refresh();
                auto& foot = *board.getPedal (id);
                const auto area = board.getFootswitchArea (id);
                p.parameters.copyState();
                p.undoManager.clearUndoHistory();
                ed.mouseDown (mouseEvent (foot, area.getCentre()));
                click (foot, area.getCentre());
                board.refresh();
                expectEquals (getParam (p, parameter), 1.0f, parameter);
                expect (board.isLit (id) && board.isGlowing (id), "lit, blooming");
                expect (board.getTabLeds (tab)[0].id == id && board.getTabLeds (tab)[0].lit);
                p.parameters.copyState();
                expect (p.undoManager.undo());
                board.refresh();
                expectEquals (getParam (p, parameter), 0.0f, "one undo step");
                expect (! board.getTabLeds (tab)[0].lit);

                // A right-click: MIDI learn for its switch (it's tagged), nothing flipped, the tab kept.
                expectEquals (foot.getProperties()[ui::parameterIdProperty].toString(), parameter);
                foot.mouseDown (mouseEvent (foot, area.getCentre(), juce::ModifierKeys::rightButtonModifier));
                foot.mouseUp (mouseEvent (foot, area.getCentre(), juce::ModifierKeys::rightButtonModifier));
                expectEquals (getParam (p, parameter), 0.0f);
                expect (board.getTab() == tab);
                log.add (juce::String (ui::boardTabLabel (tab)) + ": " + ui::info (first).name + " dragged after " + ui::info (second).name + " (" + saved.joinIntoString (", ")
                         + "), " + parameter + " on by its footswitch and off by one undo, tagged " + parameter);
            }
            logMessage ("  -> " + log.joinIntoString ("; "));
        }

        beginTest ("the auto-wah on the board: on Pre FX after Gate A with its LED on the strip, the row still fitting at 1.18; its sweep window follows the "
                   "filter while it plays and rests dark when it's off or its section is");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();
            using B = ui::BlockId;

            // Where it stands: the second pedal and the second LED of Pre FX, the row's six pedals still at 1.18 and inside
            // the board, with the cable's "to Amp & Cab" room after them.
            const auto pre = board.getTabBlocks (ui::BoardTab::pre);
            expect (pre == std::vector<B> { B::gateA, B::wah, B::preCompressor, B::boost, B::overdrive, B::preEq });
            expect (board.getTabLeds (ui::BoardTab::pre).size() == 6 && board.getTabLeds (ui::BoardTab::pre)[1].id == B::wah);
            expectWithinAbsoluteError (board.getPedalScale (ui::BoardTab::pre), 1.18f, 0.01f);
            const auto lastRight = board.getPedal (pre.back())->getRight();
            expect (lastRight <= board.getWidth() - ui::Board::sidePad, "the row inside the board");
            const auto room = board.getWidth() - ui::Board::sidePad - lastRight;
            const auto onward = ui::theme::textWidth (ui::theme::geist (ui::theme::Weight::regular, 12.0f), "to Amp & Cab");
            expectGreaterThan ((float) room, onward + 12.0f, "room for the cable's name after the row");
            expect (board.getTab() == ui::BoardTab::pre);

            // Selecting it from another tab opens Pre FX, and a click on it (not its footswitch) selects it.
            ed.selectBlock (B::delay);
            expect (board.getTab() == ui::BoardTab::post && ! board.isShown (B::wah));
            ed.selectBlock (B::wah);
            expect (board.getTab() == ui::BoardTab::pre && board.isShown (B::wah) && ed.getInspector().getContent() == &ed.getPage (B::wah));
            ed.selectBlock (B::gateA);
            board.clickPedal (B::wah, false);
            expect (ed.getSelectedBlock() == B::wah);
            ed.selectBlock (B::gateA); // so the pedal sits unraised for the pixels below

            // Off by default: the window rests dark.
            board.refresh();
            expect (! board.isLit (B::wah) && board.getSweep() < 0.0f);

            // On and playing: refreshed 30 times a second (every 12 buffers), the window shows what the filter reports, to
            // within the step it redraws at (a 500th of the window, under a pixel), and it moves.
            setParam (p, "wah_on", 1.0f);
            board.refresh();
            expect (board.isLit (B::wah) && board.getSweep() >= 0.0f);
            const auto input = guitarDI ((int) (2.0 * fs));
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            float mismatch = 0.0f, lowest = 1.0f, highest = 0.0f;
            int reads = 0;
            for (size_t start = 0, n = 0; start + blockSize <= input.size(); start += blockSize, ++n)
            {
                buffer.clear();
                buffer.copyFrom (0, 0, input.data() + start, blockSize);
                p.processBlock (buffer, midi);
                if (n % 12 == 11)
                {
                    board.refresh();
                    ++reads;
                    mismatch = juce::jmax (mismatch, std::abs (board.getSweep() - juce::jlimit (0.0f, 1.0f, p.getWahControl())));
                    if (start > (size_t) (0.2 * fs))
                    {
                        lowest = juce::jmin (lowest, board.getSweep());
                        highest = juce::jmax (highest, board.getSweep());
                    }
                }
            }
            expectLessThan (mismatch, ui::Board::sweepStep, "the window shows the filter's control");
            expectGreaterThan (highest - lowest, 0.15f, "it moves while playing");

            // As drawn: the lamp sits further right for a higher control (a value set through the board's seam, as the
            // meters' tests push readings).
            auto& pedal = *board.getPedal (B::wah);
            const auto fit = ui::pedalBounds (pedal.getLocalBounds().toFloat(), B::wah);
            const auto k = fit.getWidth() / ui::pedalSize (B::wah).x;
            const auto layout = ui::pedalLayout (B::wah);
            const auto glass = (juce::Rectangle<float> (fit.getX() + layout.sweep.getX() * k, fit.getY() + layout.sweep.getY() * k, layout.sweep.getWidth() * k,
                                                        layout.sweep.getHeight() * k)
                                * 2.0f)
                                   .toNearestInt();
            const auto litAt = [&] (float control)
            {
                board.setSweep (control);
                return litIn (pedal.createComponentSnapshot (pedal.getLocalBounds(), true, 2.0f), glass);
            };
            const auto low = litAt (0.1f), high = litAt (0.9f);
            expect (low.first > 10 && high.first > 10 && high.second > low.second + 0.5 * glass.getWidth(), juce::String (low.second) + " " + juce::String (high.second));

            // Silence: it closes (Up), and the window follows it down.
            for (int b = 0; b < (int) fs / blockSize; ++b)
            {
                buffer.clear();
                p.processBlock (buffer, midi);
            }
            board.refresh();
            const auto closed = board.getSweep();
            expectLessThan (closed, 0.02f, "closed in silence");
            expect (litIn (pedal.createComponentSnapshot (pedal.getLocalBounds(), true, 2.0f), glass).first > 10, "lit, resting at Low");

            // Switched off, or its section off: the window rests dark, whatever the control held.
            setParam (p, "wah_on", 0.0f);
            board.refresh();
            const auto offSweep = board.getSweep();
            const auto offLit = litIn (pedal.createComponentSnapshot (pedal.getLocalBounds(), true, 2.0f), glass).first;
            setParam (p, "wah_on", 1.0f);
            setParam (p, "pre_fx_on", 0.0f);
            board.refresh();
            const auto sectionSweep = board.getSweep();
            const auto sectionLit = litIn (pedal.createComponentSnapshot (pedal.getLocalBounds(), true, 2.0f), glass).first;
            expect (offSweep < 0.0f && offLit == 0, "off: dark");
            expect (sectionSweep < 0.0f && sectionLit == 0 && board.isLit (B::wah), "section off: dark, its LED still lit");
            setParam (p, "pre_fx_on", 1.0f);
            logMessage ("  -> Pre FX: " + juce::String ((int) pre.size()) + " pedals at " + juce::String (board.getPedalScale (ui::BoardTab::pre), 3) + ", " + juce::String (room)
                        + " px after the last for \"to Amp & Cab\" (" + juce::String (onward, 1) + " px of text); playing, " + juce::String (reads)
                        + " reads matched the filter's control within " + juce::String (mismatch, 7) + " and swept " + juce::String (lowest, 3) + " to "
                        + juce::String (highest, 3) + "; drawn at 0.1 and 0.9 the lamp's pixels centre " + juce::String (low.second - glass.getX(), 1) + " and "
                        + juce::String (high.second - glass.getX(), 1) + " px into the " + juce::String (glass.getWidth()) + " px window; after 1 s of silence it reads "
                        + juce::String (closed, 4) + " (lit at Low); off and section off: " + juce::String (offLit) + " and "
                        + juce::String (sectionLit) + " lit pixels");
        }

        beginTest ("keys: the arrows step the presets, 1 to 8 recall scenes, T opens and closes the tuner, Escape closes an overlay; a clicked knob keeps up and down only");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            const auto factory = presets::factoryPresets();

            expect (ed.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
            p.runHousekeeping();
            ed.refresh();
            const auto first = ed.getTopBar().getShownPresetName();
            expect (ed.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
            p.runHousekeeping();
            ed.refresh();
            const auto second = ed.getTopBar().getShownPresetName();
            expect (ed.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
            p.runHousekeeping();
            ed.refresh();
            expectEquals (first, factory[0]["name"].toString());
            expectEquals (second, factory[1]["name"].toString());
            expectEquals (ed.getTopBar().getShownPresetName(), first);

            // The factory preset has scenes: 2 recalls the second, and the top bar and the footer say so.
            expect (p.getScenes().get (1).stored);
            expect (ed.keyPressed (juce::KeyPress ('2', {}, '2')));
            ed.refresh();
            expectEquals (p.getScenes().getCurrent(), 1);
            expect (ed.getTopBar().getShownScene().startsWith ("Scene 2: "), ed.getTopBar().getShownScene());
            expectEquals (ed.getScenesBar().getLitScene(), 1);
            const auto sceneText = ed.getTopBar().getShownScene();
            // An empty scene's key does nothing (storing is a click, or Store).
            const auto emptyBefore = p.getScenes().get (7).stored;
            expect (ed.keyPressed (juce::KeyPress ('8', {}, '8')));
            expect (! emptyBefore && ! p.getScenes().get (7).stored && p.getScenes().getCurrent() == 1);

            // T: the tuner opens (and engages); T again closes it (and disengages).
            expect (ed.keyPressed (juce::KeyPress ('t', {}, 't')));
            expect (ed.getOverlay() == ui::Overlay::tuner && getParam (p, "tuner_on") >= 0.5f);
            expect (ed.keyPressed (juce::KeyPress ('t', {}, 't')));
            expect (ed.getOverlay() == ui::Overlay::none && getParam (p, "tuner_on") < 0.5f);

            // [ and ]: the board's previous and next tab, wrapping round (RD5-4).
            auto& board = ed.getBoard();
            expect (board.getTab() == ui::BoardTab::pre);
            juce::StringArray stepped;
            for (const auto key : { ']', ']', ']', '[', '[' })
            {
                expect (ed.keyPressed (juce::KeyPress (key, {}, (juce::juce_wchar) key)));
                stepped.add (juce::String::charToString ((juce::juce_wchar) key) + " " + ui::boardTabLabel (board.getTab()));
            }
            expect (board.getTab() == ui::BoardTab::ampCab, ui::boardTabLabel (board.getTab()));
            expect (board.isShown (ui::BlockId::gateB) && ! board.isShown (ui::BlockId::gateA) && ! board.isShown (ui::BlockId::delay));
            expectEquals (p.parameters.state.getProperty ("uiBoardTab").toString(), juce::String ("ampcab"));
            expect (ed.getSelectedBlock() == ui::BlockId::amp, "a tab key selects nothing");

            // Escape closes Settings; with nothing open it's left to the system.
            ed.openOverlay (ui::Overlay::settings);
            expect (ed.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
            expect (ed.getOverlay() == ui::Overlay::none);
            expect (! ed.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));

            // Cmd-Z still undoes; a knob clicked into focus takes up and down, and leaves left and right to the presets.
            auto& mix = ed.getAmpView().getKnob (p.getSelectedAmp(), 6);
            const auto before = mix.getValue();
            expect (! mix.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
            expectEquals (mix.getValue(), before);
            expect (mix.keyPressed (juce::KeyPress (juce::KeyPress::upKey)));
            expectGreaterThan (mix.getValue(), before);

            // The tooltips name the keys.
            expect (ed.getTopBar().getNextButton().getTooltip().contains ("Right arrow"));
            expect (ed.getTopBar().getTunerButton().getTooltip().contains ("(T)"));
            expect (dynamic_cast<juce::Button&> (ed.getScenesBar().getTile (1)).getTooltip().contains ("key 2"));
            logMessage ("  -> Right, Right, Left: \"" + first + "\", \"" + second + "\", \"" + first + "\"; 2 recalled \"" + sceneText
                        + "\" (lit in the footer); 8 on an empty scene did nothing; T opened and closed the tuner; the tab keys: " + stepped.joinIntoString (", ")
                        + "; Escape closed Settings; a clicked knob ignored Right and took Up");
        }

        beginTest ("the stage: Amp, Cab, and EQ on its segmented control, crossfading; the inspector follows; an EQ pedal picks the EQ shown");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& stage = ed.getStage();
            auto& control = stage.getModeControl();
            expect (control.isPillStyle() && control.getNumOptions() == 3);

            // A click on "Cab": the cab's view rises over the amp's, then the amp's goes; the inspector shows the cab.
            click (control, control.optionArea (1).getCentre());
            expect (ed.getStageMode() == ui::StageMode::cab);
            const auto fading = stage.isFading();
            const auto bothDuring = stage.getView (ui::StageMode::amp).isVisible() && stage.getView (ui::StageMode::cab).isVisible();
            const auto alphaDuring = stage.getView (ui::StageMode::cab).getAlpha();
            expect (fading && bothDuring && alphaDuring < 1.0f);
            ed.finishAnimations();
            expect (! stage.isFading() && ! stage.getView (ui::StageMode::amp).isVisible() && juce::exactlyEqual (stage.getView (ui::StageMode::cab).getAlpha(), 1.0f));
            expect (ed.getSelectedBlock() == ui::BlockId::cab && ed.getInspector().getContent() == &ed.getCabView().getPanel());
            expectEquals (p.parameters.state.getProperty ("uiStage").toString(), juce::String ("cab"));

            // The fade really runs on the timer: started again and left to the message loop, it ends by itself.
            click (control, control.optionArea (2).getCentre());
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            while (stage.isFading() && juce::Time::getMillisecondCounterHiRes() - t0 < 3000.0)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            const auto fadeMs = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (! stage.isFading());
            expect (ed.getStageMode() == ui::StageMode::eq && ed.getEqStage().isVisible() && ! ed.getCabView().isVisible());
            expect (ed.getSelectedBlock() == ui::BlockId::postEq && ! ed.getEqStage().isShowingPre());

            // The pre EQ pedal puts the EQ view on the pre EQ's graph; the view's own choice puts the inspector back.
            ed.selectBlock (ui::BlockId::preEq);
            expect (ed.getEqStage().isShowingPre() && ed.getEqStage().getGraph (true).isVisible() && ! ed.getEqStage().getGraph (false).isVisible());
            auto& choice = ed.getEqStage().getChoice();
            click (choice, choice.optionArea (0).getCentre());
            expect (! ed.getEqStage().isShowingPre() && ed.getSelectedBlock() == ui::BlockId::postEq);
            expect (p.getAnalyzerTap() == AmpSimProcessor::AnalyzerTap::postSection, "the EQ view reads its EQ's tap");

            // Back to the amp; restored at the next start (the stage last shown).
            ed.setStageMode (ui::StageMode::amp, false);
            expect (ed.getSelectedBlock() == ui::BlockId::amp);
            ed.setStageMode (ui::StageMode::cab, false);
            editor.reset();
            std::unique_ptr<juce::AudioProcessorEditor> again (p.createEditor());
            auto& ed2 = dynamic_cast<AmpSimEditor&> (*again);
            expect (ed2.getStageMode() == ui::StageMode::cab && ed2.getSelectedBlock() == ui::BlockId::cab);
            logMessage ("  -> Cab: both views showing mid-fade (the cab at alpha " + juce::String (alphaDuring, 2) + "), then the cab alone, its mics in the "
                        "inspector; EQ: the fade ended on its own after " + juce::String (fadeMs, 0) + " ms of the message loop; the pre EQ pedal showed the pre "
                        "graph; the view's choice went back to the post EQ; a new editor opened on the cab");
        }

        beginTest ("overlays: the tuner, Match tone, and Settings open over the main screen and close back to it (Close, Escape, the top bar's buttons)");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::delay);
            auto& frame = ed.getOverlayFrame();
            expect (! frame.isVisible());

            juce::StringArray seen;
            for (const auto o : { ui::Overlay::tuner, ui::Overlay::toneMatch, ui::Overlay::settings })
            {
                auto& button = o == ui::Overlay::tuner ? ed.getTopBar().getTunerButton()
                               : o == ui::Overlay::toneMatch ? ed.getTopBar().getMatchToneButton() : ed.getTopBar().getSettingsButton();
                button.onClick();
                expect (ed.getOverlay() == o && frame.isVisible() && frame.getPage() != nullptr && frame.getPage()->isVisible(), ui::overlayName (o));
                expect (frame.getBounds() == juce::Rectangle<int> (0, ui::theme::topBarHeight, ui::theme::canvasWidth,
                                                                   ui::theme::canvasHeight - ui::theme::topBarHeight - ui::theme::footerHeight));
                expect (ed.getScenesBar().isVisible() && ed.getTunerReadout().isVisible(), "the footer stays");
                seen.add (ui::overlayName (o) + juce::String (" ") + frame.getPage()->getBounds().toString());
                button.onClick(); // the same button closes it
                expect (ed.getOverlay() == ui::Overlay::none && ! frame.isVisible());
                expect (ed.getSelectedBlock() == ui::BlockId::delay && ed.getInspector().getContent() == &ed.getPage (ui::BlockId::delay), "back as it was");
            }

            // The tuner: opening engages it, Close disengages it; a footswitch that engages it opens it.
            ed.openOverlay (ui::Overlay::tuner);
            expectEquals (getParam (p, "tuner_on"), 1.0f);
            ed.getOverlayFrame().getCloseButton().onClick();
            expect (ed.getOverlay() == ui::Overlay::none);
            expectEquals (getParam (p, "tuner_on"), 0.0f);
            setParam (p, "tuner_on", 1.0f);
            ed.refresh();
            expect (ed.getOverlay() == ui::Overlay::tuner);
            setParam (p, "tuner_on", 0.0f);
            ed.refresh();
            expect (ed.getOverlay() == ui::Overlay::none);

            // Settings: its own Close; its pages; the output's A/B and MIDI still wired.
            ed.openOverlay (ui::Overlay::settings);
            auto& settings = ed.getSettingsPage();
            settings.showOutput (true);
            expect (ed.getOutputPage().isVisible() && ! ed.getInputPage().isVisible());
            const auto settingsIds = taggedIn (settings);
            for (const auto* id : { "output_gain", "output_limit_on", "output_limit_ceiling", "cpu_saver", "tempo_bpm", "midi_tap_cc" })
                expect (settingsIds.contains (id), id);
            settings.getCloseButton().onClick();
            expect (ed.getOverlay() == ui::Overlay::none);

            // Match tone: its own Close.
            ed.openOverlay (ui::Overlay::toneMatch);
            expect (ed.getToneMatchPage().isVisible());
            ed.getToneMatchPage().onClose();
            expect (ed.getOverlay() == ui::Overlay::none && ! ed.getToneMatchPage().isVisible());
            logMessage ("  -> each opened from the top bar and closed by the same button, the main screen as it was after: " + seen.joinIntoString ("; ")
                        + "; the tuner's Close disengaged it and its footswitch opened it; Settings and Match tone closed with their own Close");
        }

        beginTest ("scenes in the footer: a click stores into an empty scene or recalls; the current one is lit in orange and the light glides to its new place");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& bar = ed.getScenesBar();
            expectEquals (bar.getLitScene(), -1);

            setParam (p, "delay_on", 1.0f);
            dynamic_cast<juce::Button&> (bar.getTile (0)).onClick(); // stores scene 1
            ed.refresh();
            expect (p.getScenes().get (0).stored && bar.getLitScene() == 0);
            expect (bar.getLightBounds() == bar.getTile (0).getBounds().toFloat());
            setParam (p, "delay_on", 0.0f);
            dynamic_cast<juce::Button&> (bar.getTile (3)).onClick(); // stores scene 4
            ed.refresh();
            const auto gliding = bar.isGliding();
            const auto from = bar.getLightBounds();
            expect (gliding && bar.getLitScene() == 3);
            expect (from.getX() < (float) bar.getTile (3).getX(), "the light starts from scene 1's place");
            bar.finishGlide();
            expect (bar.getLightBounds() == bar.getTile (3).getBounds().toFloat());

            // Recalled from the footer: scene 1's sound comes back, and the light glides back.
            dynamic_cast<juce::Button&> (bar.getTile (0)).onClick();
            ed.refresh();
            expectEquals (getParam (p, "delay_on"), 1.0f);
            expect (bar.getLitScene() == 0 && bar.isGliding());

            // The glide runs on its own timer.
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            while (bar.isGliding() && juce::Time::getMillisecondCounterHiRes() - t0 < 3000.0)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            expect (! bar.isGliding() && bar.getLightBounds() == bar.getTile (0).getBounds().toFloat());

            // A right-click on a scene is its menu (the editor finds it by the scene's index).
            expect ((int) bar.getTile (2).getProperties()[ui::ScenesBar::sceneIndexProperty] == 2);
            expect (ed.sceneMenuFor (0).getNumItems() >= 3);
            logMessage ("  -> scene 1 stored and lit; scene 4 stored, the light gliding from " + from.toString() + "; scene 1 recalled (delay on) and the "
                        "light glided back on its own in " + juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 0) + " ms");
        }

        beginTest ("from a step back: the preset's name at 23 px and its scene at 15 px in ink; one short line under the amp and the cab, the notes in the "
                   "preset's tooltip; the head's knobs are gear with no value arcs");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            for (int i = 0; i < 6 && ed.getTopBar().getShownPresetName() != "Tech Death"; ++i)
            {
                ed.stepPreset (1);
                waitForLoads (p);
                ed.refresh();
            }
            p.recallScene (1);
            ed.refresh();
            expectEquals (ed.getTopBar().getShownPresetName(), juce::String ("Tech Death"));

            // The type (RD4-4): the name 22 to 24 px semibold, the scene about 15 px in ink (not grey), in a 48 px box.
            const auto nameFont = juce::Font (ui::TopBar::getPresetNameFont()), sceneFont = juce::Font (ui::TopBar::getSceneFont());
            expect (nameFont.getHeightInPoints() >= 22.0f && nameFont.getHeightInPoints() <= 24.0f, juce::String (nameFont.getHeightInPoints()));
            expect (sceneFont.getHeightInPoints() >= 14.5f && sceneFont.getHeightInPoints() <= 15.5f, juce::String (sceneFont.getHeightInPoints()));
            expect (ui::TopBar::getSceneColour() == ui::theme::ink);
            auto& box = ed.getTopBar().getPresetButton();
            expectEquals (box.getHeight(), ui::TopBar::presetBoxHeight);
            // As drawn: the name's ink (the brightest pixels in the box's top 28 px) at least 15 px tall (its capitals);
            // the scene's line has ink pixels too (in grey #9a9a9a it would have none this bright).
            const auto image = box.createComponentSnapshot (box.getLocalBounds(), true, 2.0f);
            int top = 1 << 30, bottom = -1, sceneInk = 0;
            for (int y = 0; y < image.getHeight(); ++y)
                for (int x = 0; x < image.getWidth(); ++x)
                    if (image.getPixelAt (x, y).getBrightness() > 0.82f)
                    {
                        if (y < 2 * 28)
                        {
                            top = juce::jmin (top, y);
                            bottom = juce::jmax (bottom, y);
                        }
                        else
                            ++sceneInk;
                    }
            const auto nameHeight = (float) (bottom - top + 1) / 2.0f;
            expectGreaterOrEqual (nameHeight, 15.0f, "the name's capitals on screen");
            expectGreaterThan (sceneInk, 60, "the scene in ink");
            expectEquals (ed.getTopBar().getShownScene(), juce::String ("Scene 2: Lead"));

            // One short line under the amp (RD4-5): its class and its name; the capture's file in its tooltip; the rate at
            // the right. The preset's notes are the preset box's tooltip, never a line on the stage (RD4-6).
            auto& amp = ed.getAmpView();
            const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
            expectEquals (amp.getInfoLine(), "High gain" + dot + "Monolith");
            expect (amp.getInfoTooltip().contains (amp.getModelText()) && amp.getModelText() == "Monolith (built in, gain set)", amp.getInfoTooltip());
            expectEquals (amp.getRateText(), juce::String ("48 kHz"));
            const auto notes = ed.getTopBar().getPresetNotes();
            expect (notes.isNotEmpty(), "Tech Death has notes");
            expect (box.getTooltip().startsWith (notes));
            expect (! ed.getStatusText().contains (notes.substring (0, 20)), ed.getStatusText());
            ed.setStageMode (ui::StageMode::cab, false);
            ed.refresh();
            ed.finishAnimations();
            const auto cabLine = ed.getCabView().getInfoLine();
            expect (cabLine.contains (dot) && ! cabLine.startsWith ("No IR"), cabLine);
            expect (ed.getStatusText().isEmpty(), ed.getStatusText());
            ed.setStageMode (ui::StageMode::amp, false);
            ed.refresh();

            // The head's knobs are gear (RD4-8): no value arc. At full travel a chrome knob's arc ring is orange; a gear
            // knob's ring (where the arc would be: radius 30 to 35 round the hit area's centre) has no orange in it, for
            // every amp's skin.
            const auto arcPixels = [] (ui::Knob& knob)
            {
                const auto img = knob.createComponentSnapshot (knob.getLocalBounds(), true, 2.0f);
                const juce::Point<float> c { (float) knob.getWidth(), (float) (ui::Knob::margin + 32) * 2.0f };
                int n = 0;
                for (int y = 0; y < img.getHeight(); ++y)
                    for (int x = 0; x < img.getWidth(); ++x)
                    {
                        const auto r = c.getDistanceFrom ({ (float) x + 0.5f, (float) y + 0.5f }) / 2.0f;
                        const auto px = img.getPixelAt (x, y);
                        if (r > 30.0f && r < 35.0f && px.getAlpha() > 100 && std::abs ((int) px.getRed() - 255) < 50 && std::abs ((int) px.getGreen() - 138) < 50
                            && px.getBlue() < 90)
                            ++n;
                    }
                return n;
            };
            juce::StringArray arcs;
            for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
            {
                setParam (p, AmpSimProcessor::ampModelParamId, (float) a);
                ed.refresh();
                auto& knob = amp.getKnob (a, 1); // Bass (no gain-set dots round it)
                setParam (p, ui::AmpView::knobParameter (a, 1), 12.0f);
                knob.repaint();
                expect (knob.isGearStyle(), "amp " + juce::String (a));
                const auto n = arcPixels (knob);
                expectEquals (n, 0, "amp " + juce::String (a) + ": orange where an arc would be");
                arcs.add (juce::String (n));
            }
            auto& chrome = amp.getPanel().getKnob (0, 1);
            const auto control = arcPixels (chrome);
            expectGreaterThan (control, 100, "the inspector's knob at full travel has its arc");
            setParam (p, AmpSimProcessor::ampModelParamId, 2.0f);
            ed.refresh();
            logMessage ("  -> the preset's name " + juce::String (nameHeight, 1) + " px of capitals in " + juce::String (nameFont.getHeightInPoints(), 0)
                        + " px semibold, the scene in 15 px ink (" + juce::String (sceneInk) + " ink pixels at 2x); the amp's line \"" + amp.getInfoLine()
                        + "\", the cab's \"" + cabLine + "\"; the notes in the tooltip (" + juce::String (notes.length()) + " characters); orange pixels "
                        "in the arc's ring: " + arcs.joinIntoString (", ") + " on the nine heads' knobs, " + juce::String (control) + " on the inspector's");
        }

        beginTest ("snapshots: the main screen with the amp, a pre and a post pedal, the Cab and EQ stage modes, each overlay, and at a small window size");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);

            // Tech Death, a factory preset with its scenes, on its second scene, a few pedals on.
            for (int i = 0; i < 6 && ed.getTopBar().getShownPresetName() != "Tech Death"; ++i)
            {
                ed.stepPreset (1);
                waitForLoads (p);
                ed.refresh();
            }
            expectEquals (ed.getTopBar().getShownPresetName(), juce::String ("Tech Death"));
            p.recallScene (1);
            for (const auto* id : { "gate_a_on", "wah_on", "od_on", "delay_on", "reverb_on" })
                setParam (p, id, 1.0f);
            setParam (p, "wah_sensitivity", 20.0f); // Tech Death's input pins it at High at the default 50 %; at 20 % it sweeps
            play (p, 0.5);
            waitForLoads (p);
            ed.getTopBar().updateMeters (p.takePeaks(), 1.0 / 30.0);
            ed.refresh();
            ed.finishAnimations();

            juce::StringArray files;
            const auto snap = [&] (const juce::String& name)
            {
                ed.refresh();
                ed.getBoard().refresh();
                ed.finishAnimations();
                const auto file = redesignDir().getChildFile (name + ".png");
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                expect (savePng (image, file));
                files.add (file.getFileName() + " " + juce::String (image.getWidth()) + "x" + juce::String (image.getHeight()));
                return image;
            };

            ed.selectBlock (ui::BlockId::amp);
            const auto main = snap ("main_amp");

            // The board's three tabs, the amp selected (RD5-1): the main screen with each one open.
            auto& board = ed.getBoard();
            for (const auto tab : { ui::BoardTab::pre, ui::BoardTab::ampCab, ui::BoardTab::post })
            {
                click (board.getTabStrip(), board.getTabArea (tab).getCentre());
                expect (board.getTab() == tab);
                snap (juce::String ("main_board_") + ui::boardTabName (tab));
            }
            click (board.getTabStrip(), board.getTabArea (ui::BoardTab::pre).getCentre());
            ed.selectBlock (ui::BlockId::overdrive);
            snap ("main_pedal_pre_overdrive");
            // The auto-wah selected while playing: its meter's lamp and the last second's sweep, its window on the board.
            ed.selectBlock (ui::BlockId::wah);
            {
                // A second of playing (one continuous take, not the same first 33 ms again), read 30 times a second.
                const auto take = guitarDI ((int) fs);
                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;
                for (size_t start = 0, n = 0; start + blockSize <= take.size(); start += blockSize, ++n)
                {
                    buffer.clear();
                    buffer.copyFrom (0, 0, take.data() + start, blockSize);
                    p.processBlock (buffer, midi);
                    if (n % 12 == 11)
                        ed.refresh();
                }
            }
            snap ("main_pedal_pre_wah");
            ed.selectBlock (ui::BlockId::delay);
            snap ("main_pedal_post_delay");
            ed.selectBlock (ui::BlockId::harmonizer);
            snap ("main_pedal_post_harmonizer");
            ed.selectBlock (ui::BlockId::gateA);
            snap ("main_pedal_pre_gate_a");
            ed.setStageMode (ui::StageMode::cab, false);
            snap ("stage_cab");
            ed.setStageMode (ui::StageMode::eq, false);
            setParam (p, "eq_post_on", 1.0f);
            for (int m = 0; m < 9; ++m)
                setParam (p, "eq_post_g" + juce::String (m + 1), (float) std::sin (m * 0.9) * 6.0f);
            for (int frame = 0; frame < 20; ++frame)
            {
                play (p, 0.05);
                ed.getEqStage().getShownGraph().updateAnalyzer();
            }
            snap ("stage_eq");
            ed.setStageMode (ui::StageMode::amp, false);

            ed.openOverlay (ui::Overlay::tuner);
            ampsim::TunerReading e2;
            e2.hasReading = e2.live = true;
            e2.midiNote = 40;
            e2.frequency = 82.56;
            e2.cents = 2.1;
            e2.levelDb = -21.0;
            ed.getTunerPage().freeze (e2);
            ed.getTunerReadout().set (true, e2);
            snap ("overlay_tuner");
            ed.closeOverlay();
            ed.openOverlay (ui::Overlay::toneMatch);
            snap ("overlay_tone_match");
            ed.openOverlay (ui::Overlay::settings);
            ed.getSettingsPage().showOutput (false);
            snap ("overlay_settings_input");
            ed.getSettingsPage().showOutput (true);
            snap ("overlay_settings_output");
            ed.closeOverlay();

            // The main screen in a small window: the canvas at half size (the smallest the window goes), drawn at 2x
            // as a Retina screen would.
            ed.selectBlock (ui::BlockId::amp);
            editor->setSize (ui::theme::minimumWidth, ui::theme::minimumHeight);
            expectWithinAbsoluteError (ed.getCanvasScale(), 0.5f, 1.0e-6f);
            snap ("main_small_window");
            editor->setSize (1100, 760); // and a window of another shape, letterboxed
            snap ("main_letterboxed");
            editor->setSize (ui::theme::canvasWidth, ui::theme::canvasHeight);

            // The main screen's ground and panels are the theme's (sampled at 2x away from any control).
            const auto at = [&main] (int x, int y) { return main.getPixelAt (2 * x, 2 * y); };
            expect (at (990, 740).getBrightness() < 0.2f && at (1270, 300) == ui::theme::panelBg, at (1270, 300).toDisplayString (false));
            logMessage ("  -> build/proof/redesign/: " + files.joinIntoString (", "));
        }
    }
};

MainScreenTests mainScreenTests;
} // namespace

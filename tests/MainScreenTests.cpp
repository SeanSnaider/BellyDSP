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
            // The heads keep their own skins: Monolith's pointer is its emerald, Forge's its own orange, not the theme's.
            expect (knobSkin (Skin::monolith).pointer == juce::Colour (0xff34d399));
            expect (knobSkin (Skin::forge).pointer == juce::Colour (0xffff6a1f));

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

            // Every pedal on the board, in chain order: the pre section, the amp, Gate B, the cab on the top row; the
            // post section on the bottom row. Every pedal inside the board.
            const auto& board = ed.getBoard();
            int pedals = 0;
            juce::StringArray tops, bottoms;
            for (int b = 0; b < ui::numBlocks; ++b)
                if (auto* pedal = board.getPedal ((ui::BlockId) b))
                {
                    ++pedals;
                    expect (board.getLocalBounds().contains (pedal->getBounds()), ui::info ((ui::BlockId) b).name);
                }
            expectEquals (pedals, 14);
            for (auto id : board.getShownOrder (ui::Section::pre))
                tops.add (ui::info (id).name);
            for (auto id : board.getShownOrder (ui::Section::post))
                bottoms.add (ui::info (id).name);
            expect (board.getPedal (ui::BlockId::preEq)->getRight() <= board.getMarker (ui::BlockId::amp)->getX());
            expect (board.getMarker (ui::BlockId::amp)->getRight() <= board.getPedal (ui::BlockId::gateB)->getX());
            expect (board.getPedal (ui::BlockId::gateB)->getRight() <= board.getMarker (ui::BlockId::cab)->getX());
            expect (board.getPedal (ui::BlockId::postEq)->getY() > board.getPedal (ui::BlockId::gateA)->getY());
            expectGreaterThan (board.getPedalScale(), 0.6f);
            logMessage ("  -> top row: " + tops.joinIntoString (", ") + ", the amp, Gate B, the cab; bottom row: " + bottoms.joinIntoString (", ")
                        + "; pedals at " + juce::String (board.getPedalScale(), 3) + " of their size");
        }

        beginTest ("selecting a pedal: a click on it shows its every control in the inspector and raises it; the amp's knobs by default");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            auto& board = ed.getBoard();

            // The amp's inspector: its seven knobs (the playing amp's), Gate A's switch and knobs, the levels.
            const auto ampIds = taggedIn (ed.getInspector());
            for (const auto* id : { "amp1_input_trim", "amp1_output_trim", "amp1_bass", "amp1_mid", "amp1_treble", "amp1_presence", "amp1_depth", "gate_a_on",
                                    "gate_a_threshold", "input_gain", "output_gain", "amp_bypass" })
                expect (ampIds.contains (id), id);

            // A click on the delay pedal (above its footswitch): its page in the inspector, nothing switched.
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

            // The amp marker selects the amp, and the stage shows it; the cab marker the cab.
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

            // The post section's switch dims its row (each pedal keeps its own LED).
            setParam (p, "post_fx_on", 0.0f);
            board.refresh();
            expect (board.getPedal (ui::BlockId::delay)->getAlpha() < 1.0f && juce::exactlyEqual (board.getPedal (ui::BlockId::boost)->getAlpha(), 1.0f));
            setParam (p, "post_fx_on", 1.0f);
            board.refresh();

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
                        + "\" (lit in the footer); 8 on an empty scene did nothing; T opened and closed the tuner; Escape closed Settings; a clicked knob "
                        "ignored Right and took Up");
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
            for (const auto* id : { "gate_a_on", "od_on", "delay_on", "reverb_on" })
                setParam (p, id, 1.0f);
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
            ed.selectBlock (ui::BlockId::overdrive);
            snap ("main_pedal_pre_overdrive");
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

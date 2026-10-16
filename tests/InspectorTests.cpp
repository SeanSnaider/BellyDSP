// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The app redesign, phase 2: a designed inspector for every block (its main controls first and large, its modes as
// selectors, its meters live), the cab's stage view built around the speaker, and the board's amp and cab markers.
// Snapshots of each in build/proof/redesign/ (inspector_<block>.png, stage_cab_pack.png, board_markers.png).

#include "BuiltInCaptures.h"
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
    p.runHousekeeping();
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

/// A component drawn at 2x on the inspector's panel colour (the pages draw no ground of their own: the inspector does).
juce::Image onPanel (juce::Component& c, float scale = 2.0f)
{
    juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) c.getWidth() * scale), juce::roundToInt ((float) c.getHeight() * scale), true);
    juce::Graphics g (image);
    g.fillAll (ui::theme::panelBg);
    g.addTransform (juce::AffineTransform::scale (scale));
    c.paintEntireComponent (g, false);
    return image;
}

juce::File redesignDir()
{
    return proofDir().getChildFile ("redesign");
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> at, int modifiers = juce::ModifierKeys::leftButtonModifier, bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (modifiers), juce::MouseInputSource::defaultPressure, 0.0f,
                             0.0f, 0.0f, 0.0f, &c, &c, now, at, now, 1, dragged);
}

void click (juce::Component& c, juce::Point<float> at, int modifiers = juce::ModifierKeys::leftButtonModifier)
{
    c.mouseDown (mouseEvent (c, at, modifiers));
    c.mouseUp (mouseEvent (c, at, modifiers));
}

/// Plays a signal through the processor in buffers (mono into both inputs' first channel).
void play (AmpSimProcessor& p, const std::vector<float>& input, std::function<void()> eachBuffer = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        if (eachBuffer)
            eachBuffer();
    }
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

/// The component tagged with a parameter inside `c` (the first found), or nullptr.
juce::Component* findTagged (juce::Component& c, const juce::String& id)
{
    if (c.getProperties()[ui::parameterIdProperty].toString() == id)
        return &c;
    for (auto* child : c.getChildren())
        if (auto* found = findTagged (*child, id))
            return found;
    return nullptr;
}

template <typename T>
T* findTaggedAs (juce::Component& c, const juce::String& id)
{
    // Every control tagged with the ID (a page may show one parameter in more than one way), the first of type T.
    if (auto* t = dynamic_cast<T*> (&c); t != nullptr && c.getProperties()[ui::parameterIdProperty].toString() == id)
        return t;
    for (auto* child : c.getChildren())
        if (auto* found = findTaggedAs<T> (*child, id))
            return found;
    return nullptr;
}

/// Each block's designed inspector: the parameters its selectors (ChoiceBar) choose, and its main knobs (large).
struct Design
{
    ui::BlockId id;
    juce::StringArray selectors, large;
};

std::vector<Design> designs()
{
    using B = ui::BlockId;
    return {
        { B::gateA, { "gate_a_release_mode", "gate_a_detector" }, { "gate_a_threshold", "gate_a_release" } },
        { B::wah, { "wah_direction" }, { "wah_sensitivity", "wah_q" } },
        { B::preCompressor, { "comp_pre_mode", "comp_pre_detector" }, { "comp_pre_threshold", "comp_pre_ratio" } },
        { B::boost, { "boost_mode" }, { "boost_level", "boost_tilt" } },
        { B::overdrive, { "od_mode", "drive_oversampling" }, { "od_drive", "od_level" } },
        { B::preEq, { "eq_pre_mode", "eq_pre_lowcut_slope", "eq_pre_highcut_slope" }, {} },
        { B::amp, {}, { "amp1_input_trim", "amp1_output_trim" } },
        { B::gateB, { "gate_b_release_mode", "gate_b_detector" }, { "gate_b_threshold", "gate_b_release" } },
        { B::cab, {}, {} },
        { B::postEq, { "eq_post_mode" }, {} },
        { B::postCompressor, { "comp_post_mode", "comp_post_detector" }, { "comp_post_threshold", "comp_post_ratio" } },
        { B::harmonizer, { "harm_root", "harm_out_of_key", "harm_floor", "harm_v1_mode", "harm_v4_mode" }, {} },
        { B::multivoicer, { "mv_engine", "mv_mix_mode" }, { "mv_mix", "mv_spread" } },
        { B::bloom, { "bloom_phaser_mode", "bloom_phaser_stages", "bloom_phaser_shape", "bloom_flanger_shape" }, { "bloom_mix" } },
        { B::chorus, { "chorus_mode", "chorus_shape" }, { "chorus_rate", "chorus_depth" } },
        { B::delay, { "delay_mode", "delay_stereo" }, { "delay_time", "delay_feedback", "delay_mix" } },
        { B::reverb, { "reverb_engine", "reverb_shimmer_interval" }, { "reverb_mix", "reverb_decay" } },
    };
}

class InspectorTests final : public juce::UnitTest
{
public:
    InspectorTests() : juce::UnitTest ("Main screen inspectors", "ampsim") {}

    void runTest() override
    {
        beginTest ("every block's designed inspector: its modes as selectors, its main controls large and first, every parameter under its prefix; snapshots");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);

            // A sound with things happening, so the snapshots show the controls in use.
            for (const auto* id : { "gate_a_on", "wah_on", "comp_pre_on", "od_on", "boost_on", "eq_post_on", "harm_on", "mv_on", "bloom_on", "bloom_phaser_on",
                                    "chorus_on", "delay_on", "reverb_on", "harm_v1_on", "harm_v2_on" })
                setParam (p, id, 1.0f);
            setParam (p, "comp_pre_threshold", -36.0f);
            setParam (p, "comp_pre_ratio", 6.0f);
            setParam (p, "eq_pre_mode", 1.0f);
            for (int m = 0; m < 9; ++m)
                setParam (p, "eq_post_g" + juce::String (m + 1), (float) std::sin (m * 0.9) * 6.0f);
            setParam (p, "eq_post_lowcut_on", 1.0f);
            play (p, guitarDI ((int) (1.0 * fs)));
            waitForLoads (p);

            juce::StringArray report, files;
            int knobsSeen = 0;
            for (const auto& d : designs())
            {
                ed.selectBlock (d.id);
                ed.refresh();
                ed.refresh();
                ed.finishAnimations();
                auto* content = ed.getInspector().getContent();
                expect (content != nullptr, ui::info (d.id).name);
                if (content == nullptr)
                    continue;

                for (const auto& id : d.selectors)
                    expect (findTaggedAs<ui::ChoiceBar> (*content, id) != nullptr, juce::String (ui::info (d.id).pageName) + ": " + id + " is a selector");
                int largeTop = 1 << 30, otherTop = 1 << 30;
                for (const auto& id : d.large)
                {
                    auto* knob = findTaggedAs<ui::Knob> (*content, id);
                    expect (knob != nullptr && knob->getKnobSize() == ui::Knob::Size::large, juce::String (ui::info (d.id).pageName) + ": " + id + " is large");
                    if (knob != nullptr)
                        largeTop = juce::jmin (largeTop, knob->getY());
                }
                // The large knobs come before any normal-size knob: the most-used controls first. And every knob shows its
                // value under its caption, the secondary ones included (RD4-7): the flag, the room for the line, and the
                // line drawn (bright text pixels in the knob's last 16 px).
                juce::StringArray noValue;
                std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                {
                    auto* k = dynamic_cast<ui::Knob*> (&c);
                    if (k != nullptr && k->getKnobSize() != ui::Knob::Size::large && k->isVisible())
                        otherTop = juce::jmin (otherTop, k->getY());
                    const auto shown = [content] (juce::Component* x)
                    {
                        for (; x != nullptr && x != content; x = x->getParentComponent())
                            if (! x->isVisible())
                                return false;
                        return x == content;
                    };
                    if (k != nullptr && shown (k) && ! k->getBounds().isEmpty())
                    {
                        ++knobsSeen;
                        const auto image = k->createComponentSnapshot (k->getLocalBounds(), true, 2.0f);
                        int bright = 0;
                        for (int y = image.getHeight() - 2 * (ui::Knob::valueLineHeight + ui::Knob::margin + 2); y < image.getHeight(); ++y)
                            for (int x = 0; x < image.getWidth(); ++x)
                                if (const auto px = image.getPixelAt (x, y); px.getAlpha() > 120 && px.getBrightness() > 0.6f)
                                    ++bright;
                        if (! k->isValueShown() || k->getHeight() < k->getPreferredHeight() || bright < 8)
                            noValue.add (k->getCaption());
                    }
                    for (auto* child : c.getChildren())
                        walk (*child);
                };
                walk (*content);
                expect (noValue.isEmpty(), juce::String (ui::info (d.id).pageName) + ": knobs without their value shown: " + noValue.joinIntoString (", "));
                if (! d.large.isEmpty())
                    expectLessThan (largeTop, otherTop, juce::String (ui::info (d.id).pageName) + ": the large knobs come first");

                // Nothing past the column's width, nothing overlapping its neighbour (visible controls only).
                std::vector<juce::Rectangle<int>> placed;
                juce::StringArray overlaps;
                for (auto* child : content->getChildren())
                    if (child->isVisible() && ! child->getBounds().isEmpty())
                    {
                        expect (child->getRight() <= content->getWidth() && child->getX() >= 0,
                                juce::String (ui::info (d.id).pageName) + ": " + child->getBounds().toString() + " outside the column");
                        // A knob's 4 px focus margin and a switch's may touch a neighbour's; their insides mustn't.
                        const auto inside = child->getBounds().reduced (dynamic_cast<ui::Knob*> (child) != nullptr || dynamic_cast<ui::Switch*> (child) != nullptr ? 5 : 1);
                        for (const auto& other : placed)
                            if (inside.intersects (other))
                                overlaps.add (child->getProperties()[ui::parameterIdProperty].toString() + " " + child->getBounds().toString());
                        placed.push_back (inside);
                    }
                expect (overlaps.isEmpty(), juce::String (ui::info (d.id).pageName) + " overlaps: " + overlaps.joinIntoString (", "));

                const auto file = redesignDir().getChildFile ("inspector_" + juce::String (ui::info (d.id).pageName) + ".png");
                expect (savePng (onPanel (*content), file));
                files.add (file.getFileName());
                report.add (juce::String (ui::info (d.id).pageName) + " " + juce::String (d.selectors.size()) + " selectors, " + juce::String (d.large.size())
                            + " large, " + juce::String (content->getHeight()) + " px");
            }
            logMessage ("  -> " + report.joinIntoString ("; "));
            logMessage ("  -> " + juce::String (knobsSeen) + " knobs shown across the inspectors, every one with its value under its caption");
            logMessage ("  -> build/proof/redesign/: " + files.joinIntoString (", "));
        }

        beginTest ("selectors: a click picks a mode as one undo step, a right-click never does (it's MIDI learn); the mode's dims follow");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::boost);
            ed.refresh();
            auto& page = dynamic_cast<ui::BoostPage&> (ed.getPage (ui::BlockId::boost));
            auto& mode = page.getMode();
            expectEquals (mode.getNumOptions(), 3);
            auto* tilt = findTaggedAs<ui::Knob> (page, "boost_tilt");
            auto* mid = findTaggedAs<ui::Knob> (page, "boost_mid");
            const auto tiltBefore = tilt->getAlpha();

            p.parameters.copyState();
            p.undoManager.clearUndoHistory();
            ed.mouseDown (mouseEvent (mode, mode.optionArea (1).getCentre()));
            click (mode, mode.optionArea (1).getCentre());
            ed.refresh();
            expectEquals (getParam (p, "boost_mode"), 1.0f);
            expect (mode.getSelected() == 1);
            const auto tiltInTight = tilt->getAlpha();
            expect (tiltInTight < 1.0f && juce::exactlyEqual (mid->getAlpha(), 1.0f), "Tight dims the tilt and uses the mid push");

            // A right-click: nothing changes; the menu for the parameter is MIDI learn's.
            click (mode, mode.optionArea (2).getCentre(), juce::ModifierKeys::rightButtonModifier);
            expectEquals (getParam (p, "boost_mode"), 1.0f);
            const auto menu = ed.midiMenuFor ("boost_mode");
            bool learnItem = false;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                learnItem = learnItem || it.getItem().text.startsWith ("MIDI learn");
            expect (learnItem);

            // One undo step back to Clean, and the bar follows the parameter whoever moves it.
            p.parameters.copyState();
            expect (p.undoManager.undo());
            ed.refresh();
            expectEquals (getParam (p, "boost_mode"), 0.0f);
            expect (mode.getSelected() == 0 && juce::exactlyEqual (tilt->getAlpha(), 1.0f));
            setParam (p, "boost_mode", 2.0f);
            expect (mode.getSelected() == 2);

            // The keyboard: the arrows step it only once Tab gave it the focus.
            expect (! mode.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
            logMessage ("  -> a click on Tight set boost_mode 1 (one undo step; the tilt dimmed to " + juce::String (tiltInTight, 2) + " from "
                        + juce::String (tiltBefore, 2) + "); a right-click on Screamer changed nothing and the menu offers MIDI learn; a preset-style move to "
                        "Screamer moved the bar");
        }

        beginTest ("gates: the meter reads the gate live (open, closed, the level, the reduction); a drag on it sets the threshold as one undo step; linked, Gate B follows");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "gate_a_on", 1.0f);
            setParam (p, "gate_a_threshold", -40.0f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::gateA);
            auto& page = dynamic_cast<ui::GatePage&> (ed.getPage (ui::BlockId::gateA));
            auto& meter = page.getMeter();
            expect (page.getLearnButton() != nullptr && page.getLinkPad() == nullptr);

            // Playing (a held note at -10 dBFS): the meter reads what the gate reports, and says it's open.
            play (p, sine (110.0, 0.3, (int) (0.6 * fs)));
            page.updateMeter();
            const auto playing = meter.getReading();
            const auto truth = p.getGateMeter (false);
            expectWithinAbsoluteError (playing.meter.detectorDb, truth.detectorDb, 0.01f);
            expectWithinAbsoluteError (playing.meter.openDb, -40.0f, 0.5f);
            expect (truth.open && meter.getStateText() == "Open", meter.getStateText());
            const auto playingImage = meter.createComponentSnapshot (meter.getLocalBounds(), true, 2.0f);

            // Silence: closed, the reduction full.
            play (p, std::vector<float> ((size_t) (0.8 * fs), 0.0f));
            page.updateMeter();
            const auto silent = meter.getReading();
            expect (! p.getGateMeter (false).open && meter.getStateText() == "Closed", meter.getStateText());
            expectGreaterThan (silent.meter.reductionDb, 20.0f);
            expect (savePng (playingImage, redesignDir().getChildFile ("gate_meter_open.png")));
            expect (savePng (meter.createComponentSnapshot (meter.getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("gate_meter_closed.png")));

            // A drag along the bar: the threshold follows the pointer, one undo step.
            p.parameters.copyState();
            p.undoManager.clearUndoHistory();
            const auto bar = meter.barArea();
            const auto at = juce::Point<float> (meter.xForDb (-60.0f), bar.getCentreY());
            ed.mouseDown (mouseEvent (meter, at));
            meter.mouseDown (mouseEvent (meter, at));
            meter.mouseDrag (mouseEvent (meter, at, juce::ModifierKeys::leftButtonModifier, true));
            meter.mouseUp (mouseEvent (meter, at));
            const auto dragged = getParam (p, "gate_a_threshold");
            expectWithinAbsoluteError (dragged, -60.0f, 0.6f);
            p.parameters.copyState();
            expect (p.undoManager.undo());
            expectWithinAbsoluteError (getParam (p, "gate_a_threshold"), -40.0f, 0.01f);
            expectEquals (meter.getProperties()[ui::parameterIdProperty].toString(), juce::String ("gate_a_threshold"));

            // Gate B linked: its meter says so, and its own settings are dimmed.
            setParam (p, "gate_b_on", 1.0f);
            setParam (p, "gate_link", 1.0f);
            ed.selectBlock (ui::BlockId::gateB);
            ed.refresh();
            auto& pageB = dynamic_cast<ui::GatePage&> (ed.getPage (ui::BlockId::gateB));
            expect (pageB.getLinkPad() != nullptr && pageB.getLinkPad()->getToggleState());
            expectEquals (pageB.getMeter().getStateText(), juce::String ("Following Gate A"));
            expect (pageB.getThresholdKnob().getAlpha() < 1.0f && pageB.getReleaseMode().getAlpha() < 1.0f);
            expect (savePng (pageB.getMeter().createComponentSnapshot (pageB.getMeter().getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("gate_meter_linked.png")));
            logMessage ("  -> playing: \"Open\" at " + juce::String (playing.meter.detectorDb, 1) + " dBFS (the gate: " + juce::String (truth.detectorDb, 1)
                        + "), open at " + juce::String (playing.meter.openDb, 1) + ", close at " + juce::String (playing.meter.closeDb, 1) + "; silence: \"Closed\", "
                        + juce::String (silent.meter.reductionDb, 1) + " dB down; a drag to -60 dB set the threshold to " + juce::String (dragged, 1)
                        + " (one undo step back to -40); Gate B linked reads \"Following Gate A\", its knobs dimmed; gate_meter_open.png, gate_meter_closed.png, gate_meter_linked.png");
        }

        beginTest ("compressors: the meter shows the reduction live, and its last three seconds as a trace; the curve follows the knobs");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "comp_post_on", 1.0f);
            setParam (p, "comp_post_threshold", -40.0f);
            setParam (p, "comp_post_ratio", 10.0f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::postCompressor);
            auto& page = dynamic_cast<ui::CompressorPage&> (ed.getPage (ui::BlockId::postCompressor));
            auto& meter = page.getMeter();
            expectEquals (meter.getShownReduction(), 0.0f);

            int n = 0;
            float most = 0.0f, mismatch = 0.0f;
            play (p, guitarDI ((int) (1.5 * fs)), [&]
            {
                if (++n % 12 == 0) // about 30 times a second, as the page's timer
                {
                    page.updateMeter();
                    mismatch = juce::jmax (mismatch, std::abs (meter.getShownReduction() - p.getCompressorReduction (true)));
                    most = juce::jmax (most, meter.getShownReduction());
                }
            });
            expectGreaterThan (most, 3.0f);
            expectLessThan (mismatch, 0.01f);
            expect (savePng (meter.createComponentSnapshot (meter.getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("compressor_meter.png")));
            logMessage ("  -> through the post compressor at -40 dB, 10:1: up to " + juce::String (most, 1) + " dB of reduction shown, the shown value "
                        "matching the block's within " + juce::String (mismatch, 3) + " dB; compressor_meter.png");
        }

        beginTest ("the auto-wah: the meter reads the filter live (where it is, the frequency that means, the last second's sweep), rests when off; the "
                   "knobs read in Hz, ms and %, each with a tooltip");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "wah_on", 1.0f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::wah);
            auto& page = dynamic_cast<ui::AutoWahPage&> (ed.getPage (ui::BlockId::wah));
            auto& meter = page.getMeter();
            expect (ed.getInspector().getContent() == &page);

            // Playing: read 30 times a second, as the page's timer does; the reading is the block's control, and the
            // frequency it shows is the block's own mapping of it.
            int n = 0;
            float mismatch = 0.0f, frequencyError = 0.0f;
            play (p, guitarDI ((int) (1.5 * fs)), [&]
            {
                if (++n % 12 == 0)
                {
                    page.updateMeter();
                    const auto& r = meter.getReading();
                    mismatch = juce::jmax (mismatch, std::abs (r.control - juce::jlimit (0.0f, 1.0f, p.getWahControl())));
                    const auto truth = ampsim::AutoWah::centreFrequency (r.control, getParam (p, "wah_freq_low"), getParam (p, "wah_freq_high"));
                    frequencyError = juce::jmax (frequencyError, (float) std::abs (meter.getFrequency() - truth));
                }
            });
            const auto swept = meter.getSweptRange();
            const auto playingText = meter.getStateText();
            expectLessThan (mismatch, 1.0e-6f);
            expectLessThan (frequencyError, 0.01f);
            expect (meter.getReading().on && playingText.endsWith ("Hz"), playingText);
            expectGreaterThan (swept.getLength(), 0.1f, "the last second's sweep");
            expect (savePng (meter.createComponentSnapshot (meter.getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("wah_meter.png")));

            // Switched off: "Off", the lamp at rest (Low with Up, High with Down), no sweep.
            setParam (p, "wah_on", 0.0f);
            page.updateMeter();
            expect (meter.getStateText() == "Off" && meter.getReading().control == 0.0f && meter.getSweptRange().isEmpty());
            setParam (p, "wah_direction", 1.0f);
            page.updateMeter();
            expect (meter.getReading().down && meter.getReading().control == 1.0f, "Down rests at High");
            expect (savePng (meter.createComponentSnapshot (meter.getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("wah_meter_off.png")));
            setParam (p, "wah_direction", 0.0f);

            // The values as the knobs show them, and every control's tooltip.
            setParam (p, "wah_freq_low", 350.0f);
            setParam (p, "wah_freq_high", 2200.0f);
            setParam (p, "wah_attack", 5.0f);
            setParam (p, "wah_release", 150.0f);
            setParam (p, "wah_sensitivity", 50.0f);
            setParam (p, "wah_q", 4.0f);
            setParam (p, "wah_mix", 100.0f);
            juce::StringArray shown;
            const std::vector<std::pair<const char*, const char*>> expected { { "wah_freq_low", "350 Hz" }, { "wah_freq_high", "2.20 kHz" }, { "wah_attack", "5.0 ms" },
                                                                              { "wah_release", "150 ms" }, { "wah_sensitivity", "50 %" },   { "wah_q", "4.0" },
                                                                              { "wah_mix", "100 %" } };
            for (const auto& [id, text] : expected)
            {
                auto* knob = findTaggedAs<ui::Knob> (page, id);
                expect (knob != nullptr, id);
                if (knob == nullptr)
                    continue;
                expectEquals (knob->getValueText(), juce::String (text), id);
                expect (knob->getTooltip().isNotEmpty(), juce::String (id) + " has a tooltip");
                shown.add (knob->getCaption() + " " + knob->getValueText());
            }
            expect (page.getDirection().getProperties()[ui::parameterIdProperty].toString() == "wah_direction");
            logMessage ("  -> playing: " + juce::String (n / 12) + " reads matched the block's control within " + juce::String (mismatch, 7) + " and its frequency within "
                        + juce::String (frequencyError, 4) + " Hz; it read \"" + playingText + "\" at the end and swept " + juce::String (swept.getStart(), 3) + " to "
                        + juce::String (swept.getEnd(), 3) + " in the last second; off: \"Off\", resting at Low (Up) and High (Down); the knobs at the defaults: "
                        + shown.joinIntoString (", ") + "; wah_meter.png, wah_meter_off.png");
        }

        beginTest ("the EQ pedal puts the stage on its graph; the next other pedal puts it back; the player's own pick of the stage stays");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.setStageMode (ui::StageMode::amp, false);

            ed.selectBlock (ui::BlockId::postEq);
            ed.finishAnimations();
            expect (ed.getStageMode() == ui::StageMode::eq && ! ed.getEqStage().isShowingPre());
            expect (ed.getInspector().getContent() == &ed.getPage (ui::BlockId::postEq));
            ed.selectBlock (ui::BlockId::preEq);
            ed.finishAnimations();
            expect (ed.getStageMode() == ui::StageMode::eq && ed.getEqStage().isShowingPre());
            ed.selectBlock (ui::BlockId::delay);
            ed.finishAnimations();
            expect (ed.getStageMode() == ui::StageMode::amp, "back to the amp");
            expect (ed.getSelectedBlock() == ui::BlockId::delay && ed.getInspector().getContent() == &ed.getPage (ui::BlockId::delay));

            // From the cab, it goes back to the cab.
            ed.setStageMode (ui::StageMode::cab, false);
            ed.selectBlock (ui::BlockId::postEq);
            ed.finishAnimations();
            expect (ed.getStageMode() == ui::StageMode::eq);
            ed.selectBlock (ui::BlockId::reverb);
            ed.finishAnimations();
            expect (ed.getStageMode() == ui::StageMode::cab && ed.getSelectedBlock() == ui::BlockId::reverb);

            // Picked on the stage itself, EQ stays when another pedal is selected.
            auto& control = ed.getStage().getModeControl();
            click (control, control.optionArea (2).getCentre());
            ed.finishAnimations();
            ed.selectBlock (ui::BlockId::chorus);
            ed.finishAnimations();
            expect (ed.getStageMode() == ui::StageMode::eq && ed.getSelectedBlock() == ui::BlockId::chorus);
            logMessage ("  -> amp stage: the post EQ pedal showed the post graph, the pre EQ pedal the pre one, the delay put the amp back (the delay in the "
                        "inspector); from the cab it went back to the cab; EQ picked on the stage stayed with the chorus selected");
        }

        beginTest ("the harmonizer's display reads what it hears, live; the voice table dims the unused interval and a voice that's off");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "harm_on", 1.0f);
            setParam (p, "harm_v1_on", 1.0f);
            setParam (p, "harm_v2_on", 0.0f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::harmonizer);
            auto& page = dynamic_cast<ui::HarmonizerPage&> (ed.getPage (ui::BlockId::harmonizer));
            ed.refresh();
            expectEquals (page.getHearing().getText(), juce::String ("Listening"));

            // A held A (220 Hz), clean: the display shows the note the harmonizer reports, and voice 1's.
            play (p, sine (220.0, 0.3, (int) (1.0 * fs)));
            ed.refresh();
            const auto reading = page.getHearing().getReading();
            expectEquals (reading.note, p.getHarmonizerNote());
            expect (reading.note >= 0, "a note is heard");
            expect (page.getHearing().getText().startsWith ("Hearing A3"), page.getHearing().getText());
            expect (savePng (page.getHearing().createComponentSnapshot (page.getHearing().getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("harmonizer_hearing.png")));

            // Voice 1 diatonic: its semitones dimmed; voice 2 off: dimmed whole.
            expect (page.getVoiceField (0, 1).getAlpha() < page.getVoiceField (0, 0).getAlpha());
            expect (page.getVoiceField (1, 3).getAlpha() < 1.0f && juce::exactlyEqual (page.getVoiceField (0, 3).getAlpha(), 1.0f));
            // The table: each voice's rows inside the column, one under the other.
            expect (page.getVoiceRow (1).getY() > page.getVoiceRow (0).getBottom() && page.getVoiceRow (3).getRight() <= page.getWidth());
            logMessage ("  -> silence: \"Listening\"; a 220 Hz tone: \"" + page.getHearing().getText() + "\"; voice 1's semitones at alpha "
                        + juce::String (page.getVoiceField (0, 1).getAlpha(), 2) + ", voice 2 (off) at " + juce::String (page.getVoiceField (1, 3).getAlpha(), 2)
                        + "; harmonizer_hearing.png");
        }

        beginTest ("the delay's Tap pad taps the tempo and blinks at it; the reverb's Freeze is a pad; Bloom's chips show which effects are on");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::delay);
            auto& delay = dynamic_cast<ui::DelayPage&> (ed.getPage (ui::BlockId::delay));
            auto& tap = delay.getTapPad();

            // Four taps 400 ms apart (150 BPM), with the audio running between them as it would.
            for (int t = 0; t < 4; ++t)
            {
                tap.triggerClick();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
                play (p, std::vector<float> ((size_t) (0.4 * fs), 0.0f));
            }
            p.runHousekeeping();
            ed.refresh();
            const auto tapped = p.getTempo();
            expectWithinAbsoluteError (tapped, 150.0, 3.0);
            expectWithinAbsoluteError (tap.getTempo(), tapped, 0.1);
            expect (findTagged (delay, "tempo_bpm") != nullptr);

            ed.selectBlock (ui::BlockId::reverb);
            auto& reverb = dynamic_cast<ui::ReverbPage&> (ed.getPage (ui::BlockId::reverb));
            expect (dynamic_cast<ui::PadSwitch*> (&reverb.getFreezePad()) != nullptr);
            reverb.getFreezePad().setToggleState (true, juce::sendNotificationSync);
            expectEquals (getParam (p, "reverb_freeze"), 1.0f);
            reverb.getFreezePad().setToggleState (false, juce::sendNotificationSync);

            // The plate has no shimmer: its knob dims there.
            setParam (p, "reverb_engine", 2.0f);
            ed.refresh();
            expect (findTaggedAs<ui::Knob> (reverb, "reverb_shimmer")->getAlpha() < 1.0f);
            setParam (p, "reverb_engine", 1.0f);
            ed.refresh();
            expect (juce::exactlyEqual (findTaggedAs<ui::Knob> (reverb, "reverb_shimmer")->getAlpha(), 1.0f));

            // Bloom: an effect that's off has its controls dimmed.
            setParam (p, "bloom_phaser_on", 0.0f);
            setParam (p, "bloom_flanger_on", 1.0f);
            ed.selectBlock (ui::BlockId::bloom);
            ed.refresh();
            auto& bloom = ed.getPage (ui::BlockId::bloom);
            expect (findTaggedAs<ui::Knob> (bloom, "bloom_phaser_depth")->getAlpha() < 1.0f);
            expect (juce::exactlyEqual (findTaggedAs<ui::Knob> (bloom, "bloom_flanger_depth")->getAlpha(), 1.0f));
            logMessage ("  -> four taps 400 ms apart: " + juce::String (tapped, 1) + " BPM, the pad showing " + juce::String (tap.getTempo(), 1)
                        + "; Freeze's pad set reverb_freeze; the plate dimmed the shimmer; Bloom's phaser off dimmed its knobs, the flanger on kept its own");
        }

        beginTest ("the cab's stage: the speaker in its baffle with the mics on it, large; a pack's mics drag, with its capture positions; the board's markers");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            p.loadCabIR (0, writePack (tempDir().getChildFile ("inspector_pack_a")));
            p.loadCabIR (1, writePack (tempDir().getChildFile ("inspector_pack_b")));
            waitForLoads (p);
            setParam (p, AmpSimProcessor::cabParamId (0, "pos_x"), 0.2f);
            setParam (p, AmpSimProcessor::cabParamId (1, "pos_x"), 0.6f);
            setParam (p, AmpSimProcessor::cabParamId (1, "pos_y"), 0.8f);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.selectBlock (ui::BlockId::cab);
            ed.finishAnimations();
            ed.refresh();
            auto& cab = ed.getCabView();
            expect (! cab.isMarkerDimmed (0) && ! cab.isMarkerDimmed (1));
            expect (cab.getReadout().startsWith ("Mic A|"), cab.getReadout());

            // The speaker is the stage's centre and its largest part: square, at least 280 px.
            const auto a = cab.markerPosition (0);
            const auto from = a;
            cab.dragMarker (0, from, from + juce::Point<float> (60.0f, 0.0f));
            ed.refresh();
            const auto moved = getParam (p, AmpSimProcessor::cabParamId (0, "pos_x"));
            expect (std::abs (moved - 0.2f) > 0.05f, juce::String (moved));
            expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), redesignDir().getChildFile ("stage_cab_pack.png")));

            // The mics' controls are in the inspector: distance, level, phase, pan, delay, mute, for both.
            for (int m = 0; m < 2; ++m)
                for (const auto* what : { "pos_y", "level", "invert", "pan", "delay", "mute" })
                    expect (findTagged (ed.getInspector(), AmpSimProcessor::cabParamId (m, what)) != nullptr, AmpSimProcessor::cabParamId (m, what));

            // The board's markers: the amp's head and the cab's front, each with its jewel.
            auto& board = ed.getBoard();
            auto* amp = board.getMarker (ui::BlockId::amp);
            auto* cabMarker = board.getMarker (ui::BlockId::cab);
            const auto area = amp->getBounds().getUnion (cabMarker->getBounds()).expanded (16, 8);
            expect (savePng (board.createComponentSnapshot (area, true, 3.0f), redesignDir().getChildFile ("board_markers.png")));
            setParam (p, "cab_bypass", 1.0f);
            board.refresh();
            expect (savePng (board.createComponentSnapshot (area, true, 3.0f), redesignDir().getChildFile ("board_markers_cab_off.png")));
            setParam (p, "cab_bypass", 0.0f);
            logMessage ("  -> both mics on packs: \"" + cab.getReadout().replace ("|", " ") + "\"; mic A dragged 60 px: pos_x " + juce::String (moved, 3)
                        + "; every mic control in the inspector; stage_cab_pack.png, board_markers.png, board_markers_cab_off.png");
        }
    }
};

InspectorTests inspectorTests;
} // namespace

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The overlays (the app redesign, phase 3: docs/UI_DESIGN.md "The overlays"): the tuner built to read from a step
// back and the footer's reading, Settings in four sections, Match tone as six numbered steps, and the menus and
// dialogs in the same world. Snapshots in build/proof/redesign/ (overlay_*.png); the tone match states come from the
// tone match tests (redesignCopy).

#include "PluginEditor.h"
#include "TestHelpers.h"
#include "platform/AppInfo.h"
#include "ui/Overlay.h"

namespace
{
using namespace testing;

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

juce::File redesignDir() { return proofDir().getChildFile ("redesign"); }
juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

float getParam (AmpSimProcessor& p, const juce::String& id) { return p.parameters.getRawParameterValue (id)->load(); }

ampsim::TunerReading readingOf (int midiNote, double cents, double a4 = 440.0)
{
    ampsim::TunerReading r;
    r.hasReading = r.live = true;
    r.midiNote = midiNote;
    r.cents = cents;
    r.referenceA4 = a4;
    r.frequency = a4 * std::pow (2.0, (midiNote - 69 + cents / 100.0) / 12.0);
    r.levelDb = -20.0;
    return r;
}

juce::MouseEvent mouseEvent (juce::Component& c, juce::Point<float> at)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, 1, false);
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

/// Whether `child`'s bounds, in `page`'s coordinates, lie inside `area`.
bool inside (juce::Component& page, juce::Component& child, juce::Rectangle<int> area)
{
    return area.contains (page.getLocalArea (child.getParentComponent(), child.getBounds()));
}

class OverlayTests final : public juce::UnitTest
{
public:
    OverlayTests() : juce::UnitTest ("Overlays", "ampsim") {}

    void runTest() override
    {
        beginTest ("the tuner: the note, the chevrons, the segmented scale lit between segments, the string chips (click to target), the controls, its own Close; snapshots");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.openOverlay (ui::Overlay::tuner);
            auto& tuner = ed.getTunerPage();
            tuner.setTuning (0);
            expectEquals (tuner.getBounds().toString(), juce::String ("40 16 1200 598"));

            // The scale: a segment lights fully under the cents and fades to dark 3.75 cents away, so the position
            // reads between two segments; past the ends the end segment lights, as a pinned needle.
            expectWithinAbsoluteError (ui::TunerPage::segmentLight (10.0, 10.0), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (ui::TunerPage::segmentLight (10.0, 11.25), 2.0f / 3.0f, 1.0e-5f);
            expectWithinAbsoluteError (ui::TunerPage::segmentLight (12.5, 11.25), 2.0f / 3.0f, 1.0e-5f);
            expectEquals (ui::TunerPage::segmentLight (5.0, 11.25), 0.0f);
            expectWithinAbsoluteError (ui::TunerPage::segmentLight (50.0, 180.0), 1.0f, 1.0e-6f);
            expectEquals (ui::TunerPage::numSegments, 41);

            juce::StringArray files, report;
            const auto show = [&] (const ampsim::TunerReading& r, const juce::String& name)
            {
                tuner.freeze (r);
                ed.refresh();
                ed.getTunerReadout().set (true, r);
                const auto file = redesignDir().getChildFile (name + ".png");
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), file));
                files.add (file.getFileName());
                // The footer's reading alone, at 3x: the glanceable version.
                const auto footer = ed.getTunerReadout().createComponentSnapshot (ed.getTunerReadout().getLocalBounds(), true, 3.0f);
                const auto crop = redesignDir().getChildFile (name + "_footer.png");
                expect (savePng (footer, crop));
                files.add (crop.getFileName());
                report.add (name + ": \"" + tuner.getShown().note + tuner.getShown().accidental + tuner.getShown().octave + " " + tuner.getShown().cents + " "
                            + tuner.getShown().state + "\", chevrons " + juce::String (tuner.getLitChevrons()) + ", footer \"" + ed.getTunerReadout().getText() + "\"");
            };

            // In tune, flat, sharp: the chevrons say which way (both in tune).
            show (readingOf (40, 1.6), "overlay_tuner_in_tune");
            expect (tuner.getShown().inTune && tuner.getLitChevrons() == 2);
            expectEquals (ed.getTunerReadout().getText(), juce::String ("E2 +2 ct In tune"));
            show (readingOf (45, -18.0), "overlay_tuner_flat");
            expect (tuner.getLitChevrons() == -1 && tuner.getShown().state == "Flat" && tuner.getShown().currentString == 1);
            expectEquals (ed.getTunerReadout().getText(), juce::String ("A2 -18 ct Flat"));
            show (readingOf (54, 27.0), "overlay_tuner_sharp");
            expect (tuner.getLitChevrons() == 1 && tuner.getShown().note == "F" && tuner.getShown().accidental.isNotEmpty() && tuner.getShown().state == "Sharp");
            show (ampsim::TunerReading {}, "overlay_tuner_listening");
            expect (tuner.getLitChevrons() == 0 && tuner.getShown().state == "Play a string");

            // The strings are chips: a click on one targets it (its number and the accent), a second lets go.
            expect (tuner.getStringBounds (0).getWidth() >= 90.0f && tuner.getStringBounds (0).getHeight() >= 90.0f, "chips big enough to hit and read");
            for (int i = 1; i < 6; ++i)
                expect (tuner.getStringBounds (i).getX() > tuner.getStringBounds (i - 1).getRight(), "left to right, apart");
            tuner.setTuning (2); // Drop C
            const auto chip = tuner.getStringBounds (0).getCentre();
            tuner.mouseUp (mouseEvent (tuner, chip));
            expectEquals (tuner.getTarget(), 0);
            show (readingOf (36, -6.0), "overlay_tuner_target_drop_c");
            expect (tuner.getShown().note == "C" && tuner.getShown().currentString == 0);
            tuner.mouseUp (mouseEvent (tuner, chip));
            expectEquals (tuner.getTarget(), -1);
            tuner.setTuning (0);

            // The controls are in the page: the tunings (the pill), A4, Mute output; its own Close.
            expect (tuner.getTuningChoice().isPillStyle() && tuner.getTuningChoice().getNumOptions() == 4);
            const auto ids = taggedIn (tuner);
            expect (ids.contains ("tuner_a4") && ids.contains ("tuner_mute"), ids.joinIntoString (", "));
            expect (tuner.getDisplayArea().getHeight() > 300 && tuner.getDisplayArea().getWidth() == tuner.getWidth());
            tuner.getCloseButton().triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
            expect (ed.getOverlay() == ui::Overlay::none);
            expectEquals (getParam (p, "tuner_on"), 0.0f);
            logMessage ("  -> " + report.joinIntoString ("; "));
            logMessage ("  -> segment light 2/3 either side at 11.25 cents (between the 10 and 12.5 segments); Close (its own) closed it and disengaged the tuner");
            logMessage ("  -> build/proof/redesign/: " + files.joinIntoString (", "));
        }

        beginTest ("Settings: four sections in a list (Input; Output and MIDI; Audio device; About), every control in its section, 48 kHz said plainly, About's licences, updates, and source; snapshots");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.openOverlay (ui::Overlay::settings);
            auto& settings = ed.getSettingsPage();
            using S = ui::SettingsPage::Section;

            juce::StringArray files, sections;
            for (int i = 0; i < ui::SettingsPage::numSections; ++i)
            {
                const auto s = (S) i;
                settings.getSectionButton (s).triggerClick();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
                expect (settings.getSection() == s);
                auto& page = settings.getSectionPage (s);
                expect (page.isVisible(), ui::SettingsPage::sectionName (s));
                for (int j = 0; j < ui::SettingsPage::numSections; ++j)
                    if (j != i)
                        expect (! settings.getSectionPage ((S) j).isVisible());
                expect (settings.getSectionButton (s).getToggleState());
                juce::StringArray panels;
                for (const auto& panel : page.getPanels())
                {
                    expect (page.getLocalBounds().contains (panel.area), panel.title);
                    panels.add (panel.title.isEmpty() ? "(name)" : panel.title);
                }
                sections.add (ui::SettingsPage::sectionName (s) + ": " + panels.joinIntoString (", ") + " [" + taggedIn (page).joinIntoString (" ") + "]");
                ed.refresh();
                const auto name = juce::String ("overlay_settings_") + (i == 0 ? "input" : i == 1 ? "output" : i == 2 ? "device" : "about");
                const auto file = redesignDir().getChildFile (name + ".png");
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), file));
                files.add (file.getFileName());
            }

            // Every control the old pages had, in its section.
            const auto inputIds = taggedIn (settings.getInputPage()), outputIds = taggedIn (settings.getOutputPage());
            for (const auto* id : { "input_gain", "input_calibrate", "input_level_dbu" })
                expect (inputIds.contains (id), id);
            for (const auto* id : { "output_gain", "output_limit_on", "output_limit_ceiling", "cpu_saver", "tempo_bpm", "midi_tap_cc", "midi_freeze_cc", "midi_scene_cc" })
                expect (outputIds.contains (id), id);
            expect (settings.getOutputPage().getMidiButton().isVisible());

            // The input and output blocks still open their sections (selectBlock), and showOutput still works.
            ed.selectBlock (ui::BlockId::output);
            expect (ed.getOverlay() == ui::Overlay::settings && settings.isShowingOutput());
            ed.selectBlock (ui::BlockId::input);
            expect (settings.getSection() == S::input);

            // The rate, plainly: at 48 kHz it says so; at 44.1 kHz it says the output is muted and what to do.
            const auto atRate = settings.getDevicePage().getRateText();
            expectEquals (atRate, juce::String ("Running at 48 kHz."));
            AmpSimProcessor wrong;
            wrong.prepareToPlay (44100.0, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> wrongEditor (wrong.createEditor());
            auto& wrongEd = dynamic_cast<AmpSimEditor&> (*wrongEditor);
            wrongEd.openOverlay (ui::Overlay::settings);
            wrongEd.getSettingsPage().showSection (S::device);
            wrongEd.refresh();
            const auto wrongRate = wrongEd.getSettingsPage().getDevicePage().getRateText();
            expectEquals (wrongRate, juce::String ("The device is at 44.1 kHz, so the output is muted. Set it to 48 kHz."));
            const auto wrongFile = redesignDir().getChildFile ("overlay_settings_device_44k.png");
            expect (savePng (wrongEditor->createComponentSnapshot (wrongEditor->getLocalBounds(), true, 2.0f), wrongFile));
            files.add (wrongFile.getFileName());
            // No device of its own in the tests: its settings button is off, and hidden with the device's card folded to a
            // line (RD4-10), not a half-empty card round a disabled button. The rate is one line of status, not a label
            // over a giant figure: its panel is 56 high and its text on one line.
            auto& device = settings.getDevicePage();
            expect (! device.getDeviceButton().isEnabled());
            expect (device.isDeviceCardFolded() && ! device.getDeviceButton().isVisible());
            expectLessOrEqual (device.getStatusArea().getHeight(), 30, "one line");
            juce::StringArray panelSizes;
            for (const auto& panel : device.getPanels())
            {
                panelSizes.add ((panel.title.isEmpty() ? juce::String ("status") : panel.title) + " " + juce::String (panel.area.getHeight()));
                if (panel.title == "This device")
                    expectLessOrEqual (panel.area.getHeight(), 80, "folded to a line");
                expect (panel.title != "Sample rate", "no tracked label over a giant 48 kHz");
            }
            logMessage ("  -> the device page's panels (height): " + panelSizes.joinIntoString (", "));

            // About: the licences open the editor's About window (wired), updates only when an updater runs, the
            // source of this version.
            auto& about = settings.getAboutPage();
            expect (settings.onLicences != nullptr && about.onLicences != nullptr);
            expect (! about.getUpdatesButton().isEnabled(), "no updater in the tests");
            expectEquals (about.getSourceButton().getTooltip(), platform::sourceUrlForThisVersion());

            logMessage ("  -> " + sections.joinIntoString ("; "));
            logMessage ("  -> the rate at 48 kHz: \"" + atRate + "\"; at 44.1 kHz: \"" + wrongRate + "\"");
            logMessage ("  -> build/proof/redesign/: " + files.joinIntoString (", "));
        }

        beginTest ("Match tone: six numbered steps in reading order, each control in its step, the badges following the flow");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.openOverlay (ui::Overlay::toneMatch);
            auto& page = ed.getToneMatchPage();
            auto& session = page.getSession();
            page.refresh();

            // Two rows of three, read left to right, then top to bottom; inside the page, apart.
            juce::StringArray layout;
            for (int s = 1; s <= ui::ToneMatchPage::numSteps; ++s)
            {
                const auto a = page.getStepArea (s);
                expect (page.getLocalBounds().contains (a), juce::String (s));
                layout.add (juce::String (s) + " " + ui::ToneMatchPage::stepTitle (s) + " " + a.toString());
                if (s % 3 != 1)
                    expect (a.getY() == page.getStepArea (s - 1).getY() && a.getX() > page.getStepArea (s - 1).getRight(), "same row, to the right");
                for (int t = 1; t < s; ++t)
                    expect (! a.intersects (page.getStepArea (t)), "steps don't overlap");
            }
            expect (page.getStepArea (4).getY() > page.getStepArea (1).getBottom());

            // Each control in its step, and every control the page has is in one.
            juce::StringArray misplaced;
            const auto controls = page.getStepControls();
            for (const auto& [c, step] : controls)
                if (! inside (page, *c, page.getStepArea (step)))
                    misplaced.add (c->getName() + " " + c->getBounds().toString() + " not in step " + juce::String (step));
            expect (misplaced.isEmpty(), misplaced.joinIntoString ("; "));
            int listed = 0;
            for (auto* child : page.getChildren())
                for (const auto& [c, step] : controls)
                    listed += c == child ? 1 : 0;
            expectEquals (listed, page.getNumChildComponents() - 1); // all but the header's Close
            expect (inside (page, page.getModeChoice(), page.getStepArea (3)) && inside (page, page.getCleanupChoice(), page.getStepArea (3)));
            // The main actions read first.
            expect (ui::isPrimary (page.getRecordButton()) && ui::isPrimary (page.getMatchButton()) && ui::isPrimary (page.getApplyButton()));

            // The badges follow the flow.
            using St = ui::overlay::Step;
            const auto states = [&] {
                juce::String s;
                for (int i = 1; i <= ui::ToneMatchPage::numSteps; ++i)
                {
                    const auto st = page.getStepState (i);
                    s << (st == St::waiting ? "w" : st == St::next ? "n" : st == St::active ? "a" : "d");
                }
                return s;
            };
            const auto empty = states();
            expectEquals (empty, juce::String ("nwwwww"));
            expect (session.setTargetFile (fixtures().getChildFile ("target_anything.wav")));
            page.refresh();
            const auto withSong = states();
            expectEquals (withSong, juce::String ("dnwwww"));
            expect (page.getStepNote (1).contains (" to "), page.getStepNote (1));
            page.toggleTargetPlay();
            expectEquals (states().substring (0, 1), juce::String ("a"));
            page.toggleTargetPlay();
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            page.refresh();
            const auto withDi = states();
            expectEquals (withDi, juce::String ("ddnwww"));
            logMessage ("  -> " + layout.joinIntoString ("; "));
            logMessage ("  -> badges (w waiting, n next, a running, d done): empty " + empty + ", with a song " + withSong + " (Play makes 1 running), with a DI " + withDi
                        + "; the match, result, and Apply states are checked in \"Tone match app\"");
        }

        beginTest ("menus and dialogs: rounded menus with keycaps and section labels, a dialog as a rounded panel with its main button; snapshots");
        {
            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ui::LookAndFeel lf;

            // The menu's colour is a hair under opaque, so its window is transparent and the corners can be rounded.
            expect (! lf.findColour (juce::PopupMenu::backgroundColourId).isOpaque());

            // A menu drawn as JUCE draws it: the background, then each item (the brand menu's, a section, a tick, a key).
            const auto menu = ed.brandMenu();
            struct Row
            {
                juce::String text, key;
                bool separator = false, active = true, ticked = false, header = false, sub = false;
            };
            std::vector<Row> rows;
            rows.push_back ({ "Brand", {}, false, true, false, true });
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                rows.push_back ({ it.getItem().text, {}, it.getItem().isSeparator, it.getItem().isEnabled });
            rows.push_back ({ {}, {}, true });
            rows.push_back ({ "Window", {}, false, true, false, true });
            rows.push_back ({ "Tuner", "T", false, true, true });
            rows.push_back ({ "Built-in IRs", {}, false, true, false, false, true });
            const int width = 300;
            int height = 8;
            for (const auto& r : rows)
                height += r.separator ? 9 : (r.header ? 28 : 30);
            juce::Image image (juce::Image::ARGB, width * 2, height * 2, true);
            {
                juce::Graphics g (image);
                g.addTransform (juce::AffineTransform::scale (2.0f));
                lf.drawPopupMenuBackground (g, width, height);
                int y = 4;
                for (const auto& r : rows)
                {
                    const auto h = r.separator ? 9 : (r.header ? 28 : 30);
                    const juce::Rectangle<int> area (4, y, width - 8, h);
                    if (r.header)
                        lf.drawPopupMenuSectionHeader (g, area, r.text);
                    else
                        lf.drawPopupMenuItem (g, area, r.separator, r.active, r.text == "Match tone...", r.ticked, r.sub, r.text, r.key, nullptr, nullptr);
                    y += h;
                }
            }
            expect (image.getPixelAt (0, 0).getAlpha() == 0, "a rounded corner");
            expect (image.getPixelAt (width, height) == ui::theme::raised, image.getPixelAt (width, height).toDisplayString (true));
            const auto menuFile = redesignDir().getChildFile ("overlay_menu.png");
            expect (savePng (image, menuFile));

            // The scene Rename dialog: an AlertWindow in the editor's LookAndFeel, its main button primary.
            juce::AlertWindow window ("Rename scene 2", {}, juce::MessageBoxIconType::NoIcon);
            window.setLookAndFeel (&lf);
            window.addTextEditor ("name", "Lead");
            window.addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
            window.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            ui::setPrimary (*window.getButton (0));
            window.setOpaque (false);
            const auto dialog = window.createComponentSnapshot (window.getLocalBounds(), true, 2.0f);
            expect (dialog.getPixelAt (0, 0).getAlpha() == 0, "a rounded corner");
            expect (dialog.getPixelAt (dialog.getWidth() / 2, 6) == ui::theme::panelBg, dialog.getPixelAt (dialog.getWidth() / 2, 6).toDisplayString (true));
            const auto dialogFile = redesignDir().getChildFile ("overlay_dialog_rename.png");
            expect (savePng (dialog, dialogFile));
            window.setLookAndFeel (nullptr);
            logMessage ("  -> build/proof/redesign/" + menuFile.getFileName() + " (" + juce::String ((int) rows.size()) + " rows, the brand menu's own), "
                        + dialogFile.getFileName() + " (" + juce::String (window.getWidth()) + " x " + juce::String (window.getHeight()) + ")");
        }
    }
};

OverlayTests overlayTests;
} // namespace

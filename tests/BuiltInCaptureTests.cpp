// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The built-in captures (content/models: Glass, Ember, Monolith; BUILD_PLAN decision log 2026-10-03) and the
// renamed factory presets: a fresh start loads them, the factory presets use them, the capture menu puts
// one back, a cleared slot stays empty, old preset names still resolve, and renders for Sean's ears.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "Presets.h"
#include "dsp/GainSet.h"
#include "TestHelpers.h"
#include "platform/AppInfo.h"

#include <map>

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

juce::String modelPath (AmpSimProcessor& p, int slot)
{
    return p.parameters.state.getProperty (AmpSimProcessor::modelPathKey (slot)).toString();
}

/// Runs the processor on the input in 128-sample buffers (this thread is its audio thread).
Stereo play (AmpSimProcessor& p, const std::vector<float>& input)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    Stereo out;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        out.left.insert (out.left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        out.right.insert (out.right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize);
    }
    return out;
}

/// Whether each slot's model is running (the audio thread's view, after a buffer has picked up the loads).
std::array<bool, 3> running (AmpSimProcessor& p)
{
    play (p, std::vector<float> (blockSize, 0.0f));
    std::array<bool, 3> r {};
    for (int s = 0; s < 3; ++s)
        r[(size_t) s] = p.getChain().amp.slot (s).model.hasModel();
    return r;
}

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

const juce::PopupMenu::Item* findItem (const juce::PopupMenu& menu, const juce::String& startsWith)
{
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        if (it.getItem().text.startsWith (startsWith))
            return &it.getItem();
    return nullptr;
}

juce::var metadataOf (const juce::File& nam)
{
    return juce::JSON::parse (nam.loadFileAsString()).getProperty ("metadata", {});
}

class BuiltInCaptureTests final : public juce::UnitTest
{
public:
    BuiltInCaptureTests() : juce::UnitTest ("Built-in captures", "ampsim") {}

    void runTest() override
    {
        const std::array<const char*, 3> names { "Glass", "Ember", "Monolith" };
        const std::array<const char*, 3> toneTypes { "clean", "crunch", "hi_gain" };
        const std::array<const char*, 3> voices { "Clean", "Crunch", "High gain" };

        beginTest ("the three built-in captures ship in the content folder with their metadata and manifest entries");
        {
            const auto manifest = juce::JSON::parse (juce::File (AMPSIM_SOURCE_DIR).getChildFile ("content/manifest.json").loadFileAsString());
            juce::StringArray described;
            for (int s = 0; s < 3; ++s)
            {
                // Each built-in is a gain set since 2026-10-04 (BUILD_PLAN "Amp gain"): content/models/<Amp>/gainset.json
                // and its five captures, each with its metadata and its own manifest entry.
                const auto file = presets::builtInCapture (s);
                expect (file.existsAsFile() && file.isAChildOf (platform::factoryContentFolder()), file.getFullPathName());
                expectEquals (presets::builtInCapturePath (s), "factory:models/" + juce::String (names[(size_t) s]) + "/gainset.json");
                expect (presets::isBundled (file));
                ampsim::GainSet set;
                juce::String error;
                expect (ampsim::GainSet::read (file, set, error), error);
                expectEquals (set.name, juce::String (names[(size_t) s]));
                expectEquals (set.toneType, juce::String (toneTypes[(size_t) s]));
                juce::int64 bytes = 0;
                for (const auto& step : set.steps)
                {
                    const auto m = metadataOf (step.file);
                    expectEquals (m["name"].toString(), juce::String (names[(size_t) s]) + ", gain " + juce::String (step.gain, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."));
                    expectEquals (m["modeled_by"].toString(), juce::String ("BellyDSP"));
                    expectEquals (m["gear_type"].toString(), juce::String ("amp"));
                    expectEquals (m["tone_type"].toString(), juce::String (toneTypes[(size_t) s]));
                    expectWithinAbsoluteError ((double) m["input_level_dbu"], 12.0, 1.0e-9);
                    bytes += step.file.getSize();

                    const juce::var* entry = nullptr;
                    if (const auto* files = manifest["files"].getArray())
                        for (const auto& f : *files)
                            if (f["path"].toString() == "models/" + juce::String (names[(size_t) s]) + "/" + step.file.getFileName())
                                entry = &f;
                    expect (entry != nullptr, step.file.getFileName());
                    if (entry != nullptr)
                    {
                        expectEquals ((*entry)["license"].toString(), juce::String ("CC BY 4.0"));
                        expectEquals ((*entry)["author"].toString(), juce::String ("Sean Snaider"));
                        expect ((*entry)["notes"].toString().contains ("prototypes/amp_sim.py"));
                    }
                }
                described.add (juce::String (names[(size_t) s]) + " (" + set.toneType + ", a gain set of " + juce::String ((int) set.steps.size()) + ", "
                               + juce::String (bytes / 1024) + " KB, input_level_dbu 12)");
            }
            logMessage ("  -> content/models: " + described.joinIntoString (", ") + "; each with a CC BY 4.0 manifest entry");
        }

        beginTest ("a fresh processor and editor start on Glass, Ember, and Monolith, loaded off the audio thread, with no warnings");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            const auto loadingAtOnce = p.isLoading(); // the constructor only queued them
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            expect (loadingAtOnce);
            const auto status = p.getStatus();
            const auto on = running (p);
            for (int s = 0; s < 3; ++s)
            {
                expectEquals (modelPath (p, s), presets::builtInCapture (s).getFullPathName());
                expect (! status.modelError[(size_t) s] && status.model[(size_t) s] != "Empty", status.model[(size_t) s]);
                expect (on[(size_t) s]);
            }
            expect (status.warning.isEmpty(), status.warning);

            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            auto& amp = ed.getAmpView();
            const auto input = guitarDI ((int) (1.0 * fs));
            juce::StringArray rows;
            proofDir().getChildFile ("default_captures").createDirectory();
            for (int s = 0; s < 3; ++s)
            {
                setParam (p, AmpSimProcessor::slotParamId, (float) s);
                const auto out = play (p, input);
                amp.getSpectrum().update();
                ed.refresh();
                expectEquals (amp.getVoiceText(), juce::String (voices[(size_t) s]));
                expectEquals (amp.getModelText(), juce::String (names[(size_t) s]) + " (built in, gain set)");
                expectEquals (amp.getRateText(), juce::String ("48 kHz"));
                expect (amp.getJewel().isLit());
                expect (ed.getStatusText().isEmpty(), ed.getStatusText());
                expect (rms (out.left) > 1.0e-3);
                const auto file = proofDir().getChildFile ("default_captures/editor_amp_" + juce::String (names[(size_t) s]).toLowerCase() + ".png");
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), file));
                rows.add ("\"" + amp.getVoiceText() + "  Model " + amp.getModelText() + "  " + amp.getRateText() + "\"");
            }
            logMessage ("  -> a fresh start: slots 1 to 3 loaded " + juce::String (names[0]) + ", " + names[1] + ", " + names[2]
                        + " on the loader thread (still loading when the constructor returned), no errors or warnings; the amp page's info rows: "
                        + rows.joinIntoString (", ") + "; snapshots in default_captures/editor_amp_*.png");
        }

        beginTest ("the capture menu's \"Use the built-in capture\" puts a slot's own back; a cleared slot stays empty when the state is restored");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);

            p.loadModel (1, exampleModel ("A2.nam"));
            waitForLoads (p);
            const auto replaced = modelPath (p, 1) == exampleModel ("A2.nam").getFullPathName();
            const auto menu = ed.captureMenu (1);
            const auto* use = findItem (menu, "Use the built-in capture");
            expect (use != nullptr);
            const auto label = use != nullptr ? use->text : juce::String();
            const auto enabledWhileReplaced = use != nullptr && use->isEnabled;
            if (use != nullptr && use->action)
                use->action();
            waitForLoads (p);
            const auto restored = modelPath (p, 1) == presets::builtInCapture (1).getFullPathName();
            const auto menuOnBuiltIn = ed.captureMenu (1);
            const auto* useAgain = findItem (menuOnBuiltIn, "Use the built-in capture");
            expect (replaced && enabledWhileReplaced && restored);
            expectEquals (label, juce::String ("Use the built-in capture (Ember)"));
            expect (useAgain != nullptr && ! useAgain->isEnabled); // already on it
            expect (! p.getStatus().modelError[1]);

            // Clear slot 3 from the menu: saved as an empty path, and it stays empty in a restored state, even
            // though the restoring processor started loading Monolith before the state arrived.
            const auto clearMenu = ed.captureMenu (2);
            const auto* clear = findItem (clearMenu, "Clear the slot");
            expect (clear != nullptr && clear->isEnabled);
            if (clear != nullptr && clear->action)
                clear->action();
            waitForLoads (p);
            expect (p.parameters.state.hasProperty (AmpSimProcessor::modelPathKey (2)) && modelPath (p, 2).isEmpty());
            p.loadModel (0, exampleModel ("lstm.nam"));
            waitForLoads (p);
            juce::MemoryBlock saved;
            p.getStateInformation (saved);

            AmpSimProcessor q;
            q.setStateInformation (saved.getData(), (int) saved.getSize());
            waitForLoads (q);
            q.prepareToPlay (fs, blockSize);
            const auto on = running (q);
            expectEquals (modelPath (q, 0), exampleModel ("lstm.nam").getFullPathName()); // its own capture, untouched
            expectEquals (modelPath (q, 1), presets::builtInCapture (1).getFullPathName());
            expect (modelPath (q, 2).isEmpty());
            expect (on[0] && on[1] && ! on[2]);
            expectEquals (q.getStatus().model[2], juce::String ("Empty"));

            // A state saved before the built-ins existed has no entries for its unused slots: they get theirs.
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
            xml->removeAttribute (AmpSimProcessor::modelPathKey (1).toString());
            xml->removeAttribute (AmpSimProcessor::modelPathKey (2).toString());
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
            AmpSimProcessor r;
            r.setStateInformation (old.getData(), (int) old.getSize());
            waitForLoads (r);
            r.prepareToPlay (fs, blockSize);
            const auto onOld = running (r);
            expectEquals (modelPath (r, 0), exampleModel ("lstm.nam").getFullPathName());
            expectEquals (modelPath (r, 1), presets::builtInCapture (1).getFullPathName());
            expectEquals (modelPath (r, 2), presets::builtInCapture (2).getFullPathName());
            expect (onOld[0] && onOld[1] && onOld[2]);

            // Saved by a copy of the app somewhere else (a dev build, or an install since moved): the built-in
            // is found in this copy's content folder.
            const auto elsewhere = juce::File ("/nowhere/Old BellyDSP.app/Contents/Resources/content/models/Monolith.nam");
            xml->setAttribute (AmpSimProcessor::modelPathKey (2).toString(), elsewhere.getFullPathName());
            juce::MemoryBlock movedState;
            juce::AudioProcessor::copyXmlToBinary (*xml, movedState);
            AmpSimProcessor m;
            m.setStateInformation (movedState.getData(), (int) movedState.getSize());
            waitForLoads (m);
            expectEquals (modelPath (m, 2), presets::builtInCapture (2).getFullPathName());
            expect (! m.getStatus().modelError[2], m.getStatus().model[2]);
            expect (presets::bundledElsewhere (juce::File ("/nowhere/content/models/Nope.nam")) == juce::File());
            logMessage ("  -> slot 2 swapped to A2, then the menu's \"" + label + "\" loaded Ember again (and greys out while it's on); slot 3 cleared from the menu "
                        "is saved as an empty path: a restored state keeps lstm in slot 1, Ember in slot 2, and slot 3 empty (\"" + q.getStatus().model[2]
                        + "\"); a state with no entries for slots 2 and 3 (saved before the built-ins) gets Ember and Monolith there; Monolith saved by an app at \""
                        + elsewhere.getFullPathName() + "\" loads from this one's content folder");
        }

        beginTest ("the capture menu's \"Built-in amps\" lists every built-in gain set and loads any of them into any slot, which keeps its head and shows the set's name");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            auto& amp = ed.getAmpView();

            // The list: the slot defaults in slot order, then the others by tone type, then name.
            const auto sets = presets::builtInGainSets();
            juce::StringArray listed;
            for (const auto& s : sets)
                listed.add (s.name);
            expect (listed == juce::StringArray { "Glass", "Ember", "Monolith", "Lantern", "Basalt", "Comet", "Forge", "Quartz" }, listed.joinIntoString (", "));
            for (const auto& s : sets)
                expect (s.file.existsAsFile() && presets::isBundled (s.file) && s.description.isNotEmpty(), s.name);

            const auto subMenuOf = [] (const juce::PopupMenu& menu) -> const juce::PopupMenu*
            {
                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                    if (it.getItem().text == "Built-in amps")
                        return it.getItem().subMenu.get();
                return nullptr;
            };
            const auto headerOf = [] (const juce::PopupMenu& menu)
            {
                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                    if (it.getItem().isSectionHeader)
                        return it.getItem().text;
                return juce::String();
            };

            // On a fresh start slot 2 plays Ember: the submenu lists all eight, Ember ticked.
            const auto menu = ed.captureMenu (1);
            const auto* sub = subMenuOf (menu);
            expect (sub != nullptr);
            juce::StringArray items, ticked;
            if (sub != nullptr)
                for (juce::PopupMenu::MenuItemIterator it (*sub); it.next();)
                {
                    items.add (it.getItem().text);
                    if (it.getItem().isTicked)
                        ticked.add (it.getItem().text);
                }
            expectEquals (items.size(), (int) sets.size());
            expect (ticked.size() == 1 && ticked[0].startsWith ("Ember: "), ticked.joinIntoString (", "));

            // Each of the five more, into slots 1, 2, 3, 1, 2 from that slot's menu: it plays, the info row shows
            // its name, and the slot keeps its head (the menu's header still names the slot's material).
            juce::StringArray rows;
            const auto input = guitarDI ((int) (1.0 * fs));
            proofDir().getChildFile ("default_captures").createDirectory();
            for (size_t i = 3; i < sets.size(); ++i)
            {
                const auto slot = (int) (i - 3) % 3;
                const auto slotMenu = ed.captureMenu (slot);
                const auto header = headerOf (slotMenu);
                const juce::PopupMenu::Item* pick = nullptr;
                if (const auto* list = subMenuOf (slotMenu))
                    for (juce::PopupMenu::MenuItemIterator it (*list); it.next();)
                        if (it.getItem().text.startsWith (sets[i].name + ": "))
                            pick = &it.getItem();
                expect (pick != nullptr && pick->isEnabled && ! pick->isTicked, sets[i].name + ": the menu item"); 
                if (pick != nullptr && pick->action)
                    pick->action();
                waitForLoads (p);
                expectEquals (modelPath (p, slot), sets[i].file.getFullPathName());
                expect (! p.getStatus().modelError[(size_t) slot], p.getStatus().model[(size_t) slot]);
                setParam (p, AmpSimProcessor::slotParamId, (float) slot);
                const auto out = play (p, input);
                ed.refresh();
                expectEquals (amp.getModelText(), sets[i].name + " (built in, gain set)");
                expectEquals (headerOf (ed.captureMenu (slot)), header);
                expect (header.startsWith (juce::String (ui::materialName (ui::materialFor (slot)))), header);
                expect (rms (out.left) > 1.0e-3, sets[i].name + ": plays, rms " + juce::String (rms (out.left)));
                const auto menuAfter = ed.captureMenu (slot); // kept alive: the submenu pointer points into it
                const auto* again = subMenuOf (menuAfter);
                bool nowTicked = false;
                if (again != nullptr)
                    for (juce::PopupMenu::MenuItemIterator it (*again); it.next();)
                        nowTicked = nowTicked || (it.getItem().isTicked && it.getItem().text.startsWith (sets[i].name + ": "));
                expect (nowTicked, sets[i].name + ": ticked after");
                const auto png = proofDir().getChildFile ("default_captures/editor_amp_builtin_" + sets[i].name.toLowerCase() + ".png");
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), png));
                rows.add (sets[i].name + " in slot " + juce::String (slot + 1) + " (" + header + "): \"" + amp.getVoiceText() + "  Model " + amp.getModelText() + "\"");
            }
            logMessage ("  -> Built-in amps: " + items.joinIntoString (" | ") + "; loaded from the menu: " + rows.joinIntoString ("; ")
                        + "; snapshots in default_captures/editor_amp_builtin_*.png");
        }

        beginTest ("the amp picker right of the tabs: lists every built-in amp, loads the pick into the playing slot, its arrows step through them and wrap, and the badge and the picker name the amp");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            ed.refresh();
            auto& amp = ed.getAmpView();
            const auto sets = presets::builtInGainSets();
            const auto snap = [&] (const juce::String& name)
            {
                proofDir().getChildFile ("default_captures").createDirectory();
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f),
                                 proofDir().getChildFile ("default_captures/editor_amp_picker_" + name + ".png")));
            };

            // Fresh: slot 1 plays Glass; the picker and the badge say so, and the picker sits on the tabs' row,
            // right of the last tab, inside the page.
            expectEquals (amp.getPickerText(), juce::String ("Glass"));
            expectEquals (amp.getHead().getBadge(), juce::String ("Glass"));
            const auto pickerBounds = amp.getPicker().getBounds();
            const auto lastTab = amp.getTabs().getTab (AmpSimProcessor::numAmpSlots - 1);
            expect (amp.getPicker().isVisible() && amp.getLocalBounds().contains (pickerBounds));
            expect (lastTab != nullptr && pickerBounds.getX() > lastTab->getRight(), "the picker clears the tabs");
            snap ("glass");

            // The menu: a header, the eight amps (the three slot defaults, a line, the other five), Glass ticked,
            // a line, and "Load a capture file...".
            const auto menu = amp.ampMenu();
            juce::StringArray items, ticked;
            int separators = 0, header = 0;
            std::map<juce::String, std::function<void()>> actions;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                const auto& item = it.getItem();
                if (item.isSeparator)
                    ++separators;
                else if (item.isSectionHeader)
                    ++header;
                else
                {
                    items.add (item.text);
                    if (item.isTicked)
                        ticked.add (item.text);
                    actions[item.text] = item.action;
                }
            }
            expectEquals (header, 1);
            expectEquals (separators, 2);
            expectEquals (items.size(), (int) sets.size() + 1);
            for (size_t i = 0; i < sets.size(); ++i)
                expect (items[(int) i].startsWith (sets[i].name + ": "), items[(int) i]);
            expectEquals (items[items.size() - 1], juce::String ("Load a capture file..."));
            expect (ticked.size() == 1 && ticked[0].startsWith ("Glass: "), ticked.joinIntoString (", "));

            // A pick from the menu loads into the playing slot (slot 1 here) and plays.
            const auto comet = std::find_if (sets.begin(), sets.end(), [] (const presets::BuiltInAmp& a) { return a.name == "Comet"; });
            expect (comet != sets.end());
            for (const auto& [text, action] : actions)
                if (text.startsWith ("Comet: ") && action)
                    action();
            waitForLoads (p);
            ed.refresh();
            expectEquals (modelPath (p, 0), comet->file.getFullPathName());
            expect (! p.getStatus().modelError[0], p.getStatus().model[0]);
            expectEquals (amp.getPickerText(), juce::String ("Comet"));
            expectEquals (amp.getHead().getBadge(), juce::String ("Comet"));
            expectEquals (amp.slotAmpName (0), juce::String ("Comet"));
            expect (rms (play (p, guitarDI ((int) (0.5 * fs))).left) > 1.0e-3, "Comet plays");
            snap ("comet");

            // The arrows: next from Comet is the amp after it; previous twice from the first amp wraps to the last.
            juce::StringArray stepped;
            const auto indexOf = [&] (const juce::String& path)
            {
                for (size_t i = 0; i < sets.size(); ++i)
                    if (sets[i].file.getFullPathName() == path)
                        return (int) i;
                return -1;
            };
            const auto cometIndex = indexOf (comet->file.getFullPathName());
            amp.stepAmp (1);
            waitForLoads (p);
            ed.refresh();
            expectEquals (indexOf (modelPath (p, 0)), (cometIndex + 1) % (int) sets.size());
            stepped.add (amp.getPickerText());
            p.loadModel (0, sets.front().file);
            waitForLoads (p);
            ed.refresh();
            amp.stepAmp (-1);
            waitForLoads (p);
            ed.refresh();
            expectEquals (indexOf (modelPath (p, 0)), (int) sets.size() - 1);
            stepped.add (amp.getPickerText());
            expectEquals (amp.getHead().getBadge(), sets.back().name);

            // Another slot: the picker follows the playing slot; slot 2 still plays Ember on its own head.
            setParam (p, AmpSimProcessor::slotParamId, 1.0f);
            ed.refresh();
            expectEquals (amp.getPickerText(), juce::String ("Ember"));
            expectEquals (amp.getHead().getBadge(), juce::String ("Ember"));
            amp.stepAmp (1);
            waitForLoads (p);
            ed.refresh();
            expectEquals (indexOf (modelPath (p, 1)), indexOf (presets::builtInCapture (1).getFullPathName()) + 1);
            expectEquals (indexOf (modelPath (p, 0)), (int) sets.size() - 1); // slot 1 untouched
            snap ("slot2_stepped");

            // A capture that isn't built in: the picker shows its file, the badge keeps the slot's material, and
            // next starts at the first built-in amp.
            const auto own = presets::builtInCapture (2).getParentDirectory().getChildFile ("Monolith, gain 5.nam"); // one step on its own
            expect (own.existsAsFile(), own.getFullPathName());
            {
                p.loadModel (1, own);
                waitForLoads (p);
                ed.refresh();
                expectEquals (amp.getPickerText(), own.getFileNameWithoutExtension());
                expectEquals (amp.getHead().getBadge(), juce::String ("Ember"));
                amp.stepAmp (1);
                waitForLoads (p);
                ed.refresh();
                expectEquals (modelPath (p, 1), sets.front().file.getFullPathName());
            }
            logMessage ("  -> the picker at x " + juce::String (pickerBounds.getX()) + " to " + juce::String (pickerBounds.getRight()) + " of "
                        + juce::String (amp.getWidth()) + " (the last tab ends at " + juce::String (lastTab != nullptr ? lastTab->getRight() : -1)
                        + "); menu: " + items.joinIntoString (" | ") + "; next from Comet: " + stepped[0] + "; previous from Glass: " + stepped[1]
                        + "; snapshots default_captures/editor_amp_picker_*.png");
        }

        beginTest ("factory presets: renamed, and each loads the built-in captures and its bundled cab with no warnings");
        {
            const auto factory = presets::factoryPresets();
            expectEquals (factory.size(), 5);
            juce::StringArray presetNames, lines;
            for (const auto& preset : factory)
                presetNames.add (preset["name"].toString());
            expect (presetNames == juce::StringArray { "Modern Prog", "Math Rock", "Tech Death", "Metal", "Midwest Emo" }, presetNames.joinIntoString (", "));

            for (const auto& preset : factory)
            {
                AmpSimProcessor p; // built-ins off: everything here comes from the preset
                expect (p.loadPreset (preset).ok);
                waitForLoads (p);
                p.runHousekeeping();
                p.prepareToPlay (fs, blockSize);
                const auto on = running (p);
                const auto name = preset["name"].toString();
                expect (p.getPresetWarnings().isEmpty(), name + ": " + p.getPresetWarnings().joinIntoString ("; "));
                const auto status = p.getStatus();
                for (int s = 0; s < 3; ++s)
                {
                    expectEquals (presets::FileRef::fromVar (preset["amps"][s]).path, presets::builtInCapturePath (s));
                    expectEquals (modelPath (p, s), presets::builtInCapture (s).getFullPathName());
                    expect (! status.modelError[(size_t) s] && on[(size_t) s], name + " slot " + juce::String (s + 1) + ": " + status.model[(size_t) s]);
                }
                expect (! status.cabError[0] && status.cab[0] != "No IR", status.cab[0]);
                expect (juce::File (p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString()).isAChildOf (platform::factoryContentFolder()));
                expect (preset["notes"].toString().contains ("built-in gain sets"));
                juce::StringArray sceneSlots;
                for (int i = 0; i < 3; ++i)
                {
                    expect (p.recallScene (i));
                    sceneSlots.add (p.getScenes().get (i).name + " " + names[(size_t) juce::roundToInt (getParam (p, AmpSimProcessor::slotParamId))]);
                }
                lines.add (name + " (" + sceneSlots.joinIntoString (", ") + "; close mic 1 \""
                           + juce::File (p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString()).getFileNameWithoutExtension() + "\")");
            }

            // Tech Death's Boost is off (Monolith has a boost built in), still set up as the Screamer at +6 dB.
            AmpSimProcessor tech;
            expect (tech.loadPreset (factory[2]).ok);
            expect (getParam (tech, "boost_on") == 0.0f && getParam (tech, "boost_mode") == 2.0f && getParam (tech, "amp_slot") == 2.0f);
            logMessage ("  -> " + lines.joinIntoString ("; ") + ". No warnings. Tech Death's Boost is off (Monolith has its boost built in)");
        }

        beginTest ("old factory preset names resolve: a saved state on \"Polyphia\" or \"CHON\" comes back as \"Modern Prog\" or \"Math Rock\"");
        {
            expectEquals (presets::currentFactoryPresetName ("Polyphia"), juce::String ("Modern Prog"));
            expectEquals (presets::currentFactoryPresetName ("CHON"), juce::String ("Math Rock"));
            expectEquals (presets::currentFactoryPresetName ("Tech Death"), juce::String ("Tech Death"));

            const auto restoreWith = [] (const juce::String& name, const juce::String& source)
            {
                AmpSimProcessor p;
                p.parameters.state.setProperty ("presetName", name, nullptr);
                p.parameters.state.setProperty (AmpSimProcessor::presetSourceKey, source, nullptr);
                juce::MemoryBlock saved;
                p.getStateInformation (saved);
                auto q = std::make_unique<AmpSimProcessor>();
                q->setStateInformation (saved.getData(), (int) saved.getSize());
                return q;
            };
            auto polyphia = restoreWith ("Polyphia", "factory");
            auto chon = restoreWith ("CHON", "factory");
            auto mine = restoreWith ("Polyphia", "user"); // a user preset of that name is the user's business
            const auto restoredPolyphia = polyphia->getPresetName(), restoredChon = chon->getPresetName();
            expectEquals (restoredPolyphia, juce::String ("Modern Prog"));
            expectEquals (restoredChon, juce::String ("Math Rock"));
            expectEquals (mine->getPresetName(), juce::String ("Polyphia"));

            // The top bar shows the new name with the Factory tag, and the arrows step on from it.
            std::unique_ptr<juce::AudioProcessorEditor> editor (polyphia->createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.refresh();
            const auto shown = ed.getTopBar().getShownPresetName(), tag = ed.getTopBar().getShownTag();
            const auto next = ed.stepPreset (1);
            expectEquals (shown, juce::String ("Modern Prog"));
            expectEquals (tag, juce::String ("Factory"));
            expectEquals (next, juce::String ("Math Rock"));
            logMessage ("  -> saved states on \"Polyphia\" and \"CHON\" (factory) restore as \"" + restoredPolyphia + "\" and \"" + restoredChon
                        + "\"; a user preset called \"Polyphia\" keeps its name; the top bar shows \"" + shown + "\" (" + tag + "), and the next arrow goes to \"" + next + "\"");
        }

        beginTest ("listening renders: the synthetic guitar DI through the whole app on each built-in, with its factory cab, and amp only");
        {
            WithBuiltInCaptures builtIns;
            const auto input = guitarDI ((int) (8.0 * fs));
            const auto folder = proofDir().getChildFile ("default_captures");
            folder.createDirectory();
            expect (writeWav (folder.getChildFile ("guitar_di.wav"), input));
            // The cab each one plays through in the factory presets: Glass and Ember in Modern Prog (vintage
            // 4x12), Monolith in Tech Death and Metal (modern 4x12).
            const auto factory = presets::factoryPresets();
            const std::array<juce::var, 3> cabFrom { factory[0], factory[0], factory[2] };
            juce::StringArray lines;
            for (int s = 0; s < 3; ++s)
            {
                const auto cab = presets::resolve (presets::FileRef::fromVar (cabFrom[(size_t) s]["cab"]["mic1"]), "irs").file;
                for (const bool withCab : { true, false })
                {
                    AmpSimProcessor p;
                    p.loadCabIR (0, cab);
                    waitForLoads (p);
                    setParam (p, AmpSimProcessor::slotParamId, (float) s);
                    setParam (p, "cab_bypass", withCab ? 0.0f : 1.0f);
                    p.prepareToPlay (fs, blockSize);
                    const auto out = play (p, input);
                    juce::AudioBuffer<float> stereo (2, (int) out.left.size());
                    stereo.copyFrom (0, 0, out.left.data(), (int) out.left.size());
                    stereo.copyFrom (1, 0, out.right.data(), (int) out.right.size());
                    const auto file = folder.getChildFile (juce::String (names[(size_t) s]).toLowerCase() + (withCab ? "" : "_amp_only") + ".wav");
                    expect (writeWav (file, stereo));
                    // The app's own level: every capture is normalized to -18 LUFS, so a clean one on this hot DI
                    // (peaks -6 dBFS) can peak a little over 0 dBFS (the float file keeps it unclipped).
                    const auto peak = stereo.getMagnitude (0, stereo.getNumSamples());
                    expect (peak > 0.01f && peak < 2.0f && std::isfinite (peak), juce::String (peak));
                    lines.add (file.getFileName() + " (peak " + juce::String (toDb (peak), 1) + " dBFS, RMS " + juce::String (toDb (rms (out.left)), 1) + " dBFS"
                               + (withCab ? ", " + cab.getFileNameWithoutExtension() : juce::String()) + ")");
                }
            }
            logMessage ("  -> default_captures/: guitar_di.wav (8 s, peaks -6 dBFS) through the app at its defaults: " + lines.joinIntoString (", "));
        }
    }
};

static BuiltInCaptureTests builtInCaptureTests;
} // namespace

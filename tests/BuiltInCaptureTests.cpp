// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The built-in amps (content/models; BUILD_PLAN decision log 2026-10-03, "More built-in amps", "Amp switching") and
// the renamed factory presets: a fresh start loads all eight (timed, and its memory measured), the shelf picks one,
// the capture menu loads and removes a capture of your own, old preset names still resolve, and renders for Sean's ears.

#include "BuiltInCaptures.h"
#include "PluginEditor.h"
#include "Presets.h"
#include "dsp/GainSet.h"
#include "TestHelpers.h"
#include "platform/AppInfo.h"

#include <map>

#if JUCE_MAC
 #include <mach/mach.h>
 #include <malloc/malloc.h>
#endif

namespace
{
using namespace testing;

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 12000 && p.isLoading(); ++i) // eight gain sets take seconds on a busy machine
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

/// The process's physical memory footprint in bytes (what Activity Monitor shows as Memory), 0 where it can't be read.
/// Freed memory the allocator still holds is handed back first, so what's counted is what's in use.
juce::int64 memoryFootprint()
{
   #if JUCE_MAC
    malloc_zone_pressure_relief (nullptr, 0);
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (juce::int64) info.phys_footprint;
   #endif
    return 0;
}

/// A copy of an example capture whose metadata names its tone.
juce::File taggedCapture (const juce::String& source, const juce::String& toneType, const juce::String& fileName)
{
    auto json = juce::JSON::parse (exampleModel (source).loadFileAsString());
    auto* metadata = new juce::DynamicObject();
    metadata->setProperty ("tone_type", toneType);
    json.getDynamicObject()->setProperty ("metadata", juce::var (metadata));
    const auto file = tempDir().getChildFile (fileName);
    file.replaceWithText (juce::JSON::toString (json, true));
    return file;
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

        beginTest ("a fresh processor starts with all eight built-in amps loaded off the audio thread (the playing one first), only Glass running, no warnings: the load time and the memory measured");
        {
            const auto before = memoryFootprint();
            std::unique_ptr<AmpSimProcessor> bare;
            bare = std::make_unique<AmpSimProcessor>(); // built-ins off: the processor alone
            bare->prepareToPlay (fs, blockSize);
            const auto withoutSets = memoryFootprint();

            WithBuiltInCaptures builtIns;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            AmpSimProcessor p;
            const auto loadingAtOnce = p.isLoading(); // the constructor only queued them
            double firstMs = -1.0;
            for (int i = 0; i < 12000 && p.isLoading(); ++i)
            {
                if (firstMs < 0.0 && ! p.getStatus().model[0].startsWith ("Loading"))
                    firstMs = juce::Time::getMillisecondCounterHiRes() - t0;
                juce::Thread::sleep (5);
            }
            const auto allMs = juce::Time::getMillisecondCounterHiRes() - t0;
            const auto loadedBeforePrepare = memoryFootprint(); // the models as the loader sized them (before the device's block size is known)
            p.prepareToPlay (fs, blockSize);
            play (p, std::vector<float> (blockSize * 4, 0.0f));
            const auto withSets = memoryFootprint();
            expect (loadingAtOnce);
            const auto status = p.getStatus();
            for (int a = 0; a < AmpSimProcessor::numBuiltInAmps; ++a)
            {
                expectEquals (modelPath (p, a), presets::builtInCapture (a).getFullPathName());
                expect (! status.modelError[(size_t) a] && status.model[(size_t) a].startsWith (presets::builtInAmpName (a)), status.model[(size_t) a]);
                expect (p.getChain().amp.isRunning (a) == (a == 0), presets::builtInAmpName (a));
            }
            expectEquals (status.model[(size_t) AmpSimProcessor::yourCaptureAmp], juce::String ("Empty"));
            expect (status.warning.isEmpty(), status.warning);
            expectGreaterThan (firstMs, 0.0);
            const auto setsMb = (double) (withSets - withoutSets) / (1024.0 * 1024.0);
            if (withSets > 0)
                expectLessThan (setsMb, 300.0, "preloading every built-in amp must stay under about 300 MB");
            logMessage ("  -> load times on the loader thread: Glass (the playing amp, queued first) ready " + juce::String (firstMs / 1000.0, 2)
                        + " s after the constructor, all eight sets (40 standard WaveNets, each measured on 4 s of the reference DI) "
                        + juce::String (allMs / 1000.0, 2) + " s");
            logMessage ("  -> memory (the process's physical footprint): " + juce::String ((double) before / 1048576.0, 1) + " MB at the start, "
                        + juce::String ((double) withoutSets / 1048576.0, 1) + " MB with a processor and no captures, "
                        + juce::String ((double) withSets / 1048576.0, 1) + " MB with a second processor holding all eight sets: "
                        + juce::String (setsMb, 1) + " MB for the eight sets and that processor (the processor alone: "
                        + juce::String ((double) (withoutSets - before) / 1048576.0, 1) + " MB); before the 128-sample prepare, with the models sized "
                        "for the loader's default 512-sample block: " + juce::String ((double) (loadedBeforePrepare - withoutSets) / 1048576.0, 1) + " MB");
        }

        beginTest ("the capture menu: load a capture of your own (it plays as amp 9), reload, remove it (it stays removed in a restored state); amps a state has no entry for get their built-in");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            const auto yours = AmpSimProcessor::yourCaptureAmp;

            setParam (p, AmpSimProcessor::ampModelParamId, 1.0f);
            const auto menu = ed.captureMenu (1);
            juce::StringArray items;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                items.add (it.getItem().text + (it.getItem().isEnabled ? "" : " (greyed)"));
            const auto* remove = findItem (menu, "Remove your capture");
            expect (findItem (menu, "Load a capture of your own") != nullptr && findItem (menu, "Reload gainset.json") != nullptr);
            expect (remove != nullptr && ! remove->isEnabled, "nothing to remove yet");
            expect (findItem (menu, "Built-in amps") == nullptr, "the shelf picks the built-in amps now");

            p.loadYourCapture (exampleModel ("A2.nam"));
            waitForLoads (p);
            expectEquals (p.getSelectedAmp(), yours);
            expectEquals (modelPath (p, yours), exampleModel ("A2.nam").getFullPathName());
            const auto withCapture = ed.captureMenu (yours);
            const auto* removeNow = findItem (withCapture, "Remove your capture");
            expect (removeNow != nullptr && removeNow->isEnabled);
            if (removeNow != nullptr && removeNow->action)
                removeNow->action();
            waitForLoads (p);
            expectEquals (p.getSelectedAmp(), 0, "removed while it played: Glass plays");
            expect (p.parameters.state.hasProperty (AmpSimProcessor::modelPathKey (yours)) && modelPath (p, yours).isEmpty());
            expectEquals (p.getStatus().model[(size_t) yours], juce::String ("Empty"));

            juce::MemoryBlock saved;
            p.getStateInformation (saved);
            AmpSimProcessor q;
            q.setStateInformation (saved.getData(), (int) saved.getSize());
            waitForLoads (q);
            expect (modelPath (q, yours).isEmpty());
            expectEquals (q.getStatus().model[(size_t) yours], juce::String ("Empty"));
            expectEquals (modelPath (q, 1), presets::builtInCapture (1).getFullPathName());

            // A state with no entries for amps 4 to 8 (saved before they existed, with amp_model already there): they get theirs.
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
            for (int a = 3; a < AmpSimProcessor::numBuiltInAmps; ++a)
                xml->removeAttribute (AmpSimProcessor::modelPathKey (a).toString());
            // Monolith saved by a copy of the app somewhere else (a dev build, or an install since moved), and Ember as the
            // single file it was before the gain sets: both found in this copy's content folder.
            xml->setAttribute (AmpSimProcessor::modelPathKey (2).toString(), "/nowhere/Old BellyDSP.app/Contents/Resources/content/models/Monolith/gainset.json");
            xml->setAttribute (AmpSimProcessor::modelPathKey (1).toString(), "/nowhere/Old BellyDSP.app/Contents/Resources/content/models/Ember.nam");
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*xml, old);
            AmpSimProcessor r;
            r.setStateInformation (old.getData(), (int) old.getSize());
            waitForLoads (r);
            juce::StringArray loaded;
            for (int a = 0; a < AmpSimProcessor::numBuiltInAmps; ++a)
            {
                expectEquals (modelPath (r, a), presets::builtInCapture (a).getFullPathName());
                expect (! r.getStatus().modelError[(size_t) a], r.getStatus().model[(size_t) a]);
                loaded.add (presets::builtInAmpName (a));
            }
            expect (presets::bundledElsewhere (juce::File ("/nowhere/content/models/Nope.nam")) == juce::File());
            logMessage ("  -> the menu on Ember: " + items.joinIntoString (" | ") + "; a capture of your own loaded as amp 9 and played; removed from the menu: Glass plays, "
                        "amp 9 saved as an empty path and empty after a restore; a state without amps 4 to 8, Monolith saved by another copy of the app and "
                        "Ember as its old single file: " + loaded.joinIntoString (", ") + " all loaded from this copy's content folder");
        }

        beginTest ("the amp shelf: the eight built-in amps in order (each with its description), a click plays one, hovering names it, your capture's mini after a gap once loaded, each amp keeps its own knobs; snapshots with each amp playing");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            waitForLoads (p);
            p.prepareToPlay (fs, blockSize);
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            ed.showPage (ui::PageId::amp);
            auto& amp = ed.getAmpView();
            ed.refresh();

            // The list: the built-in amps in the shelf's (and amp_model's) order, each with a description.
            const auto sets = presets::builtInGainSets();
            juce::StringArray listed;
            for (const auto& s : sets)
                listed.add (s.name);
            expect (listed == juce::StringArray { "Glass", "Ember", "Monolith", "Lantern", "Basalt", "Comet", "Forge", "Quartz" }, listed.joinIntoString (", "));
            for (int a = 0; a < AmpSimProcessor::numBuiltInAmps; ++a)
            {
                expectEquals (AmpSimProcessor::ampName (a), listed[a]);
                expect (amp.ampDescription (a).isNotEmpty(), listed[a]);
            }

            // The layout: "Amp", then the minis 12 px apart in a row at the top left; no tabs; your capture's not shown yet.
            const auto first = amp.getMiniBounds (0), second = amp.getMiniBounds (1), last = amp.getMiniBounds (7);
            expectEquals (second.getX() - first.getRight(), 12);
            expect (first.getX() > 0 && first.getX() < 60 && first.getY() >= 0 && last.getBottom() <= 33, first.toString());
            expect (! amp.isOnShelf (AmpSimProcessor::yourCaptureAmp));
            expectEquals (amp.getShelfText(), juce::String ("Glass"));

            // Hovering names the amp and says what it is.
            amp.hoverMini (5);
            const auto hover = amp.getShelfText();
            expect (hover.startsWith ("Comet: ") && hover.length() > 12, hover);
            amp.hoverMini (-1);

            // Each amp keeps its own knobs: Gain and Middle set on Forge, Glass's left alone; away and back finds them.
            const auto knob = [&p] (int a, const char* name) { return getParam (p, AmpSimProcessor::ampParamId (a, name)); };
            amp.clickMini (6); // Forge
            setParam (p, AmpSimProcessor::ampParamId (6, "input_trim"), 9.6f);
            setParam (p, AmpSimProcessor::ampParamId (6, "mid"), -2.4f);
            amp.clickMini (0);
            const auto glassGain = knob (0, "input_trim");
            amp.clickMini (6);
            ed.refresh();
            expectWithinAbsoluteError (knob (6, "input_trim"), 9.6f, 1.0e-5f);
            expectWithinAbsoluteError (knob (6, "mid"), -2.4f, 1.0e-5f);
            expectWithinAbsoluteError (glassGain, 0.0f, 1.0e-5f);
            expectEquals (amp.getKnob (6, 0).getValueText(), juce::String ("7.0"));
            expect (amp.getKnob (6, 0).isVisible() && ! amp.getKnob (0, 0).isVisible());

            // A click on each mini plays that amp: the head, badge, and knobs follow; snapshots of the page and the shelf.
            const auto folder = proofDir().getChildFile ("amp_shelf");
            folder.createDirectory();
            const auto input = guitarDI ((int) (0.5 * fs));
            juce::StringArray shots;
            const auto snapshot = [&] (int a)
            {
                amp.clickMini (a);
                play (p, input);
                amp.getSpectrum().update();
                ed.refresh();
                expectEquals (p.getSelectedAmp(), a);
                expectEquals (amp.getShownAmp(), a);
                expect (amp.getHead().getMaterial() == ui::materialForAmp (a));
                expect (amp.getKnob (a, 0).isVisible());
                expect (amp.getJewel().isLit());
                const auto name = AmpSimProcessor::ampName (a).toLowerCase().replaceCharacter (' ', '_');
                const auto shelfArea = editor->getLocalArea (&amp.getShelf(), amp.getShelf().getLocalBounds());
                expect (savePng (editor->createComponentSnapshot (shelfArea, true, 2.0f), folder.getChildFile ("shelf_" + name + ".png")));
                expect (savePng (editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f), folder.getChildFile ("editor_amp_" + name + ".png")));
                shots.add (name);
            };
            for (int a = 0; a < AmpSimProcessor::numBuiltInAmps; ++a)
                snapshot (a);

            // Your capture: its mini joins the shelf after a gap once one is loaded, and plays like any other.
            p.loadYourCapture (taggedCapture ("wavenet.nam", "crunch", "Bedroom capture.nam"));
            waitForLoads (p);
            ed.refresh();
            expect (amp.isOnShelf (AmpSimProcessor::yourCaptureAmp));
            const auto gap = amp.getMiniBounds (AmpSimProcessor::yourCaptureAmp).getX() - amp.getMiniBounds (7).getRight();
            expectGreaterThan (gap, 12);
            snapshot (AmpSimProcessor::yourCaptureAmp);
            expectEquals (amp.getHead().getBadge(), juce::String ("Bedroom capture"));
            expectEquals (amp.getShelfText(), juce::String ("Your capture"));
            logMessage ("  -> shelf: " + listed.joinIntoString (", ") + " in a row 12 px apart from x " + juce::String (first.getX()) + "; hovering Comet: \"" + hover
                        + "\"; Forge kept Gain 7.0 and Middle -2.4 dB across a trip to Glass (Glass's Gain untouched); your capture's mini " + juce::String (gap)
                        + " px after Quartz's once loaded; snapshots amp_shelf/shelf_<amp>.png and amp_shelf/editor_amp_<amp>.png for " + shots.joinIntoString (", "));
        }

        beginTest ("factory presets: renamed, and each loads the built-in amps and its bundled cab with no warnings; its scenes choose among the amps");
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
                const auto name = preset["name"].toString();
                expect (p.getPresetWarnings().isEmpty(), name + ": " + p.getPresetWarnings().joinIntoString ("; "));
                const auto status = p.getStatus();
                expectEquals ((int) preset["format_version"], presets::formatVersion);
                for (int s = 0; s < AmpSimProcessor::numBuiltInAmps; ++s)
                {
                    expectEquals (presets::FileRef::fromVar (preset["amps"][s]).path, presets::builtInCapturePath (s));
                    expectEquals (modelPath (p, s), presets::builtInCapture (s).getFullPathName());
                    expect (! status.modelError[(size_t) s] && status.model[(size_t) s] != "Empty", name + " amp " + juce::String (s + 1) + ": " + status.model[(size_t) s]);
                }
                expect (presets::FileRef::fromVar (preset["amps"][AmpSimProcessor::yourCaptureAmp]).path.isEmpty());
                expect (! status.cabError[0] && status.cab[0] != "No IR", status.cab[0]);
                expect (juce::File (p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString()).isAChildOf (platform::factoryContentFolder()));
                expect (preset["notes"].toString().contains ("built-in") && ! preset["notes"].toString().containsIgnoreCase ("slot"), preset["notes"].toString());
                juce::StringArray sceneSlots;
                for (int i = 0; i < 3; ++i)
                {
                    expect (p.recallScene (i));
                    sceneSlots.add (p.getScenes().get (i).name + " " + AmpSimProcessor::ampName (juce::roundToInt (getParam (p, AmpSimProcessor::ampModelParamId))));
                }
                lines.add (name + " (" + sceneSlots.joinIntoString (", ") + "; close mic 1 \""
                           + juce::File (p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString()).getFileNameWithoutExtension() + "\")");
            }

            // Tech Death's Boost is off (Monolith has a boost built in), still set up as the Screamer at +6 dB.
            AmpSimProcessor tech;
            expect (tech.loadPreset (factory[2]).ok);
            expect (getParam (tech, "boost_on") == 0.0f && getParam (tech, "boost_mode") == 2.0f && getParam (tech, "amp_model") == 2.0f);
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
                    setParam (p, AmpSimProcessor::ampModelParamId, (float) s);
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

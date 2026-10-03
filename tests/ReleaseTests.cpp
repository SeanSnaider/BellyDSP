// What a shipped build carries besides the DSP (docs/RELEASING.md): its version, the brand menu (version,
// "Check for updates...", licences), the generated licence notices, and bundled content that presets
// refer to as "factory:...".
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"
#include "platform/AppInfo.h"
#include "platform/Updater.h"

namespace
{
using namespace testing;

void waitUntilIdle (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
    for (int i = 0; i < 100 && p.isChangingPreset(); ++i)
    {
        juce::Thread::sleep (10);
        p.runHousekeeping();
    }
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

struct MenuEntry
{
    juce::String text;
    bool enabled = false;
};

std::vector<MenuEntry> entriesOf (const juce::PopupMenu& menu)
{
    std::vector<MenuEntry> entries;
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();
        if (! item.isSeparator)
            entries.push_back ({ item.text, item.isEnabled });
    }
    return entries;
}

class ReleaseTests final : public juce::UnitTest
{
public:
    ReleaseTests() : juce::UnitTest ("Release: version, brand menu, notices, bundled content", "ampsim") {}

    void runTest() override
    {
        beginTest ("the version comes from the VERSION file, and the brand menu shows it with Check for updates and the licences");
        {
            const auto versionFile = juce::File (AMPSIM_SOURCE_DIR).getChildFile ("VERSION").loadFileAsString().trim();
            const auto version = platform::appVersion();
            // The release script builds with -DAMPSIM_VERSION, which may differ from the file; a dev build
            // uses the file. Either way it's major.minor.patch.
            const auto parts = juce::StringArray::fromTokens (version, ".", {});
            expect (parts.size() == 3 && parts.joinIntoString ("").containsOnly ("0123456789"), version);

            AmpSimProcessor p;
            std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
            auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
            const auto entries = entriesOf (ed.brandMenu());
            expectEquals ((int) entries.size(), 3);
            expectEquals (entries[0].text, "Amp Sim " + version);
            expect (! entries[0].enabled); // a label, not a command
            expectEquals (entries[1].text, juce::String ("Check for updates..."));
            expect (! entries[1].enabled); // the tests' processor isn't the standalone app: no updater runs
            expectEquals (entries[2].text, juce::String ("About / licenses"));
            expect (entries[2].enabled);
            expect (! platform::updater::isRunning());

            auto& brand = ed.getTopBar().getBrandButton();
            expect (brand.isVisible() && brand.getX() == 24 && brand.getRight() <= 24 + 136, brand.getBounds().toString());
            juce::String shown;
            for (const auto& e : entries)
                shown << (shown.isEmpty() ? "" : " | ") << e.text << (e.enabled ? "" : " (disabled)");
            logMessage ("  -> VERSION file " + versionFile + ", this build " + version + "; the brand (\"rig\", "
                        + brand.getBounds().toString() + ") opens: " + shown);
            logMessage ("  -> updater in this build: " + platform::updater::describe());
        }

        beginTest ("THIRD_PARTY_NOTICES.txt is generated from the real licence files and shown under About / licenses");
        {
            const auto notices = platform::noticesFile();
            expect (notices.existsAsFile(), notices.getFullPathName());
            const auto about = AmpSimEditor::aboutText();
            const char* required[] = {
                "JUCE 8.0.15", "AGPLv3",                                        // JUCE and its dual licence
                "NeuralAmpModelerCore", "Copyright (c) 2023 Steven Atkinson",    // NAM core's MIT licence, from its LICENSE
                "Eigen", "Mozilla Public License Version 2.0",                   // Eigen's MPL2 text, from COPYING.MPL2
                "JSON for Modern C++", "Niels Lohmann",
                "Geist", "Fraunces", "SIL OPEN FONT LICENSE",                    // both OFL.txt files
                "zlib", "libpng", "Independent JPEG Group", "FLAC", "Ogg Vorbis", "HarfBuzz", "SheenBidi",
                "Bundled captures and impulse responses"
            };
            juce::StringArray missing;
            for (const auto* text : required)
                if (! about.contains (text))
                    missing.add (text);
            expect (missing.isEmpty(), "missing from the notices: " + missing.joinIntoString (", "));
            expect (about.startsWith ("Amp Sim " + platform::appVersion()));
            const auto lines = juce::StringArray::fromLines (notices.loadFileAsString());
            logMessage ("  -> " + notices.getFileName() + ": " + juce::String (lines.size()) + " lines, " + juce::String (notices.getSize())
                        + " bytes; its contents list:");
            // The index: the lines between "Contents:" and the first section.
            for (int i = lines.indexOf ("Contents:") + 1; i > 0 && i < lines.size() && lines[i].startsWith ("  "); ++i)
                logMessage ("       " + lines[i].trim());
        }

        beginTest ("bundled content: \"factory:\" paths resolve against the app's content folder, survive a move, and never stop a preset when missing");
        {
            // A stand-in for the app's Contents/Resources/content, with a tiny capture from NAM core's examples.
            const auto factory = tempDir().getChildFile ("factory_content");
            const auto library = tempDir().getChildFile ("factory_test_library");
            factory.deleteRecursively();
            library.deleteRecursively();
            const auto models = library.getChildFile ("models"), irs = library.getChildFile ("irs");
            models.createDirectory();
            irs.createDirectory();
            presets::setLibraryRoot ("factory", factory);
            presets::setLibraryRoot ("models", models);
            presets::setLibraryRoot ("irs", irs);

            const auto bundled = factory.getChildFile ("models/Starter Clean.nam");
            bundled.getParentDirectory().createDirectory();
            expect (exampleModel ("lstm.nam").copyFileTo (bundled)); // 2.3 kB: the smallest example capture
            const auto bundledIr = factory.getChildFile ("irs/Starter 2x12.wav");
            bundledIr.getParentDirectory().createDirectory();
            expect (writeWav (bundledIr, toBuffer (syntheticCabIR (1024))));

            // Saving refers to bundled files as factory:..., not by where this copy of the app happens to be.
            AmpSimProcessor a;
            a.loadModel (0, bundled);
            a.loadCabIR (0, bundledIr);
            waitUntilIdle (a);
            const auto saved = a.capturePreset ("Starter");
            const auto ampRef = presets::FileRef::fromVar (saved["amps"][0]);
            const auto irRef = presets::FileRef::fromVar (saved["cab"]["mic1"]);
            expectEquals (ampRef.path, juce::String ("factory:models/Starter Clean.nam"));
            expectEquals (irRef.path, juce::String ("factory:irs/Starter 2x12.wav"));
            expectEquals ((int) saved["format_version"], 2);

            // Loading resolves them against the content folder.
            const auto direct = presets::resolve (ampRef, "models");
            expect (direct.found && ! direct.relinked && direct.file == bundled);
            AmpSimProcessor b;
            expect (b.loadPreset (saved).ok);
            waitUntilIdle (b);
            expect (b.getStatus().model[0].contains ("Starter Clean"), b.getStatus().model[0]);
            expect (b.getPresetWarnings().isEmpty(), b.getPresetWarnings().joinIntoString ("; "));

            // A later version renamed the file: found again by its hash inside the content folder.
            const auto renamed = factory.getChildFile ("models/Clean/Starter Clean v2.nam");
            renamed.getParentDirectory().createDirectory();
            expect (bundled.moveFileTo (renamed));
            const auto moved = presets::resolve (ampRef, "models");
            expect (moved.found && moved.relinked && moved.file == renamed, moved.file.getFullPathName());

            // An app without that file (an older version, say): the slot loads empty with a warning, the rest
            // of the preset still applies. Same as any missing library file.
            renamed.deleteFile();
            AmpSimProcessor c;
            auto withDelay = saved.clone();
            withDelay["parameters"].getDynamicObject()->setProperty ("delay_on", 1);
            expect (c.loadPreset (withDelay).ok);
            waitUntilIdle (c);
            const auto warnings = c.getPresetWarnings().joinIntoString ("; ");
            expect (warnings.contains ("factory:models/Starter Clean.nam"), warnings);
            expectEquals (c.getStatus().model[0], juce::String ("Empty"));
            expect (c.getStatus().cab[0] != "No IR", c.getStatus().cab[0]);
            expectEquals (c.parameters.getRawParameterValue ("delay_on")->load(), 1.0f);

            // The same capture copied into the user's library is found there instead.
            expect (exampleModel ("lstm.nam").copyFileTo (models.getChildFile ("My copy.nam")));
            const auto fromLibrary = presets::resolve (ampRef, "models");
            expect (fromLibrary.found && fromLibrary.relinked && fromLibrary.file == models.getChildFile ("My copy.nam"));

            // The real default is the app's own folder (Contents/Resources/content in the app bundle).
            presets::setLibraryRoot ("factory", {});
            expect (presets::libraryRoot ("factory") == platform::factoryContentFolder());

            presets::setLibraryRoot ("models", {});
            presets::setLibraryRoot ("irs", {});
            factory.deleteRecursively();
            library.deleteRecursively();
            logMessage ("  -> saved as \"" + ampRef.path + "\" and \"" + irRef.path + "\" (format_version 2); loaded: \"" + b.getStatus().model[0] + "\"");
            logMessage ("  -> renamed inside the content folder: relinked to " + renamed.getRelativePathFrom (factory));
            logMessage ("  -> missing: \"" + warnings + "\"; the IR and delay_on still applied");
            logMessage ("  -> the app's real content folder: " + platform::factoryContentFolder().getFullPathName());
        }
    }
};

ReleaseTests releaseTests;
} // namespace

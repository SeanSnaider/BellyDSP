// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// What a shipped build carries besides the DSP (docs/RELEASING.md): its version, the brand menu (version,
// "Check for updates...", licences), the generated licence notices, and bundled content that presets
// refer to as "factory:...".
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"
#include "dsp/CabIR.h"
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
        beginTest ("the version comes from the VERSION file, and the brand menu shows it with the licence, Check for updates, the source, and the licences");
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
            expectEquals ((int) entries.size(), 6);
            expectEquals (entries[0].text, "BellyDSP " + version);
            expect (! entries[0].enabled); // a label, not a command
            expectEquals (entries[1].text, juce::String ("Free software under the GNU AGPL v3 or later"));
            expect (! entries[1].enabled);
            expectEquals (entries[2].text, juce::String ("Match tone...")); // the tone match page (docs/TONE_MATCH.md)
            expect (entries[2].enabled);
            expectEquals (entries[3].text, juce::String ("Check for updates..."));
            expect (! entries[3].enabled); // the tests' processor isn't the standalone app: no updater runs
            expectEquals (entries[4].text, juce::String ("Source code for this version"));
            expect (entries[4].enabled);
            expectEquals (entries[5].text, juce::String ("About / licenses"));
            expect (entries[5].enabled);
            expect (! platform::updater::isRunning());

            // Where "Source code for this version" goes: the public repo at this version's tag.
            expectEquals (platform::sourceUrl(), juce::String ("https://github.com/SeanSnaider/BellyDSP"));
            expectEquals (platform::sourceUrlForThisVersion(), "https://github.com/SeanSnaider/BellyDSP/tree/v" + version);

            // The brand reads "BellyDSP" (the handoff's placeholder was "rig") and fits its 136 px column.
            auto& brand = ed.getTopBar().getBrandButton();
            expectEquals (brand.getName(), juce::String ("BellyDSP"));
            expect (brand.isVisible() && brand.getX() == 24 && brand.getRight() <= 24 + 136, brand.getBounds().toString());
            juce::String shown;
            for (const auto& e : entries)
                shown << (shown.isEmpty() ? "" : " | ") << e.text << (e.enabled ? "" : " (disabled)");
            logMessage ("  -> VERSION file " + versionFile + ", this build " + version + "; the brand (\"" + brand.getName() + "\", "
                        + brand.getBounds().toString() + ", " + juce::String (brand.getWidth()) + " of the 136 px column) opens: " + shown);
            logMessage ("  -> source code for this version: " + platform::sourceUrlForThisVersion());

            // The amp page with the new brand, at 2x, and the brand's corner on its own.
            const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            const auto shot = proofDir().getChildFile ("editor_brand_bellydsp.png");
            const auto corner = image.getClippedImage ({ 0, 0, 2 * (24 + 136 + 40), 2 * 56 });
            const auto cornerShot = proofDir().getChildFile ("editor_brand_bellydsp_corner.png");
            for (const auto& [img, file] : { std::pair { image, shot }, std::pair { corner, cornerShot } })
            {
                file.deleteFile();
                juce::FileOutputStream stream (file);
                expect (stream.openedOk() && juce::PNGImageFormat().writeImageToStream (img, stream), file.getFullPathName());
            }
            logMessage ("  -> snapshots: " + shot.getFileName() + " (" + juce::String (image.getWidth()) + " x " + juce::String (image.getHeight())
                        + "), " + cornerShot.getFileName());
            logMessage ("  -> updater in this build: " + platform::updater::describe());
        }

        beginTest ("THIRD_PARTY_NOTICES.txt is generated from the real licence files and shown under About / licenses");
        {
            const auto notices = platform::noticesFile();
            expect (notices.existsAsFile(), notices.getFullPathName());
            const auto about = AmpSimEditor::aboutText();
            const char* required[] = {
                "BellyDSP", "Copyright (C) 2026 Sean Snaider",                  // BellyDSP's own notice first
                "GNU Affero General Public License", "either version 3 of the License, or (at your option) any",
                "ABSOLUTELY NO WARRANTY", "https://github.com/SeanSnaider/BellyDSP",
                "GNU AFFERO GENERAL PUBLIC LICENSE", "Version 3, 19 November 2007", // the AGPLv3 text, from LICENSE
                "JUCE 8.0.15", "AGPLv3",                                        // JUCE and its dual licence
                "NeuralAmpModelerCore", "Copyright (c) 2023 Steven Atkinson",    // NAM core's MIT licence, from its LICENSE
                "Eigen", "Mozilla Public License Version 2.0",                   // Eigen's MPL2 text, from COPYING.MPL2
                "JSON for Modern C++", "Niels Lohmann",
                "demucs.cpp", "Copyright (c) 2023 Sevag H", "Demucs", "Meta Platforms",  // tone match's separation (MIT)
                "Geist", "Fraunces", "SIL OPEN FONT LICENSE",                    // both OFL.txt files
                "zlib", "libpng", "Independent JPEG Group", "FLAC", "Ogg Vorbis", "HarfBuzz", "SheenBidi",
                "Bundled captures and impulse responses"
            };
            juce::StringArray missing;
            for (const auto* text : required)
                if (! about.contains (text))
                    missing.add (text);
            expect (missing.isEmpty(), "missing from the notices: " + missing.joinIntoString (", "));
            expect (about.startsWith ("BellyDSP " + platform::appVersion()));
            expect (about.contains ("Source code for this version: https://github.com/SeanSnaider/BellyDSP/tree/v" + platform::appVersion()));
            // The notices start with BellyDSP's own licence, then JUCE (under the AGPLv3), then the rest.
            const auto text = notices.loadFileAsString();
            const auto ownAt = text.indexOf ("1. BellyDSP"), juceAt = text.indexOf ("2. JUCE 8.0.15");
            expect (text.startsWith ("BellyDSP " + platform::appVersion()) && ownAt > 0 && juceAt > ownAt, juce::String (ownAt) + " " + juce::String (juceAt));
            expect (text.contains ("BellyDSP uses JUCE under the GNU Affero General Public License v3"));
            expect (! text.containsIgnoreCase ("Amp Sim"));
            // LICENSE ships next to the notices (in the app: Contents/Resources/LICENSE.txt), unmodified.
            const auto licence = notices.getSiblingFile ("LICENSE.txt");
            expect (licence.existsAsFile() && licence.loadFileAsString() == juce::File (AMPSIM_SOURCE_DIR).getChildFile ("LICENSE").loadFileAsString(),
                    licence.getFullPathName());
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

        beginTest ("bundled IRs: each one in the manifest resolves from its factory: path inside the built app, is 48 kHz and at most 1 s, loads in the cab, and names no brand");
        {
            // The built app's content folder (BellyDSP.app/Contents/Resources/content), when the app has been
            // built in this build tree; otherwise the copy next to this test binary (the same files).
            const juce::File appContent (AMPSIM_APP_CONTENT_DIR);
            const auto content = appContent.isDirectory() ? appContent : platform::factoryContentFolder();
            presets::setLibraryRoot ("factory", content);
            const auto repoContent = juce::File (AMPSIM_SOURCE_DIR).getChildFile ("content");
            const auto manifest = juce::JSON::parse (repoContent.getChildFile ("manifest.json"));
            const auto* files = manifest["files"].getArray();
            expect (files != nullptr);

            // Brand and model names the bundled files' names and texts must not contain (CLAUDE.md), the
            // gear in these packs' handbooks among them.
            const juce::StringArray brands { "Marshall", "Celestion", "Shure", "SM57", "SM 57", "Sennheiser", "e606", "e935", "V30", "Vintage 30",
                                             "Greenback", "G12", "Behringer", "BG412", "Eminence", "DV-77", "DV77", "Rockdriver", "Pyle", "PDMIC",
                                             "Neve", "Steinberg", "Golden Age", "Mesa", "Orange", "Neural DSP", "Fortin", "Bogner", "Diezel" };
            int checked = 0, longest = 0;
            juce::int64 bytes = 0;
            juce::StringArray rows, problems;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            for (const auto& entry : *files)
            {
                const auto path = entry["path"].toString();
                if (! path.startsWith ("irs/"))
                    continue;
                ++checked;
                const auto source = repoContent.getChildFile (path);
                const presets::FileRef ref { "factory:" + path, presets::contentHash (source), source.getSize() };
                const auto r = presets::resolve (ref, "irs");
                if (! r.found || r.relinked || r.changed || ! r.file.isAChildOf (content))
                    problems.add (path + " didn't resolve in place");

                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (r.file));
                if (reader == nullptr || reader->sampleRate != 48000.0 || reader->lengthInSamples > 48000 || reader->numChannels != 1)
                    problems.add (path + " isn't a mono 48 kHz IR of at most 1 s");
                const auto length = reader != nullptr ? (int) reader->lengthInSamples : 0;
                longest = juce::jmax (longest, length);
                bytes += source.getSize();

                ampsim::CabIR mic;
                const auto loaded = mic.loadFile (r.file);
                if (! loaded.ok || loaded.message.contains ("cut"))
                    problems.add (path + ": " + loaded.message);

                for (const auto* key : { "path", "title", "description", "notes" })
                    for (const auto& brand : brands)
                        if (entry[key].toString().containsIgnoreCase (brand))
                            problems.add (path + "'s " + key + " names \"" + brand + "\"");
                if (entry["license"].toString() != "CC0 1.0" || entry["license_file"].toString() != "licenses/CC0-1.0.txt")
                    problems.add (path + ": not the CC0 entry expected");
                rows.add (r.file.getFileNameWithoutExtension() + ": " + juce::String (juce::roundToInt (1000.0 * length / 48000.0)) + " ms, loudness matched "
                          + juce::String (juce::Decibels::gainToDecibels (loaded.gain), 1) + " dB");
            }
            expectEquals (checked, 21);
            expect (problems.isEmpty(), problems.joinIntoString ("; "));
            expectEquals (longest, 48000);
            expect (content.getChildFile ("licenses/CC0-1.0.txt").loadFileAsString().contains ("CC0 1.0 Universal"));
            expect (AmpSimEditor::aboutText().contains ("Bastian Karschewski"));
            presets::setLibraryRoot ("factory", {});
            logMessage ("  -> " + juce::String (checked) + " IRs resolved from factory: paths in " + content.getFullPathName() + ", "
                        + juce::String (bytes) + " bytes in all; every one 48 kHz mono, at most 1 s (longest " + juce::String (longest) + " samples), none cut on load:");
            for (const auto& row : rows)
                logMessage ("       " + row);
        }
    }
};

ReleaseTests releaseTests;
} // namespace

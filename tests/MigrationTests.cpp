// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The one-time copy of the user's files from the app's old name, Amp Sim, to BellyDSP's folders
// (src/platform/DataMigration.h, ASSUMPTIONS DS37). Everything runs on temporary folders: the real
// ~/Library/Application Support is never touched by the tests.
#include "TestHelpers.h"
#include "platform/AppInfo.h"
#include "platform/DataMigration.h"

namespace
{
using namespace testing;
namespace migration = platform::migration;

/// A stand-in for an old install: presets, a capture, an IR in a subfolder, and the settings file.
struct OldInstall
{
    juce::File root;
    migration::Locations where;

    explicit OldInstall (const juce::String& name)
    {
        root = tempDir().getChildFile (name);
        root.deleteRecursively();
        root.createDirectory();
        where.oldFolder = root.getChildFile ("Application Support/AmpSim");
        where.newFolder = root.getChildFile ("Application Support/BellyDSP");
        where.oldSettings = root.getChildFile ("Application Support/Amp Sim.settings");
        where.newSettings = root.getChildFile ("Application Support/BellyDSP.settings");
    }

    void populate() const
    {
        write ("presets/Tech Death.json", "{\"format_version\": 2, \"name\": \"Tech Death\"}");
        write ("presets/Live/Verse.json", "{\"format_version\": 2, \"name\": \"Verse\"}");
        write ("models/High gain/Lead.nam", juce::String::repeatedString ("nam", 1000));
        write ("irs/Cab A.wav", "RIFF....WAVE");
        where.oldSettings.replaceWithText ("<?xml version=\"1.0\"?>\n<PROPERTIES><VALUE name=\"windowX\" val=\"679\"/></PROPERTIES>\n");
    }

    void write (const juce::String& relative, const juce::String& text) const
    {
        const auto f = where.oldFolder.getChildFile (relative);
        f.getParentDirectory().createDirectory();
        f.replaceWithText (text);
    }

    /// Every file under the old folder with its bytes, to prove the old install is left exactly as it was.
    juce::StringArray snapshot() const
    {
        juce::StringArray rows;
        for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
            rows.add (entry.getFile().getRelativePathFrom (root) + " " + juce::String (entry.getFile().getSize()) + " "
                      + juce::String::toHexString (entry.getFile().loadFileAsString().hashCode64()));
        rows.sort (true);
        return rows;
    }
};

class MigrationTests final : public juce::UnitTest
{
public:
    MigrationTests() : juce::UnitTest ("Rename: copying Amp Sim's data to BellyDSP", "ampsim") {}

    void runTest() override
    {
        beginTest ("the real locations: the data folder and the standalone settings file, old and new names");
        {
            const auto where = migration::legacyLocations();
            expect (where.newFolder == platform::userDataFolder(), where.newFolder.getFullPathName());
            expectEquals (where.newFolder.getFileName(), juce::String ("BellyDSP"));
            expectEquals (where.oldFolder.getFileName(), juce::String ("AmpSim"));
            expect (where.oldFolder.getParentDirectory() == where.newFolder.getParentDirectory());
            expectEquals (where.oldSettings.getFileName(), juce::String ("Amp Sim.settings"));
            expectEquals (where.newSettings.getFileName(), juce::String ("BellyDSP.settings"));
           #if JUCE_MAC
            const auto support = juce::File ("~/Library/Application Support");
            expect (where.newFolder == support.getChildFile ("BellyDSP"));
            expect (where.oldSettings == support.getChildFile ("Amp Sim.settings"));
            expect (where.newSettings == support.getChildFile ("BellyDSP.settings"));
           #endif
            for (const auto& f : { where.oldFolder, where.newFolder, where.oldSettings, where.newSettings })
                logMessage ("  -> " + f.getFullPathName());
        }

        beginTest ("an old install is copied, file for file, and the old folder and settings stay exactly as they were");
        {
            const OldInstall old ("migration_copy");
            old.populate();
            const auto before = old.snapshot();

            const auto report = migration::migrate (old.where);
            expect (report.folderCopied && report.settingsCopied && report.problems.isEmpty(), report.problems.joinIntoString ("; "));
            for (const auto* rel : { "presets/Tech Death.json", "presets/Live/Verse.json", "models/High gain/Lead.nam", "irs/Cab A.wav" })
            {
                const auto from = old.where.oldFolder.getChildFile (rel), to = old.where.newFolder.getChildFile (rel);
                expect (to.existsAsFile() && to.loadFileAsString() == from.loadFileAsString(), rel);
            }
            expectEquals (old.where.newSettings.loadFileAsString(), old.where.oldSettings.loadFileAsString());
            expectEquals (report.copiedFiles.size(), 5); // four files and the settings
            expect (! old.where.newFolder.getSiblingFile ("BellyDSP.migrating").exists()); // the staging folder is gone

            // The old install is untouched: every file it had is still there, byte for byte.
            auto after = old.snapshot();
            juce::StringArray oldOnly;
            for (const auto& row : after)
                if (! row.startsWith ("Application Support/BellyDSP"))
                    oldOnly.add (row);
            expectEquals (oldOnly.joinIntoString ("\n"), before.joinIntoString ("\n"));

            for (const auto& line : report.describe (old.where))
                logMessage ("  -> " + line.replace (old.root.getFullPathName(), "<tmp>"));
        }

        beginTest ("it happens once: an existing BellyDSP folder or settings file is never overwritten");
        {
            const OldInstall old ("migration_once");
            old.populate();
            expect (migration::migrate (old.where).anythingCopied());

            // The user changes something under the new name; a second start must not copy over it.
            old.where.newFolder.getChildFile ("presets/Tech Death.json").replaceWithText ("edited in BellyDSP");
            old.where.newSettings.replaceWithText ("new settings");
            old.write ("presets/Only in old.json", "{}");
            const auto again = migration::migrate (old.where);
            expect (! again.anythingCopied() && again.problems.isEmpty());
            expectEquals (old.where.newFolder.getChildFile ("presets/Tech Death.json").loadFileAsString(), juce::String ("edited in BellyDSP"));
            expectEquals (old.where.newSettings.loadFileAsString(), juce::String ("new settings"));
            expect (! old.where.newFolder.getChildFile ("presets/Only in old.json").exists());
            logMessage ("  -> second start: nothing copied, the edited preset and settings kept");
        }

        beginTest ("partial cases: only the settings file (no old data folder), and a fresh machine (nothing at all)");
        {
            // Sean's Mac on 2026-10-03: "Amp Sim.settings" exists, the AmpSim folder doesn't.
            const OldInstall settingsOnly ("migration_settings_only");
            settingsOnly.where.oldSettings.getParentDirectory().createDirectory();
            settingsOnly.where.oldSettings.replaceWithText ("<PROPERTIES/>");
            const auto report = migration::migrate (settingsOnly.where);
            expect (! report.folderCopied && report.settingsCopied && report.problems.isEmpty());
            expect (! settingsOnly.where.newFolder.exists()); // no folder made up out of nothing
            expectEquals (settingsOnly.where.newSettings.loadFileAsString(), juce::String ("<PROPERTIES/>"));

            const OldInstall fresh ("migration_fresh");
            const auto none = migration::migrate (fresh.where);
            expect (! none.anythingCopied() && none.problems.isEmpty());
            expect (! fresh.where.newFolder.exists() && ! fresh.where.newSettings.exists());
            logMessage ("  -> settings only: copied the settings, no folder; fresh machine: nothing copied, nothing created");
        }

        beginTest ("an interrupted earlier copy (a leftover staging folder) doesn't stop the next one");
        {
            const OldInstall old ("migration_interrupted");
            old.populate();
            const auto staging = old.where.newFolder.getSiblingFile ("BellyDSP.migrating");
            staging.getChildFile ("presets").createDirectory();
            staging.getChildFile ("presets/half-copied.json").replaceWithText ("{");
            const auto report = migration::migrate (old.where);
            expect (report.folderCopied && report.problems.isEmpty());
            expect (! staging.exists());
            expect (! old.where.newFolder.getChildFile ("presets/half-copied.json").exists());
            expect (old.where.newFolder.getChildFile ("models/High gain/Lead.nam").existsAsFile());
            logMessage ("  -> the leftover staging folder was discarded and the copy completed");
        }
    }
};

MigrationTests migrationTests;
} // namespace

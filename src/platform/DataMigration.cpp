// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "DataMigration.h"
#include "AppInfo.h"

#include <juce_data_structures/juce_data_structures.h>

namespace platform::migration
{
namespace
{
/// Where JUCE's standalone wrapper keeps its settings for an app called `name`: the same Options it
/// builds (juce_audio_plugin_client_Standalone.cpp), so the path is exactly the one it reads.
juce::File standaloneSettingsFile (const juce::String& name)
{
    juce::PropertiesFile::Options options;
    options.applicationName = name;
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "";
    return options.getDefaultFile();
}

/// Every file under `folder`, as paths relative to it, sorted.
juce::StringArray filesUnder (const juce::File& folder)
{
    juce::StringArray files;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
        files.add (entry.getFile().getRelativePathFrom (folder).replaceCharacter ('\\', '/'));
    files.sort (true);
    return files;
}
} // namespace

Locations legacyLocations()
{
    Locations where;
    where.newFolder = userDataFolder();
    where.oldFolder = where.newFolder.getSiblingFile ("AmpSim"); // the old name, same parent on both platforms
    where.oldSettings = standaloneSettingsFile ("Amp Sim");
    where.newSettings = standaloneSettingsFile (productName);
    return where;
}

Report migrate (const Locations& where)
{
    Report report;

    // 1. The data folder (first: on Windows the new settings file lives inside the new folder, and
    //    copying it first would make the folder "exist" before its contents arrived).
    if (! where.newFolder.exists() && where.oldFolder.isDirectory())
    {
        const auto staging = where.newFolder.getSiblingFile (where.newFolder.getFileName() + ".migrating");
        staging.deleteRecursively(); // a leftover from an interrupted attempt
        if (! where.oldFolder.copyDirectoryTo (staging))
            report.problems.add ("couldn't copy " + where.oldFolder.getFullPathName() + " to " + staging.getFullPathName());
        else if (! staging.moveFileTo (where.newFolder))
            report.problems.add ("couldn't rename " + staging.getFullPathName() + " to " + where.newFolder.getFullPathName());
        else
        {
            report.folderCopied = true;
            report.copiedFiles.addArray (filesUnder (where.newFolder));
        }
        if (! report.folderCopied)
            staging.deleteRecursively();
    }

    // 2. The standalone wrapper's settings file.
    if (! where.newSettings.exists() && where.oldSettings.existsAsFile())
    {
        if (where.newSettings.getParentDirectory().createDirectory().wasOk() && where.oldSettings.copyFileTo (where.newSettings))
        {
            report.settingsCopied = true;
            report.copiedFiles.add (where.oldSettings.getFileName() + " -> " + where.newSettings.getFileName());
        }
        else
            report.problems.add ("couldn't copy " + where.oldSettings.getFullPathName() + " to " + where.newSettings.getFullPathName());
    }
    return report;
}

juce::StringArray Report::describe (const Locations& where) const
{
    juce::StringArray lines;
    if (folderCopied)
        lines.add ("Copied " + where.oldFolder.getFullPathName() + " to " + where.newFolder.getFullPathName() + " (the old folder is untouched):");
    for (const auto& f : copiedFiles)
        lines.add ("  " + f);
    if (settingsCopied)
        lines.add ("Copied the settings " + where.oldSettings.getFullPathName() + " to " + where.newSettings.getFullPathName() + " (the old file is untouched)");
    for (const auto& p : problems)
        lines.add ("Problem: " + p);
    return lines;
}

Report runAtStartup()
{
    const auto where = legacyLocations();
    const auto report = migrate (where);
    if (! report.anythingCopied() && report.problems.isEmpty())
        return report;

    auto lines = report.describe (where);
    lines.insert (0, juce::String (productName) + " " + appVersion() + ", " + juce::Time::getCurrentTime().toISO8601 (true)
                         + ": moving over from the app's old name, Amp Sim");
    for (const auto& line : lines)
        juce::Logger::writeToLog (line);
    if (where.newFolder.createDirectory().wasOk())
        where.newFolder.getChildFile ("migration-log.txt").appendText (lines.joinIntoString ("\n") + "\n\n");
    return report;
}
} // namespace platform::migration

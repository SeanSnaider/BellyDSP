// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

/// The one-time move of the user's files from the app's old name ("Amp Sim") to BellyDSP's folders
/// (ASSUMPTIONS DS37). Two things moved when the app was renamed on 2026-10-03:
///
///   the data folder    ~/Library/Application Support/AmpSim     -> .../BellyDSP        (presets, models, irs)
///                      %APPDATA%\AmpSim                         -> %APPDATA%\BellyDSP
///   the settings file  ~/Library/Application Support/Amp Sim.settings -> .../BellyDSP.settings
///                      %APPDATA%\Amp Sim\Amp Sim.settings        -> %APPDATA%\BellyDSP\BellyDSP.settings
///                      (JUCE's standalone wrapper names it after the product: the audio device setup,
///                      the window position, and the last sound)
///
/// It COPIES, never moves or deletes: the old folder and file stay exactly as they were, so going back to
/// an old build loses nothing and a failed copy can be retried. Each item is copied only when its new
/// location doesn't exist yet, so it happens once, and anything already made under the new name wins.
namespace platform::migration
{
struct Locations
{
    juce::File oldFolder, newFolder;     ///< The data folders.
    juce::File oldSettings, newSettings; ///< The standalone wrapper's settings files.
};

/// The real locations on this machine.
Locations legacyLocations();

struct Report
{
    bool folderCopied = false;
    bool settingsCopied = false;
    juce::StringArray copiedFiles; ///< Each file copied, relative to the old folder (or the settings file's name).
    juce::StringArray problems;    ///< Anything that failed (the old files are untouched either way).

    bool anythingCopied() const noexcept { return folderCopied || settingsCopied; }
    /// One line per fact, for the log.
    juce::StringArray describe (const Locations& where) const;
};

/// Copies whatever needs copying (see above). Pure file work on the given locations, so tests run it on
/// temporary folders. The folder is copied into a temporary sibling first and renamed into place, so an
/// interrupted copy never leaves a half-filled new folder that would stop the next attempt.
Report migrate (const Locations& where);

/// Main thread, at startup, before the standalone wrapper reads its settings file: migrate() on the real
/// locations. When something was copied it logs each line (juce::Logger) and appends them, with the
/// time, to migration-log.txt in the new folder. Never on the audio thread (there isn't one yet).
Report runAtStartup();
} // namespace platform::migration

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Updater.h"

#include <juce_events/juce_events.h>

#include <winsparkle.h>

// WinSparkle (winsparkle.org), the Windows counterpart of Sparkle. WinSparkle.dll ships next to
// "BellyDSP.exe" (CMakeLists.txt copies it; the installer installs it).
//
// What a friend sees: once a day (and at launch when a day has passed) WinSparkle reads
// appcast-windows.xml from the latest GitHub release. If there's a newer version it shows its own
// "A new version of BellyDSP is available" dialog (Install update / Remind me later / Skip this version).
// "Install update" downloads the installer, checks its Ed25519 signature against the key below, asks the
// app to quit (the callbacks below), and runs it with the appcast's installer arguments (silent, per-user,
// so no UAC prompt), which reopens the app when it's done. WinSparkle has no fully silent mode: the
// closest, win_sparkle_check_update_with_ui_and_install(), skips the question but still shows a progress
// window and quits the app straight away, which is worse mid-song than a dialog you can postpone
// (docs/RELEASING.md, "Windows updates").
//
// UNTESTED: this file has never been compiled (no Windows machine); the CI workflow builds it.
#ifndef AMPSIM_FEED_URL
 #error "CMakeLists.txt defines AMPSIM_FEED_URL when AMPSIM_UPDATER is on"
#endif
#ifndef AMPSIM_ED_PUBLIC_KEY
 #error "CMakeLists.txt defines AMPSIM_ED_PUBLIC_KEY when AMPSIM_UPDATER is on"
#endif

namespace platform::updater
{
namespace
{
/// Owns WinSparkle's lifetime: cleaned up on the message thread when the app shuts down (JUCE deletes
/// DeletedAtShutdown objects in JUCEApplication's shutdown).
struct Session final : public juce::DeletedAtShutdown
{
    Session()
    {
        win_sparkle_set_appcast_url (AMPSIM_FEED_URL);
        win_sparkle_set_eddsa_public_key (AMPSIM_ED_PUBLIC_KEY);
        win_sparkle_set_app_details (L"Sean Snaider", L"BellyDSP", juce::String (AMPSIM_VERSION_STRING).toWideCharPointer());

        // Explicitly on, so WinSparkle doesn't ask "check automatically?" on the second launch.
        win_sparkle_set_automatic_check_for_updates (1);
        win_sparkle_set_update_check_interval (24 * 60 * 60);

        // Before installing, WinSparkle asks whether the app can quit, then asks it to. Both arrive on
        // WinSparkle's thread; quitting goes through the message thread like any other quit.
        win_sparkle_set_can_shutdown_callback ([]() -> int { return 1; });
        win_sparkle_set_shutdown_request_callback ([]
        {
            juce::MessageManager::callAsync ([]
            {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        });

        win_sparkle_init();
    }

    ~Session() override { win_sparkle_cleanup(); }
};

Session* session = nullptr;
} // namespace

void start()
{
    if (session == nullptr)
        session = new Session(); // deleted at shutdown
}

bool isRunning()
{
    return session != nullptr;
}

void checkNow()
{
    if (session != nullptr)
        win_sparkle_check_update_with_ui();
}

juce::String describe()
{
    return session != nullptr ? "Updates are checked daily and offered in a dialog" : "Updates haven't started yet";
}
} // namespace platform::updater

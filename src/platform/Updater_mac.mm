#include "Updater.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

// Sparkle 2 (sparkle-project.org), embedded in Amp Sim.app/Contents/Frameworks by CMakeLists.txt.
//
// Everything about the schedule is in Info.plist (CMakeLists.txt, AMPSIM_PLIST_TO_MERGE):
//   SUFeedURL                 the appcast: GitHub's "latest release" asset URL, which redirects to the
//                             newest release's appcast.xml (no web hosting needed)
//   SUPublicEDKey             the Ed25519 public key; an update whose signature doesn't verify is refused
//   SUEnableAutomaticChecks   YES, so Sparkle never asks "check automatically?" on the second launch
//   SUAutomaticallyUpdate     YES: a newer version downloads in the background and installs when the app
//                             quits, with no dialog
//   SUScheduledCheckInterval  86400 seconds (once a day; a check is also due at launch if a day has passed)
// Sparkle also checks that the new app is signed by the same certificate as the running one (its
// designated requirement), which is why every release is signed with the same self-signed identity.
//
// This file is compiled without ARC (as JUCE's own Objective-C++ is), so the controller is retained by
// hand: alloc gives us ownership, and it's meant to live until the process exits.
namespace platform::updater
{
namespace
{
SPUStandardUpdaterController* controller = nil;

bool bundleIsConfigured()
{
    NSBundle* main = [NSBundle mainBundle];
    const id feed = [main objectForInfoDictionaryKey: @"SUFeedURL"];
    const id key = [main objectForInfoDictionaryKey: @"SUPublicEDKey"];
    return [feed isKindOfClass: [NSString class]] && [(NSString*) feed length] > 0
        && [key isKindOfClass: [NSString class]] && [(NSString*) key length] > 0;
}
} // namespace

void start()
{
    if (controller != nil || ! bundleIsConfigured())
        return;

    // Starting the updater schedules the first check (immediately if one is overdue). Sparkle runs its
    // network and install work on its own queues and in its own helper processes (Autoupdate, Updater.app).
    controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater: YES
                                                               updaterDelegate: nil
                                                            userDriverDelegate: nil];
}

bool isRunning()
{
    return controller != nil;
}

void checkNow()
{
    if (controller != nil)
        [controller checkForUpdates: nil];
}

juce::String describe()
{
    if (controller == nil)
        return bundleIsConfigured() ? "Updates haven't started yet" : "This build doesn't update itself (no feed or key in its Info.plist)";
    return controller.updater.automaticallyDownloadsUpdates ? "Updates download in the background and install when you quit"
                                                            : "Updates are offered when they're found";
}
} // namespace platform::updater

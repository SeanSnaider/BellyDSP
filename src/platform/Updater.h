#pragma once

#include <juce_core/juce_core.h>

/// Automatic updates (docs/RELEASING.md). One implementation per platform, chosen by CMake:
///   Updater_mac.mm    Sparkle 2 (SPUStandardUpdaterController): checks the appcast once a day, downloads
///                     a newer version silently, and installs it when the app quits.
///   Updater_win.cpp   WinSparkle: checks once a day and offers the update in its own dialog; the
///                     installer runs silently and reopens the app.
///   Updater_none.cpp  Everything else: dev builds (AMPSIM_UPDATER=OFF), the tests, the console tools.
/// Message thread only. None of it ever touches the audio thread: the updater runs on its own threads and
/// processes, and the only call into the app is the request to quit before installing on Windows.
namespace platform::updater
{
/// Starts the updater once (later calls do nothing). The standalone app calls it when its window opens;
/// a plugin build must not (a host owns its own updates), and doesn't.
void start();

/// True once start() has run in a build that has an updater configured (feed URL and public key).
bool isRunning();

/// "Check for updates...": a check with the platform's own progress and result UI.
void checkNow();

/// One line for the UI about what this build does ("Updates install automatically when you quit", or
/// why there are none).
juce::String describe();
} // namespace platform::updater

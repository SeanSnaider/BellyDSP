// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Updater.h"

// Builds without an updater: dev builds (AMPSIM_UPDATER=OFF, the default, so a dev build never replaces
// itself with a release), the tests, and the console tools.
namespace platform::updater
{
void start() {}
bool isRunning() { return false; }
void checkNow() {}
juce::String describe() { return "This build doesn't update itself (a developer build)"; }
} // namespace platform::updater

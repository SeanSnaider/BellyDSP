// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

/// The app's own settings, per install: a small JSON file, settings.json, in the BellyDSP data folder
/// (platform::userDataFolder()). Things the app remembers about this install, never about a sound, so they
/// live in neither presets nor the processor's state (a preset or a project mustn't carry them to another
/// machine). Message thread only; every call reads or writes the file, which is tiny.
///
/// So far: whether Gate A's first switch-on has run Learn (ASSUMPTIONS PT13).
namespace platform::settings
{
/// The file. Tests point it at a file of their own (setFileForTests), so they never touch the user's.
juce::File file();
void setFileForTests (const juce::File& f);

bool getBool (const juce::String& key, bool defaultValue);
/// False if the file couldn't be written (the value still holds for this run).
bool setBool (const juce::String& key, bool value);

/// The keys.
inline constexpr const char* gateAutoLearnDone = "gateAutoLearnDone";
} // namespace platform::settings

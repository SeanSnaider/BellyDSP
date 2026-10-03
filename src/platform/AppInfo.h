// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

/// What the app knows about itself as a shipped product: its version and where its bundled files are.
/// (docs/RELEASING.md: the release build sets the version; the build copies the files.)
namespace platform
{
/// The version this build was made as, from the repo's VERSION file (or the release script's
/// -DAMPSIM_VERSION), for example "0.1.1".
juce::String appVersion();

/// The folder the build copies the app's own files into: on macOS the bundle's Contents/Resources, on
/// Windows (and for the console tools and tests) the folder the executable is in.
juce::File resourcesFolder();

/// THIRD_PARTY_NOTICES.txt, generated at build time (tools/notices/make_notices.cmake).
juce::File noticesFile();

/// The bundled starter content (content/ in the repo: captures, IRs, and their licences), which presets
/// refer to as "factory:models/..." and "factory:irs/...".
juce::File factoryContentFolder();
} // namespace platform

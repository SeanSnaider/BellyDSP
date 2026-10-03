// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

/// What the app knows about itself as a shipped product: its name, version, licence, where its source
/// and bundled files are, and where the user's own files live. (docs/RELEASING.md: the release build sets
/// the version; the build copies the files.)
///
/// The product is BellyDSP. Internally the code keeps its codename (namespace ampsim, AmpSimProcessor,
/// the ampsim_* tools; ASSUMPTIONS DS36): only what a user or a visitor to the repo sees is renamed.
namespace platform
{
/// "BellyDSP": the name a user sees (window title, brand, menus, data folder).
inline constexpr const char* productName = "BellyDSP";

/// The public repo, from SOURCE_URL in tools/release/release.conf: "https://github.com/SeanSnaider/BellyDSP".
juce::String sourceUrl();

/// Where this exact version's source is: <sourceUrl>/tree/v<version>. The AGPL's "Corresponding
/// Source" offer: every release is built from a commit on the public repo with that tag (RELEASING.md).
juce::String sourceUrlForThisVersion();

/// The user's own files (presets, models, irs): ~/Library/Application Support/BellyDSP on macOS,
/// %APPDATA%\BellyDSP on Windows.
juce::File userDataFolder();

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

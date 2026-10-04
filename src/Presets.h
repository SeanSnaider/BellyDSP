// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class AmpSimProcessor;

/// Presets (BUILD_PLAN "Presets and scenes"): one JSON file per preset.
///
///   {
///     "format_version": 2,
///     "name": "...",
///     "parameters": { "<parameter id>": <plain value>, ... },   every parameter except global settings
///     "amps": [ <file>, <file>, <file> ],                        one per slot, an empty path for an empty slot
///     "cab": { "mic1": <file>, "mic2": <file>, "room": <file> }, an IR file, or a pack folder for a close mic
///     "cab_assign": [ <file>, <file>, <file> ],                  each amp slot's cab for "Follow amp choice" (optional)
///     "cab_follow": true,                                        whether switching slots loads it (optional, default true)
///
///     <file> = { "path": "models/High gain/Lead.nam", "hash": "fnv1a64:0123456789abcdef", "size": 123456 }
///     The path is relative to the library root ("models/" or "irs/", under ~/Library/Application Support/
///     BellyDSP) when the file is inside it, absolute otherwise. A file that has moved is found again by
///     searching the library for the same size and content hash; one that's truly gone loads as an empty
///     slot with a warning, and the rest of the preset still loads.
///     A file bundled with the app (content/ in the repo, docs/RELEASING.md "Bundled content") is
///     "factory:models/..." or "factory:irs/...", resolved against the installed app's content folder.
///     That's still format version 2: an older build sees a path it can't find and warns, as for any
///     missing file.
///
///   Version 1 (Phase 5) stored plain absolute paths; migrateV1toV2() turns them into <file> entries
///   without a hash (so they can't be relinked if they move). Every step of the format gets one small,
///   pure, tested migration function, and the golden files in tests/fixtures/presets load the same on
///   every build.
///     "order": { "pre": [ "gate", "comp", ... ], "post": [ "eq", "comp", "bloom", ... ], "bloom": [ "bitcrush", "phaser", "flanger" ] },
///     "midi": [ { "cc": 82, "action": "toggle", "parameter": "delay_on" }, ... ],
///     "scenes": { "parameters": [ "delay_mix", ... ], "list": [ { "name": "Verse", "values": { ... } }, null, ... ] }
///   }
///
/// Loading: a parameter the file doesn't mention takes its default (so a preset always means the same
/// thing, whatever was set before); an ID the app doesn't know is skipped with a warning; a file from a
/// newer format version is refused. Global settings (input calibration, the drive blocks' oversampling,
/// and the tuner's settings so far) are never saved in a preset or changed by one.
namespace presets
{
constexpr int formatVersion = 2;

/// The library roots relinking searches: "models" for captures, "irs" for cab IRs and packs. Default
/// ~/Library/Application Support/BellyDSP/<kind> (%APPDATA%\BellyDSP\<kind> on Windows); tests point them
/// elsewhere. "factory" is the app's bundled content (platform::factoryContentFolder()), which
/// "factory:" paths resolve against and relinking also searches.
juce::File libraryRoot (const juce::String& kind);
void setLibraryRoot (const juce::String& kind, const juce::File& folder);

/// A file (or cab pack folder) as a preset refers to it.
struct FileRef
{
    juce::String path; ///< "models/..." or "irs/..." inside the library, "factory:..." for bundled content, absolute elsewhere, empty for none.
    juce::String hash; ///< contentHash() when saved; empty in presets migrated from version 1.
    juce::int64 size = 0;

    juce::var toVar() const;
    static FileRef fromVar (const juce::var& v);
};

/// 64-bit FNV-1a over the file's bytes, as "fnv1a64:" and 16 hex digits. A folder hashes its files'
/// relative paths and hashes, sorted, so the same pack anywhere has the same hash.
juce::String contentHash (const juce::File& fileOrFolder);

/// Bytes in the file, or in every file under a folder.
juce::int64 contentSize (const juce::File& fileOrFolder);

/// The reference a preset stores for this file.
FileRef makeRef (const juce::File& fileOrFolder, const juce::String& kind);

struct Resolved
{
    juce::File file;      ///< What to load (when found).
    bool found = false;
    bool relinked = false; ///< Found by its hash somewhere else in the library.
    bool changed = false;  ///< Found at its path, but its content isn't what was saved.
};

/// Where a reference points now: its path if that exists, otherwise the first file (or folder) under the
/// library root with the same size and hash.
Resolved resolve (const FileRef& ref, const juce::String& kind);

/// Format migrations: one pure function per step. migrate() runs every step a preset needs.
juce::var migrateV1toV2 (const juce::var& v1);
juce::var migrate (const juce::var& preset);

/// Parameters that belong to the rig, not the sound, and are never part of a preset.
bool isGlobal (const juce::String& parameterId);

/// The processor's current sound as a preset.
juce::var capture (AmpSimProcessor& processor, const juce::String& name);

struct ApplyResult
{
    bool ok = false;
    juce::String error;
    juce::StringArray warnings;
};

/// Message thread: sets every parameter, starts loading the captures and IRs, and sets the effect
/// order. (AmpSimProcessor::loadPreset() wraps this in a fade.)
ApplyResult apply (AmpSimProcessor& processor, const juce::var& preset);

/// Checks a preset without applying it.
ApplyResult validate (const juce::var& preset);

/// ~/Library/Application Support/BellyDSP/presets (%APPDATA%\BellyDSP\presets on Windows)
juce::File defaultFolder();

/// The factory presets (the five style presets), in the plan's order. They set the sound and its scenes,
/// put the built-in captures in their slots (Glass, Ember, Monolith) and a bundled cab IR
/// ("factory:irs/...", content/irs) in close mic 1, and say in their "notes" what to swap in.
juce::Array<juce::var> factoryPresets();

/// A factory preset's current name for a name it once had: "Polyphia" is "Modern Prog" and "CHON" is "Math
/// Rock" since 2026-10-03 (no band names in the app). Any other name comes back unchanged. A saved state
/// remembers the last factory preset by name (the top bar's name and "Factory" tag), so restoring one goes
/// through this.
juce::String currentFactoryPresetName (const juce::String& name);

/// The built-in captures (content/models, BUILD_PLAN decision log 2026-10-03): one per amp slot, named after
/// the head it plays in: slot 1 Glass (clean), slot 2 Ember (crunch), slot 3 Monolith (high gain). Stand-ins
/// trained from the project's own gray-box amp (prototypes/amp_sim.py, tools/content/make_default_captures.py)
/// until Sean's own captures replace them. A fresh slot starts on its built-in (AmpSimProcessor).
/// Since 2026-10-04 each is a gain set (BUILD_PLAN "Amp gain"): content/models/Glass/ holds five captures across
/// the amp's gain knob and the gainset.json that lists them, and the JSON is what slots, presets, and the state
/// refer to.
juce::String builtInCaptureName (int slot);
juce::String builtInCapturePath (int slot); ///< "factory:models/Glass/gainset.json", as a preset refers to it
juce::File builtInCapture (int slot);       ///< that file in the app's content folder

/// The built-in captures used to be single files ("factory:models/Glass.nam"). For a reference to one of those
/// (that factory path, or a saved absolute path ending in content/models/Glass.nam), the gain set that replaced
/// it; anything else, nothing. resolve() and a restored state use it, so old presets and states get the sets.
juce::File replacementForRetiredCapture (const juce::String& path);

/// Whether a file ships with the app (it's inside the app's content folder, libraryRoot ("factory")).
bool isBundled (const juce::File& file);

/// For a saved absolute path into some copy of the app's content folder (a dev build's, or an install that
/// has since moved): the same file in this copy's content folder, or nothing. A saved state stores absolute
/// paths, so a built-in capture saved by build/BellyDSP_artefacts/... is still found by /Applications/BellyDSP.app.
juce::File bundledElsewhere (const juce::File& savedPath);

bool save (const juce::var& preset, const juce::File& file);
juce::var load (const juce::File& file, juce::String& error);
} // namespace presets

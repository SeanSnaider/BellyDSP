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
///
///     <file> = { "path": "models/High gain/Lead.nam", "hash": "fnv1a64:0123456789abcdef", "size": 123456 }
///     The path is relative to the library root ("models/" or "irs/", under ~/Library/Application Support/
///     AmpSim) when the file is inside it, absolute otherwise. A file that has moved is found again by
///     searching the library for the same size and content hash; one that's truly gone loads as an empty
///     slot with a warning, and the rest of the preset still loads.
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
/// ~/Library/Application Support/AmpSim/<kind>; tests point them elsewhere.
juce::File libraryRoot (const juce::String& kind);
void setLibraryRoot (const juce::String& kind, const juce::File& folder);

/// A file (or cab pack folder) as a preset refers to it.
struct FileRef
{
    juce::String path; ///< "models/..." or "irs/..." inside the library, absolute outside it, empty for none.
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

/// ~/Library/Application Support/AmpSim/presets
juce::File defaultFolder();

bool save (const juce::var& preset, const juce::File& file);
juce::var load (const juce::File& file, juce::String& error);
} // namespace presets

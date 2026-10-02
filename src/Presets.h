#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class AmpSimProcessor;

/// Basic presets (BUILD_PLAN "Presets and scenes", the part built in Phase 5): one JSON file per
/// preset, written as format_version 1 of the full preset format, so Phase 11's migrations start here.
///
///   {
///     "format_version": 1,
///     "name": "...",
///     "parameters": { "<parameter id>": <plain value>, ... },   every parameter except global settings
///     "amps": [ "<capture path>", "", "" ],                      one per slot, "" for an empty slot
///     "cab": { "mic1": "<IR file or pack folder>", "mic2": "", "room": "" },
///     "order": { "pre": [ "comp", "eq" ], "post": [ "eq", "comp", "delay" ] },
///     "midi": [ { "cc": 82, "action": "toggle", "parameter": "delay_on" }, ... ]
///   }
///
/// Loading: a parameter the file doesn't mention takes its default (so a preset always means the same
/// thing, whatever was set before); an ID the app doesn't know is skipped with a warning; a file from a
/// newer format version is refused. Global settings (input calibration and the drive blocks'
/// oversampling so far) are never saved in a preset or changed by one.
namespace presets
{
constexpr int formatVersion = 1;

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

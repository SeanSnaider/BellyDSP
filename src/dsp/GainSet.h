// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace ampsim
{

/// A gain set (BUILD_PLAN "Amp gain"; docs/CAPTURING.md "Gain sets"): several captures of one amp, each
/// at a different position of the amp's own gain knob, listed in a small JSON file next to them:
///
///   {
///     "format": "bellydsp-gain-set",
///     "version": 1,
///     "name": "Glass",
///     "description": "Clean to the edge of breakup",       (optional)
///     "tone_type": "clean",                                 (optional, as in NAM's metadata)
///     "steps": [ { "gain": 0, "file": "Glass, gain 0.nam" }, { "gain": 2.5, "file": "Glass, gain 2.5.nam" }, ... ]
///   }
///
/// A capture is a snapshot of an amp at one gain setting: turning the input up past what it was trained on
/// makes the model extrapolate, not gain up (ASSUMPTIONS AG1). A set instead captures the knob itself, and
/// the slot's Gain knob moves across the steps (NamAmp). Each step's "gain" is the position (0 to 10) on the
/// slot's Gain knob that plays that capture alone; positions between two steps blend them. "file" is
/// relative to the JSON's folder. Any other fields (a step's "hash", say) are ignored.
///
/// The app refers to a set by its JSON file (conventionally gainset.json in a folder named after the amp),
/// so everything that keeps a capture's path (presets, the state, tone match) works unchanged.
struct GainSet
{
    struct Step
    {
        double gain = 0.0; ///< its position on the Gain knob, 0 to 10
        juce::File file;
    };

    static constexpr const char* formatName = "bellydsp-gain-set";
    static constexpr const char* conventionalFileName = "gainset.json";
    static constexpr int maxSteps = 11;

    juce::String name, description, toneType;
    std::vector<Step> steps; ///< in ascending gain

    /// Whether a file is a gain set's JSON (by extension and its "format" field), or a folder holding a
    /// gainset.json. Cheap enough for the message thread.
    static bool isGainSet (const juce::File& fileOrFolder);

    /// The JSON file for a gain set given as its file or its folder (the folder's gainset.json).
    static juce::File jsonFor (const juce::File& fileOrFolder);

    /// Reads and checks a set: the format and version, 1 to 11 steps, each gain in 0..10 and strictly
    /// ascending, every file present. On failure, returns false and says why in `error`.
    static bool read (const juce::File& fileOrFolder, GainSet& out, juce::String& error);
};

} // namespace ampsim

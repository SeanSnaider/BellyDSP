// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <map>

/// Scenes (BUILD_PLAN "Presets and scenes"): 8 snapshots inside a preset, so one stomp moves a song from
/// verse to chorus. A scene holds the amp that plays, every block's on/off switch, and a chosen set of
/// other parameters (delay mix or reverb level, say). Recalling one sets those parameters, so it takes
/// effect as fast as they do (the amp switch: a warm-up of about 85 ms, then a 20 ms crossfade; the blocks' 10 ms bypass fades, the knobs'
/// smoothing) and nothing reloads: it's instant from the footswitch. Everything a scene doesn't hold is
/// left alone. Message thread only.
///
/// Saved in presets and the app's state as { "parameters": [ "delay_mix", ... ], "list": [ { "name": "Verse",
/// "values": { "<id>": <plain value>, ... } } or null for an empty scene, ... ] }.
class Scenes
{
public:
    static constexpr int count = 8;

    struct Scene
    {
        bool stored = false;
        juce::String name;
        std::map<juce::String, float> values; // plain values
    };

    /// Always in a scene: the amp (amp_model; amp_slot until 2026-10-07), every block switch ("..._on" and the cab bypass), never a global
    /// setting.
    static bool isSwitch (const juce::String& parameterId);

    /// The other parameters scenes hold, chosen per preset.
    const juce::StringArray& getChosen() const noexcept { return chosen; }
    bool isChosen (const juce::String& parameterId) const { return chosen.contains (parameterId); }
    void setChosen (const juce::String& parameterId, bool shouldBeChosen);

    /// The current values of everything a scene holds, into scene `index`.
    void store (int index, juce::AudioProcessorValueTreeState& state);

    /// Sets everything scene `index` holds. Returns false (and changes nothing) for an empty scene.
    bool recall (int index, juce::AudioProcessorValueTreeState& state);

    void clear (int index);
    void rename (int index, const juce::String& name);
    const Scene& get (int index) const { return scenes[(size_t) juce::jlimit (0, count - 1, index)]; }

    /// The scene last stored or recalled, -1 for none since the preset loaded.
    int getCurrent() const noexcept { return current; }

    juce::var toVar() const;

    /// Reads a saved set, skipping values for parameters this build doesn't have (with a warning each).
    static Scenes fromVar (const juce::var& v, juce::AudioProcessorValueTreeState& state, juce::StringArray* warnings = nullptr);

private:
    std::array<Scene, count> scenes;
    juce::StringArray chosen;
    int current = -1;
};

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <vector>

/// MIDI footswitch and expression-pedal mappings (BUILD_PLAN "MIDI control"): which controller (CC)
/// does what. Mappings are edited and applied on the message thread; the audio thread only forwards
/// controller events through a lock-free FIFO (CcFifo), and the processor's timer drains it, so a
/// mapped switch takes effect within one timer tick (20 ms) and nothing on the audio thread ever looks
/// a mapping up. (The amp-slot program changes, tap tempo, and reverb freeze are handled on the audio
/// thread itself, at once.)
///
/// Saved with the state and in presets as a list: [ { "cc": 82, "action": "toggle", "parameter":
/// "delay_on" }, { "cc": 11, "action": "continuous", "parameter": "output_gain", "min": -24, "max": 6 } ].
struct MidiMapping
{
    enum class Action
    {
        toggle,     // each press (value 64 or more) flips an on/off parameter; releases (below 64) are ignored
        momentary,  // follows the switch: on at 64 or more, off below. A momentary switch is on while held;
                    // a latching one (the controller itself alternates 127 and 0) stays on until pressed again
        continuous, // the value 0 to 127 sweeps the parameter from min to max (an expression pedal)
    };

    int cc = 0;
    Action action = Action::toggle;
    juce::String parameterId;
    float minimum = 0.0f, maximum = 1.0f; // continuous: the plain-value range swept

    static const char* actionName (Action action);
};

class MidiMap
{
public:
    const std::vector<MidiMapping>& getMappings() const noexcept { return mappings; }

    /// Adds a mapping, replacing any other mapping on the same CC.
    void set (const MidiMapping& mapping);
    void remove (int cc);
    void clear() { mappings.clear(); }

    /// Message thread: one controller event. Returns true if a mapping used it.
    bool handle (int cc, int value, juce::AudioProcessorValueTreeState& state);

    /// MIDI learn: the next controller event maps to this parameter (a toggle for on/off parameters,
    /// continuous over the full range for the rest). Returns true when the event was learned.
    void startLearn (const juce::String& parameterId) { learnTarget = parameterId; }
    void cancelLearn() { learnTarget.clear(); }
    bool isLearning() const noexcept { return learnTarget.isNotEmpty(); }
    const juce::String& getLearnTarget() const noexcept { return learnTarget; }
    bool learn (int cc, juce::AudioProcessorValueTreeState& state);

    juce::var toVar() const;
    /// Reads a saved list, skipping entries that don't name a known parameter (warnings for each).
    static MidiMap fromVar (const juce::var& v, juce::AudioProcessorValueTreeState& state, juce::StringArray* warnings = nullptr);

private:
    std::vector<MidiMapping> mappings;
    juce::String learnTarget;
};

/// Audio thread to message thread: controller events, lock-free and allocation-free on both sides.
/// When full, new events are dropped rather than ever blocking the audio thread.
class CcFifo
{
public:
    static constexpr int capacity = 256;

    /// Audio thread.
    bool push (int cc, int value) noexcept;

    /// Message thread: calls fn (cc, value) for each waiting event, oldest first.
    template <typename Fn>
    void drain (Fn&& fn)
    {
        const auto scope = fifo.read (fifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i)
            fn (events[(size_t) (scope.startIndex1 + i)].cc, events[(size_t) (scope.startIndex1 + i)].value);
        for (int i = 0; i < scope.blockSize2; ++i)
            fn (events[(size_t) (scope.startIndex2 + i)].cc, events[(size_t) (scope.startIndex2 + i)].value);
    }

    int getDropped() const noexcept { return dropped.load(); }

private:
    struct Event
    {
        juce::uint8 cc = 0, value = 0;
    };

    juce::AbstractFifo fifo { capacity };
    std::array<Event, capacity> events {};
    std::atomic<int> dropped { 0 };
};

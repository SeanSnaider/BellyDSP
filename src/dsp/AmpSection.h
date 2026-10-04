// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "AmpTone.h"
#include "Block.h"
#include "Gain.h"
#include "NamAmp.h"

#include <array>

namespace ampsim
{

/// The amp: three NAM slots that all run all the time on the same input (BUILD_PLAN "Seamless amp
/// switching"). Because every model keeps processing, its history (the receptive field) is always
/// current, so switching never restarts a model: the output simply crossfades from one slot to
/// another over 20 ms. The price is three models' worth of CPU (measured: 12.6-15.6% of the deadline
/// for three A1 standard models).
///
/// Each slot: the capture (with its Gain knob) -> tone controls -> output trim.
///
/// The switch is an equal-power crossfade built from independent linear ramps: each slot has a
/// position p ramping toward 1 (selected) or 0 (not), and contributes sin(pi/2 p) of its output.
/// For a plain A -> B switch that's cos(pi/2 t) and sin(pi/2 t), whose squares sum to 1, so the
/// level doesn't dip midway. A second switch mid-fade just redirects the ramps, with no jump.
class AmpSection : public Block
{
public:
    static constexpr int numSlots = 3;
    static constexpr double switchSeconds = 0.020;

    /// A slot's Gain knob, as its parameter stores it: amp*_input_trim, in dB from -24 to +24, which the
    /// head shows as 0 to 10 (position = 5 + dB / 4.8). The parameter ID (and this member's name) date
    /// from when Gain was always an input trim; it's permanent, so the meaning moved instead (BUILD_PLAN
    /// "Amp gain"): the slot's NamAmp turns the position into a place across a gain set's steps, or into a
    /// loudness-compensated input trim for a single capture, and does the smoothing. Audio thread.
    class GainKnob
    {
    public:
        void setGainDecibels (float db) noexcept { position = positionForDb (db); }
        void setPosition (float newPosition) noexcept { position = juce::jlimit (0.0f, NamAmp::gainMax, newPosition); }
        float getPosition() const noexcept { return position; }

        static float positionForDb (float db) noexcept
        {
            return juce::jlimit (0.0f, NamAmp::gainMax, NamAmp::gainDefault + db / (NamAmp::singleTrimRangeDb / NamAmp::gainDefault));
        }
        static float dbForPosition (float p) noexcept { return NamAmp::singleTrimDb (p); }

    private:
        float position = NamAmp::gainDefault;
    };

    struct Slot
    {
        GainKnob inputTrim; // the Gain knob (see GainKnob)
        NamAmp model;
        AmpTone tone;
        Gain outputTrim { false };
    };

    Slot& slot (int index) { return slots[(size_t) index]; }
    const Slot& slot (int index) const { return slots[(size_t) index]; }

    /// Audio thread. Crossfades to this slot (0-based) over 20 ms. Already selected: nothing happens.
    void selectSlot (int index);
    int getSelectedSlot() const noexcept { return selected; }

    /// Audio thread, for tests and meters: true while a slot switch is fading.
    bool isSwitching() const noexcept;

    /// Audio thread, for tests: true while any slot is fading in a newly loaded model.
    bool isLoadingModel() const noexcept;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;

private:
    std::array<Slot, numSlots> slots;
    std::array<juce::SmoothedValue<float>, numSlots> position; // 1 = fully selected, 0 = silent
    juce::AudioBuffer<float> slotOutputs;                       // one channel per slot
    int selected = 0;
};

} // namespace ampsim

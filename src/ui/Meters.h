#pragma once

#include "Theme.h"

#include <vector>

namespace ui
{

/// A peak meter (UI_DESIGN "Meter"): vertical bars on a -60 to 0 dBFS scale, green to -12 dBFS, amber
/// to -3, red above; a peak-hold line that stays 1.5 s; and a clip light that latches until the meter is
/// clicked. The editor feeds it the processor's peaks 30 times a second (the audio thread only writes
/// atomics); it falls at 24 dB a second and repaints only when what it shows changes.
class LevelMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    LevelMeter (juce::String label, int numChannels);

    /// GUI thread: the highest sample (linear) on each channel since the last call, `seconds` ago.
    void push (const float* peaks, double seconds);

    /// What each bar shows and holds, in dBFS (for tests and the readout).
    float getShownDb (int channel) const { return channels[(size_t) channel].shownDb; }
    float getHeldDb (int channel) const { return channels[(size_t) channel].heldDb; }
    bool isClipped() const;
    void resetClip();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    /// A sample at or above this is a clip (0 dBFS less a hair, as converters clip at full scale).
    static constexpr float clipLevel = 0.999f;

private:
    struct Channel
    {
        float shownDb = -100.0f, heldDb = -100.0f;
        double heldFor = 0.0;
        bool clipped = false;
    };

    juce::String label;
    std::vector<Channel> channels;
    std::vector<Channel> drawn; // what was painted last, to repaint only on change
};

/// A gain reduction meter (UI_DESIGN "Meter"): a bar growing down from the top, 0 to `rangeDb`.
class ReductionMeter final : public juce::Component
{
public:
    explicit ReductionMeter (float rangeDb = 24.0f) : range (rangeDb) {}
    void setReduction (float db);
    void paint (juce::Graphics&) override;

private:
    const float range;
    float reductionDb = 0.0f;
};

/// The CPU meter: the audio callback's time against its deadline (ASSUMPTIONS U5), as a number and a bar.
class CpuMeter final : public juce::Component
{
public:
    void setLoad (float percent);
    float getLoad() const noexcept { return load; }
    void paint (juce::Graphics&) override;

private:
    float load = 0.0f;
    int shown = -1;
};

} // namespace ui

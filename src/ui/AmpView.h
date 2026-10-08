// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "AmpHead.h"
#include "Analyzer.h"
#include "Pages.h"
#include "SectionTabs.h"

#include <map>

namespace ui
{

/// The output spectrum under the amp (handoff 6, option 1; ASSUMPTIONS UH13): the analyzer's post tap (the
/// chain's output), 4096-point Hann-windowed FFTs off the audio thread, smoothed over time and over 1/6 of
/// an octave, drawn as a 1.5 px emerald line over a faint emerald fill fading to transparent, on a log
/// axis from 20 Hz to 20 kHz with gridlines at 100 Hz, 1 kHz, and 10 kHz, a dashed 0 dB line, and 11 px
/// faint labels. The level is relative to the spectrum's own average between 100 Hz and 5 kHz (the dashed
/// line), so the curve shows the tone's balance whatever the playing level; silence draws no curve.
class SpectrumView final : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (AmpSimProcessor& processor);
    ~SpectrumView() override;

    /// Starts and stops reading the ring (the page's visibility).
    void setActive (bool shouldRead);

    /// Pulls what's in the ring, runs the FFT, and repaints (30 times a second while active).
    void update();
    SpectrumAnalyzer& getAnalyzer() noexcept { return analyzer; }

    /// The curve's dB (relative to the average) at a frequency, for tests; nothing (-inf) in silence.
    float relativeLevelAt (double frequency) const;
    bool hasCurve() const noexcept { return reference > -200.0f; }

    static constexpr float rangeDb = 30.0f; // the plot spans +-30 dB around the dashed line

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { update(); }
    juce::Rectangle<float> plotArea() const;

    AmpSimProcessor& ampSim;
    SpectrumAnalyzer analyzer;
    std::vector<float> columns; // the smoothed curve, per column of the plot (dB relative)
    float reference = -300.0f;
    bool active = false;
};

/// The Amp page (handoff 4.5): the amp tabs (30 x 20 minis, names, PC numbers), the 290 px stage with its
/// faint emerald glow and the 960 x 262 head of the playing slot, the info row (the capture's voice from
/// its metadata, its file or "Glass (built in)" for a bundled one, and 48 kHz or a message), the output
/// spectrum, and the shared strip (Input,
/// Gate with its open light, Output).
///
/// Each slot's seven knobs are its own parameters (ASSUMPTIONS UH3): Gain is amp*_input_trim, Master
/// its output trim, the rest its tone bands; all show 0 to 10 with one decimal (5.0 is 0 dB). Switching
/// slots (a tab, the footswitch, a scene) swaps the head's materials and its knobs.
///
/// Gain (BUILD_PLAN "Amp gain"; ASSUMPTIONS AG9) shows its position, 0 to 10 with one decimal, while dragged or
/// hovered, like the other knobs. With a gain set loaded it moves across the set's captures, and small dots just
/// outside its arc mark the steps, the one or two it's playing lit emerald. With a single capture it's the
/// capture's loudness-compensated trim (no dots).
class AmpView final : public ControlGroup, private juce::Timer
{
public:
    explicit AmpView (AmpSimProcessor& processor);
    ~AmpView() override;

    /// A click on the grille, the badge, or the model's name: load a capture into the slot. A right-click
    /// there: the capture menu (load, reload, clear).
    std::function<void (int slot)> onLoadCapture;
    std::function<void (int slot, juce::Component& near)> onCaptureMenu;

    /// The amp picker right of the tabs (Sean, 2026-10-07: "I can't find the new amps"): its menu lists every
    /// built-in amp (presets::builtInGainSets) and loads the one picked into the playing slot; its arrows step
    /// through them, wrapping around. The capture menu (a right-click on the grille) keeps reload and clear.
    juce::PopupMenu ampMenu();
    void stepAmp (int delta);
    juce::String getPickerText() const;
    juce::Component& getPicker() noexcept;
    /// The name the slot's tab and badge show: a gain set's name (any slot can play any built-in amp), else
    /// the slot's material.
    juce::String slotAmpName (int slot);

    /// The message shown where "48 kHz" sits (empty: the rate).
    void setStatus (const juce::String& message);

    int getShownSlot() const noexcept { return shownSlot; }
    TabRow& getTabs() noexcept { return tabs; }
    AmpHead& getHead() noexcept { return head; }
    PilotJewel& getJewel() noexcept { return jewel; }
    SpectrumView& getSpectrum() noexcept { return spectrum; }
    Knob& getKnob (int slot, int index) { return *knobs[(size_t) slot][(size_t) index]; }
    juce::Component& getGrille() noexcept;
    bool isGateLightOn() const noexcept { return gateOpen; }
    /// The gate's first switch-on, as the strip shows it beside the Gate group (empty: nothing shown).
    juce::String getGatePromptText() const { return gatePrompt; }
    Switch& getGateSwitch() noexcept { return *gateOn; }

    /// What the info row shows, for tests.
    juce::String getVoiceText() const;
    juce::String getModelText() const;
    juce::String getRateText() const;

    /// The capture's tone type from its NAM metadata ("Clean", "Crunch", "High gain"), or a gain set's
    /// "tone_type", or empty. Never the gear's make or model (no product names in the UI).
    static juce::String toneTypeOf (const juce::File& namFile);

    /// The Gain positions of a slot's gain set's steps (empty for a single capture or an empty slot), read
    /// from its gainset.json once per path.
    const std::vector<float>& gainSteps (int slot);

    /// For tests: the step dots as drawn now (the positions, and which are lit).
    std::vector<float> getShownGainSteps() const;
    std::vector<bool> getLitGainSteps() const;

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int numKnobs = 7;

private:
    class InfoRow;
    class AmpPicker;
    class Grille;
    class GateLight;
    class GainSteps;
    void updateGainSteps();
    void updateGatePrompt();
    void timerCallback() override;
    void showSlot (int slot);

    TabRow tabs;
    AmpHead head;
    PilotJewel jewel;
    std::unique_ptr<Grille> grille;
    std::unique_ptr<InfoRow> info;
    std::array<std::array<Knob*, numKnobs>, AmpSimProcessor::numAmpSlots> knobs {};
    SpectrumView spectrum;
    Knob *input = nullptr, *threshold = nullptr, *release = nullptr, *output = nullptr;
    Switch* gateOn = nullptr; // the strip's Gate group: Gate A's own switch (gate_a_on), under the group's name
    std::unique_ptr<GateLight> gateLight;
    juce::Rectangle<int> stageArea, curveArea, stripArea;
    std::array<int, 3> dividers {};
    int shownSlot = -1;
    bool gateOpen = false;
    juce::String gatePrompt; // the first switch-on's prompt or result, shown in the strip right of the groups
    float gateLearnProgress = -1.0f; // 0 .. 1 while Learn measures; -1 otherwise
    juce::Rectangle<int> promptArea;
    std::map<juce::String, juce::String> toneTypes; // capture path -> its tone type (read once)
    struct SetInfo
    {
        juce::String name;
        std::vector<float> steps;
    };
    std::map<juce::String, SetInfo> gainSets; // capture path -> its set's name and steps (empty: not a set)
    const SetInfo& setInfo (const juce::String& path);
    std::unique_ptr<GainSteps> gainStepDots;
    std::unique_ptr<AmpPicker> picker;
    std::vector<presets::BuiltInAmp> builtIns; // read once: the content folder doesn't change while the app runs
    int builtInIndex (int slot) const;         ///< the built-in amp the slot plays, or -1
};

} // namespace ui

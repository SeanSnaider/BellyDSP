// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "AmpHead.h"
#include "Analyzer.h"
#include "Pages.h"

#include <map>

namespace ui
{

/// The output spectrum (handoff 6, option 1; ASSUMPTIONS UH13), at the foot of the amp's inspector: the analyzer's
/// post tap (the chain's output), 4096-point Hann-windowed FFTs off the audio thread, smoothed over time and over 1/6
/// of an octave, drawn as a 1.5 px orange line over a faint fill fading to transparent, on a log axis from 20 Hz to
/// 20 kHz with gridlines at 100 Hz, 1 kHz, and 10 kHz, a dashed 0 dB line, and 11 px faint labels. The level is
/// relative to the spectrum's own average between 100 Hz and 5 kHz (the dashed line), so the curve shows the tone's
/// balance whatever the playing level; silence draws no curve.
class SpectrumView final : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (AmpSimProcessor& processor);
    ~SpectrumView() override;

    /// Starts and stops reading the ring (the inspector's visibility).
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

/// The head's material for an amp (0 to 8): each built-in amp its own (materialForAmp by name), your capture Custom.
Material materialForAmp (int amp);

class AmpView;

/// The amp in the inspector (the default selection): its name and description over its seven knobs (Gain and
/// Master, then the five tone knobs, in the chrome), the input and output levels, and the output spectrum. Only the
/// amp (RD4-9): Gate A is its own pedal. The knobs on the head and these are the same parameters, so turning either
/// moves both.
class AmpPanel final : public ControlGroup
{
public:
    AmpPanel (AmpSimProcessor& processor, AmpView& view);
    ~AmpPanel() override;

    Knob& getKnob (int amp, int index) { return *knobs[(size_t) amp][(size_t) index]; }
    SpectrumView& getSpectrum() noexcept { return spectrum; }

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    int preferredHeight (int width) override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void layout (InspectorLayout&);

    AmpView& view;
    std::array<std::array<Knob*, 7>, AmpSimProcessor::numAmps> knobs {};
    SpectrumView spectrum;
    Knob *input = nullptr, *output = nullptr;
    Switch* ampOn = nullptr; // the amp's switch (amp_bypass, inverted), in the header
    std::vector<InspectorLayout::Text> texts;
    juce::Rectangle<int> spectrumArea;
    int shownAmp = -1;
};

/// The stage's Amp mode (the default, and the 90% view): the shelf at the top left (Sean, 2026-10-07: "one amp,
/// picked from 8": a mini head per built-in amp, then your capture's once one is loaded), the playing amp's head at
/// large scale over the stage's lit floor, and the info row under it (the capture's voice from its metadata, its file
/// or "Glass (built in, gain set)" for a bundled one, and 48 kHz or a message). The head is drawn at its designed
/// 960 x 262 and scaled to the stage (getHeadScale()), knobs and all, so its art and its knobs' skins stay exact.
///
/// Each amp's seven knobs are its own parameters (ASSUMPTIONS UH3, AS1): Gain is amp*_input_trim, Master its output
/// trim, the rest its tone bands; all show 0 to 10 with one decimal (5.0 is 0 dB). Switching amps (the shelf, the
/// footswitch, a scene, a preset) swaps the head's material, its badge, and its knobs, so each amp comes back with
/// its knobs as they were.
///
/// Gain (BUILD_PLAN "Amp gain"; ASSUMPTIONS AG9) shows its position, 0 to 10 with one decimal, while dragged or
/// hovered, like the other knobs. With a gain set loaded it moves across the set's captures, and small dots just
/// outside its arc mark the steps, the one or two it's playing lit orange. With a single capture it's the
/// capture's loudness-compensated trim (no dots).
class AmpView final : public ControlGroup, private juce::Timer
{
public:
    explicit AmpView (AmpSimProcessor& processor);
    ~AmpView() override;

    /// A click on the grille, the badge, or the model's name: load a capture of your own (amp 9). A right-click
    /// there: the capture menu (load, reload, remove your capture).
    std::function<void (int amp)> onLoadCapture;
    std::function<void (int amp, juce::Component& near)> onCaptureMenu;

    /// The shelf: a faint "Amp", then a 30 x 20 mini head per built-in amp (ui::drawMiniAmp) 12 px apart in the
    /// shelf's order, and after a small gap your capture's once one is loaded. The playing amp has an orange
    /// underline and its name shown after the minis; hovering a mini shows its name and description there (and as a
    /// tooltip). A click plays that amp (amp_model, one undo step); a right-click learns MIDI for amp_model.
    juce::Component& getShelf() noexcept;
    /// For tests: whether an amp's mini is on the shelf, its bounds in this view, and a click on it.
    bool isOnShelf (int amp) const;
    juce::Rectangle<int> getMiniBounds (int amp) const;
    void clickMini (int amp);
    /// What the shelf shows after the minis: the playing amp's name, or the hovered one's name and description.
    juce::String getShelfText() const;
    /// For tests: the shelf's hover, as the mouse would set it (-1: none).
    void hoverMini (int amp);
    /// An amp's name ("Glass", "Your capture") and description (a built-in's from its gainset.json; your capture's
    /// file name).
    juce::String ampDescription (int amp) const;

    /// The message shown where "48 kHz" sits (empty: the rate).
    void setStatus (const juce::String& message);

    int getShownAmp() const noexcept { return shownSlot; }
    AmpHead& getHead() noexcept { return head; }
    PilotJewel& getJewel() noexcept { return jewel; }
    /// The knobs on the head (in its material's skin).
    Knob& getKnob (int amp, int index) { return *knobs[(size_t) amp][(size_t) index]; }
    juce::Component& getGrille() noexcept;
    /// The component holding the head and its knobs, drawn at the head's designed size and scaled to the stage.
    juce::Component& getHeadAssembly() noexcept;
    float getHeadScale() const noexcept { return headScale; }

    /// The amp's inspector (owned here; the editor's inspector shows it), and what moved there from this page.
    AmpPanel& getPanel() noexcept { return *panel; }
    SpectrumView& getSpectrum() noexcept { return panel->getSpectrum(); }

    /// What the info row shows, for tests: the capture's class ("High gain"), the capture's file as its tooltip names
    /// it ("Monolith (built in, gain set)"), the rate or message, the one short line ("High gain \u00b7 Monolith",
    /// RD4-5), and the row's tooltip.
    juce::String getVoiceText() const;
    juce::String getModelText() const;
    juce::String getRateText() const;
    juce::String getInfoLine() const;
    juce::String getInfoTooltip() const;

    /// The capture's tone type from its NAM metadata ("Clean", "Crunch", "High gain"), or a gain set's
    /// "tone_type", or empty. Never the gear's make or model (no product names in the UI).
    static juce::String toneTypeOf (const juce::File& namFile);

    /// The Gain positions of an amp's gain set's steps (empty for a single capture or an empty amp), read
    /// from its gainset.json once per path.
    const std::vector<float>& gainSteps (int amp);

    /// For tests: the step dots as drawn now (the positions, and which are lit).
    std::vector<float> getShownGainSteps() const;
    std::vector<bool> getLitGainSteps() const;

    /// Where the jewel lamp and each knob (its CSS box's top left) sit on the head, in the head's own coordinates
    /// (RD4-8: the lamp 40 px in, the knobs from just past it; no dead faceplate before Gain).
    static juce::Rectangle<int> jewelBounds();
    static juce::Point<int> knobCssPosition (int index);

    /// The knobs' captions and parameters, in the order across the panel.
    static juce::String knobCaption (int index);
    static juce::String knobParameter (int amp, int index);
    /// A Gain/tone/Master knob shown 0 to 10 with one decimal over its range, with its tooltip.
    static void dressKnob (Knob& knob, int index);

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int numKnobs = 7;

private:
    class InfoRow;
    class Shelf;
    class Grille;
    class GainSteps;
    class HeadAssembly;
    void updateGainSteps();
    void timerCallback() override;
    void showSlot (int amp);

    std::unique_ptr<HeadAssembly> assembly;
    AmpHead head;
    PilotJewel jewel;
    std::unique_ptr<Grille> grille;
    std::unique_ptr<InfoRow> info;
    std::array<std::array<Knob*, numKnobs>, AmpSimProcessor::numAmps> knobs {};
    int shownSlot = -1;
    float headScale = 1.0f;
    float headBottom = 0.0f; // the head's body's bottom edge on the stage, for the pool of light under it
    std::map<juce::String, juce::String> toneTypes; // capture path -> its tone type (read once)
    struct SetInfo
    {
        juce::String name;
        std::vector<float> steps;
    };
    std::map<juce::String, SetInfo> gainSets; // capture path -> its set's name and steps (empty: not a set)
    const SetInfo& setInfo (const juce::String& path);
    std::unique_ptr<GainSteps> gainStepDots;
    std::unique_ptr<Shelf> shelf;
    std::array<juce::String, AmpSimProcessor::numBuiltInAmps> descriptions; // read once from the built-ins' JSON
    bool yourCaptureLoaded() const;
    std::unique_ptr<AmpPanel> panel; // last: it reads this view's shelf and descriptions
};

} // namespace ui

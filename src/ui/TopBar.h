// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Blocks.h"
#include "Controls.h"
#include "Meters.h"
#include "PluginProcessor.h"

namespace ui
{

/// The top bar (the redesign's FIRST VIEWPORT), 52 high on the panel colour with a hairline along its bottom:
///   left    the brand (an orange dot and "BellyDSP", 15 px semibold; a click opens the version, licence,
///           updates, source, and licences menu: ASSUMPTIONS DS9), then A and B, and undo and redo
///   centre  the previous-preset arrow, the preset's name (14 px medium) over its tag ("Factory" or "User") and the
///           current scene ("Scene 2: Lead"), the next arrow, and Save. A click on the name opens the browser.
///   right   the CPU, the In and Out meters, then the buttons that open the tuner (with the note it hears while
///           it's engaged), Match tone, and Settings; the open one is underlined in orange
/// The editor wires the buttons to what they do; tooltips name the keys (the arrows, T, Cmd-Z).
class TopBar final : public juce::Component
{
public:
    explicit TopBar (AmpSimProcessor& processor);
    ~TopBar() override;

    std::function<void()> onPresetMenu, onSave, onPrevious, onNext, onTuner, onBrand, onUndo, onRedo, onMatchTone, onSettings;
    std::function<void (bool b)> onAbSelect;

    /// Message thread: the preset's name and tag, the current scene, A or B.
    void refresh();

    /// The tuner button: open (the overlay shows) and the note it shows ("-" when the tuner isn't engaged).
    void setTuner (bool open, const juce::String& note);
    const juce::String& getTunerNote() const noexcept { return tunerNote; }
    /// Which overlay is open (its button is underlined).
    void setOverlay (Overlay overlay);

    /// Message thread, at the meter rate: the processor's peaks, `seconds` since the last, and the CPU load.
    void updateMeters (const AmpSimProcessor::Peaks& peaks, double seconds);
    void setCpuLoad (float percent) { cpu.setLoad (percent); }

    LevelMeter& getInputMeter() noexcept { return inputMeter; }
    LevelMeter& getOutputMeter() noexcept { return outputMeter; }
    CpuMeter& getCpuMeter() noexcept { return cpu; }
    juce::Button& getPresetButton() noexcept;
    juce::Button& getBrandButton() noexcept;
    juce::Button& getTunerButton() noexcept;
    juce::Button& getMatchToneButton() noexcept;
    juce::Button& getSettingsButton() noexcept;
    juce::Button& getSaveButton() noexcept { return save; }
    juce::Button& getAbButton (bool b) noexcept { return b ? abB : abA; }
    juce::Button& getUndoButton() noexcept { return undo; }
    juce::Button& getRedoButton() noexcept { return redo; }
    juce::Button& getPreviousButton() noexcept { return previous; }
    juce::Button& getNextButton() noexcept { return next; }
    juce::String getShownPresetName() const;
    juce::String getShownTag() const;
    juce::String getShownScene() const;
    /// The preset box's type (RD4-4): the name in 23 px semibold, the scene in 15 px medium ink; the box is 48 high.
    static juce::FontOptions getPresetNameFont();
    static juce::FontOptions getSceneFont();
    static juce::Colour getSceneColour();
    static constexpr int presetBoxHeight = 48;
    /// The loaded preset's notes, shown as the preset box's tooltip (RD4-6; empty: none).
    void setPresetNotes (const juce::String& notes);
    juce::String getPresetNotes() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class BrandButton;
    class PresetButton;
    class BarButton;

    AmpSimProcessor& ampSim;
    std::unique_ptr<BrandButton> brand;
    std::unique_ptr<PresetButton> preset;
    std::unique_ptr<BarButton> tuner, matchTone, settings;
    IconButton previous { "Previous preset", IconButton::Icon::left }, next { "Next preset", IconButton::Icon::right };
    IconButton undo { "Undo", IconButton::Icon::undo }, redo { "Redo", IconButton::Icon::redo };
    juce::TextButton save { "Save" }, abA { "A" }, abB { "B" };
    LevelMeter inputMeter { "In", 1 }, outputMeter { "Out", 2 };
    CpuMeter cpu;
    juce::String tunerNote { "-" };
};

} // namespace ui

#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

/// A component that lays out its children with a function, so tab pages don't need a class each.
class PageComponent final : public juce::Component
{
public:
    std::function<void (juce::Rectangle<int>)> layout;
    void resized() override
    {
        if (layout)
            layout (getLocalBounds());
    }
};

/// A rotary knob with a caption, attached to one parameter. Double-click resets it to the default.
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption,
          const juce::String& suffix = " dB");
    void resized() override;

private:
    juce::Slider slider;
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

/// One amp slot's controls: load a capture, its status, and its seven knobs.
class SlotPanel final : public juce::Component
{
public:
    SlotPanel (AmpSimProcessor& processor, int slotIndex, std::function<void()> onLoad);
    void setActive (bool isActive);
    void setStatus (const juce::String& text, bool isError);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    const int slot;
    bool active = false;
    juce::Label title, status;
    juce::TextButton loadButton { "Load model..." };
    juce::OwnedArray<Knob> knobs;
};

/// Where a close mic sits on its cab pack's map: across the speaker (cap to edge) left to right, and
/// distance (closest capture to farthest) top to bottom. Shows the pack's captured positions; dragging
/// moves the mic by setting its two position parameters, and the processor re-morphs the IR.
class MicPositionPad final : public juce::Component
{
public:
    MicPositionPad (juce::AudioProcessorValueTreeState& state, const juce::String& xParameterId, const juce::String& yParameterId);

    /// The pack's captured positions (empty: no pack, so the pad is inactive).
    void setPoints (std::vector<juce::Point<float>> packPoints);
    /// Repaints if the position parameters moved (automation, a preset, another view).
    void refresh();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> mapArea() const;
    void moveTo (juce::Point<float> position);

    juce::RangedAudioParameter* xParameter;
    juce::RangedAudioParameter* yParameter;
    std::vector<juce::Point<float>> points;
    juce::Point<float> drawnPosition { -1.0f, -1.0f };
    bool dragging = false;
};

/// One cab mic's controls: load an IR (or, for a close mic, a cab pack), its status, the position pad
/// for a pack, and its knobs and switches.
class MicPanel final : public juce::Component
{
public:
    MicPanel (AmpSimProcessor& processor, int micIndex, std::function<void()> onLoad, std::function<void()> onLoadPack);
    void setStatus (const juce::String& text, bool isError);
    void setPackPoints (const std::vector<ampsim::CabPack::Point>& packPoints);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    const int mic;
    juce::Label title, status;
    juce::TextButton loadButton { "Load IR..." }, packButton { "Load pack..." };
    std::unique_ptr<MicPositionPad> pad;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAttachments;
    juce::ComboBox channel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelAttachment;
};

/// A basic panel: header with the slot selector and global levels, then an Amps tab (three slots)
/// and a Cab tab (three mics, alignment, cuts). The real UI comes in Phase 11 (BUILD_PLAN "GUI").
class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// For tests and snapshots: show the Amps (0) or Cab (1) tab, and refresh the status lines now.
    void showTab (int index) { tabs.setCurrentTabIndex (index); }
    void refresh() { timerCallback(); }

private:
    void timerCallback() override;
    void chooseFile (const juce::String& title, const juce::String& patterns, const juce::Identifier& lastPathKey,
                     std::function<void (const juce::File&)> onChosen, bool folders = false);

    AmpSimProcessor& ampSim;

    std::array<juce::TextButton, AmpSimProcessor::numAmpSlots> slotButtons;
    Knob inputKnob, outputKnob;
    juce::Label warningLabel;

    PageComponent ampsPage, cabPage;
    juce::OwnedArray<SlotPanel> slotPanels;
    juce::OwnedArray<MicPanel> micPanels;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    // Cab page, global section.
    juce::ToggleButton alignButton { "Auto-align close mics" }, lowCutButton { "Low cut" }, highCutButton { "High cut" },
        cabBypassButton { "Bypass cab" };
    juce::Label alignmentLabel;
    Knob lowCutKnob, highCutKnob;
    juce::ComboBox lowCutSlope, highCutSlope;
    juce::AudioProcessorValueTreeState::ButtonAttachment alignAttachment, lowCutAttachment, highCutAttachment, cabBypassAttachment;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment lowCutSlopeAttachment, highCutSlopeAttachment;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

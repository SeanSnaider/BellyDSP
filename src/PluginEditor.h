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

/// A rotary knob with a caption, attached to one parameter.
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption);
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

/// A basic panel: header with the slot selector and global levels, then an Amps tab (three slots)
/// and a Cab tab. The real UI comes in Phase 11 (BUILD_PLAN "GUI").
class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void chooseFile (const juce::String& title, const juce::String& patterns, const juce::Identifier& lastPathKey,
                     std::function<void (const juce::File&)> onChosen);

    AmpSimProcessor& ampSim;

    std::array<juce::TextButton, AmpSimProcessor::numAmpSlots> slotButtons;
    Knob inputKnob, outputKnob;
    juce::Label warningLabel;

    PageComponent ampsPage, cabPage;
    juce::OwnedArray<SlotPanel> slotPanels;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    juce::TextButton loadIRButton { "Load cab IR..." };
    juce::Label irLabel;
    juce::ToggleButton cabBypassButton { "Bypass cab" };
    juce::AudioProcessorValueTreeState::ButtonAttachment cabBypassAttachment;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

/// A basic panel: load a capture and an IR, two level knobs, cab bypass, and status lines. The real
/// UI comes in Phase 11 (BUILD_PLAN "GUI").
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

    juce::TextButton loadModelButton { "Load amp model..." };
    juce::TextButton loadIRButton { "Load cab IR..." };
    juce::Label modelLabel, irLabel, warningLabel;

    juce::Slider inputGainKnob, outputGainKnob;
    juce::Label inputGainCaption { {}, "Input Gain" }, outputGainCaption { {}, "Output Level" };
    juce::ToggleButton cabBypassButton { "Bypass cab" };

    juce::AudioProcessorValueTreeState::SliderAttachment inputGainAttachment, outputGainAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment cabBypassAttachment;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

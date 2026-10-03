#pragma once

#include "Controls.h"
#include "Meters.h"
#include "PluginProcessor.h"

namespace ui
{

/// The top bar (UI_DESIGN "Layout"): the preset's name (a click opens the browser), Save, A/B with Copy,
/// Undo, Redo, MIDI mappings, the Tuner, the tempo with Tap, the input and output meters, the CPU meter,
/// and the view settings (UI scale). The editor wires the buttons to what they do.
class TopBar final : public juce::Component
{
public:
    explicit TopBar (AmpSimProcessor& processor);
    ~TopBar() override;

    std::function<void()> onPresetMenu, onSave, onUndo, onRedo, onAbCopy, onMidi, onSettings, onTap;
    std::function<void (bool b)> onAbSelect; // false: A, true: B

    /// Message thread: the preset's name, Undo and Redo, and which of A and B is showing.
    void refresh();

    /// Message thread, at the meter rate: the processor's peaks and CPU load, `seconds` since the last.
    void updateMeters (const AmpSimProcessor::Peaks& peaks, float cpuPercent, double seconds);

    LevelMeter& getInputMeter() noexcept { return inputMeter; }
    LevelMeter& getOutputMeter() noexcept { return outputMeter; }
    CpuMeter& getCpuMeter() noexcept { return cpu; }
    juce::Button& getPresetButton() noexcept;
    juce::Button& getTunerButton() noexcept { return tuner; }
    juce::Button& getSettingsButton() noexcept { return settings; }
    juce::Button& getMidiButton() noexcept { return midi; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PresetButton;

    AmpSimProcessor& ampSim;
    std::unique_ptr<PresetButton> preset;
    juce::TextButton save { "Save" }, abA { "A" }, abB { "B" }, tap { "Tap" };
    IconButton abCopy { "Copy", IconButton::Icon::copy }, undo { "Undo", IconButton::Icon::undo }, redo { "Redo", IconButton::Icon::redo },
        midi { "MIDI", IconButton::Icon::midi }, settings { "Settings", IconButton::Icon::settings };
    IgnoresRightClick<juce::TextButton> tuner { "Tuner" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tunerAttachment;
    ValueField tempo;
    LevelMeter inputMeter { "IN", 1 }, outputMeter { "OUT", 2 };
    CpuMeter cpu;
};

} // namespace ui

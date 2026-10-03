#pragma once

#include "Controls.h"
#include "Meters.h"
#include "PluginProcessor.h"

namespace ui
{

/// The top bar (handoff 4.3), 56 high with a 1 px line along the bottom, 24 px in from each side, 20 px
/// between its three parts:
///   left    the brand: a 7 px emerald dot and "rig", 15 px semibold, in a 136 px column
///   centre  the previous-preset arrow, the preset's name (14 px medium) with its tag ("Factory" or
///           "User", 12 px faint) in a 300 x 32 box, the next arrow, and Save (outlined)
///   right   the Tuner button (the note it hears in emerald while the tuner is engaged, "-" otherwise;
///           a 1 px emerald underline while the tuner page is open), then the In and Out meters
/// A click on the name opens the preset browser; the arrows step through the presets (UH6). A click on the
/// brand opens a small menu with the version, "Check for updates...", and the licences (ASSUMPTIONS DS9).
/// The editor wires the buttons to what they do.
class TopBar final : public juce::Component
{
public:
    explicit TopBar (AmpSimProcessor& processor);
    ~TopBar() override;

    std::function<void()> onPresetMenu, onSave, onPrevious, onNext, onTuner, onBrand;

    /// Message thread: the preset's name and tag.
    void refresh();

    /// The tuner button: open (the page shows) and the note it shows ("-" when the tuner isn't engaged).
    void setTuner (bool pageOpen, const juce::String& note);
    const juce::String& getTunerNote() const noexcept { return tunerNote; }

    /// Message thread, at the meter rate: the processor's peaks, `seconds` since the last.
    void updateMeters (const AmpSimProcessor::Peaks& peaks, double seconds);

    LevelMeter& getInputMeter() noexcept { return inputMeter; }
    LevelMeter& getOutputMeter() noexcept { return outputMeter; }
    juce::Button& getPresetButton() noexcept;
    juce::Button& getBrandButton() noexcept;
    juce::Button& getTunerButton() noexcept;
    juce::Button& getSaveButton() noexcept { return save; }
    juce::String getShownPresetName() const;
    juce::String getShownTag() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class BrandButton;
    class PresetButton;
    class TunerButton;

    AmpSimProcessor& ampSim;
    std::unique_ptr<BrandButton> brand;
    std::unique_ptr<PresetButton> preset;
    std::unique_ptr<TunerButton> tuner;
    IconButton previous { "Previous preset", IconButton::Icon::left }, next { "Next preset", IconButton::Icon::right };
    juce::TextButton save { "Save" };
    LevelMeter inputMeter { "In", 1 }, outputMeter { "Out", 2 };
    juce::String tunerNote { "-" };
};

} // namespace ui

#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

/// The tuner (BUILD_PLAN "Tuner", Display; UI_DESIGN "Layout", Tuner): a full-window overlay while the
/// tuner is engaged, closed by its own button or the footswitch. The note in the huge style, the
/// frequency and cents, and a needle or a strobe. Needle mode draws the smoothed cents on a -50 to +50
/// scale; strobe mode moves a striped band at a speed proportional to the offset (the analysis integrates
/// the phase), standing still when in tune. Colours: green within 1 cent, amber within 5, red beyond,
/// grey while holding a decayed note. It reads the processor's tuner 60 times a second while showing.
class TunerView final : public juce::Component, private juce::Timer
{
public:
    explicit TunerView (AmpSimProcessor& processor);
    ~TunerView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

    /// Needle or strobe, remembered in the app's state.
    void setStrobe (bool shouldShowStrobe);
    bool isStrobe() const noexcept { return strobe; }

    /// For tests and snapshots: draw this reading from now on, instead of the live one.
    void freeze (const ampsim::TunerReading& r)
    {
        frozen = true;
        reading = r;
        repaint();
    }

private:
    void timerCallback() override;
    void paintNeedle (juce::Graphics&, juce::Rectangle<float> area, juce::Colour colour) const;
    void paintStrobe (juce::Graphics&, juce::Rectangle<float> area, juce::Colour colour) const;

    AmpSimProcessor& ampSim;
    ampsim::TunerReading reading;
    bool strobe = false, frozen = false;
    juce::TextButton needleButton { "Needle" }, strobeButton { "Strobe" }, closeButton { "Close tuner" };
    ui::Switch muteButton { "Mute while tuning" };
    ui::Knob a4Knob;
    juce::AudioProcessorValueTreeState::ButtonAttachment muteAttachment;
    juce::Rectangle<int> displayArea, footerArea;
};

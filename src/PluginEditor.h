#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

/// A control that leaves right-clicks (and ctrl-clicks) to the editor's MIDI learn menu. Without this
/// a JUCE button flips on any click, and a linear slider jumps to where it was clicked.
template <typename Control>
class IgnoresRightClick : public Control
{
public:
    using Control::Control;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Control::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Control::mouseDrag (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Control::mouseUp (e);
    }
};
using ToggleControl = IgnoresRightClick<juce::ToggleButton>;
using SliderControl = IgnoresRightClick<juce::Slider>;

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
    SliderControl slider;
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
    juce::OwnedArray<ToggleControl> toggles;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAttachments;
    juce::ComboBox channel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelAttachment;
};

/// A section's order: its blocks left to right, each with buttons to move it earlier or later.
class OrderStrip final : public juce::Component
{
public:
    OrderStrip (AmpSimProcessor& processor, ampsim::Chain::Section section);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void move (int position, int by);

    AmpSimProcessor& ampSim;
    const ampsim::Chain::Section section;
    juce::StringArray shown;
    juce::OwnedArray<juce::Label> names;
    juce::OwnedArray<juce::TextButton> earlier, later;
};

/// One compressor's controls, with its gain reduction meter.
class CompressorPanel final : public juce::Component
{
public:
    CompressorPanel (AmpSimProcessor& processor, const juce::String& prefix, const juce::String& title, bool isPost);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AmpSimProcessor& ampSim;
    const bool post;
    juce::Label titleLabel;
    ToggleControl onButton { "On" }, autoReleaseButton { "Auto release" }, autoMakeupButton { "Auto makeup" }, sidechainButton { "Sidechain HPF" };
    juce::ComboBox mode, detector;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachments;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment, detectorAttachment;
    float reductionDb = 0.0f;
    juce::Rectangle<int> meterArea;
};

/// The EQ's response curve, computed from the current parameter values (Equalizer::responseDb).
class EqCurve final : public juce::Component, private juce::Timer
{
public:
    EqCurve (juce::AudioProcessorValueTreeState& state, const juce::String& prefix);
    void paint (juce::Graphics&) override;

    /// Recomputes and repaints if the settings changed (its own 30 Hz timer calls this too).
    void refresh();

private:
    void timerCallback() override { refresh(); }

    params::EqualizerParameters values;
    std::vector<double> frequencies, curve;
    ampsim::Equalizer::Settings drawn;
    bool everDrawn = false;
};

/// One EQ's controls: on, mode, the curve, nine graphic sliders or five parametric bands, and cuts.
class EqualizerPanel final : public juce::Component
{
public:
    EqualizerPanel (AmpSimProcessor& processor, const juce::String& prefix, const juce::String& title);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct BandControls
    {
        juce::Label title;
        juce::ComboBox type;
        SliderControl frequency { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
        SliderControl gain { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
        SliderControl q { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> frequencyAttachment, gainAttachment, qAttachment;

        std::vector<juce::Component*> components() { return { &title, &type, &frequency, &gain, &q }; }
    };

    juce::AudioProcessorValueTreeState& state;
    const juce::String prefix;
    juce::Label titleLabel;
    ToggleControl onButton { "On" }, lowCutButton { "Low cut" }, highCutButton { "High cut" };
    juce::ComboBox mode, lowCutSlope, highCutSlope;
    EqCurve curve;
    juce::OwnedArray<SliderControl> sliders;
    juce::OwnedArray<juce::Label> sliderLabels;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachments;
    juce::OwnedArray<BandControls> bands;
    juce::OwnedArray<juce::Label> bandHeadings; // Type, Frequency, Gain, Q above the band rows
    std::unique_ptr<Knob> lowCutKnob, highCutKnob;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachments;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachments;
    bool parametricShown = false;
};

/// A small helper for the effect panels: knobs in a grid, toggles, and combo boxes bound to parameters.
class EffectPanel : public juce::Component
{
public:
    EffectPanel (AmpSimProcessor& processor, const juce::String& title, const juce::String& onParameterId);
    void paint (juce::Graphics&) override;

protected:
    Knob& addKnob (const juce::String& id, const juce::String& caption, const juce::String& suffix = " dB");
    ToggleControl& addToggle (const juce::String& id, const juce::String& text);
    juce::ComboBox& addCombo (const juce::String& id, const juce::StringArray& items);
    /// Lays out the title row and returns the area below it.
    juce::Rectangle<int> layoutTitle();

    AmpSimProcessor& ampSim;
    juce::Label titleLabel;
    ToggleControl onButton { "On" };
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ToggleControl> toggles;
    juce::OwnedArray<juce::ComboBox> combos;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachments;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachments;
};

/// The chorus's controls.
class ChorusPanel final : public EffectPanel
{
public:
    explicit ChorusPanel (AmpSimProcessor& processor);
    void refresh(); // rate knob or note, depending on sync
    void resized() override;

private:
    juce::ComboBox *mode = nullptr, *shape = nullptr, *note = nullptr;
    ToggleControl *sync = nullptr, *analog = nullptr, *noise = nullptr, *highPass = nullptr;
    Knob *rate = nullptr, *depth = nullptr, *mix = nullptr, *width = nullptr, *highPassHz = nullptr;
    int shownSync = -1;
};

/// The reverb's controls, with freeze.
class ReverbPanel final : public EffectPanel
{
public:
    explicit ReverbPanel (AmpSimProcessor& processor);
    void refresh(); // pre-delay knob or note, depending on sync
    void resized() override;

private:
    juce::ComboBox *engine = nullptr, *preDelayNote = nullptr;
    ToggleControl *freeze = nullptr, *preDelaySync = nullptr;
    std::vector<Knob*> grid;
    Knob* preDelay = nullptr;
    int shownSync = -1;
};

/// One noise gate's controls with its detector meter. Gate A's panel has Learn; Gate B's has the link
/// switch, and while linked its own knobs are dimmed (it applies Gate A's decision).
class GatePanel final : public EffectPanel
{
public:
    GatePanel (AmpSimProcessor& processor, bool isGateB);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    const bool gateB;
    juce::ComboBox *releaseMode = nullptr, *detector = nullptr;
    ToggleControl *sidechain = nullptr, *link = nullptr;
    juce::TextButton learnButton { "Learn" };
    AmpSimProcessor::GateMeter meter;
    bool linked = false, learning = false;
    float learnProgress = 0.0f;
    juce::Rectangle<int> meterArea;
};

/// The delay's controls, plus the tempo and its tap button.
class DelayPanel final : public juce::Component
{
public:
    explicit DelayPanel (AmpSimProcessor& processor);
    void refresh(); // shows note or time controls (sync), right-side or offset controls (layout)
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AmpSimProcessor& ampSim;
    juce::Label titleLabel;
    ToggleControl onButton { "On" }, syncButton { "Sync to tempo" };
    juce::ComboBox mode, stereo, note, rightNote;
    juce::Label noteLabel, rightNoteLabel;
    juce::TextButton tapButton { "Tap" };
    std::unique_ptr<Knob> time, rightTime, offset, feedback, lowCut, highCut, modDepth, modRate, duck, mix, tempo;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachments;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachments;
    int shownState = -1;
};

/// A basic panel: header with the slot selector and global levels, then tabs: Amps (three slots),
/// Cab (three mics, alignment, cuts), Pre FX and Post FX (order, compressor, EQ). The real UI comes in
/// Phase 11 (BUILD_PLAN "GUI").
class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// For tests and snapshots: show the Amps (0), Cab (1), Gates & Drive (2), Pre FX (3), Post FX (4), or
    /// Time FX (5) tab, and refresh the status lines now.
    void showTab (int index) { tabs.setCurrentTabIndex (index); }
    void refresh() { timerCallback(); }

    /// A right-click on any control attached to a parameter: MIDI learn for it, or change or forget its
    /// mappings (BUILD_PLAN "MIDI control": click a control, press a switch, done). The editor listens
    /// to every child's mouse events for this; the controls themselves ignore right-clicks.
    void mouseDown (const juce::MouseEvent& e) override;

    /// For tests: the menu a right-click on this parameter's control would show.
    juce::PopupMenu midiMenuFor (const juce::String& parameterId);

private:
    void timerCallback() override;
    void chooseFile (const juce::String& title, const juce::String& patterns, const juce::Identifier& lastPathKey,
                     std::function<void (const juce::File&)> onChosen, bool folders = false);

    AmpSimProcessor& ampSim;

    void savePreset();
    void loadPreset();
    void showMidiMappings();

    std::array<juce::TextButton, AmpSimProcessor::numAmpSlots> slotButtons;
    juce::Label presetLabel;
    juce::TextButton savePresetButton { "Save..." }, loadPresetButton { "Load..." }, midiButton { "MIDI..." };
    juce::String presetMessage;
    Knob inputKnob, outputKnob;
    juce::Label warningLabel;

    PageComponent ampsPage, cabPage, gatesPage, preFxPage, postFxPage, timeFxPage;
    std::unique_ptr<GatePanel> gateAPanel, gateBPanel;
    std::unique_ptr<DelayPanel> delayPanel;
    std::unique_ptr<ChorusPanel> chorusPanel;
    std::unique_ptr<ReverbPanel> reverbPanel;
    juce::OwnedArray<SlotPanel> slotPanels;
    std::unique_ptr<OrderStrip> preOrder, postOrder;
    std::unique_ptr<CompressorPanel> preComp, postComp;
    std::unique_ptr<EqualizerPanel> preEq, postEq;

    // Amps page, input calibration row.
    ToggleControl calibrateButton { "Calibrate input to each capture" };
    juce::Label interfaceLevelLabel, calibrationHint;
    SliderControl interfaceLevel { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::AudioProcessorValueTreeState::ButtonAttachment calibrateAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment interfaceLevelAttachment;
    juce::OwnedArray<MicPanel> micPanels;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    // Cab page, global section.
    ToggleControl alignButton { "Auto-align close mics" }, lowCutButton { "Low cut" }, highCutButton { "High cut" },
        cabBypassButton { "Bypass cab" };
    juce::Label alignmentLabel;
    Knob lowCutKnob, highCutKnob;
    juce::ComboBox lowCutSlope, highCutSlope;
    juce::AudioProcessorValueTreeState::ButtonAttachment alignAttachment, lowCutAttachment, highCutAttachment, cabBypassAttachment;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment lowCutSlopeAttachment, highCutSlopeAttachment;

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

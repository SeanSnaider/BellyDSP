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
    /// A row of block names with arrows that move each one earlier or later: a chain section's order, or
    /// the order inside Bloom.
    OrderStrip (const juce::String& title, std::function<juce::StringArray()> getOrder, std::function<void (const juce::StringArray&)> setOrder);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void move (int position, int by);

    const juce::String title;
    std::function<juce::StringArray()> getOrder;
    std::function<void (const juce::StringArray&)> setOrder;
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
    juce::ComboBox *engine = nullptr, *preDelayNote = nullptr, *shimmerInterval = nullptr;
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

/// The boost's controls; the knobs the current mode doesn't use are dimmed.
class BoostPanel final : public EffectPanel
{
public:
    explicit BoostPanel (AmpSimProcessor& processor);
    void refresh();
    void resized() override;

private:
    juce::ComboBox* mode = nullptr;
    Knob *level = nullptr, *tilt = nullptr, *tightHz = nullptr, *mid = nullptr;
    int shownMode = -1;
};

/// The overdrive's controls, with the drive blocks' oversampling (a global setting).
class OverdrivePanel final : public EffectPanel
{
public:
    explicit OverdrivePanel (AmpSimProcessor& processor);
    void refresh();
    void resized() override;

private:
    juce::ComboBox *mode = nullptr, *oversampling = nullptr;
    ToggleControl* tight = nullptr;
    Knob *drive = nullptr, *tone = nullptr, *level = nullptr, *mix = nullptr, *tightHz = nullptr;
    int shownTight = -1;
};

/// Bloom's bitcrusher.
class BitcrushPanel final : public EffectPanel
{
public:
    explicit BitcrushPanel (AmpSimProcessor& processor);
    void resized() override;

private:
    ToggleControl* dither = nullptr;
};

/// Bloom's phaser: the controls the mode doesn't use are dimmed, and the rate becomes a note when synced.
class PhaserPanel final : public EffectPanel
{
public:
    explicit PhaserPanel (AmpSimProcessor& processor);
    void refresh();
    void resized() override;

private:
    juce::ComboBox *mode = nullptr, *stages = nullptr, *shape = nullptr, *note = nullptr;
    ToggleControl *sync = nullptr, *classicFeedback = nullptr;
    Knob *rate = nullptr, *depth = nullptr, *low = nullptr, *high = nullptr, *feedback = nullptr, *stereo = nullptr, *mix = nullptr;
    int shownMode = -1, shownSync = -1;
};

/// Bloom's flanger: the rate becomes a note when synced; through-zero adds 5 ms of latency while it's on.
class FlangerPanel final : public EffectPanel
{
public:
    explicit FlangerPanel (AmpSimProcessor& processor);
    void refresh();
    void resized() override;

private:
    juce::ComboBox *shape = nullptr, *note = nullptr;
    ToggleControl *sync = nullptr, *negative = nullptr, *throughZero = nullptr;
    Knob *manual = nullptr, *depth = nullptr, *rate = nullptr, *feedback = nullptr, *stereo = nullptr, *mix = nullptr;
    int shownSync = -1;
};

/// The multivoicer: engine, voice count, mix, spread, wet high-pass, a starting-point menu, and a row of
/// controls per voice (rows past the voice count are dimmed).
class MultivoicerPanel final : public EffectPanel
{
public:
    explicit MultivoicerPanel (AmpSimProcessor& processor);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::ComboBox *engine = nullptr;
    ToggleControl* highPass = nullptr;
    juce::TextButton startingPoints { "Starting points..." };
    struct Row
    {
        juce::Label label;
        std::array<SliderControl, 6> sliders;
    };
    std::array<Row, ampsim::Multivoicer::maxVoices> rows;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> rowAttachments;
    int shownVoices = -1;
    juce::Rectangle<int> headerArea;
};

/// The harmonizer: key, scale (with a 12-note custom scale), the out-of-key rule, glide, lowest note,
/// level, a row per voice, and what it hears now.
class HarmonizerPanel final : public EffectPanel
{
public:
    explicit HarmonizerPanel (AmpSimProcessor& processor);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::ComboBox *root = nullptr, *scale = nullptr, *outOfKey = nullptr, *floor = nullptr;
    std::array<juce::TextButton, 12> customNotes;
    struct Row
    {
        ToggleControl on;
        juce::ComboBox mode;
        std::array<SliderControl, 6> sliders; // steps, semitones, octave, level, pan, humanize
    };
    std::array<Row, ampsim::Harmonizer::maxVoices> rows;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachments;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rowCombos;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> rowButtons;
    juce::String hearing;
    int shownMask = -1, shownScale = -1;
    juce::Rectangle<int> statusArea, headerArea;
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
/// The tuner's display (BUILD_PLAN "Tuner", Display), shown over the tabs while the tuner is engaged.
/// Needle mode draws the smoothed cents on a -50 to +50 scale; strobe mode moves a striped band at a speed
/// proportional to the offset (the analysis integrates the phase), standing still when in tune. It reads
/// the processor's tuner 60 times a second while it's showing.
class TunerView final : public juce::Component, private juce::Timer
{
public:
    explicit TunerView (AmpSimProcessor& processor);
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
    ToggleControl muteButton { "Mute while tuning" };
    Knob a4Knob;
    juce::AudioProcessorValueTreeState::ButtonAttachment muteAttachment;
    juce::Rectangle<int> displayArea;
};

class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// For tests and snapshots: show the Amps (0), Cab (1), Gates & Drive (2), Pre FX (3), Post FX (4),
    /// Pitch (5), Bloom (6), or Time FX (7) tab, and refresh the status lines now.
    void showTab (int index) { tabs.setCurrentTabIndex (index); }
    void refresh() { timerCallback(); }

    /// A right-click on any control attached to a parameter: MIDI learn for it, or change or forget its
    /// mappings (BUILD_PLAN "MIDI control": click a control, press a switch, done). The editor listens
    /// to every child's mouse events for this; the controls themselves ignore right-clicks.
    void mouseDown (const juce::MouseEvent& e) override;

    /// For tests: the menu a right-click on this parameter's control would show.
    juce::PopupMenu midiMenuFor (const juce::String& parameterId);

    /// For tests and snapshots.
    TunerView& getTunerView() noexcept { return tunerView; }

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
    IgnoresRightClick<juce::TextButton> tunerButton { "Tuner" };

    // Scenes: click one to recall it (or to store the current sound in an empty one); Store, then a scene,
    // overwrites it; right-click a scene to store over it or clear it.
    juce::Label scenesLabel;
    std::array<IgnoresRightClick<juce::TextButton>, Scenes::count> sceneButtons;
    juce::TextButton storeSceneButton { "Store" };
    void clickScene (int index);
    void sceneMenu (int index);
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tunerAttachment;
    juce::String presetMessage;
    Knob inputKnob, outputKnob;
    juce::Label warningLabel;

    PageComponent ampsPage, cabPage, gatesPage, preFxPage, postFxPage, pitchPage, bloomPage, timeFxPage;
    std::unique_ptr<MultivoicerPanel> multivoicerPanel;
    std::unique_ptr<HarmonizerPanel> harmonizerPanel;
    std::unique_ptr<OrderStrip> bloomOrder;
    std::unique_ptr<BitcrushPanel> bitcrushPanel;
    std::unique_ptr<PhaserPanel> phaserPanel;
    std::unique_ptr<FlangerPanel> flangerPanel;
    ToggleControl bloomOnButton { "Bloom on" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bloomOnAttachment;
    std::unique_ptr<Knob> bloomMixKnob;
    juce::Label bloomLatencyLabel;
    std::unique_ptr<GatePanel> gateAPanel, gateBPanel;
    std::unique_ptr<BoostPanel> boostPanel;
    std::unique_ptr<OverdrivePanel> overdrivePanel;
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
    TunerView tunerView { ampSim };

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

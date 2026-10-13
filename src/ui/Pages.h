// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Analyzer.h"
#include "Blocks.h"
#include "Controls.h"
#include "Meters.h"
#include "PluginProcessor.h"

#include <array>
#include <memory>
#include <vector>

namespace ui
{

/// A component that builds and owns controls attached to parameters: knobs, switches, combo boxes,
/// fields, and faders, every one tagged with its parameter for MIDI learn. Pages, the inspector's panels,
/// and the cards inside them are made of these.
class ControlGroup : public juce::Component
{
public:
    explicit ControlGroup (AmpSimProcessor& processor);
    ~ControlGroup() override;

    /// Ten times a second while showing: status lines, meters, and what a mode dims or hides.
    virtual void refresh() {}

    /// The page became the visible one, or stopped being it (pages with their own timers start and stop them).
    virtual void pageShown() {}
    virtual void pageHidden() {}

    /// The height this needs at a width (the inspector sizes its content with it and scrolls past the rest).
    virtual int preferredHeight (int /*width*/) { return getHeight(); }

    AmpSimProcessor& getProcessor() noexcept { return ampSim; }

protected:
    Knob& addKnob (const juce::String& parameterId, const juce::String& caption, const juce::String& suffix = " dB",
                   Knob::Size size = Knob::Size::normal);
    Switch& addSwitch (const juce::String& parameterId, const juce::String& text);
    juce::ComboBox& addCombo (const juce::String& parameterId, const juce::StringArray& items);
    ValueField& addField (const juce::String& parameterId, const juce::String& suffix);
    Fader& addFader (const juce::String& parameterId, const juce::String& caption, const juce::String& suffix);
    juce::TextButton& addButton (const juce::String& text, std::function<void()> onClick);
    juce::Label& addLabel (const juce::String& text, theme::Text style = theme::Text::label, juce::Colour colour = theme::textDim);

    /// Takes ownership of a component made here and shows it.
    template <typename Child>
    Child& adopt (std::unique_ptr<Child> child)
    {
        auto& c = *child;
        addAndMakeVisible (c);
        owned.add (child.release());
        return c;
    }

    /// A small uppercase heading over a group of controls, drawn in this area (call from a layout).
    void heading (juce::Rectangle<int> area, const juce::String& title);
    void clearHeadings()
    {
        headings.clear();
        cards.clear();
    }
    void paintHeadings (juce::Graphics&) const;

    /// One control in a card: knobs and switches take their preferred size, anything else the size given.
    struct CardItem
    {
        juce::Component* component = nullptr;
        int width = 0, height = 0;
    };
    struct CardSpec
    {
        juce::String title;
        std::vector<CardItem> items;
        int extraWidth = 0; // room for something the page draws in the card itself
    };

    /// A card (a hairline panel with its heading inside), drawn with the headings. Returns the area inside it,
    /// under the heading.
    juce::Rectangle<int> card (juce::Rectangle<int> area, const juce::String& title);

    /// Cards across a row: each as wide as its controls need plus a share of the row's spare width, with
    /// its controls centred in it as one block. Returns each card's inside area.
    std::vector<juce::Rectangle<int>> layoutCards (juce::Rectangle<int> row, const std::vector<CardSpec>& specs, int gap = theme::space::m);

    /// A card's height for contents this tall (its padding and heading included).
    static constexpr int cardPadding = 12, cardHeading = 22;
    static int cardHeight (int contentHeight) { return 2 * cardPadding + cardHeading + contentHeight; }
    static int knobCardHeight (Knob::Size size = Knob::Size::normal) { return cardHeight (Knob::preferredHeight (size)); }

    AmpSimProcessor& ampSim;
    juce::AudioProcessorValueTreeState& state;

private:
    juce::OwnedArray<juce::Component> owned;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachments;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachments;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> headings, cards;
};

// ---- The inspector's column ------------------------------------------------------------------------

/// The inspector's layout (the redesign: the selected block's every control in a 300 px column on the right):
/// sections, each a small heading and a hairline over rows of controls, in one grid. Knobs go three to a row,
/// choices sit on a row of their own with their name on the left, switches flow left to right, fields sit
/// under their names three to a row. A page describes its controls once, in order (BlockPage::describe); the
/// same description measures the column's height (place = false) and then places the controls (place = true),
/// so the two always agree.
class InspectorLayout
{
public:
    InspectorLayout (juce::Rectangle<int> area, bool place);

    /// A heading over what follows, with a gap above it (none above the first thing in the column); with `right`,
    /// a control at the heading's right end (a voice's switch).
    void section (const juce::String& title, juce::Component* right = nullptr);
    /// Knobs in rows of three, each centred in its third of the width; a nullptr leaves its cell empty.
    void knobs (const std::vector<Knob*>& knobs);
    /// A control on a row of its own with its name on the left (a choice, a field).
    void labelled (const juce::String& caption, juce::Component* control, int height = theme::controlHeight);
    /// A control across the whole row.
    void row (juce::Component* control, int height = theme::controlHeight);
    /// Switches left to right at their own widths, wrapping onto more rows as needed.
    void switches (const std::vector<Switch*>& switches);
    /// Buttons sharing a row in equal widths (nullptrs skipped).
    void buttons (const std::vector<juce::Component*>& buttons, int height = theme::controlHeight);
    /// Fields under their names, `perRow` to a row.
    void fields (const std::vector<std::pair<juce::String, juce::Component*>>& captioned, int perRow = 3);
    /// An area the page paints (a meter, a curve), across the row; with `right`, a control at the row's right
    /// end, `rightWidth` wide, and the area left of it.
    void area (juce::Rectangle<int>* target, int height, juce::Component* right = nullptr, int rightWidth = 0);
    /// A control on top of a knob's cell (a synced note where the rate knob is): call after placing the knob.
    void inCell (juce::Component* control, Knob* knob, int height = theme::controlHeight);
    /// Text in the page's own words (what a mode does), wrapping; `lines` is the room it gets.
    void note (const juce::String& words, int lines = 2);
    void gap (int pixels) { y += pixels; }

    int getBottom() const noexcept { return y; }
    int getWidth() const noexcept { return bounds.getWidth(); }
    bool isPlacing() const noexcept { return placing; }

    /// What the page paints for the layout: the headings, the captions, the notes.
    struct Text
    {
        enum class Kind
        {
            heading,
            caption,
            fieldCaption,
            cellCaption,
            note
        };
        juce::Rectangle<int> area;
        juce::String words;
        Kind kind;
    };
    const std::vector<Text>& getTexts() const noexcept { return texts; }
    static void paintTexts (juce::Graphics& g, const std::vector<Text>& texts);

    static constexpr int sectionGap = 22, headingHeight = 16, headingGap = 10, rowGap = 8, captionWidth = 92;

private:
    void begin(); // the gap before a row (not before the first)
    void set (juce::Component* c, juce::Rectangle<int> r) const;

    juce::Rectangle<int> bounds;
    const bool placing;
    int y = 0;
    bool first = true, afterHeading = false;
    std::vector<Text> texts;
};

/// One block's controls in the inspector (every pedal's: the pre and post blocks and Gate B): a header (the
/// block's name, where it sits, and its switch) over its controls in sections (InspectorLayout), all built from
/// the shared components and attached to their parameters. A page exists all the time; the inspector shows the
/// selected one, and only it is refreshed.
class BlockPage : public ControlGroup
{
public:
    BlockPage (AmpSimProcessor& processor, BlockId id, juce::String title, juce::String subtitle);
    ~BlockPage() override;

    BlockId getBlockId() const noexcept { return id; }
    const juce::String& getTitle() const noexcept { return title; }

    int preferredHeight (int width) override;
    void paint (juce::Graphics&) override;
    void resized() override;

    /// Gives every parameter whose ID starts with `prefix` and that has no control on this page yet a generic one
    /// built from its type (a switch, a choice, or a knob, named after the parameter), in a "More" section at the
    /// end: a parameter added to the block later is never out of reach. Returns how many it added.
    int coverParameters (const juce::String& prefix, const juce::StringArray& except = {});
    int getGenericCount() const noexcept { return (int) generic.size(); }

    /// The column's padding and the header's height.
    static constexpr int padding = 16;
    static int headerHeight (const juce::String& subtitle, int width);

protected:
    /// The block's controls, in order, as the column shows them.
    virtual void describe (InspectorLayout&) = 0;
    virtual void paintContent (juce::Graphics&) {}

    Switch* onSwitch = nullptr; // the block's own switch, in the header, when it has one

private:
    void describeAll (InspectorLayout&);

    const BlockId id;
    const juce::String title, subtitle;
    std::vector<InspectorLayout::Text> texts;
    std::vector<std::pair<juce::String, juce::Component*>> generic; // coverParameters()'s controls, with their names
};

// ---- Layout helpers ----------------------------------------------------------------------------------

/// Places knobs left to right at their preferred size, centred vertically in the row; nullptr leaves a
/// gap. Returns what's left of the row.
juce::Rectangle<int> placeKnobs (juce::Rectangle<int> row, const std::vector<Knob*>& knobs, Knob::Size size, int gap = 4);

/// The knob size that fits `count` knobs across `width`.
Knob::Size knobSizeFor (int width, int count, int gap = 4);

/// Width of a knob row.
int knobRowWidth (int count, Knob::Size size, int gap = 4);

/// Sets a component's bounds from the left of a row, centred vertically; the row loses that width and
/// `gapAfter` (nullptr just leaves the space).
void place (juce::Component* c, juce::Rectangle<int>& row, int width, int height = theme::controlHeight, int gapAfter = theme::space::s);

/// A switch at its preferred width.
void place (Switch* s, juce::Rectangle<int>& row, int gapAfter = theme::space::l);

/// Dims a control the current mode doesn't use (it stays editable).
void dim (juce::Component* c, bool used);

// ---- Pages --------------------------------------------------------------------------------------------

/// A noise gate: its knobs, release mode, detector, sidechain filter, the detector's level against the
/// open and close thresholds, and the gain reduction. Gate A has Learn; Gate B the link, and while linked
/// its own settings are dimmed (it applies Gate A's decision).
class GatePage final : public BlockPage
{
public:
    GatePage (AmpSimProcessor& processor, bool isGateB);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    void paintContent (juce::Graphics&) override;

    const bool gateB;
    std::vector<Knob*> knobs;
    juce::ComboBox *releaseMode = nullptr, *detector = nullptr;
    Switch *sidechain = nullptr, *link = nullptr;
    juce::TextButton* learn = nullptr;
    ReductionMeter* reduction = nullptr;
    AmpSimProcessor::GateMeter meter;
    bool linked = false, learning = false;
    float learnProgress = 0.0f;
    juce::Rectangle<int> meterArea;
};

/// A compressor: mode, detector, its eight knobs, the auto and sidechain switches, its static curve, and
/// the gain reduction.
class CompressorPage final : public BlockPage
{
public:
    CompressorPage (AmpSimProcessor& processor, bool isPost);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    void paintContent (juce::Graphics&) override;

    const bool post;
    const juce::String prefix;
    std::vector<Knob*> knobs;
    juce::ComboBox *mode = nullptr, *detector = nullptr;
    Switch *autoRelease = nullptr, *autoMakeup = nullptr, *sidechain = nullptr;
    ReductionMeter* reduction = nullptr;
    juce::Rectangle<int> curveArea;
    float shownThreshold = 1.0f, shownRatio = 0.0f, shownKnee = -1.0f;
};

/// The boost: mode and the knobs the mode uses (the others dimmed).
class BoostPage final : public BlockPage
{
public:
    explicit BoostPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    juce::ComboBox* mode = nullptr;
    Knob *level = nullptr, *tilt = nullptr, *tightHz = nullptr, *mid = nullptr;
    juce::Label* about = nullptr;
    int shownMode = -1;
};

/// The overdrive: mode, drive, tone, level, mix, the tight filter, and the drive blocks' oversampling.
class OverdrivePage final : public BlockPage
{
public:
    explicit OverdrivePage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    juce::ComboBox *mode = nullptr, *oversampling = nullptr;
    Switch* tight = nullptr;
    Knob *drive = nullptr, *tone = nullptr, *level = nullptr, *mix = nullptr, *tightHz = nullptr;
    juce::Label *oversamplingNote = nullptr, *about = nullptr;
    int shownTight = -1, shownMode = -1;
};

/// The EQ's response over a live spectrum, with a draggable handle per band (on the stage, its EQ mode).
/// Parametric: drag a band's handle for its frequency and gain, scroll on it for Q, alt-click or double-click
/// to flatten it. Graphic: drag a slider's handle up and down.
class EqGraph final : public juce::Component, private juce::Timer
{
public:
    EqGraph (AmpSimProcessor& processor, const juce::String& prefix, bool isPost);
    ~EqGraph() override;

    /// Recomputes the curve if the settings moved.
    void refreshCurve();
    /// Pulls the tapped samples, runs the FFT, and repaints (30 times a second while it shows).
    void updateAnalyzer();
    SpectrumAnalyzer& getAnalyzer() noexcept { return analyzer; }
    bool isPost() const noexcept { return post; }

    /// The handle under a point (-1 for none), and its parameters, for tests.
    int handleAt (juce::Point<float> position) const;
    juce::Point<float> handlePosition (int handle) const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Handle
    {
        juce::String frequencyId, gainId, qId, typeId; // graphic handles have only a gain
        double frequency = 1000.0;
    };

    void timerCallback() override; // ends a run of wheel moves (one Q gesture)
    bool parametric() const;
    int numHandles() const;
    const Handle& handle (int index) const;
    juce::Rectangle<float> plot() const;
    float xFor (double frequency) const;
    double frequencyAt (float x) const;
    float yForGain (double db) const;
    double gainAt (float y) const;
    float levelDisplayOffset (double frequency) const;
    void setHovered (int index);
    void resetHandle (int index);

    AmpSimProcessor& ampSim;
    const juce::String prefix;
    const bool post;
    params::EqualizerParameters values;
    ampsim::Equalizer::Settings drawn;
    bool everDrawn = false;
    std::vector<double> frequencies, curve;
    std::vector<Handle> bandHandles, sliderHandles;
    SpectrumAnalyzer analyzer;
    int hovered = -1, dragging = -1;
    bool wheeling = false;
    juce::Point<float> lastDrag;
    double dragFrequency = 0.0, dragGain = 0.0;
};

/// An EQ in the inspector: on, mode, the nine sliders (graphic) or the five bands (parametric), and the cuts.
/// Its graph is on the stage (EqStage).
class EqPage final : public BlockPage
{
public:
    EqPage (AmpSimProcessor& processor, bool isPost);
    ~EqPage() override;
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    void showMode (bool parametric);

    const juce::String prefix;
    juce::ComboBox* mode = nullptr;
    std::vector<Fader*> sliders;
    struct Band
    {
        juce::ComboBox* type = nullptr;
        ValueField *frequency = nullptr, *gain = nullptr, *q = nullptr;
    };
    std::array<Band, ampsim::Equalizer::numParametricBands> bands;
    Switch *lowCut = nullptr, *highCut = nullptr;
    Knob *lowCutFrequency = nullptr, *highCutFrequency = nullptr;
    juce::ComboBox *lowCutSlope = nullptr, *highCutSlope = nullptr;
    int shownMode = -1;
};

/// The stage's EQ mode: one EQ's graph at the stage's size (the post EQ, or the pre EQ: a choice at its top left,
/// which also follows the EQ pedal selected on the board), with the live spectrum at its tap while it shows.
class EqStage final : public ControlGroup, private juce::Timer
{
public:
    explicit EqStage (AmpSimProcessor& processor);
    ~EqStage() override;

    /// Shows the pre EQ's graph (true) or the post EQ's.
    void showPre (bool pre);
    bool isShowingPre() const noexcept { return pre; }
    EqGraph& getGraph (bool preEq) noexcept { return preEq ? *preGraph : *postGraph; }
    EqGraph& getShownGraph() noexcept { return getGraph (pre); }
    Segmented& getChoice() noexcept { return *choice; }

    /// The player picked the other EQ on the stage.
    std::function<void (bool pre)> onChoose;

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { getShownGraph().updateAnalyzer(); }
    void tap();

    std::unique_ptr<EqGraph> postGraph, preGraph;
    std::unique_ptr<Segmented> choice;
    bool pre = false, showing = false;
};

/// The harmonizer: key, scale (with the 12-note custom scale), the out-of-key rule, the lowest note,
/// glide and level, a section per voice, and what it hears now.
class HarmonizerPage final : public BlockPage
{
public:
    explicit HarmonizerPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    void paintContent (juce::Graphics&) override;

    juce::ComboBox *root = nullptr, *scale = nullptr, *outOfKey = nullptr, *floor = nullptr;
    Knob *glide = nullptr, *level = nullptr;
    std::array<juce::TextButton*, 12> customNotes {};
    struct Row
    {
        Switch* on = nullptr;
        juce::ComboBox* mode = nullptr;
        std::array<ValueField*, 6> fields {}; // steps, semitones, octave, level, pan, humanize
    };
    std::array<Row, ampsim::Harmonizer::maxVoices> rows;
    juce::String hearing;
    int shownMask = -1, shownScale = -1;
    juce::Rectangle<int> hearingArea;
};

/// The multivoicer: engine, mix mode (Blend or Add), voice count, mix, spread, the wet high-pass, the four
/// one-click shapes, the starting points, and a section per voice (voices past the count dimmed), each with an
/// interval picker (a chevron beside its semitones) for the named intervals.
class MultivoicerPage final : public BlockPage
{
public:
    explicit MultivoicerPage (AmpSimProcessor& processor);
    void refresh() override;

    /// For tests: the shape buttons (Octave up, Octave down, Power fifth, Octave stack), a voice's interval
    /// picker, and the menu that picker shows.
    juce::Button& getShapeButton (int index) { return *shapes[(size_t) index]; }
    juce::Button& getIntervalPicker (int voice) { return *rows[(size_t) voice].picker; }
    juce::PopupMenu intervalMenu (int voice);

private:
    void describe (InspectorLayout&) override;

    juce::ComboBox *engine = nullptr, *mixMode = nullptr;
    Switch* highPass = nullptr;
    juce::TextButton* startingPoints = nullptr;
    std::array<juce::TextButton*, 4> shapes {};
    Knob *voices = nullptr, *mix = nullptr, *spread = nullptr, *highPassHz = nullptr;
    struct Row
    {
        std::array<ValueField*, 6> fields {}; // semitones, cents, delay, pan, level, drift
        juce::Button* picker = nullptr;       // beside the semitones: the named intervals
    };
    std::array<Row, ampsim::Multivoicer::maxVoices> rows;
    int shownVoices = -1;
};

/// Bloom: its switch and mix, the order of its three effects (drag the chips), and the bitcrusher, phaser,
/// and flanger, a section each, in that order.
class BloomPage final : public BlockPage
{
public:
    explicit BloomPage (AmpSimProcessor& processor);
    void refresh() override;

    class OrderChips;
    OrderChips& getOrderChips() noexcept { return *chips; }

private:
    struct Effect
    {
        juce::String title;
        Switch* on = nullptr;
        std::vector<std::pair<juce::String, juce::ComboBox*>> combos;
        std::vector<Switch*> switches;
        std::vector<Knob*> knobs;
        juce::ComboBox* note = nullptr; // shown instead of the rate knob when synced
        Knob* rate = nullptr;
    };
    void describe (InspectorLayout&) override;

    ValueField* mix = nullptr;
    juce::Label* latency = nullptr;
    OrderChips* chips = nullptr;
    Effect crush, phaser, flanger;
    juce::StringArray shownOrder;
    int shownPhaserMode = -1, shownPhaserSync = -1, shownFlangerSync = -1;
};

/// The chorus.
class ChorusPage final : public BlockPage
{
public:
    explicit ChorusPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    juce::ComboBox *mode = nullptr, *shape = nullptr, *note = nullptr;
    Switch *sync = nullptr, *analog = nullptr, *noise = nullptr, *highPass = nullptr;
    Knob *rate = nullptr, *depth = nullptr, *mix = nullptr, *width = nullptr, *highPassHz = nullptr;
    int shownSync = -1;
};

/// The delay, with the tempo and Tap.
class DelayPage final : public BlockPage
{
public:
    explicit DelayPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    juce::ComboBox *mode = nullptr, *stereo = nullptr, *note = nullptr, *rightNote = nullptr;
    Switch* sync = nullptr;
    Knob *time = nullptr, *rightTime = nullptr, *offset = nullptr, *feedback = nullptr, *mix = nullptr, *lowCut = nullptr, *highCut = nullptr,
         *modDepth = nullptr, *modRate = nullptr, *duck = nullptr, *tempo = nullptr;
    juce::TextButton* tap = nullptr;
    int shownState = -1;
};

/// The reverb, with freeze and the shimmer.
class ReverbPage final : public BlockPage
{
public:
    explicit ReverbPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void describe (InspectorLayout&) override;
    juce::ComboBox *engine = nullptr, *preDelayNote = nullptr, *shimmerInterval = nullptr;
    Switch *freeze = nullptr, *preDelaySync = nullptr;
    Knob *mix = nullptr, *preDelay = nullptr, *decay = nullptr, *size = nullptr, *lowDecay = nullptr, *highDecay = nullptr, *diffusion = nullptr,
         *earlyLate = nullptr, *modDepth = nullptr, *modRate = nullptr, *width = nullptr, *ducking = nullptr, *lowCut = nullptr, *highCut = nullptr,
         *shimmer = nullptr;
    int shownSync = -1;
};

/// The tempo notes' names ("1/4", "1/8D", ...) in parameter order.
juce::StringArray noteNames();

} // namespace ui

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
/// fields, and faders, every one tagged with its parameter for MIDI learn. Pages and the cards inside
/// them (an amp slot, a cab mic, one of Bloom's effects) are made of these.
class ControlGroup : public juce::Component
{
public:
    explicit ControlGroup (AmpSimProcessor& processor);
    ~ControlGroup() override;

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

    /// A card (a raised panel with its heading inside, UI_DESIGN "Components"), drawn with the headings.
    /// Returns the area inside it, under the heading.
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

/// One block's editor, shown under the chain strip when its block is selected (UI_DESIGN "Layout",
/// Editor). A page is a panel with a header (the section's colour, the block's name, where it sits, and
/// its switch) over the block's controls, all built from the shared components and attached to their
/// parameters. Pages exist all the time; only the selected one is visible, and only it is refreshed.
class BlockPage : public ControlGroup
{
public:
    BlockPage (AmpSimProcessor& processor, BlockId id, juce::String title, juce::String subtitle);
    ~BlockPage() override;

    BlockId getBlockId() const noexcept { return id; }

    /// Ten times a second while showing: status lines, meters, and what a mode dims or hides.
    virtual void refresh() {}

    /// The page became the visible one, or stopped being it (pages with their own timers start and stop them).
    virtual void pageShown() {}
    virtual void pageHidden() {}

    void paint (juce::Graphics&) override;
    void resized() override;

protected:
    virtual void layoutContent (juce::Rectangle<int> area) = 0;
    virtual void paintContent (juce::Graphics&) {}

    /// The header's free space between the title and the switch (layoutContent may put controls there).
    juce::Rectangle<int> headerSpace;
    Switch* onSwitch = nullptr; // the block's own switch, in the header, when it has one

    static constexpr int headerHeight = 36;

private:
    const BlockId id;
    const juce::String title, subtitle;
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

/// Input and output: the input gain and output level, and the footswitch's built-in controllers.
class IoPage final : public BlockPage
{
public:
    explicit IoPage (AmpSimProcessor& processor);

private:
    void layoutContent (juce::Rectangle<int> area) override;
    Knob *input = nullptr, *output = nullptr;
    ValueField *tapCc = nullptr, *freezeCc = nullptr, *sceneCc = nullptr;
    juce::Label *tapLabel = nullptr, *freezeLabel = nullptr, *sceneLabel = nullptr, *note = nullptr;
};

/// The amp: three slot cards (load a capture, its status, the trims and the tone controls; click a card's
/// Play to switch to it) and the input calibration under them.
class AmpPage final : public BlockPage
{
public:
    AmpPage (AmpSimProcessor& processor, std::function<void (int slot)> onLoad);
    void refresh() override;

private:
    class SlotCard;
    void layoutContent (juce::Rectangle<int> area) override;
    std::array<SlotCard*, AmpSimProcessor::numAmpSlots> cards {};
    Switch* calibrate = nullptr;
    ValueField* interfaceLevel = nullptr;
    juce::Label* interfaceLabel = nullptr;
};

/// The cab: a speaker seen from the side with the two close mics on it (drag a mic once its pack is
/// loaded: across the cone from the cap to the edge, and away from the grille), a card per mic (its IR or
/// pack, level, pan, delay, polarity, mute, and the file's channel), the room mic's card, alignment, and
/// the cuts.
class CabPage final : public BlockPage
{
public:
    CabPage (AmpSimProcessor& processor, std::function<void (int mic, bool pack)> onLoad);
    void refresh() override;

    class SpeakerMap;
    SpeakerMap& getSpeakerMap() noexcept { return *map; }

private:
    class MicCard;
    void layoutContent (juce::Rectangle<int> area) override;
    SpeakerMap* map = nullptr;
    std::array<MicCard*, AmpSimProcessor::numCabMics> mics {};
    Switch *align = nullptr, *lowCut = nullptr, *highCut = nullptr;
    juce::Label* alignment = nullptr;
    Knob *lowCutFrequency = nullptr, *highCutFrequency = nullptr;
    juce::ComboBox *lowCutSlope = nullptr, *highCutSlope = nullptr;
};

/// A noise gate: its knobs, release mode, detector, sidechain filter, the detector's level against the
/// open and close thresholds, and the gain reduction. Gate A has Learn; Gate B the link, and while linked
/// its own settings are dimmed (it applies Gate A's decision).
class GatePage final : public BlockPage
{
public:
    GatePage (AmpSimProcessor& processor, bool isGateB);
    void refresh() override;

private:
    void layoutContent (juce::Rectangle<int> area) override;
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
    void layoutContent (juce::Rectangle<int> area) override;
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
    void layoutContent (juce::Rectangle<int> area) override;
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
    void layoutContent (juce::Rectangle<int> area) override;
    juce::ComboBox *mode = nullptr, *oversampling = nullptr;
    Switch* tight = nullptr;
    Knob *drive = nullptr, *tone = nullptr, *level = nullptr, *mix = nullptr, *tightHz = nullptr;
    juce::Label *oversamplingNote = nullptr, *about = nullptr;
    int shownTight = -1, shownMode = -1;
};

/// The EQ's response over a live spectrum, with a draggable handle per band (UI_DESIGN "Layout", EQ
/// page). Parametric: drag a band's handle for its frequency and gain, scroll on it for Q, alt-click
/// or double-click to flatten it. Graphic: drag a slider's handle up and down.
class EqGraph final : public juce::Component, private juce::Timer
{
public:
    EqGraph (AmpSimProcessor& processor, const juce::String& prefix, bool isPost);
    ~EqGraph() override;

    /// Recomputes the curve if the settings moved.
    void refreshCurve();
    /// Pulls the tapped samples, runs the FFT, and repaints (30 times a second while the page shows).
    void updateAnalyzer();
    SpectrumAnalyzer& getAnalyzer() noexcept { return analyzer; }

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

/// The EQ: on, mode, the graph, the nine sliders or the five bands, and the cuts.
class EqPage final : public BlockPage, private juce::Timer
{
public:
    EqPage (AmpSimProcessor& processor, bool isPost);
    ~EqPage() override;
    void refresh() override;
    void pageShown() override;
    void pageHidden() override;

    EqGraph& getGraph() noexcept { return *graph; }

private:
    void timerCallback() override { graph->updateAnalyzer(); }
    void layoutContent (juce::Rectangle<int> area) override;
    void showMode (bool parametric);

    const bool post;
    const juce::String prefix;
    EqGraph* graph = nullptr;
    juce::ComboBox* mode = nullptr;
    std::vector<Fader*> sliders;
    struct Band
    {
        juce::Label* number = nullptr;
        juce::ComboBox* type = nullptr;
        ValueField *frequency = nullptr, *gain = nullptr, *q = nullptr;
    };
    std::array<Band, ampsim::Equalizer::numParametricBands> bands;
    Switch *lowCut = nullptr, *highCut = nullptr;
    Knob *lowCutFrequency = nullptr, *highCutFrequency = nullptr;
    juce::ComboBox *lowCutSlope = nullptr, *highCutSlope = nullptr;
    int shownMode = -1;
};

/// The harmonizer: key, scale (with the 12-note custom scale), the out-of-key rule, the lowest note,
/// glide and level, a row per voice, and what it hears now.
class HarmonizerPage final : public BlockPage
{
public:
    explicit HarmonizerPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void layoutContent (juce::Rectangle<int> area) override;
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
    juce::Rectangle<int> hearingArea, columnsArea;
};

/// The multivoicer: engine, voice count, mix, spread, the wet high-pass, the starting points, and a row
/// per voice (rows past the voice count dimmed).
class MultivoicerPage final : public BlockPage
{
public:
    explicit MultivoicerPage (AmpSimProcessor& processor);
    void refresh() override;

private:
    void layoutContent (juce::Rectangle<int> area) override;
    void paintContent (juce::Graphics&) override;

    juce::ComboBox* engine = nullptr;
    Switch* highPass = nullptr;
    juce::TextButton* startingPoints = nullptr;
    Knob *voices = nullptr, *mix = nullptr, *spread = nullptr, *highPassHz = nullptr;
    struct Row
    {
        juce::Label* number = nullptr;
        std::array<ValueField*, 6> fields {}; // semitones, cents, delay, pan, level, drift
    };
    std::array<Row, ampsim::Multivoicer::maxVoices> rows;
    int shownVoices = -1;
    juce::Rectangle<int> columnsArea;
};

/// Bloom: its switch and mix, the order of its three effects (drag the chips), and the bitcrusher, phaser,
/// and flanger side by side in that order.
class BloomPage final : public BlockPage
{
public:
    explicit BloomPage (AmpSimProcessor& processor);
    void refresh() override;

    class OrderChips;
    OrderChips& getOrderChips() noexcept { return *chips; }

private:
    class EffectCard;
    void layoutContent (juce::Rectangle<int> area) override;

    ValueField* mix = nullptr;
    juce::Label *mixLabel = nullptr, *latency = nullptr;
    OrderChips* chips = nullptr;
    EffectCard *crush = nullptr, *phaser = nullptr, *flanger = nullptr;
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
    void layoutContent (juce::Rectangle<int> area) override;
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
    void layoutContent (juce::Rectangle<int> area) override;
    juce::ComboBox *mode = nullptr, *stereo = nullptr, *note = nullptr, *rightNote = nullptr;
    juce::Label *noteLabel = nullptr, *rightNoteLabel = nullptr;
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
    void layoutContent (juce::Rectangle<int> area) override;
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

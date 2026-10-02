#include "PluginEditor.h"
#include "BlockParameters.h"

namespace
{
const auto background = juce::Colour (0xff17191d);
const auto panel = juce::Colour (0xff22252b);
const auto accent = juce::Colour (0xff3fa7d6);
const auto textColour = juce::Colour (0xffe6e6e6);
const auto dimText = juce::Colour (0xff9aa0a8);
const auto errorColour = juce::Colour (0xffff6b5e);
const auto warningColour = juce::Colour (0xffffc04d);

const juce::Identifier parameterIdProperty { "parameterId" };

/// Marks a control with the parameter it's attached to, so a right-click on it can offer MIDI learn.
/// Returns the control, so it can wrap an attachment's argument.
template <typename Control>
Control& tagged (Control& control, const juce::String& parameterId)
{
    control.getProperties().set (parameterIdProperty, parameterId);
    return control;
}

/// A combo box must have its items before a parameter attachment is made for it.
juce::ComboBox& withItems (juce::ComboBox& box, const juce::StringArray& items)
{
    box.addItemList (items, 1);
    return box;
}

void stylePanelTitle (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, textColour);
}

void styleStatus (juce::Label& label)
{
    label.setColour (juce::Label::textColourId, textColour);
    label.setJustificationType (juce::Justification::topLeft);
    label.setMinimumHorizontalScale (0.8f);
}
} // namespace

// ---- Knob -------------------------------------------------------------------------------------

Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption,
            const juce::String& suffix)
    : attachment (state, parameterId, tagged (slider, parameterId))
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
    slider.setTextValueSuffix (suffix);

    if (auto* param = state.getParameter (parameterId))
        slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    addAndMakeVisible (slider);

    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::textColourId, dimText);
    addAndMakeVisible (label);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    label.setBounds (area.removeFromTop (18));
    slider.setBounds (area);
}

// ---- SlotPanel --------------------------------------------------------------------------------

SlotPanel::SlotPanel (AmpSimProcessor& processor, int slotIndex, std::function<void()> onLoad)
    : slot (slotIndex)
{
    stylePanelTitle (title, "Amp " + juce::String (slot + 1));
    addAndMakeVisible (title);
    styleStatus (status);
    addAndMakeVisible (status);

    loadButton.onClick = std::move (onLoad);
    addAndMakeVisible (loadButton);

    auto& state = processor.parameters;
    knobs.add (new Knob (state, AmpSimProcessor::ampParamId (slot, "input_trim"), "Input"));
    for (const auto& band : ampsim::AmpTone::bands)
        knobs.add (new Knob (state, AmpSimProcessor::ampParamId (slot, juce::String (band.name).toLowerCase()), band.name));
    knobs.add (new Knob (state, AmpSimProcessor::ampParamId (slot, "output_trim"), "Output"));

    for (auto* knob : knobs)
        addAndMakeVisible (knob);
}

void SlotPanel::setActive (bool isActive)
{
    if (active != isActive)
    {
        active = isActive;
        repaint();
    }
}

void SlotPanel::setStatus (const juce::String& text, bool isError)
{
    status.setText (text == "Empty" ? juce::String ("Empty: the clean DI passes through") : text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, isError ? errorColour : textColour);
}

void SlotPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    if (active)
    {
        g.setColour (accent);
        g.drawRoundedRectangle (bounds, 6.0f, 2.0f);
    }
}

void SlotPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    title.setBounds (top.removeFromLeft (80));
    loadButton.setBounds (top.removeFromRight (120));
    area.removeFromTop (4);
    status.setBounds (area.removeFromTop (38));
    area.removeFromTop (6);

    // Knobs: Input, the five tone bands, Output, in two rows of four and three.
    const auto rowHeight = (area.getHeight() - 12) / 2;
    auto row1 = area.removeFromTop (rowHeight);
    area.removeFromTop (12);
    auto row2 = area;
    const auto width = row1.getWidth() / 4;

    for (int i = 0; i < knobs.size(); ++i)
    {
        auto& row = i < 4 ? row1 : row2;
        knobs[i]->setBounds (row.removeFromLeft (width));
    }
}

// ---- MicPositionPad ---------------------------------------------------------------------------

MicPositionPad::MicPositionPad (juce::AudioProcessorValueTreeState& state, const juce::String& xParameterId,
                                const juce::String& yParameterId)
    : xParameter (state.getParameter (xParameterId)), yParameter (state.getParameter (yParameterId))
{
    jassert (xParameter != nullptr && yParameter != nullptr);
}

void MicPositionPad::setPoints (std::vector<juce::Point<float>> packPoints)
{
    if (packPoints != points)
    {
        points = std::move (packPoints);
        setMouseCursor (points.empty() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}

void MicPositionPad::refresh()
{
    const juce::Point<float> position { xParameter->convertFrom0to1 (xParameter->getValue()),
                                        yParameter->convertFrom0to1 (yParameter->getValue()) };
    if (position != drawnPosition)
        repaint();
}

juce::Rectangle<float> MicPositionPad::mapArea() const
{
    // Room for the axis captions: "cap" / "edge" below, "close" / "far" on the left.
    return getLocalBounds().toFloat().withTrimmedLeft (34.0f).withTrimmedBottom (16.0f).reduced (6.0f);
}

void MicPositionPad::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto map = mapArea();
    g.setColour (background.brighter (0.05f));
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (dimText.withAlpha (0.25f));
    for (int i = 0; i <= 4; ++i)
    {
        const auto x = map.getX() + map.getWidth() * (float) i / 4.0f;
        const auto y = map.getY() + map.getHeight() * (float) i / 4.0f;
        g.drawVerticalLine (juce::roundToInt (x), map.getY(), map.getBottom());
        g.drawHorizontalLine (juce::roundToInt (y), map.getX(), map.getRight());
    }

    g.setColour (dimText);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("cap", juce::Rectangle<float> (map.getX() - 10.0f, map.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
    g.drawText ("edge", juce::Rectangle<float> (map.getRight() - 36.0f, map.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredRight);
    g.drawText ("close", juce::Rectangle<float> (2.0f, map.getY() - 4.0f, 34.0f, 14.0f), juce::Justification::centredLeft);
    g.drawText ("far", juce::Rectangle<float> (2.0f, map.getBottom() - 10.0f, 34.0f, 14.0f), juce::Justification::centredLeft);

    drawnPosition = { xParameter->convertFrom0to1 (xParameter->getValue()), yParameter->convertFrom0to1 (yParameter->getValue()) };

    if (points.empty())
    {
        g.drawFittedText ("Load a cab pack (a folder of IRs)\nto move this mic", map.toNearestInt(), juce::Justification::centred, 2);
        return;
    }

    const auto toScreen = [map] (juce::Point<float> p) { return juce::Point<float> (map.getX() + p.x * map.getWidth(), map.getY() + p.y * map.getHeight()); };

    g.setColour (textColour.withAlpha (0.7f));
    for (const auto& p : points)
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (toScreen (p)));

    const auto mic = toScreen (drawnPosition);
    g.setColour (accent);
    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (mic));
    g.setColour (textColour);
    g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (mic), 1.5f);
}

void MicPositionPad::moveTo (juce::Point<float> position)
{
    const auto map = mapArea();
    const auto x = juce::jlimit (0.0f, 1.0f, (position.x - map.getX()) / map.getWidth());
    const auto y = juce::jlimit (0.0f, 1.0f, (position.y - map.getY()) / map.getHeight());
    xParameter->setValueNotifyingHost (xParameter->convertTo0to1 (x));
    yParameter->setValueNotifyingHost (yParameter->convertTo0to1 (y));
    repaint();
}

void MicPositionPad::mouseDown (const juce::MouseEvent& e)
{
    if (points.empty())
        return;

    dragging = true;
    xParameter->beginChangeGesture();
    yParameter->beginChangeGesture();
    moveTo (e.position);
}

void MicPositionPad::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        moveTo (e.position);
}

void MicPositionPad::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    xParameter->endChangeGesture();
    yParameter->endChangeGesture();
}

// ---- MicPanel ---------------------------------------------------------------------------------

MicPanel::MicPanel (AmpSimProcessor& processor, int micIndex, std::function<void()> onLoad, std::function<void()> onLoadPack)
    : mic (micIndex)
{
    const bool isRoom = mic == AmpSimProcessor::roomMic;
    stylePanelTitle (title, isRoom ? juce::String ("Room mic") : "Close mic " + juce::String (mic + 1));
    addAndMakeVisible (title);
    styleStatus (status);
    addAndMakeVisible (status);

    loadButton.onClick = std::move (onLoad);
    addAndMakeVisible (loadButton);

    if (! isRoom)
    {
        packButton.onClick = std::move (onLoadPack);
        packButton.setTooltip ("A folder of IRs of one cab at different mic positions, which makes this mic movable");
        addAndMakeVisible (packButton);
        pad = std::make_unique<MicPositionPad> (processor.parameters, AmpSimProcessor::cabParamId (mic, "pos_x"),
                                                AmpSimProcessor::cabParamId (mic, "pos_y"));
        addAndMakeVisible (*pad);
    }

    auto& state = processor.parameters;
    knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "level"), "Level"));

    if (isRoom)
    {
        knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "predelay"), "Pre-delay", " ms"));
    }
    else
    {
        knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "pan"), "Pan", ""));
        knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "delay"), "Delay", " smp"));

        auto* invert = toggles.add (new ToggleControl ("Invert"));
        toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, AmpSimProcessor::cabParamId (mic, "invert"), tagged (*invert, AmpSimProcessor::cabParamId (mic, "invert"))));

        withItems (channel, { "Left channel", "Right channel" });
        channelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, AmpSimProcessor::cabParamId (mic, "channel"), tagged (channel, AmpSimProcessor::cabParamId (mic, "channel")));
        addAndMakeVisible (channel);
    }

    auto* mute = toggles.add (new ToggleControl ("Mute"));
    toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, AmpSimProcessor::cabParamId (mic, "mute"), tagged (*mute, AmpSimProcessor::cabParamId (mic, "mute"))));

    for (auto* knob : knobs)
        addAndMakeVisible (knob);
    for (auto* toggle : toggles)
        addAndMakeVisible (toggle);
}

void MicPanel::setStatus (const juce::String& text, bool isError)
{
    status.setText (text == "No IR" ? juce::String ("No IR: this mic is silent") : text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, isError ? errorColour : textColour);
}

void MicPanel::setPackPoints (const std::vector<ampsim::CabPack::Point>& packPoints)
{
    if (pad == nullptr)
        return;

    std::vector<juce::Point<float>> positions;
    for (const auto& p : packPoints)
        positions.push_back ({ (float) p.x, (float) p.y });
    pad->setPoints (std::move (positions));
    pad->refresh();
}

void MicPanel::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
}

void MicPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    if (pad != nullptr)
    {
        packButton.setBounds (top.removeFromRight (92));
        top.removeFromRight (4);
        loadButton.setBounds (top.removeFromRight (84));
    }
    else
    {
        loadButton.setBounds (top.removeFromRight (100));
    }
    title.setBounds (top);
    area.removeFromTop (4);
    status.setBounds (area.removeFromTop (38));
    area.removeFromTop (4);

    if (pad != nullptr)
    {
        pad->setBounds (area.removeFromTop (juce::jmax (90, area.getHeight() - 150)));
        area.removeFromTop (6);
    }

    auto switches = area.removeFromBottom (28);
    if (mic != AmpSimProcessor::roomMic)
        channel.setBounds (switches.removeFromRight (130));
    for (auto* toggle : toggles)
        toggle->setBounds (switches.removeFromLeft (80));

    area.removeFromBottom (6);
    const auto width = area.getWidth() / 3;
    for (auto* knob : knobs)
        knob->setBounds (area.removeFromLeft (width));
}

// ---- OrderStrip -------------------------------------------------------------------------------

namespace
{
juce::String displayName (const juce::String& blockName)
{
    if (blockName == "gate") return "Gate A";
    if (blockName == "boost") return "Boost";
    if (blockName == "overdrive") return "Overdrive";
    if (blockName == "bloom") return "Bloom";
    if (blockName == "harmonizer") return "Harmonizer";
    if (blockName == "multivoicer") return "Multivoicer";
    if (blockName == "bitcrush") return "Bitcrush";
    if (blockName == "phaser") return "Phaser";
    if (blockName == "flanger") return "Flanger";
    if (blockName == "comp") return "Compressor";
    if (blockName == "eq") return "EQ";
    if (blockName == "delay") return "Delay";
    if (blockName == "chorus") return "Chorus";
    if (blockName == "reverb") return "Reverb";
    return blockName;
}
} // namespace

OrderStrip::OrderStrip (const juce::String& titleToShow, std::function<juce::StringArray()> getter, std::function<void (const juce::StringArray&)> setter)
    : title (titleToShow), getOrder (std::move (getter)), setOrder (std::move (setter))
{
    refresh();
}

void OrderStrip::refresh()
{
    const auto order = getOrder();
    if (order == shown)
        return;

    shown = order;
    names.clear();
    earlier.clear();
    later.clear();

    for (int i = 0; i < shown.size(); ++i)
    {
        auto* name = names.add (new juce::Label ({}, juce::String (i + 1) + ". " + displayName (shown[i])));
        name->setJustificationType (juce::Justification::centred);
        name->setColour (juce::Label::textColourId, textColour);
        addAndMakeVisible (name);

        auto* left = earlier.add (new juce::TextButton ("<"));
        left->setTooltip ("Move earlier in the chain");
        left->setEnabled (i > 0);
        left->onClick = [this, i] { move (i, -1); };
        addAndMakeVisible (left);

        auto* right = later.add (new juce::TextButton (">"));
        right->setTooltip ("Move later in the chain");
        right->setEnabled (i + 1 < shown.size());
        right->onClick = [this, i] { move (i, 1); };
        addAndMakeVisible (right);
    }
    resized();
}

void OrderStrip::move (int position, int by)
{
    auto order = shown;
    order.move (position, position + by);
    setOrder (order);
    refresh();
}

void OrderStrip::paint (juce::Graphics& g)
{
    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (title, getLocalBounds().removeFromLeft (170), juce::Justification::centredLeft);
}

void OrderStrip::resized()
{
    auto area = getLocalBounds();
    area.removeFromLeft (175);
    const auto cellWidth = juce::jmin (200, area.getWidth() / juce::jmax (1, names.size()));
    for (int i = 0; i < names.size(); ++i)
    {
        auto cell = area.removeFromLeft (cellWidth).reduced (4, 2);
        earlier[i]->setBounds (cell.removeFromLeft (28));
        later[i]->setBounds (cell.removeFromRight (28));
        names[i]->setBounds (cell);
    }
}

// ---- CompressorPanel ----------------------------------------------------------------------------

CompressorPanel::CompressorPanel (AmpSimProcessor& processor, const juce::String& p, const juce::String& title, bool isPost)
    : ampSim (processor), post (isPost)
{
    auto& state = processor.parameters;
    stylePanelTitle (titleLabel, title);
    addAndMakeVisible (titleLabel);

    for (auto [button, id] : std::initializer_list<std::pair<ToggleControl*, juce::String>> {
             { &onButton, p + "_on" }, { &autoReleaseButton, p + "_auto_release" }, { &autoMakeupButton, p + "_auto_makeup" }, { &sidechainButton, p + "_sc_hpf" } })
    {
        buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, id, tagged (*button, id)));
        addAndMakeVisible (button);
    }

    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, p + "_mode", tagged (withItems (mode, { "Studio", "Pedal" }), p + "_mode"));
    detectorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, p + "_detector", tagged (withItems (detector, { "Peak", "RMS" }), p + "_detector"));
    addAndMakeVisible (mode);
    addAndMakeVisible (detector);

    knobs.add (new Knob (state, p + "_threshold", "Threshold"));
    knobs.add (new Knob (state, p + "_ratio", "Ratio", ":1"));
    knobs.add (new Knob (state, p + "_knee", "Knee"));
    knobs.add (new Knob (state, p + "_attack", "Attack", " ms"));
    knobs.add (new Knob (state, p + "_release", "Release", " ms"));
    knobs.add (new Knob (state, p + "_makeup", "Makeup"));
    knobs.add (new Knob (state, p + "_mix", "Mix", " %"));
    knobs.add (new Knob (state, p + "_sc_freq", "SC freq", " Hz"));
    for (auto* knob : knobs)
        addAndMakeVisible (knob);
}

void CompressorPanel::refresh()
{
    const auto reduction = ampSim.getCompressorReduction (post);
    if (std::abs (reduction - reductionDb) > 0.05f)
    {
        reductionDb = reduction;
        repaint (meterArea);
    }
}

void CompressorPanel::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);

    // Gain reduction meter: 0 to 24 dB, filling from the left.
    g.setColour (background);
    g.fillRect (meterArea);
    g.setColour (accent);
    g.fillRect (meterArea.withWidth (juce::roundToInt ((float) meterArea.getWidth() * juce::jlimit (0.0f, 1.0f, reductionDb / 24.0f))));
    g.setColour (textColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Gain reduction " + juce::String (reductionDb, 1) + " dB", meterArea, juce::Justification::centred);
}

void CompressorPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    onButton.setBounds (top.removeFromRight (60));
    titleLabel.setBounds (top);
    area.removeFromTop (6);
    auto combos = area.removeFromTop (26);
    mode.setBounds (combos.removeFromLeft (combos.getWidth() / 2).reduced (2, 0));
    detector.setBounds (combos.reduced (2, 0));
    area.removeFromTop (8);

    meterArea = area.removeFromBottom (22);
    area.removeFromBottom (8);
    auto toggles = area.removeFromBottom (26);
    const auto toggleWidth = toggles.getWidth() / 3;
    autoReleaseButton.setBounds (toggles.removeFromLeft (toggleWidth));
    autoMakeupButton.setBounds (toggles.removeFromLeft (toggleWidth));
    sidechainButton.setBounds (toggles);
    area.removeFromBottom (6);

    const auto rowHeight = area.getHeight() / 2;
    for (int row = 0; row < 2; ++row)
    {
        auto line = area.removeFromTop (rowHeight);
        const auto width = line.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
            knobs[row * 4 + i]->setBounds (line.removeFromLeft (width));
    }
}

// ---- EqCurve ----------------------------------------------------------------------------------

EqCurve::EqCurve (juce::AudioProcessorValueTreeState& state, const juce::String& prefix)
{
    values.bind (state, prefix);
    for (int i = 0; i < 160; ++i)
        frequencies.push_back (20.0 * std::pow (1000.0, i / 159.0));
    refresh();
    startTimerHz (30);
}

void EqCurve::refresh()
{
    const auto settings = values.read();
    const auto same = [] (const ampsim::Equalizer::Settings& a, const ampsim::Equalizer::Settings& b)
    {
        // Field by field (comparing the raw bytes would include padding): any change redraws.
        const auto eq = [] (float x, float y) { return std::abs (x - y) < 1.0e-6f; };
        if (a.mode != b.mode || a.lowCut.on != b.lowCut.on || a.highCut.on != b.highCut.on || a.lowCut.slope != b.lowCut.slope
            || a.highCut.slope != b.highCut.slope || ! eq (a.lowCut.frequency, b.lowCut.frequency) || ! eq (a.highCut.frequency, b.highCut.frequency))
            return false;
        for (size_t m = 0; m < a.sliders.size(); ++m)
            if (! eq (a.sliders[m], b.sliders[m]))
                return false;
        for (size_t i = 0; i < a.bands.size(); ++i)
            if (a.bands[i].type != b.bands[i].type || ! eq (a.bands[i].frequency, b.bands[i].frequency) || ! eq (a.bands[i].gainDb, b.bands[i].gainDb)
                || ! eq (a.bands[i].q, b.bands[i].q))
                return false;
        return true;
    };

    if (everDrawn && same (settings, drawn))
        return;

    drawn = settings;
    everDrawn = true;
    curve = ampsim::Equalizer::responseDb (settings, frequencies, 48000.0);
    repaint();
}

void EqCurve::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (area, 4.0f);

    const auto x = [&] (double f) { return area.getX() + area.getWidth() * (float) (std::log (f / 20.0) / std::log (1000.0)); };
    const auto y = [&] (double db) { return area.getCentreY() - area.getHeight() * 0.5f * (float) (juce::jlimit (-24.0, 24.0, db) / 24.0); };

    g.setColour (dimText.withAlpha (0.25f));
    for (auto f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        g.drawVerticalLine (juce::roundToInt (x (f)), area.getY(), area.getBottom());
    for (auto db : { -12.0, 0.0, 12.0 })
        g.drawHorizontalLine (juce::roundToInt (y (db)), area.getX(), area.getRight());

    g.setColour (dimText);
    g.setFont (juce::FontOptions (10.0f));
    for (auto [f, label] : std::initializer_list<std::pair<double, const char*>> { { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } })
        g.drawText (label, juce::Rectangle<float> (x (f) + 2.0f, area.getBottom() - 13.0f, 30.0f, 12.0f), juce::Justification::centredLeft);
    for (auto db : { -12.0, 12.0 })
        g.drawText (juce::String (db, 0), juce::Rectangle<float> (area.getX() + 2.0f, y (db) - 12.0f, 30.0f, 12.0f), juce::Justification::centredLeft);

    if (curve.empty())
        return;

    juce::Path path;
    for (size_t i = 0; i < curve.size(); ++i)
    {
        const juce::Point<float> p { x (frequencies[i]), y (curve[i]) };
        if (i == 0)
            path.startNewSubPath (p);
        else
            path.lineTo (p);
    }
    g.setColour (accent);
    g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));
}

// ---- EqualizerPanel ---------------------------------------------------------------------------

EqualizerPanel::EqualizerPanel (AmpSimProcessor& processor, const juce::String& p, const juce::String& title)
    : state (processor.parameters), prefix (p), curve (processor.parameters, p)
{
    stylePanelTitle (titleLabel, title);
    addAndMakeVisible (titleLabel);
    addAndMakeVisible (curve);

    for (auto [button, id] : std::initializer_list<std::pair<ToggleControl*, juce::String>> {
             { &onButton, p + "_on" }, { &lowCutButton, p + "_lowcut_on" }, { &highCutButton, p + "_highcut_on" } })
    {
        buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, id, tagged (*button, id)));
        addAndMakeVisible (button);
    }

    const juce::StringArray slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    for (auto [box, id, items] : std::initializer_list<std::tuple<juce::ComboBox*, juce::String, juce::StringArray>> {
             { &mode, p + "_mode", { "Graphic", "Parametric" } }, { &lowCutSlope, p + "_lowcut_slope", slopes }, { &highCutSlope, p + "_highcut_slope", slopes } })
    {
        comboAttachments.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (state, id, tagged (withItems (*box, items), id)));
        addAndMakeVisible (box);
    }

    static const char* sliderNames[] = { "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    for (int m = 0; m < ampsim::Equalizer::numGraphicBands; ++m)
    {
        auto* slider = sliders.add (new SliderControl (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow));
        slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 18);
        sliderAttachments.add (new juce::AudioProcessorValueTreeState::SliderAttachment (state, params::EqualizerParameters::sliderId (p, m), tagged (*slider, params::EqualizerParameters::sliderId (p, m))));
        slider->setDoubleClickReturnValue (true, 0.0);
        addChildComponent (slider);

        auto* label = sliderLabels.add (new juce::Label ({}, sliderNames[m]));
        label->setJustificationType (juce::Justification::centred);
        label->setColour (juce::Label::textColourId, dimText);
        addChildComponent (label);
    }

    for (int b = 0; b < ampsim::Equalizer::numParametricBands; ++b)
    {
        auto* band = bands.add (new BandControls());
        band->title.setText ("Band " + juce::String (b + 1), juce::dontSendNotification);
        band->title.setJustificationType (juce::Justification::centred);
        band->title.setColour (juce::Label::textColourId, dimText);
        band->typeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            state, params::EqualizerParameters::bandId (p, b, "type"), tagged (withItems (band->type, { "Peak", "Low shelf", "High shelf", "Notch" }), params::EqualizerParameters::bandId (p, b, "type")));
        using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        band->frequencyAttachment = std::make_unique<Attachment> (state, params::EqualizerParameters::bandId (p, b, "freq"), tagged (band->frequency, params::EqualizerParameters::bandId (p, b, "freq")));
        band->gainAttachment = std::make_unique<Attachment> (state, params::EqualizerParameters::bandId (p, b, "gain"), tagged (band->gain, params::EqualizerParameters::bandId (p, b, "gain")));
        band->qAttachment = std::make_unique<Attachment> (state, params::EqualizerParameters::bandId (p, b, "q"), tagged (band->q, params::EqualizerParameters::bandId (p, b, "q")));
        band->frequency.setTextValueSuffix (" Hz");
        band->gain.setTextValueSuffix (" dB");
        band->gain.setDoubleClickReturnValue (true, 0.0);
        for (auto* slider : { &band->frequency, &band->gain, &band->q })
            slider->setColour (juce::Slider::trackColourId, accent.withAlpha (0.55f));
        for (auto* c : band->components())
            addChildComponent (c);
    }

    for (auto heading : { "Band", "Type", "Frequency", "Gain", "Q" })
    {
        auto* label = bandHeadings.add (new juce::Label ({}, heading));
        label->setJustificationType (juce::Justification::centred);
        label->setColour (juce::Label::textColourId, dimText);
        addChildComponent (label);
    }

    lowCutKnob = std::make_unique<Knob> (state, p + "_lowcut_freq", "Low cut", " Hz");
    highCutKnob = std::make_unique<Knob> (state, p + "_highcut_freq", "High cut", " Hz");
    addAndMakeVisible (*lowCutKnob);
    addAndMakeVisible (*highCutKnob);

    parametricShown = ! (state.getRawParameterValue (p + "_mode")->load() >= 0.5f); // so refresh() sets the visibility
    refresh();
}

void EqualizerPanel::refresh()
{
    curve.refresh();

    const bool parametric = state.getRawParameterValue (prefix + "_mode")->load() >= 0.5f;
    if (parametric == parametricShown)
        return;

    parametricShown = parametric;
    for (auto* s : sliders) s->setVisible (! parametric);
    for (auto* l : sliderLabels) l->setVisible (! parametric);
    for (auto* l : bandHeadings) l->setVisible (parametric);
    for (auto* band : bands)
        for (auto* c : band->components())
            c->setVisible (parametric);
}

void EqualizerPanel::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
}

void EqualizerPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    onButton.setBounds (top.removeFromRight (60));
    mode.setBounds (top.removeFromRight (140).reduced (0, 1));
    titleLabel.setBounds (top);
    area.removeFromTop (6);
    curve.setBounds (area.removeFromTop (juce::jmin (150, area.getHeight() / 4)));
    area.removeFromTop (8);

    // Bottom: the cuts.
    auto cuts = area.removeFromBottom (96);
    const auto half = cuts.getWidth() / 2;
    auto low = cuts.removeFromLeft (half), high = cuts;
    lowCutButton.setBounds (low.removeFromLeft (90).withSizeKeepingCentre (90, 26));
    lowCutKnob->setBounds (low.removeFromLeft (100));
    lowCutSlope.setBounds (low.removeFromLeft (120).withSizeKeepingCentre (120, 26));
    highCutButton.setBounds (high.removeFromLeft (90).withSizeKeepingCentre (90, 26));
    highCutKnob->setBounds (high.removeFromLeft (100));
    highCutSlope.setBounds (high.removeFromLeft (120).withSizeKeepingCentre (120, 26));
    area.removeFromBottom (6);

    // Middle: nine sliders, or five band columns (same space, one or the other is visible).
    const auto sliderWidth = area.getWidth() / ampsim::Equalizer::numGraphicBands;
    auto sliderArea = area;
    for (int m = 0; m < sliders.size(); ++m)
    {
        auto column = sliderArea.removeFromLeft (sliderWidth);
        sliderLabels[m]->setBounds (column.removeFromTop (18));
        sliders[m]->setBounds (column);
    }

    // Parametric: a heading row, then one row per band: name, type, frequency, gain, Q.
    const std::array<float, 5> share { 0.10f, 0.20f, 0.28f, 0.24f, 0.18f };
    const auto rowHeight = juce::jmin (40, (area.getHeight() - 20) / ampsim::Equalizer::numParametricBands);
    auto headings = area.removeFromTop (20);
    const auto width = (float) headings.getWidth();
    for (int i = 0; i < bandHeadings.size(); ++i)
        bandHeadings[i]->setBounds (headings.removeFromLeft (juce::roundToInt (width * share[(size_t) i])));
    for (auto* band : bands)
    {
        auto row = area.removeFromTop (rowHeight).reduced (0, 3);
        const auto cells = band->components();
        for (size_t i = 0; i < cells.size(); ++i)
            cells[i]->setBounds (row.removeFromLeft (juce::roundToInt (width * share[i])).reduced (3, 0));
    }
}

// ---- EffectPanel, ChorusPanel, ReverbPanel ----------------------------------------------------

EffectPanel::EffectPanel (AmpSimProcessor& processor, const juce::String& title, const juce::String& onParameterId)
    : ampSim (processor)
{
    stylePanelTitle (titleLabel, title);
    addAndMakeVisible (titleLabel);
    buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (processor.parameters, onParameterId, tagged (onButton, onParameterId)));
    addAndMakeVisible (onButton);
}

void EffectPanel::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
}

Knob& EffectPanel::addKnob (const juce::String& id, const juce::String& caption, const juce::String& suffix)
{
    auto* knob = knobs.add (new Knob (ampSim.parameters, id, caption, suffix));
    addAndMakeVisible (knob);
    return *knob;
}

ToggleControl& EffectPanel::addToggle (const juce::String& id, const juce::String& text)
{
    auto* toggle = toggles.add (new ToggleControl (text));
    buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (ampSim.parameters, id, tagged (*toggle, id)));
    addAndMakeVisible (toggle);
    return *toggle;
}

juce::ComboBox& EffectPanel::addCombo (const juce::String& id, const juce::StringArray& items)
{
    auto* box = combos.add (new juce::ComboBox());
    comboAttachments.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (ampSim.parameters, id, tagged (withItems (*box, items), id)));
    addAndMakeVisible (box);
    return *box;
}

juce::Rectangle<int> EffectPanel::layoutTitle()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    onButton.setBounds (top.removeFromRight (60));
    titleLabel.setBounds (top);
    area.removeFromTop (6);
    return area;
}

namespace
{
juce::StringArray noteNameList()
{
    juce::StringArray names;
    for (const auto& n : ampsim::tempo::notes)
        names.add (n.name);
    return names;
}

/// Lays knobs (nullptr = skip a cell) out in rows of `columns`, each row `rowHeight` tall.
void layoutGrid (juce::Rectangle<int> area, const std::vector<juce::Component*>& cells, int columns, int rowHeight)
{
    const auto width = area.getWidth() / columns;
    for (size_t i = 0; i < cells.size(); ++i)
    {
        if (i % (size_t) columns == 0 && i > 0)
            area.removeFromTop (rowHeight);
        if (cells[i] != nullptr)
            cells[i]->setBounds (juce::Rectangle<int> (area.getX() + (int) (i % (size_t) columns) * width, area.getY(), width, rowHeight));
    }
}
} // namespace

ChorusPanel::ChorusPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Chorus", "chorus_on")
{
    mode = &addCombo ("chorus_mode", { "Classic", "Dimension", "Tri" });
    shape = &addCombo ("chorus_shape", { "Triangle", "Sine", "Random" });
    sync = &addToggle ("chorus_sync", "Sync to tempo");
    note = &addCombo ("chorus_note", noteNameList());
    rate = &addKnob ("chorus_rate", "Rate", " Hz");
    depth = &addKnob ("chorus_depth", "Depth", " %");
    mix = &addKnob ("chorus_mix", "Mix", " %");
    width = &addKnob ("chorus_width", "Width", " %");
    analog = &addToggle ("chorus_analog", "Analog");
    noise = &addToggle ("chorus_noise", "Noise");
    highPass = &addToggle ("chorus_hp", "Low-end protection");
    highPassHz = &addKnob ("chorus_hp_freq", "Protect below", " Hz");
    refresh();
}

void ChorusPanel::refresh()
{
    const auto synced = ampSim.parameters.getRawParameterValue ("chorus_sync")->load() >= 0.5f ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    note->setVisible (synced == 1);
    rate->setVisible (synced == 0);
}

void ChorusPanel::resized()
{
    auto area = layoutTitle();
    auto combosRow = area.removeFromTop (26);
    mode->setBounds (combosRow.removeFromLeft (combosRow.getWidth() / 2).reduced (2, 0));
    shape->setBounds (combosRow.reduced (2, 0));
    area.removeFromTop (6);
    auto syncRow = area.removeFromTop (26);
    sync->setBounds (syncRow.removeFromLeft (150));
    note->setBounds (syncRow.reduced (2, 0));
    area.removeFromTop (4);
    auto togglesRow = area.removeFromBottom (26);
    analog->setBounds (togglesRow.removeFromLeft (80));
    noise->setBounds (togglesRow.removeFromLeft (70));
    highPass->setBounds (togglesRow);
    area.removeFromBottom (4);
    layoutGrid (area, { rate, depth, mix, width, highPassHz }, 3, area.getHeight() / 2);
}

ReverbPanel::ReverbPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Reverb", "reverb_on")
{
    engine = &addCombo ("reverb_engine", { "Room", "Hall", "Plate" });
    freeze = &addToggle ("reverb_freeze", "Freeze");
    preDelaySync = &addToggle ("reverb_predelay_sync", "Sync pre-delay");
    preDelayNote = &addCombo ("reverb_predelay_note", noteNameList());
    preDelay = &addKnob ("reverb_predelay", "Pre-delay", " ms");
    grid = { &addKnob ("reverb_mix", "Mix", " %"), preDelay, &addKnob ("reverb_decay", "Decay", " s"), &addKnob ("reverb_size", "Size", " %"),
             &addKnob ("reverb_low_decay", "Low decay", " x"), &addKnob ("reverb_high_decay", "High decay", " x"),
             &addKnob ("reverb_diffusion", "Diffusion", " %"), &addKnob ("reverb_early_late", "Early/late", " %"),
             &addKnob ("reverb_mod_depth", "Mod depth", " %"), &addKnob ("reverb_mod_rate", "Mod rate", " Hz"),
             &addKnob ("reverb_width", "Width", " %"), &addKnob ("reverb_ducking", "Ducking", " %"),
             &addKnob ("reverb_lowcut", "Low cut", " Hz"), &addKnob ("reverb_highcut", "High cut", " Hz"),
             &addKnob ("reverb_shimmer", "Shimmer", " %") };
    shimmerInterval = &addCombo ("reverb_shimmer_interval", { "+12", "+7", "+19", "+24" });
    shimmerInterval->setTooltip ("Shimmer interval (Room and Hall)");
    refresh();
}

void ReverbPanel::refresh()
{
    const auto synced = ampSim.parameters.getRawParameterValue ("reverb_predelay_sync")->load() >= 0.5f ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    preDelayNote->setVisible (synced == 1);
    preDelay->setVisible (synced == 0);
}

void ReverbPanel::resized()
{
    auto area = layoutTitle();
    auto row = area.removeFromTop (26);
    engine->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
    freeze->setBounds (row.reduced (6, 0));
    area.removeFromTop (6);
    auto syncRow = area.removeFromTop (26);
    preDelaySync->setBounds (syncRow.removeFromLeft (150));
    preDelayNote->setBounds (syncRow.reduced (2, 0));
    area.removeFromTop (4);
    std::vector<juce::Component*> cells (grid.begin(), grid.end());
    layoutGrid (area, cells, 4, area.getHeight() / 4);
    // The shimmer's interval sits beside its knob, in the last row's next cell.
    shimmerInterval->setBounds (grid.back()->getBounds().translated (grid.back()->getWidth(), 0).withSizeKeepingCentre (grid.back()->getWidth() - 8, 26));
}

// ---- GatePanel --------------------------------------------------------------------------------

GatePanel::GatePanel (AmpSimProcessor& processor, bool isGateB)
    : EffectPanel (processor, isGateB ? "Gate B (after the amp)" : "Gate A (before the amp)", isGateB ? "gate_b_on" : "gate_a_on"),
      gateB (isGateB)
{
    const juce::String p = isGateB ? "gate_b" : "gate_a";
    addKnob (p + "_threshold", "Threshold");
    addKnob (p + "_hysteresis", "Hysteresis");
    addKnob (p + "_hold", "Hold", " ms");
    addKnob (p + "_attack", "Attack", " ms");
    addKnob (p + "_release", "Release", " ms");
    addKnob (p + "_range", "Range");
    addKnob (p + "_sc_freq", "SC freq", " Hz");
    releaseMode = &addCombo (p + "_release_mode", { "Adaptive release", "Classic release" });
    detector = &addCombo (p + "_detector", { "Detect from DI", "Detect from own input" });
    sidechain = &addToggle (p + "_sc_hpf", "Sidechain HPF");

    if (isGateB)
    {
        link = &addToggle ("gate_link", "Linked to Gate A");
    }
    else
    {
        learnButton.setTooltip ("Mute the strings, then press: measures the noise for 2 s and sets the threshold above it");
        learnButton.onClick = [this] { ampSim.learnGates(); };
        addAndMakeVisible (learnButton);
    }
}

void GatePanel::refresh()
{
    const auto newMeter = ampSim.getGateMeter (gateB);
    const auto newLinked = gateB && ampSim.parameters.getRawParameterValue ("gate_link")->load() >= 0.5f;
    const auto newLearning = ampSim.isLearningGates() && (! gateB || ampSim.isGateBOnItsOwn());
    const auto newProgress = ampSim.getGateLearnProgress();

    if (newLinked != linked)
    {
        // Linked, Gate B's own settings don't apply: dim them (they stay editable for when it's unlinked).
        for (auto* c : std::initializer_list<juce::Component*> { releaseMode, detector, sidechain })
            c->setAlpha (newLinked ? 0.4f : 1.0f);
        for (auto* knob : knobs)
            knob->setAlpha (newLinked ? 0.4f : 1.0f);
    }

    const auto changed = std::abs (newMeter.detectorDb - meter.detectorDb) > 0.5f || newMeter.open != meter.open
                         || std::abs (newMeter.openDb - meter.openDb) > 0.05f || std::abs (newMeter.reductionDb - meter.reductionDb) > 0.5f
                         || newLinked != linked || newLearning != learning || std::abs (newProgress - learnProgress) > 0.01f;
    meter = newMeter;
    linked = newLinked;
    learning = newLearning;
    learnProgress = newProgress;
    learnButton.setButtonText (learning ? "Learning..." : "Learn");
    if (changed)
        repaint (meterArea);
}

void GatePanel::paint (juce::Graphics& g)
{
    EffectPanel::paint (g);

    // The detector's level on a -100 to 0 dBFS scale, against the open (bright) and close (amber)
    // thresholds; the gap between them is the hysteresis.
    auto bar = meterArea;
    const auto textRow = bar.removeFromTop (18);
    const auto xFor = [&bar] (float db)
    { return (float) bar.getX() + (float) bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 100.0f); };

    g.setColour (background);
    g.fillRect (bar);
    if (! linked)
    {
        g.setColour ((meter.open ? accent : dimText).withAlpha (0.8f));
        g.fillRect (bar.toFloat().withRight (xFor (meter.detectorDb)));
        g.setColour (textColour);
        g.drawLine (xFor (meter.openDb), (float) bar.getY(), xFor (meter.openDb), (float) bar.getBottom(), 2.0f);
        g.setColour (warningColour);
        g.drawLine (xFor (meter.closeDb), (float) bar.getY(), xFor (meter.closeDb), (float) bar.getBottom(), 2.0f);
    }

    const auto dB = [] (float value) { return juce::String (juce::roundToInt (value)); };
    juce::String text;
    if (learning)
        text = "Learning the noise floor: keep the strings muted (" + juce::String (juce::roundToInt (learnProgress * 100.0f)) + "%)";
    else if (linked)
        text = juce::String ("Following Gate A: ") + (meter.reductionDb < 1.0f ? "open" : "closing, " + dB (meter.reductionDb) + " dB down");
    else
        text = (meter.open ? "Open" : "Closed") + juce::String (", level ") + dB (meter.detectorDb) + " dBFS, reduction "
               + (meter.reductionDb >= 99.0f ? juce::String ("full") : dB (meter.reductionDb) + " dB");
    g.setColour (learning ? warningColour : textColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (text, textRow, juce::Justification::centredLeft);
}

void GatePanel::resized()
{
    auto area = layoutTitle();
    releaseMode->setBounds (area.removeFromTop (26).reduced (2, 0));
    area.removeFromTop (4);
    detector->setBounds (area.removeFromTop (26).reduced (2, 0));
    area.removeFromTop (6);

    auto toggles = area.removeFromTop (26);
    sidechain->setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
    if (link != nullptr)
        link->setBounds (toggles);
    else
        learnButton.setBounds (toggles.reduced (2, 0));
    area.removeFromTop (8);

    meterArea = area.removeFromBottom (40);
    area.removeFromBottom (8);
    std::vector<juce::Component*> cells;
    for (auto* knob : knobs)
        cells.push_back (knob);
    layoutGrid (area, cells, 2, juce::jmin (110, area.getHeight() / 4));
}

// ---- HarmonizerPanel ------------------------------------------------------------------------------

HarmonizerPanel::HarmonizerPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Harmonizer", "harm_on")
{
    root = &addCombo ("harm_root", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    juce::StringArray scales;
    for (const auto* name : ampsim::harmony::scaleNames)
        scales.add (name);
    scale = &addCombo ("harm_scale", scales);
    outOfKey = &addCombo ("harm_out_of_key", { "Out of key: parallel", "Out of key: snap" });
    floor = &addCombo ("harm_floor", { "Lowest note 110 Hz (A)", "Lowest note 80 Hz (low E)", "Lowest note 60 Hz (drop)" });
    addKnob ("harm_glide", "Glide", " ms");
    addKnob ("harm_level", "Level");

    // The custom scale: 12 switches, one per semitone above the root, writing the mask parameter's bits.
    static const char* names[] = { "1", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7" };
    for (int k = 0; k < 12; ++k)
    {
        auto& button = customNotes[(size_t) k];
        button.setButtonText (names[k]);
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonOnColourId, accent);
        button.onClick = [this, k]
        {
            auto* parameter = ampSim.parameters.getParameter ("harm_custom_mask");
            auto mask = juce::roundToInt (parameter->convertFrom0to1 (parameter->getValue()));
            mask = customNotes[(size_t) k].getToggleState() ? (mask | (1 << k)) : (mask & ~(1 << k));
            parameter->setValueNotifyingHost (parameter->convertTo0to1 ((float) mask));
        };
        addAndMakeVisible (button);
    }

    static const char* what[] = { "steps", "semitones", "octave", "level", "pan", "humanize" };
    static const char* suffix[] = { " steps", " st", " oct", " dB", " %", " ms" };
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        auto& row = rows[(size_t) v];
        row.on.setButtonText (juce::String (v + 1));
        const auto onId = params::HarmonizerParameters::voiceId (v, "on");
        rowButtons.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (processor.parameters, onId, tagged (row.on, onId)));
        addAndMakeVisible (row.on);
        const auto modeId = params::HarmonizerParameters::voiceId (v, "mode");
        rowCombos.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (processor.parameters, modeId, tagged (withItems (row.mode, { "Diatonic", "Chromatic" }), modeId)));
        addAndMakeVisible (row.mode);
        for (size_t k = 0; k < row.sliders.size(); ++k)
        {
            auto& slider = row.sliders[k];
            slider.setSliderStyle (juce::Slider::LinearBar);
            slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 0, 0);
            slider.setTextValueSuffix (suffix[k]);
            slider.setColour (juce::Slider::trackColourId, accent.withAlpha (0.55f));
            const auto id = params::HarmonizerParameters::voiceId (v, what[k]);
            sliderAttachments.add (new juce::AudioProcessorValueTreeState::SliderAttachment (processor.parameters, id, tagged (slider, id)));
            addAndMakeVisible (slider);
        }
    }
    refresh();
}

void HarmonizerPanel::refresh()
{
    // The custom scale's switches follow the parameter, and only matter for the Custom scale.
    const auto mask = juce::roundToInt (ampSim.parameters.getRawParameterValue ("harm_custom_mask")->load());
    const auto currentScale = juce::roundToInt (ampSim.parameters.getRawParameterValue ("harm_scale")->load());
    if (mask != shownMask || currentScale != shownScale)
    {
        shownMask = mask;
        shownScale = currentScale;
        for (int k = 0; k < 12; ++k)
        {
            customNotes[(size_t) k].setToggleState ((mask >> k) & 1, juce::dontSendNotification);
            customNotes[(size_t) k].setAlpha (currentScale == (int) ampsim::harmony::Scale::custom ? 1.0f : 0.35f);
        }
    }

    // Diatonic voices use steps, chromatic ones semitones: dim the other.
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        const auto diatonic = ampSim.parameters.getRawParameterValue (params::HarmonizerParameters::voiceId (v, "mode"))->load() < 0.5f;
        rows[(size_t) v].sliders[0].setAlpha (diatonic ? 1.0f : 0.35f);
        rows[(size_t) v].sliders[1].setAlpha (diatonic ? 0.35f : 1.0f);
    }

    juce::String text;
    if (const auto note = ampSim.getHarmonizerNote(); note < 0)
        text = "Listening (no single note)";
    else
    {
        text = "Hearing " + juce::MidiMessage::getMidiNoteName (note, true, true, 4);
        for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
            if (const auto shift = ampSim.getHarmonizerShift (v); shift != ampsim::Harmonizer::shownSilent)
                text << "   " << (v + 1) << ": " << (shift >= 0 ? "+" : "") << shift << " (" << juce::MidiMessage::getMidiNoteName (note + shift, true, true, 4) << ")";
    }
    if (text != hearing)
    {
        hearing = text;
        repaint (statusArea);
    }
}

void HarmonizerPanel::paint (juce::Graphics& g)
{
    EffectPanel::paint (g);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f));
    auto heads = headerArea;
    heads.removeFromLeft (150);
    const auto width = heads.getWidth() / 6;
    for (const auto* name : { "Steps", "Semitones", "Octave", "Level", "Pan", "Humanize" })
        g.drawText (name, heads.removeFromLeft (width), juce::Justification::centred);
    g.setColour (textColour);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (hearing, statusArea, juce::Justification::centredLeft);
}

void HarmonizerPanel::resized()
{
    auto area = layoutTitle();
    auto top = area.removeFromTop (26);
    root->setBounds (top.removeFromLeft (70).reduced (2, 0));
    scale->setBounds (top.removeFromLeft (190).reduced (2, 0));
    outOfKey->setBounds (top.removeFromLeft (180).reduced (2, 0));
    floor->setBounds (top.reduced (2, 0));
    area.removeFromTop (6);
    auto custom = area.removeFromTop (24);
    const auto noteWidth = custom.getWidth() / 12;
    for (auto& button : customNotes)
        button.setBounds (custom.removeFromLeft (noteWidth).reduced (1, 0));
    area.removeFromTop (6);
    auto knobsRow = area.removeFromTop (100);
    knobs[0]->setBounds (knobsRow.removeFromLeft (110));
    knobs[1]->setBounds (knobsRow.removeFromLeft (110));
    statusArea = knobsRow.reduced (8, 30);
    area.removeFromTop (6);
    headerArea = area.removeFromTop (18);
    for (auto& row : rows)
    {
        auto line = area.removeFromTop (30).reduced (0, 2);
        row.on.setBounds (line.removeFromLeft (44));
        row.mode.setBounds (line.removeFromLeft (106).reduced (2, 0));
        const auto width = line.getWidth() / (int) row.sliders.size();
        for (auto& slider : row.sliders)
            slider.setBounds (line.removeFromLeft (width).reduced (2, 0));
    }
}

// ---- MultivoicerPanel -----------------------------------------------------------------------------

MultivoicerPanel::MultivoicerPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Multivoicer", "mv_on")
{
    engine = &addCombo ("mv_engine", { "Poly (chords)", "Mono (single notes)" });
    addKnob ("mv_voices", "Voices", "");
    addKnob ("mv_mix", "Mix", " %");
    addKnob ("mv_spread", "Spread", " %");
    addKnob ("mv_hp_freq", "Wet HPF", " Hz");
    highPass = &addToggle ("mv_hp", "Wet high-pass");

    startingPoints.onClick = [this]
    {
        juce::PopupMenu menu;
        using SP = ampsim::Multivoicer::StartingPoint;
        for (const auto& [startingPoint, name] : std::initializer_list<std::pair<SP, const char*>> {
                 { SP::unisonDouble, "Unison double" }, { SP::octaveStack, "Octave stack" }, { SP::fifthsStack, "Fifths stack" },
                 { SP::doubleOctaves, "Double + Octaves" } })
            menu.addItem (name, [this, sp = startingPoint] { params::MultivoicerParameters::applyStartingPoint (ampSim.parameters, sp); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (startingPoints));
    };
    addAndMakeVisible (startingPoints);

    static const char* what[] = { "semitones", "cents", "delay", "pan", "level", "drift" };
    static const char* suffix[] = { " st", " ct", " ms", " %", " dB", " %" };
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        auto& row = rows[(size_t) v];
        row.label.setText (juce::String (v + 1), juce::dontSendNotification);
        row.label.setColour (juce::Label::textColourId, textColour);
        row.label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (row.label);
        for (size_t k = 0; k < row.sliders.size(); ++k)
        {
            auto& slider = row.sliders[k];
            slider.setSliderStyle (juce::Slider::LinearBar);
            slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 0, 0);
            slider.setTextValueSuffix (suffix[k]);
            slider.setColour (juce::Slider::trackColourId, accent.withAlpha (0.55f));
            const auto id = params::MultivoicerParameters::voiceId (v, what[k]);
            rowAttachments.add (new juce::AudioProcessorValueTreeState::SliderAttachment (processor.parameters, id, tagged (slider, id)));
            addAndMakeVisible (slider);
        }
    }
    refresh();
}

void MultivoicerPanel::refresh()
{
    const auto voices = juce::roundToInt (ampSim.parameters.getRawParameterValue ("mv_voices")->load());
    if (voices == shownVoices)
        return;
    shownVoices = voices;
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        auto& row = rows[(size_t) v];
        const auto alpha = v < voices ? 1.0f : 0.35f;
        row.label.setAlpha (alpha);
        for (auto& slider : row.sliders)
            slider.setAlpha (alpha);
    }
}

void MultivoicerPanel::paint (juce::Graphics& g)
{
    EffectPanel::paint (g);
    // Column headings over the voice table.
    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f));
    auto heads = headerArea;
    heads.removeFromLeft (30);
    const auto width = heads.getWidth() / 6;
    for (const auto* name : { "Interval", "Fine", "Delay", "Pan", "Level", "Drift" })
        g.drawText (name, heads.removeFromLeft (width), juce::Justification::centred);
}

void MultivoicerPanel::resized()
{
    auto area = layoutTitle();
    auto top = area.removeFromTop (26);
    engine->setBounds (top.removeFromLeft (200).reduced (2, 0));
    highPass->setBounds (top.removeFromLeft (140).reduced (4, 0));
    startingPoints.setBounds (top.removeFromRight (150).reduced (2, 0));
    area.removeFromTop (6);
    std::vector<juce::Component*> cells;
    for (auto* knob : knobs)
        cells.push_back (knob);
    layoutGrid (area.removeFromTop (110), cells, 4, 110);
    area.removeFromTop (8);
    headerArea = area.removeFromTop (18);
    const auto rowHeight = juce::jmin (30, area.getHeight() / ampsim::Multivoicer::maxVoices);
    for (auto& row : rows)
    {
        auto line = area.removeFromTop (rowHeight).reduced (0, 2);
        row.label.setBounds (line.removeFromLeft (30));
        const auto width = line.getWidth() / (int) row.sliders.size();
        for (auto& slider : row.sliders)
            slider.setBounds (line.removeFromLeft (width).reduced (2, 0));
    }
}

// ---- Bloom's panels ----------------------------------------------------------------------------------

BitcrushPanel::BitcrushPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Bitcrush", "bloom_crush_on")
{
    addKnob ("bloom_crush_bits", "Bits", " bits");
    addKnob ("bloom_crush_rate", "Rate", " Hz");
    addKnob ("bloom_crush_tone", "Tone", " Hz");
    addKnob ("bloom_crush_mix", "Mix", " %");
    dither = &addToggle ("bloom_crush_dither", "Dither (hiss instead of gated decays)");
}

void BitcrushPanel::resized()
{
    auto area = layoutTitle();
    dither->setBounds (area.removeFromBottom (28));
    std::vector<juce::Component*> cells;
    for (auto* knob : knobs)
        cells.push_back (knob);
    layoutGrid (area, cells, 2, juce::jmin (120, area.getHeight() / 2));
}

PhaserPanel::PhaserPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Phaser", "bloom_phaser_on")
{
    mode = &addCombo ("bloom_phaser_mode", { "Classic", "Modern", "Vibe" });
    stages = &addCombo ("bloom_phaser_stages", { "2 stages", "4 stages", "6 stages", "8 stages", "12 stages" });
    shape = &addCombo ("bloom_phaser_shape", { "Sine", "Triangle" });
    note = &addCombo ("bloom_phaser_note", noteNameList());
    sync = &addToggle ("bloom_phaser_sync", "Sync");
    classicFeedback = &addToggle ("bloom_phaser_classic_fb", "Feedback (later version)");
    rate = &addKnob ("bloom_phaser_rate", "Rate", " Hz");
    depth = &addKnob ("bloom_phaser_depth", "Depth", " %");
    low = &addKnob ("bloom_phaser_low", "Low", " Hz");
    high = &addKnob ("bloom_phaser_high", "High", " Hz");
    feedback = &addKnob ("bloom_phaser_feedback", "Resonance", " %");
    stereo = &addKnob ("bloom_phaser_stereo", "Stereo", " deg");
    mix = &addKnob ("bloom_phaser_mix", "Mix", " %");
}

void PhaserPanel::refresh()
{
    // Classic: fixed 4 stages, triangle, its own range, optional feedback. Modern: everything. Vibe: its
    // own stages, range, and sine.
    const auto currentMode = juce::roundToInt (ampSim.parameters.getRawParameterValue ("bloom_phaser_mode")->load());
    const auto currentSync = ampSim.parameters.getRawParameterValue ("bloom_phaser_sync")->load() >= 0.5f ? 1 : 0;
    if (currentMode != shownMode)
    {
        shownMode = currentMode;
        const auto modern = currentMode == 1;
        for (auto* c : std::initializer_list<juce::Component*> { stages, shape, low, high, feedback })
            c->setAlpha (modern ? 1.0f : 0.35f);
        classicFeedback->setAlpha (currentMode == 0 ? 1.0f : 0.35f);
    }
    if (currentSync != shownSync)
    {
        shownSync = currentSync;
        rate->setVisible (currentSync == 0);
        note->setVisible (currentSync == 1);
    }
}

void PhaserPanel::resized()
{
    auto area = layoutTitle();
    auto row = area.removeFromTop (26);
    const auto third = row.getWidth() / 3;
    mode->setBounds (row.removeFromLeft (third).reduced (2, 0));
    stages->setBounds (row.removeFromLeft (third).reduced (2, 0));
    shape->setBounds (row.reduced (2, 0));
    area.removeFromTop (6);
    auto toggles = area.removeFromTop (26);
    sync->setBounds (toggles.removeFromLeft (80));
    classicFeedback->setBounds (toggles);
    area.removeFromTop (6);
    const auto rowHeight = juce::jmin (115, area.getHeight() / 4);
    layoutGrid (area, { rate, depth, low, high, feedback, stereo, mix }, 2, rowHeight);
    note->setBounds (rate->getBounds().withSizeKeepingCentre (rate->getWidth() - 12, 26));
}

FlangerPanel::FlangerPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Flanger", "bloom_flanger_on")
{
    shape = &addCombo ("bloom_flanger_shape", { "Triangle", "Sine", "Random" });
    note = &addCombo ("bloom_flanger_note", noteNameList());
    sync = &addToggle ("bloom_flanger_sync", "Sync");
    negative = &addToggle ("bloom_flanger_negative", "Negative");
    throughZero = &addToggle ("bloom_flanger_tz", "Through-zero (+5 ms latency)");
    manual = &addKnob ("bloom_flanger_manual", "Manual", " ms");
    depth = &addKnob ("bloom_flanger_depth", "Depth", " %");
    rate = &addKnob ("bloom_flanger_rate", "Rate", " Hz");
    feedback = &addKnob ("bloom_flanger_feedback", "Feedback", " %");
    stereo = &addKnob ("bloom_flanger_stereo", "Stereo", " deg");
    mix = &addKnob ("bloom_flanger_mix", "Mix", " %");
}

void FlangerPanel::refresh()
{
    const auto currentSync = ampSim.parameters.getRawParameterValue ("bloom_flanger_sync")->load() >= 0.5f ? 1 : 0;
    if (currentSync != shownSync)
    {
        shownSync = currentSync;
        rate->setVisible (currentSync == 0);
        note->setVisible (currentSync == 1);
    }
}

void FlangerPanel::resized()
{
    auto area = layoutTitle();
    shape->setBounds (area.removeFromTop (26).reduced (2, 0));
    area.removeFromTop (6);
    auto toggles = area.removeFromTop (26);
    sync->setBounds (toggles.removeFromLeft (80));
    negative->setBounds (toggles.removeFromLeft (100));
    throughZero->setBounds (area.removeFromTop (26));
    area.removeFromTop (6);
    layoutGrid (area, { manual, depth, rate, feedback, stereo, mix }, 2, juce::jmin (115, area.getHeight() / 3));
    note->setBounds (rate->getBounds().withSizeKeepingCentre (rate->getWidth() - 12, 26));
}

// ---- BoostPanel, OverdrivePanel -----------------------------------------------------------------

BoostPanel::BoostPanel (AmpSimProcessor& processor) : EffectPanel (processor, "Boost", "boost_on")
{
    mode = &addCombo ("boost_mode", { "Clean", "Tight", "Screamer" });
    level = &addKnob ("boost_level", "Level");
    tilt = &addKnob ("boost_tilt", "Tilt");
    tightHz = &addKnob ("boost_tight_freq", "Tight", " Hz");
    mid = &addKnob ("boost_mid", "Mid push");
}

void BoostPanel::refresh()
{
    // Clean uses the tilt, Tight the tight frequency and the mid push; Screamer only the level.
    const auto current = juce::roundToInt (ampSim.parameters.getRawParameterValue ("boost_mode")->load());
    if (current == shownMode)
        return;
    shownMode = current;
    tilt->setAlpha (current == 0 ? 1.0f : 0.35f);
    tightHz->setAlpha (current == 1 ? 1.0f : 0.35f);
    mid->setAlpha (current == 1 ? 1.0f : 0.35f);
}

void BoostPanel::resized()
{
    auto area = layoutTitle();
    mode->setBounds (area.removeFromTop (26).reduced (2, 0));
    area.removeFromTop (10);
    layoutGrid (area, { level, tilt, tightHz, mid }, 2, juce::jmin (120, area.getHeight() / 2));
}

OverdrivePanel::OverdrivePanel (AmpSimProcessor& processor) : EffectPanel (processor, "Overdrive", "od_on")
{
    mode = &addCombo ("od_mode", params::OverdriveParameters::modeNames());
    drive = &addKnob ("od_drive", "Drive", " %");
    tone = &addKnob ("od_tone", "Tone", " %");
    level = &addKnob ("od_level", "Level");
    mix = &addKnob ("od_mix", "Mix", " %");
    tight = &addToggle ("od_tight", "Tight");
    tightHz = &addKnob ("od_tight_freq", "Tight", " Hz");
    oversampling = &addCombo ("drive_oversampling", { "4x oversampling (boost and overdrive)", "8x oversampling (boost and overdrive)" });
}

void OverdrivePanel::refresh()
{
    const auto on = ampSim.parameters.getRawParameterValue ("od_tight")->load() >= 0.5f ? 1 : 0;
    if (on == shownTight)
        return;
    shownTight = on;
    tightHz->setAlpha (on == 1 ? 1.0f : 0.35f);
}

void OverdrivePanel::resized()
{
    auto area = layoutTitle();
    mode->setBounds (area.removeFromTop (26).reduced (2, 0));
    area.removeFromTop (10);
    oversampling->setBounds (area.removeFromBottom (26).reduced (2, 0));
    const auto rowHeight = juce::jmin (120, area.getHeight() / 3);
    layoutGrid (area.removeFromTop (2 * rowHeight), { drive, tone, level, mix }, 2, rowHeight);
    auto tightRow = area.removeFromTop (rowHeight);
    tight->setBounds (tightRow.removeFromLeft (tightRow.getWidth() / 2).withSizeKeepingCentre (tightRow.getWidth() / 2 - 8, 26));
    tightHz->setBounds (tightRow);
}

// ---- DelayPanel -------------------------------------------------------------------------------

DelayPanel::DelayPanel (AmpSimProcessor& processor) : ampSim (processor)
{
    auto& state = processor.parameters;
    stylePanelTitle (titleLabel, "Delay");
    addAndMakeVisible (titleLabel);

    for (auto [button, id] : std::initializer_list<std::pair<ToggleControl*, const char*>> { { &onButton, "delay_on" }, { &syncButton, "delay_sync" } })
    {
        buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, id, tagged (*button, id)));
        addAndMakeVisible (button);
    }

    juce::StringArray notes;
    for (const auto& n : ampsim::tempo::notes)
        notes.add (n.name);
    for (auto [box, id, items] : std::initializer_list<std::tuple<juce::ComboBox*, const char*, juce::StringArray>> {
             { &mode, "delay_mode", { "Digital", "Analog", "Tape" } }, { &stereo, "delay_stereo", { "Stereo", "Ping-pong", "Dual" } },
             { &note, "delay_note", notes }, { &rightNote, "delay_note_right", notes } })
    {
        comboAttachments.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (state, id, tagged (withItems (*box, items), id)));
        addAndMakeVisible (box);
    }
    for (auto [label, text] : std::initializer_list<std::pair<juce::Label*, const char*>> { { &noteLabel, "Time" }, { &rightNoteLabel, "Right time" } })
    {
        label->setText (text, juce::dontSendNotification);
        label->setJustificationType (juce::Justification::centred);
        label->setColour (juce::Label::textColourId, dimText);
        addAndMakeVisible (label);
    }

    time = std::make_unique<Knob> (state, "delay_time", "Time", " ms");
    rightTime = std::make_unique<Knob> (state, "delay_time_right", "Right time", " ms");
    offset = std::make_unique<Knob> (state, "delay_offset", "R offset", " ms");
    feedback = std::make_unique<Knob> (state, "delay_feedback", "Feedback", " %");
    lowCut = std::make_unique<Knob> (state, "delay_lowcut", "Low cut", " Hz");
    highCut = std::make_unique<Knob> (state, "delay_highcut", "High cut", " Hz");
    modDepth = std::make_unique<Knob> (state, "delay_mod_depth", "Mod depth", " ms");
    modRate = std::make_unique<Knob> (state, "delay_mod_rate", "Mod rate", " Hz");
    duck = std::make_unique<Knob> (state, "delay_duck", "Ducking");
    mix = std::make_unique<Knob> (state, "delay_mix", "Mix", " %");
    tempo = std::make_unique<Knob> (state, "tempo_bpm", "Tempo", " BPM");
    for (auto* k : { time.get(), rightTime.get(), offset.get(), feedback.get(), lowCut.get(), highCut.get(), modDepth.get(), modRate.get(), duck.get(), mix.get(), tempo.get() })
        addAndMakeVisible (k);

    tapButton.setTooltip ("Tap the tempo (or use the footswitch's tap CC)");
    tapButton.onClick = [this] { ampSim.tapTempo(); };
    addAndMakeVisible (tapButton);
    refresh();
}

void DelayPanel::refresh()
{
    auto& state = ampSim.parameters;
    const bool synced = state.getRawParameterValue ("delay_sync")->load() >= 0.5f;
    const auto layout = juce::roundToInt (state.getRawParameterValue ("delay_stereo")->load());
    const auto newState = (synced ? 1 : 0) + 2 * layout;
    if (newState == shownState)
        return;

    shownState = newState;
    note.setVisible (synced);
    noteLabel.setVisible (synced);
    time->setVisible (! synced);
    const bool dual = layout == 2;
    rightNote.setVisible (synced && dual);
    rightNoteLabel.setVisible (synced && dual);
    rightTime->setVisible (! synced && dual);
    offset->setVisible (layout == 0);
}

void DelayPanel::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
}

void DelayPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    onButton.setBounds (top.removeFromRight (60));
    titleLabel.setBounds (top);
    area.removeFromTop (6);
    auto combos = area.removeFromTop (26);
    mode.setBounds (combos.removeFromLeft (combos.getWidth() / 2).reduced (2, 0));
    stereo.setBounds (combos.reduced (2, 0));
    area.removeFromTop (6);
    syncButton.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);

    // Row 1: time (or note), right time (or note) / offset, feedback, mix.
    const auto rowHeight = area.getHeight() / 3;
    auto row = area.removeFromTop (rowHeight);
    const auto width = row.getWidth() / 4;
    auto cell = row.removeFromLeft (width);
    time->setBounds (cell);
    noteLabel.setBounds (cell.removeFromTop (18));
    note.setBounds (cell.withSizeKeepingCentre (cell.getWidth() - 8, 26));
    cell = row.removeFromLeft (width);
    rightTime->setBounds (cell);
    offset->setBounds (cell);
    rightNoteLabel.setBounds (cell.removeFromTop (18));
    rightNote.setBounds (cell.withSizeKeepingCentre (cell.getWidth() - 8, 26));
    feedback->setBounds (row.removeFromLeft (width));
    mix->setBounds (row);

    row = area.removeFromTop (rowHeight);
    for (auto* k : { lowCut.get(), highCut.get(), modDepth.get(), modRate.get() })
        k->setBounds (row.removeFromLeft (width));

    row = area;
    duck->setBounds (row.removeFromLeft (width));
    tempo->setBounds (row.removeFromLeft (width));
    tapButton.setBounds (row.removeFromLeft (width).withSizeKeepingCentre (width - 16, 32));
}

// ---- AmpSimEditor -----------------------------------------------------------------------------

// ---- TunerView ----------------------------------------------------------------------------------

namespace
{
const juce::Identifier tunerDisplayKey { "tunerDisplay" };
const auto inTuneColour = juce::Colour (0xff4cd98a);
} // namespace

TunerView::TunerView (AmpSimProcessor& processor)
    : ampSim (processor),
      a4Knob (processor.parameters, "tuner_a4", "A4", " Hz"),
      muteAttachment (processor.parameters, "tuner_mute", tagged (muteButton, "tuner_mute"))
{
    setOpaque (true);
    for (auto* button : { &needleButton, &strobeButton })
    {
        button->setClickingTogglesState (false);
        button->setColour (juce::TextButton::buttonOnColourId, accent);
        addAndMakeVisible (button);
    }
    needleButton.onClick = [this] { setStrobe (false); };
    strobeButton.onClick = [this] { setStrobe (true); };
    closeButton.onClick = [this]
    {
        if (auto* tunerSwitch = ampSim.parameters.getParameter ("tuner_on"))
            tunerSwitch->setValueNotifyingHost (0.0f);
    };
    addAndMakeVisible (closeButton);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (a4Knob);
    setStrobe (ampSim.parameters.state.getProperty (tunerDisplayKey).toString() == "strobe");
}

void TunerView::setStrobe (bool shouldShowStrobe)
{
    strobe = shouldShowStrobe;
    ampSim.parameters.state.setProperty (tunerDisplayKey, strobe ? "strobe" : "needle", nullptr);
    needleButton.setToggleState (! strobe, juce::dontSendNotification);
    strobeButton.setToggleState (strobe, juce::dontSendNotification);
    repaint();
}

void TunerView::visibilityChanged()
{
    if (isVisible())
        startTimerHz (60);
    else
        stopTimer();
}

void TunerView::timerCallback()
{
    if (! frozen)
        reading = ampSim.getTunerReading();
    repaint (displayArea);
}

void TunerView::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);

    // In tune within 1 cent: green; within 5: amber; further: red. Grey while holding a decayed note.
    const auto offset = std::abs (reading.cents);
    const auto colour = ! reading.hasReading || ! reading.live ? dimText
                        : offset < 1.0                         ? inTuneColour
                        : offset < 5.0                         ? warningColour
                                                               : errorColour;
    auto area = displayArea.toFloat();
    auto footer = area.removeFromBottom (22.0f);
    auto top = area.removeFromTop (area.getHeight() * 0.40f);

    g.setColour (reading.hasReading ? (reading.live ? textColour : dimText) : dimText);
    g.setFont (juce::FontOptions (96.0f, juce::Font::bold));
    g.drawText (reading.hasReading ? juce::MidiMessage::getMidiNoteName (reading.midiNote, true, true, 4) : juce::String ("--"),
                top.removeFromTop (top.getHeight() * 0.72f), juce::Justification::centred);

    g.setFont (juce::FontOptions (20.0f));
    const auto sign = reading.cents >= 0.0 ? "+" : "";
    g.drawText (reading.hasReading ? juce::String (reading.frequency, 2) + " Hz      " + sign + juce::String (reading.cents, 1) + " cents"
                                         + (reading.live ? "" : "      (holding)")
                                   : juce::String ("Play one string"),
                top, juce::Justification::centred);

    if (strobe)
        paintStrobe (g, area.reduced (60.0f, 30.0f), colour);
    else
        paintNeedle (g, area.reduced (60.0f, 6.0f), colour);

    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Input " + (reading.levelDb > -200.0 ? juce::String (juce::roundToInt (reading.levelDb)) + " dBFS" : juce::String ("silent"))
                    + "      A4 = " + juce::String (reading.referenceA4, 1) + " Hz",
                footer, juce::Justification::centred);
}

void TunerView::paintNeedle (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour) const
{
    // A 120 degree arc for -50 to +50 cents, angles clockwise from straight up.
    const juce::Point<float> centre { area.getCentreX(), area.getBottom() - 8.0f };
    const auto radius = std::min (area.getWidth() * 0.5f, area.getHeight() - 16.0f);
    const auto angleFor = [] (double cents) { return (float) (juce::jlimit (-50.0, 50.0, cents) / 50.0 * juce::MathConstants<double>::pi / 3.0); };
    const auto towards = [&] (float angle, float r) { return centre + juce::Point<float> (std::sin (angle), -std::cos (angle)) * r; };

    juce::Path zone; // the in-tune zone, +-1 cent
    zone.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, angleFor (-1.0), angleFor (1.0), true);
    g.setColour (inTuneColour.withAlpha (0.5f));
    g.strokePath (zone, juce::PathStrokeType (10.0f));

    g.setFont (juce::FontOptions (12.0f));
    for (int c = -50; c <= 50; c += 5)
    {
        const auto major = c % 10 == 0;
        g.setColour (c == 0 ? textColour : dimText);
        g.drawLine ({ towards (angleFor (c), radius * (major ? 0.86f : 0.92f)), towards (angleFor (c), radius) }, c == 0 ? 3.0f : (major ? 2.0f : 1.0f));
        if (major)
        {
            const auto at = towards (angleFor (c), radius * 0.77f);
            g.drawText ((c > 0 ? "+" : "") + juce::String (c), juce::Rectangle<float> (40.0f, 16.0f).withCentre (at), juce::Justification::centred);
        }
    }

    if (reading.hasReading)
    {
        g.setColour (colour);
        g.drawLine ({ centre, towards (angleFor (reading.cents), radius * 0.97f) }, 4.0f);
    }
    g.setColour (textColour);
    g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (centre));
}

void TunerView::paintStrobe (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour) const
{
    // Stripes that drift by the analysis's strobe phase: still when in tune, rightwards when sharp, at a
    // quarter of a stripe per second per cent. Two bands, the lower with stripes half as wide, as on a
    // mechanical strobe, so small offsets are easier to see.
    const auto bands = { std::pair<float, float> { 0.0f, 64.0f }, std::pair<float, float> { 0.5f, 32.0f } };
    for (const auto& [fromTop, period] : bands)
    {
        auto band = area.withTrimmedTop (area.getHeight() * fromTop).withHeight (area.getHeight() * 0.42f);
        g.setColour (background);
        g.fillRect (band);
        g.saveState();
        g.reduceClipRegion (band.toNearestInt());
        g.setColour (colour);
        const auto offset = (float) reading.strobePhase * period;
        for (auto x = band.getX() - period + offset; x < band.getRight(); x += period)
            g.fillRect (juce::Rectangle<float> (x, band.getY(), period * 0.5f, band.getHeight()));
        g.restoreState();
    }
    g.setColour (textColour);
    g.drawLine (area.getCentreX(), area.getY() - 6.0f, area.getCentreX(), area.getBottom() + 6.0f, 2.0f);
}

void TunerView::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto controls = area.removeFromRight (180);
    auto modeRow = controls.removeFromTop (30);
    needleButton.setBounds (modeRow.removeFromLeft (modeRow.getWidth() / 2).reduced (2, 0));
    strobeButton.setBounds (modeRow.reduced (2, 0));
    controls.removeFromTop (16);
    a4Knob.setBounds (controls.removeFromTop (120));
    controls.removeFromTop (8);
    muteButton.setBounds (controls.removeFromTop (28));
    closeButton.setBounds (controls.removeFromBottom (34).reduced (2, 0));
    displayArea = area.reduced (8);
}

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p)
    : AudioProcessorEditor (&p),
      ampSim (p),
      inputKnob (p.parameters, "input_gain", "Input"),
      outputKnob (p.parameters, "output_gain", "Output"),
      calibrateAttachment (p.parameters, "input_calibrate", tagged (calibrateButton, "input_calibrate")),
      interfaceLevelAttachment (p.parameters, "input_level_dbu", tagged (interfaceLevel, "input_level_dbu")),
      lowCutKnob (p.parameters, "cab_lowcut_freq", "Low cut", " Hz"),
      highCutKnob (p.parameters, "cab_highcut_freq", "High cut", " Hz"),
      alignAttachment (p.parameters, "cab_align", tagged (alignButton, "cab_align")),
      lowCutAttachment (p.parameters, "cab_lowcut_on", tagged (lowCutButton, "cab_lowcut_on")),
      highCutAttachment (p.parameters, "cab_highcut_on", tagged (highCutButton, "cab_highcut_on")),
      cabBypassAttachment (p.parameters, "cab_bypass", tagged (cabBypassButton, "cab_bypass")),
      lowCutSlopeAttachment (p.parameters, "cab_lowcut_slope", tagged (withItems (lowCutSlope, { "12 dB/oct", "24 dB/oct" }), "cab_lowcut_slope")),
      highCutSlopeAttachment (p.parameters, "cab_highcut_slope", tagged (withItems (highCutSlope, { "12 dB/oct", "24 dB/oct" }), "cab_highcut_slope"))
{
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        auto& button = slotButtons[(size_t) s];
        button.setButtonText ("Amp " + juce::String (s + 1));
        button.setColour (juce::TextButton::buttonOnColourId, accent);
        button.onClick = [this, s]
        {
            if (auto* param = ampSim.parameters.getParameter (AmpSimProcessor::slotParamId))
                param->setValueNotifyingHost (param->convertTo0to1 ((float) s));
        };
        addAndMakeVisible (button);

        slotPanels.add (new SlotPanel (p, s, [this, s]
        {
            chooseFile ("Choose a NAM capture for Amp " + juce::String (s + 1), "*.nam", AmpSimProcessor::modelPathKey (s),
                        [this, s] (const juce::File& f) { ampSim.loadModel (s, f); });
        }));
        ampsPage.addAndMakeVisible (slotPanels.getLast());
    }

    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        micPanels.add (new MicPanel (p, m, [this, m]
        {
            chooseFile ("Choose an impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::cabPathKey (m),
                        [this, m] (const juce::File& f) { ampSim.loadCabIR (m, f); });
        },
        [this, m]
        {
            chooseFile ("Choose a cab pack folder", {}, AmpSimProcessor::cabPathKey (m),
                        [this, m] (const juce::File& f) { ampSim.loadCabIR (m, f); }, true);
        }));
        cabPage.addAndMakeVisible (micPanels.getLast());
    }

    alignmentLabel.setColour (juce::Label::textColourId, dimText);
    for (auto* c : std::initializer_list<juce::Component*> { &alignButton, &alignmentLabel, &lowCutButton, &lowCutKnob, &lowCutSlope,
                                                              &highCutButton, &highCutKnob, &highCutSlope, &cabBypassButton })
        cabPage.addAndMakeVisible (c);

    addAndMakeVisible (inputKnob);
    addAndMakeVisible (outputKnob);

    presetLabel.setColour (juce::Label::textColourId, textColour);
    presetLabel.setFont (juce::FontOptions (14.0f));
    addAndMakeVisible (presetLabel);
    savePresetButton.onClick = [this] { savePreset(); };
    loadPresetButton.onClick = [this] { loadPreset(); };
    midiButton.onClick = [this] { showMidiMappings(); };
    tunerButton.setClickingTogglesState (true);
    tunerButton.setColour (juce::TextButton::buttonOnColourId, accent);
    tunerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.parameters, "tuner_on", tagged (tunerButton, "tuner_on"));
    addAndMakeVisible (tunerButton);
    midiButton.setTooltip ("MIDI mappings. Right-click any control to learn one.");
    addAndMakeVisible (savePresetButton);
    addAndMakeVisible (loadPresetButton);
    addAndMakeVisible (midiButton);
    addMouseListener (this, true); // right-clicks anywhere in the panel, for MIDI learn; transactions for undo

    undoButton.onClick = [this] { ampSim.parameters.copyState(); ampSim.undoManager.undo(); };
    redoButton.onClick = [this] { ampSim.undoManager.redo(); };
    abButton.onClick = [this] { ampSim.parameters.copyState(); ampSim.abSwitch(); timerCallback(); };
    abButton.setTooltip ("Switch between two versions of the settings (captures and IRs stay)");
    abCopyButton.onClick = [this] { ampSim.abCopyToOther(); };
    for (auto* b : { &undoButton, &redoButton, &abButton, &abCopyButton })
        addAndMakeVisible (b);
    setWantsKeyboardFocus (true);

    scenesLabel.setText ("Scenes", juce::dontSendNotification);
    scenesLabel.setColour (juce::Label::textColourId, dimText);
    addAndMakeVisible (scenesLabel);
    for (int i = 0; i < Scenes::count; ++i)
    {
        auto& button = sceneButtons[(size_t) i];
        button.setButtonText (juce::String (i + 1));
        button.setColour (juce::TextButton::buttonOnColourId, accent);
        button.onClick = [this, i] { clickScene (i); };
        addAndMakeVisible (button);
    }
    storeSceneButton.setClickingTogglesState (true);
    storeSceneButton.setColour (juce::TextButton::buttonOnColourId, warningColour.darker (0.3f));
    storeSceneButton.setTooltip ("Then click a scene to store the current sound in it");
    addAndMakeVisible (storeSceneButton);

    warningLabel.setColour (juce::Label::textColourId, warningColour);
    warningLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (warningLabel);

    interfaceLevel.setTextValueSuffix (" dBu");
    interfaceLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
    interfaceLevelLabel.setText ("Interface level at 0 dBFS", juce::dontSendNotification);
    interfaceLevelLabel.setColour (juce::Label::textColourId, dimText);
    calibrationHint.setText ("Solo 4th Gen instrument input at minimum gain: +12 dBu. Captures without a recorded level aren't changed.",
                             juce::dontSendNotification);
    calibrationHint.setColour (juce::Label::textColourId, dimText);
    calibrationHint.setFont (juce::FontOptions (12.0f));
    for (auto* c : std::initializer_list<juce::Component*> { &calibrateButton, &interfaceLevelLabel, &interfaceLevel, &calibrationHint })
        ampsPage.addAndMakeVisible (c);

    ampsPage.layout = [this] (juce::Rectangle<int> amps)
    {
        amps.reduce (8, 8);

        // Bottom: the input calibration.
        auto calibration = amps.removeFromBottom (52).reduced (4, 0);
        auto row = calibration.removeFromTop (28);
        calibrateButton.setBounds (row.removeFromLeft (250));
        interfaceLevelLabel.setBounds (row.removeFromLeft (170));
        interfaceLevel.setBounds (row.removeFromLeft (300));
        calibrationHint.setBounds (calibration);

        // Three slot panels side by side.
        const auto panelWidth = amps.getWidth() / AmpSimProcessor::numAmpSlots;
        for (auto* slotPanel : slotPanels)
            slotPanel->setBounds (amps.removeFromLeft (panelWidth).reduced (4));
    };

    cabPage.layout = [this] (juce::Rectangle<int> cab)
    {
        cab.reduce (8, 8);

        // Bottom: alignment, cuts, bypass.
        auto global = cab.removeFromBottom (150).reduced (4);
        auto alignRow = global.removeFromTop (26);
        alignButton.setBounds (alignRow.removeFromLeft (190));
        alignmentLabel.setBounds (alignRow);
        global.removeFromTop (4);
        lowCutButton.setBounds (global.removeFromLeft (90).withSizeKeepingCentre (90, 26));
        lowCutKnob.setBounds (global.removeFromLeft (90));
        lowCutSlope.setBounds (global.removeFromLeft (110).withSizeKeepingCentre (110, 26));
        global.removeFromLeft (20);
        highCutButton.setBounds (global.removeFromLeft (90).withSizeKeepingCentre (90, 26));
        highCutKnob.setBounds (global.removeFromLeft (90));
        highCutSlope.setBounds (global.removeFromLeft (110).withSizeKeepingCentre (110, 26));
        cabBypassButton.setBounds (global.removeFromRight (120).withSizeKeepingCentre (120, 26));

        // Top: the three mics.
        const auto panelWidth = cab.getWidth() / AmpSimProcessor::numCabMics;
        for (auto* micPanel : micPanels)
            micPanel->setBounds (cab.removeFromLeft (panelWidth).reduced (4));
    };

    // Pre FX and Post FX: the section's order, its compressor, and its EQ.
    using Section = ampsim::Chain::Section;
    preOrder = std::make_unique<OrderStrip> ("Order (before the amp):", [&p] { return p.getSectionOrder (Section::pre); },
                                             [&p] (const juce::StringArray& order) { p.setSectionOrder (Section::pre, order); });
    postOrder = std::make_unique<OrderStrip> ("Order (after the cab):", [&p] { return p.getSectionOrder (Section::post); },
                                              [&p] (const juce::StringArray& order) { p.setSectionOrder (Section::post, order); });
    preComp = std::make_unique<CompressorPanel> (p, "comp_pre", "Compressor", false);
    postComp = std::make_unique<CompressorPanel> (p, "comp_post", "Compressor", true);
    preEq = std::make_unique<EqualizerPanel> (p, "eq_pre", "EQ (before the amp)");
    postEq = std::make_unique<EqualizerPanel> (p, "eq_post", "EQ (after the cab)");

    for (auto [page, order, comp, eq] : std::initializer_list<std::tuple<PageComponent*, OrderStrip*, CompressorPanel*, EqualizerPanel*>> {
             { &preFxPage, preOrder.get(), preComp.get(), preEq.get() }, { &postFxPage, postOrder.get(), postComp.get(), postEq.get() } })
    {
        page->addAndMakeVisible (order);
        page->addAndMakeVisible (comp);
        page->addAndMakeVisible (eq);
        page->layout = [order, comp, eq] (juce::Rectangle<int> area)
        {
            area.reduce (8, 8);
            order->setBounds (area.removeFromTop (34));
            area.removeFromTop (6);
            comp->setBounds (area.removeFromLeft (380).reduced (4));
            eq->setBounds (area.reduced (4));
        };
    }

    // Gates & Drive, in signal order: Gate A, the boost, and the overdrive before the amp; Gate B after it.
    // (The pre section can be reordered on the Pre FX tab; this page keeps the default order.)
    gateAPanel = std::make_unique<GatePanel> (p, false);
    gateBPanel = std::make_unique<GatePanel> (p, true);
    boostPanel = std::make_unique<BoostPanel> (p);
    overdrivePanel = std::make_unique<OverdrivePanel> (p);
    for (auto* c : std::initializer_list<juce::Component*> { gateAPanel.get(), boostPanel.get(), overdrivePanel.get(), gateBPanel.get() })
        gatesPage.addAndMakeVisible (c);
    gatesPage.layout = [this] (juce::Rectangle<int> area)
    {
        area.reduce (8, 8);
        const auto width = area.getWidth() / 4;
        for (auto* c : std::initializer_list<juce::Component*> { gateAPanel.get(), boostPanel.get(), overdrivePanel.get() })
            c->setBounds (area.removeFromLeft (width).reduced (4));
        gateBPanel->setBounds (area.reduced (4));
    };

    tabs.addTab ("Amps", panel, &ampsPage, false);
    tabs.addTab ("Cab", panel, &cabPage, false);
    tabs.addTab ("Gates & Drive", panel, &gatesPage, false);
    tabs.addTab ("Pre FX", panel, &preFxPage, false);
    tabs.addTab ("Post FX", panel, &postFxPage, false);

    // Pitch: the multivoicer (and, in Phase 10, the harmonizer).
    multivoicerPanel = std::make_unique<MultivoicerPanel> (p);
    harmonizerPanel = std::make_unique<HarmonizerPanel> (p);
    pitchPage.addAndMakeVisible (multivoicerPanel.get());
    pitchPage.addAndMakeVisible (harmonizerPanel.get());
    pitchPage.layout = [this] (juce::Rectangle<int> area)
    {
        area.reduce (8, 8);
        harmonizerPanel->setBounds (area.removeFromLeft (area.getWidth() / 2).reduced (4));
        multivoicerPanel->setBounds (area.reduced (4));
    };
    tabs.addTab ("Pitch", panel, &pitchPage, false);

    // Bloom: the container's switch, mix, and order, then its three effects side by side.
    bloomOrder = std::make_unique<OrderStrip> ("Order inside Bloom:", [&p] { return p.getBloomOrder(); },
                                               [&p] (const juce::StringArray& order) { p.setBloomOrder (order); });
    bitcrushPanel = std::make_unique<BitcrushPanel> (p);
    phaserPanel = std::make_unique<PhaserPanel> (p);
    flangerPanel = std::make_unique<FlangerPanel> (p);
    bloomOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.parameters, "bloom_on", tagged (bloomOnButton, "bloom_on"));
    bloomMixKnob = std::make_unique<Knob> (p.parameters, "bloom_mix", "Bloom mix", " %");
    bloomLatencyLabel.setColour (juce::Label::textColourId, warningColour);
    for (auto* c : std::initializer_list<juce::Component*> { bloomOrder.get(), bitcrushPanel.get(), phaserPanel.get(), flangerPanel.get(), &bloomOnButton,
                                                             bloomMixKnob.get(), &bloomLatencyLabel })
        bloomPage.addAndMakeVisible (c);
    bloomPage.layout = [this] (juce::Rectangle<int> area)
    {
        area.reduce (8, 8);
        auto top = area.removeFromTop (100);
        bloomOnButton.setBounds (top.removeFromLeft (120).withSizeKeepingCentre (120, 28));
        bloomMixKnob->setBounds (top.removeFromLeft (100));
        top.removeFromLeft (12);
        bloomLatencyLabel.setBounds (top.removeFromBottom (24));
        bloomOrder->setBounds (top.withSizeKeepingCentre (top.getWidth(), 34));
        area.removeFromTop (6);
        const auto width = area.getWidth() / 3;
        bitcrushPanel->setBounds (area.removeFromLeft (width).reduced (4));
        phaserPanel->setBounds (area.removeFromLeft (width).reduced (4));
        flangerPanel->setBounds (area.reduced (4));
    };
    tabs.addTab ("Bloom", panel, &bloomPage, false);

    // Time FX: chorus, delay, and reverb side by side, in their default order.
    chorusPanel = std::make_unique<ChorusPanel> (p);
    delayPanel = std::make_unique<DelayPanel> (p);
    reverbPanel = std::make_unique<ReverbPanel> (p);
    for (auto* c : std::initializer_list<juce::Component*> { chorusPanel.get(), delayPanel.get(), reverbPanel.get() })
        timeFxPage.addAndMakeVisible (c);
    timeFxPage.layout = [this] (juce::Rectangle<int> area)
    {
        area.reduce (8, 8);
        const auto width = area.getWidth() / 3;
        chorusPanel->setBounds (area.removeFromLeft (width).reduced (4));
        delayPanel->setBounds (area.removeFromLeft (width).reduced (4));
        reverbPanel->setBounds (area.reduced (4));
    };
    tabs.addTab ("Time FX", panel, &timeFxPage, false);
    tabs.setTabBarDepth (30);
    addAndMakeVisible (tabs);
    addChildComponent (tunerView); // over the tabs while the tuner is engaged

    setSize (1280, 820);
    timerCallback(); // show the current state right away
    startTimerHz (10);
}

AmpSimEditor::~AmpSimEditor()
{
    stopTimer();
}

void AmpSimEditor::chooseFile (const juce::String& title, const juce::String& patterns,
                               const juce::Identifier& lastPathKey,
                               std::function<void (const juce::File&)> onChosen, bool folders)
{
    // Start in the folder of the last file loaded, if there is one.
    const auto lastPath = ampSim.parameters.state.getProperty (lastPathKey).toString();
    const auto start = juce::File::isAbsolutePath (lastPath)
                           ? juce::File (lastPath).getParentDirectory()
                           : juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
    const auto flags = juce::FileBrowserComponent::openMode
                       | (folders ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles);
    chooser->launchAsync (flags, [onChosen, folders] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (folders ? file.isDirectory() : file.existsAsFile())
                                  onChosen (file);
                          });
}

void AmpSimEditor::savePreset()
{
    const auto folder = presets::defaultFolder();
    folder.createDirectory();
    const auto name = ampSim.getPresetName();
    chooser = std::make_unique<juce::FileChooser> ("Save preset", folder.getChildFile ((name.isEmpty() ? juce::String ("My tone") : name) + ".json"), "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              file = file.withFileExtension ("json");
                              const auto preset = ampSim.capturePreset (file.getFileNameWithoutExtension());
                              presetMessage = presets::save (preset, file) ? juce::String() : "Couldn't write " + file.getFullPathName();
                              ampSim.parameters.state.setProperty ("presetName", file.getFileNameWithoutExtension(), nullptr);
                              timerCallback();
                          });
}

void AmpSimEditor::loadPreset()
{
    const auto folder = presets::defaultFolder();
    folder.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Load preset", folder, "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (! file.existsAsFile())
                                  return;
                              juce::String error;
                              const auto preset = presets::load (file, error);
                              const auto result = error.isEmpty() ? ampSim.loadPreset (preset) : presets::ApplyResult { false, error, {} };
                              presetMessage = result.ok ? juce::String() : result.error;
                              timerCallback();
                          });
}

void AmpSimEditor::timerCallback()
{
    const auto status = ampSim.getStatus();
    const auto active = juce::roundToInt (ampSim.parameters.getRawParameterValue (AmpSimProcessor::slotParamId)->load());

    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        slotButtons[(size_t) s].setToggleState (s == active, juce::dontSendNotification);
        slotPanels[s]->setActive (s == active);
        slotPanels[s]->setStatus (status.model[(size_t) s], status.modelError[(size_t) s]);
    }

    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        micPanels[m]->setStatus (status.cab[(size_t) m], status.cabError[(size_t) m]);
        if (m != AmpSimProcessor::roomMic)
            micPanels[m]->setPackPoints (ampSim.getCabPackPoints (m));
    }

    alignmentLabel.setText (status.alignment, juce::dontSendNotification);

    // The warning line: the sample rate first, then MIDI learn waiting for a controller, then a preset
    // that couldn't load or loaded with gaps.
    auto warning = status.warning;
    if (const auto& midi = ampSim.getMidiMap(); warning.isEmpty() && midi.isLearning())
        if (auto* parameter = ampSim.parameters.getParameter (midi.getLearnTarget()))
            warning = "MIDI learn: press a footswitch or move a pedal for " + parameter->getName (64) + " (right-click it again to cancel)";
    if (warning.isEmpty())
        warning = presetMessage.isNotEmpty() ? presetMessage : ampSim.getPresetWarnings().joinIntoString ("; ");
    warningLabel.setText (warning, juce::dontSendNotification);

    const auto presetName = ampSim.getPresetName();
    presetLabel.setText ("Preset: " + (presetName.isEmpty() ? juce::String ("(unsaved)") : presetName)
                             + (ampSim.isChangingPreset() ? "  (loading...)" : ""),
                         juce::dontSendNotification);

    for (auto* strip : { preOrder.get(), postOrder.get() })
        strip->refresh();
    for (auto* comp : { preComp.get(), postComp.get() })
        comp->refresh();
    for (auto* eq : { preEq.get(), postEq.get() })
        eq->refresh();
    delayPanel->refresh();
    chorusPanel->refresh();
    reverbPanel->refresh();
    bloomOrder->refresh();
    multivoicerPanel->refresh();
    harmonizerPanel->refresh();
    phaserPanel->refresh();
    flangerPanel->refresh();
    bloomLatencyLabel.setText (ampSim.getLatencySamples() > 0 ? "Through-zero flanging adds " + juce::String (ampSim.getLatencySamples() * 1000.0 / 48000.0, 1)
                                                                    + " ms of latency while it's on"
                                                              : juce::String(),
                               juce::dontSendNotification);
    gateAPanel->refresh();
    gateBPanel->refresh();
    boostPanel->refresh();
    overdrivePanel->refresh();

    // Scene buttons: stored ones bright, empty ones faint, the current one lit.
    for (int i = 0; i < Scenes::count; ++i)
    {
        const auto& scene = ampSim.getScenes().get (i);
        auto& button = sceneButtons[(size_t) i];
        button.setAlpha (scene.stored ? 1.0f : 0.45f);
        button.setToggleState (scene.stored && ampSim.getScenes().getCurrent() == i, juce::dontSendNotification);
        button.setTooltip (scene.stored ? scene.name : "Empty: click to store the current sound");
    }

    undoButton.setEnabled (ampSim.undoManager.canUndo());
    redoButton.setEnabled (ampSim.undoManager.canRedo());
    abButton.setButtonText (ampSim.isOnB() ? "B" : "A");
    abCopyButton.setButtonText (ampSim.isOnB() ? "Copy to A" : "Copy to B");

    // The tuner covers the tabs while it's engaged.
    if (const auto tuning = ampSim.parameters.getRawParameterValue ("tuner_on")->load() >= 0.5f; tunerView.isVisible() != tuning)
    {
        tunerView.setVisible (tuning);
        if (tuning)
            tunerView.toFront (false);
    }
}

void AmpSimEditor::clickScene (int index)
{
    auto& scenes = ampSim.getScenes();
    if (storeSceneButton.getToggleState() || ! scenes.get (index).stored)
    {
        ampSim.storeScene (index);
        storeSceneButton.setToggleState (false, juce::dontSendNotification);
    }
    else
    {
        ampSim.recallScene (index);
    }
    timerCallback();
}

void AmpSimEditor::sceneMenu (int index)
{
    juce::PopupMenu menu;
    const auto& scene = ampSim.getScenes().get (index);
    menu.addSectionHeader (scene.stored ? scene.name : "Scene " + juce::String (index + 1) + " (empty)");
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    menu.addItem ("Store the current sound here", [safe, index]
    {
        if (safe != nullptr)
        {
            safe->ampSim.storeScene (index);
            safe->timerCallback();
        }
    });
    menu.addItem ("Clear", scene.stored, false, [safe, index]
    {
        if (safe != nullptr)
        {
            safe->ampSim.getScenes().clear (index);
            safe->timerCallback();
        }
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sceneButtons[(size_t) index]));
}

bool AmpSimEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        ampSim.parameters.copyState();
        return ampSim.undoManager.undo();
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return ampSim.undoManager.redo();
    return false;
}

void AmpSimEditor::mouseDown (const juce::MouseEvent& e)
{
    // Every press starts a new undo step: whatever this gesture changes undoes together. (The tree catches
    // up with the last gesture's values first, so they land in the step they belong to.)
    if (! e.mods.isPopupMenu())
    {
        ampSim.parameters.copyState();
        ampSim.undoManager.beginNewTransaction();
        return;
    }

    for (int i = 0; i < Scenes::count; ++i)
        if (e.eventComponent == &sceneButtons[(size_t) i])
        {
            sceneMenu (i);
            return;
        }

    // The control under the click, or the nearest parent that is one (a slider's text box, a knob).
    for (auto* c = e.eventComponent; c != nullptr && c != this; c = c->getParentComponent())
        if (const auto id = c->getProperties()[parameterIdProperty].toString(); id.isNotEmpty())
        {
            midiMenuFor (id).showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }
}

juce::PopupMenu AmpSimEditor::midiMenuFor (const juce::String& parameterId)
{
    juce::PopupMenu menu;
    auto* parameter = ampSim.parameters.getParameter (parameterId);
    if (parameter == nullptr)
        return menu;

    // Menu actions run later, on the message thread; the editor may be gone by then.
    const auto map = [safe = juce::Component::SafePointer<AmpSimEditor> (this)] () -> MidiMap*
    { return safe != nullptr ? &safe->ampSim.getMidiMap() : nullptr; };
    const auto learning = ampSim.getMidiMap().isLearning() && ampSim.getMidiMap().getLearnTarget() == parameterId;
    const auto isSwitch = dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr;

    menu.addSectionHeader (parameter->getName (64));
    menu.addItem (learning ? "Cancel MIDI learn" : "MIDI learn: then press a footswitch or move a pedal", [map, parameterId, learning]
    {
        if (auto* m = map())
        {
            if (learning)
                m->cancelLearn();
            else
                m->startLearn (parameterId);
        }
    });

    // Scenes always hold the amp slot and every switch; any other parameter can be added.
    if (! Scenes::isSwitch (parameterId) && ! presets::isGlobal (parameterId))
    {
        const auto held = ampSim.getScenes().isChosen (parameterId);
        menu.addSeparator();
        menu.addItem ("Held by scenes", true, held, [safe = juce::Component::SafePointer<AmpSimEditor> (this), parameterId, held]
        {
            if (safe != nullptr)
                safe->ampSim.getScenes().setChosen (parameterId, ! held);
        });
    }

    for (const auto& mapping : ampSim.getMidiMap().getMappings())
    {
        if (mapping.parameterId != parameterId)
            continue;

        const auto cc = mapping.cc;
        menu.addSeparator();
        menu.addSectionHeader ("CC " + juce::String (cc) + ": " + MidiMapping::actionName (mapping.action));
        if (isSwitch)
        {
            // Toggle suits switches that leave the state to the app; momentary follows the switch,
            // which also suits a latching controller that alternates 127 and 0 itself.
            auto changed = mapping;
            const auto toToggle = mapping.action != MidiMapping::Action::toggle;
            changed.action = toToggle ? MidiMapping::Action::toggle : MidiMapping::Action::momentary;
            menu.addItem (toToggle ? "Flip on each press (toggle)" : "Follow the switch (momentary, or a latching footswitch)", [map, changed]
            {
                if (auto* m = map())
                    m->set (changed);
            });
        }
        menu.addItem ("Forget CC " + juce::String (cc), [map, cc]
        {
            if (auto* m = map())
                m->remove (cc);
        });
    }
    return menu;
}

void AmpSimEditor::showMidiMappings()
{
    juce::PopupMenu menu;
    const auto map = [safe = juce::Component::SafePointer<AmpSimEditor> (this)] () -> MidiMap*
    { return safe != nullptr ? &safe->ampSim.getMidiMap() : nullptr; };
    const auto& mappings = ampSim.getMidiMap().getMappings();

    if (mappings.empty())
        menu.addItem ("No mappings yet: right-click a control to learn one", false, false, nullptr);

    for (const auto& mapping : mappings)
    {
        const auto cc = mapping.cc;
        auto* parameter = ampSim.parameters.getParameter (mapping.parameterId);
        juce::PopupMenu forget;
        forget.addItem ("Forget", [map, cc]
        {
            if (auto* m = map())
                m->remove (cc);
        });
        menu.addSubMenu ("CC " + juce::String (cc) + ": " + (parameter != nullptr ? parameter->getName (64) : mapping.parameterId) + " ("
                             + MidiMapping::actionName (mapping.action) + ")",
                         forget);
    }

    if (! mappings.empty())
    {
        menu.addSeparator();
        menu.addItem ("Forget all", [map]
        {
            if (auto* m = map())
                m->clear();
        });
    }

    menu.addSeparator();
    const auto value = [this] (const char* id) { return juce::String (juce::roundToInt (ampSim.parameters.getRawParameterValue (id)->load())); };
    menu.addSectionHeader ("Built in: program change 1 to 3 picks the amp, CC " + value ("midi_tap_cc") + " taps the tempo, CC "
                           + value ("midi_freeze_cc") + " freezes the reverb");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (midiButton));
}

void AmpSimEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto header = getLocalBounds().reduced (16).removeFromTop (96);
    g.setColour (textColour);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("Amp Sim", header.removeFromTop (30).removeFromLeft (200), juce::Justification::centredLeft);

}

void AmpSimEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto header = area.removeFromTop (96);

    outputKnob.setBounds (header.removeFromRight (90));
    inputKnob.setBounds (header.removeFromRight (90));
    header.removeFromRight (16);

    auto buttons = header.removeFromRight (300).withSizeKeepingCentre (300, 34);
    for (auto& button : slotButtons)
        button.setBounds (buttons.removeFromLeft (100).reduced (3, 0));

    auto presetRow = getLocalBounds().reduced (16).removeFromTop (96).withTrimmedTop (36).removeFromTop (30);
    presetLabel.setBounds (presetRow.removeFromLeft (300));
    savePresetButton.setBounds (presetRow.removeFromLeft (80).reduced (2, 0));
    loadPresetButton.setBounds (presetRow.removeFromLeft (80).reduced (2, 0));
    midiButton.setBounds (presetRow.removeFromLeft (80).reduced (2, 0));
    tunerButton.setBounds (presetRow.removeFromLeft (80).reduced (2, 0));
    presetRow.removeFromLeft (12);
    undoButton.setBounds (presetRow.removeFromLeft (64).reduced (2, 0));
    redoButton.setBounds (presetRow.removeFromLeft (64).reduced (2, 0));
    presetRow.removeFromLeft (12);
    abButton.setBounds (presetRow.removeFromLeft (44).reduced (2, 0));
    abCopyButton.setBounds (presetRow.removeFromLeft (90).reduced (2, 0));

    auto sceneRow = getLocalBounds().reduced (16).removeFromTop (96).withTrimmedTop (70).removeFromTop (26);
    scenesLabel.setBounds (sceneRow.removeFromLeft (60));
    for (auto& button : sceneButtons)
        button.setBounds (sceneRow.removeFromLeft (34).reduced (2, 0));
    sceneRow.removeFromLeft (6);
    storeSceneButton.setBounds (sceneRow.removeFromLeft (70).reduced (2, 0));

    warningLabel.setBounds (area.removeFromBottom (40));
    area.removeFromBottom (6);
    tabs.setBounds (area); // each page lays itself out when the tabs size it
    tunerView.setBounds (area);
}

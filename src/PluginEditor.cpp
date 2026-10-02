#include "PluginEditor.h"

namespace
{
const auto background = juce::Colour (0xff17191d);
const auto panel = juce::Colour (0xff22252b);
const auto accent = juce::Colour (0xff3fa7d6);
const auto textColour = juce::Colour (0xffe6e6e6);
const auto dimText = juce::Colour (0xff9aa0a8);
const auto errorColour = juce::Colour (0xffff6b5e);
const auto warningColour = juce::Colour (0xffffc04d);

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
    : attachment (state, parameterId, slider)
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

        auto* invert = toggles.add (new juce::ToggleButton ("Invert"));
        toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, AmpSimProcessor::cabParamId (mic, "invert"), *invert));

        withItems (channel, { "Left channel", "Right channel" });
        channelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, AmpSimProcessor::cabParamId (mic, "channel"), channel);
        addAndMakeVisible (channel);
    }

    auto* mute = toggles.add (new juce::ToggleButton ("Mute"));
    toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, AmpSimProcessor::cabParamId (mic, "mute"), *mute));

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
    if (blockName == "comp") return "Compressor";
    if (blockName == "eq") return "EQ";
    if (blockName == "delay") return "Delay";
    return blockName;
}
} // namespace

OrderStrip::OrderStrip (AmpSimProcessor& processor, ampsim::Chain::Section sectionToShow)
    : ampSim (processor), section (sectionToShow)
{
    refresh();
}

void OrderStrip::refresh()
{
    const auto order = ampSim.getSectionOrder (section);
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
    ampSim.setSectionOrder (section, order);
    refresh();
}

void OrderStrip::paint (juce::Graphics& g)
{
    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (section == ampsim::Chain::Section::pre ? "Order (before the amp):" : "Order (after the cab):",
                getLocalBounds().removeFromLeft (170), juce::Justification::centredLeft);
}

void OrderStrip::resized()
{
    auto area = getLocalBounds();
    area.removeFromLeft (175);
    for (int i = 0; i < names.size(); ++i)
    {
        auto cell = area.removeFromLeft (200).reduced (4, 2);
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

    for (auto [button, id] : std::initializer_list<std::pair<juce::ToggleButton*, juce::String>> {
             { &onButton, p + "_on" }, { &autoReleaseButton, p + "_auto_release" }, { &autoMakeupButton, p + "_auto_makeup" }, { &sidechainButton, p + "_sc_hpf" } })
    {
        buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, id, *button));
        addAndMakeVisible (button);
    }

    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, p + "_mode", withItems (mode, { "Studio", "Pedal" }));
    detectorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, p + "_detector", withItems (detector, { "Peak", "RMS" }));
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

    for (auto [button, id] : std::initializer_list<std::pair<juce::ToggleButton*, juce::String>> {
             { &onButton, p + "_on" }, { &lowCutButton, p + "_lowcut_on" }, { &highCutButton, p + "_highcut_on" } })
    {
        buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, id, *button));
        addAndMakeVisible (button);
    }

    const juce::StringArray slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    for (auto [box, id, items] : std::initializer_list<std::tuple<juce::ComboBox*, juce::String, juce::StringArray>> {
             { &mode, p + "_mode", { "Graphic", "Parametric" } }, { &lowCutSlope, p + "_lowcut_slope", slopes }, { &highCutSlope, p + "_highcut_slope", slopes } })
    {
        comboAttachments.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (state, id, withItems (*box, items)));
        addAndMakeVisible (box);
    }

    static const char* sliderNames[] = { "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    for (int m = 0; m < ampsim::Equalizer::numGraphicBands; ++m)
    {
        auto* slider = sliders.add (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow));
        slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 18);
        sliderAttachments.add (new juce::AudioProcessorValueTreeState::SliderAttachment (state, params::EqualizerParameters::sliderId (p, m), *slider));
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
            state, params::EqualizerParameters::bandId (p, b, "type"), withItems (band->type, { "Peak", "Low shelf", "High shelf", "Notch" }));
        using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        band->frequencyAttachment = std::make_unique<Attachment> (state, params::EqualizerParameters::bandId (p, b, "freq"), band->frequency);
        band->gainAttachment = std::make_unique<Attachment> (state, params::EqualizerParameters::bandId (p, b, "gain"), band->gain);
        band->qAttachment = std::make_unique<Attachment> (state, params::EqualizerParameters::bandId (p, b, "q"), band->q);
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

// ---- DelayPanel -------------------------------------------------------------------------------

DelayPanel::DelayPanel (AmpSimProcessor& processor) : ampSim (processor)
{
    auto& state = processor.parameters;
    stylePanelTitle (titleLabel, "Delay");
    addAndMakeVisible (titleLabel);

    for (auto [button, id] : std::initializer_list<std::pair<juce::ToggleButton*, const char*>> { { &onButton, "delay_on" }, { &syncButton, "delay_sync" } })
    {
        buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, id, *button));
        addAndMakeVisible (button);
    }

    juce::StringArray notes;
    for (const auto& n : ampsim::tempo::notes)
        notes.add (n.name);
    for (auto [box, id, items] : std::initializer_list<std::tuple<juce::ComboBox*, const char*, juce::StringArray>> {
             { &mode, "delay_mode", { "Digital", "Analog", "Tape" } }, { &stereo, "delay_stereo", { "Stereo", "Ping-pong", "Dual" } },
             { &note, "delay_note", notes }, { &rightNote, "delay_note_right", notes } })
    {
        comboAttachments.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (state, id, withItems (*box, items)));
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

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p)
    : AudioProcessorEditor (&p),
      ampSim (p),
      inputKnob (p.parameters, "input_gain", "Input"),
      outputKnob (p.parameters, "output_gain", "Output"),
      calibrateAttachment (p.parameters, "input_calibrate", calibrateButton),
      interfaceLevelAttachment (p.parameters, "input_level_dbu", interfaceLevel),
      lowCutKnob (p.parameters, "cab_lowcut_freq", "Low cut", " Hz"),
      highCutKnob (p.parameters, "cab_highcut_freq", "High cut", " Hz"),
      alignAttachment (p.parameters, "cab_align", alignButton),
      lowCutAttachment (p.parameters, "cab_lowcut_on", lowCutButton),
      highCutAttachment (p.parameters, "cab_highcut_on", highCutButton),
      cabBypassAttachment (p.parameters, "cab_bypass", cabBypassButton),
      lowCutSlopeAttachment (p.parameters, "cab_lowcut_slope", withItems (lowCutSlope, { "12 dB/oct", "24 dB/oct" })),
      highCutSlopeAttachment (p.parameters, "cab_highcut_slope", withItems (highCutSlope, { "12 dB/oct", "24 dB/oct" }))
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
    addAndMakeVisible (savePresetButton);
    addAndMakeVisible (loadPresetButton);

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
    preOrder = std::make_unique<OrderStrip> (p, ampsim::Chain::Section::pre);
    postOrder = std::make_unique<OrderStrip> (p, ampsim::Chain::Section::post);
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

    tabs.addTab ("Amps", panel, &ampsPage, false);
    tabs.addTab ("Cab", panel, &cabPage, false);
    tabs.addTab ("Pre FX", panel, &preFxPage, false);
    tabs.addTab ("Post FX", panel, &postFxPage, false);

    // Time FX: the delay (chorus and reverb join it).
    delayPanel = std::make_unique<DelayPanel> (p);
    timeFxPage.addAndMakeVisible (*delayPanel);
    timeFxPage.layout = [this] (juce::Rectangle<int> area)
    {
        area.reduce (8, 8);
        delayPanel->setBounds (area.removeFromLeft (juce::jmin (area.getWidth(), 520)).reduced (4));
    };
    tabs.addTab ("Time FX", panel, &timeFxPage, false);
    tabs.setTabBarDepth (30);
    addAndMakeVisible (tabs);

    setSize (1120, 800);
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

    // The warning line: the sample rate first, then a preset that couldn't load or loaded with gaps.
    auto warning = status.warning;
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

    warningLabel.setBounds (area.removeFromBottom (40));
    area.removeFromBottom (6);
    tabs.setBounds (area); // each page lays itself out when the tabs size it
}

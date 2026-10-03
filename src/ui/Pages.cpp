#include "Pages.h"

namespace ui
{

using namespace theme;

// ---- ControlGroup ----------------------------------------------------------------------------------------

ControlGroup::ControlGroup (AmpSimProcessor& processor) : ampSim (processor), state (processor.parameters) {}

ControlGroup::~ControlGroup()
{
    // The attachments go before the controls they're attached to.
    comboAttachments.clear();
    buttonAttachments.clear();
}

Knob& ControlGroup::addKnob (const juce::String& parameterId, const juce::String& caption, const juce::String& suffix, Knob::Size size)
{
    return adopt (std::make_unique<Knob> (state, parameterId, caption, suffix, size));
}

Switch& ControlGroup::addSwitch (const juce::String& parameterId, const juce::String& label)
{
    auto& control = adopt (std::make_unique<Switch> (label));
    buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, parameterId, tagged (control, parameterId)));
    return control;
}

juce::ComboBox& ControlGroup::addCombo (const juce::String& parameterId, const juce::StringArray& items)
{
    auto& box = adopt (std::make_unique<juce::ComboBox>());
    comboAttachments.add (new juce::AudioProcessorValueTreeState::ComboBoxAttachment (state, parameterId, tagged (withItems (box, items), parameterId)));
    return box;
}

ValueField& ControlGroup::addField (const juce::String& parameterId, const juce::String& suffix)
{
    return adopt (std::make_unique<ValueField> (state, parameterId, suffix));
}

Fader& ControlGroup::addFader (const juce::String& parameterId, const juce::String& caption, const juce::String& suffix)
{
    return adopt (std::make_unique<Fader> (state, parameterId, caption, suffix));
}

juce::TextButton& ControlGroup::addButton (const juce::String& label, std::function<void()> onClick)
{
    auto& button = adopt (std::make_unique<juce::TextButton> (label));
    button.onClick = std::move (onClick);
    return button;
}

juce::Label& ControlGroup::addLabel (const juce::String& words, Text style, juce::Colour colour)
{
    auto& label = adopt (std::make_unique<juce::Label>());
    label.setText (words, juce::dontSendNotification);
    label.setFont (font (style));
    label.setColour (juce::Label::textColourId, colour);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setMinimumHorizontalScale (0.85f);
    label.setBorderSize ({});
    return label;
}

void ControlGroup::heading (juce::Rectangle<int> area, const juce::String& words)
{
    headings.emplace_back (area, words);
}

void ControlGroup::paintHeadings (juce::Graphics& g) const
{
    // Cards first (a raised panel with its heading inside), then the free-standing headings.
    for (const auto& [area, words] : cards)
    {
        const auto r = area.toFloat().reduced (0.5f);
        g.setColour (surfaceRaised.withAlpha (0.42f));
        g.fillRoundedRectangle (r, radiusPanel);
        g.setColour (outline);
        g.drawRoundedRectangle (r, radiusPanel, 1.0f);
        if (words.isNotEmpty())
            drawGroupHeading (g, area.reduced (cardPadding, 0).withY (area.getY() + cardPadding - 2).withHeight (16), words);
    }
    for (const auto& [area, words] : headings)
        drawGroupHeading (g, area, words);
}

juce::Rectangle<int> ControlGroup::card (juce::Rectangle<int> area, const juce::String& title)
{
    cards.emplace_back (area, title);
    auto inside = area.reduced (cardPadding);
    if (title.isNotEmpty())
        inside.removeFromTop (cardHeading);
    return inside;
}

std::vector<juce::Rectangle<int>> ControlGroup::layoutCards (juce::Rectangle<int> row, const std::vector<CardSpec>& specs, int gap)
{
    const auto widthOf = [] (const CardItem& item)
    {
        if (item.width > 0)
            return item.width;
        if (auto* knob = dynamic_cast<Knob*> (item.component))
            return Knob::preferredWidth (knob->getKnobSize());
        if (auto* toggle = dynamic_cast<Switch*> (item.component))
            return toggle->getPreferredWidth();
        return item.component != nullptr ? item.component->getWidth() : 0;
    };
    const auto heightOf = [] (const CardItem& item)
    {
        if (item.height > 0)
            return item.height;
        if (auto* knob = dynamic_cast<Knob*> (item.component))
            return Knob::preferredHeight (knob->getKnobSize());
        return dynamic_cast<Switch*> (item.component) != nullptr ? switchHeight : controlHeight;
    };
    const auto blockWidth = [&widthOf] (const CardSpec& spec)
    {
        auto width = spec.extraWidth;
        for (size_t i = 0; i < spec.items.size(); ++i)
            width += widthOf (spec.items[i]) + (i > 0 ? controlGap : 0);
        return width;
    };

    // Each card's natural width (its controls, or its heading if that's wider), then the spare width
    // shared out in proportion.
    std::vector<int> natural;
    int total = 0;
    for (const auto& spec : specs)
    {
        const auto headingWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font ("Semibold", 11.0f), spec.title.toUpperCase())) + 24;
        natural.push_back (2 * cardPadding + juce::jmax (blockWidth (spec), headingWidth));
        total += natural.back();
    }
    const auto spare = juce::jmax (0, row.getWidth() - total - gap * ((int) specs.size() - 1));

    std::vector<juce::Rectangle<int>> insides;
    for (size_t k = 0; k < specs.size(); ++k)
    {
        const auto width = k + 1 == specs.size() ? row.getWidth() : natural[k] + (total > 0 ? spare * natural[k] / total : 0);
        const auto inside = card (row.removeFromLeft (width), specs[k].title);
        row.removeFromLeft (gap);

        auto cursor = inside.withSizeKeepingCentre (juce::jmin (inside.getWidth(), blockWidth (specs[k])), inside.getHeight());
        for (const auto& item : specs[k].items)
        {
            const auto w = widthOf (item), h = juce::jmin (heightOf (item), inside.getHeight());
            const auto cell = cursor.removeFromLeft (w);
            cursor.removeFromLeft (controlGap);
            if (item.component != nullptr)
                item.component->setBounds (cell.withSizeKeepingCentre (w, h));
        }
        insides.push_back (inside);
    }
    return insides;
}

// ---- BlockPage -----------------------------------------------------------------------------------------

BlockPage::BlockPage (AmpSimProcessor& processor, BlockId blockId, juce::String titleText, juce::String subtitleText)
    : ControlGroup (processor), id (blockId), title (std::move (titleText)), subtitle (std::move (subtitleText))
{
    // The block's own switch sits in the header. The cab's is its bypass, so it says so.
    if (const auto& i = info (id); *i.onParameter != 0)
        onSwitch = &addSwitch (i.onParameter, i.switchInverted ? "Bypass" : "On");
}

BlockPage::~BlockPage() = default;

void BlockPage::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (surface);
    g.fillRoundedRectangle (bounds, radiusPanel);
    g.setColour (outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radiusPanel, 1.0f);

    // The header: the section's colour, the block's name, and where it sits in the chain.
    auto header = getLocalBounds().reduced (panelPadding).withHeight (headerHeight);
    g.setColour (sectionColour (info (id).section));
    g.fillEllipse (header.removeFromLeft (10).toFloat().withSizeKeepingCentre (9.0f, 9.0f));
    header.removeFromLeft (space::s);
    const auto titleFont = font (Text::title);
    g.setFont (titleFont);
    g.setColour (text);
    g.drawText (title, header.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (titleFont, title)) + 2),
                juce::Justification::centredLeft, false);
    header.removeFromLeft (space::m);
    g.setFont (font (Text::label));
    g.setColour (textDim);
    g.drawText (subtitle, header.withRight (headerSpace.isEmpty() ? header.getRight() : headerSpace.getX()), juce::Justification::centredLeft, true);

    paintHeadings (g);
    paintContent (g);
}

void BlockPage::resized()
{
    clearHeadings();
    auto area = getLocalBounds().reduced (panelPadding);
    auto header = area.removeFromTop (headerHeight);
    area.removeFromTop (space::m);

    if (onSwitch != nullptr)
    {
        const auto width = onSwitch->getPreferredWidth();
        onSwitch->setBounds (header.removeFromRight (width).withSizeKeepingCentre (width, switchHeight));
        header.removeFromRight (space::xl);
    }

    const auto titleWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font (Text::title), title));
    const auto subtitleWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font (Text::label), subtitle));
    headerSpace = header.withTrimmedLeft (juce::jmin (header.getWidth(), 10 + space::s + titleWidth + space::m + subtitleWidth + space::xl));

    layoutContent (area);
}

// ---- Layout helpers --------------------------------------------------------------------------------------

int knobRowWidth (int count, Knob::Size size, int gap)
{
    return count * Knob::preferredWidth (size) + juce::jmax (0, count - 1) * gap;
}

Knob::Size knobSizeFor (int width, int count, int gap)
{
    return knobRowWidth (count, Knob::Size::normal, gap) <= width ? Knob::Size::normal : Knob::Size::compact;
}

juce::Rectangle<int> placeKnobs (juce::Rectangle<int> row, const std::vector<Knob*>& knobs, Knob::Size size, int gap)
{
    const auto width = Knob::preferredWidth (size), height = juce::jmin (Knob::preferredHeight (size), row.getHeight());
    for (auto* knob : knobs)
    {
        const auto cell = row.removeFromLeft (width);
        if (knob != nullptr)
        {
            knob->setKnobSize (size);
            knob->setBounds (cell.withSizeKeepingCentre (width, height));
        }
        row.removeFromLeft (gap);
    }
    return row;
}

juce::StringArray noteNames()
{
    juce::StringArray names;
    for (const auto& n : ampsim::tempo::notes)
        names.add (n.name);
    return names;
}

void place (juce::Component* c, juce::Rectangle<int>& row, int width, int height, int gapAfter)
{
    const auto cell = row.removeFromLeft (width);
    if (c != nullptr)
        c->setBounds (cell.withSizeKeepingCentre (width, juce::jmin (height, cell.getHeight())));
    row.removeFromLeft (gapAfter);
}

void place (Switch* s, juce::Rectangle<int>& row, int gapAfter)
{
    place (s, row, s->getPreferredWidth(), switchHeight, gapAfter);
}

void dim (juce::Component* c, bool used)
{
    if (c != nullptr)
        c->setAlpha (used ? 1.0f : unusedAlpha);
}

// ---- IoPage ----------------------------------------------------------------------------------------------

IoPage::IoPage (AmpSimProcessor& p) : BlockPage (p, BlockId::input, "Input & output", "The levels in and out, and the footswitch's controllers")
{
    input = &addKnob ("input_gain", "Input");
    output = &addKnob ("output_gain", "Output");
    tapLabel = &addLabel ("Tap tempo", Text::body, text);
    freezeLabel = &addLabel ("Reverb freeze", Text::body, text);
    sceneLabel = &addLabel ("Scenes (value 0 to 7 is scene 1 to 8)", Text::body, text);
    tapCc = &addField ("midi_tap_cc", "");
    freezeCc = &addField ("midi_freeze_cc", "");
    sceneCc = &addField ("midi_scene_cc", "");
    for (auto* field : { tapCc, freezeCc, sceneCc })
    {
        field->setFormatter ([] (float cc) { return "CC " + juce::String (juce::roundToInt (cc)); });
        field->setShowsBar (false);
    }
    note = &addLabel ("Program change 1 to 3 picks the amp slot. Right-click any control to map a footswitch or a pedal to it; the MIDI button lists the mappings.",
                      Text::label, textDim);
    note->setJustificationType (juce::Justification::topLeft);
}

void IoPage::layoutContent (juce::Rectangle<int> area)
{
    const auto height = juce::jmax (knobCardHeight(), cardHeight (3 * controlHeight + 3 * space::s + 32));
    auto row = area.removeFromTop (height);

    // The levels: the input gain before everything and the output level after it.
    auto levels = card (row.removeFromLeft (2 * cardPadding + 2 * Knob::preferredWidth (Knob::Size::normal) + space::xl), "Levels");
    placeKnobs (levels.withSizeKeepingCentre (levels.getWidth(), Knob::preferredHeight (Knob::Size::normal)), { input, output }, Knob::Size::normal, space::xl);
    row.removeFromLeft (space::m);

    // The footswitch's built-in controllers.
    auto foot = card (row.removeFromLeft (juce::jmin (row.getWidth(), 620)), "Footswitch controllers");
    for (auto [label, field] : { std::pair<juce::Label*, ValueField*> { tapLabel, tapCc }, { freezeLabel, freezeCc }, { sceneLabel, sceneCc } })
    {
        auto line = foot.removeFromTop (controlHeight);
        field->setBounds (line.removeFromRight (96));
        label->setBounds (line);
        foot.removeFromTop (space::s);
    }
    note->setBounds (foot);
}

// ---- GatePage --------------------------------------------------------------------------------------------

GatePage::GatePage (AmpSimProcessor& p, bool isGateB)
    : BlockPage (p, isGateB ? BlockId::gateB : BlockId::gateA, isGateB ? "Gate B" : "Gate A",
                 isGateB ? "After the amp, before the cab" : "Before the amp"),
      gateB (isGateB)
{
    const juce::String g = isGateB ? "gate_b" : "gate_a";
    knobs = { &addKnob (g + "_threshold", "Threshold"), &addKnob (g + "_hysteresis", "Hysteresis"), &addKnob (g + "_hold", "Hold", " ms"),
              &addKnob (g + "_attack", "Attack", " ms"), &addKnob (g + "_release", "Release", " ms"), &addKnob (g + "_range", "Range"),
              &addKnob (g + "_sc_freq", "Frequency", " Hz") };
    releaseMode = &addCombo (g + "_release_mode", { "Adaptive release", "Classic release" });
    detector = &addCombo (g + "_detector", { "Detect from the DI", "Detect from its own input" });
    sidechain = &addSwitch (g + "_sc_hpf", "High-pass");
    reduction = &adopt (std::make_unique<ReductionMeter> (40.0f));
    releaseMode->setTooltip ("Adaptive: the release follows how the note dies away. Classic: the release knob, always.");
    detector->setTooltip ("What the gate listens to: the clean guitar, or the signal arriving at the gate");

    if (gateB)
    {
        link = &addSwitch ("gate_link", "Linked to Gate A");
        link->setTooltip ("Linked, Gate B applies Gate A's decision after the amp");
    }
    else
    {
        learn = &addButton ("Learn", [this] { ampSim.learnGates(); });
        learn->setTooltip ("Mute the strings, then press: measures the noise for 2 s and sets the threshold above it");
    }
}

void GatePage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (releaseMode, toolbar, 180);
    place (detector, toolbar, 220, controlHeight, space::l);
    if (link != nullptr)
        place (link, toolbar);
    else
        place (learn, toolbar, 110);
    area.removeFromTop (space::m);

    // The gain reduction on the right, as tall as the cards.
    const auto size = knobSizeFor (area.getWidth() - 160, (int) knobs.size(), controlGap + 6);
    const auto cardsHeight = knobCardHeight (size) + space::m + cardHeight (56);
    auto meterColumn = area.removeFromRight (56).withHeight (cardsHeight);
    reduction->setBounds (card (meterColumn, {}).reduced (0, 2));
    area.removeFromRight (space::m);

    for (auto* knob : knobs)
        knob->setKnobSize (size);
    layoutCards (area.removeFromTop (knobCardHeight (size)),
                 { { "Threshold", { { knobs[0] }, { knobs[1] }, { knobs[5] } } },
                   { "Timing", { { knobs[2] }, { knobs[3] }, { knobs[4] } } },
                   { "Sidechain", { { sidechain }, { knobs[6] } } } });
    area.removeFromTop (space::m);
    meterArea = card (area.removeFromTop (cardHeight (56)), "Detector");
}

void GatePage::refresh()
{
    const auto newMeter = ampSim.getGateMeter (gateB);
    const auto newLinked = gateB && state.getRawParameterValue ("gate_link")->load() >= 0.5f;
    const auto newLearning = ampSim.isLearningGates() && (! gateB || ampSim.isGateBOnItsOwn());
    const auto newProgress = ampSim.getGateLearnProgress();

    if (newLinked != linked)
    {
        // Linked, Gate B's own settings don't apply: dim them (they stay editable for when it's unlinked).
        for (auto* c : std::initializer_list<juce::Component*> { releaseMode, detector, sidechain })
            dim (c, ! newLinked);
        for (auto* knob : knobs)
            dim (knob, ! newLinked);
    }

    const auto changed = std::abs (newMeter.detectorDb - meter.detectorDb) > 0.5f || newMeter.open != meter.open
                         || std::abs (newMeter.openDb - meter.openDb) > 0.05f || std::abs (newMeter.reductionDb - meter.reductionDb) > 0.5f
                         || newLinked != linked || newLearning != learning || std::abs (newProgress - learnProgress) > 0.01f;
    meter = newMeter;
    linked = newLinked;
    learning = newLearning;
    learnProgress = newProgress;
    if (learn != nullptr)
        learn->setButtonText (learning ? "Learning..." : "Learn");
    reduction->setReduction (juce::jmin (meter.reductionDb, 40.0f));
    if (changed)
        repaint (meterArea);
}

void GatePage::paintContent (juce::Graphics& g)
{
    // The detector's level on a -100 to 0 dBFS scale against the open (white) and close (amber)
    // thresholds; the gap between them is the hysteresis.
    auto area = meterArea;
    const auto textRow = area.removeFromTop (18);
    area.removeFromTop (6);
    const auto bar = area.removeFromTop (14);
    const auto scale = area.withTrimmedTop (2).withHeight (14);
    const auto xFor = [&bar] (float db) { return (float) bar.getX() + (float) bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 100.0f); };

    g.setColour (background);
    g.fillRoundedRectangle (bar.toFloat(), 3.0f);
    if (! linked)
    {
        g.setColour ((meter.open ? good : textDim).withAlpha (0.85f));
        g.fillRoundedRectangle (bar.toFloat().withRight (xFor (meter.detectorDb)), 3.0f);
        g.setColour (text);
        g.fillRect (juce::Rectangle<float> (xFor (meter.openDb) - 1.0f, (float) bar.getY() - 3.0f, 2.0f, (float) bar.getHeight() + 6.0f));
        g.setColour (warn);
        g.fillRect (juce::Rectangle<float> (xFor (meter.closeDb) - 1.0f, (float) bar.getY() - 3.0f, 2.0f, (float) bar.getHeight() + 6.0f));
    }

    g.setFont (font (Text::caption));
    g.setColour (textDim);
    for (int db = -100; db <= 0; db += 20)
        g.drawText (juce::String (db), juce::Rectangle<float> (xFor ((float) db) - 20.0f, (float) scale.getY(), 40.0f, (float) scale.getHeight()),
                    db == -100 ? juce::Justification::centredLeft : (db == 0 ? juce::Justification::centredRight : juce::Justification::centred), false);

    const auto dB = [] (float value) { return juce::String (juce::roundToInt (value)); };
    juce::String line;
    if (learning)
        line = "Learning the noise floor: keep the strings muted (" + juce::String (juce::roundToInt (learnProgress * 100.0f)) + "%)";
    else if (linked)
        line = juce::String ("Following Gate A: ") + (meter.reductionDb < 1.0f ? "open" : "closing, " + dB (meter.reductionDb) + " dB down");
    else
        line = (meter.open ? "Open" : "Closed") + juce::String (", level ") + dB (meter.detectorDb) + " dBFS, reduction "
               + (meter.reductionDb >= 99.0f ? juce::String ("full") : dB (meter.reductionDb) + " dB") + ".  White opens it, amber closes it.";
    g.setColour (learning ? warn : text);
    g.setFont (font (Text::label));
    g.drawText (line, textRow, juce::Justification::centredLeft, true);
}

// ---- CompressorPage --------------------------------------------------------------------------------------

CompressorPage::CompressorPage (AmpSimProcessor& p, bool isPost)
    : BlockPage (p, isPost ? BlockId::postCompressor : BlockId::preCompressor, "Compressor", isPost ? "After the cab" : "Before the amp"),
      post (isPost),
      prefix (isPost ? "comp_post" : "comp_pre")
{
    mode = &addCombo (prefix + "_mode", { "Studio", "Pedal" });
    detector = &addCombo (prefix + "_detector", { "Peak", "RMS" });
    knobs = { &addKnob (prefix + "_threshold", "Threshold"), &addKnob (prefix + "_ratio", "Ratio", ":1"), &addKnob (prefix + "_knee", "Knee"),
              &addKnob (prefix + "_attack", "Attack", " ms"), &addKnob (prefix + "_release", "Release", " ms"), &addKnob (prefix + "_makeup", "Makeup"),
              &addKnob (prefix + "_mix", "Mix", " %"), &addKnob (prefix + "_sc_freq", "Frequency", " Hz") };
    autoRelease = &addSwitch (prefix + "_auto_release", "Auto");
    autoMakeup = &addSwitch (prefix + "_auto_makeup", "Auto");
    sidechain = &addSwitch (prefix + "_sc_hpf", "High-pass");
    reduction = &adopt (std::make_unique<ReductionMeter> (24.0f));
    mode->setTooltip ("Studio: feed-forward and precise. Pedal: feedback, like a stompbox.");
    autoRelease->setTooltip ("Auto release: fast after short peaks, slow after long compression");
    autoMakeup->setTooltip ("Auto makeup: brings a -12 dBFS signal back to its level whatever the threshold and ratio");
}

void CompressorPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (mode, toolbar, 130);
    place (detector, toolbar, 110);
    area.removeFromTop (space::m);

    // The static curve and the gain reduction on the right; the controls in cards on the left.
    const auto size = knobSizeFor ((area.getWidth() - 320) / 2, 3, controlGap + 6);
    for (auto* knob : knobs)
        knob->setKnobSize (size);
    const auto rowHeight = knobCardHeight (size);
    const auto side = juce::jmin (2 * rowHeight + space::m, area.getHeight(), 330);
    auto right = area.removeFromRight (side + 56).withHeight (side);
    area.removeFromRight (space::m);
    reduction->setBounds (card (right.removeFromRight (56), {}).reduced (0, 2));
    right.removeFromRight (space::s);
    curveArea = card (right, "Curve");

    layoutCards (area.removeFromTop (rowHeight), { { "Threshold and ratio", { { knobs[0] }, { knobs[1] }, { knobs[2] } } },
                                                   { "Timing", { { knobs[3] }, { knobs[4] }, { autoRelease } } } });
    area.removeFromTop (space::m);
    layoutCards (area.removeFromTop (rowHeight), { { "Output", { { knobs[5] }, { autoMakeup }, { knobs[6] } } },
                                                   { "Sidechain", { { sidechain }, { knobs[7] } } } });
}

void CompressorPage::refresh()
{
    reduction->setReduction (ampSim.getCompressorReduction (post));

    const auto threshold = state.getRawParameterValue (prefix + "_threshold")->load();
    const auto ratio = state.getRawParameterValue (prefix + "_ratio")->load();
    const auto knee = state.getRawParameterValue (prefix + "_knee")->load();
    if (! juce::exactlyEqual (threshold, shownThreshold) || ! juce::exactlyEqual (ratio, shownRatio) || ! juce::exactlyEqual (knee, shownKnee))
    {
        shownThreshold = threshold;
        shownRatio = ratio;
        shownKnee = knee;
        repaint (curveArea);
    }
}

void CompressorPage::paintContent (juce::Graphics& g)
{
    // The static curve (Compressor::staticCurveDb, the block's own formula): output level against input
    // level from -60 to 0 dB, with the 1:1 line for reference and the threshold marked.
    const auto box = curveArea.toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (box, radiusControl);
    const auto plot = box.reduced (10.0f);
    const auto xOf = [&plot] (double db) { return plot.getX() + plot.getWidth() * (float) ((db + 60.0) / 60.0); };
    const auto yOf = [&plot] (double db) { return plot.getBottom() - plot.getHeight() * (float) ((juce::jlimit (-60.0, 0.0, db) + 60.0) / 60.0); };

    g.setColour (outline);
    for (double db = -48.0; db < 0.0; db += 12.0)
    {
        g.drawVerticalLine (juce::roundToInt (xOf (db)), plot.getY(), plot.getBottom());
        g.drawHorizontalLine (juce::roundToInt (yOf (db)), plot.getX(), plot.getRight());
    }
    g.setColour (textDim.withAlpha (0.4f));
    g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);

    const auto threshold = (double) state.getRawParameterValue (prefix + "_threshold")->load();
    const auto ratio = (double) state.getRawParameterValue (prefix + "_ratio")->load();
    const auto knee = (double) state.getRawParameterValue (prefix + "_knee")->load();
    juce::Path curve;
    for (int i = 0; i <= 120; ++i)
    {
        const auto x = -60.0 + 0.5 * i;
        const juce::Point<float> p { xOf (x), yOf (ampsim::Compressor::staticCurveDb (x, threshold, ratio, knee)) };
        i == 0 ? curve.startNewSubPath (p) : curve.lineTo (p);
    }
    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (warn);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ xOf (threshold), yOf (ampsim::Compressor::staticCurveDb (threshold, threshold, ratio, knee)) }));

    g.setFont (font (Text::caption));
    g.setColour (textDim);
    g.drawText ("in", juce::Rectangle<float> (plot.getRight() - 30.0f, plot.getBottom() - 14.0f, 28.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText ("out", juce::Rectangle<float> (plot.getX() + 2.0f, plot.getY(), 28.0f, 12.0f), juce::Justification::centredLeft, false);
}

// ---- BoostPage, OverdrivePage ------------------------------------------------------------------------

namespace
{
/// What each mode is (BUILD_PLAN "Boost and Overdrive"), in generic words: no product names in the UI.
juce::String boostModeText (int mode)
{
    switch (mode)
    {
        case 0:  return "Clean: flat gain with a tilt, the lead boost. Uses Level and Tilt.";
        case 1:  return "Tight: a high-pass and a mid push, no clipping, to tighten a high-gain amp. Uses Level, Tight, and Mid push.";
        case 2:  return "Screamer: the Mid Drive circuit at minimal drive, the classic metal boost with slight compression. Uses Level.";
        default: break;
    }
    return {};
}

juce::String overdriveModeText (int mode)
{
    switch (mode)
    {
        case 0:  return "Mid Drive: diodes in the op-amp's feedback loop: soft clipping with a mid hump.";
        case 1:  return "Distortion: hard-clipping diodes to ground, and the op-amp's slew limiting at high gain.";
        case 2:  return "Transparent: the clean and clipped signals blended, so the low end stays.";
        case 3:  return "Fuzz: four transistor clipping stages with a tone control between them.";
        default: break;
    }
    return {};
}
} // namespace

BoostPage::BoostPage (AmpSimProcessor& p) : BlockPage (p, BlockId::boost, "Boost", "Before the amp")
{
    mode = &addCombo ("boost_mode", { "Clean", "Tight", "Screamer" });
    level = &addKnob ("boost_level", "Level");
    tilt = &addKnob ("boost_tilt", "Tilt");
    tightHz = &addKnob ("boost_tight_freq", "Tight", " Hz");
    mid = &addKnob ("boost_mid", "Mid push");
    about = &addLabel ({}, Text::body, text);
    about->setJustificationType (juce::Justification::topLeft);
}

void BoostPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (mode, toolbar, 180);
    area.removeFromTop (space::m);

    auto row = area.removeFromTop (knobCardHeight());
    const auto aboutWidth = juce::jlimit (240, 420, row.getWidth() / 3);
    auto aboutCard = row.removeFromRight (aboutWidth);
    row.removeFromRight (space::m);
    layoutCards (row, { { "Level", { { level } } }, { "Clean", { { tilt } } }, { "Tight", { { tightHz }, { mid } } } });
    about->setBounds (card (aboutCard, "This mode"));
}

void BoostPage::refresh()
{
    // Clean uses the tilt, Tight the tight frequency and the mid push; Screamer only the level.
    const auto current = juce::roundToInt (state.getRawParameterValue ("boost_mode")->load());
    if (current == shownMode)
        return;
    shownMode = current;
    dim (tilt, current == 0);
    dim (tightHz, current == 1);
    dim (mid, current == 1);
    about->setText (boostModeText (current), juce::dontSendNotification);
}

OverdrivePage::OverdrivePage (AmpSimProcessor& p) : BlockPage (p, BlockId::overdrive, "Overdrive", "Before the amp; circuits solved sample by sample")
{
    mode = &addCombo ("od_mode", params::OverdriveParameters::modeNames());
    drive = &addKnob ("od_drive", "Drive", " %");
    tone = &addKnob ("od_tone", "Tone", " %");
    level = &addKnob ("od_level", "Level");
    mix = &addKnob ("od_mix", "Mix", " %");
    tight = &addSwitch ("od_tight", "On");
    tightHz = &addKnob ("od_tight_freq", "Frequency", " Hz");
    oversampling = &addCombo ("drive_oversampling", { "4x", "8x" });
    oversamplingNote = &addLabel ("For the boost and the overdrive. 8x costs about twice the CPU. A global setting, not saved in presets.", Text::label, textDim);
    oversamplingNote->setJustificationType (juce::Justification::centredLeft);
    about = &addLabel ({}, Text::body, text);
    about->setJustificationType (juce::Justification::topLeft);
}

void OverdrivePage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (mode, toolbar, 180);
    area.removeFromTop (space::m);

    layoutCards (area.removeFromTop (knobCardHeight()), { { "Drive", { { drive }, { tone } } }, { "Output", { { level }, { mix } } }, { "Tight", { { tight }, { tightHz } } } });
    area.removeFromTop (space::m);

    auto row = area.removeFromTop (cardHeight (44));
    auto sampling = card (row.removeFromLeft (juce::jmin (row.getWidth() / 2, 520)), "Oversampling");
    oversampling->setBounds (sampling.removeFromLeft (80).withSizeKeepingCentre (80, controlHeight));
    sampling.removeFromLeft (space::m);
    oversamplingNote->setBounds (sampling);
    row.removeFromLeft (space::m);
    about->setBounds (card (row, "This mode"));
}

void OverdrivePage::refresh()
{
    if (const auto on = state.getRawParameterValue ("od_tight")->load() >= 0.5f ? 1 : 0; on != shownTight)
    {
        shownTight = on;
        dim (tightHz, on == 1);
    }
    if (const auto current = juce::roundToInt (state.getRawParameterValue ("od_mode")->load()); current != shownMode)
    {
        shownMode = current;
        about->setText (overdriveModeText (current), juce::dontSendNotification);
    }
}

} // namespace ui

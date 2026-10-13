// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

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
    control.onUserToggle = [this, parameterId] (bool on) { ampSim.switchedByUser (parameterId, on); };
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
    // Cards first (a hairline box, radius 10, with its heading inside), then the free-standing headings.
    for (const auto& [area, words] : cards)
    {
        drawCard (g, area.toFloat());
        if (words.isNotEmpty())
        {
            g.setFont (geist (Weight::medium, 12.0f));
            g.setColour (inkFaint);
            g.drawText (words, area.reduced (cardPadding, 0).withY (area.getY() + cardPadding - 2).withHeight (16), juce::Justification::centredLeft, true);
        }
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
        const auto headingWidth = juce::roundToInt (textWidth (geist (Weight::medium, 12.0f), spec.title)) + 24;
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

// ---- InspectorLayout -----------------------------------------------------------------------------------

InspectorLayout::InspectorLayout (juce::Rectangle<int> area, bool place) : bounds (area), placing (place), y (area.getY()) {}

void InspectorLayout::begin()
{
    if (! first && ! afterHeading)
        y += rowGap;
    first = false;
    afterHeading = false;
}

void InspectorLayout::set (juce::Component* c, juce::Rectangle<int> r) const
{
    if (placing && c != nullptr)
        c->setBounds (r);
}

void InspectorLayout::section (const juce::String& title, juce::Component* right)
{
    if (! first)
        y += sectionGap;
    auto line = juce::Rectangle<int> (bounds.getX(), y, bounds.getWidth(), headingHeight);
    if (right != nullptr)
    {
        // A switch's pill ends on the column's right edge (the component has a 4 px focus margin all round).
        const auto* s = dynamic_cast<Switch*> (right);
        const auto w = s != nullptr ? s->getPreferredWidth() : juce::jmax (36, right->getWidth());
        set (right, juce::Rectangle<int> (line.getRight() - w + Switch::margin, line.getCentreY() - Switch::preferredHeight / 2, w, Switch::preferredHeight));
        line.removeFromRight (w + theme::space::s);
    }
    texts.push_back ({ line, title, Text::Kind::heading });
    y += headingHeight + headingGap;
    first = false;
    afterHeading = true;
}

void InspectorLayout::knobs (const std::vector<Knob*>& list)
{
    // Three to a row, each in its third of the column: the columns line up from one section to the next.
    const auto cellWidth = bounds.getWidth() / 3;
    for (size_t i = 0; i < list.size(); i += 3)
    {
        const auto end = juce::jmin (list.size(), i + 3);
        auto height = 0;
        for (auto k = i; k < end; ++k)
            if (list[k] != nullptr)
                height = juce::jmax (height, Knob::preferredHeight (list[k]->getKnobSize()));
        if (height == 0)
            continue;
        begin();
        for (auto k = i; k < end; ++k)
            if (auto* knob = list[k])
            {
                const auto cell = juce::Rectangle<int> (bounds.getX() + (int) (k - i) * cellWidth, y, cellWidth, height);
                set (knob, cell.withSizeKeepingCentre (Knob::preferredWidth (knob->getKnobSize()), Knob::preferredHeight (knob->getKnobSize())));
            }
        y += height;
    }
}

void InspectorLayout::labelled (const juce::String& caption, juce::Component* control, int height)
{
    begin();
    const juce::Rectangle<int> line (bounds.getX(), y, bounds.getWidth(), height);
    texts.push_back ({ line.withWidth (captionWidth), caption, Text::Kind::caption });
    set (control, line.withTrimmedLeft (captionWidth));
    y += height;
}

void InspectorLayout::row (juce::Component* control, int height)
{
    begin();
    set (control, { bounds.getX(), y, bounds.getWidth(), height });
    y += height;
}

void InspectorLayout::switches (const std::vector<Switch*>& list)
{
    // The pill's left edge on the column's (the component has a 4 px focus margin all round).
    begin();
    auto x = bounds.getX();
    auto any = false;
    for (auto* s : list)
    {
        if (s == nullptr)
            continue;
        const auto w = s->getPreferredWidth() - 2 * Switch::margin;
        if (any && x + w > bounds.getRight())
        {
            x = bounds.getX();
            y += Switch::preferredHeight;
        }
        set (s, { x - Switch::margin, y, w + 2 * Switch::margin, Switch::preferredHeight });
        x += w + theme::space::xl;
        any = true;
    }
    y += Switch::preferredHeight;
}

void InspectorLayout::buttons (const std::vector<juce::Component*>& list, int height)
{
    std::vector<juce::Component*> shown;
    for (auto* b : list)
        if (b != nullptr)
            shown.push_back (b);
    if (shown.empty())
        return;
    begin();
    const auto gapWidth = theme::space::s;
    const auto w = (bounds.getWidth() - gapWidth * ((int) shown.size() - 1)) / (int) shown.size();
    auto x = bounds.getX();
    for (auto* b : shown)
    {
        set (b, { x, y, b == shown.back() ? bounds.getRight() - x : w, height });
        x += w + gapWidth;
    }
    y += height;
}

void InspectorLayout::fields (const std::vector<std::pair<juce::String, juce::Component*>>& captioned, int perRow)
{
    constexpr int captionH = 14, fieldH = 24, cellGap = 8;
    perRow = juce::jmax (1, perRow);
    const auto w = (bounds.getWidth() - cellGap * (perRow - 1)) / perRow;
    for (size_t i = 0; i < captioned.size(); i += (size_t) perRow)
    {
        begin();
        for (auto k = i; k < juce::jmin (captioned.size(), i + (size_t) perRow); ++k)
        {
            const auto x = bounds.getX() + (int) (k - i) * (w + cellGap);
            texts.push_back ({ { x, y, w, captionH }, captioned[k].first, Text::Kind::fieldCaption });
            set (captioned[k].second, { x, y + captionH + 2, w, fieldH });
        }
        y += captionH + 2 + fieldH;
    }
}

void InspectorLayout::area (juce::Rectangle<int>* target, int height, juce::Component* right, int rightWidth)
{
    begin();
    juce::Rectangle<int> line (bounds.getX(), y, bounds.getWidth(), height);
    if (right != nullptr)
    {
        set (right, line.removeFromRight (rightWidth));
        line.removeFromRight (theme::space::s);
    }
    if (placing && target != nullptr)
        *target = line;
    y += height;
}

void InspectorLayout::inCell (juce::Component* control, Knob* knob, int height)
{
    if (! placing || control == nullptr || knob == nullptr)
        return;
    // In the knob's hit area, with the knob's caption where the knob has its own (while it shows instead of the knob).
    const auto cell = knob->getBounds();
    const auto css = Knob::cssSize (knob->getKnobSize());
    control->setBounds (cell.withHeight (2 * Knob::margin + css.x).withSizeKeepingCentre (knob->getWidth() + 4, height));
    if (control->isVisible())
        texts.push_back ({ juce::Rectangle<int> (cell.getX(), cell.getBottom() - Knob::margin - captionHeight, cell.getWidth(), captionHeight), knob->getCaption(),
                           Text::Kind::cellCaption });
}

void InspectorLayout::note (const juce::String& words, int lines)
{
    begin();
    const auto height = 16 * juce::jmax (1, lines);
    texts.push_back ({ { bounds.getX(), y, bounds.getWidth(), height }, words, Text::Kind::note });
    y += height;
}

void InspectorLayout::paintTexts (juce::Graphics& g, const std::vector<Text>& list)
{
    for (const auto& t : list)
    {
        switch (t.kind)
        {
            case Text::Kind::heading:
                drawGroupHeading (g, t.area, t.words);
                break;
            case Text::Kind::caption:
                g.setFont (font (theme::Text::label));
                g.setColour (inkDim);
                g.drawFittedText (t.words, t.area.withTrimmedRight (8), juce::Justification::centredLeft, 1, 0.85f);
                break;
            case Text::Kind::fieldCaption:
                g.setFont (font (theme::Text::caption));
                g.setColour (inkFaint);
                g.drawFittedText (t.words, t.area, juce::Justification::centredLeft, 1, 0.85f);
                break;
            case Text::Kind::cellCaption:
                // As a knob's own caption (Knob::paint: 12 px medium in the chrome's label colour).
                g.setFont (geist (Weight::medium, 12.0f));
                g.setColour (theme::knobSkin (theme::Skin::chrome).label);
                g.drawFittedText (t.words, t.area, juce::Justification::centred, 1, 0.85f);
                break;
            case Text::Kind::note:
                g.setFont (font (theme::Text::label));
                g.setColour (inkFaint);
                g.drawFittedText (t.words, t.area, juce::Justification::topLeft, juce::jmax (1, t.area.getHeight() / 16), 0.95f);
                break;
        }
    }
}

// ---- BlockPage -----------------------------------------------------------------------------------------

BlockPage::BlockPage (AmpSimProcessor& processor, BlockId blockId, juce::String titleText, juce::String subtitleText)
    : ControlGroup (processor), id (blockId), title (std::move (titleText)), subtitle (std::move (subtitleText))
{
    // The block's own switch sits in the header. The cab's is its bypass, so it says so.
    if (const auto& i = info (id); *i.onParameter != 0)
    {
        onSwitch = &addSwitch (i.onParameter, i.switchInverted ? "Bypass" : "On");
        onSwitch->setTooltip ("Switch " + title + " on or off (or click the footswitch of its pedal on the board)");
    }
}

BlockPage::~BlockPage() = default;

int BlockPage::headerHeight (const juce::String& sub, int width)
{
    // The title line (22), then the line about the block in up to three lines, then 14 down to the hairline.
    const auto inner = juce::jmax (1, width - 2 * padding);
    const auto lines = sub.isEmpty() ? 0 : juce::jlimit (1, 3, (int) std::ceil (textWidth (font (Text::label), sub) / (float) inner));
    return padding + 22 + (lines > 0 ? 4 + 16 * lines : 0) + 14;
}

int BlockPage::preferredHeight (int width)
{
    const auto top = headerHeight (subtitle, width) + padding;
    InspectorLayout layout ({ padding, top, width - 2 * padding, 0 }, false);
    describeAll (layout);
    return layout.getBottom() + padding + 8;
}

void BlockPage::describeAll (InspectorLayout& l)
{
    describe (l);
    if (generic.empty())
        return;
    // The generic controls: switches together, then each choice on its row, then the knobs three to a row.
    l.section ("More");
    std::vector<Switch*> switches;
    std::vector<Knob*> knobs;
    for (const auto& [caption, c] : generic)
    {
        if (auto* s = dynamic_cast<Switch*> (c))
            switches.push_back (s);
        else if (auto* k = dynamic_cast<Knob*> (c))
            knobs.push_back (k);
    }
    if (! switches.empty())
        l.switches (switches);
    for (const auto& [caption, c] : generic)
        if (dynamic_cast<juce::ComboBox*> (c) != nullptr)
            l.labelled (caption, c);
    l.knobs (knobs);
}

int BlockPage::coverParameters (const juce::String& prefix, const juce::StringArray& except)
{
    // What this page already has a control for: every tagged component inside it.
    juce::StringArray tagged;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        if (const auto tag = c.getProperties()[parameterIdProperty].toString(); tag.isNotEmpty())
            tagged.addIfNotAlreadyThere (tag);
        for (auto* child : c.getChildren())
            walk (*child);
    };
    walk (*this);

    int added = 0;
    for (auto* parameter : ampSim.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        if (ranged == nullptr || ! ranged->paramID.startsWith (prefix) || tagged.contains (ranged->paramID) || except.contains (ranged->paramID))
            continue;
        // Its name without the block's ("Delay Mix" is "Mix" here).
        auto caption = ranged->getName (64);
        if (caption.startsWithIgnoreCase (title + " "))
            caption = caption.substring (title.length() + 1);
        juce::Component* control = nullptr;
        if (dynamic_cast<juce::AudioParameterBool*> (ranged) != nullptr)
            control = &addSwitch (ranged->paramID, caption);
        else if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (ranged))
            control = &addCombo (ranged->paramID, choice->choices);
        else
            control = &addKnob (ranged->paramID, caption, ranged->getLabel().isEmpty() ? juce::String() : " " + ranged->getLabel());
        generic.emplace_back (caption, control);
        ++added;
    }
    if (added > 0)
        resized();
    return added;
}

void BlockPage::paint (juce::Graphics& g)
{
    // The header: the block's name (16 px semibold), where it sits (12 px faint, wrapping), a hairline under it.
    const auto width = getWidth() - 2 * padding;
    g.setFont (geist (Weight::semibold, 16.0f));
    g.setColour (ink);
    g.drawText (title, juce::Rectangle<int> (padding, padding, width - (onSwitch != nullptr ? onSwitch->getWidth() + 8 : 0), 22),
                juce::Justification::centredLeft, true);
    const auto height = headerHeight (subtitle, getWidth());
    if (subtitle.isNotEmpty())
    {
        g.setFont (font (Text::label));
        g.setColour (inkFaint);
        const juce::Rectangle<int> lines (padding, padding + 22 + 4, width, height - padding - 22 - 4 - 14);
        g.drawFittedText (subtitle, lines, juce::Justification::topLeft, juce::jmax (1, lines.getHeight() / 16), 1.0f);
    }
    g.setColour (line1);
    g.fillRect (padding, height - 1, width, 1);

    InspectorLayout::paintTexts (g, texts);
    paintContent (g);
}

void BlockPage::resized()
{
    clearHeadings();
    if (onSwitch != nullptr)
    {
        // The pill's right edge on the column's (the component has a 4 px focus margin all round).
        const auto w = onSwitch->getPreferredWidth();
        onSwitch->setBounds (getWidth() - padding - w + Switch::margin, padding + (22 - Switch::preferredHeight) / 2, w, Switch::preferredHeight);
    }
    const auto top = headerHeight (subtitle, getWidth()) + padding;
    InspectorLayout layout ({ padding, top, getWidth() - 2 * padding, 0 }, true);
    describeAll (layout);
    texts = layout.getTexts();
    repaint();
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
    place (s, row, s->getPreferredWidth(), Switch::preferredHeight, gapAfter);
}

void dim (juce::Component* c, bool used)
{
    if (c != nullptr)
        c->setAlpha (used ? 1.0f : unusedAlpha);
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
    releaseMode = &addCombo (g + "_release_mode", { "Adaptive", "Classic" });
    detector = &addCombo (g + "_detector", { "The clean DI", "Its own input" });
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
        learn = &addButton ("Learn the noise floor", [this] { ampSim.learnGates(); });
        learn->setTooltip ("Mute the strings, then press: measures the noise for 2 s and sets the threshold above it");
    }
}

void GatePage::describe (InspectorLayout& l)
{
    l.labelled ("Release", releaseMode);
    l.labelled ("Detector", detector);
    if (link != nullptr)
        l.switches ({ link });
    else
        l.buttons ({ learn });

    l.section ("Threshold");
    l.knobs ({ knobs[0], knobs[1], knobs[5] });
    l.section ("Timing");
    l.knobs ({ knobs[2], knobs[3], knobs[4] });
    l.section ("Sidechain");
    l.switches ({ sidechain });
    l.knobs ({ knobs[6] });
    l.section ("Detector");
    l.area (&meterArea, 100, reduction, 40);
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
        learn->setButtonText (learning ? "Learning..." : "Learn the noise floor");
    reduction->setReduction (juce::jmin (meter.reductionDb, 40.0f));
    if (changed)
        repaint (meterArea);
}

void GatePage::paintContent (juce::Graphics& g)
{
    // What it's doing in words (up to three lines), then the detector's level on a -100 to 0 dBFS scale
    // against the open (ink) and close (ink-faint) thresholds; the gap between them is the hysteresis.
    auto area = meterArea;
    const auto scale = area.removeFromBottom (14);
    area.removeFromBottom (2);
    const auto bar = area.removeFromBottom (12);
    area.removeFromBottom (8);
    const auto textArea = area;
    const auto xFor = [&bar] (float db) { return (float) bar.getX() + (float) bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 100.0f); };

    g.setColour (line2);
    g.fillRoundedRectangle (bar.toFloat(), 3.0f);
    if (! linked)
    {
        g.setColour (meter.open ? accent : inkDim);
        g.fillRoundedRectangle (bar.toFloat().withRight (xFor (meter.detectorDb)), 3.0f);
        g.setColour (text);
        g.fillRect (juce::Rectangle<float> (xFor (meter.openDb) - 1.0f, (float) bar.getY() - 3.0f, 2.0f, (float) bar.getHeight() + 6.0f));
        g.setColour (inkFaint);
        g.fillRect (juce::Rectangle<float> (xFor (meter.closeDb) - 1.0f, (float) bar.getY() - 3.0f, 2.0f, (float) bar.getHeight() + 6.0f));
    }

    // The scale's ends, and its middle.
    g.setFont (font (Text::caption));
    g.setColour (textDim);
    g.drawText ("-100", scale, juce::Justification::centredLeft, false);
    g.drawText ("-50", scale, juce::Justification::centred, false);
    g.drawText ("0 dBFS", scale, juce::Justification::centredRight, false);

    const auto dB = [] (float value) { return juce::String (juce::roundToInt (value)); };
    juce::String line;
    if (learning)
        line = "Learning the noise floor: keep the strings muted (" + juce::String (juce::roundToInt (learnProgress * 100.0f)) + "%)";
    else if (linked)
        line = juce::String ("Following Gate A: ") + (meter.reductionDb < 1.0f ? "open" : "closing, " + dB (meter.reductionDb) + " dB down");
    else
        line = (meter.open ? "Open" : "Closed") + juce::String (", level ") + dB (meter.detectorDb) + " dBFS, reduction "
               + (meter.reductionDb >= 99.0f ? juce::String ("full") : dB (meter.reductionDb) + " dB") + ". The bright mark opens it, the dim one closes it.";
    g.setColour (learning ? warn : text);
    g.setFont (font (Text::label));
    g.drawFittedText (line, textArea, juce::Justification::topLeft, juce::jmax (1, textArea.getHeight() / 16), 1.0f);
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
    autoRelease = &addSwitch (prefix + "_auto_release", "Auto release");
    autoMakeup = &addSwitch (prefix + "_auto_makeup", "Auto makeup");
    sidechain = &addSwitch (prefix + "_sc_hpf", "High-pass");
    reduction = &adopt (std::make_unique<ReductionMeter> (24.0f));
    mode->setTooltip ("Studio: feed-forward and precise. Pedal: feedback, like a stompbox.");
    autoRelease->setTooltip ("Auto release: fast after short peaks, slow after long compression");
    autoMakeup->setTooltip (juce::String ("Auto makeup: keeps the level steady whatever the threshold and ratio (it restores a ")
                            + (post ? "-12" : "-21") + " dBFS signal, where the " + (post ? "signal after the cab" : "guitar") + " typically sits)");
}

void CompressorPage::describe (InspectorLayout& l)
{
    l.labelled ("Mode", mode);
    l.labelled ("Detector", detector);
    l.area (&curveArea, 168, reduction, 40);
    l.section ("Threshold and ratio");
    l.knobs ({ knobs[0], knobs[1], knobs[2] });
    l.section ("Timing");
    l.knobs ({ knobs[3], knobs[4] });
    l.switches ({ autoRelease });
    l.section ("Output");
    l.knobs ({ knobs[5], knobs[6] });
    l.switches ({ autoMakeup });
    l.section ("Sidechain");
    l.switches ({ sidechain });
    l.knobs ({ knobs[7] });
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
    g.setColour (line1);
    g.drawRoundedRectangle (box.reduced (0.5f), radiusControl, 1.0f);
    const auto plot = box.reduced (10.0f);
    const auto xOf = [&plot] (double db) { return plot.getX() + plot.getWidth() * (float) ((db + 60.0) / 60.0); };
    const auto yOf = [&plot] (double db) { return plot.getBottom() - plot.getHeight() * (float) ((juce::jlimit (-60.0, 0.0, db) + 60.0) / 60.0); };

    g.setColour (line1);
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

/// A label that wraps (the "this mode" text).
juce::Label& wrapping (juce::Label& label)
{
    label.setJustificationType (juce::Justification::topLeft);
    label.setMinimumHorizontalScale (1.0f);
    return label;
}
} // namespace

BoostPage::BoostPage (AmpSimProcessor& p) : BlockPage (p, BlockId::boost, "Boost", "Before the amp")
{
    mode = &addCombo ("boost_mode", { "Clean", "Tight", "Screamer" });
    level = &addKnob ("boost_level", "Level");
    tilt = &addKnob ("boost_tilt", "Tilt");
    tightHz = &addKnob ("boost_tight_freq", "Tight", " Hz");
    mid = &addKnob ("boost_mid", "Mid push");
    about = &wrapping (addLabel ({}, Text::label, inkDim));
}

void BoostPage::describe (InspectorLayout& l)
{
    l.labelled ("Mode", mode);
    l.row (about, 48);
    l.section ("Level and tilt");
    l.knobs ({ level, tilt });
    l.section ("Tight");
    l.knobs ({ tightHz, mid });
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
    tight = &addSwitch ("od_tight", "Tight");
    tightHz = &addKnob ("od_tight_freq", "Frequency", " Hz");
    oversampling = &addCombo ("drive_oversampling", { "4x", "8x" });
    oversamplingNote = &wrapping (addLabel ("For the boost and the overdrive. 8x costs about twice the CPU. A global setting, not saved in presets.",
                                            Text::label, inkFaint));
    about = &wrapping (addLabel ({}, Text::label, inkDim));
}

void OverdrivePage::describe (InspectorLayout& l)
{
    l.labelled ("Mode", mode);
    l.row (about, 48);
    l.section ("Drive");
    l.knobs ({ drive, tone });
    l.section ("Output");
    l.knobs ({ level, mix });
    l.section ("Tight");
    l.switches ({ tight });
    l.knobs ({ tightHz });
    l.section ("Oversampling");
    l.labelled ("Factor", oversampling);
    l.row (oversamplingNote, 48);
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

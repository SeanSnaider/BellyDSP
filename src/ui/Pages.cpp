// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Pages.h"
#include "InspectorParts.h"
#include "PedalArt.h"

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

ChoiceBar& ControlGroup::addChoice (const juce::String& parameterId, const juce::StringArray& labels)
{
    return adopt (std::make_unique<ChoiceBar> (state, parameterId, labels));
}

PadSwitch& ControlGroup::addPad (const juce::String& parameterId, const juce::String& label, const juce::String& detail)
{
    auto& control = adopt (std::make_unique<PadSwitch> (label, detail));
    buttonAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, parameterId, tagged (control, parameterId)));
    control.onUserToggle = [this, parameterId] (bool on) { ampSim.switchedByUser (parameterId, on); };
    return control;
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

void InspectorLayout::hero (const std::vector<Knob*>& list, juce::Component* beside, int besideHeight)
{
    // Two to a row, each centred in its half; the rows are as tall as a large knob.
    const auto cellWidth = bounds.getWidth() / 2;
    const auto height = Knob::preferredHeight (Knob::Size::large);
    for (size_t i = 0; i < list.size(); i += 2)
    {
        begin();
        for (size_t k = i; k < juce::jmin (list.size(), i + 2); ++k)
        {
            const auto cell = juce::Rectangle<int> (bounds.getX() + (int) (k - i) * cellWidth, y, cellWidth, height);
            if (auto* knob = list[k])
            {
                knob->setKnobSize (Knob::Size::large);
                set (knob, cell.withSizeKeepingCentre (Knob::preferredWidth (Knob::Size::large), height));
            }
            else if (beside != nullptr)
            {
                set (beside, cell.withSizeKeepingCentre (cell.getWidth() - 8, juce::jmin (height, besideHeight > 0 ? besideHeight : height)));
            }
        }
        y += height;
    }
}

void InspectorLayout::choice (ChoiceBar* bar, const juce::String& caption)
{
    if (bar == nullptr)
        return;
    if (caption.isNotEmpty())
        labelled (caption, bar, bar->getPreferredHeight());
    else
        row (bar, bar->getPreferredHeight());
}

void InspectorLayout::knobs (const std::vector<Knob*>& list, int perRow)
{
    // Three to a row (or perRow), each in its share of the column: the columns line up from one section to the next.
    perRow = juce::jmax (1, perRow);
    const auto cellWidth = bounds.getWidth() / perRow;
    for (size_t i = 0; i < list.size(); i += (size_t) perRow)
    {
        const auto end = juce::jmin (list.size(), i + (size_t) perRow);
        // Every knob in an inspector shows its value under its caption (RD4-7), so the row is that much taller.
        auto height = 0;
        for (auto k = i; k < end; ++k)
            if (list[k] != nullptr)
            {
                list[k]->setValueShown (true);
                height = juce::jmax (height, list[k]->getPreferredHeight());
            }
        if (height == 0)
            continue;
        begin();
        for (auto k = i; k < end; ++k)
            if (auto* knob = list[k])
            {
                const auto cell = juce::Rectangle<int> (bounds.getX() + (int) (k - i) * cellWidth, y, cellWidth, height);
                set (knob, cell.withSizeKeepingCentre (Knob::preferredWidth (knob->getKnobSize()), knob->getPreferredHeight()));
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
    const auto hit = juce::roundToInt (Knob::hitSize (knob->getKnobSize()));
    control->setBounds (cell.withHeight (2 * Knob::margin + hit).withSizeKeepingCentre (juce::jmin (knob->getWidth() + 4, bounds.getWidth() / 2 - 8), height));
    if (control->isVisible())
        texts.push_back ({ juce::Rectangle<int> (cell.getX(), cell.getY() + Knob::margin + hit + 6, cell.getWidth(), captionHeight), knob->getCaption(),
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
                drawInspectorHeading (g, t.area, t.words);
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
        onSwitch->onStateChange = [this] { repaint (0, 0, getWidth(), headerHeight (subtitle, getWidth())); }; // the thumbnail's LED
    }
}

BlockPage::~BlockPage() = default;

int BlockPage::headerHeight (const juce::String& sub, int width)
{
    // The thumbnail's height, or the title line (28) and the line about the block in up to three lines if that's
    // taller; then 14 down to the hairline.
    const auto inner = juce::jmax (1, width - textLeft - padding);
    const auto lines = sub.isEmpty() ? 0 : juce::jlimit (1, 3, (int) std::ceil (textWidth (font (Text::label), sub) / (float) inner));
    return padding + juce::jmax (thumbHeight, 28 + 16 * lines) + 14;
}

juce::Rectangle<int> BlockPage::headerSwitchBounds (int width, int switchWidth)
{
    return { width - padding - switchWidth + Switch::margin, padding + 3 + (22 - Switch::preferredHeight) / 2, switchWidth, Switch::preferredHeight };
}

void BlockPage::paintHeader (juce::Graphics& g, int width, const juce::String& title, const juce::String& subtitle,
                             const std::function<void (juce::Graphics&, juce::Rectangle<float>)>& thumbnail, int switchWidth)
{
    // The gear in small, on a faint plinth so a light enclosure and a dark one both sit on the panel.
    const auto plinth = juce::Rectangle<float> ((float) padding, (float) padding, (float) thumbWidth, (float) thumbHeight);
    g.setColour (bg.withAlpha (0.7f));
    g.fillRoundedRectangle (plinth, 6.0f);
    if (thumbnail)
        thumbnail (g, plinth.reduced (2.0f));

    const auto textWidthLeft = width - textLeft - padding;
    g.setFont (geist (Weight::semibold, 17.0f));
    g.setColour (ink);
    g.drawText (title, juce::Rectangle<int> (textLeft, padding + 3, textWidthLeft - (switchWidth > 0 ? switchWidth + 4 : 0), 22), juce::Justification::centredLeft,
                true);
    const auto height = headerHeight (subtitle, width);
    if (subtitle.isNotEmpty())
    {
        g.setFont (font (Text::label));
        g.setColour (inkFaint);
        const juce::Rectangle<int> lines (textLeft, padding + 28, textWidthLeft, height - padding - 28 - 14);
        g.drawFittedText (subtitle, lines, juce::Justification::topLeft, juce::jmax (1, lines.getHeight() / 16), 1.0f);
    }
    g.setColour (line1);
    g.fillRect (padding, height - 1, width - 2 * padding, 1);
}

void BlockPage::dimAll (std::initializer_list<juce::Component*> list, float alpha)
{
    for (auto* c : list)
        if (c != nullptr)
            c->setAlpha (alpha);
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
    // The header: the pedal in small with its LED as on the board, its name, where it sits, its switch.
    const auto on = onSwitch != nullptr && onSwitch->getToggleState();
    const auto sweep = headerSweep();
    paintHeader (g, getWidth(), title, subtitle, [this, on, sweep] (juce::Graphics& gg, juce::Rectangle<float> box) { drawPedal (gg, box, id, { on, false, false, false, sweep }); },
                 onSwitch != nullptr ? onSwitch->getWidth() : 0);
    InspectorLayout::paintTexts (g, texts);
    paintContent (g);
}

void BlockPage::resized()
{
    clearHeadings();
    if (onSwitch != nullptr)
        onSwitch->setBounds (headerSwitchBounds (getWidth(), onSwitch->getPreferredWidth()));
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
                 isGateB ? "After the amp, before the cab" : "Before the amp; it listens to the clean DI"),
      gateB (isGateB)
{
    const juce::String g = isGateB ? "gate_b" : "gate_a";
    meterView = &adopt (std::make_unique<GateMeterView> (state, g + "_threshold"));
    threshold = &addKnob (g + "_threshold", "Threshold", " dB", Knob::Size::large);
    release = &addKnob (g + "_release", "Release", " ms", Knob::Size::large);
    attack = &addKnob (g + "_attack", "Attack", " ms");
    hold = &addKnob (g + "_hold", "Hold", " ms");
    range = &addKnob (g + "_range", "Range");
    hysteresis = &addKnob (g + "_hysteresis", "Hysteresis");
    scFreq = &addKnob (g + "_sc_freq", "High-pass", " Hz");
    range->setTooltip ("How far the gate turns the signal down when it closes (the bottom of the range mutes it)");
    hysteresis->setTooltip ("How far below the threshold the level must fall before the gate closes (the shaded band on the meter)");

    releaseMode = &addChoice (g + "_release_mode", { "Adaptive", "Fixed" });
    releaseMode->setOptionTooltips ({ "Adaptive: the release follows how the note dies away", "Fixed: the Release knob, always, as a classic gate" });
    detector = &addChoice (g + "_detector", { "The clean DI", "Its own input" });
    detector->setTooltip ("What the gate listens to: the clean guitar, or the signal arriving at the gate");
    sidechain = &addSwitch (g + "_sc_hpf", "High-pass");
    sidechain->setTooltip ("A high-pass on what the gate listens to, so low thumps and hum don't hold it open");

    if (gateB)
    {
        link = &addPad ("gate_link", "Linked to Gate A", "Applies Gate A's decision after the amp");
        link->setTooltip ("Linked, Gate B applies Gate A's decision after the amp (its own settings wait, dimmed)");
    }
    else
    {
        learn = &addButton ("Learn the noise floor", [this] { ampSim.learnGates(); });
        learn->setTooltip ("Mute the strings, then press: measures the noise for 2 s and sets the threshold above it");
    }
}

GatePage::~GatePage()
{
    stopTimer();
}

void GatePage::describe (InspectorLayout& l)
{
    // The meter first: what the gate is doing is what the player looks for; then the one button that sets it up.
    l.row (meterView, GateMeterView::preferredHeight);
    if (link != nullptr)
        l.row (link, PadSwitch::preferredPadHeight);
    else
        l.row (learn, controlHeight + 4);
    l.hero ({ threshold, release });
    l.choice (releaseMode, "Release");
    l.section ("Response");
    l.knobs ({ attack, hold, range });
    l.section ("Detector", sidechain);
    l.choice (detector);
    l.knobs ({ hysteresis, scFreq });
}

void GatePage::pageShown()
{
    updateMeter();
    startTimerHz (meterFps);
}

void GatePage::pageHidden()
{
    stopTimer();
}

void GatePage::updateMeter()
{
    GateMeterView::Reading r;
    r.meter = ampSim.getGateMeter (gateB);
    r.on = state.getRawParameterValue (gateB ? "gate_b_on" : "gate_a_on")->load() >= 0.5f;
    r.linked = linked;
    r.learning = learning;
    r.progress = ampSim.getGateLearnProgress();
    meterView->set (r);
}

void GatePage::refresh()
{
    const auto newLinked = gateB && state.getRawParameterValue ("gate_link")->load() >= 0.5f;
    learning = ampSim.isLearningGates() && (! gateB || ampSim.isGateBOnItsOwn());

    const auto sc = state.getRawParameterValue (gateB ? "gate_b_sc_hpf" : "gate_a_sc_hpf")->load() >= 0.5f ? 1 : 0;
    if (newLinked != linked || sc != shownSidechain)
    {
        // Linked, Gate B's own settings don't apply: dim them (they stay editable for when it's unlinked). The
        // sidechain's frequency only matters while its high-pass is on.
        linked = newLinked;
        shownSidechain = sc;
        for (auto* c : std::initializer_list<juce::Component*> { threshold, release, attack, hold, range, hysteresis, releaseMode, detector, sidechain })
            dim (c, ! linked);
        scFreq->setAlpha (linked ? unusedAlpha : (sc == 1 ? 1.0f : unusedAlpha));
    }
    if (learn != nullptr)
        learn->setButtonText (learning ? "Learning, keep the strings muted (" + juce::String (juce::roundToInt (ampSim.getGateLearnProgress() * 100.0f)) + "%)"
                                       : juce::String ("Learn the noise floor"));
    updateMeter();
}

// ---- AutoWahPage -----------------------------------------------------------------------------------------

namespace
{
/// The wah's knobs' values: whole percents, hertz as the meter writes them, milliseconds with a decimal under 10.
juce::String percentText (float v) { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String millisecondsText (float v) { return v < 9.95f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
} // namespace

AutoWahPage::AutoWahPage (AmpSimProcessor& p) : BlockPage (p, BlockId::wah, "Auto-wah", "Before the amp; it listens to the clean DI")
{
    meterView = &adopt (std::make_unique<SweepMeterView>());
    direction = &addChoice ("wah_direction", { "Up", "Down" });
    direction->setOptionTooltips ({ "Up: a harder pick opens the filter, and it closes as the note dies away (the classic)",
                                    "Down: a harder pick closes the filter, and it opens as the note dies away" });
    sensitivity = &addKnob ("wah_sensitivity", "Sensitivity", " %", Knob::Size::large);
    resonance = &addKnob ("wah_q", "Resonance", "", Knob::Size::large);
    low = &addKnob ("wah_freq_low", "Low", " Hz");
    high = &addKnob ("wah_freq_high", "High", " Hz");
    attack = &addKnob ("wah_attack", "Attack", " ms");
    release = &addKnob ("wah_release", "Release", " ms");
    mix = &addKnob ("wah_mix", "Mix", " %");

    sensitivity->setFormatter (percentText);
    mix->setFormatter (percentText);
    resonance->setFormatter ([] (float v) { return juce::String (v, 1); });
    low->setFormatter ([] (float v) { return hertzText (v); });
    high->setFormatter ([] (float v) { return hertzText (v); });
    attack->setFormatter (millisecondsText);
    release->setFormatter (millisecondsText);

    sensitivity->setTooltip ("How readily the filter opens: more opens it on lighter playing, less needs a harder pick");
    resonance->setTooltip ("How narrow and vocal the filter's peak is (its Q); the level stays about the same whatever it's set to");
    low->setTooltip ("Where the filter sits when it's closed: the heel of a wah");
    high->setTooltip ("Where the filter goes when it's fully open: the toe");
    attack->setTooltip ("How fast the filter follows a pick as it opens");
    release->setTooltip ("How fast it closes again as the note dies away");
    mix->setTooltip ("The filtered sound against the dry guitar (100 % is all wah)");
}

AutoWahPage::~AutoWahPage()
{
    stopTimer();
}

void AutoWahPage::describe (InspectorLayout& l)
{
    // What it's doing first, then the mode, the two knobs that shape it most, and the rest in sections.
    l.row (meterView, SweepMeterView::preferredHeight);
    l.choice (direction, "Direction");
    l.hero ({ sensitivity, resonance });
    l.section ("Sweep");
    l.knobs ({ low, high });
    l.section ("Response");
    l.knobs ({ attack, release });
    l.section ("Output");
    l.knobs ({ mix });
}

void AutoWahPage::pageShown()
{
    updateMeter();
    startTimerHz (meterFps);
}

void AutoWahPage::pageHidden()
{
    stopTimer();
}

float AutoWahPage::headerSweep() const
{
    return shownHeaderSweep;
}

void AutoWahPage::updateMeter()
{
    // It sounds while its switch and the pre section's are on; otherwise the control holds its last value, so the meter
    // and the header's window rest.
    SweepMeterView::Reading r;
    r.on = state.getRawParameterValue ("wah_on")->load() >= 0.5f && state.getRawParameterValue ("pre_fx_on")->load() >= 0.5f;
    r.down = juce::roundToInt (state.getRawParameterValue ("wah_direction")->load()) == 1;
    r.control = r.on ? juce::jlimit (0.0f, 1.0f, ampSim.getWahControl()) : (r.down ? 1.0f : 0.0f);
    r.lowHz = state.getRawParameterValue ("wah_freq_low")->load();
    r.highHz = state.getRawParameterValue ("wah_freq_high")->load();
    meterView->set (r);

    const auto sweep = r.on ? r.control : -1.0f;
    if ((sweep < 0.0f) != (shownHeaderSweep < 0.0f) || std::abs (sweep - shownHeaderSweep) >= 0.002f)
    {
        shownHeaderSweep = sweep;
        repaint (thumbnailArea());
    }
}

void AutoWahPage::refresh()
{
    updateMeter();
}

// ---- CompressorPage --------------------------------------------------------------------------------------

CompressorPage::CompressorPage (AmpSimProcessor& p, bool isPost)
    : BlockPage (p, isPost ? BlockId::postCompressor : BlockId::preCompressor, "Compressor", isPost ? "After the cab" : "Before the amp"),
      post (isPost),
      prefix (isPost ? "comp_post" : "comp_pre")
{
    mode = &addChoice (prefix + "_mode", { "Studio", "Pedal" });
    mode->setOptionTooltips ({ "Studio: feed-forward and precise", "Pedal: feedback, like a stompbox" });
    meterView = &adopt (std::make_unique<CompressorMeterView> (state, prefix));
    threshold = &addKnob (prefix + "_threshold", "Threshold", " dB", Knob::Size::large);
    ratio = &addKnob (prefix + "_ratio", "Ratio", ":1", Knob::Size::large);
    attack = &addKnob (prefix + "_attack", "Attack", " ms");
    release = &addKnob (prefix + "_release", "Release", " ms");
    knee = &addKnob (prefix + "_knee", "Knee");
    makeup = &addKnob (prefix + "_makeup", "Makeup");
    mix = &addKnob (prefix + "_mix", "Mix", " %");
    scFreq = &addKnob (prefix + "_sc_freq", "High-pass", " Hz");
    detector = &addChoice (prefix + "_detector", { "Peak", "RMS" });
    detector->setOptionTooltips ({ "Peak: follows every transient", "RMS: follows the average level, gentler" });
    autoRelease = &addSwitch (prefix + "_auto_release", "Auto");
    autoMakeup = &addSwitch (prefix + "_auto_makeup", "Auto");
    sidechain = &addSwitch (prefix + "_sc_hpf", "High-pass");
    autoRelease->setTooltip ("Auto release: fast after short peaks, slow after long compression");
    autoMakeup->setTooltip (juce::String ("Auto makeup: keeps the level steady whatever the threshold and ratio (it restores a ")
                            + (post ? "-12" : "-21") + " dBFS signal, where the " + (post ? "signal after the cab" : "guitar") + " typically sits)");
    sidechain->setTooltip ("A high-pass on what the compressor listens to, so the low strings don't pump it");
}

CompressorPage::~CompressorPage()
{
    stopTimer();
}

void CompressorPage::describe (InspectorLayout& l)
{
    l.choice (mode);
    l.row (meterView, CompressorMeterView::preferredHeight);
    l.hero ({ threshold, ratio });
    l.section ("Response", autoRelease);
    l.knobs ({ attack, release, knee });
    l.section ("Output", autoMakeup);
    l.knobs ({ makeup, mix });
    l.section ("Detector", sidechain);
    l.choice (detector);
    l.knobs ({ scFreq });
}

void CompressorPage::pageShown()
{
    updateMeter();
    startTimerHz (meterFps);
}

void CompressorPage::pageHidden()
{
    stopTimer();
}

void CompressorPage::updateMeter()
{
    meterView->push (ampSim.getCompressorReduction (post));
}

void CompressorPage::refresh()
{
    meterView->refreshCurve();
    if (const auto sc = state.getRawParameterValue (prefix + "_sc_hpf")->load() >= 0.5f ? 1 : 0; sc != shownSidechain)
    {
        shownSidechain = sc;
        dim (scFreq, sc == 1);
    }
}

// ---- BoostPage, OverdrivePage ------------------------------------------------------------------------

namespace
{
/// What each mode is (BUILD_PLAN "Boost and Overdrive"), in generic words: no product names in the UI.
juce::String boostModeText (int mode)
{
    switch (mode)
    {
        case 0:  return "Flat gain with a tilt: the lead boost. Uses Level and Tilt.";
        case 1:  return "A high-pass and a mid push, no clipping, to tighten a high-gain amp. Uses Level, Tight, and Mid push.";
        case 2:  return "The Mid Drive circuit at minimal drive: the classic metal boost, with slight compression. Uses Level.";
        default: break;
    }
    return {};
}

juce::String overdriveModeText (int mode)
{
    switch (mode)
    {
        case 0:  return "Diodes in the op-amp's feedback loop: soft clipping with a mid hump.";
        case 1:  return "Hard-clipping diodes to ground, and the op-amp's slew limiting at high gain.";
        case 2:  return "The clean and clipped signals blended, so the low end stays.";
        case 3:  return "Four transistor clipping stages with a tone control between them.";
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
    mode = &addChoice ("boost_mode", { "Clean", "Tight", "Screamer" });
    mode->setOptionTooltips ({ boostModeText (0), boostModeText (1), boostModeText (2) });
    level = &addKnob ("boost_level", "Level", " dB", Knob::Size::large);
    tilt = &addKnob ("boost_tilt", "Tilt", " dB", Knob::Size::large);
    tightHz = &addKnob ("boost_tight_freq", "Tight", " Hz");
    mid = &addKnob ("boost_mid", "Mid push");
    about = &wrapping (addLabel ({}, Text::label, inkDim));
}

void BoostPage::describe (InspectorLayout& l)
{
    l.choice (mode);
    l.row (about, 48);
    l.hero ({ level, tilt });
    l.section ("Tight mode");
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
    mode = &addChoice ("od_mode", params::OverdriveParameters::modeNames());
    mode->setColumns (2);
    mode->setOptionTooltips ({ overdriveModeText (0), overdriveModeText (1), overdriveModeText (2), overdriveModeText (3) });
    drive = &addKnob ("od_drive", "Drive", " %", Knob::Size::large);
    level = &addKnob ("od_level", "Level", " dB", Knob::Size::large);
    tone = &addKnob ("od_tone", "Tone", " %");
    mix = &addKnob ("od_mix", "Mix", " %");
    tight = &addSwitch ("od_tight", "Tight");
    tight->setTooltip ("A high-pass before the clipping, so the low end stays tight");
    tightHz = &addKnob ("od_tight_freq", "Tight", " Hz");
    oversampling = &addChoice ("drive_oversampling", { "4x", "8x" });
    oversamplingNote = &wrapping (addLabel ("For the boost and the overdrive. 8x costs about twice the CPU. A global setting, not saved in presets.",
                                            Text::label, inkFaint));
    about = &wrapping (addLabel ({}, Text::label, inkDim));
}

void OverdrivePage::describe (InspectorLayout& l)
{
    l.choice (mode);
    l.row (about, 32);
    l.hero ({ drive, level });
    l.section ("Shape", tight);
    l.knobs ({ tone, mix, tightHz });
    l.section ("Oversampling");
    l.choice (oversampling);
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

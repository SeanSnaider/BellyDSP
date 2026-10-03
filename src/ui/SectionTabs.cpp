// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "SectionTabs.h"

namespace ui
{

using namespace theme;

namespace
{
juce::FontOptions nameFont() { return geist (Weight::medium, 15.0f); }
juce::FontOptions suffixFont() { return geist (Weight::regular, 11.0f); }

bool sameItems (const std::vector<TabRow::Item>& a, const std::vector<TabRow::Item>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].label != b[i].label || a[i].suffix != b[i].suffix || a[i].parameterId != b[i].parameterId)
            return false;
    return true;
}
} // namespace

class TabRow::Tab final : public juce::Component
{
public:
    Tab (TabRow& r, AmpSimProcessor& p, const Item& i) : item (i), row (r)
    {
        setWantsKeyboardFocus (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle (item.label);
        if (item.parameterId.isNotEmpty())
        {
            dot = std::make_unique<PowerSwitch> (p.parameters, item.parameterId, item.inverted);
            dot->setTooltip ("Switch " + item.label + " on or off");
            addAndMakeVisible (*dot);
        }
    }

    const Item item;

    void paint (juce::Graphics& g) override
    {
        const bool isSelected = row.getSelected() == item.id;
        const bool hovered = isMouseOverOrDragging (true);
        auto x = 0.0f;
        const auto contentCentre = 10.0f; // the 20 px content box above the 12 px padding

        if (row.iconWidth > 0 && row.drawIcon)
        {
            row.drawIcon (g, juce::Rectangle<float> (x, contentCentre - (float) row.iconHeight * 0.5f, (float) row.iconWidth, (float) row.iconHeight), item.id);
            x += (float) row.iconWidth + 10.0f;
        }

        const auto nf = nameFont();
        const auto w = textWidth (nf, item.label);
        g.setFont (nf);
        g.setColour (isSelected || hovered ? ink : inkDim);
        g.drawText (item.label, juce::Rectangle<float> (x, contentCentre - 10.0f, w + 2.0f, 20.0f), juce::Justification::centredLeft, false);
        x += w;

        if (item.suffix.isNotEmpty())
        {
            // Baseline-aligned with the name (CSS align-items: center puts the 11 px box's centre on the
            // row's; the small text then sits a little low, as in the screenshots).
            const auto sf = suffixFont();
            g.setFont (sf);
            g.setColour (inkFaint);
            g.drawText (item.suffix, juce::Rectangle<float> (x + 10.0f, contentCentre - 9.0f, textWidth (sf, item.suffix) + 2.0f, 20.0f),
                        juce::Justification::centredLeft, false);
        }

        if (isSelected)
        {
            g.setColour (accent);
            g.fillRect (juce::Rectangle<float> (0.0f, (float) getHeight() - 1.0f, (float) getWidth(), 1.0f));
        }
        if (keyboardFocus)
            drawFocusRing (g, getLocalBounds().toFloat().withTrimmedBottom (12.0f));
    }

    void resized() override
    {
        if (dot != nullptr)
            dot->setBounds (getWidth() - 12, 2, 17, 17);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        if (! e.mods.isPopupMenu())
            downX = e.getEventRelativeTo (&row).position.x;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || ! row.onReorder)
            return;
        const auto x = e.getEventRelativeTo (&row).position.x;
        if (! dragging && std::abs (x - downX) > 4.0f)
        {
            dragging = true;
            row.beginDrag (item.id, downX - (float) getX());
        }
        if (dragging)
            row.dragTo (x);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (dragging)
        {
            dragging = false;
            row.endDrag();
            return;
        }
        if (getLocalBounds().contains (e.getPosition()) && row.onSelect)
            row.onSelect (item.id);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if ((key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey) && row.onSelect)
        {
            row.onSelect (item.id);
            return true;
        }
        return false;
    }

    void focusGained (FocusChangeType cause) override
    {
        keyboardFocus = cause != focusChangedByMouseClick;
        repaint();
    }
    void focusLost (FocusChangeType) override
    {
        keyboardFocus = false;
        repaint();
    }

private:
    TabRow& row;
    std::unique_ptr<PowerSwitch> dot;
    float downX = 0.0f;
    bool dragging = false, keyboardFocus = false;
};

TabRow::TabRow (AmpSimProcessor& p) : ampSim (p) {}

TabRow::~TabRow() = default;

int TabRow::tabWidth (const Item& item) const
{
    auto w = textWidth (nameFont(), item.label);
    if (iconWidth > 0)
        w += (float) iconWidth + 10.0f;
    if (item.suffix.isNotEmpty())
        w += 10.0f + textWidth (suffixFont(), item.suffix);
    if (item.parameterId.isNotEmpty())
        w += 10.0f + 7.0f;
    return (int) std::ceil (w);
}

void TabRow::setItems (const std::vector<Item>& newItems)
{
    if (sameItems (items, newItems))
        return;
    items = newItems;
    tabs.clear();
    for (const auto& item : items)
    {
        tabs.push_back (std::make_unique<Tab> (*this, ampSim, item));
        addAndMakeVisible (*tabs.back());
    }
    place();
}

void TabRow::setSelected (int id)
{
    if (id != selected)
    {
        selected = id;
        for (auto& t : tabs)
            t->repaint();
    }
}

juce::Component* TabRow::getTab (int id) const
{
    for (const auto& t : tabs)
        if (t->item.id == id)
            return t.get();
    return nullptr;
}

std::vector<int> TabRow::getShownOrder() const
{
    std::vector<int> order;
    for (const auto& item : items)
        order.push_back (item.id);
    return order;
}

void TabRow::paint (juce::Graphics& g)
{
    g.setColour (line1);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void TabRow::resized()
{
    place();
}

void TabRow::place()
{
    // Left to right, 30 apart. While a tab is dragged, the others leave a gap where it would land and it
    // follows the pointer.
    std::vector<size_t> others;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id != dragged)
            others.push_back (i);

    int x = 0;
    for (size_t k = 0; k <= others.size(); ++k)
    {
        if (dragged >= 0 && (int) k == dragInsert)
        {
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i].id == dragged)
                    x += tabWidth (items[i]) + gap;
        }
        if (k == others.size())
            break;
        const auto i = others[k];
        const auto w = tabWidth (items[i]);
        tabs[i]->setBounds (x, 0, w, height);
        x += w + gap;
    }
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == dragged)
        {
            const auto w = tabWidth (items[i]);
            tabs[i]->setBounds (juce::jlimit (0, juce::jmax (0, getWidth() - w), juce::roundToInt (dragX - dragGrab)), 0, w, height);
            tabs[i]->toFront (false);
        }
}

void TabRow::beginDrag (int id, float grabOffset)
{
    dragged = id;
    dragGrab = grabOffset;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id)
            dragInsert = (int) i;
}

void TabRow::dragTo (float x)
{
    if (dragged < 0)
        return;
    dragX = x;

    // Where it lands: before the first other tab whose centre is right of the dragged tab's centre.
    int draggedWidth = 0;
    for (const auto& item : items)
        if (item.id == dragged)
            draggedWidth = tabWidth (item);
    const auto centre = x - dragGrab + (float) draggedWidth * 0.5f;
    int insert = 0, position = 0;
    for (const auto& item : items)
    {
        if (item.id == dragged)
            continue;
        const auto w = tabWidth (item);
        if ((float) position + (float) w * 0.5f < centre)
            ++insert;
        position += w + gap;
    }
    dragInsert = insert;
    place();
}

void TabRow::endDrag()
{
    if (dragged < 0)
        return;
    std::vector<int> order;
    for (const auto& item : items)
        if (item.id != dragged)
            order.push_back (item.id);
    order.insert (order.begin() + juce::jlimit (0, (int) order.size(), dragInsert), dragged);
    const auto before = getShownOrder();
    dragged = -1;
    place();
    if (order != before && onReorder)
        onReorder (order);
}

} // namespace ui

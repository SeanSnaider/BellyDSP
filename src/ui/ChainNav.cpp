// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ChainNav.h"

namespace ui
{

using namespace theme;

ChainIcon chainIcon (PageId page)
{
    // The reference's paths, verbatim (ui_reference.html, the rail's <svg class="ico">), in 24 units.
    ChainIcon icon;
    auto& p = icon.strokes;
    const auto path = [&p] (const char* d) { p.addPath (juce::Drawable::parseSVGPath (d)); };
    const auto circle = [] (juce::Path& into, float cx, float cy, float r) { into.addEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r); };

    switch (page)
    {
        case PageId::input:
            circle (p, 7.0f, 12.0f, 3.0f);
            path ("M10 12h10M17 9v6");
            break;
        case PageId::preFx:
            p.addRoundedRectangle (6.0f, 3.0f, 12.0f, 18.0f, 2.0f);
            circle (p, 9.5f, 7.5f, 1.1f);
            circle (p, 14.5f, 7.5f, 1.1f);
            circle (p, 12.0f, 15.5f, 2.3f);
            break;
        case PageId::amp:
            p.addRoundedRectangle (3.0f, 6.0f, 18.0f, 13.0f, 2.0f);
            path ("M10 3.5h4M3 11.5h18");
            for (auto x : { 7.0f, 10.3f, 13.6f, 16.9f })
                circle (p, x, 8.8f, 0.6f);
            break;
        case PageId::eq:
            path ("M6 4v16M12 4v16M18 4v16");
            icon.fills.addRoundedRectangle (4.0f, 13.0f, 4.0f, 2.6f, 1.0f);
            icon.fills.addRoundedRectangle (10.0f, 7.0f, 4.0f, 2.6f, 1.0f);
            icon.fills.addRoundedRectangle (16.0f, 10.5f, 4.0f, 2.6f, 1.0f);
            break;
        case PageId::cab:
            p.addRoundedRectangle (4.0f, 3.0f, 16.0f, 18.0f, 2.0f);
            circle (p, 12.0f, 12.0f, 5.0f);
            circle (p, 12.0f, 12.0f, 1.4f);
            break;
        case PageId::postFx:
            path ("M3 12c1.5-5 4.5-5 6 0s4.5 5 6 0 4.5-5 6 0");
            break;
        case PageId::output:
            path ("M14 4h5a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1h-5");
            path ("M4 12h11M11 8l4 4-4 4");
            break;
        case PageId::tuner:
        case PageId::toneMatch:
        case PageId::count:
            break;
    }
    return icon;
}

void drawChainIcon (juce::Graphics& g, const ChainIcon& icon, juce::Rectangle<float> box, juce::Colour colour)
{
    const auto scale = box.getWidth() / 24.0f;
    const auto transform = juce::AffineTransform::scale (scale).translated (box.getX(), box.getY());
    const juce::PathStrokeType stroke (1.5f * scale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    g.setColour (colour);
    g.strokePath (icon.strokes, stroke, transform);
    if (! icon.fills.isEmpty())
    {
        g.setColour (bg);
        g.fillPath (icon.fills, transform);
        g.setColour (colour);
        g.strokePath (icon.fills, stroke, transform);
    }
}

/// One block on the chain.
class ChainNav::Node final : public juce::Component
{
public:
    Node (AmpSimProcessor& p, PageId pageId, const char* name, const char* parameterId, bool inverted)
        : page (pageId), label (name), icon (chainIcon (pageId))
    {
        setWantsKeyboardFocus (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle (label);
        if (*parameterId != 0)
        {
            dot = std::make_unique<PowerSwitch> (p.parameters, parameterId, inverted);
            dot->setTooltip (juce::String ("Bypass ") + name + " (click the dot; the block itself opens its page)");
            dot->onChange = [this] { repaint(); };
            dot->onUserToggle = [&p, id = juce::String (parameterId)] (bool on) { p.switchedByUser (id, on); };
            addAndMakeVisible (*dot);
            tagged (*this, parameterId); // a right-click anywhere on the block learns its bypass
        }
    }

    bool isFixed() const noexcept { return dot == nullptr; }
    bool isEngaged() const noexcept { return dot == nullptr || dot->isOn(); }
    PowerSwitch* getDot() const noexcept { return dot.get(); }

    void setActive (bool shouldBeActive)
    {
        if (active != shouldBeActive)
        {
            active = shouldBeActive;
            repaint();
        }
    }

    /// The CSS width: padding, icon, gaps, name, dot.
    int getPreferredWidth() const
    {
        const auto nameWidth = (int) std::ceil (textWidth (nameFont(), label));
        return isFixed() ? 6 + 20 + 10 + nameWidth + 6 : 14 + 20 + 10 + nameWidth + 10 + 7 + 14;
    }

    std::function<void (PageId)> onClick;

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto hovered = isMouseOverOrDragging (true);
        g.setColour (bg); // every block covers the connector, the fixed ends too
        g.fillRoundedRectangle (bounds, radiusCard);
        if (! isFixed())
        {
            g.setColour (active ? accent : (hovered ? inkFaint : line2));
            g.drawRoundedRectangle (bounds.reduced (0.5f), radiusCard, 1.0f);
        }

        // Colours: fixed ends faint; blocks ink-dim, ink when hovered or active; the active icon emerald.
        const auto textColour = isFixed() ? inkFaint : (active || hovered ? ink : inkDim);
        const auto iconColour = active ? accent : textColour;
        const auto alpha = isEngaged() ? 1.0f : 0.35f;
        auto x = isFixed() ? 6.0f : 14.0f;
        drawChainIcon (g, icon, juce::Rectangle<float> (x, bounds.getCentreY() - 10.0f, 20.0f, 20.0f), iconColour.withMultipliedAlpha (alpha));
        x += 30.0f;

        const auto f = nameFont();
        const auto w = textWidth (f, label);
        g.setFont (f);
        g.setColour (textColour);
        const auto textBox = juce::Rectangle<float> (x, bounds.getCentreY() - 9.0f, w + 2.0f, 18.0f);
        g.drawText (label, textBox, juce::Justification::centredLeft, false);
        if (! isEngaged())
        {
            // Struck through, in ink-faint (CSS text-decoration: line-through).
            g.setColour (inkFaint);
            g.fillRect (juce::Rectangle<float> (x, bounds.getCentreY() + 0.5f, w, 1.0f));
        }

        if (keyboardFocus)
            drawFocusRing (g, bounds);
    }

    void resized() override
    {
        if (dot != nullptr)
            dot->setBounds (juce::Rectangle<int> (getWidth() - 14 - 7 - 5, getHeight() / 2 - 8, 17, 17));
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && e.eventComponent == this && getLocalBounds().contains (e.getPosition()) && onClick)
            onClick (page);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if ((key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey) && onClick)
        {
            onClick (page);
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
    static juce::FontOptions nameFont() { return geist (Weight::medium, 14.0f); }

    const PageId page;
    const juce::String label;
    const ChainIcon icon;
    std::unique_ptr<PowerSwitch> dot;
    bool active = false, keyboardFocus = false;
};

ChainNav::ChainNav (AmpSimProcessor& p)
{
    struct Spec
    {
        PageId page;
        const char* name;
        const char* parameterId;
        bool inverted;
    };
    const std::array<Spec, order.size()> specs { { { PageId::input, "Input", "", false },
                                                   { PageId::preFx, "Pre FX", "pre_fx_on", false },
                                                   { PageId::amp, "Amp", "amp_bypass", true },
                                                   { PageId::eq, "EQ", "eq_post_on", false },
                                                   { PageId::cab, "Cab", "cab_bypass", true },
                                                   { PageId::postFx, "Post FX", "post_fx_on", false },
                                                   { PageId::output, "Output", "", false } } };
    for (size_t i = 0; i < specs.size(); ++i)
    {
        nodes[i] = std::make_unique<Node> (p, specs[i].page, specs[i].name, specs[i].parameterId, specs[i].inverted);
        nodes[i]->onClick = [this] (PageId page)
        {
            if (onNavigate)
                onNavigate (page);
        };
        addAndMakeVisible (*nodes[i]);
    }
    setActive (PageId::amp);
}

ChainNav::~ChainNav() = default;

void ChainNav::setActive (PageId page)
{
    active = page;
    for (size_t i = 0; i < order.size(); ++i)
        nodes[i]->setActive (order[i] == page && ! nodes[i]->isFixed());
}

juce::Component* ChainNav::getNode (PageId page) const
{
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == page)
            return nodes[i].get();
    return nullptr;
}

PowerSwitch* ChainNav::getDot (PageId page) const
{
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == page)
            return nodes[i]->getDot();
    return nullptr;
}

bool ChainNav::isBypassedShown (PageId page) const
{
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == page)
            return ! nodes[i]->isEngaged();
    return false;
}

void ChainNav::paint (juce::Graphics& g)
{
    // The 1 px line along the top, and the connector behind the blocks (90 px in from each side).
    g.setColour (line1);
    g.fillRect (0, 0, getWidth(), 1);
    g.setColour (line2);
    g.fillRect (juce::Rectangle<float> (90.0f, 36.0f, (float) getWidth() - 180.0f, 1.0f));
}

void ChainNav::resized()
{
    // Centred in the 71 px under the top line; 16 px apart.
    int total = 0;
    for (auto& node : nodes)
        total += node->getPreferredWidth();
    total += 16 * ((int) nodes.size() - 1);
    auto x = (getWidth() - total) / 2;
    for (auto& node : nodes)
    {
        const auto w = node->getPreferredWidth();
        node->setBounds (x, 14, w, 44);
        x += w + 16;
    }
}

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "LookAndFeel.h"

namespace ui
{

using namespace theme;

LookAndFeel::LookAndFeel()
{
    // The V4 scheme covers whatever JUCE draws itself (the standalone's option panes, scrollbars).
    setColourScheme ({ bg, surface, surface, line2, ink, surface, bg, accent, ink });

    const auto set = [this] (int id, juce::Colour c) { setColour (id, c); };
    set (juce::ResizableWindow::backgroundColourId, bg);
    set (juce::DocumentWindow::textColourId, ink);

    set (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    set (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    set (juce::TextButton::textColourOffId, inkDim);
    set (juce::TextButton::textColourOnId, ink);
    set (juce::ToggleButton::textColourId, inkDim);
    set (juce::ToggleButton::tickColourId, accent);
    set (juce::ToggleButton::tickDisabledColourId, inkFaint);

    set (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
    set (juce::ComboBox::buttonColourId, juce::Colours::transparentBlack);
    set (juce::ComboBox::outlineColourId, line2);
    set (juce::ComboBox::textColourId, ink);
    set (juce::ComboBox::arrowColourId, inkFaint);
    set (juce::ComboBox::focusedOutlineColourId, accent);

    set (juce::PopupMenu::backgroundColourId, surface);
    set (juce::PopupMenu::textColourId, ink);
    set (juce::PopupMenu::headerTextColourId, inkFaint);
    set (juce::PopupMenu::highlightedBackgroundColourId, line1);
    set (juce::PopupMenu::highlightedTextColourId, ink);

    set (juce::Label::textColourId, ink);
    set (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    set (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    set (juce::Label::textWhenEditingColourId, ink);
    set (juce::Label::backgroundWhenEditingColourId, bg);
    set (juce::Label::outlineWhenEditingColourId, accent);

    set (juce::TextEditor::backgroundColourId, bg);
    set (juce::TextEditor::textColourId, ink);
    set (juce::TextEditor::highlightColourId, accentDim);
    set (juce::TextEditor::highlightedTextColourId, ink);
    set (juce::TextEditor::outlineColourId, line2);
    set (juce::TextEditor::focusedOutlineColourId, accent);
    set (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    set (juce::CaretComponent::caretColourId, accent);

    set (juce::TooltipWindow::backgroundColourId, surface);
    set (juce::TooltipWindow::textColourId, ink);
    set (juce::TooltipWindow::outlineColourId, line2);

    set (juce::ScrollBar::thumbColourId, line2);
    set (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    set (juce::AlertWindow::backgroundColourId, surface);
    set (juce::AlertWindow::textColourId, ink);
    set (juce::AlertWindow::outlineColourId, line2);

    set (juce::Slider::backgroundColourId, line2);
    set (juce::Slider::trackColourId, accent);
    set (juce::Slider::thumbColourId, accent);
    set (juce::Slider::rotarySliderFillColourId, accent);
    set (juce::Slider::rotarySliderOutlineColourId, line2);
    set (juce::Slider::textBoxTextColourId, ink);
    set (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    set (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

juce::Path LookAndFeel::roundedBox (juce::Rectangle<float> r, float radius, bool roundLeft, bool roundRight)
{
    juce::Path p;
    p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius, roundLeft, roundRight, roundLeft, roundRight);
    return p;
}

// ---- Buttons --------------------------------------------------------------------------------------

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    // Outlined, like the top bar's Save (handoff 4.3): a 1 px line-2 border, ink-faint on hover, emerald
    // when the button is toggled on (A or B, Store armed). Pressed: the surface fill.
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto path = roundedBox (bounds, radiusControl, ! button.isConnectedOnLeft(), ! button.isConnectedOnRight());
    if (down)
    {
        g.setColour (surface);
        g.fillPath (path);
    }
    const auto border = button.getToggleState() ? accent : (highlighted ? inkFaint : line2);
    g.setColour (border.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return geist (Weight::regular, 13.0f);
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool highlighted, bool)
{
    g.setFont (getTextButtonFont (button, button.getHeight()));
    const auto colour = button.getToggleState() || highlighted ? ink : inkDim;
    g.setColour (colour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.45f));
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (6, 2), juce::Justification::centred, 1, 0.85f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool, bool)
{
    // A plain ToggleButton (ui::Switch draws itself): the same 28 x 16 pill.
    const auto bounds = button.getLocalBounds().toFloat();
    const auto pill = juce::Rectangle<float> (bounds.getX() + 1.0f, bounds.getCentreY() - 8.0f, 28.0f, 16.0f);
    const bool on = button.getToggleState();
    const auto colour = on ? accent : inkDim;
    g.setColour (colour.withMultipliedAlpha (0.9f));
    g.drawRoundedRectangle (pill.reduced (0.5f), 7.5f, 1.0f);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withPosition (pill.getX() + (on ? 16.0f : 4.0f), pill.getY() + 4.0f));
    if (button.getButtonText().isNotEmpty())
    {
        g.setColour (inkDim);
        g.setFont (geist (Weight::medium, 12.0f));
        g.drawText (button.getButtonText(), bounds.withTrimmedLeft (pill.getRight() + 8.0f), juce::Justification::centredLeft, false);
    }
}

// ---- Combo boxes and menus -----------------------------------------------------------------------

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const auto hovered = box.isMouseOver (true);
    g.setColour (box.hasKeyboardFocus (true) ? accent : (hovered ? inkFaint : line2));
    g.drawRoundedRectangle (bounds, radiusControl, 1.0f);

    const auto cx = (float) width - 14.0f, cy = (float) height * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour ((hovered ? inkDim : inkFaint).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return geist (Weight::regular, box.getHeight() < 26 ? 12.0f : 13.0f);
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 1, box.getWidth() - 28, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (0.8f);
}

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.fillAll (surface);
    g.setColour (line2);
    g.drawRect (bounds, 1.0f);
}

juce::Font LookAndFeel::getPopupMenuFont()
{
    return font (Text::body);
}

void LookAndFeel::getIdealPopupMenuItemSize (const juce::String& itemText, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                             int& idealHeight)
{
    if (isSeparator)
    {
        idealWidth = 50;
        idealHeight = 9;
        return;
    }

    const auto f = getPopupMenuFont();
    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : 28;
    idealWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, itemText)) + idealHeight * 2 + 12;
}

void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                                     bool isTicked, bool hasSubMenu, const juce::String& itemText, const juce::String& shortcutKeyText,
                                     const juce::Drawable*, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (outline);
        g.fillRect (area.reduced (8, 0).withSizeKeepingCentre (area.getWidth() - 16, 1));
        return;
    }

    auto r = area.reduced (4, 1);
    if (isHighlighted && isActive)
    {
        g.setColour (line1);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
    }

    const auto colour = textColour != nullptr ? *textColour : (isTicked ? accent : text);
    g.setColour (colour.withMultipliedAlpha (isActive ? 1.0f : 0.4f));
    g.setFont (getPopupMenuFont());

    auto tickArea = r.removeFromLeft (r.getHeight());
    if (isTicked)
    {
        // A check mark in the accent: the ticked item.
        const auto c = tickArea.toFloat().getCentre();
        juce::Path tick;
        tick.startNewSubPath (c.x - 5.0f, c.y);
        tick.lineTo (c.x - 1.5f, c.y + 3.5f);
        tick.lineTo (c.x + 5.0f, c.y - 4.0f);
        g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (hasSubMenu)
    {
        const auto arrow = r.removeFromRight (r.getHeight()).toFloat().getCentre();
        juce::Path chevron;
        chevron.startNewSubPath (arrow.x - 2.0f, arrow.y - 4.0f);
        chevron.lineTo (arrow.x + 2.0f, arrow.y);
        chevron.lineTo (arrow.x - 2.0f, arrow.y + 4.0f);
        g.setColour (inkFaint);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour.withMultipliedAlpha (isActive ? 1.0f : 0.4f));
    }

    g.drawFittedText (itemText, r.reduced (2, 0), juce::Justification::centredLeft, 1, 0.9f);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (inkFaint);
        g.setFont (font (Text::label));
        g.drawText (shortcutKeyText, r.reduced (6, 0), juce::Justification::centredRight, true);
    }
}

void LookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setFont (geist (Weight::medium, 12.0f));
    g.setColour (inkFaint);
    g.drawFittedText (sectionName, area.reduced (12, 0).withTrimmedTop (4), juce::Justification::centredLeft, 1, 0.8f);
}

// ---- Text ----------------------------------------------------------------------------------------

juce::Font LookAndFeel::getLabelFont (juce::Label& label)
{
    return label.getFont();
}

void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height), radiusControl);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.hasKeyboardFocus (true) ? accent : line2);
    g.drawRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f), radiusControl, 1.0f);
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto f = font (Text::label);
    const auto width = juce::jmin (420, juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, tipText)) + 20);
    const auto lines = juce::jmax (1, juce::roundToInt (std::ceil (juce::GlyphArrangement::getStringWidth (f, tipText) / 400.0f)));
    const auto height = 10 + lines * 16;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (width + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (height + 6) : screenPos.y + 6, width, height)
        .constrainedWithin (parentArea);
}

void LookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& tipText, int width, int height)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (surface);
    g.fillRoundedRectangle (bounds, radiusControl);
    g.setColour (line2);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radiusControl, 1.0f);
    g.setColour (ink);
    g.setFont (font (Text::label));
    g.drawFittedText (tipText, bounds.toNearestInt().reduced (10, 4), juce::Justification::centredLeft, 6, 0.9f);
}

std::unique_ptr<juce::FocusOutline> LookAndFeel::createFocusOutlineForComponent (juce::Component&)
{
    // JUCE makes the outline just after the focus changes (asynchronously), while a click that caused it
    // still holds the button down: then it's a mouse focus, which gets no ring.
    if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
        return nullptr;

    struct Ring final : public juce::FocusOutline::OutlineWindowProperties
    {
        juce::Rectangle<int> getOutlineBounds (juce::Component& c) override
        {
            const auto scale = juce::Component::getApproximateScaleFactorForComponent (&c);
            return c.getScreenBounds().expanded (juce::roundToInt (4.0f * scale));
        }

        void drawOutline (juce::Graphics& g, int width, int height) override
        {
            // The window is the control grown by 4 (in its own scale): the ring sits 3 outside the control.
            g.setColour (accent);
            g.drawRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f), 4.0f, 1.0f);
        }
    };
    return std::make_unique<juce::FocusOutline> (std::make_unique<Ring>());
}

juce::Font LookAndFeel::getAlertWindowTitleFont() { return font (Text::title); }
juce::Font LookAndFeel::getAlertWindowMessageFont() { return font (Text::body); }
juce::Font LookAndFeel::getAlertWindowFont() { return font (Text::body); }

void showMenu (juce::PopupMenu menu, juce::Component* target, juce::LookAndFeel* lookAndFeel, bool atMouse)
{
    // Anchored to a component, the menu also takes its UI scale (an unanchored menu would ignore it).
    menu.setLookAndFeel (lookAndFeel);
    auto options = juce::PopupMenu::Options().withStandardItemHeight (28);
    if (target != nullptr)
        options = options.withTargetComponent (target);
    if (atMouse || target == nullptr)
        options = options.withTargetScreenArea (juce::Rectangle<int> (juce::Desktop::getMousePosition(), juce::Point<int> (1, 1) + juce::Desktop::getMousePosition()));
    menu.showMenuAsync (options);
}

} // namespace ui

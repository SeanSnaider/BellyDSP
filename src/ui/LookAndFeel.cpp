#include "LookAndFeel.h"

namespace ui
{

using namespace theme;

LookAndFeel::LookAndFeel()
{
    // The V4 scheme covers whatever JUCE draws itself (the standalone's option panes, scrollbars).
    setColourScheme ({ background, surface, surface, outline, text, surfaceRaised, onAccent, accent, text });

    const auto set = [this] (int id, juce::Colour c) { setColour (id, c); };
    set (juce::ResizableWindow::backgroundColourId, background);
    set (juce::DocumentWindow::textColourId, text);

    set (juce::TextButton::buttonColourId, surfaceRaised);
    set (juce::TextButton::buttonOnColourId, accent);
    set (juce::TextButton::textColourOffId, text);
    set (juce::TextButton::textColourOnId, onAccent);
    set (juce::ToggleButton::textColourId, text);
    set (juce::ToggleButton::tickColourId, accent);
    set (juce::ToggleButton::tickDisabledColourId, textDim);

    set (juce::ComboBox::backgroundColourId, surfaceRaised);
    set (juce::ComboBox::buttonColourId, surfaceRaised);
    set (juce::ComboBox::outlineColourId, outline);
    set (juce::ComboBox::textColourId, text);
    set (juce::ComboBox::arrowColourId, textDim);
    set (juce::ComboBox::focusedOutlineColourId, accent);

    set (juce::PopupMenu::backgroundColourId, surface);
    set (juce::PopupMenu::textColourId, text);
    set (juce::PopupMenu::headerTextColourId, textDim);
    set (juce::PopupMenu::highlightedBackgroundColourId, surfaceRaised);
    set (juce::PopupMenu::highlightedTextColourId, text);

    set (juce::Label::textColourId, text);
    set (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    set (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    set (juce::Label::textWhenEditingColourId, text);
    set (juce::Label::backgroundWhenEditingColourId, background);
    set (juce::Label::outlineWhenEditingColourId, accent);

    set (juce::TextEditor::backgroundColourId, background);
    set (juce::TextEditor::textColourId, text);
    set (juce::TextEditor::highlightColourId, accentSoft);
    set (juce::TextEditor::highlightedTextColourId, text);
    set (juce::TextEditor::outlineColourId, outline);
    set (juce::TextEditor::focusedOutlineColourId, accent);
    set (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    set (juce::CaretComponent::caretColourId, accent);

    set (juce::TooltipWindow::backgroundColourId, surfaceRaised);
    set (juce::TooltipWindow::textColourId, text);
    set (juce::TooltipWindow::outlineColourId, outline);

    set (juce::ScrollBar::thumbColourId, outline.brighter (0.3f));
    set (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    set (juce::AlertWindow::backgroundColourId, surface);
    set (juce::AlertWindow::textColourId, text);
    set (juce::AlertWindow::outlineColourId, outline);

    set (juce::Slider::backgroundColourId, outline);
    set (juce::Slider::trackColourId, accent);
    set (juce::Slider::thumbColourId, accent);
    set (juce::Slider::rotarySliderFillColourId, accent);
    set (juce::Slider::rotarySliderOutlineColourId, outline);
    set (juce::Slider::textBoxTextColourId, text);
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

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour, bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = backgroundColour;
    if (down)
        fill = fill.darker (0.18f);
    else if (highlighted)
        fill = fill.brighter (0.07f);
    if (! button.isEnabled())
        fill = fill.withMultipliedAlpha (0.4f);

    const auto path = roundedBox (bounds, radiusControl, ! button.isConnectedOnLeft(), ! button.isConnectedOnRight());
    g.setColour (fill);
    g.fillPath (path);

    if (! button.getToggleState())
    {
        g.setColour ((highlighted ? outline.brighter (0.25f) : outline).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.strokePath (path, juce::PathStrokeType (1.0f));
    }
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (buttonHeight < 24 ? Text::label : Text::body);
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    g.setFont (getTextButtonFont (button, button.getHeight()));
    const auto colour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    g.setColour (colour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.45f));
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (6, 2), juce::Justification::centred, 1, 0.85f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // The pill switch (UI_DESIGN "Switch"): accent when on, a hollow outline when off, label to the right.
    const auto bounds = button.getLocalBounds().toFloat();
    const auto h = juce::jmin (16.0f, bounds.getHeight() - 4.0f);
    const auto pill = juce::Rectangle<float> (bounds.getX() + 1.0f, bounds.getCentreY() - h * 0.5f, h * 1.8f, h);
    const auto alpha = button.isEnabled() ? 1.0f : 0.4f;
    const bool on = button.getToggleState();
    const auto thumb = juce::Rectangle<float> (h - 4.0f, h - 4.0f).withCentre ({ on ? pill.getRight() - h * 0.5f : pill.getX() + h * 0.5f, pill.getCentreY() });

    if (on)
    {
        g.setColour ((highlighted ? accent.brighter (0.15f) : accent).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (pill, h * 0.5f);
        g.setColour (text.withMultipliedAlpha (alpha));
        g.fillEllipse (thumb);
    }
    else
    {
        g.setColour ((highlighted ? textDim : outline.brighter (0.35f)).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (pill.reduced (0.75f), h * 0.5f, 1.5f);
        g.setColour (textDim.withMultipliedAlpha (alpha));
        g.fillEllipse (thumb.reduced (1.0f));
    }

    if (button.getButtonText().isNotEmpty())
    {
        g.setColour (text.withMultipliedAlpha (alpha));
        g.setFont (font (Text::body));
        g.drawFittedText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft (juce::roundToInt (pill.getRight()) + 8),
                          juce::Justification::centredLeft, 1, 0.8f);
    }
}

// ---- Combo boxes and menus -----------------------------------------------------------------------

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const auto hovered = box.isMouseOver (true);
    g.setColour (hovered ? surfaceRaised.brighter (0.06f) : surfaceRaised);
    g.fillRoundedRectangle (bounds, radiusControl);
    g.setColour (box.hasKeyboardFocus (true) ? accent : (hovered ? outline.brighter (0.25f) : outline));
    g.drawRoundedRectangle (bounds, radiusControl, 1.0f);

    // The chevron.
    const auto cx = (float) width - 14.0f, cy = (float) height * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (textDim.withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return font (box.getHeight() < 26 ? Text::label : Text::body);
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
    g.setColour (outline);
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
        g.setColour (surfaceRaised);
        g.fillRoundedRectangle (r.toFloat(), radiusControl);
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
        g.setColour (textDim);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour.withMultipliedAlpha (isActive ? 1.0f : 0.4f));
    }

    g.drawFittedText (itemText, r.reduced (2, 0), juce::Justification::centredLeft, 1, 0.9f);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (textDim);
        g.setFont (font (Text::label));
        g.drawText (shortcutKeyText, r.reduced (6, 0), juce::Justification::centredRight, true);
    }
}

void LookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    g.setFont (font ("Semibold", 11.0f));
    g.setColour (textDim);
    g.drawFittedText (sectionName.toUpperCase(), area.reduced (12, 0).withTrimmedTop (4), juce::Justification::centredLeft, 1, 0.8f);
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
    g.setColour (editor.hasKeyboardFocus (true) ? accent : outline);
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
    g.setColour (surfaceRaised);
    g.fillRoundedRectangle (bounds, radiusControl);
    g.setColour (outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radiusControl, 1.0f);
    g.setColour (text);
    g.setFont (font (Text::label));
    g.drawFittedText (tipText, bounds.toNearestInt().reduced (10, 4), juce::Justification::centredLeft, 6, 0.9f);
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

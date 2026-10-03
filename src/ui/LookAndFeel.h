// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Theme.h"

namespace ui
{

/// The GUI's one LookAndFeel, in the handoff's chrome: hairline borders on the background, emerald for
/// "on" and "selected" only, the toggle pill, outlined buttons like the top bar's Save, fields with a
/// chevron, and menus on the surface colour. Everything reads its colours and type from theme::. The
/// editor sets it on itself (its children inherit it) and on every menu it opens.
class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    // Buttons. A TextButton is a raised field (accent when toggled on); ToggleButton is the pill switch.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    // Combo boxes and popup menus.
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                    int& idealHeight) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override { return 4; }

    // Text, tooltips, and the scene-rename box.
    juce::Font getLabelFont (juce::Label&) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

    /// The keyboard focus ring for JUCE's own controls (buttons, combo boxes): 1 px emerald, 3 px outside
    /// the control (handoff 5). None when a mouse click gave the focus (the CSS :focus-visible rule).
    std::unique_ptr<juce::FocusOutline> createFocusOutlineForComponent (juce::Component&) override;

    /// A rounded rectangle with each corner rounded or square, for segmented buttons (A | B).
    static juce::Path roundedBox (juce::Rectangle<float> r, float radius, bool roundLeft, bool roundRight);
};

/// Shows a menu in the GUI's style next to `target` (or at the mouse, scaled like `target`).
void showMenu (juce::PopupMenu menu, juce::Component* target, juce::LookAndFeel* lookAndFeel, bool atMouse = false);

} // namespace ui

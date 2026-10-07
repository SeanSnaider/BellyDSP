// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace ui
{

/// Every control attached to a parameter carries its ID in this property, so a right-click anywhere on
/// it can offer MIDI learn for it (AmpSimEditor::mouseDown walks up to the nearest tagged component).
inline const juce::Identifier parameterIdProperty { "parameterId" };

/// Marks a control with the parameter it's attached to. Returns the control, so it can wrap an
/// attachment's argument.
template <typename Control>
Control& tagged (Control& control, const juce::String& parameterId)
{
    control.getProperties().set (parameterIdProperty, parameterId);
    return control;
}

/// Starts a new undo step for a change a mouse press makes right away (a click that sets a value): the
/// parameter tree first catches up with values still waiting to be copied into it (it does that on a
/// timer), so the previous gesture's last values stay in their own step. Then the editor's own press
/// handler, which runs after the component's, files this change in the step begun here.
inline void beginUndoStep (juce::AudioProcessorValueTreeState& state)
{
    state.copyState();
    if (state.undoManager != nullptr)
        state.undoManager->beginNewTransaction();
}

/// Sets a parameter (plain value) as one complete gesture in its own undo step.
inline void setAsGesture (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, float plainValue)
{
    if (auto* parameter = state.getParameter (parameterId))
    {
        beginUndoStep (state);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
        parameter->endChangeGesture();
    }
}

/// A combo box must have its items before a parameter attachment is made for it.
inline juce::ComboBox& withItems (juce::ComboBox& box, const juce::StringArray& items)
{
    box.addItemList (items, 1);
    return box;
}

/// A control that leaves right-clicks (and ctrl-clicks) to the editor's MIDI learn menu. Without this
/// a JUCE button flips on any click, and a linear slider jumps to where it was clicked.
template <typename Control>
class IgnoresRightClick : public Control
{
public:
    using Control::Control;

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Control::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Control::mouseDrag (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Control::mouseUp (e);
    }
};

/// The keyboard focus ring (handoff 5): a 1px emerald outline 3px outside `box`, radius 4. Only drawn when
/// the focus came from the keyboard (the CSS :focus-visible rule): a click focuses a control (so its arrow
/// keys work) without a ring.
void drawFocusRing (juce::Graphics& g, juce::Rectangle<float> box);

/// The base of every drawn control bound to one parameter: the knob, the value field, and the fader. It
/// owns the parameter attachment and the gestures (handoff 4.1, "Interaction"):
///   drag            up raises the value; 200 points of travel cover the whole range (fields: right too)
///   shift-drag      four times finer (800 points)
///   scroll wheel    1/50 of the range per notch (a choice: one step)
///   arrow keys      1/50 of the range per press, when focused (a choice: one step)
///   double-click    a knob: back to the default; a field: type a value ("2.5k" for 2500, a choice by name)
///   alt-click       back to the default (kept from the old GUI)
///   right-click     ignored here: the editor's MIDI learn and scenes menu
/// Drags move through the parameter's normalised range, so a skewed knob (a frequency) moves evenly in its
/// skewed space. Every gesture is a parameter gesture, so the host and the undo manager see one change per
/// drag (and per key press).
class ParameterControl : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    ParameterControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, juce::String suffix);
    ~ParameterControl() override;

    juce::RangedAudioParameter& getParameter() const noexcept { return parameter; }
    float getValue() const noexcept { return value; } ///< plain value
    float getNormalisedValue() const noexcept { return parameter.convertTo0to1 (value); }

    /// Where a field's bar starts: the position of 0 for a range that spans it (bipolar), else the start.
    float getZeroPoint() const noexcept;

    /// The value as shown: the formatter's text if there is one, else the parameter's own text plus the
    /// unit, with kHz and seconds for big values.
    juce::String getValueText() const;
    void setFormatter (std::function<juce::String (float)> f)
    {
        formatter = std::move (f);
        repaint();
    }

    /// The typing box (fields: a double-click opens it; public so tests can reach it).
    void showTextEntry();
    bool isEditingText() const noexcept { return textEntry != nullptr; }
    /// Sets the value from typed text as one gesture; returns false if the text isn't a value.
    bool setFromText (const juce::String& text);

    /// Back to the parameter's default, as one gesture.
    void resetToDefault();

    bool isDragging() const noexcept { return dragging; }
    bool isHovered() const noexcept { return hovered; }

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;

    /// One wheel notch, or one arrow key press, as a fraction of the range (handoff 4.1).
    static constexpr float stepFraction = 1.0f / 50.0f;

protected:
    /// Where the typing box goes.
    virtual juce::Rectangle<int> getTextEntryBounds() const { return getLocalBounds(); }
    virtual void valueChanged() { repaint(); }

    float pixelsForFullRange = 200.0f;
    float fineFactor = 0.25f;        // shift: 800 points for the range
    bool horizontalDrag = false;     // fields: right raises the value too
    bool doubleClickTypes = false;   // fields: a double-click types a value; knobs reset
    bool hovered = false, keyboardFocus = false;

private:
    void timerCallback() override; // ends a run of wheel moves as one gesture
    void hideTextEntry (bool apply);
    void setNormalised (float normalised, bool partOfGesture);
    float stepSize() const;
    void nudge (float steps);

    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter& parameter;
    const juce::String suffix;
    std::function<juce::String (float)> formatter;
    juce::ParameterAttachment attachment;
    float value = 0.0f;

    bool dragging = false, wheeling = false;
    float dragNormalised = 0.0f;
    juce::Point<float> lastDragPosition;
    std::unique_ptr<juce::TextEditor> textEntry;
};

/// The knob (handoff 4.1): one widget in four skins and two sizes. Its CSS box is 76 x 86 (the 64 x 64
/// hit area over a 6 px gap and a 16 px label; small: 68 x 74, a 52 x 52 hit area); the component is that
/// box grown by 4 px on every side, so the value arc (which overhangs the hit area by a pixel) and the
/// focus ring (3 px outside the box) are drawn inside it. Geometry, exactly as the reference's SVG and CSS:
///   track     an arc of radius 32 in a 72 px box centred on the hit area, -135 to +135 degrees (0 = up),
///             stroke 2, round caps, the skin's track colour (small: the same drawing at 60/72 scale)
///   value     the same arc from -135 to the value, emerald, hidden at the minimum
///   body      a 40 px circle (small 32), a radial gradient from the skin's a (highlight at 38% / 30%) to b
///             at 80%, a 1 px ring, and a soft shadow (0 3 6, black at 35%)
///   pointer   a 3 x 10 bar, 4 px in from the body's edge, rotating with the value
///   label     12 px, weight 500, the skin's label colour; while dragging (and on hover) the value instead,
///             emerald in the chrome, underlined in emerald on the amp panels
class Knob final : public ParameterControl
{
public:
    enum class Size
    {
        normal, // 64
        compact // 52 (the handoff's "sm")
    };

    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, juce::String caption, juce::String suffix = " dB",
          Size size = Size::normal, theme::Skin skin = theme::Skin::chrome);

    void setCaption (const juce::String& newCaption);
    const juce::String& getCaption() const noexcept { return caption; }
    void setKnobSize (Size newSize);
    Size getKnobSize() const noexcept { return size; }
    void setSkin (theme::Skin newSkin);
    theme::Skin getSkin() const noexcept { return skin; }

    /// The amp panels underline the value while dragging; the chrome turns it emerald (handoff 4.1).
    bool isPanelStyle() const noexcept { return skin != theme::Skin::chrome; }

    /// What the label shows now: the caption, or the value while dragging or hovered.
    juce::String getShownLabel() const;

    void paint (juce::Graphics&) override;

    /// The CSS box (76 x 86 or 68 x 74) and the component's bounds for a box at a position.
    static juce::Point<int> cssSize (Size size) { return size == Size::normal ? juce::Point<int> { 76, 86 } : juce::Point<int> { 68, 74 }; }
    static constexpr int margin = 4;
    static int preferredWidth (Size size) { return cssSize (size).x + 2 * margin; }
    static int preferredHeight (Size size) { return cssSize (size).y + 2 * margin; }
    void setCssPosition (int x, int y) { setBounds (x - margin, y - margin, preferredWidth (size), preferredHeight (size)); }

private:
    juce::Rectangle<int> getTextEntryBounds() const override;
    void valueChanged() override;

    juce::String caption;
    Size size;
    theme::Skin skin;
};

/// A value in a field with a bar behind it, for tables (the voices of the harmonizer and multivoicer)
/// and small settings: drag sideways or up and down, the same gestures as a knob; a double-click types.
class ValueField final : public ParameterControl
{
public:
    ValueField (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, juce::String suffix);
    void paint (juce::Graphics&) override;

    /// Whether the bar behind the value is drawn (off for numbers that aren't a position on a range,
    /// such as the tempo or a controller number).
    void setShowsBar (bool shouldShow)
    {
        showsBar = shouldShow;
        repaint();
    }

private:
    bool showsBar = true;
};

/// A vertical fader with its caption above and value below: the graphic EQ's nine sliders.
class Fader final : public ParameterControl
{
public:
    Fader (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, juce::String caption, juce::String suffix);
    void paint (juce::Graphics&) override;

private:
    juce::Rectangle<int> getTextEntryBounds() const override;
    juce::Rectangle<float> trackArea() const;

    const juce::String caption;
};

/// The toggle (handoff 4.2): a 28 x 16 pill with a 1 px border in the text colour and an 8 px dot, the
/// label 12 px to its right after an 8 px gap. Off: the dot on the left, muted. On: border and dot emerald,
/// the dot on the right. Click, Space, or Enter flips it. Attach it with a ButtonAttachment. The component
/// is the CSS box grown by 4 px each way (the focus ring).
class Switch : public IgnoresRightClick<juce::ToggleButton>
{
public:
    explicit Switch (const juce::String& text = {});

    /// The label's colour (the amp panels use their own); default ink-dim.
    void setTextColour (juce::Colour c)
    {
        textColour = c;
        repaint();
    }

    int getPreferredWidth() const;
    static constexpr int margin = 4;
    static constexpr int preferredHeight = 16 + 2 * margin;

    /// Called with the new state when the user flips it (a click, Space, or Enter on it), after the
    /// toggle and before the attached parameter hears of it. Not called when the parameter moves it (a
    /// preset, a scene, MIDI, undo, the host): onClick can't tell those apart, since an attachment's
    /// update sends a click too.
    std::function<void (bool)> onUserToggle;

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;

private:
    void clicked() override;

    juce::Colour textColour = theme::inkDim;
    bool keyboardFocus = false;
    bool userGesture = false; // set by a mouse-up or key on the switch, consumed by the click it causes
};

/// A bypass dot (handoff 4.4): 7 px, emerald when the block is engaged, line-2 when it's bypassed. Bound
/// to a block's switch, or inverted to a bypass parameter (the amp's, the cab's). A click flips it as one
/// gesture without anything else happening (the chain block around it doesn't navigate).
class PowerSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PowerSwitch (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, bool inverted);
    bool isOn() const noexcept { return on; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    /// Flips it (a click, Space, or Enter), as one gesture in its own undo step.
    void toggle();

    std::function<void()> onChange;
    /// Called after the user flips it (toggle()), with the parameter's new value (true: 1). Never when
    /// the parameter moves it.
    std::function<void (bool)> onUserToggle;

private:
    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter& parameter;
    const bool inverted;
    juce::ParameterAttachment attachment;
    bool on = false, keyboardFocus = false;
};

/// A small square button with a drawn icon (copy, the preset arrows), styled like a TextButton.
class IconButton final : public juce::Button
{
public:
    enum class Icon
    {
        undo,
        redo,
        copy,
        settings,
        midi,
        left,
        right
    };

    IconButton (const juce::String& name, Icon icon);
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    const Icon icon;
};

/// A segmented choice (the reference's .seg, the tuner's tunings): text options 16 px apart in 12 px
/// ink-faint, the chosen one in ink with a 1 px emerald underline 4 px below. Hover lightens an option to
/// ink-dim. A click, or the arrow keys when focused, chooses.
class Segmented final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Segmented (juce::StringArray options);

    std::function<void (int)> onChange;

    void setSelected (int index, juce::NotificationType notification = juce::dontSendNotification);
    int getSelected() const noexcept { return selected; }
    int getNumOptions() const noexcept { return options.size(); }
    int getPreferredWidth() const;
    static constexpr int preferredHeight = 21;

    /// For tests: an option's area.
    juce::Rectangle<float> optionArea (int index) const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;

private:
    int optionAt (juce::Point<float> p) const;
    juce::StringArray options;
    int selected = 0, hovered = -1;
    bool keyboardFocus = false;
};

/// A section title inside a page: a 12 px heading in ink-faint and a hairline, over a group of controls.
void drawGroupHeading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

/// A card: a 1 px border in `line`, radius 10, no fill (the chrome has no raised panels).
void drawCard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill = juce::Colours::transparentBlack);

} // namespace ui

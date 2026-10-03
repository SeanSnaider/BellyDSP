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

/// The base of every drawn control bound to one parameter: the knob, the value field, and the fader.
/// It owns the parameter attachment and the gestures (UI_DESIGN "Knob"):
///   drag            up or right raises the value; the whole range is about 200 points of travel
///   shift-drag      ten times finer
///   scroll wheel    a step per notch (shift: finer)
///   double-click    type a value ("2.5k" for 2500, a choice by name)
///   alt-click       back to the default
///   right-click     ignored here: the editor's MIDI learn and scenes menu
/// Drags move through the parameter's normalised range, so a skewed knob (a frequency) moves evenly in
/// its skewed space, the way JUCE's sliders do. Every gesture is a parameter gesture, so the host and the
/// undo manager see one change per drag.
class ParameterControl : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    ParameterControl (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, juce::String suffix);
    ~ParameterControl() override;

    juce::RangedAudioParameter& getParameter() const noexcept { return parameter; }
    float getValue() const noexcept { return value; } ///< plain value
    float getNormalisedValue() const noexcept { return parameter.convertTo0to1 (value); }

    /// Where an arc or a bar starts: the position of 0 for a range that spans it (bipolar), else the start.
    float getZeroPoint() const noexcept;

    /// The value as shown: the parameter's own text plus the unit, with kHz and seconds for big values.
    juce::String getValueText() const;
    void setFormatter (std::function<juce::String (float)> f)
    {
        formatter = std::move (f);
        repaint();
    }

    /// The typing box a double-click opens (public so tests can reach it).
    void showTextEntry();
    bool isEditingText() const noexcept { return textEntry != nullptr; }
    /// Sets the value from typed text as one gesture; returns false if the text isn't a value.
    bool setFromText (const juce::String& text);

    bool isDragging() const noexcept { return dragging; }

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

protected:
    /// Where the typing box goes.
    virtual juce::Rectangle<int> getTextEntryBounds() const = 0;
    virtual void valueChanged() { repaint(); }

    float pixelsForFullRange = 200.0f;
    bool hovered = false;

private:
    void timerCallback() override; // ends a run of wheel moves as one gesture
    void hideTextEntry (bool apply);
    void setNormalised (float normalised, bool partOfGesture);
    void beginUndoStep();
    float stepSize() const;

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

/// A rotary knob (UI_DESIGN "Knob"): the caption above, a 270-degree track with the value arc in the
/// accent from the parameter's zero point, a raised face with a pointer, and the value below.
class Knob final : public ParameterControl
{
public:
    enum class Size
    {
        normal,  // 56
        compact  // 40
    };

    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, juce::String caption, juce::String suffix = " dB",
          Size size = Size::normal);

    void setCaption (const juce::String& newCaption);
    void setKnobSize (Size newSize);
    Size getKnobSize() const noexcept { return size; }
    void paint (juce::Graphics&) override;

    static int preferredWidth (Size size) { return size == Size::normal ? 72 : 62; }
    static int preferredHeight (Size size)
    {
        return (size == Size::normal ? theme::knobNormal : theme::knobCompact) + 2 * theme::captionHeight + 6;
    }

private:
    juce::Rectangle<int> getTextEntryBounds() const override;
    juce::Rectangle<float> dialArea() const;

    juce::String caption;
    Size size;
};

/// A value in a field with a bar behind it, for tables (the voices of the harmonizer and multivoicer)
/// and the top bar's tempo: drag sideways or up and down, the same gestures as a knob.
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
    juce::Rectangle<int> getTextEntryBounds() const override { return getLocalBounds(); }
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

/// The pill switch (UI_DESIGN "Switch"), drawn by the LookAndFeel; attach it with a ButtonAttachment.
class Switch final : public IgnoresRightClick<juce::ToggleButton>
{
public:
    explicit Switch (const juce::String& text = {}) { setButtonText (text); }
    int getPreferredWidth() const;
};

/// A chain block's power switch: a small pill bound to a block's on switch, or, inverted, to the cab's
/// bypass. A click flips it as one gesture.
class PowerSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PowerSwitch (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, bool inverted);
    bool isOn() const noexcept { return on; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    std::function<void()> onChange;

private:
    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter& parameter;
    const bool inverted;
    juce::ParameterAttachment attachment;
    bool on = false;
};

/// A small square button with a drawn icon (undo, redo, copy, settings), styled like a TextButton.
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

/// A section title inside a page: a small uppercase heading and a hairline, over a group of controls.
void drawGroupHeading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

/// A card: a raised panel with the outline border.
void drawCard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill = theme::surfaceRaised);

} // namespace ui

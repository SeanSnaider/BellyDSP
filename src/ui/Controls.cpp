// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Controls.h"
#include "LookAndFeel.h"

namespace ui
{

using namespace theme;

namespace
{
juce::RangedAudioParameter& lookUp (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId)
{
    auto* parameter = state.getParameter (parameterId);
    jassert (parameter != nullptr); // every control is built for an existing, permanent parameter ID
    return *parameter;
}

/// The knob's 270-degree travel, clockwise from twelve o'clock (JUCE's arc convention, and the
/// reference's: -135 to +135 degrees with 0 straight up).
constexpr float arcStart = -0.75f * juce::MathConstants<float>::pi;
constexpr float arcEnd = 0.75f * juce::MathConstants<float>::pi;

bool isArrow (const juce::KeyPress& key, int& direction)
{
    if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::rightKey)
        direction = 1;
    else if (key.getKeyCode() == juce::KeyPress::downKey || key.getKeyCode() == juce::KeyPress::leftKey)
        direction = -1;
    else
        return false;
    return key.getModifiers().withoutMouseButtons() == juce::ModifierKeys() || key.getModifiers().isShiftDown();
}
} // namespace

void drawFocusRing (juce::Graphics& g, juce::Rectangle<float> box)
{
    g.setColour (accent);
    g.drawRoundedRectangle (box.expanded (3.5f), 4.0f, 1.0f);
}

// ---- ParameterControl ----------------------------------------------------------------------------

ParameterControl::ParameterControl (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String unit)
    : state (s),
      parameter (lookUp (s, parameterId)),
      suffix (std::move (unit)),
      attachment (parameter, [this] (float v)
                  {
                      value = v;
                      valueChanged();
                  },
                  s.undoManager)
{
    tagged (*this, parameterId);
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    attachment.sendInitialUpdate();
}

ParameterControl::~ParameterControl()
{
    stopTimer();
    if (dragging || wheeling)
        attachment.endGesture();
}

float ParameterControl::getZeroPoint() const noexcept
{
    const auto& range = parameter.getNormalisableRange();
    return range.start < 0.0f && range.end > 0.0f ? range.convertTo0to1 (0.0f) : 0.0f;
}

juce::String ParameterControl::getValueText() const
{
    if (formatter)
        return formatter (value);

    const auto magnitude = std::abs (value);
    if (suffix == " Hz" && magnitude >= 1000.0f)
        return juce::String (value / 1000.0f, magnitude >= 10000.0f ? 1 : 2) + " kHz";
    if (suffix == " ms" && magnitude >= 1000.0f)
        return juce::String (value / 1000.0f, 2) + " s";
    if (suffix == " dB")
    {
        // Signed with one decimal (handoff 4.1, "Value formats"): +2.0 dB, -6.5 dB, 0.0 dB.
        const auto rounded = std::round (value * 10.0f) / 10.0f;
        return (rounded > 0.0f ? "+" : "") + juce::String (rounded == 0.0f ? 0.0f : rounded, 1) + " dB";
    }
    return parameter.getText (parameter.convertTo0to1 (value), 0) + suffix;
}

float ParameterControl::stepSize() const
{
    // Choices and small integer ranges move a whole step at a time; continuous ranges by a fraction.
    const auto steps = parameter.getNumSteps();
    return steps > 1 && steps <= 128 ? 1.0f / (float) (steps - 1) : 0.0f;
}

void ParameterControl::setNormalised (float normalised, bool partOfGesture)
{
    const auto plain = parameter.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised));
    if (partOfGesture)
        attachment.setValueAsPartOfGesture (plain);
    else
        attachment.setValueAsCompleteGesture (plain);
}

void ParameterControl::resetToDefault()
{
    beginUndoStep (state);
    attachment.setValueAsCompleteGesture (parameter.convertFrom0to1 (parameter.getDefaultValue()));
}

void ParameterControl::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! isEnabled())
        return;

    if (isEditingText())
        hideTextEntry (true);

    beginUndoStep (state);

    if (e.mods.isAltDown())
    {
        resetToDefault();
        return;
    }

    dragging = true;
    dragNormalised = getNormalisedValue();
    lastDragPosition = e.position;
    attachment.beginGesture();
    repaint();
}

void ParameterControl::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || e.mods.isPopupMenu())
        return;

    // Up raises the value (fields: right too). Each event moves by its own distance, so pressing or
    // releasing shift mid-drag changes the speed without a jump.
    auto delta = -(e.position.y - lastDragPosition.y);
    if (horizontalDrag)
        delta += e.position.x - lastDragPosition.x;
    lastDragPosition = e.position;
    const auto perPixel = (e.mods.isShiftDown() ? fineFactor : 1.0f) / pixelsForFullRange;
    dragNormalised = juce::jlimit (0.0f, 1.0f, dragNormalised + delta * perPixel);
    setNormalised (dragNormalised, true);
}

void ParameterControl::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    attachment.endGesture();
    repaint();
}

void ParameterControl::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! isEnabled())
        return;
    if (doubleClickTypes)
        showTextEntry();
    else
        resetToDefault();
}

void ParameterControl::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || isEditingText() || dragging)
        return;

    auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    if (wheel.isReversed)
        delta = -delta;
    if (delta == 0.0f)
        return;

    // A wheel's notch moves 1/50 of the range (a choice one step). A trackpad's smooth scroll moves in
    // proportion, a notch's worth for each 0.1 of scroll (about what one notch reports on macOS). A run of
    // wheel moves is one gesture (one undo step), ended 350 ms after the last move.
    const auto step = stepSize();
    float change;
    if (step > 0.0f)
        change = delta > 0.0f ? step : -step;
    else if (wheel.isSmooth)
        change = delta * stepFraction / 0.1f;
    else
        change = delta > 0.0f ? stepFraction : -stepFraction;

    if (! wheeling)
    {
        beginUndoStep (state);
        attachment.beginGesture();
        wheeling = true;
    }
    setNormalised (getNormalisedValue() + change, true);
    startTimer (350);
}

void ParameterControl::nudge (float steps)
{
    const auto step = stepSize() > 0.0f ? stepSize() : stepFraction;
    beginUndoStep (state);
    setNormalised (getNormalisedValue() + steps * step, false);
}

bool ParameterControl::keyPressed (const juce::KeyPress& key)
{
    if (int direction = 0; isArrow (key, direction) && isEnabled() && ! isEditingText())
    {
        nudge ((float) direction);
        return true;
    }
    return false;
}

void ParameterControl::focusGained (FocusChangeType cause)
{
    keyboardFocus = cause != focusChangedByMouseClick;
    repaint();
}

void ParameterControl::focusLost (FocusChangeType)
{
    keyboardFocus = false;
    repaint();
}

void ParameterControl::timerCallback()
{
    stopTimer();
    if (wheeling)
    {
        wheeling = false;
        attachment.endGesture();
    }
}

void ParameterControl::mouseEnter (const juce::MouseEvent&)
{
    hovered = true;
    repaint();
}

void ParameterControl::mouseExit (const juce::MouseEvent&)
{
    hovered = false;
    repaint();
}

bool ParameterControl::setFromText (const juce::String& typed)
{
    const auto entered = typed.trim();
    if (entered.isEmpty())
        return false;

    float plain = 0.0f;
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&parameter))
    {
        // A choice by its name (or a unique start of it).
        auto index = choice->choices.indexOf (entered, true);
        for (int i = 0; index < 0 && i < choice->choices.size(); ++i)
            if (choice->choices[i].startsWithIgnoreCase (entered))
                index = i;
        if (index < 0)
            return false;
        plain = (float) index;
    }
    else
    {
        const auto lower = entered.toLowerCase();
        const auto first = lower[0];
        const bool numeric = juce::CharacterFunctions::isDigit (first)
                             || ((first == '-' || first == '+' || first == '.') && lower.length() > 1 && juce::CharacterFunctions::isDigit (lower[1]));
        if (numeric)
        {
            plain = lower.getFloatValue();
            if (lower.containsChar ('k'))
                plain *= 1000.0f; // "2.5k", "2.5 kHz"
            else if (suffix == " ms" && lower.endsWith ("s") && ! lower.endsWith ("ms"))
                plain *= 1000.0f; // "1.2 s" for a time in milliseconds
        }
        else
        {
            // The parameter's own text: "L 30" for a pan, "On" for a switch.
            plain = parameter.convertFrom0to1 (parameter.getValueForText (entered));
        }
    }

    const auto& range = parameter.getNormalisableRange();
    plain = juce::jlimit (range.start, range.end, plain);
    beginUndoStep (state);
    attachment.setValueAsCompleteGesture (plain);
    return true;
}

void ParameterControl::showTextEntry()
{
    if (textEntry != nullptr)
        return;

    textEntry = std::make_unique<juce::TextEditor>();
    auto& entry = *textEntry;
    entry.setFont (font (Text::value));
    entry.setJustification (juce::Justification::centred);
    entry.setIndents (2, 1);
    entry.setText (getValueText(), false);
    entry.setSelectAllWhenFocused (true);

    // The editor is deleted later, never from inside its own callbacks.
    const auto finish = [safe = juce::Component::SafePointer<ParameterControl> (this)] (bool apply)
    {
        juce::MessageManager::callAsync ([safe, apply]
        {
            if (safe != nullptr)
                safe->hideTextEntry (apply);
        });
    };
    entry.onReturnKey = [finish] { finish (true); };
    entry.onEscapeKey = [finish] { finish (false); };
    entry.onFocusLost = [finish] { finish (true); };

    addAndMakeVisible (entry);
    entry.setBounds (getTextEntryBounds());
    if (isShowing())
        entry.grabKeyboardFocus();
    entry.selectAll();
}

void ParameterControl::hideTextEntry (bool apply)
{
    if (textEntry == nullptr)
        return;

    auto entry = std::move (textEntry);
    entry->onReturnKey = nullptr;
    entry->onEscapeKey = nullptr;
    entry->onFocusLost = nullptr;
    if (apply)
        setFromText (entry->getText());
    removeChildComponent (entry.get());
    repaint();
}

// ---- Knob ----------------------------------------------------------------------------------------

Knob::Knob (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String captionText, juce::String unit, Size knobSize,
            Skin knobSkinChoice)
    : ParameterControl (s, parameterId, std::move (unit)), caption (std::move (captionText)), size (knobSize), skin (knobSkinChoice)
{
    pixelsForFullRange = 200.0f;
    fineFactor = 0.25f;
}

void Knob::setKnobSize (Size newSize)
{
    if (newSize != size)
    {
        size = newSize;
        repaint();
    }
}

void Knob::setSkin (Skin newSkin)
{
    if (newSkin != skin)
    {
        skin = newSkin;
        repaint();
    }
}

void Knob::setCaption (const juce::String& newCaption)
{
    if (caption != newCaption)
    {
        caption = newCaption;
        repaint();
    }
}

void Knob::valueChanged()
{
    setTitle (caption);
    repaint();
}

juce::String Knob::getShownLabel() const
{
    return isDragging() || isHovered() ? getValueText() : caption;
}

juce::Rectangle<int> Knob::getTextEntryBounds() const
{
    return getLocalBounds().removeFromBottom (captionHeight + margin + 2);
}

void Knob::paint (juce::Graphics& g)
{
    const auto colours = knobSkin (skin);
    const auto box = getLocalBounds().toFloat().reduced ((float) margin);
    const bool normal = size == Size::normal;
    const auto hit = normal ? 64.0f : 52.0f;
    const auto wrap = juce::Rectangle<float> (box.getCentreX() - hit * 0.5f, box.getY(), hit, hit);
    const auto centre = wrap.getCentre();

    // The arcs: the reference's 72-unit SVG over the hit area grown by 4 (60 px for the small knob).
    const auto svgScale = normal ? 1.0f : 60.0f / 72.0f;
    const auto radius = 32.0f * svgScale;
    const juce::PathStrokeType stroke (2.0f * svgScale, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    const auto t = getNormalisedValue();
    const auto angle = arcStart + t * (arcEnd - arcStart);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, arcStart, arcEnd, true);
    g.setColour (colours.track);
    g.strokePath (track, stroke);

    if (t >= 0.005f) // hidden at the minimum
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, arcStart, angle, true);
        g.setColour (accent);
        g.strokePath (arc, stroke);
    }

    // The body: a soft shadow, the 1 px ring, the gradient (highlight up and to the left), the pointer.
    const auto bodySize = normal ? 40.0f : 32.0f;
    const auto body = juce::Rectangle<float> (bodySize, bodySize).withCentre (centre);
    {
        juce::Path disc;
        disc.addEllipse (body.expanded (1.0f));
        juce::DropShadow (juce::Colours::black.withAlpha (0.35f), 6, { 0, 3 }).drawForPath (g, disc);
        g.setColour (colours.ring);
        g.fillPath (disc);
    }
    {
        // CSS radial-gradient(circle at 38% 30%, a, b 80%): the farthest-corner circle from that point,
        // with b reached at 80% of its radius.
        const juce::Point<float> highlight { body.getX() + 0.38f * bodySize, body.getY() + 0.30f * bodySize };
        const auto farthest = highlight.getDistanceFrom (body.getBottomRight());
        juce::ColourGradient gradient (colours.a, highlight, colours.b, highlight.translated (farthest, 0.0f), true);
        gradient.clearColours();
        gradient.addColour (0.0, colours.a);
        gradient.addColour (0.8, colours.b);
        gradient.addColour (1.0, colours.b);
        g.setGradientFill (gradient);
        g.fillEllipse (body);
    }
    {
        juce::Path pointer;
        pointer.addRoundedRectangle (-1.5f, -bodySize * 0.5f + 4.0f, 3.0f, 10.0f, 1.5f);
        g.setColour (colours.pointer);
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
    }

    // The label, or the value while dragging (and on hover): emerald in the chrome, underlined in emerald
    // on an amp panel.
    if (! isEditingText())
    {
        const auto labelArea = juce::Rectangle<float> (box.getX() - 4.0f, wrap.getBottom() + 6.0f, box.getWidth() + 8.0f, 16.0f);
        const auto shown = getShownLabel();
        const auto f = tabular (geist (Weight::medium, 12.0f));
        g.setFont (f);
        const bool showingValue = isDragging() || isHovered();
        g.setColour (isDragging() && ! isPanelStyle() ? accent : colours.label);
        g.drawText (shown, labelArea, juce::Justification::centred, false);
        if (isDragging() && isPanelStyle() && showingValue)
        {
            const auto w = textWidth (f, shown);
            const auto baseline = labelArea.getCentreY() + 4.3f; // half the cap height below the centre
            g.setColour (accent);
            g.fillRect (juce::Rectangle<float> (labelArea.getCentreX() - w * 0.5f, baseline + 3.0f, w, 1.0f));
        }
    }

    if (keyboardFocus)
        drawFocusRing (g, box);
}

// ---- ValueField ----------------------------------------------------------------------------------

ValueField::ValueField (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String unit)
    : ParameterControl (s, parameterId, std::move (unit))
{
    pixelsForFullRange = 240.0f;
    horizontalDrag = true;
    doubleClickTypes = true;
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
}

void ValueField::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (surface);
    g.fillRoundedRectangle (bounds, 5.0f);

    // The bar: from the zero point to the value.
    const auto zero = getZeroPoint(), position = getNormalisedValue();
    auto bar = bounds.reduced (1.0f);
    bar = bar.withX (bar.getX() + bar.getWidth() * juce::jmin (zero, position)).withWidth (bar.getWidth() * std::abs (position - zero));
    if (showsBar && bar.getWidth() > 0.5f)
    {
        g.setColour (accentDim);
        g.fillRoundedRectangle (bar, 4.0f);
    }

    g.setColour (isDragging() ? accent : (hovered ? inkFaint : line2));
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    if (! isEditingText())
    {
        g.setColour (ink);
        g.setFont (font (Text::value));
        g.drawFittedText (getValueText(), getLocalBounds().reduced (4, 0), juce::Justification::centred, 1, 0.7f);
    }
    if (keyboardFocus)
        drawFocusRing (g, bounds.reduced (3.0f));
}

// ---- Fader ---------------------------------------------------------------------------------------

Fader::Fader (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String captionText, juce::String unit)
    : ParameterControl (s, parameterId, std::move (unit)), caption (std::move (captionText))
{
    pixelsForFullRange = 160.0f;
    doubleClickTypes = true;
}

juce::Rectangle<float> Fader::trackArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop ((float) captionHeight + 6.0f).withTrimmedBottom ((float) captionHeight + 8.0f);
}

juce::Rectangle<int> Fader::getTextEntryBounds() const
{
    return getLocalBounds().removeFromBottom (captionHeight + 4);
}

void Fader::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.setFont (geist (Weight::medium, 12.0f));
    g.setColour (inkDim);
    g.drawFittedText (caption, bounds.withHeight (captionHeight), juce::Justification::centred, 1, 0.8f);

    const auto track = trackArea();
    const auto lineArea = juce::Rectangle<float> (2.0f, track.getHeight()).withCentre (track.getCentre());
    g.setColour (hovered || isDragging() ? inkFaint : line2);
    g.fillRoundedRectangle (lineArea, 1.0f);

    // The fill from the zero point (0 dB) to the value, and a tick at zero.
    const auto yOf = [&track] (float normalised) { return track.getBottom() - normalised * track.getHeight(); };
    const auto y0 = yOf (getZeroPoint()), y1 = yOf (getNormalisedValue());
    g.setColour (accent);
    g.fillRect (lineArea.withY (juce::jmin (y0, y1)).withHeight (std::abs (y1 - y0)));
    g.setColour (inkFaint);
    g.fillRect (juce::Rectangle<float> (12.0f, 1.0f).withCentre ({ track.getCentreX(), y0 }));

    const auto thumb = juce::Rectangle<float> (20.0f, 8.0f).withCentre ({ track.getCentreX(), y1 });
    g.setColour (surface);
    g.fillRoundedRectangle (thumb, 3.0f);
    g.setColour (isDragging() ? accent : inkDim);
    g.drawRoundedRectangle (thumb.reduced (0.5f), 3.0f, 1.0f);

    if (! isEditingText())
    {
        g.setFont (font (Text::value));
        g.setColour (isDragging() ? accent : ink);
        g.drawFittedText (getValueText(), bounds.withTop (bounds.getBottom() - captionHeight - 2), juce::Justification::centred, 1, 0.7f);
    }
    if (keyboardFocus)
        drawFocusRing (g, bounds.toFloat().reduced (3.0f));
}

// ---- Switch --------------------------------------------------------------------------------------

Switch::Switch (const juce::String& label)
{
    setButtonText (label);
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

int Switch::getPreferredWidth() const
{
    const auto label = getButtonText();
    return 2 * margin + 28 + (label.isEmpty() ? 0 : 8 + juce::roundToInt (std::ceil (textWidth (geist (Weight::medium, 12.0f), label))));
}

void Switch::paintButton (juce::Graphics& g, bool, bool)
{
    const auto box = getLocalBounds().toFloat().reduced ((float) margin);
    const auto alpha = isEnabled() ? 1.0f : 0.4f;
    const bool on = getToggleState();
    const auto pill = juce::Rectangle<float> (box.getX(), box.getCentreY() - 8.0f, 28.0f, 16.0f);

    // The border in the text colour at 90% (CSS: border 1px currentColor, opacity .9), emerald when on.
    g.setColour ((on ? accent : textColour).withMultipliedAlpha (0.9f * alpha));
    g.drawRoundedRectangle (pill.reduced (0.5f), 7.5f, 1.0f);
    const auto dot = juce::Rectangle<float> (8.0f, 8.0f).withPosition (pill.getX() + (on ? 15.0f : 3.0f) + 1.0f, pill.getY() + 4.0f);
    g.setColour ((on ? accent : textColour).withMultipliedAlpha (0.9f * alpha));
    g.fillEllipse (dot);

    if (getButtonText().isNotEmpty())
    {
        g.setColour (textColour.withMultipliedAlpha (alpha));
        g.setFont (geist (Weight::medium, 12.0f));
        g.drawText (getButtonText(), box.withTrimmedLeft (36.0f), juce::Justification::centredLeft, false);
    }
    if (keyboardFocus)
        drawFocusRing (g, box);
}

bool Switch::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey)
    {
        triggerClick();
        return true;
    }
    return juce::ToggleButton::keyPressed (key);
}

void Switch::focusGained (FocusChangeType cause)
{
    keyboardFocus = cause != focusChangedByMouseClick;
    repaint();
}

void Switch::focusLost (FocusChangeType)
{
    keyboardFocus = false;
    repaint();
}

// ---- PowerSwitch ---------------------------------------------------------------------------------

PowerSwitch::PowerSwitch (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, bool isInverted)
    : state (s),
      parameter (lookUp (s, parameterId)),
      inverted (isInverted),
      attachment (parameter, [this] (float v)
                  {
                      on = (v >= 0.5f) != inverted;
                      repaint();
                      if (onChange)
                          onChange();
                  },
                  s.undoManager)
{
    tagged (*this, parameterId);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus (true);
    setTitle ("Bypass");
    attachment.sendInitialUpdate();
}

void PowerSwitch::paint (juce::Graphics& g)
{
    // The 7 px dot: emerald while the block is engaged, line-2 while it's bypassed (hovered: a step lighter).
    const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre (getLocalBounds().toFloat().getCentre());
    g.setColour (on ? accent : (isMouseOver() ? inkFaint : line2));
    g.fillEllipse (dot);
    if (keyboardFocus)
        drawFocusRing (g, dot);
}

void PowerSwitch::toggle()
{
    beginUndoStep (state);
    attachment.setValueAsCompleteGesture ((! on) != inverted ? 1.0f : 0.0f);
}

void PowerSwitch::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        toggle();
}

bool PowerSwitch::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey)
    {
        toggle();
        return true;
    }
    return false;
}

void PowerSwitch::focusGained (FocusChangeType cause)
{
    keyboardFocus = cause != focusChangedByMouseClick;
    repaint();
}

void PowerSwitch::focusLost (FocusChangeType)
{
    keyboardFocus = false;
    repaint();
}

// ---- IconButton ----------------------------------------------------------------------------------

IconButton::IconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i) {}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const bool chrome = icon == Icon::left || icon == Icon::right;
    if (! chrome)
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (highlighted ? inkFaint : line2);
        g.drawRoundedRectangle (bounds, radiusControl, 1.0f);
    }

    const auto c = getLocalBounds().toFloat().getCentre();
    const auto colour = (highlighted ? ink : (chrome ? inkFaint : inkDim)).withMultipliedAlpha (isEnabled() ? 1.0f : 0.35f);
    const juce::PathStrokeType stroke (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;

    switch (icon)
    {
        case Icon::undo:
        case Icon::redo:
        {
            const auto s = icon == Icon::undo ? 1.0f : -1.0f;
            p.startNewSubPath (c.x + s * 6.0f, c.y + 6.0f);
            p.lineTo (c.x + s * 6.0f, c.y + 1.0f);
            p.quadraticTo (c.x + s * 6.0f, c.y - 3.5f, c.x + s * 1.5f, c.y - 3.5f);
            p.lineTo (c.x - s * 6.0f, c.y - 3.5f);
            p.startNewSubPath (c.x - s * 2.5f, c.y - 7.0f);
            p.lineTo (c.x - s * 6.0f, c.y - 3.5f);
            p.lineTo (c.x - s * 2.5f, c.y);
            break;
        }
        case Icon::copy:
            p.addRoundedRectangle (c.x - 6.0f, c.y - 6.0f, 8.5f, 8.5f, 1.5f);
            p.addRoundedRectangle (c.x - 2.5f, c.y - 2.5f, 8.5f, 8.5f, 1.5f);
            break;
        case Icon::settings:
            for (int i = 0; i < 3; ++i)
            {
                const auto y = c.y - 5.0f + 5.0f * (float) i;
                p.startNewSubPath (c.x - 7.0f, y);
                p.lineTo (c.x + 7.0f, y);
                const auto knobX = c.x + (i == 0 ? 3.0f : (i == 1 ? -3.0f : 1.0f));
                p.addEllipse (knobX - 1.8f, y - 1.8f, 3.6f, 3.6f);
            }
            break;
        case Icon::midi:
            p.addEllipse (c.x - 7.5f, c.y - 7.5f, 15.0f, 15.0f);
            for (int i = 0; i < 5; ++i)
            {
                const auto a = juce::MathConstants<float>::pi * (0.75f + 0.375f * (float) i);
                p.addEllipse (c.x + 4.2f * std::cos (a) - 0.9f, c.y + 4.2f * std::sin (a) - 0.9f, 1.8f, 1.8f);
            }
            break;
        case Icon::left:
        case Icon::right:
        {
            // The reference's arrows are the 18 px guillemet characters; drawn as chevrons of that size.
            const auto s = icon == Icon::left ? 1.0f : -1.0f;
            p.startNewSubPath (c.x + s * 2.0f, c.y - 4.0f);
            p.lineTo (c.x - s * 2.0f, c.y);
            p.lineTo (c.x + s * 2.0f, c.y + 4.0f);
            break;
        }
    }

    g.setColour (colour);
    g.strokePath (p, stroke);
}

// ---- Segmented -----------------------------------------------------------------------------------

Segmented::Segmented (juce::StringArray choices) : options (std::move (choices))
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void Segmented::setSelected (int index, juce::NotificationType notification)
{
    index = juce::jlimit (0, juce::jmax (0, options.size() - 1), index);
    if (index == selected)
        return;
    selected = index;
    repaint();
    if (notification != juce::dontSendNotification && onChange)
        onChange (selected);
}

int Segmented::getPreferredWidth() const
{
    float w = 0.0f;
    for (int i = 0; i < options.size(); ++i)
        w += textWidth (geist (Weight::regular, 12.0f), options[i]) + (i > 0 ? 16.0f : 0.0f);
    return (int) std::ceil (w) + 2;
}

juce::Rectangle<float> Segmented::optionArea (int index) const
{
    const auto f = geist (Weight::regular, 12.0f);
    float x = 0.0f;
    for (int i = 0; i < options.size(); ++i)
    {
        const auto w = textWidth (f, options[i]);
        if (i == index)
            return { x, 0.0f, w, (float) getHeight() };
        x += w + 16.0f;
    }
    return {};
}

int Segmented::optionAt (juce::Point<float> p) const
{
    for (int i = 0; i < options.size(); ++i)
        if (optionArea (i).expanded (8.0f, 0.0f).contains (p))
            return i;
    return -1;
}

void Segmented::paint (juce::Graphics& g)
{
    g.setFont (geist (Weight::regular, 12.0f));
    for (int i = 0; i < options.size(); ++i)
    {
        const auto r = optionArea (i);
        g.setColour (i == selected ? ink : (i == hovered ? inkDim : inkFaint));
        g.drawText (options[i], r.withHeight (16.0f), juce::Justification::centredLeft, false);
        if (i == selected)
        {
            g.setColour (accent);
            g.fillRect (r.withY (20.0f).withHeight (1.0f));
        }
    }
    if (keyboardFocus)
        drawFocusRing (g, getLocalBounds().toFloat().reduced (3.0f));
}

void Segmented::mouseMove (const juce::MouseEvent& e)
{
    if (const auto h = optionAt (e.position); h != hovered)
    {
        hovered = h;
        repaint();
    }
}

void Segmented::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

void Segmented::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    if (const auto i = optionAt (e.position); i >= 0)
        setSelected (i, juce::sendNotificationSync);
}

bool Segmented::keyPressed (const juce::KeyPress& key)
{
    if (int direction = 0; isArrow (key, direction))
    {
        setSelected (selected + direction, juce::sendNotificationSync);
        return true;
    }
    return false;
}

void Segmented::focusGained (FocusChangeType cause)
{
    keyboardFocus = cause != focusChangedByMouseClick;
    repaint();
}

void Segmented::focusLost (FocusChangeType)
{
    keyboardFocus = false;
    repaint();
}

// ---- Drawing helpers -----------------------------------------------------------------------------

void drawGroupHeading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const auto f = geist (Weight::medium, 12.0f);
    g.setFont (f);
    g.setColour (inkFaint);
    g.drawText (title, area, juce::Justification::centredLeft, false);
    const auto w = juce::roundToInt (textWidth (f, title));
    g.setColour (line1);
    g.fillRect (juce::Rectangle<int> (area.getX() + w + 10, area.getCentreY(), juce::jmax (0, area.getWidth() - w - 10), 1));
}

void drawCard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill)
{
    if (! fill.isTransparent())
    {
        g.setColour (fill);
        g.fillRoundedRectangle (area, radiusCard);
    }
    g.setColour (line1);
    g.drawRoundedRectangle (area.reduced (0.5f), radiusCard, 1.0f);
}

} // namespace ui

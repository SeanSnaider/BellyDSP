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

/// Angles of the knob's 270-degree travel, clockwise from twelve o'clock (JUCE's arc convention).
constexpr float arcStart = -0.75f * juce::MathConstants<float>::pi;
constexpr float arcEnd = 0.75f * juce::MathConstants<float>::pi;

/// Starts a fresh undo step: the parameter tree first catches up with values still waiting to be
/// copied into it (it does so on a timer), so the previous gesture's last values land in its own step.
void flushUndo (juce::AudioProcessorValueTreeState& state)
{
    state.copyState();
}
} // namespace

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
    setWantsKeyboardFocus (false);
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
    return parameter.getText (parameter.convertTo0to1 (value), 0) + suffix;
}

float ParameterControl::stepSize() const
{
    // Choices and small integer ranges move a whole step at a time; continuous ranges by a fraction.
    const auto steps = parameter.getNumSteps();
    return steps > 1 && steps <= 128 ? 1.0f / (float) (steps - 1) : 0.0f;
}

void ParameterControl::beginUndoStep()
{
    flushUndo (state);
}

void ParameterControl::setNormalised (float normalised, bool partOfGesture)
{
    const auto plain = parameter.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised));
    if (partOfGesture)
        attachment.setValueAsPartOfGesture (plain);
    else
        attachment.setValueAsCompleteGesture (plain);
}

void ParameterControl::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! isEnabled())
        return;

    if (isEditingText())
        hideTextEntry (true);

    beginUndoStep();

    if (e.mods.isAltDown())
    {
        // Alt-click: back to the default, as one gesture.
        attachment.setValueAsCompleteGesture (parameter.convertFrom0to1 (parameter.getDefaultValue()));
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

    // Up or right raises the value. Each event moves by its own distance, so pressing or releasing
    // shift mid-drag changes the speed without a jump.
    const auto delta = (e.position.x - lastDragPosition.x) - (e.position.y - lastDragPosition.y);
    lastDragPosition = e.position;
    const auto perPixel = (e.mods.isShiftDown() ? 0.1f : 1.0f) / pixelsForFullRange;
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
    if (! e.mods.isPopupMenu() && isEnabled())
        showTextEntry();
}

void ParameterControl::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || isEditingText() || dragging)
        return;

    auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    if (wheel.isReversed)
        delta = -delta;
    if (delta == 0.0f)
        return;

    // A notch of a wheel moves 15% of the range (1.5% with shift), as JUCE's sliders do; a choice moves
    // one step per notch. A run of wheel moves is one gesture (one undo step), ended 350 ms after the
    // last move, so a trackpad's stream of small scrolls doesn't fill the undo history.
    const auto step = stepSize();
    const auto change = step > 0.0f ? (delta > 0.0f ? step : -step) : delta * (e.mods.isShiftDown() ? 0.015f : 0.15f);
    if (! wheeling)
    {
        beginUndoStep();
        attachment.beginGesture();
        wheeling = true;
    }
    setNormalised (getNormalisedValue() + change, true);
    startTimer (350);
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
    beginUndoStep();
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

Knob::Knob (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String captionText, juce::String unit, Size knobSize)
    : ParameterControl (s, parameterId, std::move (unit)), caption (std::move (captionText)), size (knobSize)
{
    pixelsForFullRange = size == Size::normal ? 200.0f : 160.0f;
}

void Knob::setKnobSize (Size newSize)
{
    if (newSize != size)
    {
        size = newSize;
        pixelsForFullRange = size == Size::normal ? 200.0f : 160.0f;
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

juce::Rectangle<float> Knob::dialArea() const
{
    auto area = getLocalBounds().toFloat().withTrimmedTop ((float) captionHeight + 2.0f).withTrimmedBottom ((float) captionHeight + 2.0f);
    const auto diameter = juce::jmin ((float) (size == Size::normal ? knobNormal : knobCompact), area.getWidth(), area.getHeight());
    return juce::Rectangle<float> (diameter, diameter).withCentre (area.getCentre());
}

juce::Rectangle<int> Knob::getTextEntryBounds() const
{
    return juce::Rectangle<int> (0, getHeight() - captionHeight - 2, getWidth(), captionHeight + 2).withSizeKeepingCentre (juce::jmin (getWidth(), 76), captionHeight + 4);
}

void Knob::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const auto dial = dialArea();

    // The caption above and the value below, hugging the dial.
    const auto captionArea = juce::Rectangle<float> ((float) bounds.getX(), dial.getY() - (float) captionHeight - 3.0f, (float) bounds.getWidth(), (float) captionHeight);
    const auto valueArea = juce::Rectangle<float> ((float) bounds.getX(), dial.getBottom() + 3.0f, (float) bounds.getWidth(), (float) captionHeight);
    g.setFont (font (Text::label));
    g.setColour (textDim);
    g.drawFittedText (caption, captionArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);

    if (! isEditingText())
    {
        g.setFont (font (Text::value));
        g.setColour (isDragging() ? accent : text);
        g.drawFittedText (getValueText(), valueArea.toNearestInt(), juce::Justification::centred, 1, 0.75f);
    }

    // The track and the value arc, from the zero point (the centre of a bipolar range).
    const bool normal = size == Size::normal;
    const auto stroke = normal ? 3.0f : 2.5f;
    const auto radius = dial.getWidth() * 0.5f - stroke * 0.5f;
    const auto centre = dial.getCentre();
    const auto angleOf = [] (float normalised) { return arcStart + normalised * (arcEnd - arcStart); };

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, arcStart, arcEnd, true);
    g.setColour (hovered || isDragging() ? outline.brighter (0.3f) : outline);
    g.strokePath (track, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto from = angleOf (getZeroPoint()), to = angleOf (getNormalisedValue());
    if (std::abs (to - from) > 0.01f)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::jmin (from, to), juce::jmax (from, to), true);
        g.setColour (accent);
        g.strokePath (arc, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The face: raised, lit from above, with a soft shadow.
    const auto faceRadius = radius - (normal ? 6.5f : 5.0f);
    const auto face = juce::Rectangle<float> (faceRadius * 2.0f, faceRadius * 2.0f).withCentre (centre);
    juce::Path facePath;
    facePath.addEllipse (face);
    juce::DropShadow (juce::Colours::black.withAlpha (0.45f), normal ? 6 : 4, { 0, 2 }).drawForPath (g, facePath);
    g.setGradientFill (juce::ColourGradient (surfaceRaised.brighter (0.12f), face.getCentreX(), face.getY(), surfaceRaised.darker (0.15f),
                                             face.getCentreX(), face.getBottom(), false));
    g.fillPath (facePath);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawEllipse (face.reduced (0.5f), 1.0f);

    // The pointer.
    const auto angle = angleOf (getNormalisedValue());
    const juce::Point<float> direction { std::sin (angle), -std::cos (angle) };
    g.setColour (text);
    g.drawLine (juce::Line<float> (centre + direction * (faceRadius * 0.35f), centre + direction * (faceRadius * 0.82f)), normal ? 2.5f : 2.0f);
}

// ---- ValueField ----------------------------------------------------------------------------------

ValueField::ValueField (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String unit)
    : ParameterControl (s, parameterId, std::move (unit))
{
    pixelsForFullRange = 240.0f;
}

void ValueField::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (hovered ? surfaceRaised.brighter (0.06f) : surfaceRaised);
    g.fillRoundedRectangle (bounds, radiusControl);

    // The bar: from the zero point to the value.
    const auto zero = getZeroPoint(), position = getNormalisedValue();
    auto bar = bounds.reduced (1.0f);
    bar = bar.withX (bar.getX() + bar.getWidth() * juce::jmin (zero, position)).withWidth (bar.getWidth() * std::abs (position - zero));
    if (showsBar && bar.getWidth() > 0.5f)
    {
        g.setColour (accentSoft);
        g.fillRoundedRectangle (bar, radiusControl - 1.0f);
    }

    g.setColour (isDragging() ? accent : (hovered ? outline.brighter (0.25f) : outline));
    g.drawRoundedRectangle (bounds, radiusControl, 1.0f);

    if (! isEditingText())
    {
        g.setColour (text);
        g.setFont (font (Text::value));
        g.drawFittedText (getValueText(), getLocalBounds().reduced (4, 0), juce::Justification::centred, 1, 0.7f);
    }
}

// ---- Fader ---------------------------------------------------------------------------------------

Fader::Fader (juce::AudioProcessorValueTreeState& s, const juce::String& parameterId, juce::String captionText, juce::String unit)
    : ParameterControl (s, parameterId, std::move (unit)), caption (std::move (captionText))
{
    pixelsForFullRange = 160.0f;
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
    g.setFont (font (Text::label));
    g.setColour (textDim);
    g.drawFittedText (caption, bounds.withHeight (captionHeight), juce::Justification::centred, 1, 0.8f);

    const auto track = trackArea();
    const auto line = juce::Rectangle<float> (4.0f, track.getHeight()).withCentre (track.getCentre());
    g.setColour (hovered || isDragging() ? outline.brighter (0.3f) : outline);
    g.fillRoundedRectangle (line, 2.0f);

    // The fill from the zero point (0 dB) to the value, and a tick at zero.
    const auto yOf = [&track] (float normalised) { return track.getBottom() - normalised * track.getHeight(); };
    const auto y0 = yOf (getZeroPoint()), y1 = yOf (getNormalisedValue());
    g.setColour (accent);
    g.fillRoundedRectangle (line.withY (juce::jmin (y0, y1)).withHeight (std::abs (y1 - y0)), 2.0f);
    g.setColour (textDim.withAlpha (0.6f));
    g.fillRect (juce::Rectangle<float> (14.0f, 1.0f).withCentre ({ track.getCentreX(), y0 }));

    const auto thumb = juce::Rectangle<float> (22.0f, 10.0f).withCentre ({ track.getCentreX(), y1 });
    g.setColour (surfaceRaised.brighter (0.1f));
    g.fillRoundedRectangle (thumb, 3.0f);
    g.setColour (isDragging() ? accent : text.withAlpha (0.8f));
    g.drawRoundedRectangle (thumb.reduced (0.5f), 3.0f, 1.0f);

    if (! isEditingText())
    {
        g.setFont (font (Text::value));
        g.setColour (isDragging() ? accent : text);
        g.drawFittedText (getValueText(), bounds.withTop (bounds.getBottom() - captionHeight - 2), juce::Justification::centred, 1, 0.7f);
    }
}

// ---- Switch, PowerSwitch -------------------------------------------------------------------------

int Switch::getPreferredWidth() const
{
    return 30 + (getButtonText().isEmpty() ? 0 : 8 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (font (Text::body), getButtonText())) + 4);
}

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
    attachment.sendInitialUpdate();
}

void PowerSwitch::paint (juce::Graphics& g)
{
    // A small pill: the accent and a light thumb on the right when on, hollow with the thumb on the
    // left when off (state is never shown by colour alone).
    const auto h = juce::jmin (14.0f, (float) getHeight() - 2.0f);
    const auto pill = juce::Rectangle<float> (h * 1.75f, h).withCentre (getLocalBounds().toFloat().getCentre());
    const auto thumb = juce::Rectangle<float> (h - 4.0f, h - 4.0f).withCentre ({ on ? pill.getRight() - h * 0.5f : pill.getX() + h * 0.5f, pill.getCentreY() });
    const auto hover = isMouseOver();
    if (on)
    {
        g.setColour (hover ? accent.brighter (0.15f) : accent);
        g.fillRoundedRectangle (pill, h * 0.5f);
        g.setColour (text);
        g.fillEllipse (thumb);
    }
    else
    {
        g.setColour (hover ? textDim : outline.brighter (0.45f));
        g.drawRoundedRectangle (pill.reduced (0.75f), h * 0.5f, 1.5f);
        g.setColour (textDim);
        g.fillEllipse (thumb.reduced (1.0f));
    }
}

void PowerSwitch::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    flushUndo (state);
    attachment.setValueAsCompleteGesture ((! on) != inverted ? 1.0f : 0.0f);
}

// ---- IconButton ----------------------------------------------------------------------------------

IconButton::IconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i) {}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    getLookAndFeel().drawButtonBackground (g, *this, findColour (getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId),
                                           highlighted, down);

    const auto c = getLocalBounds().toFloat().getCentre();
    const auto colour = (getToggleState() ? onAccent : theme::text).withMultipliedAlpha (isEnabled() ? 1.0f : 0.35f);
    const juce::PathStrokeType stroke (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path p;

    switch (icon)
    {
        case Icon::undo:
        case Icon::redo:
        {
            // A hook arrow: up from the bottom right, round, and back to the left (mirrored for redo).
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
            // Three faders: reads as "settings" without a gear's teeth.
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
            const auto s = icon == Icon::left ? 1.0f : -1.0f;
            p.startNewSubPath (c.x + s * 2.5f, c.y - 5.0f);
            p.lineTo (c.x - s * 2.5f, c.y);
            p.lineTo (c.x + s * 2.5f, c.y + 5.0f);
            break;
        }
    }

    g.setColour (colour);
    g.strokePath (p, stroke);
}

// ---- Drawing helpers -----------------------------------------------------------------------------

void drawGroupHeading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const auto f = font ("Semibold", 11.0f).withExtraKerningFactor (0.06f);
    const auto upper = title.toUpperCase();
    g.setFont (f);
    g.setColour (textDim);
    g.drawText (upper, area, juce::Justification::centredLeft, false);
    const auto textWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, upper));
    g.setColour (outline);
    g.fillRect (juce::Rectangle<int> (area.getX() + textWidth + 8, area.getCentreY(), juce::jmax (0, area.getWidth() - textWidth - 8), 1));
}

void drawCard (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill)
{
    g.setColour (fill);
    g.fillRoundedRectangle (area, radiusPanel);
    g.setColour (outline);
    g.drawRoundedRectangle (area.reduced (0.5f), radiusPanel, 1.0f);
}

} // namespace ui

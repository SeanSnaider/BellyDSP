// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "InspectorParts.h"

#include <cmath>

namespace ui
{

using namespace theme;

namespace
{
double nowSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

/// The meters' card: the ground colour in a hairline box (radius 8), so a live reading reads as an instrument set
/// into the panel.
void drawMeterCard (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (bg);
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (line1);
    g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);
}

juce::String dbText (float db)
{
    return juce::String (juce::roundToInt (db));
}
} // namespace

// ---- Headings ----------------------------------------------------------------------------------------

void drawInspectorHeading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const auto f = juce::Font (geist (Weight::semibold, 10.5f)).withExtraKerningFactor (0.09f);
    const auto words = title.toUpperCase();
    g.setFont (f);
    g.setColour (inkFaint);
    g.drawText (words, area, juce::Justification::centredLeft, false);
    const auto w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (f, words));
    g.setColour (line1);
    g.fillRect (juce::Rectangle<int> (area.getX() + w + 10, area.getCentreY(), juce::jmax (0, area.getWidth() - w - 10), 1));
}

// ---- GateMeterView -----------------------------------------------------------------------------------

GateMeterView::GateMeterView (juce::AudioProcessorValueTreeState& s, const juce::String& thresholdId)
    : state (s), threshold (*s.getParameter (thresholdId))
{
    tagged (*this, thresholdId); // a right-click on the meter is MIDI learn for the threshold it sets
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    setTooltip ("The gate's detector level against its thresholds: drag along the bar to set the threshold (the bright tick opens it, the "
                "dim one closes it; the shaded band between them is the hysteresis)");
}

GateMeterView::~GateMeterView()
{
    if (dragging)
        threshold.endChangeGesture();
}

void GateMeterView::set (const Reading& r)
{
    const auto moved = std::abs (r.meter.detectorDb - reading.meter.detectorDb) > 0.4f || r.meter.open != reading.meter.open
                       || std::abs (r.meter.openDb - reading.meter.openDb) > 0.05f || std::abs (r.meter.closeDb - reading.meter.closeDb) > 0.05f
                       || std::abs (r.meter.reductionDb - reading.meter.reductionDb) > 0.4f || r.on != reading.on || r.linked != reading.linked
                       || r.learning != reading.learning || std::abs (r.progress - reading.progress) > 0.01f;
    reading = r;
    if (moved)
        repaint();
}

juce::String GateMeterView::getStateText() const
{
    if (reading.learning)
        return "Learning " + juce::String (juce::roundToInt (reading.progress * 100.0f)) + "%";
    if (reading.linked)
        return "Following Gate A";
    if (! reading.on)
        return "Off";
    return reading.meter.open ? "Open" : "Closed";
}

juce::Rectangle<float> GateMeterView::barArea() const
{
    return { 12.0f, 34.0f, (float) getWidth() - 24.0f, 14.0f };
}

float GateMeterView::xForDb (float db) const
{
    const auto bar = barArea();
    return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db - floorDb) / -floorDb);
}

float GateMeterView::dbAtX (float x) const
{
    const auto bar = barArea();
    return floorDb + -floorDb * juce::jlimit (0.0f, 1.0f, (x - bar.getX()) / juce::jmax (1.0f, bar.getWidth()));
}

void GateMeterView::paint (juce::Graphics& g)
{
    const auto card = getLocalBounds().toFloat();
    drawMeterCard (g, card);
    const auto& m = reading.meter;
    const auto live = reading.on && ! reading.linked;
    const auto open = live && m.open;

    // The state: its LED (orange with a glow while the gate lets the guitar through) and its word; the level at the right.
    const auto led = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ 18.0f, 20.0f });
    if (open || reading.learning)
    {
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.5f), led.getCentre(), accent.withAlpha (0.0f), led.getCentre().translated (10.0f, 0.0f), true));
        g.fillEllipse (led.expanded (6.0f));
    }
    g.setColour (open || reading.learning ? accent : line2.brighter (0.15f));
    g.fillEllipse (led);
    g.setFont (geist (Weight::semibold, 13.0f));
    g.setColour (live || reading.learning ? ink : inkDim);
    g.drawText (getStateText(), juce::Rectangle<float> (30.0f, 11.0f, card.getWidth() * 0.6f, 18.0f), juce::Justification::centredLeft, false);
    g.setFont (tabular (geist (Weight::medium, 12.0f)));
    g.setColour (inkDim);
    g.drawText (m.detectorDb <= floorDb ? juce::String ("-inf dBFS") : dbText (m.detectorDb) + " dBFS",
                juce::Rectangle<float> (card.getWidth() * 0.5f, 11.0f, card.getWidth() * 0.5f - 12.0f, 18.0f), juce::Justification::centredRight, false);

    // The bar: the track, the hysteresis band, the level, the thresholds.
    const auto bar = barArea();
    g.setColour (line1);
    g.fillRoundedRectangle (bar, 4.0f);
    const auto openX = xForDb (m.openDb), closeX = xForDb (m.closeDb);
    if (! reading.linked)
    {
        g.setColour (accentDim);
        g.fillRect (juce::Rectangle<float> (juce::jmin (openX, closeX), bar.getY(), std::abs (openX - closeX), bar.getHeight()));
    }
    if (m.detectorDb > floorDb)
    {
        const auto fill = bar.withRight (xForDb (m.detectorDb)).reduced (0.0f, 3.0f);
        g.setColour (open ? accent : (live ? inkDim : inkFaint));
        g.fillRoundedRectangle (fill, 2.0f);
    }
    if (! reading.linked)
    {
        g.setColour (inkFaint);
        g.fillRect (juce::Rectangle<float> (closeX - 0.75f, bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f));
        g.setColour (ink);
        g.fillRect (juce::Rectangle<float> (openX - 1.0f, bar.getY() - 3.0f, 2.0f, bar.getHeight() + 6.0f));
        // The handle: a small triangle on the open tick, pointing down at the bar.
        juce::Path handle;
        handle.addTriangle (openX - 5.0f, bar.getY() - 10.0f, openX + 5.0f, bar.getY() - 10.0f, openX, bar.getY() - 4.0f);
        g.setColour (dragging ? accent : ink);
        g.fillPath (handle);
    }

    // The scale, every 25 dB.
    g.setFont (geist (Weight::regular, 10.0f));
    g.setColour (inkFaint);
    for (int db = -100; db <= 0; db += 25)
    {
        const auto x = xForDb ((float) db);
        const auto just = db == -100 ? juce::Justification::centredLeft : db == 0 ? juce::Justification::centredRight : juce::Justification::centred;
        const auto r = db == -100 ? juce::Rectangle<float> (x, bar.getBottom() + 3.0f, 40.0f, 12.0f)
                       : db == 0  ? juce::Rectangle<float> (x - 40.0f, bar.getBottom() + 3.0f, 40.0f, 12.0f)
                                  : juce::Rectangle<float> (x - 20.0f, bar.getBottom() + 3.0f, 40.0f, 12.0f);
        g.drawText (juce::String (db), r, just, false);
    }

    // The reduction: a caption, a thin bar from the right, its number.
    const auto rowY = card.getBottom() - 22.0f;
    g.setFont (geist (Weight::regular, 11.0f));
    g.setColour (inkFaint);
    g.drawText ("Reduction", juce::Rectangle<float> (12.0f, rowY, 70.0f, 14.0f), juce::Justification::centredLeft, false);
    const auto track = juce::Rectangle<float> (80.0f, rowY + 5.5f, card.getWidth() - 80.0f - 62.0f, 3.0f);
    g.setColour (line1);
    g.fillRoundedRectangle (track, 1.5f);
    const auto depth = juce::jlimit (0.0f, 1.0f, m.reductionDb / reductionRangeDb);
    if (depth > 0.0f)
    {
        g.setColour (accent.withAlpha (live || reading.linked ? 1.0f : 0.4f));
        g.fillRoundedRectangle (track.withLeft (track.getRight() - track.getWidth() * depth), 1.5f);
    }
    g.setFont (tabular (geist (Weight::medium, 12.0f)));
    g.setColour (ink);
    g.drawText (m.reductionDb >= 99.0f ? juce::String ("full") : dbText (m.reductionDb) + " dB",
                juce::Rectangle<float> (card.getRight() - 58.0f, rowY, 46.0f, 14.0f), juce::Justification::centredRight, false);
}

void GateMeterView::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || reading.linked || ! barArea().expanded (4.0f, 14.0f).contains (e.position))
        return;
    beginUndoStep (state);
    threshold.beginChangeGesture();
    dragging = true;
    mouseDrag (e);
}

void GateMeterView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || e.mods.isPopupMenu())
        return;
    // The open threshold goes to the level under the pointer (fine with shift: a quarter of the way).
    auto db = dbAtX (e.position.x);
    if (e.mods.isShiftDown())
        db = reading.meter.openDb + 0.25f * (db - reading.meter.openDb);
    threshold.setValueNotifyingHost (threshold.convertTo0to1 (juce::jlimit (threshold.getNormalisableRange().start, threshold.getNormalisableRange().end, db)));
    reading.meter.closeDb += threshold.convertFrom0to1 (threshold.getValue()) - reading.meter.openDb;
    reading.meter.openDb = threshold.convertFrom0to1 (threshold.getValue());
    repaint();
}

void GateMeterView::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    dragging = false;
    threshold.endChangeGesture();
    repaint();
}

// ---- CompressorMeterView -----------------------------------------------------------------------------

CompressorMeterView::CompressorMeterView (juce::AudioProcessorValueTreeState& s, const juce::String& p) : state (s), prefix (p)
{
    setInterceptsMouseClicks (false, false);
}

void CompressorMeterView::push (float reductionDb)
{
    reductionDb = juce::jmax (0.0f, reductionDb);
    history[(size_t) head] = reductionDb;
    head = (head + 1) % historyLength;
    const auto changed = std::abs (reductionDb - shown) > 0.05f || reductionDb > 0.05f;
    shown = reductionDb;
    if (changed || reductionDb > 0.0f)
        repaint();
    else
        repaint (getLocalBounds().withTrimmedLeft (getHeight())); // the trace still scrolls its last moves out
}

void CompressorMeterView::refreshCurve()
{
    const auto t = state.getRawParameterValue (prefix + "_threshold")->load();
    const auto r = state.getRawParameterValue (prefix + "_ratio")->load();
    const auto k = state.getRawParameterValue (prefix + "_knee")->load();
    if (! juce::exactlyEqual (t, drawnThreshold) || ! juce::exactlyEqual (r, drawnRatio) || ! juce::exactlyEqual (k, drawnKnee))
    {
        drawnThreshold = t;
        drawnRatio = r;
        drawnKnee = k;
        repaint();
    }
}

void CompressorMeterView::paint (juce::Graphics& g)
{
    // Left: the static curve in a square card.
    const auto side = (float) getHeight();
    const auto curveCard = juce::Rectangle<float> (0.0f, 0.0f, side, side);
    drawMeterCard (g, curveCard);
    const auto plot = curveCard.reduced (12.0f);
    const auto xOf = [&plot] (double db) { return plot.getX() + plot.getWidth() * (float) ((db + 60.0) / 60.0); };
    const auto yOf = [&plot] (double db) { return plot.getBottom() - plot.getHeight() * (float) ((juce::jlimit (-60.0, 0.0, db) + 60.0) / 60.0); };
    g.setColour (line1);
    for (double db = -48.0; db < 0.0; db += 12.0)
    {
        g.drawVerticalLine (juce::roundToInt (xOf (db)), plot.getY(), plot.getBottom());
        g.drawHorizontalLine (juce::roundToInt (yOf (db)), plot.getX(), plot.getRight());
    }
    g.setColour (inkFaint.withAlpha (0.5f));
    g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);

    const auto threshold = (double) state.getRawParameterValue (prefix + "_threshold")->load();
    const auto ratio = (double) state.getRawParameterValue (prefix + "_ratio")->load();
    const auto knee = (double) state.getRawParameterValue (prefix + "_knee")->load();
    juce::Path curve;
    for (int i = 0; i <= 120; ++i)
    {
        const auto x = -60.0 + 0.5 * i;
        const juce::Point<float> p { xOf (x), yOf (ampsim::Compressor::staticCurveDb (x, threshold, ratio, knee)) };
        i == 0 ? curve.startNewSubPath (p) : curve.lineTo (p);
    }
    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (ink);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ xOf (threshold), yOf (ampsim::Compressor::staticCurveDb (threshold, threshold, ratio, knee)) }));
    g.setFont (geist (Weight::regular, 10.0f));
    g.setColour (inkFaint);
    g.drawText ("in", juce::Rectangle<float> (plot.getRight() - 30.0f, plot.getBottom() - 12.0f, 28.0f, 12.0f), juce::Justification::centredRight, false);
    g.drawText ("out", juce::Rectangle<float> (plot.getX() + 2.0f, plot.getY(), 28.0f, 12.0f), juce::Justification::centredLeft, false);

    // Right: the number, then the trace in its own card.
    auto right = getLocalBounds().toFloat().withTrimmedLeft (side + 10.0f);
    const auto numberArea = right.removeFromTop (44.0f);
    g.setFont (tabular (geist (Weight::light, 26.0f)));
    g.setColour (shown >= 0.05f ? ink : inkDim);
    const auto number = shown >= 0.05f ? "-" + juce::String (shown, 1) : juce::String ("0.0");
    g.drawText (number, numberArea.withHeight (28.0f), juce::Justification::centredLeft, false);
    const auto numberWidth = textWidth (tabular (geist (Weight::light, 26.0f)), number);
    g.setFont (geist (Weight::regular, 12.0f));
    g.setColour (inkFaint);
    g.drawText ("dB", juce::Rectangle<float> (numberArea.getX() + numberWidth + 4.0f, numberArea.getY() + 8.0f, 24.0f, 16.0f), juce::Justification::centredLeft, false);
    g.setFont (geist (Weight::regular, 11.0f));
    g.drawText ("Gain reduction", numberArea.withTrimmedTop (28.0f), juce::Justification::centredLeft, false);

    const auto traceCard = right.withTrimmedTop (4.0f);
    drawMeterCard (g, traceCard);
    const auto trace = traceCard.reduced (8.0f, 8.0f);
    g.setColour (line1);
    for (float db = 6.0f; db < rangeDb; db += 6.0f)
        g.fillRect (juce::Rectangle<float> (trace.getX(), trace.getY() + trace.getHeight() * db / rangeDb, trace.getWidth(), 1.0f));

    // The trace: oldest at the left, now at the right edge, hanging from the top (more reduction, further down).
    juce::Path line, fill;
    fill.startNewSubPath (trace.getX(), trace.getY());
    for (int i = 0; i < historyLength; ++i)
    {
        const auto v = history[(size_t) ((head + i) % historyLength)];
        const auto x = trace.getX() + trace.getWidth() * (float) i / (float) (historyLength - 1);
        const auto y = trace.getY() + trace.getHeight() * juce::jlimit (0.0f, 1.0f, v / rangeDb);
        i == 0 ? line.startNewSubPath (x, y) : line.lineTo (x, y);
        fill.lineTo (x, y);
    }
    fill.lineTo (trace.getRight(), trace.getY());
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.22f));
    g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setFont (geist (Weight::regular, 10.0f));
    g.setColour (inkFaint);
    g.drawText ("12", juce::Rectangle<float> (trace.getRight() - 20.0f, trace.getCentreY() - 12.0f, 20.0f, 11.0f), juce::Justification::centredRight, false);
    g.drawText ("24", juce::Rectangle<float> (trace.getRight() - 20.0f, trace.getBottom() - 11.0f, 20.0f, 11.0f), juce::Justification::centredRight, false);
}

// ---- TapPad ------------------------------------------------------------------------------------------

TapPad::TapPad() : juce::Button ("Tap")
{
    setTooltip ("Tap the tempo, for every synced effect (or use the footswitch's tap CC); the light blinks on the beat");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    startTimerHz (meterFps);
}

TapPad::~TapPad()
{
    stopTimer();
}

void TapPad::setTempo (double bpm)
{
    if (std::abs (bpm - tempo) > 0.05)
    {
        tempo = bpm;
        repaint();
    }
}

bool TapPad::isBeatLit() const
{
    // On for the first 90 ms of each beat, counted from the clock (the pad shows the tempo's speed, not the song's beat).
    const auto beat = 60.0 / juce::jmax (1.0, tempo);
    return std::fmod (nowSeconds(), beat) < 0.09;
}

void TapPad::timerCallback()
{
    if (! isShowing())
        return;
    if (const auto now = isBeatLit(); now != lit || flashUntil > 0.0)
    {
        lit = now;
        if (flashUntil > 0.0 && nowSeconds() > flashUntil)
            flashUntil = 0.0;
        repaint();
    }
}

void TapPad::clicked()
{
    flashUntil = nowSeconds() + 0.15;
    repaint();
}

void TapPad::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto box = getLocalBounds().toFloat().reduced (0.5f);
    const auto flashing = flashUntil > 0.0 && nowSeconds() < flashUntil;
    g.setColour (down || flashing ? raised.brighter (0.06f) : raised);
    g.fillRoundedRectangle (box, radiusControl);
    g.setColour (flashing || down ? accent : (highlighted ? inkFaint : line2));
    g.drawRoundedRectangle (box, radiusControl, 1.0f);

    // The beat light at the top right.
    const auto led = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ box.getRight() - 11.0f, box.getY() + 11.0f });
    if (lit)
    {
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.5f), led.getCentre(), accent.withAlpha (0.0f), led.getCentre().translated (8.0f, 0.0f), true));
        g.fillEllipse (led.expanded (5.0f));
    }
    g.setColour (lit ? accent : line2.brighter (0.1f));
    g.fillEllipse (led);

    // "Tap" (the tempo itself is in the field under the pad).
    g.setFont (geist (Weight::semibold, 15.0f));
    g.setColour (ink);
    g.drawText ("Tap", box, juce::Justification::centred, false);
}

// ---- HearingView -------------------------------------------------------------------------------------

void HearingView::set (const Reading& r)
{
    if (everSet && r.note == reading.note && r.shifts == reading.shifts && r.sounding == reading.sounding)
        return;
    everSet = true;
    reading = r;
    repaint();
}

juce::String HearingView::getText() const
{
    if (reading.note < 0)
        return "Listening";
    juce::StringArray parts;
    parts.add ("Hearing " + juce::MidiMessage::getMidiNoteName (reading.note, true, true, 4));
    for (int v = 0; v < 4; ++v)
        if (reading.sounding[(size_t) v])
            parts.add (juce::String (v + 1) + " " + (reading.shifts[(size_t) v] >= 0 ? "+" : "") + juce::String (reading.shifts[(size_t) v]) + " "
                       + juce::MidiMessage::getMidiNoteName (reading.note + reading.shifts[(size_t) v], true, true, 4));
    return parts.joinIntoString (", ");
}

void HearingView::paint (juce::Graphics& g)
{
    const auto card = getLocalBounds().toFloat();
    drawMeterCard (g, card);

    // Left: the note heard, large; "Listening" while there's no single note.
    auto left = card.withWidth (card.getWidth() * 0.42f).reduced (12.0f, 10.0f);
    g.setFont (geist (Weight::regular, 11.0f));
    g.setColour (inkFaint);
    g.drawText ("Hearing", left.removeFromTop (14.0f), juce::Justification::centredLeft, false);
    const auto heard = reading.note >= 0;
    g.setFont (geist (Weight::light, heard ? 34.0f : 15.0f));
    g.setColour (heard ? ink : inkDim);
    g.drawFittedText (heard ? juce::MidiMessage::getMidiNoteName (reading.note, true, true, 4) : juce::String ("Listening..."), left.toNearestInt(),
                      juce::Justification::centredLeft, 1, 0.8f);

    // Right: each voice's row: its number, its interval, its note.
    g.setColour (line1);
    g.fillRect (juce::Rectangle<float> (card.getWidth() * 0.42f, card.getY() + 10.0f, 1.0f, card.getHeight() - 20.0f));
    auto rows = card.withTrimmedLeft (card.getWidth() * 0.42f + 12.0f).reduced (0.0f, 10.0f).withTrimmedRight (12.0f);
    const auto rowHeight = rows.getHeight() / 4.0f;
    for (int v = 0; v < 4; ++v)
    {
        auto row = rows.removeFromTop (rowHeight);
        const auto sounding = heard && reading.sounding[(size_t) v];
        g.setFont (geist (Weight::medium, 11.0f));
        g.setColour (sounding ? accent : inkFaint);
        g.drawText (juce::String (v + 1), row.removeFromLeft (16.0f), juce::Justification::centredLeft, false);
        g.setFont (tabular (geist (Weight::medium, 12.0f)));
        const auto shift = reading.shifts[(size_t) v];
        g.setColour (sounding ? ink : inkFaint);
        g.drawText (sounding ? (shift >= 0 ? "+" : "") + juce::String (shift) : juce::String ("-"), row.removeFromLeft (36.0f), juce::Justification::centredLeft, false);
        g.setColour (sounding ? inkDim : inkFaint);
        if (sounding)
            g.drawText (juce::MidiMessage::getMidiNoteName (reading.note + shift, true, true, 4), row, juce::Justification::centredLeft, false);
    }
}

// ---- The cab's front ---------------------------------------------------------------------------------

void drawCabFront (juce::Graphics& g, juce::Rectangle<float> box, bool on)
{
    const auto r = juce::jmin (box.getWidth(), box.getHeight()) * 0.07f;

    // The tolex: a dark leather-grain gradient, lit a little from below like the stage, with a hairline edge.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a2826), box.getX(), box.getY(), juce::Colour (0xff121110), box.getX(), box.getBottom(), false));
    g.fillRoundedRectangle (box, r);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (box.reduced (0.5f), r, 1.0f);

    // The grille cloth: a warm dark weave in a frame of piping, the badge at its top.
    const auto inset = juce::jmax (3.0f, box.getWidth() * 0.09f);
    const auto grille = box.reduced (inset).withTrimmedTop (inset * 0.4f);
    {
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (grille.toNearestInt());
        g.setColour (juce::Colour (0xff27231f));
        g.fillRect (grille);
        // The weave: fine diagonals both ways, alternately light and dark.
        const auto step = juce::jmax (1.6f, grille.getWidth() / 34.0f);
        for (float d = -grille.getHeight(); d < grille.getWidth(); d += step)
        {
            g.setColour (juce::Colour (0xff3b352e).withAlpha (0.55f));
            g.drawLine (grille.getX() + d, grille.getBottom(), grille.getX() + d + grille.getHeight(), grille.getY(), 0.6f);
            g.setColour (juce::Colour (0xff15130f).withAlpha (0.5f));
            g.drawLine (grille.getX() + d, grille.getY(), grille.getX() + d + grille.getHeight(), grille.getBottom(), 0.6f);
        }
        // The speakers behind it, as faint shadows of their cones: two stacked on a tall front, one on a square one.
        const auto tall = grille.getHeight() > grille.getWidth() * 1.35f;
        const auto count = tall ? 2 : 1;
        for (int i = 0; i < count; ++i)
        {
            const auto cell = tall ? grille.withHeight (grille.getHeight() / 2.0f).translated (0.0f, (float) i * grille.getHeight() / 2.0f) : grille;
            const auto d = juce::jmin (cell.getWidth(), cell.getHeight()) * 0.86f;
            const auto cone = juce::Rectangle<float> (d, d).withCentre (cell.getCentre());
            g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.42f), cone.getCentre(), juce::Colours::black.withAlpha (0.08f),
                                                     cone.getCentre().translated (d * 0.5f, 0.0f), true));
            g.fillEllipse (cone);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawEllipse (cone, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillEllipse (cone.withSizeKeepingCentre (d * 0.28f, d * 0.28f));
        }
        // The cloth's sheen, from below.
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.0f), grille.getX(), grille.getY(), juce::Colour (0x14ffd9a8), grille.getX(),
                                                 grille.getBottom(), false));
        g.fillRect (grille);
    }
    g.setColour (juce::Colour (0xff8c8378).withAlpha (0.8f));
    g.drawRoundedRectangle (grille.expanded (1.0f), 1.5f, 1.2f);

    // The badge: a small plate at the grille's top, its letters too small to read, lit when the cab plays.
    const auto badge = juce::Rectangle<float> (grille.getWidth() * 0.36f, juce::jmax (3.0f, grille.getWidth() * 0.1f))
                           .withCentre ({ grille.getCentreX(), grille.getY() + grille.getWidth() * 0.12f });
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd9d4cc), badge.getX(), badge.getY(), juce::Colour (0xff8f8a82), badge.getX(), badge.getBottom(), false));
    g.fillRoundedRectangle (badge, 1.0f);

    if (! on)
    {
        g.setColour (bg.withAlpha (0.5f));
        g.fillRoundedRectangle (box, r);
    }
}

} // namespace ui

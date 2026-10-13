// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TunerPage.h"
#include "Overlay.h"

namespace ui
{

using namespace theme;

namespace
{
const juce::Identifier tunerTuningKey { "tunerTuning" }; // the chosen tuning (app view state)
const char* const pitchNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// The page, in the 1200 x 598 the overlay frame gives it: the header (48), the display (368, from 56), and under it
// the strings and the controls (162, from 436).
constexpr int displayTop = 56, displayHeight = 368, lowerTop = 436, lowerHeight = 162, controlsWidth = 440;
constexpr int noteHeight = 200, scaleWidth = 960, chipWidth = 100, chipHeight = 98, chipGap = 12;
constexpr float noteSize = 200.0f;

/// Draws text with its baseline at y, left edge at x, with extra tracking between glyphs; returns its width.
float drawAt (juce::Graphics& g, const juce::String& s, const juce::FontOptions& f, float x, float baseline, float tracking = 0.0f)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (juce::Font (f), s, x, baseline);
    for (int i = 1; i < glyphs.getNumGlyphs(); ++i)
        glyphs.moveRangeOfGlyphs (i, 1, tracking * (float) i, 0.0f);
    glyphs.draw (g);
    return textWidth (f, s) + tracking * (float) s.length();
}

/// The baseline of a line box `height` high for a font (the content area centred in it).
float baselineIn (const juce::FontOptions& f, float top, float height)
{
    const juce::Font font (f);
    return top + (height - (font.getAscent() + font.getDescent())) * 0.5f + font.getAscent();
}

/// A chevron pointing right (+1) or left (-1), 22 wide and 40 tall, centred on c.
juce::Path chevron (juce::Point<float> c, int direction)
{
    juce::Path p;
    const auto d = (float) direction;
    p.startNewSubPath (c.x - 9.0f * d, c.y - 20.0f);
    p.lineTo (c.x + 11.0f * d, c.y);
    p.lineTo (c.x - 9.0f * d, c.y + 20.0f);
    return p;
}
} // namespace

const std::array<TunerPage::Tuning, 4>& TunerPage::tunings()
{
    static const std::array<Tuning, 4> list { { { "Standard", { 40, 45, 50, 55, 59, 64 } },
                                                 { "Drop D", { 38, 45, 50, 55, 59, 64 } },
                                                 { "Drop C", { 36, 43, 48, 53, 57, 62 } },
                                                 { "FACGCE", { 41, 45, 48, 55, 60, 64 } } } };
    return list;
}

TunerPage::TunerPage (AmpSimProcessor& p) : ControlGroup (p)
{
    juce::StringArray names;
    for (const auto& t : tunings())
        names.add (t.name);
    tuningChoice = std::make_unique<Segmented> (names);
    tuningChoice->setPillStyle (true);
    tuningChoice->onChange = [this] (int i) { setTuning (i); };
    tuningChoice->setTooltip ("The tuning: its six strings show under the display");
    addAndMakeVisible (*tuningChoice);

    const auto stepA4 = [this] (float delta)
    {
        const auto now = state.getRawParameterValue ("tuner_a4")->load();
        setAsGesture (state, "tuner_a4", juce::jlimit (430.0f, 450.0f, std::round (now + delta)));
        repaint (a4Area);
    };
    a4Down = &addButton (juce::CharPointer_UTF8 ("\xe2\x88\x92"), [stepA4] { stepA4 (-1.0f); }); // the minus sign
    a4Up = &addButton ("+", [stepA4] { stepA4 (1.0f); });
    for (auto* b : { a4Down, a4Up })
    {
        tagged (*b, "tuner_a4");
        b->setTooltip ("The reference: A4 from 430 to 450 Hz");
        b->setHasFocusOutline (true);
    }
    mute = &addSwitch ("tuner_mute", "Mute output");
    mute->setTooltip ("Silence the output while the tuner is open");
    closeButton = &addButton ("Close", [this] { if (onClose) onClose(); });
    closeButton->setTooltip ("Back to the main screen (T or Esc)");
    closeButton->setHasFocusOutline (true);

    setTuning ((int) state.state.getProperty (tunerTuningKey, 0));
}

TunerPage::~TunerPage()
{
    stopTimer();
}

void TunerPage::setTuning (int index)
{
    index = juce::jlimit (0, (int) tunings().size() - 1, index);
    if (index != tuning)
    {
        done = {};
        target = -1;
    }
    tuning = index;
    tuningChoice->setSelected (index);
    state.state.setProperty (tunerTuningKey, index, nullptr);
    update (reading);
}

void TunerPage::clickString (int index)
{
    target = target == index ? -1 : juce::jlimit (0, 5, index);
    update (reading);
}

void TunerPage::freeze (const ampsim::TunerReading& r)
{
    frozen = true;
    update (r);
}

void TunerPage::pageShown()
{
    startTimerHz (meterFps);
}

void TunerPage::pageHidden()
{
    stopTimer();
}

void TunerPage::refresh()
{
    repaint (a4Area);
    repaint (header);
}

void TunerPage::timerCallback()
{
    if (! frozen)
        update (ampSim.getTunerReading());
}

void TunerPage::update (const ampsim::TunerReading& r)
{
    reading = r;
    Shown s;
    const auto& strings = tunings()[(size_t) tuning].midi;

    if (! r.hasReading)
    {
        s.note = juce::CharPointer_UTF8 ("\xe2\x80\x93"); // an en dash
        s.hz = "--";
        s.cents = "--";
        s.state = "Play a string";
        s.currentString = target >= 0 ? target : lastString;
    }
    else
    {
        // The string: the clicked one, or the one nearest the note heard (in semitones, from the frequency).
        const auto heard = 69.0 + 12.0 * std::log2 (r.frequency / r.referenceA4);
        auto current = target;
        if (current < 0)
        {
            current = 0;
            for (int i = 1; i < 6; ++i)
                if (std::abs (heard - strings[(size_t) i]) < std::abs (heard - strings[(size_t) current]))
                    current = i;
        }
        lastString = current;
        s.currentString = current;

        // The note and its cents: the tuner's own (its smoothed offset from the note it reads), or, with a
        // string targeted, the same measured against that string's note.
        auto note = r.midiNote;
        auto cents = r.cents;
        if (target >= 0)
        {
            cents += 100.0 * (r.midiNote - strings[(size_t) target]);
            note = strings[(size_t) target];
        }
        const juce::String name (pitchNames[((note % 12) + 12) % 12]);
        s.note = name.substring (0, 1);
        s.accidental = name.length() > 1 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xaf")) : juce::String(); // the sharp sign
        s.octave = juce::String (note / 12 - 1);
        s.hz = juce::String (r.frequency, 2);
        const auto rounded = juce::roundToInt (cents);
        s.cents = (rounded >= 0 ? "+" : "") + juce::String (rounded);
        s.centsValue = cents;
        s.inTune = std::abs (cents) < inTuneCents;
        s.state = s.inTune ? "In tune" : cents < 0.0 ? "Flat" : "Sharp";
        if (! r.live)
            s.state = "Holding";

        // A string counts as tuned once it has been in tune this session.
        if (r.live && s.inTune && note == strings[(size_t) current])
            done[(size_t) current] = true;
    }
    shown = s;
    repaint();
}

float TunerPage::segmentLight (double segmentCents, double cents)
{
    // A segment lights fully under the cents and fades linearly to dark 3.75 cents (one and a half segments) away,
    // so between two segments both are part lit and the position reads between them. Off the scale: the end
    // segment, as a needle pinned at its stop.
    const auto c = juce::jlimit (-50.0, 50.0, cents);
    return (float) juce::jmax (0.0, 1.0 - std::abs (segmentCents - c) / 3.75);
}

int TunerPage::getLitChevrons() const
{
    if (! reading.hasReading)
        return 0;
    return shown.inTune ? 2 : (shown.centsValue < 0.0 ? -1 : 1);
}

juce::Rectangle<float> TunerPage::stringBox (int index) const
{
    const auto total = 6 * chipWidth + 5 * chipGap;
    const auto x0 = (float) stringsArea.getX() + (float) (stringsArea.getWidth() - total) * 0.5f;
    return { x0 + (float) (index * (chipWidth + chipGap)), (float) stringsArea.getY(), (float) chipWidth, (float) chipHeight };
}

void TunerPage::paint (juce::Graphics& g)
{
    const auto has = reading.hasReading;
    const auto inTune = has && shown.inTune;

    overlay::paintHeader (g, header, "Tuner",
                          state.getRawParameterValue ("tuner_mute")->load() >= 0.5f ? "Listening to the clean DI. The output is muted while it's open."
                                                                                     : "Listening to the clean DI. The output plays while you tune.",
                          "T|or;Esc|closes it");

    // ---- The display: a dark panel lit from below, warmer while in tune --------------------------------------
    {
        const auto d = displayArea.toFloat();
        g.setColour (panelBg);
        g.fillRoundedRectangle (d, radiusCard);
        {
            const juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (d, radiusCard);
            g.reduceClipRegion (clip);
            g.setGradientFill (juce::ColourGradient (stageLight.withAlpha (0.55f), d.getCentreX(), d.getBottom() + 40.0f, panelBg.withAlpha (0.0f),
                                                     d.getCentreX(), d.getY() + 40.0f, true));
            g.fillRect (d);
            if (inTune)
            {
                // A soft glow behind the note: the in-tune state reads from across the room.
                const auto c = noteArea.toFloat().getCentre();
                g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.16f), c.x, c.y, accent.withAlpha (0.0f), c.x + 300.0f, c.y, true));
                g.fillRect (d);
            }
        }
        g.setColour (inTune ? accent.withAlpha (0.45f) : line1);
        g.drawRoundedRectangle (d.reduced (0.5f), radiusCard, 1.0f);
    }

    const auto noteColour = inTune ? accent : (has ? ink : inkFaint);

    // The note: 200 px light, tracking -.04em; the sharp (60 px) at its top right and the octave (32 px) at its foot.
    {
        const auto big = geist (Weight::light, noteSize), acc = geist (Weight::light, 60.0f), oct = geist (Weight::light, 32.0f);
        const auto top = (float) noteArea.getY();
        const auto noteWidth = textWidth (big, shown.note) - 0.04f * noteSize;
        const auto accWidth = shown.accidental.isEmpty() ? 0.0f : textWidth (acc, shown.accidental);
        const auto octWidth = textWidth (oct, shown.octave);
        const auto sideWidth = juce::jmax (accWidth, octWidth);
        // The letter is centred on the page; the sharp and the octave hang off its right.
        auto x = (float) noteArea.getCentreX() - noteWidth * 0.5f;
        g.setColour (noteColour);
        drawAt (g, shown.note, big, x, baselineIn (big, top, (float) noteHeight), -0.04f * noteSize);
        x += noteWidth + 8.0f;
        const juce::Font accFont (acc), octFont (oct);
        g.setColour (inTune ? accent : inkDim);
        drawAt (g, shown.accidental, acc, x, top + 30.0f + accFont.getAscent());
        g.setColour (inkFaint);
        drawAt (g, shown.octave, oct, x, top + (float) noteHeight - 34.0f - octFont.getDescent());
        juce::ignoreUnused (sideWidth);

        // The chevrons: the left one points right (tune up) and lights when flat, the right one when sharp; both in
        // the accent when in tune.
        const auto lit = getLitChevrons();
        const auto cy = (float) noteArea.getCentreY() + 6.0f;
        for (const auto side : { -1, 1 })
        {
            const auto on = lit == side || lit == 2;
            const auto c = juce::Point<float> ((float) noteArea.getCentreX() + (float) side * 300.0f, cy);
            const auto path = chevron (c, -side);
            if (on)
            {
                g.setColour ((lit == 2 ? accent : ink).withAlpha (0.12f));
                g.strokePath (path, juce::PathStrokeType (12.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            g.setColour (on ? (lit == 2 ? accent : ink) : line2);
            g.strokePath (path, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setFont (geist (Weight::medium, 12.0f));
            g.setColour (on ? (lit == 2 ? accent : inkDim) : inkFaint);
            g.drawText (side < 0 ? "Flat" : "Sharp", juce::Rectangle<float> (c.x - 40.0f, cy + 30.0f, 80.0f, 16.0f), juce::Justification::centred, false);
        }
    }

    // The readout: "82.56 Hz", "+2 cents", the state; tabular figures, the state in 16 medium.
    {
        const auto faint = tabular (geist (Weight::regular, 15.0f)), bold = tabular (geist (Weight::medium, 15.0f)), stateFont = geist (Weight::medium, 16.0f);
        const auto hzText = shown.hz + " Hz";
        const auto centsWord = juce::String (" cents");
        const auto w1 = textWidth (faint, hzText), w2 = textWidth (bold, shown.cents) + textWidth (faint, centsWord), w3 = textWidth (stateFont, shown.state);
        const auto gap = 36.0f;
        auto x = (float) readArea.getCentreX() - (w1 + gap + w2 + gap + w3) * 0.5f;
        const auto baseline = baselineIn (faint, (float) readArea.getY(), (float) readArea.getHeight());
        g.setColour (inkFaint);
        x += drawAt (g, hzText, faint, x, baseline);
        g.setColour (line2);
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x + gap * 0.5f, (float) readArea.getCentreY() }));
        x += gap;
        g.setColour (has ? ink : inkFaint);
        x += drawAt (g, shown.cents, bold, x, baseline);
        g.setColour (inkFaint);
        x += drawAt (g, centsWord, faint, x, baseline);
        g.setColour (line2);
        g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x + gap * 0.5f, (float) readArea.getCentreY() }));
        x += gap;
        g.setColour (inTune ? accent : (has ? ink : inkDim));
        drawAt (g, shown.state, stateFont, x, baselineIn (stateFont, (float) readArea.getY(), (float) readArea.getHeight()));
    }

    // The scale: 41 segments, -50 to +50; taller at every 10 cents, taller still at +-25 and +-50, tallest at 0.
    {
        const auto s = scaleArea.toFloat();
        const auto mid = s.getY() + 28.0f;
        const auto xOf = [&s] (double cents) { return s.getX() + (float) ((cents + 50.0) / 100.0) * s.getWidth(); };
        if (inTune)
        {
            g.setColour (accent.withAlpha (0.14f));
            g.fillRoundedRectangle (juce::Rectangle<float> (xOf (0.0) - 46.0f, mid - 34.0f, 92.0f, 68.0f), 8.0f);
        }
        for (int k = -(numSegments / 2); k <= numSegments / 2; ++k)
        {
            const auto c = 2.5 * k;
            const auto zero = k == 0, major = k % 10 == 0, ten = k % 4 == 0;
            const auto h = zero ? 56.0f : major ? 44.0f : ten ? 36.0f : 28.0f;
            const auto bar = juce::Rectangle<float> (12.0f, h).withCentre ({ xOf (c), mid });
            const auto light = has ? segmentLight (c, shown.centsValue) : 0.0f;
            const auto centreLit = inTune && std::abs (k) <= 1;
            auto colour = zero ? line2.brighter (0.25f) : line2;
            if (centreLit)
                colour = accent;
            else if (light > 0.0f)
                colour = colour.interpolatedWith (inTune ? accent : ink, light);
            g.setColour (colour);
            g.fillRoundedRectangle (bar, 3.0f);
            if (zero && ! centreLit)
            {
                g.setColour (accent.withAlpha (0.5f));
                g.drawRoundedRectangle (bar.expanded (3.0f), 5.0f, 1.0f);
            }
            if (major || zero)
            {
                g.setColour (zero ? inkDim : inkFaint);
                g.setFont (tabular (geist (Weight::regular, 12.0f)));
                g.drawText (c > 0 ? "+" + juce::String (juce::roundToInt (c)) : juce::String (juce::roundToInt (c)),
                            juce::Rectangle<float> (xOf (c) - 30.0f, mid + 38.0f, 60.0f, 14.0f), juce::Justification::centredTop, false);
            }
        }
    }

    // ---- The strings ---------------------------------------------------------------------------------------
    {
        overlay::paintPanel (g, stringsPanel);
        auto in = stringsPanel.reduced (overlay::panelPadding + 4, overlay::panelPadding);
        auto labelRow = in.removeFromTop (16);
        overlay::paintGroupLabel (g, labelRow, "Strings, " + juce::String (tunings()[(size_t) tuning].name));
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkFaint);
        g.drawText (target >= 0 ? "Tuning to string " + juce::String (6 - target) + ". Click it again to let go"
                                : "Click a string to tune to it; the nearest one is followed",
                    labelRow, juce::Justification::centredRight, true);

        const auto& strings = tunings()[(size_t) tuning].midi;
        for (int i = 0; i < 6; ++i)
        {
            const auto box = stringBox (i);
            const auto current = i == shown.currentString;
            const auto targeted = i == target;
            const auto isDone = done[(size_t) i];
            g.setColour (current ? line1 : raised);
            g.fillRoundedRectangle (box, 8.0f);
            g.setColour (targeted ? accent : (current ? inkDim : line1));
            g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, targeted ? 1.5f : 1.0f);

            // The string's number at the top left, a check at the top right once it's been in tune.
            g.setFont (tabular (geist (Weight::medium, 11.0f)));
            g.setColour (inkFaint);
            g.drawText (juce::String (6 - i), box.reduced (10.0f, 8.0f).withHeight (14.0f), juce::Justification::centredLeft, false);
            if (targeted)
            {
                g.setColour (accent);
                g.setFont (geist (Weight::medium, 10.0f));
                g.drawText ("TARGET", box.reduced (10.0f, 8.0f).withHeight (14.0f), juce::Justification::centredRight, false);
            }
            else if (isDone)
            {
                const auto c = juce::Point<float> (box.getRight() - 16.0f, box.getY() + 15.0f);
                juce::Path tick;
                tick.startNewSubPath (c.x - 4.5f, c.y);
                tick.lineTo (c.x - 1.5f, c.y + 3.0f);
                tick.lineTo (c.x + 4.5f, c.y - 3.5f);
                g.setColour (accent);
                g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            // The note, large, and its octave beside it.
            const auto midi = strings[(size_t) i];
            const juce::String name (pitchNames[midi % 12]);
            const auto noteFont = geist (Weight::medium, 30.0f), octFont = geist (Weight::regular, 14.0f);
            const juce::String octave (midi / 12 - 1);
            const auto nw = textWidth (noteFont, name), ow = textWidth (octFont, octave);
            const auto x = box.getCentreX() - (nw + 2.0f + ow) * 0.5f;
            const auto baseline = box.getY() + 66.0f;
            g.setColour (isDone ? accent : (current ? ink : inkDim));
            drawAt (g, name, noteFont, x, baseline);
            g.setColour (inkFaint);
            drawAt (g, octave, octFont, x + nw + 2.0f, baseline);
        }
    }

    // ---- The controls ----------------------------------------------------------------------------------------
    {
        overlay::paintPanel (g, controlsPanel);
        auto in = controlsPanel.reduced (overlay::panelPadding + 4, overlay::panelPadding);
        overlay::paintGroupLabel (g, in.removeFromTop (16), "Tuning");
        overlay::paintGroupLabel (g, a4Area.withY (a4Area.getY() - 26).withHeight (16), "Reference and output");

        const auto a4 = state.getRawParameterValue ("tuner_a4")->load();
        const auto value = (std::abs (a4 - std::round (a4)) < 0.05f ? juce::String (juce::roundToInt (a4)) : juce::String (a4, 1)) + " Hz";
        g.setFont (geist (Weight::regular, 13.0f));
        g.setColour (inkDim);
        g.drawText ("A4", a4Area.withWidth (24), juce::Justification::centredLeft, false);
        g.setFont (tabular (geist (Weight::medium, 14.0f)));
        g.setColour (ink);
        g.drawText (value, a4Value, juce::Justification::centred, false);
    }
}

void TunerPage::resized()
{
    const auto w = getWidth();
    header = { 0, 0, w, overlay::headerHeight };
    closeButton->setBounds (overlay::closeBounds (header));

    displayArea = { 0, displayTop, w, displayHeight };
    noteArea = { 0, displayTop + 18, w, noteHeight };
    readArea = { 0, displayTop + 226, w, 24 };
    scaleArea = { (w - scaleWidth) / 2, displayTop + 270, scaleWidth, 84 };

    controlsPanel = { w - controlsWidth, lowerTop, controlsWidth, lowerHeight };
    stringsPanel = { 0, lowerTop, w - controlsWidth - space::m, lowerHeight };
    stringsArea = stringsPanel.reduced (overlay::panelPadding + 4, 0).withTrimmedTop (overlay::panelPadding + 16 + 12).withHeight (chipHeight);

    // The controls: the tunings (the pill) under their label; A4 with - and + and Mute output on the row under it.
    auto in = controlsPanel.reduced (overlay::panelPadding + 4, overlay::panelPadding);
    in.removeFromTop (16 + 8);
    const auto segWidth = juce::jmin (in.getWidth(), tuningChoice->getPreferredWidth());
    tuningChoice->setBounds (in.removeFromTop (Segmented::pillHeight).withWidth (segWidth));
    in.removeFromTop (16 + 26 - 16);
    auto row = in.removeFromBottom (controlHeight);
    a4Area = row.withWidth (24 + 8 + 28 + 64 + 28);
    a4Down->setBounds (a4Area.getX() + 32, row.getY(), 28, controlHeight);
    a4Value = { a4Down->getRight(), row.getY(), 64, controlHeight };
    a4Up->setBounds (a4Value.getRight(), row.getY(), 28, controlHeight);
    const auto muteWidth = mute->getPreferredWidth();
    mute->setBounds (row.getRight() - muteWidth + Switch::margin, row.getCentreY() - Switch::preferredHeight / 2, muteWidth, Switch::preferredHeight);
}

void TunerPage::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    for (int i = 0; i < 6; ++i)
        if (stringBox (i).contains (e.position))
        {
            clickString (i);
            return;
        }
}

} // namespace ui

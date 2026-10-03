// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TunerPage.h"

namespace ui
{

using namespace theme;

namespace
{
const juce::Identifier tunerTuningKey { "tunerTuning" }; // the chosen tuning (app view state)
const char* const pitchNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// The column (CSS #page-tuner): the note (10 + 190), the readout (6 + 17 + 30), the scale (70), the
// strings (34 + 54 + 34), the controls (18 + 24).
constexpr int noteTop = 10, noteHeight = 190, readHeight = 17, scaleWidth = 720, scaleHeight = 70, stringSize = 54, stringGap = 14;
constexpr int columnHeight = noteTop + noteHeight + 6 + readHeight + 30 + scaleHeight + 34 + stringSize + 34 + 1 + 18 + 24;

/// Draws text with its baseline at y (a CSS line box's baseline), left edge at x; returns its width.
float drawAt (juce::Graphics& g, const juce::String& s, const juce::FontOptions& f, float x, float baseline, float tracking = 0.0f)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (juce::Font (f), s, x, baseline);
    for (int i = 1; i < glyphs.getNumGlyphs(); ++i)
        glyphs.moveRangeOfGlyphs (i, 1, tracking * (float) i, 0.0f);
    glyphs.draw (g);
    return textWidth (f, s) + tracking * (float) s.length();
}

/// The baseline of a CSS line box `height` high for a font (the content area centred in it).
float baselineIn (const juce::FontOptions& f, float top, float height)
{
    const juce::Font font (f);
    return top + (height - (font.getAscent() + font.getDescent())) * 0.5f + font.getAscent();
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
    tuningChoice->onChange = [this] (int i) { setTuning (i); };
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

juce::Rectangle<float> TunerPage::stringCircle (int index) const
{
    const auto total = 6 * stringSize + 5 * stringGap;
    const auto x0 = (float) (getWidth() - total) * 0.5f;
    return { x0 + (float) (index * (stringSize + stringGap)), (float) stringsArea.getY(), (float) stringSize, (float) stringSize };
}

void TunerPage::paint (juce::Graphics& g)
{
    const auto noteColour = shown.inTune ? accent : (reading.hasReading ? ink : inkFaint);

    // The note: 180 px light, tracking -.04em, line height 1; the sharp (56 px, 22 px down) and the octave
    // (28 px, 22 px up from the bottom) beside it, 6 px apart (CSS .tn-note: flex, top-aligned).
    {
        const auto big = geist (Weight::light, 180.0f), acc = geist (Weight::light, 56.0f), oct = geist (Weight::light, 28.0f);
        const auto top = (float) noteArea.getY();
        const auto noteWidth = textWidth (big, shown.note) - 0.04f * 180.0f;
        const auto accWidth = shown.accidental.isEmpty() ? 0.0f : textWidth (acc, shown.accidental);
        const auto octWidth = textWidth (oct, shown.octave);
        auto x = (float) getWidth() * 0.5f - (noteWidth + 6.0f + accWidth + 6.0f + octWidth) * 0.5f;
        g.setColour (noteColour);
        drawAt (g, shown.note, big, x, baselineIn (big, top, 180.0f), -0.04f * 180.0f);
        x += noteWidth + 6.0f;
        g.setColour (inkDim);
        const juce::Font accFont (acc), octFont (oct);
        drawAt (g, shown.accidental, acc, x, top + 22.0f + accFont.getAscent());
        x += accWidth + 6.0f;
        g.setColour (inkFaint);
        drawAt (g, shown.octave, oct, x, top + (float) noteHeight - 22.0f - octFont.getDescent());
    }

    // The readout: "83.03 Hz", "Cents +13", the state; 14 px, 28 px apart, tabular figures.
    {
        const auto faint = tabular (geist (Weight::regular, 14.0f)), bold = tabular (geist (Weight::medium, 14.0f));
        const auto hzText = shown.hz + " Hz";
        const auto w1 = textWidth (faint, hzText), w2 = textWidth (faint, "Cents") + 6.0f + textWidth (bold, shown.cents), w3 = textWidth (faint, shown.state);
        auto x = (float) getWidth() * 0.5f - (w1 + 28.0f + w2 + 28.0f + w3) * 0.5f;
        const auto baseline = baselineIn (faint, (float) readArea.getY(), (float) readHeight);
        g.setColour (inkFaint);
        x += drawAt (g, hzText, faint, x, baseline) + 28.0f;
        x += drawAt (g, "Cents", faint, x, baseline) + 6.0f;
        g.setColour (reading.hasReading ? ink : inkFaint);
        x += drawAt (g, shown.cents, bold, x, baseline) + 28.0f;
        g.setColour (shown.inTune ? accent : inkDim);
        drawAt (g, shown.state, faint, x, baseline);
    }

    // The scale: the in-tune zone, ticks every 5 cents (longer at +-25, longest at 0), the labels, the needle.
    {
        const auto s = scaleArea.toFloat();
        const auto xOf = [&s] (double cents) { return s.getX() + (float) ((juce::jlimit (-50.0, 50.0, cents) + 50.0) / 100.0) * s.getWidth(); };
        if (shown.inTune)
        {
            g.setColour (accentDim);
            g.fillRoundedRectangle (juce::Rectangle<float> (xOf (0.0) - 22.0f, s.getY() + 6.0f, 44.0f, 40.0f), 4.0f);
        }
        for (int c = -50; c <= 50; c += 5)
        {
            const auto zero = c == 0, mid = c % 25 == 0;
            g.setColour (zero ? inkDim : mid ? inkFaint : line2);
            const auto top = zero ? 4.0f : mid ? 12.0f : 18.0f, height = zero ? 44.0f : mid ? 28.0f : 16.0f;
            g.fillRect (juce::Rectangle<float> (xOf (c), s.getY() + top, 1.0f, height));
            if (mid)
            {
                g.setColour (inkFaint);
                g.setFont (tabular (geist (Weight::regular, 11.0f)));
                g.drawText (c > 0 ? "+" + juce::String (c) : juce::String (c), juce::Rectangle<float> (xOf (c) - 30.0f, s.getY() + 54.0f, 60.0f, 14.0f),
                            juce::Justification::centredTop, false);
            }
        }
        if (reading.hasReading)
        {
            const auto x = xOf (shown.centsValue);
            g.setColour (shown.inTune ? accent : ink);
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 1.0f, s.getY(), 2.0f, 52.0f), 1.0f);
            g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ x, s.getY() - 1.0f }));
        }
    }

    // The strings: 54 px circles 14 apart, the note inside (16 px medium), the string's number under it.
    {
        const auto& strings = tunings()[(size_t) tuning].midi;
        for (int i = 0; i < 6; ++i)
        {
            const auto circle = stringCircle (i);
            const auto current = i == shown.currentString;
            const auto colour = current ? ink : done[(size_t) i] ? accent : inkDim;
            g.setColour (current ? ink : done[(size_t) i] ? accent : line2);
            g.drawEllipse (circle.reduced (0.5f), 1.0f);
            g.setColour (colour);
            g.setFont (geist (Weight::medium, 16.0f));
            g.drawText (pitchNames[strings[(size_t) i] % 12], circle, juce::Justification::centred, false);
            g.setColour (inkFaint);
            g.setFont (geist (Weight::regular, 11.0f));
            g.drawText (juce::String (6 - i), circle.withY (circle.getBottom() + 7.0f).withHeight (13.0f), juce::Justification::centred, false);
        }
    }

    // The controls' line, and A4's label and value.
    g.setColour (line1);
    g.fillRect (controlsArea.getX(), controlsArea.getY() - 18, controlsArea.getWidth(), 1);
    {
        const auto f = tabular (geist (Weight::regular, 13.0f));
        const auto a4 = state.getRawParameterValue ("tuner_a4")->load();
        const auto value = (std::abs (a4 - std::round (a4)) < 0.05f ? juce::String (juce::roundToInt (a4)) : juce::String (a4, 1)) + " Hz";
        g.setFont (f);
        g.setColour (inkDim);
        g.drawText ("A4", a4Area.withWidth (20), juce::Justification::centredLeft, false);
        g.setFont (tabular (geist (Weight::medium, 13.0f)));
        g.setColour (ink);
        g.drawText (value, juce::Rectangle<int> (a4Down->getRight() + 8, a4Area.getY(), 58, a4Area.getHeight()), juce::Justification::centred, false);
    }
}

void TunerPage::resized()
{
    columnTop = (getHeight() - columnHeight) / 2;
    auto y = columnTop + noteTop;
    noteArea = { 0, y, getWidth(), noteHeight };
    y += noteHeight + 6;
    readArea = { 0, y, getWidth(), readHeight };
    y += readHeight + 30;
    scaleArea = { (getWidth() - scaleWidth) / 2, y, scaleWidth, scaleHeight };
    y += scaleHeight + 34;
    stringsArea = { 0, y, getWidth(), stringSize };
    y += stringSize + 34 + 1 + 18;

    // The controls: tunings at the left, A4 in the middle, Mute output at the right of the 720 px row.
    controlsArea = { (getWidth() - scaleWidth) / 2, y, scaleWidth, 24 };
    const auto segWidth = tuningChoice->getPreferredWidth();
    tuningChoice->setBounds (controlsArea.getX(), controlsArea.getY() + 2, segWidth, Segmented::preferredHeight);
    const auto muteWidth = mute->getPreferredWidth();
    mute->setBounds (controlsArea.getRight() - muteWidth + Switch::margin, controlsArea.getCentreY() - Switch::preferredHeight / 2, muteWidth, Switch::preferredHeight);

    // A4, centred in the space between (CSS justify-content: space-between over three items).
    const auto a4Width = 20 + 8 + 24 + 8 + 58 + 8 + 24;
    const auto left = controlsArea.getX() + segWidth, right = controlsArea.getRight() - (muteWidth - 2 * Switch::margin);
    const auto a4X = left + (right - left - a4Width) / 2;
    a4Area = { a4X, controlsArea.getY(), a4Width, 24 };
    a4Down->setBounds (a4X + 28, controlsArea.getY(), 24, 24);
    a4Up->setBounds (a4X + 28 + 24 + 8 + 58 + 8, controlsArea.getY(), 24, 24);
}

void TunerPage::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    for (int i = 0; i < 6; ++i)
        if (stringCircle (i).contains (e.position))
        {
            clickString (i);
            return;
        }
}

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Overlay.h"

namespace ui
{

using namespace theme;

namespace
{
const juce::Identifier primaryProperty { "primaryButton" };
}

void setPrimary (juce::Component& button, bool primary)
{
    button.getProperties().set (primaryProperty, primary);
    button.repaint();
}

bool isPrimary (const juce::Component& button)
{
    return (bool) button.getProperties().getWithDefault (primaryProperty, false);
}

namespace overlay
{

float keycapWidth (const juce::String& key)
{
    if (key == "@left" || key == "@right")
        return 20.0f;
    return juce::jmax (20.0f, textWidth (geist (Weight::medium, 11.0f), key) + 12.0f);
}

float paintKeycap (juce::Graphics& g, const juce::String& key, float x, float centreY)
{
    const auto w = keycapWidth (key);
    const auto cap = juce::Rectangle<float> (x, centreY - 10.0f, w, 20.0f);
    g.setColour (raised);
    g.fillRoundedRectangle (cap, 4.0f);
    g.setColour (line2);
    g.drawRoundedRectangle (cap.reduced (0.5f), 4.0f, 1.0f);
    // The key's lower lip, as a keycap has.
    g.setColour (line1);
    g.fillRect (cap.reduced (3.0f, 0.0f).withTop (cap.getBottom() - 2.0f).withHeight (1.0f));
    g.setColour (inkDim);
    if (key == "@left" || key == "@right")
    {
        // The arrow keys, drawn (the interface font has no arrow glyphs).
        const auto c = cap.withTrimmedBottom (1.0f).getCentre();
        const auto d = key == "@left" ? -1.0f : 1.0f;
        juce::Path arrow;
        arrow.startNewSubPath (c.x - 4.0f * d, c.y);
        arrow.lineTo (c.x + 4.0f * d, c.y);
        arrow.startNewSubPath (c.x + 1.0f * d, c.y - 3.0f);
        arrow.lineTo (c.x + 4.0f * d, c.y);
        arrow.lineTo (c.x + 1.0f * d, c.y + 3.0f);
        g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return w;
    }
    g.setFont (geist (Weight::medium, 11.0f));
    g.drawText (key, cap.withTrimmedBottom (1.0f), juce::Justification::centred, false);
    return w;
}

void paintHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& about, const juce::String& keys)
{
    const auto titleFont = geist (Weight::semibold, 20.0f);
    const auto row = area.withHeight (32).toFloat();
    const auto tw = textWidth (titleFont, title);
    g.setFont (titleFont);
    g.setColour (ink);
    g.drawText (title, row.withWidth (tw + 2.0f), juce::Justification::centredLeft, false);

    // The keys, right to left from Close: each a keycap and its word.
    auto right = (float) closeBounds (area).getX() - 20.0f;
    if (keys.isNotEmpty())
    {
        const auto pairs = juce::StringArray::fromTokens (keys, ";", "");
        const auto words = geist (Weight::regular, 12.0f);
        for (int i = pairs.size(); --i >= 0;)
        {
            const auto key = pairs[i].upToFirstOccurrenceOf ("|", false, false);
            const auto word = pairs[i].fromFirstOccurrenceOf ("|", false, false);
            const auto ww = textWidth (words, word);
            g.setFont (words);
            g.setColour (inkFaint);
            g.drawText (word, juce::Rectangle<float> (right - ww - 1.0f, row.getY(), ww + 2.0f, row.getHeight()), juce::Justification::centredLeft, false);
            right -= ww + 6.0f + keycapWidth (key);
            paintKeycap (g, key, right, row.getCentreY());
            right -= 18.0f;
        }
    }

    if (about.isNotEmpty())
    {
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkFaint);
        const auto x = row.getX() + tw + 14.0f;
        g.drawFittedText (about, juce::Rectangle<float> (x, row.getY() + 2.0f, juce::jmax (0.0f, right - x - 12.0f), row.getHeight()).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.95f);
    }

    g.setColour (line1);
    g.fillRect (area.getX(), area.getY() + headerHeight - 9, area.getWidth(), 1);
}

juce::Rectangle<int> closeBounds (juce::Rectangle<int> header)
{
    return { header.getRight() - closeWidth, header.getY() + 2, closeWidth, controlHeight };
}

void paintPanel (juce::Graphics& g, juce::Rectangle<int> area, bool lit)
{
    const auto r = area.toFloat();
    g.setColour (panelBg);
    g.fillRoundedRectangle (r, radiusCard);
    g.setColour (lit ? accent.withAlpha (0.55f) : line1);
    g.drawRoundedRectangle (r.reduced (0.5f), radiusCard, 1.0f);
}

void paintStepBadge (juce::Graphics& g, juce::Point<float> centre, int number, Step state)
{
    const auto disc = juce::Rectangle<float> (22.0f, 22.0f).withCentre (centre);
    const auto f = tabular (geist (Weight::semibold, 12.0f));
    switch (state)
    {
        case Step::waiting:
            g.setColour (line2);
            g.drawEllipse (disc.reduced (0.5f), 1.0f);
            g.setFont (f);
            g.setColour (inkFaint);
            g.drawText (juce::String (number), disc, juce::Justification::centred, false);
            break;
        case Step::next:
            g.setColour (raised);
            g.fillEllipse (disc);
            g.setColour (inkDim);
            g.drawEllipse (disc.reduced (0.5f), 1.0f);
            g.setFont (f);
            g.setColour (ink);
            g.drawText (juce::String (number), disc, juce::Justification::centred, false);
            break;
        case Step::active:
            g.setColour (accent.withAlpha (0.18f));
            g.fillEllipse (disc.expanded (4.0f));
            g.setColour (accent);
            g.fillEllipse (disc);
            g.setFont (f);
            g.setColour (onAccent);
            g.drawText (juce::String (number), disc, juce::Justification::centred, false);
            break;
        case Step::done:
        {
            g.setColour (accentDim);
            g.fillEllipse (disc);
            g.setColour (accent);
            g.drawEllipse (disc.reduced (0.5f), 1.0f);
            juce::Path tick;
            tick.startNewSubPath (centre.x - 4.5f, centre.y + 0.5f);
            tick.lineTo (centre.x - 1.5f, centre.y + 3.5f);
            tick.lineTo (centre.x + 4.5f, centre.y - 3.5f);
            g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }
    }
}

juce::Rectangle<int> paintStepHeading (juce::Graphics& g, juce::Rectangle<int> inside, int number, const juce::String& title, Step state,
                                       const juce::String& stateText)
{
    auto row = inside.removeFromTop (22);
    paintStepBadge (g, { (float) row.getX() + 11.0f, (float) row.getCentreY() }, number, state);
    row.removeFromLeft (22 + 10);
    const auto titleFont = geist (Weight::medium, 14.0f);
    const auto tw = juce::roundToInt (textWidth (titleFont, title)) + 2;
    g.setFont (titleFont);
    g.setColour (state == Step::waiting ? inkDim : ink);
    g.drawText (title, row.removeFromLeft (tw), juce::Justification::centredLeft, false);
    if (stateText.isNotEmpty())
    {
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (state == Step::active ? accent : inkFaint);
        g.drawFittedText (stateText, row.withTrimmedLeft (12), juce::Justification::centredRight, 1, 0.9f);
    }
    inside.removeFromTop (stepHeadingHeight - 22);
    return inside;
}

void paintNote (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, bool strong)
{
    const auto r = area.toFloat();
    g.setColour (raised);
    g.fillRoundedRectangle (r, radiusControl);
    g.setColour (strong ? inkDim : line2);
    g.drawRoundedRectangle (r.reduced (0.5f), radiusControl, 1.0f);
    g.setColour (strong ? ink : inkDim);
    g.fillRoundedRectangle (juce::Rectangle<float> (r.getX() + 6.0f, r.getY() + 7.0f, 2.0f, r.getHeight() - 14.0f), 1.0f);
    // The whole text, wrapped by word: at 12 px, or at 11 when that doesn't fit (an error is shown in full).
    const auto box = area.reduced (16, 5).withTrimmedRight (-6).toFloat();
    juce::TextLayout layout;
    for (const auto size : { 12.0f, 11.0f })
    {
        juce::AttributedString words;
        words.append (text, geist (Weight::regular, size), strong ? ink : inkDim);
        words.setWordWrap (juce::AttributedString::byWord);
        words.setJustification (juce::Justification::centredLeft);
        layout.createLayout (words, box.getWidth());
        if (layout.getHeight() <= box.getHeight())
            break;
    }
    layout.draw (g, box.withSizeKeepingCentre (box.getWidth(), juce::jmin (box.getHeight(), layout.getHeight())));
}

void paintGroupLabel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
{
    const auto f = geist (Weight::medium, 10.5f);
    juce::GlyphArrangement glyphs;
    const auto upper = text.toUpperCase();
    const juce::Font font (f);
    glyphs.addLineOfText (font, upper, (float) area.getX(), (float) area.getCentreY() + (font.getAscent() - font.getDescent()) * 0.5f);
    for (int i = 1; i < glyphs.getNumGlyphs(); ++i)
        glyphs.moveRangeOfGlyphs (i, 1, 0.8f * (float) i, 0.0f);
    g.setColour (inkFaint);
    glyphs.draw (g);
}

} // namespace overlay
} // namespace ui

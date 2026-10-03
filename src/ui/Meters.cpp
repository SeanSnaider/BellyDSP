// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Meters.h"

namespace ui
{

using namespace theme;

namespace
{
float toDb (float linear)
{
    return linear > 1.0e-5f ? 20.0f * std::log10 (linear) : -100.0f;
}

} // namespace

// ---- LevelMeter ----------------------------------------------------------------------------------

LevelMeter::LevelMeter (juce::String title, int numChannels) : label (std::move (title)), channels ((size_t) juce::jmax (1, numChannels))
{
    setTooltip ("Peak level, -60 to 0 dBFS. A bright cap at the end means a clip; click to clear it.");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void LevelMeter::push (const float* peaks, double seconds)
{
    for (size_t c = 0; c < channels.size(); ++c)
    {
        auto& ch = channels[c];
        const auto db = toDb (peaks[c]);

        // Instant rise, a steady fall.
        ch.shownDb = juce::jmax (db, ch.shownDb - (float) (meterFallDbPerSecond * seconds));

        // The peak hold: a new peak resets it; otherwise it stays 1.5 s and then follows the bar down.
        if (db >= ch.heldDb)
        {
            ch.heldDb = db;
            ch.heldFor = 0.0;
        }
        else if ((ch.heldFor += seconds) > peakHoldSeconds)
        {
            ch.heldDb = ch.shownDb;
        }

        ch.clipped = ch.clipped || peaks[c] >= clipLevel;
    }

    // Repaint only when what's on screen would change (a quarter dB, or the clip light).
    bool changed = drawn.size() != channels.size();
    for (size_t c = 0; ! changed && c < channels.size(); ++c)
        changed = std::abs (channels[c].shownDb - drawn[c].shownDb) > 0.25f || std::abs (channels[c].heldDb - drawn[c].heldDb) > 0.25f
                  || channels[c].clipped != drawn[c].clipped;
    if (changed)
    {
        drawn = channels;
        repaint();
    }
}

bool LevelMeter::isClipped() const
{
    for (const auto& c : channels)
        if (c.clipped)
            return true;
    return false;
}

void LevelMeter::resetClip()
{
    for (auto& c : channels)
        c.clipped = false;
    drawn.clear();
    repaint();
}

void LevelMeter::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        resetClip();
}

int LevelMeter::getPreferredWidth() const
{
    return juce::roundToInt (std::ceil (textWidth (geist (Weight::regular, 11.0f), label))) + 8 + 56;
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setFont (geist (Weight::regular, 11.0f));
    g.setColour (inkFaint);
    g.drawText (label, area, juce::Justification::centredLeft, false);

    float shown = -100.0f, held = -100.0f;
    for (const auto& c : channels)
    {
        shown = juce::jmax (shown, c.shownDb);
        held = juce::jmax (held, c.heldDb);
    }
    const auto fraction = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db - meterFloorDb) / -meterFloorDb); };

    const auto bar = area.removeFromRight (56.0f).withSizeKeepingCentre (56.0f, 2.0f);
    g.setColour (line2);
    g.fillRoundedRectangle (bar, 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * fraction (shown)), 1.0f);
    if (held > meterFloorDb)
    {
        g.setColour (inkDim);
        g.fillRect (juce::Rectangle<float> (bar.getX() + bar.getWidth() * fraction (held) - 1.0f, bar.getY(), 1.0f, bar.getHeight()));
    }
    if (isClipped())
    {
        g.setColour (ink);
        g.fillRect (bar.withLeft (bar.getRight() - 3.0f));
    }
}

// ---- ReductionMeter ------------------------------------------------------------------------------

void ReductionMeter::setReduction (float db)
{
    db = juce::jmax (0.0f, db);
    if (std::abs (db - reductionDb) > 0.1f || (db == 0.0f) != (reductionDb == 0.0f))
    {
        reductionDb = db;
        repaint();
    }
}

void ReductionMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    const auto readout = area.removeFromBottom (captionHeight * 2 + 4);
    const auto column = area.withSizeKeepingCentre (4, area.getHeight()).toFloat();

    g.setColour (line2);
    g.fillRoundedRectangle (column, 2.0f);
    const auto depth = juce::jlimit (0.0f, 1.0f, reductionDb / range);
    g.setColour (accent);
    g.fillRoundedRectangle (column.withHeight (column.getHeight() * depth), 2.0f);

    // Ticks every 6 dB.
    g.setColour (line2);
    for (float db = 6.0f; db < range; db += 6.0f)
        g.fillRect (juce::Rectangle<float> (column.getRight() + 3.0f, column.getY() + column.getHeight() * db / range, 4.0f, 1.0f));

    g.setFont (geist (Weight::regular, 11.0f));
    g.setColour (inkFaint);
    g.drawText ("GR", readout.withHeight (captionHeight), juce::Justification::centred, false);
    g.setFont (font (Text::value));
    g.setColour (ink);
    g.drawText (reductionDb >= range ? juce::String (">") + juce::String (juce::roundToInt (range)) : juce::String (-reductionDb, 1),
                readout.withTrimmedTop (captionHeight), juce::Justification::centred, false);
}

// ---- CpuMeter ------------------------------------------------------------------------------------

void CpuMeter::setLoad (float percent)
{
    load = juce::jmax (0.0f, percent);
    if (const auto rounded = juce::roundToInt (load); rounded != shown)
    {
        shown = rounded;
        repaint();
    }
}

void CpuMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setFont (tabular (geist (Weight::regular, 11.0f)));
    g.setColour (inkFaint);
    g.drawText ("CPU " + juce::String (juce::roundToInt (load)) + "%", area, juce::Justification::centredLeft, false);

    const auto bar = area.removeFromRight (56.0f).withSizeKeepingCentre (56.0f, 2.0f);
    g.setColour (line2);
    g.fillRoundedRectangle (bar, 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, load / 100.0f)), 1.0f);
}

} // namespace ui

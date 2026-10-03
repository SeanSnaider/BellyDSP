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

/// The meter's colour zones (UI_DESIGN "Colour"): good to -12 dBFS, warn to -3, error above.
juce::Colour zoneColour (float db)
{
    return db < -12.0f ? good : (db < -3.0f ? warn : error);
}

juce::String dbText (float db)
{
    return db <= meterFloorDb ? juce::String ("-inf") : juce::String (db, 1);
}
} // namespace

// ---- LevelMeter ----------------------------------------------------------------------------------

LevelMeter::LevelMeter (juce::String title, int numChannels) : label (std::move (title)), channels ((size_t) juce::jmax (1, numChannels))
{
    setTooltip ("Peak level. The red light means a clip; click to clear it.");
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

void LevelMeter::paint (juce::Graphics& g)
{
    // Text on the left (the label over the held peak), bars on the right.
    auto area = getLocalBounds();
    const auto barWidth = 6, barGap = 2;
    const auto barsWidth = (int) channels.size() * barWidth + ((int) channels.size() - 1) * barGap;
    auto bars = area.removeFromRight (barsWidth).toFloat();
    area.removeFromRight (6);

    float held = -100.0f;
    for (const auto& c : channels)
        held = juce::jmax (held, c.heldDb);

    g.setFont (font ("Semibold", 11.0f));
    g.setColour (textDim);
    g.drawText (label, area.removeFromTop (area.getHeight() / 2), juce::Justification::bottomRight, false);
    g.setFont (font (Text::value));
    g.setColour (isClipped() ? error : text);
    g.drawText (dbText (held), area, juce::Justification::topRight, false);

    const auto clipHeight = 4.0f;
    const auto yOf = [&bars, clipHeight] (float db)
    {
        const auto t = juce::jlimit (0.0f, 1.0f, (db - meterFloorDb) / -meterFloorDb);
        return bars.getBottom() - t * (bars.getHeight() - clipHeight - 2.0f);
    };

    for (size_t c = 0; c < channels.size(); ++c)
    {
        const auto& ch = channels[c];
        const auto bar = bars.withX (bars.getX() + (float) c * (float) (barWidth + barGap)).withWidth ((float) barWidth);
        const auto light = bar.withHeight (clipHeight);
        const auto column = bar.withTrimmedTop (clipHeight + 2.0f);

        g.setColour (background);
        g.fillRoundedRectangle (column, 1.5f);

        // The bar in its zones: green below -12, amber to -3, red above.
        const auto top = yOf (ch.shownDb);
        for (const auto& [from, to] : { std::pair<float, float> { meterFloorDb, -12.0f }, { -12.0f, -3.0f }, { -3.0f, 0.0f } })
        {
            const auto y0 = yOf (from), y1 = juce::jmax (yOf (to), top);
            if (y1 < y0)
            {
                g.setColour (zoneColour (from));
                g.fillRect (juce::Rectangle<float> (column.getX(), y1, column.getWidth(), y0 - y1));
            }
        }

        if (ch.heldDb > meterFloorDb)
        {
            g.setColour (zoneColour (ch.heldDb));
            g.fillRect (juce::Rectangle<float> (column.getX(), yOf (ch.heldDb) - 1.0f, column.getWidth(), 2.0f));
        }

        g.setColour (ch.clipped ? error : outline);
        g.fillRoundedRectangle (light, 1.0f);
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
    const auto column = area.withSizeKeepingCentre (10, area.getHeight()).toFloat();

    g.setColour (background);
    g.fillRoundedRectangle (column, 2.0f);
    const auto depth = juce::jlimit (0.0f, 1.0f, reductionDb / range);
    g.setColour (accent);
    g.fillRoundedRectangle (column.withHeight (column.getHeight() * depth), 2.0f);

    // Ticks every 6 dB.
    g.setColour (outline.brighter (0.2f));
    for (float db = 6.0f; db < range; db += 6.0f)
        g.fillRect (juce::Rectangle<float> (column.getRight() + 2.0f, column.getY() + column.getHeight() * db / range, 4.0f, 1.0f));

    g.setFont (font ("Semibold", 11.0f));
    g.setColour (textDim);
    g.drawText ("GR", readout.withHeight (captionHeight), juce::Justification::centred, false);
    g.setFont (font (Text::value));
    g.setColour (text);
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
    auto area = getLocalBounds();
    auto top = area.removeFromTop (area.getHeight() / 2);
    g.setFont (font ("Semibold", 11.0f));
    g.setColour (textDim);
    g.drawText ("CPU", top, juce::Justification::bottomLeft, false);
    g.setFont (font (Text::value));
    g.setColour (load < 50.0f ? text : (load < 80.0f ? warn : error));
    g.drawText (juce::String (juce::roundToInt (load)) + "%", top, juce::Justification::bottomRight, false);

    // The bar: green under half the deadline, amber to 80%, red beyond.
    const auto bar = area.withTrimmedTop (4).withHeight (4).toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (load < 50.0f ? good : (load < 80.0f ? warn : error));
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, load / 100.0f)), 2.0f);
}

} // namespace ui

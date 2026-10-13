// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Placeholder pedal art (a plain enclosure per block) so the main screen can be built against the interface in
// PedalArt.h; the real enclosures replace this file.

#include "PedalArt.h"

#include <map>

namespace ui
{

bool isPedal (BlockId id)
{
    const auto s = info (id).section;
    return s == Section::pre || s == Section::post || id == BlockId::gateB;
}

juce::Point<float> pedalSize (BlockId) { return { 96.0f, 150.0f }; }

juce::Image renderPedal (BlockId id, float scale)
{
    const auto size = pedalSize (id);
    juce::Image image (juce::Image::ARGB, juce::roundToInt (size.x * scale), juce::roundToInt (size.y * scale), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));
    const juce::Rectangle<float> body (0.0f, 0.0f, size.x, size.y);
    g.setColour (juce::Colour (0xff2a2a2a));
    g.fillRoundedRectangle (body.reduced (2.0f), 8.0f);
    g.setColour (juce::Colour (0xff9a9a9a));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (info (id).name, body.withTrimmedTop (size.y - 34.0f), juce::Justification::centred);
    return image;
}

void drawPedal (juce::Graphics& g, juce::Rectangle<float> box, BlockId id, const PedalState& state)
{
    static std::map<std::pair<int, int>, juce::Image> cache; // message thread only
    const auto scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    auto& image = cache[{ (int) id, juce::roundToInt (scale * 100.0f) }];
    if (! image.isValid())
        image = renderPedal (id, scale);
    const auto size = pedalSize (id);
    const auto fit = box.withSizeKeepingCentre (juce::jmin (box.getWidth(), box.getHeight() * size.x / size.y),
                                                juce::jmin (box.getHeight(), box.getWidth() * size.y / size.x));
    g.drawImage (image, fit);
    const auto led = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ fit.getCentreX(), fit.getY() + fit.getHeight() * 0.12f });
    g.setColour (state.on ? juce::Colour (0xffff8a00) : juce::Colour (0xff3a3a3a));
    g.fillEllipse (led);
    if (state.selected || state.hovered)
    {
        g.setColour (state.selected ? juce::Colour (0xffff8a00) : juce::Colour (0x40ffffff));
        g.drawRoundedRectangle (fit.expanded (3.0f), 10.0f, state.selected ? 1.5f : 1.0f);
    }
}

} // namespace ui

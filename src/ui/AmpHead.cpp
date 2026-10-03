// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AmpHead.h"

namespace ui
{

using namespace theme;

const char* materialName (Material material)
{
    switch (material)
    {
        case Material::ember:    return "Ember";
        case Material::monolith: return "Monolith";
        case Material::glass:    break;
    }
    return "Glass";
}

Skin skinFor (Material material)
{
    switch (material)
    {
        case Material::ember:    return Skin::ember;
        case Material::monolith: return Skin::monolith;
        case Material::glass:    break;
    }
    return Skin::glass;
}

namespace
{
using Pattern = std::function<juce::Colour (float x, float y)>; // CSS pixels from the element's top-left

/// A vertical CSS linear-gradient (top to bottom) at y in an element `height` high.
juce::Colour vertical (juce::Colour top, juce::Colour bottom, float y, float height)
{
    return top.interpolatedWith (bottom, juce::jlimit (0.0f, 1.0f, y / height));
}

/// Source-over: `over` (with its alpha) on an opaque `under`.
juce::Colour blend (juce::Colour under, juce::Colour over)
{
    return under.overlaidWith (over);
}

/// The CSS dot texture: radial-gradient(circle at 1px 1px, colour 1px, transparent 1.5px) on a cell x cell
/// grid: full strength within 1 px of the cell's (1, 1), fading to nothing at 1.5 px.
float dot (float x, float y, float cell)
{
    const auto u = std::fmod (x, cell) - 1.0f, v = std::fmod (y, cell) - 1.0f;
    const auto d = std::sqrt (u * u + v * v);
    return juce::jlimit (0.0f, 1.0f, (1.5f - d) / 0.5f);
}

/// The standard normal's tail, for the inset shadows' blur (a CSS blur radius r is a Gaussian of sigma r / 2).
float tail (float t)
{
    return 0.5f * std::erfc (t / std::sqrt (2.0f));
}

/// Renders a pattern over `area` (CSS pixels) into an image at `scale`, sampling each device pixel's centre.
juce::Image patternImage (juce::Rectangle<float> area, float scale, const Pattern& pattern)
{
    const auto w = juce::jmax (1, juce::roundToInt (area.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt (area.getHeight() * scale));
    juce::Image image (juce::Image::ARGB, w, h, false);
    juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            data.setPixelColour (i, j, pattern (((float) i + 0.5f) / scale, ((float) j + 0.5f) / scale));
    return image;
}

/// Fills a rounded box with a pattern (the path clips it, antialiased).
void fillPattern (juce::Graphics& g, juce::Rectangle<float> box, float radius, float scale, const Pattern& pattern)
{
    juce::Path shape;
    shape.addRoundedRectangle (box, radius);
    const juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (shape);
    g.setOpacity (1.0f); // (drawImage takes the current colour's alpha as its opacity)
    g.drawImage (patternImage (box, scale, pattern), box);
}

/// Text as a path with CSS letter-spacing (added after every letter, the last included), centred in `box`
/// the way CSS centres it (the trailing spacing counts towards the width), with `padLeft` / `padRight` for
/// a padded element.
juce::Path textPath (const juce::String& text, const juce::FontOptions& font, float letterSpacing, juce::Rectangle<float> box)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (juce::Font (font), text, 0.0f, 0.0f);
    for (int i = 1; i < glyphs.getNumGlyphs(); ++i)
        glyphs.moveRangeOfGlyphs (i, 1, letterSpacing * (float) i, 0.0f);
    juce::Path p;
    glyphs.createPath (p);

    // Centre the advance box (the glyphs' run plus the trailing spacing) horizontally, and the font's
    // ascent-to-descent box vertically, as a CSS line box does.
    const auto f = juce::Font (font);
    const auto run = juce::GlyphArrangement::getStringWidth (f, text) + letterSpacing * (float) text.length();
    const auto x = box.getCentreX() - run * 0.5f;
    const auto y = box.getCentreY() + (f.getAscent() - f.getDescent()) * 0.5f;
    p.applyTransform (juce::AffineTransform::translation (x, y));
    return p;
}

void drawBadge (juce::Graphics& g, Material material, juce::Rectangle<float> grille)
{
    switch (material)
    {
        case Material::glass:
        {
            // Geist 300, 46 px, tracking .08em, light chrome; text-shadow 0 1px 0 white .3, 0 2px 4px black .6.
            const auto p = textPath ("Glass", geist (Weight::light, 46.0f), 0.08f * 46.0f, grille);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 4, { 0, 2 }).drawForPath (g, p);
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 1.0f));
            g.setColour (juce::Colour (0xffe4e6e5));
            g.fillPath (p);
            break;
        }
        case Material::ember:
        {
            // Fraunces italic 600, 52 px, gold; text-shadow 0 2px 0 black .6.
            const auto p = textPath ("Ember", fraunces (52.0f), 0.0f, grille);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 2.0f));
            g.setColour (juce::Colour (0xffd9b878));
            g.fillPath (p);
            break;
        }
        case Material::monolith:
        {
            // Geist 600, 24 px, tracking .28em, on a dark plate (padding 10 22 10 30, radius 4) with a 1 px
            // #262928 ring outside and a 2 px emerald bar along its bottom inside.
            const auto f = geist (Weight::semibold, 24.0f);
            const auto spacing = 0.28f * 24.0f;
            const auto run = textWidth (f, "Monolith") + spacing * 8.0f;
            const auto lineHeight = juce::Font (f).getAscent() + juce::Font (f).getDescent();
            const auto plate = juce::Rectangle<float> (run + 52.0f, lineHeight + 20.0f).withCentre (grille.getCentre());
            g.setColour (juce::Colour (0xff262928));
            g.fillRoundedRectangle (plate.expanded (1.0f), 5.0f);
            g.setColour (juce::Colour (0xff0d0e0e));
            g.fillRoundedRectangle (plate, 4.0f);
            {
                juce::Path shape;
                shape.addRoundedRectangle (plate, 4.0f);
                const juce::Graphics::ScopedSaveState saved (g);
                g.reduceClipRegion (shape);
                g.setColour (accent);
                g.fillRect (plate.withTop (plate.getBottom() - 2.0f));
            }
            const auto textBox = plate.withTrimmedLeft (30.0f).withTrimmedRight (22.0f).withTrimmedTop (10.0f).withTrimmedBottom (10.0f);
            const auto p = textPath ("Monolith", f, spacing, textBox.withWidth (run).withX (textBox.getX()));
            g.setColour (juce::Colour (0xffcfd6d3));
            g.fillPath (p);
            break;
        }
    }
}
} // namespace

juce::Image AmpHead::render (Material material, float scale)
{
    const auto bounds = juce::Rectangle<int> (0, 0, margin.getLeft() + headWidth + margin.getRight(), margin.getTop() + headHeight + margin.getBottom());
    juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) bounds.getWidth() * scale), juce::roundToInt ((float) bounds.getHeight() * scale), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));

    const auto head = headBox().toFloat();
    juce::Path body;
    body.addRoundedRectangle (head, radiusHead);

    // The shadows: 0 24px 40px black .55 (all three), and 0 2px 0 black .6 (the tolex heads).
    juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 40, { 0, 24 }).drawForPath (g, body);
    if (material != Material::monolith)
    {
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillPath (body, juce::AffineTransform::translation (0.0f, 2.0f));
    }

    // The handle: 150 x 14 centred above, rounded on top, with a faint highlight along its top edge.
    {
        const auto handle = juce::Rectangle<float> (head.getCentreX() - 75.0f, head.getY() - 14.0f, 150.0f, 14.0f);
        juce::Path p;
        p.addRoundedRectangle (handle.getX(), handle.getY(), handle.getWidth(), handle.getHeight(), 8.0f, 8.0f, true, true, false, false);
        g.setColour (juce::Colour (0xff0d0e0e));
        g.fillPath (p);
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (p);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (handle.withHeight (1.0f));
    }

    // The feet: 46 x 8, 40 px in from each side, under the bottom edge.
    for (const auto x : { head.getX() + 40.0f, head.getRight() - 40.0f - 46.0f })
    {
        juce::Path p;
        p.addRoundedRectangle (x, head.getBottom(), 46.0f, 8.0f, 4.0f, 4.0f, false, false, true, true);
        g.setColour (juce::Colour (0xff080909));
        g.fillPath (p);
    }

    // The body.
    const auto h = head.getHeight();
    switch (material)
    {
        case Material::glass:
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xffe4e1d9), juce::Colour (0xffc9c5ba), y, h);
                return blend (base, juce::Colours::black.withAlpha (0.07f * dot (x, y, 3.0f)));
            });
            break;
        case Material::ember:
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff55212a), juce::Colour (0xff3c1519), y, h);
                return blend (base, juce::Colours::black.withAlpha (0.28f * dot (x, y, 4.0f)));
            });
            break;
        case Material::monolith:
            fillPattern (g, head, radiusHead, scale, [h] (float, float y) { return vertical (juce::Colour (0xff1b1c1c), juce::Colour (0xff121313), y, h); });
            g.setColour (juce::Colour (0xff2a2d2c));
            g.drawRoundedRectangle (head.reduced (0.5f), radiusHead - 0.5f, 1.0f); // inset 0 0 0 1px
            break;
    }
    {
        // inset 0 1px 0 white (.12 on the tolex, .08 on the monolith): the body less itself moved down 1 px
        // (an even-odd pair), clipped to the body, is the 1 px highlight along the top inside edge.
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (body);
        juce::Path rim = body;
        rim.addPath (body, juce::AffineTransform::translation (0.0f, 1.0f));
        rim.setUsingNonZeroWinding (false);
        g.setColour (juce::Colours::white.withAlpha (material == Material::monolith ? 0.08f : 0.12f));
        g.fillPath (rim);
    }

    // The control panel.
    const auto panel = panelBox();
    const auto ph = panel.getHeight();
    switch (material)
    {
        case Material::glass:
            // Brushed: a 1 px white .18 line every 3 px over the silver gradient.
            fillPattern (g, panel, radiusPanel, scale, [ph] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xffd3d6d4), juce::Colour (0xffb4b8b6), y, ph);
                return std::fmod (x, 3.0f) < 1.0f ? blend (base, juce::Colours::white.withAlpha (0.18f)) : base;
            });
            break;
        case Material::ember:
        {
            // Near-black with the gold piping: inset 2px gold, then 2px of the panel, then 1px gold at .5.
            fillPattern (g, panel, radiusPanel, scale, [ph] (float, float y) { return vertical (juce::Colour (0xff211918), juce::Colour (0xff151010), y, ph); });
            g.setColour (juce::Colour (0xffa88a52));
            g.drawRoundedRectangle (panel.reduced (1.0f), radiusPanel - 1.0f, 2.0f);
            g.setColour (juce::Colour (0xff151010));
            g.drawRoundedRectangle (panel.reduced (3.0f), radiusPanel - 3.0f, 2.0f);
            g.setColour (juce::Colour (0x80a88a52));
            g.drawRoundedRectangle (panel.reduced (4.5f), radiusPanel - 4.5f, 1.0f);
            break;
        }
        case Material::monolith:
            g.setColour (juce::Colour (0xff0d0e0e));
            g.fillRoundedRectangle (panel, radiusPanel);
            g.setColour (juce::Colour (0xff262928));
            g.drawRoundedRectangle (panel.reduced (0.5f), radiusPanel - 0.5f, 1.0f);
            break;
    }

    // The grille, with its inset shadow (0 2px 6px black, .6; .8 on the monolith): darkest along the top
    // edge (the shadow is cast 2 px down), lighter along the sides, least at the bottom.
    const auto grille = grilleBox();
    const auto gw = grille.getWidth(), gh = grille.getHeight();
    const auto insetAlpha = material == Material::monolith ? 0.8f : 0.6f;
    const auto inset = [gw, gh, insetAlpha] (float x, float y)
    {
        constexpr float sigma = 3.0f;
        const auto top = tail ((y - 2.0f) / sigma), bottom = tail ((gh - y + 2.0f) / sigma);
        const auto left = tail (x / sigma), right = tail ((gw - x) / sigma);
        return juce::Colours::black.withAlpha (insetAlpha * juce::jmin (1.0f, top + bottom + left + right));
    };
    switch (material)
    {
        case Material::glass:
            // repeating-linear-gradient(45deg, #2b2e2c 0 2px, #222423 2px 4px): along the direction (1, -1)/sqrt 2.
            fillPattern (g, grille, radiusPanel, scale, [inset, gh] (float x, float y)
            {
                const auto t = (x - (y - gh)) * 0.70710678f;
                const auto base = std::fmod (std::fmod (t, 4.0f) + 4.0f, 4.0f) < 2.0f ? juce::Colour (0xff2b2e2c) : juce::Colour (0xff222423);
                return blend (base, inset (x, y));
            });
            break;
        case Material::ember:
            // Basketweave: 3 px columns of two browns, crossed by 3 px bands of black .25 every 6 px (from the bottom).
            fillPattern (g, grille, radiusPanel, scale, [inset, gh] (float x, float y)
            {
                auto base = std::fmod (x, 6.0f) < 3.0f ? juce::Colour (0xff3d322b) : juce::Colour (0xff2d2520);
                if (std::fmod (gh - y, 6.0f) < 3.0f)
                    base = blend (base, juce::Colours::black.withAlpha (0.25f));
                return blend (base, inset (x, y));
            });
            break;
        case Material::monolith:
            // Slot vents: 5 px of #0a0b0b, 4 px of #1d1f1f, from the bottom.
            fillPattern (g, grille, radiusPanel, scale, [inset, gh] (float x, float y)
            {
                const auto base = std::fmod (gh - y, 9.0f) < 5.0f ? juce::Colour (0xff0a0b0b) : juce::Colour (0xff1d1f1f);
                return blend (base, inset (x, y));
            });
            break;
    }

    drawBadge (g, material, grille);
    return image;
}

AmpHead::AmpHead()
{
    setInterceptsMouseClicks (false, false);
}

void AmpHead::setMaterial (Material m)
{
    if (m != material)
    {
        material = m;
        repaint();
    }
}

void AmpHead::paint (juce::Graphics& g)
{
    // Re-rendered only when the scale it's shown at or the material changes (a window resize, a slot switch).
    const auto scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (! cached.isValid() || std::abs (scale - cachedScale) > 0.01f || material != cachedMaterial)
    {
        cached = render (material, scale);
        cachedScale = scale;
        cachedMaterial = material;
        ++renders;
    }
    g.drawImage (cached, getLocalBounds().toFloat());
}

// ---- The pilot jewel -----------------------------------------------------------------------------

void PilotJewel::setLit (bool shouldBeLit)
{
    if (lit != shouldBeLit)
    {
        lit = shouldBeLit;
        repaint();
    }
}

void PilotJewel::paint (juce::Graphics& g)
{
    // 18 px: radial-gradient(circle at 35% 35%, #d7fbe9, emerald 45%, #0e5c40), a 12 px emerald glow at .55,
    // and a 3 px ring of black .35. Dark (no capture): the same in greys, no glow.
    const auto jewel = juce::Rectangle<float> (18.0f, 18.0f).withCentre (getLocalBounds().toFloat().getCentre());
    juce::Path disc;
    disc.addEllipse (jewel);
    // (CSS draws the first shadow on top: the glow over the ring.)
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (jewel.expanded (3.0f));
    if (lit)
        juce::DropShadow (accent.withAlpha (0.55f), 12, {}).drawForPath (g, disc);

    const juce::Point<float> highlight { jewel.getX() + 0.35f * 18.0f, jewel.getY() + 0.35f * 18.0f };
    const auto radius = highlight.getDistanceFrom (jewel.getBottomRight());
    juce::ColourGradient gradient (lit ? juce::Colour (0xffd7fbe9) : juce::Colour (0xff4d5351), highlight,
                                   lit ? juce::Colour (0xff0e5c40) : juce::Colour (0xff0b0c0c), highlight.translated (radius, 0.0f), true);
    gradient.addColour (0.45, lit ? accent : juce::Colour (0xff1f2322));
    g.setGradientFill (gradient);
    g.fillEllipse (jewel);
}

// ---- The mini heads ------------------------------------------------------------------------------

void drawMiniHead (juce::Graphics& g, juce::Rectangle<float> box, Material material)
{
    // 30 x 20, radius 3, a 1 px white .08 ring; a 5 px panel bar 3 px in from the sides.
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRoundedRectangle (box.expanded (1.0f), radiusMini + 1.0f);
    const auto bodyColour = material == Material::glass ? juce::Colour (0xffd9d6cd) : material == Material::ember ? juce::Colour (0xff4b1d1f) : juce::Colour (0xff161717);
    g.setColour (bodyColour);
    g.fillRoundedRectangle (box, radiusMini);

    const auto bar = juce::Rectangle<float> (box.getX() + 3.0f, box.getY() + (material == Material::monolith ? 4.0f : 3.0f), box.getWidth() - 6.0f, 5.0f);
    switch (material)
    {
        case Material::glass:
            g.setColour (juce::Colour (0xffb9bcba));
            g.fillRoundedRectangle (bar, 1.0f);
            break;
        case Material::ember:
            g.setColour (juce::Colour (0xffa88a52));
            g.fillRoundedRectangle (bar.expanded (1.0f), 2.0f);
            g.setColour (juce::Colour (0xff1c1514));
            g.fillRoundedRectangle (bar, 1.0f);
            break;
        case Material::monolith:
            g.setColour (juce::Colour (0xff0c0d0d));
            g.fillRoundedRectangle (bar, 1.0f);
            g.setColour (accent);
            g.fillRect (bar.withTop (bar.getBottom() - 1.0f));
            break;
    }
}

} // namespace ui

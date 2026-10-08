// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AmpHead.h"

#include <algorithm>
#include <cstdint>

namespace ui
{

using namespace theme;

const char* materialName (Material material)
{
    switch (material)
    {
        case Material::ember:    return "Ember";
        case Material::monolith: return "Monolith";
        case Material::forge:    return "Forge";
        case Material::basalt:   return "Basalt";
        case Material::comet:    return "Comet";
        case Material::quartz:   return "Quartz";
        case Material::lantern:  return "Lantern";
        case Material::custom:   return "Custom";
        case Material::glass:    break;
    }
    return "Glass";
}

Material materialForAmp (const juce::String& amp)
{
    const auto name = amp.trim();
    for (int m = 0; m < numMaterials; ++m)
        if ((Material) m != Material::custom && name.equalsIgnoreCase (materialName ((Material) m)))
            return (Material) m;
    return Material::custom;
}

Skin skinFor (Material material)
{
    switch (material)
    {
        case Material::ember:    return Skin::ember;
        case Material::monolith: return Skin::monolith;
        case Material::forge:    return Skin::forge;
        case Material::basalt:   return Skin::basalt;
        case Material::comet:    return Skin::comet;
        case Material::quartz:   return Skin::quartz;
        case Material::lantern:  return Skin::lantern;
        case Material::custom:   return Skin::custom;
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

/// Lightens (amount > 0, white at that alpha) or darkens (amount < 0, black) a colour.
juce::Colour shade (juce::Colour c, float amount)
{
    return amount >= 0.0f ? blend (c, juce::Colours::white.withAlpha (juce::jmin (1.0f, amount)))
                          : blend (c, juce::Colours::black.withAlpha (juce::jmin (1.0f, -amount)));
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

// ---- Procedural noise, for the newer materials' grain, flecks, and facets ------------------------
// All of it is a pure function of the CSS position (never of the device pixel), so a texture looks the same at
// every display scale, and of a fixed seed, so it's the same on every render.

/// An integer hash (the "lowbias32" finaliser: two multiply-xorshift rounds) of a lattice point, as 0 to 1.
float hash (int x, int y, int seed)
{
    const auto mix = [] (std::uint32_t h)
    {
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    };
    const auto h = mix ((std::uint32_t) x * 0x9e3779b1u ^ mix ((std::uint32_t) y + 0x632be5abu * (std::uint32_t) seed));
    return (float) (h >> 8) / 16777216.0f;
}

/// Value noise: the lattice's hashes, blended with smoothstep weights (C1 across cell edges). x, y in cells; 0 to 1.
float noise (float x, float y, int seed)
{
    const auto ix = (int) std::floor (x), iy = (int) std::floor (y);
    const auto s = [] (float t) { return t * t * (3.0f - 2.0f * t); };
    const auto tx = s (x - (float) ix), ty = s (y - (float) iy);
    const auto top = juce::jmap (tx, hash (ix, iy, seed), hash (ix + 1, iy, seed));
    const auto bottom = juce::jmap (tx, hash (ix, iy + 1, seed), hash (ix + 1, iy + 1, seed));
    return juce::jmap (ty, top, bottom);
}

/// Sparse round specks: a `density` share of the cell x cell grid's cells holds one, of radius about `radius`,
/// placed near the cell's centre (never crossing into a neighbour), with a 1 px antialiased edge. 0 to 1.
float speck (float x, float y, float cell, int seed, float density, float radius)
{
    const auto ix = (int) std::floor (x / cell), iy = (int) std::floor (y / cell);
    if (hash (ix, iy, seed) >= density)
        return 0.0f;
    const auto cx = ((float) ix + 0.5f + 0.3f * (hash (ix, iy, seed + 1) - 0.5f)) * cell;
    const auto cy = ((float) iy + 0.5f + 0.3f * (hash (ix, iy, seed + 2) - 0.5f)) * cell;
    const auto r = radius * (0.6f + 0.8f * hash (ix, iy, seed + 3));
    const auto d = std::hypot (x - cx, y - cy);
    return juce::jlimit (0.0f, 1.0f, r + 0.5f - d);
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

/// The newer badges' lettering: a word as a path in a font with letter-spacing (between letters only), stretched
/// horizontally by `stretch` (under 1 condenses, over 1 widens) and slanted by `slant` (the shear's x per unit of
/// height above the baseline: 0.2 is about an 11 degree italic). Its baseline is at y = 0, its ink starts at x = 0.
juce::Path lettering (const juce::String& text, const juce::FontOptions& font, float spacing, float stretch = 1.0f, float slant = 0.0f)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (juce::Font (font), text, 0.0f, 0.0f);
    for (int i = 1; i < glyphs.getNumGlyphs(); ++i)
        glyphs.moveRangeOfGlyphs (i, 1, spacing * (float) i, 0.0f);
    juce::Path p;
    glyphs.createPath (p);
    p.applyTransform (juce::AffineTransform (stretch, -slant, 0.0f, 0.0f, 1.0f, 0.0f)); // x' = s x - k y (y is negative above the baseline)
    p.applyTransform (juce::AffineTransform::translation (-p.getBounds().getX(), 0.0f));
    return p;
}

/// The height of a font's capitals (its "H"), to centre a word optically on its caps rather than its line box.
float capHeight (const juce::FontOptions& font)
{
    return lettering ("H", font, 0.0f).getBounds().getHeight();
}

/// Moves a lettering() path so its ink is centred horizontally in `box` and its cap height vertically.
juce::Path centred (juce::Path p, juce::Rectangle<float> box, float caps)
{
    const auto b = p.getBounds();
    p.applyTransform (juce::AffineTransform::translation (box.getCentreX() - b.getCentreX(), box.getCentreY() + caps * 0.5f));
    return p;
}

/// A word shortened to fit `maxWidth` by `widthOf`: whole if it fits, else as many leading characters as fit,
/// trimmed, and an ellipsis.
juce::String fitted (const juce::String& word, float maxWidth, const std::function<float (const juce::String&)>& widthOf)
{
    if (widthOf (word) <= maxWidth)
        return word;
    const juce::String ellipsis (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
    for (auto n = word.length() - 1; n > 0; --n)
    {
        const auto shorter = word.substring (0, n).trimEnd() + ellipsis;
        if (widthOf (shorter) <= maxWidth)
            return shorter;
    }
    return ellipsis;
}

/// A faceted gem (the Quartz badge's, and its mini's crystal): a rhombus with a crown line and a table.
void drawGem (juce::Graphics& g, juce::Point<float> c, float w, float h, juce::Colour fill, juce::Colour edge)
{
    juce::Path gem;
    gem.startNewSubPath (c.x, c.y - h * 0.5f);
    gem.lineTo (c.x + w * 0.5f, c.y - h * 0.12f);
    gem.lineTo (c.x, c.y + h * 0.5f);
    gem.lineTo (c.x - w * 0.5f, c.y - h * 0.12f);
    gem.closeSubPath();
    g.setColour (fill);
    g.fillPath (gem);
    {
        // The lower-right facets a step darker: the gem lit from the top left.
        juce::Path shadowSide;
        shadowSide.startNewSubPath (c.x, c.y - h * 0.12f);
        shadowSide.lineTo (c.x + w * 0.5f, c.y - h * 0.12f);
        shadowSide.lineTo (c.x, c.y + h * 0.5f);
        shadowSide.closeSubPath();
        g.setColour (edge.withAlpha (0.28f));
        g.fillPath (shadowSide);
    }
    g.setColour (edge);
    g.strokePath (gem, juce::PathStrokeType (juce::jmax (0.6f, w * 0.06f), juce::PathStrokeType::mitered));
    juce::Path crown;
    crown.startNewSubPath (c.x - w * 0.5f, c.y - h * 0.12f);
    crown.lineTo (c.x + w * 0.5f, c.y - h * 0.12f);
    crown.startNewSubPath (c.x, c.y - h * 0.5f);
    crown.lineTo (c.x, c.y + h * 0.5f);
    g.strokePath (crown, juce::PathStrokeType (juce::jmax (0.4f, w * 0.035f)));
}

/// A panel screw (the Forge panel's corners): a domed steel head with a hex socket.
void drawScrew (juce::Graphics& g, juce::Point<float> c)
{
    const auto head = juce::Rectangle<float> (9.0f, 9.0f).withCentre (c);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (head.translated (0.0f, 1.0f).expanded (0.5f));
    juce::ColourGradient dome (juce::Colour (0xffe3e7ea), head.getX() + 2.5f, head.getY() + 2.0f, juce::Colour (0xff4e5358), head.getRight(), head.getBottom(), true);
    g.setGradientFill (dome);
    g.fillEllipse (head);
    juce::Path hex;
    for (int k = 0; k < 6; ++k)
    {
        const auto a = juce::MathConstants<float>::twoPi * (float) k / 6.0f + 0.3f;
        const juce::Point<float> v { c.x + 2.1f * std::cos (a), c.y + 2.1f * std::sin (a) };
        k == 0 ? hex.startNewSubPath (v) : hex.lineTo (v);
    }
    hex.closeSubPath();
    g.setColour (juce::Colour (0xff1b1d20));
    g.fillPath (hex);
}

void drawBadge (juce::Graphics& g, Material material, juce::Rectangle<float> grille, const juce::String& word)
{
    // Every badge keeps clear of the grille's ends: a longer word (a user's capture on Custom) is elided.
    const auto maxWidth = grille.getWidth() - 160.0f;
    switch (material)
    {
        case Material::glass:
        {
            // Geist 300, 46 px, tracking .08em, light chrome; text-shadow 0 1px 0 white .3, 0 2px 4px black .6.
            const auto f = geist (Weight::light, 46.0f);
            const auto name = fitted (word, maxWidth, [&] (const juce::String& s) { return textWidth (f, s) + 0.08f * 46.0f * (float) s.length(); });
            const auto p = textPath (name, f, 0.08f * 46.0f, grille);
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
            const auto f = fraunces (52.0f);
            const auto name = fitted (word, maxWidth, [&] (const juce::String& s) { return textWidth (f, s); });
            const auto p = textPath (name, f, 0.0f, grille);
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
            const auto widthOf = [&] (const juce::String& s) { return textWidth (f, s) + spacing * (float) s.length(); };
            const auto name = fitted (word, maxWidth - 52.0f, widthOf);
            const auto run = widthOf (name);
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
            const auto p = textPath (name, f, spacing, textBox.withWidth (run).withX (textBox.getX()));
            g.setColour (juce::Colour (0xffcfd6d3));
            g.fillPath (p);
            break;
        }
        case Material::forge:
        {
            // Heavy and condensed: Geist 600 capitals at 58 px squeezed to 74% of their width, tracked .04em, in
            // brushed steel (a light-to-dark vertical gradient) with a hard shadow, between two hot orange rules.
            const auto f = geist (Weight::semibold, 58.0f);
            const auto make = [&] (const juce::String& s) { return lettering (s.toUpperCase(), f, 0.04f * 58.0f, 0.74f); };
            const auto name = fitted (word, maxWidth - 2.0f * 82.0f, [&] (const juce::String& s) { return make (s).getBounds().getWidth(); });
            const auto p = centred (make (name), grille, capHeight (f));
            const auto b = p.getBounds();
            juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 5, { 0, 3 }).drawForPath (g, p);
            g.setColour (juce::Colours::black);
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 1.5f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff2f4f6), 0.0f, b.getY(), juce::Colour (0xff8c939a), 0.0f, b.getBottom(), false));
            g.fillPath (p);
            g.setColour (juce::Colours::white.withAlpha (0.55f)); // the bevel's lit top edge
            {
                const juce::Graphics::ScopedSaveState saved (g);
                g.reduceClipRegion (p);
                juce::Path lit = p;
                lit.addPath (p, juce::AffineTransform::translation (0.0f, 1.0f));
                lit.setUsingNonZeroWinding (false);
                g.fillPath (lit);
            }
            for (const auto side : { -1.0f, 1.0f })
            {
                // A 3 px orange rule 56 px long, 22 px out from the word, with a faint glow.
                const auto x = side < 0.0f ? b.getX() - 22.0f - 56.0f : b.getRight() + 22.0f;
                const auto rule = juce::Rectangle<float> (x, grille.getCentreY() - 1.5f, 56.0f, 3.0f);
                juce::Path r;
                r.addRectangle (rule);
                juce::DropShadow (juce::Colour (0xffff6a1f).withAlpha (0.5f), 8, {}).drawForPath (g, r);
                g.setColour (juce::Colour (0xffff6a1f));
                g.fillRect (rule);
            }
            break;
        }
        case Material::basalt:
        {
            // Solid and wide: Geist 600 capitals at 40 px stretched to 138%, tracked .14em, in pale stone, cut into
            // the grille (a dark shadow above-left inside the strokes' edge reads as depth: drawn as a 2 px black
            // offset below and a 1 px light rim above).
            const auto f = geist (Weight::semibold, 40.0f);
            const auto make = [&] (const juce::String& s) { return lettering (s.toUpperCase(), f, 0.14f * 40.0f, 1.38f); };
            const auto name = fitted (word, maxWidth, [&] (const juce::String& s) { return make (s).getBounds().getWidth(); });
            const auto p = centred (make (name), grille, capHeight (f));
            const auto b = p.getBounds();
            juce::DropShadow (juce::Colours::black.withAlpha (0.7f), 6, { 0, 3 }).drawForPath (g, p);
            g.setColour (juce::Colour (0xff050606));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 2.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffd2d6da), 0.0f, b.getY(), juce::Colour (0xff9aa0a6), 0.0f, b.getBottom(), false));
            g.fillPath (p);
            break;
        }
        case Material::comet:
        {
            // Motion: Geist 600 at 54 px, slanted 12 degrees, in ice white with a cyan glow; a comet's tail of three
            // tapering streaks runs in from the left towards the word, the longest at its x-height, their heads
            // staggered back from the slanted letters. The word sits 90 px right of centre so the mark (the tail and
            // the word) is centred; a long word is elided to the room that leaves (96 px clear of the right end, its glow
            // included) and shortens the tail so it stays 80 px clear of the left end.
            const auto f = geist (Weight::semibold, 54.0f);
            const auto make = [&] (const juce::String& s) { return lettering (s, f, 0.01f * 54.0f, 1.0f, 0.21f); };
            const auto name = fitted (word, grille.getWidth() - 2.0f * (90.0f + 96.0f), [&] (const juce::String& s) { return make (s).getBounds().getWidth(); });
            const auto caps = capHeight (f);
            auto p = centred (make (name), grille, caps);
            p.applyTransform (juce::AffineTransform::translation (90.0f, 0.0f));
            const auto b = p.getBounds();
            const auto baseline = grille.getCentreY() + caps * 0.5f;
            const auto room = juce::jlimit (0.0f, 1.0f, (b.getX() - 34.0f - grille.getX() - 80.0f) / 236.0f);

            for (const auto& [rise, length, thickness, gap] : { std::tuple<float, float, float, float> { 0.36f, 236.0f, 5.0f, 12.0f },
                                                               { 0.64f, 150.0f, 2.6f, 34.0f }, { 0.10f, 170.0f, 2.2f, 24.0f } })
            {
                // A streak: a sliver, round at its head and fading to a point behind.
                const auto y = baseline - caps * rise;
                const auto headX = b.getX() + caps * rise * 0.21f - gap; // follows the slant
                const auto run = length * room;
                juce::Path streak;
                streak.startNewSubPath (headX - run, y);
                streak.quadraticTo (headX - run * 0.5f, y - thickness * 0.5f, headX, y - thickness * 0.5f);
                streak.addCentredArc (headX, y, thickness * 0.5f, thickness * 0.5f, 0.0f, 0.0f, juce::MathConstants<float>::pi);
                streak.quadraticTo (headX - run * 0.5f, y + thickness * 0.5f, headX - run, y);
                streak.closeSubPath();
                g.setGradientFill (juce::ColourGradient (juce::Colour (0x0067e8f9), headX - run, y, juce::Colour (0xffbff6ff), headX, y, false));
                g.fillPath (streak);
            }
            juce::DropShadow (juce::Colour (0xff3fc8f0).withAlpha (0.55f), 16, {}).drawForPath (g, p);
            g.setColour (juce::Colour (0xff030716).withAlpha (0.7f));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 2.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colours::white, 0.0f, b.getY(), juce::Colour (0xff9fe6ff), 0.0f, b.getBottom(), false));
            g.fillPath (p);
            break;
        }
        case Material::quartz:
        {
            // Crisp and thin: Geist 300 capitals at 40 px, tracked .36em, in deep violet pressed into the pale cloth
            // (a 1 px white line under each stroke), between two small faceted gems.
            const auto f = geist (Weight::light, 40.0f);
            const auto make = [&] (const juce::String& s) { return lettering (s.toUpperCase(), f, 0.36f * 40.0f); };
            const auto name = fitted (word, maxWidth - 2.0f * 50.0f, [&] (const juce::String& s) { return make (s).getBounds().getWidth(); });
            const auto caps = capHeight (f);
            const auto p = centred (make (name), grille, caps);
            const auto b = p.getBounds();
            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 1.0f));
            g.setColour (juce::Colour (0xff47325e));
            g.fillPath (p);
            for (const auto x : { b.getX() - 34.0f, b.getRight() + 34.0f })
            {
                const juce::Point<float> c { x, grille.getCentreY() };
                drawGem (g, c, 15.0f, 22.0f, juce::Colours::white, juce::Colour (0xff6d578a));
            }
            break;
        }
        case Material::lantern:
        {
            // A vintage script: Fraunces italic 600 at 58 px in cream, outlined in dark brown (a 4 px stroke under the
            // fill), with a swash under the word that sweeps up at its end.
            const auto f = fraunces (58.0f);
            const auto make = [&] (const juce::String& s) { return lettering (s, f, 0.0f); };
            const auto name = fitted (word, maxWidth - 40.0f, [&] (const juce::String& s) { return make (s).getBounds().getWidth(); });
            const auto caps = capHeight (f);
            auto p = centred (make (name), grille, caps);
            p.applyTransform (juce::AffineTransform::translation (0.0f, -7.0f)); // up a little: the swash sits under it
            const auto b = p.getBounds();
            const auto baseline = grille.getCentreY() + caps * 0.5f - 7.0f;

            // The swash: a crescent from under the first letter to past the last, thickest in the middle.
            const auto x0 = b.getX() + 6.0f, x1 = b.getRight() + 22.0f, y0 = baseline + 9.0f;
            juce::Path swash;
            swash.startNewSubPath (x0, y0);
            swash.cubicTo (x0 + (x1 - x0) * 0.35f, y0 + 10.0f, x0 + (x1 - x0) * 0.75f, y0 + 7.0f, x1, y0 - 14.0f);
            swash.cubicTo (x0 + (x1 - x0) * 0.74f, y0 + 2.0f, x0 + (x1 - x0) * 0.36f, y0 + 4.0f, x0, y0);
            swash.closeSubPath();

            juce::Path all = p;
            all.addPath (swash);
            juce::DropShadow (juce::Colours::black.withAlpha (0.65f), 6, { 0, 3 }).drawForPath (g, all);
            g.setColour (juce::Colour (0xff24140a));
            g.strokePath (all, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffbf0d2), 0.0f, b.getY(), juce::Colour (0xffe3c992), 0.0f, y0 + 10.0f, false));
            g.fillPath (all);
            break;
        }
        case Material::custom:
        {
            // Quiet: the capture's own name as typed, Geist 500 at 28 px, tracked .06em, in a soft grey.
            const auto f = geist (Weight::medium, 28.0f);
            const auto make = [&] (const juce::String& s) { return lettering (s, f, 0.06f * 28.0f); };
            const auto name = fitted (word, maxWidth, [&] (const juce::String& s) { return make (s).getBounds().getWidth(); });
            const auto p = centred (make (name), grille, capHeight (f));
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillPath (p, juce::AffineTransform::translation (0.0f, 1.5f));
            g.setColour (juce::Colour (0xffbfc4c8));
            g.fillPath (p);
            break;
        }
    }
}
} // namespace

juce::Image AmpHead::render (Material material, float scale, const juce::String& name)
{
    const auto bounds = juce::Rectangle<int> (0, 0, margin.getLeft() + headWidth + margin.getRight(), margin.getTop() + headHeight + margin.getBottom());
    juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) bounds.getWidth() * scale), juce::roundToInt ((float) bounds.getHeight() * scale), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));

    const auto head = headBox().toFloat();
    juce::Path body;
    body.addRoundedRectangle (head, radiusHead);

    // The shadows: 0 24px 40px black .55 (all of them), and 0 2px 0 black .6 (all but the monolith: the tolex heads).
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
        case Material::forge:
            // Black pebble tolex: a near-black gradient with a fine grain (2 px value noise) and a faint 3 px dot.
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff1c1d1f), juce::Colour (0xff0f1011), y, h);
                const auto grain = noise (x * 0.5f, y * 0.5f, 3) - 0.5f;
                return blend (shade (base, 0.07f * grain), juce::Colours::white.withAlpha (0.035f * dot (x, y, 3.0f)));
            });
            break;
        case Material::basalt:
            // Dark stone: a grey gradient mottled by two octaves of value noise (18 and 5 px), with sparse pale
            // flecks and darker pits.
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                auto c = vertical (juce::Colour (0xff3d4043), juce::Colour (0xff2a2c2f), y, h);
                c = shade (c, 0.10f * (noise (x / 18.0f, y / 18.0f, 21) - 0.5f) + 0.06f * (noise (x / 5.0f, y / 5.0f, 22) - 0.5f));
                c = c.interpolatedWith (juce::Colour (0xff6c7277), 0.5f * speck (x, y, 8.0f, 23, 0.09f, 0.8f));
                return c.interpolatedWith (juce::Colour (0xff17181a), 0.55f * speck (x + 4.0f, y + 4.0f, 9.0f, 24, 0.10f, 0.8f));
            });
            break;
        case Material::comet:
            // Deep navy metal-flake tolex: the navy gradient, a 3 px dot, and fine pale blue flakes.
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff1e2c5e), juce::Colour (0xff121a3f), y, h);
                const auto c = blend (base, juce::Colours::black.withAlpha (0.22f * dot (x, y, 3.0f)));
                return blend (c, juce::Colour (0xffa9c8ff).withAlpha (0.55f * speck (x, y, 4.0f, 31, 0.10f, 0.45f)));
            });
            break;
        case Material::quartz:
        {
            // Pale lilac crystal: a 26 px lattice of squares, each split on a random diagonal into two triangular
            // facets; each facet is lit by its own random tilt (+-4%), and the facets' edges catch a faint highlight.
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xffece3f2), juce::Colour (0xffd4c5df), y, h);
                constexpr float cell = 26.0f;
                const auto fx = x / cell, fy = y / cell;
                const auto i = (int) std::floor (fx), j = (int) std::floor (fy);
                const auto u = fx - (float) i, v = fy - (float) j;
                const auto flip = hash (i, j, 41) < 0.5f;
                const auto upper = flip ? u + v < 1.0f : u > v;
                const auto tilt = hash (2 * i + (upper ? 1 : 0), j, 42) - 0.5f;
                const auto diagonal = (flip ? std::abs (u + v - 1.0f) : std::abs (u - v)) * 0.70710678f;
                const auto edge = std::min ({ u, 1.0f - u, v, 1.0f - v, diagonal }) * cell; // CSS px to the nearest edge
                const auto c = shade (base, 0.08f * tilt);
                return blend (c, juce::Colours::white.withAlpha (0.22f * juce::jlimit (0.0f, 1.0f, 1.0f - edge / 0.8f)));
            });
            break;
        }
        case Material::lantern:
            // Warm tweed: a 2-over-2 diagonal twill (4 px period along x + y) of two wheat threads, a finer cross
            // grain along x - y, slubs (stretched value noise) and a lacquer that darkens downwards.
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xffd2b479), juce::Colour (0xffb38f55), y, h);
                const auto t = std::fmod (x + y, 4.0f);
                auto c = t < 2.0f ? shade (base, 0.07f) : shade (base, -0.10f);
                c = shade (c, std::fmod (std::abs (x - y), 2.0f) < 1.0f ? -0.03f : 0.0f);
                c = shade (c, 0.10f * (noise ((x + y) / 9.0f, (x - y) / 2.0f, 51) - 0.5f));
                return blend (c, juce::Colour (0xff6b4a22).withAlpha (0.10f * (1.0f - std::abs (2.0f * y / h - 1.0f))));
            });
            break;
        case Material::custom:
            // Neutral graphite: a mid-dark grey gradient with the clean head's fine 3 px dot.
            fillPattern (g, head, radiusHead, scale, [h] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff60646a), juce::Colour (0xff4a4e53), y, h);
                return blend (base, juce::Colours::black.withAlpha (0.12f * dot (x, y, 3.0f)));
            });
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
        g.setColour (juce::Colours::white.withAlpha (material == Material::monolith ? 0.08f : material == Material::quartz ? 0.4f : 0.12f));
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
        case Material::forge:
        {
            // Brushed steel, brushed horizontally (the Glass panel's lines run vertically): value noise stretched
            // 60 to 1 along x makes the long streaks, over a darker steel gradient; a screw in each corner, a dark
            // 1 px edge, and the hot orange line under the panel (in the 12 px gap above the grille) with a glow.
            fillPattern (g, panel, radiusPanel, scale, [ph] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xffa4aab0), juce::Colour (0xff7d8389), y, ph);
                const auto streak = 0.7f * (noise (x / 60.0f, y / 0.8f, 61) - 0.5f) + 0.3f * (noise (x / 9.0f, y / 0.5f, 62) - 0.5f);
                return shade (base, 0.16f * streak);
            });
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.fillRect (panel.reduced (radiusPanel, 0.0f).withHeight (1.0f));
            g.setColour (juce::Colour (0xff2b2e31));
            g.drawRoundedRectangle (panel.reduced (0.5f), radiusPanel - 0.5f, 1.0f);
            for (const auto& c : { panel.getTopLeft().translated (10.0f, 10.0f), panel.getTopRight().translated (-10.0f, 10.0f),
                                   panel.getBottomLeft().translated (10.0f, -10.0f), panel.getBottomRight().translated (-10.0f, -10.0f) })
                drawScrew (g, c);
            const auto hot = juce::Rectangle<float> (panel.getX() + 4.0f, panel.getBottom() + 5.0f, panel.getWidth() - 8.0f, 2.0f);
            juce::Path line;
            line.addRectangle (hot);
            juce::DropShadow (juce::Colour (0xffff6a1f).withAlpha (0.7f), 6, {}).drawForPath (g, line);
            g.setColour (juce::Colour (0xffff6a1f));
            g.fillRect (hot);
            g.setColour (juce::Colour (0xffffb27a));
            g.fillRect (hot.withHeight (0.5f));
            break;
        }
        case Material::basalt:
        {
            // A near-black slab in a grey frame: the frame a 4 px stone-grey border (its own gradient and a lit top
            // edge), the slab inside with a faint grain and an inset shadow along its top.
            const auto inner = panel.reduced (4.0f);
            fillPattern (g, panel, radiusPanel, scale, [ph] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff868b90), juce::Colour (0xff5d6267), y, ph);
                return shade (base, 0.08f * (noise (x / 4.0f, y / 4.0f, 71) - 0.5f));
            });
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.fillRect (panel.reduced (radiusPanel, 0.0f).withHeight (1.0f));
            const auto ih = inner.getHeight();
            fillPattern (g, inner, 3.0f, scale, [ih] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff1c1d1f), juce::Colour (0xff141516), y, ih);
                const auto c = shade (base, 0.05f * (noise (x / 3.0f, y / 3.0f, 72) - 0.5f));
                return blend (c, juce::Colours::black.withAlpha (0.55f * tail ((y - 1.0f) / 2.0f)));
            });
            g.setColour (juce::Colour (0xff3a3e42));
            g.drawRoundedRectangle (inner.reduced (0.5f), 2.5f, 1.0f);
            break;
        }
        case Material::comet:
        {
            // A dark navy panel with a 1 px blue edge, and the comet motif along its top edge: a hairline that
            // brightens from nothing at the left to ice white at the right, ending in a small glowing head.
            fillPattern (g, panel, radiusPanel, scale, [ph] (float, float y) { return vertical (juce::Colour (0xff0e1636), juce::Colour (0xff080d25), y, ph); });
            g.setColour (juce::Colour (0xff253463));
            g.drawRoundedRectangle (panel.reduced (0.5f), radiusPanel - 0.5f, 1.0f);
            const auto y = panel.getY() + 5.0f;
            const auto x0 = panel.getX() + 140.0f, x1 = panel.getRight() - 14.0f;
            juce::Path streak;
            streak.startNewSubPath (x0, y + 0.25f);
            streak.lineTo (x1, y - 0.75f);
            streak.lineTo (x1, y + 0.75f);
            streak.closeSubPath();
            g.setGradientFill (juce::ColourGradient (juce::Colour (0x0067e8f9), x0, y, juce::Colour (0xffd6f9ff), x1, y, false));
            g.fillPath (streak);
            juce::Path headDot;
            headDot.addEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x1, y }));
            juce::DropShadow (juce::Colour (0xff67e8f9), 8, {}).drawForPath (g, headDot);
            g.setColour (juce::Colours::white);
            g.fillPath (headDot);
            break;
        }
        case Material::quartz:
        {
            // A deeper lilac panel with a soft diagonal sheen (a light band across it from the top left), a lit
            // 1 px top edge and a violet 1 px edge.
            const auto pw = panel.getWidth();
            fillPattern (g, panel, radiusPanel, scale, [ph, pw] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xffb5a3c6), juce::Colour (0xff9c88b0), y, ph);
                const auto band = std::exp (-std::pow ((x - 0.28f * pw + 2.2f * y) / 160.0f, 2.0f));
                return shade (base, 0.10f * band);
            });
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillRect (panel.reduced (radiusPanel, 0.0f).withHeight (1.0f));
            g.setColour (juce::Colour (0xff7d6a92));
            g.drawRoundedRectangle (panel.reduced (0.5f), radiusPanel - 0.5f, 1.0f);
            break;
        }
        case Material::lantern:
        {
            // A brown leatherette panel (fine grain), with a thin amber pinstripe 5 px in and a dark 1 px edge.
            fillPattern (g, panel, radiusPanel, scale, [ph] (float x, float y)
            {
                const auto base = vertical (juce::Colour (0xff4c3424), juce::Colour (0xff35231a), y, ph);
                return shade (base, 0.07f * (noise (x / 2.5f, y / 2.5f, 81) - 0.5f) + 0.04f * (noise (x / 14.0f, y / 14.0f, 82) - 0.5f));
            });
            g.setColour (juce::Colour (0xff1c120b));
            g.drawRoundedRectangle (panel.reduced (0.5f), radiusPanel - 0.5f, 1.0f);
            g.setColour (juce::Colour (0xffe0a24a).withAlpha (0.55f));
            g.drawRoundedRectangle (panel.reduced (5.0f), radiusPanel - 3.0f, 1.0f);
            break;
        }
        case Material::custom:
            fillPattern (g, panel, radiusPanel, scale, [ph] (float, float y) { return vertical (juce::Colour (0xff25282a), juce::Colour (0xff1c1e20), y, ph); });
            g.setColour (juce::Colour (0xff3d4145));
            g.drawRoundedRectangle (panel.reduced (0.5f), radiusPanel - 0.5f, 1.0f);
            break;
    }

    // The grille, with its inset shadow (0 2px 6px black, .6; .8 on the monolith, .3 on Quartz's light cloth):
    // darkest along the top edge (the shadow is cast 2 px down), lighter along the sides, least at the bottom.
    const auto grille = grilleBox();
    const auto gw = grille.getWidth(), gh = grille.getHeight();
    const auto insetAlpha = material == Material::monolith ? 0.8f : material == Material::quartz ? 0.3f : 0.6f;
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
        case Material::forge:
            // Perforated steel: round 3.6 px holes on a staggered (hexagonal) grid, 6 px apart in a row and rows
            // 5.2 px apart (6 sin 60), each hole's lower lip catching a little light.
            fillPattern (g, grille, radiusPanel, scale, [inset, gh] (float x, float y)
            {
                constexpr float pitch = 6.0f, row = 5.196f, r = 1.8f;
                const auto metal = vertical (juce::Colour (0xff2e3135), juce::Colour (0xff222427), y, gh);
                float hole = 0.0f, lip = 0.0f;
                const auto j0 = (int) std::floor (y / row);
                for (int j = j0 - 1; j <= j0 + 1; ++j)
                {
                    const auto offset = (j & 1) != 0 ? pitch * 0.5f : 0.0f;
                    const auto cx = std::round ((x - offset) / pitch) * pitch + offset, cy = ((float) j + 0.5f) * row;
                    const auto d = std::hypot (x - cx, y - cy);
                    hole = juce::jmax (hole, juce::jlimit (0.0f, 1.0f, r + 0.5f - d));
                    if (y > cy)
                        lip = juce::jmax (lip, juce::jlimit (0.0f, 1.0f, 1.0f - std::abs (d - r - 0.6f) / 0.6f) * (y - cy) / (r + 0.6f));
                }
                auto c = shade (metal, 0.10f * lip);
                c = c.interpolatedWith (juce::Colour (0xff050506), hole);
                return blend (c, inset (x, y));
            });
            break;
        case Material::basalt:
            // A coarse plain weave of near-black yarn: 6 px cells, the thread on top alternating like a checkerboard,
            // each thread rounded across its width (a sine profile) and dipping where it goes under (along it).
            fillPattern (g, grille, radiusPanel, scale, [inset] (float x, float y)
            {
                constexpr float p = 6.0f;
                const auto i = (int) std::floor (x / p), j = (int) std::floor (y / p);
                const auto u = x / p - (float) i, v = y / p - (float) j;
                const auto verticalOnTop = ((i + j) & 1) == 0;
                const auto across = verticalOnTop ? u : v, along = verticalOnTop ? v : u;
                const auto profile = std::sin (juce::MathConstants<float>::pi * juce::jlimit (0.0f, 1.0f, (across - 0.08f) / 0.84f));
                const auto lum = profile * (0.7f + 0.3f * std::sin (juce::MathConstants<float>::pi * along));
                const auto c = juce::Colour (0xff0b0c0d).interpolatedWith (juce::Colour (0xff3a3d41), lum);
                return blend (c, inset (x, y));
            });
            break;
        case Material::comet:
            // A fine navy mesh: 3 px squares of the cloth between 1 px darker threads.
            fillPattern (g, grille, radiusPanel, scale, [inset, gh] (float x, float y)
            {
                const auto cloth = vertical (juce::Colour (0xff15204a), juce::Colour (0xff0f173a), y, gh);
                const auto thread = std::fmod (x, 3.0f) < 1.0f || std::fmod (y, 3.0f) < 1.0f;
                return blend (thread ? shade (cloth, -0.35f) : cloth, inset (x, y));
            });
            break;
        case Material::quartz:
            // A light, fine cloth: a 3 px diagonal twill (the Glass weave's direction, mirrored) of two pale lilac-greys.
            fillPattern (g, grille, radiusPanel, scale, [inset] (float x, float y)
            {
                const auto t = (x + y) * 0.70710678f;
                const auto base = std::fmod (t, 3.0f) < 1.5f ? juce::Colour (0xffe3dbe9) : juce::Colour (0xffd2c7db);
                return blend (base, inset (x, y));
            });
            break;
        case Material::lantern:
        {
            // Oxblood cloth: a 4 px plain weave of two reds, a faint wheat thread every 24 px, and the lantern's glow, an
            // amber light behind the cloth around the badge (an elliptical falloff, 300 x 70 px).
            const auto cx = gw * 0.5f, cy = gh * 0.5f;
            fillPattern (g, grille, radiusPanel, scale, [inset, cx, cy] (float x, float y)
            {
                const auto i = (int) std::floor (x / 2.0f), j = (int) std::floor (y / 2.0f);
                auto c = ((i + j) & 1) == 0 ? juce::Colour (0xff5e2025) : juce::Colour (0xff47161a);
                if (std::fmod (x, 24.0f) < 1.0f)
                    c = c.interpolatedWith (juce::Colour (0xffc9a66b), 0.18f);
                const auto dx = (x - cx) / 300.0f, dy = (y - cy) / 70.0f;
                c = blend (c, juce::Colour (0xffff9a2e).withAlpha (0.32f * std::exp (-(dx * dx + dy * dy) * 2.0f)));
                return blend (c, inset (x, y));
            });
            break;
        }
        case Material::custom:
            // A plain fine mesh in two dark greys (2 px checks).
            fillPattern (g, grille, radiusPanel, scale, [inset] (float x, float y)
            {
                const auto i = (int) std::floor (x / 2.0f), j = (int) std::floor (y / 2.0f);
                const auto base = ((i + j) & 1) == 0 ? juce::Colour (0xff27292c) : juce::Colour (0xff1e2022);
                return blend (base, inset (x, y));
            });
            break;
    }

    drawBadge (g, material, grille, name.isNotEmpty() ? name : juce::String (materialName (material)));
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

void AmpHead::setBadge (const juce::String& name)
{
    const auto word = name == materialName (material) ? juce::String() : name;
    if (word != badge)
    {
        badge = word;
        repaint();
    }
}

juce::String AmpHead::getBadge() const
{
    return badge.isNotEmpty() ? badge : juce::String (materialName (material));
}

void AmpHead::paint (juce::Graphics& g)
{
    // Re-rendered only when the scale it's shown at, the material, or the badge changes (a window resize, a slot
    // switch, another amp loaded).
    const auto scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (! cached.isValid() || std::abs (scale - cachedScale) > 0.01f || material != cachedMaterial || badge != cachedBadge)
    {
        cached = render (material, scale, badge);
        cachedScale = scale;
        cachedMaterial = material;
        cachedBadge = badge;
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
    // 30 x 20, radius 3, a 1 px white .08 ring; a 5 px panel bar 3 px in from the sides (4 down on the monolith).
    // Each is its full head in miniature: the body's colour, the panel's, and the one detail that names the look.
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRoundedRectangle (box.expanded (1.0f), radiusMini + 1.0f);
    const auto body = [&] (juce::Colour c)
    {
        g.setColour (c);
        g.fillRoundedRectangle (box, radiusMini);
    };
    const auto bar = juce::Rectangle<float> (box.getX() + 3.0f, box.getY() + (material == Material::monolith ? 4.0f : 3.0f), box.getWidth() - 6.0f, 5.0f);

    switch (material)
    {
        case Material::glass:
            body (juce::Colour (0xffd9d6cd));
            g.setColour (juce::Colour (0xffb9bcba));
            g.fillRoundedRectangle (bar, 1.0f);
            break;
        case Material::ember:
            body (juce::Colour (0xff4b1d1f));
            g.setColour (juce::Colour (0xffa88a52));
            g.fillRoundedRectangle (bar.expanded (1.0f), 2.0f);
            g.setColour (juce::Colour (0xff1c1514));
            g.fillRoundedRectangle (bar, 1.0f);
            break;
        case Material::monolith:
            body (juce::Colour (0xff161717));
            g.setColour (juce::Colour (0xff0c0d0d));
            g.fillRoundedRectangle (bar, 1.0f);
            g.setColour (accent);
            g.fillRect (bar.withTop (bar.getBottom() - 1.0f));
            break;
        case Material::forge:
            // Black, a brushed steel panel, a line of hot orange under it.
            body (juce::Colour (0xff111213));
            g.setColour (juce::Colour (0xff8d9399));
            g.fillRoundedRectangle (bar, 1.0f);
            g.setColour (juce::Colour (0xffff6a1f));
            g.fillRect (bar.getX(), bar.getBottom() + 1.5f, bar.getWidth(), 1.0f);
            break;
        case Material::basalt:
            // Dark stone with a few lighter flecks, a near-black panel in a grey frame.
            body (juce::Colour (0xff35383b));
            g.setColour (juce::Colour (0xff4d5155));
            for (const auto& [fx, fy] : { std::pair<float, float> { 0.22f, 0.62f }, { 0.48f, 0.78f }, { 0.71f, 0.58f }, { 0.86f, 0.82f }, { 0.36f, 0.88f } })
                g.fillEllipse (juce::Rectangle<float> (1.6f, 1.6f).withCentre ({ box.getX() + fx * box.getWidth(), box.getY() + fy * box.getHeight() }));
            g.setColour (juce::Colour (0xff7b8085));
            g.fillRoundedRectangle (bar.expanded (1.0f), 2.0f);
            g.setColour (juce::Colour (0xff1a1b1c));
            g.fillRoundedRectangle (bar, 1.0f);
            break;
        case Material::comet:
        {
            // Deep navy, a dark panel, and a comet's streak across the body to a bright head.
            body (juce::Colour (0xff17234d));
            g.setColour (juce::Colour (0xff0b1230));
            g.fillRoundedRectangle (bar, 1.0f);
            const juce::Point<float> tailEnd { box.getX() + 4.0f, box.getBottom() - 3.5f }, headPoint { box.getRight() - 7.0f, box.getY() + 11.5f };
            g.setGradientFill (juce::ColourGradient (juce::Colour (0x0067e8f9), tailEnd, juce::Colour (0xff9ff3ff), headPoint, false));
            juce::Path streak;
            streak.startNewSubPath (tailEnd);
            streak.lineTo (headPoint);
            g.strokePath (streak, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (2.6f, 2.6f).withCentre (headPoint));
            break;
        }
        case Material::quartz:
        {
            // Pale lilac, a deeper lilac panel, a small white crystal.
            body (juce::Colour (0xffe4d7ea));
            g.setColour (juce::Colour (0xffa58fb6));
            g.fillRoundedRectangle (bar, 1.0f);
            const auto c = juce::Point<float> (box.getCentreX(), box.getY() + 14.0f);
            juce::Path crystal;
            crystal.startNewSubPath (c.x, c.y - 3.5f);
            crystal.lineTo (c.x + 2.5f, c.y);
            crystal.lineTo (c.x, c.y + 3.5f);
            crystal.lineTo (c.x - 2.5f, c.y);
            crystal.closeSubPath();
            g.setColour (juce::Colours::white);
            g.fillPath (crystal);
            g.setColour (juce::Colour (0xff8d78a0));
            g.strokePath (crystal, juce::PathStrokeType (0.6f));
            break;
        }
        case Material::lantern:
        {
            // Warm tweed (a faint diagonal weave), a brown panel, an amber pilot light.
            body (juce::Colour (0xffc8a467));
            {
                const juce::Graphics::ScopedSaveState saved (g);
                juce::Path shape;
                shape.addRoundedRectangle (box, radiusMini);
                g.reduceClipRegion (shape);
                g.setColour (juce::Colour (0xff8a6a3a).withAlpha (0.35f));
                for (auto x = box.getX() - box.getHeight(); x < box.getRight(); x += 3.0f)
                    g.drawLine (x, box.getBottom(), x + box.getHeight(), box.getY(), 0.6f);
            }
            g.setColour (juce::Colour (0xff3b2a1b));
            g.fillRoundedRectangle (bar, 1.0f);
            g.setColour (juce::Colour (0xffffb347));
            g.fillEllipse (juce::Rectangle<float> (2.4f, 2.4f).withCentre ({ bar.getRight() - 2.5f, bar.getCentreY() }));
            break;
        }
        case Material::custom:
            // Neutral graphite, a darker panel.
            body (juce::Colour (0xff575b60));
            g.setColour (juce::Colour (0xff222426));
            g.fillRoundedRectangle (bar, 1.0f);
            break;
    }
}

void drawMiniAmp (juce::Graphics& g, juce::Rectangle<float> box, const juce::String& amp)
{
    drawMiniHead (g, box, materialForAmp (amp));
}

} // namespace ui

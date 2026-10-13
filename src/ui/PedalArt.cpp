// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The pedal art (the app redesign; ASSUMPTIONS PA): fourteen invented enclosures, one per effect block, drawn in code
// like the amp heads (AmpHead.cpp) and from the same toolkit: finishes evaluated per pixel as pure functions of the
// point (so they look the same at every scale), hardware from gradients, and lettering as glyph paths.
//
// Every enclosure is the same family: a die-cast box seen from above, lit from the top left, with its edge rolled
// over (a bevel computed from the rounded box's distance field), the jacks standing out of its top edge, a chrome
// LED bezel, a stomp switch, static knobs (the inspector holds the live controls), and its name in a lettering of
// its own. Only the materials, the knobs' kind and arrangement, and the silkscreen change.

#include "PedalArt.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ui
{

using namespace theme;

namespace
{
constexpr float pedalHeight = 172.0f; // every pedal, the jacks included
constexpr float jackRise = 7.0f;      // how far the jacks stand above the body
constexpr float bodyRadius = 7.0f;
constexpr float ledLens = 2.9f, ledBezel = 4.7f, ledGlowRadius = 15.0f;
constexpr float nutRadius = 11.5f; // the footswitch's hex nut, centre to corner

// The light comes from the top left (as on the heads' screws and gems): the unit vector towards it, in screen
// coordinates (y down).
constexpr float lightX = -0.32f, lightY = -0.95f;

// ---- Small colour and noise helpers (the same as AmpHead.cpp's) ----------------------------------

using Pattern = std::function<juce::Colour (float x, float y)>; // points from the element's top-left

juce::Colour vertical (juce::Colour top, juce::Colour bottom, float y, float height)
{
    return top.interpolatedWith (bottom, juce::jlimit (0.0f, 1.0f, y / height));
}

juce::Colour blend (juce::Colour under, juce::Colour over) { return under.overlaidWith (over); }

/// Lightens (amount > 0: white at that alpha) or darkens (amount < 0: black).
juce::Colour shade (juce::Colour c, float amount)
{
    return amount >= 0.0f ? blend (c, juce::Colours::white.withAlpha (juce::jmin (1.0f, amount)))
                          : blend (c, juce::Colours::black.withAlpha (juce::jmin (1.0f, -amount)));
}

/// An integer hash of a lattice point (the "lowbias32" finaliser), as 0 to 1.
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

/// Value noise: the lattice's hashes blended with smoothstep weights (C1 across cell edges). x, y in cells; 0 to 1.
float noise (float x, float y, int seed)
{
    const auto ix = (int) std::floor (x), iy = (int) std::floor (y);
    const auto s = [] (float t) { return t * t * (3.0f - 2.0f * t); };
    const auto tx = s (x - (float) ix), ty = s (y - (float) iy);
    const auto top = juce::jmap (tx, hash (ix, iy, seed), hash (ix + 1, iy, seed));
    const auto bottom = juce::jmap (tx, hash (ix, iy + 1, seed), hash (ix + 1, iy + 1, seed));
    return juce::jmap (ty, top, bottom);
}

/// Sparse round flakes: a `density` share of the cells of a cell x cell grid holds one, of radius about `radius`,
/// anywhere in its cell (the 3 x 3 cells round the point are looked at, so a flake near an edge isn't cut and the grid
/// never shows), with a 0.5 pt antialiased edge. Returns the coverage (0 to 1); `value` gets the covering flake's
/// own random 0 to 1 (its brightness or its hue, as the finish uses it).
float flake (float x, float y, float cell, int seed, float density, float radius, float& value)
{
    const auto ix = (int) std::floor (x / cell), iy = (int) std::floor (y / cell);
    float cover = 0.0f;
    for (int j = iy - 1; j <= iy + 1; ++j)
        for (int i = ix - 1; i <= ix + 1; ++i)
        {
            if (hash (i, j, seed) >= density)
                continue;
            const auto cx = ((float) i + hash (i, j, seed + 1)) * cell, cy = ((float) j + hash (i, j, seed + 2)) * cell;
            const auto r = radius * (0.6f + 0.8f * hash (i, j, seed + 3));
            const auto c = juce::jlimit (0.0f, 1.0f, r + 0.5f - std::hypot (x - cx, y - cy));
            if (c > cover)
            {
                cover = c;
                value = hash (i, j, seed + 4);
            }
        }
    return cover;
}

/// Renders a pattern over `area` (points) into an image at `scale`, sampling each device pixel's centre.
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

/// Fills a shape with a pattern (the path clips it, antialiased); the pattern's origin is `area`'s top-left.
void fillPattern (juce::Graphics& g, const juce::Path& shape, juce::Rectangle<float> area, float scale, const Pattern& pattern)
{
    const juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (shape);
    g.setOpacity (1.0f);
    g.drawImage (patternImage (area, scale, pattern), area);
}

/// A word as a path: letter-spacing between letters, stretched by `stretch` (under 1 condenses) and slanted by `slant`
/// (x per unit of height above the baseline), each letter `rise (i)` points up from the baseline. Baseline at y = 0,
/// ink from x = 0.
juce::Path lettering (const juce::String& text, const juce::FontOptions& font, float spacing, float stretch = 1.0f, float slant = 0.0f,
                      const std::function<float (int)>& rise = {})
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (juce::Font (font), text, 0.0f, 0.0f);
    for (int i = 1; i < glyphs.getNumGlyphs(); ++i)
        glyphs.moveRangeOfGlyphs (i, 1, spacing * (float) i, 0.0f);
    if (rise)
        for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
            glyphs.moveRangeOfGlyphs (i, 1, 0.0f, -rise (i));
    juce::Path p;
    glyphs.createPath (p);
    p.applyTransform (juce::AffineTransform (stretch, -slant, 0.0f, 0.0f, 1.0f, 0.0f));
    p.applyTransform (juce::AffineTransform::translation (-p.getBounds().getX(), 0.0f));
    return p;
}

/// The height of a font's capitals (its "H"), to centre a word optically.
float capHeight (const juce::FontOptions& font) { return lettering ("H", font, 0.0f).getBounds().getHeight(); }

/// A lettering() path moved so its ink is centred on x and its baseline is at y.
juce::Path placed (juce::Path p, float x, float baseline)
{
    p.applyTransform (juce::AffineTransform::translation (x - p.getBounds().getCentreX(), baseline));
    return p;
}

// ---- The designs ---------------------------------------------------------------------------------

enum class Finish
{
    hammertone, // dimpled metallic enamel
    anodised,   // dyed aluminium, brushed vertically
    brushed,    // bare aluminium, brushed horizontally
    metalFlake, // a coarse metallic flake under clear
    powder,     // powder coat with an orange-peel texture
    matte,      // flat paint with a fine tooth
    gloss,      // deep gloss enamel with a sheen
    sparkle,    // multicoloured glitter flake
    candy,      // a translucent candy colour over a metallic base
    pearl,      // a pearlescent coat whose hue shifts across it
    speckle,    // satin enamel with dark and light specks
    wrinkle,    // crinkle finish: fine wandering ridges
    sandblast   // a fine, dry, granular surface
};

enum class KnobStyle
{
    aluminium,   // machined: a spun top, a knurled edge, an engraved line
    skirted,     // black, on a skirt with a white line
    cream,       // cream plastic with a black line
    chickenHead, // black bakelite pointer
    chrome,      // a polished dome with a dot
    softCap      // soft-touch rubber in the design's knob colour
};

struct KnobSpec
{
    float x, y, d, angle; // centre, diameter, the pointer's angle in radians from 12 o'clock, clockwise
    const char* label;
};

struct Design
{
    float width = 104.0f;
    Finish finish = Finish::powder;
    juce::Colour top, bottom, fleck; // the finish's colour down the body, and its flake or highlight colour
    juce::Colour ink;                // the silkscreen
    KnobStyle knobStyle = KnobStyle::skirted;
    juce::Colour knobColour, pointer; // softCap's and chickenHead's colours
    std::vector<KnobSpec> knobs;
    juce::Point<float> led, footswitch;
    juce::Colour topper; // the footswitch's rubber topper (transparent: bare chrome)
    bool screws = false, dcJack = true;
    float bevel = 1.0f; // how strongly the edge's roll catches the light
};

/// The pre and post twins (the compressors, the EQs) share a design and differ in colourway and in the PRE / POST
/// mark (PA3).
bool isPost (BlockId id) { return id == BlockId::postCompressor || id == BlockId::postEq; }

constexpr float deg = juce::MathConstants<float>::pi / 180.0f;

Design design (BlockId id)
{
    Design d;
    const auto y0 = jackRise; // the body's top
    switch (id)
    {
        case BlockId::gateA:
            // Gunmetal hammertone, two machined knobs, the name with its letter in a box.
            d.width = 92.0f;
            d.finish = Finish::hammertone;
            d.top = juce::Colour (0xff5d6874);
            d.bottom = juce::Colour (0xff404953);
            d.fleck = juce::Colour (0xffc9d3dd);
            d.ink = juce::Colour (0xfff1f4f6);
            d.knobStyle = KnobStyle::aluminium;
            d.knobs = { { 25.0f, y0 + 30.0f, 23.0f, -40.0f * deg, "THRESH" }, { 67.0f, y0 + 30.0f, 23.0f, 70.0f * deg, "RELEASE" } };
            d.led = { 46.0f, y0 + 62.0f };
            d.footswitch = { 46.0f, y0 + 131.0f };
            d.dcJack = false;
            break;
        case BlockId::preCompressor:
        case BlockId::postCompressor:
            // Anodised aluminium (cobalt before the amp, ruby after it), four black skirted knobs in a square round
            // the LED, a light lowercase name, and its PRE or POST pill.
            d.width = 104.0f;
            d.finish = Finish::anodised;
            d.top = isPost (id) ? juce::Colour (0xff9a2a3b) : juce::Colour (0xff3a6fc4);
            d.bottom = isPost (id) ? juce::Colour (0xff6c1726) : juce::Colour (0xff24509a);
            d.fleck = juce::Colours::white;
            d.ink = juce::Colour (0xfffbfbfb);
            d.knobStyle = KnobStyle::skirted;
            d.knobs = { { 28.0f, y0 + 24.0f, 19.0f, 20.0f * deg, "THRESH" }, { 76.0f, y0 + 24.0f, 19.0f, -30.0f * deg, "RATIO" },
                        { 28.0f, y0 + 58.0f, 19.0f, -90.0f * deg, "ATTACK" }, { 76.0f, y0 + 58.0f, 19.0f, 45.0f * deg, "RELEASE" } };
            d.led = { 52.0f, y0 + 41.0f };
            d.footswitch = { 52.0f, y0 + 132.0f };
            break;
        case BlockId::boost:
            // Bare brushed aluminium with four screws, one big knob, a mode toggle, a heavy slanted name.
            d.width = 92.0f;
            d.finish = Finish::brushed;
            d.top = juce::Colour (0xffd2d6da);
            d.bottom = juce::Colour (0xffa6abb1);
            d.fleck = juce::Colours::white;
            d.ink = juce::Colour (0xff15171a);
            d.knobStyle = KnobStyle::skirted;
            d.knobs = { { 46.0f, y0 + 32.0f, 33.0f, 50.0f * deg, "LEVEL" } };
            d.led = { 24.0f, y0 + 70.0f };
            d.footswitch = { 46.0f, y0 + 131.0f };
            d.screws = true;
            d.dcJack = false;
            break;
        case BlockId::overdrive:
            // Copper metal-flake, three cream knobs in a triangle (Drive above), a serif italic name.
            d.width = 104.0f;
            d.finish = Finish::metalFlake;
            d.top = juce::Colour (0xffa75e3a);
            d.bottom = juce::Colour (0xff753a20);
            d.fleck = juce::Colour (0xffffc58a);
            d.ink = juce::Colour (0xfff7ead0);
            d.knobStyle = KnobStyle::cream;
            d.knobs = { { 52.0f, y0 + 28.0f, 25.0f, 35.0f * deg, "DRIVE" }, { 22.0f, y0 + 50.0f, 18.0f, -20.0f * deg, "LEVEL" },
                        { 82.0f, y0 + 50.0f, 18.0f, 10.0f * deg, "TONE" } };
            d.led = { 52.0f, y0 + 64.0f };
            d.footswitch = { 52.0f, y0 + 132.0f };
            break;
        case BlockId::preEq:
        case BlockId::postEq:
            // Nine sliders (the graphic EQ's bands), wide; arctic white powder before the amp, gloss black after it.
            d.width = 132.0f;
            d.finish = isPost (id) ? Finish::gloss : Finish::powder;
            d.top = isPost (id) ? juce::Colour (0xff2b2d31) : juce::Colour (0xfff1f0ec);
            d.bottom = isPost (id) ? juce::Colour (0xff131416) : juce::Colour (0xffd8d6cf);
            d.fleck = juce::Colours::white;
            d.ink = isPost (id) ? juce::Colour (0xfff2f2ef) : juce::Colour (0xff1a1c1f);
            d.led = { 110.0f, y0 + 83.0f };
            d.footswitch = { 66.0f, y0 + 132.0f };
            d.bevel = isPost (id) ? 1.4f : 0.8f;
            break;
        case BlockId::gateB:
            // Olive drab with four screws, one chicken-head on a dial, stencilled capitals, a black topper.
            d.width = 92.0f;
            d.finish = Finish::matte;
            d.top = juce::Colour (0xff626a44);
            d.bottom = juce::Colour (0xff474d30);
            d.fleck = juce::Colour (0xffd8d2b0);
            d.ink = juce::Colour (0xffeee8cc);
            d.knobStyle = KnobStyle::chickenHead;
            d.knobColour = juce::Colour (0xff17171a);
            d.pointer = juce::Colour (0xfff0ead2);
            d.knobs = { { 46.0f, y0 + 34.0f, 30.0f, -25.0f * deg, "THRESHOLD" } };
            d.led = { 46.0f, y0 + 70.0f };
            d.footswitch = { 46.0f, y0 + 131.0f };
            d.topper = juce::Colour (0xff1b1c1e);
            d.screws = true;
            d.dcJack = false;
            break;
        case BlockId::harmonizer:
            // Royal purple glitter, wide, four chrome domes in a row, a stave with a chord, tracked light capitals.
            d.width = 150.0f;
            d.finish = Finish::sparkle;
            d.top = juce::Colour (0xff5a339a);
            d.bottom = juce::Colour (0xff371d66);
            d.fleck = juce::Colour (0xffe6e0ff);
            d.ink = juce::Colour (0xfff3efff);
            d.knobStyle = KnobStyle::chrome;
            d.knobs = { { 27.0f, y0 + 26.0f, 20.0f, -60.0f * deg, "KEY" }, { 59.0f, y0 + 26.0f, 20.0f, 0.0f, "SCALE" },
                        { 91.0f, y0 + 26.0f, 20.0f, 40.0f * deg, "GLIDE" }, { 123.0f, y0 + 26.0f, 20.0f, 95.0f * deg, "LEVEL" } };
            d.led = { 75.0f, y0 + 106.0f };
            d.footswitch = { 75.0f, y0 + 136.0f };
            break;
        case BlockId::multivoicer:
            // Candy teal, a big white Voices knob with its numbered dial and three small ones, the name in layers.
            d.width = 140.0f;
            d.finish = Finish::candy;
            d.top = juce::Colour (0xff168a8d);
            d.bottom = juce::Colour (0xff0a5357);
            d.fleck = juce::Colour (0xffb8fff6);
            d.ink = juce::Colour (0xfffafffe);
            d.knobStyle = KnobStyle::softCap;
            d.knobColour = juce::Colour (0xffeef2f1);
            d.pointer = juce::Colour (0xff0d5c60);
            d.knobs = { { 38.0f, y0 + 36.0f, 28.0f, -45.0f * deg, "VOICES" }, { 86.0f, y0 + 24.0f, 17.0f, 30.0f * deg, "SPREAD" },
                        { 118.0f, y0 + 24.0f, 17.0f, -10.0f * deg, "MIX" }, { 102.0f, y0 + 54.0f, 17.0f, -70.0f * deg, "HPF" } };
            d.led = { 70.0f, y0 + 106.0f };
            d.footswitch = { 70.0f, y0 + 136.0f };
            break;
        case BlockId::bloom:
            // A rose pearl that shifts to lilac, three plum knobs in an arc with a small mix, a flower and a serif name.
            d.width = 104.0f;
            d.finish = Finish::pearl;
            d.top = juce::Colour (0xffeab3c6);
            d.bottom = juce::Colour (0xffc7a6dc);
            d.fleck = juce::Colour (0xfff7cfc0);
            d.ink = juce::Colour (0xff4a1637);
            d.knobStyle = KnobStyle::softCap;
            d.knobColour = juce::Colour (0xff5b1c45);
            d.pointer = juce::Colour (0xfffbe3ee);
            d.knobs = { { 22.0f, y0 + 38.0f, 18.0f, -50.0f * deg, "CRUSH" }, { 52.0f, y0 + 24.0f, 18.0f, 15.0f * deg, "PHASE" },
                        { 82.0f, y0 + 38.0f, 18.0f, 60.0f * deg, "FLANGE" }, { 52.0f, y0 + 56.0f, 12.0f, 0.0f, "MIX" } };
            d.led = { 52.0f, y0 + 106.0f };
            d.footswitch = { 52.0f, y0 + 136.0f };
            break;
        case BlockId::chorus:
            // Seafoam speckled enamel, two big machined knobs over two small ones, a name riding a wave.
            d.width = 104.0f;
            d.finish = Finish::speckle;
            d.top = juce::Colour (0xffa9ded4);
            d.bottom = juce::Colour (0xff7fbdb2);
            d.fleck = juce::Colour (0xff2f5450);
            d.ink = juce::Colour (0xff10303a);
            d.knobStyle = KnobStyle::aluminium;
            d.knobs = { { 29.0f, y0 + 26.0f, 22.0f, -30.0f * deg, "RATE" }, { 75.0f, y0 + 26.0f, 22.0f, 55.0f * deg, "DEPTH" },
                        { 29.0f, y0 + 58.0f, 14.0f, 0.0f, "MIX" }, { 75.0f, y0 + 58.0f, 14.0f, 80.0f * deg, "WIDTH" } };
            d.led = { 52.0f, y0 + 42.0f };
            d.footswitch = { 52.0f, y0 + 132.0f };
            break;
        case BlockId::delay:
            // Navy crinkle finish, wide, a big cream Time knob between Repeats and Mix, the name with its echoes.
            d.width = 132.0f;
            d.finish = Finish::wrinkle;
            d.top = juce::Colour (0xff2b3d6e);
            d.bottom = juce::Colour (0xff1a2650);
            d.fleck = juce::Colours::white;
            d.ink = juce::Colour (0xfff4e9cc);
            d.knobStyle = KnobStyle::cream;
            d.knobs = { { 66.0f, y0 + 30.0f, 29.0f, 20.0f * deg, "TIME" }, { 26.0f, y0 + 42.0f, 18.0f, -40.0f * deg, "FEEDBACK" },
                        { 106.0f, y0 + 42.0f, 18.0f, 30.0f * deg, "MIX" } };
            d.led = { 66.0f, y0 + 64.0f };
            d.footswitch = { 66.0f, y0 + 132.0f };
            d.topper = juce::Colour (0xff17181b);
            break;
        case BlockId::reverb:
            // Sandblasted ochre, wide, rings spreading from the LED, four black soft knobs at the corners, a wide light name.
            d.width = 132.0f;
            d.finish = Finish::sandblast;
            d.top = juce::Colour (0xffd2a64a);
            d.bottom = juce::Colour (0xffa9802f);
            d.fleck = juce::Colour (0xff5a3f12);
            d.ink = juce::Colour (0xff2a1c07);
            d.knobStyle = KnobStyle::softCap;
            d.knobColour = juce::Colour (0xff1d1c1a);
            d.pointer = juce::Colour (0xfff2d79a);
            d.knobs = { { 24.0f, y0 + 26.0f, 21.0f, 40.0f * deg, "DECAY" }, { 108.0f, y0 + 26.0f, 21.0f, -20.0f * deg, "MIX" },
                        { 24.0f, y0 + 62.0f, 15.0f, -60.0f * deg, "SIZE" }, { 108.0f, y0 + 62.0f, 15.0f, 0.0f, "PRE" } };
            d.led = { 66.0f, y0 + 44.0f };
            d.footswitch = { 66.0f, y0 + 132.0f };
            d.bevel = 0.8f;
            break;
        case BlockId::input:
        case BlockId::amp:
        case BlockId::cab:
        case BlockId::output:
        case BlockId::count: // not pedals: a plain enclosure, never shown on the board
            d.finish = Finish::matte;
            d.top = juce::Colour (0xff3a3c40);
            d.bottom = juce::Colour (0xff2a2c30);
            d.ink = ink;
            d.led = { 52.0f, y0 + 60.0f };
            d.footswitch = { 52.0f, y0 + 132.0f };
            break;
    }
    return d;
}

// ---- The finishes --------------------------------------------------------------------------------

/// Inside a rounded box (w x h, radius r): the distance in from its edge, and the edge's outward normal there (from
/// the box's signed distance field: straight sides away from the corners, radial in the corners).
struct Edge
{
    float inside, nx, ny;
};

Edge edgeOf (float x, float y, float w, float h, float r)
{
    const auto px = x - w * 0.5f, py = y - h * 0.5f;
    const auto qx = std::abs (px) - (w * 0.5f - r), qy = std::abs (py) - (h * 0.5f - r);
    const auto sx = px < 0.0f ? -1.0f : 1.0f, sy = py < 0.0f ? -1.0f : 1.0f;
    if (qx > 0.0f && qy > 0.0f)
    {
        const auto l = juce::jmax (1.0e-4f, std::hypot (qx, qy));
        return { r - l, sx * qx / l, sy * qy / l };
    }
    return qx > qy ? Edge { r - qx, sx, 0.0f } : Edge { r - qy, 0.0f, sy };
}

/// A ridged noise (1 - |2n - 1|): thin crests where the noise crosses its middle, for the crinkle finish.
float ridged (float x, float y, int seed) { return 1.0f - std::abs (2.0f * noise (x, y, seed) - 1.0f); }

/// The finish at a point of the body (points from its top-left; w x h), before the edge's roll.
juce::Colour finishAt (const Design& d, float x, float y, float w, float h)
{
    const auto base = vertical (d.top, d.bottom, y, h);
    switch (d.finish)
    {
        case Finish::hammertone:
        {
            // Hammered enamel: a 5.5 pt jittered grid of shallow dimples (a Worley field). Each dimple is a bowl, so
            // its lower half faces the light and its upper half away; the light term is the offset from the bowl's
            // centre dotted with the light (a bowl's slope points out from its centre). The thin ridges between
            // dimples (where the nearest two centres are equally far) catch a silvery line.
            // The hammer's blows overlap and the enamel flows, so the ridges are soft: each nearby dimple's tilt is
            // weighted by exp (-k d^2) of its distance (a smooth Voronoi), which rounds the steps between them.
            constexpr float cell = 5.0f, k = 7.0f;
            const auto fx = x / cell, fy = y / cell;
            const auto ix = (int) std::floor (fx), iy = (int) std::floor (fy);
            float sum = 0.0f, weights = 0.0f, best = 1.0e9f;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    const auto cx = (float) (ix + i) + hash (ix + i, iy + j, 7);
                    const auto cy = (float) (iy + j) + hash (ix + i, iy + j, 8);
                    const auto dx = fx - cx, dy = fy - cy, d2 = dx * dx + dy * dy;
                    const auto weight = std::exp (-k * d2);
                    sum += weight * -(dx * lightX + dy * lightY);
                    weights += weight;
                    best = juce::jmin (best, d2);
                }
            const auto tilt = sum / juce::jmax (1.0e-6f, weights);
            float v = 0.0f;
            const auto glint = flake (x, y, 1.4f, 9, 0.25f, 0.32f, v);
            auto c = shade (base, 0.24f * tilt + 0.05f * (noise (x / 9.0f, y / 9.0f, 10) - 0.5f));
            c = blend (c, d.fleck.withAlpha (0.10f * juce::jlimit (0.0f, 1.0f, 1.0f - best * 2.5f))); // the dimples' bright floors
            return blend (c, d.fleck.withAlpha (glint * 0.40f * v));
        }
        case Finish::anodised:
        {
            // Dyed aluminium brushed vertically: value noise stretched 70 to 1 along y gives the fine grooves; a
            // soft vertical sheen a third of the way across (the light's reflection, smeared along the grooves).
            const auto streak = 0.65f * (noise (x / 0.7f, y / 50.0f, 11) - 0.5f) + 0.35f * (noise (x / 0.35f, y / 9.0f, 12) - 0.5f);
            const auto band = std::exp (-juce::square ((x - 0.34f * w) / (0.22f * w)));
            return shade (base, 0.13f * streak + 0.10f * band);
        }
        case Finish::brushed:
        {
            // Bare aluminium brushed horizontally (the Forge panel's streaks), brighter, with a broad sheen across the
            // upper third.
            const auto streak = 0.7f * (noise (x / 60.0f, y / 0.7f, 13) - 0.5f) + 0.3f * (noise (x / 9.0f, y / 0.4f, 14) - 0.5f);
            const auto band = std::exp (-juce::square ((y - 0.30f * h) / (0.18f * h)));
            return shade (base, 0.24f * streak + 0.14f * band);
        }
        case Finish::metalFlake:
        {
            // Coarse metallic flake under a clear coat: 1.5 pt cells, most holding a flake of its own brightness
            // (bright ones in the flake colour, a few dark), and the clear coat's soft diagonal gloss.
            float v = 0.0f;
            const auto cover = flake (x, y, 1.8f, 15, 0.55f, 0.42f, v);
            auto c = v > 0.15f ? blend (base, d.fleck.withAlpha (cover * (0.06f + 0.62f * v * v * v)))
                               : blend (base, juce::Colours::black.withAlpha (cover * 0.30f));
            const auto t = (x * 0.55f + y) / h;
            return shade (c, 0.12f * std::exp (-juce::square ((t - 0.26f) / 0.11f)));
        }
        case Finish::powder:
        {
            // Powder coat: an orange-peel tooth (two octaves of fine value noise) and a soft sheen at the top.
            const auto peel = 0.05f * (noise (x / 1.3f, y / 1.3f, 16) - 0.5f) + 0.035f * (noise (x / 4.0f, y / 4.0f, 17) - 0.5f);
            return shade (base, peel + 0.05f * std::exp (-juce::square (y / (0.25f * h))));
        }
        case Finish::matte:
        {
            // Flat paint: a fine tooth and faint mottling.
            return shade (base, 0.05f * (noise (x / 0.9f, y / 0.9f, 18) - 0.5f) + 0.06f * (noise (x / 14.0f, y / 14.0f, 19) - 0.5f));
        }
        case Finish::gloss:
        {
            // Deep gloss: a broad diagonal sheen, a crisp narrow reflection inside it (a window), and the faintest peel.
            const auto t = (x * 0.6f + y) / h;
            const auto broad = std::exp (-juce::square ((t - 0.30f) / 0.16f));
            const auto window = std::exp (-juce::square ((t - 0.22f) / 0.012f));
            const auto peel = 0.02f * (noise (x / 1.5f, y / 1.5f, 20) - 0.5f);
            return shade (base, 0.10f * broad + 0.10f * window + peel);
        }
        case Finish::sparkle:
        {
            // Glitter: a dense layer of tiny dim flakes, and over it sparser larger ones, each silver, pink, or ice
            // blue by its value and mostly dim, a few catching the light (brightness to the fourth power); and a
            // clear coat's gloss.
            float v = 0.0f, u = 0.0f;
            const auto fine = flake (x, y, 1.1f, 21, 0.55f, 0.28f, u);
            auto c = blend (base, d.fleck.withAlpha (fine * 0.10f * u));
            const auto cover = flake (x, y, 2.2f, 22, 0.42f, 0.48f, v);
            const auto hue = v < 0.5f ? d.fleck : v < 0.75f ? juce::Colour (0xffff9fd8) : juce::Colour (0xff9fe4ff);
            const auto bright = std::fmod (v * 7.31f, 1.0f);
            c = blend (c, hue.withAlpha (cover * (0.08f + 0.80f * juce::square (bright * bright))));
            const auto t = (x * 0.5f + y) / h;
            return shade (c, 0.07f * std::exp (-juce::square ((t - 0.28f) / 0.14f)));
        }
        case Finish::candy:
        {
            // Candy: a fine metallic base glinting through the translucent colour, deeper at the bottom, and a
            // wet gloss (a broad sheen and a crisp reflection).
            float v = 0.0f;
            const auto cover = flake (x, y, 1.1f, 23, 0.6f, 0.35f, v);
            auto c = blend (base, d.fleck.withAlpha (cover * 0.22f * v));
            const auto t = (x * 0.6f + y) / h;
            c = shade (c, 0.13f * std::exp (-juce::square ((t - 0.30f) / 0.15f)) + 0.12f * std::exp (-juce::square ((t - 0.21f) / 0.012f)));
            return c;
        }
        case Finish::pearl:
        {
            // Pearl: the colour drifts between rose (top), lilac (bottom) and peach (the flake colour) along slow
            // diagonal waves bent by noise, with a fine mica of tiny white specks.
            const auto drift = 0.5f + 0.5f * std::sin ((x * 0.7f + y) / 30.0f + 3.0f * noise (x / 40.0f, y / 40.0f, 24));
            auto c = base.interpolatedWith (d.fleck, 0.45f * drift * (1.0f - y / h));
            c = shade (c, 0.05f * (noise (x / 6.0f, y / 6.0f, 25) - 0.5f));
            float v = 0.0f;
            const auto mica = flake (x, y, 1.0f, 26, 0.35f, 0.3f, v);
            c = blend (c, juce::Colours::white.withAlpha (mica * 0.30f * v));
            const auto t = (x * 0.5f + y) / h;
            return shade (c, 0.10f * std::exp (-juce::square ((t - 0.28f) / 0.16f)));
        }
        case Finish::speckle:
        {
            // Speckled enamel: sparse dark specks and fewer light ones on a satin sheen.
            float v = 0.0f;
            auto c = shade (base, 0.03f * (noise (x / 2.0f, y / 2.0f, 27) - 0.5f));
            c = blend (c, d.fleck.withAlpha (0.65f * flake (x, y, 3.2f, 28, 0.16f, 0.36f, v)));
            c = blend (c, juce::Colours::white.withAlpha (0.55f * flake (x + 1.3f, y + 0.7f, 2.7f, 29, 0.10f, 0.3f, v)));
            return shade (c, 0.06f * std::exp (-juce::square ((y - 0.22f * h) / (0.25f * h))));
        }
        case Finish::wrinkle:
        {
            // Crinkle: a height field of ridged noise (2.6 pt and 1.2 pt octaves) lit by its slope. The surface normal
            // is (-dh/dx, -dh/dy, 1), so a facet faces the light from the top left when its height rises to the right
            // and downwards: the light term is the gradient dotted with minus the light's direction.
            const auto height = [] (float u, float v) { return ridged (u / 2.6f, v / 2.6f, 30) + 0.45f * ridged (u / 1.2f, v / 1.2f, 31); };
            constexpr float e = 0.35f;
            const auto dhdx = (height (x + e, y) - height (x - e, y)) / (2.0f * e);
            const auto dhdy = (height (x, y + e) - height (x, y - e)) / (2.0f * e);
            const auto lit = juce::jlimit (-1.0f, 1.0f, -(dhdx * lightX + dhdy * lightY) * 0.55f);
            return shade (base, 0.20f * lit + 0.05f * (height (x, y) - 0.9f));
        }
        case Finish::sandblast:
        {
            // Sandblasted: per-point grain (a hash at 0.5 pt) over two octaves of fine noise, dry and matte.
            const auto grain = hash ((int) std::floor (x * 2.0f), (int) std::floor (y * 2.0f), 32) - 0.5f;
            const auto tooth = noise (x / 0.8f, y / 0.8f, 33) - 0.5f;
            return shade (base, 0.07f * grain + 0.06f * tooth + 0.05f * (noise (x / 10.0f, y / 10.0f, 34) - 0.5f));
        }
    }
    return base;
}

/// The finish with the edge's roll: within 5 pt of the edge the face curves over, so the top edge catches the light,
/// the bottom falls into shadow, and the last point darkens (the corner's occlusion).
juce::Colour faceAt (const Design& d, float x, float y, float w, float h)
{
    auto c = finishAt (d, x, y, w, h);
    const auto e = edgeOf (x, y, w, h, bodyRadius);
    const auto t = juce::jlimit (0.0f, 1.0f, 1.0f - e.inside / 5.0f);
    if (t > 0.0f)
    {
        const auto roll = t * t;
        const auto lit = e.nx * lightX + e.ny * lightY; // the edge's normal towards the light: +1 facing it
        c = shade (c, d.bevel * (0.24f * roll * lit) - 0.22f * roll * roll * t);
    }
    return c;
}

// ---- The hardware --------------------------------------------------------------------------------

/// A soft contact shadow under a round part.
void partShadow (juce::Graphics& g, juce::Point<float> c, float radius, float drop, float alpha)
{
    juce::Path disc;
    disc.addEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c.translated (0.0f, drop)));
    juce::DropShadow (juce::Colours::black.withAlpha (alpha), juce::jmax (2, juce::roundToInt (radius * 0.45f)), {}).drawForPath (g, disc);
    g.setColour (juce::Colours::black.withAlpha (alpha * 0.8f));
    g.fillPath (disc);
}

/// Polished chrome on a disc: a radial gradient whose rings of light and dark stand for a dome reflecting the room.
juce::ColourGradient chromeGradient (juce::Point<float> c, float r)
{
    const juce::Point<float> hot { c.x - 0.35f * r, c.y - 0.40f * r };
    juce::ColourGradient grad (juce::Colours::white, hot, juce::Colour (0xff2c3034), hot.translated (r * 1.65f, 0.0f), true);
    grad.addColour (0.18, juce::Colour (0xffe9edf0));
    grad.addColour (0.42, juce::Colour (0xff8a9096));
    grad.addColour (0.62, juce::Colour (0xffd5dade));
    grad.addColour (0.82, juce::Colour (0xff5d6268));
    return grad;
}

juce::Point<float> polar (juce::Point<float> c, float radius, float angle)
{
    return { c.x + radius * std::sin (angle), c.y - radius * std::cos (angle) };
}

void pointerLine (juce::Graphics& g, juce::Point<float> c, float from, float to, float angle, float thickness, juce::Colour colour)
{
    juce::Path p;
    p.startNewSubPath (polar (c, from, angle));
    p.lineTo (polar (c, to, angle));
    g.setColour (colour);
    g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawKnob (juce::Graphics& g, const Design& design, const KnobSpec& k, float scale)
{
    const juce::Point<float> c { k.x, k.y };
    const auto r = k.d * 0.5f;
    if (design.knobStyle != KnobStyle::chickenHead) // (the chicken-head casts its own shape's shadow)
        partShadow (g, c, r * 1.02f, r * 0.16f, 0.5f);
    const auto disc = [] (juce::Point<float> at, float radius) { return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (at); };

    switch (design.knobStyle)
    {
        case KnobStyle::aluminium:
        {
            // A knurled rim (64 teeth, alternately lit and dark round a steel ring), then the spun top: concentric
            // machining rings (a fast sine of the radius) under the bow-tie sheen spun metal shows (a cos 2 theta
            // lobe aimed at the light).
            juce::ColourGradient rim (juce::Colour (0xffe8ecef), c.x - r, c.y - r, juce::Colour (0xff60666c), c.x + r, c.y + r, false);
            g.setGradientFill (rim);
            g.fillEllipse (disc (c, r));
            g.setColour (juce::Colour (0xff3e4247).withAlpha (0.55f));
            for (int t = 0; t < 64; ++t)
            {
                const auto a = juce::MathConstants<float>::twoPi * (float) t / 64.0f;
                juce::Path tooth;
                tooth.startNewSubPath (polar (c, r * 0.83f, a));
                tooth.lineTo (polar (c, r * 0.99f, a));
                g.strokePath (tooth, juce::PathStrokeType (juce::jmax (0.35f, r * 0.035f)));
            }
            const auto topR = r * 0.80f;
            juce::Path top;
            top.addEllipse (disc (c, topR));
            const auto area = disc (c, topR);
            const auto light = std::atan2 (lightY, lightX);
            fillPattern (g, top, area, scale, [topR, light] (float x, float y)
            {
                const auto dx = x - topR, dy = y - topR;
                const auto rr = std::hypot (dx, dy), theta = std::atan2 (dy, dx);
                const auto sheen = 0.5f + 0.5f * std::cos (2.0f * (theta - light - juce::MathConstants<float>::halfPi));
                const auto rings = 0.5f + 0.5f * std::sin (rr * 7.0f);
                const auto base = juce::Colour (0xff9aa1a7).interpolatedWith (juce::Colour (0xffeef1f3), sheen);
                return shade (base, 0.06f * (rings - 0.5f));
            });
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.drawEllipse (area, juce::jmax (0.4f, r * 0.03f));
            pointerLine (g, c, r * 0.18f, r * 0.74f, k.angle, juce::jmax (0.8f, r * 0.10f), juce::Colour (0xff1b1d20));
            break;
        }
        case KnobStyle::skirted:
        {
            // A dark skirt with a white line at the pointer, the knob above it (fluted edge, domed top lit from the top
            // left), and a white line on its top.
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2e3034), c.x, c.y - r, juce::Colour (0xff0d0e10), c.x, c.y + r, false));
            g.fillEllipse (disc (c, r));
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawEllipse (disc (c, r - 0.4f), 0.6f);
            pointerLine (g, c, r * 0.76f, r * 0.97f, k.angle, juce::jmax (0.7f, r * 0.08f), juce::Colour (0xfff2f2f2));
            const auto bodyR = r * 0.70f;
            g.setColour (juce::Colour (0xff050506));
            g.fillEllipse (disc (c.translated (0.0f, r * 0.05f), bodyR + 0.4f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4c51), c.x - bodyR * 0.6f, c.y - bodyR * 0.7f, juce::Colour (0xff111214), c.x + bodyR * 0.6f,
                                                     c.y + bodyR, false));
            g.fillEllipse (disc (c, bodyR));
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            for (int t = 0; t < 28; ++t)
            {
                const auto a = juce::MathConstants<float>::twoPi * (float) t / 28.0f;
                juce::Path flute;
                flute.startNewSubPath (polar (c, bodyR * 0.80f, a));
                flute.lineTo (polar (c, bodyR * 0.98f, a));
                g.strokePath (flute, juce::PathStrokeType (juce::jmax (0.4f, bodyR * 0.06f)));
            }
            juce::ColourGradient cap (juce::Colour (0xff55585e), c.x - bodyR * 0.4f, c.y - bodyR * 0.5f, juce::Colour (0xff17181b), c.x + bodyR * 0.5f, c.y + bodyR * 0.6f, true);
            g.setGradientFill (cap);
            g.fillEllipse (disc (c, bodyR * 0.78f));
            pointerLine (g, c, bodyR * 0.15f, bodyR * 0.72f, k.angle, juce::jmax (0.8f, r * 0.09f), juce::Colour (0xfff4f4f4));
            break;
        }
        case KnobStyle::cream:
        {
            // Cream plastic: a thin dark base ring, a domed top (light at the top left, warm shadow at the bottom
            // right), and a black line.
            g.setColour (juce::Colour (0xff2a241b));
            g.fillEllipse (disc (c, r));
            juce::ColourGradient body (juce::Colour (0xfffff9ea), c.x - r * 0.45f, c.y - r * 0.5f, juce::Colour (0xffa8987a), c.x + r * 0.6f, c.y + r * 0.8f, true);
            body.addColour (0.45, juce::Colour (0xffeee3c8));
            g.setGradientFill (body);
            g.fillEllipse (disc (c, r * 0.90f));
            g.setColour (juce::Colour (0x26000000));
            g.drawEllipse (disc (c, r * 0.68f), juce::jmax (0.4f, r * 0.04f));
            pointerLine (g, c, r * 0.12f, r * 0.80f, k.angle, juce::jmax (0.9f, r * 0.11f), juce::Colour (0xff1c1a16));
            break;
        }
        case KnobStyle::chickenHead:
        {
            // A bakelite chicken-head: a round boss with a tapered pointer, a cream line along it, and a dial of 11
            // dots round it printed on the enclosure.
            for (int t = 0; t <= 10; ++t)
            {
                const auto a = (-135.0f + 27.0f * (float) t) * deg;
                g.setColour (design.ink.withAlpha (t == 0 || t == 10 ? 0.95f : 0.7f));
                const auto dotR = t % 5 == 0 ? 1.0f : 0.65f;
                g.fillEllipse (disc (polar (c, r * 1.22f, a), dotR));
            }
            juce::Path pointer;
            const auto tip = polar (c, r * 0.98f, k.angle), tail = polar (c, r * 0.62f, k.angle + juce::MathConstants<float>::pi);
            const auto side = [&] (float s, float rr) { return polar (c, rr, k.angle + s * juce::MathConstants<float>::halfPi); };
            pointer.startNewSubPath (tip);
            pointer.quadraticTo (side (1.0f, r * 0.30f), side (1.0f, r * 0.52f));
            pointer.quadraticTo (tail, side (-1.0f, r * 0.52f));
            pointer.quadraticTo (side (-1.0f, r * 0.30f), tip);
            pointer.closeSubPath();
            pointer.addEllipse (disc (c, r * 0.52f));
            pointer.setUsingNonZeroWinding (true);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), juce::jmax (2, juce::roundToInt (r * 0.30f)), { 0, juce::roundToInt (r * 0.18f) })
                .drawForPath (g, pointer);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3c3d42), c.x - r * 0.5f, c.y - r * 0.6f, design.knobColour, c.x + r * 0.4f, c.y + r * 0.6f, false));
            g.fillPath (pointer);
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.strokePath (pointer, juce::PathStrokeType (0.5f));
            juce::ColourGradient boss (juce::Colour (0xff4b4d53), c.x - r * 0.2f, c.y - r * 0.25f, juce::Colour (0xff0e0e10), c.x + r * 0.35f, c.y + r * 0.4f, true);
            g.setGradientFill (boss);
            g.fillEllipse (disc (c, r * 0.36f));
            pointerLine (g, c, r * 0.10f, r * 0.86f, k.angle, juce::jmax (0.8f, r * 0.07f), design.pointer);
            break;
        }
        case KnobStyle::chrome:
        {
            // A polished dome: a dark collar, the chrome, a black dot near the edge at the pointer.
            g.setColour (juce::Colour (0xff15171a));
            g.fillEllipse (disc (c, r));
            g.setGradientFill (chromeGradient (c, r * 0.9f));
            g.fillEllipse (disc (c, r * 0.9f));
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.drawEllipse (disc (c, r * 0.9f), 0.5f);
            g.setColour (juce::Colour (0xff121315));
            g.fillEllipse (disc (polar (c, r * 0.62f, k.angle), juce::jmax (0.9f, r * 0.12f)));
            break;
        }
        case KnobStyle::softCap:
        {
            // Soft-touch rubber: the side a step darker, the flat top with a faint radial falloff, and a line in the
            // pointer colour.
            g.setColour (design.knobColour.darker (0.55f));
            g.fillEllipse (disc (c.translated (0.0f, r * 0.06f), r));
            juce::ColourGradient top (design.knobColour.brighter (0.25f), c.x - r * 0.4f, c.y - r * 0.5f, design.knobColour.darker (0.25f), c.x + r * 0.7f,
                                      c.y + r * 0.8f, true);
            g.setGradientFill (top);
            g.fillEllipse (disc (c, r * 0.93f));
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawEllipse (disc (c, r * 0.90f), 0.5f);
            pointerLine (g, c, r * 0.20f, r * 0.80f, k.angle, juce::jmax (0.9f, r * 0.11f), design.pointer);
            break;
        }
    }
}

/// Text in the silkscreen's small capitals, centred on a point (its cap height centred on y).
void silk (juce::Graphics& g, const juce::String& text, juce::Point<float> c, juce::Colour colour, float size = 5.4f, float tracking = 0.14f)
{
    const auto f = geist (Weight::semibold, size);
    auto p = lettering (text, f, tracking * size);
    p = placed (p, c.x, c.y + capHeight (f) * 0.5f);
    g.setColour (colour);
    g.fillPath (p);
}

/// A 1/4 inch jack standing out of the body's top edge, seen from above: its hex nut (three lit faces) and the
/// threaded sleeve beyond it. Drawn before the body, so it comes out from under the edge.
void drawJack (juce::Graphics& g, float x)
{
    const auto nut = juce::Rectangle<float> (x - 6.5f, jackRise - 4.2f, 13.0f, 5.5f);
    const auto faces = std::array<juce::Colour, 3> { juce::Colour (0xffd9dde0), juce::Colour (0xffa3a9ae), juce::Colour (0xff5d6268) };
    for (int f = 0; f < 3; ++f)
    {
        g.setColour (faces[(size_t) f]);
        g.fillRect (nut.getX() + (float) f * nut.getWidth() / 3.0f, nut.getY(), nut.getWidth() / 3.0f + 0.05f, nut.getHeight());
    }
    const auto sleeve = juce::Rectangle<float> (x - 4.3f, 0.6f, 8.6f, nut.getY() - 0.6f + 0.2f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe6e9ec), sleeve.getX(), 0.0f, juce::Colour (0xff53585e), sleeve.getRight(), 0.0f, false));
    g.fillRect (sleeve);
    g.setColour (juce::Colours::black.withAlpha (0.30f));
    for (auto tx = sleeve.getX() + 0.8f; tx < sleeve.getRight(); tx += 1.1f)
        g.fillRect (tx, sleeve.getY(), 0.35f, sleeve.getHeight());
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawRect (nut, 0.4f);
}

/// The DC jack: a black plastic barrel with a thin metal band.
void drawDcJack (juce::Graphics& g, float x)
{
    const auto barrel = juce::Rectangle<float> (x - 4.5f, 1.5f, 9.0f, jackRise);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3c40), barrel.getX(), 0.0f, juce::Colour (0xff0c0d0e), barrel.getRight(), 0.0f, false));
    g.fillRoundedRectangle (barrel, 1.0f);
    g.setColour (juce::Colour (0xffa9aeb3));
    g.fillRect (barrel.getX(), barrel.getY() + 1.2f, barrel.getWidth(), 0.7f);
}

/// A cross-head screw in the face.
void drawScrew (juce::Graphics& g, juce::Point<float> c)
{
    const auto r = 2.9f;
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (juce::Rectangle<float> (r * 2.0f + 0.8f, r * 2.0f + 0.8f).withCentre (c.translated (0.0f, 0.5f)));
    juce::ColourGradient dome (juce::Colour (0xfff0f2f4), c.x - r * 0.6f, c.y - r * 0.7f, juce::Colour (0xff52575c), c.x + r, c.y + r, true);
    g.setGradientFill (dome);
    g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
    g.setColour (juce::Colour (0xff23262a));
    const auto a = 0.5f;
    for (const auto angle : { a, a + juce::MathConstants<float>::halfPi })
    {
        juce::Path slot;
        slot.startNewSubPath (polar (c, r * 0.62f, angle));
        slot.lineTo (polar (c, r * 0.62f, angle + juce::MathConstants<float>::pi));
        g.strokePath (slot, juce::PathStrokeType (0.75f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

/// The LED's chrome bezel and its dark lens (the off state, in the accent's own tint); drawPedal lights it.
void drawLedBezel (juce::Graphics& g, juce::Point<float> c)
{
    const auto disc = [&] (float radius) { return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c); };
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (disc (ledBezel + 0.6f).translated (0.0f, 0.6f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff4f6f8), c.x - ledBezel, c.y - ledBezel, juce::Colour (0xff4b5055), c.x + ledBezel, c.y + ledBezel, false));
    g.fillEllipse (disc (ledBezel));
    g.setColour (juce::Colour (0xff0a0b0c));
    g.fillEllipse (disc (ledLens + 0.55f));
    juce::ColourGradient lens (accent.withMultipliedBrightness (0.42f).withMultipliedSaturation (0.7f), c.x - 0.8f, c.y - 0.9f, juce::Colour (0xff120c08), c.x + ledLens,
                               c.y + ledLens, true);
    g.setGradientFill (lens);
    g.fillEllipse (disc (ledLens));
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.fillEllipse (juce::Rectangle<float> (1.3f, 0.9f).withCentre (c.translated (-0.9f, -1.1f)));
}

/// The stomp switch: a hex nut on the face and the domed chrome button in its bushing, or a rubber topper over it.
void drawFootswitch (juce::Graphics& g, juce::Point<float> c, juce::Colour topper)
{
    partShadow (g, c, nutRadius * 0.92f, 1.6f, 0.45f);
    juce::Path hex;
    for (int k = 0; k < 6; ++k)
    {
        const auto a = juce::MathConstants<float>::twoPi * (float) k / 6.0f + juce::MathConstants<float>::pi / 6.0f;
        const juce::Point<float> v { c.x + nutRadius * std::cos (a), c.y + nutRadius * std::sin (a) };
        k == 0 ? hex.startNewSubPath (v) : hex.lineTo (v);
    }
    hex.closeSubPath();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffeef1f3), c.x - nutRadius, c.y - nutRadius, juce::Colour (0xff4a4f55), c.x + nutRadius, c.y + nutRadius, false));
    g.fillPath (hex);
    // The nut's chamfer: a lighter ring where the flats are cut round, and its dark edge.
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawEllipse (juce::Rectangle<float> (nutRadius * 1.66f, nutRadius * 1.66f).withCentre (c), 0.6f);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.strokePath (hex, juce::PathStrokeType (0.5f));

    const auto disc = [&] (float radius, juce::Point<float> at) { return juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (at); };
    if (topper.isTransparent())
    {
        g.setColour (juce::Colour (0xff111214));
        g.fillEllipse (disc (7.6f, c));
        g.setGradientFill (chromeGradient (c, 6.6f));
        g.fillEllipse (disc (6.6f, c));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawEllipse (disc (6.6f, c), 0.5f);
    }
    else
    {
        // A rubber topper: wider than the button, its side darker, its top a soft dome with a fine ribbed ring.
        g.setColour (topper.darker (0.6f));
        g.fillEllipse (disc (9.6f, c.translated (0.0f, 0.8f)));
        juce::ColourGradient top (topper.brighter (0.55f), c.x - 3.5f, c.y - 4.5f, topper.darker (0.2f), c.x + 7.0f, c.y + 8.0f, true);
        g.setGradientFill (top);
        g.fillEllipse (disc (9.0f, c));
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        for (const auto rr : { 3.0f, 5.0f, 7.0f })
            g.drawEllipse (disc (rr, c), 0.5f);
    }
}

/// A mini toggle: its bushing nut and a bat lever leaning towards `angle`.
void drawToggle (juce::Graphics& g, juce::Point<float> c, float angle)
{
    partShadow (g, c, 3.6f, 0.8f, 0.4f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe6e9ec), c.x - 3.6f, c.y - 3.6f, juce::Colour (0xff50555b), c.x + 3.6f, c.y + 3.6f, false));
    g.fillEllipse (juce::Rectangle<float> (7.2f, 7.2f).withCentre (c));
    g.setColour (juce::Colour (0xff15171a));
    g.fillEllipse (juce::Rectangle<float> (3.4f, 3.4f).withCentre (c));
    const auto tip = polar (c, 6.0f, angle);
    juce::Path bat;
    bat.startNewSubPath (c);
    bat.lineTo (tip);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.strokePath (bat, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), juce::AffineTransform::translation (0.6f, 1.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff5f7f8), c.x - 1.5f, c.y, juce::Colour (0xff70757b), c.x + 1.5f, c.y, false));
    g.strokePath (bat, juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setGradientFill (chromeGradient (tip, 1.5f));
    g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (tip));
}

/// A fader: a dark slot with ticks either side, and its cap at `position` (0 bottom, 1 top).
void drawFader (juce::Graphics& g, float x, float top, float bottom, float position, juce::Colour ink)
{
    g.setColour (ink.withAlpha (0.45f));
    for (int t = 0; t <= 4; ++t)
    {
        const auto y = juce::jmap ((float) t / 4.0f, top, bottom);
        const auto half = t == 2 ? 3.2f : 2.4f;
        g.fillRect (x - 2.0f - half, y - 0.25f, half, 0.5f);
        g.fillRect (x + 2.0f, y - 0.25f, half, 0.5f);
    }
    const auto slot = juce::Rectangle<float> (x - 1.1f, top - 1.0f, 2.2f, bottom - top + 2.0f);
    g.setColour (juce::Colour (0xff050506));
    g.fillRoundedRectangle (slot, 1.1f);
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.fillRect (slot.getX(), slot.getBottom() - 0.4f, slot.getWidth(), 0.4f);
    const auto y = juce::jmap (position, bottom, top);
    const auto cap = juce::Rectangle<float> (8.4f, 5.6f).withCentre ({ x, y });
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (cap.translated (0.3f, 1.0f).expanded (0.4f), 1.4f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a4c51), 0.0f, cap.getY(), juce::Colour (0xff141518), 0.0f, cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 1.2f);
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.fillRect (cap.getX() + 1.0f, cap.getY() + 0.4f, cap.getWidth() - 2.0f, 0.5f);
    g.setColour (juce::Colour (0xfff2f2f2));
    g.fillRect (cap.getX() + 0.8f, y - 0.35f, cap.getWidth() - 1.6f, 0.7f);
}

// ---- The names -----------------------------------------------------------------------------------

/// The PRE or POST mark on a twin: a small rounded pill in the ink, the word knocked out in the body's colour.
void drawMark (juce::Graphics& g, BlockId id, juce::Point<float> c, juce::Colour ink, juce::Colour body)
{
    const auto word = juce::String (isPost (id) ? "POST" : "PRE");
    const auto f = geist (Weight::semibold, 5.6f);
    auto p = lettering (word, f, 0.16f * 5.6f);
    const auto w = p.getBounds().getWidth() + 6.0f;
    const auto pill = juce::Rectangle<float> (w, 8.0f).withCentre (c);
    g.setColour (ink);
    g.fillRoundedRectangle (pill, 4.0f);
    p = placed (p, c.x, c.y + capHeight (f) * 0.5f);
    g.setColour (body);
    g.fillPath (p);
}

/// The name's lettering (the main ink, in place) and its decoration (what goes with it in other inks or alphas).
struct Name
{
    juce::Path ink;
    std::function<void (juce::Graphics&)> under, over; // drawn before and after the ink
};

Name nameFor (BlockId id, const Design& d)
{
    Name n;
    const auto cx = d.width * 0.5f, y0 = jackRise;
    switch (id)
    {
        case BlockId::gateA:
        {
            // Wide semibold capitals tracked out, "GATE" and its letter in a ruled box.
            const auto f = geist (Weight::semibold, 13.5f);
            auto gate = lettering ("GATE", f, 0.16f * 13.5f, 1.22f);
            auto a = lettering ("A", f, 0.0f, 1.22f);
            const auto caps = capHeight (f);
            const auto box = juce::Rectangle<float> (a.getBounds().getWidth() + 6.0f, caps + 6.0f);
            const auto total = gate.getBounds().getWidth() + 5.0f + box.getWidth();
            const auto left = cx - total * 0.5f, baseline = y0 + 92.0f;
            gate.applyTransform (juce::AffineTransform::translation (left, baseline));
            const auto boxAt = box.withPosition (left + gate.getBounds().getWidth() + 5.0f, baseline - caps - 3.0f);
            a.applyTransform (juce::AffineTransform::translation (boxAt.getCentreX() - a.getBounds().getWidth() * 0.5f, baseline));
            n.ink = gate;
            n.ink.addPath (a);
            n.over = [boxAt, ink = d.ink] (juce::Graphics& g)
            {
                g.setColour (ink);
                g.drawRoundedRectangle (boxAt, 1.5f, 1.1f);
            };
            break;
        }
        case BlockId::preCompressor:
        case BlockId::postCompressor:
        {
            // Light lowercase, slightly condensed.
            const auto f = geist (Weight::light, 16.0f);
            n.ink = placed (lettering ("compressor", f, 0.01f * 16.0f, 0.90f), cx, y0 + 97.0f);
            n.over = [id, d, cx, y0] (juce::Graphics& g) { drawMark (g, id, { cx, y0 + 108.0f }, d.ink, d.bottom.interpolatedWith (d.top, 0.4f)); };
            break;
        }
        case BlockId::boost:
        {
            // Heavy capitals slanted forward, over a thick and a thin rule.
            const auto f = geist (Weight::semibold, 21.0f);
            n.ink = placed (lettering ("BOOST", f, 0.04f * 21.0f, 1.04f, 0.20f), cx, y0 + 97.0f);
            const auto b = n.ink.getBounds();
            n.ink.addRectangle (b.getX() - 1.0f, b.getBottom() + 3.0f, b.getWidth() - 1.0f, 1.7f);
            n.ink.addRectangle (b.getX() - 1.6f, b.getBottom() + 6.0f, b.getWidth() - 1.0f, 0.7f);
            break;
        }
        case BlockId::overdrive:
        {
            // A serif italic in cream with a dark drop under it.
            const auto f = fraunces (21.0f);
            n.ink = placed (lettering ("Overdrive", f, 0.0f), cx, y0 + 95.0f);
            const auto p = n.ink;
            n.under = [p] (juce::Graphics& g)
            {
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillPath (p, juce::AffineTransform::translation (0.0f, 1.2f));
            };
            break;
        }
        case BlockId::preEq:
        case BlockId::postEq:
        {
            // Medium capitals tracked very wide, with the mark at the left and the LED at the right.
            const auto f = geist (Weight::medium, 22.0f);
            n.ink = placed (lettering ("EQ", f, 0.24f * 22.0f), cx, y0 + 91.0f);
            n.over = [id, d, y0] (juce::Graphics& g) { drawMark (g, id, { 22.0f, y0 + 83.0f }, d.ink, d.top.interpolatedWith (d.bottom, 0.5f)); };
            break;
        }
        case BlockId::gateB:
        {
            // Stencilled capitals: the letters broken by a bridge across their middle (cut when the ink is filled:
            // drawName's clip).
            const auto f = geist (Weight::semibold, 15.0f);
            n.ink = placed (lettering ("GATE B", f, 0.12f * 15.0f, 1.12f), cx, y0 + 94.0f);
            break;
        }
        case BlockId::harmonizer:
        {
            // Light capitals tracked far apart between two hairline rules.
            const auto f = geist (Weight::light, 11.5f);
            n.ink = placed (lettering ("HARMONIZER", f, 0.30f * 11.5f), cx, y0 + 88.0f);
            const auto b = n.ink.getBounds();
            n.ink.addRectangle (b.getX(), b.getY() - 5.0f, b.getWidth(), 0.6f);
            n.ink.addRectangle (b.getX(), b.getBottom() + 4.4f, b.getWidth(), 0.6f);
            break;
        }
        case BlockId::multivoicer:
        {
            // Semibold lowercase with two more voices behind it, detuned either side: the word again in ice cyan a
            // little to the left and in deep teal a little to the right, so the white sits on a split edge of colour.
            const auto f = geist (Weight::semibold, 16.0f);
            n.ink = placed (lettering ("multivoicer", f, 0.01f * 16.0f), cx, y0 + 93.0f);
            const auto p = n.ink;
            n.under = [p] (juce::Graphics& g)
            {
                g.setColour (juce::Colour (0xff7ff0e4).withAlpha (0.85f));
                g.fillPath (p, juce::AffineTransform::translation (-1.4f, -0.3f));
                g.setColour (juce::Colour (0xff02292c).withAlpha (0.75f));
                g.fillPath (p, juce::AffineTransform::translation (1.4f, 0.6f));
            };
            break;
        }
        case BlockId::bloom:
        {
            // A big serif italic in plum, a five-petal flower before it.
            const auto f = fraunces (25.0f);
            auto p = lettering ("Bloom", f, 0.0f);
            const auto w = p.getBounds().getWidth() + 14.0f;
            const auto left = cx - w * 0.5f + 14.0f, baseline = y0 + 95.0f;
            p.applyTransform (juce::AffineTransform::translation (left, baseline));
            n.ink = p;
            const juce::Point<float> centre { left - 8.0f, baseline - capHeight (f) * 0.5f };
            for (int k = 0; k < 5; ++k)
            {
                juce::Path petal;
                petal.addEllipse (-1.9f, -6.2f, 3.8f, 5.2f);
                petal.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::twoPi * (float) k / 5.0f).translated (centre));
                n.ink.addPath (petal);
            }
            const auto fill = d.top;
            n.over = [centre, fill] (juce::Graphics& g)
            {
                g.setColour (fill);
                g.fillEllipse (juce::Rectangle<float> (2.2f, 2.2f).withCentre (centre));
            };
            break;
        }
        case BlockId::chorus:
        {
            // Medium lowercase riding a sine wave (each letter raised by its place in the wave).
            const auto f = geist (Weight::medium, 18.0f);
            n.ink = placed (lettering ("chorus", f, 0.02f * 18.0f, 1.0f, 0.0f, [] (int i) { return 2.2f * std::sin ((float) i * 1.05f); }), cx, y0 + 95.0f);
            break;
        }
        case BlockId::delay:
        {
            // Semibold capitals whose last letter repeats twice after the word, fading (the repeats); the whole mark
            // is centred.
            const auto f = geist (Weight::semibold, 16.0f);
            const auto spacing = 0.10f * 16.0f;
            auto p = lettering ("DELAY", f, spacing);
            auto y = lettering ("Y", f, 0.0f);
            const auto step = y.getBounds().getWidth() + spacing;
            const auto left = cx - (p.getBounds().getWidth() + 2.0f * step) * 0.5f, baseline = y0 + 95.0f;
            p.applyTransform (juce::AffineTransform::translation (left, baseline));
            y.applyTransform (juce::AffineTransform::translation (p.getBounds().getRight() - y.getBounds().getWidth(), baseline));
            n.ink = p;
            const auto ink = d.ink;
            n.over = [y, step, ink] (juce::Graphics& g)
            {
                g.setColour (ink.withAlpha (0.45f));
                g.fillPath (y, juce::AffineTransform::translation (step, 0.0f));
                g.setColour (ink.withAlpha (0.18f));
                g.fillPath (y, juce::AffineTransform::translation (2.0f * step, 0.0f));
            };
            break;
        }
        case BlockId::reverb:
        {
            // Light lowercase stretched wide.
            const auto f = geist (Weight::light, 19.0f);
            n.ink = placed (lettering ("reverb", f, 0.03f * 19.0f, 1.30f), cx, y0 + 97.0f);
            break;
        }
        case BlockId::input:
        case BlockId::amp:
        case BlockId::cab:
        case BlockId::output:
        case BlockId::count: // not pedals: a plain enclosure, never shown on the board
            n.ink = placed (lettering (info (id).name, geist (Weight::medium, 12.0f), 0.0f), cx, y0 + 95.0f);
            break;
    }
    return n;
}

void drawName (juce::Graphics& g, BlockId id, const Design& d)
{
    const auto n = nameFor (id, d);
    if (n.under)
        n.under (g);
    g.setColour (d.ink);
    if (id == BlockId::gateB)
    {
        // The stencil's bridge: a gap across the letters' middle.
        const auto b = n.ink.getBounds();
        juce::Path keep;
        keep.addRectangle (b.expanded (3.0f));
        keep.addRectangle (b.getX() - 2.0f, b.getCentreY() - 0.6f, b.getWidth() + 4.0f, 1.2f);
        keep.setUsingNonZeroWinding (false); // everything round the letters but the bridge
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (keep);
        g.fillPath (n.ink);
    }
    else
        g.fillPath (n.ink);
    if (n.over)
        n.over (g);
}

// ---- The silkscreen's graphics ------------------------------------------------------------------

void drawGraphics (juce::Graphics& g, BlockId id, const Design& d)
{
    const auto y0 = jackRise, cx = d.width * 0.5f;
    switch (id)
    {
        case BlockId::gateA:
        {
            // A gate's envelope under the name: a level that drops to nothing below the threshold.
            juce::Path p;
            const auto y = y0 + 104.0f;
            p.startNewSubPath (cx - 22.0f, y - 4.0f);
            p.lineTo (cx - 4.0f, y - 4.0f);
            p.lineTo (cx - 4.0f, y);
            p.lineTo (cx + 22.0f, y);
            g.setColour (d.ink.withAlpha (0.55f));
            g.strokePath (p, juce::PathStrokeType (0.7f));
            break;
        }
        case BlockId::overdrive:
        {
            // A sine with its peaks clipped flat, under the name.
            juce::Path p;
            const auto y = y0 + 106.0f;
            for (int i = 0; i <= 60; ++i)
            {
                const auto t = (float) i / 60.0f;
                const auto v = juce::jlimit (-0.6f, 0.6f, std::sin (t * juce::MathConstants<float>::twoPi * 2.0f));
                const juce::Point<float> pt { cx - 20.0f + 40.0f * t, y - 4.0f * v };
                i == 0 ? p.startNewSubPath (pt) : p.lineTo (pt);
            }
            g.setColour (d.ink.withAlpha (0.6f));
            g.strokePath (p, juce::PathStrokeType (0.8f));
            break;
        }
        case BlockId::preEq:
        case BlockId::postEq:
        {
            // The nine faders (their caps in a gentle smile) and a +, 0, - scale at the left.
            const auto top = y0 + 20.0f, bottom = y0 + 64.0f;
            const std::array<float, 9> curve { 0.72f, 0.62f, 0.52f, 0.44f, 0.40f, 0.46f, 0.55f, 0.66f, 0.74f };
            for (int b = 0; b < 9; ++b)
                drawFader (g, 26.0f + 10.0f * (float) b, top, bottom, curve[(size_t) b], d.ink);
            silk (g, "+", { 11.0f, top }, d.ink, 6.0f, 0.0f);
            silk (g, "0", { 11.0f, (top + bottom) * 0.5f }, d.ink, 5.4f, 0.0f);
            silk (g, "-", { 11.0f, bottom }, d.ink, 6.0f, 0.0f);
            break;
        }
        case BlockId::harmonizer:
        {
            // A five-line stave across the middle with a stacked chord (a root and its third and fifth above).
            const auto top = y0 + 52.0f;
            g.setColour (d.ink.withAlpha (0.45f));
            for (int l = 0; l < 5; ++l)
                g.fillRect (cx - 46.0f, top + 3.0f * (float) l, 92.0f, 0.45f);
            g.setColour (d.ink.withAlpha (0.9f));
            for (const auto& [x, step] : { std::pair<float, int> { cx - 18.0f, 6 }, { cx + 2.0f, 4 }, { cx + 2.0f, 2 }, { cx + 2.0f, 6 }, { cx + 22.0f, 3 } })
            {
                const auto y = top + 1.5f * (float) step;
                juce::Path head;
                head.addEllipse (-1.9f, -1.35f, 3.8f, 2.7f);
                head.applyTransform (juce::AffineTransform::rotation (-0.35f).translated (x, y));
                g.fillPath (head);
            }
            break;
        }
        case BlockId::multivoicer:
        {
            // The Voices knob's dial: eight dots from 1 to 8, the even ones numbered.
            const auto& k = d.knobs[0];
            for (int v = 0; v < 8; ++v)
            {
                const auto a = (-135.0f + 270.0f * (float) v / 7.0f) * deg;
                const auto at = polar ({ k.x, k.y }, k.d * 0.5f + 3.6f, a);
                g.setColour (d.ink.withAlpha (0.85f));
                g.fillEllipse (juce::Rectangle<float> (1.5f, 1.5f).withCentre (at));
                if (v % 2 == 1)
                    silk (g, juce::String (v + 1), polar ({ k.x, k.y }, k.d * 0.5f + 8.2f, a), d.ink.withAlpha (0.85f), 4.6f, 0.0f);
            }
            break;
        }
        case BlockId::chorus:
        {
            // Two sines a quarter-cycle apart under the name.
            for (const auto phase : { 0.0f, juce::MathConstants<float>::halfPi })
            {
                juce::Path p;
                for (int i = 0; i <= 60; ++i)
                {
                    const auto t = (float) i / 60.0f;
                    const juce::Point<float> pt { cx - 22.0f + 44.0f * t, y0 + 107.0f - 2.4f * std::sin (t * juce::MathConstants<float>::twoPi * 1.5f + phase) };
                    i == 0 ? p.startNewSubPath (pt) : p.lineTo (pt);
                }
                g.setColour (d.ink.withAlpha (phase == 0.0f ? 0.65f : 0.35f));
                g.strokePath (p, juce::PathStrokeType (0.7f));
            }
            break;
        }
        case BlockId::reverb:
        {
            // Rings spreading from the LED, fading as they go.
            for (int ring = 1; ring <= 5; ++ring)
            {
                const auto r = ledBezel + 4.0f * (float) ring;
                g.setColour (d.ink.withAlpha (0.42f - 0.07f * (float) ring));
                g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (d.led), 0.6f);
            }
            break;
        }
        case BlockId::delay:
        {
            // A row of shrinking taps under the name, the repeats dying away.
            for (int t = 0; t < 6; ++t)
            {
                const auto h = 6.0f * std::pow (0.7f, (float) t);
                g.setColour (d.ink.withAlpha (0.65f));
                g.fillRect (cx - 20.0f + 8.0f * (float) t, y0 + 109.0f - h, 1.2f, h);
            }
            break;
        }
        case BlockId::bloom:
        case BlockId::preCompressor:
        case BlockId::postCompressor:
        case BlockId::boost:
        case BlockId::gateB:
        case BlockId::input:
        case BlockId::amp:
        case BlockId::cab:
        case BlockId::output:
        case BlockId::count:
            break;
    }
}

// ---- The enclosure -------------------------------------------------------------------------------

juce::Rectangle<float> bodyOf (const Design& d) { return { 0.0f, jackRise, d.width, pedalHeight - jackRise }; }

juce::Image renderArt (BlockId id, float scale, bool withName)
{
    const auto d = design (id);
    juce::Image image (juce::Image::ARGB, juce::roundToInt (d.width * scale), juce::roundToInt (pedalHeight * scale), true);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));

    const auto body = bodyOf (d);
    juce::Path shape;
    shape.addRoundedRectangle (body, bodyRadius);

    // The jacks, from under the top edge: in at the left and out at the right (the board's chain runs left to right:
    // PA5), the DC jack between them.
    const auto inX = d.width * (d.width < 100.0f ? 0.24f : 0.22f), outX = d.width - inX;
    drawJack (g, inX);
    drawJack (g, outX);
    if (d.dcJack)
        drawDcJack (g, d.width * 0.5f);

    // The face and its roll.
    const auto w = body.getWidth(), h = body.getHeight();
    fillPattern (g, shape, body, scale, [&d, w, h] (float x, float y) { return faceAt (d, x, y, w, h); });
    {
        // A crisp lit line along the top inside edge, and a hairline of shadow round the whole box.
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (shape);
        juce::Path rim = shape;
        rim.addPath (shape, juce::AffineTransform::translation (0.0f, 0.8f));
        rim.setUsingNonZeroWinding (false);
        g.setColour (juce::Colours::white.withAlpha (0.30f));
        g.fillPath (rim);
    }
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.strokePath (shape, juce::PathStrokeType (0.6f));

    // The silkscreen: the jacks' labels, the graphics, the knobs' labels.
    const auto silkInk = d.ink.withAlpha (0.88f);
    silk (g, "IN", { inX, jackRise + 6.0f }, silkInk, 4.8f);
    silk (g, "OUT", { outX, jackRise + 6.0f }, silkInk, 4.8f);
    if (d.dcJack)
        silk (g, "9V", { d.width * 0.5f, jackRise + 6.0f }, silkInk.withMultipliedAlpha (0.8f), 4.4f);
    drawGraphics (g, id, d);
    for (const auto& k : d.knobs)
        silk (g, k.label, { k.x, k.y + k.d * 0.5f + (d.knobStyle == KnobStyle::chickenHead ? 8.5f : 5.4f) }, silkInk);

    if (d.screws)
        for (const auto& c : { juce::Point<float> { 7.5f, jackRise + 7.5f }, { d.width - 7.5f, jackRise + 7.5f }, { 7.5f, pedalHeight - 7.5f },
                               { d.width - 7.5f, pedalHeight - 7.5f } })
            drawScrew (g, c);

    // The hardware.
    for (const auto& k : d.knobs)
        drawKnob (g, d, k, scale);
    if (id == BlockId::boost)
    {
        // The boost's three-way mode toggle (Clean, Tight, Screamer), at the right of the LED, leaning to Clean.
        const juce::Point<float> t { 68.0f, d.led.y };
        drawToggle (g, t, -35.0f * deg);
        silk (g, "C", { t.x - 9.0f, t.y - 5.0f }, silkInk, 5.0f, 0.0f);
        silk (g, "T", { t.x, t.y - 9.0f }, silkInk, 5.0f, 0.0f);
        silk (g, "S", { t.x + 9.0f, t.y - 5.0f }, silkInk, 5.0f, 0.0f);
    }
    drawLedBezel (g, d.led);
    drawFootswitch (g, d.footswitch, d.topper);

    if (withName)
        drawName (g, id, d);
    return image;
}

// ---- The cache -----------------------------------------------------------------------------------

/// A pedal's art and its contact shadow at one scale.
struct Cached
{
    int key = -1; // the scale in hundredths
    juce::Image art, shadow;
};

constexpr float shadowPad = 10.0f; // points of room round the body in the shadow image

juce::Image renderShadow (BlockId id, float scale)
{
    const auto d = design (id);
    const auto body = bodyOf (d);
    juce::Image image (juce::Image::ARGB, juce::roundToInt ((body.getWidth() + 2.0f * shadowPad) * scale), juce::roundToInt ((body.getHeight() + 2.0f * shadowPad) * scale),
                       true);
    juce::Graphics g (image);
    juce::Path p;
    p.addRoundedRectangle (body.withPosition (shadowPad, shadowPad) * scale, bodyRadius * scale);
    juce::DropShadow (juce::Colours::black, juce::jmax (1, juce::roundToInt (5.0f * scale)), {}).drawForPath (g, p);
    return image;
}

std::array<std::array<Cached, 2>, numBlocks>& cache() // message thread only
{
    static std::array<std::array<Cached, 2>, numBlocks> c;
    return c;
}

int renders = 0;

/// The art at a scale: the most recent of two scales kept per pedal (a board and a larger view of the same pedal
/// don't thrash), rendered only when neither matches.
const Cached& cachedArt (BlockId id, float scale)
{
    const auto key = juce::roundToInt (scale * 100.0f);
    auto& slots = cache()[(size_t) id];
    if (slots[0].key == key)
        return slots[0];
    std::swap (slots[0], slots[1]);
    if (slots[0].key != key)
    {
        const auto s = (float) key / 100.0f;
        slots[0] = { key, renderArt (id, s, true), renderShadow (id, s) };
        ++renders;
    }
    return slots[0];
}

/// Snaps a length in points to the device's pixel grid.
float snap (float v, float physical) { return std::round (v * physical) / physical; }
} // namespace

// ---- The interface -------------------------------------------------------------------------------

bool isPedal (BlockId id)
{
    const auto s = info (id).section;
    return s == Section::pre || s == Section::post || id == BlockId::gateB;
}

juce::Point<float> pedalSize (BlockId id) { return { design (id).width, pedalHeight }; }

juce::Image renderPedal (BlockId id, float scale) { return renderArt (id, scale, true); }

juce::Image renderPedalUnlettered (BlockId id, float scale) { return renderArt (id, scale, false); }

juce::Colour pedalNameInk (BlockId id) { return design (id).ink; }

PedalLayout pedalLayout (BlockId id)
{
    const auto d = design (id);
    const auto disc = [] (juce::Point<float> c, float r) { return juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c); };
    return { bodyOf (d), disc (d.led, ledBezel), disc (d.led, ledGlowRadius), disc (d.footswitch, nutRadius), nameFor (id, d).ink.getBounds() };
}

int pedalRenderCount() { return renders; }

juce::Rectangle<float> pedalBounds (juce::Rectangle<float> box, BlockId id)
{
    const auto size = pedalSize (id);
    const auto outer = size + juce::Point<float> (2.0f * pedalMargin, 2.0f * pedalMargin);
    const auto k = juce::jmin (box.getWidth() / outer.x, box.getHeight() / outer.y);
    return juce::Rectangle<float> (size.x * k, size.y * k).withCentre (box.getCentre());
}

void drawPedal (juce::Graphics& g, juce::Rectangle<float> box, BlockId id, const PedalState& state)
{
    const auto physical = juce::jmax (0.01f, g.getInternalContext().getPhysicalPixelScaleFactor());
    auto fit = pedalBounds (box, id);
    if (fit.isEmpty())
        return;
    const auto size = pedalSize (id);
    const auto k = fit.getWidth() / size.x; // points on screen per enclosure point
    const auto& cached = cachedArt (id, physical * k);

    // Raised when selected, lifted a little when hovered: the art rises, and its shadow falls further and softer.
    const auto lift = state.selected ? 3.0f : state.hovered ? 1.5f : 0.0f;
    fit = fit.withPosition (snap (fit.getX(), physical), snap (fit.getY() - lift * k, physical));
    const auto layout = pedalLayout (id);
    const auto toScreen = [&] (juce::Rectangle<float> r)
    {
        return juce::Rectangle<float> (fit.getX() + r.getX() * k, fit.getY() + r.getY() * k, r.getWidth() * k, r.getHeight() * k);
    };
    const auto body = toScreen (layout.body);

    {
        // The contact shadow (the body's blurred silhouette), dropped 3 pt at rest and further as the pedal rises.
        const auto shadowBox = body.expanded (shadowPad * k).translated (0.0f, (3.0f + 1.6f * lift) * k);
        const juce::Graphics::ScopedSaveState saved (g);
        g.setColour (juce::Colours::black.withAlpha (state.selected ? 0.72f : state.hovered ? 0.66f : 0.60f));
        g.drawImage (cached.shadow, shadowBox, juce::RectanglePlacement::stretchToFit, true);
        if (state.selected)
        {
            // A warm spill of the accent under the raised pedal.
            g.setColour (accent.withAlpha (0.22f));
            g.drawImage (cached.shadow, body.expanded (2.0f * k).translated (0.0f, 2.0f * k), juce::RectanglePlacement::stretchToFit, true);
        }
    }

    if (state.selected)
    {
        // The selected ring: the accent, 3.5 pt out from the body, under the art so the jacks stand in front of it.
        g.setColour (accent);
        g.drawRoundedRectangle (body.expanded (3.5f * k), (bodyRadius + 3.5f) * k, juce::jmax (1.0f, 1.5f * k));
    }

    {
        const juce::Graphics::ScopedSaveState saved (g);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);
        g.drawImage (cached.art, fit);
    }

    if (state.on)
    {
        // The LED lit: a soft glow on the face round it, then the lens itself, hot in the middle.
        const auto c = toScreen (layout.led).getCentre();
        const auto glowR = ledGlowRadius * k, lensR = ledLens * k;
        juce::ColourGradient glow (accent.withAlpha (0.50f), c, accent.withAlpha (0.0f), c.translated (glowR, 0.0f), true);
        glow.addColour (0.22, accent.withAlpha (0.34f));
        glow.addColour (0.55, accent.withAlpha (0.08f));
        g.setGradientFill (glow);
        g.fillEllipse (juce::Rectangle<float> (glowR * 2.0f, glowR * 2.0f).withCentre (c));
        juce::ColourGradient lens (accent.interpolatedWith (juce::Colours::white, 0.75f), c.translated (-0.25f * lensR, -0.3f * lensR), accent.darker (0.35f),
                                   c.translated (lensR * 1.1f, 0.0f), true);
        lens.addColour (0.45, accent.brighter (0.15f));
        g.setGradientFill (lens);
        g.fillEllipse (juce::Rectangle<float> (lensR * 2.0f, lensR * 2.0f).withCentre (c));
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.fillEllipse (juce::Rectangle<float> (1.3f * k, 0.9f * k).withCentre (c.translated (-0.9f * k, -1.1f * k)));
    }
}

} // namespace ui

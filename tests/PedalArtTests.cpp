// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The pedal art (src/ui/PedalArt.*, the app redesign): every pedal renders at its declared size with a solid body,
// the cache renders once per pedal and scale, the names read against their enclosures, the pedals are told apart by
// colour, and off is dimmed whole with a near-black LED while on is lit with a larger LED and a halo (RD4-1, RD4-2). Proof images go to <proof-dir>/pedals/.

#include "ui/AmpHead.h"
#include "ui/PedalArt.h"
#include "TestHelpers.h"

#include <set>

namespace
{
using namespace testing;
using ui::BlockId;

bool savePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
}

/// The mean colour of an image over a rectangle (device pixels), alpha ignored.
juce::Colour meanColour (const juce::Image& image, juce::Rectangle<int> area)
{
    area = area.getIntersection (image.getBounds());
    double r = 0, g = 0, b = 0;
    const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
        {
            const auto c = data.getPixelColour (x, y);
            r += c.getRed();
            g += c.getGreen();
            b += c.getBlue();
        }
    const auto n = juce::jmax (1.0, (double) area.getWidth() * area.getHeight());
    return juce::Colour ((juce::uint8) juce::roundToInt (r / n), (juce::uint8) juce::roundToInt (g / n), (juce::uint8) juce::roundToInt (b / n));
}

/// WCAG 2's contrast ratio (relative luminance of the linearised sRGB).
double contrastRatio (juce::Colour a, juce::Colour b)
{
    const auto luminance = [] (juce::Colour c)
    {
        const auto lin = [] (double v)
        {
            v /= 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin (c.getRed()) + 0.7152 * lin (c.getGreen()) + 0.0722 * lin (c.getBlue());
    };
    const auto la = luminance (a), lb = luminance (b);
    return (juce::jmax (la, lb) + 0.05) / (juce::jmin (la, lb) + 0.05);
}

double colourDistance (juce::Colour a, juce::Colour b)
{
    return std::sqrt (juce::square ((double) a.getRed() - b.getRed()) + juce::square ((double) a.getGreen() - b.getGreen())
                      + juce::square ((double) a.getBlue() - b.getBlue()));
}

std::vector<BlockId> pedals()
{
    std::vector<BlockId> list;
    for (int b = 0; b < ui::numBlocks; ++b)
        if (ui::isPedal ((BlockId) b))
            list.push_back ((BlockId) b);
    return list;
}

juce::String nameOf (BlockId id) { return ui::info (id).pageName; }

/// Whether a point (in the enclosure's points) is inside its body's rounded box shrunk by `inset`.
bool insideBody (juce::Rectangle<float> body, float radius, float inset, float x, float y)
{
    const auto b = body.reduced (inset);
    const auto r = juce::jmax (0.0f, radius - inset);
    const auto qx = std::abs (x - b.getCentreX()) - (b.getWidth() * 0.5f - r), qy = std::abs (y - b.getCentreY()) - (b.getHeight() * 0.5f - r);
    return std::hypot (juce::jmax (qx, 0.0f), juce::jmax (qy, 0.0f)) + juce::jmin (juce::jmax (qx, qy), 0.0f) - r < 0.0f;
}

/// A pedal drawn by drawPedal (cache and state) into an image `box` points big at a scale, on the board's colour.
juce::Image drawn (BlockId id, juce::Rectangle<float> box, float scale, const ui::PedalState& state, juce::Colour ground = juce::Colour (0xff161616))
{
    juce::Image image (juce::Image::ARGB, juce::roundToInt (box.getRight() * scale), juce::roundToInt (box.getBottom() * scale), true);
    juce::Graphics g (image);
    g.fillAll (ground);
    g.addTransform (juce::AffineTransform::scale (scale));
    ui::drawPedal (g, box, id, state);
    return image;
}

/// A box that fits a pedal at `k` points per enclosure point, with drawPedal's margin round it.
juce::Rectangle<float> boxFor (BlockId id, float k)
{
    const auto size = ui::pedalSize (id);
    return { 0.0f, 0.0f, (size.x + 2.0f * ui::pedalMargin) * k, (size.y + 2.0f * ui::pedalMargin) * k };
}

class PedalArtTests final : public juce::UnitTest
{
public:
    PedalArtTests() : juce::UnitTest ("Pedal art", "ampsim") {}

    void runTest() override
    {
        const auto list = pedals();
        const auto dir = proofDir().getChildFile ("pedals");
        dir.createDirectory();

        beginTest ("the board's pedals: the pre and post blocks and Gate B, fourteen, in chain order");
        {
            const std::vector<BlockId> expected { BlockId::gateA,  BlockId::preCompressor,  BlockId::boost,      BlockId::overdrive,   BlockId::preEq,
                                                  BlockId::gateB,  BlockId::postEq,         BlockId::postCompressor, BlockId::harmonizer, BlockId::multivoicer,
                                                  BlockId::bloom,  BlockId::chorus,         BlockId::delay,      BlockId::reverb };
            expect (list == expected);
            for (const auto id : { BlockId::input, BlockId::amp, BlockId::cab, BlockId::output })
                expect (! ui::isPedal (id), ui::info (id).name);
        }

        beginTest ("every pedal renders at 1x and 2x at its declared size (one height, its own width), with no transparent holes in its body");
        {
            juce::StringArray sizes;
            for (const auto id : list)
            {
                const auto size = ui::pedalSize (id);
                expectEquals (size.y, ui::pedalSize (BlockId::gateA).y, "every pedal the same height");
                const auto layout = ui::pedalLayout (id);
                expect (juce::Rectangle<float> (size.x, size.y).contains (layout.body), nameOf (id));
                expect (layout.body.contains (layout.led) && layout.body.contains (layout.footswitch) && layout.body.contains (layout.name), nameOf (id) + ": its parts are on its body");
                expect (! layout.footswitch.intersects (layout.led) && ! layout.name.intersects (layout.footswitch), nameOf (id) + ": parts apart");
                for (const auto scale : { 1.0f, 2.0f })
                {
                    const auto art = ui::renderPedal (id, scale);
                    expectEquals (art.getWidth(), juce::roundToInt (size.x * scale), nameOf (id));
                    expectEquals (art.getHeight(), juce::roundToInt (size.y * scale), nameOf (id));
                    // Every pixel whose centre is more than a pixel inside the body's rounded box is opaque.
                    int holes = 0, inside = 0;
                    const juce::Image::BitmapData data (art, juce::Image::BitmapData::readOnly);
                    for (int y = 0; y < art.getHeight(); ++y)
                        for (int x = 0; x < art.getWidth(); ++x)
                            if (insideBody (layout.body, 7.0f, 1.0f / scale, ((float) x + 0.5f) / scale, ((float) y + 0.5f) / scale))
                            {
                                ++inside;
                                if (data.getPixelColour (x, y).getAlpha() != 255)
                                    ++holes;
                            }
                    expectEquals (holes, 0, nameOf (id) + " at " + juce::String (scale) + "x");
                    expectGreaterThan (inside, (int) (0.9f * layout.body.getWidth() * layout.body.getHeight() * scale * scale));
                }
                sizes.add (nameOf (id) + " " + juce::String (size.x, 0) + " x " + juce::String (size.y, 0));
            }
            logMessage ("  -> sizes (points): " + sizes.joinIntoString (", "));
        }

        beginTest ("the cache: one render per pedal and scale; repaints and state changes never re-render");
        {
            const auto before = ui::pedalRenderCount();
            const auto drawAll = [&] (float k, const ui::PedalState& state)
            {
                juce::Image image (juce::Image::ARGB, 400, 400, true);
                juce::Graphics g (image);
                g.addTransform (juce::AffineTransform::scale (2.0f));
                for (const auto id : list)
                    ui::drawPedal (g, boxFor (id, k), id, state);
            };
            drawAll (0.75f, {});
            const auto first = ui::pedalRenderCount() - before;
            expectEquals (first, (int) list.size(), "one render per pedal for a new scale");
            for (int frame = 0; frame < 30; ++frame)
                drawAll (0.75f, { frame % 2 == 0, frame % 3 == 0, frame % 5 == 0 });
            expectEquals (ui::pedalRenderCount() - before, first, "30 frames of state changes re-render nothing");
            drawAll (1.0f, {});
            expectEquals (ui::pedalRenderCount() - before, 2 * (int) list.size(), "a new size renders once more each");
            drawAll (0.75f, { true, true, false });
            drawAll (1.0f, { true, false, true });
            expectEquals (ui::pedalRenderCount() - before, 2 * (int) list.size(), "two sizes in turn (a board and a larger view) don't thrash");
            logMessage ("  -> renders: " + juce::String (first) + " for 14 pedals at one scale, none in 30 frames of state changes, "
                        + juce::String (ui::pedalRenderCount() - before) + " after a second scale and switching back and forth");
        }

        beginTest ("each name's contrast against the enclosure under it is at least 4.5:1");
        {
            juce::StringArray measured;
            for (const auto id : list)
            {
                const auto blank = ui::renderPedalUnlettered (id, 2.0f);
                const auto box = (ui::pedalLayout (id).name * 2.0f).toNearestInt();
                const auto under = meanColour (blank, box);
                const auto ratio = contrastRatio (ui::pedalNameInk (id), under);
                expectGreaterOrEqual (ratio, 4.5, nameOf (id) + " at " + juce::String (ratio, 2) + ":1");
                measured.add (nameOf (id) + " " + juce::String (ratio, 1) + ":1");
            }
            logMessage ("  -> name contrast: " + measured.joinIntoString (", "));
        }

        beginTest ("every pair of pedals is distinct by its body's average colour");
        {
            std::vector<juce::Colour> means;
            juce::StringArray measured;
            for (const auto id : list)
            {
                const auto art = ui::renderPedal (id, 1.0f);
                means.push_back (meanColour (art, ui::pedalLayout (id).body.reduced (3.0f).toNearestInt()));
                measured.add (nameOf (id) + " #" + means.back().toDisplayString (false));
            }
            double closest = 1.0e9;
            juce::String pair;
            for (size_t a = 0; a < means.size(); ++a)
                for (size_t b = a + 1; b < means.size(); ++b)
                    if (const auto d = colourDistance (means[a], means[b]); d < closest)
                    {
                        closest = d;
                        pair = nameOf (list[a]) + " and " + nameOf (list[b]);
                    }
            expectGreaterThan (closest, 24.0, "closest: " + pair);
            logMessage ("  -> body means: " + measured.joinIntoString (", "));
            logMessage ("  -> closest pair: " + pair + " at " + juce::String (closest, 1) + " (0 to 441; the heads' limit is 24)");
        }

        beginTest ("off is dimmed whole (55% brightness, less saturation) with a near-black LED; on is lit with a larger LED and a halo; a section that's off dims too; selected and hovered move the pedal");
        {
            juce::StringArray measured;
            // The first art's LED: a 2.9 pt lens in a 4.7 pt bezel, its glow 15 pt (before RD4-2).
            constexpr float firstLens = 2.9f;
            for (const auto id : list)
            {
                constexpr float scale = 2.0f, k = 1.0f;
                const auto box = boxFor (id, k);
                const auto off = drawn (id, box, scale, {}), on = drawn (id, box, scale, { true, false, false });
                const auto sectionOff = drawn (id, box, scale, { true, false, false, true });
                const auto fit = ui::pedalBounds (box, id);
                const auto layout = ui::pedalLayout (id);
                const auto toImage = [&] (juce::Rectangle<float> r)
                {
                    return juce::Rectangle<float> (fit.getX() + r.getX() * k, fit.getY() + r.getY() * k, r.getWidth() * k, r.getHeight() * k) * scale;
                };
                const auto glow = toImage (layout.ledGlow).expanded (1.0f);
                const auto body = toImage (layout.body).reduced (4.0f);

                // The enclosure off (away from the LED's halo): 55% of its brightness on, and less saturated.
                const juce::Image::BitmapData a (off, juce::Image::BitmapData::readOnly), b (on, juce::Image::BitmapData::readOnly),
                    so (sectionOff, juce::Image::BitmapData::readOnly);
                double brightOn = 0.0, brightOff = 0.0, brightSection = 0.0, satOn = 0.0, satOff = 0.0;
                int n = 0;
                for (int y = (int) body.getY(); y < (int) body.getBottom(); ++y)
                    for (int x = (int) body.getX(); x < (int) body.getRight(); ++x)
                        if (! glow.contains ((float) x + 0.5f, (float) y + 0.5f))
                        {
                            brightOn += b.getPixelColour (x, y).getBrightness();
                            brightOff += a.getPixelColour (x, y).getBrightness();
                            brightSection += so.getPixelColour (x, y).getBrightness();
                            satOn += b.getPixelColour (x, y).getSaturation();
                            satOff += a.getPixelColour (x, y).getSaturation();
                            ++n;
                        }
                const auto ratio = brightOff / brightOn;
                expectWithinAbsoluteError (ratio, (double) ui::dimBrightness, 0.05, nameOf (id) + ": off at about 55% brightness");
                expectWithinAbsoluteError (brightSection / brightOn, ratio, 0.01, nameOf (id) + ": a section that's off dims it the same way");
                expect (satOff <= satOn * 0.6 + 1.0e-6 * n, nameOf (id) + ": off at lower saturation");

                // The off lens: near-black #2a2a2a, no amber in it (its channels within 3 of each other).
                const auto c = (toImage (layout.ledLens).getCentre()).toInt();
                const auto lensOff = a.getPixelColour (c.x + 1, c.y + 1);
                expect (colourDistance (lensOff, ui::ledOffColour) < 8.0, nameOf (id) + ": off lens #" + lensOff.toDisplayString (false));
                expect (std::abs ((int) lensOff.getRed() - (int) lensOff.getBlue()) <= 3, nameOf (id) + ": no tint");

                // The on lens: the accent, 1.5 times the first art's, and a halo round it: the face lit in the accent's
                // direction (warmer than off by a clear margin) out past twice the lens's radius.
                expect (colourDistance (b.getPixelColour (c.x, c.y), ui::theme::accent) < colourDistance (lensOff, ui::theme::accent), nameOf (id));
                expectWithinAbsoluteError (layout.ledLens.getWidth() * 0.5f / firstLens, 1.5f, 0.05f, nameOf (id) + ": the lens 1.5x");
                int litLens = 0;
                const auto lensBox = toImage (layout.ledLens).expanded (2.0f).toNearestInt();
                for (int y = lensBox.getY(); y < lensBox.getBottom(); ++y)
                    for (int x = lensBox.getX(); x < lensBox.getRight(); ++x)
                        litLens += colourDistance (b.getPixelColour (x, y), ui::theme::accent) < 110.0 ? 1 : 0;
                const auto firstArea = juce::MathConstants<double>::pi * juce::square (firstLens * k * scale);
                expectGreaterThan ((double) litLens, 1.8 * firstArea, nameOf (id) + ": a lit area over 1.8 times the first art's lens (1.5x across)");
                const auto haloPoint = juce::Point<int> (c.x, juce::roundToInt ((float) c.y + 2.6f * layout.ledLens.getHeight() * 0.5f * k * scale));
                // The art itself at that point (no LED state): the lit pedal there is the art with the halo over it.
                const auto art = ui::renderPedal (id, scale);
                const auto bare = art.getPixelAt (haloPoint.x - juce::roundToInt (fit.getX() * scale), haloPoint.y - juce::roundToInt (fit.getY() * scale));
                const auto haloOn = b.getPixelColour (haloPoint.x, haloPoint.y);
                expectGreaterThan (colourDistance (bare, ui::theme::accent) - colourDistance (haloOn, ui::theme::accent), 8.0,
                                   nameOf (id) + ": the halo warms the face out past twice the lens's radius");

                const auto selected = drawn (id, box, scale, { false, true, false }), hovered = drawn (id, box, scale, { false, false, true });
                int movedSelected = 0, movedHovered = 0;
                const juce::Image::BitmapData s (selected, juce::Image::BitmapData::readOnly), h (hovered, juce::Image::BitmapData::readOnly);
                for (int y = 0; y < off.getHeight(); ++y)
                    for (int x = 0; x < off.getWidth(); ++x)
                    {
                        movedSelected += s.getPixelColour (x, y) != a.getPixelColour (x, y) ? 1 : 0;
                        movedHovered += h.getPixelColour (x, y) != a.getPixelColour (x, y) ? 1 : 0;
                    }
                expectGreaterThan (movedSelected, movedHovered, nameOf (id) + ": selected stands out more than hovered");
                expectGreaterThan (movedHovered, 0, nameOf (id) + ": hovered lifts");
                // Everything stays inside the box.
                expect (box.contains (fit.expanded (3.5f + 1.5f).translated (0.0f, -3.0f)), nameOf (id) + ": the ring stays in its box");
                measured.add (nameOf (id) + " " + juce::String (ratio, 2) + ", lens #" + lensOff.toDisplayString (false) + ", lit " + juce::String (litLens) + " px");
            }
            logMessage ("  -> off vs on (brightness ratio away from the LED, the off lens, the lit lens's pixels at 2x): " + measured.joinIntoString ("; "));
        }

        beginTest ("proof: each pedal off, on, and selected; the contact sheet with the amp heads");
        {
            const auto ground = juce::Colour (0xff121212);
            for (const auto id : list)
            {
                // The pedal at 3x, off, on, and selected and on, side by side on the studio ground.
                constexpr float scale = 3.0f;
                const auto box = boxFor (id, 1.0f);
                juce::Image sheet (juce::Image::RGB, juce::roundToInt (box.getWidth() * scale * 3.0f), juce::roundToInt (box.getHeight() * scale), true);
                juce::Graphics g (sheet);
                g.fillAll (ground);
                g.addTransform (juce::AffineTransform::scale (scale));
                const ui::PedalState states[] { {}, { true, false, false }, { true, true, false } };
                for (int s = 0; s < 3; ++s)
                    ui::drawPedal (g, box.translated (box.getWidth() * (float) s, 0.0f), id, states[s]);
                expect (savePng (sheet, dir.getChildFile ("pedal_" + nameOf (id) + ".png")));
            }

            // The contact sheet: three heads at 1x for scale and family, then the board at 2x, every pedal off and on, and
            // the board as the main screen might show it (0.7 of the art's size, at 2x).
            const std::array<ui::Material, 3> heads { ui::Material::glass, ui::Material::forge, ui::Material::lantern };
            const auto headArt = ui::AmpHead::render (heads[0], 1.0f);
            constexpr int gap = 40;
            float boardWidth = 0.0f;
            for (const auto id : list)
                boardWidth += boxFor (id, 1.0f).getWidth();
            const auto width = juce::jmax (3 * headArt.getWidth() + 4 * gap, juce::roundToInt (boardWidth * 2.0f) + 2 * gap);
            const auto rowH = juce::roundToInt (boxFor (BlockId::gateA, 1.0f).getHeight() * 2.0f);
            const auto smallH = juce::roundToInt (boxFor (BlockId::gateA, 0.7f).getHeight() * 2.0f);
            juce::Image sheet (juce::Image::RGB, width, headArt.getHeight() + 2 * rowH + smallH + 5 * gap, true);
            juce::Graphics g (sheet);
            g.fillAll (ground);
            for (size_t m = 0; m < heads.size(); ++m)
                g.drawImageAt (ui::AmpHead::render (heads[m], 1.0f), gap + (int) m * (headArt.getWidth() + gap), gap);
            const auto board = [&] (float y, float k, const std::function<ui::PedalState (size_t)>& state)
            {
                const juce::Graphics::ScopedSaveState saved (g);
                g.addTransform (juce::AffineTransform::scale (2.0f));
                auto x = (float) gap / 2.0f;
                for (size_t p = 0; p < list.size(); ++p)
                {
                    const auto box = boxFor (list[p], k);
                    ui::drawPedal (g, box.translated (x, y / 2.0f), list[p], state (p));
                    x += box.getWidth();
                }
            };
            const auto top = (float) (headArt.getHeight() + 2 * gap);
            board (top, 1.0f, [] (size_t) { return ui::PedalState {}; });
            board (top + (float) (rowH + gap), 1.0f, [] (size_t) { return ui::PedalState { true, false, false }; });
            // The small board: a mix of on and off, the overdrive selected and the delay hovered.
            board (top + (float) (2 * rowH + 2 * gap), 0.7f, [&] (size_t p)
            {
                const auto id = list[p];
                return ui::PedalState { p % 3 != 1, id == BlockId::overdrive, id == BlockId::delay };
            });
            expect (savePng (sheet, dir.getChildFile ("contact_sheet.png")));
            logMessage ("  -> wrote " + dir.getFullPathName() + "/pedal_<page>.png (14) and contact_sheet.png");
        }
    }
};

static PedalArtTests pedalArtTests;
} // namespace

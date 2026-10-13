// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Blocks.h"

namespace ui
{

/// The pedals on the main screen's board (the app redesign, .impeccable/surfaces/src-plugineditor-cpp.md): every
/// block of the pre and post sections, and Gate B, drawn as a pedal enclosure of its own, in code like the amp heads
/// (AmpHead.h). The board shows them in chain order; the inspector shows the selected one's controls.
///
/// The art is procedural and cached at the scale it's shown at (renderPedal), so drawing one each frame only blits
/// an image. A pedal's state is drawn on top of its cached art: the LED (lit in theme::accent with a halo when on,
/// near-black when off), and the selected and hovered rings. A pedal that's off, or whose section is off, draws its
/// enclosure dimmed (ASSUMPTIONS RD4-1): the cache keeps a dimmed copy of the art beside it, made in the same render.
struct PedalState
{
    bool on = false;
    bool selected = false;
    bool hovered = false;
    bool sectionOff = false; ///< its section (pre_fx_on, post_fx_on) is off: dimmed even while its own switch is on
};

/// A dimmed enclosure's brightness and saturation, as fractions of the art's own (RD4-1).
constexpr float dimBrightness = 0.55f, dimSaturation = 0.45f;

/// The off LED's lens: near-black, with no tint of the accent (RD4-2).
inline const juce::Colour ledOffColour { 0xff2a2a2a };

/// Whether drawPedal dims a pedal in this state: off, or in a section that's off.
inline bool pedalDimmed (const PedalState& s) { return ! s.on || s.sectionOff; }

/// Whether a block is drawn as a pedal on the board (the pre and post blocks, and Gate B).
bool isPedal (BlockId id);

/// The enclosure's size in points at scale 1: every pedal is the same height; widths vary by enclosure.
juce::Point<float> pedalSize (BlockId id);

/// The enclosure art without its state (no LED glow, no rings), at a scale (pixels per point), uncached.
juce::Image renderPedal (BlockId id, float scale);

/// The pedal in `box` (its aspect kept, centred), with its state, from a cache of renderPedal per id and scale.
/// The enclosure is inset by pedalMargin (scaled with it) so the lift, the selected ring, and the shadow stay inside
/// `box`; pedalBounds() is where the enclosure lands.
void drawPedal (juce::Graphics& g, juce::Rectangle<float> box, BlockId id, const PedalState& state);

// ---- Added with the real art ---------------------------------------------------------------------

/// The room drawPedal leaves round the enclosure, in the enclosure's points.
constexpr float pedalMargin = 8.0f;

/// Where drawPedal draws the enclosure (at rest: hovered and selected pedals rise a little from there).
juce::Rectangle<float> pedalBounds (juce::Rectangle<float> box, BlockId id);

/// Where an enclosure's parts are, in its points at scale 1 (the art's top-left at 0, 0): the body (the art less the
/// jacks standing above it), the LED's bezel and the reach of its glow when lit, the footswitch's nut, and the
/// silkscreened name. ledLens is the lens inside the bezel (the part that lights). Map one into pedalBounds() for hit-testing (x * bounds.getWidth() / pedalSize().x).
struct PedalLayout
{
    juce::Rectangle<float> body, led, ledGlow, footswitch, name, ledLens;
};

PedalLayout pedalLayout (BlockId id);

/// The name's ink, and the art without the name (what the name is printed on): for its contrast.
juce::Colour pedalNameInk (BlockId id);
juce::Image renderPedalUnlettered (BlockId id, float scale);

/// For tests: how many times drawPedal's cache has rendered an enclosure (once per pedal and scale, never per frame).
int pedalRenderCount();

} // namespace ui

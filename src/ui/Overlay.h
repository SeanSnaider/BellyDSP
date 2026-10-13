// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Theme.h"

namespace ui
{

/// What the three overlays (the tuner, Match tone, Settings) draw with, so they read as one family in the
/// redesign's world (phase 3; docs/UI_DESIGN.md "The overlays"): one header (the title, a line about it, the keys,
/// and Close at the right, over a hairline), panels on the ground, numbered step badges, keycaps, and status notes.
/// Every colour is a theme token.
namespace overlay
{
/// The header's height in an overlay's page (the 1200 x 598 the frame gives it), and Close's size in it.
constexpr int headerHeight = 48, closeWidth = 84;

/// The header: the title (20 semibold), a line about it after it (12 faint), the keys on the right before Close
/// (keycaps and their words, `keys` as "Esc|closes" pairs separated by ';'), and a hairline under it all.
/// Close is the page's own button, placed with closeBounds.
void paintHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& about, const juce::String& keys = {});
juce::Rectangle<int> closeBounds (juce::Rectangle<int> header);

/// A panel on the ground: the panel colour, a hairline border, radius 10. `lit`: the border in the accent, faint
/// (the step that's running).
void paintPanel (juce::Graphics& g, juce::Rectangle<int> area, bool lit = false);

/// A small keycap with its key ("Space", "T", "1"; "@left" and "@right" draw the arrow keys); returns its width. `x` is its left edge, centred on `centreY`.
float paintKeycap (juce::Graphics& g, const juce::String& key, float x, float centreY);
float keycapWidth (const juce::String& key);

/// A step's state, for its badge: not yet reachable, the next thing to do, running now, or done.
enum class Step
{
    waiting,
    next,
    active,
    done
};

/// A numbered step badge (22 px): waiting is a faint ring; next is the number in ink on a raised disc with a
/// stronger ring; active is the accent disc with the number in the ground colour; done is a check in the accent.
void paintStepBadge (juce::Graphics& g, juce::Point<float> centre, int number, Step state);

/// A step's heading in its panel: the badge, the title (14 medium; faint while waiting), and a short state on the
/// right (12; the accent while active). Returns the area under the heading.
juce::Rectangle<int> paintStepHeading (juce::Graphics& g, juce::Rectangle<int> panelInside, int number, const juce::String& title, Step state,
                                       const juce::String& stateText = {});

/// The height a step's heading takes (its badge row and the gap under it).
constexpr int stepHeadingHeight = 34;
/// A panel's inner padding.
constexpr int panelPadding = 14;

/// A note in a box: a hairline-bordered note in ink with a short bar at the left (an error, or what Apply did). Red
/// stays for clipping only, so a problem is said in words (ASSUMPTIONS UH5).
void paintNote (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, bool strong);

/// A small uppercase label over a group inside a panel ("COUNT-IN"), 10.5 medium faint with tracking.
void paintGroupLabel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text);
} // namespace overlay

/// Marks a TextButton as the step's main action (Match, Apply, Record): the LookAndFeel draws it filled with the
/// raised colour, a stronger border, and medium ink text. (Orange stays for on, selected, active: a primary
/// button turns orange only while what it started runs, as Record does while recording.)
void setPrimary (juce::Component& button, bool primary = true);
bool isPrimary (const juce::Component& button);

} // namespace ui

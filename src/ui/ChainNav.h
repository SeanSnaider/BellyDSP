// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Blocks.h"
#include "Controls.h"
#include "PluginProcessor.h"

#include <array>
#include <memory>

namespace ui
{

/// The signal chain along the bottom (handoff 4.4): Input, Pre FX, Amp, EQ, Cab, Post FX, Output, 16 px
/// apart and centred, over a 1 px line-2 connector, with a 1 px line along the top. Each processing block
/// is 44 high with 14 px padding, radius 10, a 1 px line-2 border, filled with the background so it covers
/// the connector: a 20 px line icon (the reference's SVG paths), its name (14 px, weight 500), and a 7 px
/// bypass dot. Input and Output are fixed ends: no border, no dot, faint.
///
/// The page showing has its block outlined in emerald with an emerald icon and an ink name (none while the
/// tuner is open). Clicking a block opens its page; clicking its dot toggles its bypass without
/// navigating. A bypassed block draws its icon at 35% and strikes its name through. The dots:
///   Pre FX   pre_fx_on              EQ    eq_post_on
///   Amp      amp_bypass (inverted)  Cab   cab_bypass (inverted)
///   Post FX  post_fx_on
class ChainNav final : public juce::Component
{
public:
    explicit ChainNav (AmpSimProcessor& processor);
    ~ChainNav() override;

    /// A block (not its dot) was clicked.
    std::function<void (PageId)> onNavigate;

    /// The page showing (PageId::tuner, or anything without a block: none highlighted).
    void setActive (PageId page);
    PageId getActive() const noexcept { return active; }

    /// For tests: a page's block, its dot (nullptr for Input and Output), and whether it's drawn bypassed.
    juce::Component* getNode (PageId page) const;
    PowerSwitch* getDot (PageId page) const;
    bool isBypassedShown (PageId page) const;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// The order along the chain.
    static constexpr std::array<PageId, 7> order { PageId::input, PageId::preFx, PageId::amp, PageId::eq, PageId::cab, PageId::postFx, PageId::output };

private:
    class Node;
    std::array<std::unique_ptr<Node>, order.size()> nodes;
    PageId active = PageId::amp;
};

/// The chain's 24-unit line icons (handoff 4.4, the reference's SVG): `strokes` is drawn with a 1.5 unit
/// round stroke, `fills` (the EQ's slider caps) are filled with the background first and then stroked.
struct ChainIcon
{
    juce::Path strokes, fills;
};
ChainIcon chainIcon (PageId page);

/// Draws an icon in a 20 x 20 box (the 24-unit drawing scaled, stroke 1.5 units as in the reference).
void drawChainIcon (juce::Graphics& g, const ChainIcon& icon, juce::Rectangle<float> box, juce::Colour colour);

} // namespace ui

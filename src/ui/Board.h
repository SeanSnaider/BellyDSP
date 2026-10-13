// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Controls.h"
#include "PedalArt.h"
#include "PluginProcessor.h"

#include <array>
#include <memory>
#include <vector>

namespace ui
{

/// The board under the stage (the app redesign's FIRST VIEWPORT): every pedal (ui::isPedal) drawn with drawPedal in
/// chain order, on two rows like a real board. The top row is the pre section in its saved order, then the amp's
/// marker, Gate B, and the cab's marker; the bottom row is the post section in its saved order. Each row starts with
/// its section's name and switch (pre_fx_on, post_fx_on) as a labelled toggle; a section that's off dims its row, the
/// way a pedal that's off is dimmed (RD4-1).
///
///   a click on a pedal's footswitch   switches the block on or off, as one undo step (its LED follows)
///   a click anywhere else on it       selects it: the inspector shows its controls, and it's drawn raised
///   a drag along its row              reorders the section (setSectionOrder, the chain's click-free dip); the
///                                     other pedals make room where it will land. Gate B stays where it is.
///   a click on the amp or cab marker  selects the amp or the cab; the dot on it switches it (amp_bypass,
///                                     cab_bypass, inverted)
///   a right-click                     MIDI learn for the pedal's switch (each pedal is tagged with it)
///
/// The LEDs show each block's own switch, live (refresh() reads them); one that comes on lights with a short glow.
class Board final : public juce::Component, private juce::Timer
{
public:
    explicit Board (AmpSimProcessor& processor);
    ~Board() override;

    /// A pedal or a marker was clicked (not its footswitch or dot).
    std::function<void (BlockId)> onSelect;

    /// The selected block, drawn raised (any block: one not on the board selects nothing here).
    void setSelected (BlockId id);
    BlockId getSelected() const noexcept { return selected; }

    /// Message thread, 30 times a second: the sections' orders, the LEDs, the sections' switches, the amp's name.
    void refresh();

    /// The order shown on a row (Section::pre: the pre section's pedals; Section::post: the post section's).
    std::vector<BlockId> getShownOrder (Section section) const;

    /// For tests: a pedal's component (nullptr if the block isn't one), where its footswitch is in it, whether its
    /// LED is lit, a click on it (on the footswitch or elsewhere), and a drag of it along its row by `dx`.
    juce::Component* getPedal (BlockId id) const;
    juce::Rectangle<float> getFootswitchArea (BlockId id) const;
    bool isLit (BlockId id) const;
    /// Whether a pedal draws dimmed: switched off, or its section off (RD4-1).
    bool isDimmed (BlockId id) const;
    void clickPedal (BlockId id, bool onFootswitch);
    void dragPedal (BlockId id, float dx);
    juce::Component* getMarker (BlockId id) const; ///< BlockId::amp or BlockId::cab
    PowerSwitch* getSectionSwitch (Section section) const;
    /// The scale the pedals are drawn at (screen points per enclosure point).
    float getPedalScale() const noexcept { return scale; }

    void paint (juce::Graphics&) override;
    void resized() override;

    // A drag in progress (the pedals call these; x in the board's coordinates).
    void beginDrag (BlockId id, float grabOffset);
    void dragTo (float x);
    void endDrag();

    static constexpr int labelWidth = 64, sidePad = 18, rowPad = 12, rowGap = 6, markerWidth = 70;

private:
    class Pedal;
    class Marker;

    void layoutRows();
    void timerCallback() override;
    Pedal* pedalFor (BlockId id) const;
    bool sectionOn (Section section) const;

    AmpSimProcessor& ampSim;
    std::vector<std::unique_ptr<Pedal>> pedals;
    std::unique_ptr<Marker> ampMarker, cabMarker;
    std::unique_ptr<PowerSwitch> preSwitch, postSwitch;
    std::array<std::vector<BlockId>, 2> order; // the pre and post sections as shown
    std::array<juce::Rectangle<int>, 2> rows;
    BlockId selected = BlockId::amp;
    float scale = 0.6f;

    BlockId dragged = BlockId::count;
    int dragInsert = 0;
    float dragX = 0.0f, dragGrab = 0.0f, dragStartCentre = 0.0f;
};

} // namespace ui

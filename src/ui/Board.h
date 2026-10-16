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

/// The board's three tabs (Sean's call, 2026-10-13: BUILD_PLAN decision log). Each shows one part of the chain on one
/// row: the pre section's pedals, the amp with Gate B and the cab, or the post section's pedals.
enum class BoardTab
{
    pre,
    ampCab,
    post,
    count
};

constexpr int numBoardTabs = (int) BoardTab::count;

/// A tab's permanent name (the app state's "board tab last shown", and snapshots): "pre", "ampcab", "post".
const char* boardTabName (BoardTab tab);
/// A tab's label on the strip: "Pre FX", "Amp & Cab", "Post FX".
const char* boardTabLabel (BoardTab tab);
/// The tab that holds a block (the pre and post sections' pedals, or the amp, Gate B, and the cab); count for the
/// input and the output, which aren't on the board.
BoardTab boardTabFor (BlockId id);
/// A tab by its permanent name, or count if none.
BoardTab boardTabNamed (const juce::String& name);

/// The board under the stage (the app redesign's FIRST VIEWPORT): every pedal (ui::isPedal) drawn with drawPedal in
/// chain order, in three tabs (ASSUMPTIONS RD5-1 to RD5-8). A strip along the top holds the tabs, each its name and a
/// row of tiny LEDs, one per block in it in the order shown, lit while that block is on and dimmed while its section
/// is off, so the tabs that aren't open still show what's on. A click anywhere on a tab opens it. Under the strip,
/// the open tab's row:
///
///   Pre FX     the pre section's pedals in their saved order; the section's switch (pre_fx_on, a labelled toggle)
///              at the strip's right
///   Amp & Cab  the amp's marker, Gate B, and the cab's marker as larger tiles, centred, the cable running through
///   Post FX    the post section's pedals in their saved order; post_fx_on at the strip's right
///
/// Each row's pedals are drawn at the scale that fits it (the row's height, or its width when that's tighter). A
/// section that's off dims its pedals, the way a pedal that's off is dimmed (RD4-1). Gate B isn't in either section
/// (the chain bypasses it by gate_b_on alone), so nothing dims it but its own switch.
///
///   a click on a pedal's footswitch   switches the block on or off, as one undo step (its LED follows)
///   a click anywhere else on it       selects it: the inspector shows its controls, and it's drawn raised
///   a drag along its row              reorders the section (setSectionOrder, the chain's click-free dip); the
///                                     other pedals make room where it will land. Gate B stays where it is.
///   a click on the amp or cab marker  selects the amp or the cab; the dot on it switches it (amp_bypass,
///                                     cab_bypass, inverted)
///   a right-click                     MIDI learn for the pedal's switch (each pedal is tagged with it)
///
/// The selection opens its tab (setSelected), wherever it came from. The open tab is UI state only: the editor keeps
/// it in the app state (onTabChange), never in a parameter.
///
/// The LEDs show each block's own switch, live (refresh() reads them); one that comes on lights with a short glow. The
/// auto-wah's sweep window follows its filter, live, while it sounds (refresh() reads AmpSimProcessor::getWahControl).
class Board final : public juce::Component, private juce::Timer
{
public:
    explicit Board (AmpSimProcessor& processor);
    ~Board() override;

    /// A pedal or a marker was clicked (not its footswitch or dot).
    std::function<void (BlockId)> onSelect;
    /// The open tab changed (a click on the strip, a key, or the selection moving to another tab).
    std::function<void (BoardTab)> onTabChange;

    /// The selected block, drawn raised (any block: one not on the board selects nothing here). A block on another
    /// tab opens that tab.
    void setSelected (BlockId id);
    BlockId getSelected() const noexcept { return selected; }

    /// The open tab (Pre FX by default). setTab calls onTabChange if it changed and `notify` is true.
    void setTab (BoardTab tab, bool notify = true);
    BoardTab getTab() const noexcept { return tab; }
    /// The next or previous tab, wrapping round (the [ and ] keys).
    void stepTab (int direction);

    /// Message thread, 30 times a second: the sections' orders, the LEDs, the sections' switches, the amp's name.
    void refresh();

    /// The order shown on a row (Section::pre: the pre section's pedals; Section::post: the post section's).
    std::vector<BlockId> getShownOrder (Section section) const;
    /// A tab's blocks in the order shown (its LEDs' order).
    std::vector<BlockId> getTabBlocks (BoardTab t) const;

    /// One of a tab's LEDs: its block, whether it's lit (the block's own switch is on), and whether it's dimmed (its
    /// section's switch is off).
    struct TabLed
    {
        BlockId id = BlockId::count;
        bool lit = false, dimmed = false;
    };
    std::vector<TabLed> getTabLeds (BoardTab t) const;

    /// For tests: the strip, a tab's area in it, a pedal's component (nullptr if the block isn't one), where its
    /// footswitch is in it, whether its LED is lit, whether it's showing (on the open tab), a click on it (on the
    /// footswitch or elsewhere), and a drag of it along its row by `dx`.
    juce::Component& getTabStrip() noexcept;
    juce::Rectangle<float> getTabArea (BoardTab t) const;
    /// Where a tab's LED (its index in getTabLeds) is drawn, in the strip's coordinates.
    juce::Point<float> getTabLedCentre (BoardTab t, int index) const;
    /// Whether a pedal's LED is in its switch-on bloom.
    bool isGlowing (BlockId id) const;
    juce::Component* getPedal (BlockId id) const;
    juce::Rectangle<float> getFootswitchArea (BlockId id) const;
    bool isLit (BlockId id) const;
    /// The auto-wah's sweep window: what its pedal shows (0 at Low to 1 at High, or below 0 for a dark window).
    /// refresh() sets it from AmpSimProcessor::getWahControl while the wah sounds; tests set it directly, as the meters'
    /// tests push readings into their views.
    void setSweep (float control);
    float getSweep() const;
    /// The smallest move of the control that redraws the window (a 500th of it: under a pixel at any board scale).
    static constexpr float sweepStep = 0.002f;
    /// Whether a pedal draws dimmed: switched off, or its section off (RD4-1).
    bool isDimmed (BlockId id) const;
    /// Whether a block's pedal or marker is showing (its tab is open).
    bool isShown (BlockId id) const;
    void clickPedal (BlockId id, bool onFootswitch);
    void dragPedal (BlockId id, float dx);
    juce::Component* getMarker (BlockId id) const; ///< BlockId::amp or BlockId::cab
    PowerSwitch* getSectionSwitch (Section section) const;
    /// The scale the open tab's pedals are drawn at (screen points per enclosure point), or a given tab's.
    float getPedalScale() const noexcept { return getPedalScale (tab); }
    float getPedalScale (BoardTab t) const noexcept { return tabScale[(size_t) t]; }

    void paint (juce::Graphics&) override;
    void resized() override;

    // A drag in progress (the pedals call these; x in the board's coordinates).
    void beginDrag (BlockId id, float grabOffset);
    void dragTo (float x);
    void endDrag();

    /// The board's geometry: the gutter at the sides, the strip (its top and height), the gap under it, the room
    /// under the row, the marker's tile width in enclosure points, and the room between the Amp & Cab tab's tiles.
    static constexpr int sidePad = 18, stripTop = 10, stripHeight = 30, stripGap = 8, bottomPad = 10, ampCabGap = 30;
    static constexpr float markerPoints = 140.0f;

private:
    class Pedal;
    class Marker;
    class Tabs;

    void layoutRows();
    void showTab();
    void timerCallback() override;
    Pedal* pedalFor (BlockId id) const;
    bool sectionOn (Section section) const;
    bool blockOn (BlockId id) const;
    juce::Rectangle<int> rowFor (BoardTab t) const;

    AmpSimProcessor& ampSim;
    std::vector<std::unique_ptr<Pedal>> pedals;
    std::unique_ptr<Marker> ampMarker, cabMarker;
    std::unique_ptr<PowerSwitch> preSwitch, postSwitch;
    std::unique_ptr<Tabs> tabs;
    std::array<std::vector<BlockId>, 2> order; // the pre and post sections as shown
    juce::Rectangle<int> content;              // the open tab's row, under the strip
    std::array<float, numBoardTabs> tabScale { 0.8f, 0.8f, 0.8f };
    BlockId selected = BlockId::amp;
    BoardTab tab = BoardTab::pre;
    juce::uint32 ledSignature = 0;

    BlockId dragged = BlockId::count;
    int dragInsert = 0;
    float dragX = 0.0f, dragGrab = 0.0f, dragStartCentre = 0.0f;
};

} // namespace ui

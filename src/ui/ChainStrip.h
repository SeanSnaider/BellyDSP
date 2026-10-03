#pragma once

#include "Blocks.h"
#include "Controls.h"
#include "PluginProcessor.h"

#include <array>
#include <memory>
#include <vector>

namespace ui
{

/// The chain strip (UI_DESIGN "Chain block" and "Layout"): every block in signal order, as cards in two
/// rows (ASSUMPTIONS U9). Row one: the input, the pre section (a labelled box), the amp, Gate B, and the
/// cab; a line turns from the cab down into row two: the post section and the output.
///
/// A card shows its section's colour bar, the block's name, its power switch, and a state line (the mode,
/// or "off"); an off block is dimmed and its switch hollow. Clicking a card selects it (its page shows
/// below). A card in the pre or post section can be dragged along its section: the others open a gap
/// where it will land, and dropping it asks the processor for the new order with setSectionOrder(), the
/// same call the old order strip made, which the chain applies with its click-free dip.
class ChainStrip final : public juce::Component, private juce::Timer
{
public:
    explicit ChainStrip (AmpSimProcessor& processor);
    ~ChainStrip() override;

    /// A click selected this block.
    std::function<void (BlockId)> onSelect;

    void setSelected (BlockId id);
    BlockId getSelected() const noexcept { return selected; }

    /// Message thread, ten times a second: the orders, the switches, and the state lines.
    void refresh (const AmpSimProcessor::Status& status);

    /// For tests: a block's card, and a section's blocks as shown, left to right.
    juce::Component* getCard (BlockId id) const;
    std::vector<BlockId> getShownOrder (Section section) const;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Dragging a card along its section (positions in the strip's coordinates). The cards call these.
    void beginDrag (BlockId id, juce::Point<float> position);
    void dragTo (juce::Point<float> position);
    void endDrag();
    bool isDragging() const noexcept { return dragged != BlockId::count; }

private:
    class Card;

    void timerCallback() override;
    void placeCards (bool animate);
    juce::Rectangle<int> slot (Section section, int index) const;
    juce::Rectangle<int> sectionBox (Section section) const { return section == Section::pre ? preBox : postBox; }
    std::vector<BlockId>& order (Section section) { return section == Section::pre ? preOrder : postOrder; }
    juce::String describe (BlockId id, const AmpSimProcessor::Status& status) const;
    void select (BlockId id);

    AmpSimProcessor& ampSim;
    std::array<std::unique_ptr<Card>, numBlocks> cards;
    std::vector<BlockId> preOrder, postOrder;
    BlockId selected = BlockId::amp;

    // Layout, from resized().
    int cardWidth = 104;
    juce::Rectangle<int> preBox, postBox, inputSlot, ampSlot, gateBSlot, cabSlot, outputSlot;

    // A drag in progress.
    BlockId dragged = BlockId::count;
    Section dragSection = Section::pre;
    std::vector<BlockId> dragOthers; // the section without the dragged card
    int dragInsert = 0;
    float dragGrab = 0.0f;
};

} // namespace ui

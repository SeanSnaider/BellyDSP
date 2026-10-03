#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

#include <memory>
#include <vector>

namespace ui
{

/// A row of text tabs in the amp tabs' style (handoff 4.5, .amp-select): 30 px apart, 15 px medium names
/// in ink-dim (ink when hovered or selected), an optional small suffix in 11 px ink-faint ("PC 1"), an
/// optional icon before the name (the amp tabs' 30 x 20 minis), an optional bypass dot after it, 12 px of
/// padding under the text, a 1 px emerald underline on the selected tab, and a 1 px line under the row.
///
/// With onReorder set, a tab can be dragged along the row (ASSUMPTIONS H1): the others make room where it
/// will land, and dropping it reports the new order. A short wiggle is a click, not a drag.
class TabRow final : public juce::Component
{
public:
    struct Item
    {
        int id = 0;
        juce::String label, suffix;
        juce::String parameterId; // the bypass dot's parameter (empty: no dot)
        bool inverted = false;
    };

    explicit TabRow (AmpSimProcessor& processor);
    ~TabRow() override;

    /// Rebuilds the tabs (only if the items changed).
    void setItems (const std::vector<Item>& items);
    void setSelected (int id);
    int getSelected() const noexcept { return selected; }

    std::function<void (int id)> onSelect;
    std::function<void (const std::vector<int>& order)> onReorder;
    std::function<void (juce::Graphics&, juce::Rectangle<float> box, int id)> drawIcon;
    int iconWidth = 0, iconHeight = 0;

    /// For tests: a tab, and the ids left to right as shown.
    juce::Component* getTab (int id) const;
    std::vector<int> getShownOrder() const;

    static constexpr int height = 33; // 20 of content, 12 of padding, the 1 px line
    static constexpr int gap = 30;

    void paint (juce::Graphics&) override;
    void resized() override;

    // A drag in progress (the tabs call these; positions in the row's coordinates).
    void beginDrag (int id, float grabOffset);
    void dragTo (float x);
    void endDrag();

private:
    class Tab;
    void place();
    int tabWidth (const Item& item) const;

    AmpSimProcessor& ampSim;
    std::vector<Item> items;
    std::vector<std::unique_ptr<Tab>> tabs;
    int selected = -1;

    int dragged = -1, dragInsert = 0;
    float dragX = 0.0f, dragGrab = 0.0f;
};

} // namespace ui

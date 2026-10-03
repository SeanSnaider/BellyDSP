// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Controls.h"
#include "Scenes.h"

#include <array>
#include <memory>

namespace ui
{

/// The scenes (on the Output page, ASSUMPTIONS UH6): eight numbered tiles with the scene's name under the
/// number, and Store. A stored scene is bright, an empty one faint, the current one outlined in emerald
/// over the soft emerald fill. The behaviour is the editor's (click to recall or to store into an empty
/// scene; Store, then a scene, to overwrite; right-click for store, rename, and clear).
class ScenesBar final : public juce::Component
{
public:
    ScenesBar();
    ~ScenesBar() override;

    std::function<void (int)> onClick;

    /// Message thread: the tiles from the preset's scenes.
    void refresh (const Scenes& scenes);

    bool isStoreArmed() const { return store.getToggleState(); }
    void setStoreArmed (bool armed);

    /// The tile for scene `index` (0 to 7); its "sceneIndex" property is the index, for the editor's
    /// right-click handling.
    juce::Component& getTile (int index);
    juce::Button& getStoreButton() noexcept { return store; }
    static inline const juce::Identifier sceneIndexProperty { "sceneIndex" };

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Tile;
    std::array<std::unique_ptr<Tile>, Scenes::count> tiles;
    juce::TextButton store { "Store" };
};

} // namespace ui

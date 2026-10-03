// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ScenesBar.h"

namespace ui
{

using namespace theme;

class ScenesBar::Tile final : public IgnoresRightClick<juce::Button>
{
public:
    explicit Tile (int sceneIndex) : IgnoresRightClick<juce::Button> ("Scene " + juce::String (sceneIndex + 1)), index (sceneIndex)
    {
        getProperties().set (sceneIndexProperty, index);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void set (bool isStored, bool isCurrent, const juce::String& sceneName, bool storeArmed)
    {
        if (isStored != stored || isCurrent != current || sceneName != name || storeArmed != armed)
        {
            stored = isStored;
            current = isCurrent;
            name = sceneName;
            armed = storeArmed;
            setTooltip (stored ? name + ": click to recall; right-click to store over it, rename, or clear"
                               : juce::String ("Empty: click to store the current sound here"));
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        // Hairline tiles: a stored scene in ink, an empty one faint; the current one outlined in emerald
        // over the soft emerald fill; armed for storing, every tile outlined in emerald (a target).
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        if (current && stored)
        {
            g.setColour (accentDim);
            g.fillRoundedRectangle (bounds, radiusControl);
        }
        else if (down)
        {
            g.setColour (surface);
            g.fillRoundedRectangle (bounds, radiusControl);
        }
        g.setColour (armed || (current && stored) ? accent : (highlighted ? inkFaint : line2));
        g.drawRoundedRectangle (bounds, radiusControl, 1.0f);

        auto area = getLocalBounds().reduced (10, 4);
        g.setFont (tabular (geist (Weight::semibold, 14.0f)));
        g.setColour (stored ? ink : inkFaint);
        g.drawText (juce::String (index + 1), area.removeFromTop (area.getHeight() / 2 + 1), juce::Justification::bottomLeft, false);
        g.setFont (geist (Weight::regular, 11.0f));
        g.setColour (stored ? inkDim : inkFaint);
        g.drawFittedText (stored ? name : juce::String ("Empty"), area, juce::Justification::topLeft, 1, 0.8f);
    }

private:
    const int index;
    bool stored = false, current = false, armed = false;
    juce::String name;
};

ScenesBar::ScenesBar()
{
    for (int i = 0; i < Scenes::count; ++i)
    {
        tiles[(size_t) i] = std::make_unique<Tile> (i);
        tiles[(size_t) i]->onClick = [this, i]
        {
            if (onClick)
                onClick (i);
        };
        addAndMakeVisible (*tiles[(size_t) i]);
    }

    store.setClickingTogglesState (true);
    store.setTooltip ("Then click a scene to store the current sound in it");
    store.onClick = [this] { setStoreArmed (store.getToggleState()); };
    addAndMakeVisible (store);
}

ScenesBar::~ScenesBar() = default;

juce::Component& ScenesBar::getTile (int index)
{
    return *tiles[(size_t) juce::jlimit (0, Scenes::count - 1, index)];
}

void ScenesBar::setStoreArmed (bool armed)
{
    store.setToggleState (armed, juce::dontSendNotification);
    store.setButtonText (armed ? "Pick a scene" : "Store");
}

void ScenesBar::refresh (const Scenes& scenes)
{
    for (int i = 0; i < Scenes::count; ++i)
    {
        const auto& scene = scenes.get (i);
        tiles[(size_t) i]->set (scene.stored, scenes.getCurrent() == i, scene.name, isStoreArmed());
    }
}

void ScenesBar::paint (juce::Graphics&) {}

void ScenesBar::resized()
{
    auto area = getLocalBounds();
    store.setBounds (area.removeFromRight (112).withSizeKeepingCentre (112, controlHeight));
    area.removeFromRight (space::l);

    const auto gap = space::s;
    const auto width = juce::jmin (150, (area.getWidth() - (Scenes::count - 1) * gap) / Scenes::count);
    for (auto& tile : tiles)
    {
        tile->setBounds (area.removeFromLeft (width));
        area.removeFromLeft (gap);
    }
}

} // namespace ui

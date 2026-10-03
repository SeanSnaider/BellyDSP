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
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto alpha = stored ? 1.0f : offAlpha;

        auto fill = stored ? surfaceRaised : surface;
        if (down)
            fill = fill.darker (0.15f);
        else if (highlighted)
            fill = fill.brighter (0.07f);
        g.setColour (fill);
        g.fillRoundedRectangle (bounds, radiusControl);
        if (current && stored)
        {
            g.setColour (accentSoft);
            g.fillRoundedRectangle (bounds, radiusControl);
        }

        // Armed for storing: every tile is a target, outlined in amber.
        g.setColour (armed ? warn : (current && stored ? accent : outline));
        g.drawRoundedRectangle (bounds, radiusControl, current || armed ? 1.5f : 1.0f);

        auto area = getLocalBounds().reduced (10, 4);
        g.setFont (font ("Semibold", 14.0f));
        g.setColour (theme::text.withMultipliedAlpha (alpha));
        g.drawText (juce::String (index + 1), area.removeFromTop (area.getHeight() / 2 + 1), juce::Justification::bottomLeft, false);
        g.setFont (font (Text::caption));
        g.setColour ((current && stored ? theme::text : textDim).withMultipliedAlpha (alpha));
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
    store.setColour (juce::TextButton::buttonOnColourId, warn);
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

void ScenesBar::paint (juce::Graphics& g)
{
    g.setColour (surface);
    g.fillRect (getLocalBounds());
    g.setColour (outline);
    g.fillRect (getLocalBounds().withHeight (1));

    g.setFont (font ("Semibold", 11.0f).withExtraKerningFactor (0.08f));
    g.setColour (textDim);
    g.drawText ("SCENES", getLocalBounds().withTrimmedLeft (space::l).withWidth (60), juce::Justification::centredLeft, false);
}

void ScenesBar::resized()
{
    auto area = getLocalBounds().reduced (space::l, 6).withTrimmedTop (1);
    area.removeFromLeft (64); // the label
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

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ScenesBar.h"

namespace ui
{

using namespace theme;

namespace
{
double nowSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}
} // namespace

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
            const auto key = " (key " + juce::String (index + 1) + ")";
            setTooltip (stored ? name + ": click to recall" + key + "; right-click to store over it, rename, or clear"
                               : "Scene " + juce::String (index + 1) + " is empty: click to store the current sound here");
            repaint();
        }
    }

    bool isLit() const noexcept { return current && stored; }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        // A stored scene on a raised button; an empty one a faint outline. The current one's orange light is the
        // bar's (it glides), so this only draws its text dark on it. Armed for storing, every button is outlined.
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        if (! isLit())
        {
            if (stored)
            {
                g.setColour (down ? line1 : (highlighted ? raised.brighter (0.06f) : raised));
                g.fillRoundedRectangle (bounds, radiusControl);
            }
            g.setColour (armed ? accent : (stored ? line1 : (highlighted ? inkFaint : line1)));
            g.drawRoundedRectangle (bounds, radiusControl, 1.0f);
        }
        else if (armed)
        {
            g.setColour (ink);
            g.drawRoundedRectangle (bounds, radiusControl, 1.0f);
        }

        auto area = getLocalBounds().reduced (12, 0);
        const auto numberFont = tabular (geist (Weight::semibold, 14.0f));
        g.setFont (numberFont);
        g.setColour (isLit() ? onAccent : (stored ? ink : inkFaint));
        const auto numberWidth = juce::roundToInt (textWidth (numberFont, juce::String (index + 1))) + 8;
        g.drawText (juce::String (index + 1), area.removeFromLeft (numberWidth), juce::Justification::centredLeft, false);
        g.setFont (geist (isLit() ? Weight::semibold : Weight::regular, 13.0f));
        g.setColour (isLit() ? onAccent : (stored ? inkDim : inkFaint));
        g.drawFittedText (stored ? name : juce::String ("Empty"), area, juce::Justification::centredLeft, 1, 0.8f);
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

ScenesBar::~ScenesBar()
{
    stopTimer();
}

juce::Component& ScenesBar::getTile (int index)
{
    return *tiles[(size_t) juce::jlimit (0, Scenes::count - 1, index)];
}

void ScenesBar::setStoreArmed (bool armed)
{
    store.setToggleState (armed, juce::dontSendNotification);
    store.setButtonText (armed ? "Pick a scene" : "Store");
}

juce::Rectangle<float> ScenesBar::targetLight() const
{
    return lit >= 0 ? tiles[(size_t) lit]->getBounds().toFloat() : juce::Rectangle<float>();
}

void ScenesBar::refresh (const Scenes& scenes)
{
    auto newLit = -1;
    for (int i = 0; i < Scenes::count; ++i)
    {
        const auto& scene = scenes.get (i);
        tiles[(size_t) i]->set (scene.stored, scenes.getCurrent() == i, scene.name, isStoreArmed());
        if (tiles[(size_t) i]->isLit())
            newLit = i;
    }
    if (newLit == lit)
        return;

    // The light glides from where it was to the new scene (it appears in place when there was none).
    const auto had = lit >= 0 && ! light.isEmpty();
    lit = newLit;
    if (lit < 0)
    {
        light = {};
        stopTimer();
    }
    else if (! had)
    {
        light = targetLight();
    }
    else
    {
        glideFrom = light;
        glideStart = nowSeconds();
        startTimerHz (animationFps);
    }
    repaint();
}

void ScenesBar::finishGlide()
{
    stopTimer();
    light = targetLight();
    repaint();
}

void ScenesBar::timerCallback()
{
    // Ease out (a cubic): quick to leave, settling into place.
    const auto t = juce::jlimit (0.0, 1.0, (nowSeconds() - glideStart) / sceneGlideSeconds);
    const auto e = (float) (1.0 - std::pow (1.0 - t, 3.0));
    const auto to = targetLight();
    light = { glideFrom.getX() + (to.getX() - glideFrom.getX()) * e, to.getY(), glideFrom.getWidth() + (to.getWidth() - glideFrom.getWidth()) * e, to.getHeight() };
    if (t >= 1.0)
        finishGlide();
    else
        repaint();
}

void ScenesBar::paint (juce::Graphics& g)
{
    // The current scene's light, under the buttons.
    if (! light.isEmpty())
    {
        g.setColour (accent.withAlpha (0.18f));
        g.fillRoundedRectangle (light.expanded (3.0f), radiusControl + 3.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (light.reduced (0.5f), radiusControl);
    }
}

void ScenesBar::resized()
{
    auto area = getLocalBounds();
    store.setBounds (area.removeFromRight (104).withSizeKeepingCentre (104, 34));
    area.removeFromRight (space::l);

    const auto gap = space::s;
    const auto width = juce::jmin (124, (area.getWidth() - (Scenes::count - 1) * gap) / Scenes::count);
    for (auto& tile : tiles)
    {
        tile->setBounds (area.removeFromLeft (width));
        area.removeFromLeft (gap);
    }
    if (! isTimerRunning())
        light = targetLight();
}

// ---- TunerReadout -----------------------------------------------------------------------------------

TunerReadout::TunerReadout()
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("The tuner (T): click to open or close it");
}

void TunerReadout::set (bool isEngaged, const ampsim::TunerReading& reading)
{
    const auto newHasNote = isEngaged && reading.hasReading && reading.midiNote >= 0;
    juce::String newNote, newCents, newVerdict;
    auto newInTune = false;
    if (newHasNote)
    {
        newNote = juce::MidiMessage::getMidiNoteName (reading.midiNote, true, true, 4);
        const auto c = juce::roundToInt (reading.cents);
        newCents = (c > 0 ? "+" : "") + juce::String (c) + " ct";
        newInTune = std::abs (reading.cents) <= 3.0;
        newVerdict = newInTune ? "In tune" : (reading.cents < 0.0 ? "Flat" : "Sharp");
    }
    if (isEngaged != engaged || newHasNote != hasNote || newNote != note || newCents != cents || newVerdict != verdict || newInTune != inTune)
    {
        engaged = isEngaged;
        hasNote = newHasNote;
        note = newNote;
        cents = newCents;
        verdict = newVerdict;
        inTune = newInTune;
        repaint();
    }
}

juce::String TunerReadout::getText() const
{
    if (! engaged)
        return "Tuner  T";
    if (! hasNote)
        return "Tuner: play a string";
    return note + " " + cents + " " + verdict;
}

void TunerReadout::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    if (! engaged || ! hasNote)
    {
        // "Tuner" and its key in a small keycap, or the prompt while it listens.
        const auto words = engaged ? juce::String ("Tuner: play a string") : juce::String ("Tuner");
        g.setFont (geist (Weight::regular, 13.0f));
        const auto w = textWidth (geist (Weight::regular, 13.0f), words);
        auto x = area.getRight() - w - (engaged ? 0.0f : 28.0f);
        g.setColour (engaged ? ink : inkDim);
        g.drawText (words, juce::Rectangle<float> (x, area.getY(), w + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);
        if (! engaged)
        {
            const auto cap = juce::Rectangle<float> (area.getRight() - 20.0f, area.getCentreY() - 10.0f, 20.0f, 20.0f);
            g.setColour (line2);
            g.drawRoundedRectangle (cap.reduced (0.5f), 4.0f, 1.0f);
            g.setFont (geist (Weight::medium, 11.0f));
            g.setColour (inkDim);
            g.drawText ("T", cap, juce::Justification::centred, false);
        }
        return;
    }

    // Right to left: the verdict, the cents, the note (large).
    const auto verdictFont = geist (Weight::medium, 13.0f), centsFont = tabular (geist (Weight::regular, 13.0f)), noteFont = geist (Weight::semibold, 22.0f);
    const auto vw = textWidth (verdictFont, verdict), cw = textWidth (centsFont, cents), nw = textWidth (noteFont, note);
    auto x = area.getRight() - vw;
    g.setFont (verdictFont);
    g.setColour (inTune ? accent : inkDim);
    g.drawText (verdict, juce::Rectangle<float> (x, area.getY(), vw + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);
    x -= 12.0f + cw;
    g.setFont (centsFont);
    g.setColour (inkDim);
    g.drawText (cents, juce::Rectangle<float> (x, area.getY(), cw + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);
    x -= 12.0f + nw;
    g.setFont (noteFont);
    g.setColour (inTune ? accent : ink);
    g.drawText (note, juce::Rectangle<float> (x, area.getY(), nw + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);
}

void TunerReadout::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && onClick)
        onClick();
}

} // namespace ui

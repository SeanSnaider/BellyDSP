// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Controls.h"
#include "Scenes.h"
#include "dsp/TunerAnalysis.h"

#include <array>
#include <memory>

namespace ui
{

/// The scenes, along the main screen's footer: eight buttons, each its number and its name, then Store. A stored
/// scene reads in ink on a raised button, an empty one is a faint outline; the current one is lit in orange, and on a
/// change the light glides to its new place. Armed for storing, every button is outlined in orange (a target). The
/// behaviour is the editor's (click to recall, or to store into an empty scene; Store, then a scene, to overwrite;
/// right-click for store, rename, and clear; the keys 1 to 8 recall).
class ScenesBar final : public juce::Component, private juce::Timer
{
public:
    ScenesBar();
    ~ScenesBar() override;

    std::function<void (int)> onClick;

    /// Message thread: the buttons from the preset's scenes.
    void refresh (const Scenes& scenes);

    bool isStoreArmed() const { return store.getToggleState(); }
    void setStoreArmed (bool armed);

    /// The button for scene `index` (0 to 7); its "sceneIndex" property is the index, for the editor's
    /// right-click handling.
    juce::Component& getTile (int index);
    juce::Button& getStoreButton() noexcept { return store; }
    static inline const juce::Identifier sceneIndexProperty { "sceneIndex" };

    /// The lit scene (-1 for none), where the light is drawn now, and the end of a glide (tests and snapshots).
    int getLitScene() const noexcept { return lit; }
    juce::Rectangle<float> getLightBounds() const noexcept { return light; }
    bool isGliding() const noexcept { return isTimerRunning(); }
    void finishGlide();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Tile;
    void timerCallback() override;
    juce::Rectangle<float> targetLight() const;

    std::array<std::unique_ptr<Tile>, Scenes::count> tiles;
    juce::TextButton store { "Store" };
    int lit = -1;
    juce::Rectangle<float> light, glideFrom;
    double glideStart = 0.0;
};

/// The tuner's reading in the footer: while the tuner is engaged, the note it hears (orange within 3 cents), the
/// cents, and Flat, Sharp, or In tune; otherwise "Tuner" and its key. A click opens or closes the tuner.
class TunerReadout final : public juce::Component, public juce::SettableTooltipClient
{
public:
    TunerReadout();

    std::function<void()> onClick;

    /// Message thread: whether the tuner is engaged, and its reading.
    void set (bool engaged, const ampsim::TunerReading& reading);
    /// What it shows, for tests: "E2 +3 ct In tune", "Tuner: play a string", "Tuner  T".
    juce::String getText() const;

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    bool engaged = false, hasNote = false, inTune = false;
    juce::String note, cents, verdict;
};

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"

#include <array>

namespace ui
{

/// The stage (the main screen's upper left, 980 x 350): a dark room lit from below, its floor catching a warm
/// light, showing the playing amp, the cab, or an EQ's graph. A standard segmented control at its top right picks
/// which (Amp, Cab, EQ); the views crossfade as it switches. The views are the editor's (AmpView, CabView, EqStage);
/// the stage starts and stops them as they show and hide (ControlGroup::pageShown and pageHidden).
class Stage final : public juce::Component, private juce::Timer
{
public:
    Stage (ControlGroup& amp, ControlGroup& cab, ControlGroup& eq);
    ~Stage() override;

    /// The player picked a mode on the control (not called by setMode).
    std::function<void (StageMode)> onModeChosen;

    /// Shows a mode: crossfading (theme::stageFadeSeconds) or at once.
    void setMode (StageMode mode, bool animate = true);
    StageMode getMode() const noexcept { return mode; }
    Segmented& getModeControl() noexcept { return control; }
    ControlGroup& getView (StageMode m) noexcept { return *views[(size_t) m]; }

    /// For tests and snapshots: whether a crossfade is running, and its end now.
    bool isFading() const noexcept { return isTimerRunning(); }
    void finishFade();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    std::array<ControlGroup*, numStageModes> views {};
    Segmented control;
    StageMode mode = StageMode::amp, leaving = StageMode::count;
    double fadeStart = 0.0;
};

/// The inspector (the main screen's right, 300 wide, from the top bar to the footer): the selected block's
/// controls, in a column that scrolls when it's taller than the panel. Its content is a page or panel owned
/// elsewhere (a BlockPage, the amp's or the cab's panel); the inspector starts and stops it as it shows and hides,
/// and re-measures it each refresh (a mode that shows other controls changes its height).
class Inspector final : public juce::Component
{
public:
    Inspector();
    ~Inspector() override;

    void show (ControlGroup* content);
    ControlGroup* getContent() const noexcept { return content; }
    juce::Viewport& getViewport() noexcept { return viewport; }

    /// Ten times a second: the content's refresh, and its height.
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int scrollBarWidth = 8;

private:
    void fit();
    juce::Viewport viewport;
    ControlGroup* content = nullptr;
};

/// An overlay's frame (the tuner, Match tone, Settings): it covers the main screen between the top bar and the
/// footer with the ground colour and holds a page there, padded as the pages were designed (40 at the sides, 16 at
/// the top and bottom: the 1200 x 598 they were drawn for). The tuner gets a header with its title, the key that
/// closes it, and Close; Match tone and Settings bring their own.
class OverlayFrame final : public juce::Component
{
public:
    OverlayFrame();

    /// Holds a page (owned elsewhere), with a header (a title) or without one.
    void hold (juce::Component* page, const juce::String& title);
    juce::Component* getPage() const noexcept { return page; }

    std::function<void()> onClose;
    juce::Button& getCloseButton() noexcept { return close; }

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int padSide = 40, padTop = 16, padBottom = 16, headerHeight = 44;

private:
    juce::Component* page = nullptr;
    juce::String title;
    juce::TextButton close { "Close" };
};

} // namespace ui

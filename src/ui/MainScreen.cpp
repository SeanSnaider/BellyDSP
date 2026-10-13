// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "MainScreen.h"

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

// ---- Stage ----------------------------------------------------------------------------------------------

Stage::Stage (ControlGroup& amp, ControlGroup& cab, ControlGroup& eq)
    : control (juce::StringArray { stageModeLabel (StageMode::amp), stageModeLabel (StageMode::cab), stageModeLabel (StageMode::eq) })
{
    views = { &amp, &cab, &eq };
    for (auto* v : views)
    {
        addChildComponent (*v);
        v->setAlpha (1.0f);
    }
    control.setPillStyle (true);
    control.setTooltip ("What the stage shows: the amp, the cab, or an EQ's graph");
    control.setOptionTooltips ({ "The amp, its head and its shelf", "The cab: its library, speaker, and mics", "An EQ's graph: the post EQ or the pre EQ" });
    control.onChange = [this] (int i)
    {
        setMode ((StageMode) i);
        if (onModeChosen)
            onModeChosen ((StageMode) i);
    };
    addAndMakeVisible (control);
    views[(size_t) mode]->setVisible (true);
}

Stage::~Stage()
{
    stopTimer();
}

void Stage::setMode (StageMode m, bool animate)
{
    if (m == StageMode::count)
        return;
    control.setSelected ((int) m);
    if (m == mode && views[(size_t) m]->isVisible())
        return;
    if (isTimerRunning())
        finishFade();

    auto& before = *views[(size_t) mode];
    auto& after = *views[(size_t) m];
    const auto changed = &before != &after;
    leaving = changed ? mode : StageMode::count;
    mode = m;
    after.setVisible (true);
    after.toFront (false);
    control.toFront (false);
    after.pageShown();
    after.refresh();
    if (changed && animate)
    {
        // The crossfade: the new view rises from nothing over the old one, which then goes.
        after.setAlpha (0.0f);
        fadeStart = nowSeconds();
        startTimerHz (animationFps);
    }
    else
    {
        after.setAlpha (1.0f);
        if (changed)
        {
            before.setVisible (false);
            before.pageHidden();
        }
        leaving = StageMode::count;
    }
}

void Stage::finishFade()
{
    stopTimer();
    views[(size_t) mode]->setAlpha (1.0f);
    if (leaving != StageMode::count)
    {
        auto& before = *views[(size_t) leaving];
        before.setVisible (false);
        before.setAlpha (1.0f);
        before.pageHidden();
    }
    leaving = StageMode::count;
}

void Stage::timerCallback()
{
    const auto t = juce::jlimit (0.0, 1.0, (nowSeconds() - fadeStart) / stageFadeSeconds);
    // Smoothstep, so it eases in and out.
    const auto s = (float) (t * t * (3.0 - 2.0 * t));
    views[(size_t) mode]->setAlpha (s);
    if (leaving != StageMode::count)
        views[(size_t) leaving]->setAlpha (1.0f - s);
    if (t >= 1.0)
        finishFade();
}

void Stage::paint (juce::Graphics& g)
{
    // The dark room, and its light: a warm pool on the floor from below, widest at the bottom edge, fading into
    // the dark by two thirds of the way up; the floor itself a shade lighter than the wall, meeting it softly.
    const auto b = getLocalBounds().toFloat();
    g.fillAll (bg);
    const auto horizon = b.getBottom() - b.getHeight() * 0.26f;
    g.setGradientFill (juce::ColourGradient (bg, 0.0f, horizon - 30.0f, juce::Colour (0xff181716), 0.0f, b.getBottom(), false));
    g.fillRect (b.withTop (horizon - 30.0f));

    const juce::Point<float> centre { b.getCentreX(), b.getBottom() + b.getHeight() * 0.08f };
    const auto rx = b.getWidth() * 0.62f, ry = b.getHeight() * 0.95f;
    {
        const juce::Graphics::ScopedSaveState saved (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, ry / rx, centre.x, centre.y));
        juce::ColourGradient pool (stageLight, centre, stageLight.withAlpha (0.0f), centre.translated (rx, 0.0f), true);
        pool.addColour (0.45, stageLight.withAlpha (0.45f));
        g.setGradientFill (pool);
        g.fillEllipse (juce::Rectangle<float> (2.0f * rx, 2.0f * rx).withCentre (centre));
    }
    // A whisper of the accent on the floor under the gear: a flat ellipse, so it has no edges.
    {
        const juce::Point<float> c { b.getCentreX(), b.getBottom() };
        const auto wx = b.getWidth() * 0.42f, wy = 70.0f;
        const juce::Graphics::ScopedSaveState saved (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, wy / wx, c.x, c.y));
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.06f), c, accent.withAlpha (0.0f), c.translated (wx, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (2.0f * wx, 2.0f * wx).withCentre (c));
    }
}

void Stage::resized()
{
    for (auto* v : views)
        v->setBounds (getLocalBounds());
    const auto w = control.getPreferredWidth();
    control.setBounds (getWidth() - 24 - w, 16, w, Segmented::pillHeight);
}

// ---- Inspector ------------------------------------------------------------------------------------------

Inspector::Inspector()
{
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (scrollBarWidth);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
    addAndMakeVisible (viewport);
}

Inspector::~Inspector()
{
    viewport.setViewedComponent (nullptr, false);
}

void Inspector::show (ControlGroup* newContent)
{
    if (newContent == content)
        return;
    if (content != nullptr)
    {
        content->pageHidden();
        viewport.setViewedComponent (nullptr, false);
    }
    content = newContent;
    if (content != nullptr)
    {
        content->setVisible (true);
        fit();
        viewport.setViewedComponent (content, false);
        viewport.setViewPosition (0, 0);
        content->pageShown();
        content->refresh();
        fit();
    }
}

void Inspector::fit()
{
    if (content == nullptr || viewport.getWidth() <= 0)
        return;
    const auto width = viewport.getWidth() - scrollBarWidth;
    const auto height = juce::jmax (viewport.getHeight(), content->preferredHeight (width));
    if (content->getWidth() != width || content->getHeight() != height)
        content->setSize (width, height);
}

void Inspector::refresh()
{
    if (content == nullptr)
        return;
    content->refresh();
    fit();
}

void Inspector::paint (juce::Graphics& g)
{
    g.fillAll (panelBg);
    g.setColour (line1);
    g.fillRect (0, 0, 1, getHeight());
}

void Inspector::resized()
{
    viewport.setBounds (getLocalBounds().withTrimmedLeft (1));
    fit();
}

// ---- OverlayFrame ---------------------------------------------------------------------------------------

OverlayFrame::OverlayFrame()
{
    close.setTooltip ("Back to the main screen (Esc)");
    close.onClick = [this] { if (onClose) onClose(); };
    close.setHasFocusOutline (true);
    addChildComponent (close);
    setInterceptsMouseClicks (true, true); // the main screen under it takes no clicks while it's open
}

void OverlayFrame::hold (juce::Component* newPage, const juce::String& newTitle)
{
    if (page != nullptr && page != newPage && page->getParentComponent() == this)
        removeChildComponent (page);
    page = newPage;
    title = newTitle;
    close.setVisible (title.isNotEmpty());
    if (page != nullptr)
        addAndMakeVisible (*page);
    resized();
    repaint();
}

void OverlayFrame::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    if (title.isNotEmpty())
    {
        g.setFont (geist (Weight::semibold, 18.0f));
        g.setColour (ink);
        const auto w = textWidth (geist (Weight::semibold, 18.0f), title);
        g.drawText (title, juce::Rectangle<float> ((float) padSide, (float) padTop, w + 2.0f, 30.0f), juce::Justification::centredLeft, false);
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkFaint);
        g.drawText ("T or Esc closes it", juce::Rectangle<float> ((float) padSide + w + 14.0f, (float) padTop, 200.0f, 30.0f), juce::Justification::centredLeft,
                    false);
        g.setColour (line1);
        g.fillRect (padSide, padTop + headerHeight - 2, getWidth() - 2 * padSide, 1);
    }
}

void OverlayFrame::resized()
{
    auto area = getLocalBounds().withTrimmedLeft (padSide).withTrimmedRight (padSide).withTrimmedTop (padTop).withTrimmedBottom (padBottom);
    if (title.isNotEmpty())
    {
        auto header = area.removeFromTop (headerHeight);
        close.setBounds (header.getRight() - 80, header.getY() + 1, 80, controlHeight);
    }
    if (page != nullptr)
        page->setBounds (area);
}

} // namespace ui

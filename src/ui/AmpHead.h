// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Theme.h"

#include <array>

namespace ui
{

/// The three amp heads' materials (handoff 4.6): slot 1 wears Glass, slot 2 Ember, slot 3 Monolith, always
/// (ASSUMPTIONS UH12). The names are invented and appear on the badges and tabs.
enum class Material
{
    glass,
    ember,
    monolith
};

inline Material materialFor (int slot) { return (Material) juce::jlimit (0, 2, slot); }
const char* materialName (Material material);
theme::Skin skinFor (Material material);

/// The amp head (handoff 4.6), one component for all three: a 150 x 14 handle above a 960 x 262 body
/// (radius 14, padding 16, a soft drop shadow), the 118 px control panel on top and the grille below
/// (radius 6, an inset shadow, the badge centred), and two feet under the bottom edge. Only the materials
/// change: the body's tolex, the panel, the grille's weave, and the badge.
///
/// Everything here is drawn once, procedurally, into a cached image at the scale it's shown at, and only
/// re-rendered when that scale or the material changes; a repaint just draws the image (handoff 4.6,
/// "Textures"). The patterns are the reference's CSS gradients evaluated per pixel. The knobs, the pilot
/// jewel, and the grille's click area are separate components on top.
///
/// The component's bounds are the body's box grown by `margin` (room for the handle, the feet, and the
/// shadow); headBox() is the body's box inside it.
class AmpHead final : public juce::Component
{
public:
    AmpHead();

    void setMaterial (Material m);
    Material getMaterial() const noexcept { return material; }

    /// The word on the badge, in the material's lettering: the amp the slot plays ("Comet" on the Glass head, since
    /// any slot can load any built-in amp); empty: the material's own name. Re-renders the art when it changes.
    void setBadge (const juce::String& name);
    juce::String getBadge() const;

    static constexpr int headWidth = 960, headHeight = 262;
    static inline const juce::BorderSize<int> margin { 24, 56, 72, 56 }; // top, left, bottom, right
    static juce::Rectangle<int> headBox() { return { margin.getLeft(), margin.getTop(), headWidth, headHeight }; }

    /// The panel and the grille, in the component's coordinates.
    static juce::Rectangle<float> panelBox() { return headBox().toFloat().reduced (16.0f).withHeight (118.0f); }
    static juce::Rectangle<float> grilleBox() { return headBox().toFloat().reduced (16.0f).withTrimmedTop (130.0f); }

    /// For tests: how many times the art has been rendered (it must not be per frame).
    int getRenderCount() const noexcept { return renders; }

    void paint (juce::Graphics&) override;

    /// The art at a scale, uncached (tests and the cache), with a badge word (empty: the material's name).
    static juce::Image render (Material material, float scale, const juce::String& name = {});

private:
    Material material = Material::glass;
    juce::String badge; // empty: the material's name
    juce::Image cached;
    float cachedScale = 0.0f;
    Material cachedMaterial = Material::glass;
    juce::String cachedBadge;
    int renders = 0;
};

/// The pilot light (handoff 4.6): an 18 px jewel, lit emerald with a glow while the slot has a capture
/// loaded, dark otherwise (UH4).
class PilotJewel final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PilotJewel() { setInterceptsMouseClicks (false, false); }
    void setLit (bool shouldBeLit);
    bool isLit() const noexcept { return lit; }
    void paint (juce::Graphics&) override;

private:
    bool lit = false;
};

/// The amp tabs' 30 x 20 mini heads (handoff 4.5, .mini).
void drawMiniHead (juce::Graphics& g, juce::Rectangle<float> box, Material material);

/// A built-in amp's 30 x 20 mini head, by its name (the Amp page's shelf): the three slot materials' own minis for
/// Glass, Ember, and Monolith, a look of its own for each of the five more (Forge, Basalt, Comet, Quartz, Lantern),
/// and a plain grey one for any other gain set.
void drawMiniAmp (juce::Graphics& g, juce::Rectangle<float> box, const juce::String& amp);

} // namespace ui

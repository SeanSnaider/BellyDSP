// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

/// The design tokens of the app redesign (.impeccable/surfaces/src-plugineditor-cpp.md, the direction contract's
/// OWN-WORLD): every colour, type style, size, and radius the GUI uses, by name. Components never hold hex values
/// of their own; changing the look means changing this file.
///
/// A dark studio ground with hairlines and one orange accent, which only ever means "on, selected, or active"
/// (the LEDs, the knob arcs, the lit scene, the selected pedal). Red means one thing: a clip. Any other warning
/// is written in words, in the neutral inks (ASSUMPTIONS UH5). The amp heads keep their own materials and
/// knob skins (below); only the knobs' value arcs follow the accent.
namespace ui::theme
{

// ---- Colour: the redesign's tokens ---------------------------------------------------------------

inline const juce::Colour bg { 0xff121212 };         // the ground: the window, the stage's dark
inline const juce::Colour panelBg { 0xff1a1a1a };    // panels: the top bar, the inspector, the footer
inline const juce::Colour raised { 0xff1f1f1f };     // raised: buttons, fields, knob bodies, hover fills
inline const juce::Colour line1 { 0xff2c2c2c };      // hairlines: 1 px dividers and borders
inline const juce::Colour line2 { 0xff3a3a3a };      // stronger borders, inactive knob tracks, off states
inline const juce::Colour ink { 0xffe8e8e8 };        // primary text
inline const juce::Colour inkDim { 0xff9a9a9a };     // secondary text
inline const juce::Colour inkFaint { 0xff6b6b6b };   // tertiary text, axis labels (3.5:1 on the ground)
inline const juce::Colour accent { 0xffff8a00 };     // orange: on, selected, active, the LEDs, the value arcs
inline const juce::Colour accentDim { 0x24ff8a00 };  // rgba(255, 138, 0, 0.14): soft orange fills
inline const juce::Colour onAccent { 0xff121212 };   // text on an orange fill (the lit scene)
inline const juce::Colour clip { 0xffe5484d };       // red: a clip, and nothing else
inline const juce::Colour stageLight { 0xff34302b }; // the warm light on the stage's floor, from below
inline const juce::Colour boardTray { 0xff161616 };  // the board the pedals stand on
inline const juce::Colour letterbox { 0xff0a0a0a };  // around the 1280 x 760 canvas when the window's aspect differs
inline const juce::Colour scrim { 0xc80a0a0a };      // over the main screen while an overlay is open

/// Knob bodies in the chrome and hover fills: the raised colour.
inline const juce::Colour surface = raised;

// ---- Colour: the older names the effect pages were written with, mapped onto the tokens ----------

inline const juce::Colour background = bg;
inline const juce::Colour surfaceRaised = raised;
inline const juce::Colour outline = line2;
inline const juce::Colour text = ink;
inline const juce::Colour textDim = inkDim;
inline const juce::Colour accentSoft = accentDim;
inline const juce::Colour good = accent;   // in range, signal present
inline const juce::Colour warn = ink;      // the text itself carries the warning (UH5)
inline const juce::Colour error = ink;     // likewise
inline const juce::Colour sectionPre = inkFaint;
inline const juce::Colour sectionAmp = inkFaint;
inline const juce::Colour sectionCab = inkFaint;
inline const juce::Colour sectionPost = inkFaint;
inline const juce::Colour sectionInOut = inkFaint;
inline const juce::Colour micOne = accent; // the cab page's mic A
inline const juce::Colour micTwo = ink;    // mic B
inline const juce::Colour micRoom = inkDim;

/// A dimmed control (a knob the current mode doesn't use) and an off block's icon (handoff 4.4: 35%).
constexpr float offAlpha = 0.45f;
constexpr float unusedAlpha = 0.35f;

// ---- Knob skins (handoff 4.1) --------------------------------------------------------------------

struct KnobSkin
{
    juce::Colour a, b, ring, pointer, label, track;
};

enum class Skin
{
    chrome,   // the default, in the UI
    glass,    // amp 1's panel
    ember,    // amp 2's panel
    monolith, // amp 3's panel
    forge,    // the five more built-in amps' panels (ASSUMPTIONS UH20 to UH25), and a user capture's
    basalt,
    comet,
    quartz,
    lantern,
    custom
};

inline KnobSkin knobSkin (Skin skin)
{
    switch (skin)
    {
        case Skin::glass:    return { juce::Colour (0xfffafafa), juce::Colour (0xff9ea2a0), juce::Colour (0xff6d716f), juce::Colour (0xff1d1f1e),
                                      juce::Colour (0xff2b2e2c), juce::Colour (0x2e000000) };
        case Skin::ember:    return { juce::Colour (0xff3a3633), juce::Colour (0xff0f0e0d), juce::Colour (0xff000000), juce::Colour (0xffefdfba),
                                      juce::Colour (0xffd8c49a), juce::Colour (0x29efdfba) };
        // Monolith's pointer is its own emerald (the head's material), not the theme's accent.
        case Skin::monolith: return { juce::Colour (0xff3c3f3e), juce::Colour (0xff141515), juce::Colour (0xff050505), juce::Colour (0xff34d399),
                                      juce::Colour (0xff9aa4a0), juce::Colour (0x17ffffff) };
        // The five more heads and the custom one (UH20 to UH25): a body that reads on its own panel, a pointer in
        // the look's accent, a label with at least 4.5:1 contrast against the panel's middle.
        case Skin::forge:    return { juce::Colour (0xff45494d), juce::Colour (0xff0b0c0d), juce::Colour (0xff000000), juce::Colour (0xffff6a1f),
                                      juce::Colour (0xff121417), juce::Colour (0x38000000) };
        case Skin::basalt:   return { juce::Colour (0xff53575b), juce::Colour (0xff17181a), juce::Colour (0xff050505), juce::Colour (0xffe4e7ea),
                                      juce::Colour (0xffaab0b5), juce::Colour (0x1affffff) };
        case Skin::comet:    return { juce::Colour (0xff34437a), juce::Colour (0xff0a1027), juce::Colour (0xff02040f), juce::Colour (0xffb9f4ff),
                                      juce::Colour (0xffa3bce6), juce::Colour (0x248fd8ff) };
        case Skin::quartz:   return { juce::Colour (0xfffdfbff), juce::Colour (0xffb9aac9), juce::Colour (0xff7a6690), juce::Colour (0xff3a2a4c),
                                      juce::Colour (0xff261b31), juce::Colour (0x33261b31) };
        case Skin::lantern:  return { juce::Colour (0xfff7eed6), juce::Colour (0xffbda374), juce::Colour (0xff1e130a), juce::Colour (0xff3a2210),
                                      juce::Colour (0xffead6ab), juce::Colour (0x2bead6ab) };
        case Skin::custom:   return { juce::Colour (0xff45494d), juce::Colour (0xff151617), juce::Colour (0xff050505), juce::Colour (0xffdfe3e6),
                                      juce::Colour (0xffa1a7ac), juce::Colour (0x17ffffff) };
        case Skin::chrome:   break;
    }
    return { juce::Colour (0xff2b2b2b), juce::Colour (0xff171717), line2, ink, inkDim, line2 };
}

// ---- Type ----------------------------------------------------------------------------------------

/// Geist in four weights (and Fraunces for one badge), bundled with the app and loaded from memory once
/// (Fonts.cpp). If a file fails to load, the system UI font stands in. Sizes are CSS pixels, which are
/// JUCE's point heights (the em size).
enum class Weight
{
    light,    // 300
    regular,  // 400
    medium,   // 500
    semibold  // 600
};

juce::FontOptions geist (Weight weight, float size);

/// Fraunces SemiBold Italic, an instance of the variable font at the badge's optical size (52).
juce::FontOptions fraunces (float size);

/// Tabular figures, for numbers that change live (handoff 3: knob values, Hz, cents).
inline juce::FontOptions tabular (juce::FontOptions f) { return f.withFeatureEnabled ("tnum"); }

/// Whether the bundled fonts loaded (false: the system font is standing in).
bool bundledFontsLoaded();

/// The type styles the effect pages use by name.
enum class Text
{
    title,   // 15 medium: a page's title
    body,    // 13 regular: buttons, menus, combo boxes
    value,   // 12 medium, tabular: values in fields and meters
    label,   // 12 regular: captions, headings
    caption, // 11 regular: axis labels, small meta
    huge     // 180 light: the tuner's note
};

inline juce::FontOptions font (Text style)
{
    switch (style)
    {
        case Text::title:   return geist (Weight::medium, 15.0f);
        case Text::body:    return geist (Weight::regular, 13.0f);
        case Text::value:   return tabular (geist (Weight::medium, 12.0f));
        case Text::label:   return geist (Weight::regular, 12.0f);
        case Text::caption: return geist (Weight::regular, 11.0f);
        case Text::huge:    return geist (Weight::light, 180.0f);
    }
    return geist (Weight::regular, 13.0f);
}

/// The same family by a weight's name ("Light", "Regular", "Medium", "Semibold", "Bold"), as the effect
/// pages ask for it. Bold maps to 600, the heaviest weight bundled.
inline juce::FontOptions font (const char* weight, float size)
{
    const juce::String w (weight);
    const auto x = w.startsWithIgnoreCase ("light")  ? Weight::light
                 : w.startsWithIgnoreCase ("medium") ? Weight::medium
                 : w.startsWithIgnoreCase ("semi") || w.startsWithIgnoreCase ("bold") ? Weight::semibold
                                                                                      : Weight::regular;
    return geist (x, size);
}

/// The width of a line of text in a font.
inline float textWidth (const juce::FontOptions& f, const juce::String& s)
{
    return juce::GlyphArrangement::getStringWidth (juce::Font (f), s);
}

// ---- Shape and spacing (handoff 3, "Shape") ------------------------------------------------------

constexpr float radiusWindow = 12.0f;
constexpr float radiusHead = 14.0f;
constexpr float radiusCard = 10.0f; // chain blocks and cards
constexpr float radiusPanel = 6.0f; // amp panel and grille, and the effect pages' cards
constexpr float radiusControl = 6.0f; // small buttons, fields
constexpr float radiusMini = 3.0f;

/// The 4-point grid.
namespace space
{
constexpr int xs = 4, s = 8, m = 12, l = 16, xl = 24, xxl = 32;
}

constexpr int panelPadding = 0;
constexpr int controlGap = 8;
constexpr int groupGap = 16;

constexpr int controlHeight = 28; // buttons, combo boxes, fields
constexpr int switchHeight = 24;  // a toggle (16 high) with its 4 px focus margin all round
constexpr int captionHeight = 16; // a knob's label

// ---- Layout (the redesign's FIRST VIEWPORT) -----------------------------------------------------

/// The fixed logical canvas. The window scales it uniformly and letterboxes it (UH11).
constexpr int canvasWidth = 1280, canvasHeight = 760;

/// The main screen: the top bar across; on the left the stage over the board; on the right the inspector; the
/// footer (the scenes and the tuner's reading) across the bottom. Overlays cover everything under the top bar.
constexpr int topBarHeight = 52;
constexpr int footerHeight = 78;
constexpr int inspectorWidth = 300;
constexpr int stageHeight = 350;
constexpr int boardHeight = canvasHeight - topBarHeight - footerHeight - stageHeight; // 280: two rows of pedals at about two thirds of their size
constexpr int stageWidth = canvasWidth - inspectorWidth;                              // 980

/// The smallest window (the canvas at half size).
constexpr int minimumWidth = canvasWidth / 2, minimumHeight = canvasHeight / 2;

// ---- Motion --------------------------------------------------------------------------------------

constexpr int meterFps = 30;               // meters, the analyzer, and the tuner while they move
constexpr int animationFps = 60;           // the stage's crossfade, the scene glide, the LEDs' glow
constexpr double stageFadeSeconds = 0.18;  // picking Amp, Cab, or EQ crossfades the stage
constexpr double sceneGlideSeconds = 0.16; // the lit scene glides to its new place
constexpr double ledGlowSeconds = 0.35;    // an LED lights with a short glow
constexpr double peakHoldSeconds = 1.5;
constexpr double meterFallDbPerSecond = 24.0;
constexpr float meterFloorDb = -60.0f;

} // namespace ui::theme

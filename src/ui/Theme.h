#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

/// The design tokens from the UI handoff (docs/ui/amp-ui-handoff/ui_reference.html, its :root block and
/// the knob skins): every colour, type style, size, and radius the GUI uses, by name. Components never
/// hold hex values of their own; changing the look means changing this file.
///
/// The chrome is dark with hairlines and one accent, emerald, which only ever means "active, on, or
/// correct" (handoff section 3). There are no warning colours: a warning is written in words, in the
/// neutral inks (ASSUMPTIONS H5).
namespace ui::theme
{

// ---- Colour: the handoff's tokens ----------------------------------------------------------------

inline const juce::Colour bg { 0xff0b0c0c };        // window background
inline const juce::Colour surface { 0xff111313 };   // knob bodies in chrome, hover fills
inline const juce::Colour line1 { 0xff1e2221 };     // "line": 1px dividers and borders
inline const juce::Colour line2 { 0xff2a2f2d };     // "line-2": stronger borders, inactive knob tracks, off states
inline const juce::Colour ink { 0xffe6ebe9 };       // primary text
inline const juce::Colour inkDim { 0xff8a938f };    // secondary text
inline const juce::Colour inkFaint { 0xff525a57 };  // tertiary text, axis labels
inline const juce::Colour accent { 0xff34d399 };    // emerald: active, on, in tune, value arcs
inline const juce::Colour accentDim { 0x2434d399 }; // rgba(52, 211, 153, 0.14): soft emerald fills
inline const juce::Colour letterbox { 0xff050606 }; // around the 1280 x 760 canvas when the window's aspect differs

// ---- Colour: the older names the effect pages were written with, mapped onto the tokens ----------
// (The undesigned pages keep their layouts; ASSUMPTIONS H1. These keep them in the new palette.)

inline const juce::Colour background = bg;
inline const juce::Colour surfaceRaised = surface;
inline const juce::Colour outline = line2;
inline const juce::Colour text = ink;
inline const juce::Colour textDim = inkDim;
inline const juce::Colour accentSoft = accentDim;
inline const juce::Colour onAccent = bg;
inline const juce::Colour good = accent;   // in range, signal present
inline const juce::Colour warn = ink;      // was amber: now the text itself carries the warning (H5)
inline const juce::Colour error = ink;     // was red: likewise
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
    chrome,  // the default, in the UI
    glass,   // amp 1's panel
    ember,   // amp 2's panel
    monolith // amp 3's panel
};

inline KnobSkin knobSkin (Skin skin)
{
    switch (skin)
    {
        case Skin::glass:    return { juce::Colour (0xfffafafa), juce::Colour (0xff9ea2a0), juce::Colour (0xff6d716f), juce::Colour (0xff1d1f1e),
                                      juce::Colour (0xff2b2e2c), juce::Colour (0x2e000000) };
        case Skin::ember:    return { juce::Colour (0xff3a3633), juce::Colour (0xff0f0e0d), juce::Colour (0xff000000), juce::Colour (0xffefdfba),
                                      juce::Colour (0xffd8c49a), juce::Colour (0x29efdfba) };
        case Skin::monolith: return { juce::Colour (0xff3c3f3e), juce::Colour (0xff141515), juce::Colour (0xff050505), accent,
                                      juce::Colour (0xff9aa4a0), juce::Colour (0x17ffffff) };
        case Skin::chrome:   break;
    }
    return { juce::Colour (0xff1a1d1c), surface, line2, ink, inkDim, line2 };
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

// ---- Layout (handoff 2) --------------------------------------------------------------------------

/// The fixed logical canvas. The window scales it uniformly and letterboxes it (H11).
constexpr int canvasWidth = 1280, canvasHeight = 760;
constexpr int topBarHeight = 56;
constexpr int chainHeight = 72;
constexpr int mainPadTop = 18, mainPadSide = 40, mainPadBottom = 16;

/// The smallest window (the canvas at half size).
constexpr int minimumWidth = canvasWidth / 2, minimumHeight = canvasHeight / 2;

// ---- Motion --------------------------------------------------------------------------------------

constexpr int meterFps = 30;            // meters, the analyzer, and the tuner while they move
constexpr double peakHoldSeconds = 1.5;
constexpr double meterFallDbPerSecond = 24.0;
constexpr float meterFloorDb = -60.0f;

} // namespace ui::theme

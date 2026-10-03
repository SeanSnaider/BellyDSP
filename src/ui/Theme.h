#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

/// The design tokens from docs/UI_DESIGN.md: every colour, type style, size, and spacing the GUI uses,
/// by name. Components never hold hex values or magic sizes of their own; changing the look means
/// changing this file (ASSUMPTIONS U1).
namespace ui::theme
{

// ---- Colour --------------------------------------------------------------------------------------

inline const juce::Colour background { 0xff121417 };    // the window
inline const juce::Colour surface { 0xff1b1e23 };       // panels, the chain strip's sections, the bars
inline const juce::Colour surfaceRaised { 0xff242830 }; // cards, knob faces, fields, hovered blocks
inline const juce::Colour outline { 0xff2f343d };       // borders, separators, knob tracks
inline const juce::Colour text { 0xffe8eaed };          // primary text and values
inline const juce::Colour textDim { 0xff9aa1ac };       // labels, units, captions
inline const juce::Colour accent { 0xff3fa7d6 };        // on, selected, knob arcs, focus
inline const juce::Colour accentSoft { 0x593fa7d6 };    // accent at 35%: fills behind the accent
inline const juce::Colour onAccent { 0xff121417 };      // text on an accent fill (7:1; white would be 2.9:1)
inline const juce::Colour good { 0xff4cd98a };          // in tune, signal present, meters below -12 dBFS
inline const juce::Colour warn { 0xffffc04d };          // meters -12 to -3 dBFS, warnings
inline const juce::Colour error { 0xffff6b5e };         // clipping, errors, way out of tune

// The thin bar on each chain block: which section it lives in.
inline const juce::Colour sectionPre { 0xff7a8cff };
inline const juce::Colour sectionAmp { 0xffff9f43 };
inline const juce::Colour sectionCab { 0xffb57cff };
inline const juce::Colour sectionPost { 0xff3fd0c9 };
inline const juce::Colour sectionInOut { 0xff9aa1ac };

// The two close mics on the cab page, so a mic's card and its marker on the speaker match.
inline const juce::Colour micOne { 0xff3fa7d6 };
inline const juce::Colour micTwo { 0xffff9f43 };
inline const juce::Colour micRoom { 0xffb57cff };

/// An off block, a dimmed control (a knob the current mode doesn't use), and an empty scene.
constexpr float offAlpha = 0.45f;
constexpr float unusedAlpha = 0.35f;

// ---- Type ----------------------------------------------------------------------------------------

/// Type styles (UI_DESIGN "Typography"), sizes in points at UI scale 1. The system UI font (San
/// Francisco on macOS) stands in until Sean picks one to embed (U2).
enum class Text
{
    title,   // 18 semibold: panel titles, the preset name
    body,    // 14 regular: buttons, menus, combo boxes
    value,   // 13 medium, tabular figures: knob values, meters' numbers
    label,   // 12 regular: knob captions, column headings
    caption, // 11 regular: the chain blocks' state lines, axis labels (U10)
    huge     // 96 bold: the tuner's note
};

inline juce::Font font (Text style)
{
    const auto make = [] (const char* weight, float points)
    { return juce::FontOptions (juce::Font::getSystemUIFontName(), weight, 10.0f).withPointHeight (points); };

    switch (style)
    {
        case Text::title:   return make ("Semibold", 18.0f);
        case Text::body:    return make ("Regular", 14.0f);
        case Text::value:   return make ("Medium", 13.0f).withFeatureEnabled ("tnum");
        case Text::label:   return make ("Regular", 12.0f);
        case Text::caption: return make ("Regular", 11.0f);
        case Text::huge:    return make ("Bold", 96.0f);
    }
    return make ("Regular", 14.0f);
}

/// The same family at another size and weight (a block's name on the chain strip, the tuner's line).
inline juce::Font font (const char* weight, float points)
{
    return juce::FontOptions (juce::Font::getSystemUIFontName(), weight, 10.0f).withPointHeight (points);
}

// ---- Spacing and shape ---------------------------------------------------------------------------

/// The 4-point grid.
namespace space
{
constexpr int xs = 4, s = 8, m = 12, l = 16, xl = 24, xxl = 32;
}

constexpr float radiusPanel = 6.0f;   // panels and cards
constexpr float radiusControl = 4.0f; // buttons and fields
constexpr int panelPadding = 16;
constexpr int controlGap = 8;
constexpr int groupGap = 16;

constexpr int controlHeight = 28;  // buttons, combo boxes, fields
constexpr int switchHeight = 24;   // a pill switch with its label
constexpr int knobNormal = 56;     // knob diameters
constexpr int knobCompact = 40;
constexpr int captionHeight = 16;  // a knob's caption above it, its value below it

// ---- Layout --------------------------------------------------------------------------------------

constexpr int windowWidth = 1280, windowHeight = 820;   // the window opens at this size (U4)
constexpr int minimumWidth = 1100, minimumHeight = 720; // and resizes down to this (in UI points)
constexpr std::array<float, 4> uiScales { 0.75f, 1.0f, 1.25f, 1.5f };

constexpr int topBarHeight = 56;
constexpr int chainStripHeight = 156; // two rows of blocks (U9)
constexpr int statusHeight = 30;      // the warning line above the scenes
constexpr int scenesBarHeight = 48;

constexpr int chainBlockMaxWidth = 140; // chain blocks share the row's width up to this (U9)
constexpr int chainBlockHeight = 48;
constexpr int chainCapWidth = 44;       // the IN and OUT ends of the chain
constexpr int chainGap = 6;             // between blocks in a section

// ---- Motion --------------------------------------------------------------------------------------

constexpr int meterFps = 30;            // meters and the analyzer while they move
constexpr double peakHoldSeconds = 1.5;
constexpr double meterFallDbPerSecond = 24.0;
constexpr float meterFloorDb = -60.0f;

} // namespace ui::theme

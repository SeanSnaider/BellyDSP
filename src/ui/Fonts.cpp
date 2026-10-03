// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Theme.h"
#include "FontData.h"

namespace ui::theme
{

namespace
{
/// The bundled typefaces, made from the embedded files the first time a font is asked for (on the
/// message thread) and kept for the life of the app. A file that doesn't load leaves a null entry, and
/// that weight falls back to the system UI font.
struct Typefaces
{
    std::array<juce::Typeface::Ptr, 4> geist;
    juce::Typeface::Ptr fraunces;

    Typefaces()
    {
        const auto make = [] (const char* data, int size) -> juce::Typeface::Ptr
        {
            if (data == nullptr || size <= 0)
                return nullptr;
            return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
        };
        geist[0] = make (FontData::GeistLight_ttf, FontData::GeistLight_ttfSize);
        geist[1] = make (FontData::GeistRegular_ttf, FontData::GeistRegular_ttfSize);
        geist[2] = make (FontData::GeistMedium_ttf, FontData::GeistMedium_ttfSize);
        geist[3] = make (FontData::GeistSemiBold_ttf, FontData::GeistSemiBold_ttfSize);
        fraunces = make (FontData::FrauncesSemiBoldItalic_ttf, FontData::FrauncesSemiBoldItalic_ttfSize);
    }
};

const Typefaces& typefaces()
{
    static const Typefaces t;
    return t;
}

const char* systemStyle (Weight weight)
{
    switch (weight)
    {
        case Weight::light:    return "Light";
        case Weight::medium:   return "Medium";
        case Weight::semibold: return "Semibold";
        case Weight::regular:  break;
    }
    return "Regular";
}
} // namespace

juce::FontOptions geist (Weight weight, float size)
{
    if (const auto& face = typefaces().geist[(size_t) weight]; face != nullptr)
        return juce::FontOptions (face).withPointHeight (size);
    return juce::FontOptions (juce::Font::getSystemUIFontName(), systemStyle (weight), 10.0f).withPointHeight (size);
}

juce::FontOptions fraunces (float size)
{
    if (const auto& face = typefaces().fraunces; face != nullptr)
        return juce::FontOptions (face).withPointHeight (size);
    return juce::FontOptions ("Georgia", "Bold Italic", 10.0f).withPointHeight (size);
}

bool bundledFontsLoaded()
{
    const auto& t = typefaces();
    return t.fraunces != nullptr && std::all_of (t.geist.begin(), t.geist.end(), [] (const auto& f) { return f != nullptr; });
}

} // namespace ui::theme

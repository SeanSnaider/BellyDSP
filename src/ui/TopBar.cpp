// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TopBar.h"
#include "platform/AppInfo.h"

namespace ui
{

using namespace theme;

/// The brand: a 7 px emerald dot, 8 px, then the product's name, "BellyDSP" (15 px semibold), where the
/// handoff draws its placeholder "rig". It's also the handle of a small menu (the version, the licence,
/// "Check for updates...", the source, the notices), so it looks the same at rest and only the cursor
/// and the tooltip say it can be clicked (ASSUMPTIONS DS9).
class TopBar::BrandButton final : public juce::Button
{
public:
    BrandButton() : juce::Button (platform::productName)
    {
        setTooltip (juce::String (platform::productName) + " " + platform::appVersion() + ": updates, source, and licences");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    static int preferredWidth() { return 15 + (int) std::ceil (textWidth (font(), platform::productName)) + 2; }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        const auto h = (float) getHeight();
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ 3.5f, h * 0.5f }));
        g.setFont (font());
        g.setColour (ink);
        g.drawText (platform::productName, juce::Rectangle<float> (15.0f, 0.0f, (float) getWidth() - 15.0f, h), juce::Justification::centredLeft, false);
    }

private:
    static juce::FontOptions font() { return geist (Weight::semibold, 15.0f); }
};

/// The preset's name and tag, centred in a 300 x 32 box: the browser's handle.
class TopBar::PresetButton final : public juce::Button
{
public:
    PresetButton() : juce::Button ("Preset")
    {
        setTooltip ("Presets: the factory five, your preset folder, open, and save as");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void set (const juce::String& newName, const juce::String& newTag)
    {
        if (newName != name || newTag != tag)
        {
            name = newName;
            tag = newTag;
            setTitle (name);
            repaint();
        }
    }

    juce::String name, tag;

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        // CSS: flex, centred, gap 10; the name 14 px medium, the tag 12 px regular ink-faint.
        const auto nameFont = geist (Weight::medium, 14.0f), tagFont = geist (Weight::regular, 12.0f);
        const auto nameWidth = juce::jmin (textWidth (nameFont, name), (float) getWidth() - 60.0f);
        const auto tagWidth = tag.isEmpty() ? 0.0f : textWidth (tagFont, tag);
        const auto total = nameWidth + (tag.isEmpty() ? 0.0f : 10.0f + tagWidth);
        auto x = ((float) getWidth() - total) * 0.5f;
        const auto h = (float) getHeight();

        g.setFont (nameFont);
        g.setColour (highlighted ? ink : ink.withMultipliedAlpha (0.96f));
        g.drawFittedText (name, juce::Rectangle<float> (x, 0.0f, nameWidth + 1.0f, h).toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
        if (tag.isNotEmpty())
        {
            x += nameWidth + 10.0f;
            g.setFont (tagFont);
            g.setColour (inkFaint);
            g.drawText (tag, juce::Rectangle<float> (x, 1.0f, tagWidth + 2.0f, h), juce::Justification::centredLeft, false);
        }
    }
};

/// "Tuner" and the note it hears; underlined in emerald along the bar's bottom edge while its page shows.
class TopBar::TunerButton final : public juce::Button
{
public:
    TunerButton() : juce::Button ("Tuner")
    {
        setTooltip ("The tuner (mutes the output while it's open, unless you switch that off)");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void set (bool open, const juce::String& newNote)
    {
        if (open != isOpen || newNote != note)
        {
            isOpen = open;
            note = newNote;
            repaint();
        }
    }

    static int preferredWidth (const juce::String& note)
    {
        return (int) std::ceil (textWidth (geist (Weight::regular, 13.0f), "Tuner") + 8.0f + juce::jmax (12.0f, textWidth (noteFont(), note)));
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto labelFont = geist (Weight::regular, 13.0f);
        const auto w = textWidth (labelFont, "Tuner");
        const auto h = (float) getHeight();
        g.setFont (labelFont);
        g.setColour (isOpen || highlighted ? ink : inkDim);
        g.drawText ("Tuner", juce::Rectangle<float> (0.0f, 0.0f, w + 1.0f, h), juce::Justification::centredLeft, false);

        // The note: emerald only while the tuner hears one (the CSS .note rule); a plain dash otherwise.
        g.setFont (noteFont());
        const bool hearing = note != "-";
        g.setColour (hearing ? accent : inkFaint);
        g.drawText (note, juce::Rectangle<float> (w + 8.0f, 0.0f, (float) getWidth() - w - 8.0f, h), juce::Justification::centredLeft, false);

        if (isOpen)
        {
            g.setColour (accent);
            g.fillRect (juce::Rectangle<float> (0.0f, h - 1.0f, (float) getWidth(), 1.0f));
        }
    }

private:
    static juce::FontOptions noteFont() { return geist (Weight::semibold, 13.0f); }
    bool isOpen = false;
    juce::String note { "-" };
};

TopBar::TopBar (AmpSimProcessor& processor)
    : ampSim (processor), brand (std::make_unique<BrandButton>()), preset (std::make_unique<PresetButton>()), tuner (std::make_unique<TunerButton>())
{
    brand->onClick = [this] { if (onBrand) onBrand(); };
    preset->onClick = [this] { if (onPresetMenu) onPresetMenu(); };
    previous.onClick = [this] { if (onPrevious) onPrevious(); };
    next.onClick = [this] { if (onNext) onNext(); };
    save.onClick = [this] { if (onSave) onSave(); };
    tuner->onClick = [this] { if (onTuner) onTuner(); };
    tagged (*tuner, "tuner_on"); // a right-click learns a footswitch for the tuner

    previous.setTooltip ("Previous preset");
    next.setTooltip ("Next preset");
    save.setTooltip ("Save the preset to a file");
    for (auto* c : std::initializer_list<juce::Component*> { brand.get(), preset.get(), &previous, &next, &save, tuner.get(), &inputMeter, &outputMeter })
        addAndMakeVisible (c);
    for (auto* b : std::initializer_list<juce::Component*> { brand.get(), preset.get(), &previous, &next, &save, tuner.get() })
        b->setHasFocusOutline (true);
    refresh();
}

TopBar::~TopBar() = default;

juce::Button& TopBar::getBrandButton() noexcept
{
    return *brand;
}

juce::Button& TopBar::getPresetButton() noexcept
{
    return *preset;
}

juce::Button& TopBar::getTunerButton() noexcept
{
    return *tuner;
}

juce::String TopBar::getShownPresetName() const
{
    return preset->name;
}

juce::String TopBar::getShownTag() const
{
    return preset->tag;
}

void TopBar::refresh()
{
    const auto name = ampSim.getPresetName();
    const auto source = ampSim.parameters.state.getProperty ("presetSource").toString();
    const auto tag = name.isEmpty() ? juce::String() : (source == "factory" ? juce::String ("Factory") : juce::String ("User"));
    preset->set (name.isEmpty() ? juce::String ("Untitled") : name, tag);
}

void TopBar::setTuner (bool pageOpen, const juce::String& note)
{
    if (note != tunerNote)
    {
        tunerNote = note;
        resized();
    }
    tuner->set (pageOpen, note);
}

void TopBar::updateMeters (const AmpSimProcessor::Peaks& peaks, double seconds)
{
    inputMeter.push (&peaks.input, seconds);
    const float out[] { peaks.left, peaks.right };
    outputMeter.push (out, seconds);
}

void TopBar::paint (juce::Graphics& g)
{
    // The brand is its own button (BrandButton), in the 136 px column at the left.
    g.setColour (line1);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void TopBar::resized()
{
    // CSS: padding 0 24, gap 20; the brand 136 wide; the preset part fills; the right part its content.
    auto area = getLocalBounds().reduced (24, 0);
    brand->setBounds (area.removeFromLeft (136).withWidth (BrandButton::preferredWidth()));
    area.removeFromLeft (20);

    // Right: the tuner button (56 high), 22, In, 22, Out.
    const auto outWidth = outputMeter.getPreferredWidth(), inWidth = inputMeter.getPreferredWidth();
    outputMeter.setBounds (area.removeFromRight (outWidth).withSizeKeepingCentre (outWidth, 14));
    area.removeFromRight (22);
    inputMeter.setBounds (area.removeFromRight (inWidth).withSizeKeepingCentre (inWidth, 14));
    area.removeFromRight (22);
    const auto tunerWidth = TunerButton::preferredWidth (tunerNote);
    tuner->setBounds (area.removeFromRight (tunerWidth).withHeight (getHeight()));
    area.removeFromRight (20);

    // Centre: arrow 28, 4, name 300, 4, arrow 28, 8, Save (12 + text + 12), centred in what's left.
    const auto saveWidth = 24 + (int) std::ceil (textWidth (geist (Weight::regular, 13.0f), "Save")) + 2;
    const auto groupWidth = 28 + 4 + 300 + 4 + 28 + 8 + saveWidth;
    auto group = area.withSizeKeepingCentre (groupWidth, 32);
    previous.setBounds (group.removeFromLeft (28).withSizeKeepingCentre (28, 28));
    group.removeFromLeft (4);
    preset->setBounds (group.removeFromLeft (300));
    group.removeFromLeft (4);
    next.setBounds (group.removeFromLeft (28).withSizeKeepingCentre (28, 28));
    group.removeFromLeft (8);
    save.setBounds (group.removeFromLeft (saveWidth).withSizeKeepingCentre (saveWidth, 28));
}

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "TopBar.h"
#include "platform/AppInfo.h"

namespace ui
{

using namespace theme;

/// The brand: a 7 px orange dot, 8 px, then the product's name, "BellyDSP" (15 px semibold). It's also the handle of
/// a small menu (the version, the licence, "Check for updates...", the source, the notices), so it looks the same at
/// rest and only the cursor and the tooltip say it can be clicked (ASSUMPTIONS DS9).
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

/// The preset's name (14 px medium) over its tag and the current scene (11 px, faint), centred in a box: the
/// browser's handle.
class TopBar::PresetButton final : public juce::Button
{
public:
    PresetButton() : juce::Button ("Preset")
    {
        setTooltip ("Presets: the factory five, your preset folder, open, and save as (the left and right arrows step through them)");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void set (const juce::String& newName, const juce::String& newTag, const juce::String& newScene)
    {
        if (newName != name || newTag != tag || newScene != scene)
        {
            name = newName;
            tag = newTag;
            scene = newScene;
            setTitle (name);
            repaint();
        }
    }

    juce::String name, tag, scene;

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto box = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (highlighted ? raised.brighter (0.05f) : raised);
        g.fillRoundedRectangle (box, radiusControl);
        g.setColour (line1);
        g.drawRoundedRectangle (box, radiusControl, 1.0f);

        auto area = getLocalBounds().reduced (12, 3);
        g.setFont (geist (Weight::semibold, 14.0f));
        g.setColour (ink);
        g.drawFittedText (name, area.removeFromTop (area.getHeight() / 2 + 2), juce::Justification::centredBottom, 1, 0.85f);

        // The tag, then the scene in ink-dim (the scene matters more while playing).
        const auto small = geist (Weight::regular, 11.0f);
        const auto tagText = tag.isEmpty() ? juce::String() : tag + "   ";
        const auto tw = textWidth (small, tagText), sw = textWidth (geist (Weight::medium, 11.0f), scene);
        auto x = (float) area.getCentreX() - (tw + sw) * 0.5f;
        g.setFont (small);
        g.setColour (inkFaint);
        g.drawText (tagText, juce::Rectangle<float> (x, (float) area.getY(), tw + 1.0f, (float) area.getHeight()), juce::Justification::centredTop, false);
        x += tw;
        g.setFont (geist (Weight::medium, 11.0f));
        g.setColour (inkDim);
        g.drawText (scene, juce::Rectangle<float> (x, (float) area.getY(), sw + 2.0f, (float) area.getHeight()), juce::Justification::centredTop, false);
    }
};

/// A text button on the bar (Tuner, Match tone, Settings): 13 px ink-dim (ink when hovered or open), an optional note
/// after it in orange (the tuner's), and an orange underline along the bar's bottom while what it opens is open.
class TopBar::BarButton final : public juce::Button
{
public:
    BarButton (const juce::String& label, const juce::String& tip) : juce::Button (label)
    {
        setTooltip (tip);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setHasFocusOutline (true);
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

    bool isShownOpen() const noexcept { return isOpen; }

    int preferredWidth() const
    {
        const auto w = textWidth (labelFont(), getName()) + (note.isEmpty() ? 0.0f : 8.0f + juce::jmax (12.0f, textWidth (noteFont(), note)));
        return (int) std::ceil (w) + 2;
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto w = textWidth (labelFont(), getName());
        const auto h = (float) getHeight();
        g.setFont (labelFont());
        g.setColour (isOpen || highlighted ? ink : inkDim);
        g.drawText (getName(), juce::Rectangle<float> (0.0f, 0.0f, w + 1.0f, h), juce::Justification::centredLeft, false);
        if (note.isNotEmpty())
        {
            // The note: orange only while the tuner hears one; a plain dash otherwise.
            g.setFont (noteFont());
            g.setColour (note != "-" ? accent : inkFaint);
            g.drawText (note, juce::Rectangle<float> (w + 8.0f, 0.0f, (float) getWidth() - w - 8.0f, h), juce::Justification::centredLeft, false);
        }
        if (isOpen)
        {
            g.setColour (accent);
            g.fillRect (juce::Rectangle<float> (0.0f, h - 2.0f, (float) getWidth(), 2.0f));
        }
    }

private:
    static juce::FontOptions labelFont() { return geist (Weight::regular, 13.0f); }
    static juce::FontOptions noteFont() { return geist (Weight::semibold, 13.0f); }
    bool isOpen = false;
    juce::String note;
};

TopBar::TopBar (AmpSimProcessor& processor)
    : ampSim (processor),
      brand (std::make_unique<BrandButton>()),
      preset (std::make_unique<PresetButton>()),
      tuner (std::make_unique<BarButton> ("Tuner", "The tuner (T): mutes the output while it's open, unless you switch that off")),
      matchTone (std::make_unique<BarButton> ("Match tone", "Match tone: set the rig to the guitar tone of a song")),
      settings (std::make_unique<BarButton> ("Settings", "Settings: the input and output, the limiter, the CPU saver, A/B, the tempo, and MIDI"))
{
    brand->onClick = [this] { if (onBrand) onBrand(); };
    preset->onClick = [this] { if (onPresetMenu) onPresetMenu(); };
    previous.onClick = [this] { if (onPrevious) onPrevious(); };
    next.onClick = [this] { if (onNext) onNext(); };
    save.onClick = [this] { if (onSave) onSave(); };
    tuner->onClick = [this] { if (onTuner) onTuner(); };
    matchTone->onClick = [this] { if (onMatchTone) onMatchTone(); };
    settings->onClick = [this] { if (onSettings) onSettings(); };
    undo.onClick = [this] { if (onUndo) onUndo(); };
    redo.onClick = [this] { if (onRedo) onRedo(); };
    abA.onClick = [this] { if (onAbSelect) onAbSelect (false); };
    abB.onClick = [this] { if (onAbSelect) onAbSelect (true); };
    tagged (*tuner, "tuner_on"); // a right-click learns a footswitch for the tuner
    tuner->set (false, {});

    abA.setConnectedEdges (juce::Button::ConnectedOnRight);
    abB.setConnectedEdges (juce::Button::ConnectedOnLeft);
    abA.setTooltip ("Settings A: compare two versions of the settings (captures and IRs stay; Copy is in Settings, Output)");
    abB.setTooltip ("Settings B: compare two versions of the settings (captures and IRs stay; Copy is in Settings, Output)");
    previous.setTooltip ("Previous preset (Left arrow)");
    next.setTooltip ("Next preset (Right arrow)");
    undo.setTooltip ("Undo (Cmd-Z)");
    redo.setTooltip ("Redo (Shift-Cmd-Z)");
    save.setTooltip ("Save the preset to a file");
    for (auto* c : std::initializer_list<juce::Component*> { brand.get(), &abA, &abB, &undo, &redo, &previous, preset.get(), &next, &save, &cpu, &inputMeter,
                                                             &outputMeter, tuner.get(), matchTone.get(), settings.get() })
        addAndMakeVisible (c);
    for (auto* b : std::initializer_list<juce::Component*> { brand.get(), preset.get(), &previous, &next, &save, &abA, &abB, &undo, &redo })
        b->setHasFocusOutline (true);
    refresh();
}

TopBar::~TopBar() = default;

juce::Button& TopBar::getBrandButton() noexcept { return *brand; }
juce::Button& TopBar::getPresetButton() noexcept { return *preset; }
juce::Button& TopBar::getTunerButton() noexcept { return *tuner; }
juce::Button& TopBar::getMatchToneButton() noexcept { return *matchTone; }
juce::Button& TopBar::getSettingsButton() noexcept { return *settings; }
juce::String TopBar::getShownPresetName() const { return preset->name; }
juce::String TopBar::getShownTag() const { return preset->tag; }
juce::String TopBar::getShownScene() const { return preset->scene; }

void TopBar::refresh()
{
    const auto name = ampSim.getPresetName();
    const auto source = ampSim.parameters.state.getProperty ("presetSource").toString();
    const auto tag = name.isEmpty() ? juce::String() : (source == "factory" ? juce::String ("Factory") : juce::String ("User"));
    const auto& scenes = ampSim.getScenes();
    const auto current = scenes.getCurrent();
    juce::String scene ("No scene");
    if (current >= 0 && scenes.get (current).stored)
        scene = "Scene " + juce::String (current + 1) + ": " + scenes.get (current).name;
    preset->set (name.isEmpty() ? juce::String ("Untitled") : name, tag, scene);

    const auto onB = ampSim.isOnB();
    abA.setToggleState (! onB, juce::dontSendNotification);
    abB.setToggleState (onB, juce::dontSendNotification);
    undo.setEnabled (ampSim.undoManager.canUndo());
    redo.setEnabled (ampSim.undoManager.canRedo());
}

void TopBar::setTuner (bool open, const juce::String& note)
{
    if (note != tunerNote)
    {
        tunerNote = note;
        tuner->set (open, note == "-" ? juce::String() : note);
        resized();
    }
    tuner->set (open, note == "-" ? juce::String() : note); // the note only while it hears one
}

void TopBar::setOverlay (Overlay overlay)
{
    matchTone->set (overlay == Overlay::toneMatch, {});
    settings->set (overlay == Overlay::settings, {});
    tuner->set (overlay == Overlay::tuner, tunerNote == "-" ? juce::String() : tunerNote);
}

void TopBar::updateMeters (const AmpSimProcessor::Peaks& peaks, double seconds)
{
    inputMeter.push (&peaks.input, seconds);
    const float out[] { peaks.left, peaks.right };
    outputMeter.push (out, seconds);
    outputMeter.pushLimiter (peaks.limiterDb, seconds);
}

void TopBar::paint (juce::Graphics& g)
{
    g.fillAll (panelBg);
    g.setColour (line1);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    // Hairlines between the bar's groups.
    for (auto x : { abA.getX() - 12, cpu.getX() - 16, tuner->getX() - 16 })
        g.fillRect (x, 14, 1, getHeight() - 28);
}

void TopBar::resized()
{
    auto area = getLocalBounds().reduced (18, 0);
    const auto h = getHeight();

    // Left: the brand, A | B, undo and redo.
    brand->setBounds (area.removeFromLeft (BrandButton::preferredWidth()).withHeight (h));
    area.removeFromLeft (24);
    abA.setBounds (area.removeFromLeft (30).withSizeKeepingCentre (30, 26));
    abB.setBounds (area.removeFromLeft (30).withSizeKeepingCentre (30, 26));
    area.removeFromLeft (10);
    undo.setBounds (area.removeFromLeft (28).withSizeKeepingCentre (28, 26));
    area.removeFromLeft (4);
    redo.setBounds (area.removeFromLeft (28).withSizeKeepingCentre (28, 26));
    area.removeFromLeft (16);

    // Right, from the edge in: Settings, Match tone, the tuner; then the meters and the CPU.
    const auto barButton = [&] (BarButton& b, int gapBefore)
    {
        const auto w = b.preferredWidth();
        b.setBounds (area.removeFromRight (w).withHeight (h));
        area.removeFromRight (gapBefore);
    };
    barButton (*settings, 18);
    barButton (*matchTone, 18);
    barButton (*tuner, 32);
    const auto outWidth = outputMeter.getPreferredWidth(), inWidth = inputMeter.getPreferredWidth();
    outputMeter.setBounds (area.removeFromRight (outWidth).withSizeKeepingCentre (outWidth, 30)); // room under the bar for "limit"
    area.removeFromRight (18);
    inputMeter.setBounds (area.removeFromRight (inWidth).withSizeKeepingCentre (inWidth, 14));
    area.removeFromRight (18);
    cpu.setBounds (area.removeFromRight (118).withSizeKeepingCentre (118, 16));
    area.removeFromRight (32);

    // Centre: arrow, the preset's box, arrow, Save, centred in what's left.
    const auto saveWidth = 24 + (int) std::ceil (textWidth (geist (Weight::regular, 13.0f), "Save")) + 2;
    const auto boxWidth = juce::jlimit (180, 300, area.getWidth() - (28 + 4 + 4 + 28 + 8 + saveWidth));
    const auto groupWidth = 28 + 4 + boxWidth + 4 + 28 + 8 + saveWidth;
    auto group = area.withSizeKeepingCentre (juce::jmin (groupWidth, area.getWidth()), 38);
    previous.setBounds (group.removeFromLeft (28).withSizeKeepingCentre (28, 28));
    group.removeFromLeft (4);
    preset->setBounds (group.removeFromLeft (boxWidth));
    group.removeFromLeft (4);
    next.setBounds (group.removeFromLeft (28).withSizeKeepingCentre (28, 28));
    group.removeFromLeft (8);
    save.setBounds (group.removeFromLeft (saveWidth).withSizeKeepingCentre (saveWidth, 28));
}

} // namespace ui

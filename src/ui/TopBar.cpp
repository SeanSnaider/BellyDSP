#include "TopBar.h"

namespace ui
{

using namespace theme;

/// The preset's name in the title style with a chevron: the browser's handle.
class TopBar::PresetButton final : public juce::Button
{
public:
    PresetButton() : juce::Button ("Preset")
    {
        setTooltip ("Presets: the factory five, your preset folder, open, and save as");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setText (const juce::String& name, bool loading)
    {
        if (name != shown || loading != busy)
        {
            shown = name;
            busy = loading;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? surfaceRaised.darker (0.1f) : (highlighted ? surfaceRaised.brighter (0.05f) : surfaceRaised));
        g.fillRoundedRectangle (bounds, radiusControl);
        g.setColour (highlighted ? outline.brighter (0.25f) : outline);
        g.drawRoundedRectangle (bounds, radiusControl, 1.0f);

        auto area = getLocalBounds().reduced (12, 0);
        const auto chevronArea = area.removeFromRight (14).toFloat();
        g.setFont (font (Text::title));
        g.setColour (shown.isEmpty() ? textDim : theme::text);
        g.drawFittedText (shown.isEmpty() ? juce::String ("Untitled") : shown, area.withTrimmedRight (busy ? 64 : 0), juce::Justification::centredLeft, 1, 0.8f);
        if (busy)
        {
            g.setFont (font (Text::caption));
            g.setColour (textDim);
            g.drawText ("loading...", area, juce::Justification::centredRight, false);
        }

        const auto c = chevronArea.getCentre();
        juce::Path chevron;
        chevron.startNewSubPath (c.x - 4.0f, c.y - 2.0f);
        chevron.lineTo (c.x, c.y + 2.0f);
        chevron.lineTo (c.x + 4.0f, c.y - 2.0f);
        g.setColour (textDim);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    juce::String shown;
    bool busy = false;
};

TopBar::TopBar (AmpSimProcessor& processor)
    : ampSim (processor), preset (std::make_unique<PresetButton>()), tempo (processor.parameters, "tempo_bpm", " BPM")
{
    preset->onClick = [this] { if (onPresetMenu) onPresetMenu(); };
    save.onClick = [this] { if (onSave) onSave(); };
    undo.onClick = [this] { if (onUndo) onUndo(); };
    redo.onClick = [this] { if (onRedo) onRedo(); };
    abA.onClick = [this] { if (onAbSelect) onAbSelect (false); };
    abB.onClick = [this] { if (onAbSelect) onAbSelect (true); };
    abCopy.onClick = [this] { if (onAbCopy) onAbCopy(); };
    midi.onClick = [this] { if (onMidi) onMidi(); };
    settings.onClick = [this] { if (onSettings) onSettings(); };
    tap.onClick = [this] { if (onTap) onTap(); };

    save.setTooltip ("Save the preset to a file");
    abA.setTooltip ("Compare two versions of the settings (captures and IRs stay)");
    abB.setTooltip ("Compare two versions of the settings (captures and IRs stay)");
    abA.setConnectedEdges (juce::Button::ConnectedOnRight);
    abB.setConnectedEdges (juce::Button::ConnectedOnLeft);
    undo.setTooltip ("Undo (cmd-Z)");
    redo.setTooltip ("Redo (shift-cmd-Z)");
    midi.setTooltip ("MIDI mappings. Right-click any control to learn one.");
    settings.setTooltip ("View settings: UI scale");
    tap.setTooltip ("Tap the tempo (or use the footswitch's tap CC)");
    tempo.setTooltip ("Tempo: drag, scroll, or double-click to type");
    tempo.setFormatter ([] (float bpm) { return juce::String (bpm, 1) + " BPM"; });
    tempo.setShowsBar (false);

    tuner.setClickingTogglesState (true);
    tuner.setTooltip ("The tuner (mutes the output while it's showing, unless you switch that off)");
    tunerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (processor.parameters, "tuner_on", tagged (tuner, "tuner_on"));

    for (auto* c : std::initializer_list<juce::Component*> { preset.get(), &save, &abA, &abB, &abCopy, &undo, &redo, &midi, &tuner, &tempo, &tap,
                                                             &inputMeter, &outputMeter, &cpu, &settings })
        addAndMakeVisible (c);

    refresh();
}

TopBar::~TopBar() = default;

juce::Button& TopBar::getPresetButton() noexcept
{
    return *preset;
}

void TopBar::refresh()
{
    preset->setText (ampSim.getPresetName(), ampSim.isChangingPreset());
    undo.setEnabled (ampSim.undoManager.canUndo());
    redo.setEnabled (ampSim.undoManager.canRedo());
    abA.setToggleState (! ampSim.isOnB(), juce::dontSendNotification);
    abB.setToggleState (ampSim.isOnB(), juce::dontSendNotification);
    abCopy.setTooltip (ampSim.isOnB() ? "Copy B to A" : "Copy A to B");
}

void TopBar::updateMeters (const AmpSimProcessor::Peaks& peaks, float cpuPercent, double seconds)
{
    inputMeter.push (&peaks.input, seconds);
    const float out[] { peaks.left, peaks.right };
    outputMeter.push (out, seconds);
    cpu.setLoad (cpuPercent);
}

void TopBar::paint (juce::Graphics& g)
{
    g.setColour (surface);
    g.fillRect (getLocalBounds());
    g.setColour (outline);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

void TopBar::resized()
{
    auto area = getLocalBounds().reduced (space::l, 0).withTrimmedBottom (1);
    const auto centred = [&area] (juce::Rectangle<int> r, int height) { return r.withSizeKeepingCentre (r.getWidth(), height).withY (area.getCentreY() - height / 2); };
    const auto take = [&area, &centred] (int width, int height = 32)
    {
        auto r = centred (area.removeFromLeft (width), height);
        return r;
    };
    const auto takeRight = [&area, &centred] (int width, int height = 32) { return centred (area.removeFromRight (width), height); };

    // Right: the view settings, the CPU, and the meters.
    settings.setBounds (takeRight (32));
    area.removeFromRight (space::m);
    cpu.setBounds (takeRight (60, 34));
    area.removeFromRight (space::l);
    outputMeter.setBounds (takeRight (56, 40));
    area.removeFromRight (space::m);
    inputMeter.setBounds (takeRight (50, 40));
    area.removeFromRight (space::xl);

    // Left: the preset, then the editing tools, the tuner, and the tempo.
    const auto fixed = 60 + 16 + 64 + 4 + 32 + 16 + 32 + 4 + 32 + 16 + 32 + 8 + 72 + 16 + 100 + 6 + 52;
    preset->setBounds (take (juce::jlimit (170, 340, area.getWidth() - fixed - 8), 36));
    area.removeFromLeft (space::s);
    save.setBounds (take (60));
    area.removeFromLeft (space::l);
    abA.setBounds (take (32));
    abB.setBounds (take (32));
    area.removeFromLeft (space::xs);
    abCopy.setBounds (take (32));
    area.removeFromLeft (space::l);
    undo.setBounds (take (32));
    area.removeFromLeft (space::xs);
    redo.setBounds (take (32));
    area.removeFromLeft (space::l);
    midi.setBounds (take (32));
    area.removeFromLeft (space::s);
    tuner.setBounds (take (72));
    area.removeFromLeft (space::l);
    tempo.setBounds (take (100, 28));
    area.removeFromLeft (6);
    tap.setBounds (take (52));
}

} // namespace ui

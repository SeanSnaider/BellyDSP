#pragma once

#include "PluginProcessor.h"
#include "ui/ChainStrip.h"
#include "ui/LookAndFeel.h"
#include "ui/Pages.h"
#include "ui/ScenesBar.h"
#include "ui/TopBar.h"
#include "ui/TunerView.h"

#include <juce_audio_processors/juce_audio_processors.h>

using ui::IgnoresRightClick;
using ToggleControl = ui::IgnoresRightClick<juce::ToggleButton>;

/// The GUI (BUILD_PLAN "GUI", docs/UI_DESIGN.md). From the top: the top bar (preset, A/B, undo, tuner,
/// tempo, meters, CPU), the chain strip (every block in signal order; click to edit, switch to bypass,
/// drag to reorder), the selected block's page, the warning line, and the scenes along the bottom. The
/// tuner covers it all while it's engaged.
///
/// Everything is laid out in UI points inside one content component, which the UI scale (75 to 150%)
/// scales: the window is the content's size times the scale, so a bigger scale makes a bigger window
/// with the same layout. The scale and the window's size are app state, never presets.
class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// Selects a block (its card lights up and its page shows), as a click on its card does.
    void selectBlock (ui::BlockId id);
    ui::BlockId getSelectedBlock() const noexcept { return selected; }
    ui::BlockPage& getPage (ui::BlockId id) { return *pageFor[(size_t) id]; }

    ui::ChainStrip& getChainStrip() noexcept { return chainStrip; }
    ui::TopBar& getTopBar() noexcept { return topBar; }
    ui::ScenesBar& getScenesBar() noexcept { return scenesBar; }
    TunerView& getTunerView() noexcept { return tunerView; }

    /// The UI scale, one of 0.75, 1, 1.25, 1.5 (others snap to the nearest): the window keeps its size in
    /// UI points and grows or shrinks with the scale. Saved in the app's state.
    void setUiScale (float scale);
    float getUiScale() const noexcept { return uiScale; }

    /// For tests and snapshots: what the status timer does, now (status lines, switches, scenes, tuner).
    void refresh() { refreshState(); }

    /// The warning line as shown (for tests).
    juce::String getStatusText() const;

    /// A right-click on any control attached to a parameter: MIDI learn for it, its mappings, and "Held by
    /// scenes" (BUILD_PLAN "MIDI control": click a control, press a switch, done). The editor listens to
    /// every child's mouse events for this; the controls themselves ignore right-clicks. Every other press
    /// starts an undo step.
    void mouseDown (const juce::MouseEvent& e) override;

    /// For tests: the menu a right-click on this parameter's control would show.
    juce::PopupMenu midiMenuFor (const juce::String& parameterId);

private:
    class StatusLine;

    void timerCallback() override;
    void refreshState();
    void layoutContent();
    void showPage (ui::BlockPage* page);

    void chooseFile (const juce::String& title, const juce::String& patterns, const juce::Identifier& lastPathKey,
                     std::function<void (const juce::File&)> onChosen, bool folders = false);
    void savePreset();
    void loadPreset();
    void loadPresetFile (const juce::File& file);
    void showPresetMenu();
    void showMidiMappings();
    void showSettingsMenu();
    void clickScene (int index);
    void sceneMenu (int index);
    void undo();
    void redo();
    bool keyPressed (const juce::KeyPress& key) override;

    ui::LookAndFeel lookAndFeel; // first, so it outlives every component that draws with it
    AmpSimProcessor& ampSim;
    float uiScale = 1.0f;

    juce::Component content; // everything, laid out in UI points and scaled by the UI scale
    ui::TopBar topBar { ampSim };
    ui::ChainStrip chainStrip { ampSim };
    std::vector<std::unique_ptr<ui::BlockPage>> pages;
    std::array<ui::BlockPage*, ui::numBlocks> pageFor {};
    ui::BlockPage* shownPage = nullptr;
    ui::BlockId selected = ui::BlockId::amp;
    std::unique_ptr<StatusLine> statusLine;
    ui::ScenesBar scenesBar;
    TunerView tunerView { ampSim };
    juce::TooltipWindow tooltips { &content, 600 };

    juce::String presetMessage;
    bool presetMessageIsError = false;
    double lastMeterMs = 0.0;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

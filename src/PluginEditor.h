// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PluginProcessor.h"
#include "ui/AmpView.h"
#include "ui/Board.h"
#include "ui/CabView.h"
#include "ui/LookAndFeel.h"
#include "ui/MainPages.h"
#include "ui/MainScreen.h"
#include "ui/Pages.h"
#include "ui/ScenesBar.h"
#include "ui/TopBar.h"
#include "ui/ToneMatchPage.h"
#include "ui/TunerPage.h"

#include <juce_audio_processors/juce_audio_processors.h>

using ui::IgnoresRightClick;
using ToggleControl = ui::IgnoresRightClick<juce::ToggleButton>;

/// The GUI: one main screen (the app redesign, .impeccable/surfaces/src-plugineditor-cpp.md, its FIRST VIEWPORT) on a
/// fixed 1280 x 760 canvas, scaled uniformly to the window and letterboxed (ASSUMPTIONS UH11):
///   the top bar      across the top (52): the brand, A/B and undo, the preset and its scene, the CPU and meters,
///                    and the buttons that open the tuner, Match tone, and Settings
///   the stage        the left 980 under it (350): the playing amp lit from below, or the cab, or an EQ's graph
///   the board        under the stage (280): every pedal in chain order around the amp's and the cab's markers
///   the inspector    the right 300, from the top bar to the footer: the selected block's every control
///   the footer       across the bottom (78): the eight scenes and the tuner's reading
/// The tuner, Match tone, and Settings open over the stage, the board, and the inspector, and close back to them.
/// Messages (the sample rate, MIDI learn, a preset's problems) show in the amp's info row on the stage, and
/// elsewhere in a 12 px line at the bottom left of the stage or the overlay (UH5).
///
/// Keys (ASSUMPTIONS RD): the left and right arrows step the presets, 1 to 8 recall the scenes, T opens and closes
/// the tuner, Escape closes an overlay, Cmd-Z and Shift-Cmd-Z undo and redo; Match tone's own keys (Space, 1 to 3)
/// while it's open.
class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// Selects a block: the inspector shows its controls and the board raises its pedal. The amp and the cab also
    /// put the stage on them; an EQ pedal puts the stage's EQ view on its graph; the input and the output open
    /// Settings on their page.
    void selectBlock (ui::BlockId id);
    ui::BlockId getSelectedBlock() const noexcept { return selectedBlock; }

    /// What the stage shows, as its segmented control picks it: the inspector follows (the amp, the cab, the EQ
    /// shown).
    void setStageMode (ui::StageMode mode, bool animate = true);
    ui::StageMode getStageMode() const noexcept { return stage->getMode(); }

    /// Opens an overlay over the main screen (the tuner engages the tuner; closing it disengages it), or closes it.
    void openOverlay (ui::Overlay overlay);
    void closeOverlay();
    ui::Overlay getOverlay() const noexcept { return overlay; }

    /// A pedal's controls (every pedal's: the pre and post blocks and Gate B).
    ui::BlockPage& getPage (ui::BlockId id) { return *blockPageFor[(size_t) id]; }
    /// What the inspector shows for a block (a page, or the amp's or the cab's panel; nullptr for the input and output).
    ui::ControlGroup* inspectorContentFor (ui::BlockId id);

    ui::TopBar& getTopBar() noexcept { return topBar; }
    ui::Board& getBoard() noexcept { return *board; }
    ui::Stage& getStage() noexcept { return *stage; }
    ui::Inspector& getInspector() noexcept { return *inspector; }
    ui::OverlayFrame& getOverlayFrame() noexcept { return *overlayFrame; }
    ui::AmpView& getAmpView() noexcept { return *ampView; }
    ui::CabView& getCabView() noexcept { return *cabView; }
    ui::EqStage& getEqStage() noexcept { return *eqStage; }
    ui::SettingsPage& getSettingsPage() noexcept { return *settingsPage; }
    ui::OutputPage& getOutputPage() noexcept { return settingsPage->getOutputPage(); }
    ui::InputPage& getInputPage() noexcept { return settingsPage->getInputPage(); }
    ui::ScenesBar& getScenesBar() noexcept { return scenesBar; }
    ui::TunerReadout& getTunerReadout() noexcept { return tunerReadout; }
    ui::TunerPage& getTunerPage() noexcept { return *tunerPage; }
    ui::ToneMatchPage& getToneMatchPage() noexcept { return *toneMatchPage; }

    /// The canvas's scale and where it sits in the window (letterboxed when the aspect differs).
    float getCanvasScale() const noexcept { return canvasScale; }
    juce::Rectangle<int> getCanvasBounds() const;

    /// For tests and snapshots: what the status timer does, now (status lines, switches, scenes, tuner), and the end
    /// of every animation in progress (the stage's crossfade, the scene light's glide).
    void refresh() { refreshState(); }
    void finishAnimations();

    /// The message line as shown (for tests): the amp's info row or the line at the bottom left.
    juce::String getStatusText() const { return statusText; }

    /// A/B: show B (true) or A, as the top bar's buttons do.
    void abSelect (bool b);

    /// The previous (-1) or next (+1) preset in the list (the factory five, then the preset folder), as
    /// the top bar's arrows and the left and right keys do. Returns the name loaded (empty if there's nothing to step to).
    juce::String stepPreset (int delta);

    /// The keys (above). Public so tests can press them.
    bool keyPressed (const juce::KeyPress& key) override;

    /// A right-click on any control attached to a parameter: MIDI learn for it, its mappings, and "Held by
    /// scenes" (BUILD_PLAN "MIDI control"). The editor listens to every child's mouse events for this; the
    /// controls themselves ignore right-clicks. Every other press starts an undo step.
    void mouseDown (const juce::MouseEvent& e) override;

    /// For tests: the menu a right-click on this parameter's control would show.
    juce::PopupMenu midiMenuFor (const juce::String& parameterId);

    /// For tests: the menu a right-click on scene `index` (0 to 7) would show: store the current
    /// sound there, rename it (a dialog), or clear it.
    juce::PopupMenu sceneMenuFor (int index);

    /// A click on scene `index` in the footer: recall it, or store the current sound in it when it's empty or Store
    /// is armed.
    void clickScene (int index);

    /// The preset browser's menu (a click on the preset's name shows it).
    juce::PopupMenu presetMenu();

    /// The brand's menu (a click on "BellyDSP" shows it): "BellyDSP <version>" and "Free software under the
    /// GNU AGPL v3 or later" (labels), "Match tone..." (the tone match page; docs/TONE_MATCH.md), "Check for
    /// updates..." (enabled when this build has an updater running), "Source code for this version" (opens
    /// <source>/tree/v<version>), and "About / licenses" (ASSUMPTIONS DS9, DS38, TM).
    juce::PopupMenu brandMenu();
    /// A cab mic's load menu (0, 1, or the room): Load an IR file, Built-in IRs (a submenu per bundled
    /// cab, as on the cab's list), Load a cab pack folder (close mics), Clear.
    juce::PopupMenu micMenu (int mic);
    /// The playing amp's capture menu (a right-click at the grille or the model's name): Load a capture of your
    /// own... (played as your capture, amp 9), Reload, and Remove your capture. The built-in amps are picked on the
    /// shelf (ASSUMPTIONS AS10).
    juce::PopupMenu captureMenu (int amp);

    /// What "About / licenses" shows: the name, version, licence, source link, update status, and THIRD_PARTY_NOTICES.txt.
    static juce::String aboutText();

private:
    class Canvas;
    class StatusLine;

    void timerCallback() override;
    void refreshState();
    void layoutCanvas();
    void select (ui::BlockId id, bool fromStage);
    ui::ControlGroup* overlayPage (ui::Overlay o);

    void chooseFile (const juce::String& title, const juce::String& patterns, const juce::Identifier& lastPathKey,
                     std::function<void (const juce::File&)> onChosen, bool folders = false);
    void savePreset();
    void loadPreset();
    void loadPresetFile (const juce::File& file);
    void loadFactoryPreset (const juce::var& preset);
    void showPresetMenu();
    void showAbout();
    void showMidiMappings();
    void loadCapture (int slot);
    void showCaptureMenu (int slot, juce::Component& near);
    void showMicMenu (int mic, juce::Component& near);
    void sceneMenu (int index);
    void undo();
    void redo();

    ui::LookAndFeel lookAndFeel; // first, so it outlives every component that draws with it
    AmpSimProcessor& ampSim;
    float canvasScale = 1.0f;

    std::unique_ptr<Canvas> canvas; // everything, laid out on the 1280 x 760 canvas and scaled to the window
    ui::TopBar topBar { ampSim };
    ui::ScenesBar scenesBar;
    ui::TunerReadout tunerReadout;
    std::vector<std::unique_ptr<ui::BlockPage>> blockPages;
    std::array<ui::BlockPage*, ui::numBlocks> blockPageFor {};
    std::unique_ptr<ui::AmpView> ampView;
    std::unique_ptr<ui::CabView> cabView;
    std::unique_ptr<ui::EqStage> eqStage;
    std::unique_ptr<ui::SettingsPage> settingsPage;
    std::unique_ptr<ui::TunerPage> tunerPage;
    std::unique_ptr<ui::ToneMatchPage> toneMatchPage;
    std::unique_ptr<ui::Stage> stage;
    std::unique_ptr<ui::Board> board;
    std::unique_ptr<ui::Inspector> inspector;
    std::unique_ptr<ui::OverlayFrame> overlayFrame;
    ui::Overlay overlay = ui::Overlay::none;
    ui::BlockId selectedBlock = ui::BlockId::count;
    std::unique_ptr<StatusLine> statusLine;
    std::unique_ptr<juce::TooltipWindow> tooltips;

    juce::String presetMessage, statusText;
    double lastMeterMs = 0.0;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Component::SafePointer<juce::AlertWindow> renameWindow; // the open scene Rename dialog, if any

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

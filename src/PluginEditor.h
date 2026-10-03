// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PluginProcessor.h"
#include "ui/AmpView.h"
#include "ui/CabView.h"
#include "ui/ChainNav.h"
#include "ui/LookAndFeel.h"
#include "ui/MainPages.h"
#include "ui/Pages.h"
#include "ui/TopBar.h"
#include "ui/TunerPage.h"

#include <juce_audio_processors/juce_audio_processors.h>

using ui::IgnoresRightClick;
using ToggleControl = ui::IgnoresRightClick<juce::ToggleButton>;

/// The GUI, after the UI handoff (docs/ui/amp-ui-handoff/; BUILD_PLAN "GUI"). A fixed 1280 x 760 canvas,
/// scaled uniformly to the window and letterboxed (ASSUMPTIONS UH11): the top bar (56), the main area
/// (padding 18 top, 40 sides, 16 bottom) showing one page, and the signal chain along the bottom (72),
/// whose blocks open the pages: Input, Pre FX, Amp, EQ, Cab, Post FX, Output. The top bar's Tuner opens
/// the tuner page. Messages (the sample rate, MIDI learn, a preset's problems) show on the Amp page's info
/// row, and elsewhere in a 12 px line at the bottom-left of the main area (UH5).
class AmpSimEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AmpSimEditor (AmpSimProcessor&);
    ~AmpSimEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /// Shows a page, as a click on its chain block does (the tuner's engages the tuner; leaving it
    /// disengages it).
    void showPage (ui::PageId page);
    ui::PageId getShownPage() const noexcept { return shownPage; }

    /// Shows the page an effect block is on, with its tab selected (the amp, the cab, the ends: their pages).
    void selectBlock (ui::BlockId id);
    ui::BlockId getSelectedBlock() const noexcept { return selectedBlock; }

    /// An effect block's editor (every block but the input, amp, cab, and output).
    ui::BlockPage& getPage (ui::BlockId id) { return *blockPageFor[(size_t) id]; }

    /// A main page's component.
    juce::Component& getPageComponent (ui::PageId page) { return *pageComponents[(size_t) page]; }

    ui::ChainNav& getChainNav() noexcept { return chainNav; }
    ui::TopBar& getTopBar() noexcept { return topBar; }
    ui::SectionPage& getSectionPage (ui::Section section) { return section == ui::Section::pre ? *prePage : *postPage; }
    ui::EqView& getEqView() noexcept { return *eqView; }
    ui::AmpView& getAmpView() noexcept { return *ampView; }
    ui::CabView& getCabView() noexcept { return *cabView; }
    ui::OutputPage& getOutputPage() noexcept { return *outputPage; }
    ui::ScenesBar& getScenesBar() noexcept { return outputPage->getScenesBar(); }
    ui::TunerPage& getTunerPage() noexcept { return *tunerPage; }

    /// The canvas's scale and where it sits in the window (letterboxed when the aspect differs).
    float getCanvasScale() const noexcept { return canvasScale; }
    juce::Rectangle<int> getCanvasBounds() const;

    /// For tests and snapshots: what the status timer does, now (status lines, switches, scenes, tuner).
    void refresh() { refreshState(); }

    /// The message line as shown (for tests): the Amp page's info row or the line at the bottom-left.
    juce::String getStatusText() const { return statusText; }

    /// A/B: show B (true) or A, as the Output page's buttons do.
    void abSelect (bool b);

    /// The previous (-1) or next (+1) preset in the list (the factory five, then the preset folder), as
    /// the top bar's arrows do. Returns the name loaded (empty if there's nothing to step to).
    juce::String stepPreset (int delta);

    /// A right-click on any control attached to a parameter: MIDI learn for it, its mappings, and "Held by
    /// scenes" (BUILD_PLAN "MIDI control"). The editor listens to every child's mouse events for this; the
    /// controls themselves ignore right-clicks. Every other press starts an undo step.
    void mouseDown (const juce::MouseEvent& e) override;

    /// For tests: the menu a right-click on this parameter's control would show.
    juce::PopupMenu midiMenuFor (const juce::String& parameterId);

    /// For tests: the menu a right-click on scene tile `index` (0 to 7) would show: store the current
    /// sound there, rename it (a dialog), or clear it.
    juce::PopupMenu sceneMenuFor (int index);

    /// The preset browser's menu (a click on the preset's name shows it).
    juce::PopupMenu presetMenu();

    /// The brand's menu (a click on "rig" shows it): "Amp Sim <version>", "Check for updates..." (enabled
    /// when this build has an updater running), and "About / licenses" (ASSUMPTIONS DS9).
    juce::PopupMenu brandMenu();
    /// A cab mic's load menu (0, 1, or the room): Load an IR file, Built-in IRs (a submenu per bundled
    /// cab, as on the cab page), Load a cab pack folder (close mics), Clear.
    juce::PopupMenu micMenu (int mic);

    /// What "About / licenses" shows: the name, version, update status, and THIRD_PARTY_NOTICES.txt.
    static juce::String aboutText();

private:
    class Canvas;
    class StatusLine;

    void timerCallback() override;
    void refreshState();
    void layoutCanvas();
    ui::ControlGroup* hooksFor (ui::PageId page);

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
    void clickScene (int index);
    void sceneMenu (int index);
    bool keyPressed (const juce::KeyPress& key) override;

    ui::LookAndFeel lookAndFeel; // first, so it outlives every component that draws with it
    AmpSimProcessor& ampSim;
    float canvasScale = 1.0f;

    std::unique_ptr<Canvas> canvas; // everything, laid out on the 1280 x 760 canvas and scaled to the window
    ui::TopBar topBar { ampSim };
    ui::ChainNav chainNav { ampSim };
    std::vector<std::unique_ptr<ui::BlockPage>> blockPages;
    std::array<ui::BlockPage*, ui::numBlocks> blockPageFor {};
    std::unique_ptr<ui::AmpView> ampView;
    std::unique_ptr<ui::CabView> cabView;
    std::unique_ptr<ui::SectionPage> prePage, postPage;
    std::unique_ptr<ui::EqView> eqView;
    std::unique_ptr<ui::InputPage> inputPage;
    std::unique_ptr<ui::OutputPage> outputPage;
    std::unique_ptr<ui::TunerPage> tunerPage;
    std::array<juce::Component*, ui::numPages> pageComponents {};
    ui::PageId shownPage = ui::PageId::count, pageBeforeTuner = ui::PageId::amp;
    ui::BlockId selectedBlock = ui::BlockId::amp;
    std::unique_ptr<StatusLine> statusLine;
    std::unique_ptr<juce::TooltipWindow> tooltips;

    juce::String presetMessage, statusText;
    double lastMeterMs = 0.0;
    int ticks = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Component::SafePointer<juce::AlertWindow> renameWindow; // the open scene Rename dialog, if any

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AmpSimEditor)
};

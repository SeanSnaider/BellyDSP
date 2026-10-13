// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PluginEditor.h"
#include "BlockParameters.h"
#include "platform/AppInfo.h"
#include "platform/SampleRateGuard.h"
#include "platform/Updater.h"
#include "ui/Overlay.h"

using namespace ui::theme;

namespace
{
const juce::Identifier uiStageKey { "uiStage" };       // the stage mode last shown (app state)
const juce::Identifier uiSelectedKey { "uiSelected" }; // the block last selected (app state: its pageName)
const juce::Identifier uiBoardTabKey { "uiBoardTab" }; // the board's tab last open (app state: ui::boardTabName)
const juce::Identifier presetSourceKey = AmpSimProcessor::presetSourceKey; // "factory" or "user": the top bar's tag

/// The main screen's parts on the canvas.
juce::Rectangle<int> stageArea() { return { 0, topBarHeight, stageWidth, stageHeight }; }
juce::Rectangle<int> boardArea() { return { 0, topBarHeight + stageHeight, stageWidth, boardHeight }; }
juce::Rectangle<int> inspectorArea() { return { stageWidth, topBarHeight, inspectorWidth, canvasHeight - topBarHeight - footerHeight }; }
juce::Rectangle<int> footerArea() { return { 0, canvasHeight - footerHeight, canvasWidth, footerHeight }; }
juce::Rectangle<int> overlayArea() { return { 0, topBarHeight, canvasWidth, canvasHeight - topBarHeight - footerHeight }; }

/// A rate as the warning says it: "44.1 kHz", "96 kHz".
juce::String rateText (double hz)
{
    const auto k = hz / 1000.0;
    return (std::abs (k - std::round (k)) < 0.05 ? juce::String (juce::roundToInt (k)) : juce::String (k, 1)) + " kHz";
}

/// Every preset the arrows step through: the factory five, then the preset folder's files (sorted, the
/// subfolders' too).
struct PresetEntry
{
    juce::String name;
    bool factory = false;
    juce::var preset;
    juce::File file;
};

std::vector<PresetEntry> presetList()
{
    std::vector<PresetEntry> list;
    for (const auto& preset : presets::factoryPresets())
        list.push_back ({ preset["name"].toString(), true, preset, {} });
    if (const auto folder = presets::defaultFolder(); folder.isDirectory())
    {
        auto files = folder.findChildFiles (juce::File::findFiles, true, "*.json");
        files.sort();
        for (const auto& f : files)
            list.push_back ({ f.getFileNameWithoutExtension(), false, {}, f });
    }
    return list;
}

/// Each pedal's parameter prefix: its page covers every parameter under it (BlockPage::coverParameters).
juce::String prefixFor (ui::BlockId id)
{
    using B = ui::BlockId;
    switch (id)
    {
        case B::gateA:          return "gate_a_";
        case B::gateB:          return "gate_b_";
        case B::preCompressor:  return "comp_pre_";
        case B::postCompressor: return "comp_post_";
        case B::boost:          return "boost_";
        case B::overdrive:      return "od_";
        case B::preEq:          return "eq_pre_";
        case B::postEq:         return "eq_post_";
        case B::harmonizer:     return "harm_";
        case B::multivoicer:    return "mv_";
        case B::bloom:          return "bloom_";
        case B::chorus:         return "chorus_";
        case B::delay:          return "delay_";
        case B::reverb:         return "reverb_";
        case B::input:
        case B::amp:
        case B::cab:
        case B::output:
        case B::count:          break;
    }
    return {};
}
} // namespace

/// The 1280 x 760 canvas: the window's ground with its 12 px corners and 1 px hairline border.
class AmpSimEditor::Canvas final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        g.setColour (bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), radiusWindow);

        // The footer's panel, under the scenes and the tuner's reading, with a hairline along its top.
        juce::Path window;
        window.addRoundedRectangle (getLocalBounds().toFloat(), radiusWindow);
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (window);
        g.setColour (panelBg);
        g.fillRect (footerArea());
        g.setColour (line1);
        g.fillRect (footerArea().withHeight (1));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (line1);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), radiusWindow, 1.0f);
    }
};

/// The message line at the bottom left of the stage or an overlay: 12 px ink-dim, one line, the whole text in its
/// tooltip.
class AmpSimEditor::StatusLine final : public juce::Component, public juce::SettableTooltipClient
{
public:
    StatusLine() { setInterceptsMouseClicks (true, false); }

    void set (const juce::String& newMessage)
    {
        if (newMessage != message)
        {
            message = newMessage;
            setTooltip (message);
            setVisible (message.isNotEmpty());
            repaint();
        }
    }

    /// On the stage's Cab and EQ views the line sits at the right of the foot (the cab's short line has its left).
    void setRightAligned (bool shouldBeRight)
    {
        if (shouldBeRight != rightAligned)
        {
            rightAligned = shouldBeRight;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkDim);
        g.drawFittedText (message, getLocalBounds(), rightAligned ? juce::Justification::centredRight : juce::Justification::centredLeft, 1, 0.9f);
    }

private:
    juce::String message;
    bool rightAligned = false;
};

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p) : AudioProcessorEditor (&p), ampSim (p)
{
    setLookAndFeel (&lookAndFeel);
    canvas = std::make_unique<Canvas>();
    addAndMakeVisible (*canvas);
    tooltips = std::make_unique<juce::TooltipWindow> (canvas.get(), 600);
    statusLine = std::make_unique<StatusLine>();

    // Every pedal's controls, all made now so selecting one is instant; each covers every parameter under its
    // block's prefix (a parameter added later without a control gets a generic one).
    using B = ui::BlockId;
    const auto add = [this] (std::unique_ptr<ui::BlockPage> page, B id)
    {
        blockPageFor[(size_t) id] = page.get();
        blockPages.push_back (std::move (page));
    };
    add (std::make_unique<ui::GatePage> (p, false), B::gateA);
    add (std::make_unique<ui::CompressorPage> (p, false), B::preCompressor);
    add (std::make_unique<ui::BoostPage> (p), B::boost);
    add (std::make_unique<ui::OverdrivePage> (p), B::overdrive);
    add (std::make_unique<ui::EqPage> (p, false), B::preEq);
    add (std::make_unique<ui::GatePage> (p, true), B::gateB);
    add (std::make_unique<ui::EqPage> (p, true), B::postEq);
    add (std::make_unique<ui::CompressorPage> (p, true), B::postCompressor);
    add (std::make_unique<ui::HarmonizerPage> (p), B::harmonizer);
    add (std::make_unique<ui::MultivoicerPage> (p), B::multivoicer);
    add (std::make_unique<ui::BloomPage> (p), B::bloom);
    add (std::make_unique<ui::ChorusPage> (p), B::chorus);
    add (std::make_unique<ui::DelayPage> (p), B::delay);
    add (std::make_unique<ui::ReverbPage> (p), B::reverb);
    for (auto& page : blockPages)
        page->coverParameters (prefixFor (page->getBlockId()), { "harm_custom_mask" /* its twelve note buttons */ });

    // The stage's views, the overlays' pages.
    ampView = std::make_unique<ui::AmpView> (p);
    ampView->onLoadCapture = [this] (int slot) { loadCapture (slot); };
    ampView->onCaptureMenu = [this] (int slot, juce::Component& near) { showCaptureMenu (slot, near); };
    cabView = std::make_unique<ui::CabView> (p);
    cabView->onBrowse = [this] (bool pack)
    {
        const auto picked = [this] (const juce::File& f) { cabView->pick (f); };
        if (pack)
            chooseFile ("Choose a cab pack folder", {}, AmpSimProcessor::cabPathKey (0), picked, true);
        else
            chooseFile ("Choose an impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::cabPathKey (0), picked);
    };
    cabView->onMicMenu = [this] (int mic, juce::Component& near) { showMicMenu (mic, near); };
    eqStage = std::make_unique<ui::EqStage> (p);
    eqStage->onChoose = [this] (bool pre) { select (pre ? B::preEq : B::postEq, true); };

    settingsPage = std::make_unique<ui::SettingsPage> (p);
    settingsPage->onClose = [this] { closeOverlay(); };
    settingsPage->onLicences = [this] { showAbout(); };
    auto& output = settingsPage->getOutputPage();
    output.onAbSelect = [this] (bool b) { abSelect (b); };
    output.onAbCopy = [this] { ampSim.abCopyToOther(); };
    output.onTap = [this] { ampSim.tapTempo(); };
    output.onMidiMappings = [this] { showMidiMappings(); };
    tunerPage = std::make_unique<ui::TunerPage> (p);
    tunerPage->onClose = [this] { closeOverlay(); };
    toneMatchPage = std::make_unique<ui::ToneMatchPage> (p);
    toneMatchPage->onClose = [this] { closeOverlay(); };

    // The main screen.
    stage = std::make_unique<ui::Stage> (*ampView, *cabView, *eqStage);
    stage->onModeChosen = [this] (ui::StageMode m) { setStageMode (m); };
    board = std::make_unique<ui::Board> (p);
    board->onSelect = [this] (B id) { selectBlock (id); };
    board->onTabChange = [this] (ui::BoardTab t) { ampSim.parameters.state.setProperty (uiBoardTabKey, ui::boardTabName (t), nullptr); };
    inspector = std::make_unique<ui::Inspector>();
    overlayFrame = std::make_unique<ui::OverlayFrame>();
    overlayFrame->onClose = [this] { closeOverlay(); };

    canvas->addAndMakeVisible (*stage);
    canvas->addAndMakeVisible (*board);
    canvas->addAndMakeVisible (*inspector);
    canvas->addAndMakeVisible (scenesBar);
    canvas->addAndMakeVisible (tunerReadout);
    canvas->addChildComponent (*overlayFrame);
    canvas->addAndMakeVisible (topBar);
    canvas->addChildComponent (*statusLine);

    topBar.onPresetMenu = [this] { showPresetMenu(); };
    topBar.onSave = [this] { savePreset(); };
    topBar.onPrevious = [this] { stepPreset (-1); };
    topBar.onNext = [this] { stepPreset (1); };
    topBar.onTuner = [this] { overlay == ui::Overlay::tuner ? closeOverlay() : openOverlay (ui::Overlay::tuner); };
    topBar.onMatchTone = [this] { overlay == ui::Overlay::toneMatch ? closeOverlay() : openOverlay (ui::Overlay::toneMatch); };
    topBar.onSettings = [this] { overlay == ui::Overlay::settings ? closeOverlay() : openOverlay (ui::Overlay::settings); };
    topBar.onBrand = [this] { ui::showMenu (brandMenu(), &topBar.getBrandButton(), &lookAndFeel); };
    topBar.onAbSelect = [this] (bool b) { abSelect (b); };
    topBar.onUndo = [this] { undo(); };
    topBar.onRedo = [this] { redo(); };
    scenesBar.onClick = [this] (int i) { clickScene (i); };
    tunerReadout.onClick = [this] { overlay == ui::Overlay::tuner ? closeOverlay() : openOverlay (ui::Overlay::tuner); };
    ui::tagged (tunerReadout, "tuner_on");

    addMouseListener (this, true); // right-clicks anywhere, for MIDI learn; every press starts an undo step
    setWantsKeyboardFocus (true);

    // The app's view state: the stage mode and the block last selected, and the window's size (UH11; the older
    // page key is left alone and no longer used).
    const auto& state = ampSim.parameters.state;
    auto firstMode = ui::StageMode::amp;
    for (int m = 0; m < ui::numStageModes; ++m)
        if (state.getProperty (uiStageKey).toString() == ui::stageModeName ((ui::StageMode) m))
            firstMode = (ui::StageMode) m;
    stage->setMode (firstMode, false);
    auto firstBlock = ui::blockNamed (state.getProperty (uiSelectedKey).toString());
    if (firstBlock == B::count || firstBlock == B::input || firstBlock == B::output)
        firstBlock = firstMode == ui::StageMode::cab ? B::cab : (firstMode == ui::StageMode::eq ? B::postEq : B::amp);

    // (Read before setting the limits: the limits resize the editor, which would write its size.)
    const auto width = juce::jmax (minimumWidth, (int) state.getProperty (AmpSimProcessor::uiWidthKey, canvasWidth));
    const auto height = juce::jmax (minimumHeight, (int) state.getProperty (AmpSimProcessor::uiHeightKey, canvasHeight));
    setResizable (true, false);
    setResizeLimits (minimumWidth, minimumHeight, 8192, 8192);
    setSize (width, height);
    // The board's tab last open (Pre FX the first time), read before the selection (which opens its own tab) and set
    // after it: a remembered tab wins over the one the selection would open.
    const auto firstTab = ui::boardTabNamed (state.getProperty (uiBoardTabKey).toString());
    select (firstBlock, false);
    board->setTab (firstTab == ui::BoardTab::count ? ui::BoardTab::pre : firstTab);

    refreshState();
    startTimerHz (meterFps);

    // Automatic updates, in the standalone app only (a plugin's updates belong to whoever installed it, and
    // the tests' processors are never "Standalone"). Message thread; the updater never touches audio.
    // Also in the standalone app only: keep the audio device at 48 kHz (src/platform/SampleRateGuard.h).
    if (ampSim.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
    {
        platform::updater::start();
        platform::samplerate::start();
    }
}

AmpSimEditor::~AmpSimEditor()
{
    stopTimer();
    platform::samplerate::stop();
    if (auto* page = overlayPage (overlay))
        page->pageHidden();
    inspector->show (nullptr);
    stage->finishFade();
    stage->getView (stage->getMode()).pageHidden();
    ampSim.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::off); // nobody reads the ring any more
    if (renameWindow != nullptr)
    {
        // A Rename dialog is a separate window that outlives us: it must stop drawing with our
        // LookAndFeel before that's destroyed, and it's dismissed (as a cancel).
        renameWindow->setLookAndFeel (nullptr);
        renameWindow->exitModalState (0);
    }
    setLookAndFeel (nullptr);
}

// ---- The canvas and the window ----------------------------------------------------------------------

void AmpSimEditor::paint (juce::Graphics& g)
{
    g.fillAll (letterbox);
}

juce::Rectangle<int> AmpSimEditor::getCanvasBounds() const
{
    const auto w = juce::roundToInt ((float) canvasWidth * canvasScale), h = juce::roundToInt ((float) canvasHeight * canvasScale);
    return { (getWidth() - w) / 2, (getHeight() - h) / 2, w, h };
}

void AmpSimEditor::resized()
{
    // The canvas keeps its 1280 x 760 layout and is scaled uniformly to fit, centred: letterboxed in the
    // darker colour when the window's aspect differs (UH11).
    canvasScale = juce::jmin ((float) getWidth() / (float) canvasWidth, (float) getHeight() / (float) canvasHeight);
    const auto target = getCanvasBounds();
    canvas->setBounds (0, 0, canvasWidth, canvasHeight);
    canvas->setTransform (juce::AffineTransform::scale (canvasScale).translated ((float) target.getX(), (float) target.getY()));
    layoutCanvas();

    auto& state = ampSim.parameters.state;
    state.setProperty (AmpSimProcessor::uiWidthKey, getWidth(), nullptr);
    state.setProperty (AmpSimProcessor::uiHeightKey, getHeight(), nullptr);
}

void AmpSimEditor::layoutCanvas()
{
    topBar.setBounds (0, 0, canvasWidth, topBarHeight);
    stage->setBounds (stageArea());
    board->setBounds (boardArea());
    inspector->setBounds (inspectorArea());
    overlayFrame->setBounds (overlayArea());

    // The footer: the scenes from the left, the tuner's reading at the right.
    auto footer = footerArea().reduced (18, 0);
    tunerReadout.setBounds (footer.removeFromRight (220).withSizeKeepingCentre (220, 40));
    footer.removeFromRight (24);
    scenesBar.setBounds (footer.withSizeKeepingCentre (footer.getWidth(), 44));

    // The message line: over an overlay's foot while one is open, else at the foot of the stage (the amp's info row
    // carries it while the stage shows the amp).
    if (overlay != ui::Overlay::none)
        statusLine->setBounds (overlayArea().getX() + ui::OverlayFrame::padSide, overlayArea().getBottom() - 16, overlayArea().getWidth() - 2 * ui::OverlayFrame::padSide, 14);
    else
        statusLine->setBounds (stageArea().getX() + stageArea().getWidth() / 2, stageArea().getBottom() - 30, stageArea().getWidth() / 2 - 24, 16);
    statusLine->setRightAligned (overlay == ui::Overlay::none);
    statusLine->toFront (false);
}

// ---- The main screen: selection, the stage, the overlays -------------------------------------------

ui::ControlGroup* AmpSimEditor::inspectorContentFor (ui::BlockId id)
{
    if (id == ui::BlockId::amp)
        return &ampView->getPanel();
    if (id == ui::BlockId::cab)
        return &cabView->getPanel();
    if (id == ui::BlockId::count || id == ui::BlockId::input || id == ui::BlockId::output)
        return nullptr;
    return blockPageFor[(size_t) id];
}

void AmpSimEditor::selectBlock (ui::BlockId id)
{
    select (id, false);
}

void AmpSimEditor::select (ui::BlockId id, bool fromStage)
{
    if (id == ui::BlockId::count)
        return;

    // The input and the output are Settings' pages.
    if (id == ui::BlockId::input || id == ui::BlockId::output)
    {
        openOverlay (ui::Overlay::settings);
        settingsPage->showOutput (id == ui::BlockId::output);
        return;
    }

    // A block on the main screen: back to it from an overlay (the board and the inspector are under it).
    if (overlay != ui::Overlay::none)
        closeOverlay();

    selectedBlock = id;
    inspector->show (inspectorContentFor (id));
    board->setSelected (id);
    ampSim.parameters.state.setProperty (uiSelectedKey, ui::info (id).pageName, nullptr);

    // The stage follows the amp and the cab. An EQ pedal puts the stage on its graph (the bands are in the inspector,
    // the curve and the spectrum on the stage), and the next other pedal puts the stage back where it was, so dialling
    // an EQ never leaves the amp hidden (ASSUMPTIONS RD23).
    const auto isEq = id == ui::BlockId::preEq || id == ui::BlockId::postEq;
    if (isEq)
        eqStage->showPre (id == ui::BlockId::preEq);
    if (fromStage)
        return;
    if (id == ui::BlockId::amp || id == ui::BlockId::cab)
    {
        eqShownForPedal = false;
        setStageMode (id == ui::BlockId::amp ? ui::StageMode::amp : ui::StageMode::cab);
    }
    else if (isEq)
    {
        if (stage->getMode() != ui::StageMode::eq)
        {
            stageBeforeEq = stage->getMode();
            eqShownForPedal = true;
        }
        showStage (ui::StageMode::eq, true);
    }
    else if (eqShownForPedal && stage->getMode() == ui::StageMode::eq)
    {
        eqShownForPedal = false;
        showStage (stageBeforeEq, true);
    }
}

void AmpSimEditor::showStage (ui::StageMode mode, bool animate)
{
    // The stage alone, the selection left as it is.
    stage->setMode (mode, animate);
    ampSim.parameters.state.setProperty (uiStageKey, ui::stageModeName (mode), nullptr);
    layoutCanvas();
    refreshState();
}

void AmpSimEditor::setStageMode (ui::StageMode mode, bool animate)
{
    if (mode == ui::StageMode::count)
        return;
    eqShownForPedal = false; // the player picked the stage: it stays where they put it
    stage->setMode (mode, animate);
    ampSim.parameters.state.setProperty (uiStageKey, ui::stageModeName (mode), nullptr);
    const auto wanted = mode == ui::StageMode::amp ? ui::BlockId::amp
                        : mode == ui::StageMode::cab ? ui::BlockId::cab
                                                     : (eqStage->isShowingPre() ? ui::BlockId::preEq : ui::BlockId::postEq);
    if (selectedBlock != wanted)
        select (wanted, true);
    layoutCanvas();
    refreshState();
}

ui::ControlGroup* AmpSimEditor::overlayPage (ui::Overlay o)
{
    switch (o)
    {
        case ui::Overlay::tuner:     return tunerPage.get();
        case ui::Overlay::toneMatch: return toneMatchPage.get();
        case ui::Overlay::settings:  return settingsPage.get();
        case ui::Overlay::none:      break;
    }
    return nullptr;
}

void AmpSimEditor::openOverlay (ui::Overlay o)
{
    if (o == ui::Overlay::none)
    {
        closeOverlay();
        return;
    }
    if (o == overlay)
        return;

    // Leaving the tuner disengages it; opening it engages it (the tuner never runs behind the main screen).
    auto* tunerOn = ampSim.parameters.getParameter ("tuner_on");
    if (overlay == ui::Overlay::tuner && tunerOn->getValue() >= 0.5f)
        tunerOn->setValueNotifyingHost (0.0f);
    if (o == ui::Overlay::tuner && tunerOn->getValue() < 0.5f)
        tunerOn->setValueNotifyingHost (1.0f);

    if (auto* old = overlayPage (overlay))
    {
        old->setVisible (false);
        old->pageHidden();
    }
    overlay = o;
    auto* page = overlayPage (o);
    overlayFrame->hold (page, {}); // each overlay draws its own header with Close (ui::overlay::paintHeader)
    overlayFrame->setVisible (true);
    overlayFrame->toFront (false);
    topBar.toFront (false);
    page->setVisible (true);
    page->pageShown();
    page->refresh();
    topBar.setOverlay (o);
    layoutCanvas();
    refreshState();
}

void AmpSimEditor::closeOverlay()
{
    if (overlay == ui::Overlay::none)
        return;
    auto* tunerOn = ampSim.parameters.getParameter ("tuner_on");
    if (overlay == ui::Overlay::tuner && tunerOn->getValue() >= 0.5f)
        tunerOn->setValueNotifyingHost (0.0f);
    if (auto* page = overlayPage (overlay))
    {
        page->setVisible (false);
        page->pageHidden();
    }
    overlay = ui::Overlay::none;
    overlayFrame->setVisible (false);
    topBar.setOverlay (overlay);
    layoutCanvas();
    refreshState();
    if (isShowing())
        grabKeyboardFocus(); // the keys work again from the main screen
}

void AmpSimEditor::finishAnimations()
{
    stage->finishFade();
    scenesBar.finishGlide();
}

// ---- The timer --------------------------------------------------------------------------------------

void AmpSimEditor::timerCallback()
{
    // The meters, the LEDs, and the tuner's reading every frame (30 a second); everything else ten times a second.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = lastMeterMs > 0.0 ? juce::jlimit (0.0, 0.5, (now - lastMeterMs) / 1000.0) : 1.0 / meterFps;
    lastMeterMs = now;
    topBar.updateMeters (ampSim.takePeaks(), seconds);
    const auto cpu = ampSim.getCpuLoad();
    topBar.setCpuLoad (cpu);
    getOutputPage().getCpuMeter().setLoad (cpu);
    board->refresh();
    tunerReadout.set (ampSim.isTunerEngaged() && ampSim.parameters.getRawParameterValue ("tuner_on")->load() >= 0.5f, ampSim.getTunerReading());

    if (++ticks % 3 == 0)
        refreshState();
}

void AmpSimEditor::refreshState()
{
    topBar.refresh();
    inspector->refresh();
    stage->getView (stage->getMode()).refresh();
    if (auto* page = overlayPage (overlay))
        page->refresh();
    board->refresh();
    scenesBar.refresh (ampSim.getScenes());

    // The tuner follows its switch both ways: a footswitch that engages it opens its overlay, and one that
    // disengages it closes the overlay.
    const auto tuning = ampSim.parameters.getRawParameterValue ("tuner_on")->load() >= 0.5f;
    if (tuning && overlay != ui::Overlay::tuner)
        openOverlay (ui::Overlay::tuner);
    else if (! tuning && overlay == ui::Overlay::tuner)
        closeOverlay();

    // The tuner button's note: the one it hears, only while it's engaged.
    juce::String note ("-");
    if (const auto reading = ampSim.getTunerReading(); tuning && ampSim.isTunerEngaged() && reading.hasReading)
        note = juce::MidiMessage::getMidiNoteName (reading.midiNote, true, false, 4);
    topBar.setTuner (overlay == ui::Overlay::tuner, note);

    // The message: the sample rate first, then the gate's first switch-on learning the noise floor, then
    // MIDI learn waiting for a controller, then a preset that couldn't load or loaded with gaps. A preset's notes
    // are the preset box's tooltip (RD4-6).
    juce::String message;
    if (! ampSim.isSampleRateOk())
        message = rateText (ampSim.getDeviceSampleRate()) + ": muted, set the interface to 48 kHz";
    else if (const auto prompt = ampSim.getAutoLearnPrompt(); prompt.isNotEmpty())
        message = prompt;
    else if (const auto& midi = ampSim.getMidiMap(); midi.isLearning())
    {
        if (auto* parameter = ampSim.parameters.getParameter (midi.getLearnTarget()))
            message = "MIDI learn: press a footswitch or move a pedal for " + parameter->getName (64) + " (right-click it again to cancel)";
    }
    if (message.isEmpty())
        message = presetMessage.isNotEmpty() ? presetMessage : ampSim.getPresetWarnings().joinIntoString ("; ");
    statusText = message;
    topBar.setPresetNotes (presetNotes);
    // While the stage shows the amp, its info row carries the message (the gate's first switch-on included: Gate A
    // is switched from its own pedal now, RD4-9).
    ampView->setStatus (message);
    const auto inInfoRow = overlay == ui::Overlay::none && stage->getMode() == ui::StageMode::amp;
    statusLine->set (inInfoRow ? juce::String() : message);
}

// ---- Files and presets ------------------------------------------------------------------------------

void AmpSimEditor::chooseFile (const juce::String& title, const juce::String& patterns, const juce::Identifier& lastPathKey,
                               std::function<void (const juce::File&)> onChosen, bool folders)
{
    // Start in the folder of the last file loaded, if there is one.
    const auto lastPath = ampSim.parameters.state.getProperty (lastPathKey).toString();
    const auto start = juce::File::isAbsolutePath (lastPath) ? juce::File (lastPath).getParentDirectory()
                                                             : juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
    const auto flags = juce::FileBrowserComponent::openMode
                       | (folders ? juce::FileBrowserComponent::canSelectDirectories : juce::FileBrowserComponent::canSelectFiles);
    chooser->launchAsync (flags, [onChosen, folders] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (folders ? file.isDirectory() : file.existsAsFile())
                                  onChosen (file);
                          });
}

void AmpSimEditor::savePreset()
{
    const auto folder = presets::defaultFolder();
    folder.createDirectory();
    const auto name = ampSim.getPresetName();
    chooser = std::make_unique<juce::FileChooser> ("Save preset", folder.getChildFile ((name.isEmpty() ? juce::String ("My tone") : name) + ".json"), "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              file = file.withFileExtension ("json");
                              const auto preset = ampSim.capturePreset (file.getFileNameWithoutExtension());
                              const auto saved = presets::save (preset, file);
                              presetMessage = saved ? juce::String() : "Couldn't write " + file.getFullPathName();
                              ampSim.parameters.state.setProperty ("presetName", file.getFileNameWithoutExtension(), nullptr);
                              ampSim.parameters.state.setProperty (presetSourceKey, "user", nullptr);
                              refreshState();
                          });
}

void AmpSimEditor::loadPreset()
{
    const auto folder = presets::defaultFolder();
    folder.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Load preset", folder, "*.json");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (const auto file = fc.getResult(); file.existsAsFile())
                                  loadPresetFile (file);
                          });
}

void AmpSimEditor::loadPresetFile (const juce::File& file)
{
    juce::String problem;
    const auto preset = presets::load (file, problem);
    const auto result = problem.isEmpty() ? ampSim.loadPreset (preset) : presets::ApplyResult { false, problem, {} };
    presetMessage = result.ok ? juce::String() : result.error;
    presetNotes = result.ok ? preset["notes"].toString() : juce::String();
    if (result.ok)
        ampSim.parameters.state.setProperty (presetSourceKey, "user", nullptr);
    refreshState();
}

void AmpSimEditor::loadFactoryPreset (const juce::var& preset)
{
    const auto result = ampSim.loadPreset (preset);
    // A preset's notes are about the preset: the preset box's tooltip, not a line on the stage (RD4-6).
    presetMessage = result.ok ? juce::String() : result.error;
    presetNotes = result.ok ? preset["notes"].toString() : juce::String();
    if (result.ok)
        ampSim.parameters.state.setProperty (presetSourceKey, "factory", nullptr);
    refreshState();
}

juce::String AmpSimEditor::stepPreset (int delta)
{
    const auto list = presetList();
    if (list.empty())
        return {};

    // Where the current preset is in the list (by its source and name); from nowhere, the first or last.
    const auto name = ampSim.getPresetName();
    const auto factory = ampSim.parameters.state.getProperty (presetSourceKey).toString() == "factory";
    int current = -1;
    for (int i = 0; i < (int) list.size(); ++i)
        if (list[(size_t) i].name == name && list[(size_t) i].factory == factory)
            current = i;
    const auto count = (int) list.size();
    const auto next = current < 0 ? (delta > 0 ? 0 : count - 1) : ((current + delta) % count + count) % count;

    const auto& entry = list[(size_t) next];
    if (entry.factory)
        loadFactoryPreset (entry.preset);
    else
        loadPresetFile (entry.file);
    return entry.name;
}

juce::PopupMenu AmpSimEditor::presetMenu()
{
    // The browser: the factory presets, the preset folder (its subfolders as submenus), open, save as.
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    menu.addSectionHeader ("Factory presets (load your own captures and IRs)");
    for (const auto& preset : presets::factoryPresets())
        menu.addItem (preset["name"].toString(), [safe, preset]
        {
            if (safe != nullptr)
                safe->loadFactoryPreset (preset);
        });

    std::function<int (juce::PopupMenu&, const juce::File&, int)> addFolder = [&addFolder, safe] (juce::PopupMenu& into, const juce::File& folder, int depth)
    {
        int count = 0;
        auto subfolders = folder.findChildFiles (juce::File::findDirectories, false);
        subfolders.sort();
        for (const auto& sub : subfolders)
        {
            juce::PopupMenu inner;
            if (depth < 3 && addFolder (inner, sub, depth + 1) > 0)
            {
                into.addSubMenu (sub.getFileName(), inner);
                ++count;
            }
        }
        auto files = folder.findChildFiles (juce::File::findFiles, false, "*.json");
        files.sort();
        for (const auto& file : files)
        {
            if (count >= 200)
                break;
            into.addItem (file.getFileNameWithoutExtension(), [safe, file]
            {
                if (safe != nullptr)
                    safe->loadPresetFile (file);
            });
            ++count;
        }
        return count;
    };

    if (const auto folder = presets::defaultFolder(); folder.isDirectory())
    {
        juce::PopupMenu probe;
        if (addFolder (probe, folder, 0) > 0)
        {
            menu.addSeparator();
            menu.addSectionHeader ("Your presets");
            addFolder (menu, folder, 0);
        }
    }

    menu.addSeparator();
    menu.addItem ("Open a preset file...", [safe] { if (safe != nullptr) safe->loadPreset(); });
    menu.addItem ("Save as...", [safe] { if (safe != nullptr) safe->savePreset(); });
    return menu;
}

void AmpSimEditor::showPresetMenu()
{
    ui::showMenu (presetMenu(), &topBar.getPresetButton(), &lookAndFeel);
}

// ---- The brand's menu: version, updates, licences -------------------------------------------------

juce::PopupMenu AmpSimEditor::brandMenu()
{
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    // Two labels, then the commands. The licence line and the source link are the AGPL's "Appropriate
    // Legal Notices" and its offer of the Corresponding Source (sections 5 and 13), one click away.
    menu.addItem (juce::String (platform::productName) + " " + platform::appVersion(), false, false, nullptr);
    menu.addItem ("Free software under the GNU AGPL v3 or later", false, false, nullptr);
    menu.addSeparator();
    menu.addItem ("Match tone...", [safe] { if (safe != nullptr) safe->openOverlay (ui::Overlay::toneMatch); });
    menu.addSeparator();
    menu.addItem ("Check for updates...", platform::updater::isRunning(), false, [] { platform::updater::checkNow(); });
    menu.addItem ("Source code for this version", [] { juce::URL (platform::sourceUrlForThisVersion()).launchInDefaultBrowser(); });
    menu.addItem ("About / licenses", [safe] { if (safe != nullptr) safe->showAbout(); });
    return menu;
}

juce::String AmpSimEditor::aboutText()
{
    juce::String about;
    about << platform::productName << " " << platform::appVersion() << "\n"
          << "Sean Snaider's guitar amp sim and multi-effects, with NAM captures running on NeuralAmpModelerCore.\n"
          << "Copyright (C) 2026 Sean Snaider. Free software under the GNU AGPL v3 or later, with ABSOLUTELY NO WARRANTY.\n"
          << "Source code for this version: " << platform::sourceUrlForThisVersion() << "\n"
          << platform::updater::describe() << ".\n\n";
    const auto notices = platform::noticesFile();
    if (notices.existsAsFile())
        about << notices.loadFileAsString();
    else
        about << "THIRD_PARTY_NOTICES.txt is missing from this build (expected at " << notices.getFullPathName() << ").\n";
    return about;
}

void AmpSimEditor::showAbout()
{
    // A plain, non-modal window with the text, read-only and scrollable.
    auto editor = std::make_unique<juce::TextEditor>();
    editor->setMultiLine (true);
    editor->setReadOnly (true);
    editor->setScrollbarsShown (true);
    editor->setCaretVisible (false);
    editor->setFont (geist (Weight::regular, 13.0f));
    editor->setColour (juce::TextEditor::backgroundColourId, bg);
    editor->setColour (juce::TextEditor::textColourId, ink);
    editor->setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor->setText (aboutText(), false);
    editor->setSize (640, 520);

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "About " + juce::String (platform::productName);
    options.content.setOwned (editor.release());
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = bg;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

// ---- Captures ---------------------------------------------------------------------------------------

void AmpSimEditor::loadCapture (int)
{
    // Whichever amp plays, a file loaded here is your capture (amp 9), which then plays.
    chooseFile ("Choose a NAM capture, or a gain set's gainset.json, to play as your capture", "*.nam;*.json",
                AmpSimProcessor::modelPathKey (AmpSimProcessor::yourCaptureAmp), [this] (const juce::File& f) { ampSim.loadYourCapture (f); });
}

void AmpSimEditor::showCaptureMenu (int slot, juce::Component& near)
{
    ui::showMenu (captureMenu (slot), &near, &lookAndFeel, true);
}

juce::PopupMenu AmpSimEditor::captureMenu (int amp)
{
    // The old slot card's functions, on a right-click at the grille or the model's name (UH12), for the amps
    // (AS10): load your own capture, reload the playing amp's file, remove your capture.
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    amp = juce::jlimit (0, AmpSimProcessor::numAmps - 1, amp);
    const auto path = ampSim.parameters.state.getProperty (AmpSimProcessor::modelPathKey (amp)).toString();
    const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    const auto own = ampSim.parameters.state.getProperty (AmpSimProcessor::modelPathKey (AmpSimProcessor::yourCaptureAmp)).toString();
    menu.addSectionHeader (AmpSimProcessor::ampName (amp));
    menu.addItem ("Load a capture of your own...", [safe, amp] { if (safe != nullptr) safe->loadCapture (amp); });
    menu.addItem ("Reload " + (file != juce::File() ? file.getFileName() : juce::String ("the capture")), file.existsAsFile(), false, [safe, amp, file]
    {
        if (safe != nullptr)
            safe->ampSim.loadModel (amp, file);
    });
    menu.addItem ("Remove your capture", own.isNotEmpty(), false, [safe]
    {
        if (safe == nullptr)
            return;
        // Removed while it plays: Glass plays instead (an empty amp would pass the DI through).
        if (safe->ampSim.getSelectedAmp() == AmpSimProcessor::yourCaptureAmp)
            safe->ampSim.selectAmp (0);
        safe->ampSim.clearModel (AmpSimProcessor::yourCaptureAmp);
    });
    return menu;
}

void AmpSimEditor::showMicMenu (int mic, juce::Component& near)
{
    ui::showMenu (micMenu (mic), &near, &lookAndFeel, true);
}

juce::PopupMenu AmpSimEditor::micMenu (int mic)
{
    // A mic's file: load an IR (or, for a close mic, a cab pack), or clear it. Close mic 1's loads count as
    // picks: they're assigned to the playing amp slot (UH8).
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    const auto load = [safe, mic] (const juce::File& f)
    {
        if (safe == nullptr)
            return;
        if (mic == 0)
            safe->ampSim.pickCab (f);
        else
            safe->ampSim.loadCabIR (mic, f);
    };
    menu.addSectionHeader (mic == AmpSimProcessor::roomMic ? juce::String ("Room mic") : mic == 0 ? juce::String ("Mic A (close mic 1)") : juce::String ("Mic B (close mic 2)"));
    menu.addItem ("Load an IR file...", [safe, mic, load]
    {
        if (safe != nullptr)
            safe->chooseFile ("Choose an impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::cabPathKey (mic), load);
    });
    // The IRs bundled with the app (inside the app, where a file chooser can't easily reach), a submenu
    // per cab as on the cab page's list.
    juce::StringArray groups;
    std::vector<juce::PopupMenu> groupMenus;
    for (const auto& e : cabView->getEntries())
    {
        if (! e.builtIn || e.pack)
            continue;
        auto index = groups.indexOf (e.group);
        if (index < 0)
        {
            groups.add (e.group);
            groupMenus.emplace_back();
            index = groups.size() - 1;
        }
        const auto file = e.file;
        groupMenus[(size_t) index].addItem (e.name, [load, file] { load (file); });
    }
    if (! groups.isEmpty())
    {
        juce::PopupMenu builtIn;
        for (int i = 0; i < groups.size(); ++i)
            builtIn.addSubMenu (groups[i].startsWith ("Built in, ") ? groups[i].substring (10) : groups[i], groupMenus[(size_t) i]);
        menu.addSubMenu ("Built-in IRs", builtIn);
    }
    if (mic != AmpSimProcessor::roomMic)
        menu.addItem ("Load a cab pack folder (makes the mic movable)...", [safe, mic, load]
        {
            if (safe != nullptr)
                safe->chooseFile ("Choose a cab pack folder", {}, AmpSimProcessor::cabPathKey (mic), load, true);
        });
    const auto loaded = ampSim.parameters.state.getProperty (AmpSimProcessor::cabPathKey (mic)).toString().isNotEmpty();
    menu.addItem ("Clear", loaded, false, [safe, mic]
    {
        if (safe != nullptr)
            safe->ampSim.clearCabIR (mic);
    });
    return menu;
}

// ---- A/B and scenes ---------------------------------------------------------------------------------

void AmpSimEditor::abSelect (bool b)
{
    if (b != ampSim.isOnB())
    {
        ampSim.parameters.copyState();
        ampSim.abSwitch();
        refreshState();
    }
}

void AmpSimEditor::clickScene (int index)
{
    // Click to recall a scene, or to store the current sound in an empty one; Store, then a scene, overwrites.
    auto& bar = getScenesBar();
    auto& scenes = ampSim.getScenes();
    if (bar.isStoreArmed() || ! scenes.get (index).stored)
    {
        ampSim.storeScene (index);
        bar.setStoreArmed (false);
    }
    else
    {
        ampSim.recallScene (index);
    }
    refreshState();
}

void AmpSimEditor::sceneMenu (int index)
{
    ui::showMenu (sceneMenuFor (index), &getScenesBar().getTile (index), &lookAndFeel);
}

juce::PopupMenu AmpSimEditor::sceneMenuFor (int index)
{
    juce::PopupMenu menu;
    const auto& scene = ampSim.getScenes().get (index);
    menu.addSectionHeader (scene.stored ? scene.name : "Scene " + juce::String (index + 1) + " (empty)");
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    const auto changed = [] (AmpSimEditor& e)
    {
        e.refreshState();
    };
    menu.addItem ("Store the current sound here", [safe, index, changed]
    {
        if (safe != nullptr)
        {
            safe->ampSim.storeScene (index);
            changed (*safe);
        }
    });
    menu.addItem ("Rename...", scene.stored, false, [safe, index, changed]
    {
        if (safe == nullptr)
            return;
        auto* window = new juce::AlertWindow ("Rename scene " + juce::String (index + 1), {}, juce::MessageBoxIconType::NoIcon, safe.getComponent());
        window->setLookAndFeel (&safe->lookAndFeel); // the editor's destructor detaches it if still open
        safe->renameWindow = window;
        window->addTextEditor ("name", safe->ampSim.getScenes().get (index).name);
        window->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        ui::setPrimary (*window->getButton (0));
        window->setOpaque (false); // its LookAndFeel draws it as a rounded panel (ui::LookAndFeel::drawAlertBox)
        window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, index, window, changed] (int result)
        {
            if (result == 1 && safe != nullptr)
                if (const auto name = window->getTextEditorContents ("name").trim(); name.isNotEmpty())
                {
                    safe->ampSim.getScenes().rename (index, name);
                    changed (*safe);
                }
        }), true);
    });
    menu.addItem ("Clear", scene.stored, false, [safe, index, changed]
    {
        if (safe != nullptr)
        {
            safe->ampSim.getScenes().clear (index);
            changed (*safe);
        }
    });
    return menu;
}

// ---- Undo, keys, and right-clicks -------------------------------------------------------------------

void AmpSimEditor::undo()
{
    ampSim.parameters.copyState();
    ampSim.undoManager.undo();
    refreshState();
}

void AmpSimEditor::redo()
{
    ampSim.undoManager.redo();
    refreshState();
}

bool AmpSimEditor::keyPressed (const juce::KeyPress& key)
{
    // Undo and redo: Cmd-Z and Shift-Cmd-Z (UH6), and the top bar's buttons.
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        ampSim.parameters.copyState();
        const auto undone = ampSim.undoManager.undo();
        refreshState();
        return undone;
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        const auto redone = ampSim.undoManager.redo();
        refreshState();
        return redone;
    }

    // Escape closes an overlay.
    if (key == juce::KeyPress::escapeKey)
    {
        if (overlay == ui::Overlay::none)
            return false;
        closeOverlay();
        return true;
    }

    // The tone match page's comparison keys (Space, 1, 2, 3) come first while it's open.
    if (overlay == ui::Overlay::toneMatch && toneMatchPage->keyPressed (key))
        return true;

    // The rest are plain keys (with no modifier but Shift on the letter).
    const auto mods = key.getModifiers().withoutMouseButtons();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    // The character typed (or, without one, the key's code).
    const auto typed = key.getTextCharacter() != 0 ? key.getTextCharacter() : (juce::juce_wchar) key.getKeyCode();

    // T: the tuner, open or closed.
    if (typed == 't' || typed == 'T')
    {
        overlay == ui::Overlay::tuner ? closeOverlay() : openOverlay (ui::Overlay::tuner);
        return true;
    }

    // [ and ]: the board's previous and next tab (wrapping round).
    if (typed == '[' || typed == ']')
    {
        board->stepTab (typed == '[' ? -1 : 1);
        return true;
    }

    // The left and right arrows: the previous and next preset (a knob with the keyboard's focus keeps them).
    if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)
    {
        stepPreset (key.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }

    // 1 to 8: recall a scene (an empty one is left alone: storing is a click, or Store).
    if (typed >= '1' && typed <= '8')
    {
        const auto index = (int) (typed - '1');
        if (ampSim.getScenes().get (index).stored)
        {
            ampSim.recallScene (index);
            refreshState();
        }
        return true;
    }
    return false;
}

void AmpSimEditor::mouseDown (const juce::MouseEvent& e)
{
    // Every press starts a new undo step: whatever this gesture changes undoes together. (The tree catches
    // up with the last gesture's values first, so they land in the step they belong to.)
    if (! e.mods.isPopupMenu())
    {
        ampSim.parameters.copyState();
        ampSim.undoManager.beginNewTransaction();
        return;
    }

    // The scene or control under the click, or the nearest parent that is one (a knob's typing box, a
    // combo box's label, a chain block).
    for (auto* c = e.eventComponent; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (const auto scene = c->getProperties()[ui::ScenesBar::sceneIndexProperty]; ! scene.isVoid())
        {
            sceneMenu ((int) scene);
            return;
        }
        if (const auto id = c->getProperties()[ui::parameterIdProperty].toString(); id.isNotEmpty())
        {
            ui::showMenu (midiMenuFor (id), c, &lookAndFeel, true);
            return;
        }
    }
}

juce::PopupMenu AmpSimEditor::midiMenuFor (const juce::String& parameterId)
{
    juce::PopupMenu menu;
    auto* parameter = ampSim.parameters.getParameter (parameterId);
    if (parameter == nullptr)
        return menu;

    // Menu actions run later, on the message thread; the editor may be gone by then.
    const auto map = [safe = juce::Component::SafePointer<AmpSimEditor> (this)] () -> MidiMap*
    { return safe != nullptr ? &safe->ampSim.getMidiMap() : nullptr; };
    const auto learning = ampSim.getMidiMap().isLearning() && ampSim.getMidiMap().getLearnTarget() == parameterId;
    const auto isSwitch = dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr;

    menu.addSectionHeader (parameter->getName (64));
    menu.addItem (learning ? "Cancel MIDI learn" : "MIDI learn: then press a footswitch or move a pedal", [map, parameterId, learning]
    {
        if (auto* m = map())
        {
            if (learning)
                m->cancelLearn();
            else
                m->startLearn (parameterId);
        }
    });

    // Scenes always hold the amp slot and every switch; any other parameter can be added.
    if (! Scenes::isSwitch (parameterId) && ! presets::isGlobal (parameterId))
    {
        const auto held = ampSim.getScenes().isChosen (parameterId);
        menu.addSeparator();
        menu.addItem ("Held by scenes", true, held, [safe = juce::Component::SafePointer<AmpSimEditor> (this), parameterId, held]
        {
            if (safe != nullptr)
                safe->ampSim.getScenes().setChosen (parameterId, ! held);
        });
    }

    for (const auto& mapping : ampSim.getMidiMap().getMappings())
    {
        if (mapping.parameterId != parameterId)
            continue;

        const auto cc = mapping.cc;
        menu.addSeparator();
        menu.addSectionHeader ("CC " + juce::String (cc) + ": " + MidiMapping::actionName (mapping.action));
        if (isSwitch)
        {
            // Toggle suits switches that leave the state to the app; momentary follows the switch, which
            // also suits a latching controller that alternates 127 and 0 itself.
            auto changed = mapping;
            const auto toToggle = mapping.action != MidiMapping::Action::toggle;
            changed.action = toToggle ? MidiMapping::Action::toggle : MidiMapping::Action::momentary;
            menu.addItem (toToggle ? "Flip on each press (toggle)" : "Follow the switch (momentary, or a latching footswitch)", [map, changed]
            {
                if (auto* m = map())
                    m->set (changed);
            });
        }
        menu.addItem ("Forget CC " + juce::String (cc), [map, cc]
        {
            if (auto* m = map())
                m->remove (cc);
        });
    }
    return menu;
}

void AmpSimEditor::showMidiMappings()
{
    juce::PopupMenu menu;
    const auto map = [safe = juce::Component::SafePointer<AmpSimEditor> (this)] () -> MidiMap*
    { return safe != nullptr ? &safe->ampSim.getMidiMap() : nullptr; };
    const auto& mappings = ampSim.getMidiMap().getMappings();

    if (mappings.empty())
        menu.addItem ("No mappings yet: right-click a control to learn one", false, false, nullptr);

    for (const auto& mapping : mappings)
    {
        const auto cc = mapping.cc;
        auto* parameter = ampSim.parameters.getParameter (mapping.parameterId);
        juce::PopupMenu forget;
        forget.addItem ("Forget", [map, cc]
        {
            if (auto* m = map())
                m->remove (cc);
        });
        menu.addSubMenu ("CC " + juce::String (cc) + ": " + (parameter != nullptr ? parameter->getName (64) : mapping.parameterId) + " ("
                             + MidiMapping::actionName (mapping.action) + ")",
                         forget);
    }

    if (! mappings.empty())
    {
        menu.addSeparator();
        menu.addItem ("Forget all", [map]
        {
            if (auto* m = map())
                m->clear();
        });
    }

    menu.addSeparator();
    const auto value = [this] (const char* id) { return juce::String (juce::roundToInt (ampSim.parameters.getRawParameterValue (id)->load())); };
    menu.addSectionHeader ("Built in: program change 1 to 9 picks the amp, CC " + value ("midi_tap_cc") + " taps the tempo, CC "
                           + value ("midi_freeze_cc") + " freezes the reverb, CC " + value ("midi_scene_cc") + " picks a scene");
    ui::showMenu (menu, &getOutputPage().getMidiButton(), &lookAndFeel);
}

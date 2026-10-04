// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PluginEditor.h"
#include "BlockParameters.h"
#include "platform/AppInfo.h"
#include "platform/Updater.h"

using namespace ui::theme;

namespace
{
const juce::Identifier uiPageKey { "uiPage" };            // the page last shown (app state)
const juce::Identifier presetSourceKey = AmpSimProcessor::presetSourceKey; // "factory" or "user": the top bar's tag

/// The main area inside the canvas (handoff 2: between the top bar and the chain, padded 18 / 40 / 16).
juce::Rectangle<int> mainArea()
{
    return { mainPadSide, topBarHeight + mainPadTop, canvasWidth - 2 * mainPadSide, canvasHeight - topBarHeight - chainHeight - mainPadTop - mainPadBottom };
}

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
} // namespace

/// The 1280 x 760 canvas: the window's background with its 12 px corners and 1 px line border (handoff 2).
class AmpSimEditor::Canvas final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        g.setColour (bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), radiusWindow);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (line1);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), radiusWindow, 1.0f);
    }
};

/// The message line at the bottom-left of the main area: 12 px ink-dim, one line, the whole text in its
/// tooltip.
class AmpSimEditor::StatusLine final : public juce::Component, public juce::SettableTooltipClient
{
public:
    void set (const juce::String& newMessage)
    {
        if (newMessage != message)
        {
            message = newMessage;
            setTooltip (message);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkDim);
        g.drawFittedText (message, getLocalBounds(), juce::Justification::centredLeft, 1, 0.9f);
    }

private:
    juce::String message;
};

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p) : AudioProcessorEditor (&p), ampSim (p)
{
    setLookAndFeel (&lookAndFeel);
    canvas = std::make_unique<Canvas>();
    addAndMakeVisible (*canvas);
    tooltips = std::make_unique<juce::TooltipWindow> (canvas.get(), 600);
    statusLine = std::make_unique<StatusLine>();

    // The effect blocks' editors, all made now so switching is instant.
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

    // The main pages.
    const auto pageOf = [this] (B id) -> ui::BlockPage& { return *blockPageFor[(size_t) id]; };
    prePage = std::make_unique<ui::SectionPage> (p, ui::Section::pre, pageOf);
    postPage = std::make_unique<ui::SectionPage> (p, ui::Section::post, pageOf);
    eqView = std::make_unique<ui::EqView> (p, *blockPageFor[(size_t) B::postEq], *blockPageFor[(size_t) B::preEq]);
    inputPage = std::make_unique<ui::InputPage> (p);
    outputPage = std::make_unique<ui::OutputPage> (p);
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

    tunerPage = std::make_unique<ui::TunerPage> (p);

    using P = ui::PageId;
    pageComponents[(size_t) P::input] = inputPage.get();
    pageComponents[(size_t) P::preFx] = prePage.get();
    pageComponents[(size_t) P::amp] = ampView.get();
    pageComponents[(size_t) P::eq] = eqView.get();
    pageComponents[(size_t) P::cab] = cabView.get();
    pageComponents[(size_t) P::postFx] = postPage.get();
    pageComponents[(size_t) P::output] = outputPage.get();
    pageComponents[(size_t) P::tuner] = tunerPage.get();
    for (auto* page : pageComponents)
        canvas->addChildComponent (page);

    canvas->addAndMakeVisible (topBar);
    canvas->addAndMakeVisible (chainNav);
    canvas->addAndMakeVisible (*statusLine);

    chainNav.onNavigate = [this] (P page) { showPage (page); };
    topBar.onPresetMenu = [this] { showPresetMenu(); };
    topBar.onSave = [this] { savePreset(); };
    topBar.onPrevious = [this] { stepPreset (-1); };
    topBar.onNext = [this] { stepPreset (1); };
    topBar.onTuner = [this] { showPage (shownPage == P::tuner ? pageBeforeTuner : P::tuner); };
    topBar.onBrand = [this] { ui::showMenu (brandMenu(), &topBar.getBrandButton(), &lookAndFeel); };
    outputPage->onAbSelect = [this] (bool b) { abSelect (b); };
    outputPage->onAbCopy = [this] { ampSim.abCopyToOther(); };
    outputPage->onTap = [this] { ampSim.tapTempo(); };
    outputPage->onMidiMappings = [this] { showMidiMappings(); };
    outputPage->getScenesBar().onClick = [this] (int i) { clickScene (i); };

    addMouseListener (this, true); // right-clicks anywhere, for MIDI learn; every press starts an undo step
    setWantsKeyboardFocus (true);

    // The app's view state: the page last shown, and the window's size (UH11; the old UI scale key is
    // left alone and no longer used).
    const auto& state = ampSim.parameters.state;
    auto first = P::amp;
    for (int i = 0; i < ui::numPages; ++i)
        if ((P) i != P::tuner && state.getProperty (uiPageKey).toString() == ui::pageName ((P) i))
            first = (P) i;
    showPage (first);

    // (Read before setting the limits: the limits resize the editor, which would write its size.)
    const auto width = juce::jmax (minimumWidth, (int) state.getProperty (AmpSimProcessor::uiWidthKey, canvasWidth));
    const auto height = juce::jmax (minimumHeight, (int) state.getProperty (AmpSimProcessor::uiHeightKey, canvasHeight));
    setResizable (true, false);
    setResizeLimits (minimumWidth, minimumHeight, 8192, 8192);
    setSize (width, height);

    refreshState();
    startTimerHz (meterFps);

    // Automatic updates, in the standalone app only (a plugin's updates belong to whoever installed it, and
    // the tests' processors are never "Standalone"). Message thread; the updater never touches audio.
    if (ampSim.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        platform::updater::start();
}

AmpSimEditor::~AmpSimEditor()
{
    stopTimer();
    if (auto* hooks = hooksFor (shownPage))
        hooks->pageHidden();
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
    chainNav.setBounds (0, canvasHeight - chainHeight, canvasWidth, chainHeight);
    for (auto* page : pageComponents)
        page->setBounds (mainArea());
    statusLine->setBounds (mainArea().getX(), mainArea().getBottom(), mainArea().getWidth(), mainPadBottom);
}

// ---- Pages ------------------------------------------------------------------------------------------

ui::ControlGroup* AmpSimEditor::hooksFor (ui::PageId page)
{
    if (page == ui::PageId::count)
        return nullptr;
    return dynamic_cast<ui::ControlGroup*> (pageComponents[(size_t) page]);
}

void AmpSimEditor::showPage (ui::PageId page)
{
    if (page == ui::PageId::count || page == shownPage)
        return;

    // Leaving the tuner disengages it; opening it engages it (the tuner never runs behind another page).
    auto* tunerOn = ampSim.parameters.getParameter ("tuner_on");
    if (shownPage == ui::PageId::tuner && page != ui::PageId::tuner && tunerOn->getValue() >= 0.5f)
        tunerOn->setValueNotifyingHost (0.0f);
    if (page == ui::PageId::tuner)
    {
        if (shownPage != ui::PageId::count)
            pageBeforeTuner = shownPage;
        if (tunerOn->getValue() < 0.5f)
            tunerOn->setValueNotifyingHost (1.0f);
    }

    if (shownPage != ui::PageId::count)
    {
        pageComponents[(size_t) shownPage]->setVisible (false);
        if (auto* hooks = hooksFor (shownPage))
            hooks->pageHidden();
    }
    shownPage = page;
    pageComponents[(size_t) page]->setVisible (true);
    if (auto* hooks = hooksFor (page))
    {
        hooks->pageShown();
        hooks->refresh();
    }
    chainNav.setActive (page);
    if (page != ui::PageId::tuner)
        ampSim.parameters.state.setProperty (uiPageKey, ui::pageName (page), nullptr);
    refreshState();
}

void AmpSimEditor::selectBlock (ui::BlockId id)
{
    if (id == ui::BlockId::count)
        return;
    selectedBlock = id;
    const auto page = ui::pageFor (id);
    if (page == ui::PageId::preFx || page == ui::PageId::postFx)
        getSectionPage (page == ui::PageId::preFx ? ui::Section::pre : ui::Section::post).select (id);
    showPage (page);
}

// ---- The timer --------------------------------------------------------------------------------------

void AmpSimEditor::timerCallback()
{
    // The meters every frame (30 a second); everything else ten times a second.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = lastMeterMs > 0.0 ? juce::jlimit (0.0, 0.5, (now - lastMeterMs) / 1000.0) : 1.0 / meterFps;
    lastMeterMs = now;
    topBar.updateMeters (ampSim.takePeaks(), seconds);
    outputPage->getCpuMeter().setLoad (ampSim.getCpuLoad());

    if (++ticks % 3 == 0)
        refreshState();
}

void AmpSimEditor::refreshState()
{
    topBar.refresh();
    if (auto* hooks = hooksFor (shownPage))
        hooks->refresh();

    // The tuner follows its switch both ways: a footswitch that engages it opens its page, and one that
    // disengages it (or the page's own close button) goes back to the page before.
    const auto tuning = ampSim.parameters.getRawParameterValue ("tuner_on")->load() >= 0.5f;
    if (tuning && shownPage != ui::PageId::tuner)
        showPage (ui::PageId::tuner);
    else if (! tuning && shownPage == ui::PageId::tuner)
        showPage (pageBeforeTuner);

    // The tuner button's note: the one it hears, only while it's engaged.
    juce::String note ("-");
    if (const auto reading = ampSim.getTunerReading(); tuning && ampSim.isTunerEngaged() && reading.hasReading)
        note = juce::MidiMessage::getMidiNoteName (reading.midiNote, true, false, 4);
    topBar.setTuner (shownPage == ui::PageId::tuner, note);

    // The message: the sample rate first, then MIDI learn waiting for a controller, then a preset that
    // couldn't load or loaded with gaps (or a factory preset's notes).
    juce::String message;
    if (! ampSim.isSampleRateOk())
        message = rateText (ampSim.getDeviceSampleRate()) + ": muted, set the interface to 48 kHz";
    else if (const auto& midi = ampSim.getMidiMap(); midi.isLearning())
    {
        if (auto* parameter = ampSim.parameters.getParameter (midi.getLearnTarget()))
            message = "MIDI learn: press a footswitch or move a pedal for " + parameter->getName (64) + " (right-click it again to cancel)";
    }
    if (message.isEmpty())
        message = presetMessage.isNotEmpty() ? presetMessage : ampSim.getPresetWarnings().joinIntoString ("; ");
    statusText = message;
    ampView->setStatus (message);
    statusLine->set (shownPage == ui::PageId::amp ? juce::String() : message);
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
    if (result.ok)
        ampSim.parameters.state.setProperty (presetSourceKey, "user", nullptr);
    refreshState();
}

void AmpSimEditor::loadFactoryPreset (const juce::var& preset)
{
    const auto result = ampSim.loadPreset (preset);
    presetMessage = result.ok ? preset["notes"].toString() : result.error;
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

void AmpSimEditor::loadCapture (int slot)
{
    chooseFile ("Choose a NAM capture for " + juce::String (ui::materialName (ui::materialFor (slot))) + " (amp slot " + juce::String (slot + 1) + ")", "*.nam",
                AmpSimProcessor::modelPathKey (slot), [this, slot] (const juce::File& f) { ampSim.loadModel (slot, f); });
}

void AmpSimEditor::showCaptureMenu (int slot, juce::Component& near)
{
    ui::showMenu (captureMenu (slot), &near, &lookAndFeel, true);
}

juce::PopupMenu AmpSimEditor::captureMenu (int slot)
{
    // The old slot card's functions, on a right-click at the grille or the model's name (UH12).
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    const auto path = ampSim.parameters.state.getProperty (AmpSimProcessor::modelPathKey (slot)).toString();
    const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    menu.addSectionHeader (juce::String (ui::materialName (ui::materialFor (slot))) + ", amp slot " + juce::String (slot + 1));
    menu.addItem ("Load capture...", [safe, slot] { if (safe != nullptr) safe->loadCapture (slot); });
    menu.addItem ("Reload " + (file != juce::File() ? file.getFileName() : juce::String ("the capture")), file.existsAsFile(), false, [safe, slot, file]
    {
        if (safe != nullptr)
            safe->ampSim.loadModel (slot, file);
    });
    const auto builtIn = presets::builtInCapture (slot);
    menu.addItem ("Use the built-in capture (" + presets::builtInCaptureName (slot) + ")", builtIn.existsAsFile() && file != builtIn, false, [safe, slot]
    {
        if (safe != nullptr)
            safe->ampSim.useBuiltInCapture (slot);
    });
    menu.addItem ("Clear the slot (the DI passes through)", file != juce::File(), false, [safe, slot]
    {
        if (safe != nullptr)
            safe->ampSim.clearModel (slot);
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
    auto& scenesBar = getScenesBar();
    auto& scenes = ampSim.getScenes();
    if (scenesBar.isStoreArmed() || ! scenes.get (index).stored)
    {
        ampSim.storeScene (index);
        scenesBar.setStoreArmed (false);
    }
    else
    {
        ampSim.recallScene (index);
    }
    refreshState();
    outputPage->refresh();
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
        e.outputPage->refresh();
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

bool AmpSimEditor::keyPressed (const juce::KeyPress& key)
{
    // Undo and redo have no buttons in the handoff's frame: Cmd-Z and Shift-Cmd-Z (UH6).
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
    menu.addSectionHeader ("Built in: program change 1 to 3 picks the amp, CC " + value ("midi_tap_cc") + " taps the tempo, CC "
                           + value ("midi_freeze_cc") + " freezes the reverb, CC " + value ("midi_scene_cc") + " picks a scene");
    ui::showMenu (menu, &outputPage->getMidiButton(), &lookAndFeel);
}

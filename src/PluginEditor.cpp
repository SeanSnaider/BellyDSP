#include "PluginEditor.h"
#include "BlockParameters.h"

using namespace ui::theme;

namespace
{
const juce::Identifier uiPageKey { "uiPage" }; // the page last shown (app state)

float snapScale (float scale)
{
    auto best = uiScales[1];
    for (auto s : uiScales)
        if (std::abs (s - scale) < std::abs (best - scale))
            best = s;
    return best;
}
} // namespace

/// The warning line above the scenes: the sample rate first, then MIDI learn waiting for a controller,
/// then a preset's notes or what went wrong loading it. Two lines at most; the whole text is its tooltip.
class AmpSimEditor::StatusLine final : public juce::Component, public juce::SettableTooltipClient
{
public:
    void set (const juce::String& newMessage, juce::Colour newColour)
    {
        if (newMessage != message || newColour != colour)
        {
            message = newMessage;
            colour = newColour;
            setTooltip (message);
            repaint();
        }
    }

    const juce::String& getMessage() const noexcept { return message; }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().reduced (space::l, 0);
        g.setFont (font (Text::label));
        if (message.isEmpty())
        {
            g.setColour (textDim.withAlpha (0.75f));
            g.drawText ("Right-click any control for MIDI learn and scenes.  Cmd-Z undoes.", area, juce::Justification::centredLeft, true);
            return;
        }

        // A marker in the message's colour; warnings and errors are written in it too.
        g.setColour (colour);
        g.fillRoundedRectangle (area.removeFromLeft (4).withSizeKeepingCentre (4, 18).toFloat(), 2.0f);
        area.removeFromLeft (space::s);
        g.setColour (colour == warn || colour == error ? colour : ui::theme::text);
        g.drawFittedText (message, area, juce::Justification::centredLeft, 2, 0.9f);
    }

private:
    juce::String message;
    juce::Colour colour { textDim };
};

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p) : AudioProcessorEditor (&p), ampSim (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (content);
    statusLine = std::make_unique<StatusLine>();

    // A page per block (the input and the output share one), all made now so switching is instant.
    const auto add = [this] (std::unique_ptr<ui::BlockPage> page, std::initializer_list<ui::BlockId> ids)
    {
        for (auto id : ids)
            pageFor[(size_t) id] = page.get();
        content.addChildComponent (*page);
        pages.push_back (std::move (page));
    };
    using B = ui::BlockId;
    add (std::make_unique<ui::IoPage> (p), { B::input, B::output });
    add (std::make_unique<ui::GatePage> (p, false), { B::gateA });
    add (std::make_unique<ui::CompressorPage> (p, false), { B::preCompressor });
    add (std::make_unique<ui::BoostPage> (p), { B::boost });
    add (std::make_unique<ui::OverdrivePage> (p), { B::overdrive });
    add (std::make_unique<ui::EqPage> (p, false), { B::preEq });
    add (std::make_unique<ui::AmpPage> (p, [this] (int slot)
    {
        chooseFile ("Choose a NAM capture for Amp " + juce::String (slot + 1), "*.nam", AmpSimProcessor::modelPathKey (slot),
                    [this, slot] (const juce::File& f) { ampSim.loadModel (slot, f); });
    }), { B::amp });
    add (std::make_unique<ui::GatePage> (p, true), { B::gateB });
    add (std::make_unique<ui::CabPage> (p, [this] (int mic, bool pack)
    {
        if (pack)
            chooseFile ("Choose a cab pack folder", {}, AmpSimProcessor::cabPathKey (mic), [this, mic] (const juce::File& f) { ampSim.loadCabIR (mic, f); }, true);
        else
            chooseFile ("Choose an impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::cabPathKey (mic),
                        [this, mic] (const juce::File& f) { ampSim.loadCabIR (mic, f); });
    }), { B::cab });
    add (std::make_unique<ui::EqPage> (p, true), { B::postEq });
    add (std::make_unique<ui::CompressorPage> (p, true), { B::postCompressor });
    add (std::make_unique<ui::HarmonizerPage> (p), { B::harmonizer });
    add (std::make_unique<ui::MultivoicerPage> (p), { B::multivoicer });
    add (std::make_unique<ui::BloomPage> (p), { B::bloom });
    add (std::make_unique<ui::ChorusPage> (p), { B::chorus });
    add (std::make_unique<ui::DelayPage> (p), { B::delay });
    add (std::make_unique<ui::ReverbPage> (p), { B::reverb });

    for (auto* c : std::initializer_list<juce::Component*> { &topBar, &chainStrip, statusLine.get(), &scenesBar })
        content.addAndMakeVisible (c);
    content.addChildComponent (tunerView); // over everything while the tuner is engaged

    chainStrip.onSelect = [this] (ui::BlockId id) { selectBlock (id); };
    topBar.onPresetMenu = [this] { showPresetMenu(); };
    topBar.onSave = [this] { savePreset(); };
    topBar.onUndo = [this] { undo(); };
    topBar.onRedo = [this] { redo(); };
    topBar.onAbSelect = [this] (bool b)
    {
        if (b != ampSim.isOnB())
        {
            ampSim.parameters.copyState();
            ampSim.abSwitch();
            refreshState();
        }
    };
    topBar.onAbCopy = [this] { ampSim.abCopyToOther(); };
    topBar.onMidi = [this] { showMidiMappings(); };
    topBar.onSettings = [this] { showSettingsMenu(); };
    topBar.onTap = [this] { ampSim.tapTempo(); };
    scenesBar.onClick = [this] (int i) { clickScene (i); };

    addMouseListener (this, true); // right-clicks anywhere, for MIDI learn; every press starts an undo step
    setWantsKeyboardFocus (true);

    // The app's view state: the page last shown, the UI scale, and the window's size in UI points.
    const auto& state = ampSim.parameters.state;
    auto first = B::amp;
    for (int b = 0; b < ui::numBlocks; ++b)
        if (state.getProperty (uiPageKey).toString() == ui::info ((B) b).pageName)
        {
            first = (B) b;
            break;
        }
    selectBlock (first);

    uiScale = snapScale ((float) (double) state.getProperty (AmpSimProcessor::uiScaleKey, 1.0));
    const auto width = juce::jmax (minimumWidth, (int) state.getProperty (AmpSimProcessor::uiWidthKey, windowWidth));
    const auto height = juce::jmax (minimumHeight, (int) state.getProperty (AmpSimProcessor::uiHeightKey, windowHeight));
    setResizable (true, false);
    setResizeLimits (juce::roundToInt ((float) minimumWidth * uiScale), juce::roundToInt ((float) minimumHeight * uiScale), 8192, 8192);
    setSize (juce::roundToInt ((float) width * uiScale), juce::roundToInt ((float) height * uiScale));

    refreshState();
    startTimerHz (meterFps);
}

AmpSimEditor::~AmpSimEditor()
{
    stopTimer();
    if (shownPage != nullptr)
        shownPage->pageHidden();
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

// ---- Layout and the UI scale ------------------------------------------------------------------------

void AmpSimEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void AmpSimEditor::resized()
{
    // The window is the content's size times the UI scale; the content is laid out in UI points.
    const auto width = juce::roundToInt ((float) getWidth() / uiScale), height = juce::roundToInt ((float) getHeight() / uiScale);
    content.setTransform (juce::AffineTransform::scale (uiScale));
    content.setBounds (0, 0, width, height);
    layoutContent();

    auto& state = ampSim.parameters.state;
    state.setProperty (AmpSimProcessor::uiWidthKey, width, nullptr);
    state.setProperty (AmpSimProcessor::uiHeightKey, height, nullptr);
}

void AmpSimEditor::layoutContent()
{
    auto area = content.getLocalBounds();
    tunerView.setBounds (area);
    topBar.setBounds (area.removeFromTop (topBarHeight));
    scenesBar.setBounds (area.removeFromBottom (scenesBarHeight));
    statusLine->setBounds (area.removeFromBottom (statusHeight));
    chainStrip.setBounds (area.removeFromTop (chainStripHeight));

    const auto pageArea = area.reduced (space::l, 0).withTrimmedTop (space::xs);
    for (auto& page : pages)
        page->setBounds (pageArea);
}

void AmpSimEditor::setUiScale (float scale)
{
    // Keep the size in UI points; the window follows the scale.
    const auto width = content.getWidth(), height = content.getHeight();
    uiScale = snapScale (scale);
    ampSim.parameters.state.setProperty (AmpSimProcessor::uiScaleKey, uiScale, nullptr);
    setResizeLimits (juce::roundToInt ((float) minimumWidth * uiScale), juce::roundToInt ((float) minimumHeight * uiScale), 8192, 8192);
    setSize (juce::roundToInt ((float) width * uiScale), juce::roundToInt ((float) height * uiScale));
    resized();
}

// ---- Pages ------------------------------------------------------------------------------------------

void AmpSimEditor::selectBlock (ui::BlockId id)
{
    if (id == ui::BlockId::count)
        return;
    selected = id;
    chainStrip.setSelected (id);
    showPage (pageFor[(size_t) id]);
    ampSim.parameters.state.setProperty (uiPageKey, ui::info (id).pageName, nullptr);
}

void AmpSimEditor::showPage (ui::BlockPage* page)
{
    if (page == shownPage)
        return;
    if (shownPage != nullptr)
    {
        shownPage->setVisible (false);
        shownPage->pageHidden();
    }
    shownPage = page;
    if (shownPage != nullptr)
    {
        shownPage->setVisible (true);
        shownPage->pageShown();
        shownPage->refresh();
    }
}

// ---- The timer --------------------------------------------------------------------------------------

void AmpSimEditor::timerCallback()
{
    // The meters every frame (30 a second); everything else ten times a second.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = lastMeterMs > 0.0 ? juce::jlimit (0.0, 0.5, (now - lastMeterMs) / 1000.0) : 1.0 / meterFps;
    lastMeterMs = now;
    topBar.updateMeters (ampSim.takePeaks(), ampSim.getCpuLoad(), seconds);

    if (++ticks % 3 == 0)
        refreshState();
}

void AmpSimEditor::refreshState()
{
    const auto status = ampSim.getStatus();
    topBar.refresh();
    chainStrip.refresh (status);
    scenesBar.refresh (ampSim.getScenes());
    if (shownPage != nullptr)
        shownPage->refresh();

    // The warning line: the sample rate first, then MIDI learn waiting for a controller, then a preset
    // that couldn't load or loaded with gaps (or a factory preset's notes).
    juce::String message;
    auto colour = warn;
    if (status.warning.isNotEmpty())
    {
        message = status.warning;
        colour = error;
    }
    else if (const auto& midi = ampSim.getMidiMap(); midi.isLearning())
    {
        if (auto* parameter = ampSim.parameters.getParameter (midi.getLearnTarget()))
        {
            message = "MIDI learn: press a footswitch or move a pedal for " + parameter->getName (64) + " (right-click it again to cancel)";
            colour = accent;
        }
    }
    if (message.isEmpty() && presetMessage.isNotEmpty())
    {
        message = presetMessage;
        colour = presetMessageIsError ? error : textDim;
    }
    else if (message.isEmpty())
    {
        message = ampSim.getPresetWarnings().joinIntoString ("; ");
    }
    statusLine->set (message, colour);

    // The tuner covers everything while it's engaged.
    if (const auto tuning = ampSim.parameters.getRawParameterValue ("tuner_on")->load() >= 0.5f; tunerView.isVisible() != tuning)
    {
        tunerView.setVisible (tuning);
        if (tuning)
            tunerView.toFront (false);
    }
}

juce::String AmpSimEditor::getStatusText() const
{
    return statusLine->getMessage();
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
                              presetMessageIsError = ! saved;
                              ampSim.parameters.state.setProperty ("presetName", file.getFileNameWithoutExtension(), nullptr);
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
    presetMessageIsError = ! result.ok;
    refreshState();
}

void AmpSimEditor::showPresetMenu()
{
    // The browser: the factory presets, the preset folder (its subfolders as submenus), open, save as.
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    menu.addSectionHeader ("Factory presets (load your own captures and IRs)");
    for (const auto& preset : presets::factoryPresets())
        menu.addItem (preset["name"].toString(), [safe, preset]
        {
            if (safe == nullptr)
                return;
            const auto result = safe->ampSim.loadPreset (preset);
            safe->presetMessage = result.ok ? preset["notes"].toString() : result.error;
            safe->presetMessageIsError = ! result.ok;
            safe->refreshState();
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
    ui::showMenu (menu, &topBar.getPresetButton(), &lookAndFeel);
}

void AmpSimEditor::showSettingsMenu()
{
    juce::PopupMenu menu;
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    menu.addSectionHeader ("UI scale");
    for (auto s : uiScales)
        menu.addItem (juce::String (juce::roundToInt (s * 100.0f)) + "%", true, std::abs (s - uiScale) < 0.01f, [safe, s]
        {
            if (safe != nullptr)
                safe->setUiScale (s);
        });
    ui::showMenu (menu, &topBar.getSettingsButton(), &lookAndFeel);
}

// ---- Scenes -----------------------------------------------------------------------------------------

void AmpSimEditor::clickScene (int index)
{
    // Click to recall a scene, or to store the current sound in an empty one; Store, then a scene, overwrites.
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
}

void AmpSimEditor::sceneMenu (int index)
{
    ui::showMenu (sceneMenuFor (index), &scenesBar.getTile (index), &lookAndFeel);
}

juce::PopupMenu AmpSimEditor::sceneMenuFor (int index)
{
    juce::PopupMenu menu;
    const auto& scene = ampSim.getScenes().get (index);
    menu.addSectionHeader (scene.stored ? scene.name : "Scene " + juce::String (index + 1) + " (empty)");
    const auto safe = juce::Component::SafePointer<AmpSimEditor> (this);
    menu.addItem ("Store the current sound here", [safe, index]
    {
        if (safe != nullptr)
        {
            safe->ampSim.storeScene (index);
            safe->refreshState();
        }
    });
    menu.addItem ("Rename...", scene.stored, false, [safe, index]
    {
        if (safe == nullptr)
            return;
        auto* window = new juce::AlertWindow ("Rename scene " + juce::String (index + 1), {}, juce::MessageBoxIconType::NoIcon, safe.getComponent());
        window->setLookAndFeel (&safe->lookAndFeel); // the editor's destructor detaches it if still open
        safe->renameWindow = window;
        window->addTextEditor ("name", safe->ampSim.getScenes().get (index).name);
        window->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, index, window] (int result)
        {
            if (result == 1 && safe != nullptr)
                if (const auto name = window->getTextEditorContents ("name").trim(); name.isNotEmpty())
                {
                    safe->ampSim.getScenes().rename (index, name);
                    safe->refreshState();
                }
        }), true);
    });
    menu.addItem ("Clear", scene.stored, false, [safe, index]
    {
        if (safe != nullptr)
        {
            safe->ampSim.getScenes().clear (index);
            safe->refreshState();
        }
    });
    return menu;
}

// ---- Undo, keys, and right-clicks -------------------------------------------------------------------

void AmpSimEditor::undo()
{
    ampSim.parameters.copyState(); // the tree catches up with the last gesture first
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
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        ampSim.parameters.copyState();
        return ampSim.undoManager.undo();
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
        return ampSim.undoManager.redo();
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
    // combo box's label, a chain block's card).
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
    ui::showMenu (menu, &topBar.getMidiButton(), &lookAndFeel);
}

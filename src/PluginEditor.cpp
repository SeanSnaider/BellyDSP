#include "PluginEditor.h"

namespace
{
const auto background = juce::Colour (0xff17191d);
const auto panel = juce::Colour (0xff22252b);
const auto accent = juce::Colour (0xff3fa7d6);
const auto textColour = juce::Colour (0xffe6e6e6);
const auto dimText = juce::Colour (0xff9aa0a8);
const auto errorColour = juce::Colour (0xffff6b5e);
const auto warningColour = juce::Colour (0xffffc04d);

/// A combo box must have its items before a parameter attachment is made for it.
juce::ComboBox& withItems (juce::ComboBox& box, const juce::StringArray& items)
{
    box.addItemList (items, 1);
    return box;
}

void stylePanelTitle (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, textColour);
}

void styleStatus (juce::Label& label)
{
    label.setColour (juce::Label::textColourId, textColour);
    label.setJustificationType (juce::Justification::topLeft);
    label.setMinimumHorizontalScale (0.8f);
}
} // namespace

// ---- Knob -------------------------------------------------------------------------------------

Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption,
            const juce::String& suffix)
    : attachment (state, parameterId, slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
    slider.setTextValueSuffix (suffix);

    if (auto* param = state.getParameter (parameterId))
        slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

    addAndMakeVisible (slider);

    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setColour (juce::Label::textColourId, dimText);
    addAndMakeVisible (label);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    label.setBounds (area.removeFromTop (18));
    slider.setBounds (area);
}

// ---- SlotPanel --------------------------------------------------------------------------------

SlotPanel::SlotPanel (AmpSimProcessor& processor, int slotIndex, std::function<void()> onLoad)
    : slot (slotIndex)
{
    stylePanelTitle (title, "Amp " + juce::String (slot + 1));
    addAndMakeVisible (title);
    styleStatus (status);
    addAndMakeVisible (status);

    loadButton.onClick = std::move (onLoad);
    addAndMakeVisible (loadButton);

    auto& state = processor.parameters;
    knobs.add (new Knob (state, AmpSimProcessor::ampParamId (slot, "input_trim"), "Input"));
    for (const auto& band : ampsim::AmpTone::bands)
        knobs.add (new Knob (state, AmpSimProcessor::ampParamId (slot, juce::String (band.name).toLowerCase()), band.name));
    knobs.add (new Knob (state, AmpSimProcessor::ampParamId (slot, "output_trim"), "Output"));

    for (auto* knob : knobs)
        addAndMakeVisible (knob);
}

void SlotPanel::setActive (bool isActive)
{
    if (active != isActive)
    {
        active = isActive;
        repaint();
    }
}

void SlotPanel::setStatus (const juce::String& text, bool isError)
{
    status.setText (text == "Empty" ? juce::String ("Empty: the clean DI passes through") : text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, isError ? errorColour : textColour);
}

void SlotPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    if (active)
    {
        g.setColour (accent);
        g.drawRoundedRectangle (bounds, 6.0f, 2.0f);
    }
}

void SlotPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    title.setBounds (top.removeFromLeft (80));
    loadButton.setBounds (top.removeFromRight (120));
    area.removeFromTop (4);
    status.setBounds (area.removeFromTop (38));
    area.removeFromTop (6);

    // Knobs: Input, the five tone bands, Output, in two rows of four and three.
    const auto rowHeight = (area.getHeight() - 12) / 2;
    auto row1 = area.removeFromTop (rowHeight);
    area.removeFromTop (12);
    auto row2 = area;
    const auto width = row1.getWidth() / 4;

    for (int i = 0; i < knobs.size(); ++i)
    {
        auto& row = i < 4 ? row1 : row2;
        knobs[i]->setBounds (row.removeFromLeft (width));
    }
}

// ---- MicPositionPad ---------------------------------------------------------------------------

MicPositionPad::MicPositionPad (juce::AudioProcessorValueTreeState& state, const juce::String& xParameterId,
                                const juce::String& yParameterId)
    : xParameter (state.getParameter (xParameterId)), yParameter (state.getParameter (yParameterId))
{
    jassert (xParameter != nullptr && yParameter != nullptr);
}

void MicPositionPad::setPoints (std::vector<juce::Point<float>> packPoints)
{
    if (packPoints != points)
    {
        points = std::move (packPoints);
        setMouseCursor (points.empty() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}

void MicPositionPad::refresh()
{
    const juce::Point<float> position { xParameter->convertFrom0to1 (xParameter->getValue()),
                                        yParameter->convertFrom0to1 (yParameter->getValue()) };
    if (position != drawnPosition)
        repaint();
}

juce::Rectangle<float> MicPositionPad::mapArea() const
{
    // Room for the axis captions: "cap" / "edge" below, "close" / "far" on the left.
    return getLocalBounds().toFloat().withTrimmedLeft (34.0f).withTrimmedBottom (16.0f).reduced (6.0f);
}

void MicPositionPad::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto map = mapArea();
    g.setColour (background.brighter (0.05f));
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (dimText.withAlpha (0.25f));
    for (int i = 0; i <= 4; ++i)
    {
        const auto x = map.getX() + map.getWidth() * (float) i / 4.0f;
        const auto y = map.getY() + map.getHeight() * (float) i / 4.0f;
        g.drawVerticalLine (juce::roundToInt (x), map.getY(), map.getBottom());
        g.drawHorizontalLine (juce::roundToInt (y), map.getX(), map.getRight());
    }

    g.setColour (dimText);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("cap", juce::Rectangle<float> (map.getX() - 10.0f, map.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
    g.drawText ("edge", juce::Rectangle<float> (map.getRight() - 36.0f, map.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centredRight);
    g.drawText ("close", juce::Rectangle<float> (2.0f, map.getY() - 4.0f, 34.0f, 14.0f), juce::Justification::centredLeft);
    g.drawText ("far", juce::Rectangle<float> (2.0f, map.getBottom() - 10.0f, 34.0f, 14.0f), juce::Justification::centredLeft);

    drawnPosition = { xParameter->convertFrom0to1 (xParameter->getValue()), yParameter->convertFrom0to1 (yParameter->getValue()) };

    if (points.empty())
    {
        g.drawFittedText ("Load a cab pack (a folder of IRs)\nto move this mic", map.toNearestInt(), juce::Justification::centred, 2);
        return;
    }

    const auto toScreen = [map] (juce::Point<float> p) { return juce::Point<float> (map.getX() + p.x * map.getWidth(), map.getY() + p.y * map.getHeight()); };

    g.setColour (textColour.withAlpha (0.7f));
    for (const auto& p : points)
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (toScreen (p)));

    const auto mic = toScreen (drawnPosition);
    g.setColour (accent);
    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (mic));
    g.setColour (textColour);
    g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (mic), 1.5f);
}

void MicPositionPad::moveTo (juce::Point<float> position)
{
    const auto map = mapArea();
    const auto x = juce::jlimit (0.0f, 1.0f, (position.x - map.getX()) / map.getWidth());
    const auto y = juce::jlimit (0.0f, 1.0f, (position.y - map.getY()) / map.getHeight());
    xParameter->setValueNotifyingHost (xParameter->convertTo0to1 (x));
    yParameter->setValueNotifyingHost (yParameter->convertTo0to1 (y));
    repaint();
}

void MicPositionPad::mouseDown (const juce::MouseEvent& e)
{
    if (points.empty())
        return;

    dragging = true;
    xParameter->beginChangeGesture();
    yParameter->beginChangeGesture();
    moveTo (e.position);
}

void MicPositionPad::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        moveTo (e.position);
}

void MicPositionPad::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    xParameter->endChangeGesture();
    yParameter->endChangeGesture();
}

// ---- MicPanel ---------------------------------------------------------------------------------

MicPanel::MicPanel (AmpSimProcessor& processor, int micIndex, std::function<void()> onLoad, std::function<void()> onLoadPack)
    : mic (micIndex)
{
    const bool isRoom = mic == AmpSimProcessor::roomMic;
    stylePanelTitle (title, isRoom ? juce::String ("Room mic") : "Close mic " + juce::String (mic + 1));
    addAndMakeVisible (title);
    styleStatus (status);
    addAndMakeVisible (status);

    loadButton.onClick = std::move (onLoad);
    addAndMakeVisible (loadButton);

    if (! isRoom)
    {
        packButton.onClick = std::move (onLoadPack);
        packButton.setTooltip ("A folder of IRs of one cab at different mic positions, which makes this mic movable");
        addAndMakeVisible (packButton);
        pad = std::make_unique<MicPositionPad> (processor.parameters, AmpSimProcessor::cabParamId (mic, "pos_x"),
                                                AmpSimProcessor::cabParamId (mic, "pos_y"));
        addAndMakeVisible (*pad);
    }

    auto& state = processor.parameters;
    knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "level"), "Level"));

    if (isRoom)
    {
        knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "predelay"), "Pre-delay", " ms"));
    }
    else
    {
        knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "pan"), "Pan", ""));
        knobs.add (new Knob (state, AmpSimProcessor::cabParamId (mic, "delay"), "Delay", " smp"));

        auto* invert = toggles.add (new juce::ToggleButton ("Invert"));
        toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, AmpSimProcessor::cabParamId (mic, "invert"), *invert));

        withItems (channel, { "Left channel", "Right channel" });
        channelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, AmpSimProcessor::cabParamId (mic, "channel"), channel);
        addAndMakeVisible (channel);
    }

    auto* mute = toggles.add (new juce::ToggleButton ("Mute"));
    toggleAttachments.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (state, AmpSimProcessor::cabParamId (mic, "mute"), *mute));

    for (auto* knob : knobs)
        addAndMakeVisible (knob);
    for (auto* toggle : toggles)
        addAndMakeVisible (toggle);
}

void MicPanel::setStatus (const juce::String& text, bool isError)
{
    status.setText (text == "No IR" ? juce::String ("No IR: this mic is silent") : text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, isError ? errorColour : textColour);
}

void MicPanel::setPackPoints (const std::vector<ampsim::CabPack::Point>& packPoints)
{
    if (pad == nullptr)
        return;

    std::vector<juce::Point<float>> positions;
    for (const auto& p : packPoints)
        positions.push_back ({ (float) p.x, (float) p.y });
    pad->setPoints (std::move (positions));
    pad->refresh();
}

void MicPanel::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
}

void MicPanel::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (28);
    if (pad != nullptr)
    {
        packButton.setBounds (top.removeFromRight (92));
        top.removeFromRight (4);
        loadButton.setBounds (top.removeFromRight (84));
    }
    else
    {
        loadButton.setBounds (top.removeFromRight (100));
    }
    title.setBounds (top);
    area.removeFromTop (4);
    status.setBounds (area.removeFromTop (38));
    area.removeFromTop (4);

    if (pad != nullptr)
    {
        pad->setBounds (area.removeFromTop (juce::jmax (90, area.getHeight() - 150)));
        area.removeFromTop (6);
    }

    auto switches = area.removeFromBottom (28);
    if (mic != AmpSimProcessor::roomMic)
        channel.setBounds (switches.removeFromRight (130));
    for (auto* toggle : toggles)
        toggle->setBounds (switches.removeFromLeft (80));

    area.removeFromBottom (6);
    const auto width = area.getWidth() / 3;
    for (auto* knob : knobs)
        knob->setBounds (area.removeFromLeft (width));
}

// ---- AmpSimEditor -----------------------------------------------------------------------------

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p)
    : AudioProcessorEditor (&p),
      ampSim (p),
      inputKnob (p.parameters, "input_gain", "Input"),
      outputKnob (p.parameters, "output_gain", "Output"),
      lowCutKnob (p.parameters, "cab_lowcut_freq", "Low cut", " Hz"),
      highCutKnob (p.parameters, "cab_highcut_freq", "High cut", " Hz"),
      alignAttachment (p.parameters, "cab_align", alignButton),
      lowCutAttachment (p.parameters, "cab_lowcut_on", lowCutButton),
      highCutAttachment (p.parameters, "cab_highcut_on", highCutButton),
      cabBypassAttachment (p.parameters, "cab_bypass", cabBypassButton),
      lowCutSlopeAttachment (p.parameters, "cab_lowcut_slope", withItems (lowCutSlope, { "12 dB/oct", "24 dB/oct" })),
      highCutSlopeAttachment (p.parameters, "cab_highcut_slope", withItems (highCutSlope, { "12 dB/oct", "24 dB/oct" }))
{
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        auto& button = slotButtons[(size_t) s];
        button.setButtonText ("Amp " + juce::String (s + 1));
        button.setColour (juce::TextButton::buttonOnColourId, accent);
        button.onClick = [this, s]
        {
            if (auto* param = ampSim.parameters.getParameter (AmpSimProcessor::slotParamId))
                param->setValueNotifyingHost (param->convertTo0to1 ((float) s));
        };
        addAndMakeVisible (button);

        slotPanels.add (new SlotPanel (p, s, [this, s]
        {
            chooseFile ("Choose a NAM capture for Amp " + juce::String (s + 1), "*.nam", AmpSimProcessor::modelPathKey (s),
                        [this, s] (const juce::File& f) { ampSim.loadModel (s, f); });
        }));
        ampsPage.addAndMakeVisible (slotPanels.getLast());
    }

    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        micPanels.add (new MicPanel (p, m, [this, m]
        {
            chooseFile ("Choose an impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::cabPathKey (m),
                        [this, m] (const juce::File& f) { ampSim.loadCabIR (m, f); });
        },
        [this, m]
        {
            chooseFile ("Choose a cab pack folder", {}, AmpSimProcessor::cabPathKey (m),
                        [this, m] (const juce::File& f) { ampSim.loadCabIR (m, f); }, true);
        }));
        cabPage.addAndMakeVisible (micPanels.getLast());
    }

    alignmentLabel.setColour (juce::Label::textColourId, dimText);
    for (auto* c : std::initializer_list<juce::Component*> { &alignButton, &alignmentLabel, &lowCutButton, &lowCutKnob, &lowCutSlope,
                                                              &highCutButton, &highCutKnob, &highCutSlope, &cabBypassButton })
        cabPage.addAndMakeVisible (c);

    addAndMakeVisible (inputKnob);
    addAndMakeVisible (outputKnob);

    warningLabel.setColour (juce::Label::textColourId, warningColour);
    warningLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (warningLabel);

    ampsPage.layout = [this] (juce::Rectangle<int> amps)
    {
        // Three slot panels side by side.
        amps.reduce (8, 8);
        const auto panelWidth = amps.getWidth() / AmpSimProcessor::numAmpSlots;
        for (auto* slotPanel : slotPanels)
            slotPanel->setBounds (amps.removeFromLeft (panelWidth).reduced (4));
    };

    cabPage.layout = [this] (juce::Rectangle<int> cab)
    {
        cab.reduce (8, 8);

        // Bottom: alignment, cuts, bypass.
        auto global = cab.removeFromBottom (150).reduced (4);
        auto alignRow = global.removeFromTop (26);
        alignButton.setBounds (alignRow.removeFromLeft (190));
        alignmentLabel.setBounds (alignRow);
        global.removeFromTop (4);
        lowCutButton.setBounds (global.removeFromLeft (90).withSizeKeepingCentre (90, 26));
        lowCutKnob.setBounds (global.removeFromLeft (90));
        lowCutSlope.setBounds (global.removeFromLeft (110).withSizeKeepingCentre (110, 26));
        global.removeFromLeft (20);
        highCutButton.setBounds (global.removeFromLeft (90).withSizeKeepingCentre (90, 26));
        highCutKnob.setBounds (global.removeFromLeft (90));
        highCutSlope.setBounds (global.removeFromLeft (110).withSizeKeepingCentre (110, 26));
        cabBypassButton.setBounds (global.removeFromRight (120).withSizeKeepingCentre (120, 26));

        // Top: the three mics.
        const auto panelWidth = cab.getWidth() / AmpSimProcessor::numCabMics;
        for (auto* micPanel : micPanels)
            micPanel->setBounds (cab.removeFromLeft (panelWidth).reduced (4));
    };

    tabs.addTab ("Amps", panel, &ampsPage, false);
    tabs.addTab ("Cab", panel, &cabPage, false);
    tabs.setTabBarDepth (30);
    addAndMakeVisible (tabs);

    setSize (960, 720);
    timerCallback(); // show the current state right away
    startTimerHz (10);
}

AmpSimEditor::~AmpSimEditor()
{
    stopTimer();
}

void AmpSimEditor::chooseFile (const juce::String& title, const juce::String& patterns,
                               const juce::Identifier& lastPathKey,
                               std::function<void (const juce::File&)> onChosen, bool folders)
{
    // Start in the folder of the last file loaded, if there is one.
    const auto lastPath = ampSim.parameters.state.getProperty (lastPathKey).toString();
    const auto start = juce::File::isAbsolutePath (lastPath)
                           ? juce::File (lastPath).getParentDirectory()
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

void AmpSimEditor::timerCallback()
{
    const auto status = ampSim.getStatus();
    const auto active = juce::roundToInt (ampSim.parameters.getRawParameterValue (AmpSimProcessor::slotParamId)->load());

    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        slotButtons[(size_t) s].setToggleState (s == active, juce::dontSendNotification);
        slotPanels[s]->setActive (s == active);
        slotPanels[s]->setStatus (status.model[(size_t) s], status.modelError[(size_t) s]);
    }

    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        micPanels[m]->setStatus (status.cab[(size_t) m], status.cabError[(size_t) m]);
        if (m != AmpSimProcessor::roomMic)
            micPanels[m]->setPackPoints (ampSim.getCabPackPoints (m));
    }

    alignmentLabel.setText (status.alignment, juce::dontSendNotification);
    warningLabel.setText (status.warning, juce::dontSendNotification);
}

void AmpSimEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto header = getLocalBounds().reduced (16).removeFromTop (96);
    g.setColour (textColour);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("Amp Sim", header.removeFromTop (30).removeFromLeft (200), juce::Justification::centredLeft);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Three NAM slots, always running. Footswitch: program change 1/2/3.", header.removeFromTop (20),
                juce::Justification::centredLeft);
}

void AmpSimEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto header = area.removeFromTop (96);

    outputKnob.setBounds (header.removeFromRight (90));
    inputKnob.setBounds (header.removeFromRight (90));
    header.removeFromRight (16);

    auto buttons = header.removeFromRight (300).withSizeKeepingCentre (300, 34);
    for (auto& button : slotButtons)
        button.setBounds (buttons.removeFromLeft (100).reduced (3, 0));

    warningLabel.setBounds (area.removeFromBottom (40));
    area.removeFromBottom (6);
    tabs.setBounds (area); // each page lays itself out when the tabs size it
}

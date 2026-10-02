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
} // namespace

// ---- Knob -------------------------------------------------------------------------------------

Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterId, const juce::String& caption)
    : attachment (state, parameterId, slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
    slider.setTextValueSuffix (" dB");
    slider.setDoubleClickReturnValue (true, 0.0); // double-click resets to 0 dB
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
    title.setText ("Amp " + juce::String (slot + 1), juce::dontSendNotification);
    title.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, textColour);
    addAndMakeVisible (title);

    status.setColour (juce::Label::textColourId, textColour);
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (0.8f);
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

// ---- AmpSimEditor -----------------------------------------------------------------------------

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p)
    : AudioProcessorEditor (&p),
      ampSim (p),
      inputKnob (p.parameters, "input_gain", "Input"),
      outputKnob (p.parameters, "output_gain", "Output"),
      cabBypassAttachment (p.parameters, "cab_bypass", cabBypassButton)
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

    addAndMakeVisible (inputKnob);
    addAndMakeVisible (outputKnob);

    warningLabel.setColour (juce::Label::textColourId, warningColour);
    warningLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (warningLabel);

    loadIRButton.onClick = [this]
    {
        chooseFile ("Choose a cab impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::irPathKey,
                    [this] (const juce::File& f) { ampSim.loadImpulseResponse (f); });
    };
    irLabel.setColour (juce::Label::textColourId, textColour);
    cabPage.addAndMakeVisible (loadIRButton);
    cabPage.addAndMakeVisible (irLabel);
    cabPage.addAndMakeVisible (cabBypassButton);

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
        auto row = cab.reduced (16).removeFromTop (34);
        loadIRButton.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (10);
        cabBypassButton.setBounds (row.removeFromRight (120));
        irLabel.setBounds (row);
    };

    tabs.addTab ("Amps", panel, &ampsPage, false);
    tabs.addTab ("Cab", panel, &cabPage, false);
    tabs.setTabBarDepth (30);
    addAndMakeVisible (tabs);

    setSize (920, 640);
    timerCallback(); // show the current state right away
    startTimerHz (10);
}

AmpSimEditor::~AmpSimEditor()
{
    stopTimer();
}

void AmpSimEditor::chooseFile (const juce::String& title, const juce::String& patterns,
                               const juce::Identifier& lastPathKey,
                               std::function<void (const juce::File&)> onChosen)
{
    // Start in the folder of the last file loaded, if there is one.
    const auto lastPath = ampSim.parameters.state.getProperty (lastPathKey).toString();
    const auto start = juce::File::isAbsolutePath (lastPath)
                           ? juce::File (lastPath).getParentDirectory()
                           : juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [onChosen] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
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

    irLabel.setText ("Cab: " + status.cab, juce::dontSendNotification);
    irLabel.setColour (juce::Label::textColourId, status.cabError ? errorColour : textColour);
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

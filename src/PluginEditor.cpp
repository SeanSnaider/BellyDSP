#include "PluginEditor.h"

namespace
{
const auto background = juce::Colour (0xff17191d);
const auto panel = juce::Colour (0xff22252b);
const auto textColour = juce::Colour (0xffe6e6e6);
const auto dimText = juce::Colour (0xff9aa0a8);
const auto errorColour = juce::Colour (0xffff6b5e);
const auto warningColour = juce::Colour (0xffffc04d);

void styleKnob (juce::Slider& knob)
{
    knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    knob.setTextValueSuffix (" dB");
    knob.setDoubleClickReturnValue (true, 0.0); // double-click resets to 0 dB
}
} // namespace

AmpSimEditor::AmpSimEditor (AmpSimProcessor& p)
    : AudioProcessorEditor (&p),
      ampSim (p),
      inputGainAttachment (p.parameters, "input_gain", inputGainKnob),
      outputGainAttachment (p.parameters, "output_gain", outputGainKnob),
      cabBypassAttachment (p.parameters, "cab_bypass", cabBypassButton)
{
    for (auto* label : { &modelLabel, &irLabel })
    {
        label->setColour (juce::Label::textColourId, textColour);
        label->setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (label);
    }

    warningLabel.setColour (juce::Label::textColourId, warningColour);
    warningLabel.setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (warningLabel);

    for (auto* caption : { &inputGainCaption, &outputGainCaption })
    {
        caption->setJustificationType (juce::Justification::centred);
        caption->setColour (juce::Label::textColourId, dimText);
        addAndMakeVisible (caption);
    }

    styleKnob (inputGainKnob);
    styleKnob (outputGainKnob);
    addAndMakeVisible (inputGainKnob);
    addAndMakeVisible (outputGainKnob);
    addAndMakeVisible (cabBypassButton);

    loadModelButton.onClick = [this]
    {
        chooseFile ("Choose a NAM capture (.nam)", "*.nam", AmpSimProcessor::modelPathKey,
                    [this] (const juce::File& f) { ampSim.loadModel (f); });
    };

    loadIRButton.onClick = [this]
    {
        chooseFile ("Choose a cab impulse response", "*.wav;*.aif;*.aiff;*.flac", AmpSimProcessor::irPathKey,
                    [this] (const juce::File& f) { ampSim.loadImpulseResponse (f); });
    };

    addAndMakeVisible (loadModelButton);
    addAndMakeVisible (loadIRButton);

    setSize (560, 360);
    timerCallback(); // show the current status right away
    startTimerHz (4);
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

    modelLabel.setText ("Amp: " + status.model, juce::dontSendNotification);
    modelLabel.setColour (juce::Label::textColourId, status.modelError ? errorColour : textColour);

    irLabel.setText ("Cab: " + status.cab, juce::dontSendNotification);
    irLabel.setColour (juce::Label::textColourId, status.cabError ? errorColour : textColour);

    warningLabel.setText (status.warning, juce::dontSendNotification);
}

void AmpSimEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto area = getLocalBounds().reduced (16);
    auto header = area.removeFromTop (34);
    g.setColour (textColour);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("Amp Sim", header.removeFromLeft (140), juce::Justification::centredLeft);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("NAM amp + IR cab", header, juce::Justification::centredLeft);

    // Panels behind the loader rows and the knobs.
    area.removeFromTop (8);
    g.setColour (panel);
    g.fillRoundedRectangle (area.removeFromTop (88).toFloat(), 6.0f);
    area.removeFromTop (10);
    g.fillRoundedRectangle (area.removeFromTop (130).toFloat(), 6.0f);
}

void AmpSimEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    area.removeFromTop (34 + 8);

    auto loaders = area.removeFromTop (88).reduced (10, 8);
    auto modelRow = loaders.removeFromTop (34);
    loadModelButton.setBounds (modelRow.removeFromLeft (150).reduced (0, 3));
    modelRow.removeFromLeft (10);
    modelLabel.setBounds (modelRow);

    loaders.removeFromTop (4);
    auto irRow = loaders.removeFromTop (34);
    loadIRButton.setBounds (irRow.removeFromLeft (150).reduced (0, 3));
    irRow.removeFromLeft (10);
    cabBypassButton.setBounds (irRow.removeFromRight (110));
    irLabel.setBounds (irRow);

    area.removeFromTop (10);
    auto knobs = area.removeFromTop (130).reduced (10, 6);
    auto left = knobs.removeFromLeft (knobs.getWidth() / 2);
    inputGainCaption.setBounds (left.removeFromTop (20));
    inputGainKnob.setBounds (left);
    outputGainCaption.setBounds (knobs.removeFromTop (20));
    outputGainKnob.setBounds (knobs);

    area.removeFromTop (4);
    warningLabel.setBounds (area); // about 44 px: room for two lines
}

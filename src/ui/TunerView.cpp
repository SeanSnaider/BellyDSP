#include "TunerView.h"

using namespace ui::theme;

namespace
{
const juce::Identifier tunerDisplayKey { "tunerDisplay" };
}

TunerView::TunerView (AmpSimProcessor& processor)
    : ampSim (processor),
      a4Knob (processor.parameters, "tuner_a4", "A4", " Hz"),
      muteAttachment (processor.parameters, "tuner_mute", ui::tagged (muteButton, "tuner_mute"))
{
    setOpaque (true);
    for (auto* button : { &needleButton, &strobeButton })
    {
        button->setClickingTogglesState (false);
        addAndMakeVisible (button);
    }
    needleButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    strobeButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    needleButton.onClick = [this] { setStrobe (false); };
    strobeButton.onClick = [this] { setStrobe (true); };
    closeButton.onClick = [this]
    {
        if (auto* tunerSwitch = ampSim.parameters.getParameter ("tuner_on"))
            tunerSwitch->setValueNotifyingHost (0.0f);
    };
    addAndMakeVisible (closeButton);
    addAndMakeVisible (muteButton);
    addAndMakeVisible (a4Knob);
    setStrobe (ampSim.parameters.state.getProperty (tunerDisplayKey).toString() == "strobe");
}

TunerView::~TunerView()
{
    stopTimer();
}

void TunerView::setStrobe (bool shouldShowStrobe)
{
    strobe = shouldShowStrobe;
    ampSim.parameters.state.setProperty (tunerDisplayKey, strobe ? "strobe" : "needle", nullptr);
    needleButton.setToggleState (! strobe, juce::dontSendNotification);
    strobeButton.setToggleState (strobe, juce::dontSendNotification);
    repaint();
}

void TunerView::visibilityChanged()
{
    if (isVisible())
        startTimerHz (60);
    else
        stopTimer();
}

void TunerView::timerCallback()
{
    if (! frozen)
        reading = ampSim.getTunerReading();
    repaint (displayArea);
    repaint (footerArea);
}

void TunerView::paint (juce::Graphics& g)
{
    g.fillAll (background);

    // The heading.
    g.setFont (font ("Semibold", 12.0f).withExtraKerningFactor (0.1f));
    g.setColour (textDim);
    g.drawText ("TUNER", getLocalBounds().reduced (space::xl, space::l).withHeight (32), juce::Justification::centredLeft, false);

    const auto offset = std::abs (reading.cents);
    const auto colour = ! reading.hasReading || ! reading.live ? textDim : offset < 1.0 ? good : offset < 5.0 ? warn : error;

    auto area = displayArea.toFloat();
    auto top = area.removeFromTop (area.getHeight() * 0.42f);

    g.setColour (reading.hasReading ? (reading.live ? text : textDim) : textDim);
    g.setFont (font (Text::huge));
    g.drawText (reading.hasReading ? juce::MidiMessage::getMidiNoteName (reading.midiNote, true, true, 4) : juce::String ("--"),
                top.removeFromTop (top.getHeight() * 0.74f), juce::Justification::centred);

    g.setFont (juce::FontOptions (juce::Font::getSystemUIFontName(), "Regular", 10.0f).withPointHeight (20.0f).withFeatureEnabled ("tnum"));
    g.setColour (textDim);
    const auto sign = reading.cents >= 0.0 ? "+" : "";
    g.drawText (reading.hasReading ? juce::String (reading.frequency, 2) + " Hz      " + sign + juce::String (reading.cents, 1) + " cents"
                                         + (reading.live ? "" : "      (holding)")
                                   : juce::String ("Play one string"),
                top, juce::Justification::centred);

    if (strobe)
        paintStrobe (g, area.reduced (area.getWidth() * 0.12f, 24.0f), colour);
    else
        paintNeedle (g, area.reduced (area.getWidth() * 0.1f, 4.0f), colour);

    g.setColour (textDim);
    g.setFont (font (Text::label));
    g.drawText ("Input " + (reading.levelDb > -200.0 ? juce::String (juce::roundToInt (reading.levelDb)) + " dBFS" : juce::String ("silent"))
                    + "      A4 = " + juce::String (reading.referenceA4, 1) + " Hz",
                footerArea, juce::Justification::centred);
}

void TunerView::paintNeedle (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour) const
{
    // A 120 degree arc for -50 to +50 cents, angles clockwise from straight up.
    const juce::Point<float> centre { area.getCentreX(), area.getBottom() - 8.0f };
    const auto radius = std::min (area.getWidth() * 0.5f, area.getHeight() - 16.0f);
    const auto angleFor = [] (double cents) { return (float) (juce::jlimit (-50.0, 50.0, cents) / 50.0 * juce::MathConstants<double>::pi / 3.0); };
    const auto towards = [&] (float angle, float r) { return centre + juce::Point<float> (std::sin (angle), -std::cos (angle)) * r; };

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, angleFor (-50.0), angleFor (50.0), true);
    g.setColour (outline);
    g.strokePath (track, juce::PathStrokeType (2.0f));

    juce::Path zone; // the in-tune zone, +-1 cent
    zone.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, angleFor (-1.0), angleFor (1.0), true);
    g.setColour (good.withAlpha (0.6f));
    g.strokePath (zone, juce::PathStrokeType (10.0f));

    g.setFont (font (Text::label));
    for (int c = -50; c <= 50; c += 5)
    {
        const auto major = c % 10 == 0;
        g.setColour (c == 0 ? text : textDim);
        g.drawLine ({ towards (angleFor (c), radius * (major ? 0.86f : 0.92f)), towards (angleFor (c), radius) }, c == 0 ? 3.0f : (major ? 2.0f : 1.0f));
        if (major)
        {
            const auto at = towards (angleFor (c), radius * 0.77f);
            g.drawText ((c > 0 ? "+" : "") + juce::String (c), juce::Rectangle<float> (40.0f, 16.0f).withCentre (at), juce::Justification::centred);
        }
    }

    if (reading.hasReading)
    {
        g.setColour (colour);
        g.drawLine ({ centre, towards (angleFor (reading.cents), radius * 0.97f) }, 4.0f);
    }
    g.setColour (text);
    g.fillEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (centre));
    g.setColour (background);
    g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (centre));
}

void TunerView::paintStrobe (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour) const
{
    // Stripes that drift by the analysis's strobe phase: still when in tune, rightwards when sharp, at a
    // quarter of a stripe per second per cent. Two bands, the lower with stripes half as wide, as on a
    // mechanical strobe, so small offsets are easier to see.
    const auto bands = { std::pair<float, float> { 0.0f, 64.0f }, std::pair<float, float> { 0.5f, 32.0f } };
    for (const auto& [fromTop, period] : bands)
    {
        auto band = area.withTrimmedTop (area.getHeight() * fromTop).withHeight (area.getHeight() * 0.42f);
        g.setColour (surface);
        g.fillRoundedRectangle (band, radiusControl);
        g.saveState();
        g.reduceClipRegion (band.toNearestInt());
        g.setColour (colour);
        const auto shift = (float) reading.strobePhase * period;
        for (auto x = band.getX() - period + shift; x < band.getRight(); x += period)
            g.fillRect (juce::Rectangle<float> (x, band.getY(), period * 0.5f, band.getHeight()));
        g.restoreState();
    }
    g.setColour (text);
    g.drawLine (area.getCentreX(), area.getY() - 6.0f, area.getCentreX(), area.getBottom() + 6.0f, 2.0f);
}

void TunerView::resized()
{
    auto area = getLocalBounds().reduced (space::xl, space::l);
    auto top = area.removeFromTop (32);
    closeButton.setBounds (top.removeFromRight (120));
    top.removeFromRight (space::l);
    strobeButton.setBounds (top.removeFromRight (80));
    needleButton.setBounds (top.removeFromRight (80));

    auto bottom = area.removeFromBottom (ui::Knob::preferredHeight (ui::Knob::Size::normal));
    a4Knob.setBounds (bottom.removeFromLeft (ui::Knob::preferredWidth (ui::Knob::Size::normal)));
    bottom.removeFromLeft (space::l);
    muteButton.setBounds (bottom.removeFromLeft (muteButton.getPreferredWidth()).withSizeKeepingCentre (muteButton.getPreferredWidth(), switchHeight));
    footerArea = bottom.withTrimmedRight (ui::Knob::preferredWidth (ui::Knob::Size::normal) + space::l + muteButton.getPreferredWidth());

    area.removeFromBottom (space::m);
    displayArea = area.reduced (space::s);
}

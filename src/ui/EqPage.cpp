#include "Pages.h"

namespace ui
{

using namespace theme;

namespace
{
constexpr double minFrequency = 20.0, maxFrequency = 20000.0;
constexpr double curveRangeDb = 24.0;                  // the curve's scale: +-24 dB
constexpr float analyzerTopDb = 0.0f, analyzerBottomDb = -90.0f;
constexpr double analyzerTiltDbPerOctave = 3.0;        // display only: pink noise reads flat (U11)
constexpr double sampleRate = 48000.0;                 // the chain only runs at 48 kHz

juce::String hertzText (double f)
{
    return f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
}

juce::RangedAudioParameter* parameter (AmpSimProcessor& p, const juce::String& id)
{
    return id.isEmpty() ? nullptr : p.parameters.getParameter (id);
}

float plainValue (AmpSimProcessor& p, const juce::String& id)
{
    return p.parameters.getRawParameterValue (id)->load();
}

void setPlain (AmpSimProcessor& p, const juce::String& id, float value)
{
    if (auto* param = parameter (p, id))
        param->setValueNotifyingHost (param->convertTo0to1 (value));
}
} // namespace

// ---- EqGraph ---------------------------------------------------------------------------------------------

EqGraph::EqGraph (AmpSimProcessor& p, const juce::String& prefixToUse, bool isPost) : ampSim (p), prefix (prefixToUse), post (isPost)
{
    values.bind (p.parameters, prefix);
    for (int i = 0; i < 240; ++i)
        frequencies.push_back (minFrequency * std::pow (maxFrequency / minFrequency, i / 239.0));

    using EP = params::EqualizerParameters;
    for (int b = 0; b < ampsim::Equalizer::numParametricBands; ++b)
        bandHandles.push_back ({ EP::bandId (prefix, b, "freq"), EP::bandId (prefix, b, "gain"), EP::bandId (prefix, b, "q"), EP::bandId (prefix, b, "type"), 0.0 });
    for (int m = 0; m < ampsim::Equalizer::numGraphicBands; ++m)
        sliderHandles.push_back ({ {}, EP::sliderId (prefix, m), {}, {}, ampsim::Equalizer::graphicCentres[(size_t) m] });

    refreshCurve();
}

EqGraph::~EqGraph()
{
    stopTimer();
    if (wheeling)
        if (auto* q = parameter (ampSim, handle (juce::jmax (0, hovered)).qId))
            q->endChangeGesture();
    if (dragging >= 0)
        for (const auto& id : { handle (dragging).frequencyId, handle (dragging).gainId })
            if (auto* param = parameter (ampSim, id))
                param->endChangeGesture();
}

bool EqGraph::parametric() const
{
    return values.mode.index() == 1;
}

int EqGraph::numHandles() const
{
    return parametric() ? (int) bandHandles.size() : (int) sliderHandles.size();
}

const EqGraph::Handle& EqGraph::handle (int index) const
{
    const auto& list = parametric() ? bandHandles : sliderHandles;
    return list[(size_t) juce::jlimit (0, (int) list.size() - 1, index)];
}

juce::Rectangle<float> EqGraph::plot() const
{
    return getLocalBounds().toFloat().reduced (1.0f);
}

float EqGraph::xFor (double f) const
{
    const auto r = plot();
    return r.getX() + r.getWidth() * (float) (std::log (f / minFrequency) / std::log (maxFrequency / minFrequency));
}

double EqGraph::frequencyAt (float x) const
{
    const auto r = plot();
    return juce::jlimit (minFrequency, maxFrequency, minFrequency * std::pow (maxFrequency / minFrequency, (double) ((x - r.getX()) / r.getWidth())));
}

float EqGraph::yForGain (double db) const
{
    const auto r = plot();
    return r.getCentreY() - (r.getHeight() * 0.5f - 12.0f) * (float) (juce::jlimit (-curveRangeDb, curveRangeDb, db) / curveRangeDb);
}

double EqGraph::gainAt (float y) const
{
    const auto r = plot();
    return (double) ((r.getCentreY() - y) / (r.getHeight() * 0.5f - 12.0f)) * curveRangeDb;
}

float EqGraph::levelDisplayOffset (double f) const
{
    // The tapped signal back to what the EQ sees: the pre tap is the guitar before the input gain, the post
    // tap the output after the output level. Then the display tilt, pivoting at 1 kHz.
    const auto gain = post ? -plainValue (ampSim, "output_gain") : plainValue (ampSim, "input_gain");
    return gain + (float) (analyzerTiltDbPerOctave * std::log2 (f / 1000.0));
}

juce::Point<float> EqGraph::handlePosition (int index) const
{
    const auto& h = handle (index);
    if (parametric())
    {
        const auto f = (double) plainValue (ampSim, h.frequencyId);
        const auto notch = juce::roundToInt (plainValue (ampSim, h.typeId)) == (int) ampsim::Equalizer::BandType::notch;
        return { xFor (f), yForGain (notch ? 0.0 : (double) plainValue (ampSim, h.gainId)) };
    }
    return { xFor (h.frequency), yForGain ((double) plainValue (ampSim, h.gainId)) };
}

int EqGraph::handleAt (juce::Point<float> position) const
{
    int best = -1;
    auto bestDistance = 13.0f;
    for (int i = 0; i < numHandles(); ++i)
        if (const auto d = handlePosition (i).getDistanceFrom (position); d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    return best;
}

void EqGraph::refreshCurve()
{
    const auto settings = values.read();
    const auto same = [] (const ampsim::Equalizer::Settings& a, const ampsim::Equalizer::Settings& b)
    {
        // Field by field (the raw bytes would include padding): any change redraws.
        const auto eq = [] (float x, float y) { return std::abs (x - y) < 1.0e-6f; };
        if (a.mode != b.mode || a.lowCut.on != b.lowCut.on || a.highCut.on != b.highCut.on || a.lowCut.slope != b.lowCut.slope
            || a.highCut.slope != b.highCut.slope || ! eq (a.lowCut.frequency, b.lowCut.frequency) || ! eq (a.highCut.frequency, b.highCut.frequency))
            return false;
        for (size_t m = 0; m < a.sliders.size(); ++m)
            if (! eq (a.sliders[m], b.sliders[m]))
                return false;
        for (size_t i = 0; i < a.bands.size(); ++i)
            if (a.bands[i].type != b.bands[i].type || ! eq (a.bands[i].frequency, b.bands[i].frequency) || ! eq (a.bands[i].gainDb, b.bands[i].gainDb)
                || ! eq (a.bands[i].q, b.bands[i].q))
                return false;
        return true;
    };

    if (everDrawn && same (settings, drawn))
        return;

    drawn = settings;
    everDrawn = true;
    curve = ampsim::Equalizer::responseDb (settings, frequencies, sampleRate);
    repaint();
}

void EqGraph::updateAnalyzer()
{
    refreshCurve();
    if (analyzer.pull (ampSim.getAnalyzerRing()) > 0)
    {
        analyzer.compute();
        repaint();
    }
}

void EqGraph::paint (juce::Graphics& g)
{
    const auto r = plot();
    g.setColour (background);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), radiusControl);

    // The grid: octaves-ish across, every 12 dB down.
    g.setColour (outline);
    for (auto f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        g.drawVerticalLine (juce::roundToInt (xFor (f)), r.getY(), r.getBottom());
    for (auto db : { -24.0, -12.0, 12.0, 24.0 })
        g.drawHorizontalLine (juce::roundToInt (yForGain (db)), r.getX(), r.getRight());
    g.setColour (outline.brighter (0.35f));
    g.drawHorizontalLine (juce::roundToInt (yForGain (0.0)), r.getX(), r.getRight());

    // The live spectrum, behind the curve.
    if (analyzer.getSamplesTaken() > 0)
    {
        juce::Path spectrum;
        const auto yOf = [&r] (float db) { return r.getBottom() - r.getHeight() * juce::jlimit (0.0f, 1.0f, (db - analyzerBottomDb) / (analyzerTopDb - analyzerBottomDb)); };
        spectrum.startNewSubPath (r.getX(), r.getBottom());
        for (auto x = r.getX(); x <= r.getRight(); x += 2.0f)
        {
            const auto f = frequencyAt (x);
            const auto level = analyzer.levelAt (f, frequencyAt (x + 2.0f) / f, sampleRate) + levelDisplayOffset (f);
            spectrum.lineTo (x, yOf (level));
        }
        spectrum.lineTo (r.getRight(), r.getBottom());
        spectrum.closeSubPath();
        const auto colour = post ? sectionPost : sectionPre;
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.28f), 0.0f, r.getY(), colour.withAlpha (0.04f), 0.0f, r.getBottom(), false));
        g.fillPath (spectrum);
        g.setColour (colour.withAlpha (0.45f));
        g.strokePath (spectrum, juce::PathStrokeType (1.0f));
    }

    // The response: filled toward 0 dB, then the line; dimmed while the EQ is off.
    const auto on = values.isOn();
    if (! curve.empty())
    {
        juce::Path line;
        for (size_t i = 0; i < curve.size(); ++i)
        {
            const juce::Point<float> p { xFor (frequencies[i]), yForGain (curve[i]) };
            i == 0 ? line.startNewSubPath (p) : line.lineTo (p);
        }
        auto fill = line;
        fill.lineTo (r.getRight(), yForGain (0.0));
        fill.lineTo (r.getX(), yForGain (0.0));
        fill.closeSubPath();
        g.setColour (accent.withAlpha (on ? 0.16f : 0.06f));
        g.fillPath (fill);
        g.setColour (on ? accent : accent.withAlpha (0.4f));
        g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The labels.
    g.setFont (font (Text::caption));
    g.setColour (textDim);
    for (auto [f, label] : std::initializer_list<std::pair<double, const char*>> { { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                                                                   { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" } })
        g.drawText (label, juce::Rectangle<float> (xFor (f) + 3.0f, r.getBottom() - 15.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
    for (auto db : { -12.0, 12.0 })
        g.drawText ((db > 0 ? "+" : "") + juce::String (juce::roundToInt (db)), juce::Rectangle<float> (r.getX() + 4.0f, yForGain (db) - 13.0f, 30.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    g.drawText (post ? "Spectrum: the post section's output" : "Spectrum: the guitar coming in", r.reduced (8.0f, 5.0f).withHeight (13.0f),
                juce::Justification::centredRight, false);

    // The handles: numbered bands (parametric) or the nine sliders (graphic).
    const auto bands = parametric();
    for (int i = 0; i < numHandles(); ++i)
    {
        const auto p = handlePosition (i);
        const auto active = i == hovered || i == dragging;
        const auto radius = bands ? (active ? 10.0f : 8.5f) : (active ? 7.0f : 5.5f);
        const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p);
        g.setColour (active ? accent : surfaceRaised);
        g.fillEllipse (circle);
        g.setColour (on ? accent : accent.withAlpha (0.5f));
        g.drawEllipse (circle.reduced (0.75f), 1.5f);
        if (bands)
        {
            g.setColour (active ? onAccent : theme::text);
            g.setFont (font ("Semibold", 10.5f));
            g.drawText (juce::String (i + 1), circle, juce::Justification::centred, false);
        }
    }

    // A readout by the handle under the pointer.
    if (const auto shown = dragging >= 0 ? dragging : hovered; shown >= 0)
    {
        const auto& h = handle (shown);
        juce::String readout;
        if (bands)
        {
            auto* type = parameter (ampSim, h.typeId);
            readout = (type != nullptr ? type->getCurrentValueAsText() : juce::String()) + "  " + hertzText (plainValue (ampSim, h.frequencyId)) + "  "
                      + juce::String (plainValue (ampSim, h.gainId), 1) + " dB  Q " + juce::String (plainValue (ampSim, h.qId), 2);
        }
        else
        {
            readout = hertzText (h.frequency) + "  " + juce::String (plainValue (ampSim, h.gainId), 1) + " dB";
        }
        const auto p = handlePosition (shown);
        const auto width = juce::GlyphArrangement::getStringWidth (font (Text::label), readout) + 16.0f;
        auto box = juce::Rectangle<float> (width, 22.0f).withCentre ({ p.x, p.y - 24.0f });
        box = box.withX (juce::jlimit (r.getX() + 2.0f, r.getRight() - width - 2.0f, box.getX())).withY (juce::jmax (r.getY() + 2.0f, box.getY()));
        g.setColour (surfaceRaised);
        g.fillRoundedRectangle (box, radiusControl);
        g.setColour (outline);
        g.drawRoundedRectangle (box, radiusControl, 1.0f);
        g.setColour (theme::text);
        g.setFont (font (Text::label));
        g.drawText (readout, box, juce::Justification::centred, false);
    }
}

void EqGraph::setHovered (int index)
{
    if (index == hovered)
        return;
    hovered = index;

    // A right-click on a handle offers MIDI learn for its gain, like any control.
    if (hovered >= 0)
        getProperties().set (parameterIdProperty, handle (hovered).gainId);
    else
        getProperties().remove (parameterIdProperty);
    setMouseCursor (hovered >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void EqGraph::mouseMove (const juce::MouseEvent& e)
{
    setHovered (handleAt (e.position));
}

void EqGraph::mouseExit (const juce::MouseEvent&)
{
    if (dragging < 0)
        setHovered (-1);
}

void EqGraph::resetHandle (int index)
{
    // Flat again: the band's (or slider's) gain back to 0 dB, as one gesture.
    setAsGesture (ampSim.parameters, handle (index).gainId, 0.0f);
}

void EqGraph::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    const auto index = handleAt (e.position);
    if (index < 0)
        return;
    if (e.mods.isAltDown())
    {
        resetHandle (index);
        return;
    }

    dragging = index;
    setHovered (index);
    const auto& h = handle (index);
    beginUndoStep (ampSim.parameters);
    for (const auto& id : { h.frequencyId, h.gainId })
        if (auto* param = parameter (ampSim, id))
            param->beginChangeGesture();
    lastDrag = e.position;
    dragFrequency = h.frequencyId.isEmpty() ? h.frequency : (double) plainValue (ampSim, h.frequencyId);
    dragGain = (double) plainValue (ampSim, h.gainId);
}

void EqGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0 || e.mods.isPopupMenu())
        return;

    // Relative moves (shift: ten times finer), so grabbing a handle off-centre doesn't jump it.
    const auto& h = handle (dragging);
    const auto fine = e.mods.isShiftDown() ? 0.1f : 1.0f;
    const auto delta = (e.position - lastDrag) * fine;
    lastDrag = e.position;

    const auto range = parametric() ? ampsim::Equalizer::parametricRangeDb : ampsim::Equalizer::sliderRangeDb;
    const auto r = plot();
    dragGain = juce::jlimit (-range, range, dragGain - (double) delta.y / (double) (r.getHeight() * 0.5f - 12.0f) * curveRangeDb);
    const auto notch = parametric() && juce::roundToInt (plainValue (ampSim, h.typeId)) == (int) ampsim::Equalizer::BandType::notch;
    if (! notch)
        setPlain (ampSim, h.gainId, (float) dragGain);

    if (h.frequencyId.isNotEmpty())
    {
        dragFrequency = frequencyAt (xFor (dragFrequency) + delta.x);
        setPlain (ampSim, h.frequencyId, (float) dragFrequency);
    }
    repaint();
}

void EqGraph::mouseUp (const juce::MouseEvent&)
{
    if (dragging < 0)
        return;
    const auto& h = handle (dragging);
    for (const auto& id : { h.frequencyId, h.gainId })
        if (auto* param = parameter (ampSim, id))
            param->endChangeGesture();
    dragging = -1;
    repaint();
}

void EqGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto index = handleAt (e.position); index >= 0 && ! e.mods.isPopupMenu())
        resetHandle (index);
}

void EqGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // The scroll wheel on a band's handle changes its Q (a run of moves is one undo step).
    const auto index = handleAt (e.position);
    if (! parametric() || index < 0)
        return;
    auto* q = parameter (ampSim, handle (index).qId);
    if (q == nullptr)
        return;

    const auto delta = (wheel.isReversed ? -1.0f : 1.0f) * (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY);
    if (! wheeling)
    {
        beginUndoStep (ampSim.parameters);
        q->beginChangeGesture();
        wheeling = true;
        setHovered (index);
    }
    const auto current = q->convertFrom0to1 (q->getValue());
    const auto next = juce::jlimit (0.1f, 18.0f, current * std::exp (delta * (e.mods.isShiftDown() ? 0.15f : 1.5f)));
    q->setValueNotifyingHost (q->convertTo0to1 (next));
    startTimer (350);
    repaint();
}

void EqGraph::timerCallback()
{
    stopTimer();
    if (! wheeling)
        return;
    wheeling = false;
    if (auto* q = parameter (ampSim, handle (juce::jmax (0, hovered)).qId))
        q->endChangeGesture();
}

// ---- EqPage ----------------------------------------------------------------------------------------------

EqPage::EqPage (AmpSimProcessor& p, bool isPost)
    : BlockPage (p, isPost ? BlockId::postEq : BlockId::preEq, "EQ",
                 isPost ? "After the cab" : "Before the amp"),
      post (isPost),
      prefix (isPost ? "eq_post" : "eq_pre")
{
    using EP = params::EqualizerParameters;
    graph = &adopt (std::make_unique<EqGraph> (p, prefix, post));
    mode = &addCombo (prefix + "_mode", { "Graphic", "Parametric" });

    static const char* sliderNames[] = { "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    for (int m = 0; m < ampsim::Equalizer::numGraphicBands; ++m)
        sliders.push_back (&addFader (EP::sliderId (prefix, m), sliderNames[m], " dB"));

    for (int b = 0; b < ampsim::Equalizer::numParametricBands; ++b)
    {
        auto& band = bands[(size_t) b];
        band.type = &addCombo (EP::bandId (prefix, b, "type"), { "Peak", "Low shelf", "High shelf", "Notch" });
        band.frequency = &addField (EP::bandId (prefix, b, "freq"), " Hz");
        band.gain = &addField (EP::bandId (prefix, b, "gain"), " dB");
        band.q = &addField (EP::bandId (prefix, b, "q"), "");
        band.q->setFormatter ([] (float q) { return "Q " + juce::String (q, 2); });
    }

    const juce::StringArray slopes { "12 dB/oct", "24 dB/oct", "48 dB/oct" };
    lowCut = &addSwitch (prefix + "_lowcut_on", {});
    highCut = &addSwitch (prefix + "_highcut_on", {});
    lowCutFrequency = &addKnob (prefix + "_lowcut_freq", "Frequency", " Hz", Knob::Size::compact);
    highCutFrequency = &addKnob (prefix + "_highcut_freq", "Frequency", " Hz", Knob::Size::compact);
    lowCutSlope = &addCombo (prefix + "_lowcut_slope", slopes);
    highCutSlope = &addCombo (prefix + "_highcut_slope", slopes);
    refresh();
}

EqPage::~EqPage()
{
    stopTimer();
}

void EqPage::showMode (bool parametric)
{
    for (auto* s : sliders)
        s->setVisible (! parametric);
    for (auto& band : bands)
        for (auto* c : std::initializer_list<juce::Component*> { band.type, band.frequency, band.gain, band.q })
            c->setVisible (parametric);
    resized(); // the cards follow the mode
    graph->repaint();
}

void EqPage::refresh()
{
    graph->refreshCurve();
    const auto parametric = juce::roundToInt (state.getRawParameterValue (prefix + "_mode")->load());
    if (parametric != shownMode)
    {
        shownMode = parametric;
        showMode (parametric == 1);
    }
}

void EqPage::pageShown()
{
    // The analyzer reads this EQ's tap from now on: drop what the ring holds from before.
    ampSim.setAnalyzerTap (post ? AmpSimProcessor::AnalyzerTap::postSection : AmpSimProcessor::AnalyzerTap::preSection);
    ampSim.getAnalyzerRing().discardAll();
    graph->getAnalyzer().reset();
    startTimerHz (meterFps);
}

void EqPage::pageHidden()
{
    stopTimer();
    const auto mine = post ? AmpSimProcessor::AnalyzerTap::postSection : AmpSimProcessor::AnalyzerTap::preSection;
    if (ampSim.getAnalyzerTap() == mine)
        ampSim.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::off);
}

void EqPage::layoutContent (juce::Rectangle<int> area)
{
    if (! headerSpace.isEmpty())
        mode->setBounds (headerSpace.removeFromRight (140).withSizeKeepingCentre (140, controlHeight));

    // The graph on top; along the bottom, the sliders (graphic) or a card per band (parametric), and the cuts.
    constexpr int fieldHeight = 23, fieldGap = 3;
    auto bottom = area.removeFromBottom (cardHeight (4 * fieldHeight + 3 * fieldGap));
    area.removeFromBottom (space::m);
    graph->setBounds (area);

    layoutCards (bottom.removeFromRight (juce::jmin (bottom.getWidth() / 2, 2 * 236 + space::m)),
                 { { "Low cut", { { lowCut }, { lowCutFrequency }, { lowCutSlope, 96 } } },
                   { "High cut", { { highCut }, { highCutFrequency }, { highCutSlope, 96 } } } });
    bottom.removeFromRight (space::m);

    if (shownMode != 1)
    {
        // Graphic: the nine octave sliders in one card.
        auto inside = card (bottom, "Octave bands");
        const auto width = juce::jmin (72, inside.getWidth() / (int) sliders.size());
        auto faders = inside.withSizeKeepingCentre (width * (int) sliders.size(), inside.getHeight());
        for (auto* s : sliders)
            s->setBounds (faders.removeFromLeft (width).reduced (4, 0));
        return;
    }

    // Parametric: a card per band, with its type and then its frequency, gain, and Q as fields (the graph's
    // handles are the quick way; these are for exact values).
    const auto gap = space::s;
    const auto width = (bottom.getWidth() - gap * ((int) bands.size() - 1)) / (int) bands.size();
    for (size_t b = 0; b < bands.size(); ++b)
    {
        auto inside = card (bottom.removeFromLeft (width), "Band " + juce::String ((int) b + 1));
        bottom.removeFromLeft (gap);
        for (auto* c : std::initializer_list<juce::Component*> { bands[b].type, bands[b].frequency, bands[b].gain, bands[b].q })
        {
            c->setBounds (inside.removeFromTop (fieldHeight));
            inside.removeFromTop (fieldGap);
        }
    }
}

} // namespace ui

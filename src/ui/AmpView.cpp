// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AmpView.h"

namespace ui
{

using namespace theme;

namespace
{
/// The x of a frequency on the 20 Hz to 20 kHz log axis (the reference's fx(), as a fraction).
float logPosition (double f)
{
    return (float) ((std::log10 (f) - std::log10 (20.0)) / 3.0);
}

/// The knob captions, in the order across the panel, and each one's parameter name in the slot.
struct AmpKnobSpec
{
    const char* caption;
    const char* parameter;
    float rangeDb; // the parameter's range is -rangeDb..+rangeDb; the knob shows 0 to 10 over it
};
const std::array<AmpKnobSpec, AmpView::numKnobs> ampKnobs { {
    { "Gain", "input_trim", 24.0f },
    { "Bass", "bass", ampsim::AmpTone::rangeDb },
    { "Middle", "mid", ampsim::AmpTone::rangeDb },
    { "Treble", "treble", ampsim::AmpTone::rangeDb },
    { "Presence", "presence", ampsim::AmpTone::rangeDb },
    { "Depth", "depth", ampsim::AmpTone::rangeDb },
    { "Master", "output_trim", 24.0f },
} };

// The page's vertical stack (handoff 4.5), in the page's coordinates (the main area, 1200 x 598): the
// tabs (33), 16, the stage (290), 16, the info row (17), 16, the spectrum (to 16 above the strip), the
// strip (87: a 1 px line, 12 px, a 74 px small knob).
constexpr int tabsHeight = TabRow::height;
constexpr int stageTop = tabsHeight + 16, stageHeight = 290;
constexpr int infoTop = stageTop + stageHeight + 16, infoHeight = 17;
constexpr int curveTop = infoTop + infoHeight + 16;
constexpr int stripHeight = 1 + 12 + 74;
} // namespace

// ---- SpectrumView --------------------------------------------------------------------------------

SpectrumView::SpectrumView (AmpSimProcessor& p) : ampSim (p)
{
    setInterceptsMouseClicks (false, false);
}

SpectrumView::~SpectrumView()
{
    stopTimer();
}

void SpectrumView::setActive (bool shouldRead)
{
    if (shouldRead == active)
        return;
    active = shouldRead;
    if (active)
    {
        // The chain's output (the post section times the output level: the level is relative here anyway).
        ampSim.getAnalyzerRing().discardAll();
        analyzer.reset();
        reference = -300.0f;
        ampSim.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::postSection);
        startTimerHz (meterFps);
    }
    else
    {
        stopTimer();
        ampSim.setAnalyzerTap (AmpSimProcessor::AnalyzerTap::off);
    }
}

juce::Rectangle<float> SpectrumView::plotArea() const
{
    // CSS: a 1 px line on top, 10 px of padding, the plot, then 4 px and a 14 px row of labels.
    return getLocalBounds().toFloat().withTrimmedTop (11.0f).withTrimmedBottom (18.0f);
}

void SpectrumView::update()
{
    if (analyzer.pull (ampSim.getAnalyzerRing()) <= 0)
        return;
    analyzer.compute();

    // One value per 4 px column, averaged in power over the 1/6 octave around it (fractional-octave
    // smoothing; below a few hundred Hz, where a bin is wider than that, interpolated between bins), on top
    // of the analyzer's own smoothing over time.
    const auto width = juce::jmax (8, (int) plotArea().getWidth());
    const auto count = width / 4 + 1;
    columns.resize ((size_t) count);
    const auto& spectrum = analyzer.getSpectrum();
    const auto binHz = ampsim::NamAmp::requiredSampleRate / SpectrumAnalyzer::fftSize;
    const auto halfBand = std::pow (2.0, 1.0 / 12.0);
    double sum = 0.0;
    int n = 0;
    for (int c = 0; c < count; ++c)
    {
        const auto f = 20.0 * std::pow (1000.0, (double) c / (double) (count - 1));
        const auto lo = (int) std::ceil (f / halfBand / binHz), hi = (int) std::floor (f * halfBand / binHz);
        float db;
        if (hi - lo >= 1)
        {
            double power = 0.0;
            for (int b = lo; b <= juce::jmin (hi, (int) spectrum.size() - 1); ++b)
                power += std::pow (10.0, spectrum[(size_t) b] / 10.0);
            db = (float) (10.0 * std::log10 (juce::jmax (1.0e-30, power / (double) (hi - lo + 1))));
        }
        else
        {
            const auto position = f / binHz;
            const auto b = juce::jlimit (0, (int) spectrum.size() - 2, (int) position);
            const auto t = (float) (position - b);
            db = spectrum[(size_t) b] + t * (spectrum[(size_t) b + 1] - spectrum[(size_t) b]);
        }
        columns[(size_t) c] = db;
        if (f >= 100.0 && f <= 5000.0)
        {
            sum += db;
            ++n;
        }
    }

    // The dashed line is the average between 100 Hz and 5 kHz, followed slowly (about a third of a second)
    // so the curve doesn't bob with each pick; below -100 dBFS there's nothing to show.
    const auto mean = n > 0 ? (float) (sum / n) : -300.0f;
    if (mean < -100.0f)
        reference = -300.0f;
    else
        reference = reference < -200.0f ? mean : reference + 0.1f * (mean - reference);
    repaint();
}

float SpectrumView::relativeLevelAt (double frequency) const
{
    if (! hasCurve() || columns.empty())
        return -std::numeric_limits<float>::infinity();
    const auto c = juce::jlimit (0, (int) columns.size() - 1, juce::roundToInt (logPosition (frequency) * (float) (columns.size() - 1)));
    return columns[(size_t) c] - reference;
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (line1);
    g.fillRect (bounds.withHeight (1.0f));

    const auto plot = plotArea();
    const auto xOf = [&plot] (double f) { return plot.getX() + logPosition (f) * plot.getWidth(); };

    // Gridlines at 100 Hz, 1 kHz, 10 kHz, and the dashed 0 dB line through the middle.
    g.setColour (line1);
    for (const auto f : { 100.0, 1000.0, 10000.0 })
        g.fillRect (juce::Rectangle<float> (std::round (xOf (f)), plot.getY(), 1.0f, plot.getHeight()));
    {
        juce::Path zero;
        zero.startNewSubPath (plot.getX(), plot.getCentreY());
        zero.lineTo (plot.getRight(), plot.getCentreY());
        const float dashes[] { 3.0f, 5.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (zero, zero, dashes, 2);
        g.fillPath (zero);
    }

    // The curve and its fill (handoff 6: a 1.5 px emerald line, a faint emerald fill fading out downwards).
    if (hasCurve() && columns.size() > 1)
    {
        const auto yOf = [&plot] (float db)
        {
            const auto t = juce::jlimit (-1.0f, 1.0f, db / rangeDb);
            return plot.getCentreY() - t * plot.getHeight() * 0.48f;
        };
        juce::Path curve;
        for (size_t c = 0; c < columns.size(); ++c)
        {
            const juce::Point<float> p { plot.getX() + plot.getWidth() * (float) c / (float) (columns.size() - 1), yOf (columns[c] - reference) };
            c == 0 ? curve.startNewSubPath (p) : curve.lineTo (p);
        }
        auto fill = curve;
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.lineTo (plot.getX(), plot.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.16f), 0.0f, plot.getY(), accent.withAlpha (0.0f), 0.0f, plot.getBottom(), false));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // The labels, centred on their gridlines, and the title over the top left.
    g.setFont (geist (Weight::regular, 11.0f));
    g.setColour (inkFaint);
    for (const auto& [f, label] : { std::pair<double, const char*> { 100.0, "100 Hz" }, { 1000.0, "1 kHz" }, { 10000.0, "10 kHz" } })
        g.drawText (label, juce::Rectangle<float> (xOf (f) - 40.0f, bounds.getBottom() - 14.0f, 80.0f, 14.0f), juce::Justification::centred, false);
    g.setFont (geist (Weight::regular, 12.0f));
    g.drawText ("Output spectrum", juce::Rectangle<float> (0.0f, 10.0f, 200.0f, 16.0f), juce::Justification::centredLeft, false);
}

// ---- The info row --------------------------------------------------------------------------------

/// The capture's voice on the left (14 px ink-dim); "Model <file>" and the rate (or a message) on the
/// right (13 px faint, the file in ink-dim). The file name is clickable, like the grille.
class AmpView::InfoRow final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit InfoRow (AmpView& v) : view (v) {}

    void set (const juce::String& newVoice, const juce::String& newModel, const juce::String& newRate)
    {
        if (newVoice != voice || newModel != model || newRate != rate)
        {
            voice = newVoice;
            model = newModel;
            rate = newRate;
            setTooltip (rate.contains (" ") ? rate : juce::String());
            repaint();
        }
    }

    juce::String voice, model, rate;

    juce::Rectangle<float> modelArea() const
    {
        const auto rateWidth = textWidth (small(), rate);
        const auto modelWidth = textWidth (small(), model);
        const auto right = (float) getWidth() - 2.0f - rateWidth - 18.0f;
        return { right - modelWidth, 0.0f, modelWidth, (float) getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        // Baselines aligned (CSS align-items: baseline): the 14 px voice and the 13 px rest share one.
        const auto h = (float) getHeight();
        const auto baseline = h - 4.0f;
        const auto draw = [&g, baseline] (const juce::String& words, const juce::FontOptions& f, float x, float w)
        {
            g.setFont (f);
            const juce::Font font (f);
            g.drawText (words, juce::Rectangle<float> (x, baseline - font.getAscent(), w + 2.0f, font.getAscent() + font.getDescent()),
                        juce::Justification::topLeft, false);
        };
        g.setColour (inkDim);
        draw (voice, geist (Weight::regular, 14.0f), 2.0f, (float) getWidth() * 0.6f);

        const auto rateWidth = textWidth (small(), rate);
        g.setColour (inkFaint);
        draw (rate, small(), (float) getWidth() - 2.0f - rateWidth, rateWidth);
        const auto m = modelArea();
        g.setColour (hovered ? ink : inkDim);
        draw (model, small(), m.getX(), m.getWidth());
        const auto label = juce::String ("Model ");
        g.setColour (inkFaint);
        draw (label, small(), m.getX() - textWidth (small(), label), textWidth (small(), label));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto over = modelArea().contains (e.position);
        if (over != hovered)
        {
            hovered = over;
            setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = false;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! modelArea().contains (e.position))
            return;
        if (e.mods.isPopupMenu())
        {
            if (view.onCaptureMenu)
                view.onCaptureMenu (view.shownSlot, *this);
        }
        else if (view.onLoadCapture)
        {
            view.onLoadCapture (view.shownSlot);
        }
    }

private:
    static juce::FontOptions small() { return geist (Weight::regular, 13.0f); }
    AmpView& view;
    bool hovered = false;
};

/// The grille (and its badge): a click loads a capture; a right-click opens the capture menu.
class AmpView::Grille final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Grille (AmpView& v) : view (v) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        if (e.mods.isPopupMenu())
        {
            if (view.onCaptureMenu)
                view.onCaptureMenu (view.shownSlot, *this);
        }
        else if (view.onLoadCapture)
        {
            view.onLoadCapture (view.shownSlot);
        }
    }

private:
    AmpView& view;
};

/// The strip's 6 px gate light: emerald while Gate A lets the guitar through.
class AmpView::GateLight final : public juce::Component, public juce::SettableTooltipClient
{
public:
    GateLight() { setTooltip ("Gate A: lit while it lets the guitar through (its gain above -6 dB)"); }
    void set (bool on)
    {
        if (on != lit)
        {
            lit = on;
            repaint();
        }
    }
    void paint (juce::Graphics& g) override
    {
        g.setColour (lit ? accent : line2);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (getLocalBounds().toFloat().getCentre()));
    }

private:
    bool lit = false;
};

// ---- AmpView -------------------------------------------------------------------------------------

AmpView::AmpView (AmpSimProcessor& p) : ControlGroup (p), tabs (p), spectrum (p)
{
    addAndMakeVisible (tabs);
    std::vector<TabRow::Item> items;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        items.push_back ({ s, materialName (materialFor (s)), "PC " + juce::String (s + 1), {}, false });
    tabs.iconWidth = 30;
    tabs.iconHeight = 20;
    tabs.drawIcon = [] (juce::Graphics& g, juce::Rectangle<float> box, int id) { drawMiniHead (g, box, materialFor (id)); };
    tabs.setItems (items);
    tagged (tabs, AmpSimProcessor::slotParamId); // a right-click on the tabs learns the slot
    tabs.onSelect = [this] (int slot) { setAsGesture (state, AmpSimProcessor::slotParamId, (float) slot); };

    addAndMakeVisible (head);
    addAndMakeVisible (jewel);
    grille = std::make_unique<Grille> (*this);
    addAndMakeVisible (*grille);

    // Each slot's seven knobs, in its own material's skin, shown only while the slot plays.
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        for (int k = 0; k < numKnobs; ++k)
        {
            const auto& spec = ampKnobs[(size_t) k];
            auto& knob = addKnob (AmpSimProcessor::ampParamId (s, spec.parameter), spec.caption, " dB", Knob::Size::normal);
            knob.setSkin (skinFor (materialFor (s)));
            const auto range = spec.rangeDb;
            knob.setFormatter ([range] (float db) { return juce::String ((db + range) / (2.0f * range) * 10.0f, 1); });
            knob.setTooltip (juce::String (spec.caption) + (k == 0 ? ": the capture's input trim, 0 to 10 over -24 to +24 dB (5.0 is unity)"
                                                            : k == numKnobs - 1 ? ": the capture's output trim, 0 to 10 over -24 to +24 dB (5.0 is unity)"
                                                                                : ": 0 to 10 over -12 to +12 dB (5.0 is flat)"));
            knob.setVisible (false);
            knobs[(size_t) s][(size_t) k] = &knob;
        }

    info = std::make_unique<InfoRow> (*this);
    addAndMakeVisible (*info);
    addAndMakeVisible (spectrum);

    // The shared strip (handoff 4.5, without the Doubler: ASSUMPTIONS UH7).
    input = &addKnob ("input_gain", "Input", " dB", Knob::Size::compact);
    threshold = &addKnob ("gate_a_threshold", "Threshold", " dB", Knob::Size::compact);
    threshold->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)) + " dB"; });
    release = &addKnob ("gate_a_release", "Release", " ms", Knob::Size::compact);
    release->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)) + " ms"; });
    output = &addKnob ("output_gain", "Output", " dB", Knob::Size::compact);
    gateLight = std::make_unique<GateLight>();
    addAndMakeVisible (*gateLight);

    showSlot (0);
    refresh();
}

AmpView::~AmpView()
{
    stopTimer();
    spectrum.setActive (false);
}

void AmpView::showSlot (int slot)
{
    slot = juce::jlimit (0, AmpSimProcessor::numAmpSlots - 1, slot);
    if (slot == shownSlot)
        return;
    shownSlot = slot;
    tabs.setSelected (slot);
    head.setMaterial (materialFor (slot));
    grille->setTooltip ("Click to load a .nam capture into " + juce::String (materialName (materialFor (slot))) + " (amp slot " + juce::String (slot + 1)
                        + "); right-click to reload or clear it");
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        for (auto* knob : knobs[(size_t) s])
            knob->setVisible (s == slot);
}

void AmpView::setStatus (const juce::String& message)
{
    info->set (info->voice, info->model, message.isEmpty() ? juce::String ("48 kHz") : message);
}

juce::Component& AmpView::getGrille() noexcept
{
    return *grille;
}

juce::String AmpView::getVoiceText() const { return info->voice; }
juce::String AmpView::getModelText() const { return info->model; }
juce::String AmpView::getRateText() const { return info->rate; }

juce::String AmpView::toneTypeOf (const juce::File& namFile)
{
    // NAM's metadata names the kind of tone ("clean", "overdrive", "crunch", "hi_gain", "fuzz"). Only that
    // is shown: the gear's make and model never reach the UI.
    const auto type = juce::JSON::parse (namFile.loadFileAsString()).getProperty ("metadata", {}).getProperty ("tone_type", {}).toString().trim();
    if (type.isEmpty())
        return {};
    if (type == "hi_gain")
        return "High gain";
    const auto words = type.replaceCharacter ('_', ' ').toLowerCase();
    return words.substring (0, 1).toUpperCase() + words.substring (1);
}

void AmpView::refresh()
{
    showSlot (juce::roundToInt (state.getRawParameterValue (AmpSimProcessor::slotParamId)->load()));

    // The capture: lit while one is loaded; its voice from the metadata, its file.
    const auto status = ampSim.getStatus();
    const auto& line = status.model[(size_t) shownSlot];
    const auto failed = status.modelError[(size_t) shownSlot];
    const auto path = state.state.getProperty (AmpSimProcessor::modelPathKey (shownSlot)).toString();
    const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    const auto loading = line.startsWith ("Loading");
    const auto loaded = file != juce::File() && ! failed && line != "Empty" && ! loading;
    jewel.setLit (loaded);
    jewel.setTooltip (loaded ? "A capture is loaded" : "No capture loaded");

    juce::String voice, model;
    if (file == juce::File() || line == "Empty")
    {
        voice = "No capture loaded";
        model = "none, click the grille to load one";
    }
    else
    {
        // A capture that ships with the app shows as "Glass (built in)"; any other, by its file name.
        model = presets::isBundled (file) ? file.getFileNameWithoutExtension() + " (built in)" : file.getFileName();
        if (failed)
            voice = line;
        else if (loading)
            voice = "Loading...";
        else
        {
            if (toneTypes.find (path) == toneTypes.end())
                toneTypes[path] = toneTypeOf (file);
            voice = toneTypes[path].isNotEmpty() ? toneTypes[path] : juce::String ("Capture loaded");
        }
    }
    info->set (voice, model, info->rate.isEmpty() ? juce::String ("48 kHz") : info->rate);
}

void AmpView::pageShown()
{
    spectrum.setActive (true);
    startTimerHz (meterFps);
}

void AmpView::pageHidden()
{
    spectrum.setActive (false);
    stopTimer();
}

void AmpView::timerCallback()
{
    gateOpen = ampSim.isGateOpen();
    gateLight->set (gateOpen);
}

void AmpView::paint (juce::Graphics& g)
{
    // The stage's glow: radial-gradient(ellipse 60% 70% at 50% 100%, emerald .05, transparent 70%).
    {
        const auto stage = stageArea.toFloat();
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (stageArea);
        const auto rx = stage.getWidth() * 0.6f, ry = stage.getHeight() * 0.7f;
        const juce::Point<float> centre { stage.getCentreX(), stage.getBottom() };
        g.addTransform (juce::AffineTransform::scale (1.0f, ry / rx, centre.x, centre.y));
        juce::ColourGradient glow (accent.withAlpha (0.05f), centre, accent.withAlpha (0.0f), centre.translated (rx, 0.0f), true);
        glow.clearColours();
        glow.addColour (0.0, accent.withAlpha (0.05f));
        glow.addColour (0.7, accent.withAlpha (0.0f));
        glow.addColour (1.0, accent.withAlpha (0.0f));
        g.setGradientFill (glow);
        g.fillRect (juce::Rectangle<float> (centre.x - rx, centre.y - rx, 2.0f * rx, 2.0f * rx));
    }

    // The strip: its line, the group names, the dividers.
    g.setColour (line1);
    g.fillRect (stripArea.withHeight (1));
    const auto knobsTop = stripArea.getY() + 13;
    for (size_t i = 0; i < 2; ++i) // the last group has none
        g.fillRect (dividers[i], knobsTop, 1, 74);
    g.setFont (geist (Weight::regular, 12.0f));
    g.setColour (inkFaint);
    for (const auto& [x, name] : { std::pair<int, const char*> { 0, "Input" }, { dividers[0] + 27, "Gate" }, { dividers[1] + 27, "Output" } })
        g.drawText (name, juce::Rectangle<int> (x, knobsTop, 52, 74), juce::Justification::centredLeft, false);
}

void AmpView::resized()
{
    tabs.setBounds (0, 0, getWidth(), tabsHeight);
    stageArea = { 0, stageTop, getWidth(), stageHeight };

    // The head, centred at the bottom of the stage.
    const auto headX = (getWidth() - AmpHead::headWidth) / 2, headY = stageArea.getBottom() - AmpHead::headHeight;
    head.setBounds (headX - AmpHead::margin.getLeft(), headY - AmpHead::margin.getTop(), AmpHead::headWidth + AmpHead::margin.getLeftAndRight(),
                    AmpHead::headHeight + AmpHead::margin.getTopAndBottom());
    const auto inHead = [&] (juce::Rectangle<float> r) { return r.translated ((float) head.getX(), (float) head.getY()).toNearestInt(); };
    grille->setBounds (inHead (AmpHead::grilleBox()));

    // The panel: padding 0 22; the 96 px side column holds the jewel (centred, as the CSS does without
    // the toggles: UH4), 20 px, then the knobs spread evenly (space-between) over the rest.
    const auto panel = AmpHead::panelBox().translated ((float) head.getX(), (float) head.getY());
    jewel.setBounds (juce::Rectangle<int> (30, 30).withCentre ({ juce::roundToInt (panel.getX() + 22.0f + 9.0f), juce::roundToInt (panel.getCentreY()) }));
    const auto knobsLeft = panel.getX() + 22.0f + 96.0f + 20.0f, knobsWidth = panel.getRight() - 22.0f - knobsLeft;
    const auto css = Knob::cssSize (Knob::Size::normal);
    for (auto& set : knobs)
        for (int k = 0; k < numKnobs; ++k)
            set[(size_t) k]->setCssPosition (juce::roundToInt (knobsLeft + (knobsWidth - (float) css.x) * (float) k / (float) (numKnobs - 1)),
                                             juce::roundToInt (panel.getCentreY() - (float) css.y * 0.5f));

    info->setBounds (0, infoTop, getWidth(), infoHeight);
    stripArea = { 0, getHeight() - stripHeight, getWidth(), stripHeight };
    curveArea = { 0, curveTop, getWidth(), stripArea.getY() - 16 - curveTop };
    spectrum.setBounds (curveArea);

    // The strip's groups (CSS .grp: gap 2, padding-right 26, a 1 px border, margin-right 26): a 52 px name,
    // the small knobs, the gate's light 10 px after its knobs.
    const auto knobsTop = stripArea.getY() + 13;
    auto x = 52 + 2;
    input->setCssPosition (x, knobsTop);
    x += 68 + 26;
    dividers[0] = x;
    x += 1 + 26 + 52 + 2;
    threshold->setCssPosition (x, knobsTop);
    x += 68 + 2;
    release->setCssPosition (x, knobsTop);
    x += 68 + 2 + 10;
    gateLight->setBounds (juce::Rectangle<int> (14, 14).withCentre ({ x + 3, knobsTop + 37 }));
    x += 6 + 26;
    dividers[1] = x;
    x += 1 + 26 + 52 + 2;
    output->setCssPosition (x, knobsTop);
    dividers[2] = x + 68 + 26; // (the last group has no divider)
}

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AmpView.h"
#include "LookAndFeel.h"
#include "dsp/GainSet.h"

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

    /// Where everything goes, right to left: the rate or message at the right edge, "Model <file>" left of it,
    /// the voice from the left edge. The voice is never covered: whatever's left between it and the model goes
    /// to the message, which is cut short with "..." when it doesn't fit (its tooltip has all of it). If even a
    /// short message doesn't fit beside the model, the model gives way to the message. Found 2026-10-06: a
    /// factory preset's notes, drawn at full width from the right edge, ran over the voice and pushed the model
    /// name off the left.
    struct Layout
    {
        float rateX = 0.0f, rateWidth = 0.0f;
        juce::Rectangle<float> model; // empty when the model gives way
    };

    Layout layout() const
    {
        constexpr float gap = 18.0f, edge = 2.0f, minimumMessage = 120.0f;
        const auto width = (float) getWidth();
        const auto left = edge + textWidth (geist (Weight::regular, 14.0f), voice) + 24.0f; // the voice's end, plus room
        const auto labelWidth = textWidth (small(), "Model ");
        const auto modelWidth = model.isEmpty() ? 0.0f : textWidth (small(), model);
        const auto modelBlock = model.isEmpty() ? 0.0f : labelWidth + modelWidth + gap;

        Layout l;
        auto rateSpace = width - edge - left - modelBlock;
        auto showModel = ! model.isEmpty();
        if (rateSpace < std::min (minimumMessage, textWidth (small(), rate)) && showModel)
        {
            showModel = false; // a message too long for the space beside the model: the model gives way
            rateSpace = width - edge - left;
        }
        l.rateWidth = juce::jlimit (0.0f, std::max (0.0f, rateSpace), textWidth (small(), rate));
        l.rateX = width - edge - l.rateWidth;
        if (showModel)
            l.model = { l.rateX - gap - modelWidth, 0.0f, modelWidth, (float) getHeight() };
        return l;
    }

    juce::Rectangle<float> modelArea() const { return layout().model; }

    void paint (juce::Graphics& g) override
    {
        // Baselines aligned (CSS align-items: baseline): the 14 px voice and the 13 px rest share one.
        const auto h = (float) getHeight();
        const auto baseline = h - 4.0f;
        const auto draw = [&g, baseline] (const juce::String& words, const juce::FontOptions& f, float x, float w)
        {
            g.setFont (f);
            const juce::Font font (f);
            // Ellipses: a message wider than the room it's given ends in "..." instead of running on.
            g.drawText (words, juce::Rectangle<float> (x, baseline - font.getAscent(), w + 2.0f, font.getAscent() + font.getDescent()),
                        juce::Justification::topLeft, true);
        };
        const auto l = layout();
        g.setColour (inkDim);
        draw (voice, geist (Weight::regular, 14.0f), 2.0f, textWidth (geist (Weight::regular, 14.0f), voice));

        g.setColour (inkFaint);
        draw (rate, small(), l.rateX, l.rateWidth);
        if (! l.model.isEmpty())
        {
            g.setColour (hovered ? ink : inkDim);
            draw (model, small(), l.model.getX(), l.model.getWidth());
            const auto label = juce::String ("Model ");
            g.setColour (inkFaint);
            draw (label, small(), l.model.getX() - textWidth (small(), label), textWidth (small(), label));
        }
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

// ---- The amp shelf -------------------------------------------------------------------------------

/// The shelf (AmpView::getShelf): "Amp" in 12 px faint on the 20 px content line, then the minis, then the playing
/// amp's name (15 px medium ink) or the hovered one's name and description (ink-dim). The playing mini has a 2 px
/// orange underline 6 px under it.
class AmpView::Shelf final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Shelf (AmpView& v) : view (v) { setRepaintsOnMouseActivity (false); }

    static constexpr float miniWidth = 30.0f, miniHeight = 20.0f, spacing = 12.0f, captureGap = 28.0f, line = 20.0f;

    void set (int newPlaying, bool newShowCapture)
    {
        if (newPlaying != playing || newShowCapture != showCapture)
        {
            playing = newPlaying;
            showCapture = newShowCapture;
            repaint();
        }
    }

    bool shows (int amp) const { return amp >= 0 && (amp < AmpSimProcessor::numBuiltInAmps || (amp == AmpSimProcessor::yourCaptureAmp && showCapture)); }

    juce::Rectangle<float> miniBox (int amp) const
    {
        auto x = std::ceil (textWidth (labelFont(), "Amp")) + 14.0f; // whole pixels, so the minis sit exactly 12 apart
        x += (float) juce::jmin (amp, AmpSimProcessor::numBuiltInAmps) * (miniWidth + spacing);
        if (amp >= AmpSimProcessor::numBuiltInAmps)
            x += captureGap - spacing; // a small gap before your capture
        return { x, (line - miniHeight) * 0.5f, miniWidth, miniHeight };
    }

    int ampAt (juce::Point<float> p) const
    {
        for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
            if (shows (a) && miniBox (a).expanded (spacing * 0.5f, 6.0f).contains (p))
                return a;
        return -1;
    }

    juce::String text() const
    {
        if (hovered >= 0)
        {
            const auto description = view.ampDescription (hovered);
            return AmpSimProcessor::ampName (hovered) + (description.isNotEmpty() ? ": " + description : juce::String());
        }
        return AmpSimProcessor::ampName (playing);
    }

    void setHovered (int amp)
    {
        if (amp != hovered)
        {
            hovered = amp;
            setTooltip (amp >= 0 ? text() : juce::String());
            setMouseCursor (amp >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (labelFont());
        g.setColour (inkFaint);
        g.drawText ("Amp", juce::Rectangle<float> (0.0f, 0.0f, textWidth (labelFont(), "Amp") + 2.0f, line), juce::Justification::centredLeft, false);

        for (int a = 0; a < AmpSimProcessor::numAmps; ++a)
        {
            if (! shows (a))
                continue;
            const auto box = miniBox (a);
            {
                // The ones not playing a little dimmer, so the playing one reads first; the hovered one at full.
                const juce::Graphics::ScopedSaveState saved (g);
                if (a != playing && a != hovered)
                    g.beginTransparencyLayer (0.62f);
                drawMiniAmp (g, box, AmpSimProcessor::ampName (a));
                if (a != playing && a != hovered)
                    g.endTransparencyLayer();
            }
            if (a == playing)
            {
                g.setColour (accent);
                g.fillRect (juce::Rectangle<float> (box.getX(), box.getBottom() + 6.0f, box.getWidth(), 2.0f));
            }
        }

        const auto last = shows (AmpSimProcessor::yourCaptureAmp) ? AmpSimProcessor::yourCaptureAmp : AmpSimProcessor::numBuiltInAmps - 1;
        const auto x = miniBox (last).getRight() + 18.0f;
        const auto words = text();
        g.setFont (hovered >= 0 ? geist (Weight::regular, 14.0f) : nameFont());
        g.setColour (hovered >= 0 ? inkDim : ink);
        g.drawText (words, juce::Rectangle<float> (x, 0.0f, (float) getWidth() - x, line), juce::Justification::centredLeft, true);
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (ampAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return; // the editor's right-click: MIDI learn for amp_model (the shelf is tagged with it)
        if (const auto amp = ampAt (e.position); amp >= 0)
            view.clickMini (amp);
    }

    int playing = 0, hovered = -1;
    bool showCapture = false;

private:
    static juce::FontOptions nameFont() { return geist (Weight::medium, 15.0f); }
    static juce::FontOptions labelFont() { return geist (Weight::regular, 12.0f); }
    AmpView& view;
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

/// The Gain knob's step dots (ASSUMPTIONS AG9): with a gain set, a 3 px dot 5 px outside the knob's arc at each
/// step's position, the one it plays (on a step) or the two it blends (between steps) in orange, the others in
/// the panel's label colour at 45%. Drawn over the knob, never taking its clicks.
class AmpView::GainSteps final : public juce::Component
{
public:
    static constexpr int pad = 6; // around the knob's bounds, room for the dots

    GainSteps() { setInterceptsMouseClicks (false, false); }

    void set (const std::vector<float>& newSteps, float newPosition, juce::Colour newColour)
    {
        if (newSteps == steps && juce::exactlyEqual (newPosition, position) && newColour == colour)
            return;
        steps = newSteps;
        position = newPosition;
        colour = newColour;
        repaint();
    }

    const std::vector<float>& getSteps() const noexcept { return steps; }

    /// Which steps the position plays: itself on a step, both neighbours between two.
    std::vector<bool> lit() const
    {
        std::vector<bool> on (steps.size(), false);
        for (size_t k = 0; k < steps.size(); ++k)
        {
            if (std::abs (steps[k] - position) < 0.01f)
            {
                on[k] = true;
                return on;
            }
            if (k + 1 < steps.size() && steps[k] < position && position < steps[k + 1] && std::abs (steps[k + 1] - position) >= 0.01f)
            {
                on[k] = on[k + 1] = true;
                return on;
            }
        }
        if (! steps.empty())
            on[position < steps.front() ? 0 : steps.size() - 1] = true; // outside the set's range: its end
        return on;
    }

    void paint (juce::Graphics& g) override
    {
        // The knob's geometry (Knob::paint): its 64 px hit area at the top of its box, the arc at radius 32
        // from -135 to +135 degrees, clockwise from 12 o'clock.
        const auto cx = (float) getWidth() * 0.5f, cy = (float) (pad + Knob::margin + 32);
        const auto r = 37.0f;
        const auto on = lit();
        for (size_t k = 0; k < steps.size(); ++k)
        {
            const auto angle = juce::MathConstants<float>::pi * (-0.75f + 1.5f * steps[k] / ampsim::NamAmp::gainMax);
            const juce::Point<float> p { cx + r * std::sin (angle), cy - r * std::cos (angle) };
            g.setColour (on[k] ? accent : colour.withAlpha (0.45f));
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (p));
        }
    }

private:
    std::vector<float> steps;
    float position = 5.0f;
    juce::Colour colour;
};

/// The head and everything on it, at the head's designed size (AmpHead's component box), scaled to the stage by a
/// transform: the art, the jewel, the grille's click area, the seven knobs of each amp, and the Gain step dots.
class AmpView::HeadAssembly final : public juce::Component
{
public:
    HeadAssembly()
    {
        setInterceptsMouseClicks (false, true);
        setSize (AmpHead::headWidth + AmpHead::margin.getLeftAndRight(), AmpHead::headHeight + AmpHead::margin.getTopAndBottom());
    }
};

// ---- The knobs ---------------------------------------------------------------------------------------

juce::String AmpView::knobCaption (int index)
{
    return ampKnobs[(size_t) juce::jlimit (0, numKnobs - 1, index)].caption;
}

juce::String AmpView::knobParameter (int amp, int index)
{
    return AmpSimProcessor::ampParamId (amp, ampKnobs[(size_t) juce::jlimit (0, numKnobs - 1, index)].parameter);
}

void AmpView::dressKnob (Knob& knob, int k)
{
    const auto& spec = ampKnobs[(size_t) juce::jlimit (0, numKnobs - 1, k)];
    const auto range = spec.rangeDb;
    knob.setFormatter ([range] (float db) { return juce::String ((db + range) / (2.0f * range) * 10.0f, 1); });
    knob.setTooltip (juce::String (spec.caption) + (k == 0 ? ": with a gain set, moves across its captures, blending the two nearest; with a "
                                                             "single capture, its input trim, -24 to +24 dB. Either way the level stays put"
                                                    : k == numKnobs - 1 ? ": the capture's output trim, 0 to 10 over -24 to +24 dB (5.0 is unity)"
                                                                        : ": 0 to 10 over -12 to +12 dB (5.0 is flat)"));
}

// ---- AmpPanel: the amp in the inspector ----------------------------------------------------------

/// The gate's 6 px open light: orange while Gate A is on and lets the guitar through, dark while it's closing or
/// switched off.
class AmpPanel::GateLight final : public juce::Component, public juce::SettableTooltipClient
{
public:
    GateLight() { setTooltip ("Gate A: lit while it's on and lets the guitar through (its gain above -6 dB)"); }
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
        const auto r = juce::Rectangle<float> (7.0f, 7.0f).withCentre (getLocalBounds().toFloat().getCentre());
        if (lit)
        {
            g.setColour (accent.withAlpha (0.25f));
            g.fillEllipse (r.expanded (3.0f));
        }
        g.setColour (lit ? accent : line2);
        g.fillEllipse (r);
    }

private:
    bool lit = false;
};

AmpPanel::AmpPanel (AmpSimProcessor& p, AmpView& v) : ControlGroup (p), view (v), spectrum (p)
{
    ampOn = &adopt (std::make_unique<Switch> ("On"));
    ampOn->onStateChange = [this] { repaint (0, 0, getWidth(), BlockPage::headerHeight (view.ampDescription (shownAmp), getWidth())); }; // the header's head dims when off
    // The amp's switch is its bypass, inverted: on while the amp plays (the board's amp marker has the same dot).
    ampOn->setClickingTogglesState (true);
    tagged (*ampOn, "amp_bypass");
    ampOn->setTooltip ("The amp on or off (off passes the DI straight to the cab)");
    ampOn->onClick = [this]
    {
        if (auto* bypass = state.getParameter ("amp_bypass"))
        {
            const auto wanted = ampOn->getToggleState() ? 0.0f : 1.0f;
            if (! juce::exactlyEqual (bypass->getValue(), wanted))
            {
                beginUndoStep (state);
                bypass->beginChangeGesture();
                bypass->setValueNotifyingHost (wanted);
                bypass->endChangeGesture();
            }
        }
    };

    // Each amp's seven knobs in the chrome, shown only while the amp plays.
    for (int s = 0; s < AmpSimProcessor::numAmps; ++s)
        for (int k = 0; k < AmpView::numKnobs; ++k)
        {
            auto& knob = addKnob (AmpView::knobParameter (s, k), AmpView::knobCaption (k), " dB", Knob::Size::normal);
            AmpView::dressKnob (knob, k);
            knob.setVisible (false);
            knobs[(size_t) s][(size_t) k] = &knob;
        }

    input = &addKnob ("input_gain", "Input", " dB");
    output = &addKnob ("output_gain", "Output", " dB");
    threshold = &addKnob ("gate_a_threshold", "Threshold", " dB");
    threshold->setFormatter ([] (float x) { return juce::String (juce::roundToInt (x)) + " dB"; });
    release = &addKnob ("gate_a_release", "Release", " ms");
    release->setFormatter ([] (float x) { return juce::String (juce::roundToInt (x)) + " ms"; });
    // Gate A's switch (Sean's play test, 2026-10-04: "the noise gate doesn't work"): Threshold and Release here
    // are Gate A's, which starts off like every effect, so its switch sits with them.
    gateOn = &addSwitch ("gate_a_on", {});
    gateOn->setTooltip ("Gate A on or off (Threshold and Release only act while it's on)");
    gateLight = std::make_unique<GateLight>();
    addAndMakeVisible (*gateLight);
    addAndMakeVisible (spectrum);
    refresh();
}

AmpPanel::~AmpPanel()
{
    stopTimer();
    spectrum.setActive (false);
}

void AmpPanel::layout (InspectorLayout& l)
{
    const auto amp = juce::jlimit (0, AmpSimProcessor::numAmps - 1, shownAmp);
    const auto& k = knobs[(size_t) amp];
    l.hero ({ k[0], k[6] });
    l.section ("Tone");
    l.knobs ({ k[1], k[2], k[3], k[4], k[5] });
    l.section ("Gate A, before the amp", gateOn);
    l.knobs ({ threshold, release });
    if (l.isPlacing())
        gateLight->setBounds (release->getBounds().translated (release->getWidth() / 2 + 18, 0).withSizeKeepingCentre (20, 20).withY (release->getY() + 22));
    l.area (&promptArea, gatePrompt.isNotEmpty() ? 34 : 0);
    l.section ("Input and output");
    l.knobs ({ input, output });
    l.section ("Output spectrum");
    l.area (&spectrumArea, 132);
    if (l.isPlacing())
        spectrum.setBounds (spectrumArea.withTrimmedTop (-10));
}

int AmpPanel::preferredHeight (int width)
{
    InspectorLayout l ({ BlockPage::padding, BlockPage::headerHeight (view.ampDescription (shownAmp), width) + BlockPage::padding,
                         width - 2 * BlockPage::padding, 0 },
                       false);
    layout (l);
    return l.getBottom() + BlockPage::padding + 8;
}

void AmpPanel::resized()
{
    const auto width = getWidth() - 2 * BlockPage::padding;
    ampOn->setBounds (BlockPage::headerSwitchBounds (getWidth(), ampOn->getPreferredWidth()));
    InspectorLayout l ({ BlockPage::padding, BlockPage::headerHeight (view.ampDescription (shownAmp), getWidth()) + BlockPage::padding, width, 0 }, true);
    layout (l);
    texts = l.getTexts();
    repaint();
}

void AmpPanel::paint (juce::Graphics& g)
{
    // The header, as a pedal's: the playing amp's head in small (its own material), its name, its description.
    const auto amp = juce::jmax (0, shownAmp);
    const auto on = ampOn->getToggleState();
    BlockPage::paintHeader (g, getWidth(), AmpSimProcessor::ampName (amp), view.ampDescription (shownAmp),
                            [amp, on] (juce::Graphics& gg, juce::Rectangle<float> box)
                            {
                                const juce::Graphics::ScopedSaveState saved (gg);
                                if (! on)
                                    gg.beginTransparencyLayer (offAlpha);
                                drawSmallHead (gg, box.reduced (2.0f),
                                             AmpSimProcessor::ampName (amp));
                                if (! on)
                                    gg.endTransparencyLayer();
                            },
                            ampOn->getWidth());
    InspectorLayout::paintTexts (g, texts);

    // The gate's first switch-on: the prompt in ink (the result after it in ink-dim), and while Learn measures,
    // its progress as a thin orange bar under the words.
    if (gatePrompt.isNotEmpty() && ! promptArea.isEmpty())
    {
        auto area = promptArea;
        const auto measuring = gateLearnProgress >= 0.0f;
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (measuring ? ink : inkDim);
        g.drawFittedText (gatePrompt, area.removeFromTop (28), juce::Justification::topLeft, 2, 0.9f);
        if (measuring)
        {
            const auto bar = area.withHeight (3).toFloat();
            g.setColour (line2);
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, gateLearnProgress)), 1.5f);
        }
    }
}

void AmpPanel::refresh()
{
    const auto amp = ampSim.getSelectedAmp();
    if (amp != shownAmp)
    {
        shownAmp = amp;
        for (int s = 0; s < AmpSimProcessor::numAmps; ++s)
            for (auto* knob : knobs[(size_t) s])
                knob->setVisible (s == amp);
        resized();
    }
    ampOn->setToggleState (state.getRawParameterValue ("amp_bypass")->load() < 0.5f, juce::dontSendNotification);
    updateGatePrompt();
}

void AmpPanel::pageShown()
{
    spectrum.setActive (true);
    startTimerHz (meterFps);
}

void AmpPanel::pageHidden()
{
    spectrum.setActive (false);
    stopTimer();
}

void AmpPanel::timerCallback()
{
    gateOpen = ampSim.isGateOpen() && state.getRawParameterValue ("gate_a_on")->load() >= 0.5f;
    gateLight->set (gateOpen);
    updateGatePrompt();
}

void AmpPanel::updateGatePrompt()
{
    // The gate's first switch-on (ASSUMPTIONS PT13): the prompt by the switch that started it, with the
    // measurement's progress under it, then the threshold it set. (The info row says the same: UH5.)
    const auto prompt = ampSim.getAutoLearnPrompt();
    const auto progress = prompt == AmpSimProcessor::autoLearnPromptText ? (ampSim.isLearningGates() ? ampSim.getGateLearnProgress() : 0.0f) : -1.0f;
    if (prompt != gatePrompt || std::abs (progress - gateLearnProgress) > 0.005f)
    {
        const auto relayout = prompt.isEmpty() != gatePrompt.isEmpty();
        gatePrompt = prompt;
        gateLearnProgress = progress;
        if (relayout)
            resized();
        repaint (promptArea);
    }
}

// ---- AmpView -------------------------------------------------------------------------------------

Material materialForAmp (int amp)
{
    // Each built-in amp wears its own material (by name), your capture Custom.
    return amp >= 0 && amp < AmpSimProcessor::numBuiltInAmps ? materialForAmp (presets::builtInAmpName (amp)) : Material::custom;
}

AmpView::AmpView (AmpSimProcessor& p) : ControlGroup (p)
{
    shelf = std::make_unique<Shelf> (*this);
    addAndMakeVisible (*shelf);
    tagged (*shelf, AmpSimProcessor::ampModelParamId); // a right-click on the shelf learns the amp choice

    // Each built-in amp's description, from its gainset.json (read once: the content folder doesn't change).
    for (const auto& amp : presets::builtInGainSets())
        for (int a = 0; a < AmpSimProcessor::numBuiltInAmps; ++a)
            if (amp.file == presets::builtInCapture (a))
                descriptions[(size_t) a] = amp.description;

    assembly = std::make_unique<HeadAssembly>();
    addAndMakeVisible (*assembly);
    assembly->addAndMakeVisible (head);
    head.setBounds (assembly->getLocalBounds());
    assembly->addAndMakeVisible (jewel);
    grille = std::make_unique<Grille> (*this);
    assembly->addAndMakeVisible (*grille);

    // Each amp's seven knobs, in its material's skin, shown only while the amp plays.
    for (int s = 0; s < AmpSimProcessor::numAmps; ++s)
        for (int k = 0; k < numKnobs; ++k)
        {
            auto& knob = addKnob (knobParameter (s, k), knobCaption (k), " dB", Knob::Size::normal);
            knob.setSkin (skinFor (materialForAmp (s)));
            dressKnob (knob, k);
            knob.setVisible (false);
            assembly->addChildComponent (knob); // drawn on the head, at its scale
            knobs[(size_t) s][(size_t) k] = &knob;
        }

    gainStepDots = std::make_unique<GainSteps>();
    assembly->addAndMakeVisible (*gainStepDots);

    info = std::make_unique<InfoRow> (*this);
    addAndMakeVisible (*info);

    // The head's layout inside the assembly, at its designed size: the grille, the jewel, the knobs on the panel.
    grille->setBounds (AmpHead::grilleBox().toNearestInt());
    // The panel: padding 0 22; the 96 px side column holds the jewel (centred, as the CSS does without the
    // toggles: UH4), 20 px, then the knobs spread evenly (space-between) over the rest.
    const auto panelBox = AmpHead::panelBox();
    jewel.setBounds (juce::Rectangle<int> (30, 30).withCentre ({ juce::roundToInt (panelBox.getX() + 22.0f + 9.0f), juce::roundToInt (panelBox.getCentreY()) }));
    const auto knobsLeft = panelBox.getX() + 22.0f + 96.0f + 20.0f, knobsWidth = panelBox.getRight() - 22.0f - knobsLeft;
    const auto css = Knob::cssSize (Knob::Size::normal);
    for (auto& set : knobs)
        for (int k = 0; k < numKnobs; ++k)
            set[(size_t) k]->setCssPosition (juce::roundToInt (knobsLeft + (knobsWidth - (float) css.x) * (float) k / (float) (numKnobs - 1)),
                                             juce::roundToInt (panelBox.getCentreY() - (float) css.y * 0.5f));
    gainStepDots->setBounds (knobs[0][0]->getBounds().expanded (GainSteps::pad));

    panel = std::make_unique<AmpPanel> (p, *this);
    showSlot (0);
    refresh();
}

AmpView::~AmpView()
{
    stopTimer();
}

void AmpView::showSlot (int slot)
{
    slot = juce::jlimit (0, AmpSimProcessor::numAmps - 1, slot);
    if (slot == shownSlot)
        return;
    shownSlot = slot;
    head.setMaterial (materialForAmp (slot));
    head.setBadge (AmpSimProcessor::ampName (slot));
    grille->setTooltip (slot == AmpSimProcessor::yourCaptureAmp ? "Click to load a capture of your own (a .nam or a gain set); right-click to reload or remove it"
                                                              : "Click to load a capture of your own (a .nam or a gain set), played as your capture; right-click for more");
    for (int s = 0; s < AmpSimProcessor::numAmps; ++s)
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

juce::Component& AmpView::getShelf() noexcept
{
    return *shelf;
}

juce::Component& AmpView::getHeadAssembly() noexcept
{
    return *assembly;
}

bool AmpView::yourCaptureLoaded() const
{
    // Your capture's mini shows once one is loaded (or loading, or missing: something to pick and see why).
    const auto path = state.state.getProperty (AmpSimProcessor::modelPathKey (AmpSimProcessor::yourCaptureAmp)).toString();
    return path.isNotEmpty();
}

bool AmpView::isOnShelf (int amp) const { return shelf->shows (amp); }

juce::Rectangle<int> AmpView::getMiniBounds (int amp) const
{
    return shelf->miniBox (amp).translated ((float) shelf->getX(), (float) shelf->getY()).getSmallestIntegerContainer();
}

void AmpView::clickMini (int amp)
{
    if (! shelf->shows (amp))
        return;
    setAsGesture (state, AmpSimProcessor::ampModelParamId, (float) amp);
    refresh();
}

juce::String AmpView::getShelfText() const { return shelf->text(); }
void AmpView::hoverMini (int amp) { shelf->setHovered (amp); }

juce::String AmpView::ampDescription (int amp) const
{
    if (amp >= 0 && amp < AmpSimProcessor::numBuiltInAmps)
        return descriptions[(size_t) amp];
    const auto path = state.state.getProperty (AmpSimProcessor::modelPathKey (AmpSimProcessor::yourCaptureAmp)).toString();
    return juce::File::isAbsolutePath (path) ? juce::File (path).getFileName() : juce::String();
}

juce::String AmpView::getVoiceText() const { return info->voice; }
juce::String AmpView::getModelText() const { return info->model; }
juce::String AmpView::getRateText() const { return info->rate; }

const AmpView::SetInfo& AmpView::setInfo (const juce::String& path)
{
    if (const auto it = gainSets.find (path); it != gainSets.end())
        return it->second;

    SetInfo found;
    const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    ampsim::GainSet set;
    juce::String why;
    if (file != juce::File() && ampsim::GainSet::isGainSet (file) && ampsim::GainSet::read (file, set, why) && set.steps.size() > 1)
    {
        found.name = set.name;
        for (const auto& step : set.steps)
            found.steps.push_back ((float) step.gain);
    }
    return gainSets[path] = found;
}

const std::vector<float>& AmpView::gainSteps (int slot)
{
    return setInfo (state.state.getProperty (AmpSimProcessor::modelPathKey (juce::jlimit (0, AmpSimProcessor::numAmps - 1, slot))).toString()).steps;
}

std::vector<float> AmpView::getShownGainSteps() const { return gainStepDots->getSteps(); }
std::vector<bool> AmpView::getLitGainSteps() const { return gainStepDots->lit(); }

void AmpView::updateGainSteps()
{
    if (shownSlot < 0)
        return;
    const auto& knob = *knobs[(size_t) shownSlot][0];
    const auto loaded = ampSim.getStatus().model[(size_t) shownSlot] != "Empty";
    gainStepDots->set (loaded ? gainSteps (shownSlot) : std::vector<float> {},
                       ampsim::AmpSection::GainKnob::positionForDb (knob.getValue()), knobSkin (knob.getSkin()).label);
}

juce::String AmpView::toneTypeOf (const juce::File& namFile)
{
    // NAM's metadata names the kind of tone ("clean", "overdrive", "crunch", "hi_gain", "fuzz"); a gain set
    // says it at the top level. Only that is shown: the gear's make and model never reach the UI.
    const auto json = juce::JSON::parse (namFile.loadFileAsString());
    const auto type = (json.hasProperty ("tone_type") ? json.getProperty ("tone_type", {})
                                                      : json.getProperty ("metadata", {}).getProperty ("tone_type", {})).toString().trim();
    if (type.isEmpty())
        return {};
    if (type == "hi_gain")
        return "High gain";
    const auto words = type.replaceCharacter ('_', ' ').toLowerCase();
    return words.substring (0, 1).toUpperCase() + words.substring (1);
}

void AmpView::refresh()
{
    showSlot (ampSim.getSelectedAmp());

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
        // A gain set shows its name: "Glass (built in, gain set)".
        if (const auto& set = setInfo (path); ! set.steps.empty())
            model = set.name + (presets::isBundled (file) ? " (built in, gain set)" : " (gain set)");
        else
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
    updateGainSteps();

    // The shelf: the playing amp underlined, your capture's mini once one is loaded. The badge names the amp; your
    // capture's (Custom) names the capture itself.
    shelf->set (shownSlot, yourCaptureLoaded());
    if (shownSlot == AmpSimProcessor::yourCaptureAmp)
    {
        juce::String name;
        if (file != juce::File())
            name = setInfo (path).name.isNotEmpty() ? setInfo (path).name : file.getFileNameWithoutExtension();
        head.setBadge (name.isNotEmpty() ? name : AmpSimProcessor::ampName (shownSlot));
    }
}

void AmpView::pageShown()
{
    startTimerHz (meterFps);
}

void AmpView::pageHidden()
{
    stopTimer();
}

void AmpView::timerCallback()
{
    updateGainSteps(); // the lit steps follow the knob, whoever turns it
}

void AmpView::paint (juce::Graphics&)
{
    // The stage paints the dark and its light; the head is its own art.
}

void AmpView::resized()
{
    // The shelf on the stage's top line (the stage's Amp / Cab / EQ control is at its right end).
    shelf->setBounds (24, 22, juce::jmin (640, getWidth() - 300), 30);

    // The head, as large as the stage allows with room for its handle and feet, centred over the floor; the info
    // row along the bottom.
    const auto infoHeight = 17, bottomPad = 14;
    info->setBounds (24, getHeight() - bottomPad - infoHeight, getWidth() - 48, infoHeight);
    const auto top = 72.0f, bottom = (float) info->getY() - 22.0f; // room for the handle above and the feet below
    const auto maxWidth = (float) getWidth() - 136.0f;
    headScale = juce::jmin (maxWidth / (float) AmpHead::headWidth, (bottom - top) / (float) AmpHead::headHeight);
    const auto bodyWidth = (float) AmpHead::headWidth * headScale;
    const auto bodyX = ((float) getWidth() - bodyWidth) * 0.5f, bodyY = top;
    assembly->setTopLeftPosition (0, 0);
    assembly->setTransform (juce::AffineTransform::scale (headScale)
                                .translated (bodyX - (float) AmpHead::margin.getLeft() * headScale, bodyY - (float) AmpHead::margin.getTop() * headScale));
}

} // namespace ui

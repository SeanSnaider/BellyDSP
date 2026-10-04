// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatchPage.h"

#include "tonematch/AudioFileInput.h"

namespace ui
{

using namespace theme;
using Mode = ampsim::tonematch::Mode;

namespace
{
juce::String timeText (double seconds)
{
    const auto m = (int) (seconds / 60.0);
    return juce::String (m) + ":" + juce::String (seconds - 60.0 * m, 1).paddedLeft ('0', 4);
}

juce::String signedDb (double v, int decimals = 1)
{
    return (v >= 0.0 ? "+" : "") + juce::String (v, decimals) + " dB";
}

void drawText (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> area, Text style, juce::Colour colour,
               juce::Justification j = juce::Justification::centredLeft)
{
    g.setFont (font (style));
    g.setColour (colour);
    g.drawText (s, area, j, true);
}

void drawWrapped (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> area, Text style, juce::Colour colour)
{
    g.setFont (font (style));
    g.setColour (colour);
    g.drawFittedText (s, area, juce::Justification::topLeft, 4, 1.0f);
}

constexpr int waveformHeight = 96;

const char* const honestNote =
    "A song only holds the rig's output, mixed with everything else, so the real amp can't be recovered. "
    "This finds the closest fingerprint with what BellyDSP has: an amp, its Gain and tone, a built-in cab, and a match EQ.";
} // namespace

// ---- The waveform ---------------------------------------------------------------------------------------------

class ToneMatchPage::Waveform final : public juce::Component
{
public:
    explicit Waveform (ToneMatchSession& s) : session (s) {}

    std::function<void()> onRangeChanged;

    void update()
    {
        version = session.getTargetVersion();
        const auto& x = session.getTarget();
        peaks.assign ((size_t) columns, { 0.0f, 0.0f });
        if (x.empty())
        {
            repaint();
            return;
        }
        for (int c = 0; c < columns; ++c)
        {
            const auto a = x.size() * (size_t) c / (size_t) columns, b = std::max (a + 1, x.size() * (size_t) (c + 1) / (size_t) columns);
            float lo = 0.0f, hi = 0.0f;
            for (size_t i = a; i < b && i < x.size(); ++i)
            {
                lo = std::min (lo, x[i]);
                hi = std::max (hi, x[i]);
            }
            peaks[(size_t) c] = { lo, hi };
        }
        float top = 1.0e-6f;
        for (const auto& [lo, hi] : peaks)
            top = std::max (top, std::max (-lo, hi));
        scale = 1.0f / top;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (bg);
        g.fillRoundedRectangle (r, radiusControl);
        g.setColour (line1);
        g.drawRoundedRectangle (r.reduced (0.5f), radiusControl, 1.0f);

        if (version != session.getTargetVersion())
            update(); // the target changed without going through the page (a test, or a drop elsewhere)
        const auto length = session.getTargetSeconds();
        if (length <= 0.0 || peaks.size() != (size_t) columns)
        {
            drawText (g, "Choose a song or a guitar track, or drop one here", getLocalBounds(), Text::label, inkFaint, juce::Justification::centred);
            return;
        }

        const auto [start, end] = session.getRange();
        const auto x0 = xFor (start), x1 = xFor (end);
        g.setColour (accentDim);
        g.fillRect (juce::Rectangle<float> (x0, r.getY() + 1.0f, x1 - x0, r.getHeight() - 2.0f));

        const auto mid = r.getCentreY(), half = r.getHeight() * 0.5f - 6.0f;
        for (int c = 0; c < columns; ++c)
        {
            const auto x = r.getX() + 2.0f + (r.getWidth() - 4.0f) * ((float) c + 0.5f) / (float) columns;
            const auto [lo, hi] = peaks[(size_t) c];
            g.setColour (x >= x0 && x <= x1 ? ink.withAlpha (0.8f) : inkFaint);
            g.drawVerticalLine (juce::roundToInt (x), mid - half * hi * scale - 0.5f, mid - half * lo * scale + 0.5f);
        }

        g.setColour (accent);
        g.fillRect (juce::Rectangle<float> (x0 - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f));
        g.fillRect (juce::Rectangle<float> (x1 - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f));

        g.setFont (font (Text::caption));
        g.setColour (inkFaint);
        g.drawText ("0:00", getLocalBounds().reduced (6, 3), juce::Justification::bottomLeft, false);
        g.drawText (timeText (length), getLocalBounds().reduced (6, 3), juce::Justification::bottomRight, false);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (session.getTargetSeconds() <= 0.0)
            return;
        const auto [start, end] = session.getRange();
        const auto x = (float) e.x;
        if (std::abs (x - xFor (start)) < 6.0f)
            drag = Drag::start;
        else if (std::abs (x - xFor (end)) < 6.0f)
            drag = Drag::end;
        else if (x > xFor (start) && x < xFor (end))
        {
            drag = Drag::move;
            grabOffset = secondsAt (x) - start;
        }
        else
        {
            drag = Drag::create;
            anchor = secondsAt (x);
        }
        dragged = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto t = secondsAt ((float) e.x);
        const auto [start, end] = session.getRange();
        dragged = true;
        switch (drag)
        {
            case Drag::start:  session.setRange (std::min (t, end - ToneMatchSession::minRangeSeconds), end); break;
            case Drag::end:    session.setRange (start, std::max (t, start + ToneMatchSession::minRangeSeconds)); break;
            case Drag::move:   session.setRange (t - grabOffset, t - grabOffset + (end - start)); break;
            case Drag::create: session.setRange (std::min (anchor, t), std::max (anchor, t)); break;
            case Drag::none:   return;
        }
        repaint();
        if (onRangeChanged)
            onRangeChanged();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A click without a drag puts a default-length range (30 s, or what fits) starting there.
        if (drag == Drag::create && ! dragged)
        {
            session.setRange (secondsAt ((float) e.x), secondsAt ((float) e.x) + ToneMatchSession::defaultRangeSeconds);
            repaint();
            if (onRangeChanged)
                onRangeChanged();
        }
        drag = Drag::none;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto [start, end] = session.getRange();
        const auto nearEdge = std::abs ((float) e.x - xFor (start)) < 6.0f || std::abs ((float) e.x - xFor (end)) < 6.0f;
        setMouseCursor (nearEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

private:
    float xFor (double seconds) const
    {
        const auto length = std::max (1.0e-9, session.getTargetSeconds());
        return 2.0f + (float) (getWidth() - 4) * (float) (seconds / length);
    }

    double secondsAt (float x) const
    {
        return juce::jlimit (0.0, session.getTargetSeconds(), (double) (x - 2.0f) / (double) std::max (1, getWidth() - 4) * session.getTargetSeconds());
    }

    enum class Drag
    {
        none,
        start,
        end,
        move,
        create
    };

    ToneMatchSession& session;
    static constexpr int columns = 480;
    std::vector<std::pair<float, float>> peaks;
    float scale = 1.0f;
    int version = -1;
    Drag drag = Drag::none;
    double anchor = 0.0, grabOffset = 0.0;
    bool dragged = false;
};

// ---- The match EQ curve ------------------------------------------------------------------------------------

class ToneMatchPage::EqCurve final : public juce::Component
{
public:
    void setResult (const ToneMatchSession::MatchResult* r)
    {
        target.clear();
        curve.clear();
        weight.clear();
        if (r != nullptr && r->ok)
        {
            // The fit ignores overall level (it's matched separately), so the curve is drawn shifted by the
            // weighted mean difference to the dots: where it lines up with them is what it fits.
            const auto atCentres = ampsim::tonematch::eqDb (r->eq);
            double num = 0.0, den = 0.0, wMax = 1.0e-9;
            for (int b = 0; b < ampsim::tonematch::numBands; ++b)
            {
                num += r->weights[(size_t) b] * (r->eqTarget[(size_t) b] - atCentres[(size_t) b]);
                den += r->weights[(size_t) b];
                wMax = std::max (wMax, r->weights[(size_t) b]);
            }
            offset = num / den;
            for (int b = 0; b < ampsim::tonematch::numBands; ++b)
            {
                target.push_back ({ ampsim::tonematch::bands().centre[(size_t) b], r->eqTarget[(size_t) b] });
                weight.push_back ((float) (r->weights[(size_t) b] / wMax));
            }
            ampsim::Equalizer::Settings s;
            s.mode = ampsim::Equalizer::Mode::parametric;
            s.bands = r->eq;
            std::vector<double> f;
            for (int i = 0; i < 240; ++i)
                f.push_back (minF * std::pow (maxF / minF, i / 239.0));
            const auto db = ampsim::Equalizer::responseDb (s, f, 48000.0);
            for (size_t i = 0; i < f.size(); ++i)
                curve.push_back ({ f[i], db[i] + offset });
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (bg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), radiusControl);
        g.setColour (line2);
        for (auto f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
            g.drawVerticalLine (juce::roundToInt (xFor (f)), r.getY(), r.getBottom());
        for (auto db : { -12.0, -6.0, 6.0, 12.0 })
            g.drawHorizontalLine (juce::roundToInt (yFor (db)), r.getX(), r.getRight());
        g.setColour (line2.brighter (0.35f));
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0)), r.getX(), r.getRight());

        if (! target.empty())
        {
            // What was left to fit (after the amp, Gain, tone, and cab), smoothed and capped: dots and a thin line.
            juce::Path p;
            for (size_t i = 0; i < target.size(); ++i)
            {
                const juce::Point<float> pt { xFor (target[i].first), yFor (target[i].second) };
                i == 0 ? p.startNewSubPath (pt) : p.lineTo (pt);
            }
            g.setColour (inkDim.withAlpha (0.35f));
            g.strokePath (p, juce::PathStrokeType (1.0f));
            // Each dot as bright as its band counted: a band far below the target's loudest (nothing was
            // played there) barely counts, and is drawn faint.
            for (size_t i = 0; i < target.size(); ++i)
            {
                g.setColour (ink.withAlpha (0.15f + 0.75f * weight[i]));
                g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ xFor (target[i].first), yFor (target[i].second) }));
            }
        }
        if (! curve.empty())
        {
            juce::Path line;
            for (size_t i = 0; i < curve.size(); ++i)
            {
                const juce::Point<float> pt { xFor (curve[i].first), yFor (curve[i].second) };
                i == 0 ? line.startNewSubPath (pt) : line.lineTo (pt);
            }
            auto fill = line;
            fill.lineTo (r.getRight(), yFor (0.0));
            fill.lineTo (r.getX(), yFor (0.0));
            fill.closeSubPath();
            g.setColour (accent.withAlpha (0.16f));
            g.fillPath (fill);
            g.setColour (accent);
            g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setFont (font (Text::caption));
        g.setColour (textDim);
        for (auto [f, label] : std::initializer_list<std::pair<double, const char*>> { { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                                                                       { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" } })
            g.drawText (label, juce::Rectangle<float> (xFor (f) + 3.0f, r.getBottom() - 15.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
        for (auto db : { -12.0, -6.0, 6.0, 12.0 })
            g.drawText ((db > 0 ? "+" : "") + juce::String (juce::roundToInt (db)), juce::Rectangle<float> (r.getX() + 4.0f, yFor (db) - 13.0f, 30.0f, 12.0f),
                        juce::Justification::centredLeft, false);
        g.drawText (target.empty() ? "The match EQ appears here" : "Match EQ (line) over what was left to fit (dots, faint ones barely count)",
                    r.reduced (8.0f, 5.0f).withHeight (13.0f), juce::Justification::centredRight, false);
    }

private:
    float xFor (double f) const
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        return r.getX() + r.getWidth() * (float) (std::log (f / minF) / std::log (maxF / minF));
    }

    float yFor (double db) const
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        return r.getCentreY() - (r.getHeight() * 0.5f - 14.0f) * (float) (juce::jlimit (-rangeDb, rangeDb, db) / rangeDb);
    }

    static constexpr double minF = 40.0, maxF = 16000.0, rangeDb = 15.0;
    std::vector<std::pair<double, double>> target, curve;
    std::vector<float> weight;
    double offset = 0.0;
};

// ---- The page -------------------------------------------------------------------------------------------------

ToneMatchPage::ToneMatchPage (AmpSimProcessor& p)
    : ControlGroup (p), separator (std::make_unique<ampsim::tonematch::GuitarSeparator> (ampsim::tonematch::GuitarSeparator::defaultFolder())),
      session (p)
{
    // Separation (Stage C): on the session's worker thread, install the model the first time (the download's
    // size is said on the page before anyone switches it on), then separate.
    session.setSeparator ([sep = separator.get()] (const std::vector<float>& x, const std::atomic<bool>& cancel,
                                                   const ampsim::tonematch::ProgressFn& progress, juce::String& problem) {
        using GS = ampsim::tonematch::GuitarSeparator;
        const auto installShare = sep->isInstalled() ? 0.0 : 0.3;
        if (! sep->isInstalled())
        {
            problem = sep->install (GS::weightsUrl, cancel, [&] (double f, const juce::String& s) { progress (installShare * f, s); });
            if (problem.isNotEmpty())
                return std::vector<float>();
        }
        return sep->separate (x, cancel, [&] (double f, const juce::String& s) { progress (installShare + (1.0 - installShare) * f, s); }, problem);
    });

    waveform = std::make_unique<Waveform> (session);
    waveform->onRangeChanged = [this] { repaint(); };
    addAndMakeVisible (*waveform);

    eqCurve = std::make_unique<EqCurve>();
    addAndMakeVisible (*eqCurve);

    modeChoice = std::make_unique<Segmented> (juce::StringArray { "Same part", "Anything" });
    modeChoice->setSelected (1);
    modeChoice->onChange = [this] (int i) {
        session.setMode (i == 0 ? Mode::samePart : Mode::anything);
        repaint();
    };
    addAndMakeVisible (*modeChoice);

    separateSwitch = std::make_unique<Switch> ("Separate the guitar first");
    separateSwitch->onClick = [this] {
        session.setSeparate (separateSwitch->getToggleState());
        repaint();
    };
    addChildComponent (*separateSwitch); // shown when the build has a separator

    targetButton = &addButton ("Choose file...", [this] { chooseTarget(); });
    referenceButton = &addButton ("Choose file...", [this] { chooseReference(); });
    recordButton = &addButton ("Record", [this] { toggleRecording(); });
    matchButton = &addButton ("Match", [this] { startMatch(); });
    cancelButton = &addButton ("Cancel", [this] { session.cancel(); });
    applyButton = &addButton ("Apply", [this] { apply(); });
    discardButton = &addButton ("Discard", [this] { discard(); });
    closeButton = &addButton ("Close", [this] {
        if (onClose)
            onClose();
    });
    refresh();
}

ToneMatchPage::~ToneMatchPage()
{
    stopTimer();
}

void ToneMatchPage::pageShown()
{
    separateSwitch->setVisible (session.hasSeparator());
    startTimerHz (15);
}

void ToneMatchPage::pageHidden()
{
    // A recording or a match keeps going while another page shows (the session runs it); only the display stops.
    if (! session.isRecording() && ! session.isMatching())
        stopTimer();
}

void ToneMatchPage::timerCallback()
{
    session.poll();
    refresh();
    repaint (matchCard);
    repaint (referenceCard);
    if (! isShowing() && ! session.isRecording() && ! session.isMatching())
        stopTimer();
}

void ToneMatchPage::chooseTarget()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a song or a guitar track", juce::File(), ampsim::tonematch::AudioFileInput::wildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
        if (fc.getResult() != juce::File())
        {
            session.setTargetFile (fc.getResult());
            targetChanged();
        }
    });
}

void ToneMatchPage::chooseReference()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a DI recording of your playing", juce::File(), ampsim::tonematch::AudioFileInput::wildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
        if (fc.getResult() != juce::File())
        {
            session.setReferenceFile (fc.getResult());
            refresh();
            repaint();
        }
    });
}

void ToneMatchPage::toggleRecording()
{
    if (session.isRecording())
        session.stopRecording();
    else
    {
        session.startRecording();
        startTimerHz (15);
    }
    refresh();
    repaint();
}

void ToneMatchPage::startMatch()
{
    if (session.startMatch())
    {
        applied = false;
        resultText.clear();
        eqCurve->setResult (nullptr);
        startTimerHz (15);
    }
    refresh();
    repaint();
}

void ToneMatchPage::apply()
{
    applied = session.apply();
    refresh();
    repaint();
}

void ToneMatchPage::discard()
{
    session.discard();
    applied = false;
    eqCurve->setResult (nullptr);
    refresh();
    repaint();
}

void ToneMatchPage::targetChanged()
{
    waveform->update();
    refresh();
    repaint();
}

bool ToneMatchPage::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && juce::File (files[0]).existsAsFile();
}

void ToneMatchPage::filesDropped (const juce::StringArray& files, int x, int y)
{
    // Dropped on the DI card: the reference. Anywhere else: the target.
    if (referenceCard.contains (x, y))
        session.setReferenceFile (juce::File (files[0]));
    else
    {
        session.setTargetFile (juce::File (files[0]));
        targetChanged();
    }
    refresh();
    repaint();
}

void ToneMatchPage::updateResultText()
{
    if (! session.hasResult())
    {
        resultText.clear();
        return;
    }
    const auto& r = session.getResult();
    juce::StringArray lines;
    const auto capture = ampSim.getSlotCapture (r.slot).getFileNameWithoutExtension();
    lines.add ("Amp  " + capture + " (slot " + juce::String (r.slot + 1) + "), Gain " + signedDb (r.gainDb));
    juce::StringArray tone;
    for (size_t b = 0; b < ampsim::AmpTone::numBands; ++b)
        tone.add (juce::String (ampsim::AmpTone::bands[b].name) + " " + signedDb (r.tone[b]));
    lines.add ("Tone  " + tone.joinIntoString (", "));
    lines.add ("Cab  " + r.cab.getFileNameWithoutExtension());
    juce::StringArray eq;
    for (const auto& band : r.eq)
        if (std::abs (band.gainDb) >= 0.5f)
            eq.add ((band.type == ampsim::Equalizer::BandType::lowShelf ? "low shelf " : band.type == ampsim::Equalizer::BandType::highShelf ? "high shelf " : "")
                    + juce::String (juce::roundToInt (band.frequency)) + " Hz " + signedDb (band.gainDb));
    lines.add ("Match EQ  " + (eq.isEmpty() ? juce::String ("flat") : eq.joinIntoString (", ")));
    resultText = lines.joinIntoString ("\n");
}

void ToneMatchPage::refresh()
{
    session.poll();
    const auto matching = session.isMatching();
    const auto recording = session.isRecording();
    recordButton->setButtonText (recording ? "Stop" : "Record");
    recordButton->setEnabled (! matching);
    referenceButton->setEnabled (! matching && ! recording);
    targetButton->setEnabled (! matching);
    matchButton->setEnabled (session.whyCantMatch().isEmpty());
    cancelButton->setEnabled (matching);
    applyButton->setEnabled (session.hasResult() && ! matching);
    discardButton->setEnabled (session.hasResult() && ! matching);
    modeChoice->setEnabled (! matching);
    separateSwitch->setEnabled (! matching);

    if (session.hasResult() && resultText.isEmpty())
    {
        updateResultText();
        eqCurve->setResult (&session.getResult());
    }
    else if (! session.hasResult())
        resultText.clear();

    if (session.getError().isNotEmpty())
        statusText = session.getError();
    else if (matching)
        statusText = session.getStage();
    else if (applied)
        statusText = "Applied. Undo puts your settings back.";
    else
        statusText = session.whyCantMatch();
}

void ToneMatchPage::resized()
{
    clearHeadings();
    auto area = getLocalBounds();
    header = area.removeFromTop (58);
    closeButton->setBounds (header.getRight() - 80, header.getY(), 80, controlHeight);

    area.removeFromTop (8);
    auto row = area.removeFromTop (272);
    targetCard = row.removeFromLeft (560);
    row.removeFromLeft (space::m);
    referenceCard = row.removeFromLeft (318);
    row.removeFromLeft (space::m);
    matchCard = row;
    area.removeFromTop (space::m);
    resultCard = area;

    // Target.
    {
        auto in = card (targetCard, "Target");
        auto top = in.removeFromTop (controlHeight);
        targetButton->setBounds (top.removeFromLeft (110));
        in.removeFromTop (space::s);
        waveform->setBounds (in.removeFromTop (waveformHeight));
        in.removeFromTop (space::s + 16); // the range line, painted
        separateSwitch->setBounds (in.removeFromTop (Switch::preferredHeight).withWidth (separateSwitch->getPreferredWidth()));
    }

    // Your DI.
    {
        auto in = card (referenceCard, "Your DI");
        auto top = in.removeFromTop (controlHeight);
        recordButton->setBounds (top.removeFromLeft (84));
        top.removeFromLeft (space::s);
        referenceButton->setBounds (top.removeFromLeft (110));
        in.removeFromTop (space::s + 18 + space::m); // the status line, painted
        modeChoice->setBounds (in.removeFromTop (Segmented::preferredHeight).withWidth (modeChoice->getPreferredWidth()));
    }

    // Match.
    {
        auto in = card (matchCard, "Match");
        matchButton->setBounds (in.removeFromTop (controlHeight).withWidth (120));
        in.removeFromTop (space::m);
        progressArea = in.removeFromTop (6);
        in.removeFromTop (space::xl + 18);
        cancelButton->setBounds (in.removeFromTop (controlHeight).withWidth (90));
    }

    // Result.
    {
        auto in = card (resultCard, "Result");
        auto left = in.removeFromLeft (560);
        in.removeFromLeft (space::l);
        eqCurve->setBounds (in);
        auto buttons = left.removeFromBottom (controlHeight);
        applyButton->setBounds (buttons.removeFromLeft (90));
        buttons.removeFromLeft (space::s);
        discardButton->setBounds (buttons.removeFromLeft (90));
        resultTextArea = left;
    }
}

void ToneMatchPage::paint (juce::Graphics& g)
{
    paintHeadings (g);

    // Header: the title, what it does, and the honest note.
    {
        auto h = header;
        drawText (g, "Match tone", h.removeFromTop (20), Text::title, ink);
        drawText (g, "Recreate a recording's guitar tone with BellyDSP's own amps, cabs, and EQ.", h.removeFromTop (18), Text::body, inkDim);
        drawWrapped (g, honestNote, h.withTrimmedRight (100), Text::caption, inkFaint);
    }

    // Target: the file, the range, the hint.
    {
        auto in = targetCard.reduced (cardPadding).withTrimmedTop (cardHeading);
        auto top = in.removeFromTop (controlHeight).withTrimmedLeft (110 + space::m);
        const auto name = session.getTargetName();
        drawText (g, name.isEmpty() ? "No file" : name + "  (" + timeText (session.getTargetSeconds()) + ")", top, Text::label, name.isEmpty() ? inkFaint : ink);
        in.removeFromTop (space::s + waveformHeight + 4);
        const auto [start, end] = session.getRange();
        drawText (g, session.getTarget().empty() ? juce::String() : "Matching " + timeText (start) + " to " + timeText (end) + "  (" + juce::String (end - start, 1) + " s)",
                  in.removeFromTop (16), Text::value, inkDim);
        in.removeFromTop (space::s + Switch::preferredHeight + space::s);
        juce::String hint = "Pick a section where the lead guitar dominates. A song's other instruments and its mix change what's measured";
        if (session.hasSeparator())
        {
            hint << "; separation (Demucs) keeps every guitar, not just the lead, and takes about a minute per minute of audio.";
            if (! separator->isInstalled())
                hint << " The first time, it downloads the model (" << juce::String (juce::roundToInt ((double) ampsim::tonematch::GuitarSeparator::weightsBytes / 1.0e6))
                     << " MB) from its author's page into the BellyDSP data folder.";
        }
        else
            hint << ".";
        drawWrapped (g, hint, in.removeFromTop (48), Text::caption, inkFaint);
    }

    // Your DI: what's recorded or chosen, and what the mode means.
    {
        auto in = referenceCard.reduced (cardPadding).withTrimmedTop (cardHeading + controlHeight + space::s);
        juce::String status;
        if (session.isRecording())
            status = "Recording your DI... " + juce::String (ampSim.getDiRecorder().recordedSeconds(), 1) + " s of "
                     + juce::String ((int) ampsim::tonematch::DiRecorder::maxSeconds);
        else if (session.getReference().empty())
            status = "Record a minute of playing, or choose a DI file";
        else
            status = session.getReferenceName() + ", " + juce::String (session.getReferenceSeconds(), 1) + " s";
        drawText (g, status, in.removeFromTop (18), Text::label, session.isRecording() ? accent : (session.getReference().empty() ? inkFaint : ink));
        in.removeFromTop (space::m + Segmented::preferredHeight + space::s);
        drawWrapped (g, session.getMode() == Mode::samePart
                            ? "Same part: you played the same part as the target. The two are lined up in time and compared moment by moment."
                            : "Anything: compared by long-term statistics. Play the same kind of part (a lead for a lead, in a similar register).",
                     in.removeFromTop (48), Text::caption, inkFaint);
    }

    // Match: the progress and the stage (or what's missing).
    {
        const auto p = progressArea.toFloat();
        g.setColour (line2);
        g.fillRoundedRectangle (p, 3.0f);
        if (session.isMatching() || session.hasResult())
        {
            g.setColour (accent);
            g.fillRoundedRectangle (p.withWidth (p.getWidth() * (float) (session.isMatching() ? session.getProgress() : 1.0)), 3.0f);
        }
        auto line = progressArea.withY (progressArea.getBottom() + space::s).withHeight (36);
        drawWrapped (g, statusText.isEmpty() ? (session.hasResult() ? "Done in " + juce::String (session.getResult().runtimeSeconds, 1) + " s" : "Ready")
                                             : statusText,
                     line, Text::caption, session.getError().isNotEmpty() ? ink : inkDim);
    }

    // Result: the settings, the closeness, and what it means.
    {
        auto in = resultTextArea;
        if (! session.hasResult())
        {
            drawWrapped (g, "Nothing matched yet. The result is shown here first; nothing changes until you press Apply.", in.removeFromTop (40), Text::label, inkFaint);
            return;
        }
        const auto& r = session.getResult();
        auto big = in.removeFromTop (34);
        g.setFont (tabular (geist (Weight::light, 30.0f)));
        g.setColour (ink);
        g.drawText (juce::String (juce::roundToInt (r.closeness)), big.removeFromLeft (52), juce::Justification::centredLeft, false);
        drawText (g, "closeness out of 100  (spectral error " + juce::String (r.spectralErrorAfterEqDb, 1) + " dB, distortion distance "
                         + juce::String (r.distortion, 1) + ", " + (r.mode == Mode::samePart ? "same part" : "anything") + ")",
                  big, Text::label, inkDim);
        in.removeFromTop (space::s);
        g.setFont (font (Text::label));
        g.setColour (ink);
        g.drawFittedText (resultText, in.removeFromTop (78), juce::Justification::topLeft, 5, 1.0f);
        in.removeFromTop (space::s);
        drawWrapped (g, "The score says how close the spectra and the distortion measurements came, not how it sounds: only your ears can judge that. "
                        "Apply sets these as normal settings (one undo step), so a preset saves them.",
                     in.removeFromTop (40), Text::caption, inkFaint);
    }
}

} // namespace ui

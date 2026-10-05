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

    /// The target's and the match render's long-term spectra (dB per tone match band), drawn on the same dB
    /// scale as the EQ, level-matched to each other (their weighted means equal, with the fit's weights)
    /// and shifted together so the target's loudest counted band sits at +9 dB. Empty: none drawn.
    void setSpectra (const std::vector<double>& targetDb, const std::vector<double>& matchDb, const std::vector<double>& weights)
    {
        targetSpectrum.clear();
        matchSpectrum.clear();
        const auto n = (size_t) ampsim::tonematch::numBands;
        if (targetDb.size() == n && matchDb.size() == n && weights.size() == n)
        {
            const auto diff = ampsim::tonematch::weightedMean (targetDb, weights) - ampsim::tonematch::weightedMean (matchDb, weights);
            double wMax = 0.0, top = -1.0e9;
            for (auto w : weights)
                wMax = std::max (wMax, w);
            for (size_t b = 0; b < n; ++b)
                if (weights[b] >= 0.25 * wMax)
                    top = std::max (top, targetDb[b]);
            const auto shift = 9.0 - top;
            // Third-octave smoothed for the display (smoothBands, as every comparison in the fit is), so the
            // lines show the tone's shape rather than the notes played.
            const auto t = ampsim::tonematch::smoothBands (targetDb), m = ampsim::tonematch::smoothBands (matchDb);
            for (size_t b = 0; b < n; ++b)
            {
                const auto f = ampsim::tonematch::bands().centre[b];
                targetSpectrum.push_back ({ f, t[b] + shift });
                matchSpectrum.push_back ({ f, m[b] + diff + shift });
            }
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
        // The spectra: thin lines, the target in ink and the match in emerald, under the EQ curve.
        const auto spectrumPath = [this] (const std::vector<std::pair<double, double>>& points) {
            juce::Path path;
            for (size_t i = 0; i < points.size(); ++i)
            {
                const juce::Point<float> pt { xFor (points[i].first), yFor (points[i].second, false) }; // past the edge: clipped, not flattened
                i == 0 ? path.startNewSubPath (pt) : path.lineTo (pt);
            }
            return path;
        };
        if (! targetSpectrum.empty())
        {
            const juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (r.toNearestInt());
            g.setColour (ink.withAlpha (0.85f));
            g.strokePath (spectrumPath (targetSpectrum), juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (accent.withAlpha (0.85f));
            g.strokePath (spectrumPath (matchSpectrum), juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
        g.drawText (target.empty() ? "The match EQ appears here" : "Match EQ (thick line) over what was left to fit (dots, faint ones barely count)",
                    r.reduced (8.0f, 5.0f).withHeight (13.0f), juce::Justification::centredRight, false);

        // The spectra's legend, under the EQ's caption: a short line in each colour before its name.
        if (! targetSpectrum.empty())
        {
            auto row = r.reduced (8.0f, 5.0f).withTrimmedTop (16.0f).withHeight (13.0f);
            const auto f = font (Text::caption);
            const juce::String note ("long-term spectra, level-matched");
            const auto noteWidth = textWidth (f, note);
            auto item = [&] (const juce::String& name, juce::Colour c) {
                const auto w = textWidth (f, name);
                auto area = row.removeFromRight (w + 22.0f);
                g.setColour (c);
                g.fillRect (juce::Rectangle<float> (area.getX(), area.getCentreY() - 0.5f, 14.0f, 1.5f));
                g.setColour (textDim);
                g.drawText (name, area.withTrimmedLeft (18.0f), juce::Justification::centredLeft, false);
            };
            g.setColour (textDim);
            g.drawText (note, row.removeFromRight (noteWidth + 4.0f), juce::Justification::centredRight, false);
            row.removeFromRight (10.0f);
            item ("Match", accent);
            item ("Target", ink);
        }
    }

private:
    float xFor (double f) const
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        return r.getX() + r.getWidth() * (float) (std::log (f / minF) / std::log (maxF / minF));
    }

    float yFor (double db, bool clamp = true) const
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        return r.getCentreY() - (r.getHeight() * 0.5f - 14.0f) * (float) ((clamp ? juce::jlimit (-rangeDb, rangeDb, db) : db) / rangeDb);
    }

    static constexpr double minF = 40.0, maxF = 16000.0, rangeDb = 15.0;
    std::vector<std::pair<double, double>> target, curve, targetSpectrum, matchSpectrum;
    std::vector<float> weight;
    double offset = 0.0;
};

// ---- The comparison's loop -------------------------------------------------------------------------------

class ToneMatchPage::LoopStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LoopStrip (ToneMatchSession& s) : session (s) {}

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (bg);
        g.fillRoundedRectangle (r, radiusControl);
        g.setColour (line1);
        g.drawRoundedRectangle (r.reduced (0.5f), radiusControl, 1.0f);

        const auto& audio = session.getCompareAudio (ToneMatchSession::sourceTarget);
        if (audio.data() != peaksOf || audio.size() != peaksLength)
            updatePeaks (audio);
        const auto length = session.getSectionSeconds();
        if (length <= 0.0 || peaks.empty())
            return;

        const auto [a, b] = session.getLoop();
        const auto x0 = xFor (a), x1 = xFor (b);
        g.setColour (accentDim);
        g.fillRect (juce::Rectangle<float> (x0, r.getY() + 1.0f, x1 - x0, r.getHeight() - 2.0f));
        const auto mid = r.getCentreY(), half = r.getHeight() * 0.5f - 4.0f;
        for (int c = 0; c < columns; ++c)
        {
            const auto x = 2.0f + (r.getWidth() - 4.0f) * ((float) c + 0.5f) / (float) columns;
            const auto [lo, hi] = peaks[(size_t) c];
            g.setColour (x >= x0 && x <= x1 ? ink.withAlpha (0.7f) : inkFaint);
            g.drawVerticalLine (juce::roundToInt (x), mid - half * hi * scale - 0.5f, mid - half * lo * scale + 0.5f);
        }
        g.setColour (accent);
        g.fillRect (juce::Rectangle<float> (x0 - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f));
        g.fillRect (juce::Rectangle<float> (x1 - 1.0f, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f));

        // The playhead, as a fraction of the loop (in anything mode the DI's loop isn't the target's, so the
        // fraction is the honest common measure).
        if (playhead >= 0.0f)
        {
            const auto x = x0 + (x1 - x0) * playhead;
            g.setColour (ink);
            g.fillRect (juce::Rectangle<float> (x - 0.75f, r.getY() + 3.0f, 1.5f, r.getHeight() - 6.0f));
        }
    }

    void setPlayhead (float fraction)
    {
        if (std::abs (fraction - playhead) > 1.0e-4f)
        {
            playhead = fraction;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (session.getSectionSeconds() <= 0.0)
            return;
        const auto [start, end] = session.getLoop();
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
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto t = secondsAt ((float) e.x);
        const auto [start, end] = session.getLoop();
        const auto minimum = ToneMatchSession::minLoopSeconds;
        switch (drag)
        {
            case Drag::start:  session.setLoop (std::min (t, end - minimum), end); break;
            case Drag::end:    session.setLoop (start, std::max (t, start + minimum)); break;
            case Drag::move:   session.setLoop (t - grabOffset, t - grabOffset + (end - start)); break;
            case Drag::create: session.setLoop (std::min (anchor, t), std::max (anchor, t)); break;
            case Drag::none:   return;
        }
        repaint();
        if (onChanged)
            onChanged();
    }

    void mouseUp (const juce::MouseEvent&) override { drag = Drag::none; }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        // Back to the whole matched section.
        session.setLoop (0.0, session.getSectionSeconds());
        repaint();
        if (onChanged)
            onChanged();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto [start, end] = session.getLoop();
        const auto nearEdge = std::abs ((float) e.x - xFor (start)) < 6.0f || std::abs ((float) e.x - xFor (end)) < 6.0f;
        setMouseCursor (nearEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    std::function<void()> onChanged;

private:
    void updatePeaks (const std::vector<float>& x)
    {
        peaksOf = x.data();
        peaksLength = x.size();
        peaks.clear();
        if (x.empty())
            return;
        peaks.assign ((size_t) columns, { 0.0f, 0.0f });
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
    }

    float xFor (double seconds) const
    {
        const auto length = std::max (1.0e-9, session.getSectionSeconds());
        return 2.0f + (float) (getWidth() - 4) * (float) (seconds / length);
    }

    double secondsAt (float x) const
    {
        return juce::jlimit (0.0, session.getSectionSeconds(), (double) (x - 2.0f) / (double) std::max (1, getWidth() - 4) * session.getSectionSeconds());
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
    static constexpr int columns = 170;
    std::vector<std::pair<float, float>> peaks;
    const float* peaksOf = nullptr;
    size_t peaksLength = 0;
    float scale = 1.0f, playhead = -1.0f;
    Drag drag = Drag::none;
    double anchor = 0.0, grabOffset = 0.0;
};

// ---- The preview level --------------------------------------------------------------------------------------

class ToneMatchPage::LevelBar final : public juce::Component, public juce::SettableTooltipClient
{
public:
    LevelBar()
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setTooltip ("The comparison's level (double-click: 0 dB)");
    }

    std::function<void (float)> onChange;

    void setDb (float db)
    {
        value = juce::jlimit (ToneMatchSession::minPreviewLevelDb, ToneMatchSession::maxPreviewLevelDb, db);
        repaint();
    }
    float getDb() const noexcept { return value; }

    void paint (juce::Graphics& g) override
    {
        // A 3 px track (line-2), emerald up to the value, a 9 px dot at it, and a tick at 0 dB.
        const auto r = getLocalBounds().toFloat().reduced (5.0f, 0.0f);
        const auto y = r.getCentreY();
        g.setColour (line2);
        g.fillRoundedRectangle (r.withHeight (3.0f).withCentre ({ r.getCentreX(), y }), 1.5f);
        const auto x = xFor (value);
        g.setColour (isEnabled() ? accent : inkFaint);
        g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), y - 1.5f, x - r.getX(), 3.0f), 1.5f);
        g.setColour (inkFaint);
        g.fillRect (juce::Rectangle<float> (xFor (0.0f) - 0.5f, y - 6.0f, 1.0f, 3.0f));
        g.setColour (isEnabled() ? ink : inkFaint);
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ x, y }));
    }

    void mouseDown (const juce::MouseEvent& e) override { set (dbAt ((float) e.x)); }
    void mouseDrag (const juce::MouseEvent& e) override { set (dbAt ((float) e.x)); }
    void mouseDoubleClick (const juce::MouseEvent&) override { set (0.0f); }

private:
    void set (float db)
    {
        // Half-dB steps.
        setDb (std::round (db * 2.0f) / 2.0f);
        if (onChange)
            onChange (value);
    }

    float xFor (float db) const
    {
        const auto r = getLocalBounds().toFloat().reduced (5.0f, 0.0f);
        return r.getX() + r.getWidth() * (db - ToneMatchSession::minPreviewLevelDb) / (ToneMatchSession::maxPreviewLevelDb - ToneMatchSession::minPreviewLevelDb);
    }

    float dbAt (float x) const
    {
        const auto r = getLocalBounds().toFloat().reduced (5.0f, 0.0f);
        return ToneMatchSession::minPreviewLevelDb
               + (ToneMatchSession::maxPreviewLevelDb - ToneMatchSession::minPreviewLevelDb) * juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth());
    }

    float value = 0.0f;
};

// ---- The page -------------------------------------------------------------------------------------------------

ToneMatchPage::ToneMatchPage (AmpSimProcessor& p)
    : ControlGroup (p), separator (std::make_unique<ampsim::tonematch::GuitarSeparator> (ampsim::tonematch::GuitarSeparator::defaultFolder())),
      session (p)
{
    // Separation (Stage C): on the session's worker thread, install the model the first time (the download's
    // size is said on the page before anyone switches it on), then separate. The separator and the URL are
    // read when a match starts (setSeparationSource doesn't change them while one runs).
    session.setSeparator ([this] (const std::vector<float>& x, const std::atomic<bool>& cancel,
                                  const ampsim::tonematch::ProgressFn& progress, juce::String& problem) {
        auto* sep = separator.get();
        const auto installShare = sep->isInstalled() ? 0.0 : 0.3;
        if (! sep->isInstalled())
        {
            problem = sep->install (separationUrl, cancel, [&] (double f, const juce::String& s) { progress (installShare * f, s); });
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

    // The comparison (A/B): the sources, Play, the loop, the level match, the live mute, the level.
    playButton = &addButton ("Play", [this] { togglePreview(); });
    playButton->setTooltip ("Play or stop the comparison, looped (Space)");
    sourceChoice = std::make_unique<Segmented> (juce::StringArray { "Target", "Match", "Current" });
    sourceChoice->onChange = [this] (int i) { selectSource (i); };
    addChildComponent (*sourceChoice);
    loopStrip = std::make_unique<LoopStrip> (session);
    loopStrip->setTooltip ("The loop: drag to set it, drag its edges or middle to adjust, double-click for the whole section");
    loopStrip->onChanged = [this] { repaint (compareArea); };
    addChildComponent (*loopStrip);
    levelMatchSwitch = std::make_unique<Switch> ("Level match");
    levelMatchSwitch->setToggleState (session.getLevelMatch(), juce::dontSendNotification);
    levelMatchSwitch->setTooltip ("All three at the same loudness (BS.1770), so the comparison is about tone, not volume");
    levelMatchSwitch->onClick = [this] {
        session.setLevelMatch (levelMatchSwitch->getToggleState());
        repaint (compareArea);
    };
    addChildComponent (*levelMatchSwitch);
    muteSwitch = std::make_unique<Switch> ("Mute my guitar while comparing");
    muteSwitch->setToggleState (session.getMuteLive(), juce::dontSendNotification);
    muteSwitch->onClick = [this] { session.setMuteLive (muteSwitch->getToggleState()); };
    addChildComponent (*muteSwitch);
    levelBar = std::make_unique<LevelBar>();
    levelBar->setDb (session.getPreviewLevelDb());
    levelBar->onChange = [this] (float db) {
        session.setPreviewLevelDb (db);
        repaint (levelValueArea);
    };
    addChildComponent (*levelBar);

    setWantsKeyboardFocus (true);
    refresh();
}

ToneMatchPage::~ToneMatchPage()
{
    stopTimer();
}

void ToneMatchPage::setSeparationSource (const juce::File& modelFolder, const juce::String& url)
{
    jassert (! session.isMatching());
    if (session.isMatching())
        return;
    separator = std::make_unique<ampsim::tonematch::GuitarSeparator> (modelFolder);
    separationUrl = url;
}

void ToneMatchPage::pageShown()
{
    separateSwitch->setVisible (session.hasSeparator());
    startTimerHz (15);
}

void ToneMatchPage::pageHidden()
{
    // Leaving the page stops the comparison, so the live guitar comes back.
    session.setPreviewPlaying (false);

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
    if (! isShowing() && ! session.isRecording() && ! session.isMatching() && ! session.isRenderingCompare())
        stopTimer();
}

void ToneMatchPage::togglePreview()
{
    session.setPreviewPlaying (! session.isPreviewPlaying());
    refresh();
    repaint (compareArea);
}

void ToneMatchPage::selectSource (int source)
{
    session.setPreviewSource (source);
    sourceChoice->setSelected (source, juce::dontSendNotification);
    repaint (compareArea);
}

bool ToneMatchPage::keyPressed (const juce::KeyPress& key)
{
    // The comparison's keys, once there's something to compare: Space plays or stops, 1 2 3 pick the source.
    if (! session.isCompareReady() || key.getModifiers().isAnyModifierKeyDown())
        return false;
    if (key == juce::KeyPress::spaceKey)
    {
        togglePreview();
        return true;
    }
    auto c = (int) key.getTextCharacter();
    if (c == 0)
        c = key.getKeyCode();
    if (c >= '1' && c <= '3')
    {
        selectSource ((int) (c - '1'));
        return true;
    }
    return false;
}

juce::String ToneMatchPage::getLoudnessText() const
{
    juce::StringArray parts;
    for (int s = 0; s < ToneMatchSession::numSources; ++s)
    {
        const auto l = session.getLoudness (s);
        parts.add (ToneMatchSession::sourceName (s) + " " + (std::isfinite (l) ? juce::String (l, 1) : juce::String ("-")));
    }
    return parts.joinIntoString (", ") + " LUFS";
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
    resultLabels.clear();
    resultItems.clear();
    if (! session.hasResult())
    {
        resultText.clear();
        return;
    }
    const auto& r = session.getResult();
    const auto capture = ampSim.getSlotCapture (r.slot).getFileNameWithoutExtension();
    auto add = [this] (const juce::String& label, juce::StringArray items) {
        resultLabels.add (label);
        resultItems.push_back (std::move (items));
    };
    add ("Amp", { capture + " (slot " + juce::String (r.slot + 1) + ")", "Gain " + signedDb (r.gainDb) });
    juce::StringArray tone;
    for (size_t b = 0; b < ampsim::AmpTone::numBands; ++b)
        tone.add (juce::String (ampsim::AmpTone::bands[b].name) + " " + signedDb (r.tone[b]));
    add ("Tone", tone);
    add ("Cab", { r.cab.getFileNameWithoutExtension() });

    // The match EQ's audible bands, low to high.
    auto bands = std::vector<ampsim::Equalizer::Band> (r.eq.begin(), r.eq.end());
    std::sort (bands.begin(), bands.end(), [] (const auto& x, const auto& y) { return x.frequency < y.frequency; });
    juce::StringArray eq;
    for (const auto& band : bands)
        if (std::abs (band.gainDb) >= 0.5f)
            eq.add ((band.type == ampsim::Equalizer::BandType::lowShelf ? "low shelf " : band.type == ampsim::Equalizer::BandType::highShelf ? "high shelf " : "")
                    + juce::String (juce::roundToInt (band.frequency)) + " Hz " + signedDb (band.gainDb));
    add ("Match EQ", eq.isEmpty() ? juce::StringArray { "flat" } : eq);

    juce::StringArray lines;
    for (int i = 0; i < resultLabels.size(); ++i)
        lines.add (resultLabels[i] + "  " + resultItems[(size_t) i].joinIntoString (", "));
    resultText = lines.joinIntoString ("\n");
}

juce::StringArray ToneMatchPage::packItems (const juce::Font& f, const juce::String& label, const juce::StringArray& items, float width)
{
    // Greedy: each item goes on the current line if the line still fits, otherwise it starts the next.
    // An item is never broken (a value and its unit stay together), so a line may only overrun when one
    // item alone is wider than the space, and then it's drawn whole.
    const auto prefix = label + "  ";
    const auto indent = juce::GlyphArrangement::getStringWidth (f, prefix);
    juce::StringArray lines;
    juce::String current;
    for (int i = 0; i < items.size(); ++i)
    {
        const auto piece = items[i] + (i + 1 < items.size() ? "," : "");
        const auto candidate = current.isEmpty() ? piece : current + " " + piece;
        if (current.isNotEmpty() && juce::GlyphArrangement::getStringWidth (f, candidate) > width - indent)
        {
            lines.add (current);
            current = piece;
        }
        else
            current = candidate;
    }
    lines.add (current);
    lines.set (0, prefix + lines[0]);
    return lines;
}

juce::String ToneMatchPage::getApplyNote() const
{
    juce::StringArray names;
    for (const auto& fx : ToneMatchSession::preEffectsApplyTurnsOff())
        names.add (fx.name);
    const auto on = session.preEffectsOnNow();
    return "Apply sets these as normal settings, in one undo step, and switches off the pre effects: " + names.joinIntoString (", ") + (on.isEmpty() ? " (none is on now)" : " (on now: " + on.joinIntoString (", ") + ")")
           + ". The noise gate stays as it is.";
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
        updateResultText(); // clears it

    // The comparison: shown with a result, usable once its renders are in.
    const auto comparing = session.hasResult();
    const auto ready = session.isCompareReady();
    for (auto* c : std::initializer_list<juce::Component*> { playButton, sourceChoice.get(), loopStrip.get(), levelMatchSwitch.get(), muteSwitch.get(), levelBar.get() })
    {
        c->setVisible (comparing);
        c->setEnabled (ready);
    }
    playButton->setButtonText (session.isPreviewPlaying() ? "Stop" : "Play");
    loopStrip->setPlayhead (session.isPreviewPlaying() && ampSim.getPreviewPlayer().isActive() ? ampSim.getPreviewPlayer().getPlayheadFraction() : -1.0f);
    sourceChoice->setSelected (session.getPreviewSource(), juce::dontSendNotification);
    const auto haveSpectra = ! session.getTargetSpectrum().empty() && ! session.getMatchSpectrum().empty();
    if (haveSpectra != spectraShown)
    {
        spectraShown = haveSpectra;
        if (haveSpectra)
            eqCurve->setSpectra (session.getTargetSpectrum(), session.getMatchSpectrum(), session.getResult().weights);
        else
            eqCurve->setSpectra ({}, {}, {});
    }

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
    targetCard = row.removeFromLeft (520);
    row.removeFromLeft (space::m);
    referenceCard = row.removeFromLeft (284);
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

    // Match, and under it the comparison (once there's a result).
    {
        auto in = card (matchCard, "Match");
        auto top = in.removeFromTop (controlHeight);
        matchButton->setBounds (top.removeFromLeft (110));
        top.removeFromLeft (space::s);
        cancelButton->setBounds (top.removeFromLeft (84));
        in.removeFromTop (space::m);
        progressArea = in.removeFromTop (6);
        in.removeFromTop (space::s);
        statusArea = in; // the stage, or what went wrong (an error can take several lines)

        // The comparison takes the bottom of the card: the sources (and their loudness under them), the
        // loop, the live mute, the level match and the level.
        auto compare = in.withTrimmedTop (30 + 4); // under two lines of status
        compareArea = compare;
        auto sources = compare.removeFromTop (controlHeight);
        playButton->setBounds (sources.removeFromLeft (72));
        sources.removeFromLeft (space::l);
        sourceChoice->setBounds (sources.withWidth (sourceChoice->getPreferredWidth()).withSizeKeepingCentre (sourceChoice->getPreferredWidth(), Segmented::preferredHeight));
        loudnessRow = compare.removeFromTop (14).withTrimmedLeft (72 + space::l);
        compare.removeFromTop (6);
        loopStrip->setBounds (compare.removeFromTop (34));
        compare.removeFromTop (6);
        muteSwitch->setBounds (compare.removeFromTop (Switch::preferredHeight).withWidth (muteSwitch->getPreferredWidth()).translated (-Switch::margin, 0));
        compare.removeFromTop (2);
        auto levels = compare.removeFromTop (Switch::preferredHeight);
        levelMatchSwitch->setBounds (levels.removeFromLeft (levelMatchSwitch->getPreferredWidth()).translated (-Switch::margin, 0));
        levels.removeFromLeft (space::l);
        levelLabelArea = levels.removeFromLeft (34);
        levelValueArea = levels.removeFromRight (44);
        levelBar->setBounds (levels);
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
        buttons.removeFromLeft (space::m);
        applyNoteArea = buttons; // the score's caveat, beside the buttons
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
        auto shown = statusText.isEmpty() ? (session.hasResult() ? "Done in " + juce::String (session.getResult().runtimeSeconds, 1) + " s." : "Ready")
                                         : statusText;
        if (statusText.isEmpty() && session.hasResult())
        {
            // With a result, the line also says what the comparison is doing and how the sources line up.
            if (const auto c = session.getCompareStatus(); c.isNotEmpty())
                shown << " " << c;
            else if (session.isCurrentStale())
                shown << " Current follows your settings once they stop moving.";
            else
                shown << (session.isAligned() ? " Same part: your DI is lined up with the target in time."
                                              : " Anything: the target loops the range, your DI its whole take.");
        }
        g.setFont (font (Text::caption));
        g.setColour (session.getError().isNotEmpty() ? ink : inkDim);
        const auto area = session.hasResult() ? statusArea.withHeight (30) : statusArea;
        g.drawFittedText (shown, area, juce::Justification::topLeft, juce::jmax (1, area.getHeight() / 15), 1.0f);

        if (session.hasResult())
        {
            // Under each source, its loudness as it is (before the level match).
            g.setFont (tabular (font (Text::caption)));
            for (int s = 0; s < ToneMatchSession::numSources; ++s)
            {
                const auto l = session.getLoudness (s);
                const auto option = sourceChoice->optionArea (s).toNearestInt().translated (sourceChoice->getX(), 0);
                g.setColour (inkFaint);
                g.drawText (std::isfinite (l) ? juce::String (l, 1) : juce::String ("-"), loudnessRow.withX (option.getX()).withWidth (60),
                            juce::Justification::centredLeft, false);
            }
            g.drawText ("LUFS", loudnessRow, juce::Justification::centredRight, false);
            g.setFont (font (Text::caption));
            g.setColour (session.isCompareReady() ? inkDim : inkFaint);
            g.drawText ("Level", levelLabelArea, juce::Justification::centredLeft, false);
            g.drawText ((session.getPreviewLevelDb() > 0.0f ? "+" : "") + juce::String (session.getPreviewLevelDb(), 1) + " dB", levelValueArea,
                        juce::Justification::centredRight, false);
        }
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
        const auto noteArea = in.removeFromBottom (34).withTrimmedBottom (4); // what Apply does: two caption lines above the buttons
        auto big = in.removeFromTop (34);
        g.setFont (tabular (geist (Weight::light, 30.0f)));
        g.setColour (ink);
        g.drawText (juce::String (juce::roundToInt (r.closeness)), big.removeFromLeft (52), juce::Justification::centredLeft, false);
        drawText (g, "closeness out of 100  (spectral error " + juce::String (r.spectralErrorAfterEqDb, 1) + " dB, distortion distance "
                         + juce::String (r.distortion, 1) + ", " + (r.mode == Mode::samePart ? "same part" : "anything") + ")",
                  big, Text::label, inkDim);
        in.removeFromTop (space::s);
        // The settings, one line each, broken only between items (packItems), continuation lines indented.
        const auto f = juce::Font (font (Text::label));
        g.setFont (f);
        g.setColour (ink);
        const auto lineHeight = (int) std::ceil (f.getHeight() * 1.25f);
        for (int i = 0; i < resultLabels.size(); ++i)
        {
            const auto lines = packItems (f, resultLabels[i], resultItems[(size_t) i], (float) in.getWidth());
            const auto indent = (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, resultLabels[i] + "  "));
            for (int l = 0; l < lines.size(); ++l)
                g.drawText (lines[l], in.removeFromTop (lineHeight).withTrimmedLeft (l == 0 ? 0 : indent), juce::Justification::centredLeft, false);
        }
        in.removeFromTop (space::s);
        // What Apply does, full width above the buttons; the score's caveat beside them.
        g.setFont (font (Text::caption));
        g.setColour (inkDim);
        g.drawFittedText (getApplyNote(), noteArea, juce::Justification::bottomLeft, 2, 1.0f);
        g.setColour (inkFaint);
        g.drawFittedText ("The score measures the spectra and the distortion, not the sound: only your ears can judge that.", applyNoteArea,
                          juce::Justification::centredLeft, 2, 1.0f);
    }
}

} // namespace ui

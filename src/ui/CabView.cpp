// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "CabView.h"
#include "InspectorParts.h"
#include "LookAndFeel.h"

#include <cmath>

#include <algorithm>
#include <map>

namespace ui
{

using namespace theme;

namespace
{
bool isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac");
}

int audioFilesIn (const juce::File& folder)
{
    return folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav;*.aif;*.aiff;*.flac");
}

/// A folder is a cab pack if it has a manifest or at least two IRs.
bool isPack (const juce::File& folder)
{
    return folder.getChildFile ("cabpack.json").existsAsFile() || audioFilesIn (folder) >= 2;
}

constexpr int leftWidth = 250, rightWidth = 260, columnGap = 32;
constexpr int entryHeight = 52;
constexpr int groupHeadingHeight = 28, groupHeadingPad = 6; // a 16 px line of 12 px text, 6 above and below
constexpr int libraryHintHeight = 44;                      // "Put IR files ... in <folder>", up to 3 lines
constexpr float speakerRadius = 160.0f; // the reference's SVG units (the speaker is 400 of them across)
constexpr float maxRadius = 0.95f; // markers stay inside 95% of the speaker's radius
} // namespace

juce::String CabView::zoneFor (float r)
{
    // The reference's zones, as fractions of the speaker's radius.
    return r < 0.20f ? "Cap" : r < 0.32f ? "Cap edge" : r < 0.80f ? "Cone" : "Cone edge";
}

// ---- The match curve's view -------------------------------------------------------------------------

/// The match curve as it plays: MatchCurve::targetDb at the current amount, on a log axis from 20 Hz to
/// 20 kHz, with faint lines at 100 Hz, 1 kHz, and 10 kHz and at 0 dB. The scale fits the curve in 3 dB
/// steps, +-6 dB at least and +-15 (the curve's clamp) at most, and says which in the corner. Read
/// only: tone match sets the curve. Recomputed only when the curve or the amount changes.
class CabView::MatchCurveView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int numPoints = 160;
    static constexpr double loHz = 20.0, hiHz = 20000.0;

    void set (const ampsim::MatchCurve::Curve& newCurve, float amount, bool isOn)
    {
        if (newCurve == curve && std::abs (amount - shownAmount) < 1.0e-4f && isOn == on)
            return;
        curve = newCurve;
        shownAmount = amount;
        on = isOn;
        drawnDb.clear();
        rangeDb = 6.0f;
        if (! curve.isFlat())
        {
            std::vector<double> freqs;
            for (int i = 0; i < numPoints; ++i)
                freqs.push_back (loHz * std::pow (hiHz / loHz, (double) i / (numPoints - 1)));
            for (auto v : ampsim::MatchCurve::targetDb (curve, amount / 100.0, freqs))
                drawnDb.push_back ((float) v);
            float peak = 0.0f;
            for (auto v : drawnDb)
                peak = std::max (peak, std::abs (v));
            rangeDb = juce::jlimit (6.0f, (float) ampsim::MatchCurve::maxDb, 3.0f * std::ceil (peak / 3.0f));
        }
        repaint();
    }

    const std::vector<float>& getDrawnDb() const noexcept { return drawnDb; }
    bool isOn() const noexcept { return on; }
    float getRangeDb() const noexcept { return rangeDb; }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (line1);
        g.drawRoundedRectangle (area, radiusMini, 1.0f);

        // No curve: just the frame and what sets one.
        if (drawnDb.empty())
        {
            g.setFont (font (Text::label));
            g.setColour (inkFaint);
            g.drawText ("No curve yet: tone match sets one", area, juce::Justification::centred, false);
            return;
        }

        const auto xFor = [&] (double hz) { return area.getX() + area.getWidth() * (float) (std::log (hz / loHz) / std::log (hiHz / loHz)); };
        const auto yFor = [&] (float db) { return area.getCentreY() - (area.getHeight() * 0.5f - 4.0f) * db / rangeDb; };

        // The grid: decades and 0 dB, with the decades' labels in the corner they leave free.
        g.setFont (geist (Weight::regular, 10.0f));
        for (const auto& [hz, label] : { std::pair<double, const char*> { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } })
        {
            const auto x = xFor (hz);
            g.setColour (line1);
            g.fillRect (juce::Rectangle<float> (x, area.getY() + 1.0f, 1.0f, area.getHeight() - 2.0f));
            g.setColour (inkFaint);
            g.drawText (label, juce::Rectangle<float> (x + 3.0f, area.getBottom() - 13.0f, 30.0f, 12.0f), juce::Justification::centredLeft, false);
        }
        g.setColour (line2);
        g.fillRect (juce::Rectangle<float> (area.getX() + 1.0f, area.getCentreY(), area.getWidth() - 2.0f, 1.0f));
        g.setColour (inkFaint);
        g.drawText (juce::String::fromUTF8 ("\xc2\xb1") + juce::String (juce::roundToInt (rangeDb)) + " dB", area.reduced (5.0f, 2.0f).withHeight (12.0f),
                    juce::Justification::centredRight, false);

        juce::Path line;
        for (int i = 0; i < (int) drawnDb.size(); ++i)
        {
            const auto x = area.getX() + area.getWidth() * (float) i / (float) (drawnDb.size() - 1);
            const auto y = yFor (drawnDb[(size_t) i]);
            if (i == 0)
                line.startNewSubPath (x, y);
            else
                line.lineTo (x, y);
        }
        auto fill = line;
        fill.lineTo (area.getRight(), area.getCentreY());
        fill.lineTo (area.getX(), area.getCentreY());
        fill.closeSubPath();

        const auto alpha = on ? 1.0f : offAlpha;
        g.setColour (accentDim.withMultipliedAlpha (alpha));
        g.fillPath (fill);
        g.setColour (accent.withMultipliedAlpha (alpha));
        g.strokePath (line, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    ampsim::MatchCurve::Curve curve;
    float shownAmount = -1.0f, rangeDb = 6.0f;
    bool on = false;
    std::vector<float> drawnDb;
};

// ---- The library list ----------------------------------------------------------------------------

class CabView::LibraryList final : public juce::Component
{
public:
    explicit LibraryList (CabView& v) : view (v) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    juce::String selectedPath;

    void paint (juce::Graphics& g) override
    {
        const auto& listed = view.entries;
        if (listed.empty())
        {
            g.setFont (geist (Weight::medium, 14.0f));
            g.setColour (inkDim);
            g.drawText ("No cabinets in your library yet", juce::Rectangle<int> (14, 10, getWidth() - 14, 18), juce::Justification::centredLeft, true);
            g.setFont (geist (Weight::regular, 12.0f));
            g.setColour (inkFaint);
            g.drawFittedText ("Put IR files or cab pack folders in " + presets::libraryRoot ("irs").getFullPathName(),
                              juce::Rectangle<int> (14, 30, getWidth() - 14, 40), juce::Justification::topLeft, 3, 0.9f);
            g.setColour (line2);
            g.fillRect (0, 0, 1, getHeight());
            return;
        }

        // Group headings (12 px regular, ink-faint: quieter than the card's own 12 px medium heading).
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkFaint);
        for (const auto& [headingTop, heading] : view.headings)
            g.drawText (heading, juce::Rectangle<int> (0, headingTop + groupHeadingPad, getWidth(), 16), juce::Justification::centredLeft, true);
        if (view.libraryHintTop >= 0)
            g.drawFittedText ("Put IR files or cab pack folders in " + presets::libraryRoot ("irs").getFullPathName(),
                              juce::Rectangle<int> (14, view.libraryHintTop, getWidth() - 14, libraryHintHeight), juce::Justification::topLeft, 3, 0.9f);

        for (int i = 0; i < (int) listed.size(); ++i)
        {
            const auto& e = listed[(size_t) i];
            const auto row = juce::Rectangle<int> (0, view.entryTops[(size_t) i], getWidth(), entryHeight);
            const auto selected = e.file.getFullPathName() == selectedPath;
            // CSS .cab-opt: padding 10 0 10 14, a 1 px left border; the name 14 medium, the line 12 faint.
            g.setColour (selected ? accent : line2);
            g.fillRect (row.withWidth (1));
            g.setFont (geist (Weight::medium, 14.0f));
            g.setColour (selected || i == hovered ? ink : inkDim);
            g.drawText (e.name, row.withTrimmedLeft (14).withTrimmedTop (10).withHeight (17), juce::Justification::centredLeft, true);
            g.setFont (geist (Weight::regular, 12.0f));
            g.setColour (inkFaint);
            g.drawText (e.description, row.withTrimmedLeft (14).withTrimmedTop (29).withHeight (15), juce::Justification::centredLeft, true);
        }
    }

    /// The entry at y in this component, or -1 (a heading, the hint, or below the last entry).
    int entryAt (int y) const
    {
        for (size_t i = 0; i < view.entryTops.size(); ++i)
            if (y >= view.entryTops[i] && y < view.entryTops[i] + entryHeight)
                return (int) i;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto h = entryAt (e.y);
        if (h != hovered)
        {
            hovered = h;
            repaint();
        }
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()))
            if (const auto i = entryAt (e.y); i >= 0)
                view.clickEntry (i);
    }

private:
    CabView& view;
    int hovered = -1;
};

// ---- The drop zone -------------------------------------------------------------------------------

class CabView::DropZone final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DropZone (CabView& v) : view (v) { setTooltip ("Drop an IR file or a cab pack folder here, or Browse"); }

    bool highlighted = false;

    juce::Rectangle<float> browseArea() const
    {
        const auto f = geist (Weight::medium, 12.0f);
        const auto orWidth = textWidth (geist (Weight::regular, 12.0f), "or ");
        const auto w = orWidth + textWidth (f, "Browse");
        const auto x = ((float) getWidth() - w) * 0.5f + orWidth;
        return { x, 15.0f + 18.0f, textWidth (f, "Browse"), 18.0f };
    }

    void paint (juce::Graphics& g) override
    {
        // CSS .ir: padding 14, radius 8, a dashed 1 px line-2 border; 12 px faint, line height 1.5.
        juce::Path box;
        box.addRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 8.0f);
        const float dashes[] { 4.0f, 3.0f };
        juce::Path dashed;
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, box, dashes, 2);
        g.setColour (highlighted ? inkFaint : line2);
        g.fillPath (dashed);

        const auto regular = geist (Weight::regular, 12.0f);
        g.setFont (regular);
        g.setColour (inkFaint);
        g.drawText ("Drop an IR file here", juce::Rectangle<float> (0.0f, 15.0f, (float) getWidth(), 18.0f), juce::Justification::centred, false);
        const auto b = browseArea();
        const auto orWidth = textWidth (regular, "or ");
        g.drawText ("or ", juce::Rectangle<float> (b.getX() - orWidth, b.getY(), orWidth + 1.0f, b.getHeight()), juce::Justification::centredLeft, false);
        g.setFont (geist (Weight::medium, 12.0f));
        g.setColour (accent);
        g.drawText ("Browse", b.withWidth (b.getWidth() + 2.0f), juce::Justification::centredLeft, false);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (browseArea().expanded (4.0f).contains (e.position) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || ! browseArea().expanded (4.0f).contains (e.position))
            return;
        // Browse: an IR file or a cab pack folder.
        juce::PopupMenu menu;
        const auto safe = juce::Component::SafePointer<CabView> (&view);
        menu.addItem ("An impulse response file...", [safe] { if (safe != nullptr && safe->onBrowse) safe->onBrowse (false); });
        menu.addItem ("A cab pack folder...", [safe] { if (safe != nullptr && safe->onBrowse) safe->onBrowse (true); });
        ui::showMenu (menu, this, &getLookAndFeel());
    }

private:
    CabView& view;
};

// ---- The speaker ---------------------------------------------------------------------------------

/// The speaker on the stage (phase 2: the cab's view is built around it): a 12-inch driver in its baffle, drawn in
/// code and cached at the size it's shown (the baffle's tolex, the basket's rim with its bolts, the rubber surround,
/// the paper cone with its ribs, the dust cap, lit warmly from below like the stage), and the two close mics on it,
/// large: each a capsule over the cone (its grille mesh, a ring in the mic's colour, its letter) with its body hanging
/// below it, as a mic on a stand seen from the front. The farther a mic is from the grille (its Distance), the smaller
/// it's drawn. A mic with a cab pack moves: drag it across the cone (its distance from the centre over the cone's
/// radius is its position, pos_x; the angle is only for looks and isn't saved), and the pack's capture positions show
/// as small dots along its way. A mic with a plain IR can't move: it's dimmed.
class CabView::Speaker final : public juce::Component
{
public:
    explicit Speaker (AmpSimProcessor& p) : ampSim (p)
    {
        for (int m = 0; m < 2; ++m)
        {
            position[(size_t) m] = p.parameters.getParameter (AmpSimProcessor::cabParamId (m, "pos_x"));
            distance[(size_t) m] = p.parameters.getParameter (AmpSimProcessor::cabParamId (m, "pos_y"));
        }
    }

    // The markers' angles: for looks only, never saved (UH8). The reference's starting places.
    std::array<float, 2> angle { std::atan2 (-0.05f, -0.12f), std::atan2 (0.30f, 0.42f) };
    std::array<bool, 2> hasPack {};
    std::array<std::vector<float>, 2> packPositions; // each pack's capture positions across the cone (0 to 1)
    float scale = 0.95f; // pixels per SVG unit: the component is the speaker's 400 units across

    float radiusOf (int mic) const { return position[(size_t) mic]->convertFrom0to1 (position[(size_t) mic]->getValue()) * maxRadius; }
    float distanceOf (int mic) const { return distance[(size_t) mic]->getValue(); }

    juce::Point<float> marker (int mic) const
    {
        const auto r = radiusOf (mic) * speakerRadius * scale;
        return getLocalBounds().toFloat().getCentre() + juce::Point<float> (std::cos (angle[(size_t) mic]), std::sin (angle[(size_t) mic])) * r;
    }

    /// A capsule's radius in pixels: 17 at the grille, three quarters of that at the pack's farthest.
    float capsuleRadius (int mic) const { return 17.0f * (1.0f - 0.25f * juce::jlimit (0.0f, 1.0f, distanceOf (mic))); }

    void paint (juce::Graphics& g) override
    {
        if (art.isNull() || art.getWidth() != getWidth() * 2 || art.getHeight() != getHeight() * 2)
            art = render (getWidth(), getHeight(), scale, 2.0f);
        g.drawImage (art, getLocalBounds().toFloat());

        // The packs' capture positions along each movable mic's way, then the mics: B first, so A sits on top.
        const auto c = getLocalBounds().toFloat().getCentre();
        for (int m = 1; m >= 0; --m)
            if (hasPack[(size_t) m])
            {
                g.setColour ((m == 0 ? accent : ink).withAlpha (0.45f));
                const juce::Point<float> dir { std::cos (angle[(size_t) m]), std::sin (angle[(size_t) m]) };
                for (auto x : packPositions[(size_t) m])
                    g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c + dir * (x * maxRadius * speakerRadius * scale)));
            }
        for (int m = 1; m >= 0; --m)
            drawMic (g, m);
    }

    int markerAt (juce::Point<float> p) const
    {
        for (int m = 0; m < 2; ++m)
            if (hasPack[(size_t) m] && marker (m).getDistanceFrom (p) <= capsuleRadius (m) + 6.0f)
                return m;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto over = markerAt (e.position);
        setMouseCursor (over >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        if (over != hovered)
        {
            hovered = over;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hovered >= 0)
        {
            hovered = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        dragging = markerAt (e.position);
        if (dragging < 0)
            return;
        beginUndoStep (ampSim.parameters);
        position[(size_t) dragging]->beginChangeGesture();
        grabOffset = e.position - marker (dragging);
        moveTo (e.position - grabOffset);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging >= 0 && ! e.mods.isPopupMenu())
            moveTo (e.position - grabOffset);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            position[(size_t) dragging]->endChangeGesture();
        dragging = -1;
        repaint();
    }

    void moveTo (juce::Point<float> p)
    {
        // The distance from the centre over the speaker's radius, clamped to 0.95, is the position across
        // the cone: pos_x = r / 0.95. The angle only places the marker.
        const auto d = (p - getLocalBounds().toFloat().getCentre()) / (speakerRadius * scale);
        const auto r = juce::jmin (maxRadius, std::hypot (d.x, d.y));
        if (r > 0.001f)
            angle[(size_t) dragging] = std::atan2 (d.y, d.x);
        auto* param = position[(size_t) dragging];
        param->setValueNotifyingHost (param->convertTo0to1 (r / maxRadius));
        repaint();
    }

    /// The baffle and the driver at a size, uncached (the cache, and tests).
    static juce::Image render (int width, int height, float unitScale, float pixelScale)
    {
        juce::Image image (juce::Image::ARGB, juce::jmax (1, juce::roundToInt ((float) width * pixelScale)), juce::jmax (1, juce::roundToInt ((float) height * pixelScale)),
                           true);
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (pixelScale));
        const auto b = juce::Rectangle<float> ((float) width, (float) height);
        const auto c = b.getCentre();
        const auto u = unitScale; // pixels per unit: the cone's edge is at 160 units

        // The baffle: a square of black tolex, its corners rounded, a shade lighter at the top, with a hairline edge.
        const auto baffle = b.reduced (1.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1f1d1b), 0.0f, baffle.getY(), juce::Colour (0xff0e0d0c), 0.0f, baffle.getBottom(), false));
        g.fillRoundedRectangle (baffle, 14.0f);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (baffle, 14.0f, 1.5f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawRoundedRectangle (baffle.reduced (3.0f), 12.0f, 1.0f);

        const auto circle = [&c] (float r) { return juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c); };

        // The cut-out's shadow, then the basket's rim: brushed steel, darker below, with eight bolts.
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.7f), c, juce::Colours::black.withAlpha (0.0f), c.translated (196.0f * u, 0.0f), true));
        g.fillEllipse (circle (196.0f * u));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff57534e), c.x, c.y - 186.0f * u, juce::Colour (0xff1c1b1a), c.x, c.y + 186.0f * u, false));
        g.fillEllipse (circle (184.0f * u));
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawEllipse (circle (184.0f * u), 1.0f);
        for (int i = 0; i < 8; ++i)
        {
            const auto a = juce::MathConstants<float>::twoPi * ((float) i + 0.5f) / 8.0f;
            const auto p = c + juce::Point<float> (std::cos (a), std::sin (a)) * (174.0f * u);
            g.setColour (juce::Colour (0xff0d0d0c));
            g.fillEllipse (juce::Rectangle<float> (9.0f * u, 9.0f * u).withCentre (p));
            g.setColour (juce::Colour (0xff8a857e));
            g.fillEllipse (juce::Rectangle<float> (5.0f * u, 5.0f * u).withCentre (p.translated (-0.6f * u, -0.6f * u)));
        }

        // The surround: a rubber roll, its crest catching the light at the top left.
        g.setColour (juce::Colour (0xff0b0a0a));
        g.fillEllipse (circle (166.0f * u));
        {
            juce::Path roll;
            roll.addEllipse (circle (162.0f * u));
            roll.addEllipse (circle (150.0f * u));
            roll.setUsingNonZeroWinding (false);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3b3835), c.x - 120.0f * u, c.y - 120.0f * u, juce::Colour (0xff080808), c.x + 120.0f * u,
                                                     c.y + 120.0f * u, false));
            g.fillPath (roll);
        }

        // The cone: paper, a radial gradient from the cap out, its ribs as faint rings, and the light from below.
        {
            juce::ColourGradient paper (juce::Colour (0xff2c2925), c, juce::Colour (0xff121110), c.translated (150.0f * u, 0.0f), true);
            paper.addColour (0.45, juce::Colour (0xff201e1b));
            g.setGradientFill (paper);
            g.fillEllipse (circle (150.0f * u));
            for (auto r : { 128.0f, 108.0f, 88.0f, 68.0f })
            {
                g.setColour (juce::Colours::black.withAlpha (0.28f));
                g.drawEllipse (circle (r * u), 1.2f);
                g.setColour (juce::Colours::white.withAlpha (0.035f));
                g.drawEllipse (circle (r * u - 1.5f), 1.0f);
            }
            const juce::Graphics::ScopedSaveState saved (g);
            juce::Path cone;
            cone.addEllipse (circle (150.0f * u));
            g.reduceClipRegion (cone);
            g.setGradientFill (juce::ColourGradient (theme::stageLight.withAlpha (0.0f), c.x, c.y, theme::stageLight.withAlpha (0.5f), c.x, c.y + 150.0f * u, false));
            g.fillEllipse (circle (150.0f * u));
        }

        // The dust cap: a dome, lit at its top left, with a soft shadow round it.
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.6f), c, juce::Colours::black.withAlpha (0.0f), c.translated (52.0f * u, 0.0f), true));
        g.fillEllipse (circle (52.0f * u));
        {
            const juce::Point<float> light { c.x - 14.0f * u, c.y - 16.0f * u };
            juce::ColourGradient dome (juce::Colour (0xff57524b), light, juce::Colour (0xff151413), light.translated (60.0f * u, 0.0f), true);
            g.setGradientFill (dome);
            g.fillEllipse (circle (42.0f * u));
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.drawEllipse (circle (42.0f * u), 1.0f);
        }
        return image;
    }

private:
    void drawMic (juce::Graphics& g, int m) const
    {
        const auto colour = m == 0 ? accent : ink;
        const auto alpha = hasPack[(size_t) m] ? 1.0f : 0.4f;
        const auto p = marker (m);
        const auto r = capsuleRadius (m);
        const auto lifted = dragging == m || hovered == m;

        // The body hanging below the capsule: a steel tube with a highlight down its left, and its shadow on the cone.
        const auto body = juce::Rectangle<float> (r * 1.1f, r * 3.6f).withPosition (p.x - r * 0.55f, p.y + r * 0.4f);
        g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
        g.fillRoundedRectangle (body.translated (3.0f, 4.0f), r * 0.3f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6b6966).withMultipliedAlpha (alpha), body.getX(), 0.0f, juce::Colour (0xff1d1c1b).withMultipliedAlpha (alpha),
                                                 body.getRight(), 0.0f, false));
        g.fillRoundedRectangle (body, r * 0.3f);

        // The capsule: its shadow, a dark mesh, the ring in the mic's colour, and its letter beside it.
        g.setColour (juce::Colours::black.withAlpha (0.5f * alpha));
        g.fillEllipse (juce::Rectangle<float> (2.0f * r + 4.0f, 2.0f * r + 4.0f).withCentre (p.translated (2.0f, 3.0f)));
        const auto capsule = juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (p);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3835).withMultipliedAlpha (alpha), capsule.getX(), capsule.getY(),
                                                 juce::Colour (0xff0c0c0b).withMultipliedAlpha (alpha), capsule.getRight(), capsule.getBottom(), false));
        g.fillEllipse (capsule);
        {
            const juce::Graphics::ScopedSaveState saved (g);
            juce::Path round;
            round.addEllipse (capsule.reduced (2.0f));
            g.reduceClipRegion (round);
            g.setColour (juce::Colours::white.withAlpha (0.10f * alpha));
            for (auto x = capsule.getX(); x < capsule.getRight(); x += 3.0f)
                g.drawVerticalLine (juce::roundToInt (x), capsule.getY(), capsule.getBottom());
            for (auto y = capsule.getY(); y < capsule.getBottom(); y += 3.0f)
                g.drawHorizontalLine (juce::roundToInt (y), capsule.getX(), capsule.getRight());
        }
        if (lifted)
        {
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.35f), p, colour.withAlpha (0.0f), p.translated (r + 10.0f, 0.0f), true));
            g.fillEllipse (capsule.expanded (10.0f));
        }
        g.setColour (colour.withMultipliedAlpha (alpha));
        g.drawEllipse (capsule.reduced (0.75f), lifted ? 2.5f : 2.0f);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (p));

        // The letter on a small tag up and to the right.
        const auto tag = juce::Rectangle<float> (18.0f, 16.0f).withPosition (p.x + r * 0.7f, p.y - r - 12.0f);
        g.setColour (juce::Colours::black.withAlpha (0.65f * alpha));
        g.fillRoundedRectangle (tag, 4.0f);
        g.setColour (colour.withMultipliedAlpha (alpha));
        g.setFont (geist (Weight::semibold, 12.0f));
        g.drawText (m == 0 ? "A" : "B", tag, juce::Justification::centred, false);
    }

    AmpSimProcessor& ampSim;
    std::array<juce::RangedAudioParameter*, 2> position {}, distance {};
    juce::Image art;
    int dragging = -1, hovered = -1;
    juce::Point<float> grabOffset;
};

// ---- A mic's file link ---------------------------------------------------------------------------

class CabView::MicLink final : public juce::Component, public juce::SettableTooltipClient
{
public:
    MicLink (CabView& v, int m) : view (v), mic (m) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void set (const juce::String& newText)
    {
        if (newText != text)
        {
            text = newText;
            setTooltip (text + ": click to load an IR or a cab pack, or to clear it");
            repaint();
        }
    }
    juce::String text;

    void paint (juce::Graphics& g) override
    {
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (isMouseOver() ? inkDim : inkFaint);
        g.drawFittedText (text, getLocalBounds(), juce::Justification::centredLeft, 1, 0.85f);
    }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && view.onMicMenu)
            view.onMicMenu (mic, *this);
    }

private:
    CabView& view;
    const int mic;
};

// ---- The cab in the inspector ----------------------------------------------------------------------

/// The cab's inspector: its switch, the two close mics (each its file, Distance and Level, Flip phase and Mute, its
/// pan, delay, and channel), and the cuts. The controls are the view's (made, attached, and owned there); this
/// holds and lays them out.
class CabView::Panel final : public ControlGroup
{
public:
    explicit Panel (CabView& v) : ControlGroup (v.getProcessor()), view (v)
    {
        addAndMakeVisible (*view.cabOn);
        for (int m = 0; m < 2; ++m)
        {
            auto& c = view.mics[(size_t) m];
            addAndMakeVisible (*view.links[(size_t) m]);
            for (auto* control : std::initializer_list<juce::Component*> { c.distance, c.level, c.flip, c.mute, c.pan, c.delay, c.channel })
                addAndMakeVisible (*control);
        }
        for (auto* control : std::initializer_list<juce::Component*> { view.lowCut, view.highCut, view.lowCutOn, view.highCutOn, view.lowSlope, view.highSlope })
            addAndMakeVisible (*control);
        view.cabOn->onStateChange = [this] { repaint (0, 0, getWidth(), BlockPage::headerHeight (subtitle, getWidth())); };
    }

    static inline const juce::String subtitle { "After Gate B. Drag the mics on the speaker; the library and the room mic are on the stage" };

    void layout (InspectorLayout& l)
    {
        // Each close mic: its heading (Mute at its right), its file, Distance and Level, its pan, delay, and channel,
        // and the phase flip. Then the cuts.
        for (int m = 0; m < 2; ++m)
        {
            auto& c = view.mics[(size_t) m];
            l.section (m == 0 ? "Mic A, close mic 1" : "Mic B, close mic 2", c.mute);
            juce::Rectangle<int> linkArea;
            l.area (&linkArea, 18);
            if (l.isPlacing())
            {
                linkRows[(size_t) m] = linkArea;
                view.links[(size_t) m]->setBounds (linkArea.withTrimmedLeft (18));
            }
            l.knobs ({ c.distance, c.level });
            l.fields ({ { "Pan", c.pan }, { "Delay", c.delay }, { "Channel", c.channel } });
            l.switches ({ c.flip });
        }
        l.section ("Cuts");
        l.switches ({ view.lowCutOn, view.highCutOn });
        l.knobs ({ view.lowCut, view.highCut });
        l.fields ({ { "Low cut slope", view.lowSlope }, { "High cut slope", view.highSlope } }, 2);
    }

    int preferredHeight (int width) override
    {
        InspectorLayout l ({ BlockPage::padding, BlockPage::headerHeight (subtitle, width) + BlockPage::padding, width - 2 * BlockPage::padding, 0 }, false);
        layout (l);
        return l.getBottom() + BlockPage::padding + 8;
    }

    void resized() override
    {
        view.cabOn->setBounds (BlockPage::headerSwitchBounds (getWidth(), view.cabOn->getPreferredWidth()));
        InspectorLayout l ({ BlockPage::padding, BlockPage::headerHeight (subtitle, getWidth()) + BlockPage::padding, getWidth() - 2 * BlockPage::padding, 0 },
                           true);
        layout (l);
        texts = l.getTexts();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto on = view.cabOn->getToggleState();
        BlockPage::paintHeader (g, getWidth(), "Cab", subtitle,
                                [on] (juce::Graphics& gg, juce::Rectangle<float> box) { drawCabFront (gg, box.withSizeKeepingCentre (box.getWidth() - 2.0f, box.getHeight() - 8.0f), on); },
                                view.cabOn->getWidth());
        InspectorLayout::paintTexts (g, texts);

        // Each mic's colour, as its capsule on the speaker: a ringed dot before its file.
        for (int m = 0; m < 2; ++m)
        {
            const auto row = linkRows[(size_t) m];
            if (row.isEmpty())
                continue;
            const auto dot = juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ (float) row.getX() + 6.0f, (float) row.getCentreY() });
            g.setColour (m == 0 ? accent : ink);
            g.drawEllipse (dot, 1.5f);
            g.fillEllipse (dot.reduced (3.0f));
        }
    }

private:
    CabView& view;
    std::vector<InspectorLayout::Text> texts;
    std::array<juce::Rectangle<int>, 2> linkRows;
};

// ---- CabView -------------------------------------------------------------------------------------

CabView::CabView (AmpSimProcessor& p) : ControlGroup (p)
{
    list = std::make_unique<LibraryList> (*this);
    listViewport = std::make_unique<juce::Viewport>();
    listViewport->setViewedComponent (list.get(), false);
    listViewport->setScrollBarsShown (true, false);
    listViewport->setScrollBarThickness (6);
    addAndMakeVisible (*listViewport);

    follow = &adopt (std::make_unique<Switch> ("Follow amp choice"));
    follow->setTooltip ("Switching amps loads the cab last picked for that amp into close mic 1. Picking a cab here turns this off.");
    follow->onClick = [this] { ampSim.setCabFollow (follow->getToggleState()); };
    follow->setClickingTogglesState (true);
    dropZone = std::make_unique<DropZone> (*this);
    addAndMakeVisible (*dropZone);
    speaker = std::make_unique<Speaker> (p);
    addAndMakeVisible (*speaker);
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        links[(size_t) m] = std::make_unique<MicLink> (*this, m);
        addAndMakeVisible (*links[(size_t) m]);
    }

    for (int m = 0; m < 2; ++m)
    {
        auto& c = mics[(size_t) m];
        c.distance = &addKnob (AmpSimProcessor::cabParamId (m, "pos_y"), "Distance", "");
        c.distance->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; });
        c.distance->setTooltip ("Distance from the grille, from the pack's closest capture (0%) to its farthest (100%). Moves a mic with a cab pack.");
        c.level = &addKnob (AmpSimProcessor::cabParamId (m, "level"), "Level", " dB");
        c.flip = &addSwitch (AmpSimProcessor::cabParamId (m, "invert"), "Flip phase");
        c.mute = &addSwitch (AmpSimProcessor::cabParamId (m, "mute"), "Mute");
        c.pan = &addField (AmpSimProcessor::cabParamId (m, "pan"), "");
        c.delay = &addField (AmpSimProcessor::cabParamId (m, "delay"), "");
        c.delay->setShowsBar (false);
        c.channel = &addField (AmpSimProcessor::cabParamId (m, "channel"), "");
        c.channel->setShowsBar (false);
        c.channel->setTooltip ("Which channel of a stereo IR file this mic uses");
    }

    lowCut = &addKnob ("cab_lowcut_freq", "Low cut", " Hz");
    lowCut->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)) + " Hz"; });
    highCut = &addKnob ("cab_highcut_freq", "High cut", " Hz");
    highCut->setFormatter ([] (float v) { return juce::String (v / 1000.0f, 1) + " kHz"; });
    lowCutOn = &addSwitch ("cab_lowcut_on", "Low cut");
    highCutOn = &addSwitch ("cab_highcut_on", "High cut");
    lowCutOn->setTooltip ("Low cut on or off");
    highCutOn->setTooltip ("High cut on or off");
    lowSlope = &addField ("cab_lowcut_slope", "");
    highSlope = &addField ("cab_highcut_slope", "");
    for (auto* f : { lowSlope, highSlope })
    {
        f->setShowsBar (false);
        f->setFormatter ([] (float v) { return v >= 0.5f ? juce::String ("24 dB") : juce::String ("12 dB"); });
        f->setTooltip ("Slope per octave");
    }

    roomLevel =&addField (AmpSimProcessor::cabParamId (AmpSimProcessor::roomMic, "level"), " dB");
    roomPreDelay = &addField (AmpSimProcessor::cabParamId (AmpSimProcessor::roomMic, "predelay"), " ms");
    roomPreDelay->setShowsBar (false);
    roomLevel->setShowsBar (false);
    roomLevelLabel = &addLabel ("Level", Text::caption, inkFaint);
    roomPreDelayLabel = &addLabel ("Pre-delay", Text::caption, inkFaint);
    roomMute = &addSwitch (AmpSimProcessor::cabParamId (AmpSimProcessor::roomMic, "mute"), "Mute");
    align = &addSwitch ("cab_align", "Auto-align mics A and B");
    align->setTooltip ("Lines the two close mics up in time (and polarity), measured from their IRs");

    matchView = std::make_unique<MatchCurveView>();
    matchView->setTooltip ("The match curve: a fine correction after the cab, set by tone match");
    addAndMakeVisible (*matchView);
    matchOn = &addSwitch ("match_curve_on", "On");
    matchOn->setTooltip ("The match curve on or off");
    matchAmount = &addField ("match_curve_amount", "%");
    matchAmount->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)) + "%"; });
    matchAmount->setTooltip ("How much of the curve applies: its dB times this");
    matchAmountLabel = &addLabel ("Amount", Text::caption, inkFaint);

    // The cab's switch is its bypass, inverted: on while the cab plays (the board's cab marker has the same dot).
    cabOn = &adopt (std::make_unique<Switch> ("On"));
    cabOn->setClickingTogglesState (true);
    tagged (*cabOn, "cab_bypass");
    cabOn->setTooltip ("The cab on or off (off sends the amp's output on without a speaker)");
    cabOn->onClick = [this]
    {
        if (auto* bypass = state.getParameter ("cab_bypass"))
        {
            const auto wanted = cabOn->getToggleState() ? 0.0f : 1.0f;
            if (! juce::exactlyEqual (bypass->getValue(), wanted))
            {
                beginUndoStep (state);
                bypass->beginChangeGesture();
                bypass->setValueNotifyingHost (wanted);
                bypass->endChangeGesture();
            }
        }
    };

    panel = std::make_unique<Panel> (*this);
    rescan();
    refresh();
}

ControlGroup& CabView::getPanel() noexcept
{
    return *panel;
}

CabView::~CabView()
{
    listViewport->setViewedComponent (nullptr, false);
}

void CabView::rescan()
{
    entries.clear();

    // Built in: the IRs bundled with the app (content/irs). Each subfolder is one cab, listed under its
    // own heading; every IR in it is an entry of its own. A folder of bundled IRs is never treated as a
    // pack (that would map unrelated captures onto mic positions); only one with a cabpack.json is.
    // The list line under each name is the manifest's "description" for that file.
    const auto factory = presets::libraryRoot ("factory");
    std::map<juce::String, juce::String> descriptions;
    if (const auto manifest = juce::JSON::parse (factory.getChildFile ("manifest.json")); manifest.isObject())
        if (const auto* files = manifest["files"].getArray())
            for (const auto& f : *files)
                descriptions[f["path"].toString()] = f["description"].toString();

    const auto addBuiltIn = [&] (const juce::File& file, const juce::String& cab)
    {
        // "Modern 4x12, dynamic, 75 W, var. 1" under "Built in, Modern 4x12" reads "Dynamic, 75 W, var. 1".
        auto name = file.getFileNameWithoutExtension();
        if (cab.isNotEmpty() && name.startsWithIgnoreCase (cab + ", "))
            name = name.substring (cab.length() + 2);
        name = name.substring (0, 1).toUpperCase() + name.substring (1);
        const auto relative = file.getRelativePathFrom (factory).replaceCharacter ('\\', '/');
        const auto it = descriptions.find (relative);
        const auto description = it != descriptions.end() && it->second.isNotEmpty() ? it->second : juce::String ("Built-in impulse response");
        entries.push_back ({ file, name, description, false, true, cab.isEmpty() ? juce::String ("Built in") : "Built in, " + cab });
    };
    if (const auto builtIn = factory.getChildFile ("irs"); builtIn.isDirectory())
    {
        const auto first = entries.size();
        auto children = builtIn.findChildFiles (juce::File::findFilesAndDirectories, false);
        for (const auto& child : children)
        {
            if (child.isDirectory() && child.getChildFile ("cabpack.json").existsAsFile())
                entries.push_back ({ child, child.getFileName(), "Cab pack, " + juce::String (audioFilesIn (child)) + " captures", true, true, "Built in" });
            else if (child.isDirectory())
            {
                for (const auto& f : child.findChildFiles (juce::File::findFiles, false))
                    if (isAudioFile (f))
                        addBuiltIn (f, child.getFileName());
            }
            else if (isAudioFile (child))
            {
                addBuiltIn (child, {});
            }
        }
        // Loose files first, then each cab's folder in name order; inside a folder, by name.
        std::stable_sort (entries.begin() + (std::ptrdiff_t) first, entries.end(), [] (const Entry& a, const Entry& b)
                          {
                              if (a.group != b.group)
                                  return a.group == "Built in" || (b.group != "Built in" && a.group.compareNatural (b.group) < 0);
                              return a.name.compareNatural (b.name) < 0;
                          });
    }
    const auto numBuiltIn = entries.size();

    // The user's library: every cab pack folder (a manifest, or two or more IRs) and every IR file that
    // isn't in a pack, up to three folders deep, sorted by name.
    const auto root = presets::libraryRoot ("irs");
    std::function<void (const juce::File&, int)> scan = [&] (const juce::File& folder, int depth)
    {
        auto children = folder.findChildFiles (juce::File::findFilesAndDirectories, false);
        children.sort();
        for (const auto& child : children)
        {
            if (child.isDirectory())
            {
                if (isPack (child))
                    entries.push_back ({ child, child.getFileName(), "Cab pack, " + juce::String (audioFilesIn (child)) + " captures", true, false, "Your library" });
                else if (depth < 3)
                    scan (child, depth + 1);
            }
            else if (isAudioFile (child))
            {
                const auto where = folder == root ? juce::String() : ", " + folder.getFileName();
                entries.push_back ({ child, child.getFileNameWithoutExtension(), "Impulse response" + where, false, false, "Your library" });
            }
        }
    };
    if (root.isDirectory() && root != factory.getChildFile ("irs"))
        scan (root, 0);
    std::stable_sort (entries.begin() + (std::ptrdiff_t) numBuiltIn, entries.end(),
                      [] (const Entry& a, const Entry& b) { return a.name.compareNatural (b.name) < 0; });
    resized();
    list->repaint();
}

void CabView::layoutList()
{
    // Headings only when there's something built in: a library-only list looks as it always did.
    entryTops.clear();
    headings.clear();
    libraryHintTop = -1;
    const auto anyBuiltIn = std::any_of (entries.begin(), entries.end(), [] (const Entry& e) { return e.builtIn; });
    int y = 0;
    juce::String group;
    for (const auto& e : entries)
    {
        if (anyBuiltIn && e.group != group)
        {
            group = e.group;
            headings.push_back ({ y, group });
            y += groupHeadingHeight;
        }
        entryTops.push_back (y);
        y += entryHeight;
    }
    if (anyBuiltIn && group != "Your library")
    {
        // Nothing in the user's library yet: its heading, and where to put files.
        headings.push_back ({ y, "Your library" });
        y += groupHeadingHeight;
        libraryHintTop = y;
        y += libraryHintHeight;
    }
    listContentHeight = y;
}

juce::StringArray CabView::getGroupHeadings() const
{
    juce::StringArray result;
    for (const auto& h : headings)
        result.add (h.second);
    return result;
}

int CabView::getAlignSwitchBottom() const
{
    return align->getBottom();
}

int CabView::entryTop (int index) const
{
    return index >= 0 && index < (int) entryTops.size() ? entryTops[(size_t) index] : -1;
}

void CabView::scrollSelectionIntoView()
{
    // When close mic 1's file changes (a preset, Follow, a pick), bring its entry into view if it's listed.
    if (list->selectedPath == scrolledTo)
        return;
    scrolledTo = list->selectedPath;
    for (size_t i = 0; i < entries.size() && i < entryTops.size(); ++i)
        if (entries[i].file.getFullPathName() == scrolledTo)
        {
            const auto top = entryTops[i], view = listViewport->getViewPositionY(), height = listViewport->getMaximumVisibleHeight();
            if (top < view || top + entryHeight > view + height)
                listViewport->setViewPosition (0, juce::jmax (0, top - (i > 0 && entries[i - 1].group != entries[i].group ? groupHeadingHeight : 0)));
            return;
        }
}

void CabView::pick (const juce::File& fileOrPack)
{
    ampSim.pickCab (fileOrPack);
    ampSim.setCabFollow (false);
    refresh();
}

void CabView::clickEntry (int index)
{
    if (index >= 0 && index < (int) entries.size())
        pick (entries[(size_t) index].file);
}

juce::Point<float> CabView::markerPosition (int mic) const
{
    return speaker->marker (mic) + speaker->getPosition().toFloat();
}

void CabView::dragMarker (int mic, juce::Point<float> from, juce::Point<float> to)
{
    juce::ignoreUnused (mic);
    const auto local = [this] (juce::Point<float> p) { return p - speaker->getPosition().toFloat(); };
    const auto now = juce::Time::getCurrentTime();
    const auto event = [&] (juce::Point<float> at, bool dragged)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), local (at), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                 juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, speaker.get(), speaker.get(), now, local (from), now, 1, dragged);
    };
    speaker->mouseDown (event (from, false));
    for (int step = 1; step <= 10; ++step)
        speaker->mouseDrag (event (from + (to - from) * ((float) step / 10.0f), true));
    speaker->mouseUp (event (to, true));
}

juce::String CabView::getReadout() const
{
    return readout;
}

bool CabView::isMarkerDimmed (int mic) const
{
    return ! speaker->hasPack[(size_t) juce::jlimit (0, 1, mic)];
}

void CabView::refresh()
{
    const auto status = ampSim.getStatus();
    list->selectedPath = state.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString();
    list->repaint();
    scrollSelectionIntoView();
    follow->setToggleState (ampSim.isCabFollowing(), juce::dontSendNotification);
    cabOn->setToggleState (state.getRawParameterValue ("cab_bypass")->load() < 0.5f, juce::dontSendNotification);

    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        const auto path = state.state.getProperty (AmpSimProcessor::cabPathKey (m)).toString();
        auto shown = juce::File::isAbsolutePath (path) ? juce::File (path).getFileName() : juce::String ("Load an IR");
        if (status.cabError[(size_t) m])
            shown = "Missing: " + shown;
        links[(size_t) m]->set (shown);
    }

    // Packs make mics movable; a plain IR's marker is dimmed.
    bool changed = false;
    for (int m = 0; m < 2; ++m)
    {
        const auto points = ampSim.getCabPackPoints (m);
        const auto pack = ! points.empty();
        changed = changed || pack != speaker->hasPack[(size_t) m];
        speaker->hasPack[(size_t) m] = pack;
        // The pack's capture positions across the cone, each once (the speaker draws them along the mic's way).
        std::vector<float> across;
        for (const auto& point : points)
            if (std::none_of (across.begin(), across.end(), [&point] (float x) { return std::abs (x - (float) point.x) < 0.01f; }))
                across.push_back ((float) point.x);
        speaker->packPositions[(size_t) m] = std::move (across);
    }
    juce::String newReadout;
    if (! speaker->hasPack[0] && ! speaker->hasPack[1])
        newReadout = "Load a pack to move mics";
    else
        for (int m = 0; m < 2; ++m)
            newReadout << (m == 0 ? "Mic A|" : "|Mic B|") << (speaker->hasPack[(size_t) m] ? zoneFor (speaker->radiusOf (m)) : juce::String ("no pack"));
    if (newReadout != readout || changed)
    {
        readout = newReadout;
        repaint();
    }
    speaker->repaint();

    matchView->set (ampSim.getMatchCurve(), state.getRawParameterValue ("match_curve_amount")->load(),
                    state.getRawParameterValue ("match_curve_on")->load() >= 0.5f);
}

juce::Rectangle<int> CabView::getMatchCurveViewBounds() const { return matchView->getBounds(); }
std::vector<float> CabView::getMatchCurveDrawnDb() const { return matchView->getDrawnDb(); }
bool CabView::isMatchCurveDrawnOn() const { return matchView->isOn(); }

void CabView::pageShown()
{
    rescan();
    refresh();
}

void CabView::paint (juce::Graphics& g)
{
    // The headings, as the inspector's (small capitals and a hairline).
    drawInspectorHeading (g, leftColumn.withHeight (16), "Cabinet");
    drawInspectorHeading (g, roomHeading, "Room mic");
    drawInspectorHeading (g, matchHeading.withRight (matchAmountLabel->getX() - 4), "Match curve");
}

void CabView::paintOverChildren (juce::Graphics& g)
{
    // The mics' zones on the baffle's top corners ("A Cap edge", "B Cone"), or what moves them, at its foot.
    const auto baffle = speaker->getBounds().toFloat();
    const auto chip = [&g] (juce::Rectangle<float> r, juce::Colour dot, const juce::String& letter, const juce::String& words)
    {
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (line1);
        g.drawRoundedRectangle (r.reduced (0.5f), r.getHeight() * 0.5f, 1.0f);
        auto inner = r.reduced (10.0f, 0.0f);
        if (letter.isNotEmpty())
        {
            g.setColour (dot);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ inner.getX() + 3.0f, inner.getCentreY() }));
            inner.removeFromLeft (11.0f);
            g.setFont (geist (Weight::semibold, 12.0f));
            g.drawText (letter, inner.removeFromLeft (12.0f), juce::Justification::centredLeft, false);
        }
        g.setFont (geist (letter.isNotEmpty() ? Weight::medium : Weight::regular, 12.0f));
        g.setColour (letter.isNotEmpty() ? ink : inkDim);
        g.drawText (words, inner, letter.isNotEmpty() ? juce::Justification::centredLeft : juce::Justification::centred, false);
    };
    if (! readout.contains ("|"))
    {
        const auto w = textWidth (geist (Weight::regular, 12.0f), readout) + 24.0f;
        chip (juce::Rectangle<float> (w, 24.0f).withCentre ({ baffle.getCentreX(), baffle.getBottom() - 24.0f }), {}, {}, readout);
        return;
    }
    const auto parts = juce::StringArray::fromTokens (readout, "|", "");
    for (int m = 0; m < 2 && 2 * m + 1 < parts.size(); ++m)
    {
        const auto words = parts[2 * m + 1];
        const auto w = 10.0f + 11.0f + 12.0f + textWidth (geist (Weight::medium, 12.0f), words) + 12.0f;
        const auto r = m == 0 ? juce::Rectangle<float> (baffle.getX() + 12.0f, baffle.getY() + 12.0f, w, 24.0f)
                              : juce::Rectangle<float> (baffle.getRight() - 12.0f - w, baffle.getY() + 12.0f, w, 24.0f);
        chip (r, m == 0 ? accent : ink, m == 0 ? "A" : "B", words);
    }
}

void CabView::resized()
{
    // Three columns on the stage, under its top line (the stage's Amp / Cab / EQ control is at its right end):
    // the library on the left, the speaker in the middle, the match curve and the room mic on the right.
    clearHeadings();
    constexpr int side = 24, top = 22, bottomPad = 40;
    auto area = getLocalBounds().withTrimmedLeft (side).withTrimmedRight (side).withTrimmedTop (top).withTrimmedBottom (bottomPad);
    leftColumn = area.removeFromLeft (leftWidth);
    area.removeFromLeft (columnGap);
    rightColumn = area.removeFromRight (rightWidth).withTrimmedTop (38); // under the stage's control
    area.removeFromRight (columnGap);
    centreColumn = area;

    // Left: the heading, the list (it scrolls), Follow, the drop zone at the bottom.
    constexpr int dropHeight = 62;
    dropZone->setBounds (leftColumn.getX(), leftColumn.getBottom() - dropHeight, leftColumn.getWidth(), dropHeight);
    const auto followY = dropZone->getY() - 12 - 16;
    follow->setBounds (leftColumn.getX() - Switch::margin, followY - Switch::margin, follow->getPreferredWidth(), Switch::preferredHeight);
    const auto listTop = leftColumn.getY() + 16 + 12;
    layoutList();
    const auto room = followY - 14 - listTop;
    const auto listHeight = entries.empty() ? juce::jmin (room, 76) : juce::jmin (listContentHeight, room);
    const auto scrolls = ! entries.empty() && listContentHeight > room;
    listViewport->setBounds (leftColumn.getX(), listTop, leftColumn.getWidth(), listHeight);
    list->setSize (leftColumn.getWidth() - (scrolls ? 8 : 0), entries.empty() ? listHeight : listContentHeight);

    // Centre: the speaker in its baffle, as large as the stage's height allows (the message line keeps the foot).
    const auto speakerSize = juce::jmin (centreColumn.getWidth(), getHeight() - 14 - 36);
    speaker->scale = (float) speakerSize / 400.0f;
    speaker->setBounds (juce::Rectangle<int> (speakerSize, speakerSize).withCentre ({ centreColumn.getCentreX(), 14 + speakerSize / 2 }));

    // Right: the match curve's line (its heading, Amount, and switch) over its view; then the room mic and the
    // alignment.
    {
        constexpr int viewHeight = 64;
        auto strip = rightColumn.withHeight (22 + 8 + viewHeight);
        auto row = strip.removeFromTop (22);
        strip.removeFromTop (8);
        matchView->setBounds (strip);
        matchHeading = row.withWidth (100).withSizeKeepingCentre (100, 16);
        matchOn->setBounds (row.getRight() - matchOn->getPreferredWidth() + Switch::margin, row.getCentreY() - Switch::preferredHeight / 2,
                            matchOn->getPreferredWidth(), Switch::preferredHeight);
        auto left = row.withRight (matchOn->getX() - 8);
        matchAmount->setBounds (left.removeFromRight (50));
        left.removeFromRight (2);
        matchAmountLabel->setBounds (left.removeFromRight (juce::roundToInt (textWidth (font (Text::caption), "Amount")) + 4));
    }

    auto y = matchView->getBottom() + 28;
    roomHeading = { rightColumn.getX(), y, rightColumn.getWidth(), 16 };
    y += 16 + 10;
    auto roomRow = juce::Rectangle<int> (rightColumn.getX(), y, rightColumn.getWidth(), 22);
    roomLevelLabel->setBounds (roomRow.removeFromLeft (juce::roundToInt (textWidth (font (Text::caption), "Level")) + 4));
    roomLevel->setBounds (roomRow.removeFromLeft (70));
    roomRow.removeFromLeft (12);
    roomPreDelayLabel->setBounds (roomRow.removeFromLeft (juce::roundToInt (textWidth (font (Text::caption), "Pre-delay")) + 4));
    roomPreDelay->setBounds (roomRow.removeFromLeft (juce::jmin (70, roomRow.getWidth())));
    y += 22 + 10;
    roomMute->setBounds (rightColumn.getX() - Switch::margin, y - Switch::margin, roomMute->getPreferredWidth(), Switch::preferredHeight);
    links[(size_t) AmpSimProcessor::roomMic]->setBounds (rightColumn.getX() + 80, y, rightColumn.getWidth() - 80, 16);
    y += 16 + 18;
    align->setBounds (rightColumn.getX() - Switch::margin, y - Switch::margin, align->getPreferredWidth(), Switch::preferredHeight);
}

// ---- Dropping files ------------------------------------------------------------------------------

bool CabView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (const juce::File f (path); isAudioFile (f) || (f.isDirectory() && isPack (f)))
            return true;
    return false;
}

void CabView::fileDragEnter (const juce::StringArray&, int, int)
{
    dropZone->highlighted = true;
    dropZone->repaint();
}

void CabView::fileDragExit (const juce::StringArray&)
{
    dropZone->highlighted = false;
    dropZone->repaint();
}

void CabView::filesDropped (const juce::StringArray& files, int, int)
{
    dropZone->highlighted = false;
    dropZone->repaint();
    for (const auto& path : files)
        if (const juce::File f (path); isAudioFile (f) || (f.isDirectory() && isPack (f)))
        {
            pick (f);
            return;
        }
}

} // namespace ui

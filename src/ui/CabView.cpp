// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "CabView.h"
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

/// The reference's speaker (outer ring, cone rings, dust cap, crosshair) at 380 px, and the two close
/// mics' markers on it.
class CabView::Speaker final : public juce::Component
{
public:
    explicit Speaker (AmpSimProcessor& p) : ampSim (p)
    {
        for (int m = 0; m < 2; ++m)
            position[(size_t) m] = p.parameters.getParameter (AmpSimProcessor::cabParamId (m, "pos_x"));
    }

    // The markers' angles: for looks only, never saved (UH8). The reference's starting places.
    std::array<float, 2> angle { std::atan2 (-0.05f, -0.12f), std::atan2 (0.30f, 0.42f) };
    std::array<bool, 2> hasPack {};
    float scale = 0.95f; // pixels per SVG unit: the component is the speaker's 400 units across

    float radiusOf (int mic) const { return position[(size_t) mic]->convertFrom0to1 (position[(size_t) mic]->getValue()) * maxRadius; }

    juce::Point<float> marker (int mic) const
    {
        const auto r = radiusOf (mic) * speakerRadius * scale;
        return getLocalBounds().toFloat().getCentre() + juce::Point<float> (std::cos (angle[(size_t) mic]), std::sin (angle[(size_t) mic])) * r;
    }

    void paint (juce::Graphics& g) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        const auto ring = [this, &g, c] (float r, juce::Colour colour)
        {
            g.setColour (colour);
            g.drawEllipse (juce::Rectangle<float> (2.0f * r * scale, 2.0f * r * scale).withCentre (c), 1.0f);
        };
        ring (190.0f, line1);
        ring (160.0f, line2);
        for (auto r : { 132.0f, 104.0f, 76.0f })
            ring (r, line1);
        const auto cap = juce::Rectangle<float> (88.0f * scale, 88.0f * scale).withCentre (c);
        g.setColour (surface);
        g.fillEllipse (cap);
        ring (44.0f, line2);
        g.setColour (line2);
        g.drawLine (c.x - 6.0f * scale, c.y, c.x + 6.0f * scale, c.y, 1.0f);
        g.drawLine (c.x, c.y - 6.0f * scale, c.x, c.y + 6.0f * scale, 1.0f);

        // The markers: an 11 unit ring (filled with the background) with a 3 unit dot, the letter up and
        // to the right; mic B first, so A sits on top. A mic without a pack is dimmed.
        for (int m = 1; m >= 0; --m)
        {
            const auto colour = (m == 0 ? accent : ink).withMultipliedAlpha (hasPack[(size_t) m] ? 1.0f : 0.35f);
            const auto p = marker (m);
            g.setColour (bg);
            g.fillEllipse (juce::Rectangle<float> (22.0f * scale, 22.0f * scale).withCentre (p));
            g.setColour (colour);
            g.drawEllipse (juce::Rectangle<float> (22.0f * scale, 22.0f * scale).withCentre (p), 1.5f * scale);
            g.fillEllipse (juce::Rectangle<float> (6.0f * scale, 6.0f * scale).withCentre (p));
            g.setFont (geist (Weight::medium, 13.0f * scale));
            g.drawText (m == 0 ? "A" : "B", juce::Rectangle<float> (p.x + 18.0f * scale, p.y - 12.0f * scale - 13.0f, 20.0f, 14.0f),
                        juce::Justification::bottomLeft, false);
        }
    }

    int markerAt (juce::Point<float> p) const
    {
        for (int m = 0; m < 2; ++m)
            if (hasPack[(size_t) m] && marker (m).getDistanceFrom (p) <= 20.0f * scale)
                return m;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (markerAt (e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
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
        moveTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging >= 0 && ! e.mods.isPopupMenu())
            moveTo (e.position);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
            position[(size_t) dragging]->endChangeGesture();
        dragging = -1;
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

private:
    AmpSimProcessor& ampSim;
    std::array<juce::RangedAudioParameter*, 2> position {};
    int dragging = -1;
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
    }

    static inline const juce::String subtitle { "Close mics A and B, and the cuts. The library, the speaker, the room mic, and the match curve are on the stage" };

    void layout (InspectorLayout& l)
    {
        for (int m = 0; m < 2; ++m)
        {
            auto& c = view.mics[(size_t) m];
            l.section (m == 0 ? "Mic A, close mic 1" : "Mic B, close mic 2");
            juce::Rectangle<int> linkArea;
            l.area (&linkArea, 18);
            if (l.isPlacing())
                view.links[(size_t) m]->setBounds (linkArea.withTrimmedLeft (16));
            l.knobs ({ c.distance, c.level });
            l.switches ({ c.flip, c.mute });
            l.fields ({ { "Pan", c.pan }, { "Delay", c.delay }, { "Channel", c.channel } });
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
        const auto w = view.cabOn->getPreferredWidth();
        view.cabOn->setBounds (getWidth() - BlockPage::padding - w + Switch::margin, BlockPage::padding + (22 - Switch::preferredHeight) / 2, w,
                               Switch::preferredHeight);
        InspectorLayout l ({ BlockPage::padding, BlockPage::headerHeight (subtitle, getWidth()) + BlockPage::padding, getWidth() - 2 * BlockPage::padding, 0 },
                           true);
        layout (l);
        texts = l.getTexts();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto width = getWidth() - 2 * BlockPage::padding;
        g.setFont (geist (Weight::semibold, 16.0f));
        g.setColour (ink);
        g.drawText ("Cab", juce::Rectangle<int> (BlockPage::padding, BlockPage::padding, width - 60, 22), juce::Justification::centredLeft, false);
        const auto height = BlockPage::headerHeight (subtitle, getWidth());
        g.setFont (font (Text::label));
        g.setColour (inkFaint);
        const juce::Rectangle<int> lines (BlockPage::padding, BlockPage::padding + 26, width, height - BlockPage::padding - 26 - 14);
        g.drawFittedText (subtitle, lines, juce::Justification::topLeft, juce::jmax (1, lines.getHeight() / 16), 1.0f);
        g.setColour (line1);
        g.fillRect (BlockPage::padding, height - 1, width, 1);
        InspectorLayout::paintTexts (g, texts);

        // Each mic's colour, as its marker on the speaker: a dot before its file.
        for (int m = 0; m < 2; ++m)
        {
            const auto link = view.links[(size_t) m]->getBounds();
            g.setColour (m == 0 ? accent : ink);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) link.getX() - 12.0f, (float) link.getCentreY() }));
        }
    }

private:
    CabView& view;
    std::vector<InspectorLayout::Text> texts;
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
        c.distance = &addKnob (AmpSimProcessor::cabParamId (m, "pos_y"), "Distance", "", Knob::Size::compact);
        c.distance->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; });
        c.distance->setTooltip ("Distance from the grille, from the pack's closest capture (0%) to its farthest (100%). Moves a mic with a cab pack.");
        c.level = &addKnob (AmpSimProcessor::cabParamId (m, "level"), "Level", " dB", Knob::Size::compact);
        c.flip = &addSwitch (AmpSimProcessor::cabParamId (m, "invert"), "Flip phase");
        c.mute = &addSwitch (AmpSimProcessor::cabParamId (m, "mute"), "Mute");
        c.pan = &addField (AmpSimProcessor::cabParamId (m, "pan"), "");
        c.delay = &addField (AmpSimProcessor::cabParamId (m, "delay"), "");
        c.delay->setShowsBar (false);
        c.channel = &addField (AmpSimProcessor::cabParamId (m, "channel"), "");
        c.channel->setShowsBar (false);
        c.channel->setTooltip ("Which channel of a stereo IR file this mic uses");
    }

    lowCut = &addKnob ("cab_lowcut_freq", "Low cut", " Hz", Knob::Size::compact);
    lowCut->setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)) + " Hz"; });
    highCut = &addKnob ("cab_highcut_freq", "High cut", " Hz", Knob::Size::compact);
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
        const auto pack = ! ampSim.getCabPackPoints (m).empty();
        changed = changed || pack != speaker->hasPack[(size_t) m];
        speaker->hasPack[(size_t) m] = pack;
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
    // The headings (12 px medium ink-faint).
    g.setFont (geist (Weight::medium, 12.0f));
    g.setColour (inkFaint);
    g.drawText ("Cabinet", leftColumn.withHeight (16), juce::Justification::centredLeft, false);
    g.drawText ("Room mic", roomHeading, juce::Justification::centredLeft, false);
    g.drawText ("Match curve", matchHeading, juce::Justification::centredLeft, false);
    g.setColour (line1);
    g.fillRect (rightColumn.getX(), roomHeading.getY() - 14, rightColumn.getWidth(), 1);

    // The readout under the speaker: "Mic A Cap   Mic B Cone" (12 px faint, the zone in ink, medium).
    const auto readoutArea = juce::Rectangle<int> (centreColumn.getX(), speaker->getBottom() + 10, centreColumn.getWidth(), 16);
    if (! readout.contains ("|"))
    {
        g.setFont (geist (Weight::regular, 12.0f));
        g.setColour (inkFaint);
        g.drawText (readout, readoutArea, juce::Justification::centred, false);
    }
    else
    {
        const auto parts = juce::StringArray::fromTokens (readout, "|", "");
        const auto label = geist (Weight::regular, 12.0f), value = geist (Weight::medium, 12.0f);
        float total = 0.0f;
        for (int i = 0; i < parts.size(); ++i)
            total += textWidth (i % 2 == 0 ? label : value, parts[i]) + (i % 2 == 0 ? 4.0f : (i + 1 < parts.size() ? 28.0f : 0.0f));
        auto x = (float) readoutArea.getCentreX() - total * 0.5f;
        for (int i = 0; i < parts.size(); ++i)
        {
            const auto& f = i % 2 == 0 ? label : value;
            g.setFont (f);
            g.setColour (i % 2 == 0 ? inkFaint : ink);
            const auto w = textWidth (f, parts[i]);
            g.drawText (parts[i], juce::Rectangle<float> (x, (float) readoutArea.getY(), w + 2.0f, 16.0f), juce::Justification::centredLeft, false);
            x += w + (i % 2 == 0 ? 4.0f : 28.0f);
        }
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

    // Centre: the speaker as large as the column allows, over its readout.
    const auto speakerSize = (float) juce::jmin (centreColumn.getWidth(), centreColumn.getHeight() - 30);
    speaker->scale = speakerSize / 400.0f;
    speaker->setBounds (juce::Rectangle<int> ((int) speakerSize, (int) speakerSize).withCentre ({ centreColumn.getCentreX(), centreColumn.getY() + (int) speakerSize / 2 }));

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
        matchAmount->setBounds (left.removeFromRight (56));
        left.removeFromRight (4);
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

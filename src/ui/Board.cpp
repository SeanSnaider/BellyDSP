// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Board.h"
#include "AmpHead.h"
#include "InspectorParts.h"

namespace ui
{

using namespace theme;

namespace
{
constexpr float pedalRows = 172.0f + 2.0f * pedalMargin; // a pedal's box height in enclosure points
constexpr int itemGap = 2;                                 // between boxes (the boxes' own margins do the spacing)

ampsim::Chain::Section chainSection (int row)
{
    return row == 0 ? ampsim::Chain::Section::pre : ampsim::Chain::Section::post;
}

Section sectionOfRow (int row)
{
    return row == 0 ? Section::pre : Section::post;
}

double nowSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}
} // namespace

// ---- A pedal -------------------------------------------------------------------------------------------

class Board::Pedal final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Pedal (Board& b, AmpSimProcessor& p, BlockId blockId) : id (blockId), board (b), ampSim (p)
    {
        const auto& i = info (id);
        parameter = p.parameters.getParameter (i.onParameter);
        tagged (*this, i.onParameter); // a right-click learns MIDI for its switch
        setTitle (i.name);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        const auto where = i.section == Section::pre ? juce::String ("before the amp") : i.section == Section::post ? juce::String ("after the cab")
                                                                                                                    : juce::String ("after the amp");
        setTooltip (juce::String (i.name) + " (" + where + "): click the footswitch to switch it on or off, anywhere else to see its controls"
                    + (i.section == Section::amp ? juce::String() : juce::String (". Drag it along the row to move it")));
        on = isOn();
    }

    const BlockId id;

    bool isOn() const { return parameter != nullptr && parameter->getValue() >= 0.5f; }
    bool reorderable() const { return info (id).section == Section::pre || info (id).section == Section::post; }

    /// The footswitch's reach in this component: around the nut, down to the body's foot.
    juce::Rectangle<float> footswitchArea() const
    {
        const auto fit = pedalBounds (getLocalBounds().toFloat(), id);
        const auto k = fit.getWidth() / pedalSize (id).x;
        const auto layout = pedalLayout (id);
        const auto nut = layout.footswitch;
        const auto bodyBottom = fit.getY() + layout.body.getBottom() * k;
        const auto cx = fit.getX() + nut.getCentreX() * k, top = fit.getY() + (nut.getY() - 12.0f) * k;
        return { cx - 26.0f * k, top, 52.0f * k, bodyBottom - top };
    }

    /// Updates the LED from the parameter; true if it changed (a switch-on starts the glow).
    bool poll()
    {
        const auto now = isOn();
        if (now == on)
            return false;
        on = now;
        if (on)
            glowStart = nowSeconds();
        repaint();
        return true;
    }

    float glowLeft() const
    {
        if (glowStart < 0.0)
            return 0.0f;
        return (float) juce::jlimit (0.0, 1.0, 1.0 - (nowSeconds() - glowStart) / ledGlowSeconds);
    }

    void paint (juce::Graphics& g) override
    {
        const auto box = getLocalBounds().toFloat();
        drawPedal (g, box, id, { on, board.selected == id, hovered && board.dragged != id, sectionOff });

        // An LED that has just come on blooms for a moment (the art's own glow is the steady state).
        if (const auto left = glowLeft(); left > 0.0f)
        {
            const auto fit = pedalBounds (box, id);
            const auto k = fit.getWidth() / pedalSize (id).x;
            const auto lift = board.selected == id ? 3.0f : 0.0f;
            const auto led = pedalLayout (id).led;
            const juce::Point<float> c { fit.getX() + led.getCentreX() * k, fit.getY() + (led.getCentreY() - lift) * k };
            const auto r = 34.0f * k;
            juce::ColourGradient bloom (accent.withAlpha (0.6f * left * left), c, accent.withAlpha (0.0f), c.translated (r, 0.0f), true);
            g.setGradientFill (bloom);
            g.fillEllipse (juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (c));
        }
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        hovered = true;
        repaint();
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = false;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        if (! e.mods.isPopupMenu())
            downX = e.getEventRelativeTo (&board).position.x;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || ! reorderable())
            return;
        const auto x = e.getEventRelativeTo (&board).position.x;
        if (! dragging && std::abs (x - downX) > 4.0f)
        {
            dragging = true;
            board.beginDrag (id, downX - (float) getX());
        }
        if (dragging)
            board.dragTo (x);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (dragging)
        {
            dragging = false;
            board.endDrag();
            return;
        }
        if (getLocalBounds().contains (e.getPosition()))
            click (footswitchArea().contains (e.position));
    }

    void click (bool footswitch)
    {
        if (footswitch)
            toggle();
        else if (board.onSelect)
            board.onSelect (id);
    }

    /// The footswitch: the block on or off, as one complete gesture in its own undo step.
    void toggle()
    {
        if (parameter == nullptr)
            return;
        const auto turnOn = ! isOn();
        beginUndoStep (ampSim.parameters);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (turnOn ? 1.0f : 0.0f);
        parameter->endChangeGesture();
        ampSim.switchedByUser (info (id).onParameter, turnOn);
        board.refresh();
    }

private:
    Board& board;
    AmpSimProcessor& ampSim;
    juce::RangedAudioParameter* parameter = nullptr;
    bool on = false, hovered = false, dragging = false;
    float downX = 0.0f;

public:
    bool sectionOff = false; // its section is off: drawn dimmed (RD4-1), its LED still telling its own switch

private:
    double glowStart = -1.0;
};

// ---- The amp's and the cab's markers -------------------------------------------------------------------

/// A tile on the Amp & Cab tab where the amp (its head in small and its name) or the cab (its front) sits in the
/// chain, with its switch's dot where a pedal's footswitch would be. Drawn at the tab's scale: every size below is the
/// first board's (0.665 of the pedals' size, a 70 px tile) times `unit()`.
class Board::Marker final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Marker (Board& b, AmpSimProcessor& p, BlockId blockId) : id (blockId), board (b)
    {
        dot = std::make_unique<PowerSwitch> (p.parameters, id == BlockId::amp ? "amp_bypass" : "cab_bypass", true);
        dot->setTooltip (id == BlockId::amp ? "The amp on or off" : "The cab on or off");
        dot->onChange = [this] { repaint(); };
        addAndMakeVisible (*dot);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip (id == BlockId::amp ? "The amp: click to dial it in the inspector (pick another on the stage's shelf)"
                                       : "The cab: click for its mics and cuts in the inspector, and its library and speaker on the stage");
    }

    const BlockId id;
    juce::String ampName;

    /// The scale the first board drew its markers at; unit() is the Amp & Cab tab's scale over it.
    static constexpr float referenceScale = 0.665f;
    float unit() const { return board.getPedalScale (BoardTab::ampCab) / referenceScale; }

    void set (const juce::String& name)
    {
        if (name != ampName)
        {
            ampName = name;
            repaint();
        }
    }

    juce::Rectangle<float> tile() const
    {
        // The pedals' bodies' height: under the jacks, inside the boxes' margins.
        const auto s = board.getPedalScale (BoardTab::ampCab);
        return getLocalBounds().toFloat().reduced (4.0f * unit(), 0.0f).withTrimmedTop ((pedalMargin + 7.0f) * s).withTrimmedBottom (pedalMargin * s);
    }

    /// Where the jewel (the switch's dot) sits: centred low on the tile, where a pedal's footswitch would be.
    juce::Point<float> jewelCentre() const
    {
        const auto t = tile();
        return { t.getCentreX(), t.getBottom() - 16.0f * unit() - currentLift() };
    }

    /// How far it's raised: 3 points (at the tab's scale) while selected, as a pedal.
    float currentLift() const { return board.selected == id ? 3.0f * board.getPedalScale (BoardTab::ampCab) : 0.0f; }

    void paint (juce::Graphics& g) override
    {
        // A piece of gear on the board like the pedals: raised and ringed in orange when selected, a soft shadow under
        // it, a dark steel plate with four screws, and on it the amp's own head in small (its material) or the cab's
        // front (tolex and grille), the name silkscreened under it, and the switch as a pilot jewel in a bezel.
        const auto u = unit();
        const auto isSelected = board.selected == id;
        const auto lift = currentLift();
        const auto t = tile().translated (0.0f, -lift);
        const auto on = dot->isOn();
        const auto r = 7.0f * u;

        {
            juce::Path body;
            body.addRoundedRectangle (t, r);
            juce::DropShadow (juce::Colours::black.withAlpha (isSelected ? 0.7f : 0.55f), juce::roundToInt (8.0f * u + lift * 2.0f), { 0, juce::roundToInt (3.0f * u + lift) })
                .drawForPath (g, body);
        }
        if (isSelected)
        {
            g.setColour (accent.withAlpha (0.2f));
            g.fillRoundedRectangle (t.expanded (2.0f * u).translated (0.0f, 2.0f * u), r + 2.0f * u);
            g.setColour (accent);
            g.drawRoundedRectangle (t.expanded (3.0f * u), r + 3.0f * u, 1.5f * juce::jmin (u, 1.4f));
        }

        // The plate: brushed steel, darker toward its foot, a highlight along its top edge.
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff34322f), t.getX(), t.getY(), juce::Colour (0xff181716), t.getX(), t.getBottom(), false));
        g.fillRoundedRectangle (t, r);
        g.setColour (juce::Colours::white.withAlpha (0.03f));
        for (auto y = t.getY() + 3.0f; y < t.getBottom() - 3.0f; y += 2.0f)
            g.drawHorizontalLine (juce::roundToInt (y), t.getX() + 3.0f, t.getRight() - 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine (juce::roundToInt (t.getY() + 1.0f), t.getX() + r, t.getRight() - r);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (t.reduced (0.5f), r, 1.0f);
        for (auto [sx, sy] : { std::pair<float, float> { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } })
        {
            const auto inset = 6.0f * u;
            const juce::Point<float> c { sx < 0.5f ? t.getX() + inset : t.getRight() - inset, sy < 0.5f ? t.getY() + inset : t.getBottom() - inset };
            g.setColour (juce::Colour (0xff0e0d0d));
            g.fillEllipse (juce::Rectangle<float> (4.5f * u, 4.5f * u).withCentre (c));
            g.setColour (juce::Colour (0xff7d7871));
            g.fillEllipse (juce::Rectangle<float> (2.5f * u, 2.5f * u).withCentre (c.translated (-0.3f * u, -0.3f * u)));
        }

        // The gear on the plate.
        auto face = t.reduced (8.0f * u, 0.0f).withTrimmedTop (12.0f * u);
        {
            const juce::Graphics::ScopedSaveState saved (g);
            if (! on)
                g.beginTransparencyLayer (0.55f);
            if (id == BlockId::amp)
            {
                // As wide as the face, or as tall as leaves room for the name and the jewel under it (a wide tile).
                const auto h = juce::jmin (face.getWidth() * 0.78f, t.getHeight() - 66.0f * u);
                const auto head = juce::Rectangle<float> (h / 0.78f, h).withPosition (face.getCentreX() - h / 0.78f * 0.5f, face.getY() + 4.0f * u);
                drawSmallHead (g, head, ampName);
                face.removeFromTop (head.getHeight() + 10.0f * u);
            }
            else
            {
                const auto front = face.withHeight (juce::jmin (face.getWidth() * 1.25f, t.getHeight() - 58.0f * u));
                drawCabFront (g, front, true);
                face.removeFromTop (front.getHeight() + 6.0f * u);
            }
            if (! on)
                g.endTransparencyLayer();
        }

        // The name, silkscreened; the bezel round the jewel.
        g.setFont (juce::Font (geist (Weight::semibold, 10.0f * u)).withExtraKerningFactor (0.06f));
        g.setColour (on ? ink : inkFaint);
        g.drawFittedText ((id == BlockId::amp ? ampName : juce::String ("Cab")).toUpperCase(), face.removeFromTop (14.0f * u).toNearestInt(), juce::Justification::centred, 1, 0.7f);
        const auto jc = jewelCentre();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8f8a82), jc.x, jc.y - 8.0f * u, juce::Colour (0xff2a2826), jc.x, jc.y + 8.0f * u, false));
        g.fillEllipse (juce::Rectangle<float> (15.0f * u, 15.0f * u).withCentre (jc));
        g.setColour (juce::Colour (0xff0b0b0b));
        g.fillEllipse (juce::Rectangle<float> (11.0f * u, 11.0f * u).withCentre (jc));
        if (on)
        {
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.45f), jc, accent.withAlpha (0.0f), jc.translated (14.0f * u, 0.0f), true));
            g.fillEllipse (juce::Rectangle<float> (28.0f * u, 28.0f * u).withCentre (jc));
        }
    }

    void resized() override
    {
        // The dot is the PowerSwitch (its 7 px lens), scaled with the tile about the jewel's centre.
        const auto c = jewelCentre();
        dot->setTransform ({});
        dot->setBounds (juce::Rectangle<int> (17, 17).withCentre ({ juce::roundToInt (c.x), juce::roundToInt (c.y) }));
        const auto u = unit();
        dot->setTransform (juce::AffineTransform::scale (u, u, (float) dot->getBounds().getCentreX(), (float) dot->getBounds().getCentreY()));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && board.onSelect)
            board.onSelect (id);
    }

    PowerSwitch& getDot() noexcept { return *dot; }

private:
    Board& board;
    std::unique_ptr<PowerSwitch> dot;
};

// ---- The tabs -------------------------------------------------------------------------------------------

/// The strip of tabs along the board's top: the stage's pill (DESIGN.md, "Pill": a raised hairline capsule, 30 high,
/// 13 px medium, the chosen segment lighter with its name in orange), each segment its name and a row of tiny LEDs,
/// one per block in that tab. A lit LED is the accent with a small halo, an off one the dead lens; a section that's
/// off draws its LEDs at 45% (each still telling its own switch). A click anywhere on a tab opens it.
class Board::Tabs final : public juce::Component, public juce::TooltipClient
{
public:
    explicit Tabs (Board& b) : board (b)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle ("Board tabs");
    }

    static juce::FontOptions font() { return geist (Weight::medium, 13.0f); }
    static constexpr float inset = 3.0f, pad = 16.0f, nameGap = 10.0f, ledSize = 7.0f, ledPitch = 12.0f;

    float tabWidth (BoardTab t) const
    {
        const auto n = (float) board.getTabBlocks (t).size();
        return pad + textWidth (font(), boardTabLabel (t)) + nameGap + n * ledPitch - (ledPitch - ledSize) + pad;
    }

    int preferredWidth() const
    {
        auto w = 2.0f * inset;
        for (int t = 0; t < numBoardTabs; ++t)
            w += tabWidth ((BoardTab) t);
        return (int) std::ceil (w);
    }

    juce::Rectangle<float> area (BoardTab which) const
    {
        auto x = inset;
        for (int t = 0; t < numBoardTabs; ++t)
        {
            const auto w = tabWidth ((BoardTab) t);
            if ((BoardTab) t == which)
                return { x, inset, w, (float) getHeight() - 2.0f * inset };
            x += w;
        }
        return {};
    }

    /// The centre of a tab's LED `index` (its order in the tab).
    juce::Point<float> ledCentre (BoardTab t, int index) const
    {
        const auto r = area (t);
        return { r.getX() + pad + textWidth (font(), boardTabLabel (t)) + nameGap + ledSize * 0.5f + (float) index * ledPitch, r.getCentreY() };
    }

    BoardTab tabAt (juce::Point<float> p) const
    {
        for (int t = 0; t < numBoardTabs; ++t)
            if (area ((BoardTab) t).expanded (0.0f, inset).contains (p))
                return (BoardTab) t;
        return BoardTab::count;
    }

    void paint (juce::Graphics& g) override
    {
        const auto box = getLocalBounds().toFloat().reduced (0.5f);
        const auto radius = box.getHeight() * 0.5f;
        g.setColour (raised);
        g.fillRoundedRectangle (box, radius);
        g.setColour (line1);
        g.drawRoundedRectangle (box, radius, 1.0f);

        g.setFont (font());
        for (int i = 0; i < numBoardTabs; ++i)
        {
            const auto t = (BoardTab) i;
            const auto r = area (t);
            const auto open = t == board.getTab();
            if (open)
            {
                g.setColour (line1.brighter (0.15f));
                g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            }
            else if (t == hovered)
            {
                g.setColour (line1.withAlpha (0.5f));
                g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            }
            const auto label = boardTabLabel (t);
            const auto nameWidth = textWidth (font(), label);
            g.setColour (open ? accent : (t == hovered ? ink : inkDim));
            g.drawText (label, juce::Rectangle<float> (r.getX() + pad, r.getY(), nameWidth + 2.0f, r.getHeight()), juce::Justification::centredLeft, false);

            // The LEDs: one per block, in the order shown.
            auto index = 0;
            for (const auto& led : board.getTabLeds (t))
            {
                const auto c = ledCentre (t, index++);
                const auto fade = led.dimmed ? 0.45f : 1.0f;
                if (led.lit)
                {
                    juce::ColourGradient halo (accent.withAlpha (0.42f * fade), c, accent.withAlpha (0.0f), c.translated (ledSize, 0.0f), true);
                    g.setGradientFill (halo);
                    g.fillEllipse (juce::Rectangle<float> (2.0f * ledSize, 2.0f * ledSize).withCentre (c));
                    g.setColour (accent.withMultipliedAlpha (fade));
                    g.fillEllipse (juce::Rectangle<float> (ledSize, ledSize).withCentre (c));
                    g.setColour (juce::Colour (0xfffff1dc).withAlpha (0.85f * fade)); // the white-hot middle, as the pedals'
                    g.fillEllipse (juce::Rectangle<float> (2.4f, 2.4f).withCentre (c));
                }
                else
                {
                    // The dead lens (RD4-2): near-black, ringed so it reads on the raised capsule.
                    g.setColour (juce::Colours::black.withAlpha (0.55f * fade));
                    g.fillEllipse (juce::Rectangle<float> (ledSize + 2.0f, ledSize + 2.0f).withCentre (c));
                    g.setColour (ledOffColour.withMultipliedAlpha (fade));
                    g.fillEllipse (juce::Rectangle<float> (ledSize, ledSize).withCentre (c));
                    g.setColour (line2.brighter (0.25f).withAlpha (0.9f * fade));
                    g.drawEllipse (juce::Rectangle<float> (ledSize, ledSize).withCentre (c).reduced (0.5f), 1.0f);
                }
            }
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (tabAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (BoardTab::count); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (const auto t = tabAt (e.position); t != BoardTab::count)
            board.setTab (t);
    }

    juce::String getTooltip() override
    {
        if (hovered == BoardTab::count)
            return {};
        // What's on in it, by name: the strip's LEDs in words.
        juce::StringArray on;
        auto dimmed = false;
        for (const auto& led : board.getTabLeds (hovered))
        {
            if (led.lit)
                on.add (led.id == BlockId::amp ? juce::String ("the amp") : led.id == BlockId::cab ? juce::String ("the cab") : juce::String (info (led.id).name));
            dimmed = dimmed || led.dimmed;
        }
        auto tip = juce::String (boardTabLabel (hovered)) + ": ";
        if (on.isEmpty())
            tip << "nothing on";
        else
            tip << on.joinIntoString (", ") << " on";
        if (dimmed)
            tip << " (the section is off)";
        return tip + ". Click to show it; [ and ] step the tabs";
    }

private:
    void setHovered (BoardTab t)
    {
        if (t != hovered)
        {
            hovered = t;
            repaint();
        }
    }

    Board& board;
    BoardTab hovered = BoardTab::count;
};

// ---- The tabs' names ------------------------------------------------------------------------------------

const char* boardTabName (BoardTab tab)
{
    switch (tab)
    {
        case BoardTab::ampCab: return "ampcab";
        case BoardTab::post:   return "post";
        case BoardTab::pre:
        case BoardTab::count:  break;
    }
    return "pre";
}

const char* boardTabLabel (BoardTab tab)
{
    switch (tab)
    {
        case BoardTab::ampCab: return "Amp & Cab";
        case BoardTab::post:   return "Post FX";
        case BoardTab::pre:
        case BoardTab::count:  break;
    }
    return "Pre FX";
}

BoardTab boardTabFor (BlockId id)
{
    switch (info (id).section)
    {
        case Section::pre:   return BoardTab::pre;
        case Section::amp:
        case Section::cab:   return BoardTab::ampCab;
        case Section::post:  return BoardTab::post;
        case Section::inOut: break;
    }
    return BoardTab::count;
}

BoardTab boardTabNamed (const juce::String& name)
{
    for (int t = 0; t < numBoardTabs; ++t)
        if (name == boardTabName ((BoardTab) t))
            return (BoardTab) t;
    return BoardTab::count;
}

// ---- Board ----------------------------------------------------------------------------------------------

Board::Board (AmpSimProcessor& p) : ampSim (p)
{
    for (int b = 0; b < numBlocks; ++b)
        if (isPedal ((BlockId) b))
        {
            pedals.push_back (std::make_unique<Pedal> (*this, p, (BlockId) b));
            addAndMakeVisible (*pedals.back());
        }
    ampMarker = std::make_unique<Marker> (*this, p, BlockId::amp);
    cabMarker = std::make_unique<Marker> (*this, p, BlockId::cab);
    addAndMakeVisible (*ampMarker);
    addAndMakeVisible (*cabMarker);
    preSwitch = std::make_unique<PowerSwitch> (p.parameters, "pre_fx_on", false);
    postSwitch = std::make_unique<PowerSwitch> (p.parameters, "post_fx_on", false);
    preSwitch->setTooltip ("The whole pre section on or off (each pedal keeps its own switch)");
    postSwitch->setTooltip ("The whole post section on or off (each pedal keeps its own switch)");
    for (auto* s : { preSwitch.get(), postSwitch.get() })
    {
        s->setLabelled (true);
        s->onChange = [this] { refresh(); repaint(); };
        addAndMakeVisible (*s);
    }
    tabs = std::make_unique<Tabs> (*this);
    addAndMakeVisible (*tabs);
    refresh();
    showTab();
}

Board::~Board()
{
    stopTimer();
}

Board::Pedal* Board::pedalFor (BlockId id) const
{
    for (const auto& pedal : pedals)
        if (pedal->id == id)
            return pedal.get();
    return nullptr;
}

juce::Component* Board::getPedal (BlockId id) const { return pedalFor (id); }

juce::Component* Board::getMarker (BlockId id) const
{
    return id == BlockId::amp ? ampMarker.get() : id == BlockId::cab ? cabMarker.get() : nullptr;
}

PowerSwitch* Board::getSectionSwitch (Section section) const
{
    return section == Section::pre ? preSwitch.get() : section == Section::post ? postSwitch.get() : nullptr;
}

juce::Component& Board::getTabStrip() noexcept { return *tabs; }

juce::Rectangle<float> Board::getTabArea (BoardTab t) const { return tabs->area (t); }

juce::Point<float> Board::getTabLedCentre (BoardTab t, int index) const { return tabs->ledCentre (t, index); }

bool Board::isGlowing (BlockId id) const
{
    auto* pedal = pedalFor (id);
    return pedal != nullptr && pedal->glowLeft() > 0.0f;
}

juce::Rectangle<float> Board::getFootswitchArea (BlockId id) const
{
    if (auto* pedal = pedalFor (id))
        return pedal->footswitchArea();
    return {};
}

bool Board::isLit (BlockId id) const
{
    auto* pedal = pedalFor (id);
    return pedal != nullptr && pedal->isOn();
}

bool Board::isDimmed (BlockId id) const
{
    auto* pedal = pedalFor (id);
    return pedal != nullptr && pedalDimmed ({ pedal->isOn(), false, false, pedal->sectionOff });
}

bool Board::isShown (BlockId id) const
{
    const auto* c = pedalFor (id) != nullptr ? (juce::Component*) pedalFor (id) : getMarker (id);
    return c != nullptr && c->isVisible();
}

void Board::clickPedal (BlockId id, bool onFootswitch)
{
    if (auto* pedal = pedalFor (id))
        pedal->click (onFootswitch);
}

void Board::dragPedal (BlockId id, float dx)
{
    auto* pedal = pedalFor (id);
    if (pedal == nullptr || ! pedal->reorderable())
        return;
    const auto start = (float) pedal->getBounds().getCentreX();
    beginDrag (id, start - (float) pedal->getX());
    for (int step = 1; step <= 10; ++step)
        dragTo (start + dx * (float) step / 10.0f);
    endDrag();
}

std::vector<BlockId> Board::getShownOrder (Section section) const
{
    return order[section == Section::post ? 1 : 0];
}

std::vector<BlockId> Board::getTabBlocks (BoardTab t) const
{
    if (t == BoardTab::ampCab)
        return { BlockId::amp, BlockId::gateB, BlockId::cab };
    return order[t == BoardTab::post ? 1 : 0];
}

bool Board::blockOn (BlockId id) const
{
    // The markers' switches are inverted (amp_bypass, cab_bypass): on while the parameter is off.
    if (id == BlockId::amp || id == BlockId::cab)
    {
        const auto* p = ampSim.parameters.getParameter (id == BlockId::amp ? "amp_bypass" : "cab_bypass");
        return p != nullptr && p->getValue() < 0.5f;
    }
    return isLit (id);
}

std::vector<Board::TabLed> Board::getTabLeds (BoardTab t) const
{
    std::vector<TabLed> leds;
    for (auto id : getTabBlocks (t))
    {
        // Gate B (Section::amp) and the markers have no section switch over them: the chain bypasses Gate B by its own
        // switch alone (AmpSimProcessor::applyEffectParameters), so its LED is never dimmed.
        const auto section = info (id).section;
        const auto underSwitch = section == Section::pre || section == Section::post;
        leds.push_back ({ id, blockOn (id), underSwitch && ! sectionOn (section) });
    }
    return leds;
}

void Board::setSelected (BlockId id)
{
    if (id == selected)
        return;
    const auto old = selected;
    selected = id;
    for (auto b : { old, id })
    {
        if (auto* pedal = pedalFor (b))
            pedal->repaint();
        if (auto* marker = getMarker (b))
        {
            marker->resized(); // its jewel rises with it
            marker->repaint();
        }
    }
    // The selection opens its tab, wherever it came from (the stage, a key, a test, a restored state).
    if (const auto t = boardTabFor (id); t != BoardTab::count)
        setTab (t);
}

void Board::setTab (BoardTab t, bool notify)
{
    if (t == BoardTab::count || t == tab)
        return;
    if (dragged != BlockId::count)
        endDrag();
    tab = t;
    showTab();
    if (notify && onTabChange)
        onTabChange (tab);
}

void Board::stepTab (int direction)
{
    setTab ((BoardTab) (((int) tab + (direction < 0 ? numBoardTabs - 1 : 1)) % numBoardTabs));
}

void Board::showTab()
{
    // Only the open tab's gear shows; the rest keep their places (laid out for their own tabs) while hidden.
    for (auto& pedal : pedals)
        pedal->setVisible (boardTabFor (pedal->id) == tab);
    ampMarker->setVisible (tab == BoardTab::ampCab);
    cabMarker->setVisible (tab == BoardTab::ampCab);
    preSwitch->setVisible (tab == BoardTab::pre);
    postSwitch->setVisible (tab == BoardTab::post);
    tabs->repaint();
    repaint();
}

bool Board::sectionOn (Section section) const
{
    const auto* s = getSectionSwitch (section);
    return s == nullptr || s->isOn();
}

void Board::refresh()
{
    // The sections' saved orders (a preset, undo, another view may have changed them).
    auto relayout = false;
    for (int r = 0; r < 2; ++r)
    {
        if (dragged != BlockId::count && info (dragged).section == sectionOfRow (r))
            continue; // not under a drag
        std::vector<BlockId> shown;
        for (const auto& name : ampSim.getSectionOrder (chainSection (r)))
            if (const auto id = blockFor (sectionOfRow (r), name); id != BlockId::count)
                shown.push_back (id);
        if (shown != order[(size_t) r])
        {
            order[(size_t) r] = shown;
            relayout = true;
        }
    }
    if (relayout)
        layoutRows();

    // The LEDs, live; a section that's off dims its pedals.
    auto glowing = false;
    for (auto& pedal : pedals)
    {
        pedal->poll();
        glowing = glowing || pedal->glowLeft() > 0.0f;
        // A section that's off dims its pedals as a pedal that's off is dimmed (RD4-1): one treatment for "not sounding".
        const auto section = info (pedal->id).section;
        const auto off = section != Section::amp && ! sectionOn (section);
        if (off != pedal->sectionOff)
        {
            pedal->sectionOff = off;
            pedal->repaint();
        }
    }
    if (glowing && ! isTimerRunning())
        startTimerHz (animationFps);
    ampMarker->set (AmpSimProcessor::ampName (ampSim.getSelectedAmp()));

    // The strip's LEDs: repainted when any of them, or the order they're in, changes.
    juce::uint32 signature = 2166136261u; // FNV-1a over every LED's block, light, and dimming
    for (int t = 0; t < numBoardTabs; ++t)
        for (const auto& led : getTabLeds ((BoardTab) t))
        {
            signature = (signature ^ (juce::uint32) ((int) led.id * 4 + (led.lit ? 1 : 0) + (led.dimmed ? 2 : 0))) * 16777619u;
        }
    if (signature != ledSignature)
    {
        ledSignature = signature;
        tabs->repaint();
    }
}

void Board::timerCallback()
{
    auto glowing = false;
    for (auto& pedal : pedals)
        if (pedal->glowLeft() > 0.0f)
        {
            glowing = true;
            pedal->repaint();
        }
    if (! glowing)
    {
        stopTimer();
        for (auto& pedal : pedals)
            pedal->repaint(); // the last frame, with the bloom gone
    }
}

juce::Rectangle<int> Board::rowFor (BoardTab t) const
{
    // The pedals' boxes stand in the row at their tab's scale, centred in its height.
    const auto height = juce::roundToInt (pedalRows * tabScale[(size_t) t]);
    return content.withSizeKeepingCentre (content.getWidth(), height);
}

void Board::paint (juce::Graphics& g)
{
    // The tray and a hairline along its top.
    g.fillAll (boardTray);
    g.setColour (line1);
    g.fillRect (0, 0, getWidth(), 1);

    // The open section's switch, named, at the strip's right.
    if (const auto* s = tab == BoardTab::pre ? preSwitch.get() : tab == BoardTab::post ? postSwitch.get() : nullptr)
    {
        const auto font = geist (Weight::regular, 12.0f);
        const auto label = juce::String (tab == BoardTab::pre ? "Pre FX section" : "Post FX section");
        g.setFont (font);
        g.setColour (s->isOn() ? inkDim : inkFaint);
        const auto w = textWidth (font, label) + 2.0f;
        g.drawText (label, juce::Rectangle<float> ((float) s->getX() - 8.0f - w, (float) s->getY(), w, (float) s->getHeight()), juce::Justification::centredRight, false);
    }

    // The signal's cable along the row, behind the gear, at the height of the pedals' jacks' line.
    const auto row = rowFor (tab);
    const auto y = (float) row.getY() + (float) row.getHeight() * 0.62f;
    g.setColour (line2);
    if (tab == BoardTab::ampCab)
    {
        // Through the whole row: in from the pre section at the left, out to the post section at the right, each end
        // named.
        g.fillRect (juce::Rectangle<float> ((float) content.getX(), y, (float) content.getWidth(), 2.0f));
        const auto font = geist (Weight::regular, 12.0f);
        g.setFont (font);
        g.setColour (inkFaint);
        const auto band = juce::Rectangle<float> ((float) content.getX(), y - 22.0f, (float) content.getWidth(), 16.0f);
        g.drawText ("from Pre FX", band, juce::Justification::centredLeft, false);
        g.drawText ("to Post FX", band, juce::Justification::centredRight, false);
        return;
    }
    auto right = content.getX();
    for (auto id : getTabBlocks (tab))
        if (auto* pedal = pedalFor (id))
            right = juce::jmax (right, pedal->getRight());
    // Where the row leaves room after its last pedal (the pre section's, mostly), the cable runs on to the row's end,
    // named for where it goes, as the Amp & Cab tab's ends are.
    const auto font = geist (Weight::regular, 12.0f);
    const auto onward = juce::String (tab == BoardTab::pre ? "to Amp & Cab" : "to the output");
    if (content.getRight() - right > (int) textWidth (font, onward) + 40)
    {
        right = content.getRight();
        g.setFont (font);
        g.setColour (inkFaint);
        g.drawText (onward, juce::Rectangle<float> ((float) content.getX(), y - 22.0f, (float) content.getWidth(), 16.0f), juce::Justification::centredRight, false);
        g.setColour (line2);
    }
    g.fillRect (juce::Rectangle<float> ((float) content.getX() - 6.0f, y, (float) (right - content.getX()) + 6.0f, 2.0f));
}

void Board::resized()
{
    // The strip at the top: the tabs from the left, the open section's switch at the right.
    const auto strip = juce::Rectangle<int> (sidePad, stripTop, getWidth() - 2 * sidePad, stripHeight);
    tabs->setBounds (strip.withWidth (tabs->preferredWidth()));
    for (auto* s : { preSwitch.get(), postSwitch.get() })
        s->setBounds (strip.getRight() - PowerSwitch::labelledWidth + 4, strip.getCentreY() - PowerSwitch::labelledHeight / 2, PowerSwitch::labelledWidth,
                      PowerSwitch::labelledHeight);

    // The row under it, and each tab's scale: the row's height (every pedal is 172 points tall plus its margins), or
    // its width where the tab's gear is wider than the row.
    content = { sidePad, stripTop + stripHeight + stripGap, getWidth() - 2 * sidePad, getHeight() - (stripTop + stripHeight + stripGap) - bottomPad };
    const auto heightFit = (float) content.getHeight() / pedalRows;
    for (int t = 0; t < numBoardTabs; ++t)
    {
        float points = 0.0f;
        int room = 0; // screen points between the boxes
        if ((BoardTab) t == BoardTab::ampCab)
        {
            points = 2.0f * markerPoints + pedalSize (BlockId::gateB).x + 2.0f * pedalMargin;
            room = 2 * ampCabGap + 2 * 90; // and room at the ends for the cable's names
        }
        else
            for (int b = 0; b < numBlocks; ++b)
                if (isPedal ((BlockId) b) && boardTabFor ((BlockId) b) == (BoardTab) t)
                {
                    points += pedalSize ((BlockId) b).x + 2.0f * pedalMargin;
                    room += itemGap;
                }
        const auto widthFit = (float) (content.getWidth() - room) / juce::jmax (1.0f, points);
        tabScale[(size_t) t] = juce::jmin (heightFit, widthFit);
    }
    layoutRows();
}

void Board::layoutRows()
{
    for (int r = 0; r < 2; ++r)
    {
        const auto t = r == 0 ? BoardTab::pre : BoardTab::post;
        const auto s = tabScale[(size_t) t];
        const auto widthOf = [s] (BlockId id) { return juce::roundToInt ((pedalSize (id).x + 2.0f * pedalMargin) * s); };
        const auto row = rowFor (t);
        const auto& items = order[(size_t) r];
        const auto draggedHere = dragged != BlockId::count && info (dragged).section == sectionOfRow (r);
        std::vector<BlockId> others;
        for (auto id : items)
            if (id != dragged)
                others.push_back (id);

        // Left to right; while a pedal is dragged, the others leave a gap where it would land and it follows the
        // pointer.
        auto x = row.getX();
        for (size_t k = 0; k <= others.size(); ++k)
        {
            if (draggedHere && (int) k == dragInsert)
                x += widthOf (dragged) + itemGap;
            if (k == others.size())
                break;
            if (auto* pedal = pedalFor (others[k]))
                pedal->setBounds (x, row.getY(), widthOf (others[k]), row.getHeight());
            x += widthOf (others[k]) + itemGap;
        }
        if (draggedHere)
            if (auto* pedal = pedalFor (dragged))
            {
                const auto w = widthOf (dragged);
                pedal->setBounds (juce::jlimit (row.getX(), juce::jmax (row.getX(), row.getRight() - w), juce::roundToInt (dragX - dragGrab)), row.getY(), w,
                                  row.getHeight());
                pedal->toFront (false);
            }
    }

    // The Amp & Cab tab: the amp, Gate B, and the cab, centred in the row with room between them for the cable.
    {
        const auto s = tabScale[(size_t) BoardTab::ampCab];
        const auto row = rowFor (BoardTab::ampCab);
        const auto marker = juce::roundToInt (markerPoints * s);
        const auto gate = juce::roundToInt ((pedalSize (BlockId::gateB).x + 2.0f * pedalMargin) * s);
        auto x = row.getCentreX() - (2 * marker + gate + 2 * ampCabGap) / 2;
        ampMarker->setBounds (x, row.getY(), marker, row.getHeight());
        x += marker + ampCabGap;
        if (auto* g = pedalFor (BlockId::gateB))
            g->setBounds (x, row.getY(), gate, row.getHeight());
        x += gate + ampCabGap;
        cabMarker->setBounds (x, row.getY(), marker, row.getHeight());
    }
    repaint();
}

void Board::beginDrag (BlockId id, float grabOffset)
{
    dragged = id;
    dragGrab = grabOffset;
    if (auto* pedal = pedalFor (id))
        dragStartCentre = (float) pedal->getBounds().getCentreX();
    const auto& items = order[info (id).section == Section::post ? 1 : 0];
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i] == id)
            dragInsert = (int) i;
}

void Board::dragTo (float x)
{
    if (dragged == BlockId::count)
        return;
    dragX = x;
    const auto r = info (dragged).section == Section::post ? 1 : 0;
    const auto s = tabScale[(size_t) (r == 0 ? BoardTab::pre : BoardTab::post)];
    const auto widthOf = [s] (BlockId id) { return (pedalSize (id).x + 2.0f * pedalMargin) * s; };

    // Where it lands: after every other pedal it has passed. A pedal counts as passed once the dragged one's centre is a
    // quarter of its width beyond its centre, the way the drag is going: dropped on a neighbour's middle, it takes that
    // neighbour's place. The centres are where the pedals stood when the drag began (the dragged one in its own place),
    // so a drag to the right and one to the left work alike.
    const auto centre = x - dragGrab + widthOf (dragged) * 0.5f;
    const auto direction = centre < dragStartCentre ? -1.0f : 1.0f;
    int insert = 0;
    auto position = (float) content.getX();
    for (auto id : order[(size_t) r])
    {
        const auto w = widthOf (id);
        if (id != dragged && position + w * 0.5f - direction * w * 0.25f < centre)
            ++insert;
        position += w + (float) itemGap;
    }
    dragInsert = insert;
    layoutRows();
}

void Board::endDrag()
{
    if (dragged == BlockId::count)
        return;
    const auto r = info (dragged).section == Section::post ? 1 : 0;
    std::vector<BlockId> moved;
    for (auto id : order[(size_t) r])
        if (id != dragged)
            moved.push_back (id);
    moved.insert (moved.begin() + juce::jlimit (0, (int) moved.size(), dragInsert), dragged);
    dragged = BlockId::count;
    if (moved != order[(size_t) r])
    {
        juce::StringArray names;
        for (auto id : moved)
            names.add (info (id).orderName);
        beginUndoStep (ampSim.parameters);
        ampSim.setSectionOrder (chainSection (r), names);
        order[(size_t) r] = moved;
    }
    layoutRows();
}

} // namespace ui

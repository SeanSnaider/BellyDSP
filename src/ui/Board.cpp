// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Board.h"
#include "AmpHead.h"

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
        drawPedal (g, box, id, { on, board.selected == id, hovered && board.dragged != id });

        // An LED that has just come on blooms for a moment (the art's own glow is the steady state).
        if (const auto left = glowLeft(); left > 0.0f)
        {
            const auto fit = pedalBounds (box, id);
            const auto k = fit.getWidth() / pedalSize (id).x;
            const auto lift = board.selected == id ? 3.0f : 0.0f;
            const auto led = pedalLayout (id).led;
            const juce::Point<float> c { fit.getX() + led.getCentreX() * k, fit.getY() + (led.getCentreY() - lift) * k };
            const auto r = 22.0f * k;
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
    double glowStart = -1.0;
};

// ---- The amp's and the cab's markers -------------------------------------------------------------------

/// A slim tile between the pedals where the amp (its mini head and name) or the cab (a speaker) sits in the chain,
/// with its switch's dot where a pedal's footswitch would be.
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
        return getLocalBounds().toFloat().reduced (4.0f, 0.0f).withTrimmedTop ((pedalMargin + 7.0f) * board.scale).withTrimmedBottom (pedalMargin * board.scale);
    }

    void paint (juce::Graphics& g) override
    {
        const auto isSelected = board.selected == id;
        auto t = tile().translated (0.0f, isSelected ? -3.0f * board.scale : 0.0f);
        g.setColour (raised.withAlpha (isSelected ? 1.0f : 0.85f));
        g.fillRoundedRectangle (t, 8.0f);
        g.setColour (isSelected ? accent : line1);
        g.drawRoundedRectangle (t.reduced (0.5f), 8.0f, isSelected ? 1.5f : 1.0f);

        auto area = t.reduced (6.0f, 10.0f);
        g.setFont (geist (Weight::semibold, 10.0f));
        g.setColour (inkFaint);
        g.drawText (id == BlockId::amp ? "AMP" : "CAB", area.removeFromTop (14.0f), juce::Justification::centred, false);
        area.removeFromTop (8.0f);
        const auto icon = area.removeFromTop (30.0f);
        const auto on = dot->isOn();
        {
            const juce::Graphics::ScopedSaveState saved (g);
            if (! on)
                g.beginTransparencyLayer (offAlpha);
            if (id == BlockId::amp)
                drawMiniAmp (g, juce::Rectangle<float> (45.0f, 30.0f).withCentre (icon.getCentre()), ampName);
            else
            {
                // A speaker: its rim, cone, and dust cap.
                const auto c = icon.getCentre();
                g.setColour (inkDim);
                g.drawEllipse (juce::Rectangle<float> (28.0f, 28.0f).withCentre (c), 1.2f);
                g.setColour (line2);
                g.drawEllipse (juce::Rectangle<float> (18.0f, 18.0f).withCentre (c), 1.0f);
                g.setColour (inkDim);
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (c));
            }
            if (! on)
                g.endTransparencyLayer();
        }
        area.removeFromTop (6.0f);
        g.setFont (geist (Weight::medium, 11.0f));
        g.setColour (on ? ink : inkFaint);
        g.drawFittedText (id == BlockId::amp ? ampName : juce::String ("Cab"), area.removeFromTop (16.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    void resized() override
    {
        const auto t = tile();
        dot->setBounds (juce::Rectangle<int> (17, 17).withCentre ({ juce::roundToInt (t.getCentreX()), juce::roundToInt (t.getBottom() - 18.0f) }));
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
        s->onChange = [this] { refresh(); repaint(); };
        addAndMakeVisible (*s);
    }
    refresh();
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
            marker->repaint();
    }
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

    // The LEDs, live; a section that's off dims its row.
    auto glowing = false;
    for (auto& pedal : pedals)
    {
        pedal->poll();
        glowing = glowing || pedal->glowLeft() > 0.0f;
        const auto section = info (pedal->id).section;
        pedal->setAlpha (section == Section::amp || sectionOn (section) ? 1.0f : 0.45f);
    }
    if (glowing && ! isTimerRunning())
        startTimerHz (animationFps);
    ampMarker->set (AmpSimProcessor::ampName (ampSim.getSelectedAmp()));
}

void Board::timerCallback()
{
    auto glowing = false;
    for (auto& pedal : pedals)
        if (pedal->glowLeft() > 0.0f || pedal->isOn())
        {
            if (pedal->glowLeft() > 0.0f)
            {
                glowing = true;
                pedal->repaint();
            }
        }
    if (! glowing)
    {
        stopTimer();
        for (auto& pedal : pedals)
            pedal->repaint(); // the last frame, with the bloom gone
    }
}

void Board::paint (juce::Graphics& g)
{
    // The tray, a hairline along its top, and each row's name with its section's switch beside it.
    g.fillAll (boardTray);
    g.setColour (line1);
    g.fillRect (0, 0, getWidth(), 1);

    for (int r = 0; r < 2; ++r)
    {
        const auto row = rows[(size_t) r];
        const auto label = juce::Rectangle<int> (sidePad, row.getY(), labelWidth - 12, row.getHeight());
        g.setFont (geist (Weight::semibold, 11.0f));
        g.setColour (sectionOn (sectionOfRow (r)) ? inkDim : inkFaint);
        g.drawText (r == 0 ? "PRE" : "POST", label.withHeight (16).withCentre ({ label.getCentreX(), row.getCentreY() - 12 }), juce::Justification::centredLeft,
                    false);

        // The signal's cable along the row, behind the pedals: from the row's start to its last item.
        auto right = row.getX();
        for (auto* c : getChildren())
            if (c->getY() >= row.getY() - 4 && c->getBottom() <= row.getBottom() + 4 && dynamic_cast<PowerSwitch*> (c) == nullptr)
                right = juce::jmax (right, c->getRight());
        const auto y = (float) row.getY() + (float) row.getHeight() * 0.62f;
        g.setColour (line2);
        g.fillRect (juce::Rectangle<float> ((float) row.getX() - 6.0f, y, (float) (right - row.getX()) + 6.0f, 2.0f));
    }
}

void Board::resized()
{
    // Two rows, each a pedal's box high: the scale that fits them (every pedal is 172 points tall plus its margins).
    const auto inner = getHeight() - 2 * rowPad - rowGap;
    const auto rowHeight = inner / 2;
    scale = (float) rowHeight / pedalRows;
    rows[0] = { sidePad + labelWidth, rowPad, getWidth() - 2 * sidePad - labelWidth, rowHeight };
    rows[1] = rows[0].withY (rowPad + rowHeight + rowGap);
    for (int r = 0; r < 2; ++r)
    {
        auto* s = r == 0 ? preSwitch.get() : postSwitch.get();
        s->setBounds (juce::Rectangle<int> (17, 17).withCentre ({ sidePad + 4, rows[(size_t) r].getCentreY() + 8 }));
    }
    layoutRows();
}

void Board::layoutRows()
{
    const auto widthOf = [this] (BlockId id) { return juce::roundToInt ((pedalSize (id).x + 2.0f * pedalMargin) * scale); };
    const auto height = juce::roundToInt (pedalRows * scale);

    for (int r = 0; r < 2; ++r)
    {
        const auto row = rows[(size_t) r];
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
                pedal->setBounds (x, row.getY(), widthOf (others[k]), height);
            x += widthOf (others[k]) + itemGap;
        }
        if (draggedHere)
            if (auto* pedal = pedalFor (dragged))
            {
                const auto w = widthOf (dragged);
                pedal->setBounds (juce::jlimit (row.getX(), juce::jmax (row.getX(), row.getRight() - w), juce::roundToInt (dragX - dragGrab)), row.getY(), w, height);
                pedal->toFront (false);
            }

        if (r == 0)
        {
            // After the pre section: the amp, Gate B, the cab (fixed).
            x += 6;
            ampMarker->setBounds (x, row.getY(), markerWidth, height);
            x += markerWidth + itemGap;
            if (auto* gate = pedalFor (BlockId::gateB))
                gate->setBounds (x, row.getY(), widthOf (BlockId::gateB), height);
            x += widthOf (BlockId::gateB) + itemGap;
            cabMarker->setBounds (x, row.getY(), markerWidth, height);
        }
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
    const auto widthOf = [this] (BlockId id) { return (pedalSize (id).x + 2.0f * pedalMargin) * scale; };

    // Where it lands: after every other pedal it has passed. A pedal counts as passed once the dragged one's centre is a
    // quarter of its width beyond its centre, the way the drag is going: dropped on a neighbour's middle, it takes that
    // neighbour's place.
    const auto centre = x - dragGrab + widthOf (dragged) * 0.5f;
    const auto direction = centre < dragStartCentre ? -1.0f : 1.0f;
    int insert = 0;
    auto position = (float) rows[(size_t) r].getX();
    for (auto id : order[(size_t) r])
    {
        if (id == dragged)
            continue;
        const auto w = widthOf (id);
        if (position + w * 0.5f - direction * w * 0.25f < centre)
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

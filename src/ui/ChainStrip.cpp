#include "ChainStrip.h"

namespace ui
{

using namespace theme;

namespace
{
constexpr int stripPad = 16; // the window's side gutter
constexpr int topPad = 10;
constexpr int boxPad = 8;    // inside a section box, around its cards
constexpr int rowGap = 14;   // between the two rows (the turn runs through it)
constexpr int unitGap = 12;  // between the ends, the section boxes, and the fixed cards
constexpr int boxHeight = chainBlockHeight + 2 * boxPad;
constexpr int fixedWidth = 132; // row one beyond its eight cards: the input's cap and every gap

ampsim::Chain::Section chainSection (Section section)
{
    return section == Section::pre ? ampsim::Chain::Section::pre : ampsim::Chain::Section::post;
}

juce::String signedDb (float db)
{
    return (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB";
}
} // namespace

// ---- Card ------------------------------------------------------------------------------------------

class ChainStrip::Card final : public juce::Component, public juce::SettableTooltipClient
{
public:
    Card (ChainStrip& strip, BlockId blockId) : id (blockId), owner (strip)
    {
        const auto& i = info (id);
        if (*i.onParameter != 0)
        {
            power = std::make_unique<PowerSwitch> (strip.ampSim.parameters, i.onParameter, i.switchInverted);
            power->setTooltip (juce::String (i.switchInverted ? "Put the cab in or take it out" : "Switch it on or off"));
            power->onChange = [this] { repaint(); };
            addAndMakeVisible (*power);
            tagged (*this, i.onParameter); // a right-click anywhere on the card: MIDI learn for its switch
        }
        else if (id == BlockId::amp)
        {
            tagged (*this, AmpSimProcessor::slotParamId);
        }

        const auto reorderable = i.section == Section::pre || i.section == Section::post;
        setTooltip (reorderable ? juce::String ("Click to edit; drag along the section to reorder") : juce::String ("Click to edit"));
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    const BlockId id;
    juce::Rectangle<int> target;
    bool lifted = false;

    bool isOn() const { return power == nullptr || power->isOn(); }

    void update (const juce::String& line, bool isSelected, int activeSlot)
    {
        if (line != stateLine || isSelected != selected || activeSlot != slotShown)
        {
            stateLine = line;
            selected = isSelected;
            slotShown = activeSlot;
            repaint();
        }
    }

    void resized() override
    {
        if (power != nullptr)
            power->setBounds (getWidth() - 36, 5, 30, 20);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        // The amp's three slot squares switch the slot (one undo step each).
        if (id == BlockId::amp)
            for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
                if (slotSquare (s).contains (e.position))
                    setAsGesture (owner.ampSim.parameters, AmpSimProcessor::slotParamId, (float) s);

        owner.select (id);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto section = info (id).section;
        if (e.mods.isPopupMenu() || (section != Section::pre && section != Section::post))
            return;

        if (! owner.isDragging())
        {
            if (e.getDistanceFromDragStart() < 5)
                return;
            owner.beginDrag (id, e.getEventRelativeTo (&owner).getMouseDownPosition().toFloat());
        }
        owner.dragTo (e.getEventRelativeTo (&owner).position);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (owner.isDragging() && owner.dragged == id)
            owner.endDrag();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& i = info (id);
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto on = isOn();
        const auto alpha = on ? 1.0f : offAlpha;
        const auto hover = isMouseOver (true) && ! lifted;

        juce::Path body;
        body.addRoundedRectangle (bounds, radiusPanel);
        g.setColour (hover && ! selected ? surfaceRaised.brighter (0.06f) : surfaceRaised);
        g.fillPath (body);
        if (selected)
        {
            g.setColour (accentSoft);
            g.fillPath (body);
        }

        // The section's bar along the top.
        {
            const juce::Graphics::ScopedSaveState saved (g);
            g.reduceClipRegion (body);
            g.setColour (sectionColour (i.section).withMultipliedAlpha (alpha));
            g.fillRect (bounds.withHeight (3.0f));
        }

        g.setColour (selected ? accent : (lifted ? accent.withAlpha (0.7f) : outline));
        g.strokePath (body, juce::PathStrokeType (selected ? 1.5f : 1.0f));

        if (i.section == Section::inOut)
        {
            g.setFont (font ("Semibold", 11.0f));
            g.setColour (textDim);
            g.drawText (id == BlockId::input ? "IN" : "OUT", getLocalBounds().withTrimmedTop (6).withHeight (18), juce::Justification::centred, false);
            g.setFont (font (Text::caption));
            g.drawFittedText (stateLine, getLocalBounds().withTrimmedTop (24).withHeight (16).reduced (2, 0), juce::Justification::centred, 1, 0.7f);
            return;
        }

        const auto nameRight = power != nullptr ? getWidth() - 38 : (id == BlockId::amp ? (int) slotSquare (0).getX() - 4 : getWidth() - 10);
        g.setFont (font ("Semibold", 13.0f));
        g.setColour (text.withMultipliedAlpha (alpha));
        g.drawFittedText (i.name, juce::Rectangle<int> (10, 8, nameRight - 10, 17), juce::Justification::centredLeft, 1, 0.8f);
        g.setFont (font (Text::caption));
        g.setColour (textDim.withMultipliedAlpha (alpha));
        g.drawFittedText (on ? stateLine : juce::String ("off"), juce::Rectangle<int> (10, 27, getWidth() - 18, 14), juce::Justification::centredLeft, 1, 0.75f);

        if (id == BlockId::amp)
        {
            for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
            {
                const auto square = slotSquare (s);
                const auto active = s == slotShown;
                g.setColour (active ? accent : surfaceRaised.brighter (0.08f));
                g.fillRoundedRectangle (square, 3.0f);
                if (! active)
                {
                    g.setColour (outline.brighter (0.2f));
                    g.drawRoundedRectangle (square.reduced (0.5f), 3.0f, 1.0f);
                }
                g.setColour (active ? onAccent : textDim);
                g.setFont (font ("Semibold", 10.0f));
                g.drawText (juce::String (s + 1), square, juce::Justification::centred, false);
            }
        }
    }

private:
    juce::Rectangle<float> slotSquare (int s) const
    {
        return { (float) getWidth() - 8.0f - (float) (AmpSimProcessor::numAmpSlots - s) * 17.0f, 7.0f, 15.0f, 15.0f };
    }

    ChainStrip& owner;
    std::unique_ptr<PowerSwitch> power;
    juce::String stateLine;
    bool selected = false;
    int slotShown = -1;
};

// ---- ChainStrip ------------------------------------------------------------------------------------

ChainStrip::ChainStrip (AmpSimProcessor& processor) : ampSim (processor)
{
    setOpaque (true);
    for (int b = 0; b < numBlocks; ++b)
    {
        cards[(size_t) b] = std::make_unique<Card> (*this, (BlockId) b);
        addAndMakeVisible (*cards[(size_t) b]);
    }
    refresh (ampSim.getStatus());
}

ChainStrip::~ChainStrip()
{
    stopTimer();
}

juce::Component* ChainStrip::getCard (BlockId id) const
{
    return cards[(size_t) id].get();
}

std::vector<BlockId> ChainStrip::getShownOrder (Section section) const
{
    return section == Section::pre ? preOrder : postOrder;
}

void ChainStrip::select (BlockId id)
{
    setSelected (id);
    if (onSelect)
        onSelect (id);
}

void ChainStrip::setSelected (BlockId id)
{
    if (id == selected)
        return;
    selected = id;
    for (auto& card : cards)
        card->repaint();
}

void ChainStrip::resized()
{
    // Row one (the input, the pre section, the amp, Gate B, the cab) is eight cards wide plus its fixed
    // parts; row two (the post section, the output) is aligned to the same right edge.
    const auto available = getWidth() - 2 * stripPad;
    cardWidth = juce::jlimit (72, chainBlockMaxWidth, (available - fixedWidth) / 8);
    const auto rowWidth = 8 * cardWidth + fixedWidth;
    const auto x0 = (getWidth() - rowWidth) / 2;
    const auto row1 = topPad, row2 = topPad + boxHeight + rowGap;

    inputSlot = { x0, row1 + boxPad, chainCapWidth, chainBlockHeight };
    preBox = { inputSlot.getRight() + unitGap, row1, 2 * boxPad + 5 * cardWidth + 4 * chainGap, boxHeight };
    ampSlot = { preBox.getRight() + unitGap, row1 + boxPad, cardWidth, chainBlockHeight };
    gateBSlot = ampSlot.translated (cardWidth + unitGap, 0);
    cabSlot = gateBSlot.translated (cardWidth + unitGap, 0);
    postBox = { x0, row2, 2 * boxPad + 8 * cardWidth + 7 * chainGap, boxHeight };
    outputSlot = { cabSlot.getRight() - chainCapWidth, row2 + boxPad, chainCapWidth, chainBlockHeight };

    placeCards (false);
}

juce::Rectangle<int> ChainStrip::slot (Section section, int index) const
{
    const auto box = sectionBox (section);
    return { box.getX() + boxPad + index * (cardWidth + chainGap), box.getY() + boxPad, cardWidth, chainBlockHeight };
}

void ChainStrip::placeCards (bool animate)
{
    const auto set = [this, animate] (BlockId id, juce::Rectangle<int> bounds)
    {
        auto& card = *cards[(size_t) id];
        card.target = bounds;
        if (! animate || ! isShowing())
            card.setBounds (bounds);
    };

    set (BlockId::input, inputSlot);
    set (BlockId::amp, ampSlot);
    set (BlockId::gateB, gateBSlot);
    set (BlockId::cab, cabSlot);
    set (BlockId::output, outputSlot);

    for (auto section : { Section::pre, Section::post })
    {
        if (isDragging() && dragSection == section)
        {
            // The others, with a gap where the dragged card will land.
            for (size_t i = 0; i < dragOthers.size(); ++i)
                set (dragOthers[i], slot (section, (int) i < dragInsert ? (int) i : (int) i + 1));
        }
        else
        {
            const auto& shown = order (section);
            for (size_t i = 0; i < shown.size(); ++i)
                set (shown[i], slot (section, (int) i));
        }
    }

    if (animate && isShowing())
        startTimerHz (60);
}

void ChainStrip::timerCallback()
{
    // Each card eases a third of the way to its place per frame: about 100 ms to settle.
    bool moving = false;
    for (auto& card : cards)
    {
        if (card->id == dragged)
            continue;
        const auto now = card->getBounds();
        if (now == card->target)
            continue;
        const auto step = [] (int from, int to) { return std::abs (to - from) <= 1 ? to : from + juce::roundToInt ((float) (to - from) * 0.35f); };
        card->setBounds (step (now.getX(), card->target.getX()), step (now.getY(), card->target.getY()), card->target.getWidth(), card->target.getHeight());
        moving = moving || card->getBounds() != card->target;
    }
    if (! moving)
        stopTimer();
}

void ChainStrip::beginDrag (BlockId id, juce::Point<float> position)
{
    const auto section = info (id).section;
    if (isDragging() || (section != Section::pre && section != Section::post))
        return;

    auto& card = *cards[(size_t) id];
    dragged = id;
    dragSection = section;
    dragOthers = order (section);
    const auto at = std::find (dragOthers.begin(), dragOthers.end(), id);
    dragInsert = (int) (at - dragOthers.begin());
    if (at != dragOthers.end())
        dragOthers.erase (at);
    dragGrab = position.x - (float) card.getX();
    card.lifted = true;
    card.toFront (false);
    card.repaint();
}

void ChainStrip::dragTo (juce::Point<float> position)
{
    if (! isDragging())
        return;

    // The card follows the mouse along its section (never out of it); the slot nearest its left edge is
    // where it will land.
    auto& card = *cards[(size_t) dragged];
    const auto box = sectionBox (dragSection);
    const auto first = box.getX() + boxPad;
    const auto x = juce::jlimit ((float) first - 4.0f, (float) (box.getRight() - boxPad - cardWidth) + 4.0f, position.x - dragGrab);
    card.setTopLeftPosition (juce::roundToInt (x), box.getY() + boxPad - 2);

    const auto insert = juce::jlimit (0, (int) dragOthers.size(), juce::roundToInt ((x - (float) first) / (float) (cardWidth + chainGap)));
    if (insert != dragInsert)
    {
        dragInsert = insert;
        placeCards (true);
    }
    repaint();
}

void ChainStrip::endDrag()
{
    if (! isDragging())
        return;

    auto newOrder = dragOthers;
    newOrder.insert (newOrder.begin() + juce::jlimit (0, (int) newOrder.size(), dragInsert), dragged);
    auto& card = *cards[(size_t) dragged];
    card.lifted = false;
    const auto section = dragSection;
    dragged = BlockId::count;
    dragOthers.clear();

    if (newOrder != order (section))
    {
        // The same request the order strip made: the processor checks it, saves it, and the chain dips
        // the section, swaps, and fades back.
        juce::StringArray names;
        for (auto id : newOrder)
            names.add (info (id).orderName);
        order (section) = newOrder;
        ampSim.setSectionOrder (chainSection (section), names);
    }

    placeCards (true);
    card.repaint();
    repaint();
}

void ChainStrip::refresh (const AmpSimProcessor::Status& status)
{
    if (! isDragging())
    {
        bool changed = false;
        for (auto section : { Section::pre, Section::post })
        {
            std::vector<BlockId> shown;
            for (const auto& name : ampSim.getSectionOrder (chainSection (section)))
                if (const auto id = blockFor (section, name); id != BlockId::count)
                    shown.push_back (id);
            if (shown != order (section))
            {
                order (section) = shown;
                changed = true;
            }
        }
        if (changed)
            placeCards (true);
    }

    const auto activeSlot = juce::roundToInt (ampSim.parameters.getRawParameterValue (AmpSimProcessor::slotParamId)->load());
    for (auto& card : cards)
        card->update (describe (card->id, status), card->id == selected, activeSlot);
}

juce::String ChainStrip::describe (BlockId id, const AmpSimProcessor::Status& status) const
{
    auto& state = ampSim.parameters;
    const auto value = [&state] (const char* parameterId) { return state.getRawParameterValue (parameterId)->load(); };
    const auto choice = [&state] (const char* parameterId)
    {
        auto* parameter = state.getParameter (parameterId);
        return parameter != nullptr ? parameter->getCurrentValueAsText() : juce::String();
    };

    switch (id)
    {
        case BlockId::input:  return signedDb (value ("input_gain"));
        case BlockId::output: return signedDb (value ("output_gain"));
        case BlockId::gateA:
            return ampSim.isLearningGates() ? juce::String ("Learning...") : juce::String (juce::roundToInt (value ("gate_a_threshold"))) + " dB, " + choice ("gate_a_release_mode");
        case BlockId::gateB:
            return value ("gate_link") >= 0.5f ? juce::String ("Follows Gate A") : juce::String (juce::roundToInt (value ("gate_b_threshold"))) + " dB, " + choice ("gate_b_release_mode");
        case BlockId::preCompressor:  return choice ("comp_pre_mode") + ", " + juce::String (value ("comp_pre_ratio"), 1) + ":1";
        case BlockId::postCompressor: return choice ("comp_post_mode") + ", " + juce::String (value ("comp_post_ratio"), 1) + ":1";
        case BlockId::boost:          return choice ("boost_mode");
        case BlockId::overdrive:      return choice ("od_mode");
        case BlockId::preEq:          return choice ("eq_pre_mode");
        case BlockId::postEq:         return choice ("eq_post_mode");
        case BlockId::amp:
        {
            const auto slot = juce::jlimit (0, AmpSimProcessor::numAmpSlots - 1, juce::roundToInt (value (AmpSimProcessor::slotParamId.toRawUTF8())));
            const auto& model = status.model[(size_t) slot];
            return status.modelError[(size_t) slot] ? juce::String ("Capture missing") : model.upToFirstOccurrenceOf (" (", false, false);
        }
        case BlockId::cab:
        {
            int close = 0;
            for (int m = 0; m < AmpSimProcessor::roomMic; ++m)
                close += status.cab[(size_t) m] != "No IR" && ! status.cabError[(size_t) m] ? 1 : 0;
            const auto room = status.cab[(size_t) AmpSimProcessor::roomMic] != "No IR" && ! status.cabError[(size_t) AmpSimProcessor::roomMic];
            if (close == 0 && ! room)
                return "No IRs";
            return juce::String (close) + (close == 1 ? " mic" : " mics") + (room ? " + room" : "");
        }
        case BlockId::harmonizer: return choice ("harm_root") + " " + choice ("harm_scale");
        case BlockId::multivoicer:
            return choice ("mv_engine") + ", " + juce::String (juce::roundToInt (value ("mv_voices"))) + " voices";
        case BlockId::bloom:
        {
            juce::StringArray on;
            for (const auto& [parameterId, name] : { std::pair<const char*, const char*> { "bloom_crush_on", "Crush" }, { "bloom_phaser_on", "Phaser" }, { "bloom_flanger_on", "Flanger" } })
                if (value (parameterId) >= 0.5f)
                    on.add (name);
            return on.isEmpty() ? juce::String ("Nothing on") : on.joinIntoString (" + ");
        }
        case BlockId::chorus: return choice ("chorus_mode");
        case BlockId::delay:
            return (value ("delay_sync") >= 0.5f ? choice ("delay_note") : juce::String (juce::roundToInt (value ("delay_time"))) + " ms") + ", " + choice ("delay_mode");
        case BlockId::reverb: return choice ("reverb_engine") + ", " + juce::String (value ("reverb_decay"), 1) + " s";
        case BlockId::count:  break;
    }
    return {};
}

void ChainStrip::paint (juce::Graphics& g)
{
    g.fillAll (background);

    // The signal's path: one line through every card, turning down from the cab into row two. Cards are
    // drawn over it, so it shows in the gaps between them.
    const auto y1 = (float) ampSlot.getCentreY(), y2 = (float) outputSlot.getCentreY();
    const auto turnX = (float) cabSlot.getRight() + 8.0f;
    const auto midY = (float) (preBox.getBottom() + postBox.getY()) * 0.5f - 2.0f;
    const auto backX = (float) postBox.getX() - 8.0f;
    juce::Path path;
    path.startNewSubPath ((float) inputSlot.getCentreX(), y1);
    path.lineTo (turnX, y1);
    path.lineTo (turnX, midY);
    path.lineTo (backX, midY);
    path.lineTo (backX, y2);
    path.lineTo ((float) outputSlot.getCentreX(), y2);
    g.setColour (outline.brighter (0.2f));
    g.strokePath (path.createPathWithRoundedCorners (6.0f), juce::PathStrokeType (1.5f));

    // Chevrons in the gaps: which way the signal goes.
    const auto chevron = [&g] (float x, float y)
    {
        juce::Path p;
        p.startNewSubPath (x - 2.5f, y - 4.0f);
        p.lineTo (x + 1.5f, y);
        p.lineTo (x - 2.5f, y + 4.0f);
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    g.setColour (textDim.withAlpha (0.7f));
    chevron ((float) inputSlot.getRight() + unitGap * 0.5f, y1);
    chevron ((float) preBox.getRight() + unitGap * 0.5f, y1);
    chevron ((float) ampSlot.getRight() + unitGap * 0.5f, y1);
    chevron ((float) gateBSlot.getRight() + unitGap * 0.5f, y1);
    chevron ((float) postBox.getRight() + (float) (outputSlot.getX() - postBox.getRight()) * 0.5f, y2);

    // The two reorderable sections: labelled boxes in their colours.
    for (const auto& [box, label, colour] : { std::tuple<juce::Rectangle<int>, const char*, juce::Colour> { preBox, "PRE", sectionPre },
                                              std::tuple<juce::Rectangle<int>, const char*, juce::Colour> { postBox, "POST", sectionPost } })
    {
        g.setColour (colour.withAlpha (0.45f));
        g.drawRoundedRectangle (box.toFloat().reduced (0.5f), radiusPanel, 1.0f);

        const auto labelFont = font ("Semibold", 11.0f).withExtraKerningFactor (0.08f);
        const auto hintFont = font (Text::caption);
        const juce::String hint ("drag to reorder");
        const auto labelWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (labelFont, label));
        const auto hintWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (hintFont, hint));
        const auto legend = juce::Rectangle<int> (box.getX() + 10, box.getY() - 7, labelWidth + hintWidth + 22, 14);
        g.setColour (background);
        g.fillRect (legend);
        g.setFont (labelFont);
        g.setColour (colour);
        g.drawText (label, legend.withTrimmedLeft (4), juce::Justification::centredLeft, false);
        g.setFont (hintFont);
        g.setColour (textDim);
        g.drawText (hint, legend.withTrimmedLeft (labelWidth + 14), juce::Justification::centredLeft, false);
    }

    // The lifted card's shadow.
    if (isDragging())
    {
        const auto& card = *cards[(size_t) dragged];
        juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 14, { 0, 5 }).drawForRectangle (g, card.getBounds());
    }
}

} // namespace ui

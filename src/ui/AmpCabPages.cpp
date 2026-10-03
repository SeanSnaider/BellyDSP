#include "Pages.h"

namespace ui
{

using namespace theme;

namespace
{
void paintCardBackground (juce::Graphics& g, juce::Rectangle<int> bounds, bool active, juce::Colour activeColour = accent)
{
    const auto r = bounds.toFloat().reduced (0.5f);
    g.setColour (surfaceRaised.withAlpha (0.42f));
    g.fillRoundedRectangle (r, radiusPanel);
    g.setColour (active ? activeColour : outline);
    g.drawRoundedRectangle (r, radiusPanel, active ? 1.5f : 1.0f);
}
} // namespace

// ---- AmpPage -------------------------------------------------------------------------------------------

/// One amp slot (UI_DESIGN "Layout": the amp page's slot cards): its capture, the capture's status, the
/// tone controls, and the trims. The playing slot is outlined in the amp's colour and badged; the others
/// have a Play button.
class AmpPage::SlotCard final : public ControlGroup
{
public:
    SlotCard (AmpSimProcessor& p, int slotIndex, std::function<void()> onLoad) : ControlGroup (p), slot (slotIndex)
    {
        load = &addButton ("Load capture...", std::move (onLoad));
        load->setTooltip ("A .nam capture (amp only; the cab comes after)");
        play = &addButton ("Play", [this] { setAsGesture (state, AmpSimProcessor::slotParamId, (float) slot); });
        play->setTooltip ("Switch to this slot (or press program change " + juce::String (slot + 1) + " on the footswitch)");
        tagged (*play, AmpSimProcessor::slotParamId);
        status = &addLabel ({}, Text::label, theme::text);
        status->setJustificationType (juce::Justification::topLeft);

        for (const auto& band : ampsim::AmpTone::bands)
            tone.push_back (&addKnob (AmpSimProcessor::ampParamId (slot, juce::String (band.name).toLowerCase()), band.name));
        trims = { &addKnob (AmpSimProcessor::ampParamId (slot, "input_trim"), "Input"), &addKnob (AmpSimProcessor::ampParamId (slot, "output_trim"), "Output") };
    }

    void setActive (bool isActive)
    {
        if (active != isActive)
        {
            active = isActive;
            play->setVisible (! active);
            repaint();
        }
    }

    void setStatus (const juce::String& line, bool isError)
    {
        status->setText (line == "Empty" ? juce::String ("Empty: the clean DI passes through") : line, juce::dontSendNotification);
        status->setColour (juce::Label::textColourId, isError ? error : (line == "Empty" ? textDim : theme::text));
    }

    void paint (juce::Graphics& g) override
    {
        paintCardBackground (g, getLocalBounds(), active, sectionAmp);
        auto header = getLocalBounds().reduced (space::l, space::m).withHeight (controlHeight);
        g.setFont (font ("Semibold", 16.0f));
        g.setColour (theme::text);
        g.drawText ("Amp " + juce::String (slot + 1), header.removeFromLeft (64), juce::Justification::centredLeft, false);
        if (active)
        {
            // The playing slot's badge.
            const auto badge = header.removeFromLeft (70).withSizeKeepingCentre (66, 20).toFloat();
            g.setColour (sectionAmp);
            g.fillRoundedRectangle (badge, 10.0f);
            g.setColour (onAccent);
            g.setFont (font ("Semibold", 10.5f).withExtraKerningFactor (0.06f));
            g.drawText ("PLAYING", badge, juce::Justification::centred, false);
        }
        paintHeadings (g);
    }

    void resized() override
    {
        clearHeadings();
        auto area = getLocalBounds().reduced (space::l, space::m);
        auto header = area.removeFromTop (controlHeight);
        load->setBounds (header.removeFromRight (128));
        header.removeFromLeft (64 + space::xs);
        play->setBounds (header.removeFromLeft (64));
        area.removeFromTop (space::s);
        status->setBounds (area.removeFromTop (34));
        area.removeFromTop (space::s);

        // The tone controls and the trims as one block, centred in what's left.
        const auto size = knobSizeFor (area.getWidth(), (int) tone.size(), 2);
        const auto rowHeight = Knob::preferredHeight (size);
        const auto blockHeight = 2 * (18 + rowHeight) + space::m;
        auto block = area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), blockHeight));
        heading (block.removeFromTop (18), "Tone");
        placeKnobs (block.removeFromTop (rowHeight), tone, size, 2);
        block.removeFromTop (space::m);
        heading (block.removeFromTop (18), "Trims, before and after the capture");
        placeKnobs (block.removeFromTop (rowHeight), trims, size, 2);
    }

private:
    const int slot;
    bool active = false;
    juce::TextButton *load = nullptr, *play = nullptr;
    juce::Label* status = nullptr;
    std::vector<Knob*> tone, trims;
};

AmpPage::AmpPage (AmpSimProcessor& p, std::function<void (int)> onLoad) : BlockPage (p, BlockId::amp, "Amp", "Three captures, always running")
{
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        cards[(size_t) s] = &adopt (std::make_unique<SlotCard> (p, s, [onLoad, s] { onLoad (s); }));

    // The input calibration (global settings) in the header.
    calibrate = &addSwitch ("input_calibrate", "Calibrate to captures");
    calibrate->setTooltip ("A capture that recorded its input level hears the guitar at the level it was made with. A global setting.");
    interfaceLabel = &addLabel ("Interface at 0 dBFS", Text::label, textDim);
    interfaceLabel->setJustificationType (juce::Justification::centredRight);
    interfaceLevel = &addField ("input_level_dbu", " dBu");
    interfaceLevel->setShowsBar (false);
    interfaceLevel->setTooltip ("The analog level that reaches 0 dBFS on your interface. Solo 4th Gen instrument input at minimum gain: +12 dBu. "
                                "Captures without a recorded level aren't changed.");
    refresh();
}

void AmpPage::layoutContent (juce::Rectangle<int> area)
{
    auto header = headerSpace;
    interfaceLevel->setBounds (header.removeFromRight (96).withSizeKeepingCentre (96, controlHeight));
    header.removeFromRight (space::s);
    interfaceLabel->setBounds (header.removeFromRight (124));
    header.removeFromRight (space::l);
    calibrate->setBounds (header.removeFromRight (calibrate->getPreferredWidth()).withSizeKeepingCentre (calibrate->getPreferredWidth(), switchHeight));

    const auto gap = space::m;
    const auto width = (area.getWidth() - 2 * gap) / AmpSimProcessor::numAmpSlots;
    for (auto* card : cards)
    {
        card->setBounds (area.removeFromLeft (width));
        area.removeFromLeft (gap);
    }
}

void AmpPage::refresh()
{
    const auto status = ampSim.getStatus();
    const auto active = juce::roundToInt (state.getRawParameterValue (AmpSimProcessor::slotParamId)->load());
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        cards[(size_t) s]->setActive (s == active);
        cards[(size_t) s]->setStatus (status.model[(size_t) s], status.modelError[(size_t) s]);
    }
}

// ---- CabPage: the speaker map ---------------------------------------------------------------------------

/// The speaker seen in section from the side, with the close mics in front of it (UI_DESIGN "Layout": the
/// cab page's speaker drawing and draggable mics). The map under the speaker is a cab pack's position map:
/// across, the dust cap (left, on the speaker's axis) to the cone's edge (right); down, the closest capture
/// (at the grille) to the farthest. A loaded pack's captured positions are dots in the mic's colour;
/// dragging a mic sets its two position parameters, and the processor re-morphs its IR (the pack logic
/// of Phase 3, unchanged). A mic without a pack is drawn hollow and doesn't move.
class CabPage::SpeakerMap final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SpeakerMap (AmpSimProcessor& p) : ampSim (p)
    {
        for (int m = 0; m < 2; ++m)
        {
            xParameter[(size_t) m] = p.parameters.getParameter (AmpSimProcessor::cabParamId (m, "pos_x"));
            yParameter[(size_t) m] = p.parameters.getParameter (AmpSimProcessor::cabParamId (m, "pos_y"));
        }
        setTooltip ("Load a cab pack into a close mic, then drag the mic: across the cone, and away from the grille");
    }

    void setPoints (int mic, std::vector<juce::Point<float>> packPoints)
    {
        if (packPoints != points[(size_t) mic])
        {
            points[(size_t) mic] = std::move (packPoints);
            repaint();
        }
    }

    void setRoomLoaded (bool loaded)
    {
        if (loaded != roomLoaded)
        {
            roomLoaded = loaded;
            repaint();
        }
    }

    /// Repaints if a mic's position parameters moved (automation, a preset, another view).
    void refresh()
    {
        for (int m = 0; m < 2; ++m)
            if (position (m) != drawn[(size_t) m])
            {
                repaint();
                return;
            }
    }

    /// Where a mic is on the map, 0 to 1 each way (its two parameters).
    juce::Point<float> position (int mic) const
    {
        return { xParameter[(size_t) mic]->convertFrom0to1 (xParameter[(size_t) mic]->getValue()),
                 yParameter[(size_t) mic]->convertFrom0to1 (yParameter[(size_t) mic]->getValue()) };
    }

    juce::Rectangle<float> map() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft (46.0f).withTrimmedRight (20.0f).withTrimmedTop (speakerHeight + 22.0f).withTrimmedBottom (44.0f);
    }

    juce::Point<float> toScreen (juce::Point<float> p) const
    {
        const auto m = map();
        return { m.getX() + p.x * m.getWidth(), m.getY() + p.y * m.getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (background);
        g.fillRoundedRectangle (bounds, radiusPanel);
        g.setColour (outline);
        g.drawRoundedRectangle (bounds.reduced (0.5f), radiusPanel, 1.0f);

        const auto m = map();
        drawSpeaker (g);

        // The map's grid and its axes.
        g.setColour (outline);
        for (int i = 0; i <= 4; ++i)
        {
            g.drawVerticalLine (juce::roundToInt (m.getX() + m.getWidth() * (float) i / 4.0f), m.getY(), m.getBottom());
            g.drawHorizontalLine (juce::roundToInt (m.getY() + m.getHeight() * (float) i / 4.0f), m.getX(), m.getRight());
        }
        g.setFont (font (Text::caption));
        g.setColour (textDim);
        g.drawText ("cap", juce::Rectangle<float> (m.getX() - 12.0f, m.getBottom() + 4.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);
        g.drawText ("edge", juce::Rectangle<float> (m.getRight() - 30.0f, m.getBottom() + 4.0f, 40.0f, 14.0f), juce::Justification::centredRight, false);
        g.drawText ("close", juce::Rectangle<float> (6.0f, m.getY() - 2.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);
        g.drawText ("far", juce::Rectangle<float> (6.0f, m.getBottom() - 12.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);

        // Each pack's captured positions, then the mics.
        for (int mic = 0; mic < 2; ++mic)
        {
            g.setColour (colourOf (mic).withAlpha (0.55f));
            for (const auto& p : points[(size_t) mic])
                g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (toScreen (p)));
        }
        for (int mic = 0; mic < 2; ++mic)
        {
            drawn[(size_t) mic] = position (mic);
            drawMic (g, toScreen (drawn[(size_t) mic]), colourOf (mic), juce::String (mic + 1), ! points[(size_t) mic].empty(), mic == hovered || mic == dragging);
        }

        if (points[0].empty() && points[1].empty())
        {
            g.setFont (font (Text::label));
            g.setColour (textDim);
            g.drawFittedText ("Load a cab pack (a folder of IRs of one cab at several mic positions) into a close mic to move it here",
                              m.reduced (16.0f, 0.0f).withTrimmedTop (m.getHeight() * 0.62f).toNearestInt(), juce::Justification::centredTop, 3);
        }

        // The legend. The room mic has no position: it's the room, off the map.
        auto legend = bounds.withTrimmedLeft (46.0f).removeFromBottom (22.0f).withTrimmedBottom (4.0f);
        const auto captionFont = font (Text::caption);
        g.setFont (captionFont);
        for (const auto& [colour, label, live] : { std::tuple<juce::Colour, const char*, bool> { micOne, "Close 1", ! points[0].empty() },
                                                   std::tuple<juce::Colour, const char*, bool> { micTwo, "Close 2", ! points[1].empty() },
                                                   std::tuple<juce::Colour, const char*, bool> { micRoom, "Room (no position)", roomLoaded } })
        {
            const auto dot = legend.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f);
            if (live)
            {
                g.setColour (colour);
                g.fillEllipse (dot);
            }
            else
            {
                g.setColour (colour.withAlpha (0.6f));
                g.drawEllipse (dot.reduced (0.5f), 1.2f);
            }
            legend.removeFromLeft (5.0f);
            g.setColour (textDim);
            const auto width = juce::GlyphArrangement::getStringWidth (captionFont, label) + 2.0f;
            g.drawText (label, legend.removeFromLeft (width), juce::Justification::centredLeft, false);
            legend.removeFromLeft (14.0f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto mic = micAt (e.position);
        if (mic != hovered)
        {
            hovered = mic;
            setMouseCursor (mic >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        // The mic under the pointer; or, clicking the map, the nearest mic that can move.
        auto mic = micAt (e.position);
        if (mic < 0 && map().expanded (8.0f).contains (e.position))
        {
            auto best = std::numeric_limits<float>::max();
            for (int m = 0; m < 2; ++m)
                if (! points[(size_t) m].empty())
                    if (const auto d = toScreen (position (m)).getDistanceFrom (e.position); d < best)
                    {
                        best = d;
                        mic = m;
                    }
        }
        if (mic < 0)
            return;

        dragging = mic;
        beginUndoStep (ampSim.parameters);
        xParameter[(size_t) mic]->beginChangeGesture();
        yParameter[(size_t) mic]->beginChangeGesture();
        moveTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging >= 0 && ! e.mods.isPopupMenu())
            moveTo (e.position);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging < 0)
            return;
        xParameter[(size_t) dragging]->endChangeGesture();
        yParameter[(size_t) dragging]->endChangeGesture();
        dragging = -1;
        repaint();
    }

private:
    static constexpr float speakerHeight = 100.0f;

    static juce::Colour colourOf (int mic) { return mic == 0 ? micOne : micTwo; }

    int micAt (juce::Point<float> p) const
    {
        for (int m = 1; m >= 0; --m)
            if (! points[(size_t) m].empty())
            {
                const auto tip = toScreen (position (m));
                if (juce::Rectangle<float> (tip.x - 12.0f, tip.y - 6.0f, 24.0f, 40.0f).contains (p))
                    return m;
            }
        return -1;
    }

    void moveTo (juce::Point<float> p)
    {
        const auto m = map();
        const auto x = juce::jlimit (0.0f, 1.0f, (p.x - m.getX()) / m.getWidth());
        const auto y = juce::jlimit (0.0f, 1.0f, (p.y - m.getY()) / m.getHeight());
        xParameter[(size_t) dragging]->setValueNotifyingHost (xParameter[(size_t) dragging]->convertTo0to1 (x));
        yParameter[(size_t) dragging]->setValueNotifyingHost (yParameter[(size_t) dragging]->convertTo0to1 (y));
        repaint();
    }

    /// A microphone pointing up at the speaker, its tip on the point: a round head with a grille and a body.
    static void drawMic (juce::Graphics& g, juce::Point<float> tip, juce::Colour colour, const juce::String& label, bool live, bool highlighted)
    {
        const auto head = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ tip.x, tip.y + 7.0f });
        const auto body = juce::Rectangle<float> (tip.x - 5.0f, head.getBottom() - 2.0f, 10.0f, 22.0f);
        juce::Path shape;
        shape.addEllipse (head);
        shape.addRoundedRectangle (body, 3.0f);

        if (live)
        {
            juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 6, { 0, 2 }).drawForPath (g, shape);
            g.setColour (highlighted ? colour.brighter (0.25f) : colour);
            g.fillPath (shape);
            g.setColour (background.withAlpha (0.5f));
            for (int i = -1; i <= 1; ++i)
                g.drawHorizontalLine (juce::roundToInt (head.getCentreY() + 3.0f * (float) i), head.getX() + 3.0f, head.getRight() - 3.0f);
            g.setColour (onAccent);
        }
        else
        {
            g.setColour (colour.withAlpha (0.55f));
            g.strokePath (shape, juce::PathStrokeType (1.5f));
            g.setColour (colour.withAlpha (0.8f));
        }
        g.setFont (font ("Semibold", 10.0f));
        g.drawText (label, body.withTrimmedTop (4.0f), juce::Justification::centredTop, false);
    }

    /// The speaker in half section, facing down toward the mics: its axis on the left (where the dust cap
    /// sits), the cone's edge on the right. At the back the magnet and pole piece and the basket out to the
    /// frame; then the voice coil, the spider, the cone from its neck out to the surround, the dust cap on
    /// the axis, and the grille's plane just in front.
    void drawSpeaker (juce::Graphics& g) const
    {
        const auto m = map();
        const auto L = m.getX(), W = m.getWidth(), T = 12.0f, G = m.getY() - 10.0f; // G: the grille's plane
        const auto x = [L, W] (float t) { return L + t * W; };
        const juce::PathStrokeType thin (1.0f), edge (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        // The basket: from the magnet out to the frame's lip, behind the cone.
        juce::Path basket;
        basket.startNewSubPath (x (0.27f), T + 4.0f);
        basket.lineTo (x (0.97f), G - 26.0f);
        basket.lineTo (x (0.97f), G - 16.0f);
        basket.lineTo (x (0.27f), T + 26.0f);
        basket.closeSubPath();
        g.setColour (outline.withAlpha (0.55f));
        g.fillPath (basket);
        g.setColour (outline.brighter (0.25f));
        g.strokePath (basket, thin);

        // The magnet (a ring) and the pole piece, on the axis at the back.
        const auto magnet = juce::Rectangle<float> (L, T, 0.27f * W, 24.0f);
        g.setGradientFill (juce::ColourGradient (outline.brighter (0.2f), 0.0f, magnet.getY(), outline.darker (0.3f), 0.0f, magnet.getBottom(), false));
        g.fillRect (magnet);
        g.setColour (outline.brighter (0.45f));
        g.drawRect (magnet, 1.0f);
        g.setColour (outline.brighter (0.1f));
        g.fillRect (juce::Rectangle<float> (L, magnet.getBottom(), 0.045f * W, 18.0f));

        // The voice coil's former and the spider (the wavy suspension behind the cone).
        g.setColour (textDim.withAlpha (0.5f));
        g.fillRect (juce::Rectangle<float> (x (0.05f), T + 30.0f, 0.025f * W, 22.0f));
        juce::Path spider;
        spider.startNewSubPath (x (0.075f), T + 42.0f);
        for (int i = 1; i <= 8; ++i)
            spider.lineTo (x (0.075f + 0.02f * (float) i), T + 42.0f + ((i % 2) == 0 ? 0.0f : -3.0f));
        g.strokePath (spider, thin);

        // The cone: from its neck at the voice coil, forward and out to the surround, curved (concave
        // seen from the front), with some thickness.
        const juce::Point<float> neck { x (0.075f), T + 52.0f }, rim { x (0.86f), G - 14.0f }, bend { x (0.45f), G - 34.0f };
        juce::Path cone;
        cone.startNewSubPath (neck);
        cone.quadraticTo (bend, rim);
        cone.lineTo (rim.translated (0.0f, -4.0f));
        cone.quadraticTo (bend.translated (0.0f, -5.0f), neck.translated (0.0f, -5.0f));
        cone.closeSubPath();
        g.setGradientFill (juce::ColourGradient (surfaceRaised.brighter (0.35f), neck.x, neck.y, surfaceRaised.brighter (0.1f), rim.x, rim.y, false));
        g.fillPath (cone);
        g.setColour (textDim.withAlpha (0.8f));
        g.strokePath (cone, thin);

        // The dust cap: a dome on the axis, bulging forward from the cone's neck.
        juce::Path cap;
        cap.startNewSubPath (L, neck.y - 3.0f);
        cap.lineTo (neck.translated (0.0f, -3.0f));
        cap.quadraticTo (x (0.06f), G - 30.0f, L, G - 28.0f);
        cap.closeSubPath();
        g.setColour (surfaceRaised.brighter (0.5f));
        g.fillPath (cap);
        g.setColour (textDim.withAlpha (0.8f));
        g.strokePath (cap, thin);

        // The surround: a half roll joining the cone to the frame, and the frame's front gasket.
        juce::Path surround;
        surround.addCentredArc (x (0.905f), G - 15.0f, 0.045f * W, 7.0f, 0.0f, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi * 3.0f, true);
        g.strokePath (surround, edge);
        g.setColour (outline.brighter (0.35f));
        g.fillRoundedRectangle (juce::Rectangle<float> (x (0.95f), G - 20.0f, 0.05f * W, 12.0f), 2.0f);

        // The axis and the grille's plane.
        const float dashes[] { 3.0f, 3.0f };
        juce::Path axis, grille;
        axis.startNewSubPath (L, T);
        axis.lineTo (L, m.getBottom());
        grille.startNewSubPath (L, G);
        grille.lineTo (m.getRight(), G);
        juce::PathStrokeType (1.0f).createDashedStroke (axis, axis, dashes, 2);
        juce::PathStrokeType (1.0f).createDashedStroke (grille, grille, dashes, 2);
        g.setColour (textDim.withAlpha (0.3f));
        g.fillPath (axis);
        g.setColour (textDim.withAlpha (0.45f));
        g.fillPath (grille);

        g.setFont (font (Text::caption));
        g.setColour (textDim);
        g.drawText ("grille", juce::Rectangle<float> (m.getRight() - 64.0f, G + 1.0f, 62.0f, 12.0f), juce::Justification::centredRight, false);
        g.drawText ("magnet", juce::Rectangle<float> (magnet.getRight() + 6.0f, T - 2.0f, 80.0f, 12.0f), juce::Justification::centredLeft, false);
        g.drawText ("cone", juce::Rectangle<float> (x (0.5f), G - 52.0f, 60.0f, 12.0f), juce::Justification::centredLeft, false);
    }

    AmpSimProcessor& ampSim;
    std::array<juce::RangedAudioParameter*, 2> xParameter {}, yParameter {};
    std::array<std::vector<juce::Point<float>>, 2> points;
    std::array<juce::Point<float>, 2> drawn;
    bool roomLoaded = false;
    int dragging = -1, hovered = -1;
};

// ---- CabPage: a mic's card ------------------------------------------------------------------------------

class CabPage::MicCard final : public ControlGroup
{
public:
    MicCard (AmpSimProcessor& p, int micIndex, std::function<void()> onLoad, std::function<void()> onLoadPack) : ControlGroup (p), mic (micIndex)
    {
        const bool room = mic == AmpSimProcessor::roomMic;
        load = &addButton ("IR...", std::move (onLoad));
        load->setTooltip ("Load an impulse response (.wav, .aif, .flac)");
        if (! room)
        {
            pack = &addButton ("Pack...", std::move (onLoadPack));
            pack->setTooltip ("A folder of IRs of one cab at different mic positions, which makes this mic movable");
        }
        status = &addLabel ({}, Text::label, theme::text);
        status->setJustificationType (juce::Justification::topLeft);

        knobs.push_back (&addKnob (AmpSimProcessor::cabParamId (mic, "level"), "Level", " dB", Knob::Size::compact));
        if (room)
        {
            knobs.push_back (&addKnob (AmpSimProcessor::cabParamId (mic, "predelay"), "Pre-delay", " ms", Knob::Size::compact));
        }
        else
        {
            knobs.push_back (&addKnob (AmpSimProcessor::cabParamId (mic, "pan"), "Pan", "", Knob::Size::compact));
            knobs.push_back (&addKnob (AmpSimProcessor::cabParamId (mic, "delay"), "Delay", " smp", Knob::Size::compact));
            invert = &addSwitch (AmpSimProcessor::cabParamId (mic, "invert"), "Invert");
            channel = &addCombo (AmpSimProcessor::cabParamId (mic, "channel"), { "Left", "Right" });
            channel->setTooltip ("Which channel of a stereo IR file this mic uses");
        }
        mute = &addSwitch (AmpSimProcessor::cabParamId (mic, "mute"), "Mute");
    }

    void setStatus (const juce::String& line, bool isError)
    {
        status->setText (line == "No IR" ? juce::String ("No IR: this mic is silent") : line, juce::dontSendNotification);
        status->setColour (juce::Label::textColourId, isError ? error : (line == "No IR" ? textDim : theme::text));
    }

    void paint (juce::Graphics& g) override
    {
        paintCardBackground (g, getLocalBounds(), false);
        auto header = getLocalBounds().reduced (space::m).withHeight (controlHeight);
        const auto colour = mic == 0 ? micOne : (mic == 1 ? micTwo : micRoom);
        g.setColour (colour);
        g.fillEllipse (header.removeFromLeft (10).toFloat().withSizeKeepingCentre (8.0f, 8.0f));
        header.removeFromLeft (space::s);
        g.setFont (font ("Semibold", 14.0f));
        g.setColour (theme::text);
        g.drawText (mic == AmpSimProcessor::roomMic ? juce::String ("Room") : "Close " + juce::String (mic + 1), header, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (space::m);
        auto header = area.removeFromTop (controlHeight);
        if (pack != nullptr)
        {
            pack->setBounds (header.removeFromRight (64));
            header.removeFromRight (space::xs);
        }
        load->setBounds (header.removeFromRight (pack != nullptr ? 48 : 64));
        area.removeFromTop (space::s);
        status->setBounds (area.removeFromTop (30));

        // The switches along the bottom; the knobs centred between them and the status.
        auto switches = area.removeFromBottom (controlHeight);
        if (invert != nullptr)
            place (invert, switches, space::s);
        place (mute, switches, space::s);
        if (channel != nullptr)
            channel->setBounds (switches.removeFromRight (78).withSizeKeepingCentre (78, controlHeight));
        const auto height = Knob::preferredHeight (Knob::Size::compact);
        const auto width = knobRowWidth ((int) knobs.size(), Knob::Size::compact, 2);
        placeKnobs (area.withSizeKeepingCentre (width, height), knobs, Knob::Size::compact, 2);
    }

private:
    const int mic;
    juce::TextButton *load = nullptr, *pack = nullptr;
    juce::Label* status = nullptr;
    std::vector<Knob*> knobs;
    Switch *invert = nullptr, *mute = nullptr;
    juce::ComboBox* channel = nullptr;
};

// ---- CabPage ---------------------------------------------------------------------------------------------

CabPage::CabPage (AmpSimProcessor& p, std::function<void (int, bool)> onLoad)
    : BlockPage (p, BlockId::cab, "Cab", "Two close mics and a room mic; mono in, stereo out")
{
    map = &adopt (std::make_unique<SpeakerMap> (p));
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
        mics[(size_t) m] = &adopt (std::make_unique<MicCard> (p, m, [onLoad, m] { onLoad (m, false); }, [onLoad, m] { onLoad (m, true); }));

    align = &addSwitch ("cab_align", "Auto-align the close mics");
    alignment = &addLabel ({}, Text::label, textDim);
    alignment->setJustificationType (juce::Justification::topLeft);
    lowCut = &addSwitch ("cab_lowcut_on", {}); // the cards' headings name them
    highCut = &addSwitch ("cab_highcut_on", {});
    lowCutFrequency = &addKnob ("cab_lowcut_freq", "Frequency", " Hz", Knob::Size::compact);
    highCutFrequency = &addKnob ("cab_highcut_freq", "Frequency", " Hz", Knob::Size::compact);
    lowCutSlope = &addCombo ("cab_lowcut_slope", { "12 dB/oct", "24 dB/oct" });
    highCutSlope = &addCombo ("cab_highcut_slope", { "12 dB/oct", "24 dB/oct" });
    refresh();
}

void CabPage::layoutContent (juce::Rectangle<int> area)
{
    map->setBounds (area.removeFromLeft (juce::jlimit (300, 460, area.getWidth() * 36 / 100)));
    area.removeFromLeft (space::m);

    // The alignment and the cuts along the bottom; the three mics' cards above them.
    auto bottom = area.removeFromBottom (knobCardHeight (Knob::Size::compact));
    area.removeFromBottom (space::m);
    const auto gap = space::m;
    const auto width = (area.getWidth() - 2 * gap) / AmpSimProcessor::numCabMics;
    for (auto* card : mics)
    {
        card->setBounds (area.removeFromLeft (width));
        area.removeFromLeft (gap);
    }

    layoutCards (bottom.removeFromRight (juce::jmin (bottom.getWidth() - 220 - gap, 2 * 236 + gap)),
                 { { "Low cut", { { lowCut }, { lowCutFrequency }, { lowCutSlope, 96 } } },
                   { "High cut", { { highCut }, { highCutFrequency }, { highCutSlope, 96 } } } });
    bottom.removeFromRight (gap);
    auto inside = card (bottom, "Alignment");
    auto line = inside.removeFromTop (switchHeight);
    place (align, line);
    inside.removeFromTop (space::xs);
    alignment->setBounds (inside);
}

void CabPage::refresh()
{
    const auto status = ampSim.getStatus();
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
        mics[(size_t) m]->setStatus (status.cab[(size_t) m], status.cabError[(size_t) m]);

    for (int m = 0; m < AmpSimProcessor::roomMic; ++m)
    {
        std::vector<juce::Point<float>> positions;
        for (const auto& p : ampSim.getCabPackPoints (m))
            positions.push_back ({ (float) p.x, (float) p.y });
        map->setPoints (m, std::move (positions));
    }
    map->setRoomLoaded (status.cab[(size_t) AmpSimProcessor::roomMic] != "No IR" && ! status.cabError[(size_t) AmpSimProcessor::roomMic]);
    map->refresh();
    alignment->setText (status.alignment, juce::dontSendNotification);
}

} // namespace ui

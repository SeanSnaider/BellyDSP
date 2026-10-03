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

// ---- CabPage: the speaker map ---------------------------------------------------------------------------

/// The speaker's face along the top and the close mics in front of it (UI_DESIGN "Layout": the cab page's
/// speaker drawing and draggable mics). The map under the speaker is a cab pack's position map: across,
/// the dust cap (left, on the speaker's axis) to the cone's edge (right); down, the closest capture (at the
/// grille) to the farthest. Each mic's dashed line marks the spot on the face it points at. A loaded pack's
/// captured positions are dots in the mic's colour; dragging a mic sets its two position parameters, and
/// the processor re-morphs its IR (the pack logic of Phase 3, unchanged). A mic without a pack is drawn
/// hollow and doesn't move.
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

        // Each pack's captured positions; each mic's aim, up to the spot on the cone it points at; the mics.
        for (int mic = 0; mic < 2; ++mic)
        {
            g.setColour (colourOf (mic).withAlpha (0.55f));
            for (const auto& p : points[(size_t) mic])
                g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (toScreen (p)));
        }
        const float dashes[] { 4.0f, 3.0f };
        for (int mic = 0; mic < 2; ++mic)
        {
            if (points[(size_t) mic].empty())
                continue;
            const auto tip = toScreen (position (mic));
            const auto face = speakerBand();
            juce::Path aim;
            aim.startNewSubPath (tip);
            aim.lineTo (tip.x, face.getBottom());
            juce::PathStrokeType (1.2f).createDashedStroke (aim, aim, dashes, 2);
            g.setColour (colourOf (mic).withAlpha (0.6f));
            g.fillPath (aim);
            g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ tip.x, face.getCentreY() }), 2.0f);
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
        auto legend = bounds.withTrimmedLeft (14.0f).removeFromBottom (22.0f).withTrimmedBottom (4.0f);
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

    /// The band across the top where the speaker's face is drawn.
    juce::Rectangle<float> speakerBand() const
    {
        const auto m = map();
        return { m.getX(), 12.0f, m.getWidth(), speakerHeight - 18.0f };
    }

    /// The speaker's face, from its centre (the dust cap, on the axis at the map's "cap" end) out to its
    /// edge: the right half of a speaker seen from the front, in a band across the top, so each spot on the
    /// face sits straight above the places on the map that aim at it. Concentric, from the outside in: the
    /// frame with a mounting bolt, the surround's roll, the cone with its ribs, and the dust cap's dome.
    /// Under it, the grille's plane, where the map's distances start.
    void drawSpeaker (juce::Graphics& g) const
    {
        const auto m = map();
        const auto band = speakerBand();
        const auto radius = band.getWidth();
        const juce::Point<float> centre { band.getX(), band.getCentreY() };
        const auto disc = [&centre, radius] (float t) { return juce::Rectangle<float> (2.0f * t * radius, 2.0f * t * radius).withCentre (centre); };

        {
            const juce::Graphics::ScopedSaveState saved (g);
            juce::Path clip;
            clip.addRoundedRectangle (band, radiusControl);
            g.reduceClipRegion (clip);

            // The frame and its lip.
            g.setColour (outline.darker (0.2f));
            g.fillEllipse (disc (1.05f));
            g.setColour (outline.brighter (0.1f));
            g.fillEllipse (disc (0.965f));

            // The surround: a roll, lit on its inner side.
            g.setGradientFill (juce::ColourGradient (surfaceRaised.brighter (0.3f), centre.x + 0.86f * radius, centre.y, outline.darker (0.1f),
                                                     centre.x + 0.95f * radius, centre.y, false));
            g.fillEllipse (disc (0.945f));

            // The cone: lighter toward its neck, with faint concentric ribs.
            g.setGradientFill (juce::ColourGradient (surfaceRaised.brighter (0.3f), centre.x, centre.y, surfaceRaised.darker (0.15f),
                                                     centre.x + 0.865f * radius, centre.y, true));
            g.fillEllipse (disc (0.865f));
            g.setColour (juce::Colours::black.withAlpha (0.18f));
            for (auto t : { 0.3f, 0.45f, 0.6f, 0.75f })
                g.drawEllipse (disc (t), 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            for (auto t : { 0.305f, 0.455f, 0.605f, 0.755f })
                g.drawEllipse (disc (t), 1.0f);

            // The dust cap: a dome, its highlight up and to the right of the axis.
            g.setGradientFill (juce::ColourGradient (surfaceRaised.brighter (0.75f), centre.x + 0.05f * radius, centre.y - 0.08f * radius,
                                                     surfaceRaised.brighter (0.05f), centre.x + 0.16f * radius, centre.y + 0.05f * radius, true));
            g.fillEllipse (disc (0.16f));
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.drawEllipse (disc (0.16f), 1.5f);

            // A mounting bolt on the frame.
            g.setColour (outline.brighter (0.6f));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ centre.x + 0.985f * radius, centre.y }));
        }
        g.setColour (outline.brighter (0.2f));
        g.drawRoundedRectangle (band, radiusControl, 1.0f);

        g.setFont (font (Text::caption));
        g.setColour (theme::text.withAlpha (0.75f));
        g.drawText ("dust cap", band.withTrimmedLeft (8.0f).withTrimmedTop (4.0f).withHeight (13.0f), juce::Justification::centredLeft, false);
        g.drawText ("cone", band.withTrimmedLeft (0.45f * radius).withTrimmedTop (4.0f).withHeight (13.0f), juce::Justification::centredLeft, false);
        g.drawText ("edge", band.withTrimmedLeft (0.86f * radius - 30.0f).withWidth (60.0f).withTrimmedTop (4.0f).withHeight (13.0f), juce::Justification::centred, false);

        // The grille's plane, where the distances start.
        const auto grilleY = m.getY() - 6.0f;
        const float dashes[] { 3.0f, 3.0f };
        juce::Path grille;
        grille.startNewSubPath (m.getX(), grilleY);
        grille.lineTo (m.getRight(), grilleY);
        juce::PathStrokeType (1.0f).createDashedStroke (grille, grille, dashes, 2);
        g.setColour (textDim.withAlpha (0.45f));
        g.fillPath (grille);
        g.setColour (textDim);
        g.drawText ("grille", juce::Rectangle<float> (m.getRight() - 64.0f, grilleY - 13.0f, 62.0f, 12.0f), juce::Justification::centredRight, false);
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
        const auto shown = line == "No IR" ? juce::String ("No IR: this mic is silent") : line;
        status->setText (shown, juce::dontSendNotification);
        status->setTooltip (shown);
        status->setColour (juce::Label::textColourId, isError ? error : (line == "No IR" ? textDim : theme::text));
    }

    /// The narrowest a card can be with all its controls side by side.
    static int minimumWidth (bool room) { return room ? 175 : 262; }

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
            place (invert, switches, 6);
        place (mute, switches, 6);
        if (channel != nullptr)
            channel->setBounds (switches.removeFromRight (64).withSizeKeepingCentre (64, 24));
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

    align = &addSwitch ("cab_align", "Auto-align");
    align->setTooltip ("Lines the two close mics up in time (and polarity), measured from their IRs");
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
    // The speaker map takes about a third, but leaves the mics' cards the width they need.
    const auto gap = space::m;
    const auto cardsNeed = 2 * MicCard::minimumWidth (false) + MicCard::minimumWidth (true) + 2 * gap;
    map->setBounds (area.removeFromLeft (juce::jlimit (300, 460, juce::jmin (area.getWidth() * 36 / 100, area.getWidth() - cardsNeed - gap))));
    area.removeFromLeft (gap);

    // The alignment and the cuts along the bottom; the three mics' cards above them (equal thirds when
    // there's room, otherwise the room mic's card, which holds less, gives way first).
    auto bottom = area.removeFromBottom (knobCardHeight (Knob::Size::compact));
    area.removeFromBottom (gap);
    const auto third = (area.getWidth() - 2 * gap) / AmpSimProcessor::numCabMics;
    const auto roomWidth = third >= MicCard::minimumWidth (false) ? third : juce::jmax (MicCard::minimumWidth (true), area.getWidth() - 2 * gap - 2 * MicCard::minimumWidth (false));
    const auto closeWidth = (area.getWidth() - 2 * gap - roomWidth) / 2;
    for (int m = 0; m < AmpSimProcessor::numCabMics; ++m)
    {
        mics[(size_t) m]->setBounds (area.removeFromLeft (m == AmpSimProcessor::roomMic ? roomWidth : closeWidth));
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

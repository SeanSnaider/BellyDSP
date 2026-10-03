#include "LookAndFeel.h"
#include "Pages.h"

#include <map>

namespace ui
{

using namespace theme;

namespace
{
bool isOn (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    return state.getRawParameterValue (id)->load() >= 0.5f;
}

int indexOf (juce::AudioProcessorValueTreeState& state, const juce::String& id)
{
    return juce::roundToInt (state.getRawParameterValue (id)->load());
}

/// Puts a combo box where a knob would be, under a caption like the knob's (a synced time's note).
void placeInKnobCell (juce::Label* caption, juce::ComboBox* box, juce::Rectangle<int> cell)
{
    if (caption != nullptr)
        caption->setBounds (cell.removeFromTop (captionHeight + 2));
    box->setBounds (cell.withSizeKeepingCentre (juce::jmax (64, cell.getWidth() - 4), controlHeight));
}

/// Column headings over a table of fields.
void paintColumns (juce::Graphics& g, juce::Rectangle<int> area, std::initializer_list<const char*> names)
{
    g.setFont (font (Text::label));
    g.setColour (textDim);
    const auto width = area.getWidth() / (int) names.size();
    for (const auto* name : names)
        g.drawText (name, area.removeFromLeft (width), juce::Justification::centred, false);
}
} // namespace

// ---- HarmonizerPage --------------------------------------------------------------------------------------

HarmonizerPage::HarmonizerPage (AmpSimProcessor& p)
    : BlockPage (p, BlockId::harmonizer, "Harmonizer", "Harmonies in the key, after the amp and cab; the pitch is read from the clean DI")
{
    root = &addCombo ("harm_root", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    juce::StringArray scales;
    for (const auto* name : ampsim::harmony::scaleNames)
        scales.add (name);
    scale = &addCombo ("harm_scale", scales);
    outOfKey = &addCombo ("harm_out_of_key", { "Out of key: parallel", "Out of key: snap" });
    floor = &addCombo ("harm_floor", { "Lowest note 110 Hz (A)", "Lowest note 80 Hz (low E)", "Lowest note 60 Hz (drop)" });
    glide = &addKnob ("harm_glide", "Glide", " ms");
    level = &addKnob ("harm_level", "Level");

    // The custom scale: 12 switches, one per semitone above the root, writing the mask parameter's bits.
    static const char* names[] = { "1", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7" };
    for (int k = 0; k < 12; ++k)
    {
        auto& button = addButton (names[k], {});
        button.setClickingTogglesState (true);
        button.setTooltip ("The custom scale's notes, counted from the key (used when the scale is Custom)");
        button.onClick = [this, k]
        {
            auto* mask = state.getParameter ("harm_custom_mask");
            auto bits = juce::roundToInt (mask->convertFrom0to1 (mask->getValue()));
            bits = customNotes[(size_t) k]->getToggleState() ? (bits | (1 << k)) : (bits & ~(1 << k));
            setAsGesture (state, "harm_custom_mask", (float) bits);
        };
        customNotes[(size_t) k] = &button;
    }

    static const char* what[] = { "steps", "semitones", "octave", "level", "pan", "humanize" };
    static const char* suffix[] = { " steps", " st", " oct", " dB", " %", " ms" };
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        auto& row = rows[(size_t) v];
        row.on = &addSwitch (params::HarmonizerParameters::voiceId (v, "on"), "Voice " + juce::String (v + 1));
        row.mode = &addCombo (params::HarmonizerParameters::voiceId (v, "mode"), { "Diatonic", "Chromatic" });
        for (size_t k = 0; k < row.fields.size(); ++k)
            row.fields[k] = &addField (params::HarmonizerParameters::voiceId (v, what[k]), suffix[k]);
    }
    refresh();
}

void HarmonizerPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (root, toolbar, 70);
    place (scale, toolbar, 190);
    place (outOfKey, toolbar, 180);
    place (floor, toolbar, 210);
    area.removeFromTop (space::m);

    // The custom scale, the glide and level, and what it hears; then the voices.
    const auto voicesHeight = cardHeight (18 + 4 * 30);
    const auto size = area.getHeight() >= knobCardHeight() + space::m + voicesHeight ? Knob::Size::normal : Knob::Size::compact;
    glide->setKnobSize (size);
    level->setKnobSize (size);
    std::vector<CardItem> notes;
    for (auto* button : customNotes)
        notes.push_back ({ button, 32, controlHeight });
    const auto insides = layoutCards (area.removeFromTop (knobCardHeight (size)),
                                      { { "Custom scale", notes }, { "Glide and level", { { glide }, { level } } }, { "Hearing", {}, 240 } });
    hearingArea = insides.back().withSizeKeepingCentre (insides.back().getWidth(), juce::jmin (insides.back().getHeight(), 48));
    area.removeFromTop (space::m);

    auto voices = card (area, "Voices");
    columnsArea = voices.removeFromTop (18).withTrimmedLeft (100 + 120 + space::s);
    const auto rowHeight = juce::jmin (32, voices.getHeight() / ampsim::Harmonizer::maxVoices);
    for (auto& r : rows)
    {
        auto line = voices.removeFromTop (rowHeight).reduced (0, 3);
        place (r.on, line, 100, switchHeight, 0);
        place (r.mode, line, 120, controlHeight, space::s);
        const auto width = line.getWidth() / (int) r.fields.size();
        for (auto* field : r.fields)
            field->setBounds (line.removeFromLeft (width).reduced (3, 0));
    }
}

void HarmonizerPage::paintContent (juce::Graphics& g)
{
    paintColumns (g, columnsArea, { "Steps", "Semitones", "Octave", "Level", "Pan", "Humanize" });

    // What it hears now, in a box.
    g.setColour (background);
    g.fillRoundedRectangle (hearingArea.toFloat(), radiusControl);
    g.setColour (theme::text);
    g.setFont (font (Text::body));
    g.drawFittedText (hearing, hearingArea.reduced (12, 0), juce::Justification::centredLeft, 2, 0.85f);
}

void HarmonizerPage::refresh()
{
    // The custom scale's switches follow the parameter, and only matter for the Custom scale.
    const auto mask = indexOf (state, "harm_custom_mask");
    const auto currentScale = indexOf (state, "harm_scale");
    if (mask != shownMask || currentScale != shownScale)
    {
        shownMask = mask;
        shownScale = currentScale;
        for (int k = 0; k < 12; ++k)
        {
            customNotes[(size_t) k]->setToggleState ((mask >> k) & 1, juce::dontSendNotification);
            customNotes[(size_t) k]->setAlpha (currentScale == (int) ampsim::harmony::Scale::custom ? 1.0f : unusedAlpha);
        }
    }

    // Diatonic voices use steps, chromatic ones semitones: dim the other.
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        const auto diatonic = indexOf (state, params::HarmonizerParameters::voiceId (v, "mode")) == 0;
        rows[(size_t) v].fields[0]->setAlpha (diatonic ? 1.0f : unusedAlpha);
        rows[(size_t) v].fields[1]->setAlpha (diatonic ? unusedAlpha : 1.0f);
    }

    juce::String heard;
    if (const auto note = ampSim.getHarmonizerNote(); note < 0)
        heard = "Listening (no single note)";
    else
    {
        heard = "Hearing " + juce::MidiMessage::getMidiNoteName (note, true, true, 4);
        for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
            if (const auto shift = ampSim.getHarmonizerShift (v); shift != ampsim::Harmonizer::shownSilent)
                heard << "   " << (v + 1) << ": " << (shift >= 0 ? "+" : "") << shift << " (" << juce::MidiMessage::getMidiNoteName (note + shift, true, true, 4) << ")";
    }
    if (heard != hearing)
    {
        hearing = heard;
        repaint (hearingArea);
    }
}

// ---- MultivoicerPage -------------------------------------------------------------------------------------

MultivoicerPage::MultivoicerPage (AmpSimProcessor& p)
    : BlockPage (p, BlockId::multivoicer, "Multivoicer", "Up to eight shifted, delayed, panned copies")
{
    engine = &addCombo ("mv_engine", { "Poly (chords)", "Mono (single notes)" });
    highPass = &addSwitch ("mv_hp", "Wet high-pass");
    voices = &addKnob ("mv_voices", "Voices", "");
    mix = &addKnob ("mv_mix", "Mix", " %");
    spread = &addKnob ("mv_spread", "Spread", " %");
    highPassHz = &addKnob ("mv_hp_freq", "Wet HPF", " Hz");

    startingPoints = &addButton ("Starting points...", {});
    startingPoints->setTooltip ("Set every voice to one of the starting points");
    startingPoints->onClick = [this]
    {
        juce::PopupMenu menu;
        using SP = ampsim::Multivoicer::StartingPoint;
        for (const auto& [startingPoint, name] : std::initializer_list<std::pair<SP, const char*>> {
                 { SP::unisonDouble, "Unison double" }, { SP::octaveStack, "Octave stack" }, { SP::fifthsStack, "Fifths stack" }, { SP::doubleOctaves, "Double + Octaves" } })
            menu.addItem (name, [this, sp = startingPoint]
            {
                beginUndoStep (state);
                params::MultivoicerParameters::applyStartingPoint (state, sp);
            });
        showMenu (menu, startingPoints, &getLookAndFeel());
    };

    static const char* what[] = { "semitones", "cents", "delay", "pan", "level", "drift" };
    static const char* suffix[] = { " st", " ct", " ms", " %", " dB", " %" };
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        auto& row = rows[(size_t) v];
        row.number = &addLabel (juce::String (v + 1), Text::value, theme::text);
        row.number->setJustificationType (juce::Justification::centred);
        for (size_t k = 0; k < row.fields.size(); ++k)
            row.fields[k] = &addField (params::MultivoicerParameters::voiceId (v, what[k]), suffix[k]);
    }
    refresh();
}

void MultivoicerPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (engine, toolbar, 200, controlHeight, space::l);
    place (highPass, toolbar);
    startingPoints->setBounds (toolbar.removeFromRight (170));
    area.removeFromTop (space::m);

    // The ensemble's knobs, two by two, in a card on the left; the voices' table in a card on the right.
    const auto size = area.getHeight() >= cardHeight (2 * Knob::preferredHeight (Knob::Size::normal) + space::s) ? Knob::Size::normal : Knob::Size::compact;
    const auto knobWidth = Knob::preferredWidth (size), knobHeight = Knob::preferredHeight (size);
    auto left = card (area.removeFromLeft (2 * cardPadding + 2 * knobWidth + space::m), "Ensemble");
    left = left.withSizeKeepingCentre (left.getWidth(), juce::jmin (left.getHeight(), 2 * knobHeight + space::s));
    placeKnobs (left.removeFromTop (knobHeight), { voices, mix }, size, space::m);
    left.removeFromTop (space::s);
    placeKnobs (left.removeFromTop (knobHeight), { spread, highPassHz }, size, space::m);
    area.removeFromLeft (space::m);

    auto table = card (area, "Voices");
    columnsArea = table.removeFromTop (18).withTrimmedLeft (32);
    const auto rowHeight = juce::jmin (30, table.getHeight() / ampsim::Multivoicer::maxVoices);
    for (auto& r : rows)
    {
        auto line = table.removeFromTop (rowHeight).reduced (0, 2);
        r.number->setBounds (line.removeFromLeft (32));
        const auto width = line.getWidth() / (int) r.fields.size();
        for (auto* field : r.fields)
            field->setBounds (line.removeFromLeft (width).reduced (3, 0));
    }
}

void MultivoicerPage::paintContent (juce::Graphics& g)
{
    paintColumns (g, columnsArea, { "Interval", "Fine", "Delay", "Pan", "Level", "Drift" });
}

void MultivoicerPage::refresh()
{
    const auto count = indexOf (state, "mv_voices");
    if (count == shownVoices)
        return;
    shownVoices = count;
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        const auto alpha = v < count ? 1.0f : unusedAlpha;
        rows[(size_t) v].number->setAlpha (alpha);
        for (auto* field : rows[(size_t) v].fields)
            field->setAlpha (alpha);
    }
}

// ---- BloomPage -------------------------------------------------------------------------------------------

/// The order of Bloom's three effects, as chips with arrows between them. Drag a chip along the row to
/// move it; the order is saved by effect name like the sections' (AmpSimProcessor::setBloomOrder).
class BloomPage::OrderChips final : public juce::Component, public juce::SettableTooltipClient
{
public:
    OrderChips (std::function<juce::StringArray()> getter, std::function<void (const juce::StringArray&)> setter)
        : getOrder (std::move (getter)), setOrder (std::move (setter))
    {
        setTooltip ("The order inside Bloom: drag an effect to move it");
        refresh();
    }

    void refresh()
    {
        if (dragging >= 0)
            return;
        if (const auto order = getOrder(); order != shown)
        {
            shown = order;
            repaint();
        }
    }

    const juce::StringArray& getShown() const noexcept { return shown; }

    /// Moves the effect at `from` to `to` (what a drag does).
    void move (int from, int to)
    {
        auto order = shown;
        order.move (from, to);
        if (order != shown)
        {
            shown = order;
            setOrder (order);
        }
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        for (int i = 0; i < shown.size(); ++i)
            if (chip (i).contains (e.position))
            {
                dragging = i;
                target = i;
                grab = e.position.x - chip (i).getX();
                dragX = chip (i).getX();
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        dragX = juce::jlimit (0.0f, (float) getWidth() - chipWidth(), e.position.x - grab);
        target = juce::jlimit (0, shown.size() - 1, juce::roundToInt (dragX / (chipWidth() + gap)));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging < 0)
            return;
        const auto from = dragging;
        dragging = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        move (from, target);
    }

    void paint (juce::Graphics& g) override
    {
        // The chips in their places (with a gap at the target while dragging), arrows between them.
        std::vector<int> others;
        for (int i = 0; i < shown.size(); ++i)
            if (i != dragging)
                others.push_back (i);

        g.setColour (textDim);
        for (int slot = 0; slot + 1 < shown.size(); ++slot)
        {
            const auto x = chip (slot).getRight() + gap * 0.5f, y = (float) getHeight() * 0.5f;
            juce::Path arrow;
            arrow.startNewSubPath (x - 6.0f, y);
            arrow.lineTo (x + 5.0f, y);
            arrow.startNewSubPath (x + 1.0f, y - 4.0f);
            arrow.lineTo (x + 5.0f, y);
            arrow.lineTo (x + 1.0f, y + 4.0f);
            g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        for (size_t k = 0; k < others.size(); ++k)
        {
            const auto slot = dragging >= 0 && (int) k >= target ? (int) k + 1 : (int) k;
            drawChip (g, chip (slot), others[k], false);
        }
        if (dragging >= 0)
            drawChip (g, chip (0).withX (dragX), dragging, true);
    }

private:
    float chipWidth() const { return ((float) getWidth() - gap * (float) (juce::jmax (1, shown.size()) - 1)) / (float) juce::jmax (1, shown.size()); }
    juce::Rectangle<float> chip (int slot) const { return { (float) slot * (chipWidth() + gap), 1.0f, chipWidth(), (float) getHeight() - 2.0f }; }

    void drawChip (juce::Graphics& g, juce::Rectangle<float> r, int index, bool lifted) const
    {
        g.setColour (surfaceRaised);
        g.fillRoundedRectangle (r, radiusControl);
        g.setColour (lifted ? accent : outline.brighter (0.15f));
        g.drawRoundedRectangle (r.reduced (0.5f), radiusControl, lifted ? 1.5f : 1.0f);
        const auto name = shown[index].substring (0, 1).toUpperCase() + shown[index].substring (1);
        g.setFont (font ("Semibold", 11.0f));
        g.setColour (textDim);
        auto area = r.reduced (10.0f, 0.0f);
        g.drawText (juce::String (index + 1), area.removeFromLeft (14.0f), juce::Justification::centredLeft, false);
        g.setColour (theme::text);
        g.setFont (font (Text::body));
        g.drawText (name, area, juce::Justification::centred, false);
    }

    std::function<juce::StringArray()> getOrder;
    std::function<void (const juce::StringArray&)> setOrder;
    juce::StringArray shown;
    int dragging = -1, target = -1;
    float grab = 0.0f, dragX = 0.0f;
    static constexpr float gap = 24.0f;
};

/// One of Bloom's effects: its switch and controls, in a card.
class BloomPage::EffectCard final : public ControlGroup
{
public:
    EffectCard (AmpSimProcessor& p, juce::String name, const juce::String& onId) : ControlGroup (p), title (std::move (name))
    {
        on = &addSwitch (onId, "On");
    }

    juce::String title;
    Switch* on = nullptr;
    std::vector<juce::Component*> combos;  // the first row
    std::vector<Switch*> switches;         // the second row
    std::vector<Knob*> knobs;
    juce::ComboBox* note = nullptr;        // shown instead of the rate knob when synced
    Knob* rate = nullptr;
    juce::Label* footnote = nullptr;       // a line along the bottom (the flanger's latency)

    using ControlGroup::addCombo;
    using ControlGroup::addKnob;
    using ControlGroup::addLabel;
    using ControlGroup::addSwitch;

    void paint (juce::Graphics& g) override
    {
        drawCard (g, getLocalBounds().toFloat());
        g.setFont (font (Text::title));
        g.setColour (theme::text);
        g.drawText (title, getLocalBounds().reduced (space::m).withHeight (controlHeight), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (space::m);
        auto header = area.removeFromTop (controlHeight);
        on->setBounds (header.removeFromRight (on->getPreferredWidth()).withSizeKeepingCentre (on->getPreferredWidth(), switchHeight));
        area.removeFromTop (space::s);
        if (footnote != nullptr)
            footnote->setBounds (area.removeFromBottom (32));

        if (! combos.empty())
        {
            auto row = area.removeFromTop (controlHeight);
            const auto width = (row.getWidth() - space::s * ((int) combos.size() - 1)) / (int) combos.size();
            for (auto* c : combos)
                place (c, row, width);
            area.removeFromTop (space::s);
        }
        if (! switches.empty())
        {
            auto row = area.removeFromTop (switchHeight);
            for (auto* s : switches)
                place (s, row, space::m);
            area.removeFromTop (space::m);
        }

        // The knobs in rows of as many as fit, each row centred.
        const auto size = Knob::Size::compact;
        const auto perRow = juce::jmax (1, (area.getWidth() + 2) / (Knob::preferredWidth (size) + 2));
        for (size_t i = 0; i < knobs.size(); i += (size_t) perRow)
        {
            std::vector<Knob*> line (knobs.begin() + (long) i, knobs.begin() + (long) juce::jmin (knobs.size(), i + (size_t) perRow));
            auto row = area.removeFromTop (Knob::preferredHeight (size));
            placeKnobs (row.withSizeKeepingCentre (knobRowWidth ((int) line.size(), size, 2), row.getHeight()), line, size, 2);
            area.removeFromTop (space::s);
        }
        if (note != nullptr && rate != nullptr)
            note->setBounds (rate->getBounds().withSizeKeepingCentre (rate->getWidth() + 6, controlHeight));
    }
};

BloomPage::BloomPage (AmpSimProcessor& p) : BlockPage (p, BlockId::bloom, "Bloom", "Three effects in one block")
{
    // In the header: Bloom's mix and the order of its effects.
    mixLabel = &addLabel ("Mix", Text::label, textDim);
    mixLabel->setJustificationType (juce::Justification::centredRight);
    mix = &addField ("bloom_mix", " %");
    chips = &adopt (std::make_unique<OrderChips> ([&p] { return p.getBloomOrder(); }, [&p] (const juce::StringArray& order) { p.setBloomOrder (order); }));

    crush = &adopt (std::make_unique<EffectCard> (p, "Bitcrush", "bloom_crush_on"));
    crush->knobs = { &crush->addKnob ("bloom_crush_bits", "Bits", " bits", Knob::Size::compact), &crush->addKnob ("bloom_crush_rate", "Rate", " Hz", Knob::Size::compact),
                     &crush->addKnob ("bloom_crush_tone", "Tone", " Hz", Knob::Size::compact), &crush->addKnob ("bloom_crush_mix", "Mix", " %", Knob::Size::compact) };
    crush->switches = { &crush->addSwitch ("bloom_crush_dither", "Dither (hiss, not gated decays)") };

    phaser = &adopt (std::make_unique<EffectCard> (p, "Phaser", "bloom_phaser_on"));
    phaser->combos = { &phaser->addCombo ("bloom_phaser_mode", { "Classic", "Modern", "Vibe" }),
                       &phaser->addCombo ("bloom_phaser_stages", { "2 stages", "4 stages", "6 stages", "8 stages", "12 stages" }),
                       &phaser->addCombo ("bloom_phaser_shape", { "Sine", "Triangle" }) };
    phaser->switches = { &phaser->addSwitch ("bloom_phaser_sync", "Sync"), &phaser->addSwitch ("bloom_phaser_classic_fb", "Feedback (later version)") };
    phaser->rate = &phaser->addKnob ("bloom_phaser_rate", "Rate", " Hz", Knob::Size::compact);
    phaser->knobs = { phaser->rate, &phaser->addKnob ("bloom_phaser_depth", "Depth", " %", Knob::Size::compact),
                      &phaser->addKnob ("bloom_phaser_low", "Low", " Hz", Knob::Size::compact), &phaser->addKnob ("bloom_phaser_high", "High", " Hz", Knob::Size::compact),
                      &phaser->addKnob ("bloom_phaser_feedback", "Resonance", " %", Knob::Size::compact),
                      &phaser->addKnob ("bloom_phaser_stereo", "Stereo", " deg", Knob::Size::compact), &phaser->addKnob ("bloom_phaser_mix", "Mix", " %", Knob::Size::compact) };
    phaser->note = &phaser->addCombo ("bloom_phaser_note", noteNames());

    flanger = &adopt (std::make_unique<EffectCard> (p, "Flanger", "bloom_flanger_on"));
    flanger->combos = { &flanger->addCombo ("bloom_flanger_shape", { "Triangle", "Sine", "Random" }) };
    flanger->switches = { &flanger->addSwitch ("bloom_flanger_sync", "Sync"), &flanger->addSwitch ("bloom_flanger_negative", "Negative"),
                          &flanger->addSwitch ("bloom_flanger_tz", "Through-zero") };
    flanger->rate = &flanger->addKnob ("bloom_flanger_rate", "Rate", " Hz", Knob::Size::compact);
    flanger->knobs = { &flanger->addKnob ("bloom_flanger_manual", "Manual", " ms", Knob::Size::compact),
                       &flanger->addKnob ("bloom_flanger_depth", "Depth", " %", Knob::Size::compact), flanger->rate,
                       &flanger->addKnob ("bloom_flanger_feedback", "Feedback", " %", Knob::Size::compact),
                       &flanger->addKnob ("bloom_flanger_stereo", "Stereo", " deg", Knob::Size::compact), &flanger->addKnob ("bloom_flanger_mix", "Mix", " %", Knob::Size::compact) };
    flanger->note = &flanger->addCombo ("bloom_flanger_note", noteNames());
    flanger->switches[2]->setTooltip ("Through-zero flanging: the dry signal is delayed 5 ms (reported to the host) while it's on");
    latency = flanger->footnote = &flanger->addLabel ({}, Text::label, warn);
    latency->setJustificationType (juce::Justification::centredLeft);
    refresh();
}

void BloomPage::layoutContent (juce::Rectangle<int> area)
{
    // In the header: the order, then the mix.
    auto header = headerSpace;
    mix->setBounds (header.removeFromRight (96).withSizeKeepingCentre (96, controlHeight));
    header.removeFromRight (space::s);
    mixLabel->setBounds (header.removeFromRight (32));
    header.removeFromRight (space::xl);
    const auto chipsWidth = juce::jmin (header.getWidth(), 420);
    chips->setBounds (header.removeFromRight (chipsWidth).withSizeKeepingCentre (chipsWidth, 30));

    // The three effects side by side, in the order they run.
    std::map<juce::String, EffectCard*> byName { { "bitcrush", crush }, { "phaser", phaser }, { "flanger", flanger } };
    const auto width = (area.getWidth() - 2 * space::m) / 3;
    for (const auto& name : shownOrder.isEmpty() ? ampSim.getBloomOrder() : shownOrder)
        if (auto* effect = byName[name])
        {
            effect->setBounds (area.removeFromLeft (width));
            area.removeFromLeft (space::m);
        }
}

void BloomPage::refresh()
{
    chips->refresh();
    if (const auto order = ampSim.getBloomOrder(); order != shownOrder)
    {
        shownOrder = order;
        resized();
    }

    // Classic: fixed 4 stages, triangle, its own range, optional feedback. Modern: everything. Vibe: its
    // own stages, range, and sine.
    if (const auto mode = indexOf (state, "bloom_phaser_mode"); mode != shownPhaserMode)
    {
        shownPhaserMode = mode;
        const auto modern = mode == 1;
        for (auto* c : std::initializer_list<juce::Component*> { phaser->combos[1], phaser->combos[2], phaser->knobs[2], phaser->knobs[3], phaser->knobs[4] })
            dim (c, modern);
        dim (phaser->switches[1], mode == 0);
    }
    if (const auto sync = isOn (state, "bloom_phaser_sync") ? 1 : 0; sync != shownPhaserSync)
    {
        shownPhaserSync = sync;
        phaser->rate->setVisible (sync == 0);
        phaser->note->setVisible (sync == 1);
    }
    if (const auto sync = isOn (state, "bloom_flanger_sync") ? 1 : 0; sync != shownFlangerSync)
    {
        shownFlangerSync = sync;
        flanger->rate->setVisible (sync == 0);
        flanger->note->setVisible (sync == 1);
    }

    latency->setText (ampSim.getLatencySamples() > 0 ? "Through-zero adds " + juce::String (ampSim.getLatencySamples() * 1000.0 / 48000.0, 1)
                                                            + " ms of latency while it's on"
                                                      : juce::String(),
                      juce::dontSendNotification);
}

// ---- ChorusPage ------------------------------------------------------------------------------------------

ChorusPage::ChorusPage (AmpSimProcessor& p) : BlockPage (p, BlockId::chorus, "Chorus", "After the cab")
{
    mode = &addCombo ("chorus_mode", { "Classic", "Dimension", "Tri" });
    shape = &addCombo ("chorus_shape", { "Triangle", "Sine", "Random" });
    sync = &addSwitch ("chorus_sync", "Sync to tempo");
    note = &addCombo ("chorus_note", noteNames());
    rate = &addKnob ("chorus_rate", "Rate", " Hz");
    depth = &addKnob ("chorus_depth", "Depth", " %");
    mix = &addKnob ("chorus_mix", "Mix", " %");
    width = &addKnob ("chorus_width", "Width", " %");
    highPassHz = &addKnob ("chorus_hp_freq", "Protect below", " Hz");
    analog = &addSwitch ("chorus_analog", "Analog");
    noise = &addSwitch ("chorus_noise", "Noise");
    highPass = &addSwitch ("chorus_hp", "On");
    mode->setTooltip ("Classic: one voice. Dimension: a wide, gentle doubling that cancels in mono. Tri: three voices.");
    highPass->setTooltip ("Low-end protection: below the frequency only the dry signal passes, so the low strings stay solid");
    refresh();
}

void ChorusPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (mode, toolbar, 140);
    place (shape, toolbar, 130, controlHeight, space::l);
    place (sync, toolbar);
    area.removeFromTop (space::m);

    layoutCards (area.removeFromTop (knobCardHeight()), { { "Speed and depth", { { rate }, { depth } } }, { "Blend", { { mix }, { width } } },
                                                          { "Low-end protection", { { highPass }, { highPassHz } } }, { "Character", { { analog }, { noise } } } });
    placeInKnobCell (nullptr, note, rate->getBounds());
}

void ChorusPage::refresh()
{
    const auto synced = isOn (state, "chorus_sync") ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    note->setVisible (synced == 1);
    rate->setVisible (synced == 0);
}

// ---- DelayPage -------------------------------------------------------------------------------------------

DelayPage::DelayPage (AmpSimProcessor& p) : BlockPage (p, BlockId::delay, "Delay", "After the cab; repeats ring on when switched off")
{
    mode = &addCombo ("delay_mode", { "Digital", "Analog", "Tape" });
    stereo = &addCombo ("delay_stereo", { "Stereo", "Ping-pong", "Dual" });
    sync = &addSwitch ("delay_sync", "Sync to tempo");
    note = &addCombo ("delay_note", noteNames());
    rightNote = &addCombo ("delay_note_right", noteNames());
    noteLabel = &addLabel ("Time", Text::label, textDim);
    rightNoteLabel = &addLabel ("Right time", Text::label, textDim);
    for (auto* l : { noteLabel, rightNoteLabel })
        l->setJustificationType (juce::Justification::centred);

    time = &addKnob ("delay_time", "Time", " ms");
    rightTime = &addKnob ("delay_time_right", "Right time", " ms");
    offset = &addKnob ("delay_offset", "R offset", " ms");
    feedback = &addKnob ("delay_feedback", "Feedback", " %");
    mix = &addKnob ("delay_mix", "Mix", " %");
    lowCut = &addKnob ("delay_lowcut", "Low cut", " Hz");
    highCut = &addKnob ("delay_highcut", "High cut", " Hz");
    modDepth = &addKnob ("delay_mod_depth", "Depth", " ms");
    modRate = &addKnob ("delay_mod_rate", "Rate", " Hz");
    duck = &addKnob ("delay_duck", "Ducking");
    tempo = &addKnob ("tempo_bpm", "Tempo", " BPM");
    tap = &addButton ("Tap", [this] { ampSim.tapTempo(); });
    tap->setTooltip ("Tap the tempo (or use the footswitch's tap CC)");
    stereo->setTooltip ("Stereo: the right repeats offset a little. Ping-pong: repeats alternate sides. Dual: a time per side.");
    duck->setTooltip ("How far the repeats duck while you play");
    refresh();
}

void DelayPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (mode, toolbar, 130);
    place (stereo, toolbar, 130, controlHeight, space::l);
    place (sync, toolbar);
    area.removeFromTop (space::m);

    const auto size = knobSizeFor (area.getWidth() - 4 * 24 - 3 * space::m, 9, controlGap);
    for (auto* knob : { time, rightTime, offset, feedback, mix, lowCut, highCut, modDepth, modRate, duck, tempo })
        knob->setKnobSize (size);
    layoutCards (area.removeFromTop (knobCardHeight (size)), { { "Time", { { time }, { rightTime }, { feedback }, { mix } } },
                                                               { "Tone of the repeats", { { lowCut }, { highCut } } },
                                                               { "Modulation", { { modDepth }, { modRate } } },
                                                               { "Ducking", { { duck } } } });

    // The synced times sit where the time knobs are; the offset shares the right time's place.
    offset->setBounds (rightTime->getBounds());
    placeInKnobCell (noteLabel, note, time->getBounds());
    placeInKnobCell (rightNoteLabel, rightNote, rightTime->getBounds());

    area.removeFromTop (space::m);
    auto row = area.removeFromTop (knobCardHeight (size));
    layoutCards (row.removeFromLeft (juce::jmin (row.getWidth(), 360)), { { "Tempo, shared by every synced effect", { { tempo }, { tap, 64, 32 } } } });
}

void DelayPage::refresh()
{
    const bool synced = isOn (state, "delay_sync");
    const auto layout = indexOf (state, "delay_stereo");
    const auto newState = (synced ? 1 : 0) + 2 * layout;
    if (newState == shownState)
        return;

    shownState = newState;
    note->setVisible (synced);
    noteLabel->setVisible (synced);
    time->setVisible (! synced);
    const bool dual = layout == 2;
    rightNote->setVisible (synced && dual);
    rightNoteLabel->setVisible (synced && dual);
    rightTime->setVisible (! synced && dual);
    offset->setVisible (layout == 0);
}

// ---- ReverbPage ------------------------------------------------------------------------------------------

ReverbPage::ReverbPage (AmpSimProcessor& p) : BlockPage (p, BlockId::reverb, "Reverb", "After the cab; the tail rings on when switched off")
{
    engine = &addCombo ("reverb_engine", { "Room", "Hall", "Plate" });
    freeze = &addSwitch ("reverb_freeze", "Freeze");
    preDelaySync = &addSwitch ("reverb_predelay_sync", "Sync pre-delay");
    preDelayNote = &addCombo ("reverb_predelay_note", noteNames());
    mix = &addKnob ("reverb_mix", "Mix", " %");
    preDelay = &addKnob ("reverb_predelay", "Pre-delay", " ms");
    decay = &addKnob ("reverb_decay", "Decay", " s");
    size = &addKnob ("reverb_size", "Size", " %");
    earlyLate = &addKnob ("reverb_early_late", "Early/late", " %");
    diffusion = &addKnob ("reverb_diffusion", "Diffusion", " %");
    width = &addKnob ("reverb_width", "Width", " %");
    lowDecay = &addKnob ("reverb_low_decay", "Low decay", " x");
    highDecay = &addKnob ("reverb_high_decay", "High decay", " x");
    lowCut = &addKnob ("reverb_lowcut", "Low cut", " Hz");
    highCut = &addKnob ("reverb_highcut", "High cut", " Hz");
    modDepth = &addKnob ("reverb_mod_depth", "Mod depth", " %");
    modRate = &addKnob ("reverb_mod_rate", "Mod rate", " Hz");
    ducking = &addKnob ("reverb_ducking", "Ducking", " %");
    shimmer = &addKnob ("reverb_shimmer", "Shimmer", " %");
    shimmerInterval = &addCombo ("reverb_shimmer_interval", { "+12", "+7", "+19", "+24" });
    shimmerInterval->setTooltip ("The shimmer's interval in semitones (Room and Hall)");
    freeze->setTooltip ("Holds the tail forever (also on the footswitch's freeze CC)");
    refresh();
}

void ReverbPage::layoutContent (juce::Rectangle<int> area)
{
    auto toolbar = area.removeFromTop (controlHeight);
    place (engine, toolbar, 130, controlHeight, space::l);
    place (freeze, toolbar);
    place (preDelaySync, toolbar, space::s);
    place (preDelayNote, toolbar, 100);
    area.removeFromTop (space::m);

    const auto knobSize = knobSizeFor (area.getWidth() - 2 * 24 - space::m, 11, controlGap);
    for (auto* knob : { mix, preDelay, decay, size, earlyLate, diffusion, width, lowDecay, highDecay, lowCut, highCut, modDepth, modRate, ducking, shimmer })
        knob->setKnobSize (knobSize);
    layoutCards (area.removeFromTop (knobCardHeight (knobSize)), { { "Space", { { mix }, { preDelay }, { decay }, { size }, { earlyLate }, { diffusion }, { width } } },
                                                                   { "Tone", { { lowDecay }, { highDecay }, { lowCut }, { highCut } } } });
    area.removeFromTop (space::m);
    layoutCards (area.removeFromTop (knobCardHeight (knobSize)), { { "Motion", { { modDepth }, { modRate }, { ducking } } },
                                                                   { "Shimmer (Room and Hall)", { { shimmer }, { shimmerInterval, 84 } } } });
}

void ReverbPage::refresh()
{
    const auto synced = isOn (state, "reverb_predelay_sync") ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    preDelayNote->setVisible (synced == 1);
    preDelay->setAlpha (synced == 1 ? unusedAlpha : 1.0f);
}

} // namespace ui

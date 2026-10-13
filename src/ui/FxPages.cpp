// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

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
    outOfKey = &addCombo ("harm_out_of_key", { "Parallel", "Snap to the key" });
    floor = &addCombo ("harm_floor", { "110 Hz (A)", "80 Hz (low E)", "60 Hz (drop)" });
    outOfKey->setTooltip ("What a note outside the key does: shift in parallel, or snap to the key");
    floor->setTooltip ("The lowest note the harmonizer listens for");
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
        row.on = &addSwitch (params::HarmonizerParameters::voiceId (v, "on"), {});
        row.on->setTooltip ("Voice " + juce::String (v + 1) + " on or off");
        row.mode = &addCombo (params::HarmonizerParameters::voiceId (v, "mode"), { "Diatonic (steps)", "Chromatic (semitones)" });
        for (size_t k = 0; k < row.fields.size(); ++k)
            row.fields[k] = &addField (params::HarmonizerParameters::voiceId (v, what[k]), suffix[k]);
    }
    refresh();
}

void HarmonizerPage::describe (InspectorLayout& l)
{
    l.labelled ("Key", root);
    l.labelled ("Scale", scale);
    l.labelled ("Out of key", outOfKey);
    l.labelled ("Lowest note", floor);
    l.section ("Hearing");
    l.area (&hearingArea, 44);
    l.section ("Glide and level");
    l.knobs ({ glide, level });
    l.section ("Custom scale");
    l.buttons ({ customNotes[0], customNotes[1], customNotes[2], customNotes[3], customNotes[4], customNotes[5] }, 26);
    l.buttons ({ customNotes[6], customNotes[7], customNotes[8], customNotes[9], customNotes[10], customNotes[11] }, 26);
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        auto& r = rows[(size_t) v];
        l.section ("Voice " + juce::String (v + 1), r.on);
        l.labelled ("Interval", r.mode);
        l.fields ({ { "Steps", r.fields[0] }, { "Semitones", r.fields[1] }, { "Octave", r.fields[2] },
                    { "Level", r.fields[3] }, { "Pan", r.fields[4] }, { "Humanize", r.fields[5] } });
    }
}

void HarmonizerPage::paintContent (juce::Graphics& g)
{
    // What it hears now, in a box.
    g.setColour (background);
    g.fillRoundedRectangle (hearingArea.toFloat(), radiusControl);
    g.setColour (theme::text);
    g.setFont (font (Text::label));
    g.drawFittedText (hearing, hearingArea.reduced (10, 0), juce::Justification::centredLeft, 2, 0.9f);
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

namespace
{
/// The interval picker inside a voice's semitones field: a small chevron in ink-faint (ink on hover), no box, so
/// the fields stay quiet. A click opens the named intervals.
class ChevronButton final : public juce::Button
{
public:
    ChevronButton() : juce::Button ("Interval") { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        juce::Path chevron;
        chevron.startNewSubPath (c.x - 3.5f, c.y - 1.5f);
        chevron.lineTo (c.x, c.y + 2.0f);
        chevron.lineTo (c.x + 3.5f, c.y - 1.5f);
        g.setColour (over || down ? theme::ink : theme::inkFaint);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};
} // namespace

MultivoicerPage::MultivoicerPage (AmpSimProcessor& p)
    : BlockPage (p, BlockId::multivoicer, "Multivoicer", "Up to eight shifted, delayed, panned copies")
{
    engine = &addCombo ("mv_engine", { "Poly (chords)", "Mono (single notes)" });
    mixMode = &addCombo ("mv_mix_mode", { "Blend", "Add" });
    mixMode->setTooltip ("Blend: Mix trades the dry for the voices (equal power). Add: the dry stays whole and Mix sets the voices' level on top");
    highPass = &addSwitch ("mv_hp", "Wet high-pass");

    using Shape = params::MultivoicerParameters::Shape;
    const std::array<std::pair<Shape, const char*>, 4> shapeList { { { Shape::octaveUp, "One voice an octave up, added on top of the dry" },
                                                                     { Shape::octaveDown, "One voice an octave down, added on top of the dry" },
                                                                     { Shape::powerFifth, "A fifth (+7) and an octave (+12), added on top of the dry" },
                                                                     { Shape::octaveStack, "An octave down and an octave up, added on top of the dry" } } };
    for (size_t i = 0; i < shapes.size(); ++i)
    {
        const auto shape = shapeList[i].first;
        shapes[i] = &addButton (params::MultivoicerParameters::shapeName (shape), [this, shape]
        {
            beginUndoStep (state);
            params::MultivoicerParameters::applyShape (state, shape);
        });
        shapes[i]->setTooltip (juce::String (shapeList[i].second) + " (sets the voices, their delays to 0, and the mix to Add; one undo step)");
    }
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
        for (size_t k = 0; k < row.fields.size(); ++k)
            row.fields[k] = &addField (params::MultivoicerParameters::voiceId (v, what[k]), suffix[k]);
        row.picker = &adopt (std::make_unique<ChevronButton>());
        row.picker->setTooltip ("Pick an interval for voice " + juce::String (v + 1));
        row.picker->onClick = [this, v] { showMenu (intervalMenu (v), rows[(size_t) v].picker, &getLookAndFeel()); };
    }
    refresh();
}

juce::PopupMenu MultivoicerPage::intervalMenu (int voice)
{
    juce::PopupMenu menu;
    const auto current = indexOf (state, params::MultivoicerParameters::voiceId (voice, "semitones"));
    for (const auto& interval : params::MultivoicerParameters::namedIntervals())
    {
        const auto label = juce::String (interval.name) + "  (" + (interval.semitones > 0 ? "+" : "") + juce::String (interval.semitones) + ")";
        menu.addItem (label, true, current == interval.semitones, [this, voice, st = interval.semitones]
        {
            beginUndoStep (state);
            params::MultivoicerParameters::applyInterval (state, voice, st);
        });
    }
    return menu;
}

void MultivoicerPage::describe (InspectorLayout& l)
{
    l.labelled ("Engine", engine);
    l.labelled ("Mix mode", mixMode);
    l.section ("Shapes");
    l.buttons ({ shapes[0], shapes[1] });
    l.buttons ({ shapes[2], shapes[3] });
    l.buttons ({ startingPoints });
    l.section ("Ensemble");
    l.knobs ({ voices, mix, spread });
    l.switches ({ highPass });
    l.knobs ({ highPassHz });
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        auto& r = rows[(size_t) v];
        l.section ("Voice " + juce::String (v + 1));
        l.fields ({ { "Interval", r.fields[0] }, { "Fine", r.fields[1] }, { "Delay", r.fields[2] },
                    { "Pan", r.fields[3] }, { "Level", r.fields[4] }, { "Drift", r.fields[5] } });
        if (l.isPlacing())
        {
            // The chevron inside the interval's field, at its right end.
            auto cell = r.fields[0]->getBounds();
            r.picker->setBounds (cell.removeFromRight (18));
            r.fields[0]->setBounds (cell);
        }
    }
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
        for (auto* field : rows[(size_t) v].fields)
            field->setAlpha (alpha);
        rows[(size_t) v].picker->setAlpha (alpha);
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
            arrow.startNewSubPath (x - 4.0f, y);
            arrow.lineTo (x + 3.0f, y);
            arrow.startNewSubPath (x, y - 3.0f);
            arrow.lineTo (x + 3.0f, y);
            arrow.lineTo (x, y + 3.0f);
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
        g.setColour (lifted ? accent : outline);
        g.drawRoundedRectangle (r.reduced (0.5f), radiusControl, lifted ? 1.5f : 1.0f);
        const auto name = shown[index].substring (0, 1).toUpperCase() + shown[index].substring (1);
        g.setColour (theme::text);
        g.setFont (font (Text::label));
        g.drawFittedText (name, r.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    std::function<juce::StringArray()> getOrder;
    std::function<void (const juce::StringArray&)> setOrder;
    juce::StringArray shown;
    int dragging = -1, target = -1;
    float grab = 0.0f, dragX = 0.0f;
    static constexpr float gap = 14.0f;
};

BloomPage::BloomPage (AmpSimProcessor& p) : BlockPage (p, BlockId::bloom, "Bloom", "Three effects in one block, in the order shown")
{
    mix = &addField ("bloom_mix", " %");
    chips = &adopt (std::make_unique<OrderChips> ([&p] { return p.getBloomOrder(); }, [&p] (const juce::StringArray& order) { p.setBloomOrder (order); }));

    crush.title = "Bitcrush";
    crush.on = &addSwitch ("bloom_crush_on", {});
    crush.knobs = { &addKnob ("bloom_crush_bits", "Bits", " bits"), &addKnob ("bloom_crush_rate", "Rate", " Hz"),
                    &addKnob ("bloom_crush_tone", "Tone", " Hz"), &addKnob ("bloom_crush_mix", "Mix", " %") };
    crush.switches = { &addSwitch ("bloom_crush_dither", "Dither (hiss, not gated decays)") };

    phaser.title = "Phaser";
    phaser.on = &addSwitch ("bloom_phaser_on", {});
    phaser.combos = { { "Mode", &addCombo ("bloom_phaser_mode", { "Classic", "Modern", "Vibe" }) },
                      { "Stages", &addCombo ("bloom_phaser_stages", { "2 stages", "4 stages", "6 stages", "8 stages", "12 stages" }) },
                      { "Shape", &addCombo ("bloom_phaser_shape", { "Sine", "Triangle" }) } };
    phaser.switches = { &addSwitch ("bloom_phaser_sync", "Sync"), &addSwitch ("bloom_phaser_classic_fb", "Feedback (later version)") };
    phaser.rate = &addKnob ("bloom_phaser_rate", "Rate", " Hz");
    phaser.knobs = { phaser.rate, &addKnob ("bloom_phaser_depth", "Depth", " %"), &addKnob ("bloom_phaser_low", "Low", " Hz"),
                     &addKnob ("bloom_phaser_high", "High", " Hz"), &addKnob ("bloom_phaser_feedback", "Resonance", " %"),
                     &addKnob ("bloom_phaser_stereo", "Stereo", " deg"), &addKnob ("bloom_phaser_mix", "Mix", " %") };
    phaser.note = &addCombo ("bloom_phaser_note", noteNames());

    flanger.title = "Flanger";
    flanger.on = &addSwitch ("bloom_flanger_on", {});
    flanger.combos = { { "Shape", &addCombo ("bloom_flanger_shape", { "Triangle", "Sine", "Random" }) } };
    flanger.switches = { &addSwitch ("bloom_flanger_sync", "Sync"), &addSwitch ("bloom_flanger_negative", "Negative"),
                         &addSwitch ("bloom_flanger_tz", "Through-zero") };
    flanger.rate = &addKnob ("bloom_flanger_rate", "Rate", " Hz");
    flanger.knobs = { &addKnob ("bloom_flanger_manual", "Manual", " ms"), &addKnob ("bloom_flanger_depth", "Depth", " %"), flanger.rate,
                      &addKnob ("bloom_flanger_feedback", "Feedback", " %"), &addKnob ("bloom_flanger_stereo", "Stereo", " deg"),
                      &addKnob ("bloom_flanger_mix", "Mix", " %") };
    flanger.note = &addCombo ("bloom_flanger_note", noteNames());
    flanger.switches[2]->setTooltip ("Through-zero flanging: the dry signal is delayed 5 ms (reported to the host) while it's on");
    for (auto* e : { &crush, &phaser, &flanger })
        e->on->setTooltip (e->title + " on or off, inside Bloom");
    latency = &addLabel ({}, Text::label, warn);
    latency->setJustificationType (juce::Justification::topLeft);
    refresh();
}

void BloomPage::describe (InspectorLayout& l)
{
    l.labelled ("Mix", mix, 24);
    l.section ("Order");
    l.row (chips, 30);

    // The three effects in the order they run.
    std::map<juce::String, Effect*> byName { { "bitcrush", &crush }, { "phaser", &phaser }, { "flanger", &flanger } };
    for (const auto& name : shownOrder.isEmpty() ? ampSim.getBloomOrder() : shownOrder)
        if (auto* effect = byName[name])
        {
            l.section (effect->title, effect->on);
            for (const auto& [caption, combo] : effect->combos)
                l.labelled (caption, combo);
            l.switches (effect->switches);
            l.knobs (effect->knobs);
            l.inCell (effect->note, effect->rate);
        }
    l.row (latency, 32);
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
        for (auto* c : std::initializer_list<juce::Component*> { phaser.combos[1].second, phaser.combos[2].second, phaser.knobs[2], phaser.knobs[3], phaser.knobs[4] })
            dim (c, modern);
        dim (phaser.switches[1], mode == 0);
    }
    if (const auto sync = isOn (state, "bloom_phaser_sync") ? 1 : 0; sync != shownPhaserSync)
    {
        shownPhaserSync = sync;
        phaser.rate->setVisible (sync == 0);
        phaser.note->setVisible (sync == 1);
        resized(); // the note takes the rate's caption
    }
    if (const auto sync = isOn (state, "bloom_flanger_sync") ? 1 : 0; sync != shownFlangerSync)
    {
        shownFlangerSync = sync;
        flanger.rate->setVisible (sync == 0);
        flanger.note->setVisible (sync == 1);
        resized();
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
    highPass = &addSwitch ("chorus_hp", "Low-end protection");
    mode->setTooltip ("Classic: one voice. Dimension: a wide, gentle doubling that cancels in mono. Tri: three voices.");
    highPass->setTooltip ("Low-end protection: below the frequency only the dry signal passes, so the low strings stay solid");
    refresh();
}

void ChorusPage::describe (InspectorLayout& l)
{
    l.labelled ("Mode", mode);
    l.labelled ("Shape", shape);
    l.switches ({ sync });
    l.section ("Speed and depth");
    l.knobs ({ rate, depth });
    l.inCell (note, rate);
    l.section ("Blend");
    l.knobs ({ mix, width });
    l.section ("Low end");
    l.switches ({ highPass });
    l.knobs ({ highPassHz });
    l.section ("Character");
    l.switches ({ analog, noise });
}

void ChorusPage::refresh()
{
    const auto synced = isOn (state, "chorus_sync") ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    note->setVisible (synced == 1);
    rate->setVisible (synced == 0);
    resized(); // the note takes the rate's caption
}

// ---- DelayPage -------------------------------------------------------------------------------------------

DelayPage::DelayPage (AmpSimProcessor& p) : BlockPage (p, BlockId::delay, "Delay", "After the cab; repeats ring on when switched off")
{
    mode = &addCombo ("delay_mode", { "Digital", "Analog", "Tape" });
    stereo = &addCombo ("delay_stereo", { "Stereo", "Ping-pong", "Dual" });
    sync = &addSwitch ("delay_sync", "Sync to tempo");
    note = &addCombo ("delay_note", noteNames());
    rightNote = &addCombo ("delay_note_right", noteNames());
    note->setTooltip ("The time, as a note at the tempo");
    rightNote->setTooltip ("The right side's time, as a note at the tempo");

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

void DelayPage::describe (InspectorLayout& l)
{
    l.labelled ("Mode", mode);
    l.labelled ("Stereo", stereo);
    l.switches ({ sync });
    l.section ("Time");
    l.knobs ({ time, rightTime, feedback });
    // The synced times sit where the time knobs are; the offset shares the right time's place.
    if (l.isPlacing())
        offset->setBounds (rightTime->getBounds());
    l.inCell (note, time);
    l.inCell (rightNote, rightTime);
    l.section ("Mix and ducking");
    l.knobs ({ mix, duck });
    l.section ("Tone of the repeats");
    l.knobs ({ lowCut, highCut });
    l.section ("Modulation");
    l.knobs ({ modDepth, modRate });
    l.section ("Tempo, for every synced effect");
    l.knobs ({ tempo });
    if (l.isPlacing())
        tap->setBounds (tempo->getBounds().translated (tempo->getWidth() + 8, 0).withSizeKeepingCentre (72, controlHeight));
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
    time->setVisible (! synced);
    const bool dual = layout == 2;
    rightNote->setVisible (synced && dual);
    rightTime->setVisible (! synced && dual);
    offset->setVisible (layout == 0);
    resized(); // the notes take the times' captions
}

// ---- ReverbPage ------------------------------------------------------------------------------------------

ReverbPage::ReverbPage (AmpSimProcessor& p) : BlockPage (p, BlockId::reverb, "Reverb", "After the cab; the tail rings on when switched off")
{
    engine = &addCombo ("reverb_engine", { "Room", "Hall", "Plate" });
    freeze = &addSwitch ("reverb_freeze", "Freeze");
    preDelaySync = &addSwitch ("reverb_predelay_sync", "Sync pre-delay");
    preDelayNote = &addCombo ("reverb_predelay_note", noteNames());
    preDelayNote->setTooltip ("The pre-delay, as a note at the tempo");
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

void ReverbPage::describe (InspectorLayout& l)
{
    l.labelled ("Engine", engine);
    l.switches ({ freeze, preDelaySync });
    l.section ("Space");
    l.knobs ({ mix, decay, size, preDelay, earlyLate, diffusion, width });
    l.inCell (preDelayNote, preDelay);
    l.section ("Tone");
    l.knobs ({ lowDecay, highDecay, lowCut, highCut });
    l.section ("Motion");
    l.knobs ({ modDepth, modRate, ducking });
    l.section ("Shimmer (Room and Hall)");
    l.knobs ({ shimmer });
    l.labelled ("Interval", shimmerInterval);
}

void ReverbPage::refresh()
{
    const auto synced = isOn (state, "reverb_predelay_sync") ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    preDelayNote->setVisible (synced == 1);
    preDelay->setVisible (synced == 0);
    resized(); // the note takes the pre-delay's caption
}

} // namespace ui

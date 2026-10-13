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

juce::String signedInt (float v)
{
    const auto i = juce::roundToInt (v);
    return (i > 0 ? "+" : "") + juce::String (i);
}

juce::String signedOne (float v)
{
    const auto r = std::round (v * 10.0f) / 10.0f;
    return (r > 0.0f ? "+" : "") + juce::String (juce::exactlyEqual (r, 0.0f) ? 0.0f : r, 1);
}

juce::String panText (float v)
{
    const auto i = juce::roundToInt (v);
    return i == 0 ? juce::String ("C") : (i < 0 ? "L" : "R") + juce::String (std::abs (i));
}

/// A table's column captions (10.5 px medium, ink-faint), each over its column.
void paintCaptions (juce::Graphics& g, juce::Rectangle<int> row, const std::vector<std::pair<int, int>>& columns, const juce::StringArray& captions,
                    juce::Justification just = juce::Justification::centredLeft)
{
    g.setFont (geist (Weight::medium, 10.5f));
    g.setColour (inkFaint);
    for (size_t i = 0; i < columns.size() && (int) i < captions.size(); ++i)
        g.drawText (captions[(int) i], juce::Rectangle<int> (row.getX() + columns[i].first, row.getY(), columns[i].second, row.getHeight()), just, false);
}
} // namespace

// ---- HarmonizerPage --------------------------------------------------------------------------------------

namespace
{
// The voice table's columns (x from the column's left, width), for a 259 px column: line 1 the switch, the interval
// mode, steps, semitones, octave; line 2 (under the mode) level, pan, humanize.
constexpr int voiceLead = 32;
const std::vector<std::pair<int, int>> harmonyLine1 { { voiceLead, 82 }, { 118, 44 }, { 166, 44 }, { 214, 45 } };
const std::vector<std::pair<int, int>> harmonyLine2 { { voiceLead, 76 }, { 112, 68 }, { 184, 75 } };
constexpr int voiceLineHeight = 24, voiceLineGap = 4;
} // namespace

HarmonizerPage::HarmonizerPage (AmpSimProcessor& p)
    : BlockPage (p, BlockId::harmonizer, "Harmonizer", "Harmonies in the key, after the amp and cab; it reads the pitch from the clean DI")
{
    hearingView = &adopt (std::make_unique<HearingView>());
    root = &addChoice ("harm_root", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    root->setColumns (6);
    root->setTooltip ("The key the harmonies follow");
    juce::StringArray scales;
    for (const auto* name : ampsim::harmony::scaleNames)
        scales.add (name);
    scale = &addCombo ("harm_scale", scales);
    outOfKey = &addChoice ("harm_out_of_key", { "Parallel", "Snap to key" });
    outOfKey->setOptionTooltips ({ "A note outside the key shifts in parallel", "A note outside the key snaps to the key" });
    floor = &addChoice ("harm_floor", { "A 110", "E 80", "Drop 60" });
    floor->setTooltip ("The lowest note the harmonizer listens for (Hz): A, the low E, or a drop tuning");
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
        row.mode = &addChoice (params::HarmonizerParameters::voiceId (v, "mode"), { "Steps", "Semis" });
        row.mode->setOptionTooltips ({ "Diatonic: the interval in steps of the scale", "Chromatic: the interval in semitones" });
        for (size_t k = 0; k < row.fields.size(); ++k)
            row.fields[k] = &addField (params::HarmonizerParameters::voiceId (v, what[k]), suffix[k]);
        row.fields[0]->setFormatter (signedInt);
        row.fields[1]->setFormatter (signedInt);
        row.fields[2]->setFormatter (signedInt);
        row.fields[3]->setFormatter ([] (float db) { return signedOne (db) + " dB"; });
        row.fields[4]->setFormatter (panText);
        row.fields[5]->setFormatter ([] (float ms) { return juce::String (juce::roundToInt (ms)) + " ms"; });
        for (auto* f : { row.fields[0], row.fields[1], row.fields[2] })
            f->setShowsBar (false);
        const auto n = juce::String (v + 1);
        row.fields[0]->setTooltip ("Voice " + n + "'s interval in steps of the scale (a diatonic voice)");
        row.fields[1]->setTooltip ("Voice " + n + "'s interval in semitones (a chromatic voice)");
        row.fields[2]->setTooltip ("Voice " + n + "'s octaves up or down, on top of its interval");
        row.fields[5]->setTooltip ("Voice " + n + "'s humanize: a little random timing, like a second player");
    }
    refresh();
}

void HarmonizerPage::describe (InspectorLayout& l)
{
    l.row (hearingView, HearingView::preferredHeight);

    l.section ("Key and scale");
    l.choice (root);
    l.labelled ("Scale", scale);
    l.buttons ({ customNotes[0], customNotes[1], customNotes[2], customNotes[3], customNotes[4], customNotes[5] }, 24);
    l.buttons ({ customNotes[6], customNotes[7], customNotes[8], customNotes[9], customNotes[10], customNotes[11] }, 24);

    // The voices: the captions of both lines, then each voice's two lines.
    l.section ("Voices");
    l.area (&captionRows[0], 12);
    captionRows[1] = {};
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        juce::Rectangle<int> top, bottom;
        l.area (&top, voiceLineHeight);
        l.gap (voiceLineGap - InspectorLayout::rowGap);
        l.area (&bottom, voiceLineHeight);
        if (v == 0)
            captionRows[1] = bottom.withHeight (0);
        if (! l.isPlacing())
            continue;
        auto& r = rows[(size_t) v];
        voiceRows[(size_t) v] = top.getUnion (bottom);
        r.on->setBounds (juce::Rectangle<int> (r.on->getPreferredWidth(), Switch::preferredHeight).withPosition (top.getX() - Switch::margin, top.getCentreY() - Switch::preferredHeight / 2));
        const auto cell = [] (juce::Rectangle<int> line, std::pair<int, int> c) { return juce::Rectangle<int> (line.getX() + c.first, line.getY(), c.second, line.getHeight()); };
        r.mode->setBounds (cell (top, harmonyLine1[0]));
        r.fields[0]->setBounds (cell (top, harmonyLine1[1]));
        r.fields[1]->setBounds (cell (top, harmonyLine1[2]));
        r.fields[2]->setBounds (cell (top, harmonyLine1[3]));
        r.fields[3]->setBounds (cell (bottom, harmonyLine2[0]));
        r.fields[4]->setBounds (cell (bottom, harmonyLine2[1]));
        r.fields[5]->setBounds (cell (bottom, harmonyLine2[2]));
    }

    l.section ("Behaviour");
    l.choice (outOfKey, "Out of key");
    l.choice (floor, "Lowest note");
    l.knobs ({ glide, level });
}

void HarmonizerPage::paintContent (juce::Graphics& g)
{
    // The table's captions: line 1's over the first row; line 2's are carried by the fields' own units, so its
    // captions sit at the column's left under the voice's number.
    paintCaptions (g, captionRows[0], harmonyLine1, { "Interval in", "Steps", "Semis", "Octave" });

    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        const auto row = voiceRows[(size_t) v];
        if (row.isEmpty())
            continue;
        // The voice's number under its switch, orange while it's on; a hairline between voices.
        const auto on = rows[(size_t) v].on->getToggleState();
        g.setFont (geist (Weight::semibold, 12.0f));
        g.setColour (on ? accent : inkFaint);
        g.drawText (juce::String (v + 1), juce::Rectangle<int> (row.getX(), row.getBottom() - voiceLineHeight, 26, voiceLineHeight), juce::Justification::centred, false);
        if (v > 0)
        {
            g.setColour (line1);
            g.fillRect (row.getX(), row.getY() - 6, row.getWidth(), 1);
        }
    }
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

    // Diatonic voices use steps, chromatic ones semitones: dim the other. A voice that's off dims whole.
    for (int v = 0; v < ampsim::Harmonizer::maxVoices; ++v)
    {
        auto& r = rows[(size_t) v];
        const auto diatonic = indexOf (state, params::HarmonizerParameters::voiceId (v, "mode")) == 0;
        const auto base = r.on->getToggleState() ? 1.0f : offAlpha;
        r.mode->setAlpha (base);
        r.fields[0]->setAlpha (base * (diatonic ? 1.0f : unusedAlpha));
        r.fields[1]->setAlpha (base * (diatonic ? unusedAlpha : 1.0f));
        for (size_t k = 2; k < r.fields.size(); ++k)
            r.fields[k]->setAlpha (base);
    }
    repaint();

    HearingView::Reading heard;
    heard.note = ampSim.getHarmonizerNote();
    for (int v = 0; v < ampsim::Harmonizer::maxVoices && v < 4; ++v)
    {
        const auto shift = ampSim.getHarmonizerShift (v);
        heard.sounding[(size_t) v] = heard.note >= 0 && shift != ampsim::Harmonizer::shownSilent;
        heard.shifts[(size_t) v] = heard.sounding[(size_t) v] ? shift : 0;
    }
    hearingView->set (heard);
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

// The voice table's columns for a 259 px column: the number, interval (with its picker), cents, delay, pan, level, drift.
const std::vector<std::pair<int, int>> voiceColumns { { 0, 14 }, { 16, 50 }, { 68, 35 }, { 105, 39 }, { 146, 34 }, { 182, 42 }, { 226, 33 } };
constexpr int voiceRowHeight = 24, voiceRowGap = 4;
} // namespace

MultivoicerPage::MultivoicerPage (AmpSimProcessor& p)
    : BlockPage (p, BlockId::multivoicer, "Multivoicer", "Up to eight shifted, delayed, panned copies")
{
    engine = &addChoice ("mv_engine", { "Poly", "Mono" });
    engine->setOptionTooltips ({ "Poly: shifts chords as well as single notes", "Mono: tuned for single notes, tighter tracking" });
    mixMode = &addChoice ("mv_mix_mode", { "Blend", "Add" });
    mixMode->setOptionTooltips ({ "Blend: Mix trades the dry for the voices (equal power)", "Add: the dry stays whole and Mix sets the voices' level on top" });
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
    mix = &addKnob ("mv_mix", "Mix", " %", Knob::Size::large);
    spread = &addKnob ("mv_spread", "Spread", " %", Knob::Size::large);
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
        // The table's columns carry the units in their captions; the cells hold the numbers.
        row.fields[0]->setFormatter (signedInt);
        row.fields[1]->setFormatter (signedOne);
        row.fields[2]->setFormatter ([] (float ms) { return juce::String (ms, ms < 100.0f ? 1 : 0); });
        row.fields[3]->setFormatter (panText);
        row.fields[4]->setFormatter (signedOne);
        row.fields[5]->setFormatter ([] (float d) { return juce::String (juce::roundToInt (d)); });
        row.fields[0]->setShowsBar (false);
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
    l.choice (engine, "Engine");
    l.choice (mixMode, "Mix mode");
    l.hero ({ mix, spread });
    l.section ("Shapes");
    l.buttons ({ shapes[0], shapes[1] });
    l.buttons ({ shapes[2], shapes[3] });
    l.buttons ({ startingPoints });
    l.section ("Ensemble", highPass);
    l.knobs ({ voices, highPassHz });

    // The voices: a caption row, then a row each.
    l.section ("Voices");
    l.area (&captionRow, 12);
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        juce::Rectangle<int> line;
        l.area (&line, voiceRowHeight);
        l.gap (voiceRowGap - InspectorLayout::rowGap);
        if (! l.isPlacing())
            continue;
        voiceRows[(size_t) v] = line;
        auto& r = rows[(size_t) v];
        for (size_t k = 0; k < r.fields.size(); ++k)
        {
            const auto c = voiceColumns[k + 1];
            r.fields[k]->setBounds (line.getX() + c.first, line.getY(), c.second, line.getHeight());
        }
        // The chevron inside the interval's field, at its right end.
        auto cell = r.fields[0]->getBounds();
        r.picker->setBounds (cell.removeFromRight (16));
        r.fields[0]->setBounds (cell);
    }
}

void MultivoicerPage::paintContent (juce::Graphics& g)
{
    paintCaptions (g, captionRow, voiceColumns, { "", "Interval", "Cents", "Delay", "Pan", "Level", "Drift" });
    for (int v = 0; v < ampsim::Multivoicer::maxVoices; ++v)
    {
        const auto row = voiceRows[(size_t) v];
        if (row.isEmpty())
            continue;
        g.setFont (geist (Weight::semibold, 12.0f));
        g.setColour (v < shownVoices ? accent : inkFaint);
        g.drawText (juce::String (v + 1), row.withWidth (voiceColumns[0].second), juce::Justification::centredLeft, false);
    }
}

void MultivoicerPage::refresh()
{
    if (const auto hp = isOn (state, "mv_hp") ? 1 : 0; hp != shownHighPass)
    {
        shownHighPass = hp;
        dim (highPassHz, hp == 1);
    }
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
    repaint();
}

// ---- BloomPage -------------------------------------------------------------------------------------------

/// The order of Bloom's three effects, as chips with arrows between them. Drag a chip along the row to
/// move it; the order is saved by effect name like the sections' (AmpSimProcessor::setBloomOrder).
class BloomPage::OrderChips final : public juce::Component, public juce::SettableTooltipClient
{
public:
    OrderChips (std::function<juce::StringArray()> getter, std::function<void (const juce::StringArray&)> setter, std::function<bool (const juce::String&)> onGetter)
        : getOrder (std::move (getter)), setOrder (std::move (setter)), isEffectOn (std::move (onGetter))
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
        juce::String lit;
        for (const auto& name : shown)
            lit << (isEffectOn (name) ? "1" : "0");
        if (lit != shownLit)
        {
            shownLit = lit;
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

        g.setColour (inkFaint);
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
        // A chip: raised, its order number and name, and an LED for whether that effect is on.
        const auto on = isEffectOn (shown[index]);
        g.setColour (surfaceRaised);
        g.fillRoundedRectangle (r, radiusControl);
        g.setColour (lifted ? accent : line2);
        g.drawRoundedRectangle (r.reduced (0.5f), radiusControl, lifted ? 1.5f : 1.0f);
        const auto led = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getX() + 11.0f, r.getCentreY() });
        g.setColour (on ? accent : line2.brighter (0.1f));
        g.fillEllipse (led);
        const auto name = shown[index].substring (0, 1).toUpperCase() + shown[index].substring (1);
        g.setColour (on ? ink : inkDim);
        g.setFont (geist (Weight::medium, 12.0f));
        g.drawFittedText (name, r.withTrimmedLeft (18.0f).reduced (2.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    std::function<juce::StringArray()> getOrder;
    std::function<void (const juce::StringArray&)> setOrder;
    std::function<bool (const juce::String&)> isEffectOn;
    juce::StringArray shown;
    juce::String shownLit;
    int dragging = -1, target = -1;
    float grab = 0.0f, dragX = 0.0f;
    static constexpr float gap = 14.0f;
};

BloomPage::BloomPage (AmpSimProcessor& p) : BlockPage (p, BlockId::bloom, "Bloom", "Three effects in one block, in the order shown")
{
    mix = &addKnob ("bloom_mix", "Mix", " %", Knob::Size::large);
    const auto onIdOf = [] (const juce::String& name) { return name == "bitcrush" ? juce::String ("bloom_crush_on") : "bloom_" + name + "_on"; };
    chips = &adopt (std::make_unique<OrderChips> ([&p] { return p.getBloomOrder(); }, [&p] (const juce::StringArray& order) { p.setBloomOrder (order); },
                                                  [this, onIdOf] (const juce::String& name)
                                                  {
                                                      auto* v = state.getRawParameterValue (onIdOf (name));
                                                      return v != nullptr && v->load() >= 0.5f;
                                                  }));

    crush.title = "Bitcrush";
    crush.on = &addSwitch ("bloom_crush_on", {});
    crush.knobs = { &addKnob ("bloom_crush_bits", "Bits", " bits"), &addKnob ("bloom_crush_rate", "Rate", " Hz"),
                    &addKnob ("bloom_crush_tone", "Tone", " Hz"), &addKnob ("bloom_crush_mix", "Mix", " %") };
    crush.switches = { &addSwitch ("bloom_crush_dither", "Dither (hiss, not gated decays)") };

    phaser.title = "Phaser";
    phaser.on = &addSwitch ("bloom_phaser_on", {});
    phaser.choices = { { "Mode", &addChoice ("bloom_phaser_mode", { "Classic", "Modern", "Vibe" }) },
                       { "Stages", &addChoice ("bloom_phaser_stages", { "2", "4", "6", "8", "12" }) },
                       { "Shape", &addChoice ("bloom_phaser_shape", { "Sine", "Triangle" }) } };
    phaser.choices[1].second->setTooltip ("The number of all-pass stages (Modern only)");
    phaser.switches = { &addSwitch ("bloom_phaser_sync", "Sync"), &addSwitch ("bloom_phaser_classic_fb", "Feedback (later version)") };
    phaser.rate = &addKnob ("bloom_phaser_rate", "Rate", " Hz");
    phaser.knobs = { phaser.rate, &addKnob ("bloom_phaser_depth", "Depth", " %"), &addKnob ("bloom_phaser_mix", "Mix", " %"),
                     &addKnob ("bloom_phaser_low", "Low", " Hz"), &addKnob ("bloom_phaser_high", "High", " Hz"),
                     &addKnob ("bloom_phaser_feedback", "Resonance", " %"), &addKnob ("bloom_phaser_stereo", "Stereo", " deg") };
    phaser.note = &addCombo ("bloom_phaser_note", noteNames());

    flanger.title = "Flanger";
    flanger.on = &addSwitch ("bloom_flanger_on", {});
    flanger.choices = { { "Shape", &addChoice ("bloom_flanger_shape", { "Triangle", "Sine", "Random" }) } };
    flanger.switches = { &addSwitch ("bloom_flanger_sync", "Sync"), &addSwitch ("bloom_flanger_negative", "Negative"),
                         &addSwitch ("bloom_flanger_tz", "Through-zero") };
    flanger.rate = &addKnob ("bloom_flanger_rate", "Rate", " Hz");
    flanger.knobs = { flanger.rate, &addKnob ("bloom_flanger_depth", "Depth", " %"), &addKnob ("bloom_flanger_mix", "Mix", " %"),
                      &addKnob ("bloom_flanger_manual", "Manual", " ms"), &addKnob ("bloom_flanger_feedback", "Feedback", " %"),
                      &addKnob ("bloom_flanger_stereo", "Stereo", " deg") };
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
    l.section ("Order");
    l.row (chips, 32);
    l.hero ({ mix, nullptr }, latency, 40);

    // The three effects in the order they run.
    std::map<juce::String, Effect*> byName { { "bitcrush", &crush }, { "phaser", &phaser }, { "flanger", &flanger } };
    for (const auto& name : shownOrder.isEmpty() ? ampSim.getBloomOrder() : shownOrder)
        if (auto* effect = byName[name])
        {
            l.section (effect->title, effect->on);
            for (const auto& [caption, bar] : effect->choices)
                l.choice (bar, caption);
            l.knobs (effect->knobs);
            l.inCell (effect->note, effect->rate);
            l.switches (effect->switches);
        }
}

void BloomPage::dimEffect (Effect& e, float alpha)
{
    for (const auto& [caption, bar] : e.choices)
        bar->setAlpha (alpha);
    for (auto* s : e.switches)
        s->setAlpha (alpha);
    for (auto* k : e.knobs)
        k->setAlpha (alpha);
    if (e.note != nullptr)
        e.note->setAlpha (alpha);
}

void BloomPage::refresh()
{
    chips->refresh();
    if (const auto order = ampSim.getBloomOrder(); order != shownOrder)
    {
        shownOrder = order;
        resized();
    }

    // Each effect's controls at full strength while it's on, dimmed while it's off; then what each mode doesn't use.
    // Classic: fixed 4 stages, triangle, its own range, optional feedback. Modern: everything. Vibe: its own stages,
    // range, and sine.
    const auto alphaOf = [this] (const char* id) { return isOn (state, id) ? 1.0f : offAlpha; };
    const auto crushAlpha = alphaOf ("bloom_crush_on"), phaserAlpha = alphaOf ("bloom_phaser_on"), flangerAlpha = alphaOf ("bloom_flanger_on");
    dimEffect (crush, crushAlpha);
    dimEffect (phaser, phaserAlpha);
    dimEffect (flanger, flangerAlpha);
    const auto mode = indexOf (state, "bloom_phaser_mode");
    const auto modern = mode == 1;
    for (auto* c : std::initializer_list<juce::Component*> { phaser.choices[1].second, phaser.choices[2].second, phaser.knobs[3], phaser.knobs[4], phaser.knobs[5] })
        c->setAlpha (phaserAlpha * (modern ? 1.0f : unusedAlpha));
    phaser.switches[1]->setAlpha (phaserAlpha * (mode == 0 ? 1.0f : unusedAlpha));
    shownPhaserMode = mode;

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
    mode = &addChoice ("chorus_mode", { "Classic", "Dimension", "Tri" });
    mode->setOptionTooltips ({ "Classic: one voice", "Dimension: a wide, gentle doubling that cancels in mono", "Tri: three voices" });
    shape = &addChoice ("chorus_shape", { "Triangle", "Sine", "Random" });
    sync = &addSwitch ("chorus_sync", "Sync to tempo");
    note = &addCombo ("chorus_note", noteNames());
    rate = &addKnob ("chorus_rate", "Rate", " Hz", Knob::Size::large);
    depth = &addKnob ("chorus_depth", "Depth", " %", Knob::Size::large);
    mix = &addKnob ("chorus_mix", "Mix", " %");
    width = &addKnob ("chorus_width", "Width", " %");
    highPassHz = &addKnob ("chorus_hp_freq", "Below", " Hz");
    analog = &addSwitch ("chorus_analog", "Analog");
    noise = &addSwitch ("chorus_noise", "Noise");
    highPass = &addSwitch ("chorus_hp", "Protect");
    highPass->setTooltip ("Low-end protection: below the frequency only the dry signal passes, so the low strings stay solid");
    analog->setTooltip ("The bucket-brigade's soft, slightly dark repeats");
    noise->setTooltip ("A little of an old chip's hiss");
    refresh();
}

void ChorusPage::describe (InspectorLayout& l)
{
    l.choice (mode);
    l.hero ({ rate, depth });
    l.inCell (note, rate);
    l.switches ({ sync });
    l.section ("Blend");
    l.knobs ({ mix, width });
    l.choice (shape, "Shape");
    l.section ("Low end", highPass);
    l.knobs ({ highPassHz });
    l.section ("Character");
    l.switches ({ analog, noise });
}

void ChorusPage::refresh()
{
    if (const auto hp = isOn (state, "chorus_hp") ? 1 : 0; hp != shownHighPass)
    {
        shownHighPass = hp;
        dim (highPassHz, hp == 1);
    }
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
    mode = &addChoice ("delay_mode", { "Digital", "Analog", "Tape" });
    mode->setOptionTooltips ({ "Digital: clean repeats", "Analog: darker with each repeat", "Tape: wow, flutter, and saturation" });
    stereo = &addChoice ("delay_stereo", { "Stereo", "Ping-pong", "Dual" });
    stereo->setOptionTooltips ({ "Stereo: the right repeats offset a little", "Ping-pong: repeats alternate sides", "Dual: a time per side" });
    sync = &addSwitch ("delay_sync", "Sync");
    sync->setTooltip ("Sync the time to the tempo, as a note");
    note = &addCombo ("delay_note", noteNames());
    rightNote = &addCombo ("delay_note_right", noteNames());
    note->setTooltip ("The time, as a note at the tempo");
    rightNote->setTooltip ("The right side's time, as a note at the tempo");

    time = &addKnob ("delay_time", "Time", " ms", Knob::Size::large);
    feedback = &addKnob ("delay_feedback", "Feedback", " %", Knob::Size::large);
    mix = &addKnob ("delay_mix", "Mix", " %", Knob::Size::large);
    rightTime = &addKnob ("delay_time_right", "Right time", " ms");
    offset = &addKnob ("delay_offset", "R offset", " ms");
    offset->setValueShown (true); // placed in the right time's cell, not by the layout's knobs() (RD4-7)
    lowCut = &addKnob ("delay_lowcut", "Low cut", " Hz");
    highCut = &addKnob ("delay_highcut", "High cut", " Hz");
    modDepth = &addKnob ("delay_mod_depth", "Depth", " ms");
    modRate = &addKnob ("delay_mod_rate", "Rate", " Hz");
    duck = &addKnob ("delay_duck", "Ducking");
    duck->setTooltip ("How far the repeats duck while you play");
    tempo = &addField ("tempo_bpm", " BPM");
    tempo->setShowsBar (false);
    tempo->setFormatter ([] (float bpm) { return juce::String (bpm, 1) + " BPM"; });
    tempo->setTooltip ("The tempo, for every synced effect: drag, or double-click to type");
    tap = &adopt (std::make_unique<TapPad>());
    tap->onClick = [this] { ampSim.tapTempo(); };
    refresh();
}

DelayPage::~DelayPage()
{
    stopTimer();
}

void DelayPage::describe (InspectorLayout& l)
{
    l.choice (mode);

    // The time large on the left; Tap, the tempo, and Sync stacked on the right.
    juce::Rectangle<int> row;
    l.area (&row, Knob::preferredHeight (Knob::Size::large));
    if (l.isPlacing())
    {
        auto left = row.removeFromLeft (row.getWidth() / 2);
        time->setBounds (left.withSizeKeepingCentre (Knob::preferredWidth (Knob::Size::large), Knob::preferredHeight (Knob::Size::large)));
        auto right = row.reduced (6, 2);
        tap->setBounds (right.removeFromTop (54));
        right.removeFromTop (6);
        tempo->setBounds (right.removeFromTop (24));
        right.removeFromTop (6);
        sync->setBounds (right.getX() - Switch::margin, right.getY(), sync->getPreferredWidth(), Switch::preferredHeight);
    }
    l.inCell (note, time);
    l.hero ({ feedback, mix });

    l.section ("Stereo");
    l.choice (stereo);
    l.knobs ({ rightTime, duck });
    // The offset shares the right time's place (Stereo has an offset, Dual a right time, Ping-pong neither).
    if (l.isPlacing())
        offset->setBounds (rightTime->getBounds());
    l.inCell (rightNote, rightTime);
    l.section ("Tone of the repeats");
    l.knobs ({ lowCut, highCut });
    l.section ("Modulation");
    l.knobs ({ modDepth, modRate });
}

void DelayPage::pageShown()
{
    tap->setTempo (ampSim.getTempo());
    startTimerHz (10);
}

void DelayPage::pageHidden()
{
    stopTimer();
}

void DelayPage::timerCallback()
{
    tap->setTempo (ampSim.getTempo());
    if (const auto shown = getSyncedTimeText(); shown != shownSyncedTime)
    {
        shownSyncedTime = shown;
        repaint (time->getBounds());
    }
}

juce::String DelayPage::getSyncedTimeText() const
{
    if (! isOn (state, "delay_sync"))
        return {};
    const auto& n = ampsim::tempo::notes[(size_t) juce::jlimit (0, (int) ampsim::tempo::notes.size() - 1, indexOf (state, "delay_note"))];
    return juce::String (juce::roundToInt (ampsim::tempo::milliseconds (n.division, n.feel, ampSim.getTempo()))) + " ms";
}

void DelayPage::paintContent (juce::Graphics& g)
{
    // Synced, the note takes the time knob's place; its length at the tempo sits where the knob's value would.
    if (const auto ms = getSyncedTimeText(); ms.isNotEmpty())
    {
        const auto cell = time->getBounds();
        const auto y = cell.getY() + Knob::margin + juce::roundToInt (Knob::hitSize (Knob::Size::large)) + 6 + 17;
        g.setFont (tabular (geist (Weight::medium, 14.0f)));
        g.setColour (ink);
        g.drawText (ms, juce::Rectangle<int> (cell.getX(), y, cell.getWidth(), 18), juce::Justification::centred, false);
    }
}

void DelayPage::refresh()
{
    tap->setTempo (ampSim.getTempo());
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
    engine = &addChoice ("reverb_engine", { "Room", "Hall", "Plate" });
    engine->setOptionTooltips ({ "Room: close, early reflections", "Hall: big and smooth", "Plate: bright and dense, no shimmer" });
    freeze = &addPad ("reverb_freeze", "Freeze", "Holds the tail forever");
    freeze->setTooltip ("Holds the tail forever (also on the footswitch's freeze CC)");
    preDelaySync = &addSwitch ("reverb_predelay_sync", "Sync");
    preDelaySync->setTooltip ("Sync the pre-delay to the tempo, as a note");
    preDelayNote = &addCombo ("reverb_predelay_note", noteNames());
    preDelayNote->setTooltip ("The pre-delay, as a note at the tempo");
    mix = &addKnob ("reverb_mix", "Mix", " %", Knob::Size::large);
    decay = &addKnob ("reverb_decay", "Decay", " s", Knob::Size::large);
    preDelay = &addKnob ("reverb_predelay", "Pre-delay", " ms");
    size = &addKnob ("reverb_size", "Size", " %");
    earlyLate = &addKnob ("reverb_early_late", "Early/late", " %");
    diffusion = &addKnob ("reverb_diffusion", "Diffusion", " %");
    width = &addKnob ("reverb_width", "Width", " %");
    lowDecay = &addKnob ("reverb_low_decay", "Low decay", " x");
    highDecay = &addKnob ("reverb_high_decay", "High decay", " x");
    lowCut = &addKnob ("reverb_lowcut", "Low cut", " Hz");
    highCut = &addKnob ("reverb_highcut", "High cut", " Hz");
    modDepth = &addKnob ("reverb_mod_depth", "Depth", " %");
    modRate = &addKnob ("reverb_mod_rate", "Rate", " Hz");
    ducking = &addKnob ("reverb_ducking", "Ducking", " %");
    shimmer = &addKnob ("reverb_shimmer", "Shimmer", " %");
    shimmerInterval = &addChoice ("reverb_shimmer_interval", { "+12", "+7", "+19", "+24" });
    shimmerInterval->setTooltip ("The shimmer's interval in semitones (Room and Hall)");
    refresh();
}

void ReverbPage::describe (InspectorLayout& l)
{
    l.choice (engine);
    l.hero ({ mix, decay });
    l.row (freeze, PadSwitch::preferredPadHeight);
    l.section ("Space", preDelaySync);
    l.knobs ({ size, preDelay, earlyLate, diffusion, width });
    l.inCell (preDelayNote, preDelay);
    l.section ("Damping");
    l.knobs ({ lowDecay, highDecay });
    l.section ("Cuts");
    l.knobs ({ lowCut, highCut });
    l.section ("Motion");
    l.knobs ({ modDepth, modRate, ducking });
    l.section ("Shimmer, Room and Hall");
    l.knobs ({ shimmer });
    l.choice (shimmerInterval, "Interval");
}

void ReverbPage::refresh()
{
    // The plate has no shimmer: its controls dim there.
    if (const auto e = indexOf (state, "reverb_engine"); e != shownEngine)
    {
        shownEngine = e;
        dim (shimmer, e != 2);
        dim (shimmerInterval, e != 2);
    }
    const auto synced = isOn (state, "reverb_predelay_sync") ? 1 : 0;
    if (synced == shownSync)
        return;
    shownSync = synced;
    preDelayNote->setVisible (synced == 1);
    preDelay->setVisible (synced == 0);
    resized(); // the note takes the pre-delay's caption
}

} // namespace ui

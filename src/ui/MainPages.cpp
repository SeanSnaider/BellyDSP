// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "MainPages.h"

namespace ui
{

using namespace theme;

namespace
{
/// The tab's name for a block: the gate tab holds both gates.
juce::String tabName (BlockId id)
{
    return id == BlockId::gateA ? juce::String ("Gate") : juce::String (info (id).name);
}

ampsim::Chain::Section chainSection (Section section)
{
    return section == Section::pre ? ampsim::Chain::Section::pre : ampsim::Chain::Section::post;
}
} // namespace

void paintPageHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle)
{
    const auto titleFont = font (Text::title);
    g.setFont (titleFont);
    g.setColour (ink);
    const auto w = juce::roundToInt (textWidth (titleFont, title)) + 2;
    g.drawText (title, area.removeFromLeft (w), juce::Justification::centredLeft, false);
    area.removeFromLeft (space::m);
    g.setFont (font (Text::label));
    g.setColour (inkFaint);
    g.drawText (subtitle, area, juce::Justification::centredLeft, true);
}

// ---- SectionPage -------------------------------------------------------------------------------------

SectionPage::SectionPage (AmpSimProcessor& p, Section s, std::function<BlockPage& (BlockId)> lookUp)
    : ControlGroup (p), section (s), pageOf (std::move (lookUp)), tabs (p), selected (s == Section::pre ? BlockId::gateA : BlockId::postEq)
{
    addAndMakeVisible (tabs);
    tabs.onSelect = [this] (int id) { select ((BlockId) id); };
    tabs.onReorder = [this] (const std::vector<int>& order)
    {
        juce::StringArray names;
        for (auto id : order)
            names.add (info ((BlockId) id).orderName);
        ampSim.setSectionOrder (chainSection (section), names);
        updateTabs();
    };

    if (section == Section::pre)
    {
        gateChoice = std::make_unique<Segmented> (juce::StringArray { "Gate A, before the amp", "Gate B, after the amp" });
        gateChoice->onChange = [this] (int i) { showGateB (i == 1); };
        addChildComponent (*gateChoice);
    }
    updateTabs();
    tabs.setSelected ((int) selected);
}

SectionPage::~SectionPage() = default;

void SectionPage::updateTabs()
{
    std::vector<TabRow::Item> items;
    for (const auto& name : ampSim.getSectionOrder (chainSection (section)))
        if (const auto id = blockFor (section, name); id != BlockId::count)
            items.push_back ({ (int) id, tabName (id), {}, info (id).onParameter, false });
    tabs.setItems (items);
}

BlockId SectionPage::pageBlock() const
{
    return selected == BlockId::gateA && gateB ? BlockId::gateB : selected;
}

void SectionPage::select (BlockId id)
{
    if (id == BlockId::gateB)
    {
        gateB = true;
        id = BlockId::gateA;
    }
    else if (id == BlockId::gateA)
    {
        gateB = false;
    }
    selected = id;
    tabs.setSelected ((int) selected);
    if (gateChoice != nullptr)
        gateChoice->setSelected (gateB ? 1 : 0);
    showSelected();
    resized();
}

void SectionPage::showGateB (bool b)
{
    gateB = b;
    if (gateChoice != nullptr)
        gateChoice->setSelected (b ? 1 : 0);
    showSelected();
}

void SectionPage::showSelected()
{
    auto& page = pageOf (pageBlock());
    if (shown != &page || page.getParentComponent() != this)
    {
        if (shown != nullptr && shown != &page && shown->getParentComponent() == this)
        {
            shown->setVisible (false);
            if (showing)
                shown->pageHidden();
        }
        shown = &page;
        addAndMakeVisible (page);
        if (showing)
        {
            page.pageShown();
            page.refresh();
        }
    }
    if (gateChoice != nullptr)
        gateChoice->setVisible (selected == BlockId::gateA);
    resized();
}

void SectionPage::refresh()
{
    updateTabs();
    if (shown != nullptr && shown->getParentComponent() == this)
        shown->refresh();
}

void SectionPage::pageShown()
{
    showing = true;
    shown = nullptr; // so showSelected() reclaims the page (the EQ page may have borrowed it) and starts it
    showSelected();
}

void SectionPage::pageHidden()
{
    if (shown != nullptr && shown->getParentComponent() == this)
        shown->pageHidden();
    showing = false;
}

void SectionPage::resized()
{
    auto area = getLocalBounds();
    auto row = area.removeFromTop (TabRow::height);
    tabs.setBounds (row);
    if (gateChoice != nullptr)
    {
        const auto w = gateChoice->getPreferredWidth();
        gateChoice->setBounds (row.removeFromRight (w).withSizeKeepingCentre (w, Segmented::preferredHeight).withY (row.getY() + 2));
    }
    area.removeFromTop (space::l);
    if (shown != nullptr && shown->getParentComponent() == this)
        shown->setBounds (area);
}

// ---- EqView ------------------------------------------------------------------------------------------

EqView::EqView (AmpSimProcessor& p, BlockPage& post, BlockPage& preEqPage) : ControlGroup (p), postEq (post), preEq (preEqPage), tabs (p)
{
    addAndMakeVisible (tabs);
    tabs.setItems ({ { 0, "Post EQ", "after the cab", "eq_post_on", false }, { 1, "Pre EQ", "before the amp", "eq_pre_on", false } });
    tabs.setSelected (0);
    tabs.onSelect = [this] (int id) { showPre (id == 1); };
}

void EqView::showPre (bool shouldShowPre)
{
    if (pre != shouldShowPre)
    {
        if (showing && current().getParentComponent() == this)
        {
            current().setVisible (false);
            current().pageHidden();
        }
        pre = shouldShowPre;
    }
    tabs.setSelected (pre ? 1 : 0);
    showSelected();
}

void EqView::showSelected()
{
    auto& page = current();
    if (auto& other = pre ? postEq : preEq; other.getParentComponent() == this)
        other.setVisible (false);
    addAndMakeVisible (page);
    if (showing)
    {
        page.pageShown();
        page.refresh();
    }
    resized();
}

void EqView::refresh()
{
    if (current().getParentComponent() == this)
        current().refresh();
}

void EqView::pageShown()
{
    showing = true;
    showSelected();
}

void EqView::pageHidden()
{
    if (current().getParentComponent() == this)
        current().pageHidden();
    showing = false;
}

void EqView::resized()
{
    auto area = getLocalBounds();
    tabs.setBounds (area.removeFromTop (TabRow::height));
    area.removeFromTop (space::l);
    for (auto* page : { &postEq, &preEq })
        if (page->getParentComponent() == this)
            page->setBounds (area);
}

// ---- InputPage ---------------------------------------------------------------------------------------

InputPage::InputPage (AmpSimProcessor& p) : ControlGroup (p)
{
    input = &addKnob ("input_gain", "Input");
    calibrate = &addSwitch ("input_calibrate", "Calibrate to captures");
    calibrate->setTooltip ("A capture that recorded its input level hears the guitar at the level it was made with. A global setting.");
    interfaceLabel = &addLabel ("Interface level at 0 dBFS", Text::label, inkDim);
    interfaceLevel = &addField ("input_level_dbu", " dBu");
    interfaceLevel->setShowsBar (false);
    interfaceLevel->setTooltip ("The analog level that reaches 0 dBFS on your interface. Solo 4th Gen instrument input at minimum gain: +12 dBu.");
    calibrationNote = &addLabel ("A capture that says what level it was trained at hears your guitar at that level: set the interface's level here once. "
                                 "Captures without a recorded level aren't changed. Global settings, never in presets.",
                                 Text::label, inkFaint);
    calibrationNote->setJustificationType (juce::Justification::topLeft);
    routingNote = &addLabel ("The guitar comes in on the interface's input 1 (channel 0). The input gain sits before everything, the "
                             "gates' detectors and the tuner included.",
                             Text::label, inkFaint);
    routingNote->setJustificationType (juce::Justification::topLeft);
}

void InputPage::paint (juce::Graphics& g)
{
    paintPageHeader (g, getLocalBounds().withHeight (28), "Input", "Where the guitar comes in");
    paintHeadings (g);
}

void InputPage::resized()
{
    clearHeadings();
    auto area = getLocalBounds().withTrimmedTop (28 + space::l);
    auto row = area.removeFromTop (cardHeight (Knob::preferredHeight (Knob::Size::normal)));

    auto level = card (row.removeFromLeft (2 * cardPadding + 160), "Level");
    input->setBounds (level.withSizeKeepingCentre (Knob::preferredWidth (Knob::Size::normal), Knob::preferredHeight (Knob::Size::normal)));
    row.removeFromLeft (space::m);

    auto calibration = card (row.removeFromLeft (juce::jmin (row.getWidth(), 560)), "Calibration");
    auto line = calibration.removeFromTop (Switch::preferredHeight);
    place (calibrate, line, space::xl);
    interfaceLabel->setBounds (line.removeFromLeft (150));
    interfaceLevel->setBounds (line.removeFromLeft (96).withSizeKeepingCentre (96, 24));
    calibration.removeFromTop (space::s);
    calibrationNote->setBounds (calibration);

    area.removeFromTop (space::l);
    routingNote->setBounds (area.removeFromTop (40).withWidth (juce::jmin (area.getWidth(), 720)));
}

// ---- OutputPage --------------------------------------------------------------------------------------

OutputPage::OutputPage (AmpSimProcessor& p) : ControlGroup (p)
{
    output = &addKnob ("output_gain", "Output");
    limiter = &addSwitch ("output_limit_on", "Limiter");
    limiter->setTooltip ("The output safety limiter: nothing leaves above the ceiling, with no added latency. Untouched while the level stays "
                         "2 dB under the ceiling. A global setting: presets and scenes never change it");
    ceiling = &addField ("output_limit_ceiling", " dBFS");
    ceiling->setTooltip ("The limiter's ceiling: the highest level the output can reach");
    ceiling->setShowsBar (false);
    saver = &addSwitch ("cpu_saver", "CPU saver");
    saver->setTooltip ("For slower computers: switched-off drives stop (and take 0.2 s to come on), the drives run at 2x, and Gain "
                       "plays its nearest of the five steps. Costs some tone. A global setting: presets and scenes never change it");
    abA = &addButton ("A", [this] { if (onAbSelect) onAbSelect (false); });
    abB = &addButton ("B", [this] { if (onAbSelect) onAbSelect (true); });
    abA->setConnectedEdges (juce::Button::ConnectedOnRight);
    abB->setConnectedEdges (juce::Button::ConnectedOnLeft);
    abA->setTooltip ("Compare two versions of the settings (captures and IRs stay)");
    abB->setTooltip ("Compare two versions of the settings (captures and IRs stay)");
    abCopy.onClick = [this] { if (onAbCopy) onAbCopy(); };
    abCopy.setTooltip ("Copy what's showing into the other of A and B");
    addAndMakeVisible (abCopy);

    tempo = &addField ("tempo_bpm", " BPM");
    tempo->setShowsBar (false);
    tap = &addButton ("Tap", [this] { if (onTap) onTap(); });
    tap->setTooltip ("Tap the tempo (or tap the footswitch's tap CC)");

    tapLabel = &addLabel ("Tap tempo", Text::body, inkDim);
    freezeLabel = &addLabel ("Reverb freeze", Text::body, inkDim);
    sceneLabel = &addLabel ("Scenes (value 0 to 7 is scene 1 to 8)", Text::body, inkDim);
    tapCc = &addField ("midi_tap_cc", "");
    freezeCc = &addField ("midi_freeze_cc", "");
    sceneCc = &addField ("midi_scene_cc", "");
    for (auto* field : { tapCc, freezeCc, sceneCc })
    {
        field->setFormatter ([] (float cc) { return "CC " + juce::String (juce::roundToInt (cc)); });
        field->setShowsBar (false);
    }
    midi = &addButton ("MIDI mappings...", [this] { if (onMidiMappings) onMidiMappings(); });
    note = &addLabel ("Program change 1 to 3 picks the amp. Right-click any control to map a footswitch or a pedal to it. "
                      "Cmd-Z undoes, Shift-Cmd-Z redoes.",
                      Text::label, inkFaint);
    note->setJustificationType (juce::Justification::topLeft);

    addAndMakeVisible (scenes);
    addAndMakeVisible (cpu);
    for (auto* b : std::initializer_list<juce::Component*> { abA, abB, &abCopy, tap, midi })
        b->setHasFocusOutline (true);
}

OutputPage::~OutputPage() = default;

void OutputPage::refresh()
{
    const auto onB = ampSim.isOnB();
    abA->setToggleState (! onB, juce::dontSendNotification);
    abB->setToggleState (onB, juce::dontSendNotification);
    scenes.refresh (ampSim.getScenes());
}

void OutputPage::paint (juce::Graphics& g)
{
    paintPageHeader (g, getLocalBounds().withHeight (28), "Output", "The level out, compare, tempo, scenes, and the footswitch");
    paintHeadings (g);
}

void OutputPage::resized()
{
    clearHeadings();
    auto area = getLocalBounds().withTrimmedTop (28 + space::l);
    const auto gap = space::m;

    // Row one: the level, A/B, the tempo, the CPU.
    auto row = area.removeFromTop (cardHeight (Knob::preferredHeight (Knob::Size::normal)));
    auto level = card (row.removeFromLeft (2 * cardPadding + 300), "Level");
    output->setBounds (level.removeFromLeft (140).withSizeKeepingCentre (Knob::preferredWidth (Knob::Size::normal), Knob::preferredHeight (Knob::Size::normal)));
    {
        // The limiter beside it: its switch, and the ceiling under it.
        auto column = level.withSizeKeepingCentre (level.getWidth(), 2 * controlHeight + space::s);
        limiter->setBounds (column.removeFromTop (controlHeight));
        column.removeFromTop (space::s);
        ceiling->setBounds (column.removeFromTop (controlHeight).withWidth (120));
    }
    row.removeFromLeft (gap);

    auto compare = card (row.removeFromLeft (220), "Compare");
    auto line = compare.withSizeKeepingCentre (compare.getWidth(), controlHeight);
    abA->setBounds (line.removeFromLeft (52));
    abB->setBounds (line.removeFromLeft (52));
    line.removeFromLeft (space::s);
    abCopy.setBounds (line.removeFromLeft (controlHeight));
    row.removeFromLeft (gap);

    auto tempoCard = card (row.removeFromLeft (260), "Tempo");
    line = tempoCard.withSizeKeepingCentre (tempoCard.getWidth(), controlHeight);
    tempo->setBounds (line.removeFromLeft (120));
    line.removeFromLeft (space::s);
    tap->setBounds (line.removeFromLeft (64));
    row.removeFromLeft (gap);

    auto cpuCard = card (row.removeFromLeft (juce::jmin (row.getWidth(), 200)), "Audio thread");
    {
        // The CPU meter, and the CPU saver under it.
        auto column = cpuCard.withSizeKeepingCentre (cpuCard.getWidth(), 16 + space::m + controlHeight);
        cpu.setBounds (column.removeFromTop (16).withWidth (120));
        column.removeFromTop (space::m);
        saver->setBounds (column.removeFromTop (controlHeight));
    }
    area.removeFromTop (gap);

    // Row two: the scenes.
    auto scenesCard = card (area.removeFromTop (cardHeight (48)), "Scenes");
    scenes.setBounds (scenesCard);
    area.removeFromTop (gap);

    // Row three: the footswitch's controllers.
    auto foot = card (area.removeFromTop (cardHeight (3 * controlHeight + 2 * space::s)), "Footswitch");
    auto left = foot.removeFromLeft (juce::jmin (foot.getWidth() / 2, 460));
    for (auto [label, field] : { std::pair<juce::Label*, ValueField*> { tapLabel, tapCc }, { freezeLabel, freezeCc }, { sceneLabel, sceneCc } })
    {
        auto l = left.removeFromTop (controlHeight);
        field->setBounds (l.removeFromRight (96));
        label->setBounds (l);
        left.removeFromTop (space::s);
    }
    foot.removeFromLeft (space::xl);
    midi->setBounds (foot.removeFromTop (controlHeight).withWidth (150));
    foot.removeFromTop (space::s);
    note->setBounds (foot);
}

} // namespace ui

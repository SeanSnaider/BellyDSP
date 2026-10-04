// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"
#include "ScenesBar.h"
#include "SectionTabs.h"

namespace ui
{

/// The Pre FX and Post FX pages (ASSUMPTIONS UH1): a row of tabs for the section's blocks in their current
/// order, each with its own on dot, and the selected block's editor below. Dragging a tab reorders the
/// section (the same setSectionOrder() call, applied with the chain's click-free dip). The pre section's
/// gate tab holds both gates, with a Gate A / Gate B switch at the right of the tab row (UH2).
class SectionPage final : public ControlGroup
{
public:
    SectionPage (AmpSimProcessor& processor, Section section, std::function<BlockPage& (BlockId)> pageOf);
    ~SectionPage() override;

    void select (BlockId id);
    BlockId getSelected() const noexcept { return selected; }
    TabRow& getTabs() noexcept { return tabs; }
    Section getSection() const noexcept { return section; }

    /// The gate tab's two gates (pre section only).
    void showGateB (bool b);
    bool isShowingGateB() const noexcept { return gateB; }

    /// The block page showing under the tabs.
    BlockPage* getShownBlockPage() const noexcept { return shown; }

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void resized() override;

private:
    void showSelected();
    void updateTabs();
    BlockId pageBlock() const;

    const Section section;
    std::function<BlockPage& (BlockId)> pageOf;
    TabRow tabs;
    std::unique_ptr<Segmented> gateChoice;
    BlockId selected;
    bool gateB = false, showing = false;
    BlockPage* shown = nullptr;
};

/// The EQ node's page (UH1): the post EQ, with tabs to switch to the pre EQ (which is also on the Pre FX
/// page; one editor, shown wherever it was asked for last).
class EqView final : public ControlGroup
{
public:
    EqView (AmpSimProcessor& processor, BlockPage& postEq, BlockPage& preEq);

    void showPre (bool pre);
    bool isShowingPre() const noexcept { return pre; }
    TabRow& getTabs() noexcept { return tabs; }

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void resized() override;

private:
    void showSelected();
    BlockPage& current() const { return pre ? preEq : postEq; }

    BlockPage &postEq, &preEq;
    TabRow tabs;
    bool pre = false, showing = false;
};

/// A page header in the effect pages' style: the title (15 px medium) and a line about it (12 px faint).
void paintPageHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle);

/// The Input page (UH1): the input gain, the input calibration, and a note on where the guitar comes in.
class InputPage final : public ControlGroup
{
public:
    explicit InputPage (AmpSimProcessor& processor);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Knob* input = nullptr;
    Switch* calibrate = nullptr;
    ValueField* interfaceLevel = nullptr;
    juce::Label *interfaceLabel = nullptr, *calibrationNote = nullptr, *routingNote = nullptr;
};

/// The Output page (UH6): the output level, A/B compare with Copy, the tempo with Tap, the CPU meter, the
/// eight scenes with Store, and the footswitch's controllers with the list of MIDI mappings.
class OutputPage final : public ControlGroup
{
public:
    explicit OutputPage (AmpSimProcessor& processor);
    ~OutputPage() override;

    std::function<void (bool b)> onAbSelect;
    std::function<void()> onAbCopy, onTap, onMidiMappings;

    ScenesBar& getScenesBar() noexcept { return scenes; }
    CpuMeter& getCpuMeter() noexcept { return cpu; }
    juce::Button& getMidiButton() noexcept { return *midi; }

    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Knob* output = nullptr;
    Switch* limiter = nullptr;     // output_limit_on (a global setting)
    ValueField* ceiling = nullptr; // output_limit_ceiling
    juce::TextButton *abA = nullptr, *abB = nullptr, *tap = nullptr, *midi = nullptr;
    IconButton abCopy { "Copy", IconButton::Icon::copy };
    ValueField *tempo = nullptr, *tapCc = nullptr, *freezeCc = nullptr, *sceneCc = nullptr;
    juce::Label *tapLabel = nullptr, *freezeLabel = nullptr, *sceneLabel = nullptr, *note = nullptr;
    ScenesBar scenes;
    CpuMeter cpu;
};

} // namespace ui

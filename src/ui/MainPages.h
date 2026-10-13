// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"

namespace ui
{

/// A page header in the effect pages' style: the title (15 px medium) and a line about it (12 px faint).
void paintPageHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle);

/// The Input page (in Settings): the input gain, the input calibration, and a note on where the guitar comes in.
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

/// The Output page (in Settings): the output level and the limiter, A/B compare with Copy, the tempo with Tap, the
/// CPU meter and the CPU saver, and the footswitch's controllers with the list of MIDI mappings. (The scenes are in
/// the main screen's footer.)
class OutputPage final : public ControlGroup
{
public:
    explicit OutputPage (AmpSimProcessor& processor);
    ~OutputPage() override;

    std::function<void (bool b)> onAbSelect;
    std::function<void()> onAbCopy, onTap, onMidiMappings;

    CpuMeter& getCpuMeter() noexcept { return cpu; }
    juce::Button& getMidiButton() noexcept { return *midi; }

    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Knob* output = nullptr;
    Switch* limiter = nullptr;     // output_limit_on (a global setting)
    ValueField* ceiling = nullptr; // output_limit_ceiling
    Switch* saver = nullptr;       // cpu_saver (a global setting)
    juce::TextButton *abA = nullptr, *abB = nullptr, *tap = nullptr, *midi = nullptr;
    IconButton abCopy { "Copy", IconButton::Icon::copy };
    ValueField *tempo = nullptr, *tapCc = nullptr, *freezeCc = nullptr, *sceneCc = nullptr;
    juce::Label *tapLabel = nullptr, *freezeLabel = nullptr, *sceneLabel = nullptr, *note = nullptr;
    CpuMeter cpu;
};

/// Settings, an overlay over the main screen: the Input and Output pages, one at a time under a segmented control
/// (Input; Output and MIDI), with its title and Close (Escape closes it too).
class SettingsPage final : public ControlGroup
{
public:
    explicit SettingsPage (AmpSimProcessor& processor);
    ~SettingsPage() override;

    std::function<void()> onClose;

    void showOutput (bool output);
    bool isShowingOutput() const noexcept { return showingOutput; }
    InputPage& getInputPage() noexcept { return input; }
    OutputPage& getOutputPage() noexcept { return output; }
    Segmented& getTabs() noexcept { return tabs; }
    juce::Button& getCloseButton() noexcept { return close; }

    void refresh() override;
    void pageShown() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    InputPage input;
    OutputPage output;
    Segmented tabs { juce::StringArray { "Input", "Output and MIDI" } };
    juce::TextButton close { "Close" };
    bool showingOutput = false;
};

} // namespace ui

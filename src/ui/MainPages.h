// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"

namespace ui
{

/// A page header in the effect pages' style: the title (15 px medium) and a line about it (12 px faint).
void paintPageHeader (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& subtitle);

/// A settings section's panels: each a panel (ui::overlay::paintPanel) with its label at the top left. A section
/// lays them out in resized() and paints them first.
class SettingsSection : public ControlGroup
{
public:
    using ControlGroup::ControlGroup;

    /// The panels' areas and labels, as laid out (for tests: how the section is organised).
    struct Panel
    {
        juce::Rectangle<int> area;
        juce::String title;
    };
    const std::vector<Panel>& getPanels() const noexcept { return panels; }

protected:
    /// Adds a panel and returns the area inside it, under its label.
    juce::Rectangle<int> panel (juce::Rectangle<int> area, const juce::String& title);
    void paintPanels (juce::Graphics& g) const;
    std::vector<Panel> panels;

    static constexpr int padding = 18, labelHeight = 16, labelGap = 14;
};

/// Settings' Input section: the input gain, the input calibration, and where the guitar comes in.
class InputPage final : public SettingsSection
{
public:
    explicit InputPage (AmpSimProcessor& processor);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Knob* input = nullptr;
    Switch* calibrate = nullptr;
    ValueField* interfaceLevel = nullptr;
    juce::Label *interfaceLabel = nullptr, *calibrationNote = nullptr, *routingNote = nullptr, *levelNote = nullptr;
};

/// Settings' Output and MIDI section: the output level and the limiter, A/B compare with Copy, the tempo with Tap, the
/// CPU meter and the CPU saver, and the footswitch's controllers with the list of MIDI mappings and the keys. (The
/// scenes are in the main screen's footer.)
class OutputPage final : public SettingsSection
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
    juce::Label *tapLabel = nullptr, *freezeLabel = nullptr, *sceneLabel = nullptr, *note = nullptr, *ceilingLabel = nullptr, *compareNote = nullptr,
                *tempoNote = nullptr, *saverNote = nullptr;
    CpuMeter cpu;
    juce::Rectangle<int> keysArea;
};

/// Settings' Audio device section: the 48 kHz requirement said plainly, what the device is running at now (its name,
/// driver, rate, buffer, and latencies, in the standalone app), the setup a player needs, and the app's own audio
/// settings (ASSUMPTIONS RD3-7).
class AudioDevicePage final : public SettingsSection
{
public:
    explicit AudioDevicePage (AmpSimProcessor& processor);

    /// What the rate panel says now: "Running at 48 kHz.", or the rate it's at and that the output is muted.
    juce::String getRateText() const;
    bool isRateOk() const;
    juce::Button& getDeviceButton() noexcept { return *deviceButton; }

    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TextButton* deviceButton = nullptr;
    juce::Rectangle<int> ratePanel, deviceRows, setupArea;
    juce::String shownRate, shownDevice;
};

/// Settings' About section: the name and version, the licences (the app's AGPL, the captures' CC BY 4.0, the IRs'
/// CC0, the font's OFL; the full notices in their own window), updates, and the source of this exact version.
class AboutPage final : public SettingsSection
{
public:
    explicit AboutPage (AmpSimProcessor& processor);

    std::function<void()> onLicences;
    juce::Button& getLicencesButton() noexcept { return *licences; }
    juce::Button& getUpdatesButton() noexcept { return *updates; }
    juce::Button& getSourceButton() noexcept { return *source; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TextButton *licences = nullptr, *updates = nullptr, *source = nullptr;
    juce::Rectangle<int> identity, licenceRows, updatesText, sourceText;
};

/// Settings, an overlay over the main screen (phase 3; ASSUMPTIONS RD3-6 to RD3-9): the header with Close, a column of
/// sections at the left (Input; Output and MIDI; Audio device; About), and the chosen section's panels at the right.
class SettingsPage final : public ControlGroup
{
public:
    explicit SettingsPage (AmpSimProcessor& processor);
    ~SettingsPage() override;

    enum class Section
    {
        input,
        output,
        device,
        about
    };
    static constexpr int numSections = 4;
    static juce::String sectionName (Section s);

    std::function<void()> onClose;
    /// "Licences..." in About (the editor's About window with every notice).
    std::function<void()> onLicences;

    void showSection (Section s);
    Section getSection() const noexcept { return section; }
    /// The Input or the Output section (selecting the input or the output block opens these).
    void showOutput (bool showOutputSection) { showSection (showOutputSection ? Section::output : Section::input); }
    bool isShowingOutput() const noexcept { return section == Section::output; }

    InputPage& getInputPage() noexcept { return input; }
    OutputPage& getOutputPage() noexcept { return output; }
    AudioDevicePage& getDevicePage() noexcept { return device; }
    AboutPage& getAboutPage() noexcept { return about; }
    SettingsSection& getSectionPage (Section s);
    juce::Button& getCloseButton() noexcept { return close; }
    /// The section list's entry for a section (a click shows it; Up and Down step it with the keyboard's focus).
    juce::Button& getSectionButton (Section s) noexcept;

    void refresh() override;
    void pageShown() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int navWidth = 220;

private:
    class SectionButton;
    InputPage input;
    OutputPage output;
    AudioDevicePage device;
    AboutPage about;
    std::array<std::unique_ptr<SectionButton>, numSections> sectionButtons;
    juce::TextButton close { "Close" };
    Section section = Section::input;
    juce::Rectangle<int> header, nav;
};

} // namespace ui

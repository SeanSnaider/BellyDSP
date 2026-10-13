// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Pages.h"

namespace ui
{

/// The tuner, an overlay opened from the top bar's Tuner, the footer's reading, or T (it engages the tuner;
/// closing it disengages it). Built to be read from a step back with a guitar on (the redesign, phase 3;
/// ASSUMPTIONS RD3-1 to RD3-4):
///
///   the display   a lit panel: the note at 200 px (light; the accent within 3 cents) with its sharp and octave,
///                 a chevron either side (the left one lights when flat, "tune up"; the right one when sharp;
///                 both in the accent when in tune), the frequency, the cents and Flat / Sharp / In tune, and a
///                 960 px cents scale of 41 segments (-50 to +50, 2.5 cents apart) whose lit segment follows the
///                 cents, its neighbours lit in proportion so the position reads between them; within 3 cents
///                 the middle three light in the accent with a glow
///   the strings   the chosen tuning's six strings as chips (the string's number, its note and octave): the one
///                 being tuned outlined in ink, a clicked target in the accent, a string that's been in tune this
///                 session with its note in the accent and a check
///   the controls  the tunings (Standard, Drop D, Drop C, FACGCE), A4 from 430 to 450 Hz with - and +, and
///                 Mute output
///
/// It reads the processor's tuner (its own thread analyses the DI) 30 times a second while showing. The current
/// string is the one nearest the note heard, unless one has been clicked: then the note and the cents are measured
/// against that string, so a string far out (or dropped) can be brought to it; a second click on it lets go
/// (ASSUMPTIONS UH9). The tuning is the app's view state, saved with it, never a parameter.
class TunerPage final : public ControlGroup, private juce::Timer
{
public:
    explicit TunerPage (AmpSimProcessor& processor);
    ~TunerPage() override;

    struct Tuning
    {
        const char* name;
        std::array<int, 6> midi; // low string (6) to high (1)
    };
    static const std::array<Tuning, 4>& tunings();

    void setTuning (int index);
    int getTuning() const noexcept { return tuning; }
    /// Targets a string (0 = string 6), or lets go of it when it's targeted already.
    void clickString (int index);
    int getTarget() const noexcept { return target; }

    /// Reads the tuner now (what the page's timer does 30 times a second).
    void poll() { timerCallback(); }

    /// For tests and snapshots: draw this reading from now on, instead of the live one.
    void freeze (const ampsim::TunerReading& r);

    /// What's shown, for tests.
    struct Shown
    {
        juce::String note, accidental, octave, hz, cents, state;
        double centsValue = 0.0;
        bool inTune = false;
        int currentString = 0;
    };
    Shown getShown() const { return shown; }
    bool isStringDone (int index) const { return done[(size_t) index]; }

    /// The page's Close (Escape and T close it too).
    std::function<void()> onClose;
    juce::Button& getCloseButton() noexcept { return *closeButton; }
    Segmented& getTuningChoice() noexcept { return *tuningChoice; }

    /// For tests: where things are drawn (the page's own coordinates).
    juce::Rectangle<int> getDisplayArea() const noexcept { return displayArea; }
    juce::Rectangle<float> getStringBounds (int index) const { return stringBox (index); }
    /// The scale's segment for a number of cents (-50 to +50 in steps of 2.5) and how lit it's drawn (0 to 1).
    static constexpr int numSegments = 41;
    static float segmentLight (double segmentCents, double cents);
    /// Which of the flat (-1) and sharp (+1) chevrons is lit now (0: neither, 2: both, in tune).
    int getLitChevrons() const;

    void refresh() override;
    void pageShown() override;
    void pageHidden() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    static constexpr float inTuneCents = 3.0f;

private:
    void timerCallback() override;
    void update (const ampsim::TunerReading& r);
    juce::Rectangle<float> stringBox (int index) const;

    ampsim::TunerReading reading;
    bool frozen = false;
    int tuning = 0, target = -1, lastString = 0;
    std::array<bool, 6> done {};
    Shown shown;

    std::unique_ptr<Segmented> tuningChoice;
    juce::TextButton *a4Down = nullptr, *a4Up = nullptr, *closeButton = nullptr;
    Switch* mute = nullptr;
    juce::Rectangle<int> header, displayArea, noteArea, readArea, scaleArea, stringsPanel, stringsArea, controlsPanel, a4Area, a4Value;
};

} // namespace ui

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Controls.h"
#include "PluginProcessor.h"

#include <array>

namespace ui
{

/// The parts the designed inspectors are built from (the app redesign, phase 2) beyond the plain controls: the live
/// meters of the gates and the compressors, the delay's Tap pad, the harmonizer's "what it hears" display, and the
/// cab's front (the board's marker and the cab's header). All draw on the message thread from values the audio
/// thread publishes as atomics; none of them touches the audio thread.

/// A gate's meter: what it's doing in a word (Open, Closed, Learning, Following Gate A, Off) with its LED, the
/// detector's level against the thresholds on a -100 to 0 dBFS bar, and the gain reduction.
///
///   the bar        line-2 track; the detector's level fills it from the left, orange while the gate is open, ink-dim
///                  while it's closed. Between the close threshold (a faint tick) and the open one (an ink tick with a
///                  handle above it) the band is shaded: the hysteresis. A drag anywhere on the bar sets the threshold
///                  to the level under the pointer (one gesture, one undo step).
///   the reduction  a thin bar under it, growing from the right, 0 to 40 dB (full: the gate is muting), and its number.
class GateMeterView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    GateMeterView (juce::AudioProcessorValueTreeState& state, const juce::String& thresholdId);
    ~GateMeterView() override;

    struct Reading
    {
        AmpSimProcessor::GateMeter meter;
        bool on = false, linked = false, learning = false;
        float progress = 0.0f;
    };
    /// A new reading; repaints only when what it shows moves.
    void set (const Reading& reading);
    const Reading& getReading() const noexcept { return reading; }
    /// The state as the meter says it ("Open", "Closed", "Off", "Learning 40%", "Following Gate A").
    juce::String getStateText() const;

    static constexpr float floorDb = -100.0f, reductionRangeDb = 40.0f;
    static constexpr int preferredHeight = 88;
    juce::Rectangle<float> barArea() const;
    float xForDb (float db) const;
    float dbAtX (float x) const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::AudioProcessorValueTreeState& state;
    juce::RangedAudioParameter& threshold;
    Reading reading;
    bool dragging = false;
};

/// A compressor's meter: its static curve (Compressor::staticCurveDb, the block's own formula) on the left, output
/// against input from -60 to 0 dB with the 1:1 line and the threshold marked; on the right the gain reduction now, as a
/// number and a bar growing down from 0 to 24 dB, over the last three seconds of it as a trace hanging from the top.
class CompressorMeterView final : public juce::Component
{
public:
    CompressorMeterView (juce::AudioProcessorValueTreeState& state, const juce::String& prefix);

    /// The gain reduction now (dB, 0 or more), pushed 30 times a second while the page shows: it joins the trace.
    void push (float reductionDb);
    float getShownReduction() const noexcept { return shown; }
    /// The settings moved: the curve redraws.
    void refreshCurve();

    static constexpr float rangeDb = 24.0f;
    static constexpr int historyLength = 90; // three seconds at 30 a second
    static constexpr int preferredHeight = 140;

    void paint (juce::Graphics&) override;

private:
    juce::AudioProcessorValueTreeState& state;
    const juce::String prefix;
    std::array<float, historyLength> history {};
    int head = 0;
    float shown = 0.0f, drawnThreshold = 1.0f, drawnRatio = 0.0f, drawnKnee = -1.0f;
};

/// The delay's Tap pad: a raised pad with "Tap", its LED blinking at the tempo (the tempo's field sits under it). A click taps the
/// tempo (the editor's AmpSimProcessor::tapTempo through onClick); the pad flashes for a moment.
class TapPad final : public juce::Button, private juce::Timer
{
public:
    TapPad();
    ~TapPad() override;

    /// The tempo in effect (BPM), for the line under "Tap" and the blink (30 times a second while it shows).
    void setTempo (double bpm);
    double getTempo() const noexcept { return tempo; }
    bool isBeatLit() const;

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    void clicked() override;

private:
    void timerCallback() override;
    double tempo = 120.0, flashUntil = 0.0;
    bool lit = false;
};

/// The harmonizer's display: the note it hears now, large, and each voice's note beside it (its interval and name,
/// in its row), or "Listening" when there's no single note.
class HearingView final : public juce::Component
{
public:
    struct Reading
    {
        int note = -1;
        std::array<int, 4> shifts { 0, 0, 0, 0 };
        std::array<bool, 4> sounding {};
    };
    void set (const Reading& r);
    const Reading& getReading() const noexcept { return reading; }
    /// The voices' line as text ("1 +4 G#4", ...), for tests.
    juce::String getText() const;
    static constexpr int preferredHeight = 84;
    void paint (juce::Graphics&) override;

private:
    Reading reading;
    bool everSet = false;
};

/// The cab's front in small (the board's cab marker and the cab's inspector header): a tolex box with piping, a woven
/// grille cloth with the speaker's ring showing faintly through it, and a small badge. `on` false draws it unlit.
void drawCabFront (juce::Graphics& g, juce::Rectangle<float> box, bool on);

/// An inspector heading (phase 2): the title in 10.5 px semibold capitals, tracked, ink-faint, and a hairline after it.
void drawInspectorHeading (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title);

} // namespace ui

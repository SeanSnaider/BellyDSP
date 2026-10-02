#pragma once

#include "dsp/Chorus.h"
#include "dsp/Compressor.h"
#include "dsp/Delay.h"
#include "dsp/Equalizer.h"
#include "dsp/Reverb.h"

#include <juce_audio_processors/juce_audio_processors.h>

/// Parameter glue for the effect blocks: each struct declares one block instance's parameters (with
/// permanent IDs under a prefix such as "comp_pre"), binds to their raw values, and reads them into
/// the block's Settings once per buffer on the audio thread. Keeps PluginProcessor thin as blocks are
/// added.
///
/// Parameter IDs are permanent once presets exist: never rename one, add a new ID instead.
namespace params
{

using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;
using State = juce::AudioProcessorValueTreeState;

/// A raw parameter value, read on the audio thread without locks.
struct Raw
{
    std::atomic<float>* value = nullptr;

    void bind (State& state, const juce::String& id)
    {
        value = state.getRawParameterValue (id);
        jassert (value != nullptr);
    }
    float get() const noexcept { return value->load (std::memory_order_relaxed); }
    bool on() const noexcept { return get() >= 0.5f; }
    int index() const noexcept { return juce::roundToInt (get()); }
};

struct CompressorParameters
{
    static void addTo (Layout& layout, const juce::String& prefix, const juce::String& name, bool onByDefault);
    void bind (State& state, const juce::String& prefix);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Compressor::Settings read() const noexcept;

    Raw on, mode, detector, threshold, ratio, knee, attack, release, autoRelease, makeup, autoMakeup, mix, sidechainOn, sidechainHz;
};

struct EqualizerParameters
{
    static void addTo (Layout& layout, const juce::String& prefix, const juce::String& name, bool onByDefault);
    void bind (State& state, const juce::String& prefix);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Equalizer::Settings read() const noexcept;

    static juce::String sliderId (const juce::String& prefix, int band) { return prefix + "_g" + juce::String (band + 1); }
    static juce::String bandId (const juce::String& prefix, int band, const juce::String& what)
    {
        return prefix + "_b" + juce::String (band + 1) + "_" + what;
    }

    Raw on, mode;
    std::array<Raw, ampsim::Equalizer::numGraphicBands> sliders;
    struct Band
    {
        Raw type, frequency, gain, q;
    };
    std::array<Band, ampsim::Equalizer::numParametricBands> bands;
    Raw lowCutOn, lowCutFrequency, lowCutSlope, highCutOn, highCutFrequency, highCutSlope;
};

struct DelayParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }

    /// The settings at this tempo (synced times are note lengths at `bpm`).
    ampsim::Delay::Settings read (double bpm) const noexcept;

    Raw on, mode, stereoMode, sync, time, note, rightTime, rightNote, offset, feedback, lowCut, highCut, modDepth, modRate, duck, mix;
};

struct ChorusParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }

    /// The settings at this tempo (a synced rate is one LFO cycle per note length at `bpm`).
    ampsim::Chorus::Settings read (double bpm) const noexcept;

    Raw on, mode, shape, sync, rate, note, depth, mix, width, analog, noise, highPass, highPassHz;
};

struct ReverbParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }
    bool isFrozen() const noexcept { return freeze.on(); }

    /// The settings at this tempo (a synced pre-delay is a note length at `bpm`). `freezeOverride` (0 or 1)
    /// replaces the freeze switch when a footswitch has just toggled it; -1 means use the switch.
    ampsim::Reverb::Settings read (double bpm, int freezeOverride = -1) const noexcept;

    Raw on, engine, mix, preDelay, preDelaySync, preDelayNote, size, decay, lowDecay, highDecay, diffusion, modDepth, modRate,
        width, earlyLate, lowCut, highCut, ducking, freeze;
};

/// Shared helpers for declaring parameters.
juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre, float interval = 0.0f);
juce::AudioParameterFloatAttributes decibels();
juce::AudioParameterFloatAttributes hertz();
juce::AudioParameterFloatAttributes milliseconds();
juce::AudioParameterFloatAttributes percent();

} // namespace params

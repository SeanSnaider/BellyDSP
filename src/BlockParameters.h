#pragma once

#include "dsp/Bloom.h"
#include "dsp/Boost.h"
#include "dsp/Chorus.h"
#include "dsp/Compressor.h"
#include "dsp/Delay.h"
#include "dsp/Equalizer.h"
#include "dsp/Gate.h"
#include "dsp/Harmonizer.h"
#include "dsp/Multivoicer.h"
#include "dsp/Overdrive.h"
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

/// One noise gate (prefix "gate_a" or "gate_b"). The link switch, "gate_link", is the processor's.
struct GateParameters
{
    static void addTo (Layout& layout, const juce::String& prefix, const juce::String& name);
    void bind (State& state, const juce::String& prefix);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Gate::Settings read() const noexcept;

    Raw on, threshold, hysteresis, hold, attack, release, releaseMode, range, detector, sidechainOn, sidechainHz;
};

/// The boost ("boost_*"). Oversampling and the interface's full-scale voltage are global settings the
/// processor passes in.
struct BoostParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Boost::Settings read (int oversampling, double voltsAtFullScale) const noexcept;

    Raw on, mode, level, tilt, tightHz, mid;
};

/// The overdrive ("od_*"). Its mode list only ever grows at the end, because a saved preset holds the
/// mode's index (and Overdrive::Mode is numbered the same way).
struct OverdriveParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Overdrive::Settings read (int oversampling, double voltsAtFullScale) const noexcept;

    static juce::StringArray modeNames(); // message thread (it allocates)
    static constexpr int numModes = 4;

    Raw on, mode, drive, tone, level, mix, tightOn, tightHz;
};

/// Bloom ("bloom_*"): the container's switch and mix, and its three effects. The order inside Bloom is
/// the processor's (saved by effect name, like the sections'). Synced rates are one LFO cycle per note
/// length at the tempo.
struct BloomParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Bloom::Settings read (double bpm, const ampsim::Bloom::Order& order) const noexcept;

    Raw on, mix;
    Raw crushOn, crushBits, crushRate, crushDither, crushTone, crushMix;
    Raw phaserOn, phaserMode, phaserStages, phaserShape, phaserSync, phaserRate, phaserNote, phaserDepth, phaserLow, phaserHigh, phaserFeedback,
        phaserClassicFeedback, phaserStereo, phaserMix;
    Raw flangerOn, flangerManual, flangerDepth, flangerShape, flangerSync, flangerRate, flangerNote, flangerFeedback, flangerNegative, flangerStereo,
        flangerMix, flangerThroughZero;
};

/// The multivoicer ("mv_*"): engine, voice count, mix, spread, and the wet high-pass, then each voice
/// ("mv_v1_semitones", ...). The defaults are the Double + Octaves starting point; the other starting
/// points are written into these parameters by applyStartingPoint().
struct MultivoicerParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Multivoicer::Settings read() const noexcept;

    static juce::String voiceId (int voice, const char* what) { return "mv_v" + juce::String (voice + 1) + "_" + what; }

    /// Message thread: sets every voice (and the voice count) to a starting point.
    static void applyStartingPoint (State& state, ampsim::Multivoicer::StartingPoint point);

    Raw on, engine, voiceCount, mix, spread, highPass, highPassHz;
    struct Voice
    {
        Raw semitones, cents, delay, pan, level, drift;
    };
    std::array<Voice, ampsim::Multivoicer::maxVoices> voices;
};

/// The harmonizer ("harm_*"): key (root, scale, custom mask), the rule for out-of-key notes, glide,
/// detection floor, overall level, and four voices ("harm_v1_on", ...). Each voice has both a diatonic
/// interval (steps) and a chromatic one (semitones); its mode picks which applies. A4 is the tuner's.
struct HarmonizerParameters
{
    static void addTo (Layout& layout);
    void bind (State& state);
    bool isOn() const noexcept { return on.on(); }
    ampsim::Harmonizer::Settings read (double referenceA4) const noexcept;

    static juce::String voiceId (int voice, const char* what) { return "harm_v" + juce::String (voice + 1) + "_" + what; }

    Raw on, root, scale, customMask, outOfKey, glide, floor, level;
    struct Voice
    {
        Raw on, mode, steps, semitones, octave, level, pan, humanize;
    };
    std::array<Voice, ampsim::Harmonizer::maxVoices> voices;
};

/// The drive blocks' oversampling factor ("drive_oversampling": 4x or 8x), a global setting.
int oversamplingFactor (const Raw& choice) noexcept;

/// Volts at 0 dBFS for the drive circuits, from the interface's input level in dBu (the same number the
/// NAM calibration uses): the peak of a sine at that RMS level, sqrt(2) x 0.7746 V x 10^(dBu / 20).
double voltsAtFullScale (double interfaceDbu) noexcept;

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
        width, earlyLate, lowCut, highCut, ducking, freeze, shimmer, shimmerInterval;
};

/// Shared helpers for declaring parameters.
juce::NormalisableRange<float> skewedRange (float lo, float hi, float centre, float interval = 0.0f);
juce::AudioParameterFloatAttributes decibels();
juce::AudioParameterFloatAttributes hertz();
juce::AudioParameterFloatAttributes milliseconds();
juce::AudioParameterFloatAttributes percent();

} // namespace params

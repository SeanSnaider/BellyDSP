// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Lfo.h"
#include "PitchShifter.h"
#include "Psola.h"
#include "Svf.h"

#include <array>

namespace ampsim
{

/// The multivoicer (BUILD_PLAN "Multivoicer", design review round 13): up to eight pitch-shifted copies of
/// the signal, each with its own interval, delay, pan, level, and optional drift, on the shared pitch
/// shifter (PitchShifter.h, Psola.h). A stereo post-FX block with zero latency: the dry is never delayed or
/// touched, and the voices trail it like doubling delays.
///
/// Signal path, per sample:
///   1. The wet input is the mono sum of the stereo input, (L + R) / 2, fed to one shared PitchShifterInput
///      (and, for Mono, one shared PSOLA analysis whose pitch detector reads the clean DI).
///   2. Each voice shifts it by 2^((semitones + cents/100) / 12), on top of its own delay (0 to 50 ms).
///      Poly (default, since Sean plays chords constantly) uses a GranularVoice, which works on anything.
///      Mono uses a PsolaVoice, cleaner on single notes (above all an octave down), and crossfades each
///      voice to its granular twin whenever the analysis isn't confident (chords, noise, silence): that is
///      how Mono degrades on chords, by sounding like Poly instead of producing garbage.
///   3. Drift (per voice, 0 to 1): a slow random wander of up to +-3 cents in pitch and 0 to 2 ms in timing,
///      on smoothed random LFOs with each voice's own seed, so doubles sound like separate takes.
///   4. Each voice is panned with constant power scaled by sqrt(2): left sqrt(2) cos(theta), right
///      sqrt(2) sin(theta), theta = (pan + 1) pi/4, so a centred voice is unity on each side (as the cab's
///      mics are) and L^2 + R^2 = 2 everywhere. Spread scales every pan (0 puts all voices in the middle).
///   5. The wet bus is divided by sqrt(sum of the voices' squared levels): the voices are decorrelated from
///      each other (different intervals and delays), so their powers add, and this holds the wet at the
///      input's level whatever the voice count, which is what the equal-power mix below assumes.
///   6. Optional wet high-pass (12 dB/oct Butterworth, off by default so octave-down voices keep their lows).
///   7. Mix, equal power: out = cos(mix pi/2) dry + sin(mix pi/2) wet, with exact endpoints (mix 0 is the
///      input bit for bit). The wet is decorrelated from the dry (it is shifted and delayed), so this holds
///      the level at every mix, like the chorus, delay, and reverb.
///
/// Smoothing: levels, pans, spread, drift amounts, the mix, and the high-pass amount over 20 ms; the
/// high-pass frequency over 25 ms with coefficients every 32 samples; intervals glide in the log domain over
/// 30 ms inside each voice; a delay change crossfades the voice to a head at the new delay (no pitch bend).
/// A voice count change fades voices in and out over 20 ms; a voice that starts again begins from a fresh
/// state (it was silent). Poly/Mono switches, and Mono's moves between PSOLA and its granular fallback,
/// crossfade over 20 ms with equal power (the two engines trail by different amounts, so they are only
/// partly correlated); both engines run during a fade.
class Multivoicer : public Block
{
public:
    static constexpr int maxVoices = 8;
    static constexpr double maxDelayMs = 50.0;
    static constexpr double driftCents = 3.0, driftDelayMs = 2.0;
    static constexpr double driftPitchHz = 0.31, driftTimeHz = 0.19;
    static constexpr double minHighPassHz = 40.0, maxHighPassHz = 1000.0;
    static constexpr double minLevelDb = -60.0, maxLevelDb = 6.0; // a voice at -60 dB is off
    static constexpr double smoothingSeconds = 0.020;
    static constexpr double highPassSmoothingSeconds = 0.025;
    static constexpr double engineFadeSeconds = 0.020; // Poly/Mono, and Mono's fallback to granular and back
    static constexpr double glideMs = 30.0;
    static constexpr int coefficientInterval = 32;

    enum class Engine
    {
        poly, // granular: chords and anything else
        mono  // PSOLA: single notes, clean octave-down; granular while the analysis isn't confident
    };

    struct Voice
    {
        double semitones = 0.0; // -24 to +24
        double cents = 0.0;     // -100 to +100
        double delayMs = 0.0;   // 0 to 50, on top of the engine's own trail
        double pan = 0.0;       // -1 left to +1 right
        double levelDb = 0.0;   // -60 (off) to +6
        double drift = 0.0;     // 0 to 1
    };

    enum class StartingPoint
    {
        unisonDouble,
        octaveStack,
        fifthsStack,
        doubleOctaves
    };

    struct Settings
    {
        Engine engine = Engine::poly;
        int voiceCount = 4;
        std::array<Voice, maxVoices> voices {};
        double spread = 1.0; // 0 to 1, scales every pan
        double mix = 0.5;    // 0 dry to 1 wet
        bool wetHighPass = false;
        double wetHighPassHz = 100.0;
    };

    /// The voice settings for each starting point (engine, mix, and high-pass at their defaults):
    ///   Unison double    +8 and -10 cents, 0 and 7 ms, panned hard left and right, drift on
    ///   Octave stack     -12 and +12 semitones, panned slightly apart
    ///   Fifths stack     +7 semitones, plus the fifth an octave higher (+19) 6 dB down
    ///   Double + Octaves the unison double, plus -12 and +12 semitones 6 dB down (the default)
    static Settings startingPoint (StartingPoint point);

    /// The default settings: Double + Octaves.
    static Settings defaults() { return startingPoint (StartingPoint::doubleOctaves); }

    /// The equal-power mix law with exact endpoints.
    struct MixGains
    {
        double dry, wet;
    };
    static MixGains mixGains (double mix) noexcept;

    /// The constant-power pan law scaled by sqrt(2): {left, right} for a pan in [-1, 1].
    static std::pair<double, double> panGains (double pan) noexcept;

    /// A voice's pitch ratio.
    static double ratioOf (const Voice& v) noexcept { return std::pow (2.0, (v.semitones + v.cents / 100.0) / 12.0); }

    Multivoicer() { settings = defaults(); }

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings) noexcept;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }
    int latencySamples() const override { return 0; }

    // For tests and meters.
    bool isVoiceRunning (int v) const noexcept { return voices[(size_t) v].running; }
    const GranularVoice& getGranularVoice (int v) const noexcept { return voices[(size_t) v].granular; }
    const PsolaVoice& getPsolaVoice (int v) const noexcept { return voices[(size_t) v].psola; }
    const PsolaAnalysis& getAnalysis() const noexcept { return analysis; }
    /// How much of each voice comes from PSOLA right now: 0 in Poly, 1 in Mono while the analysis is sure.
    double getPsolaShare() const noexcept { return psolaShare.getCurrentValue(); }
    double getWetNormalization() const noexcept { return normalization.getCurrentValue(); }

private:
    struct VoiceState
    {
        GranularVoice granular;
        PsolaVoice psola;
        Lfo pitchDrift, timeDrift;
        juce::SmoothedValue<double> level { 0.0 }, left { 1.0 }, right { 1.0 }, drift { 0.0 };
        bool wanted = false;  // within the voice count with a level above -60 dB
        bool running = false; // being computed: wanted, or still fading out
    };

    void configureVoice (int index, bool restart) noexcept;
    void designHighPass (double frequency) noexcept;

    double sampleRate = 48000.0;
    Settings settings;

    PitchShifterInput input;
    PsolaAnalysis analysis;
    bool psolaRunning = false; // the PSOLA voices are computed while any of them can be heard
    std::array<VoiceState, maxVoices> voices;

    juce::SmoothedValue<double> mix { 0.5 }, normalization { 1.0 }, highPassOn { 0.0 }, psolaShare { 0.0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> highPassHz { 100.0 };
    std::array<Svf, 2> highPass;
    int samplesUntilUpdate = 0;
};

} // namespace ampsim

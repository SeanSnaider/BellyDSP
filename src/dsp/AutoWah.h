// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Svf.h"

#include <atomic>

namespace ampsim
{

/// The auto-wah (BUILD_PLAN "Auto-wah"): a mono pre-FX block. A wah pedal is a resonant band-pass whose
/// centre frequency the foot moves; here an envelope follower moves it instead: pick harder and the filter
/// opens, and as the note decays it closes. prototypes/autowah.py is the reference it matches (the golden test
/// in tests/AutoWahTests.cpp), and docs/guides/AUTOWAH_GUIDE.md walks through the design.
///
/// The detector reads the clean DI snapshot (BlockContext::di), never the processed signal, as the gates and the
/// harmonizer do, so the wah reacts to the guitar as played wherever it's dragged in the Pre FX order (after the
/// compressor it would otherwise barely move). With no DI (the block driven alone in tests) it reads its own input.
///
/// 1. The envelope follower, after Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor Design: A
///    Tutorial and Analysis", JAES 60(6), 2012 (the compressor's): rectify, then a branching one-pole,
///        r = |d[n]|,   a = attack if r > e else release,   e = a e + (1 - a) r.
///    Why a = exp(-1 / (tau fs)): from rest, a step from 0 to 1 gives e[n] = 1 - a^n. That reaches 1 - 1/e (63 %)
///    after tau seconds, n = tau fs samples, exactly when a^(tau fs) = 1/e, so a = exp(-1 / (tau fs)). Falling, e
///    decays as a^n and reaches 1/e (37 %) after tau. So the Attack and Release knobs are exact time constants.
///    The coefficients are recomputed only when the knobs change (exp isn't free).
///
/// 2. The level as a control c between 0 (closed) and 1 (open), in dB, because guitar dynamics are logarithmic:
///    a soft note might sit at -40 dBFS and a hard pick at -10, and a linear mapping would only hear the loudest
///    attacks.
///        level_db = 20 log10(e + 1e-9)                      the offset keeps log10 finite in silence (-180 dB)
///        floor_db = -20 - 0.4 sensitivity_percent           -20 dB at 0 %, -40 at the default 50 %, -60 at 100 %
///        c        = clamp((level_db - floor_db) / 30, 0, 1) a fixed 30 dB span from closed to open
///    With Direction Down, c becomes 1 - c: the pick closes the filter, and the decay opens it.
///
/// 3. The exponential sweep: fc = f_low (f_high / f_low)^c. Pitch is exponential, so equal steps of c are equal
///    musical intervals, and c = 0.5 lands on the geometric mean sqrt(f_low f_high), halfway in octaves. A linear
///    sweep would race through the low end and crawl through the top.
///
/// 4. The filter: the trapezoidal SVF's band-pass (Svf.h: Simper, Cytomic 2013; Zavalishin, "The Art of VA Filter
///    Design", ch. 4), H(s) = k s / (s^2 + k s + 1) with k = 1/Q, exactly 0 dB at fc. Why it can be moved in
///    chunks: its state variables are the two integrators' contents (the trapezoidal integrators' memories), which
///    a coefficient change leaves alone, so moving fc changes how the state evolves from here on but never jolts
///    the state itself, and the filter stays stable for any positive g and k at every step. A direct-form biquad's
///    state is past inputs and outputs, which belong to the old coefficients, so a fast sweep can pop or blow up.
///
/// 5. How often fc moves: every redesignInterval samples, from the envelope as it stands at the start of the
///    chunk (Svf::design calls tan, and the mapping log10 and pow). The interval is counted across buffers, so the
///    output doesn't depend on the host's buffer size. Four samples was measured, not guessed (BUILD_PLAN
///    "Auto-wah", the redesign interval; the "Auto-wah" tests' interval study): with a 1 ms attack the envelope
///    moves 28 % of a step in 16 samples, and at 16 (and at 8) the staircase in fc added measurable energy above
///    6 kHz on picked notes; at 4 it doesn't, against redesigning every sample.
///
/// 6. The level hold. The band-pass is 0 dB only at fc, so it keeps a slice of the guitar and drops the rest: at the
///    defaults the DI's loudness fell by 12 to 14 dB (the gain staging audit, 2026-10-15), which would starve the amp
///    after it of drive (a block at its defaults may move the amp's drive by about 1 dB). So the wet is made up:
///        wet = 10^(makeupDb / 20) sqrt(Q / 4) bandpass(x)
///    Why sqrt(Q): for a signal with equal energy per octave (pink, and roughly a guitar), a band-pass's output power
///    is its noise bandwidth in octaves, the integral of |H|^2 over ln(f), which for k s / (s^2 + k s + 1) is about
///    pi / (2 Q). Power falls as 1/Q, so amplitude as 1/sqrt(Q). Measured on the guitar DI through the prototype, the
///    loss followed -10 log10(Q / 4) within 0.3 dB from Q 1 to 12. makeupDb is the loss at Q 4 and the other defaults
///    on the gain staging audit's guitar DI, midway between its -12 and -6 dBFS peaks (14.1 and 12.1 dB: a harder pick
///    opens the filter and reads louder), so both stay within the audit's 1 dB; at -9 dBFS it reads -0.2 dB (the "Gain
///    staging calibration" tests). Like a real wah's, the resonance is a peak above the dry level.
///
/// The mix is (1 - mix) x + mix wet, smoothed per sample; at a settled 0 the dry passes untouched, bit for bit,
/// while the filter and the envelope keep running so turning it up starts from live state. Low, High and Q are
/// smoothed multiplicatively (musical ratios), sensitivity and mix linearly, 20 ms each. Attack, release and
/// direction only change how the envelope moves or is read, never the signal directly, so they aren't smoothed.
///
/// Low above High: the parameter ranges meet at 800 Hz (Low 150 to 800, High 800 to 4000), and the block clamps to
/// the same ranges, so Low never exceeds High. The formula needs no sorting anyway: with Low above High it would
/// simply sweep downwards, Low staying the closed position and High the open one (ASSUMPTIONS AW5).
class AutoWah final : public Block
{
public:
    enum class Direction
    {
        up,  // the pick opens the filter (a wah rocked forward)
        down // the pick closes it: a backwards wah
    };

    /// The parameters (BUILD_PLAN "Auto-wah"; the parameter defaults come from here).
    struct Settings
    {
        float sensitivity = 50.0f; // %: how hard you have to pick to open it
        float freqLowHz = 350.0f;  // the closed position (heel)
        float freqHighHz = 2200.0f; // the open position (toe)
        float q = 4.0f;            // resonance
        float attackMs = 5.0f;
        float releaseMs = 150.0f;
        Direction direction = Direction::up;
        float mix = 1.0f;          // 0 dry, 1 wet
    };

    // The parameter ranges, which the block clamps to as well.
    static constexpr double minSensitivity = 0.0, maxSensitivity = 100.0;
    static constexpr double minLowHz = 150.0, maxLowHz = 800.0;
    static constexpr double minHighHz = 800.0, maxHighHz = 4000.0;
    static constexpr double minQ = 1.0, maxQ = 12.0;
    static constexpr double minAttackMs = 1.0, maxAttackMs = 50.0;
    static constexpr double minReleaseMs = 20.0, maxReleaseMs = 500.0;

    // The level mapping (2) and the redesign interval (5).
    static constexpr double spanDb = 30.0;
    static constexpr double floorAtZeroDb = -20.0, floorPerPercentDb = 0.4;
    static constexpr double levelOffset = 1.0e-9;
    static constexpr int redesignInterval = 4;
    static constexpr double smoothingSeconds = 0.020;

    // The level hold (6): the makeup at the reference Q, and the reference.
    static constexpr double makeupDb = 13.1;
    static constexpr double makeupReferenceQ = 4.0;

    /// The wet's gain for a resonance (6): makeupDb at Q 4, rising as sqrt(Q).
    static double wetGain (double q) noexcept;

    /// The envelope (linear) to the control between 0 (closed) and 1 (open), Direction applied (2).
    static double controlFor (double envelope, double sensitivityPercent, Direction direction) noexcept;

    /// The control to the centre frequency (3): low at 0, high at 1, the geometric mean at 0.5.
    static double centreFrequency (double control, double lowHz, double highHz) noexcept;

    /// Audio thread, once per buffer before process().
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    // latencySamples() and isStereo() keep Block's defaults: 0 and false (a mono pre-FX block).

    /// The thread that calls process(), for tests: the envelope (linear) after the last sample, and the centre
    /// frequency the filter is designed at now.
    double getEnvelope() const noexcept { return envelope; }
    double getCentreFrequency() const noexcept { return centre; }

    /// Any thread: the control (0 closed, 1 open, Direction applied) at the end of the last buffer, for a display.
    float getControl() const noexcept { return meterControl.load (std::memory_order_relaxed); }

    /// For the redesign interval study only (tests/AutoWahTests.cpp): redesign every `samples` samples instead
    /// of redesignInterval. The app never calls it.
    void setRedesignIntervalForStudy (int samples) noexcept;

private:
    double coefficient (double milliseconds) const noexcept;
    void updateTimeConstants() noexcept;
    void redesign() noexcept;

    double sampleRate = 48000.0;
    Settings settings;
    Svf filter;

    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> freqLow { 350.0 }, freqHigh { 2200.0 }, q { 4.0 };
    juce::SmoothedValue<double> sensitivity { 50.0 }, mix { 1.0 };

    double attack = 0.0, release = 0.0; // one-pole coefficients
    double envelope = 0.0;              // the follower's state, linear
    double centre = 350.0;              // the filter's current centre frequency
    double control = 0.0;               // the control at the last redesign
    double gain = 1.0;                  // the wet's level hold at the current Q (6)
    int interval = redesignInterval;
    int untilRedesign = 0;              // samples left in the current chunk (counted across buffers)

    std::atomic<float> meterControl { 0.0f };
};

} // namespace ampsim

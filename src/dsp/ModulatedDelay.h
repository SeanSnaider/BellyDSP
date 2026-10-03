// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "DelayLine.h"
#include "Lfo.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace ampsim
{

/// The shared modulated-delay engine (BUILD_PLAN "Shared modulated-delay engine"): one channel of a delay
/// line read by up to four "voices", each swept by its own LFO. Chorus, vibrato, the flanger (Phase 8), and
/// the granular pitch shifter (Phase 9) are all this machine with different settings, so it's built once:
///   chorus     1 or 2 voices per channel (Tri's three spread over both), 6 to 15 ms, no feedback (Chorus.h)
///   vibrato    1 voice, heard 100% wet
///   flanger    1 voice, 0.5 to 10 ms, feedback
///   granular   3 to 4 heads swept by sawtooths with their own windows: it drives read() and write()
///              directly with delays it computes itself
///
/// Each voice v reads the line at
///     d_v(t) = base_v + depth_v * m_v(t)        m_v = the voice's LFO in [-1, 1], optionally inverted
/// between samples by 4-point Hermite interpolation (DelayLine.h), and the voices are summed with their
/// levels. Optional feedback sends that sum back into the line through a soft clip:
///     w[n] = x[n] + tanh(feedback * wet[n]),    wet[n] = sum_v level_v * w[n - d_v(n)]
/// |tanh| < 1, so whatever the feedback, |w| < max|x| + 1: the loop can't run away. (A Hermite read can
/// overshoot its samples by up to 1.25x, the sum of its weights' magnitudes at t = 0.5, so a voice's tap
/// stays below 1.25 (max|x| + 1).)
///
/// What the sweep does to pitch (the Doppler view): reading y(t) = x(t - d(t)) maps input time to output
/// time with slope 1 - d'(t), so a component at frequency f comes out at
///     f_out(t) = f (1 - d'(t))
/// A sine LFO of depth A seconds at rate r, d = d0 + A sin(2 pi r t), has d' = 2 pi r A cos(2 pi r t), so
/// the pitch swings smoothly between f (1 - 2 pi r A) and f (1 + 2 pi r A). A triangle has constant slope
/// 4 r A in each half cycle, so the pitch sits at f (1 - 4 r A) or f (1 + 4 r A), switching at the corners.
/// Example: a 2 ms triangle at 1 Hz holds +-0.8%, about +-14 cents. An oscillating read head only wobbles
/// pitch around the original; a steady shift needs the granular shifter's sawtooth heads (BUILD_PLAN,
/// correction from design review round 13).
///
/// Reading before writing: each sample the voices read the line as it stands, whose newest entry is the
/// previous input, and then the new input (plus feedback) is written. So read(d) is the input d samples
/// before the one about to be written. Feedback needs this order, since the loop has to read before it
/// writes. Hermite needs a sample on each side of the read position, which the newest stored sample only
/// provides from d = 2 on, so delays are clamped to at least 2 samples (42 us at 48 kHz).
///
/// Smoothing: base delay, depth, rate, and feedback glide to new settings over the time given to
/// prepare(); a jump in delay would be a click, and even a glide bends pitch while it moves (a 4 ms change
/// over 100 ms is a 4% bend while it lasts). A change in the number of voices fades voices in or out over
/// 20 ms, and a voice that starts joins in step with the others: at voice 0's phase plus its own offset.
/// A shape change applies at once (the LFO value jumps), so a caller that changes shape while the engine is
/// audible crossfades to a second engine, as the chorus does. Phase offsets apply when a voice starts.
class ModulatedDelay
{
public:
    static constexpr int maxVoices = 4;
    static constexpr double minDelaySamples = 2.0;
    static constexpr double voiceFadeSeconds = 0.020;
    static constexpr double maxFeedback = 0.99;

    struct Voice
    {
        double baseDelayMs = 10.0; // the centre of the sweep
        double depthMs = 0.0;      // half the sweep: the delay moves over base +- depth
        Lfo::Shape shape = Lfo::Shape::triangle;
        double rateHz = 1.0;
        double phase = 0.0;        // the LFO's offset in cycles (1/3 = 120 degrees)
        bool inverted = false;     // the LFO upside down: exact antiphase for every shape, random included
        juce::int64 seed = 1;      // the random shape's sequence (an inverted partner shares its seed)
        double level = 1.0;        // the voice's gain in the wet sum
    };

    struct Settings
    {
        int numVoices = 1;
        std::array<Voice, maxVoices> voices {};
        double feedback = 0.0; // fraction of the wet sum fed back, -0.99 to 0.99, through tanh

        /// Above 0, a first-order high-pass at this frequency inside the feedback loop, ahead of the tanh:
        ///     w[n] = x[n] + tanh(feedback * HP(wet[n]))
        /// so low frequencies don't recirculate and build up (the flanger's loop, Flanger.h). The high-pass
        /// is a TPT one-pole (Zavalishin, "The Art of VA Filter Design", ch. 3), H(s) = s / (s + wc),
        /// prewarped with g = tan(pi fc / fs). 0, the default, leaves the loop exactly as it was.
        double feedbackHighPassHz = 0.0;
    };

    /// Not real-time: allocates the line for delays up to maxDelayMs. smoothingSeconds is how long base
    /// delay, depth, rate, and feedback take to reach a new setting.
    void prepare (double sampleRate, double maxDelayMs, double smoothingSeconds);

    /// Clears the line and restarts every voice at phase 0 plus its offset, with no ramps. Allocation-free.
    void reset();

    /// Audio thread, once per buffer: the new targets.
    void setSettings (const Settings& settings);

    /// Restarts every wanted voice's LFO at `phase` plus its offset (reseeding the random shape) and jumps
    /// every ramp to its target, without touching the line. For an engine that is silent and about to fade
    /// in, so it continues another engine's LFO cycle.
    void restart (double phase);

    /// One sample: the voices' wet sum for input x. x (plus feedback) is written after the voices read.
    float processSample (float x) noexcept;

    /// The line's input `delaySamples` before the sample about to be written, by Hermite interpolation,
    /// clamped to [2, getMaxDelaySamples()]. With write(), the low-level path for layers that compute their
    /// own read positions.
    float read (double delaySamples) const noexcept;

    /// Writes the next sample without running the voices (keeps a silent engine's line current).
    void write (float x) noexcept { line.write (x); }

    double getMaxDelaySamples() const noexcept { return maxDelay; }

    /// A voice's delay in samples at the last processSample(): the measured LFO path for tests and meters.
    double getVoiceDelay (int voice) const noexcept { return voices[(size_t) voice].delay; }
    bool isVoiceRunning (int voice) const noexcept { return voices[(size_t) voice].running; }

    /// The engine's LFO cycle position: the first running voice's phase minus its offset, in [0, 1).
    double getPhase() const noexcept;

    /// The feedback amount used for the last processSample() (it glides to a new setting), for a caller that
    /// scales its wet by it, as the flanger's level compensation does.
    double getFeedback() const noexcept { return feedback.getCurrentValue(); }

private:
    struct VoiceState
    {
        Lfo lfo;
        juce::SmoothedValue<double> base { 0.0 }, depth { 0.0 }, level { 0.0 }; // samples, samples, gain
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> rate { 1.0 };
        double offset = 0.0;
        bool inverted = false;
        juce::int64 seed = 1;
        bool wanted = false;  // within numVoices with a level above 0
        bool running = false; // being computed: wanted, or still fading out
        double delay = 0.0;
    };

    double tap (double delay) const noexcept { return (double) line.read (delay - 1.0); }

    Settings settings;
    DelayLine line;
    std::array<VoiceState, maxVoices> voices;
    juce::SmoothedValue<double> feedback { 0.0 };
    double sampleRate = 48000.0;
    double maxDelay = 2048.0;
    double restartPhase = 0.0;

    // The optional loop high-pass (Settings::feedbackHighPassHz): G = g / (1 + g), and the integrator's state.
    bool loopHighPass = false;
    double loopHighPassG = 0.0, loopHighPassState = 0.0;
};

} // namespace ampsim

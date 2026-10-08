// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "AmpTone.h"
#include "Block.h"
#include "Gain.h"
#include "NamAmp.h"

#include <array>

namespace ampsim
{

/// The amp: nine amps, of which only the selected one runs (BUILD_PLAN "Amp switching", Sean's decision of
/// 2026-10-07). The first eight are the built-in amps, in the shelf's order (Glass, Ember, Monolith, Lantern,
/// Basalt, Comet, Forge, Quartz; the processor loads their gain sets), the ninth the user's own capture. Each
/// keeps its own knobs, so switching away and back finds them as they were.
///
/// Each amp: the capture (with its Gain knob) -> tone controls -> output trim.
///
/// A switch. Until 2026-10-07 the three amp slots all ran all the time, so a switch could crossfade at once
/// to a model whose history was current. Now an amp that isn't selected doesn't run at all, so its models'
/// history (their receptive field: the past samples each output sample depends on) is stale. A switch
/// therefore goes in two stages:
///
///   1. Warm-up, unheard: the incoming amp starts running on the live input while the outgoing one keeps
///      playing alone. NAM's WaveNet has finite memory, so once a model has run on live input for its
///      receptive field (NamAmp::getWarmupSamples(): the prewarm length, 4093 samples for a standard
///      WaveNet), its output is exactly what it would have been had it run all along. The warm-up is counted
///      in whole buffers (4096 samples at 128, 85.3 ms).
///   2. The crossfade: 20 ms, equal power, built from independent linear ramps as before. Each amp has a
///      position p ramping toward 1 (heard) or 0, and contributes sin(pi/2 p) of its output, which for a plain
///      A -> B switch is cos(pi/2 t) and sin(pi/2 t), whose squares sum to 1, so the level doesn't dip midway.
///      Then the outgoing amp stops.
///
/// So a switch never clicks, but it lands a warm-up late (about 0.1 s). A new switch during the warm-up
/// retargets: the amp warming stops and the newest one starts warming (or, if the newest is the amp being
/// heard, nothing more happens). An amp still running (the one fading out, say) is warm already, so
/// switching back to it redirects the ramps at once with no jump. Steady state runs one amp: one model on a
/// gain set's step, two between steps, and up to three while its Gain moves (NamAmp); a switch adds the
/// incoming amp for the warm-up and the fade.
class AmpSection : public Block
{
public:
    /// The eight built-in amps and the user's capture (AmpSimProcessor::yourCaptureAmp is the last).
    static constexpr int numAmps = 9;
    static constexpr double switchSeconds = 0.020;

    /// An amp's Gain knob, as its parameter stores it: amp*_input_trim, in dB from -24 to +24, which the
    /// head shows as 0 to 10 (position = 5 + dB / 4.8). The parameter ID (and this member's name) date
    /// from when Gain was always an input trim; it's permanent, so the meaning moved instead (BUILD_PLAN
    /// "Amp gain"): the amp's NamAmp turns the position into a place across a gain set's steps, or into a
    /// loudness-compensated input trim for a single capture, and does the smoothing. Audio thread.
    class GainKnob
    {
    public:
        void setGainDecibels (float db) noexcept { position = positionForDb (db); }
        void setPosition (float newPosition) noexcept { position = juce::jlimit (0.0f, NamAmp::gainMax, newPosition); }
        float getPosition() const noexcept { return position; }

        static float positionForDb (float db) noexcept
        {
            return juce::jlimit (0.0f, NamAmp::gainMax, NamAmp::gainDefault + db / (NamAmp::singleTrimRangeDb / NamAmp::gainDefault));
        }
        static float dbForPosition (float p) noexcept { return NamAmp::singleTrimDb (p); }

    private:
        float position = NamAmp::gainDefault;
    };

    struct Amp
    {
        GainKnob inputTrim; // the Gain knob (see GainKnob)
        NamAmp model;
        AmpTone tone;
        Gain outputTrim { false };
    };

    Amp& amp (int index) { return amps[(size_t) index]; }
    const Amp& amp (int index) const { return amps[(size_t) index]; }

    /// Audio thread (or before prepare()). Switches to this amp (0-based): it warms up unheard, then the output
    /// crossfades to it over 20 ms. Already the newest request: nothing happens.
    void selectAmp (int index);

    /// The amp most recently selected (the one warming up, if a switch is under way).
    int getSelectedAmp() const noexcept { return selected; }

    /// Audio thread, once per buffer: the CPU saver's Gain (BUILD_PLAN "CPU", the CPU saver). On, the amp heard plays the
    /// gain set's step nearest its Gain, alone (NamAmp::Blend::nearest: one model), and a Gain crossing a midpoint
    /// crosses to the next step as an unheard amp's does (the new step warms, then the position moves at the Gain's
    /// slew under the blend law). Off, it blends exactly (two models between steps). Either way round the position
    /// moves at the slew, so the change never jumps. A single capture is unaffected.
    void setNearestStepOnly (bool nearestOnly) noexcept { nearestStepOnly = nearestOnly; }
    bool isNearestStepOnly() const noexcept { return nearestStepOnly; }

    /// The amp being heard, or fading in: the selected one once its warm-up is done.
    int getHeardAmp() const noexcept { return heard; }

    /// Audio thread, for tests and meters: true while a switch is warming up or fading.
    bool isSwitching() const noexcept { return isWarmingUp() || isFading(); }
    bool isWarmingUp() const noexcept { return selected != heard; }
    bool isFading() const noexcept;

    /// Audio thread, for tests and meters: whether an amp ran in the last buffer, how many did, and how many
    /// NAM models they ran in all (NamAmp::getRunningSteps()).
    bool isRunning (int index) const noexcept { return state[(size_t) index].running; }
    int getRunningAmps() const noexcept;
    int getRunningModels() const noexcept;

    /// Audio thread, for tests: true while any amp is fading in a newly loaded model.
    bool isLoadingModel() const noexcept;

    /// Audio thread, for tests: an amp's own output in the last buffer (after its tone and trim, before the
    /// switch's fade), getNumSamples() of the last process() long. Only meaningful for an amp that ran.
    const float* getAmpOutput (int index) const noexcept { return ampOutputs.getReadPointer (index); }

    /// The warm-up a switch to this amp waits for, in samples: its model's receptive field rounded up to whole
    /// buffers of `blockSize` (0 for an amp with no capture, which passes the input through).
    int warmupSamplesFor (int index, int blockSize) const noexcept;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;

private:
    struct State
    {
        bool running = false;
        int warmed = 0; // samples run since it last started (capped)
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> position; // 1 = heard, 0 = silent
    };

    static constexpr int maxWarmed = 1 << 30;

    void start (int index) noexcept;

    std::array<Amp, numAmps> amps;
    std::array<State, numAmps> state;
    juce::AudioBuffer<float> ampOutputs; // one channel per amp
    int selected = 0; // the newest request
    int heard = 0;    // the amp whose position heads for 1
    bool nearestStepOnly = false; // the CPU saver (setNearestStepOnly)
};

} // namespace ampsim

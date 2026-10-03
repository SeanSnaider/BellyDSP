// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "DriveEngine.h"

namespace ampsim
{

/// The overdrive (BUILD_PLAN "Boost and Overdrive"): a mono pre-FX block on the shared drive engine.
///
/// Modes, each a circuit model in DriveCircuits.h (generic names in the UI; the circuits they model are
/// named only in docs and comments):
///   - Mid Drive: Tube Screamer-style (TS808). Diodes in the op-amp's feedback loop, so the clipped signal
///     is added to the clean one: soft clipping, a mid hump, and bass that passes clean.
///   - Distortion: RAT-style. A slow LM308 op-amp at up to 67 dB of gain (its slew rate and bandwidth are
///     modeled), then hard-clipping diodes to ground and a passive low-pass filter.
///   - Transparent: Klon-style. A gain stage into germanium diodes, summed with two clean paths whose
///     share the Drive knob turns down as it turns the gain up, then an active treble control.
///   - Fuzz: Big Muff-style. Four transistor stages: a booster, two clipping stages with diodes in their
///     feedback, the passive mid-scooping tone stack, and an output booster.
/// Adding a mode is adding a drive::Circuit and an entry here.
///
/// Controls: Drive and Tone are the pedal's own pots (audio taper where the original is); Level is a gain
/// after the circuit's output with its volume pot at maximum; Mix blends the circuit with the dry signal
/// (phase-aligned, see DriveEngine); Tight is a 12 dB/oct high-pass before the circuit (off at 20 Hz).
/// Mode switches crossfade over 20 ms.
class Overdrive : public Block
{
public:
    /// The processor maps the od_mode choice index straight onto this: append new modes, never reorder.
    enum class Mode
    {
        midDrive,
        distortion,
        transparent,
        fuzz
    };

    struct Settings
    {
        Mode mode = Mode::midDrive;
        float drive = 0.5f;
        float tone = 0.5f;
        float levelDb = 0.0f;
        float mix = 1.0f;
        float tightHz = DriveEngine::tightOffHz;
        int oversampling = 4;
        double voltsAtFullScale = drive::defaultVoltsAtFullScale;
    };

    Overdrive();

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;

    DriveEngine& getEngine() noexcept { return engine; }

private:
    DriveEngine engine;
};

} // namespace ampsim

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "GainSet.h"
#include "Handoff.h"

#include <NAM/dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace ampsim
{

/// One amp slot: a capture running on NeuralAmpModelerCore, the official NAM engine. The capture is a
/// single .nam file or a gain set (GainSet.h: several captures of one amp across its gain knob), and the
/// slot's Gain knob (setGain(), 0 to 10) means something different for each (BUILD_PLAN "Amp gain"):
///
///   A gain set: Gain chooses a position across the steps. On a step, that step's model plays alone; between
///   two steps, both run and their outputs blend (render(), the blend law). Every step is loudness-normalized,
///   so Gain changes the saturation, not the volume.
///
///   A single capture: Gain is the model's input trim, 0 to 10 over -24 to +24 dB (5 is unity), and it's
///   loudness-compensated: on load, the capture's output loudness is measured at every 3 dB of trim, and
///   the output is turned by the inverse, so Gain changes the drive and not the volume. A single capture
///   can't clean up or gain up beyond what it was trained on; this only keeps the level steady.
///
/// Threads:
///   loadModel() runs on a background thread. It parses the files, builds the networks, allocates their
///   buffers, measures and prewarms them, then queues the finished capture for the audio thread.
///   process() runs on the audio thread. When a new capture is waiting, the old and new outputs are
///   crossfaded over 20 ms (both run during the fade), and the old one goes back through the Handoff to be
///   freed off the audio thread.
///
/// With no capture loaded, the slot passes its input through unchanged.
class NamAmp : public Block
{
public:
    /// NAM captures are trained at 48 kHz, and v1 only runs at that rate (BUILD_PLAN "Sample rate").
    static constexpr double requiredSampleRate = 48000.0;

    /// Every model is normalized so the reference guitar DI comes out at this loudness (BS.1770 LUFS).
    static constexpr double targetLoudnessLufs = -18.0;

    /// Seconds of the reference guitar DI rendered through each model to measure its loudness.
    static constexpr double loudnessProbeSeconds = 4.0;

    static constexpr double switchFadeSeconds = 0.020;

    // ---- The Gain knob -------------------------------------------------------------------------------

    /// Gain positions run 0 to 10; 5 is the default (and a single capture's unity trim).
    static constexpr float gainMax = 10.0f, gainDefault = 5.0f;

    /// A single capture's trim range: 0 to 10 is -24 to +24 dB, 4.8 dB per unit.
    static constexpr float singleTrimRangeDb = 24.0f;
    static float singleTrimDb (float position) noexcept { return (position - gainDefault) * (singleTrimRangeDb / gainDefault); }

    /// The single capture's loudness compensation is measured at every 3 dB of trim (17 points), on this
    /// many seconds of the reference DI (one pass of its 2 s phrase: 1 s, half of it, left A2.nam 2 LU off on a
    /// longer DI; the whole phrase keeps the example models within 0.2 LU), once
    /// per file (NamAmp.cpp keeps the curves).
    static constexpr int compensationPoints = 17;
    static constexpr float compensationStepDb = 3.0f;
    static constexpr double compensationProbeSeconds = 2.0;

    /// The Gain position moves toward the knob at most this fast (positions per second): 0 to 10 in 0.4 s.
    /// It's the knob's smoothing: a linear ramp, like the other knobs', but rate-limited rather than
    /// time-limited so that a set's next model always has time to warm up before it's needed (one step,
    /// 2.5 positions, takes 100 ms; a standard WaveNet's receptive field is 85 ms).
    static constexpr float gainSlewPerSecond = 25.0f;

    /// A set runs at most this many of its step models at once: the one or two it's blending, plus the
    /// next one or two in the direction the knob is moving, warming up hidden.
    static constexpr int maxRunningSteps = 3;

    /// Input calibration (BUILD_PLAN "Input level"). A capture's metadata can say what analog level
    /// reached 0 dBFS on the rig it was trained with (`input_level_dbu`). If our interface's 0 dBFS is
    /// a different analog level, the same guitar arrives at a different digital level than the model
    /// learned from, and it distorts more or less than the real amp would. A signal at V dBu reaches
    /// the model as V - interfaceDbu dBFS, but the model was trained on V - captureDbu dBFS, so the
    /// model's input gets interfaceDbu - captureDbu dB of gain to make up the difference.
    struct Calibration
    {
        bool enabled = false;
        double interfaceInputDbu = 12.0; // the analog level of a full-scale (0 dBFS) input
    };

    struct LoadResult
    {
        bool ok = false;
        juce::String message;          // why it failed, or a short description of what loaded
        double normalizationDb = 0.0;  // gain applied to the model's output (a set: its first step's)
        double measuredLufs = 0.0;     // the model's output loudness on the reference DI, before normalizing
        double calibrationDb = 0.0;    // gain applied to the model's input (0 without calibration)
        bool hasInputLevel = false;    // the file says what level it was captured at
        double captureInputDbu = 0.0;

        bool isGainSet = false;
        std::vector<double> stepGains;        // a set: each step's Gain position
        std::vector<double> stepLufs;         // a set: each step's loudness on the reference DI, before normalizing
        std::vector<double> stepCorrelation;  // a set: the normalized outputs' correlation, step i with step i + 1
        std::vector<double> compensationDb;   // a single capture: the output gain at trims -24, -21, ... +24 dB
    };

    NamAmp() = default;
    ~NamAmp() override;

    /// Background thread; takes tens to hundreds of milliseconds (a second or two for a single capture's
    /// compensation, or a five-step set). `file` is a .nam, or a gain set's JSON or its folder. Pass
    /// normalize = false to hear (or test) the raw model level: no loudness normalization and no
    /// compensation. The loudness measurement includes the calibration gain, so a calibrated slot still
    /// lands at the target loudness.
    LoadResult loadModel (const juce::File& file, bool normalize, const Calibration& calibration);
    LoadResult loadModel (const juce::File& file, bool normalize = true); // without calibration

    /// Background thread: empties the slot (crossfades to passthrough like any model change).
    void clearModel();

    /// Any non-audio thread. Frees models the audio thread has finished with.
    void collectGarbage() { handoff.collect(); }

    /// Audio thread, once per buffer before process(): the Gain knob's position, 0 to 10.
    void setGain (float position) noexcept { gainTarget = juce::jlimit (0.0f, gainMax, position); }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;

    /// Real-time safe, so it can't clear the model's history: NAM's Reset() allocates. It only
    /// finishes a switch that's in progress.
    void reset() override;

    /// Audio thread, for tests and meters: true while a model switch is crossfading.
    bool isSwitching() const noexcept { return fading; }

    /// Audio thread, for tests: whether a model is running (false = passthrough).
    bool hasModel() const noexcept { return current != nullptr && ! current->steps.empty(); }

    /// Audio thread, for tests and meters: whether the capture is a gain set, its effective Gain position
    /// (where the smoothing has got to; -1 before it first runs), whether that's still moving toward the
    /// knob, and how many step models ran in the last buffer (the current capture's; 0 for passthrough).
    bool hasGainSet() const noexcept { return current != nullptr && current->steps.size() > 1; }
    float getGainPosition() const noexcept { return current != nullptr ? current->position : -1.0f; }
    bool isGainMoving() const noexcept;
    int getRunningSteps() const noexcept;

    /// For tests: the effective Gain position at each sample of the last buffer that rendered the current
    /// capture's Gain moving (a set writes it every buffer; a single capture only while it moves).
    const std::vector<float>& getLastPositions() const noexcept { return positions; }

private:
    struct Step
    {
        std::unique_ptr<nam::DSP> dsp;
        float normalizationGain = 1.0f; // linear, on the output
        float position = gainDefault;   // a set: its Gain position
        int warmed = 0;                 // audio thread: samples run since it last started
        bool running = false;           // audio thread
    };

    struct Model
    {
        std::vector<Step> steps;         // empty: passthrough; one: a single capture; more: a gain set
        std::vector<float> correlation;  // a set: step i's normalized output with step i + 1's
        float inputGain = 1.0f;          // linear, the input calibration
        std::array<float, compensationPoints> compensationDb {}; // a single capture, at trims -24 .. +24 dB
        int warmupSamples = 1;           // a set: how long a step must run before it can be heard
        float position = -1.0f;          // audio thread: the effective Gain position (-1: not run yet)
    };

    static constexpr int maxWarmed = 1 << 30;

    void pickUpNewModel();
    void finishSwitchIfDone();
    void render (Model* model, const float* input, float* output, int numSamples);
    void renderSingle (Model& model, const float* input, float* output, int numSamples);
    void renderSet (Model& model, const float* input, float* output, int numSamples);
    void startModel (Model& model) noexcept;
    float slewTarget (float position, float target) const noexcept;

    Handoff<Model> handoff;

    // Audio thread state.
    Model* current = nullptr;   // nullptr = passthrough
    Model* fadingOut = nullptr; // the outgoing model (nullptr when fading in from passthrough)
    Model* toRetire = nullptr;  // done with, waiting for the retire slot to free up
    bool fading = false;
    int fadePosition = 0;
    int fadeLength = 960;
    float gainTarget = gainDefault;
    float slewPerSample = gainSlewPerSecond / (float) requiredSampleRate;

    std::vector<float> inputCopy, outgoing, scaledInput, positions;
    juce::AudioBuffer<float> stepOutputs; // one channel per step of a set (GainSet::maxSteps)

    // Written by prepare(), read by the loader thread.
    std::atomic<int> loaderMaxBlockSize { 4096 };
};

} // namespace ampsim

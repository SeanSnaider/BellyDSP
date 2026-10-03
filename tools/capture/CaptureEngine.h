// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

// The play-and-record engine behind ampsim_capture (docs/CAPTURING.md), kept apart from the audio device
// so the tests can run it against a simulated one.
//
// A capture plays NAM's standard training file (input.wav, "v3.0.0") out of the interface into the gear
// and records what comes back. NAM's trainer then needs the recording ("output.wav") to have exactly the
// input's sample rate and length, starting at the same moment, so the engine does both in the same
// audio callback: sample n of the input goes out in the same callback that records sample n of the
// return. The round trip through the converters and the gear shows up as a constant delay inside the
// recording, which the trainer measures from the file's calibration blips and removes itself.

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>
#include <vector>

namespace capture
{

constexpr double sampleRate = 48000.0;

/// NAM's input file, version 3.0.0 (neural-amp-modeler's train/core.py, _V3_DATA_INFO): 190 s at 48 kHz.
///   0 to 9 s validation, 9 to 10 s silence, blips (single-sample impulses) at 10.5 and 11.5 s,
///   12 to 15 s chirps, 15 to 17 s noise, 17 to 180.5 s training audio, 180.5 to 181 s silence,
///   181 to 190 s the validation again.
namespace v3
{
constexpr int length = 9'120'000;
constexpr int blips[] { 504'000, 552'000 };
constexpr int noiseStart = 492'000, noiseEnd = 498'000;  // the silence the trigger level is measured on
constexpr int levelCheckLength = 17 * 48'000;            // through the noise burst: everything but the training audio
} // namespace v3

/// Plays a stimulus out of one output channel and records one input channel, sample for sample, in
/// the same callback. Everything is allocated in prepare(); process() only indexes into it.
class Engine
{
public:
    /// Message thread, before the device starts. Plays the first `samplesToPlay` samples of `stimulus`
    /// (all of it if 0) scaled by `outputGain`, and records as many.
    void prepare (const std::vector<float>& stimulus, float outputGain, int samplesToPlay = 0);

    /// Audio thread, once per callback: `input` is the return channel (may be null: records silence),
    /// `output` the channel the stimulus goes out of (may be null). Once everything has played, it
    /// outputs silence and records nothing more. No allocation, no locks.
    void process (const float* input, float* output, int numSamples) noexcept;

    bool isDone() const noexcept { return position.load (std::memory_order_acquire) >= total; }
    int getPosition() const noexcept { return position.load (std::memory_order_acquire); }
    int getLength() const noexcept { return total; }

    /// Any thread: the return's peak since the last call (for a running level display), and the number
    /// of recorded samples at or above full scale so far.
    float takePeak() noexcept { return runningPeak.exchange (0.0f); }
    int getClippedSamples() const noexcept { return clipped.load(); }

    /// After isDone(): the recording, exactly getLength() samples.
    const std::vector<float>& getRecording() const noexcept { return recording; }

private:
    std::vector<float> played, recording;
    int total = 0;
    std::atomic<int> position { 0 };
    std::atomic<float> runningPeak { 0.0f };
    std::atomic<int> clipped { 0 };
};

/// Runs an engine to the end against a simulated device instead of an audio interface, in callbacks of
/// `blockSize` samples, as the real device would: `device` receives what the engine played into the
/// gear this callback and writes what came back (the same number of samples), keeping its own state
/// (a delay line, a circuit) between calls. Returns the number of callbacks.
int simulate (Engine& engine, int blockSize, const std::function<void (const float* played, float* returned, int numSamples)>& device);

struct Levels
{
    float peakDb = -200.0f, rmsDb = -200.0f;
    int clippedSamples = 0; // at or above 0 dBFS (|x| >= 0.99997, the largest 24-bit value)
};
Levels measure (const float* samples, int numSamples);

/// The round trip in samples, measured the way NAM's trainer does (train/core.py,
/// _calibrate_latency_v_all, version 3 data): the recording around both blips (1000 samples before to
/// 10000 after each) is averaged, and the first sample whose magnitude rises above the silence's
/// (max(noise + 0.0003, 1.001 noise), noise being the largest magnitude in the silence just before) is
/// where the gear answered. Returns -1 if nothing answered (or the recording is too short).
/// NAM's trainer measures this itself (and adds a safety margin); this is only reported.
int measureLatency (const std::vector<float>& recording);

/// Reads a mono 48 kHz WAV into floats. Empty, with `error` set, if it isn't one.
std::vector<float> readStimulus (const juce::File& file, juce::String& error);

/// Writes mono 24-bit PCM at 48 kHz (what NAM's trainer reads: it uses wavio, which reads PCM).
bool writeRecording (const juce::File& file, const std::vector<float>& samples, juce::String& error);

} // namespace capture

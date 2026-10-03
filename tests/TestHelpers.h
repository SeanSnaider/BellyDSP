// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <functional>
#include <vector>

namespace testing
{

constexpr double fs = 48000.0;
constexpr int blockSize = 128;
constexpr double deadlineMicros = 1.0e6 * blockSize / fs; // 2666.7 us per 128-sample block

// ---- Files ----------------------------------------------------------------------------------
juce::File namDir();
juce::File exampleModel (const juce::String& fileName);
juce::File exampleInputFile(); // NAM core's 2 s example DI, 48 kHz mono
juce::File namRenderTool();    // NAM core's own render tool, built from its source
juce::File& proofDir();        // where proof artifacts go (set from the command line)
juce::File tempDir();

juce::AudioBuffer<float> readWav (const juce::File& file, double* sampleRateOut = nullptr);
bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate = fs);
bool writeWav (const juce::File& file, const std::vector<float>& mono, double sampleRate = fs);

// ---- Signals --------------------------------------------------------------------------------
std::vector<float> sine (double frequency, double amplitude, int numSamples);
std::vector<float> whiteNoise (int numSamples, float amplitude, juce::int64 seed);
/// Pink noise (Paul Kellet's refined filter: within ~0.05 dB of -3 dB/octave above 10 Hz). For comparisons.
std::vector<float> pinkNoise (int numSamples, juce::int64 seed = 1);
/// NAM core's example_audio/input.wav: 1 s of digital silence, then 1 s of a steady -9 dB RMS test
/// tone. Not a guitar. Looped if minimumSamples is longer. Use guitarDI() for anything musical.
std::vector<float> exampleInput (int minimumSamples = 0);

/// A deterministic stand-in for a guitar DI, peaking at -6 dBFS: Karplus-Strong plucked strings (the
/// idea behind synth_test_riff() in prototypes/amp_sim.py) playing a 2 s phrase on repeat: palm-muted
/// low E chugs, an E5 power chord, a run on the G string, an A5. Gaps are at most 30 ms.
std::vector<float> guitarDI (int numSamples);

/// A broadband stimulus for differential tests: 2 s of guitarDI, a 2 s logarithmic sweep from 20 Hz to
/// 20 kHz at -12 dBFS, 0.5 s of white noise at -20 dBFS, and 0.25 s of silence.
std::vector<float> richStimulus();

/// The crude 4x12 "cab" from prototypes/amp_sim.py (90 Hz high-pass, +3 dB at 120 Hz, -3 dB at
/// 400 Hz, +4 dB at 2.5 kHz, two 5 kHz low-passes, all RBJ cookbook biquads) rendered as an
/// impulse response. A stand-in for a real cab IR in tests.
std::vector<double> syntheticCabIR (int length = 4096, double presenceDb = 4.0, double lowpassHz = 5000.0);
std::vector<double> unitEnergy (const std::vector<double>& h);
juce::AudioBuffer<float> toBuffer (const std::vector<double>& samples);
juce::AudioBuffer<float> toBuffer (const std::vector<float>& samples);

/// y[n] = sum_k h[k] x[n-k] in double precision, the brute-force reference for the cab.
std::vector<double> directConvolution (const std::vector<float>& x, const std::vector<double>& h);

// ---- Measurements ---------------------------------------------------------------------------
double rms (const float* x, size_t n);
double rms (const std::vector<float>& x);
double toDb (double linear);
double maxAbsDifference (const std::vector<float>& a, const std::vector<float>& b);
double maxAbsDifference (const std::vector<float>& a, const std::vector<double>& b);
/// RMS of (actual - expected) relative to the RMS of expected, in dB.
double relativeErrorDb (const std::vector<float>& actual, const std::vector<double>& expected);
double relativeErrorDb (const std::vector<float>& actual, const std::vector<float>& expected);
/// Largest sample-to-sample jump in [start, end). A click shows up as an outlier here.
double maxStep (const std::vector<float>& x, size_t start = 0, size_t end = SIZE_MAX);

juce::String dB (double value);
juce::String micros (double value);

// ---- Running audio through blocks -------------------------------------------------------------
struct Stereo
{
    std::vector<float> left, right;
};

/// Feeds a mono signal through process() in fixed-size buffers. Both channels of each buffer start
/// as a copy of the input. The callback gets the buffer and its first sample's index.
Stereo runInBlocks (const std::vector<float>& input, int bufferSize,
                    const std::function<void (juce::dsp::AudioBlock<float>&, size_t start)>& process);

} // namespace testing

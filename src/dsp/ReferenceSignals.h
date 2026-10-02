#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace ampsim
{

/// A deterministic stand-in for a guitar DI, peaking at -6 dBFS: Karplus-Strong plucked strings (the
/// idea behind synth_test_riff() in prototypes/amp_sim.py) playing a 2 s phrase on repeat: palm-muted
/// low E chugs, an E5 power chord, a run on the G string, an A5. Gaps are at most 30 ms. Different
/// seeds give different plucks of the same phrase.
///
/// Karplus-Strong: a burst of noise circulates in a delay line one period long, through a two-point
/// average (a gentle low-pass) and a decay factor, so it rings at the string's pitch and loses its
/// highs over time, roughly like a plucked string.
///
/// The loader measures every model's loudness on this signal (BUILD_PLAN "Loudness matching"). A real
/// recorded DI should replace it once Sean records one.
std::vector<float> referenceGuitarDI (int numSamples, double sampleRate = 48000.0, juce::int64 seed = 42);

/// White noise (equal power per hertz), uniform in [-0.3, 0.3]. The reference for loudness-matching
/// cab IRs. Raw amp-only output is bright, and the cab's job is largely to remove that fizz, so a
/// reference with plenty of treble predicts a cab's effect on real amp output better than pink noise
/// does. The "Cab normalization study" test measured this: across clean and distorted guitar through
/// stock, dark, and bright cabs, white noise left at most 2.1 LU of difference, against 6.6 LU for pink.
std::vector<float> referenceWhiteNoise (int numSamples, juce::int64 seed = 1);

} // namespace ampsim

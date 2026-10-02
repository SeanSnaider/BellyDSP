#pragma once

#include <vector>

namespace ampsim::loudness
{

/// One biquad's coefficients, normalized so a0 = 1.
struct Biquad
{
    double b0, b1, b2, a1, a2;
};

/// The two K-weighting stages of ITU-R BS.1770-4. The standard publishes them as coefficients at
/// 48 kHz. These analog-derived formulas (as used by libebur128) reproduce those exactly and extend
/// to other sample rates:
///   preFilter: a high shelf, about +4 dB above ~1.7 kHz, modeling the head's acoustic effect
///   rlbFilter: a 2nd-order high-pass around 38 Hz (the "revised low-frequency B" curve)
Biquad preFilter (double sampleRate);
Biquad rlbFilter (double sampleRate);

/// Integrated loudness in LUFS (BS.1770-4), offline. Per channel: K-weight, then mean square over
/// 400 ms blocks overlapping by 75%. Block loudness is l_j = -0.691 + 10 log10(sum over channels of
/// z_j). Gating: drop blocks below -70 LUFS (absolute gate), then drop blocks more than 10 LU below
/// the loudness of what's left (relative gate). The result is -0.691 + 10 log10 of the mean z over
/// the surviving blocks. The -0.691 cancels the K-filter's gain at 1 kHz, so a full-scale 1 kHz sine
/// in both stereo channels reads 0 LUFS.
/// Returns -infinity for silence or anything shorter than one 400 ms block.
double integrated (const std::vector<const float*>& channels, int numSamples, double sampleRate);
double integratedMono (const float* samples, int numSamples, double sampleRate);

/// Linear convolution y = x * h through FFTs (offline, allocates). The output has x's length.
std::vector<float> fftConvolve (const std::vector<float>& x, const float* h, int hLength);

} // namespace ampsim::loudness

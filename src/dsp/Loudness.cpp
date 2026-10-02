#include "Loudness.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <limits>

namespace ampsim::loudness
{

Biquad preFilter (double sampleRate)
{
    // High shelf: f0, gain G (dB), and Q chosen so that at 48 kHz the coefficients equal BS.1770's
    // published values. Vb sets the shelf's midpoint gain.
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const auto K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const auto Vh = std::pow (10.0, G / 20.0);
    const auto Vb = std::pow (Vh, 0.4996667741545416);
    const auto a0 = 1.0 + K / Q + K * K;

    return { (Vh + Vb * K / Q + K * K) / a0,
             2.0 * (K * K - Vh) / a0,
             (Vh - Vb * K / Q + K * K) / a0,
             2.0 * (K * K - 1.0) / a0,
             (1.0 - K / Q + K * K) / a0 };
}

Biquad rlbFilter (double sampleRate)
{
    // High-pass at f0 with quality Q. The numerator stays 1, -2, 1, as in the standard.
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const auto K = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const auto a0 = 1.0 + K / Q + K * K;

    return { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
}

namespace
{
/// K-weights one channel and returns its running sum of squares (prefix sums), in double, so any
/// block's mean square is one subtraction.
std::vector<double> kWeightedPrefixSquares (const float* x, int n, double sampleRate)
{
    const auto pre = preFilter (sampleRate), rlb = rlbFilter (sampleRate);
    double px1 = 0, px2 = 0, py1 = 0, py2 = 0, rx1 = 0, rx2 = 0, ry1 = 0, ry2 = 0;
    std::vector<double> prefix ((size_t) n + 1, 0.0);

    for (int i = 0; i < n; ++i)
    {
        const double in = x[i];
        const auto y1 = pre.b0 * in + pre.b1 * px1 + pre.b2 * px2 - pre.a1 * py1 - pre.a2 * py2;
        px2 = px1; px1 = in; py2 = py1; py1 = y1;
        const auto y2 = rlb.b0 * y1 + rlb.b1 * rx1 + rlb.b2 * rx2 - rlb.a1 * ry1 - rlb.a2 * ry2;
        rx2 = rx1; rx1 = y1; ry2 = ry1; ry1 = y2;
        prefix[(size_t) i + 1] = prefix[(size_t) i] + y2 * y2;
    }

    return prefix;
}
} // namespace

double integrated (const std::vector<const float*>& channels, int numSamples, double sampleRate)
{
    const auto blockLength = juce::roundToInt (0.4 * sampleRate);
    const auto step = juce::roundToInt (0.1 * sampleRate); // 75% overlap
    constexpr auto silence = -std::numeric_limits<double>::infinity();

    if (numSamples < blockLength || channels.empty())
        return silence;

    std::vector<std::vector<double>> prefixes;
    for (const auto* channel : channels)
        prefixes.push_back (kWeightedPrefixSquares (channel, numSamples, sampleRate));

    // z_j: the sum over channels (each weighted 1, as for mono and stereo) of the block's mean square.
    std::vector<double> z;
    for (int start = 0; start + blockLength <= numSamples; start += step)
    {
        double sum = 0.0;
        for (const auto& prefix : prefixes)
            sum += (prefix[(size_t) (start + blockLength)] - prefix[(size_t) start]) / blockLength;
        z.push_back (sum);
    }

    const auto blockLoudness = [] (double meanSquare) { return -0.691 + 10.0 * std::log10 (meanSquare); };

    // Absolute gate at -70 LUFS.
    double sum = 0.0;
    int count = 0;
    for (auto zj : z)
        if (zj > 0.0 && blockLoudness (zj) > -70.0)
        {
            sum += zj;
            ++count;
        }

    if (count == 0)
        return silence;

    // Relative gate 10 LU below the absolute-gated loudness.
    const auto relativeGate = blockLoudness (sum / count) - 10.0;
    sum = 0.0;
    count = 0;
    for (auto zj : z)
        if (zj > 0.0 && blockLoudness (zj) > -70.0 && blockLoudness (zj) > relativeGate)
        {
            sum += zj;
            ++count;
        }

    return count > 0 ? blockLoudness (sum / count) : silence;
}

double integratedMono (const float* samples, int numSamples, double sampleRate)
{
    return integrated ({ samples }, numSamples, sampleRate);
}

std::vector<float> fftConvolve (const std::vector<float>& x, const float* h, int hLength)
{
    const auto fullLength = (int) x.size() + hLength - 1;
    int order = 1;
    while ((1 << order) < fullLength)
        ++order;

    const auto size = (size_t) 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> X (2 * size, 0.0f), H (2 * size, 0.0f); // JUCE needs 2 * size for real transforms

    std::copy (x.begin(), x.end(), X.begin());
    std::copy (h, h + hLength, H.begin());
    fft.performRealOnlyForwardTransform (X.data(), true);
    fft.performRealOnlyForwardTransform (H.data(), true);

    // Multiply the spectra: bins 0..size/2, stored as interleaved (re, im) pairs.
    for (size_t k = 0; k <= size / 2; ++k)
    {
        const auto a = X[2 * k], b = X[2 * k + 1], c = H[2 * k], d = H[2 * k + 1];
        X[2 * k] = a * c - b * d;
        X[2 * k + 1] = a * d + b * c;
    }

    fft.performRealOnlyInverseTransform (X.data()); // JUCE's inverse divides by size
    X.resize (x.size());
    return X;
}

} // namespace ampsim::loudness

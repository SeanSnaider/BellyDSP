// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "MinimumPhase.h"
#include "FftDouble.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace ampsim::minphase
{

int fftOrderFor (int irLength)
{
    int order = 12;
    while ((1 << order) < 8 * irLength)
        ++order;
    return order;
}

std::vector<double> logMagnitude (const std::vector<float>& h, int fftOrder)
{
    const FftDouble fft (fftOrder);
    const auto size = (size_t) fft.size();
    std::vector<std::complex<double>> spectrum (size);
    for (size_t i = 0; i < std::min (h.size(), size); ++i)
        spectrum[i] = h[i];
    fft.forward (spectrum);

    std::vector<double> magnitude (size / 2 + 1);
    double peak = 0.0;
    for (size_t k = 0; k <= size / 2; ++k)
    {
        magnitude[k] = std::abs (spectrum[k]);
        peak = std::max (peak, magnitude[k]);
    }

    const auto floor = std::max (peak * 1.0e-6, 1.0e-30); // 120 dB below the peak
    for (auto& m : magnitude)
        m = std::log (std::max (m, floor));

    // A cab IR has (nearly) exact zeros at DC (a speaker can't move air at 0 Hz) and often at Nyquist
    // (its low-pass). Those bins then sit at the floor, tens of dB below their neighbours, and a
    // one-bin spike in the log spectrum puts a constant into the cepstrum, which the folding turns
    // into a low-level error spread over the whole result (about -60 dB here). Giving those two bins
    // their neighbours' values removes it (-80 dB), and 0 Hz and 24 kHz are inaudible anyway.
    magnitude[0] = magnitude[1];
    magnitude[size / 2] = magnitude[size / 2 - 1];

    return magnitude;
}

std::vector<float> fromLogMagnitude (const std::vector<double>& logMag, int fftOrder, double delaySamples, int length)
{
    const FftDouble fft (fftOrder);
    const auto size = (size_t) fft.size();
    std::vector<std::complex<double>> buffer (size);

    // 1. Real cepstrum: the inverse FFT of the log magnitude, extended to the full (even) spectrum, so
    //    its inverse is real.
    for (size_t k = 0; k <= size / 2; ++k)
        buffer[k] = logMag[k];
    for (size_t k = 1; k < size / 2; ++k)
        buffer[size - k] = logMag[k];
    fft.inverse (buffer);

    // 2. Fold onto positive quefrency: the cepstrum of the minimum-phase system with this magnitude.
    //    Its even part is the original cepstrum, so the magnitude is untouched.
    std::vector<std::complex<double>> folded (size);
    folded[0] = buffer[0].real();
    for (size_t n = 1; n < size / 2; ++n)
        folded[n] = 2.0 * buffer[n].real();
    folded[size / 2] = buffer[size / 2].real();

    // 3. Back to the frequency domain and exponentiate: H_min = exp(FFT(c_min)). Then the delay, as the
    //    linear phase exp(-j 2 pi f d / N) with f = k for the positive frequencies and k - N for the
    //    negative ones, which keeps the spectrum conjugate-symmetric (a real IR).
    fft.forward (folded);
    for (size_t k = 0; k < size; ++k)
    {
        const auto f = k <= size / 2 ? (double) k : (double) k - (double) size;
        const auto delayPhase = -2.0 * std::numbers::pi * f * delaySamples / (double) size;
        folded[k] = std::exp (folded[k] + std::complex<double> (0.0, delayPhase));
    }

    // 4. Into the time domain. (At Nyquist a fractional delay's phase isn't real; taking the real part
    //    keeps the real-valued half of that one bin.)
    fft.inverse (folded);
    std::vector<float> out ((size_t) std::min ((size_t) length, size));
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = (float) folded[i].real();
    return out;
}

std::vector<float> minimumPhase (const std::vector<float>& h, int fftOrder)
{
    return fromLogMagnitude (logMagnitude (h, fftOrder), fftOrder, 0.0, (int) h.size());
}

double onset (const std::vector<float>& h)
{
    float peak = 0.0f;
    for (auto v : h)
        peak = std::max (peak, std::abs (v));

    if (peak <= 0.0f)
        return 0.0;

    const auto threshold = 0.1f * peak; // -20 dB

    for (size_t n = 0; n < h.size(); ++n)
    {
        const auto a = std::abs (h[n]);
        if (a < threshold)
            continue;

        if (n == 0)
            return 0.0;

        // Linear interpolation of where |h| crosses the threshold, between samples n-1 and n.
        const auto before = std::abs (h[n - 1]);
        return (double) (n - 1) + (double) (threshold - before) / (double) (a - before);
    }

    return 0.0;
}

} // namespace ampsim::minphase

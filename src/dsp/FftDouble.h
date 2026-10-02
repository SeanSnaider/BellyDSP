#pragma once

#include <complex>
#include <vector>

namespace ampsim
{

/// A double-precision complex FFT for offline analysis on the loader thread (minimum-phase cab
/// morphing), where JUCE's float FFT isn't accurate enough: near a cab's deep zeros (DC, Nyquist) the
/// float transform's rounding noise is all that's left of the spectrum, and the cepstrum smears that
/// noise across every frequency.
///
/// Radix-2, iterative (Cooley-Tukey, decimation in time): reorder the input by bit-reversed index,
/// then log2(N) passes of butterflies, X[k] = E[k] + W^k O[k] and X[k + N/2] = E[k] - W^k O[k], where E
/// and O are the transforms of the even and odd samples and W = exp(-j 2 pi / N). Twiddles W^k are
/// computed directly (not by repeated multiplication), which keeps the error near 1e-16 log2(N).
class FftDouble
{
public:
    explicit FftDouble (int order);

    int size() const noexcept { return n; }

    /// In place. Forward: X[k] = sum x[n] e^{-j 2 pi k n / N}. Inverse: the same with +j, divided by N.
    void forward (std::vector<std::complex<double>>& data) const { perform (data, false); }
    void inverse (std::vector<std::complex<double>>& data) const;

private:
    void perform (std::vector<std::complex<double>>& data, bool conjugateTwiddles) const;

    int n;
    std::vector<std::complex<double>> twiddles; // W^k for k < N/2
    std::vector<int> bitReversed;
};

} // namespace ampsim

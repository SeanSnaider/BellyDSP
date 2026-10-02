#pragma once

#include <array>
#include <complex>
#include <vector>

namespace ampsim
{

/// Polyphase IIR halfband filters (BUILD_PLAN "Boost and Overdrive", Oversampling), after Laurent de
/// Soras' HIIR library and Valenzuela and Constantinides (1983). Ported from prototypes/halfband.py,
/// whose golden coefficients (tests/fixtures/halfband_coefficients.csv) the tests hold this to.
///
/// A halfband low-pass at the high rate splits into two allpass branches:
///     H(z) = ( A0(z^2) + z^-1 A1(z^2) ) / 2,     A(z^2) = prod_k (a_k + z^-2) / (1 + a_k z^-2)
/// with the coefficients a_0, a_1, a_2, ... alternating between A0 and A1. The branches run at the low
/// rate (z^2 at the high rate is one low-rate sample), so each coefficient costs one multiply per
/// low-rate sample. The two branches' outputs are in phase below fs/4 and in antiphase above it: their
/// sum keeps the low half, their difference the high half.
///
/// Why IIR rather than the usual linear-phase FIR halfband: an FIR adds a fixed delay of half its length
/// (tens of samples), and the foundation rules allow no added latency. This filter's delay is its own
/// group delay: a few samples at low frequencies, growing near the band edge. The up-then-down chain is
/// even better than two filters in a row: upsampling sends A0 x and A1 x out as the even and odd samples,
/// and downsampling feeds the odd samples to A0 and the even ones to A1 and averages, so with nothing in
/// between the chain is exactly A0(z) A1(z) at the low rate, an allpass: magnitude 1 at every frequency.
///
/// Coefficients come from an elliptic design. For N coefficients (filter order 2N + 1) and a transition
/// band of width `transition` (a fraction of the high rate) centred on fs/4:
///     k = tan^2((1 - 2 transition) pi / 4)            elliptic modulus
///     q = e + 2e^5 + 15e^9 + 150e^13,  e = (1 - (1-k^2)^(1/4)) / (2 (1 + (1-k^2)^(1/4)))   nome
///     w_i = 2 q^(1/4) sum_m (-1)^m q^(m(m+1)) sin((2m+1) c pi / (2N+1))
///                / (1 + 2 sum_m (-1)^m q^(m^2) cos(2 m c pi / (2N+1))),   c = i + 1
///     x_i = sqrt((1 - w_i^2 k)(1 - w_i^2 / k)) / (1 + w_i^2),   a_i = (1 - x_i) / (1 + x_i)
/// (the Jacobi theta-function series of HIIR's PolyphaseIir2Designer).
namespace halfband
{
static constexpr int maxCoefficients = 12;

/// The allpass coefficients for numCoefficients (1 to maxCoefficients) and a normalized transition width
/// in (0, 0.5). Allocates: call it off the audio thread.
std::vector<double> design (int numCoefficients, double transition);

/// H(e^{j 2 pi f}) at f, a fraction of the high rate (0 to 0.5), from the polyphase form above.
std::complex<double> response (const std::vector<double>& coefficients, double frequency);
} // namespace halfband

/// One 2x stage's allpass sections (HIIR's StageProc). Each section is the first-order allpass
///     y[n] = a (x[n] - y[n-1]) + x[n-1],   H(z) = (a + z^-1) / (1 + a z^-1)   at the low rate,
/// and coefficient k filters branch k % 2. With an odd count the last section is on branch 0 only.
/// State is double precision (foundation rules: IIR state in f64).
class HalfbandStage
{
public:
    void setCoefficients (const std::vector<double>& coefficients);
    void reset() noexcept;
    int numCoefficients() const noexcept { return count; }
    double coefficient (int k) const noexcept { return a[(size_t) k]; }

    /// Upsampling: one low-rate sample in, two high-rate samples out (A0 x first, then A1 x).
    void up (double in, double& out0, double& out1) noexcept
    {
        out0 = in;
        out1 = in;
        run (out0, out1);
    }

    /// Downsampling: two high-rate samples in (in0 first), one low-rate sample out:
    /// (A0 in1 + A1 in0) / 2, which is H filtered and every second sample kept.
    double down (double in0, double in1) noexcept
    {
        auto b0 = in1, b1 = in0;
        run (b0, b1);
        return 0.5 * (b0 + b1);
    }

private:
    void run (double& branch0, double& branch1) noexcept
    {
        int k = 0;
        for (; k + 1 < count; k += 2)
        {
            const auto t0 = (branch0 - y[(size_t) k]) * a[(size_t) k] + x[(size_t) k];
            const auto t1 = (branch1 - y[(size_t) k + 1]) * a[(size_t) k + 1] + x[(size_t) k + 1];
            x[(size_t) k] = branch0;
            x[(size_t) k + 1] = branch1;
            y[(size_t) k] = t0;
            y[(size_t) k + 1] = t1;
            branch0 = t0;
            branch1 = t1;
        }
        if (k < count)
        {
            const auto t0 = (branch0 - y[(size_t) k]) * a[(size_t) k] + x[(size_t) k];
            x[(size_t) k] = branch0;
            y[(size_t) k] = t0;
            branch0 = t0;
        }
    }

    std::array<double, halfband::maxCoefficients> a {}, x {}, y {};
    int count = 0;
};

/// Cascaded 2x stages for 2x, 4x, or 8x (factor 1 passes straight through). The stage designs:
///
///   stage 1, 48 <-> 96 kHz:   8 coefficients, transition 0.04: passband to 22.1 kHz, stopband from
///                             25.9 kHz at -99.3 dB
///   stage 2, 96 <-> 192 kHz:  4 coefficients, transition 0.25: only 0-24 kHz has to pass and the images
///                             start at 72 kHz, so the wide transition reaches -116.8 dB
///   stage 3, 192 <-> 384 kHz: 3 coefficients, transition 0.375: -134.3 dB
///
/// Up and down are separate objects (each holds its own filter state), so a block can upsample its
/// input once and downsample more than one signal.
class Upsampler
{
public:
    static constexpr int maxStages = 3;

    Upsampler();
    /// 1, 2, 4, or 8. No allocation; clears the state when the factor changes.
    void setFactor (int factor) noexcept;
    int getFactor() const noexcept { return 1 << numStages; }
    void reset() noexcept;

    /// numSamples in, numSamples x factor out.
    void process (const double* in, double* out, int numSamples) noexcept;

private:
    std::array<HalfbandStage, maxStages> stages;
    int numStages = 2;
};

class Downsampler
{
public:
    static constexpr int maxStages = Upsampler::maxStages;

    Downsampler();
    void setFactor (int factor) noexcept;
    int getFactor() const noexcept { return 1 << numStages; }
    void reset() noexcept;

    /// numSamples x factor in, numSamples out.
    void process (const double* in, double* out, int numSamples) noexcept;

private:
    std::array<HalfbandStage, maxStages> stages;
    int numStages = 2;
};

namespace oversampling
{
struct StageDesign
{
    int numCoefficients;
    double transition;
};

static constexpr std::array<StageDesign, Upsampler::maxStages> stageDesigns { { { 8, 0.04 }, { 4, 0.25 }, { 3, 0.375 } } };

/// Group delay of the up-then-down chain at low frequencies, in base-rate samples. Each stage's pair is
/// A0(z) A1(z) at its low rate, a cascade of first-order allpasses, and (a + z^-1) / (1 + a z^-1) delays
/// low frequencies by (1 - a) / (1 + a) samples; stage s's low rate is 2^s times the base rate.
double groupDelaySamples (int factor);
} // namespace oversampling

} // namespace ampsim

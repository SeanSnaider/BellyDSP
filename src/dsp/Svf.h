// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <complex>

namespace ampsim
{

/// Trapezoidal-integrated state-variable filter (TPT SVF), after Andrew Simper, "Linear Trapezoidal
/// Integrated State Variable Filter With Low Noise Optimisation" (Cytomic, 2013), and Vadim
/// Zavalishin, "The Art of VA Filter Design", chapter 4.
///
/// The analog state-variable filter is two integrators in a feedback loop:
///     high = x - k*band - low,   band = high / s,   low = band / s     (s normalized to the cutoff)
/// so
///     low  = x / (s^2 + k s + 1)
///     band = x s / (s^2 + k s + 1)          k = 1/Q
///     high = x s^2 / (s^2 + k s + 1)
/// Every response type here is a mix m0*x + m1*band + m2*low of those outputs.
///
/// Discretizing both integrators with the trapezoidal rule (equivalent to the bilinear transform) and
/// solving the loop's implicit equation in closed form gives processSample(). The bilinear transform
/// warps frequency, so the integrator gain g = tan(pi fc / fs) is prewarped: the digital filter's
/// response at frequency f equals the analog prototype's at
///     Omega = tan(pi f / fs) / g
/// exactly. responseAt() evaluates that, and the tests hold the filter to it.
///
/// Why this structure (BUILD_PLAN "EQ"): its state variables are the integrators' contents, so changing
/// coefficients mid-stream doesn't jolt the output or destabilize the filter the way a direct-form
/// biquad can when a knob or an expression pedal sweeps it fast. State is double precision, because
/// low-frequency filters in f32 add audible noise and drift (foundation decisions).
class Svf
{
public:
    enum class Type
    {
        lowpass,
        highpass,
        bandpass, // constant 0 dB peak at fc
        notch,
        allpass,
        peak,     // bell: gainDb at fc
        lowShelf, // gainDb below fc, 0 dB above, half of gainDb at fc
        highShelf // gainDb above fc, 0 dB below, half of gainDb at fc
    };

    struct Coefficients
    {
        double g = 0.0, k = 1.0;             // prewarped integrator gain and damping
        double a1 = 1.0, a2 = 0.0, a3 = 0.0; // the solved loop
        double m0 = 1.0, m1 = 0.0, m2 = 0.0; // output mix of x, band, and low
    };

    /// frequency in Hz, q > 0. gainDb only matters for peak and the shelves.
    static Coefficients design (Type type, double frequency, double q, double gainDb, double sampleRate)
    {
        const auto fc = juce::jlimit (1.0, 0.49 * sampleRate, frequency); // tan() blows up at Nyquist
        const auto A = std::pow (10.0, gainDb / 40.0); // sqrt of the linear gain; peaks and shelves reach A^2

        Coefficients c;
        c.g = std::tan (juce::MathConstants<double>::pi * fc / sampleRate);
        c.k = 1.0 / q;

        switch (type)
        {
            case Type::lowpass:  c.m0 = 0.0; c.m1 = 0.0;        c.m2 = 1.0;  break;
            case Type::highpass: c.m0 = 1.0; c.m1 = -c.k;       c.m2 = -1.0; break; // x - k band - low = high
            case Type::bandpass: c.m0 = 0.0; c.m1 = c.k;        c.m2 = 0.0;  break; // k s/(s^2+ks+1): 1 at fc
            case Type::notch:    c.m0 = 1.0; c.m1 = -c.k;       c.m2 = 0.0;  break; // (s^2+1)/(s^2+ks+1)
            case Type::allpass:  c.m0 = 1.0; c.m1 = -2.0 * c.k; c.m2 = 0.0;  break; // (s^2-ks+1)/(s^2+ks+1)

            case Type::peak:
                // H = (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1): exactly A^2 at fc, 1 far away.
                c.k = 1.0 / (q * A);
                c.m0 = 1.0; c.m1 = c.k * (A * A - 1.0); c.m2 = 0.0;
                break;

            case Type::lowShelf:
                // Poles at fc/sqrt(A), zeros at fc*sqrt(A): H(0) = A^2, H(inf) = 1, |H| = A at fc.
                c.g /= std::sqrt (A);
                c.m0 = 1.0; c.m1 = c.k * (A - 1.0); c.m2 = A * A - 1.0;
                break;

            case Type::highShelf:
                // The mirror image: H(0) = 1, H(inf) = A^2, |H| = A at fc.
                c.g *= std::sqrt (A);
                c.m0 = A * A; c.m1 = c.k * (1.0 - A) * A; c.m2 = 1.0 - A * A;
                break;
        }

        // Closed-form solution of the trapezoidal loop (Simper 2013).
        c.a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
        c.a2 = c.g * c.a1;
        c.a3 = c.g * c.a2;
        return c;
    }

    /// The exact complex response of a coefficient set at frequency f: the analog prototype at the
    /// prewarped frequency. Used by the tests and, later, the EQ curve display.
    static std::complex<double> responseAt (const Coefficients& c, double f, double sampleRate)
    {
        const auto omega = std::tan (juce::MathConstants<double>::pi * f / sampleRate) / c.g;
        const std::complex<double> s (0.0, omega);
        const auto denominator = s * s + c.k * s + 1.0;
        return c.m0 + c.m1 * s / denominator + c.m2 / denominator;
    }

    void setCoefficients (const Coefficients& c) noexcept { coeffs = c; }
    const Coefficients& getCoefficients() const noexcept { return coeffs; }
    void reset() noexcept { ic1eq = ic2eq = 0.0; }

    double processSample (double v0) noexcept
    {
        // One trapezoidal step of both integrators, solved together (Simper 2013).
        const auto v3 = v0 - ic2eq;
        const auto v1 = coeffs.a1 * ic1eq + coeffs.a2 * v3;        // band
        const auto v2 = ic2eq + coeffs.a2 * ic1eq + coeffs.a3 * v3; // low
        ic1eq = 2.0 * v1 - ic1eq;
        ic2eq = 2.0 * v2 - ic2eq;
        return coeffs.m0 * v0 + coeffs.m1 * v1 + coeffs.m2 * v2;
    }

private:
    Coefficients coeffs;
    double ic1eq = 0.0, ic2eq = 0.0; // the integrators' states
};

} // namespace ampsim

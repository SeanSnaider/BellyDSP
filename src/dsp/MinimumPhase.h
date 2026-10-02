#pragma once

#include <vector>

namespace ampsim::minphase
{

/// The tools for morphing between cab IRs without comb filtering (BUILD_PLAN "Movable mics").
///
/// Crossfading two IRs that arrive at different times sums two delayed copies of the sound, which is
/// a comb filter: a hollow, phasey tone. Instead, each IR is split into a bulk delay (when the sound
/// arrives) and a minimum-phase part (its magnitude response with all of its energy as early as
/// possible). Morphing interpolates the log-magnitude spectra and the delays separately, rebuilds a
/// minimum-phase IR from the interpolated magnitude, and puts the interpolated delay back.
///
/// The minimum-phase reconstruction uses the real cepstrum (Oppenheim and Schafer, "Discrete-Time
/// Signal Processing", the chapter on cepstrum analysis and minimum-phase systems):
///   1. c[n] = IFFT( log |H[k]| ), the real cepstrum of h
///   2. fold it onto positive quefrency: c_min[0] = c[0], c_min[n] = 2 c[n] for 0 < n < N/2,
///      c_min[N/2] = c[N/2], 0 for the rest
///   3. H_min[k] = exp( FFT(c_min)[k] ), then h_min = IFFT(H_min)
/// Folding makes log H_min analytic (all its poles and zeros inside the unit circle), which is what
/// minimum phase means, while keeping |H_min| = |H|. N must be several times longer than the IR, or
/// the cepstrum wraps around (time aliasing); fftOrderFor() picks one.

/// An FFT order whose size is at least 8 times the IR length (and at least 2^12).
int fftOrderFor (int irLength);

/// log |H[k]| for k = 0..N/2, natural log, floored 120 dB below the peak so silence doesn't give -inf.
/// The DC and Nyquist bins take their neighbours' values (see the .cpp for why).
std::vector<double> logMagnitude (const std::vector<float>& h, int fftOrder);

/// The minimum-phase impulse response with the given log magnitude (natural log, N/2+1 bins),
/// delayed by a fractional number of samples (a linear phase term applied in the frequency domain),
/// truncated to `length` samples.
std::vector<float> fromLogMagnitude (const std::vector<double>& logMag, int fftOrder, double delaySamples, int length);

/// The minimum-phase version of h, same length, no added delay.
std::vector<float> minimumPhase (const std::vector<float>& h, int fftOrder);

/// When the sound arrives: where |h| first comes within 20 dB of its peak, linearly interpolated
/// between the two samples around the crossing. Used as the bulk delay of each captured IR.
double onset (const std::vector<float>& h);

} // namespace ampsim::minphase

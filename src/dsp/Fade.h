// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <cmath>

namespace ampsim
{

/// An S-shaped fade: the smoothstep r^2 (3 - 2r) of a linear ramp r in [0, 1]. Bloom's on/off, bypass, reorder,
/// and mode fades run r through it. It starts and ends with zero slope, where a plain linear ramp has a corner
/// at each end: a jump in the gain's slope, whose spectral splatter falls off as 1/f^2 against the S-curve's
/// 1/f^3. Mid-fade it's a little steeper (1.5 times at the centre). It's odd-symmetric, S(1 - r) = 1 - S(r), so
/// two signals faded with S(r) and S(1 - r) still have gains summing to exactly 1, and S(0) = 0 and S(1) = 1
/// exactly, so a finished fade is bit-transparent.
inline double sCurve (double r) noexcept
{
    return r * r * (3.0 - 2.0 * r);
}

/// The level hold for a linear dry/wet mix of a signal with a phase-turned copy of itself (the phaser's allpass
/// chain, the flanger's delay): out = g(a) ((1 - a) x + a w), with
///     g(a) = 1 / sqrt((1 - a)^2 + a^2).
/// Why: where w has the same magnitude as x and a phase phi relative to it, |(1 - a) + a e^(j phi)|^2 =
/// (1 - a)^2 + a^2 + 2 a (1 - a) cos(phi). Averaged over the spectrum (phi spread evenly over the comb's or the
/// sweep's teeth, so cos(phi) averages 0) the mix keeps only (1 - a)^2 + a^2 of the power: half (-3 dB) at
/// a = 1/2, the deepest-notch setting. g puts that power back, so switching the effect on doesn't drop the level
/// (measured on the whole rig, 2026-10-04: the phaser -4.0 dB, the flanger -3.1 dB at their defaults). The notches
/// stay exactly where and as deep as they were; the in-phase peaks between them rise by up to 20 log10(g), 3 dB
/// at a = 1/2. g(0) = g(1) = 1 exactly, so the dry (and the wet alone) are untouched.
inline double mixLevelHold (double a) noexcept
{
    if (a <= 0.0 || a >= 1.0)
        return 1.0;
    return 1.0 / std::sqrt ((1.0 - a) * (1.0 - a) + a * a);
}

} // namespace ampsim

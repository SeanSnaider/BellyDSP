#pragma once

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

} // namespace ampsim

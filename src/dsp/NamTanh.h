// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

namespace ampsim::namtanh
{

/// The tanh NAM core's WaveNets use (BUILD_PLAN "CPU").
///
/// Every layer of a standard WaveNet ends in tanh, and NAM core v0.6.0 (activations.h, ActivationTanh) computes
/// it one element at a time with std::tanh: 240 calls per sample for a standard A1 WaveNet (16 channels x 10
/// layers + 8 x 10), 30720 per 128-sample buffer, at about 3.5 ns each on an M5 Pro. That was about three
/// quarters of a model's time. The vectorised one is Eigen's (Eigen 5.0, GenericPacketMathFunctions.h,
/// ptanh_float): a [9/8] odd/even rational minimax approximation on [-8.67, 8.67] with the input clamped where
/// it reaches exactly +-1, evaluated 4 (NEON) or 8 (AVX) floats at a time, 0.25 ns per element.
///
/// Measured against the libm tanhf it replaces, over every float in [-12, 12] (2.19e9 values): identical on 94.7%,
/// at most 5 ulp apart, at most 3.0e-7 apart (libm's own error against the exact tanh is 4.9e-8). That is the
/// size of float rounding, not an approximation like NAM's own Fasttanh (8.7e-4 off, which NAM's plugin turns on).
/// What it does to a whole model's output is measured in the tests ("NAM slot": against NAM core's own render
/// tool, which keeps the stock tanh).
enum class Tanh
{
    vectorised, // Eigen's packet tanh (the app's)
    stock       // NAM core's ActivationTanh, std::tanh per element
};

/// Puts this tanh into NAM core's activation registry, which every model loaded afterwards takes its "Tanh" from
/// (a model keeps the one it was built with). Not thread-safe against loads running at the same time: the app calls
/// it once, before its first load (useAppTanh); tests switch it between loads.
void use (Tanh which);

/// The app's choice, installed once on the first call (thread-safe): NamAmp calls it before every load.
void useAppTanh();

/// Which tanh models loaded now get.
Tanh current();

} // namespace ampsim::namtanh

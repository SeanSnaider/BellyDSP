// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

/// Keeps the standalone app's audio device at 48 kHz, the only rate the chain runs at (NAM captures are
/// trained at 48 kHz; at any other rate the processor mutes and says why).
///
/// Why it exists: the device's rate can change under the app. On macOS, Apple Music switches the output
/// device to a lossless song's own rate (usually 44.1 kHz), and the interface keeps that rate afterwards;
/// the standalone wrapper then opens (and saves) the device at whatever rate it's running. Sean hit exactly
/// that on 2026-10-04: a silent app, with the warning easy to miss.
///
/// What it does: once a second, on the message thread, if the current device isn't at 48 kHz but lists
/// 48 kHz among its rates, it asks the device manager to switch to 48 kHz (which the standalone wrapper
/// then saves as the chosen setup). It stops trying, and leaves the processor's mute and warning in place,
/// when the device can't do 48 kHz or when something keeps switching it away (more than maxSwitches in a
/// minute), so it never fights another app forever. Never touches the audio thread.
///
/// One implementation per build, chosen by CMake: SampleRateGuard_standalone.cpp in the app (it reaches
/// JUCE's StandalonePluginHolder), SampleRateGuard_none.cpp in the tests and anything that isn't the
/// standalone app.
namespace platform::samplerate
{
inline constexpr double requiredRate = 48000.0;
inline constexpr int maxSwitches = 5;

/// Message thread. Starts the once-a-second check (later calls do nothing). The editor calls it in the
/// standalone app only.
void start();

/// Message thread. Stops the check (the editor calls it when it closes).
void stop();
} // namespace platform::samplerate

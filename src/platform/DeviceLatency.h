// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

/// The audio device's latencies as the device reports them, for tone match's play-along takes
/// (docs/TONE_MATCH.md, "Play along"): the song the app plays reaches the player's ears the output latency
/// after the callback that wrote it, and what the player plays reaches the callback the input latency after
/// it left the strings, so a recording lines up with the song once it's shifted by their sum.
///
/// JUCE's definitions (juce_AudioIODevice.h): output latency, "the delay in samples between a callback
/// getting a block of data, and that data actually getting played"; input latency, "the delay in samples
/// between some audio actually arriving at the soundcard, and the callback getting passed this block of
/// data". CoreAudio's include the device's own latency, its safety offset, the stream latency, and one
/// buffer each way; ASIO's are what the driver reports (ASIOGetLatencies), which includes the buffer.
///
/// One implementation per build, chosen by CMake: DeviceLatency_standalone.cpp in the app (it reaches JUCE's
/// StandalonePluginHolder, as SampleRateGuard does), DeviceLatency_none.cpp in the tests and the tools.
/// Message thread.
namespace platform::device
{
struct Latency
{
    bool known = false; ///< false: no device (or not the standalone app); both are 0 then
    int inputSamples = 0, outputSamples = 0;
    int bufferSize = 0;
    double sampleRate = 0.0;
};

Latency reportedLatency();
} // namespace platform::device

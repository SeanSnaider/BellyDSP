// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <optional>

/// Windows Audio (Exclusive Mode) and the standalone app's one input: what to do when the device won't open.
///
/// Why it exists: JUCE's WASAPI backend opens "highest active input + 1" channels, so with the guitar on
/// Input 1 (the app is a mono-in processor, so exactly one input is active) it asks an exclusive-mode endpoint
/// for a mono stream. Exclusive mode has no Windows mixer to convert formats, and many interfaces' capture
/// endpoints only offer stereo (Sean's Scarlett Solo, "Analogue 1 + 2 (Focusrite USB Audio)", on
/// 2026-10-05), so the open fails with "Couldn't open the input device!". Input 2 alone opens two channels
/// and works; shared mode and DirectSound convert, so they never fail this way; CoreAudio opens every channel.
///
/// What to do: open Inputs 1 and 2 instead. JUCE's AudioProcessorPlayer gives device channel 0 to the
/// processor's one input; channel 1 lands in the second output channel's buffer, which the chain overwrites
/// (the tests check that Input 2 never reaches the output). The standalone app's device guard
/// (SampleRateGuard_standalone.cpp) applies it once a second, on the message thread.
namespace platform::exclusivemode
{
/// JUCE's name for the WASAPI exclusive-mode device type.
inline constexpr const char* typeName = "Windows Audio (Exclusive Mode)";

/// The setup to retry with, or nothing: only for the exclusive-mode type, with no device open, an input
/// device named, and Input 1 alone requested. The retry asks for Inputs 1 and 2, so if it fails too, the
/// next check sees two inputs and leaves it alone: one retry per failure, never a loop.
///
/// @param deviceType  the device manager's current type name.
/// @param setup       the device manager's current setup (after a failed open it holds the failed request).
/// @param deviceOpen  whether the device manager has a device open.
inline std::optional<juce::AudioDeviceManager::AudioDeviceSetup> retryWithInputPair (const juce::String& deviceType,
                                                                                     const juce::AudioDeviceManager::AudioDeviceSetup& setup,
                                                                                     bool deviceOpen)
{
    juce::BigInteger inputOneAlone;
    inputOneAlone.setBit (0);

    if (deviceType != typeName || deviceOpen || setup.inputDeviceName.isEmpty() || setup.inputChannels != inputOneAlone)
        return std::nullopt;

    auto retry = setup;
    retry.useDefaultInputChannels = false;
    retry.inputChannels.setRange (0, 2, true);
    return retry;
}
} // namespace platform::exclusivemode

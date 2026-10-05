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
/// (SampleRateGuard_standalone.cpp) applies it once a second, on the message thread. Once a pair has
/// opened, the device manager asks for two inputs by default, so later device and type changes from the
/// Options dialog open a pair directly.
namespace platform::exclusivemode
{
/// JUCE's name for the WASAPI exclusive-mode device type.
inline constexpr const char* typeName = "Windows Audio (Exclusive Mode)";

/// What the device guard sees in the device manager.
struct Situation
{
    juce::String deviceType;                          ///< The current type's name.
    juce::AudioDeviceManager::AudioDeviceSetup setup; ///< The current setup. A failed open clears its device names.
    bool deviceOpen = false;
    juce::AudioDeviceManager::AudioDeviceSetup lastOpened; ///< The last setup that opened (any type), for its device names.
    juce::StringArray inputs, outputs;                ///< The current type's devices.
    int defaultInput = -1, defaultOutput = -1;        ///< Indices into inputs/outputs (-1: none).
};

/// The setup to retry with, or nothing: only for the exclusive-mode type, with no device open and Input 1
/// alone requested. The device names are the last opened ones where this type lists them, else this type's
/// defaults, so the retry never names a device that isn't there. It asks for Inputs 1 and 2, so if it fails
/// too, the next check sees two inputs and leaves it alone: one retry per failure, never a loop.
inline std::optional<juce::AudioDeviceManager::AudioDeviceSetup> retryWithInputPair (const Situation& s)
{
    juce::BigInteger inputOneAlone;
    inputOneAlone.setBit (0);

    if (s.deviceType != typeName || s.deviceOpen || s.setup.inputChannels != inputOneAlone)
        return std::nullopt;

    const auto pick = [] (const juce::String& current, const juce::String& lastOpened, const juce::StringArray& names, int fallback) {
        if (current.isNotEmpty() && names.contains (current))
            return current;
        if (lastOpened.isNotEmpty() && names.contains (lastOpened))
            return lastOpened;
        return juce::isPositiveAndBelow (fallback, names.size()) ? names[fallback] : juce::String();
    };

    auto retry = s.setup;
    retry.inputDeviceName = pick (s.setup.inputDeviceName, s.lastOpened.inputDeviceName, s.inputs, s.defaultInput);
    retry.outputDeviceName = pick (s.setup.outputDeviceName, s.lastOpened.outputDeviceName, s.outputs, s.defaultOutput);
    if (retry.inputDeviceName.isEmpty())
        return std::nullopt;

    retry.useDefaultInputChannels = false;
    retry.inputChannels.setRange (0, 2, true);
    return retry;
}
} // namespace platform::exclusivemode

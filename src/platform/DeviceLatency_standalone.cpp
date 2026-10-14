// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone
// The same headers, in the same order, as JUCE's own standalone wrapper (as SampleRateGuard_standalone.cpp),
// and before anything else that includes JUCE. On Windows they include <windows.h>, which defines min and max
// as macros; juce_core's juce_StandardHeader.h #undefs them, but only the first time juce_core is included.
// With DeviceLatency.h (juce_core) first, the undef ran before <windows.h>, the macros survived, and they broke
// juce_FloatVectorOperations.h in the Windows build of v0.3.0.
 #include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
 #include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
 #include <juce_audio_plugin_client/detail/juce_PluginUtilities.h>
 #include <juce_audio_devices/juce_audio_devices.h>
 #include <juce_gui_extra/juce_gui_extra.h>
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

#include "DeviceLatency.h" // after JUCE's standalone headers, which must come first (above)

namespace platform::device
{
Latency reportedLatency()
{
#if JucePlugin_Build_Standalone
    // getInstance() is null in any other wrapper (a plugin later: the host's own latency isn't ours to read).
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        if (auto* device = holder->deviceManager.getCurrentAudioDevice())
        {
            Latency l;
            l.known = true;
            l.inputSamples = device->getInputLatencyInSamples();
            l.outputSamples = device->getOutputLatencyInSamples();
            l.bufferSize = device->getCurrentBufferSizeSamples();
            l.sampleRate = device->getCurrentSampleRate();
            return l;
        }
#endif
    return {};
}

Info currentDevice()
{
    Info info;
#if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        if (auto* device = holder->deviceManager.getCurrentAudioDevice())
        {
            info.known = true;
            info.name = device->getName();
            info.type = device->getTypeName();
            info.latency = reportedLatency();
        }
#endif
    return info;
}

bool showSettings()
{
#if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        holder->showAudioSettingsDialog();
        return true;
    }
#endif
    return false;
}
} // namespace platform::device

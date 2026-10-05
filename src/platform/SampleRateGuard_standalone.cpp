// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "SampleRateGuard.h"
#include "ExclusiveModeInput.h"

#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_Standalone
// The same headers, in the same order, as JUCE's own standalone wrapper
// (juce_audio_plugin_client_Standalone.cpp) includes before juce_StandaloneFilterWindow.h.
 #include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
 #include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
 #include <juce_audio_plugin_client/detail/juce_PluginUtilities.h>
 #include <juce_audio_devices/juce_audio_devices.h>
 #include <juce_gui_extra/juce_gui_extra.h>
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

#include <memory>

// The standalone app's implementation (SampleRateGuard.h explains why and what). JUCE's
// StandalonePluginHolder owns the app's AudioDeviceManager; getInstance() is null in any other wrapper,
// which makes every call here a no-op outside the standalone app.
namespace platform::samplerate
{
#if JucePlugin_Build_Standalone
namespace
{
class Guard final : private juce::Timer
{
public:
    Guard() { startTimer (1000); }

private:
    void timerCallback() override
    {
        auto* holder = juce::StandalonePluginHolder::getInstance();
        if (holder == nullptr)
            return;
        auto& manager = holder->deviceManager;

       #if JUCE_WINDOWS
        // Exclusive mode refused Input 1 alone (a stereo-only endpoint): open Inputs 1 and 2 (ExclusiveModeInput.h).
        if (auto* type = manager.getCurrentDeviceTypeObject())
            if (const auto retry = exclusivemode::retryWithInputPair (type->getTypeName(), manager.getAudioDeviceSetup(),
                                                                      manager.getCurrentAudioDevice() != nullptr))
            {
                const auto error = manager.setAudioDeviceSetup (*retry, true);
                juce::Logger::writeToLog ("BellyDSP: exclusive mode can't open Input 1 alone; opened Inputs 1 and 2"
                                          + (error.isEmpty() ? juce::String() : " (failed: " + error + ")"));
                return;
            }
       #endif

        auto* device = manager.getCurrentAudioDevice();
        if (device == nullptr || std::abs (device->getCurrentSampleRate() - requiredRate) < 0.5)
            return;

        // Given up: the device can't do 48 kHz, or something keeps switching it away. The processor keeps
        // the output muted and its warning up until the user picks 48 kHz in Options.
        if (gaveUp)
            return;
        if (! device->getAvailableSampleRates().contains (requiredRate))
        {
            gaveUp = true;
            juce::Logger::writeToLog ("BellyDSP: the audio device can't run at 48 kHz; the output stays muted");
            return;
        }

        // Count our switches over the last minute; past maxSwitches, another app is fighting us. Stop.
        const auto now = juce::Time::getMillisecondCounter();
        if (now - windowStartMs > 60000)
        {
            windowStartMs = now;
            switchesInWindow = 0;
        }
        if (++switchesInWindow > maxSwitches)
        {
            gaveUp = true;
            juce::Logger::writeToLog ("BellyDSP: something keeps changing the device's sample rate; leaving it alone");
            return;
        }

        auto setup = manager.getAudioDeviceSetup();
        setup.sampleRate = requiredRate;
        // treatAsChosenDevice = true: the standalone wrapper saves this as the user's setup, so the next
        // launch opens at 48 kHz too.
        const auto error = manager.setAudioDeviceSetup (setup, true);
        juce::Logger::writeToLog ("BellyDSP: switched the audio device to 48 kHz"
                                  + (error.isEmpty() ? juce::String() : " (failed: " + error + ")"));
    }

    bool gaveUp = false;
    juce::uint32 windowStartMs = 0;
    int switchesInWindow = 0;
};

std::unique_ptr<Guard> guard;
} // namespace

void start()
{
    if (guard == nullptr)
        guard = std::make_unique<Guard>();
}

void stop()
{
    guard.reset();
}
#else
// Not the standalone app: nothing to guard.
void start() {}
void stop() {}
#endif
} // namespace platform::samplerate

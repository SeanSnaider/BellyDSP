// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The standalone app's audio device handling that doesn't need a real device: the Windows exclusive-mode
// input retry (src/platform/ExclusiveModeInput.h), and the processor's side of it (Input 2 is ignored).

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "platform/ExclusiveModeInput.h"

namespace
{
using namespace testing;
using platform::exclusivemode::Situation;

const juce::String scarlettIn = "Analogue 1 + 2 (Focusrite USB Audio)", scarlettOut = "Speakers (Focusrite USB Audio)";

/// What JUCE's device manager holds after exclusive mode refused the guitar on Input 1 alone (measured on
/// Sean's Scarlett Solo, 2026-10-05): the device closed, its names cleared, one default input, and the last
/// setup that opened (DirectSound, same names) remembered by the guard.
Situation refusedInputOne()
{
    Situation s;
    s.deviceType = platform::exclusivemode::typeName;
    s.setup.sampleRate = 48000.0;
    s.setup.bufferSize = 256;
    s.setup.inputChannels.setBit (0);
    s.setup.outputChannels.setRange (0, 2, true);
    s.lastOpened.inputDeviceName = scarlettIn;
    s.lastOpened.outputDeviceName = scarlettOut;
    s.inputs = { scarlettIn, "Microphone (Blue Snowball )", "Microphone Array (Realtek(R) Audio)" };
    s.outputs = { scarlettOut, "Speakers (Realtek(R) Audio)" };
    s.defaultInput = 0;
    s.defaultOutput = 0;
    return s;
}

class AudioDeviceTests final : public juce::UnitTest
{
public:
    AudioDeviceTests() : juce::UnitTest ("Audio device: exclusive mode and one input", "ampsim") {}

    void runTest() override
    {
        using platform::exclusivemode::retryWithInputPair;

        beginTest ("exclusive mode refusing Input 1 alone is retried once with Inputs 1 and 2 on the last devices that opened");
        {
            const auto failed = refusedInputOne();
            const auto retry = retryWithInputPair (failed);
            expect (retry.has_value());
            if (retry.has_value())
            {
                expect (retry->inputChannels[0] && retry->inputChannels[1] && retry->inputChannels.countNumberOfSetBits() == 2);
                expect (! retry->useDefaultInputChannels);
                expectEquals (retry->inputDeviceName, scarlettIn);
                expectEquals (retry->outputDeviceName, scarlettOut);
                expectEquals (retry->bufferSize, 256);
                expectEquals (retry->sampleRate, 48000.0);
                auto again = failed; // the retry failed too: JUCE clears the names again, the inputs stay a pair
                again.setup = *retry;
                again.setup.inputDeviceName.clear();
                again.setup.outputDeviceName.clear();
                expect (! retryWithInputPair (again).has_value());
            }
            logMessage ("  -> after the refusal (names cleared by JUCE): retried on \"" + scarlettIn + "\" / \"" + scarlettOut
                        + "\" with Inputs 1 and 2, 48 kHz and 256 samples kept; a failed retry is left alone");
        }

        beginTest ("the retry names only devices exclusive mode lists, falling back to its defaults, and acts only on that refusal");
        {
            auto otherNames = refusedInputOne(); // the last devices that opened aren't in this type's lists
            otherNames.lastOpened.inputDeviceName = "Primary Sound Capture Driver";
            otherNames.lastOpened.outputDeviceName = "Primary Sound Driver";
            otherNames.defaultInput = 2;
            otherNames.defaultOutput = 1;
            const auto fallback = retryWithInputPair (otherNames);
            expect (fallback.has_value() && fallback->inputDeviceName == otherNames.inputs[2] && fallback->outputDeviceName == otherNames.outputs[1]);

            auto nothingOpenedYet = otherNames;
            nothingOpenedYet.lastOpened = {};
            nothingOpenedYet.defaultInput = -1;
            expect (! retryWithInputPair (nothingOpenedYet).has_value()); // no input to name

            auto open = refusedInputOne();
            open.deviceOpen = true;
            auto shared = refusedInputOne();
            shared.deviceType = "Windows Audio";
            auto directSound = refusedInputOne();
            directSound.deviceType = "DirectSound";
            auto inputTwo = refusedInputOne();
            inputTwo.setup.inputChannels.clear();
            inputTwo.setup.inputChannels.setBit (1); // opens two channels, so it never fails this way
            for (const auto* s : { &open, &shared, &directSound, &inputTwo })
                expect (! retryWithInputPair (*s).has_value());
            logMessage ("  -> unlisted last devices: this type's defaults instead; left alone: nothing to name, an open device, "
                        "shared mode, DirectSound, Input 2 alone");
        }

        beginTest ("with Inputs 1 and 2 open, the processor hears Input 1 alone: Input 2 never reaches the output");
        {
            // JUCE's AudioProcessorPlayer puts device channel 0 in the buffer's first channel (the processor's one
            // input) and device channel 1 in its second (an output). Same Input 1, with and without a loud tone
            // on Input 2: the outputs must be identical.
            AmpSimProcessor withPair, alone;
            withPair.prepareToPlay (fs, blockSize);
            alone.prepareToPlay (fs, blockSize);
            const auto input1 = guitarDI ((int) (2.0 * fs));
            const auto input2 = sine (1000.0, 0.9, (int) input1.size());
            juce::AudioBuffer<float> a (2, blockSize), b (2, blockSize);
            juce::MidiBuffer midi;
            double worst = 0.0, loudest = 0.0;
            for (size_t start = 0; start + blockSize <= input1.size(); start += blockSize)
            {
                a.copyFrom (0, 0, input1.data() + start, blockSize);
                a.copyFrom (1, 0, input2.data() + start, blockSize);
                b.copyFrom (0, 0, input1.data() + start, blockSize);
                b.clear (1, 0, blockSize);
                withPair.processBlock (a, midi);
                alone.processBlock (b, midi);
                for (int ch = 0; ch < 2; ++ch)
                    for (int n = 0; n < blockSize; ++n)
                    {
                        worst = std::max (worst, (double) std::abs (a.getSample (ch, n) - b.getSample (ch, n)));
                        loudest = std::max (loudest, (double) std::abs (b.getSample (ch, n)));
                    }
            }
            expectEquals (worst, 0.0);
            expectGreaterThan (loudest, 0.01); // the comparison is of real output, not two silences
            logMessage ("  -> 2 s of guitar on Input 1, with and without a -0.9 dBFS 1 kHz tone on Input 2: outputs differ by "
                        + juce::String (worst) + " (peak output " + juce::String (loudest, 3) + ")");
        }
    }
};

AudioDeviceTests audioDeviceTests;
} // namespace

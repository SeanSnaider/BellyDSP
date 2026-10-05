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
using Setup = juce::AudioDeviceManager::AudioDeviceSetup;

/// A failed exclusive-mode request for the guitar on Input 1 alone, as JUCE's device manager holds it.
Setup inputOneAlone()
{
    Setup s;
    s.inputDeviceName = "Analogue 1 + 2 (Focusrite USB Audio)";
    s.outputDeviceName = "Speakers (Focusrite USB Audio)";
    s.sampleRate = 48000.0;
    s.bufferSize = 256;
    s.inputChannels.setBit (0);
    s.outputChannels.setRange (0, 2, true);
    return s;
}

class AudioDeviceTests final : public juce::UnitTest
{
public:
    AudioDeviceTests() : juce::UnitTest ("Audio device: exclusive mode and one input", "ampsim") {}

    void runTest() override
    {
        using platform::exclusivemode::retryWithInputPair;
        using platform::exclusivemode::typeName;

        beginTest ("exclusive mode refusing Input 1 alone is retried once with Inputs 1 and 2, and nothing else is");
        {
            const auto failed = inputOneAlone();
            const auto retry = retryWithInputPair (typeName, failed, false);
            expect (retry.has_value());
            if (retry.has_value())
            {
                expect (retry->inputChannels[0] && retry->inputChannels[1] && retry->inputChannels.countNumberOfSetBits() == 2);
                expect (! retry->useDefaultInputChannels);
                auto rest = *retry; // everything but the inputs is the request as it was
                rest.inputChannels = failed.inputChannels;
                rest.useDefaultInputChannels = failed.useDefaultInputChannels;
                expect (rest == failed);
                expect (! retryWithInputPair (typeName, *retry, false).has_value()); // the retry failed too: left alone
            }

            auto inputTwo = failed;
            inputTwo.inputChannels.clear();
            inputTwo.inputChannels.setBit (1); // opens two channels, so it never fails this way
            auto noInput = failed;
            noInput.inputDeviceName.clear();
            expect (! retryWithInputPair (typeName, failed, true).has_value());           // it opened
            expect (! retryWithInputPair ("Windows Audio", failed, false).has_value());   // shared mode converts
            expect (! retryWithInputPair ("DirectSound", failed, false).has_value());
            expect (! retryWithInputPair (typeName, inputTwo, false).has_value());
            expect (! retryWithInputPair (typeName, noInput, false).has_value());
            logMessage ("  -> retried: exclusive mode, closed, Input 1 alone -> Inputs 1 and 2, the rest unchanged; left alone: open, "
                        "shared mode, DirectSound, Input 2 alone, no input device, and a failed retry");
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

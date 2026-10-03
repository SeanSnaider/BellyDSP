// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/AmpSection.h"

#include <chrono>
#include <numeric>

namespace
{
using namespace testing;

std::vector<float> runSection (ampsim::AmpSection& amp, const std::vector<float>& input,
                               const std::function<void (size_t)>& beforeBlock = {})
{
    const ampsim::BlockContext context;
    return runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
    {
        if (beforeBlock)
            beforeBlock (start);

        amp.process (block.getSubsetChannelBlock (0, 1), context);
    }).left;
}

std::vector<float> runModel (ampsim::NamAmp& model, const std::vector<float>& input)
{
    const ampsim::BlockContext context;
    return runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t)
    {
        model.process (block.getSubsetChannelBlock (0, 1), context);
    }).left;
}

std::vector<float> slice (const std::vector<float>& x, size_t from, size_t to)
{
    return { x.begin() + (long) from, x.begin() + (long) std::min (to, x.size()) };
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

class AmpSectionTests final : public juce::UnitTest
{
public:
    AmpSectionTests() : juce::UnitTest ("Amp section (three slots)", "ampsim") {}

    void runTest() override
    {
        const auto a1 = exampleModel ("wavenet_a1_standard.nam");
        const auto lstm = exampleModel ("lstm.nam");
        const auto small = exampleModel ("wavenet.nam");
        const auto input = guitarDI ((int) (2.0 * fs));
        const size_t switchAt = 300 * blockSize; // 0.8 s in
        const auto fade = (size_t) juce::roundToInt (fs * ampsim::AmpSection::switchSeconds);

        beginTest ("the test signal is actually playing where the switches happen");
        {
            // Guards the tests below against passing in silence, which is how a switch looks perfect.
            const auto level = toDb (rms (input.data() + switchAt - 4800, 9600));
            expectGreaterThan (level, -30.0);
            logMessage ("  -> guitar DI level within 100 ms of the switch point: " + juce::String (level, 1) + " dBFS RMS");
        }

        beginTest ("all three slots keep running, so a switch lands on a model with its full history");
        {
            ampsim::AmpSection amp;
            amp.slot (0).model.loadModel (lstm, false);
            amp.slot (1).model.loadModel (a1, false);
            amp.prepare (fs, blockSize);

            ampsim::NamAmp warm, cold; // the same capture: one fed the whole input, one only from the switch
            warm.loadModel (a1, false);
            warm.prepare (fs, blockSize);
            cold.loadModel (a1, false);
            cold.prepare (fs, blockSize);

            const auto out = runSection (amp, input, [&] (size_t start) { if (start == switchAt) amp.selectSlot (1); });
            const auto warmOut = runModel (warm, input);
            const auto coldOut = runModel (cold, slice (input, switchAt, input.size()));

            // Compare from the end of the fade to the end of the A1 model's receptive field (~85 ms),
            // where a model that just started would still be missing history.
            const auto from = switchAt + 8 * blockSize, to = switchAt + 4096;
            const auto warmError = relativeErrorDb (slice (out, from, to), slice (warmOut, from, to));
            const auto coldError = relativeErrorDb (slice (out, from, to), slice (coldOut, from - switchAt, to - switchAt));

            expectLessThan (warmError, -100.0);
            expectGreaterThan (coldError, -60.0);
            logMessage ("  -> after switching to slot 2: matches a model that ran the whole time to " + dB (warmError)
                        + "; a model that only started at the switch would be off by " + dB (coldError)
                        + " for the next 85 ms (its missing history)");
        }

        beginTest ("a slot switch is exactly the 20 ms equal-power crossfade");
        {
            ampsim::AmpSection amp;
            amp.slot (0).model.loadModel (lstm, false);
            amp.slot (1).model.loadModel (a1, false);
            amp.prepare (fs, blockSize);

            ampsim::NamAmp refA, refB;
            refA.loadModel (lstm, false);
            refA.prepare (fs, blockSize);
            refB.loadModel (a1, false);
            refB.prepare (fs, blockSize);

            bool sawSwitching = false;
            const auto out = runSection (amp, input, [&] (size_t start)
            {
                if (start == switchAt)
                    amp.selectSlot (1);
                sawSwitching = sawSwitching || amp.isSwitching();
            });
            const auto a = runModel (refA, input);
            const auto b = runModel (refB, input);

            // Slot 2's position ramps 0 -> 1 and slot 1's 1 -> 0 in 960 linear steps; each contributes
            // sin(pi/2 position) of its output.
            std::vector<float> expected (out.size());
            for (size_t n = 0; n < out.size(); ++n)
            {
                const auto t = n < switchAt ? 0.0 : std::min (1.0, (double) (n - switchAt + 1) / (double) fade);
                expected[n] = (float) (std::sin (juce::MathConstants<double>::halfPi * t) * b[n]
                                       + std::sin (juce::MathConstants<double>::halfPi * (1.0 - t)) * a[n]);
            }

            const auto deviation = maxAbsDifference (out, expected);
            const auto steady = std::max (maxStep (a, 0, switchAt), maxStep (b, switchAt));
            const auto during = maxStep (out, switchAt - 1, switchAt + fade + 1);
            const auto hardJump = std::abs (b[switchAt] - a[switchAt - 1]);

            expect (sawSwitching && ! amp.isSwitching());
            expectLessThan (deviation, 1.0e-5);
            expectLessThan (during, steady * 1.5);
            logMessage ("  -> follows sin/cos over " + juce::String ((int) fade) + " samples (20 ms): max deviation "
                        + juce::String (deviation, 8) + "; largest step during the switch " + juce::String (during, 5)
                        + " vs. " + juce::String (steady, 5) + " steady (a hard switch would jump " + juce::String (hardJump, 5) + ")");

            writeWav (proofDir().getChildFile ("slot_switch_lstm_to_a1.wav"), out);
        }

        beginTest ("switching twice within 5 ms redirects smoothly and settles on the last slot");
        {
            ampsim::AmpSection amp;
            amp.slot (0).model.loadModel (lstm, false);
            amp.slot (1).model.loadModel (a1, false);
            amp.slot (2).model.loadModel (small, false);
            amp.prepare (fs, blockSize);

            ampsim::NamAmp refA, refB, refC;
            const std::array<std::pair<ampsim::NamAmp*, juce::File>, 3> refs { { { &refA, lstm }, { &refB, a1 }, { &refC, small } } };
            for (const auto& [ref, file] : refs)
            {
                ref->loadModel (file, false);
                ref->prepare (fs, blockSize);
            }

            const auto out = runSection (amp, input, [&] (size_t start)
            {
                if (start == switchAt) amp.selectSlot (1);
                if (start == switchAt + 2 * blockSize) amp.selectSlot (2);
            });
            const auto a = runModel (refA, input), b = runModel (refB, input), c = runModel (refC, input);

            const auto steady = std::max ({ maxStep (a), maxStep (b), maxStep (c) });
            const auto during = maxStep (out, switchAt - 1, switchAt + 2 * blockSize + 2 * fade);
            const auto settled = switchAt + 2 * blockSize + 2 * fade;
            const auto settledError = relativeErrorDb (slice (out, settled, out.size()), slice (c, settled, c.size()));

            // Mid-redirect, up to three slots overlap with weights summing past 1, so steps can grow a
            // little beyond any single slot's; a click would be a jump the size of the signal itself.
            expectEquals (amp.getSelectedSlot(), 2);
            expectLessThan (during, steady * 1.6);
            expectLessThan (settledError, -100.0);
            logMessage ("  -> 1 -> 2 -> 3 within 5.3 ms: largest step " + juce::String (during, 5) + " vs. "
                        + juce::String (steady, 5) + " for any slot alone; afterwards it's slot 3 to " + dB (settledError));
        }

        beginTest ("tone controls hit their targets, and flat is bit-transparent");
        {
            ampsim::AmpTone flat;
            flat.prepare (fs);
            auto noise = whiteNoise (48000, 0.5f, 21);
            auto processed = noise;
            flat.process (processed.data(), (int) processed.size());
            expectEquals (maxAbsDifference (noise, processed), 0.0);

            juce::StringArray results;
            for (int b = 0; b < ampsim::AmpTone::numBands; ++b)
            {
                const auto& spec = ampsim::AmpTone::bands[(size_t) b];
                ampsim::AmpTone tone;
                tone.setGainDb ((ampsim::AmpTone::Band) b, 6.0f);
                tone.prepare (fs); // snaps to the target

                const auto atCentre = tone.responseDb (spec.frequency);
                const bool isShelf = spec.type == ampsim::Svf::Type::lowShelf || spec.type == ampsim::Svf::Type::highShelf;
                expectWithinAbsoluteError (atCentre, isShelf ? 3.0 : 6.0, 1.0e-6, spec.name);

                // And the running filter really does that: measure a sine at the band's frequency.
                const auto probe = sine (spec.frequency, 0.1, 48000);
                auto out = probe;
                tone.process (out.data(), (int) out.size());
                const auto measured = toDb (rms (out.data() + 24000, 24000) / rms (probe.data() + 24000, 24000));
                expectWithinAbsoluteError (measured, atCentre, 0.02, spec.name);

                results.add (juce::String (spec.name) + " " + juce::String (atCentre, 3) + " dB at "
                             + juce::String (spec.frequency, 0) + " Hz (measured " + juce::String (measured, 3) + ")");
            }

            logMessage ("  -> flat: bit-transparent. Each band at +6 dB: " + results.joinIntoString (", "));
        }

        beginTest ("tone knob moves ramp over 25 ms without zipper noise");
        {
            // Bass from 0 to +12 dB on a 150 Hz sine: AmpTone (coefficients every 32 samples) against
            // an ideal filter redesigned every sample from the same 25 ms dB ramp.
            const auto probe = sine (150.0, 0.3, 9600);
            ampsim::AmpTone tone;
            tone.prepare (fs);
            tone.setGainDb (ampsim::AmpTone::bass, 12.0f);
            auto out = probe;
            tone.process (out.data(), (int) out.size());

            // AmpTone picks up a knob change at its next 32-sample update point (here, sample 32), so
            // the ideal ramp starts there too. What's left is the stepping error alone.
            ampsim::Svf ideal;
            const auto& spec = ampsim::AmpTone::bands[ampsim::AmpTone::bass];
            std::vector<double> idealOut (probe.size());
            for (size_t n = 0; n < probe.size(); ++n)
            {
                const auto db = 12.0 * juce::jlimit (0.0, 1.0, ((double) n - 32.0) / 1200.0);
                ideal.setCoefficients (ampsim::Svf::design (spec.type, spec.frequency, spec.q, db, fs));
                idealOut[n] = ideal.processSample (probe[n]);
            }

            const auto error = relativeErrorDb (out, idealOut);
            expectLessThan (error, -40.0);
            logMessage ("  -> 0 to +12 dB bass ramp: 32-sample coefficient updates differ from per-sample updates by " + dB (error));
        }

        beginTest ("CPU: three A1 slots with tone controls, 128-sample buffers, with and without denormal flushing");
        {
            const auto longInput = guitarDI ((int) (10.0 * fs));

            // The app flushes denormals to zero in every callback (juce::ScopedNoDenormals), so that's
            // the number that counts. Measuring without it too shows what the flush is worth.
            const auto measure = [&] (bool flushDenormals)
            {
                ampsim::AmpSection amp;
                for (int s = 0; s < ampsim::AmpSection::numSlots; ++s)
                {
                    amp.slot (s).model.loadModel (a1, false);
                    amp.slot (s).tone.setGainDb (ampsim::AmpTone::mid, 3.0f);
                }
                amp.prepare (fs, blockSize);

                std::unique_ptr<juce::ScopedNoDenormals> flush;
                if (flushDenormals)
                    flush = std::make_unique<juce::ScopedNoDenormals>();

                juce::AudioBuffer<float> buffer (1, blockSize);
                const ampsim::BlockContext context;
                std::vector<double> micros;

                for (size_t start = 0; start + blockSize <= longInput.size(); start += blockSize)
                {
                    buffer.copyFrom (0, 0, longInput.data() + start, blockSize);
                    const auto t0 = std::chrono::steady_clock::now();
                    amp.process (juce::dsp::AudioBlock<float> (buffer), context);
                    micros.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
                }

                std::sort (micros.begin(), micros.end());
                return std::array<double, 3> { std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size(),
                                               micros[(size_t) (0.99 * (double) (micros.size() - 1))], micros.back() };
            };

            const auto flushed = measure (true);
            const auto unflushed = measure (false);

            expectLessThan (flushed[0], 0.5 * deadlineMicros);
            logMessage ("  -> as the app runs it (denormals flushed): mean " + juce::String (flushed[0], 1) + " us ("
                        + juce::String (100.0 * flushed[0] / deadlineMicros, 1) + "% of the 2.67 ms deadline), p99 "
                        + juce::String (flushed[1], 1) + " us, worst " + juce::String (flushed[2], 1) + " us");
            logMessage ("  -> without the flush: mean " + juce::String (unflushed[0], 1) + " us ("
                        + juce::String (100.0 * unflushed[0] / deadlineMicros, 1) + "%), p99 " + juce::String (unflushed[1], 1)
                        + " us, worst " + juce::String (unflushed[2], 1) + " us");
        }

        beginTest ("a footswitch program change switches right away, and the slot knob catches up");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            midi.addEvent (juce::MidiMessage::programChange (1, 2), 0);
            buffer.clear();
            p.processBlock (buffer, midi);
            const auto slotAfterMidi = p.getChain().amp.getSelectedSlot();
            const auto knobBefore = juce::roundToInt (p.parameters.getRawParameterValue (AmpSimProcessor::slotParamId)->load());

            p.runHousekeeping(); // what the 20 Hz timer does
            const auto knobAfter = juce::roundToInt (p.parameters.getRawParameterValue (AmpSimProcessor::slotParamId)->load());

            midi.clear();
            midi.addEvent (juce::MidiMessage::programChange (1, 7), 0); // no slot 8: ignored
            p.processBlock (buffer, midi);

            expectEquals (slotAfterMidi, 2);
            expectEquals (knobBefore, 0);
            expectEquals (knobAfter, 2);
            expectEquals (p.getChain().amp.getSelectedSlot(), 2);
            logMessage ("  -> program change 3: slot 3 selected in the same audio block; the knob read slot "
                        + juce::String (knobBefore + 1) + " until the timer ran, then slot " + juce::String (knobAfter + 1)
                        + "; program change 8 ignored");
        }

        beginTest ("slots, captures, and tone survive save and restore, and the saved slot starts without a fade");
        {
            AmpSimProcessor original;
            original.loadModel (1, small);
            original.loadModel (2, lstm);
            setParam (original, AmpSimProcessor::slotParamId, 2.0f);
            setParam (original, AmpSimProcessor::ampParamId (2, "mid"), 4.0f);
            waitForLoads (original);

            juce::MemoryBlock state;
            original.getStateInformation (state);

            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            waitForLoads (restored);
            restored.prepareToPlay (fs, blockSize);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            buffer.clear();
            restored.processBlock (buffer, midi);

            const auto status = restored.getStatus();
            expectEquals (restored.getChain().amp.getSelectedSlot(), 2);
            expect (! restored.getChain().amp.isSwitching(), "the restored slot should start without a crossfade");
            expectEquals (restored.parameters.getRawParameterValue (AmpSimProcessor::ampParamId (2, "mid"))->load(), 4.0f);
            expect (status.model[1].contains ("wavenet") && status.model[2].contains ("lstm"));
            expectEquals (status.model[0], juce::String ("Empty"));
            logMessage ("  -> restored: slot 3 active with no fade; slot 2 \"" + status.model[1] + "\", slot 3 \"" + status.model[2]
                        + "\", slot 1 " + status.model[0] + "; slot 3 Mid +4.0 dB");
        }

        beginTest ("a milestone 1 state (one model) loads into slot 1");
        {
            AmpSimProcessor fresh;
            auto tree = fresh.parameters.copyState();
            tree.setProperty (AmpSimProcessor::legacyModelPathKey, a1.getFullPathName(), nullptr);
            juce::MemoryBlock state;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), state);

            AmpSimProcessor p;
            p.setStateInformation (state.getData(), (int) state.getSize());
            waitForLoads (p);

            expect (p.getStatus().model[0].contains ("wavenet_a1_standard"));
            expect (p.parameters.state.hasProperty (AmpSimProcessor::modelPathKey (0)));
            expect (! p.parameters.state.hasProperty (AmpSimProcessor::legacyModelPathKey));
            logMessage ("  -> old \"modelPath\" became amp1ModelPath; slot 1: \"" + p.getStatus().model[0] + "\"");
        }
    }
};

AmpSectionTests ampSectionTests;
} // namespace

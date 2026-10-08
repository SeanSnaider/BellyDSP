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
                               const std::function<void (size_t)>& beforeBlock = {},
                               const std::function<void (size_t)>& afterBlock = {})
{
    const ampsim::BlockContext context;
    return runInBlocks (input, blockSize, [&] (juce::dsp::AudioBlock<float>& block, size_t start)
    {
        if (beforeBlock)
            beforeBlock (start);

        amp.process (block.getSubsetChannelBlock (0, 1), context);

        if (afterBlock)
            afterBlock (start);
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

/// A capture through a lone NamAmp that runs the whole input: what an amp that had always been running plays.
std::vector<float> always (const juce::File& file, const std::vector<float>& input)
{
    ampsim::NamAmp model;
    model.loadModel (file, false);
    model.prepare (fs, blockSize);
    return runModel (model, input);
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

/// The ideal switch from `from` to `to`, both amps always running: `from` alone until `fadeStart`, then the 20 ms
/// equal-power crossfade (the incoming amp at sin(pi/2 t), the outgoing at sin(pi/2 (1 - t)), t ramping by 1/fade
/// per sample from the first sample of the fade), then `to` alone.
std::vector<float> idealSwitch (const std::vector<float>& from, const std::vector<float>& to, size_t fadeStart, size_t fade)
{
    std::vector<float> expected (from.size());
    for (size_t n = 0; n < from.size(); ++n)
    {
        const auto t = n < fadeStart ? 0.0 : std::min (1.0, (double) (n - fadeStart + 1) / (double) fade);
        expected[n] = (float) (std::sin (juce::MathConstants<double>::halfPi * t) * to[n]
                               + std::sin (juce::MathConstants<double>::halfPi * (1.0 - t)) * from[n]);
    }
    return expected;
}

class AmpSectionTests final : public juce::UnitTest
{
public:
    AmpSectionTests() : juce::UnitTest ("Amp section (one amp at a time)", "ampsim") {}

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

        beginTest ("only the selected amp runs; a switch warms the new amp unheard for its receptive field, then is exactly the 20 ms equal-power crossfade of two always-running amps");
        {
            ampsim::AmpSection amp;
            amp.amp (0).model.loadModel (lstm, false);
            amp.amp (1).model.loadModel (a1, false);
            amp.amp (2).model.loadModel (small, false);
            amp.prepare (fs, blockSize);

            const auto warmup = (size_t) amp.warmupSamplesFor (1, blockSize);
            std::vector<int> runningAmps, runningModels;
            const auto out = runSection (amp, input, [&] (size_t start) { if (start == switchAt) amp.selectAmp (1); },
                                         [&] (size_t)
                                         {
                                             runningAmps.push_back (amp.getRunningAmps());
                                             runningModels.push_back (amp.getRunningModels());
                                         });
            const auto a = always (lstm, input), b = always (a1, input);
            const auto expected = idealSwitch (a, b, switchAt + warmup, fade);

            // When the new amp first reaches the output: the first sample that differs from the old amp alone.
            size_t audibleAt = 0;
            for (size_t n = 0; n < out.size() && audibleAt == 0; ++n)
                if (std::abs (out[n] - a[n]) > 1.0e-7f)
                    audibleAt = n;

            const auto deviation = maxAbsDifference (out, expected);
            const auto steady = std::max (maxStep (a, 0, switchAt), maxStep (b, switchAt));
            const auto during = maxStep (out, switchAt - 1, switchAt + warmup + fade + 1);
            const auto hardJump = std::abs (b[switchAt] - a[switchAt - 1]);
            const auto blockOf = [&] (size_t sample) { return sample / blockSize; };
            const auto lastBlock = runningAmps.size() - 1;

            expectEquals ((int) warmup, 4096, "a standard WaveNet's 4093-sample receptive field, in whole 128-sample buffers");
            expectEquals ((int) audibleAt, (int) (switchAt + warmup));
            expectLessThan (deviation, 1.0e-6);
            expectLessThan (during, steady * 1.5);
            expectEquals (runningAmps[blockOf (switchAt) - 1], 1, "steady state: one amp");
            expectEquals (runningAmps[blockOf (switchAt)], 2, "the warm-up: both");
            expectEquals (runningAmps[blockOf (switchAt + warmup + fade) + 1], 1, "after the fade: the new one alone");
            expectEquals (runningAmps[lastBlock], 1);
            expect (amp.isRunning (1) && ! amp.isRunning (0) && ! amp.isRunning (2) && ! amp.isSwitching());
            logMessage ("  -> switch asked for at " + juce::String (1000.0 * (double) switchAt / fs, 1) + " ms; the new amp warms unheard for "
                        + juce::String ((int) warmup) + " samples (" + juce::String (1000.0 * (double) warmup / fs, 1) + " ms) and is first heard "
                        + juce::String (1000.0 * (double) (audibleAt - switchAt) / fs, 1) + " ms after the request; the fade ends "
                        + juce::String (1000.0 * (double) (audibleAt - switchAt + fade) / fs, 1) + " ms after it");
            logMessage ("  -> against the ideal crossfade of two always-running amps: max deviation " + juce::String (deviation, 9)
                        + "; largest step " + juce::String (during, 5) + " vs. " + juce::String (steady, 5) + " steady (a hard switch would jump "
                        + juce::String (hardJump, 5) + ")");
            logMessage ("  -> amps running: " + juce::String (runningAmps[blockOf (switchAt) - 1]) + " before, " + juce::String (runningAmps[blockOf (switchAt)])
                        + " while warming and fading, " + juce::String (runningAmps[lastBlock]) + " after; NAM models: "
                        + juce::String (runningModels[blockOf (switchAt) - 1]) + ", " + juce::String (runningModels[blockOf (switchAt)]) + ", "
                        + juce::String (runningModels[lastBlock]));
            writeWav (proofDir().getChildFile ("amp_switch_lstm_to_a1.wav"), out);
        }

        beginTest ("a new amp that only started at the switch is wrong for its receptive field: why it warms up unheard");
        {
            ampsim::NamAmp cold;
            cold.loadModel (a1, false);
            cold.prepare (fs, blockSize);
            const auto warm = always (a1, input);
            const auto coldOut = runModel (cold, slice (input, switchAt, input.size()));
            const auto early = relativeErrorDb (slice (coldOut, 8 * blockSize, 4000), slice (warm, switchAt + 8 * blockSize, switchAt + 4000));
            const auto late = relativeErrorDb (slice (coldOut, 4096, 8192), slice (warm, switchAt + 4096, switchAt + 8192));
            expectGreaterThan (early, -60.0);
            expectLessThan (late, -100.0);
            logMessage ("  -> a model started at the switch, against one running all along: " + dB (early) + " over its first 85 ms (heard, that "
                        "would be the click), " + dB (late) + " after 4096 samples");
        }

        beginTest ("switching again during the warm-up retargets: the newest amp warms, the overtaken one stops; back to the heard amp, nothing changes");
        {
            const auto a = always (lstm, input), c = always (small, input);
            {
                ampsim::AmpSection amp;
                amp.amp (0).model.loadModel (lstm, false);
                amp.amp (1).model.loadModel (a1, false);
                amp.amp (2).model.loadModel (small, false);
                amp.prepare (fs, blockSize);
                const auto second = switchAt + 10 * blockSize; // 27 ms into the first warm-up
                const auto warmup = (size_t) amp.warmupSamplesFor (2, blockSize);
                int maxRunning = 0;
                bool overtakenStopped = false;
                const auto out = runSection (amp, input, [&] (size_t start)
                {
                    if (start == switchAt) amp.selectAmp (1);
                    if (start == second) amp.selectAmp (2);
                }, [&] (size_t start)
                {
                    maxRunning = std::max (maxRunning, amp.getRunningAmps());
                    if (start == second)
                        overtakenStopped = ! amp.isRunning (1) && amp.isRunning (2) && amp.isRunning (0);
                });
                const auto expected = idealSwitch (a, c, second + warmup, fade);
                const auto deviation = maxAbsDifference (out, expected);
                expect (overtakenStopped);
                expectEquals (maxRunning, 2);
                expectLessThan (deviation, 1.0e-6);
                logMessage ("  -> amp 2 asked for, then amp 3 " + juce::String (1000.0 * 10 * blockSize / fs, 1) + " ms later: amp 2 stopped at once (never heard), amp 3 "
                            "warmed from its request and faded in " + juce::String (1000.0 * (double) warmup / fs, 1) + " ms after it, at most "
                            + juce::String (maxRunning) + " amps running; max deviation from the ideal switch " + juce::String (deviation, 9));
            }
            {
                ampsim::AmpSection amp;
                amp.amp (0).model.loadModel (lstm, false);
                amp.amp (1).model.loadModel (a1, false);
                amp.prepare (fs, blockSize);
                const auto out = runSection (amp, input, [&] (size_t start)
                {
                    if (start == switchAt) amp.selectAmp (1);
                    if (start == switchAt + 10 * blockSize) amp.selectAmp (0);
                });
                const auto deviation = maxAbsDifference (out, a);
                expectEquals (deviation, 0.0);
                expect (amp.isRunning (0) && ! amp.isRunning (1) && ! amp.isSwitching());
                logMessage ("  -> amp 2 asked for, then amp 1 again during the warm-up: the output never left amp 1 (difference " + juce::String (deviation) + ")");
            }
        }

        beginTest ("switching back during the fade redirects at once (the amp fading out is still warm), with no jump");
        {
            ampsim::AmpSection amp;
            amp.amp (0).model.loadModel (lstm, false);
            amp.amp (1).model.loadModel (a1, false);
            amp.prepare (fs, blockSize);
            const auto warmup = (size_t) amp.warmupSamplesFor (1, blockSize);
            const auto back = switchAt + warmup + 3 * blockSize; // 8 ms into the 20 ms fade
            const auto out = runSection (amp, input, [&] (size_t start)
            {
                if (start == switchAt) amp.selectAmp (1);
                if (start == back) amp.selectAmp (0);
            });
            const auto a = always (lstm, input), b = always (a1, input);
            const auto steady = std::max (maxStep (a), maxStep (b));
            const auto during = maxStep (out, back - blockSize, back + 2 * fade);
            const auto settled = back + 2 * fade;
            const auto settledError = relativeErrorDb (slice (out, settled, out.size()), slice (a, settled, a.size()));
            expectLessThan (during, steady * 1.5);
            expectLessThan (settledError, -100.0);
            expect (amp.isRunning (0) && ! amp.isRunning (1));
            logMessage ("  -> back to amp 1 8 ms into the fade: largest step " + juce::String (during, 5) + " vs. " + juce::String (steady, 5)
                        + " steady; amp 1 again to " + dB (settledError) + " once the ramps settle, amp 2 stopped");
        }

        beginTest ("rapid switches (a new amp every 13 to 40 ms, during warm-ups and fades) never click, never run more than three amps, and settle on the last");
        {
            ampsim::AmpSection amp;
            amp.amp (0).model.loadModel (lstm, false);
            amp.amp (1).model.loadModel (a1, false);
            amp.amp (2).model.loadModel (small, false);
            amp.prepare (fs, blockSize);
            const auto longInput = guitarDI ((int) (3.0 * fs));
            juce::Random random (7);
            size_t next = switchAt;
            int maxRunning = 0, switches = 0, lastAmp = 0;
            const auto out = runSection (amp, longInput, [&] (size_t start)
            {
                if (start >= next && start < (size_t) (2.0 * fs))
                {
                    lastAmp = (lastAmp + 1 + random.nextInt (2)) % 3;
                    amp.selectAmp (lastAmp);
                    ++switches;
                    next = start + (size_t) (5 + random.nextInt (11)) * blockSize;
                }
            }, [&] (size_t) { maxRunning = std::max (maxRunning, amp.getRunningAmps()); });
            const std::array<juce::File, 3> files { lstm, a1, small };
            const auto last = always (files[(size_t) lastAmp], longInput);
            std::vector<float> refs[3] = { always (lstm, longInput), always (a1, longInput), always (small, longInput) };
            const auto steady = std::max ({ maxStep (refs[0]), maxStep (refs[1]), maxStep (refs[2]) });
            const auto during = maxStep (out, switchAt - 1, (size_t) (2.3 * fs));
            const auto settled = (size_t) (2.5 * fs);
            const auto settledError = relativeErrorDb (slice (out, settled, out.size()), slice (last, settled, last.size()));
            expectLessThan (during, steady * 1.6);
            expectLessOrEqual (maxRunning, 3);
            expectLessThan (settledError, -100.0);
            logMessage ("  -> " + juce::String (switches) + " switches in 1.2 s: largest step " + juce::String (during, 5) + " vs. " + juce::String (steady, 5)
                        + " for any amp alone; at most " + juce::String (maxRunning) + " amps running at once; then the last one to " + dB (settledError));
        }

        beginTest ("an amp with no capture passes the input through, and a switch to it needs no warm-up");
        {
            ampsim::AmpSection amp;
            amp.amp (0).model.loadModel (a1, false);
            amp.prepare (fs, blockSize);
            const auto out = runSection (amp, input, [&] (size_t start) { if (start == switchAt) amp.selectAmp (8); });
            const auto a = always (a1, input);
            const auto expected = idealSwitch (a, input, switchAt, fade);
            expectEquals (amp.warmupSamplesFor (8, blockSize), 0);
            expectLessThan (maxAbsDifference (out, expected), 1.0e-6);
            logMessage ("  -> to an empty amp: the 20 ms fade starts in the same buffer as the request");
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

        beginTest ("CPU: the selected A1 amp with tone controls (the other eight loaded, idle), 128-sample buffers, with and without denormal flushing");
        {
            const auto longInput = guitarDI ((int) (10.0 * fs));

            // The app flushes denormals to zero in every callback (juce::ScopedNoDenormals), so that's
            // the number that counts. Measuring without it too shows what the flush is worth.
            const auto measure = [&] (bool flushDenormals)
            {
                ampsim::AmpSection amp;
                for (int s = 0; s < ampsim::AmpSection::numAmps; ++s)
                {
                    amp.amp (s).model.loadModel (a1, false);
                    amp.amp (s).tone.setGainDb (ampsim::AmpTone::mid, 3.0f);
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

                expectEquals (amp.getRunningAmps(), 1);
                std::sort (micros.begin(), micros.end());
                return std::array<double, 3> { std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size(),
                                               micros[(size_t) (0.99 * (double) (micros.size() - 1))], micros.back() };
            };

            const auto flushed = measure (true);
            const auto unflushed = measure (false);

            expectLessThan (flushed[0], 0.25 * deadlineMicros * cpuBudgetScale());
            logMessage ("  -> as the app runs it (denormals flushed): mean " + juce::String (flushed[0], 1) + " us ("
                        + juce::String (100.0 * flushed[0] / deadlineMicros, 1) + "% of the 2.67 ms deadline), p99 "
                        + juce::String (flushed[1], 1) + " us, worst " + juce::String (flushed[2], 1) + " us");
            logMessage ("  -> without the flush: mean " + juce::String (unflushed[0], 1) + " us ("
                        + juce::String (100.0 * unflushed[0] / deadlineMicros, 1) + "%), p99 " + juce::String (unflushed[1], 1)
                        + " us, worst " + juce::String (unflushed[2], 1) + " us");
        }

        beginTest ("a footswitch program change switches right away (PC 1 to 9: the shelf's amps, then your capture), and the amp knob catches up");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            midi.addEvent (juce::MidiMessage::programChange (1, 2), 0);
            buffer.clear();
            p.processBlock (buffer, midi);
            const auto ampAfterMidi = p.getChain().amp.getSelectedAmp();
            const auto knobBefore = p.getSelectedAmp();

            // A buffer before the timer runs: the parameter's old value must not switch it back.
            midi.clear();
            p.processBlock (buffer, midi);
            const auto stillSelected = p.getChain().amp.getSelectedAmp();

            p.runHousekeeping(); // what the 20 Hz timer does
            const auto knobAfter = p.getSelectedAmp();

            midi.addEvent (juce::MidiMessage::programChange (1, 8), 0); // PC 9: your capture
            p.processBlock (buffer, midi);
            p.runHousekeeping();
            const auto yours = p.getSelectedAmp();

            midi.clear();
            midi.addEvent (juce::MidiMessage::programChange (1, 9), 0); // no amp 10: ignored
            p.processBlock (buffer, midi);
            p.runHousekeeping();

            expectEquals (ampAfterMidi, 2);
            expectEquals (stillSelected, 2);
            expectEquals (knobBefore, 0);
            expectEquals (knobAfter, 2);
            expectEquals (yours, AmpSimProcessor::yourCaptureAmp);
            expectEquals (p.getChain().amp.getSelectedAmp(), AmpSimProcessor::yourCaptureAmp);
            logMessage ("  -> program change 3: Monolith selected in the same audio block; the knob read amp "
                        + juce::String (knobBefore + 1) + " until the timer ran, then " + juce::String (knobAfter + 1)
                        + "; PC 9 selects your capture; PC 10 ignored");
        }

        beginTest ("amps, captures, and each amp's knobs survive save and restore, and the saved amp starts without a switch");
        {
            AmpSimProcessor original;
            original.loadModel (1, small);
            original.loadModel (AmpSimProcessor::yourCaptureAmp, lstm);
            setParam (original, AmpSimProcessor::ampModelParamId, (float) AmpSimProcessor::yourCaptureAmp);
            setParam (original, AmpSimProcessor::ampParamId (AmpSimProcessor::yourCaptureAmp, "mid"), 4.0f);
            setParam (original, AmpSimProcessor::ampParamId (1, "mid"), -3.0f);
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
            const auto raw = [&restored] (const juce::String& id) { return restored.parameters.getRawParameterValue (id)->load(); };
            expectEquals (restored.getChain().amp.getSelectedAmp(), AmpSimProcessor::yourCaptureAmp);
            expect (! restored.getChain().amp.isSwitching(), "the restored amp should start without a switch");
            expectEquals (raw (AmpSimProcessor::ampParamId (AmpSimProcessor::yourCaptureAmp, "mid")), 4.0f);
            expectEquals (raw (AmpSimProcessor::ampParamId (1, "mid")), -3.0f);
            expect (status.model[1].contains ("wavenet") && status.model[(size_t) AmpSimProcessor::yourCaptureAmp].contains ("lstm"));
            expectEquals (status.model[0], juce::String ("Empty"));
            logMessage ("  -> restored: your capture active with no switch, \"" + status.model[8] + "\", Middle +4.0 dB; Ember \"" + status.model[1]
                        + "\" with its own Middle -3.0 dB; Glass " + status.model[0]);
        }

        beginTest ("a milestone 1 state (one model, no amp choice) loads its model as your capture, playing");
        {
            AmpSimProcessor fresh;
            auto tree = fresh.parameters.copyState();
            tree.removeChild (tree.getChildWithProperty ("id", AmpSimProcessor::ampModelParamId), nullptr); // as old states have it
            tree.setProperty (AmpSimProcessor::legacyModelPathKey, a1.getFullPathName(), nullptr);
            juce::MemoryBlock state;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), state);

            AmpSimProcessor p;
            p.setStateInformation (state.getData(), (int) state.getSize());
            waitForLoads (p);

            const auto yours = (size_t) AmpSimProcessor::yourCaptureAmp;
            expect (p.getStatus().model[yours].contains ("wavenet_a1_standard"));
            expectEquals (p.getSelectedAmp(), AmpSimProcessor::yourCaptureAmp);
            expect (! p.parameters.state.hasProperty (AmpSimProcessor::modelPathKey (0)));
            expect (! p.parameters.state.hasProperty (AmpSimProcessor::legacyModelPathKey));
            logMessage ("  -> old \"modelPath\" was slot 1; its capture isn't a built-in, so it became your capture (amp9ModelPath), selected: \""
                        + p.getStatus().model[yours] + "\"");
        }
    }
};

AmpSectionTests ampSectionTests;
} // namespace

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// The engine plumbing the UI handoff needed (BUILD_PLAN decision log, 2026-10-03): the amp's bypass, the
// two effect sections' switches, the strip's gate light, and "Follow amp choice" (each amp slot's cab).

#include "AllocationTracking.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getParam (AmpSimProcessor& p, const juce::String& id)
{
    return p.parameters.getRawParameterValue (id)->load();
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

/// The processor's left output for a mono input, a block at a time; `each` runs before every block.
std::vector<float> run (AmpSimProcessor& p, const std::vector<float>& input, std::function<void (size_t start)> each = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> out (input.size(), 0.0f);
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        if (each)
            each (start);
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        p.processBlock (buffer, midi);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize, out.begin() + (long) start);
    }
    return out;
}

juce::File writeIR (const juce::String& name, double presenceDb)
{
    const auto file = tempDir().getChildFile (name + ".wav");
    writeWav (file, toBuffer (syntheticCabIR (2048, presenceDb, 5000.0)));
    return file;
}

class HandoffEngineTests final : public juce::UnitTest
{
public:
    HandoffEngineTests() : juce::UnitTest ("UI handoff engine", "ampsim") {}

    void runTest() override
    {
        const auto a1 = exampleModel ("wavenet_a1_standard.nam");
        const auto toggleAt = (size_t) (0.5 * fs) / blockSize * blockSize; // on a block boundary
        const auto backAt = (size_t) (1.0 * fs) / blockSize * blockSize;
        const auto fade = (size_t) (ampsim::Chain::bypassFadeSeconds * fs);

        beginTest ("amp bypass: crossfades the amp out to its own input over 10 ms with no click, and back to exactly what never-bypassed captures play");
        {
            AmpSimProcessor bypassed, reference;
            for (auto* p : { &bypassed, &reference })
            {
                p->loadModel (0, a1);
                waitForLoads (*p);
                p->prepareToPlay (fs, blockSize);
            }
            const auto input = guitarDI ((int) (1.5 * fs));
            const auto wet = run (reference, input);
            const auto out = run (bypassed, input, [&] (size_t start)
            {
                if (start == toggleAt)
                    setParam (bypassed, "amp_bypass", 1.0f);
                if (start == backAt)
                    setParam (bypassed, "amp_bypass", 0.0f);
            });

            // Bypassed (after the fade): the guitar as it went in (no cab IR, every other block at its default).
            const auto dryDifference = maxAbsDifference (std::vector<float> (out.begin() + (long) (toggleAt + fade + blockSize), out.begin() + (long) backAt),
                                                         std::vector<float> (input.begin() + (long) (toggleAt + fade + blockSize), input.begin() + (long) backAt));
            // Back on (after the fade): sample for sample what captures that were never bypassed play, because
            // they kept running on the amp's input meanwhile.
            const auto wetDifference = maxAbsDifference (std::vector<float> (out.begin() + (long) (backAt + fade + blockSize), out.end()),
                                                         std::vector<float> (wet.begin() + (long) (backAt + fade + blockSize), wet.end()));
            // No click: the largest step during each fade stays within the larger of the two signals' own steps.
            const auto window = [] (size_t at) { return std::pair<size_t, size_t> { at - 4800, at + 4800 }; };
            double fadeStep = 0.0, signalStep = 0.0;
            for (const auto at : { toggleAt, backAt })
            {
                const auto [from, to] = window (at);
                fadeStep = std::max (fadeStep, maxStep (out, at, at + fade + blockSize));
                signalStep = std::max ({ signalStep, maxStep (input, from, to), maxStep (wet, from, to) });
            }
            expectLessThan (dryDifference, 1.0e-6);
            expectLessThan (wetDifference, 1.0e-6);
            expectLessThan (fadeStep, signalStep * 1.05);
            expect (bypassed.getChain().isFullyBypassed (ampsim::Chain::Slot::amp) == false);
            logMessage ("  -> bypassed: output equals the input within " + juce::String (dryDifference, 9) + "; back on: equals never-bypassed captures within "
                        + juce::String (wetDifference, 9) + " (they kept running); largest step during the fades " + juce::String (fadeStep, 4)
                        + " vs " + juce::String (signalStep, 4) + " in the signals themselves");
        }

        beginTest ("section switches: pre_fx_on and post_fx_on bypass their blocks through the blocks' own crossfades and never touch the blocks' switches");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "boost_on", 1.0f);
            setParam (p, "boost_level", 6.0f);
            setParam (p, "chorus_on", 1.0f);
            setParam (p, "comp_post_on", 1.0f);
            const auto input = guitarDI ((int) (1.5 * fs));
            using Slot = ampsim::Chain::Slot;
            bool preOffFully = false, postOffFully = false;
            const auto out = run (p, input, [&] (size_t start)
            {
                if (start == toggleAt)
                {
                    setParam (p, "pre_fx_on", 0.0f);
                    setParam (p, "post_fx_on", 0.0f);
                }
                if (start == backAt - blockSize)
                {
                    preOffFully = p.getChain().isFullyBypassed (Slot::boost) && p.getChain().isFullyBypassed (Slot::preEq);
                    postOffFully = p.getChain().isFullyBypassed (Slot::chorus) && p.getChain().isFullyBypassed (Slot::postCompressor);
                }
                if (start == backAt)
                {
                    setParam (p, "pre_fx_on", 1.0f);
                    setParam (p, "post_fx_on", 1.0f);
                }
            });

            const auto offDifference = maxAbsDifference (std::vector<float> (out.begin() + (long) (toggleAt + fade + 2 * blockSize), out.begin() + (long) backAt),
                                                         std::vector<float> (input.begin() + (long) (toggleAt + fade + 2 * blockSize), input.begin() + (long) backAt));
            const auto ownSwitches = getParam (p, "boost_on") + getParam (p, "chorus_on") + getParam (p, "comp_post_on") + getParam (p, "eq_pre_on")
                                     + getParam (p, "eq_post_on");
            const auto backOn = ! p.getChain().isFullyBypassed (Slot::boost) && ! p.getChain().isFullyBypassed (Slot::chorus);
            const auto fadeStep = std::max (maxStep (out, toggleAt, toggleAt + fade + blockSize), maxStep (out, backAt, backAt + fade + blockSize));
            const auto playingStep = std::max (maxStep (out, toggleAt - 9600, toggleAt), maxStep (input, toggleAt - 9600, backAt + 9600));
            expect (preOffFully && postOffFully);
            expectLessThan (offDifference, 1.0e-6);
            expectEquals (ownSwitches, 5.0f);
            expect (backOn);
            expectLessThan (fadeStep, playingStep * 1.05);
            logMessage ("  -> both sections off: every block in them fully bypassed, the output equals the input within " + juce::String (offDifference, 9)
                        + "; the blocks' own switches untouched (5 of 5 still on), and back on they run again; largest step during the fades "
                        + juce::String (fadeStep, 4) + " vs " + juce::String (playingStep, 4) + " while playing");
        }

        beginTest ("gate light: Gate A's gain above -6 dB is open; off (or its section off) it's always open");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            setParam (p, "gate_a_on", 1.0f);
            setParam (p, "gate_a_threshold", -50.0f);
            run (p, std::vector<float> ((size_t) (0.5 * fs), 0.0f));
            const auto closedOnSilence = ! p.isGateOpen();
            run (p, sine (220.0, 0.3, (int) (0.1 * fs)));
            const auto openOnNote = p.isGateOpen();
            setParam (p, "gate_a_on", 0.0f);
            run (p, std::vector<float> ((size_t) (0.5 * fs), 0.0f));
            const auto openWhileOff = p.isGateOpen();
            setParam (p, "gate_a_on", 1.0f);
            setParam (p, "pre_fx_on", 0.0f);
            run (p, std::vector<float> ((size_t) (0.5 * fs), 0.0f));
            const auto openWhileSectionOff = p.isGateOpen();
            expect (closedOnSilence && openOnNote && openWhileOff && openWhileSectionOff);
            logMessage ("  -> silence: " + juce::String (closedOnSilence ? "dark" : "lit") + ", a -10 dBFS note: " + (openOnNote ? "lit" : "dark")
                        + ", gate off: " + (openWhileOff ? "lit" : "dark") + ", Pre FX off: " + (openWhileSectionOff ? "lit" : "dark"));
        }

        beginTest ("follow amp choice: switching slots loads the slot's cab into close mic 1; off, it doesn't; picking a cab assigns it; saved in the state");
        {
            const auto irA = writeIR ("follow_a", 2.0), irB = writeIR ("follow_b", 6.0), irC = writeIR ("follow_c", -2.0);
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            const auto mic1 = [&p] { return p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString(); };
            expect (p.isCabFollowing()); // on by default

            p.pickCab (irA); // slot 1 playing
            waitForLoads (p);
            expect (p.getCabAssignment (0) == irA && mic1() == irA.getFullPathName());
            setParam (p, AmpSimProcessor::ampModelParamId, 1.0f);
            p.runHousekeeping();
            const auto unassignedKeeps = mic1() == irA.getFullPathName(); // slot 2 has no cab: nothing changes
            p.pickCab (irB);
            setParam (p, AmpSimProcessor::ampModelParamId, 0.0f);
            p.runHousekeeping();
            const auto followedToA = mic1() == irA.getFullPathName();

            // From the footswitch: program change 2 picks slot 2 on the audio thread; the timer follows.
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::programChange (1, 1), 0);
            buffer.clear();
            p.processBlock (buffer, midi);
            p.runHousekeeping(); // the slot parameter catches up
            p.runHousekeeping(); // and following sees it
            const auto followedFootswitch = mic1() == irB.getFullPathName();

            p.setCabFollow (false);
            setParam (p, AmpSimProcessor::ampModelParamId, 0.0f);
            p.runHousekeeping();
            const auto notFollowing = mic1() == irB.getFullPathName();
            p.setCabFollow (true); // follows at once: slot 1's cab
            const auto followsAtOnce = mic1() == irA.getFullPathName();
            p.pickCab (irC);
            waitForLoads (p);
            const auto reassigned = p.getCabAssignment (0) == irC;

            juce::MemoryBlock state;
            p.setCabFollow (false);
            p.getStateInformation (state);
            AmpSimProcessor restored;
            restored.setStateInformation (state.getData(), (int) state.getSize());
            waitForLoads (restored);
            const auto survived = restored.getCabAssignment (0) == irC && restored.getCabAssignment (1) == irB && restored.getCabAssignment (2) == juce::File()
                                  && ! restored.isCabFollowing();

            expect (unassignedKeeps && followedToA && followedFootswitch && notFollowing && followsAtOnce && reassigned && survived);
            logMessage ("  -> slot 1 picked A, slot 2 (unassigned) kept A, picked B; back to slot 1: A; program change 2: B; with Follow off a switch "
                        "kept B; Follow back on loaded slot 1's A at once; picking C reassigned slot 1; assignments and Follow survive saving the state");
        }

        beginTest ("presets: amp_bypass, pre_fx_on, post_fx_on, and the cab assignments round-trip; older presets get the defaults");
        {
            const auto irA = writeIR ("assign_a", 3.0), irB = writeIR ("assign_b", 5.0);
            AmpSimProcessor p;
            setParam (p, "amp_bypass", 1.0f);
            setParam (p, "pre_fx_on", 0.0f);
            setParam (p, "post_fx_on", 0.0f);
            p.setCabAssignment (0, irA);
            p.setCabAssignment (2, irB);
            p.setCabFollow (false);
            const auto preset = p.capturePreset ("Assigned");
            expect (preset["cab_assign"].isArray() && preset["cab_assign"].size() == 3);

            AmpSimProcessor q;
            expect (q.loadPreset (juce::JSON::parse (juce::JSON::toString (preset))).ok);
            waitForLoads (q);
            const auto values = getParam (q, "amp_bypass") == 1.0f && getParam (q, "pre_fx_on") == 0.0f && getParam (q, "post_fx_on") == 0.0f;
            const auto assignments = q.getCabAssignment (0) == irA && q.getCabAssignment (1) == juce::File() && q.getCabAssignment (2) == irB && ! q.isCabFollowing();
            expect (values && assignments);

            // An older preset (no new keys): the amp on, both sections on, nothing assigned, following.
            expect (q.loadPreset (juce::JSON::parse (R"({ "format_version": 2, "parameters": {} })")).ok);
            waitForLoads (q);
            const auto defaults = getParam (q, "amp_bypass") == 0.0f && getParam (q, "pre_fx_on") == 1.0f && getParam (q, "post_fx_on") == 1.0f
                                  && q.getCabAssignment (0) == juce::File() && q.getCabAssignment (2) == juce::File() && q.isCabFollowing();
            expect (defaults);

            // A missing assigned cab is reported and left empty; the rest of the preset still loads.
            auto missing = juce::JSON::parse (juce::JSON::toString (preset));
            irB.moveFileTo (irB.withFileExtension ("moved"));
            expect (q.loadPreset (missing).ok);
            waitForLoads (q);
            const auto warned = q.getPresetWarnings().joinIntoString ("; ").contains ("Amp 3's cab is missing");
            expect (warned && q.getCabAssignment (2) == juce::File() && q.getCabAssignment (0) == irA);
            logMessage ("  -> round trip: amp bypassed, both sections off, slots 1 and 3 assigned, Follow off, all restored; an older preset: amp on, sections "
                        "on, nothing assigned, following; a missing assigned cab: \"" + q.getPresetWarnings().joinIntoString ("; ") + "\"");
        }
    }
};

HandoffEngineTests handoffEngineTests;
} // namespace

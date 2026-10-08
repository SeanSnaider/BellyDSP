// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "CpuProfile.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>

namespace
{
using namespace testing;
using Slot = ampsim::Chain::Slot;

/// The processor on `input` in 128-sample buffers; beforeBlock runs first (the message thread's side). The stereo output.
juce::AudioBuffer<float> render (AmpSimProcessor& p, const std::vector<float>& input, const std::function<void (int block)>& beforeBlock = {},
                                 rtcheck::Counts* counts = nullptr)
{
    const auto numBlocks = (int) input.size() / blockSize;
    juce::AudioBuffer<float> out (2, numBlocks * blockSize), buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int b = 0; b < numBlocks; ++b)
    {
        if (beforeBlock)
            beforeBlock (b);
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + b * blockSize, blockSize);
        if (counts != nullptr)
            rtcheck::begin();
        p.processBlock (buffer, midi);
        if (counts != nullptr)
            *counts += rtcheck::end();
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, b * blockSize, buffer, ch, 0, blockSize);
    }
    return out;
}

/// The largest sample-to-sample step of the left channel in [start, end).
double largestStep (const juce::AudioBuffer<float>& y, int start, int end)
{
    const auto* x = y.getReadPointer (0);
    double step = 0.0;
    for (int n = juce::jmax (1, start); n < juce::jmin (end, y.getNumSamples()); ++n)
        step = std::max (step, (double) std::abs (x[n] - x[n - 1]));
    return step;
}
} // namespace

class CpuSaverTests final : public juce::UnitTest
{
public:
    CpuSaverTests() : juce::UnitTest ("CPU saver", "ampsim") {}

    void runTest() override
    {
        beginTest ("off by default, a global setting: never in presets or scenes");
        {
            auto p = cpu::makeRig();
            expect (p->parameters.getRawParameterValue ("cpu_saver")->load() < 0.5f);
            expect (presets::isGlobal ("cpu_saver"));
        }

        beginTest ("the CPU with the saver on: the defaults, each factory preset, the heaviest rig (flat out, and paced like a device on a real-time thread)");
        cpuWithSaver();

        beginTest ("a drive switched on while the saver is on: it isn't running while off, warms up unheard for 200 ms (the output is the dry's, bit "
                   "for bit), then fades in without a click");
        driveSwitchOn();

        beginTest ("the saver toggled while playing (a drive on, a drive off, the Gain between steps): no click, and no more than two models");
        saverToggled();

        beginTest ("the Gain crossing a step's midpoint with the saver on: one model, the crossing at the slew, no click");
        gainCrossing();
    }

    void cpuWithSaver()
    {
        const auto input = guitarDI ((int) (10.0 * fs));
        struct Row
        {
            juce::String name;
            cpu::RigRun off, on, onPaced;
        };
        std::vector<Row> rows;
        const auto measure = [&] (AmpSimProcessor& p, const juce::String& name)
        {
            Row row;
            row.name = name;
            cpu::setParam (p, "cpu_saver", 0.0f);
            cpu::settleLoads (p);
            row.off = cpu::profile (p, input, name + ", saver off");
            cpu::setParam (p, "cpu_saver", 1.0f);
            cpu::settleLoads (p);
            row.on = cpu::profile (p, input, name + ", saver on");
            row.onPaced = cpu::profile (p, input, name + ", saver on, paced", {}, cpu::Pacing::device);
            for (const auto* run : { &row.off, &row.on, &row.onPaced })
            {
                expectEquals (run->counts.allocations, 0L);
                expectEquals (run->counts.frees, 0L);
                expectEquals (run->counts.blockingLocks, 0L);
            }
            expectLessOrEqual (row.on.maxModels, 1, name + ": one model with the saver");
            if (p.getChain().isFullyBypassed (Slot::overdrive))
                expect (! p.getChain().didRun (Slot::overdrive), name + ": an off overdrive stopped");
            rows.push_back (row);
        };
        {
            auto p = cpu::makeRig();
            cpu::setDefaults (*p);
            measure (*p, "Defaults");
            for (const auto& preset : presets::factoryPresets())
            {
                cpu::setParam (*p, "cpu_saver", 0.0f);
                cpu::setFactoryPreset (*p, preset);
                measure (*p, preset["name"].toString());
            }
        }
        {
            auto p = cpu::makeRig();
            cpu::setHeaviest (*p);
            measure (*p, "Heaviest");
        }

        for (const auto& row : rows)
        {
            expectLessThan (row.on.total.mean, row.off.total.mean, row.name);
            logMessage ("  -> " + row.name + ": saver off " + juce::String (row.off.meanPercent(), 1) + " / " + juce::String (row.off.p99Percent(), 1)
                        + "%, on " + juce::String (row.on.meanPercent(), 1) + " / " + juce::String (row.on.p99Percent(), 1) + "% (mean / p99, flat out; "
                        + juce::String (juce::roundToInt (100.0 * (1.0 - row.on.total.mean / row.off.total.mean))) + "% less), paced like a device "
                        + juce::String (row.onPaced.meanPercent(), 1) + " / " + juce::String (row.onPaced.p99Percent(), 1) + "%");
        }
        for (const auto& line : rows.back().on.table())
            logMessage ("  -> " + line);
    }

    void driveSwitchOn()
    {
        const auto input = guitarDI ((int) (4.0 * fs));
        // Two identical processors: one switches the overdrive on at 1 s, the other never does.
        auto a = cpu::makeRig(), b = cpu::makeRig();
        for (auto* p : { a.get(), b.get() })
        {
            cpu::setDefaults (*p);
            cpu::setParam (*p, "cpu_saver", 1.0f);
            cpu::settleLoads (*p);
            p->prepareToPlay (fs, blockSize);
        }
        const int switchBlock = (int) (1.0 * fs) / blockSize;
        bool ranWhileOff = false, warmedAtSwitch = false;
        int warmingBlocks = 0, fadeStartBlock = -1;
        rtcheck::Counts counts;
        const auto withSwitch = render (*a, input, [&] (int block)
        {
            if (block < switchBlock && block > 4)
                ranWhileOff = ranWhileOff || a->getChain().didRun (Slot::overdrive);
            if (block == switchBlock)
                cpu::setParam (*a, "od_on", 1.0f);
            if (block == switchBlock + 1)
                warmedAtSwitch = a->getChain().isWarmingUp (Slot::overdrive);
            if (block > switchBlock && a->getChain().isWarmingUp (Slot::overdrive))
                ++warmingBlocks;
            // Checked before this block runs: the warm-up ended (and the fade began) in the block before.
            if (fadeStartBlock < 0 && block > switchBlock + 1 && ! a->getChain().isWarmingUp (Slot::overdrive))
                fadeStartBlock = block - 1;
        }, &counts);
        const auto without = render (*b, input);

        expect (! ranWhileOff, "the off overdrive doesn't run with the saver on");
        expect (warmedAtSwitch, "switched on, it warms up first");
        const auto warmupMs = 1000.0 * (fadeStartBlock - switchBlock) * blockSize / fs;
        expectWithinAbsoluteError (warmupMs, 1000.0 * AmpSimProcessor::cpuSaverWarmupSeconds, 3.0);

        // During the warm-up the output is the drive-off output, sample for sample.
        const auto fadeStart = fadeStartBlock * blockSize;
        double during = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < fadeStart; ++n)
            {
                during = std::max (during, (double) std::abs (withSwitch.getSample (ch, n) - without.getSample (ch, n)));
            }
        expectEquals (during, 0.0, "the dry, bit for bit, until the fade starts");

        // The fade in: no step larger than the drive's own playing produces once it's on.
        const auto fadeStep = largestStep (withSwitch, fadeStart, fadeStart + (int) (0.05 * fs));
        const auto steady = largestStep (withSwitch, fadeStart + (int) (0.3 * fs), withSwitch.getNumSamples());
        expectLessThan (fadeStep, 1.25 * steady);
        expectEquals (counts.allocations, 0L);
        expectEquals (counts.frees, 0L);
        expectEquals (counts.blockingLocks, 0L);
        logMessage ("  -> the overdrive off with the saver on: not running; switched on, it warmed up " + juce::String (warmupMs, 1)
                    + " ms unheard (the output identical to the drive left off: max difference " + juce::String (during) + "), then faded in: largest step "
                    + juce::String (fadeStep / steady, 2) + "x the largest once it's on; 0 allocations, frees, locks");
    }

    void saverToggled()
    {
        const auto input = guitarDI ((int) (8.0 * fs));
        // Tech Death: the Screamer boost on, the overdrive off; its amp's Gain between two steps.
        auto p = cpu::makeRig();
        bool found = false;
        for (const auto& f : presets::factoryPresets())
            if (f["name"].toString() == "Tech Death")
                found = cpu::setFactoryPreset (*p, f);
        expect (found);
        const auto amp = p->getChain().amp.getSelectedAmp();
        cpu::setParam (*p, AmpSimProcessor::ampParamId (amp, "input_trim"), ampsim::AmpSection::GainKnob::dbForPosition (6.0f));
        cpu::settleLoads (*p);
        p->prepareToPlay (fs, blockSize);

        // The same playing three ways: the saver toggled every second, always off, always on.
        const int every = (int) fs / blockSize;
        int maxModels = 0;
        rtcheck::Counts counts;
        const auto toggled = render (*p, input, [&] (int block)
        {
            if (block > 0 && block % every == 0)
                cpu::setParam (*p, "cpu_saver", (block / every) % 2 == 1 ? 1.0f : 0.0f);
            maxModels = std::max (maxModels, p->getChain().amp.getRunningModels());
        }, &counts);
        cpu::setParam (*p, "cpu_saver", 0.0f);
        cpu::settleLoads (*p);
        p->prepareToPlay (fs, blockSize);
        const auto off = render (*p, input);
        cpu::setParam (*p, "cpu_saver", 1.0f);
        cpu::settleLoads (*p);
        p->prepareToPlay (fs, blockSize);
        const auto on = render (*p, input);

        double worst = 0.0;
        for (int t = 1; t < 8; ++t)
        {
            const auto start = t * every * blockSize, end = start + (int) (0.3 * fs);
            const auto reference = std::max (largestStep (off, start, end), largestStep (on, start, end));
            worst = std::max (worst, largestStep (toggled, start, end) / reference);
        }
        expectLessThan (worst, 1.25);
        expectLessOrEqual (maxModels, 2);
        expectEquals (counts.allocations, 0L);
        expectEquals (counts.frees, 0L);
        expectEquals (counts.blockingLocks, 0L);
        logMessage ("  -> Tech Death (the Screamer on, the overdrive off), the Gain at 6, the saver toggled every second for 8 s: the largest step in "
                    "the 300 ms after each toggle " + juce::String (worst, 2) + "x the largest of the same 300 ms with the saver left off or left on; at most "
                    + juce::String (maxModels) + " models; 0 allocations, frees, locks");
    }

    void gainCrossing()
    {
        const auto input = guitarDI ((int) (6.0 * fs));
        auto p = cpu::makeRig();
        cpu::setDefaults (*p);
        cpu::setParam (*p, "cpu_saver", 1.0f);
        const auto amp = p->getChain().amp.getSelectedAmp();
        const auto gain = AmpSimProcessor::ampParamId (amp, "input_trim");
        cpu::setParam (*p, gain, ampsim::AmpSection::GainKnob::dbForPosition (5.5f));
        cpu::settleLoads (*p);
        p->prepareToPlay (fs, blockSize);

        // The knob from 5.5 to 7.0 and back over 3 s, across the midpoint between the steps at 5 and 7.5 (6.25).
        int maxModels = 0, crossings = 0;
        int lastNearest = -1;
        float minPosition = 10.0f, maxPosition = 0.0f;
        const auto rendered = render (*p, input, [&] (int block)
        {
            const auto t = (double) block * blockSize / fs;
            const auto phase = std::fmod (t, 3.0) / 3.0;
            const auto position = (float) (5.5 + 1.5 * (phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase));
            cpu::setParam (*p, gain, ampsim::AmpSection::GainKnob::dbForPosition (position));
            maxModels = std::max (maxModels, p->getChain().amp.getRunningModels());
            const auto pos = p->getChain().amp.amp (amp).model.getGainPosition();
            minPosition = std::min (minPosition, pos);
            maxPosition = std::max (maxPosition, pos);
            const auto nearest = pos < 6.25f ? 0 : 1;
            if (lastNearest >= 0 && nearest != lastNearest)
                ++crossings;
            lastNearest = nearest;
        });
        // A steady reference: the Gain held at each step.
        cpu::setParam (*p, gain, ampsim::AmpSection::GainKnob::dbForPosition (5.0f));
        cpu::settleLoads (*p);
        const auto atFive = render (*p, input);
        cpu::setParam (*p, gain, ampsim::AmpSection::GainKnob::dbForPosition (7.5f));
        cpu::settleLoads (*p);
        const auto atSevenFive = render (*p, input);
        const auto ratio = largestStep (rendered, 0, rendered.getNumSamples())
                           / std::max (largestStep (atFive, 0, atFive.getNumSamples()), largestStep (atSevenFive, 0, atSevenFive.getNumSamples()));
        expectLessThan (ratio, 1.25);
        expectLessOrEqual (maxModels, 2);
        expectGreaterOrEqual (crossings, 2);
        logMessage ("  -> the Gain swept 5.5 -> 7 -> 5.5 twice with the saver on: the amp's position stayed on the steps or crossing between them ("
                    + juce::String (minPosition, 2) + " to " + juce::String (maxPosition, 2) + ", " + juce::String (crossings) + " crossings), at most "
                    + juce::String (maxModels) + " models (two only while crossing); the largest step " + juce::String (ratio, 2)
                    + "x the largest with the Gain held on either step");
    }
};

static CpuSaverTests cpuSaverTests;

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "BuiltInCaptures.h"
#include "CpuProfile.h"
#include "PluginProcessor.h"
#include "Presets.h"
#include "TestHelpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>

namespace
{
using namespace testing;

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 12000 && p.isLoading(); ++i) // the eight built-in sets take seconds on a busy machine
        juce::Thread::sleep (5);
}

struct Timing
{
    double mean = 0.0, p99 = 0.0, worst = 0.0;
    int over = 0, blocks = 0;
    rtcheck::Counts counts;

    juce::String describe() const
    {
        return juce::String (100.0 * mean / deadlineMicros, 1) + "% of the deadline on average (" + juce::String (juce::roundToInt (mean)) + " us), p99 "
               + juce::String (100.0 * p99 / deadlineMicros, 1) + "%, worst " + juce::String (100.0 * worst / deadlineMicros, 1) + "%, "
               + juce::String (over) + " of " + juce::String (blocks) + " buffers over";
    }
};

/// The processor on `input` in 128-sample buffers, timing each callback and counting the audio thread's
/// allocations, frees, and locks.
Timing play (AmpSimProcessor& p, const std::vector<float>& input, const std::function<void (size_t block)>& beforeBlock = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<double> micros;
    micros.reserve (input.size() / blockSize + 1);
    Timing t;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
        if (beforeBlock)
            beforeBlock (start / blockSize); // the message thread's side, outside the timing
        buffer.clear();
        buffer.copyFrom (0, 0, input.data() + start, blockSize);
        rtcheck::begin();
        const auto t0 = std::chrono::steady_clock::now();
        p.processBlock (buffer, midi);
        const auto elapsed = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
        const auto c = rtcheck::end();
        t.counts.allocations += c.allocations;
        t.counts.frees += c.frees;
        t.counts.blockingLocks += c.blockingLocks;
        micros.push_back (elapsed);
    }
    std::sort (micros.begin(), micros.end());
    t.blocks = (int) micros.size();
    t.mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
    t.p99 = micros[(size_t) (0.99 * (double) (micros.size() - 1))];
    t.worst = micros.back();
    t.over = (int) std::count_if (micros.begin(), micros.end(), [] (double us) { return us > deadlineMicros; });
    return t;
}
} // namespace

class FullRigTests final : public juce::UnitTest
{
public:
    FullRigTests() : juce::UnitTest ("Full rig", "ampsim") {}

    void runTest() override
    {
        beginTest ("the whole rig at once: three captures loaded (the selected one runs), three mics, every block on, 128-sample buffers: CPU against the deadline, and nothing allocated on the audio thread");
        rig (false);

        beginTest ("the same on the eight built-in amps (standard WaveNet gain sets, all loaded, one playing), as the app starts");
        rig (true);

        beginTest ("the CPU profile, block by block: the defaults, the factory presets, the heaviest rig; the budget for weaker machines (BUILD_PLAN \"CPU\")");
        profileAndBudget();
    }

    /// Every block's cost (mean, p99, worst) on the defaults, each factory preset, and the heaviest rig, and the budget
    /// they're held to (BUILD_PLAN "CPU").
    void profileAndBudget()
    {
        const auto input = guitarDI ((int) (10.0 * fs));
        // The budget runs flat out on a user-interactive thread (cpu::Pacing: the work at full clock, what a slower machine
        // scales); AMPSIM_CPU_PACING=device feeds them like a device instead. Device-paced runs of the defaults and the
        // heaviest rig are printed below for comparison, not asserted.
        const auto pacing = juce::SystemStats::getEnvironmentVariable ("AMPSIM_CPU_PACING", {}) == "device" ? cpu::Pacing::device : cpu::Pacing::flatOut;
        std::vector<cpu::RigRun> runs;
        const auto show = [&] (const cpu::RigRun& run)
        {
            for (const auto& line : run.table())
                logMessage ("  -> " + line);
            expectEquals (run.counts.allocations, 0L);
            expectEquals (run.counts.frees, 0L);
            expectEquals (run.counts.blockingLocks, 0L);
            runs.push_back (run);
        };

        cpu::RigRun defaults, typical, heaviest;
        {
            auto p = cpu::makeRig();
            cpu::setDefaults (*p);
            defaults = cpu::profile (*p, input, "Defaults (Glass, one close mic on a 1 s IR, everything else off)", {}, pacing);
            show (defaults);
            const auto paced = cpu::profile (*p, input, "Defaults, paced like a device", {}, cpu::Pacing::device);
            logMessage ("  -> " + paced.table()[0]);
            for (const auto& preset : presets::factoryPresets())
            {
                expect (cpu::setFactoryPreset (*p, preset));
                const auto run = cpu::profile (*p, input, "Factory preset " + preset["name"].toString(), {}, pacing);
                show (run);
                if (run.total.mean > typical.total.mean)
                    typical = run;
            }
        }
        {
            auto p = cpu::makeRig();
            cpu::setHeaviest (*p);
            heaviest = cpu::profile (*p, input, "Heaviest (every block on at its heaviest, three mics on 1 s IRs, the Gain between two steps)", {}, pacing);
            show (heaviest);
            const auto paced = cpu::profile (*p, input, "Heaviest, paced like a device", {}, cpu::Pacing::device);
            logMessage ("  -> " + paced.table()[0]);
            expectEquals (heaviest.maxModels, 2);
        }
        logMessage ("  -> the heaviest factory preset (typical): " + typical.name + ", " + typical.summary());

        // The budget for weaker machines (cpu::Budget: BUILD_PLAN "CPU").
        const auto check = [this] (const cpu::RigRun& run, const cpu::Budget& budget, const juce::String& what)
        {
            expectLessThan (run.meanPercent(), budget.meanPercent * cpuBudgetScale(), what + ": the mean");
            expectLessThan (run.p99Percent(), budget.p99Percent * cpuBudgetScale(), what + ": the p99");
            logMessage ("  -> budget, " + what + ": mean " + juce::String (run.meanPercent(), 1) + "% (limit " + juce::String (budget.meanPercent * cpuBudgetScale(), 1)
                        + "%), p99 " + juce::String (run.p99Percent(), 1) + "% (limit " + juce::String (budget.p99Percent * cpuBudgetScale(), 1) + "%)");
        };
        check (defaults, cpu::defaultsBudget, "defaults");
        for (const auto& run : runs)
            if (run.name.startsWith ("Factory preset"))
                check (run, cpu::typicalBudget, run.name.fromFirstOccurrenceOf ("Factory preset ", false, false) + " (typical)");
        check (heaviest, cpu::heaviestBudget, "heaviest");

        // What the pitch blocks cost when they're on (BUILD_PLAN "CPU", the options), on top of the defaults.
        {
            auto p = cpu::makeRig();
            cpu::setDefaults (*p);
            const auto base = cpu::profile (*p, input, "defaults");
            juce::StringArray lines;
            const auto add = [&] (const juce::String& name, std::initializer_list<std::pair<const char*, float>> params, ampsim::Chain::Slot slot)
            {
                for (const auto& [id, value] : params)
                    cpu::setParam (*p, id, value);
                cpu::settleLoads (*p);
                const auto run = cpu::profile (*p, input, name);
                const auto& b = run.blocks[(size_t) slot];
                expectEquals (run.counts.allocations, 0L);
                lines.add (name + ": the block " + juce::String (b.mean, 1) + " us mean (" + juce::String (100.0 * b.mean / deadlineMicros, 2) + "%), p99 "
                           + juce::String (b.p99, 1) + " us, worst " + juce::String (b.worst, 1) + " us; the whole callback " + juce::String (run.meanPercent(), 1)
                           + "% (+" + juce::String (run.meanPercent() - base.meanPercent(), 1) + "), p99 " + juce::String (run.p99Percent(), 1) + "%");
            };
            using S = ampsim::Chain::Slot;
            add ("harmonizer, 1 voice", { { "harm_on", 1 } }, S::harmonizer);
            add ("harmonizer, 2 voices", { { "harm_v2_on", 1 } }, S::harmonizer);
            add ("harmonizer, 4 voices", { { "harm_v3_on", 1 }, { "harm_v4_on", 1 } }, S::harmonizer);
            cpu::setParam (*p, "harm_on", 0);
            add ("multivoicer Poly, its default 4 voices", { { "mv_on", 1 } }, S::multivoicer);
            add ("multivoicer Poly, 8 voices", { { "mv_voices", 8 } }, S::multivoicer);
            add ("multivoicer Mono, 8 voices", { { "mv_engine", 1 } }, S::multivoicer);
            add ("multivoicer Mono, 4 voices", { { "mv_voices", 4 } }, S::multivoicer);
            for (const auto& line : lines)
                logMessage ("  -> on the defaults, " + line);
        }
    }

    /// Defaults, everything on, and the heaviest settings, timed. builtIns: the eight built-in amps the app starts
    /// with; otherwise NAM's example models (A1 standard, A2, LSTM) in amps 1 to 3. Only the selected amp runs
    /// (BUILD_PLAN "Amp switching").
    void rig (bool builtIns)
    {
        const auto irA = tempDir().getChildFile ("rig_ir_a.wav"), irB = tempDir().getChildFile ("rig_ir_b.wav"), room = tempDir().getChildFile ("rig_room.wav");
        writeWav (irA, toBuffer (syntheticCabIR (4096)));
        writeWav (irB, toBuffer (syntheticCabIR (2048, 8.0, 7000.0)));
        writeWav (room, toBuffer (syntheticCabIR (24000, 0.0, 4000.0)));

        std::unique_ptr<AmpSimProcessor> owner;
        {
            std::unique_ptr<WithBuiltInCaptures> on;
            if (builtIns)
                on = std::make_unique<WithBuiltInCaptures>();
            owner = std::make_unique<AmpSimProcessor>();
        }
        auto& p = *owner;
        if (! builtIns)
        {
            p.loadModel (0, exampleModel ("wavenet_a1_standard.nam"));
            p.loadModel (1, exampleModel ("A2.nam"));
            p.loadModel (2, exampleModel ("lstm.nam"));
        }
        p.loadCabIR (0, irA);
        p.loadCabIR (1, irB);
        p.loadCabIR (AmpSimProcessor::roomMic, room);
        waitForLoads (p);
        p.prepareToPlay (fs, blockSize);
        const auto input = guitarDI ((int) (10.0 * fs));
        if (builtIns)
            for (int s = 0; s < AmpSimProcessor::numBuiltInAmps; ++s)
                expect (p.parameters.state.getProperty (AmpSimProcessor::modelPathKey (s)).toString() == presets::builtInCapture (s).getFullPathName());

        // Defaults: the selected amp running, the cab, the EQs (flat), everything else off.
        const auto bare = play (p, input);

        // Everything on.
        for (const auto& [id, value] : std::initializer_list<std::pair<const char*, float>> {
                 { "gate_a_on", 1 }, { "gate_b_on", 1 }, { "comp_pre_on", 1 }, { "boost_on", 1 }, { "boost_mode", 2 }, { "od_on", 1 }, { "od_mode", 1 },
                 { "comp_post_on", 1 }, { "harm_on", 1 }, { "harm_v2_on", 1 }, { "mv_on", 1 }, { "bloom_on", 1 }, { "bloom_crush_on", 1 },
                 { "bloom_phaser_on", 1 }, { "bloom_flanger_on", 1 }, { "chorus_on", 1 }, { "chorus_mode", 2 }, { "delay_on", 1 }, { "delay_mode", 2 },
                 { "reverb_on", 1 }, { "reverb_engine", 1 }, { "reverb_shimmer", 50 } })
            setParam (p, id, value);
        p.runHousekeeping();
        const auto everything = play (p, input);

        // The heaviest settings: 8x drive, the multivoicer in Mono with 8 voices, all four harmonies, the Fuzz.
        for (const auto& [id, value] : std::initializer_list<std::pair<const char*, float>> {
                 { "drive_oversampling", 1 }, { "od_mode", 3 }, { "mv_engine", 1 }, { "mv_voices", 8 }, { "harm_v3_on", 1 }, { "harm_v4_on", 1 } })
            setParam (p, id, value);
        p.runHousekeeping();
        const auto heaviest = play (p, input);

        // Gain sets (BUILD_PLAN "Amp gain") with one amp at a time (BUILD_PLAN "Amp switching"): the playing amp runs
        // one step model on a step (the three runs above, at the default Gain 5), two between steps, and up to three
        // while its Gain moves (warming ahead). The heaviest settings again with the Gain between two steps, then with
        // it sweeping 0 to 10 and back, then between steps with the amp switching every 0.25 s through all eight
        // (each switch runs the incoming amp's two steps beside the outgoing one's two for the warm-up and the fade).
        Timing between, sweeping, switching;
        int maxBetween = 0, maxSweeping = 0, maxSwitching = 0, maxAmpsSwitching = 0, minBetween = 99;
        const auto models = [&] { return p.getChain().amp.getRunningModels(); };
        if (builtIns)
        {
            using GainKnob = ampsim::AmpSection::GainKnob;
            for (int s = 0; s < AmpSimProcessor::numBuiltInAmps; ++s)
                setParam (p, AmpSimProcessor::ampParamId (s, "input_trim"), GainKnob::dbForPosition (6.25f));
            p.runHousekeeping();
            play (p, std::vector<float> (blockSize * 80, 0.0f)); // reach the position (and its second step's warm-up)
            between = play (p, input, [&] (size_t)
            {
                maxBetween = std::max (maxBetween, models());
                minBetween = std::min (minBetween, models());
            });
            sweeping = play (p, input, [&] (size_t block)
            {
                const auto phase = std::fmod ((double) block * blockSize / fs, 1.6) / 1.6; // 0 -> 10 -> 0 every 1.6 s
                const auto position = (float) (10.0 * (phase < 0.5 ? 2.0 * phase : 2.0 - 2.0 * phase));
                setParam (p, AmpSimProcessor::ampParamId (0, "input_trim"), GainKnob::dbForPosition (position));
                maxSweeping = std::max (maxSweeping, models());
            });
            setParam (p, AmpSimProcessor::ampParamId (0, "input_trim"), GainKnob::dbForPosition (6.25f));
            play (p, std::vector<float> (blockSize * 80, 0.0f));
            switching = play (p, input, [&] (size_t block)
            {
                if (block % 94 == 0) // every 0.25 s: amps 1, 2, ... 8, 1, ...
                    setParam (p, AmpSimProcessor::ampModelParamId, (float) ((block / 94) % AmpSimProcessor::numBuiltInAmps));
                maxSwitching = std::max (maxSwitching, models());
                maxAmpsSwitching = std::max (maxAmpsSwitching, p.getChain().amp.getRunningAmps());
            });
            setParam (p, AmpSimProcessor::ampModelParamId, 0.0f);
            expectEquals (maxBetween, 2, "between steps: the playing amp's two");
            expectEquals (minBetween, 2);
            expectEquals (maxSweeping, ampsim::NamAmp::maxRunningSteps, "a sweep warms ahead");
            expectEquals (maxAmpsSwitching, 2);
            expectLessOrEqual (maxSwitching, 4);
            expectLessThan (between.mean, 0.6 * deadlineMicros * cpuBudgetScale());
            expectLessThan (sweeping.mean, 0.7 * deadlineMicros * cpuBudgetScale());
            expectLessThan (switching.mean, 0.6 * deadlineMicros * cpuBudgetScale());
        }

        for (const auto* t : std::initializer_list<const Timing*> { &bare, &everything, &heaviest, &between, &sweeping, &switching })
        {
            expectEquals (t->counts.allocations, 0L);
            expectEquals (t->counts.frees, 0L);
            expectEquals (t->counts.blockingLocks, 0L);
        }
        expectLessThan (everything.mean, 0.6 * deadlineMicros * cpuBudgetScale());

        const juce::String captures = builtIns ? "the eight built-in amps loaded, Glass playing" : "A1 standard, A2, LSTM loaded, A1 playing";
        logMessage ("  -> " + captures + ", three mics, everything else at its defaults: " + bare.describe());
        logMessage ("  -> " + captures + ", every block on (both gates, both compressors, Screamer boost, Distortion at 4x, harmonizer with 2 voices, multivoicer 4 voices, "
                    "Bloom with all three, Tri chorus, tape delay, Hall with 50% shimmer): " + everything.describe());
        logMessage ("  -> " + captures + ", the heaviest settings (8x drive with the Fuzz, multivoicer Mono with 8 voices, 4 harmonies): " + heaviest.describe());
        if (builtIns)
        {
            logMessage ("  -> the heaviest settings with the Gain between two steps (" + juce::String (maxBetween) + " models running): " + between.describe());
            logMessage ("  -> the heaviest settings with the Gain sweeping 0 to 10 and back every 1.6 s (at most " + juce::String (maxSweeping) + " models): "
                        + sweeping.describe());
            logMessage ("  -> the heaviest settings with the Gains between two steps and the amp switching every 0.25 s through all eight (at most "
                        + juce::String (maxAmpsSwitching) + " amps and " + juce::String (maxSwitching) + " models running): " + switching.describe());
        }
        logMessage ("  -> all the runs: 0 allocations, 0 frees, 0 blocking locks on the audio thread (" + juce::String (bare.counts.allocations + everything.counts.allocations
                    + heaviest.counts.allocations) + " counted). Timed on a normal-priority test thread, with other builds running on the machine: the "
                    "real audio thread runs at real-time priority, so the worst cases here overstate it");
    }
};

static FullRigTests fullRigTests;

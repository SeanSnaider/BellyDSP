// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AllocationTracking.h"
#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"

#include <algorithm>
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
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
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
Timing play (AmpSimProcessor& p, const std::vector<float>& input)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<double> micros;
    micros.reserve (input.size() / blockSize + 1);
    Timing t;
    for (size_t start = 0; start + blockSize <= input.size(); start += blockSize)
    {
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
        beginTest ("the whole rig at once: three captures, three mics, every block on, 128-sample buffers: CPU against the deadline, and nothing allocated on the audio thread");
        rig (false);

        beginTest ("the same on the built-in captures (Glass, Ember, Monolith: three standard WaveNets), as the app starts");
        rig (true);
    }

    /// Defaults, everything on, and the heaviest settings, timed. builtIns: the three slots run the built-in
    /// captures the app starts with; otherwise NAM's example models (A1 standard, A2, LSTM).
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
            for (int s = 0; s < 3; ++s)
                expect (p.parameters.state.getProperty (AmpSimProcessor::modelPathKey (s)).toString() == presets::builtInCapture (s).getFullPathName());

        // Defaults: three captures running, the cab, the EQs (flat), everything else off.
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

        for (const auto* t : { &bare, &everything, &heaviest })
        {
            expectEquals (t->counts.allocations, 0L);
            expectEquals (t->counts.frees, 0L);
            expectEquals (t->counts.blockingLocks, 0L);
        }
        expectLessThan (everything.mean, 0.6 * deadlineMicros);

        const juce::String captures = builtIns ? "the built-in Glass, Ember, Monolith" : "A1 standard, A2, LSTM";
        logMessage ("  -> three captures (" + captures + ") and three mics, everything else at its defaults: " + bare.describe());
        logMessage ("  -> " + captures + ", every block on (both gates, both compressors, Screamer boost, Distortion at 4x, harmonizer with 2 voices, multivoicer 4 voices, "
                    "Bloom with all three, Tri chorus, tape delay, Hall with 50% shimmer): " + everything.describe());
        logMessage ("  -> " + captures + ", the heaviest settings (8x drive with the Fuzz, multivoicer Mono with 8 voices, 4 harmonies): " + heaviest.describe());
        logMessage ("  -> all three runs: 0 allocations, 0 frees, 0 blocking locks on the audio thread (" + juce::String (bare.counts.allocations + everything.counts.allocations
                    + heaviest.counts.allocations) + " counted). Timed on a normal-priority test thread, with other builds running on the machine: the "
                    "real audio thread runs at real-time priority, so the worst cases here overstate it");
    }
};

static FullRigTests fullRigTests;

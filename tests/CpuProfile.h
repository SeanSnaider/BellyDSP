// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "AllocationTracking.h"
#include "PluginProcessor.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

/// The CPU profile of the whole rig, block by block (BUILD_PLAN "CPU"): what the "Full rig" tests assert the budget
/// on, and what `ampsim_tests --bench` prints on any machine (docs/INSTALL.md, "Checking a slower computer").
namespace testing::cpu
{

/// One cost over a run of buffers, in microseconds per 128-sample buffer.
struct Stats
{
    double mean = 0.0, p99 = 0.0, worst = 0.0;
};

/// The stages of the callback that aren't blocks (AmpSimProcessor::CpuProfile), then the chain's own work.
enum class Stage
{
    parameters,
    taps,
    chainOwn,
    output,
    count
};

/// A run of the processor on a DI, profiled buffer by buffer.
struct RigRun
{
    juce::String name;
    Stats total;                                        // the whole callback
    std::array<Stats, ampsim::Chain::numSlots> blocks;  // each slot of the chain
    std::array<Stats, (size_t) Stage::count> stages;
    int buffers = 0, over = 0;                          // over: buffers past the deadline
    int maxModels = 0, maxAmps = 0;                     // NAM models and amps running, at most
    rtcheck::Counts counts;                             // the audio thread's allocations, frees, and blocking locks

    /// The mean and p99 of the whole callback as a share of the 128-sample deadline, in percent.
    double meanPercent() const;
    double p99Percent() const;
    juce::String summary() const;

    /// The per-block table, one line per row: every block that cost anything, the stages that aren't blocks, the total.
    juce::StringArray table() const;
};

/// Runs `input` through the processor in 128-sample buffers with the profile attached. beforeBlock runs before each
/// buffer, outside the timing (the message thread's side: knob moves, switches).
RigRun profile (AmpSimProcessor& p, const std::vector<float>& input, const juce::String& name,
                const std::function<void (size_t block)>& beforeBlock = {});

/// A processor as the app starts: the eight built-in amps loaded (Glass playing), prepared at 48 kHz, 128 samples.
std::unique_ptr<AmpSimProcessor> makeRig();

/// The rigs the budget is set on (BUILD_PLAN "CPU", the budget):
///   defaults: every parameter at its default, Glass, the cab a bundled 1 s IR on close mic 1;
///   typical: a factory preset as it loads (its first scene);
///   heaviest: every block on at its heaviest settings (8x drive with the Fuzz, the multivoicer Mono with 8 voices,
///             four harmonies, Hall with shimmer), three mics on 1 s IRs (the room stereo), and the Gain between two
///             steps (two models).
void setDefaults (AmpSimProcessor& p);
bool setFactoryPreset (AmpSimProcessor& p, const juce::var& preset);
void setHeaviest (AmpSimProcessor& p);

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue);
void waitForLoads (AmpSimProcessor& p);

/// Short names for the chain's slots, for tables.
const char* slotName (ampsim::Chain::Slot slot);

/// `ampsim_tests --bench [seconds]`: the defaults, every factory preset, and the heaviest rig on this machine, block by
/// block, against the 128-sample deadline and the budget. Returns the process exit code (0).
int runBenchmark (double seconds);

} // namespace testing::cpu

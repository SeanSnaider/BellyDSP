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
/// on, and what `ampsim_tests --bench` prints on any machine (BUILD_PLAN "CPU", the benchmark).
namespace testing::cpu
{

/// The budget, as shares of the 128-sample deadline (2.67 ms) on the dev Mac (an M5 Pro), mean and p99, measured flat
/// out (Pacing::flatOut), for the rigs below. The means are what a slower machine scales: a 4-core x64 laptop from about
/// 2017, assumed 3 to 5 times slower per core, keeps the defaults and the factory presets at or under about 55% of the
/// deadline at ASIO 128. The p99s are this Mac's: its scheduler stretches 1 to 2% of buffers by up to 1.8x (an idle
/// machine more than a busy one), so they're set from the worst of six runs, quiet and loaded, with a margin, to catch a
/// regression in the tail without failing on the OS (BUILD_PLAN "CPU", the budget; ASSUMPTIONS CPU1, CPU2, CPU9). The
/// heaviest rig is for faster machines. The tests multiply these by cpuBudgetScale().
struct Budget
{
    double meanPercent, p99Percent;
};
constexpr Budget defaultsBudget { 8.0, 13.0 };
constexpr Budget typicalBudget { 11.0, 18.0 };
constexpr Budget heaviestBudget { 30.0, 45.0 };

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

/// How the buffers are fed (BUILD_PLAN "CPU", how it's measured).
///   flatOut: back to back on the highest-priority ordinary thread (macOS: the user-interactive QoS class, a performance
///            core at its full clock). The work's cost at full speed: what a slower machine scales, and what the budget
///            is set on.
///   device:  one buffer every 2.67 ms on a real-time thread, as a device calls it (macOS: a time-constraint thread in an
///            audio work interval, like Core Audio's IO thread). The OS then picks the clock to meet the deadline with
///            the least power, so the share of the deadline is higher than at full speed and says how the power
///            management runs it, not how close the machine is to dropping out.
enum class Pacing
{
    flatOut,
    device
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
    Pacing pacing = Pacing::flatOut;
    bool realtime = false;                              // ran on a real-time thread (device pacing; false: flat out, or the OS refused)
    bool workgroup = false;                             // macOS: in an audio work interval (WorkInterval in CpuProfile.cpp)
    rtcheck::Counts counts;                             // the audio thread's allocations, frees, and blocking locks

    /// The mean and p99 of the whole callback as a share of the 128-sample deadline, in percent.
    double meanPercent() const;
    double p99Percent() const;
    juce::String summary() const;

    /// The per-block table, one line per row: every block that cost anything, the stages that aren't blocks, the total.
    juce::StringArray table() const;
};

/// Runs `input` through the processor in 128-sample buffers with the profile attached, on a thread of its own fed as
/// `pacing` says (device pacing takes as long as the audio). beforeBlock runs before each buffer, on that thread,
/// outside the timing.
RigRun profile (AmpSimProcessor& p, const std::vector<float>& input, const juce::String& name,
                const std::function<void (size_t block)>& beforeBlock = {}, Pacing pacing = Pacing::flatOut);

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
/// Lets everything loaded settle while audio runs (0.5 s of wall time, 400 buffers): the cab's IRs (JUCE builds each
/// convolution engine on its own background thread and crossfades it in over 50 ms), an amp's warm-up and switch, a
/// second step's warm-up, the bypass fades.
void settleLoads (AmpSimProcessor& p);
void waitForLoads (AmpSimProcessor& p);

/// Short names for the chain's slots, for tables.
const char* slotName (ampsim::Chain::Slot slot);

/// `ampsim_tests --bench [seconds]`: the defaults, every factory preset, and the heaviest rig on this machine, block by
/// block, against the 128-sample deadline and the budget. Returns the process exit code (0).
int runBenchmark (double seconds);

/// `ampsim_tests --render-rigs <folder>`: the defaults, every factory preset, and the heaviest rig rendered on 10 s of the
/// tests' DI into 32-bit float WAVs, one per rig, so two builds' outputs can be compared bit for bit. Returns 0.
int renderRigs (const juce::File& folder);

/// `ampsim_tests --bench-gui [seconds]`: opens the editor in a real window (it appears on screen) with the defaults
/// rig playing in real time on a thread of its own, shows each page in turn, and reports the message thread's CPU
/// time on each (the meters, the analyzer, the timers, the repaints), as a share of one core. Returns 0.
int runGuiBenchmark (double secondsPerPage);

} // namespace testing::cpu

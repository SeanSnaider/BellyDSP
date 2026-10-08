// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "CpuProfile.h"

#include "BuiltInCaptures.h"
#include "Presets.h"
#include "TestHelpers.h"
#include "PluginEditor.h"
#include "platform/AppInfo.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <ctime>
 #include <sys/resource.h>
#endif

#if JUCE_MAC
 #include <AudioToolbox/AudioWorkInterval.h>
 #include <mach/mach_time.h>
 #include <os/workgroup.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <optional>
#include <thread>

namespace testing::cpu
{
namespace
{
using Slot = ampsim::Chain::Slot;

Stats statsOf (std::vector<double> micros)
{
    Stats s;
    if (micros.empty())
        return s;
    std::sort (micros.begin(), micros.end());
    s.mean = std::accumulate (micros.begin(), micros.end(), 0.0) / (double) micros.size();
    s.p99 = micros[(size_t) (0.99 * (double) (micros.size() - 1))];
    s.worst = micros.back();
    return s;
}

double percent (double micros)
{
    return 100.0 * micros / deadlineMicros;
}

juce::String pad (const juce::String& text, int width)
{
    return text.paddedRight (' ', width);
}

juce::String num (double value, int decimals, int width)
{
    return juce::String (value, decimals).paddedLeft (' ', width);
}

juce::String row (const juce::String& name, const Stats& s)
{
    return pad (name, 26) + num (s.mean, 1, 8) + num (percent (s.mean), 2, 8) + "%" + num (s.p99, 1, 9) + num (percent (s.p99), 2, 8) + "%"
           + num (s.worst, 1, 9);
}

juce::File bundledIR (const juce::String& relative)
{
    return platform::factoryContentFolder().getChildFile ("irs").getChildFile (relative);
}

// The longest of the bundled IRs (1 s), and another for the second close mic.
const char* const longIR = "Modern 4x12/Modern 4x12, dynamic, bright 60 W, var. 3.wav";
const char* const longIR2 = "Modern 4x12/Modern 4x12, dynamic, 75 W, var. 1.wav";

void settle (AmpSimProcessor& p, int buffers)
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < buffers; ++i)
    {
        buffer.clear();
        p.processBlock (buffer, midi);
    }
}
/// The calling thread's CPU time, and the whole process's, in seconds.
double threadCpuSeconds()
{
   #if JUCE_WINDOWS
    FILETIME created, exited, kernel, user;
    GetThreadTimes (GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks = [] (FILETIME f) { return (double) (((unsigned long long) f.dwHighDateTime << 32) | f.dwLowDateTime); };
    return (ticks (kernel) + ticks (user)) * 1.0e-7;
   #else
    timespec t {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &t);
    return (double) t.tv_sec + 1.0e-9 * (double) t.tv_nsec;
   #endif
}

double processCpuSeconds()
{
   #if JUCE_WINDOWS
    FILETIME created, exited, kernel, user;
    GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto ticks = [] (FILETIME f) { return (double) (((unsigned long long) f.dwHighDateTime << 32) | f.dwLowDateTime); };
    return (ticks (kernel) + ticks (user)) * 1.0e-7;
   #else
    rusage u {};
    getrusage (RUSAGE_SELF, &u);
    return (double) u.ru_utime.tv_sec + 1.0e-6 * (double) u.ru_utime.tv_usec + (double) u.ru_stime.tv_sec + 1.0e-6 * (double) u.ru_stime.tv_usec;
   #endif
}

/// The instruction set this binary was compiled for (CMakeLists.txt, AMPSIM_X86_SIMD).
const char* buildTarget()
{
   #if defined (__aarch64__) || defined (_M_ARM64)
    return "arm64 (NEON)";
   #elif defined (__AVX2__) && (defined (__FMA__) || defined (_MSC_VER))
    return "x86-64 with AVX2 and FMA";
   #elif defined (__AVX__)
    return "x86-64 with AVX (no FMA)";
   #else
    return "x86-64 with SSE2";
   #endif
}
/// What tells the OS a real-time thread's deadline. On macOS a time-constraint thread alone isn't enough: with a
/// short burst every 2.67 ms and sleep in between, the scheduler keeps it on a low clock (measured: every rig 3 to 4
/// times its flat-out cost). Core Audio's IO thread belongs to the device's audio workgroup, which reports each cycle's
/// deadline, so the cores run as fast as the deadline needs. This does the same with the API Apple provides for audio
/// threads that run at their own cadence (AudioWorkInterval.h, macOS 11): AudioWorkIntervalCreate, os_workgroup_join,
/// and os_workgroup_interval_start / finish around every buffer. Elsewhere it does nothing (Windows schedules a
/// time-critical ASIO-priority thread without one).
class WorkInterval
{
public:
    WorkInterval()
    {
       #if JUCE_MAC
        interval = AudioWorkIntervalCreate ("BellyDSP CPU profile", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
        if (interval != nullptr)
            joined = os_workgroup_join (interval, &token) == 0;
        mach_timebase_info_data_t timebase {};
        mach_timebase_info (&timebase);
        periodTicks = (std::uint64_t) (1.0e9 * blockSize / fs * timebase.denom / timebase.numer);
       #endif
    }

    ~WorkInterval()
    {
       #if JUCE_MAC
        if (joined)
            os_workgroup_leave (interval, &token);
        if (interval != nullptr)
            os_release (interval);
       #endif
    }

    bool isJoined() const noexcept { return joined; }

    /// Around each buffer: its start, and the deadline one period later.
    void begin() noexcept
    {
       #if JUCE_MAC
        if (joined)
        {
            const auto now = mach_absolute_time();
            os_workgroup_interval_start (interval, now, now + periodTicks, nullptr);
        }
       #endif
    }

    void end() noexcept
    {
       #if JUCE_MAC
        if (joined)
            os_workgroup_interval_finish (interval, nullptr);
       #endif
    }

private:
   #if JUCE_MAC
    os_workgroup_interval_t interval = nullptr;
    os_workgroup_join_token_s token {};
    std::uint64_t periodTicks = 0;
   #endif
    bool joined = false;
};

/// Runs `body` on a thread of its own and waits for it: a real-time audio thread (JUCE's startRealtimeThread, with the
/// 128-sample period and deadline at 48 kHz) for device pacing, or the highest-priority ordinary thread (macOS: the
/// user-interactive QoS class, which keeps it on a performance core) for flat out. Returns whether it got real-time
/// scheduling.
bool runOnProfileThread (bool realtimeThread, std::function<void()> body)
{
    struct Runner final : juce::Thread
    {
        explicit Runner (std::function<void()> b) : juce::Thread ("CPU profile"), work (std::move (b)) {}
        void run() override { work(); }
        std::function<void()> work;
    };
    Runner runner (std::move (body));
    const auto options = juce::Thread::RealtimeOptions {}.withPeriodHz (fs / blockSize).withApproximateAudioProcessingTime (blockSize, fs);
    const auto realtime = realtimeThread && runner.startRealtimeThread (options);
    if (! realtime)
        runner.startThread (juce::Thread::Priority::highest);
    runner.waitForThreadToExit (-1);
    return realtime;
}
} // namespace

void settleLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 100; ++i)
    {
        settle (p, 4);
        juce::Thread::sleep (5);
        p.runHousekeeping();
    }
}

const char* slotName (Slot slot)
{
    switch (slot)
    {
        case Slot::inputGain:      return "input level";
        case Slot::gateA:          return "gate A";
        case Slot::preCompressor:  return "compressor (pre)";
        case Slot::boost:          return "boost";
        case Slot::overdrive:      return "overdrive";
        case Slot::preEq:          return "EQ (pre)";
        case Slot::amp:            return "amp (NAM + tone)";
        case Slot::gateB:          return "gate B";
        case Slot::cab:            return "cab";
        case Slot::matchCurve:     return "match curve";
        case Slot::postEq:         return "EQ (post)";
        case Slot::postCompressor: return "compressor (post)";
        case Slot::harmonizer:     return "harmonizer";
        case Slot::multivoicer:    return "multivoicer";
        case Slot::bloom:          return "Bloom";
        case Slot::chorus:         return "chorus";
        case Slot::delay:          return "delay";
        case Slot::reverb:         return "reverb";
        case Slot::outputGain:     return "output level";
        case Slot::preview:        return "A/B player";
        case Slot::limiter:        return "limiter";
        case Slot::count:          break;
    }
    return "?";
}

double RigRun::meanPercent() const { return percent (total.mean); }
double RigRun::p99Percent() const { return percent (total.p99); }

juce::String RigRun::summary() const
{
    return juce::String (meanPercent(), 1) + "% of the deadline on average (" + juce::String (juce::roundToInt (total.mean)) + " us), p99 "
           + juce::String (p99Percent(), 1) + "%, worst " + juce::String (percent (total.worst), 1) + "%, " + juce::String (over) + " of "
           + juce::String (buffers) + " buffers over";
}

juce::StringArray RigRun::table() const
{
    juce::StringArray lines;
    lines.add (name + ": " + summary() + " (" + juce::String (maxModels) + " NAM models running at most; "
               + (pacing == Pacing::flatOut ? juce::String ("flat out on a user-interactive thread")
                                            : (realtime ? juce::String ("paced like a device on a real-time thread") : juce::String ("paced, but NOT real-time: the OS refused"))
                                                  + (workgroup ? " in an audio work interval" : ""))
               + ")");
    lines.add (pad ("  block / stage", 26) + "  mean us  % dl.   p99 us  % dl.  worst us");
    lines.add (row ("  parameters, MIDI", stages[(size_t) Stage::parameters]));
    lines.add (row ("  tuner + analyzer taps", stages[(size_t) Stage::taps]));
    for (size_t i = 0; i < ampsim::Chain::numSlots; ++i)
        if (blocks[i].worst >= 0.05) // a block that's off and skipped costs nothing
            lines.add (row ("  " + juce::String (slotName ((Slot) i)), blocks[i]));
    lines.add (row ("  chain (DI, copies, dips)", stages[(size_t) Stage::chainOwn]));
    lines.add (row ("  meters, fades, output", stages[(size_t) Stage::output]));
    lines.add (row ("  TOTAL", total));
    return lines;
}

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 12000 && p.isLoading(); ++i) // the eight built-in sets take seconds on a busy machine
        juce::Thread::sleep (5);
}

RigRun profile (AmpSimProcessor& p, const std::vector<float>& input, const juce::String& name, const std::function<void (size_t)>& beforeBlock,
                Pacing pacing)
{
    RigRun run;
    run.name = name;

    const auto numBuffers = input.size() / (size_t) blockSize;
    std::vector<double> total, chainOwn, parameters, taps, output;
    std::array<std::vector<double>, ampsim::Chain::numSlots> blocks;
    for (auto* v : { &total, &chainOwn, &parameters, &taps, &output })
        v->reserve (numBuffers);
    for (auto& v : blocks)
        v.reserve (numBuffers);

    AmpSimProcessor::CpuProfile prof;
    p.setCpuProfile (&prof);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    // The buffers run as a device would call them: on a real-time thread (macOS: a time-constraint thread with a
    // 2.67 ms period in an audio work interval, as Core Audio's IO thread is; Windows: time-critical priority, as an
    // ASIO thread), one buffer per 2.67 ms of wall time, sleeping in between. A normal-priority thread run flat out
    // measured the p99 differently on an idle Mac and a busy one (the cores it lands on, their clock), which says
    // nothing about the app's audio thread (2026-10-08).
    const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * blockSize / fs));
    const auto device = pacing == Pacing::device;
    run.pacing = pacing;
    run.realtime = runOnProfileThread (device, [&]
    {
        std::optional<WorkInterval> interval;
        if (device)
            interval.emplace();
        run.workgroup = interval.has_value() && interval->isJoined();
        auto next = std::chrono::steady_clock::now();
        for (size_t b = 0; b < numBuffers; ++b)
        {
            if (interval)
                interval->begin();
            if (beforeBlock)
                beforeBlock (b);
            buffer.clear();
            buffer.copyFrom (0, 0, input.data() + b * (size_t) blockSize, blockSize);
            prof.clear();
            rtcheck::begin();
            p.processBlock (buffer, midi);
            run.counts += rtcheck::end();

            total.push_back (prof.totalMicros);
            chainOwn.push_back (prof.chain.chainMicros);
            parameters.push_back (prof.parametersMicros);
            taps.push_back (prof.tapsMicros);
            // Everything after the chain, plus the bits of the chain stage the chain itself didn't time (the DI recorder).
            const auto chainTimed = std::accumulate (prof.chain.slotMicros.begin(), prof.chain.slotMicros.end(), prof.chain.chainMicros);
            output.push_back (prof.outputMicros + std::max (0.0, prof.chainMicros - chainTimed));
            for (size_t i = 0; i < ampsim::Chain::numSlots; ++i)
                blocks[i].push_back (prof.chain.slotMicros[i]);

            run.maxModels = std::max (run.maxModels, p.getChain().amp.getRunningModels());
            run.maxAmps = std::max (run.maxAmps, p.getChain().amp.getRunningAmps());

            if (interval)
            {
                interval->end();
                next += period;
                std::this_thread::sleep_until (next);
            }
        }
    });
    p.setCpuProfile (nullptr);

    run.buffers = (int) numBuffers;
    run.over = (int) std::count_if (total.begin(), total.end(), [] (double us) { return us > deadlineMicros; });
    run.total = statsOf (std::move (total));
    run.stages[(size_t) Stage::parameters] = statsOf (std::move (parameters));
    run.stages[(size_t) Stage::taps] = statsOf (std::move (taps));
    run.stages[(size_t) Stage::chainOwn] = statsOf (std::move (chainOwn));
    run.stages[(size_t) Stage::output] = statsOf (std::move (output));
    for (size_t i = 0; i < ampsim::Chain::numSlots; ++i)
        run.blocks[i] = statsOf (std::move (blocks[i]));
    return run;
}

std::unique_ptr<AmpSimProcessor> makeRig()
{
    std::unique_ptr<AmpSimProcessor> owner;
    {
        WithBuiltInCaptures on;
        owner = std::make_unique<AmpSimProcessor>();
    }
    waitForLoads (*owner);
    owner->prepareToPlay (fs, blockSize);
    return owner;
}

void setDefaults (AmpSimProcessor& p)
{
    p.loadCabIR (0, bundledIR (longIR));
    waitForLoads (p);
    settleLoads (p);
}

bool setFactoryPreset (AmpSimProcessor& p, const juce::var& preset)
{
    const auto ok = p.loadPreset (preset).ok;
    waitForLoads (p);
    for (int i = 0; i < 400 && p.isChangingPreset(); ++i)
    {
        settle (p, 4); // the preset's fade out and in run on the audio thread
        p.runHousekeeping();
        waitForLoads (p);
    }
    p.recallScene (0);
    settleLoads (p);
    return ok && ! p.isChangingPreset();
}

void setHeaviest (AmpSimProcessor& p)
{
    // Three mics on 1 s IRs, the room's stereo.
    const auto room = tempDir().getChildFile ("cpu_room.wav");
    {
        juce::AudioBuffer<float> stereo (2, 48000);
        const auto left = syntheticCabIR (48000, 0.0, 4000.0), right = syntheticCabIR (48000, 2.0, 3500.0);
        for (int n = 0; n < 48000; ++n)
        {
            stereo.setSample (0, n, (float) left[(size_t) n]);
            stereo.setSample (1, n, (float) right[(size_t) n]);
        }
        writeWav (room, stereo);
    }
    p.loadCabIR (0, bundledIR (longIR));
    p.loadCabIR (1, bundledIR (longIR2));
    p.loadCabIR (AmpSimProcessor::roomMic, room);
    waitForLoads (p);

    for (const auto& [id, value] : std::initializer_list<std::pair<const char*, float>> {
             { "gate_a_on", 1 }, { "gate_b_on", 1 }, { "comp_pre_on", 1 }, { "boost_on", 1 }, { "boost_mode", 2 }, { "od_on", 1 },
             { "comp_post_on", 1 }, { "harm_on", 1 }, { "harm_v2_on", 1 }, { "harm_v3_on", 1 }, { "harm_v4_on", 1 }, { "mv_on", 1 },
             { "mv_engine", 1 }, { "mv_voices", 8 }, { "bloom_on", 1 }, { "bloom_crush_on", 1 }, { "bloom_phaser_on", 1 }, { "bloom_flanger_on", 1 },
             { "chorus_on", 1 }, { "chorus_mode", 2 }, { "delay_on", 1 }, { "delay_mode", 2 }, { "reverb_on", 1 }, { "reverb_engine", 1 },
             { "reverb_shimmer", 50 }, { "drive_oversampling", 1 }, { "od_mode", 3 }, { "eq_pre_on", 1 }, { "eq_post_on", 1 },
             { "cab_lowcut_on", 1 }, { "cab_highcut_on", 1 } })
        setParam (p, id, value);

    // The Gain between two steps: two models (BUILD_PLAN "Amp gain").
    setParam (p, AmpSimProcessor::ampParamId (p.getChain().amp.getSelectedAmp(), "input_trim"),
              ampsim::AmpSection::GainKnob::dbForPosition (6.25f));
    settleLoads (p);
}

int runBenchmark (double seconds)
{
    // The process at high priority; each rig's buffers on a thread of their own (profile()).
    juce::Process::setPriority (juce::Process::HighPriority);
    const auto input = guitarDI ((int) (seconds * fs));
    std::cout << "BellyDSP CPU benchmark: " << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumPhysicalCpus() << " cores ("
              << juce::SystemStats::getNumCpus() << " threads), " << juce::SystemStats::getOperatingSystemName()
              << (juce::SystemStats::hasAVX2() ? ", AVX2" : "") << (juce::SystemStats::hasFMA3() ? ", FMA" : "")
              << (juce::SystemStats::hasNeon() ? ", NEON" : "") << "\n"
              << "Built for " << buildTarget() << "\n"
              << "48 kHz, 128-sample buffers (the deadline is " << juce::String (deadlineMicros, 1) << " us), " << seconds
              << " s of guitar DI per rig, flat out on a high-priority thread and then paced like a device on a real-time thread\n\n";

    std::vector<RigRun> runs;
    const auto show = [&runs] (const RigRun& run)
    {
        for (const auto& line : run.table())
            std::cout << line << "\n";
        std::cout << std::endl;
        runs.push_back (run);
    };

    // Each rig twice: flat out (the per-block table; the work at full clock) and paced like a device (the summary's
    // second pair: what the OS's power management makes of it).
    std::vector<RigRun> paced;
    const auto both = [&] (AmpSimProcessor& p, const juce::String& name)
    {
        show (profile (p, input, name));
        paced.push_back (profile (p, input, name, {}, Pacing::device));
    };
    {
        auto p = makeRig();
        setDefaults (*p);
        both (*p, "Defaults (Glass, one close mic on a 1 s IR, everything else off)");
        for (const auto& preset : presets::factoryPresets())
        {
            setFactoryPreset (*p, preset);
            both (*p, "Factory preset " + preset["name"].toString());
        }
    }
    {
        auto p = makeRig();
        setHeaviest (*p);
        both (*p, "Heaviest (every block on at its heaviest, three mics on 1 s IRs, the Gain between two steps)");
    }

    std::cout << "Summary, mean / p99 of the 128-sample deadline: flat out (the work at full clock), then paced like a device "
              << (paced.empty() || ! paced.front().realtime ? "(NOT on a real-time thread: the OS refused)" : "(a real-time thread)") << ":\n";
    for (size_t i = 0; i < runs.size(); ++i)
    {
        const auto& run = runs[i];
        std::cout << "  " << pad (run.name.upToFirstOccurrenceOf (" (", false, false), 34) << num (run.meanPercent(), 1, 6) << "% / "
                  << num (run.p99Percent(), 1, 5) << "%     paced " << num (paced[i].meanPercent(), 1, 5) << "% / " << num (paced[i].p99Percent(), 1, 5) << "%"
                  << (run.over + paced[i].over > 0 ? "   " + juce::String (run.over + paced[i].over) + " buffers over the deadline" : juce::String()) << "\n";
    }
    std::cout << "\nRule of thumb (BUILD_PLAN \"CPU\"): a flat-out mean under 50% and p99 under 60% of the deadline leave the driver and the\n"
                 "GUI room at that buffer size. The paced figures can be higher on an idle machine (the OS lowers the clock while the\n"
                 "deadline is met) and should stay well under 100%.\n";
    return 0;
}

int runGuiBenchmark (double secondsPerPage)
{
    auto p = makeRig();
    setDefaults (*p);

    // The audio: the defaults rig on a looped DI, one 128-sample buffer per 2.67 ms of wall time, as a device would
    // call it, so the meters, the analyzer, and the tuner move.
    std::atomic<bool> running { true };
    std::atomic<double> audioCpu { 0.0 };
    std::thread audio ([&]
    {
        const auto di = guitarDI ((int) (10.0 * fs));
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        const auto period = std::chrono::microseconds ((long long) deadlineMicros);
        auto next = std::chrono::steady_clock::now();
        size_t position = 0;
        const auto start = threadCpuSeconds();
        while (running.load())
        {
            buffer.clear();
            buffer.copyFrom (0, 0, di.data() + position, blockSize);
            position = (position + (size_t) blockSize) % (di.size() - (size_t) blockSize);
            p->processBlock (buffer, midi);
            audioCpu.store (threadCpuSeconds() - start);
            next += period;
            std::this_thread::sleep_until (next);
        }
    });

    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    auto& ed = dynamic_cast<AmpSimEditor&> (*editor);
    editor->setOpaque (true);
    editor->addToDesktop (juce::ComponentPeer::windowHasTitleBar | juce::ComponentPeer::windowIsResizable);
    editor->setTopLeftPosition (60, 60);
    editor->setVisible (true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (1000); // the window up, the first paint done

    std::cout << "BellyDSP GUI benchmark: " << juce::SystemStats::getCpuModel() << ", the editor at " << editor->getWidth() << " x "
              << editor->getHeight() << " (the window is on screen while it runs), the defaults rig playing\n"
              << "Each page for " << secondsPerPage << " s: the message thread's CPU time (timers, meters, the analyzer, repaints) as a share of one core\n\n";

    const auto measurePage = [&] (ui::PageId page, const juce::String& name)
    {
        ed.showPage (page);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300); // the page's first paint, not measured
        const auto wall0 = juce::Time::getMillisecondCounterHiRes();
        const auto main0 = threadCpuSeconds(), process0 = processCpuSeconds(), audio0 = audioCpu.load();
        juce::MessageManager::getInstance()->runDispatchLoopUntil ((int) (secondsPerPage * 1000.0));
        const auto wall = (juce::Time::getMillisecondCounterHiRes() - wall0) / 1000.0;
        const auto main = threadCpuSeconds() - main0, audioThread = audioCpu.load() - audio0;
        const auto others = processCpuSeconds() - process0 - main - audioThread;
        std::cout << "  " << pad (name, 26) << "message thread " << num (100.0 * main / wall, 1, 5) << "% of a core,  audio thread "
                  << num (100.0 * audioThread / wall, 1, 5) << "%,  other threads " << num (100.0 * std::max (0.0, others) / wall, 1, 5) << "%\n";
    };

    for (int i = 0; i < ui::numPages; ++i)
    {
        const auto page = (ui::PageId) i;
        if (page == ui::PageId::tuner)
        {
            setParam (*p, "tuner_on", 1.0f);
            measurePage (page, "tuner (engaged)");
            setParam (*p, "tuner_on", 0.0f);
            continue;
        }
        measurePage (page, ui::pageName (page));
    }

    editor->setVisible (false);
    editor.reset();
    running = false;
    audio.join();
    std::cout << "\nThe audio thread's share is the defaults rig in real time; the message thread runs on another core.\n";
    return 0;
}

} // namespace testing::cpu

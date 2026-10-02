#include "TunerThread.h"

#include <thread>

namespace ampsim
{

void TunerReadout::publish (const TunerReading& r) noexcept
{
    const auto s = sequence.load (std::memory_order_relaxed);
    sequence.store (s + 1, std::memory_order_relaxed); // odd: an update is in progress
    std::atomic_thread_fence (std::memory_order_release); // ...and no field store moves above that

    hasReading.store (r.hasReading, std::memory_order_relaxed);
    live.store (r.live, std::memory_order_relaxed);
    midiNote.store (r.midiNote, std::memory_order_relaxed);
    frequency.store (r.frequency, std::memory_order_relaxed);
    cents.store (r.cents, std::memory_order_relaxed);
    rawCents.store (r.rawCents, std::memory_order_relaxed);
    clarity.store (r.clarity, std::memory_order_relaxed);
    strobePhase.store (r.strobePhase, std::memory_order_relaxed);
    strobeVelocity.store (r.strobeVelocity, std::memory_order_relaxed);
    levelDb.store (r.levelDb, std::memory_order_relaxed);
    referenceA4.store (r.referenceA4, std::memory_order_relaxed);

    sequence.store (s + 2, std::memory_order_release); // even again: the fields above are visible with it
}

TunerReading TunerReadout::read() const noexcept
{
    for (;;)
    {
        const auto before = sequence.load (std::memory_order_acquire);

        if ((before & 1u) != 0)
        {
            std::this_thread::yield(); // the writer is mid-update; it never waits, so this is brief
            continue;
        }

        TunerReading r;
        r.hasReading = hasReading.load (std::memory_order_relaxed);
        r.live = live.load (std::memory_order_relaxed);
        r.midiNote = midiNote.load (std::memory_order_relaxed);
        r.frequency = frequency.load (std::memory_order_relaxed);
        r.cents = cents.load (std::memory_order_relaxed);
        r.rawCents = rawCents.load (std::memory_order_relaxed);
        r.clarity = clarity.load (std::memory_order_relaxed);
        r.strobePhase = strobePhase.load (std::memory_order_relaxed);
        r.strobeVelocity = strobeVelocity.load (std::memory_order_relaxed);
        r.levelDb = levelDb.load (std::memory_order_relaxed);
        r.referenceA4 = referenceA4.load (std::memory_order_relaxed);

        std::atomic_thread_fence (std::memory_order_acquire); // no field load moves below the re-check

        if (sequence.load (std::memory_order_relaxed) == before)
            return r;
    }
}

TunerThread::TunerThread() : juce::Thread ("Tuner analysis") {}

TunerThread::~TunerThread() { release(); }

void TunerThread::prepare (double sampleRate, int maxBlockSize)
{
    release();

    // About 1.4 s of audio (65536 samples at 48 kHz): the analysis thread can be that late before
    // anything is dropped, and a drop only restarts the note.
    ring.prepare (std::max ((int) sampleRate, 8 * maxBlockSize));
    scratch.assign (4096, 0.0f);

    TunerAnalysis::Settings settings;
    settings.referenceA4 = referenceA4.load();
    analysis.prepare (sampleRate, settings);
    readout.publish (analysis.getReading());

    // Normal priority: on macOS JUCE maps Priority::low to the "utility" quality-of-service class, whose
    // timers the OS coalesces: the "Tuner" test measured 12 updates a second instead of 60 there. The audio
    // thread runs at real-time priority far above either.
    startThread (juce::Thread::Priority::normal);
}

void TunerThread::release()
{
    stopThread (2000);
}

void TunerThread::setReferenceA4 (double hz) noexcept
{
    referenceA4.store (juce::jlimit (TunerAnalysis::minReferenceA4, TunerAnalysis::maxReferenceA4, hz), std::memory_order_relaxed);
}

void TunerThread::setUpdateRate (double hz) noexcept
{
    intervalMs.store (1000.0 / juce::jlimit (30.0, 60.0, hz), std::memory_order_relaxed);
}

void TunerThread::run()
{
    bool wasEngaged = false;

    // Updates are scheduled against a clock rather than "work, then sleep 16 ms": macOS wakes a waiting
    // thread a few milliseconds late, which on its own gave 48 updates a second instead of 60.
    auto deadline = juce::Time::getMillisecondCounterHiRes();

    while (! threadShouldExit())
    {
        if (! engaged.load (std::memory_order_acquire))
        {
            if (wasEngaged)
            {
                analysis.reset();
                readout.publish (analysis.getReading()); // an empty reading, so re-engaging shows nothing stale
                wasEngaged = false;
            }

            ring.discardAll(); // whatever the audio thread pushed just before disengaging
            ring.takeDroppedCount();
            wait (50);
            deadline = juce::Time::getMillisecondCounterHiRes();
            continue;
        }

        if (! wasEngaged)
        {
            analysis.reset();
            readout.publish (analysis.getReading());
            wasEngaged = true;
        }

        if (ring.takeDroppedCount() > 0)
            analysis.markDiscontinuity();

        bool any = false;

        for (;;)
        {
            const auto n = ring.read (scratch.data(), (int) scratch.size());

            if (n == 0)
                break;

            analysis.push (scratch.data(), n);
            any = true;
        }

        if (any)
        {
            analysis.setReferenceA4 (referenceA4.load (std::memory_order_relaxed));
            readout.publish (analysis.analyse());
        }

        const auto interval = intervalMs.load (std::memory_order_relaxed);
        const auto now = juce::Time::getMillisecondCounterHiRes();
        deadline = std::max (deadline + interval, now - interval); // after a long stall, don't try to catch up
        wait (juce::jmax (1, juce::roundToInt (deadline - now)));
    }
}

} // namespace ampsim

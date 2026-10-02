#pragma once

#include "SpscRing.h"
#include "TunerAnalysis.h"

#include <juce_core/juce_core.h>

#include <atomic>

namespace ampsim
{

/// Tuner readings from the analysis thread to the GUI through atomics, never a lock (BUILD_PLAN "Audio to
/// GUI"). A sequence counter keeps a reader from mixing two updates (a frequency from one and a note from
/// the next): the writer makes it odd before changing the fields and even again after, and a reader
/// retries if it was odd or changed while it read. That's a seqlock, written the way Hans Boehm shows is
/// correct under the C++ memory model ("Can Seqlocks Get Along with Programming Language Memory Models?",
/// MSPC 2012): every field is itself an atomic, and two fences order them against the counter.
/// One writer thread; any number of readers. The writer never waits.
class TunerReadout
{
public:
    void publish (const TunerReading& r) noexcept;

    /// A consistent snapshot of the latest publish().
    TunerReading read() const noexcept;

    /// How many times publish() has run.
    uint32_t getPublishCount() const noexcept { return sequence.load (std::memory_order_acquire) / 2; }

private:
    std::atomic<uint32_t> sequence { 0 };
    std::atomic<bool> hasReading { false }, live { false };
    std::atomic<int> midiNote { -1 };
    std::atomic<double> frequency { 0.0 }, cents { 0.0 }, rawCents { 0.0 }, clarity { 0.0 }, strobePhase { 0.0 },
        strobeVelocity { 0.0 }, levelDb { -300.0 }, referenceA4 { 440.0 };

    static_assert (std::atomic<double>::is_always_lock_free);
};

/// Hosts the tuner (BUILD_PLAN "Tuner", Hosting): the audio thread copies the DI into a lock-free ring, and
/// this thread drains it into a TunerAnalysis 60 times a second while the tuner is engaged, publishing each
/// reading to a TunerReadout. The processor owns one.
///
///     prepareToPlay:  tuner.prepare (sampleRate, samplesPerBlock);   // allocates, starts the thread
///     processBlock:   tuner.pushAudio (context.di, numSamples);       // wait-free; does nothing unless engaged
///     footswitch/GUI: tuner.setEngaged (true);                        // any thread, one atomic store
///     GUI timer:      auto r = tuner.getReading();                    // a consistent snapshot
///     settings:       tuner.setReferenceA4 (432.0);                   // any thread, used from the next update
///
/// Muting the output while tuning is the processor's job (it can check isEngaged()).
///
/// The thread runs from prepare() until destruction; while disengaged it only wakes every 50 ms to throw
/// away anything left in the ring, so setEngaged() can be a single atomic store, safe even on the audio
/// thread (the footswitch's MIDI arrives there). Engaging starts from a clean slate: the thread resets the
/// analysis and publishes an empty reading when it sees the change.
class TunerThread : private juce::Thread
{
public:
    TunerThread();
    ~TunerThread() override;

    /// Message thread, while the audio callback isn't running (prepareToPlay). Allocates the ring (about
    /// 1.4 s of audio) and the analysis, then (re)starts the analysis thread.
    void prepare (double sampleRate, int maxBlockSize);

    /// Stops the analysis thread (the destructor does this too).
    void release();

    /// Any thread. While disengaged, pushAudio() returns at once and nothing is analysed.
    void setEngaged (bool shouldBeEngaged) noexcept { engaged.store (shouldBeEngaged, std::memory_order_release); }
    bool isEngaged() const noexcept { return engaged.load (std::memory_order_acquire); }

    /// Audio thread: copies the DI into the ring. Wait-free: no allocation, no lock, no waiting.
    void pushAudio (const float* di, int numSamples) noexcept
    {
        if (engaged.load (std::memory_order_relaxed))
            ring.write (di, numSamples);
    }

    /// GUI thread: the latest reading.
    TunerReading getReading() const noexcept { return readout.read(); }

    /// Any thread: the A4 reference, clamped to 430..450 Hz.
    void setReferenceA4 (double hz) noexcept;
    double getReferenceA4() const noexcept { return referenceA4.load (std::memory_order_relaxed); }

    /// Updates per second while engaged, 30 to 60 (default 60).
    void setUpdateRate (double hz) noexcept;

    /// Analyses run since prepare() (a test and diagnostics hook).
    uint32_t getUpdateCount() const noexcept { return readout.getPublishCount(); }

private:
    void run() override;

    SpscRing<float> ring;
    TunerAnalysis analysis;
    TunerReadout readout;
    std::vector<float> scratch;
    std::atomic<bool> engaged { false };
    std::atomic<double> referenceA4 { 440.0 };
    std::atomic<double> intervalMs { 1000.0 / 60.0 };
};

} // namespace ampsim

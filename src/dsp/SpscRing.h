#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace ampsim
{

/// A lock-free single-producer, single-consumer ring buffer of samples: the audio thread writes, one
/// other thread reads (BUILD_PLAN "Tuner", Hosting). Neither side ever waits for the other.
///
/// Two counters that only grow: `written` (owned by the producer) and `consumed` (owned by the consumer).
/// written - consumed is the number of samples waiting, and a counter's position in the buffer is the
/// counter modulo the capacity, which is a power of two so that's a bit mask. Each side reads the other's
/// counter with acquire and publishes its own with release. That pairing is what makes it safe: when the
/// consumer sees `written` move, the samples the producer copied before that store are guaranteed to be
/// visible to it, and when the producer sees `consumed` move, the consumer is done reading that space.
/// (Java's volatile gives the same guarantee for a single variable.)
///
/// Writing copies at most two stretches (before and after the wrap) and does one atomic store: no
/// allocation, no lock, no loop that waits, so it's safe on the audio thread. When the ring is full the
/// producer drops what doesn't fit and counts it; the consumer finds out through takeDroppedCount().
template <typename T>
class SpscRing
{
public:
    /// Allocates. Not thread-safe: call it while neither side is running.
    void prepare (int minimumCapacity)
    {
        size_t size = 1;
        while (size < (size_t) std::max (1, minimumCapacity))
            size <<= 1;
        buffer.assign (size, T {});
        mask = size - 1;
        written.store (0, std::memory_order_relaxed);
        consumed.store (0, std::memory_order_relaxed);
        dropped.store (0, std::memory_order_relaxed);
    }

    int getCapacity() const noexcept { return (int) buffer.size(); }

    // ---- Producer (the audio thread) ------------------------------------------------------------

    /// Copies up to numSamples in; returns how many fit. Wait-free.
    int write (const T* data, int numSamples) noexcept
    {
        if (numSamples <= 0)
            return 0;

        const auto w = written.load (std::memory_order_relaxed); // only this thread changes it
        const auto c = consumed.load (std::memory_order_acquire);
        const auto space = buffer.size() - (size_t) (w - c);
        const auto n = std::min ((size_t) std::max (0, numSamples), space);

        copyIn (data, w, n);
        written.store (w + n, std::memory_order_release);

        if (n < (size_t) numSamples)
            dropped.fetch_add ((uint64_t) numSamples - n, std::memory_order_relaxed);

        return (int) n;
    }

    // ---- Consumer (one other thread) ------------------------------------------------------------

    int getNumReady() const noexcept
    {
        return (int) (written.load (std::memory_order_acquire) - consumed.load (std::memory_order_relaxed));
    }

    /// Copies up to maxSamples out, oldest first; returns how many.
    int read (T* destination, int maxSamples) noexcept
    {
        const auto c = consumed.load (std::memory_order_relaxed); // only this thread changes it
        const auto w = written.load (std::memory_order_acquire);
        const auto n = std::min ((size_t) std::max (0, maxSamples), (size_t) (w - c));

        copyOut (destination, c, n);
        consumed.store (c + n, std::memory_order_release);
        return (int) n;
    }

    /// Throws away everything waiting (the consumer's way to start fresh while the producer runs).
    void discardAll() noexcept
    {
        consumed.store (written.load (std::memory_order_acquire), std::memory_order_release);
    }

    /// Samples the producer dropped since the last call, because the ring was full.
    uint64_t takeDroppedCount() noexcept { return dropped.exchange (0, std::memory_order_relaxed); }

private:
    void copyIn (const T* data, uint64_t position, size_t n) noexcept
    {
        const auto start = (size_t) (position & mask);
        const auto first = std::min (n, buffer.size() - start);
        std::copy (data, data + first, buffer.begin() + (std::ptrdiff_t) start);
        std::copy (data + first, data + n, buffer.begin());
    }

    void copyOut (T* destination, uint64_t position, size_t n) const noexcept
    {
        const auto start = (size_t) (position & mask);
        const auto first = std::min (n, buffer.size() - start);
        std::copy (buffer.begin() + (std::ptrdiff_t) start, buffer.begin() + (std::ptrdiff_t) (start + first), destination);
        std::copy (buffer.begin(), buffer.begin() + (std::ptrdiff_t) (n - first), destination + first);
    }

    std::vector<T> buffer;
    uint64_t mask = 0;
    // Each counter on its own cache line, so the two threads don't keep stealing one line from each other.
    alignas (64) std::atomic<uint64_t> written { 0 };
    alignas (64) std::atomic<uint64_t> consumed { 0 };
    alignas (64) std::atomic<uint64_t> dropped { 0 };

    static_assert (std::atomic<uint64_t>::is_always_lock_free);
};

} // namespace ampsim

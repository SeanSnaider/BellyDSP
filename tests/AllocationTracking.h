#pragma once

namespace rtcheck
{

struct Counts
{
    long allocations = 0;
    long frees = 0;
    long blockingLocks = 0;

    Counts& operator+= (const Counts& other)
    {
        allocations += other.allocations;
        frees += other.frees;
        blockingLocks += other.blockingLocks;
        return *this;
    }
};

/// Whether blocking locks are counted on this platform: yes on macOS (AllocationTracking.cpp), no elsewhere
/// (AllocationTracking_portable.cpp, which also only sees C++ new and delete). The macOS build is the
/// authoritative real-time check.
#if defined(__APPLE__)
constexpr bool lockCountingAvailable = true;
#else
constexpr bool lockCountingAvailable = false;
#endif

/// Starts counting, for the calling thread only:
///  - heap allocations and frees, through macOS libmalloc's malloc_logger hook. That hook fires for
///    every malloc, calloc, realloc, and free in every library, including system ones.
///  - blocking mutex locks (pthread_mutex_lock), for code compiled into this test binary: our DSP,
///    JUCE, and NAM core. Locks taken inside system libraries (such as libc++'s std::mutex) aren't
///    seen.
void begin();

/// Stops counting and returns what the tracked thread did since begin().
Counts end();

} // namespace rtcheck

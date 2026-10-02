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

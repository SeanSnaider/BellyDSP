#include "AllocationTracking.h"

// The real-time checks' counters on platforms without macOS's malloc_logger hook (Windows, for the CI
// build). Weaker than the macOS version, and the test output says so:
//  - allocations and frees are counted by replacing the global operator new and delete in this test
//    binary, so C++ allocations (std::vector, juce::HeapBlock through new, ...) are seen, but a direct
//    malloc() in C code or inside a system library is not;
//  - blocking locks are not counted at all (lockCountingAvailable is false), so a 0 there proves nothing.
// The macOS build (AllocationTracking.cpp) remains the authoritative real-time safety check.
//
// UNTESTED: compiled only on Windows (CI), which hasn't run yet.

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

#if defined(_MSC_VER)
 #include <malloc.h>
#endif

namespace
{
std::atomic<bool> enabled { false };
std::atomic<std::thread::id> trackedThread {};
std::atomic<long> allocations { 0 }, frees { 0 };

bool onTrackedThread() noexcept
{
    return enabled.load (std::memory_order_relaxed) && std::this_thread::get_id() == trackedThread.load (std::memory_order_relaxed);
}

void* allocate (std::size_t size)
{
    if (onTrackedThread())
        ++allocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void release (void* p) noexcept
{
    if (p == nullptr)
        return;
    if (onTrackedThread())
        ++frees;
    std::free (p);
}

void* allocateAligned (std::size_t size, std::align_val_t alignment)
{
    if (onTrackedThread())
        ++allocations;
   #if defined(_MSC_VER)
    if (void* p = _aligned_malloc (size == 0 ? 1 : size, static_cast<std::size_t> (alignment)))
        return p;
   #else
    const auto a = static_cast<std::size_t> (alignment);
    if (void* p = std::aligned_alloc (a, ((size == 0 ? 1 : size) + a - 1) / a * a))
        return p;
   #endif
    throw std::bad_alloc();
}

void releaseAligned (void* p) noexcept
{
    if (p == nullptr)
        return;
    if (onTrackedThread())
        ++frees;
   #if defined(_MSC_VER)
    _aligned_free (p);
   #else
    std::free (p);
   #endif
}
} // namespace

// The replaceable global allocation functions ([new.delete]): every new and delete in the program.
void* operator new (std::size_t size) { return allocate (size); }
void* operator new[] (std::size_t size) { return allocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    try { return allocate (size); } catch (...) { return nullptr; }
}
void* operator new (std::size_t size, std::align_val_t a) { return allocateAligned (size, a); }
void* operator new[] (std::size_t size, std::align_val_t a) { return allocateAligned (size, a); }
void operator delete (void* p) noexcept { release (p); }
void operator delete[] (void* p) noexcept { release (p); }
void operator delete (void* p, std::size_t) noexcept { release (p); }
void operator delete[] (void* p, std::size_t) noexcept { release (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { release (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { release (p); }
void operator delete (void* p, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }

namespace rtcheck
{
void begin()
{
    allocations = 0;
    frees = 0;
    trackedThread = std::this_thread::get_id();
    enabled = true;
}

Counts end()
{
    enabled = false;
    return { allocations.load(), frees.load(), 0 };
}
} // namespace rtcheck

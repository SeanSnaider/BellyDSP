#include "AllocationTracking.h"

#include <atomic>
#include <cstdint>
#include <dlfcn.h>
#include <pthread.h>

// libmalloc calls this hook, when it's set, on every allocation and free in every zone. It's the
// hook Instruments' allocation recording uses. It isn't in a public header, so it's declared here.
typedef void (malloc_logger_t) (uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                uintptr_t result, uint32_t numHotFramesToSkip);
extern "C" malloc_logger_t* malloc_logger;

namespace
{
std::atomic<bool> enabled { false };
std::atomic<pthread_t> trackedThread { nullptr };
std::atomic<long> allocations { 0 }, frees { 0 }, locks { 0 };

constexpr uint32_t logTypeAllocate = 2;   // MALLOC_LOG_TYPE_ALLOCATE
constexpr uint32_t logTypeDeallocate = 4; // MALLOC_LOG_TYPE_DEALLOCATE

bool onTrackedThread() noexcept
{
    return enabled.load (std::memory_order_relaxed)
           && pthread_equal (pthread_self(), trackedThread.load (std::memory_order_relaxed));
}

// Must not allocate: it runs inside malloc.
void logger (uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
{
    if (! onTrackedThread())
        return;

    if ((type & logTypeAllocate) != 0)
        ++allocations;

    if ((type & logTypeDeallocate) != 0)
        ++frees;
}
} // namespace

// Defining pthread_mutex_lock in the executable wraps every call made from code compiled into it.
// RTLD_NEXT finds the real one in libSystem.
extern "C" int pthread_mutex_lock (pthread_mutex_t* mutex)
{
    using LockFn = int (*) (pthread_mutex_t*);
    static std::atomic<LockFn> real { nullptr };

    auto fn = real.load (std::memory_order_acquire);

    if (fn == nullptr)
    {
        fn = reinterpret_cast<LockFn> (dlsym (RTLD_NEXT, "pthread_mutex_lock"));
        real.store (fn, std::memory_order_release);
    }

    if (onTrackedThread())
        ++locks;

    return fn (mutex);
}

namespace rtcheck
{

void begin()
{
    allocations = 0;
    frees = 0;
    locks = 0;
    trackedThread = pthread_self();
    malloc_logger = logger;
    enabled = true;
}

Counts end()
{
    enabled = false;
    malloc_logger = nullptr;
    return { allocations.load(), frees.load(), locks.load() };
}

} // namespace rtcheck

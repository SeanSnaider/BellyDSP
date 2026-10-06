// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include <atomic>
#include <memory>

namespace ampsim
{

/// Lock-free handoff of a heap object (a loaded model, an IR) from a loader thread to the audio
/// thread, with a return path so the audio thread never frees memory (BUILD_PLAN "Threading and
/// loading"). Freeing is as forbidden on the audio thread as allocating, because the allocator
/// can take a lock.
///
/// Two single-slot mailboxes, each one an atomic pointer:
///   pending: loader -> audio thread. A newer publish replaces an unclaimed older one.
///   retired: audio thread -> loader. Whatever the audio thread is done with, freed by collect().
///
/// Every transfer is one atomic exchange, so each object has exactly one owner at a time and no
/// thread ever waits on another.
template <typename T>
class Handoff
{
public:
    Handoff() = default;
    Handoff (const Handoff&) = delete;
    Handoff& operator= (const Handoff&) = delete;

    ~Handoff()
    {
        delete pending.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    /// Loader or message thread. Takes ownership of obj.
    void publish (std::unique_ptr<T> obj)
    {
        collect();
        // If the audio thread never claimed the previous object, we get it back here and free it.
        delete pending.exchange (obj.release());
    }

    /// Any thread: whether a published object is waiting to be taken.
    bool hasPending() const noexcept { return pending.load() != nullptr; }

    /// Audio thread. Returns the newest published object, or nullptr. The caller now owns it.
    T* take() noexcept { return pending.exchange (nullptr); }

    /// Audio thread. Hands an object back to be freed elsewhere. Returns false if the return slot
    /// is still occupied; the caller keeps the object and tries again next buffer.
    bool retire (T* obj) noexcept
    {
        T* expected = nullptr;
        return retired.compare_exchange_strong (expected, obj);
    }

    /// Any non-audio thread. Frees whatever the audio thread handed back.
    void collect() { delete retired.exchange (nullptr); }

private:
    std::atomic<T*> pending { nullptr };
    std::atomic<T*> retired { nullptr };

    static_assert (std::atomic<T*>::is_always_lock_free);
};

} // namespace ampsim

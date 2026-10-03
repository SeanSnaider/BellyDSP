// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "dsp/Handoff.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <thread>

namespace
{
struct Tracked
{
    static inline std::atomic<int> alive { 0 };

    explicit Tracked (int i) : id (i) { ++alive; }
    ~Tracked() { --alive; }

    int id;
};

class HandoffTests final : public juce::UnitTest
{
public:
    HandoffTests() : juce::UnitTest ("Handoff", "ampsim") {}

    void runTest() override
    {
        beginTest ("take() returns the published object exactly once");
        {
            ampsim::Handoff<Tracked> handoff;
            handoff.publish (std::make_unique<Tracked> (1));
            auto* got = handoff.take();
            expect (got != nullptr && got->id == 1);
            expect (handoff.take() == nullptr, "a second take() must come back empty");
            expect (handoff.retire (got));
            handoff.collect();
            expectEquals (Tracked::alive.load(), 0);
        }

        beginTest ("an unclaimed object is freed when a newer one is published");
        {
            ampsim::Handoff<Tracked> handoff;
            handoff.publish (std::make_unique<Tracked> (1));
            handoff.publish (std::make_unique<Tracked> (2));
            expectEquals (Tracked::alive.load(), 1, "object 1 should have been freed by the second publish");
            auto* got = handoff.take();
            expectEquals (got->id, 2);
            delete got;
        }

        beginTest ("the return slot holds one object at a time and collect() frees it");
        {
            ampsim::Handoff<Tracked> handoff;
            auto* a = new Tracked (1);
            auto* b = new Tracked (2);
            expect (handoff.retire (a));
            expect (! handoff.retire (b), "the slot is full, so retire must fail and the caller keeps b");
            handoff.collect();
            expectEquals (Tracked::alive.load(), 1);
            expect (handoff.retire (b));
            handoff.collect();
            expectEquals (Tracked::alive.load(), 0);
        }

        beginTest ("stress: three threads, 20,000 objects, each freed exactly once and never out of order");
        {
            constexpr int numObjects = 20000;
            int taken = 0, lastId = -1;
            bool inOrder = true;

            {
                ampsim::Handoff<Tracked> handoff;
                std::atomic<bool> producerDone { false }, stop { false };

                std::thread loader ([&]
                {
                    for (int i = 0; i < numObjects; ++i)
                        handoff.publish (std::make_unique<Tracked> (i));
                    producerDone = true;
                });

                std::thread collector ([&]
                {
                    while (! stop)
                    {
                        handoff.collect();
                        std::this_thread::yield();
                    }
                });

                // This thread plays the audio thread: take, hold, retire (retrying when the slot is full).
                Tracked* held = nullptr;

                for (;;)
                {
                    if (held != nullptr && handoff.retire (held))
                        held = nullptr;

                    if (held != nullptr)
                        continue;

                    const bool finished = producerDone.load(); // read before take() so the last publish isn't missed

                    if (auto* next = handoff.take())
                    {
                        inOrder = inOrder && next->id > lastId;
                        lastId = next->id;
                        ++taken;
                        held = next;
                    }
                    else if (finished)
                    {
                        break;
                    }
                }

                stop = true;
                loader.join();
                collector.join();
                handoff.collect();
            } // the Handoff's destructor frees anything left in its slots

            expectEquals (Tracked::alive.load(), 0, "objects leaked or freed twice");
            expect (inOrder, "the audio thread saw an older object after a newer one");
            expectEquals (lastId, numObjects - 1, "the final object must always arrive");

            logMessage ("  -> " + juce::String (numObjects) + " objects published; " + juce::String (taken)
                        + " reached the audio thread, " + juce::String (numObjects - taken)
                        + " were superseded before pickup and freed on the loader side; leaks: "
                        + juce::String (Tracked::alive.load()));
        }
    }
};

HandoffTests handoffTests;
} // namespace

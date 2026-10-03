// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "NoteTracker.h"

#include <algorithm>
#include <cmath>

namespace ampsim
{

double NoteTracker::midiFromFrequency (double frequency, double referenceA4) noexcept
{
    return 69.0 + 12.0 * std::log2 (frequency / referenceA4);
}

void NoteTracker::prepare (double hopSeconds, const Settings& newSettings)
{
    hop = hopSeconds;
    settings = newSettings;
    reset();
}

void NoteTracker::reset() noexcept
{
    time = 0.0;
    lastConfident = -1.0e9;
    note = -1;
    pitch = 0.0;
    lastChange = Change::none;
    historyCount = historyNext = 0;
    candidate = false;
    settleNote = -1;
}

NoteTracker::Change NoteTracker::setNote (int newNote, Change why) noexcept
{
    note = newNote;
    ++changes;
    lastChange = why;
    settleNote = -1;
    candidate = false;
    return why;
}

NoteTracker::Change NoteTracker::update (double frequency, double clarity) noexcept
{
    time += hop;
    constexpr double eps = 1.0e-9; // so "for 30 ms" counts the hop that reaches 30 ms

    if (! (frequency > 0.0 && clarity >= settings.confidence))
    {
        candidate = false; // an onset needs two confident estimates in a row
        if (note >= 0 && time - lastConfident >= settings.loseSeconds - eps)
        {
            historyCount = 0;
            return setNote (-1, Change::release);
        }
        return Change::none;
    }

    const auto p = midiFromFrequency (frequency, settings.referenceA4);
    lastConfident = time;
    pitch = p;

    // How far the pitch has moved within the jump window: the largest distance from p to any confident
    // estimate in it.
    double moved = 0.0;
    for (int i = 0; i < historyCount; ++i)
    {
        const auto& e = history[(size_t) ((historyNext + historySize - 1 - i) % historySize)];
        if (time - e.time > settings.jumpWindowSeconds + eps)
            break;
        moved = std::max (moved, std::abs (p - e.pitch));
    }

    history[(size_t) historyNext] = { time, p };
    historyNext = (historyNext + 1) % historySize;
    historyCount = std::min (historyCount + 1, historySize);

    // Landed: the newest steadyCount estimates, on consecutive hops, within steadyCents of each other.
    const auto steadyCount = std::clamp (settings.steadyCount, 2, historySize);
    auto landed = historyCount >= steadyCount;
    double lowest = p, highest = p;
    for (int i = 1; landed && i < steadyCount; ++i)
    {
        const auto& newer = history[(size_t) ((historyNext + historySize - i) % historySize)];
        const auto& e = history[(size_t) ((historyNext + historySize - 1 - i) % historySize)];
        landed = newer.time - e.time <= hop + eps;
        lowest = std::min (lowest, e.pitch);
        highest = std::max (highest, e.pitch);
    }
    landed = landed && (highest - lowest) * 100.0 <= settings.steadyCents;

    const auto nearest = (int) std::lround (p);

    if (note < 0)
    {
        if (candidate && std::abs (p - candidatePitch) * 100.0 <= settings.agreeCents)
            return setNote (nearest, Change::onset);
        candidate = true;
        candidatePitch = p;
        return Change::none;
    }

    // A discrete jump that has landed on another semitone: re-evaluate now.
    if (landed && nearest != note && moved * 100.0 > settings.jumpCents)
        return setNote (nearest, Change::jump);

    // A slide (or a held bend): re-evaluate once it has held the new semitone long enough.
    if (nearest != note && std::abs (p - nearest) * 100.0 <= settings.settleCents)
    {
        if (nearest != settleNote)
        {
            settleNote = nearest;
            settleStart = time;
        }
        else if (time - settleStart >= settings.settleSeconds - eps)
        {
            return setNote (nearest, Change::slide);
        }
    }
    else
    {
        settleNote = -1;
    }

    return Change::none;
}

} // namespace ampsim

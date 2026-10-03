// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Gate.h"

namespace ampsim
{

/// Gate B as the chain runs it (BUILD_PLAN "Gates"): fixed between the amp and the cab, linked to Gate A
/// by default, so one gating decision is applied at both points. Two independent releases in series
/// would compound and chop note tails; linked, the amp's output is gated by exactly the gain that gated
/// its input (Gate.h, "Linking").
///
/// Linked, it applies Gate A's curve for this buffer; unlinked, it's a gate of its own with its own
/// settings and detector. Switching between the two fades from the last gain applied over 10 ms (Gate.h),
/// so the link switch never clicks. Gate A is a plain Gate in the pre section that the chain keeps
/// running while it's switched off (Chain.h, BypassState::keepRunning), so its curve is always this
/// buffer's. A normal chain block otherwise: bypass crossfades, and a fully bypassed Gate B is skipped.
class GateB final : public Block
{
public:
    explicit GateB (const Gate& leaderToFollow) : leader (leaderToFollow) {}

    Gate gate;

    /// Audio thread, once per buffer.
    void setLinked (bool shouldBeLinked) noexcept { linked = shouldBeLinked; }
    bool isLinked() const noexcept { return linked; }

    void prepare (double sampleRate, int maxBlockSize) override { gate.prepare (sampleRate, maxBlockSize); }
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override { gate.reset(); }

private:
    const Gate& leader;
    bool linked = true;
};

} // namespace ampsim

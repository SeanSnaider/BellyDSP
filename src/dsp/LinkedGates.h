#pragma once

#include "Gate.h"

#include <vector>

namespace ampsim
{

/// The two noise gates as the chain runs them (BUILD_PLAN "Gates"): Gate A in the pre FX section and
/// Gate B fixed between the amp and the cab, linked by default, so one gating decision is applied at
/// both points. Two independent releases in series would compound and chop note tails; linked, the amp's
/// output is gated by exactly the gain that gated its input (Gate.h, "Linking").
///
/// Gate A keeps detecting while it's switched off. It takes over its own bypass (the Block hook the
/// spillover blocks use) and, while bypassed, runs the gate on a copy of its input and leaves the audio
/// alone, so its curve is always current for a linked Gate B, Learn works with it off, and switching it
/// on starts from a live envelope instead of a stale one. That costs about a microsecond a buffer.
/// Switching it on or off crossfades over 10 ms, linearly like the chain's own bypass, because the two
/// signals are the same guitar with and without the gate's gain.
class GateA final : public Block
{
public:
    static constexpr double bypassFadeSeconds = 0.010;

    Gate gate;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;

    bool handlesOwnBypass() const override { return true; }
    void setBypassed (bool shouldBeBypassed) override;
    bool isBypassed() const noexcept { return bypassed; }

    /// Fully off: the fade has finished and the audio passes untouched.
    bool isFullyBypassed() const noexcept { return bypassed && ! wet.isSmoothing(); }

private:
    std::vector<float> scratch;
    juce::SmoothedValue<float> wet { 0.0f }; // 1 = gated, 0 = the input
    bool bypassed = true;                     // effects start switched off
};

/// Gate B: linked, it applies Gate A's curve for this buffer to the amp's output; unlinked, it's a gate
/// of its own with its own settings and detector. Switching between the two fades from the last gain
/// applied over 10 ms (Gate.h), so the link switch never clicks. A normal chain block otherwise: bypass
/// crossfades and a fully bypassed Gate B is skipped.
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

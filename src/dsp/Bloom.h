#pragma once

#include "Bitcrush.h"
#include "Block.h"
#include "DelayLine.h"
#include "Flanger.h"
#include "Phaser.h"

#include <array>
#include <atomic>
#include <vector>

namespace ampsim
{

/// Bloom, the modulation container (BUILD_PLAN "Bloom (modulation container)", design review round 12): a
/// stereo post-FX block holding the bitcrusher, phaser, and flanger (Bitcrush.h, Phaser.h, Flanger.h), each
/// with its own on/off, in an order that can change (default bitcrush, phaser, flanger), plus a container mix
/// and bypass.
///
/// Signal flow, per channel:
///     effects  = the three effects in order, run on v x, each fading its own mix in or out over 10 ms when
///                switched
///     out      = z ((1 - g) dry + g effects),   g = mix x on x s,   v = s x z
/// where dry is the container's input, delayed to match the latency (below), and these fade (on, s, and z
/// over 10 ms along an S-curve, Fade.h, so no fade has a corner at either end):
///   mix   the container mix, linear: every effect's output is its input filtered, quantized, or combed, so
///         it's correlated with the dry, and a linear blend holds the level where equal power would bump it up
///         to 3 dB mid-way. Mix 0 is the dry exactly (bit for bit); 1 (the default) is the effects alone.
///   on    the container's bypass, a fade of the output only. Bloom takes over its own bypass
///         (handlesOwnBypass) and keeps its effects running while bypassed (about 0.6% of the deadline with all
///         three on, next to nothing with them off), so switching it back on is a pure crossfade: no delay line
///         or filter restarts from stale or empty state, and the latency never changes with bypass.
///   s     the reorder dip: a new order fades the effects to the dry over 10 ms, swaps at the bottom, and fades
///         back, as the chain's sections do. It fades every effect's input too (v), not just the first's: an
///         effect whose input jumped at the swap would store the splice (the flanger in its delay line, the
///         phaser in its integrators) and play it back milliseconds later, once the wet is fading back in, and
///         an effect with memory keeps playing its tail after its own input has gone, so only scaling every
///         input makes each one exactly silent at the swap. During the 20 ms the wet dips deeper than the dry
///         rises (about 5 dB down mid-fade); nothing jumps. No allocation: the order is a three-element array.
///   z     the latency dip: everything, every effect's input included, fades to silence over 10 ms and back
///         around a through-zero switch.
///
/// Latency. The flanger's through-zero mode delays its dry path by 5 ms (240 samples at 48 kHz) so its wet can
/// sweep through it; the whole container then runs 240 samples late, its own dry included, and
/// latencySamples() reports it: the project's one latency exception. It depends only on the through-zero
/// setting, not on whether the flanger or the container is on, so footswitching them never changes the
/// latency (switched off, the flanger keeps delaying its input). Turning the mode on or off moves the whole
/// output by 5 ms, which would click, so Bloom dips to silence for 10 ms, switches at the bottom, and fades
/// back (20 ms in all, even when bypassed). latencySamples() changes at the bottom of the dip, readable from
/// any thread; the owner tells the host (setLatencySamples) when it changes.
///
/// Every setting arrives through setSettings() once per buffer; LFO rates in Hz (tempo sync is the
/// processor's job).
class Bloom : public Block
{
public:
    enum class Effect
    {
        bitcrush,
        phaser,
        flanger
    };

    static constexpr int numEffects = 3;
    using Order = std::array<Effect, numEffects>;
    static constexpr Order defaultOrder { Effect::bitcrush, Effect::phaser, Effect::flanger };

    static constexpr double smoothingSeconds = 0.020;
    static constexpr double bypassFadeSeconds = 0.010;
    static constexpr double reorderFadeSeconds = 0.010;
    static constexpr double latencyFadeSeconds = 0.010;

    struct Settings
    {
        Bitcrush::Settings bitcrush; // each effect's own on/off is in its settings
        Phaser::Settings phaser;
        Flanger::Settings flanger;   // flanger.throughZero sets the container's latency
        Order order = defaultOrder;  // a permutation of the three; anything else is ignored
        float mix = 1.0f;            // 0 dry to 1 the effects
    };

    static bool isValidOrder (const Order& order) noexcept;
    static const char* effectName (Effect effect) noexcept;

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    int latencySamples() const override { return latency.load (std::memory_order_acquire); }
    bool isStereo() const override { return true; }
    bool handlesOwnBypass() const override { return true; }
    void setBypassed (bool shouldBeBypassed) override;

    /// The thread that calls process(), for tests and the GUI: the order in effect, and what's fading.
    Order getAppliedOrder() const noexcept { return applied; }
    bool isReordering() const noexcept { return reorderPending || sectionGain.isSmoothing(); }
    bool isSwitchingLatency() const noexcept { return latencyPending || latencyGain.isSmoothing(); }
    bool isFullyBypassed() const noexcept { return bypassed && ! bypassGain.isSmoothing(); }

    const Bitcrush& getBitcrush() const noexcept { return bitcrush; }
    const Phaser& getPhaser() const noexcept { return phaser; }
    const Flanger& getFlanger() const noexcept { return flanger; }

private:
    Block& unit (Effect effect) noexcept;
    void applyRequests();

    Bitcrush bitcrush;
    Phaser phaser;
    Flanger flanger;

    double sampleRate = 48000.0;
    Settings settings;
    Order applied = defaultOrder, requested = defaultOrder;
    bool reorderPending = false, latencyPending = false;
    bool bypassed = false;
    bool throughZero = false; // the applied state
    int throughZeroSamples = 240;
    std::atomic<int> latency { 0 };

    juce::SmoothedValue<double> mix { 1.0 }, bypassGain { 1.0 }, sectionGain { 1.0 }, latencyGain { 1.0 };
    juce::AudioBuffer<float> input;    // the block's input, kept for the dry path
    std::vector<double> inputGains, wetGains, outputGains; // v, g, z for each sample of the buffer
    std::array<DelayLine, 2> dryLines; // the dry, delayed in through-zero mode
};

} // namespace ampsim

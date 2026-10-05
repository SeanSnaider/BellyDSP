// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Block.h"
#include "Handoff.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace ampsim
{

/// Tone match's A/B player (docs/TONE_MATCH.md, "Comparing"): plays one of three prepared sources, looped,
/// at the very end of the chain, after the output level and before the output limiter. Stereo (the mono
/// source in both channels), zero latency.
///
///   source 0  the target: the separated guitar stem, or the song section when separation is off
///   source 1  the match: the player's DI through the matched settings
///   source 2  the current settings: the same DI through what's set now
///
/// The audio lives in a Material, built and filled off the audio thread and handed over through a Handoff
/// (the old one is handed back, once no voice reads it, to be freed elsewhere). Everything else (play,
/// the source, the loops, the levels, the live mute) is atomics the audio thread reads once per buffer, so
/// process() never allocates, frees, or locks.
///
/// Voices. Each sounding stream is a voice: a source, a position, and a fade phase phi in [0, 1] that moves
/// by 1 / F per sample (F = 20 ms) toward 1 (fading in) or 0 (fading out). Its gain is sin(phi pi / 2).
/// A switch fades the old voice out and starts the new one at the corresponding position: the two gains
/// are sin and cos of the same angle, so their squares sum to 1 (an equal-power crossfade, right for two
/// different, uncorrelated signals: the total power holds). A fade reversed midway turns around from where
/// it is, so nothing ever jumps. The loop is the same thing in time: F before the loop's end a voice fades
/// out (reaching the end exactly as its gain reaches 0) while a new voice of the same source fades in from
/// the loop's start. Up to four voices sound at once; a fifth takes the place of the quietest.
///
/// Positions. Sources keep their own clocks: clock 0 is the target's, clock 1 the DI's (the match and the
/// current renders share it, so switching between them keeps the exact sample). Between the clocks, an
/// aligned Material (tone match's same-part mode) maps a position through the DTW path the matcher found:
/// two monotone tables, the other clock's position at every hop of this one, interpolated linearly. An
/// unaligned one (anything mode, nothing corresponds) keeps the offset into the loop instead, modulo the
/// other loop's length. Each clock has its own loop range.
///
/// The live guitar: while previewing with the mute on, the chain's own output fades to silence over 20 ms
/// (and back after). Idle (not playing, every voice silent, the live gain back at exactly 1), process()
/// returns without touching the audio, so the live path is bit-identical to a chain without the player.
class PreviewPlayer final : public Block
{
public:
    static constexpr int numSources = 3;
    static constexpr int maxVoices = 4;
    static constexpr double crossfadeSeconds = 0.020; // a source switch, a start, a stop, a loop wrap
    static constexpr double liveFadeSeconds = 0.020;  // the live guitar's mute
    static constexpr double gainSeconds = 0.050;      // the preview level and the sources' level-match gains
    static constexpr double minLoopSeconds = 0.5;

    /// Everything the player plays, immutable once published.
    struct Material
    {
        std::array<std::shared_ptr<const std::vector<float>>, numSources> audio; ///< 48 kHz mono; null or empty: silence
        std::array<int, numSources> clock { 0, 1, 1 };                            ///< the target's clock, the DI's

        /// Aligned (same part): sample positions on the other clock at every `hop` samples of this one,
        /// starting at `offset` (positions before the first or after the last entry are clamped to them).
        bool aligned = false;
        double hop = 2048.0, offset = 0.0;
        std::vector<double> diAtTarget, targetAtDi;
    };

    PreviewPlayer();
    ~PreviewPlayer() override;

    // ---- Message thread (or any non-audio thread) ------------------------------------------------------
    /// Hands new material to the audio thread. Playing voices crossfade to it at the same positions.
    void setMaterial (std::unique_ptr<Material> material);
    /// Frees material the audio thread has handed back.
    void collectGarbage() { handoff.collect(); }

    void setPlaying (bool shouldPlay) noexcept { playing.store (shouldPlay); }
    bool isPlaying() const noexcept { return playing.load(); }
    void setSource (int index) noexcept;
    int getSource() const noexcept { return source.load(); }
    /// A clock's loop, in samples of that clock (end exclusive). Kept at least minLoopSeconds long.
    void setLoop (int clock, int64_t start, int64_t end) noexcept;
    void setLevelDb (float db) noexcept { levelDb.store (db); }
    void setSourceGainDb (int index, float db) noexcept;
    void setMuteLive (bool shouldMute) noexcept { muteLive.store (shouldMute); }

    /// Any thread: whether anything is sounding or fading (false: the player is idle and transparent).
    bool isActive() const noexcept { return active.load (std::memory_order_relaxed); }
    /// Any thread: where the loudest voice is, as a fraction of its loop (for the playhead), and its source.
    float getPlayheadFraction() const noexcept { return playhead.load (std::memory_order_relaxed); }
    int getSoundingSource() const noexcept { return soundingSource.load (std::memory_order_relaxed); }

    /// Pure, any thread: an aligned Material's mapping from a position on one clock to the other (clock 0,
    /// the target's, to the DI's, or back). Unaligned, or with no table, the position unchanged.
    static double alignedPosition (const Material& m, int fromClock, double position);

    // ---- Block ---------------------------------------------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }

private:
    struct Voice
    {
        const Material* material = nullptr;
        int source = 0;
        int64_t position = 0;
        double phase = 0.0;   // 0 silent .. 1 full; gain = sin(phase pi / 2)
        int direction = 0;    // +1 fading in, -1 fading out, 0 steady
        bool active = false;
    };

    int spawn (const Material* material, int sourceIndex, int64_t position, double startPhase) noexcept;
    int64_t convert (const Voice& from, int toSource, const Material* toMaterial) const noexcept;
    int64_t intoLoop (int clock, int64_t position) const noexcept;
    void fadeOut (Voice& v) noexcept { v.direction = -1; }
    bool usesMaterial (const Material* m) const noexcept;

    Handoff<Material> handoff;
    Material* current = nullptr;  // audio thread (freed by the destructor or handed back)
    Material* previous = nullptr; // audio thread: still read by fading voices until they finish

    std::atomic<bool> playing { false }, muteLive { true };
    std::atomic<int> source { 0 };
    std::array<std::atomic<int64_t>, 2> loopStart {}, loopEnd {};
    std::atomic<float> levelDb { 0.0f };
    std::array<std::atomic<float>, numSources> sourceGainDb {};
    std::atomic<bool> active { false };
    std::atomic<float> playhead { 0.0f };
    std::atomic<int> soundingSource { -1 };

    // Audio thread.
    double sampleRate = 48000.0;
    int fadeSamples = 960;
    double phaseStep = 1.0 / 960.0;
    std::array<Voice, maxVoices> voices;
    int lead = -1; // the voice the next switch maps from (the newest one not fading out)
    bool wasPlaying = false;
    int lastSource = 0;
    std::array<int64_t, 2> loopA {}, loopB {}; // this buffer's loops
    juce::SmoothedValue<float> liveGain { 1.0f }, level { 1.0f };
    std::array<juce::SmoothedValue<float>, numSources> sourceGain;
};

} // namespace ampsim

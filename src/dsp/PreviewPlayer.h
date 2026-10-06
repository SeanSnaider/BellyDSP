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

/// Tone match's player (docs/TONE_MATCH.md, "Comparing" and "Play along"): plays one of five prepared
/// sources at the very end of the chain, after the output level and before the output limiter. Stereo (the
/// mono source in both channels), zero latency.
///
///   source 0  the target: the separated guitar stem, or the song section when separation is off (A/B)
///   source 1  the match: the player's DI through the matched settings (A/B)
///   source 2  the current settings: the same DI through what's set now (A/B)
///   source 3  the song: the selected section of the target file (the Target card's Play, play along)
///   source 4  the guitar only: the separated stem of that section, once a separated match has made one
///
/// The audio lives in a Material, built and filled off the audio thread and handed over through a Handoff
/// (the old one is handed back, once no voice reads it, to be freed elsewhere). Everything else (play,
/// the source, the loops, the levels, the live mute, once, the count-in) is atomics the audio thread reads
/// once per buffer, so process() never allocates, frees, or locks. A start waits for a buffer while new
/// material is still on its way, so it always starts on the material handed over with it.
///
/// Voices. Each sounding stream is a voice: a source, a position, and a fade phase phi in [0, 1] that moves
/// by 1 / F per sample (F = 20 ms) toward 1 (fading in) or 0 (fading out). Its gain is sin(phi pi / 2).
/// A switch fades the old voice out and starts the new one at the corresponding position: the two gains
/// are sin and cos of the same angle, so their squares sum to 1 (an equal-power crossfade, right for two
/// different, uncorrelated signals: the total power holds). A fade reversed midway turns around from where
/// it is, so nothing ever jumps. The loop is the same thing in time: F before the loop's end a voice fades
/// out (reaching the end exactly as its gain reaches 0) while a new voice of the same source fades in from
/// the loop's start. Up to four voices sound at once; a fifth takes the place of the quietest. Once (play
/// along): the loop's end fades the voice out the same way and nothing follows; hasFinished() turns true.
///
/// The count-in: with N beats of B samples set (B = 60 fs / BPM, not necessarily whole), a start plays
/// a click at round(k B) samples after the start for k = 0 .. N-1 (an accented one first) and the source's
/// first sample at round(N B): one beat after the last click, to the sample. The source's voice is
/// spawned at the start with that delay, so the moment is the audio thread's own count, not a timer's.
/// The block in which a start's first sample sounds reports its index (songStartedAt()), so the processor
/// can start tone match's DI recorder on the same sample.
///
/// Positions. Sources keep their own clocks: clock 0 is the target's, clock 1 the DI's (the match and the
/// current renders share it, so switching between them keeps the exact sample), clock 2 the song section's
/// (the song and its stem share it). Between clocks 0 and 1, an aligned Material (tone match's same-part
/// mode) maps a position through the DTW path the matcher found: two monotone tables, the other clock's
/// position at every hop of this one, interpolated linearly. Otherwise (anything mode, nothing corresponds)
/// the offset into the loop is kept instead, modulo the other loop's length. Each clock has its own loop.
///
/// The live guitar: while previewing with the mute on, the chain's own output fades to silence over 20 ms
/// (and back after). Idle (not playing, every voice and the click silent, the live gain back at exactly 1),
/// process() returns without touching the audio, so the live path is bit-identical to a chain without the
/// player. With the mute off, the output is the live path plus the player's sum.
class PreviewPlayer final : public Block
{
public:
    static constexpr int numSources = 5;
    static constexpr int numClocks = 3;
    static constexpr int maxVoices = 4;
    static constexpr int maxCountInBeats = 8;
    static constexpr double crossfadeSeconds = 0.020; // a source switch, a start, a stop, a loop wrap
    static constexpr double liveFadeSeconds = 0.020;  // the live guitar's mute
    static constexpr double gainSeconds = 0.050;      // the preview level and the sources' level-match gains
    static constexpr double minLoopSeconds = 0.5;

    /// Everything the player plays, immutable once published.
    struct Material
    {
        std::array<std::shared_ptr<const std::vector<float>>, numSources> audio; ///< 48 kHz mono; null or empty: silence
        std::array<int, numSources> clock { 0, 1, 1, 2, 2 };                      ///< the target's clock, the DI's, the song's
        /// The count-in's clicks: the first beat's (accented) and the others'. Null: silent clicks.
        std::shared_ptr<const std::vector<float>> clickAccent, click;

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

    /// Released, so whatever was set before it (the recorder armed, the count-in) is seen with it.
    void setPlaying (bool shouldPlay) noexcept { playing.store (shouldPlay, std::memory_order_release); }
    bool isPlaying() const noexcept { return playing.load(); }
    /// Plays from the start (the loop's start, after the count-in), even if it's playing already: what's
    /// sounding fades out as the new start begins. Set everything else first.
    void startFresh() noexcept
    {
        startSerial.fetch_add (1, std::memory_order_release);
        setPlaying (true);
    }
    void setSource (int index) noexcept;
    int getSource() const noexcept { return source.load(); }
    /// A clock's loop, in samples of that clock (end exclusive). Kept at least minLoopSeconds long.
    void setLoop (int clock, int64_t start, int64_t end) noexcept;
    void setLevelDb (float db) noexcept { levelDb.store (db); }
    void setSourceGainDb (int index, float db) noexcept;
    void setMuteLive (bool shouldMute) noexcept { muteLive.store (shouldMute); }
    /// Play once from the loop's start to its end (play along), instead of looping.
    void setOnce (bool shouldPlayOnce) noexcept { playOnce.store (shouldPlayOnce); }
    /// The count-in the next start plays: beats (0: none, at most maxCountInBeats) of beatSamples each.
    void setCountIn (int beats, double beatSamples) noexcept;
    void setClickLevelDb (float db) noexcept { clickLevelDb.store (db); }

    /// Any thread: whether anything is sounding or fading (false: the player is idle and transparent).
    bool isActive() const noexcept { return active.load (std::memory_order_relaxed); }
    /// Any thread: where the loudest voice is, as a fraction of its loop (for the playhead), and its source.
    float getPlayheadFraction() const noexcept { return playhead.load (std::memory_order_relaxed); }
    int getSoundingSource() const noexcept { return soundingSource.load (std::memory_order_relaxed); }
    /// Any thread: the loudest voice's position on its clock (samples), or -1 when nothing plays.
    int64_t getPlayheadPosition() const noexcept { return playheadPosition.load (std::memory_order_relaxed); }
    /// Any thread: the beats left in the count-in, counting the one sounding now (N at the first click, 1 at
    /// the last), 0 when not counting in.
    int getCountInRemaining() const noexcept { return countInShown.load (std::memory_order_relaxed); }
    /// Any thread: true once a play-once start has reached its end (false again at the next start).
    bool hasFinished() const noexcept { return finished.load (std::memory_order_acquire); }

    /// Audio thread, after process(): the index in the block just processed where a start's first sample
    /// sounded (after its count-in), or -1 if none did.
    int songStartedAt() const noexcept { return songStartIndex; }

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
        int64_t delay = 0;    // samples before it starts (the count-in): silent and still until then
        bool fresh = false;   // a start's voice that hasn't sounded yet (its first sample is reported)
    };

    int spawn (const Material* material, int sourceIndex, int64_t position, double startPhase, int64_t delay = 0) noexcept;
    int64_t convert (const Voice& from, int toSource, const Material* toMaterial) const noexcept;
    int64_t intoLoop (int clock, int64_t position) const noexcept;
    void fadeOut (Voice& v) noexcept
    {
        if (v.delay > 0)
            v.active = false; // it never sounded: nothing to fade
        else
            v.direction = -1;
    }
    bool usesMaterial (const Material* m) const noexcept;

    Handoff<Material> handoff;
    Material* current = nullptr;  // audio thread (freed by the destructor or handed back)
    Material* previous = nullptr; // audio thread: still read by fading voices until they finish

    std::atomic<bool> playing { false }, muteLive { true }, playOnce { false };
    std::atomic<int> source { 0 };
    std::array<std::atomic<int64_t>, numClocks> loopStart {}, loopEnd {};
    std::atomic<float> levelDb { 0.0f }, clickLevelDb { 0.0f };
    std::array<std::atomic<float>, numSources> sourceGainDb {};
    std::atomic<int> countInBeats { 0 };
    std::atomic<uint32_t> startSerial { 0 };
    std::atomic<double> countInBeatSamples { 24000.0 };
    std::atomic<bool> active { false }, finished { false };
    std::atomic<float> playhead { 0.0f };
    std::atomic<int> soundingSource { -1 }, countInShown { 0 };
    std::atomic<int64_t> playheadPosition { -1 };
    static_assert (std::atomic<double>::is_always_lock_free && std::atomic<int64_t>::is_always_lock_free);

    // Audio thread.
    double sampleRate = 48000.0;
    int fadeSamples = 960;
    double phaseStep = 1.0 / 960.0;
    std::array<Voice, maxVoices> voices;
    int lead = -1; // the voice the next switch maps from (the newest one not fading out)
    bool wasPlaying = false, ended = false, onceNow = false;
    int lastSource = 0;
    std::array<int64_t, numClocks> loopA {}, loopB {}; // this buffer's loops
    int songStartIndex = -1;
    uint32_t seenStartSerial = 0;
    // The count-in: samples since the start, the next click, when the source starts; the click sounding.
    struct CountIn
    {
        bool active = false;
        int beats = 0, next = 0;
        double beat = 24000.0;
        int64_t sample = 0, songAt = 0;
    } countIn;
    const Material* clickMaterial = nullptr; // the material the sounding click reads (kept until it ends)
    const std::vector<float>* clickBuffer = nullptr;
    size_t clickPosition = 0;
    juce::SmoothedValue<float> liveGain { 1.0f }, level { 1.0f }, clickLevel { 1.0f };
    std::array<juce::SmoothedValue<float>, numSources> sourceGain;
};

} // namespace ampsim

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PluginProcessor.h"
#include "tonematch/ToneMatcher.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/// Tone match in the app (docs/TONE_MATCH.md): what the tone match page drives. It holds the target (a
/// file and the range chosen in it), the reference (the player's DI, recorded through the processor's
/// DiRecorder or read from a file), the mode, and the result; runs the match (and, if asked, the guitar
/// separation first) on its own worker thread, never the audio thread and never the live processor;
/// and applies a result as ordinary parameter changes plus the cab, in one undo step.
///
/// Message thread, except the worker, which only reads the inputs copied for it and hands its result
/// back through `finished` (picked up by poll()).
class ToneMatchSession
{
public:
    using Mode = ampsim::tonematch::Mode;
    using MatchResult = ampsim::tonematch::MatchResult;

    explicit ToneMatchSession (AmpSimProcessor& processor);
    ~ToneMatchSession(); // cancels a running match and waits for it

    static constexpr double maxRangeSeconds = 60.0, minRangeSeconds = 3.0, defaultRangeSeconds = 30.0;
    static constexpr double maxFileSeconds = 20.0 * 60.0; // a target file is read up to 20 minutes
    static constexpr double minReferenceSeconds = 3.0;

    // ---- The target ------------------------------------------------------------------------------
    /// Reads a file (any format AudioFileInput decodes) and selects its first 30 s. False (and getError())
    /// if it can't be read.
    bool setTargetFile (const juce::File& file);
    void setTargetSignal (std::vector<float> samples48k, const juce::String& name);
    const std::vector<float>& getTarget() const noexcept { return target; }
    juce::String getTargetName() const { return targetName; }
    double getTargetSeconds() const noexcept { return (double) target.size() / 48000.0; }
    /// Goes up every time the target changes (the page's waveform redraws its peaks then).
    int getTargetVersion() const noexcept { return targetVersion; }

    /// The range of the target to match, in seconds: clamped to the file, and to 3 .. 60 s long.
    void setRange (double startSeconds, double endSeconds);
    std::pair<double, double> getRange() const noexcept { return { rangeStart, rangeEnd }; }
    std::vector<float> targetSelection() const;

    // ---- The reference (the player's DI) ---------------------------------------------------------------
    void startRecording();
    void stopRecording();
    bool isRecording() const;
    bool setReferenceFile (const juce::File& file);
    void setReferenceSignal (std::vector<float> samples48k, const juce::String& name);
    const std::vector<float>& getReference() const noexcept { return reference; }
    juce::String getReferenceName() const { return referenceName; }
    double getReferenceSeconds() const noexcept { return (double) reference.size() / 48000.0; }

    void setMode (Mode m) { mode = m; }
    Mode getMode() const noexcept { return mode; }

    /// Separate the guitar out of the target first (Demucs; Stage C). Off: the target is used as it is.
    void setSeparate (bool shouldSeparate) { separate = shouldSeparate; }
    bool getSeparate() const noexcept { return separate; }

    /// The separator, set by the app when it's available (src/tonematch/GuitarSeparator.h). Runs on the
    /// worker thread: 48 kHz mono in, the guitar stem out (empty: failed or cancelled; error in the string).
    using SeparateFn = std::function<std::vector<float> (const std::vector<float>&, const std::atomic<bool>& cancel,
                                                         const ampsim::tonematch::ProgressFn&, juce::String& error)>;
    void setSeparator (SeparateFn fn) { separator = std::move (fn); }
    bool hasSeparator() const { return separator != nullptr; }

    // ---- Matching ----------------------------------------------------------------------------------------
    /// Empty when a match can start; otherwise what's missing, in words for the page.
    juce::String whyCantMatch() const;
    bool startMatch();
    void cancel();
    bool isMatching() const noexcept { return running.load(); }
    double getProgress() const noexcept { return progress.load(); }
    juce::String getStage() const;

    /// Message thread, a few times a second (the page's timer): drains the recorder and picks up a
    /// finished match.
    void poll();

    bool hasResult() const noexcept { return resultReady; }
    const MatchResult& getResult() const noexcept { return result; }
    juce::String getError() const { return error; }

    /// The result as settings: amp_slot, that slot's Gain and tone, the post EQ (on, parametric, the five
    /// bands, its cuts off), close mic 1's cab with close mic 2 and the room muted and the cab's cuts off,
    /// the amp, cab, and post section on; and the pre effects that color the tone before the amp switched
    /// off (preEffectsApplyTurnsOff; the noise gate is left as it is: ASSUMPTIONS TM19). One undo step (the
    /// cab load included). False without a result.
    bool apply();

    /// The pre effects Apply switches off: their on/off parameter IDs and names, in chain order.
    struct PreEffect
    {
        const char* parameterId;
        const char* name;
    };
    static const std::vector<PreEffect>& preEffectsApplyTurnsOff();
    /// The names of those that are on right now ("boost, overdrive"), for the page.
    juce::StringArray preEffectsOnNow() const;
    void discard();

    /// The cab files the search chooses from: the built-in IRs, sorted by path (as the prototype sorts them).
    static std::vector<juce::File> builtInCabs();

    /// For tests: let the worker finish (or give up after timeoutMs). True if it finished.
    bool waitForMatch (int timeoutMs);

private:
    void join();

    AmpSimProcessor& ampSim;
    std::vector<float> target, reference;
    juce::String targetName, referenceName, error;
    double rangeStart = 0.0, rangeEnd = 0.0;
    int targetVersion = 0;
    Mode mode = Mode::anything;
    bool separate = false, recordingReference = false;
    SeparateFn separator;

    std::thread worker;
    std::atomic<bool> running { false }, cancelFlag { false }, finished { false };
    std::atomic<double> progress { 0.0 };
    mutable std::mutex lock; // worker <-> message thread only (never the audio thread)
    juce::String stage;
    MatchResult pending, result;
    bool resultReady = false;
};

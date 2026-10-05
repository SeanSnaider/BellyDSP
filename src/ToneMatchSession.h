// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PluginProcessor.h"
#include "tonematch/ToneMatcher.h"

#include <array>
#include <atomic>
#include <memory>
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

    // ---- Comparing (A/B: docs/TONE_MATCH.md, "Comparing") --------------------------------------------------
    // Three sources, looped by the processor's PreviewPlayer at the end of the chain: the target (what was
    // matched: the separated stem, or the song section), the match (the DI through what Apply would set),
    // and the current settings (the same DI through what's set now). The two renders run on their own
    // worker, through ToneMatcher::renderTone, never on the audio thread; they start by themselves when a
    // match finishes, and the current one again whenever the settings it depends on change.
    enum Source
    {
        sourceTarget = 0,
        sourceMatch = 1,
        sourceCurrent = 2,
        numSources = 3
    };
    static juce::String sourceName (int source);

    /// The settings the Match source plays: the result as Apply writes it (the values as the parameters
    /// store them), with the matched slot's Master as it is now. The cab IR is left empty (the worker reads it).
    ampsim::tonematch::ToneSettings matchedSettings() const;
    /// The settings the Current source plays: what's set now (the playing slot, close mic 1's IR as it
    /// plays, the post EQ if its section and itself are on).
    ampsim::tonematch::ToneSettings currentSettings() const;

    bool isRenderingCompare() const noexcept { return compareRunning.load(); }
    double getCompareProgress() const noexcept { return compareProgress.load(); }
    void cancelCompare();
    /// True when the sources are rendered and handed to the player.
    bool isCompareReady() const noexcept { return compareReady; }
    /// What the comparison is doing or why it can't (empty when it's ready and idle).
    juce::String getCompareStatus() const;
    /// The current settings changed since Current was rendered (it re-renders on its own after 300 ms still).
    bool isCurrentStale() const;

    void setPreviewPlaying (bool shouldPlay);
    bool isPreviewPlaying() const noexcept { return previewPlaying; }
    void setPreviewSource (int source);
    int getPreviewSource() const noexcept { return previewSource; }
    void setLevelMatch (bool on);
    bool getLevelMatch() const noexcept { return levelMatch; }
    void setMuteLive (bool on);
    bool getMuteLive() const noexcept { return muteLive; }
    void setPreviewLevelDb (float db);
    float getPreviewLevelDb() const noexcept { return previewLevelDb; }
    static constexpr float minPreviewLevelDb = -30.0f, maxPreviewLevelDb = 6.0f;

    /// The loop, in seconds of the matched section (0 is the section's start), 1 s or longer. The default is
    /// the whole section. In same-part mode the DI's loop follows it through the alignment; in anything
    /// mode the DI loops over its whole length, from its own start.
    void setLoop (double startSeconds, double endSeconds);
    std::pair<double, double> getLoop() const noexcept { return { loopStart, loopEnd }; }
    double getSectionSeconds() const noexcept { return comparedTarget ? (double) comparedTarget->size() / 48000.0 : 0.0; }
    static constexpr double minLoopSeconds = 1.0;
    /// Whether the DI-based sources are lined up with the target (same part, with an alignment).
    bool isAligned() const noexcept { return compareAligned; }

    /// A source's audio (48 kHz mono; empty until rendered), its BS.1770 loudness over its loop as it is
    /// (LUFS; -infinity for silence or nothing), and the gain the level match gives it (dB, 0 when off).
    const std::vector<float>& getCompareAudio (int source) const;
    double getLoudness (int source) const { return loudness[(size_t) source]; }
    float getLevelMatchGainDb (int source) const { return matchGainDb[(size_t) source]; }
    /// The DI's loop (in samples of the DI) for the current loop, as the player gets it.
    std::pair<int64_t, int64_t> getDiLoopSamples() const noexcept { return diLoop; }

    /// The long-term spectra of the target and the match render, dB in tone match's sixth-octave bands
    /// (ampsim::tonematch::bands()), empty until rendered.
    const std::vector<double>& getTargetSpectrum() const noexcept { return targetSpectrum; }
    const std::vector<double>& getMatchSpectrum() const noexcept { return matchSpectrum; }

    /// For tests: let the compare worker finish (or give up). True if nothing is rendering.
    bool waitForCompare (int timeoutMs);
    /// Starts the renders now (both, or only Current). Normally automatic. False if there's no result.
    bool startCompareRender (bool renderMatch);

private:
    void join();
    void joinCompare();
    void pollCompare();
    void stopAndClearCompare();
    void updateLoops();
    void updateLevels();
    juce::String currentFingerprint (const ampsim::tonematch::ToneSettings&) const;
    float storedValue (const juce::String& parameterId, double value) const;

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
    // What the finished match compared (the target after separation, and the DI), for the comparison.
    std::vector<float> pendingTarget, pendingReference;
    std::shared_ptr<const std::vector<float>> comparedTarget, comparedReference;

    // The comparison. The compare worker reads only what's copied for it and hands back through
    // `compareFinished` (picked up by poll()).
    struct CompareRender
    {
        bool ok = false, cancelled = false, renderedMatch = false;
        juce::String error, fingerprint;
        std::shared_ptr<const std::vector<float>> match, current;
        std::vector<double> targetSpectrum, matchSpectrum;
    };
    std::thread compareWorker;
    std::atomic<bool> compareRunning { false }, compareCancel { false }, compareFinished { false };
    std::atomic<double> compareProgress { 0.0 };
    CompareRender comparePending;
    juce::String compareStage, compareError;
    std::shared_ptr<const std::vector<float>> matchAudio, currentAudio;
    std::vector<double> targetSpectrum, matchSpectrum;
    juce::String renderedFingerprint, seenFingerprint;
    double fingerprintChangedMs = 0.0;
    bool compareReady = false, compareAligned = false, previewPlaying = false, levelMatch = true, muteLive = true;
    int previewSource = sourceTarget;
    float previewLevelDb = 0.0f;
    double loopStart = 0.0, loopEnd = 0.0;
    std::pair<int64_t, int64_t> diLoop { 0, 0 };
    std::array<double, numSources> loudness {};
    std::array<float, numSources> matchGainDb {};
    std::unique_ptr<ampsim::PreviewPlayer::Material> material; // a copy of what the player has (its tables), message thread
};

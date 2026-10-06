// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "PluginProcessor.h"
#include "dsp/Tempo.h"
#include "platform/DeviceLatency.h"
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
    /// Record without a target: the DI from the next buffer, up to a minute, until stopRecording().
    void startRecording();
    /// Stops a recording or a play-along take (a take stopped early keeps what lines up with the section).
    void stopRecording();
    /// Recording, or a play-along take running (its count-in included).
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

    // ---- Hearing the target, and playing along (docs/TONE_MATCH.md, "Play along") ------------------------
    // The selected section is played by the processor's PreviewPlayer (sources 3, the song, and 4, its
    // separated guitar), the same player and the same handoff as the A/B. The Target card's Play loops it;
    // a play-along take (Record with a target loaded) plays it once, after a count-in, while recording the
    // DI from the song's first sample, and lines the take up with the section by the device's round trip.
    enum SongSource
    {
        songFull = 0,
        songGuitar = 1
    };
    /// The separated guitar of exactly the selected section (a separated match on this range made it).
    bool hasGuitarStem() const;
    void setSongSource (int songSource);
    /// What plays: the guitar only if chosen and there's a stem for this section, otherwise the full song.
    int getSongSource() const noexcept { return songSource == songGuitar && hasGuitarStem() ? songGuitar : songFull; }
    void setSongLevelDb (float db);
    float getSongLevelDb() const noexcept { return songLevelDb; }

    /// The Target card's Play: the selected section, looped (after a count-in if that's on for Play).
    void setTargetPlaying (bool shouldPlay);
    bool isTargetPlaying() const noexcept { return targetPlaying; }

    /// Record with a target loaded: count-in (if on), then the section once, the DI recorded from its first
    /// sample; the take ends by itself at the section's end (plus the round trip). False without a target.
    bool startPlayAlong();
    bool isPlayingAlong() const noexcept { return takeRunning; }

    /// The count-in: clicks at a tempo before the song. On before Record by default, off before Play.
    void setCountInForTake (bool on) { countInForTake = on; }
    bool getCountInForTake() const noexcept { return countInForTake; }
    void setCountInForPlay (bool on) { countInForPlay = on; }
    bool getCountInForPlay() const noexcept { return countInForPlay; }
    void setCountInBeats (int beats); ///< 2 or 4
    int getCountInBeats() const noexcept { return countInBeats; }
    /// The count-in's tempo: the app's tempo (tempo_bpm, with its Tap) until one is set here.
    void setCountInBpm (double bpm);
    double getCountInBpm() const;
    bool isCountInBpmSetHere() const noexcept { return countInBpmSetHere; }
    /// A tap of the page's Tap button (the app's TapTempo rules: 2 s apart starts over, strays ignored).
    void tapCountInTempo (double nowSeconds);
    static constexpr double minBpm = 30.0, maxBpm = 300.0;
    void setClickLevelDb (float db);
    float getClickLevelDb() const noexcept { return clickLevelDb; }
    static constexpr float minLevelDb = -30.0f, maxLevelDb = 6.0f;

    /// The section's tempo, estimated from its onsets on a worker after the range stays still (0: none yet,
    /// or no clear tempo). A suggestion only; nothing waits for it.
    double getSuggestedBpm() const noexcept { return suggestedBpm.load(); }
    /// For tests: let the tempo worker finish.
    bool waitForTempo (int timeoutMs);

    /// How far the take's DI is shifted to line up: the device's reported input and output latencies plus
    /// this (ms; positive if you hear yourself playing late against the song in the take).
    void setLatencyOffsetMs (double ms);
    double getLatencyOffsetMs() const noexcept { return latencyOffsetMs; }
    static constexpr double minOffsetMs = -50.0, maxOffsetMs = 200.0;
    /// Where the latencies come from (the standalone app's device; tests put a fake device here).
    void setLatencySource (std::function<platform::device::Latency()> source) { latencySource = std::move (source); }

    /// The take's progress: the beats left in the count-in (4, 3, 2, 1; 0 when not counting in), and the
    /// song's position in the section (seconds; -1 when nothing of the section plays).
    int getCountInRemaining() const;
    double getSectionPlayheadSeconds() const;

    /// The last play-along take and how it was lined up.
    struct Take
    {
        bool valid = false;
        platform::device::Latency latency; ///< as reported when it started
        double offsetMs = 0.0;
        int64_t alignSamples = 0;          ///< input + output + offset: the raw recording's sample for the section's start
        int64_t sectionSamples = 0;
        double rangeStart = 0.0, rangeEnd = 0.0;
        int targetVersion = -1;
        int songSource = songFull;
        bool countIn = false;
        int countInBeats = 0;
        double countInBpm = 0.0;
        bool complete = false;             ///< ran to the section's end (not stopped early)
        std::vector<float> raw;            ///< the DI from the song's first sample
    };
    const Take& getTake() const noexcept { return take; }
    /// The reference is a play-along take of the section selected now (so same part uses the band).
    bool referenceIsTake() const;

    // ---- Saving a take (docs/TONE_MATCH.md, "Learning a capture from the song (prototype)") ---------------
    /// The last play-along take is of the section selected now, and nothing is recording.
    bool canSaveTake() const;
    /// Writes, into a new folder <parent>/<the target's file name without its extension>-<yyyymmdd-hhmmss>:
    ///   target.wav  the selected section, as decoded (48 kHz mono);
    ///   stem.wav    its separated guitar, if a separated match of this section made one;
    ///   di_raw.wav  the take as recorded, from the song's first sample;
    ///   di.wav      the take lined up with the section (di_raw from alignSamples on): what Same part matches;
    ///   take.json   how it was lined up (the latencies, the offset, alignSamples, the band), and the match's
    ///               DTW path when a same-part match of exactly this take exists.
    /// 32-bit float WAVs, for prototypes/learn_tone.py and a real-take benchmark. Message thread (file writes,
    /// never the audio thread). Returns the folder; an empty File (and getError()) if it couldn't.
    juce::File saveTake (const juce::File& parent);
    /// Where the page saves takes: ToneMatchTakes in the user data folder.
    static juce::File defaultTakesFolder();
    /// Goes up with every finished take (the page's Save take uses it to tell a new take from a saved one).
    int getTakeNumber() const noexcept { return takeNumber; }

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
    void finishTake (bool complete);
    void refreshSongMaterial();
    void publishMaterial();
    void startTempoEstimate();
    void joinTempo();
    void applySongPlayerSettings();
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
    std::unique_ptr<ampsim::PreviewPlayer::Material> material; // what the player has (the newest published), message thread

    // Hearing the target and playing along.
    bool targetPlaying = false, takeRunning = false, countInForTake = true, countInForPlay = false, countInBpmSetHere = false;
    int songSource = songFull, countInBeats = 4;
    double countInBpm = 120.0, latencyOffsetMs = 0.0;
    float songLevelDb = 0.0f, clickLevelDb = -6.0f;
    ampsim::TapTempo tapTempo;
    Take take;
    int takeNumber = 0;
    int64_t takeNeeded = 0;
    std::function<platform::device::Latency()> latencySource;
    // The song material's section (what source 3 and 4 hold), so a range change rebuilds them.
    double songRangeStart = -1.0, songRangeEnd = -1.0;
    int songTargetVersion = -1;
    bool songStem = false;
    double rangeChangedMs = 0.0;
    // What the finished match compared, for the guitar stem: separated, its range, its target.
    bool matchedSeparated = false, pendingSeparated = false;
    double matchedRangeStart = -1.0, matchedRangeEnd = -1.0, pendingRangeStart = 0.0, pendingRangeEnd = 0.0;
    int matchedTargetVersion = -1, pendingTargetVersion = -1;
    bool pendingBand = false;
    // The tempo estimate's worker.
    std::thread tempoWorker;
    std::atomic<bool> tempoCancel { false }, tempoRunning { false };
    std::atomic<double> suggestedBpm { 0.0 };
    double tempoRangeStart = -1.0, tempoRangeEnd = -1.0;
    int tempoTargetVersion = -1;
};

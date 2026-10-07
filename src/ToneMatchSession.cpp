// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatchSession.h"

#include "BlockParameters.h"
#include "platform/AppInfo.h"
#include "dsp/Loudness.h"
#include "tonematch/AudioFileInput.h"
#include "tonematch/InformedMask.h"
#include "tonematch/TempoEstimate.h"

#include <cmath>
#include <limits>

using namespace ampsim::tonematch;

namespace
{
bool sameSeconds (double a, double b) { return std::abs (a - b) < 1.0e-9; }

void setPlain (AmpSimProcessor& p, const juce::String& id, float value)
{
    if (auto* param = p.parameters.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
    else
        jassertfalse; // a parameter ID tone match writes must exist
}

/// Close mic 1's cab and the playing slot's cab assignment, as one undoable action, so Apply's undo
/// puts the previous cab back along with the knobs. (Cab files aren't parameters, so the parameter
/// tree's undo can't do it by itself.)
class CabChange final : public juce::UndoableAction
{
public:
    CabChange (AmpSimProcessor& p, int slotToAssign, juce::File newCab)
        : processor (p), slot (slotToAssign), next (std::move (newCab))
    {
        const auto path = p.parameters.state.getProperty (AmpSimProcessor::cabPathKey (0)).toString();
        previous = path.isNotEmpty() ? juce::File (path) : juce::File();
        previousAssignment = p.getCabAssignment (slot);
    }

    bool perform() override
    {
        processor.loadCabIR (0, next);
        processor.setCabAssignment (slot, next);
        return true;
    }

    bool undo() override
    {
        if (previous == juce::File())
            processor.clearCabIR (0);
        else
            processor.loadCabIR (0, previous);
        processor.setCabAssignment (slot, previousAssignment);
        return true;
    }

private:
    AmpSimProcessor& processor;
    int slot;
    juce::File next, previous, previousAssignment;
};
} // namespace

/// One count-in click (docs/TONE_MATCH.md, "Play along"): a sine burst, 1 ms linear rise (so it doesn't
/// start with a step) then an exponential decay with an 8 ms time constant, 40 ms long and faded to 0 over its
/// last 5 ms, peak 0.5 (-6 dBFS). The accent (beat 1) at 1760 Hz, the others at 1320 Hz, a fifth below.
static std::shared_ptr<const std::vector<float>> makeClick (double frequency)
{
    const auto fs = 48000.0;
    const auto n = (int) (0.040 * fs), rise = (int) (0.001 * fs), tail = (int) (0.005 * fs);
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const auto t = i / fs;
        auto env = std::exp (-t / 0.008);
        if (i < rise)
            env *= (double) i / rise;
        if (i >= n - tail)
            env *= (double) (n - 1 - i) / tail;
        x[(size_t) i] = (float) (0.5 * env * std::sin (juce::MathConstants<double>::twoPi * frequency * t));
    }
    return std::make_shared<const std::vector<float>> (std::move (x));
}

ToneMatchSession::ToneMatchSession (AmpSimProcessor& processor) : ampSim (processor), latencySource (&platform::device::reportedLatency)
{
    loudness.fill (-std::numeric_limits<double>::infinity());
    material = std::make_unique<ampsim::PreviewPlayer::Material>();
    material->clickAccent = makeClick (1760.0);
    material->click = makeClick (1320.0);
}

ToneMatchSession::~ToneMatchSession()
{
    cancel();
    join();
    cancelCompare();
    joinCompare();
    tempoCancel = true;
    joinTempo();
    ampSim.getPreviewPlayer().setPlaying (false); // the live guitar comes back; the material stays until the next
    if (recordingReference || takeRunning)
        ampSim.getDiRecorder().stop();
}

// ---- The target ----------------------------------------------------------------------------------------

bool ToneMatchSession::setTargetFile (const juce::File& file)
{
    const auto in = AudioFileInput::read (file, maxFileSeconds);
    if (! in.ok)
    {
        error = in.error;
        return false;
    }
    setTargetSignal (in.samples, file.getFileName());
    return true;
}

void ToneMatchSession::setTargetSignal (std::vector<float> samples48k, const juce::String& name)
{
    if (takeRunning)
        stopRecording();
    setTargetPlaying (false);
    target = std::move (samples48k);
    targetName = name;
    ++targetVersion;
    error.clear();
    suggestedBpm = 0.0;
    setRange (0.0, defaultRangeSeconds);
}

void ToneMatchSession::setRange (double startSeconds, double endSeconds)
{
    const auto length = getTargetSeconds();
    if (length <= 0.0)
    {
        rangeStart = rangeEnd = 0.0;
        return;
    }
    if (endSeconds < startSeconds)
        std::swap (startSeconds, endSeconds);
    const auto span = juce::jlimit (juce::jmin (minRangeSeconds, length), juce::jmin (maxRangeSeconds, length), endSeconds - startSeconds);
    const auto before = std::make_pair (rangeStart, rangeEnd);
    rangeStart = juce::jlimit (0.0, length - span, startSeconds);
    rangeEnd = rangeStart + span;
    if (before != std::make_pair (rangeStart, rangeEnd))
        rangeChangedMs = juce::Time::getMillisecondCounterHiRes(); // the song material and the tempo follow once it's still
}

std::vector<float> ToneMatchSession::targetSelection() const
{
    const auto a = (size_t) juce::jlimit (0.0, (double) target.size(), rangeStart * 48000.0);
    const auto b = (size_t) juce::jlimit ((double) a, (double) target.size(), rangeEnd * 48000.0);
    return std::vector<float> (target.begin() + (std::ptrdiff_t) a, target.begin() + (std::ptrdiff_t) b);
}

// ---- The reference --------------------------------------------------------------------------------------

void ToneMatchSession::startRecording()
{
    ampSim.getDiRecorder().start();
    recordingReference = true;
    error.clear();
}

void ToneMatchSession::stopRecording()
{
    if (takeRunning)
    {
        finishTake (false);
        return;
    }
    if (! recordingReference)
        return;
    auto& recorder = ampSim.getDiRecorder();
    recorder.stop();
    recordingReference = false;
    setReferenceSignal (recorder.getRecording(), "Recorded DI");
}

bool ToneMatchSession::isRecording() const
{
    return recordingReference || takeRunning;
}

bool ToneMatchSession::setReferenceFile (const juce::File& file)
{
    const auto in = AudioFileInput::read (file, DiRecorder::maxSeconds * 5.0);
    if (! in.ok)
    {
        error = in.error;
        return false;
    }
    setReferenceSignal (in.samples, file.getFileName());
    return true;
}

void ToneMatchSession::setReferenceSignal (std::vector<float> samples48k, const juce::String& name)
{
    reference = std::move (samples48k);
    referenceName = name;
    take.valid = false; // a take sets itself valid again after this (finishTake)
    error.clear();
}

// ---- Matching ---------------------------------------------------------------------------------------------

std::vector<juce::File> ToneMatchSession::builtInCabs()
{
    auto files = platform::factoryContentFolder().getChildFile ("irs").findChildFiles (juce::File::findFiles, true, "*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); });
    return std::vector<juce::File> (files.begin(), files.end());
}

juce::String ToneMatchSession::whyCantMatch() const
{
    if (running)
        return "Matching already";
    if (target.empty())
        return "Choose a target: a song or a guitar track";
    if (recordingReference || takeRunning)
        return "Stop the recording first";
    if (reference.size() < (size_t) (minReferenceSeconds * 48000.0))
        return "Record or choose your DI (at least " + juce::String ((int) minReferenceSeconds) + " s)";
    if (separate && separator == nullptr)
        return "Separation isn't available in this build";
    bool anyCapture = false;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
        anyCapture = anyCapture || ampSim.getSlotCapture (s).existsAsFile();
    if (! anyCapture)
        return "Every amp slot is empty";
    if (builtInCabs().empty())
        return "The built-in cabs are missing";
    return {};
}

juce::String ToneMatchSession::whyNoCleanup() const
{
    if (cleanupApplies())
        return {};
    return "Needs your notes lined up with the target's: a play-along take of this section, or Same part.";
}

bool ToneMatchSession::startMatch()
{
    if (whyCantMatch().isNotEmpty())
        return false;
    join();

    MatchSettings settings;
    settings.mode = mode;
    for (int s = 0; s < AmpSimProcessor::numAmpSlots; ++s)
    {
        const auto f = ampSim.getSlotCapture (s);
        settings.models[(size_t) s] = f.existsAsFile() ? f : juce::File();
    }
    settings.cabs = builtInCabs();
    settings.calibration = ampSim.getCaptureCalibration();
    // A play-along take of this section is lined up with it already: DTW only near that alignment.
    const auto isTake = referenceIsTake();
    if (mode == Mode::samePart && isTake)
        settings.alignmentBandSeconds = ampsim::tonematch::playAlongBandSeconds;
    // A take plays the target's notes: the take-aware score (Round 2) picks the winner, in either mode.
    settings.takeIsLinedUp = isTake;
    pendingSeparated = separate;
    pendingRangeStart = rangeStart;
    pendingRangeEnd = rangeEnd;
    pendingTargetVersion = targetVersion;
    pendingBand = settings.alignmentBandSeconds > 0.0;
    // The cleanup with the take, when its notes line up (cleanupApplies). A take of this section is lined up
    // already, so its DTW runs in the play-along band; any other same-part DI unbanded. On: always. Auto (Round 2,
    // docs/TONE_MATCH.md "The defaults"): only with separation on; in Same part then always, in Anything when the
    // bleed detector finds more removed than the tone loses on its own. Off: never.
    enum class Plan { none, always, ifBleed };
    auto plan = Plan::none;
    if (cleanupApplies())
    {
        if (cleanupChoice == Cleanup::on)
            plan = Plan::always;
        else if (cleanupChoice == Cleanup::automatic && separate)
            plan = mode == Mode::samePart ? Plan::always : Plan::ifBleed;
    }
    const auto automatic = cleanupChoice == Cleanup::automatic;
    const auto cleanupBand = isTake ? ampsim::tonematch::playAlongBandSeconds : 0.0;

    stopAndClearCompare();
    setTargetPlaying (false);
    cancelFlag = false;
    finished = false;
    progress = 0.0;
    running = true;
    resultReady = false;
    error.clear();
    {
        const std::lock_guard<std::mutex> l (lock);
        stage = separate ? "Starting the separation" : "Starting";
    }

    worker = std::thread ([this, settings, targetCopy = targetSelection(), referenceCopy = reference, doSeparate = separate, sep = separator,
                           plan, automatic, cleanupBand] {
        auto targetSignal = targetCopy;
        MatchResult r;
        CleanupInfo info;
        info.automatic = automatic;
        std::vector<float> rawTarget;
        // Progress: the separation half of it when on, the cleanup (about a second a minute) a twentieth; with the
        // bleed detector, the raw match, the detector, and the cleaned match share the rest.
        const auto separationShare = doSeparate ? 0.5 : 0.0;
        const auto cleanupShare = plan != Plan::none ? 0.05 : 0.0;
        auto report = [this] (double from, double to) {
            return [this, from, to] (double f, const juce::String& s) {
                progress = from + (to - from) * f;
                const std::lock_guard<std::mutex> l (lock);
                stage = s;
            };
        };
        const auto tooLittle = [&] (MatchResult& m) {
            if (doSeparate && ! m.ok && ! m.cancelled && m.error.startsWith ("The target has too little playing"))
                m.error = "The separated guitar has too little playing in it: the separation found almost no guitar in this section. "
                          "Choose a section where the guitar plays, or switch separation off.";
        };
        const auto useCleaned = [&] (ampsim::tonematch::informed::Result& cleaned) {
            info.onsets = cleaned.onsets;
            info.notes = cleaned.notes;
            info.pitched = cleaned.pitched;
            info.keptDb = cleaned.keptDb;
            info.seconds = cleaned.seconds;
            info.used = true;
            rawTarget = std::move (targetSignal);
            targetSignal = std::move (cleaned.output);
        };

        if (doSeparate && sep != nullptr)
        {
            juce::String separationError;
            targetSignal = sep (targetCopy, cancelFlag, report (0.0, separationShare), separationError);
            if (targetSignal.empty())
            {
                r.cancelled = cancelFlag.load();
                r.error = r.cancelled ? "Cancelled" : "Separation failed: " + separationError;
            }
        }
        const auto matchFrom = separationShare + cleanupShare;
        if (! targetSignal.empty() && plan == Plan::always && ! cancelFlag.load())
        {
            // The cleanup (docs/TONE_MATCH.md, "Cleaning up the target with your take"): between the separation (or
            // the section) and the match. If it finds no pitched notes in the DI, the match runs on the target as it
            // is and the result says why.
            info.attempted = true;
            info.banded = cleanupBand > 0.0;
            auto cleaned = informed::cleanUp (targetSignal, referenceCopy, cleanupBand, cancelFlag, report (separationShare, matchFrom));
            if (cleaned.cancelled)
            {
                r.cancelled = true;
                r.error = "Cancelled";
                targetSignal.clear();
            }
            else if (cleaned.ok)
                useCleaned (cleaned);
            else
                info.skipped = cleaned.error;
        }
        if (! targetSignal.empty() && plan == Plan::ifBleed)
        {
            // Auto in Anything: the raw match, the take through its pick as a bleed-free stand-in, the detector, and
            // only if it finds bleed, the cleaned target matched again.
            const auto rawEnd = matchFrom + (1.0 - matchFrom) * 0.5;
            r = ToneMatcher::match (targetSignal, referenceCopy, settings, cancelFlag, report (matchFrom, rawEnd));
            tooLittle (r);
            if (r.ok && ! cancelFlag.load())
            {
                info.attempted = true;
                info.banded = cleanupBand > 0.0;
                report (rawEnd, rawEnd + 0.05) (0.0, "Looking for bleed to clean up");
                const auto proxy = ToneMatcher::renderTone (ToneMatcher::settingsFor (r, settings), referenceCopy, cancelFlag);
                auto bleed = proxy.empty() ? informed::Bleed {} : informed::measureBleed (targetSignal, referenceCopy, proxy, cleanupBand, cancelFlag);
                if (cancelFlag.load() || bleed.cancelled)
                {
                    r = {};
                    r.cancelled = true;
                    r.error = "Cancelled";
                }
                else if (! bleed.ok)
                    info.skipped = proxy.empty() ? juce::String ("couldn't render your take through the match") : bleed.error;
                else
                {
                    info.measuredBleed = true;
                    info.excessDb = bleed.excessDb;
                    if (bleed.excessDb > informed::bleedExcessThresholdDb)
                    {
                        useCleaned (bleed.cleanedTarget);
                        r = ToneMatcher::match (targetSignal, referenceCopy, settings, cancelFlag, report (rawEnd + 0.05, 1.0));
                        tooLittle (r);
                    }
                    else
                        info.skipped = "no bleed to remove (" + juce::String (bleed.excessDb, 2) + " dB, under "
                                       + juce::String (informed::bleedExcessThresholdDb, 2) + ")";
                }
            }
        }
        else if (! targetSignal.empty())
        {
            r = ToneMatcher::match (targetSignal, referenceCopy, settings, cancelFlag, report (matchFrom, 1.0));
            tooLittle (r);
        }

        {
            const std::lock_guard<std::mutex> l (lock);
            pending = std::move (r);
            pendingRawTarget = info.used ? std::move (rawTarget) : targetSignal;
            pendingTarget = std::move (targetSignal);
            pendingReference = referenceCopy;
            pendingCleanup = info;
        }
        finished = true;
    });
    return true;
}

void ToneMatchSession::cancel()
{
    cancelFlag = true;
}

juce::String ToneMatchSession::getStage() const
{
    const std::lock_guard<std::mutex> l (lock);
    return stage;
}

void ToneMatchSession::join()
{
    if (worker.joinable())
        worker.join();
}

bool ToneMatchSession::waitForMatch (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (running && ! finished && juce::Time::getMillisecondCounterHiRes() < end)
        juce::Thread::sleep (10);
    poll();
    return ! running;
}

void ToneMatchSession::poll()
{
    if (takeRunning)
    {
        // The recorder stops itself at the take's length (arm's limit); what it was still writing is in the
        // ring, so check first and drain after.
        auto& recorder = ampSim.getDiRecorder();
        const auto stillGoing = recorder.isRecording() || recorder.isArmed();
        recorder.drain();
        if (! stillGoing || (int64_t) recorder.getRecording().size() >= takeNeeded)
            finishTake (true);
    }

    // The song material and the tempo estimate follow the range once it has been still for 250 ms.
    if (! target.empty() && juce::Time::getMillisecondCounterHiRes() - rangeChangedMs >= 250.0)
    {
        if (targetPlaying && (! sameSeconds (songRangeStart, rangeStart) || ! sameSeconds (songRangeEnd, rangeEnd) || songTargetVersion != targetVersion))
        {
            refreshSongMaterial();
            publishMaterial();
        }
        if (! tempoRunning && (! sameSeconds (tempoRangeStart, rangeStart) || ! sameSeconds (tempoRangeEnd, rangeEnd) || tempoTargetVersion != targetVersion))
            startTempoEstimate();
    }

    if (recordingReference)
    {
        auto& recorder = ampSim.getDiRecorder();
        recorder.drain();
        if (! recorder.isRecording()) // it stopped itself at a minute
        {
            recordingReference = false;
            setReferenceSignal (recorder.getRecording(), "Recorded DI");
        }
    }

    if (running && finished)
    {
        join();
        running = false;
        bool ok = false;
        {
            const std::lock_guard<std::mutex> l (lock);
            if (pending.ok)
            {
                result = pending;
                resultReady = true;
                comparedTarget = std::make_shared<const std::vector<float>> (std::move (pendingTarget));
                comparedRawTarget = std::make_shared<const std::vector<float>> (std::move (pendingRawTarget));
                cleanupInfo = pendingCleanup;
                if (previewSource == sourceTargetRaw && ! hasRawTarget())
                    previewSource = sourceTarget;
                comparedReference = std::make_shared<const std::vector<float>> (std::move (pendingReference));
                matchedSeparated = pendingSeparated;
                matchedRangeStart = pendingRangeStart;
                matchedRangeEnd = pendingRangeEnd;
                matchedTargetVersion = pendingTargetVersion;
                ok = true;
            }
            else
                error = pending.error;
        }
        if (ok)
        {
            // The comparison: the whole matched section looped, the renders started.
            loopStart = 0.0;
            loopEnd = getSectionSeconds();
            startCompareRender (true);
        }
    }

    pollCompare();
}

// ---- Applying ------------------------------------------------------------------------------------------

bool ToneMatchSession::apply()
{
    if (! resultReady)
        return false;

    const auto& r = result;
    auto& p = ampSim;
    // The parameter tree writes parameter changes into its state (and so into the undo history) on its own
    // timer; copyState() makes it do so now. First for anything still pending from before, so that lands
    // in the earlier step, then for Apply's changes, inside "Match tone".
    p.parameters.copyState();
    p.undoManager.beginNewTransaction ("Match tone");

    // The amp: the slot, its Gain (the input trim) and its five tone knobs.
    setPlain (p, AmpSimProcessor::slotParamId, (float) r.slot);
    setPlain (p, "amp_bypass", 0.0f);
    setPlain (p, AmpSimProcessor::ampParamId (r.slot, "input_trim"), (float) r.gainDb);
    for (size_t b = 0; b < ampsim::AmpTone::numBands; ++b)
        setPlain (p, AmpSimProcessor::ampParamId (r.slot, juce::String (ampsim::AmpTone::bands[b].name).toLowerCase()), (float) r.tone[b]);

    // The cab: the matched IR alone in close mic 1 (mic 2 and the room muted, no cuts, not bypassed).
    setPlain (p, "cab_bypass", 0.0f);
    setPlain (p, AmpSimProcessor::cabParamId (0, "mute"), 0.0f);
    setPlain (p, AmpSimProcessor::cabParamId (1, "mute"), 1.0f);
    setPlain (p, AmpSimProcessor::cabParamId (AmpSimProcessor::roomMic, "mute"), 1.0f);
    setPlain (p, "cab_lowcut_on", 0.0f);
    setPlain (p, "cab_highcut_on", 0.0f);
    p.undoManager.perform (new CabChange (p, r.slot, r.cab));

    // The match EQ: the post EQ, parametric, the five fitted bands, no cuts; its section on.
    const juce::String eq = "eq_post";
    setPlain (p, "post_fx_on", 1.0f);
    setPlain (p, eq + "_on", 1.0f);
    setPlain (p, eq + "_mode", 1.0f); // Parametric
    for (int b = 0; b < ampsim::Equalizer::numParametricBands; ++b)
    {
        const auto& band = r.eq[(size_t) b];
        setPlain (p, params::EqualizerParameters::bandId (eq, b, "type"), (float) (int) band.type);
        setPlain (p, params::EqualizerParameters::bandId (eq, b, "freq"), band.frequency);
        setPlain (p, params::EqualizerParameters::bandId (eq, b, "gain"), band.gainDb);
        setPlain (p, params::EqualizerParameters::bandId (eq, b, "q"), band.q);
    }
    setPlain (p, eq + "_lowcut_on", 0.0f);
    setPlain (p, eq + "_highcut_on", 0.0f);

    // The pre effects that color the tone before the amp are switched off, so the match is heard as it
    // was made (Sean, 2026-10-04: "switch them off"). The noise gate stays as it is: it only mutes
    // between notes, so it doesn't change the tone that was matched (ASSUMPTIONS TM19).
    for (const auto& fx : preEffectsApplyTurnsOff())
        setPlain (p, fx.parameterId, 0.0f);

    p.parameters.copyState();
    p.undoManager.beginNewTransaction();
    return true;
}

const std::vector<ToneMatchSession::PreEffect>& ToneMatchSession::preEffectsApplyTurnsOff()
{
    static const std::vector<PreEffect> list { { "comp_pre_on", "compressor" }, { "boost_on", "boost" }, { "od_on", "overdrive" }, { "eq_pre_on", "pre EQ" } };
    return list;
}

juce::StringArray ToneMatchSession::preEffectsOnNow() const
{
    juce::StringArray on;
    for (const auto& fx : preEffectsApplyTurnsOff())
        if (auto* param = ampSim.parameters.getRawParameterValue (fx.parameterId); param != nullptr && param->load() >= 0.5f)
            on.add (fx.name);
    return on;
}

void ToneMatchSession::discard()
{
    stopAndClearCompare();
    resultReady = false;
    result = {};
}

// ---- Comparing ------------------------------------------------------------------------------------------

juce::String ToneMatchSession::sourceName (int source)
{
    return source == sourceTarget ? "Target" : source == sourceMatch ? "Match" : source == sourceTargetRaw ? "Raw" : "Current";
}

float ToneMatchSession::storedValue (const juce::String& parameterId, double value) const
{
    // What the parameter holds after Apply writes `value` (its range snaps it), so a render of these
    // settings equals one of the settings read back after Apply.
    auto* param = ampSim.parameters.getParameter (parameterId);
    jassert (param != nullptr);
    return param != nullptr ? param->convertFrom0to1 (param->convertTo0to1 ((float) value)) : (float) value;
}

ampsim::tonematch::ToneSettings ToneMatchSession::matchedSettings() const
{
    using namespace ampsim;
    const auto& r = result;
    tonematch::ToneSettings s;
    s.model = ampSim.getSlotCapture (r.slot);
    s.calibration = ampSim.getCaptureCalibration();
    s.ampOn = true;
    s.gainDb = storedValue (AmpSimProcessor::ampParamId (r.slot, "input_trim"), r.gainDb);
    s.masterDb = ampSim.parameters.getRawParameterValue (AmpSimProcessor::ampParamId (r.slot, "output_trim"))->load();
    for (size_t b = 0; b < AmpTone::numBands; ++b)
        s.tone[b] = storedValue (AmpSimProcessor::ampParamId (r.slot, juce::String (AmpTone::bands[b].name).toLowerCase()), r.tone[b]);

    // The post EQ as Apply leaves it: the current settings (its graphic sliders are kept, unused), then
    // parametric with the five bands and no cuts, read back the way EqualizerParameters::read() reads them.
    params::EqualizerParameters eq;
    eq.bind (ampSim.parameters, "eq_post");
    s.postEq = eq.read();
    s.postEq.mode = Equalizer::Mode::parametric;
    const auto gain = [] (float v) { return std::round (v * 100.0f) / 100.0f; };
    for (int b = 0; b < Equalizer::numParametricBands; ++b)
    {
        const auto& band = r.eq[(size_t) b];
        const juce::String id = "eq_post";
        s.postEq.bands[(size_t) b] = { (Equalizer::BandType) juce::jlimit (0, 3, juce::roundToInt (storedValue (params::EqualizerParameters::bandId (id, b, "type"), (int) band.type))),
                                       storedValue (params::EqualizerParameters::bandId (id, b, "freq"), band.frequency),
                                       gain (storedValue (params::EqualizerParameters::bandId (id, b, "gain"), band.gainDb)),
                                       storedValue (params::EqualizerParameters::bandId (id, b, "q"), band.q) };
    }
    s.postEq.lowCut.on = false;
    s.postEq.highCut.on = false;
    s.postEqOn = true;
    return s;
}

ampsim::tonematch::ToneSettings ToneMatchSession::currentSettings() const
{
    using namespace ampsim;
    auto& p = ampSim;
    const auto raw = [&p] (const juce::String& id) { return p.parameters.getRawParameterValue (id)->load(); };
    const auto slot = juce::jlimit (0, AmpSimProcessor::numAmpSlots - 1, juce::roundToInt (raw (AmpSimProcessor::slotParamId)));
    tonematch::ToneSettings s;
    s.model = p.getSlotCapture (slot);
    s.calibration = p.getCaptureCalibration();
    s.ampOn = raw ("amp_bypass") < 0.5f;
    s.gainDb = raw (AmpSimProcessor::ampParamId (slot, "input_trim"));
    s.masterDb = raw (AmpSimProcessor::ampParamId (slot, "output_trim"));
    for (size_t b = 0; b < AmpTone::numBands; ++b)
        s.tone[b] = raw (AmpSimProcessor::ampParamId (slot, juce::String (AmpTone::bands[b].name).toLowerCase()));
    if (raw ("cab_bypass") < 0.5f && raw (AmpSimProcessor::cabParamId (0, "mute")) < 0.5f)
        s.cabIR = p.getCloseMicIR (0);
    params::EqualizerParameters eq;
    eq.bind (p.parameters, "eq_post");
    s.postEqOn = raw ("post_fx_on") >= 0.5f && eq.isOn();
    s.postEq = eq.read();
    return s;
}

juce::String ToneMatchSession::currentFingerprint (const ampsim::tonematch::ToneSettings& s) const
{
    // Everything a Current render depends on, as text: equal text, equal render.
    juce::String f;
    f << s.model.getFullPathName() << "|" << (int) s.ampOn << "|" << s.gainDb << "|" << s.masterDb;
    for (auto t : s.tone)
        f << "|" << t;
    f << "|" << (int) s.calibration.enabled << "|" << s.calibration.interfaceInputDbu;
    double irSum = 0.0, irEnergy = 0.0;
    for (auto v : s.cabIR)
    {
        irSum += v;
        irEnergy += (double) v * v;
    }
    f << "|ir" << (int) s.cabIR.size() << ":" << juce::String (irSum, 12) << ":" << juce::String (irEnergy, 12);
    f << "|eq" << (int) s.postEqOn << (int) s.postEq.mode;
    for (auto v : s.postEq.sliders)
        f << "," << v;
    for (const auto& b : s.postEq.bands)
        f << "," << (int) b.type << "," << b.frequency << "," << b.gainDb << "," << b.q;
    f << "," << (int) s.postEq.lowCut.on << "," << s.postEq.lowCut.frequency << "," << (int) s.postEq.lowCut.slope;
    f << "," << (int) s.postEq.highCut.on << "," << s.postEq.highCut.frequency << "," << (int) s.postEq.highCut.slope;
    return f;
}

bool ToneMatchSession::startCompareRender (bool renderMatch)
{
    if (! resultReady || comparedReference == nullptr)
        return false;
    cancelCompare();
    joinCompare();

    renderMatch = renderMatch || matchAudio == nullptr;
    auto matchSettings = matchedSettings();
    const auto cabFile = result.cab;
    auto current = currentSettings();
    const auto fingerprint = currentFingerprint (current);
    seenFingerprint = fingerprint;
    fingerprintChangedMs = 0.0;

    compareCancel = false;
    compareFinished = false;
    compareProgress = 0.0;
    compareRunning = true;
    compareError.clear();
    {
        const std::lock_guard<std::mutex> l (lock);
        compareStage = renderMatch ? "Rendering your DI through the match" : "Rendering your DI through your current settings";
    }

    compareWorker = std::thread ([this, renderMatch, matchSettings, cabFile, current, fingerprint, di = comparedReference, tgt = comparedTarget,
                                  needTargetSpectrum = targetSpectrum.empty()]() mutable {
        using namespace ampsim::tonematch;
        CompareRender out;
        out.fingerprint = fingerprint;
        out.renderedMatch = renderMatch;
        const auto stage = [this] (double fraction, const juce::String& text) {
            compareProgress = fraction;
            const std::lock_guard<std::mutex> l (lock);
            compareStage = text;
        };

        if (renderMatch)
        {
            matchSettings.cabIR = ToneMatcher::irAsPlayed (cabFile);
            auto m = ToneMatcher::renderTone (matchSettings, *di, compareCancel);
            if (m.empty() && ! compareCancel.load())
                out.error = "The match couldn't be rendered (its capture didn't load).";
            out.match = std::make_shared<const std::vector<float>> (std::move (m));
            if (! compareCancel.load() && out.error.isEmpty())
            {
                stage (0.45, "Measuring the spectra");
                out.matchSpectrum = Analysis::of (*out.match).ltas;
                if (needTargetSpectrum)
                    out.targetSpectrum = Analysis::of (*tgt).ltas;
                stage (0.5, "Rendering your DI through your current settings");
            }
        }
        if (! compareCancel.load() && out.error.isEmpty())
        {
            auto c = ToneMatcher::renderTone (current, *di, compareCancel);
            if (c.empty() && ! compareCancel.load())
                out.error = "Your current settings couldn't be rendered (the capture didn't load).";
            out.current = std::make_shared<const std::vector<float>> (std::move (c));
        }
        out.cancelled = compareCancel.load();
        out.ok = ! out.cancelled && out.error.isEmpty();
        {
            const std::lock_guard<std::mutex> l (lock);
            comparePending = std::move (out);
        }
        compareProgress = 1.0;
        compareFinished = true;
    });
    return true;
}

void ToneMatchSession::cancelCompare()
{
    compareCancel = true;
}

void ToneMatchSession::joinCompare()
{
    if (compareWorker.joinable())
        compareWorker.join();
}

bool ToneMatchSession::waitForCompare (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (compareRunning && ! compareFinished && juce::Time::getMillisecondCounterHiRes() < end)
        juce::Thread::sleep (5);
    poll();
    return ! compareRunning;
}

juce::String ToneMatchSession::getCompareStatus() const
{
    if (! resultReady)
        return "Match first: then you can hear the target, the match, and your current settings";
    if (compareRunning)
    {
        const std::lock_guard<std::mutex> l (lock);
        return compareStage + "...";
    }
    if (compareError.isNotEmpty())
        return compareError;
    return {};
}

bool ToneMatchSession::isCurrentStale() const
{
    return compareReady && currentFingerprint (currentSettings()) != renderedFingerprint;
}

void ToneMatchSession::pollCompare()
{
    auto& player = ampSim.getPreviewPlayer();
    player.collectGarbage();

    if (compareRunning && compareFinished)
    {
        joinCompare();
        compareRunning = false;
        CompareRender r;
        {
            const std::lock_guard<std::mutex> l (lock);
            r = std::move (comparePending);
        }
        if (! r.ok)
            compareError = r.cancelled ? juce::String ("Rendering cancelled") : r.error;
        else
        {
            if (r.renderedMatch)
            {
                matchAudio = r.match;
                matchSpectrum = std::move (r.matchSpectrum);
                if (! r.targetSpectrum.empty())
                    targetSpectrum = std::move (r.targetSpectrum);
            }
            currentAudio = r.current;
            renderedFingerprint = r.fingerprint;

            // The material: the three sources, and in same-part mode the alignment as two monotone tables
            // (each frame's mean partner on the other clock, at the analysis frames' centres). The song and its
            // guitar (sources 3 and 4) and the clicks stay as they are.
            auto m = std::make_unique<ampsim::PreviewPlayer::Material> (*material);
            m->audio[0] = comparedTarget;
            m->audio[1] = matchAudio;
            m->audio[2] = currentAudio;
            m->audio[(size_t) ampsim::PreviewPlayer::sourceTargetRaw] = hasRawTarget() ? comparedRawTarget : nullptr;
            m->aligned = false;
            m->diAtTarget.clear();
            m->targetAtDi.clear();
            const auto& path = result.alignmentPath;
            if (result.mode == Mode::samePart && ! path.empty())
            {
                using namespace ampsim::tonematch;
                int targetFrames = 0, diFrames = 0;
                for (const auto& [i, j] : path)
                {
                    targetFrames = juce::jmax (targetFrames, i + 1);
                    diFrames = juce::jmax (diFrames, j + 1);
                }
                std::vector<double> sumJ ((size_t) targetFrames, 0.0), sumI ((size_t) diFrames, 0.0);
                std::vector<int> countJ ((size_t) targetFrames, 0), countI ((size_t) diFrames, 0);
                for (const auto& [i, j] : path)
                {
                    sumJ[(size_t) i] += j;
                    ++countJ[(size_t) i];
                    sumI[(size_t) j] += i;
                    ++countI[(size_t) j];
                }
                m->aligned = true;
                m->hop = hop;
                m->offset = fftSize / 2.0;
                // The path visits every frame of both (its steps move by at most one), so no count is 0.
                for (size_t i = 0; i < sumJ.size(); ++i)
                    m->diAtTarget.push_back (sumJ[i] / juce::jmax (1, countJ[i]) * hop + m->offset);
                for (size_t j = 0; j < sumI.size(); ++j)
                    m->targetAtDi.push_back (sumI[j] / juce::jmax (1, countI[j]) * hop + m->offset);
            }
            compareAligned = m->aligned;
            material = std::move (m);
            compareReady = true;
            compareError.clear();
            updateLoops();
            // A take plays on what it started with: a swap mid-take would crossfade the song into itself
            // (the same audio, 3 dB up halfway through an equal-power fade). It's handed over after the take.
            if (! takeRunning)
                publishMaterial();
        }
    }

    // Current follows the settings: re-rendered once they've been still for 300 ms.
    if (compareReady && ! compareRunning)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto fingerprint = currentFingerprint (currentSettings());
        if (fingerprint != seenFingerprint)
        {
            seenFingerprint = fingerprint;
            fingerprintChangedMs = now;
        }
        if (fingerprint != renderedFingerprint && fingerprintChangedMs > 0.0 && now - fingerprintChangedMs >= 300.0)
            startCompareRender (false);
    }
}

void ToneMatchSession::stopAndClearCompare()
{
    cancelCompare();
    joinCompare();
    compareRunning = false;
    compareFinished = false;
    setPreviewPlaying (false);
    compareReady = false;
    compareAligned = false;
    compareError.clear();
    matchAudio.reset();
    currentAudio.reset();
    targetSpectrum.clear();
    matchSpectrum.clear();
    renderedFingerprint.clear();
    material->audio[0] = material->audio[1] = material->audio[2] = nullptr;
    material->audio[(size_t) ampsim::PreviewPlayer::sourceTargetRaw] = nullptr;
    material->aligned = false;
    material->diAtTarget.clear();
    material->targetAtDi.clear();
    loudness.fill (-std::numeric_limits<double>::infinity());
    matchGainDb.fill (0.0f);
}

const std::vector<float>& ToneMatchSession::getCompareAudio (int source) const
{
    static const std::vector<float> none;
    if (source == sourceTargetRaw && ! hasRawTarget())
        return none;
    const auto& p = source == sourceTarget ? comparedTarget : source == sourceMatch ? matchAudio : source == sourceTargetRaw ? comparedRawTarget : currentAudio;
    return p != nullptr ? *p : none;
}

void ToneMatchSession::setPreviewPlaying (bool shouldPlay)
{
    const auto was = previewPlaying;
    previewPlaying = shouldPlay && compareReady && ! takeRunning;
    auto& player = ampSim.getPreviewPlayer();
    if (previewPlaying)
    {
        targetPlaying = false;
        player.setOnce (false);
        player.setCountIn (0, 1.0);
        player.setMuteLive (muteLive);
        player.setLevelDb (previewLevelDb);
        player.setSource (playerSource (previewSource));
        if (! was)
            player.startFresh();
    }
    else if (was)
        player.setPlaying (false); // only what this started: a target preview or a take is left alone
}

void ToneMatchSession::setPreviewSource (int source)
{
    previewSource = source == sourceTargetRaw && hasRawTarget() ? sourceTargetRaw : juce::jlimit (0, numSources - 1, source);
    if (previewPlaying)
        ampSim.getPreviewPlayer().setSource (playerSource (previewSource));
}

void ToneMatchSession::setLevelMatch (bool on)
{
    levelMatch = on;
    updateLevels();
}

void ToneMatchSession::setMuteLive (bool on)
{
    muteLive = on;
    if (previewPlaying)
        ampSim.getPreviewPlayer().setMuteLive (on);
}

void ToneMatchSession::setPreviewLevelDb (float db)
{
    previewLevelDb = juce::jlimit (minPreviewLevelDb, maxPreviewLevelDb, db);
    if (previewPlaying)
        ampSim.getPreviewPlayer().setLevelDb (previewLevelDb);
}

void ToneMatchSession::setLoop (double startSeconds, double endSeconds)
{
    const auto length = getSectionSeconds();
    if (length <= 0.0)
        return;
    if (endSeconds < startSeconds)
        std::swap (startSeconds, endSeconds);
    const auto span = juce::jlimit (juce::jmin (minLoopSeconds, length), length, endSeconds - startSeconds);
    loopStart = juce::jlimit (0.0, length - span, startSeconds);
    loopEnd = loopStart + span;
    updateLoops();
}

void ToneMatchSession::updateLoops()
{
    // The target's loop in its own samples; the DI's through the alignment (same part), or all of it.
    auto& player = ampSim.getPreviewPlayer();
    const auto a = (int64_t) std::llround (loopStart * 48000.0), b = (int64_t) std::llround (loopEnd * 48000.0);
    player.setLoop (0, a, b);
    const auto diLength = comparedReference != nullptr ? (int64_t) comparedReference->size() : (int64_t) 0;
    if (material->aligned)
        diLoop = { (int64_t) std::llround (ampsim::PreviewPlayer::alignedPosition (*material, 0, (double) a)),
                   (int64_t) std::llround (ampsim::PreviewPlayer::alignedPosition (*material, 0, (double) b)) };
    else
        diLoop = { 0, diLength };
    player.setLoop (1, diLoop.first, diLoop.second);
    updateLevels();
}

void ToneMatchSession::updateLevels()
{
    // BS.1770 integrated loudness of each source over its own loop (ampsim::loudness, the same measurement
    // the cabs and the captures are normalized with), and the gains that bring all three to the loudness of
    // the Current render (the level the live rig plays at), or of the match, or of the target, whichever is
    // there first. Integrated loudness scales exactly with gain (both gates are relative, or far below), so
    // equal targets give equal loudness.
    const auto a = (int64_t) std::llround (loopStart * 48000.0), b = (int64_t) std::llround (loopEnd * 48000.0);
    for (int s = 0; s < numAllSources; ++s)
    {
        const auto& audio = getCompareAudio (s);
        const auto onTargetClock = s == sourceTarget || s == sourceTargetRaw;
        const auto from = onTargetClock ? a : diLoop.first, to = onTargetClock ? b : diLoop.second;
        const auto lo = juce::jlimit ((int64_t) 0, (int64_t) audio.size(), from), hi = juce::jlimit (lo, (int64_t) audio.size(), to);
        loudness[(size_t) s] = hi > lo ? ampsim::loudness::integratedMono (audio.data() + lo, (int) (hi - lo), 48000.0)
                                       : -std::numeric_limits<double>::infinity();
    }
    double reference = -std::numeric_limits<double>::infinity();
    for (auto s : { sourceCurrent, sourceMatch, sourceTarget })
        if (std::isfinite (loudness[(size_t) s]))
        {
            reference = loudness[(size_t) s];
            break;
        }
    for (int s = 0; s < numAllSources; ++s)
    {
        const auto l = loudness[(size_t) s];
        matchGainDb[(size_t) s] = levelMatch && std::isfinite (l) && std::isfinite (reference) ? (float) juce::jlimit (-40.0, 40.0, reference - l) : 0.0f;
        ampSim.getPreviewPlayer().setSourceGainDb (playerSource (s), matchGainDb[(size_t) s]);
    }
}

// ---- Hearing the target, and playing along ---------------------------------------------------------------

bool ToneMatchSession::hasGuitarStem() const
{
    return matchedSeparated && comparedRawTarget != nullptr && ! comparedRawTarget->empty() && matchedTargetVersion == targetVersion
           && sameSeconds (matchedRangeStart, rangeStart) && sameSeconds (matchedRangeEnd, rangeEnd);
}

void ToneMatchSession::setSongSource (int newSource)
{
    songSource = newSource == songGuitar ? songGuitar : songFull;
    if (targetPlaying || takeRunning)
    {
        // The stem shares the song's clock (the section's samples), so the switch keeps the exact place.
        if (getSongSource() == songGuitar && ! songStem)
        {
            refreshSongMaterial();
            publishMaterial();
        }
        ampSim.getPreviewPlayer().setSource (getSongSource() == songGuitar ? 4 : 3);
    }
}

void ToneMatchSession::setSongLevelDb (float db)
{
    songLevelDb = juce::jlimit (minLevelDb, maxLevelDb, db);
    if (targetPlaying || takeRunning)
        ampSim.getPreviewPlayer().setLevelDb (songLevelDb);
}

void ToneMatchSession::setClickLevelDb (float db)
{
    clickLevelDb = juce::jlimit (minLevelDb, maxLevelDb, db);
    ampSim.getPreviewPlayer().setClickLevelDb (clickLevelDb);
}

void ToneMatchSession::setCountInBeats (int beats)
{
    countInBeats = beats <= 2 ? 2 : 4;
}

void ToneMatchSession::setCountInBpm (double bpm)
{
    countInBpm = juce::jlimit (minBpm, maxBpm, bpm);
    countInBpmSetHere = true;
}

double ToneMatchSession::getCountInBpm() const
{
    return countInBpmSetHere ? countInBpm : juce::jlimit (minBpm, maxBpm, ampSim.getTempo());
}

void ToneMatchSession::tapCountInTempo (double nowSeconds)
{
    if (tapTempo.tap (nowSeconds) && tapTempo.hasTempo())
        setCountInBpm (std::round (tapTempo.getBpm() * 10.0) / 10.0);
}

void ToneMatchSession::setLatencyOffsetMs (double ms)
{
    latencyOffsetMs = juce::jlimit (minOffsetMs, maxOffsetMs, ms);
}

void ToneMatchSession::refreshSongMaterial()
{
    // Sources 3 and 4 on clock 2, the section's own samples: the song's selected section (a copy, at most a
    // minute), and its separated guitar when a separated match made one of exactly this section.
    material->audio[3] = std::make_shared<const std::vector<float>> (targetSelection());
    songStem = hasGuitarStem();
    material->audio[4] = songStem ? comparedRawTarget : nullptr; // the stem as separated, not cleaned up
    songRangeStart = rangeStart;
    songRangeEnd = rangeEnd;
    songTargetVersion = targetVersion;
}

void ToneMatchSession::publishMaterial()
{
    auto& player = ampSim.getPreviewPlayer();
    player.collectGarbage();
    if (material->audio[3] != nullptr)
        player.setLoop (2, 0, (int64_t) material->audio[3]->size());
    player.setMaterial (std::make_unique<ampsim::PreviewPlayer::Material> (*material));
}

void ToneMatchSession::applySongPlayerSettings()
{
    auto& player = ampSim.getPreviewPlayer();
    player.setSource (getSongSource() == songGuitar ? 4 : 3);
    player.setLevelDb (songLevelDb);
    player.setClickLevelDb (clickLevelDb);
    player.setSourceGainDb (3, 0.0f);
    player.setSourceGainDb (4, 0.0f);
    player.setMuteLive (false); // the song is for playing along to: your guitar stays in
}

void ToneMatchSession::setTargetPlaying (bool shouldPlay)
{
    auto& player = ampSim.getPreviewPlayer();
    if (! shouldPlay || target.empty() || takeRunning)
    {
        if (targetPlaying)
            player.setPlaying (false);
        targetPlaying = false;
        return;
    }
    if (previewPlaying)
        previewPlaying = false; // the A/B stops; this takes the player
    if (! sameSeconds (songRangeStart, rangeStart) || ! sameSeconds (songRangeEnd, rangeEnd) || songTargetVersion != targetVersion
        || songStem != hasGuitarStem() || material->audio[3] == nullptr)
        refreshSongMaterial();
    publishMaterial();
    applySongPlayerSettings();
    player.setOnce (false);
    player.setCountIn (countInForPlay ? countInBeats : 0, 60.0 * 48000.0 / getCountInBpm());
    player.startFresh();
    targetPlaying = true;
}

bool ToneMatchSession::startPlayAlong()
{
    if (target.empty() || running || takeRunning)
        return false;
    if (recordingReference)
    {
        ampSim.getDiRecorder().stop();
        recordingReference = false;
    }
    previewPlaying = false;
    targetPlaying = false;
    error.clear();

    refreshSongMaterial();
    publishMaterial();

    // The alignment (docs/TONE_MATCH.md, "Play along"). The player's song sample p sounds at the output
    // (the callback's sample) t0 + p, reaches the ears the output latency later, and what the guitarist
    // plays against it reaches the callback the input latency after that: at t0 + p + L, L = input +
    // output latency (+ the user's offset). The recorder starts at t0 (the song's first sample), so the
    // take's sample p + L is the guitar played against song sample p: the reference is the take from L on,
    // the section's length (the recorder runs L past the section's end to catch its last notes).
    take = {};
    take.latency = latencySource != nullptr ? latencySource() : platform::device::Latency {};
    take.offsetMs = latencyOffsetMs;
    take.alignSamples = std::max<int64_t> (0, (int64_t) take.latency.inputSamples + take.latency.outputSamples + std::llround (latencyOffsetMs * 48.0));
    take.sectionSamples = (int64_t) material->audio[3]->size();
    take.rangeStart = rangeStart;
    take.rangeEnd = rangeEnd;
    take.targetVersion = targetVersion;
    take.songSource = getSongSource();
    take.countIn = countInForTake;
    take.countInBeats = countInForTake ? countInBeats : 0;
    take.countInBpm = getCountInBpm();
    takeNeeded = take.sectionSamples + take.alignSamples;

    auto& player = ampSim.getPreviewPlayer();
    applySongPlayerSettings();
    player.setOnce (true);
    player.setCountIn (take.countInBeats, 60.0 * 48000.0 / take.countInBpm);
    ampSim.getDiRecorder().arm ((int) takeNeeded); // before the start, which releases it to the audio thread
    player.startFresh();
    takeRunning = true;
    return true;
}

void ToneMatchSession::finishTake (bool complete)
{
    if (! takeRunning)
        return;
    takeRunning = false;
    auto& recorder = ampSim.getDiRecorder();
    recorder.stop();
    ampSim.getPreviewPlayer().setPlaying (false);
    publishMaterial(); // anything the comparison rendered during the take

    take.raw = recorder.getRecording();
    take.complete = complete && (int64_t) take.raw.size() >= takeNeeded;
    // The reference: the take from the aligned start, at most the section's length.
    const auto from = (size_t) std::min<int64_t> (take.alignSamples, (int64_t) take.raw.size());
    const auto to = (size_t) std::min<int64_t> (take.alignSamples + take.sectionSamples, (int64_t) take.raw.size());
    if (to - from < (size_t) (minReferenceSeconds * 48000.0))
    {
        error = "The take was too short to match (at least " + juce::String ((int) minReferenceSeconds) + " s of the section)";
        return;
    }
    setReferenceSignal (std::vector<float> (take.raw.begin() + (std::ptrdiff_t) from, take.raw.begin() + (std::ptrdiff_t) to), "Play-along take");
    take.valid = true;
    ++takeNumber;
    // The mode stays as it is: Anything is the default after a take too since Round 2 (tone_bench: Anything beat
    // Same part on 38 of 50 cases); the take still lines up, so the cleanup can run in either.
}

bool ToneMatchSession::referenceIsTake() const
{
    return take.valid && take.targetVersion == targetVersion && sameSeconds (take.rangeStart, rangeStart) && sameSeconds (take.rangeEnd, rangeEnd);
}

// ---- Saving a take ------------------------------------------------------------------------------------------

namespace
{
/// 48 kHz mono, 32-bit float (so nothing is rounded on the way to the prototype).
bool writeMonoWav (const juce::File& file, const std::vector<float>& x)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
        return false;
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                                      .withSampleRate (48000.0)
                                                                      .withNumChannels (1)
                                                                      .withBitsPerSample (32)
                                                                      .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
        return false;
    const float* channels[] = { x.data() };
    return writer->writeFromFloatArrays (channels, 1, (int) x.size()) && writer->flush();
}
} // namespace

bool ToneMatchSession::canSaveTake() const
{
    return ! takeRunning && ! recordingReference && referenceIsTake() && ! take.raw.empty() && ! reference.empty();
}

juce::File ToneMatchSession::defaultTakesFolder()
{
    return platform::userDataFolder().getChildFile ("ToneMatchTakes");
}

juce::File ToneMatchSession::saveTake (const juce::File& parent)
{
    if (! canSaveTake())
    {
        error = "There's no play-along take of this section to save";
        return {};
    }
    auto name = juce::File::createLegalFileName (juce::File::createFileWithoutCheckingPath ("/" + targetName).getFileNameWithoutExtension()).trim();
    if (name.isEmpty())
        name = "target";
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    auto folder = parent.getChildFile (name + "-" + stamp);
    for (int i = 2; folder.exists(); ++i)
        folder = parent.getChildFile (name + "-" + stamp + "-" + juce::String (i));
    if (! folder.createDirectory())
    {
        error = "Couldn't create " + folder.getFullPathName();
        return {};
    }

    const auto stem = hasGuitarStem();
    bool ok = writeMonoWav (folder.getChildFile ("target.wav"), targetSelection()) && writeMonoWav (folder.getChildFile ("di_raw.wav"), take.raw)
              && writeMonoWav (folder.getChildFile ("di.wav"), reference);
    if (ok && stem)
        ok = writeMonoWav (folder.getChildFile ("stem.wav"), *comparedRawTarget);

    // How the take was lined up (docs/TONE_MATCH.md, "Lining it up"), and the match's DTW path if a same-part
    // match of exactly this take exists (the path is against the winning slot's render at Gain 0, in analysis
    // frames: frame f covers samples f hop .. f hop + fftSize).
    auto* root = new juce::DynamicObject();
    juce::var json (root);
    root->setProperty ("format", "bellydsp-tone-match-take");
    root->setProperty ("version", 1);
    root->setProperty ("app_version", platform::appVersion());
    root->setProperty ("saved", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("sample_rate", 48000);
    root->setProperty ("target_file", targetName);
    root->setProperty ("range_start_s", rangeStart);
    root->setProperty ("range_end_s", rangeEnd);
    juce::var files (new juce::DynamicObject());
    files.getDynamicObject()->setProperty ("target", "target.wav");
    files.getDynamicObject()->setProperty ("stem", stem ? juce::var ("stem.wav") : juce::var());
    files.getDynamicObject()->setProperty ("di_raw", "di_raw.wav");
    files.getDynamicObject()->setProperty ("di", "di.wav");
    root->setProperty ("files", files);
    root->setProperty ("section_samples", (juce::int64) take.sectionSamples);
    root->setProperty ("align_samples", (juce::int64) take.alignSamples);
    root->setProperty ("latency_ms", (double) take.alignSamples / 48.0);
    root->setProperty ("complete", take.complete);
    juce::var latency (new juce::DynamicObject());
    latency.getDynamicObject()->setProperty ("known", take.latency.known);
    latency.getDynamicObject()->setProperty ("input_samples", take.latency.inputSamples);
    latency.getDynamicObject()->setProperty ("output_samples", take.latency.outputSamples);
    latency.getDynamicObject()->setProperty ("buffer_size", take.latency.bufferSize);
    latency.getDynamicObject()->setProperty ("device_sample_rate", take.latency.sampleRate);
    root->setProperty ("device_latency", latency);
    root->setProperty ("offset_ms", take.offsetMs);
    juce::var countIn (new juce::DynamicObject());
    countIn.getDynamicObject()->setProperty ("on", take.countIn);
    countIn.getDynamicObject()->setProperty ("beats", take.countInBeats);
    countIn.getDynamicObject()->setProperty ("bpm", take.countInBpm);
    root->setProperty ("count_in", countIn);
    root->setProperty ("song_source", take.songSource == songGuitar ? "guitar" : "full");
    root->setProperty ("band_seconds", ampsim::tonematch::playAlongBandSeconds);
    const auto matchedThisTake = resultReady && result.mode == Mode::samePart && comparedReference != nullptr && *comparedReference == reference
                                 && matchedTargetVersion == targetVersion && sameSeconds (matchedRangeStart, rangeStart) && sameSeconds (matchedRangeEnd, rangeEnd);
    if (matchedThisTake && ! result.alignmentPath.empty())
    {
        juce::var dtw (new juce::DynamicObject());
        dtw.getDynamicObject()->setProperty ("fft_size", ampsim::tonematch::fftSize);
        dtw.getDynamicObject()->setProperty ("hop", ampsim::tonematch::hop);
        dtw.getDynamicObject()->setProperty ("slot", result.slot);
        juce::Array<juce::var> path;
        path.ensureStorageAllocated ((int) result.alignmentPath.size());
        for (const auto& [i, j] : result.alignmentPath)
            path.add (juce::Array<juce::var> { i, j });
        dtw.getDynamicObject()->setProperty ("path", path);
        root->setProperty ("dtw", dtw);
    }
    else
        root->setProperty ("dtw", juce::var());
    ok = ok && folder.getChildFile ("take.json").replaceWithText (juce::JSON::toString (json, juce::JSON::FormatOptions {}.withSpacing (juce::JSON::Spacing::multiLine)));
    if (! ok)
    {
        error = "Couldn't write the take into " + folder.getFullPathName();
        return {};
    }
    error.clear();
    return folder;
}

int ToneMatchSession::getCountInRemaining() const
{
    return (takeRunning || targetPlaying) ? ampSim.getPreviewPlayer().getCountInRemaining() : 0;
}

double ToneMatchSession::getSectionPlayheadSeconds() const
{
    if (! (takeRunning || targetPlaying))
        return -1.0;
    const auto& player = ampSim.getPreviewPlayer();
    const auto source = player.getSoundingSource();
    if (source != 3 && source != 4)
        return -1.0;
    return (double) player.getPlayheadPosition() / 48000.0;
}

// ---- The tempo suggestion ---------------------------------------------------------------------------------

void ToneMatchSession::startTempoEstimate()
{
    joinTempo();
    tempoRangeStart = rangeStart;
    tempoRangeEnd = rangeEnd;
    tempoTargetVersion = targetVersion;
    suggestedBpm = 0.0;
    tempoCancel = false;
    tempoRunning = true;
    tempoWorker = std::thread ([this, x = targetSelection()] {
        const auto t = ampsim::tonematch::estimateTempo (x.data(), (int) x.size(), 48000.0, tempoCancel);
        if (! tempoCancel.load())
            suggestedBpm = t.confident ? t.bpm : 0.0;
        tempoRunning = false;
    });
}

void ToneMatchSession::joinTempo()
{
    if (tempoWorker.joinable())
    {
        tempoCancel = true;
        tempoWorker.join();
    }
}

bool ToneMatchSession::waitForTempo (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        poll();
        if (! tempoRunning && sameSeconds (tempoRangeStart, rangeStart) && sameSeconds (tempoRangeEnd, rangeEnd) && tempoTargetVersion == targetVersion)
            return true;
        juce::Thread::sleep (5);
    }
    return false;
}

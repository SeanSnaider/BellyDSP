// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatchSession.h"

#include "BlockParameters.h"
#include "platform/AppInfo.h"
#include "dsp/Loudness.h"
#include "tonematch/AudioFileInput.h"

#include <cmath>
#include <limits>

using namespace ampsim::tonematch;

namespace
{
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

ToneMatchSession::ToneMatchSession (AmpSimProcessor& processor) : ampSim (processor)
{
    loudness.fill (-std::numeric_limits<double>::infinity());
}

ToneMatchSession::~ToneMatchSession()
{
    cancel();
    join();
    cancelCompare();
    joinCompare();
    ampSim.getPreviewPlayer().setPlaying (false); // the live guitar comes back; the material stays until the next
    if (recordingReference)
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
    target = std::move (samples48k);
    targetName = name;
    ++targetVersion;
    error.clear();
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
    rangeStart = juce::jlimit (0.0, length - span, startSeconds);
    rangeEnd = rangeStart + span;
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
    if (! recordingReference)
        return;
    auto& recorder = ampSim.getDiRecorder();
    recorder.stop();
    recordingReference = false;
    setReferenceSignal (recorder.getRecording(), "Recorded DI");
}

bool ToneMatchSession::isRecording() const
{
    return recordingReference;
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
    if (recordingReference)
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

    stopAndClearCompare();
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

    worker = std::thread ([this, settings, targetCopy = targetSelection(), referenceCopy = reference, doSeparate = separate, sep = separator] {
        auto targetSignal = targetCopy;
        MatchResult r;
        const auto separationShare = doSeparate ? 0.5 : 0.0;
        auto report = [this] (double from, double to) {
            return [this, from, to] (double f, const juce::String& s) {
                progress = from + (to - from) * f;
                const std::lock_guard<std::mutex> l (lock);
                stage = s;
            };
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
        if (! targetSignal.empty())
        {
            r = ToneMatcher::match (targetSignal, referenceCopy, settings, cancelFlag, report (separationShare, 1.0));
            if (doSeparate && ! r.ok && ! r.cancelled && r.error.startsWith ("The target has too little playing"))
                r.error = "The separated guitar has too little playing in it: the separation found almost no guitar in this section. "
                          "Choose a section where the guitar plays, or switch separation off.";
        }

        {
            const std::lock_guard<std::mutex> l (lock);
            pending = std::move (r);
            pendingTarget = std::move (targetSignal);
            pendingReference = referenceCopy;
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
                comparedReference = std::make_shared<const std::vector<float>> (std::move (pendingReference));
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
    return source == sourceTarget ? "Target" : source == sourceMatch ? "Match" : "Current";
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
            // (each frame's mean partner on the other clock, at the analysis frames' centres).
            auto m = std::make_unique<ampsim::PreviewPlayer::Material>();
            m->audio = { comparedTarget, matchAudio, currentAudio };
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
            material = std::make_unique<ampsim::PreviewPlayer::Material> (*m);
            compareReady = true;
            compareError.clear();
            updateLoops();
            player.setMaterial (std::move (m));
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
    material.reset();
    loudness.fill (-std::numeric_limits<double>::infinity());
    matchGainDb.fill (0.0f);
}

const std::vector<float>& ToneMatchSession::getCompareAudio (int source) const
{
    static const std::vector<float> none;
    const auto& p = source == sourceTarget ? comparedTarget : source == sourceMatch ? matchAudio : currentAudio;
    return p != nullptr ? *p : none;
}

void ToneMatchSession::setPreviewPlaying (bool shouldPlay)
{
    previewPlaying = shouldPlay && compareReady;
    auto& player = ampSim.getPreviewPlayer();
    player.setMuteLive (muteLive);
    player.setLevelDb (previewLevelDb);
    player.setSource (previewSource);
    player.setPlaying (previewPlaying);
}

void ToneMatchSession::setPreviewSource (int source)
{
    previewSource = juce::jlimit (0, numSources - 1, source);
    ampSim.getPreviewPlayer().setSource (previewSource);
}

void ToneMatchSession::setLevelMatch (bool on)
{
    levelMatch = on;
    updateLevels();
}

void ToneMatchSession::setMuteLive (bool on)
{
    muteLive = on;
    ampSim.getPreviewPlayer().setMuteLive (on);
}

void ToneMatchSession::setPreviewLevelDb (float db)
{
    previewLevelDb = juce::jlimit (minPreviewLevelDb, maxPreviewLevelDb, db);
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
    if (material != nullptr && material->aligned)
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
    for (int s = 0; s < numSources; ++s)
    {
        const auto& audio = getCompareAudio (s);
        const auto from = s == sourceTarget ? a : diLoop.first, to = s == sourceTarget ? b : diLoop.second;
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
    for (int s = 0; s < numSources; ++s)
    {
        const auto l = loudness[(size_t) s];
        matchGainDb[(size_t) s] = levelMatch && std::isfinite (l) && std::isfinite (reference) ? (float) juce::jlimit (-40.0, 40.0, reference - l) : 0.0f;
        ampSim.getPreviewPlayer().setSourceGainDb (s, matchGainDb[(size_t) s]);
    }
}

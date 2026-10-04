// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "ToneMatchSession.h"

#include "BlockParameters.h"
#include "platform/AppInfo.h"
#include "tonematch/AudioFileInput.h"

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

ToneMatchSession::ToneMatchSession (AmpSimProcessor& processor) : ampSim (processor) {}

ToneMatchSession::~ToneMatchSession()
{
    cancel();
    join();
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
        const std::lock_guard<std::mutex> l (lock);
        if (pending.ok)
        {
            result = pending;
            resultReady = true;
        }
        else
            error = pending.error;
    }
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
    resultReady = false;
    result = {};
}

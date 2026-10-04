// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "NamAmp.h"
#include "Loudness.h"
#include "ReferenceSignals.h"

#include <NAM/get_dsp.h>

#include <cmath>
#include <filesystem>

namespace ampsim
{

namespace
{
/// Parses a .nam, checks it suits an amp slot (mono, 48 kHz), and sizes its buffers. Reset() also prewarms
/// it: runs silence through until its receptive field (the window of past samples each output sample
/// depends on) holds settled history, so the model doesn't start with a burst.
std::unique_ptr<nam::DSP> openModel (const juce::File& file, int maxBlock, juce::String& error)
{
    const auto name = file.getFileName();
    std::unique_ptr<nam::DSP> dsp;

    try
    {
        // Skip get_dsp()'s own prewarm: Reset() below prewarms once we know the block size.
        nam::DspLoadOptions options;
        options.prewarm = false;
        dsp = nam::get_dsp (std::filesystem::path (file.getFullPathName().toStdString()), options);
    }
    catch (const std::exception& e)
    {
        error = "Couldn't load " + name + ": " + juce::String::fromUTF8 (e.what());
        return {};
    }

    if (dsp == nullptr)
    {
        error = "Couldn't load " + name;
        return {};
    }

    if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1)
    {
        error = name + " has " + juce::String (dsp->NumInputChannels()) + " input and " + juce::String (dsp->NumOutputChannels())
              + " output channels, but amp slots need a mono model";
        return {};
    }

    const auto trainedRate = dsp->GetExpectedSampleRate();
    if (trainedRate > 0.0 && std::abs (trainedRate - NamAmp::requiredSampleRate) > 0.5 && ! dsp->SupportsArbitrarySampleRate())
    {
        error = name + " was trained at " + juce::String (juce::roundToInt (trainedRate)) + " Hz, but amp slots run at 48000 Hz";
        return {};
    }

    dsp->Reset (NamAmp::requiredSampleRate, maxBlock);
    return dsp;
}

/// Runs a model over a buffer, never handing it more frames than its buffers were sized for (a model
/// queued before the most recent prepare() can be sized for a smaller block).
void runModel (nam::DSP& dsp, float* input, float* output, int numSamples) noexcept
{
    const int chunk = juce::jmax (1, dsp.GetMaxBufferSize());

    for (int start = 0; start < numSamples; start += chunk)
    {
        NAM_SAMPLE* in = input + start;
        NAM_SAMPLE* out = output + start;
        dsp.process (&in, &out, juce::jmin (chunk, numSamples - start));
    }
}

/// Offline (loader thread): the model's output for `input` times `gain`, from its current state.
std::vector<float> renderOffline (nam::DSP& dsp, const std::vector<float>& input, float gain)
{
    std::vector<float> in (input), out (input.size());
    juce::FloatVectorOperations::multiply (in.data(), gain, (int) in.size());
    runModel (dsp, in.data(), out.data(), (int) in.size());
    return out;
}

NamAmp::LoadResult failed (const juce::String& error)
{
    NamAmp::LoadResult result;
    result.message = error;
    return result;
}

float dbToGain (float db) noexcept
{
    return std::exp (db * 0.11512925464970229f); // 10^(dB / 20) = e^(dB ln(10) / 20)
}

/// The single capture's output compensation (dB) at a trim, linear in dB between the measured points.
float compensationAt (const std::array<float, NamAmp::compensationPoints>& c, float trimDb) noexcept
{
    const auto x = juce::jlimit (0.0f, (float) (NamAmp::compensationPoints - 1),
                                 (trimDb + NamAmp::singleTrimRangeDb) / NamAmp::compensationStepDb);
    const auto i = juce::jmin (NamAmp::compensationPoints - 2, (int) x);
    return c[(size_t) i] + (x - (float) i) * (c[(size_t) i + 1] - c[(size_t) i]);
}

/// The blend's level correction for two step outputs of equal power and correlation rho. The blend is
/// y = (1 - a) x1 + a x2, so its power is
///     E[y^2] = sigma^2 ((1 - a)^2 + a^2 + 2 a (1 - a) rho),
/// which dips mid-blend by 10 log10((1 + rho) / 2) dB at a = 1/2 (0 dB for identical signals, -3 dB for
/// unrelated ones). Dividing by the square root restores sigma^2 exactly for any a. For rho = 1 the
/// correction is 1 and this is a plain linear crossfade; for rho = 0 it becomes an equal-power one.
float blendCorrection (float a, float rho) noexcept
{
    const auto b = 1.0f - a;
    return 1.0f / std::sqrt (b * b + a * a + 2.0f * a * b * rho);
}
} // namespace

NamAmp::~NamAmp()
{
    delete current;
    delete fadingOut;
    delete toRetire;
}

NamAmp::LoadResult NamAmp::loadModel (const juce::File& file, bool normalize)
{
    return loadModel (file, normalize, Calibration {});
}

NamAmp::LoadResult NamAmp::loadModel (const juce::File& file, bool normalize, const Calibration& calibration)
{
    const auto maxBlock = loaderMaxBlockSize.load();
    const auto isSet = file.isDirectory() || file.hasFileExtension ("json");
    std::vector<juce::File> files;
    GainSet set;
    LoadResult result;

    if (isSet)
    {
        juce::String error;
        if (! GainSet::read (file, set, error))
            return failed (error);
        for (const auto& s : set.steps)
            files.push_back (s.file);
        result.isGainSet = set.steps.size() > 1;
    }
    else
    {
        files.push_back (file);
    }

    auto model = std::make_unique<Model>();
    model->steps.resize (files.size());

    for (size_t i = 0; i < files.size(); ++i)
    {
        juce::String error;
        model->steps[i].dsp = openModel (files[i], maxBlock, error);
        if (model->steps[i].dsp == nullptr)
            return failed (error);
        model->steps[i].position = isSet ? (float) set.steps[i].gain : gainDefault;
        model->warmupSamples = juce::jmax (model->warmupSamples, model->steps[i].dsp->GetPrewarmSamples());
    }

    // The input calibration comes from the (first) capture's metadata: a set's steps were all captured on
    // the same rig, at the same input level.
    auto& first = *model->steps.front().dsp;
    result.ok = true;
    result.message = isSet ? set.name : file.getFileNameWithoutExtension();
    result.hasInputLevel = first.HasInputLevel();
    result.captureInputDbu = result.hasInputLevel ? first.GetInputLevel() : 0.0;

    if (calibration.enabled && result.hasInputLevel)
    {
        result.calibrationDb = calibration.interfaceInputDbu - result.captureInputDbu;
        model->inputGain = juce::Decibels::decibelsToGain ((float) result.calibrationDb, -1000.0f);
    }

    // Measure each model's loudness (BUILD_PLAN "Loudness matching"): render the reference guitar DI
    // through it and K-weight and gate the result (BS.1770). Every model gets the same signal and the
    // same method, so captures land at the same perceived level. A file's own loudness field can't
    // promise that, because it depends on whatever signal its trainer used. Then reset again so the
    // model starts clean.
    static const auto referenceDI = referenceGuitarDI ((int) (loudnessProbeSeconds * requiredSampleRate));
    std::vector<std::vector<float>> rendered;
    juce::StringArray notes;

    if (calibration.enabled && result.hasInputLevel)
        notes.add ("input calibrated " + juce::String (result.calibrationDb, 1) + " dB");

    for (auto& step : model->steps)
    {
        rendered.push_back (renderOffline (*step.dsp, referenceDI, model->inputGain)); // the input as process() scales it
        step.dsp->Reset (requiredSampleRate, maxBlock);
        const auto lufs = loudness::integratedMono (rendered.back().data(), (int) rendered.back().size(), requiredSampleRate);
        result.stepLufs.push_back (lufs);

        if (normalize && std::isfinite (lufs))
            step.normalizationGain = juce::Decibels::decibelsToGain ((float) (targetLoudnessLufs - lufs));
    }

    result.measuredLufs = result.stepLufs.front();
    if (normalize && std::isfinite (result.measuredLufs))
        result.normalizationDb = targetLoudnessLufs - result.measuredLufs;

    if (result.isGainSet)
    {
        // Each step's Gain position, and how alike neighbouring steps' outputs are: the Pearson correlation
        // of their (normalized) renders of the reference DI, after its first 0.1 s. The blend's level
        // correction uses it (blendCorrection). Clamped to [0, 1]: two steps of one amp don't anti-correlate.
        for (size_t i = 0; i < model->steps.size(); ++i)
            result.stepGains.push_back (model->steps[i].position);

        const auto skip = (size_t) (0.1 * requiredSampleRate);
        for (size_t i = 0; i + 1 < rendered.size(); ++i)
        {
            double xy = 0.0, xx = 0.0, yy = 0.0;
            for (size_t n = skip; n < rendered[i].size(); ++n)
            {
                xy += (double) rendered[i][n] * rendered[i + 1][n];
                xx += (double) rendered[i][n] * rendered[i][n];
                yy += (double) rendered[i + 1][n] * rendered[i + 1][n];
            }
            const auto rho = xx > 0.0 && yy > 0.0 ? xy / std::sqrt (xx * yy) : 1.0;
            result.stepCorrelation.push_back (rho);
            model->correlation.push_back ((float) juce::jlimit (0.0, 1.0, rho));
        }

        notes.add ("gain set, " + juce::String ((int) model->steps.size()) + " steps at "
                   + [&] { juce::StringArray g; for (auto v : result.stepGains) g.add (juce::String (v, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")); return g.joinIntoString (", "); }());
        if (normalize)
            notes.add ("each normalized to " + juce::String (targetLoudnessLufs, 0) + " LUFS");
    }
    else if (normalize && std::isfinite (result.measuredLufs))
    {
        // A single capture: its loudness at every 3 dB of trim, -24 to +24 dB, on one pass of the reference
        // DI's phrase, from a freshly reset model each time. The compensation at a trim is the loudness at
        // 0 dB minus the loudness there, so Gain keeps the normalized level (0 dB at unity trim exactly).
        static const auto probe = referenceGuitarDI ((int) (compensationProbeSeconds * requiredSampleRate));
        auto& dsp = *model->steps.front().dsp;
        std::array<double, compensationPoints> lufs {};

        for (int j = 0; j < compensationPoints; ++j)
        {
            const auto trimDb = -singleTrimRangeDb + compensationStepDb * (float) j;
            const auto out = renderOffline (dsp, probe, model->inputGain * juce::Decibels::decibelsToGain (trimDb));
            dsp.Reset (requiredSampleRate, maxBlock);
            lufs[(size_t) j] = loudness::integratedMono (out.data(), (int) out.size(), requiredSampleRate);
        }

        const auto unity = lufs[(size_t) compensationPoints / 2];
        for (int j = 0; j < compensationPoints; ++j)
        {
            const auto c = std::isfinite (lufs[(size_t) j]) && std::isfinite (unity) ? unity - lufs[(size_t) j] : 0.0;
            model->compensationDb[(size_t) j] = (float) juce::jlimit (-60.0, 60.0, c);
            result.compensationDb.push_back (model->compensationDb[(size_t) j]);
        }

        notes.add (juce::String (result.measuredLufs, 1) + " LUFS on the reference DI, normalized " + juce::String (result.normalizationDb, 1) + " dB");
    }
    else if (normalize)
    {
        notes.add ("silent on the reference DI, so not normalized");
    }

    if (! notes.isEmpty())
        result.message << " (" << notes.joinIntoString ("; ") << ")";

    handoff.publish (std::move (model));
    return result;
}

void NamAmp::clearModel()
{
    handoff.publish (std::make_unique<Model>()); // no steps: renders as passthrough
}

void NamAmp::prepare (double sampleRate, int maxBlockSize)
{
    inputCopy.assign ((size_t) maxBlockSize, 0.0f);
    outgoing.assign ((size_t) maxBlockSize, 0.0f);
    scaledInput.assign ((size_t) maxBlockSize, 0.0f);
    positions.assign ((size_t) maxBlockSize, 0.0f);
    stepOutputs.setSize (GainSet::maxSteps, maxBlockSize);
    stepOutputs.clear();
    fadeLength = juce::jmax (1, juce::roundToInt (sampleRate * switchFadeSeconds));
    slewPerSample = gainSlewPerSecond / (float) sampleRate;
    loaderMaxBlockSize = maxBlockSize;

    // prepare() never overlaps process(), so a waiting model can go live right away with no
    // fade, and anything finished with can be freed here directly.
    if (auto* next = handoff.take())
    {
        delete current;
        current = next;
    }

    delete fadingOut;
    fadingOut = nullptr;
    delete toRetire;
    toRetire = nullptr;
    fading = false;
    handoff.collect();

    // Resize the models' buffers for this block size, settle their history again, and put the Gain
    // straight at the knob.
    if (current != nullptr)
    {
        for (auto& step : current->steps)
            step.dsp->Reset (requiredSampleRate, maxBlockSize);
        startModel (*current);
    }
}

void NamAmp::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    pickUpNewModel();

    const auto numSamples = (int) block.getNumSamples();
    auto* io = block.getChannelPointer (0);

    // NAM reads its input and writes its output through separate pointers, so give it a copy of
    // the input instead of assuming in-place processing is safe.
    std::copy (io, io + numSamples, inputCopy.begin());
    render (current, inputCopy.data(), io, numSamples);

    if (! fading)
        return;

    render (fadingOut, inputCopy.data(), outgoing.data(), numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        // Equal-power crossfade. As t goes from 0 to 1, the incoming model's gain is sin(pi/2 t)
        // and the outgoing one's is cos(pi/2 t). Since sin^2 + cos^2 = 1, the summed power of
        // two uncorrelated signals (two different amps) stays constant, so the switch doesn't
        // dip in level halfway through the way a linear fade would.
        const auto t = juce::jmin (1.0f, (float) fadePosition / (float) fadeLength);
        const auto angle = juce::MathConstants<float>::halfPi * t;
        io[i] = io[i] * std::sin (angle) + outgoing[(size_t) i] * std::cos (angle);
        ++fadePosition;
    }

    finishSwitchIfDone();
}

void NamAmp::reset()
{
    if (fading)
    {
        fadePosition = fadeLength;
        finishSwitchIfDone();
    }
}

bool NamAmp::isGainMoving() const noexcept
{
    if (current == nullptr || current->steps.empty() || current->position < 0.0f)
        return false;
    const auto target = current->steps.size() > 1
                          ? juce::jlimit (current->steps.front().position, current->steps.back().position, gainTarget)
                          : gainTarget;
    return ! juce::exactlyEqual (current->position, target);
}

int NamAmp::getRunningSteps() const noexcept
{
    if (current == nullptr || current->steps.empty())
        return 0;
    if (current->steps.size() == 1)
        return 1;
    int running = 0;
    for (const auto& step : current->steps)
        running += step.running ? 1 : 0;
    return running;
}

void NamAmp::pickUpNewModel()
{
    if (toRetire != nullptr && handoff.retire (toRetire))
        toRetire = nullptr;

    // One switch at a time. A newer model waits in the mailbox until the last switch has fully
    // finished and the outgoing model has been handed back.
    if (fading || toRetire != nullptr)
        return;

    if (auto* next = handoff.take())
    {
        fadingOut = current; // nullptr for the first model, which then fades in from passthrough
        current = next;
        fading = true;
        fadePosition = 0;
    }
}

void NamAmp::finishSwitchIfDone()
{
    if (fadePosition < fadeLength)
        return;

    fading = false;

    if (fadingOut != nullptr && ! handoff.retire (fadingOut))
        toRetire = fadingOut;

    fadingOut = nullptr;
}

float NamAmp::slewTarget (float position, float target) const noexcept
{
    // A linear ramp at the slew rate, landing exactly on the target.
    const auto distance = target - position;
    return std::abs (distance) <= slewPerSample ? target : position + (distance > 0.0f ? slewPerSample : -slewPerSample);
}

void NamAmp::startModel (Model& model) noexcept
{
    // A capture's first buffer (or a prepare): the Gain jumps straight to the knob, and the step models it
    // needs there count as warm. They were prewarmed with silence on the loader thread, like any newly
    // loaded model, and the 20 ms switch fade covers their start.
    auto& steps = model.steps;
    if (steps.empty())
        return;

    if (steps.size() == 1)
    {
        model.position = gainTarget;
        steps.front().running = true;
        return;
    }

    model.position = juce::jlimit (steps.front().position, steps.back().position, gainTarget);
    for (auto& step : steps)
    {
        const auto needed = std::abs (step.position - model.position) < 1.0e-6f; // on a step
        step.running = needed;
        step.warmed = needed ? maxWarmed : 0;
    }

    // Between two steps: both.
    for (size_t k = 0; k + 1 < steps.size(); ++k)
        if (steps[k].position < model.position && model.position < steps[k + 1].position)
        {
            steps[k].running = steps[k + 1].running = true;
            steps[k].warmed = steps[k + 1].warmed = maxWarmed;
        }
}

void NamAmp::render (Model* model, const float* input, float* output, int numSamples)
{
    if (model == nullptr || model->steps.empty())
    {
        std::copy (input, input + numSamples, output); // no model: pass the input through
        return;
    }

    if (model->position < 0.0f)
        startModel (*model);

    if (model->steps.size() == 1)
        renderSingle (*model, input, output, numSamples);
    else
        renderSet (*model, input, output, numSamples);
}

void NamAmp::renderSingle (Model& model, const float* input, float* output, int numSamples)
{
    auto& step = model.steps.front();
    auto position = model.position;

    // The trim goes in front of the model and the compensation after it, both from the same smoothed Gain
    // position, sample by sample while it moves, so the level stays put as the drive changes.
    if (juce::exactlyEqual (position, gainTarget))
    {
        const auto trim = singleTrimDb (position);
        juce::FloatVectorOperations::multiply (scaledInput.data(), input, model.inputGain * dbToGain (trim), numSamples);
        runModel (*step.dsp, scaledInput.data(), output, numSamples);
        juce::FloatVectorOperations::multiply (output, step.normalizationGain * dbToGain (compensationAt (model.compensationDb, trim)), numSamples);
        return;
    }

    for (int n = 0; n < numSamples; ++n)
    {
        position = slewTarget (position, gainTarget);
        positions[(size_t) n] = position;
        scaledInput[(size_t) n] = input[n] * model.inputGain * dbToGain (singleTrimDb (position));
    }

    runModel (*step.dsp, scaledInput.data(), output, numSamples);

    for (int n = 0; n < numSamples; ++n)
        output[n] *= step.normalizationGain * dbToGain (compensationAt (model.compensationDb, singleTrimDb (positions[(size_t) n])));

    model.position = position;
}

void NamAmp::renderSet (Model& model, const float* input, float* output, int numSamples)
{
    auto& steps = model.steps;
    const int count = (int) steps.size();
    const auto target = juce::jlimit (steps.front().position, steps.back().position, gainTarget);
    auto position = model.position;

    // 1. The bracket: the step at or below the position (lo) and, between steps, the one above (hi). Both
    //    are running and warm: the position only ever moves where they are (step 3).
    int lo = 0;
    while (lo + 1 < count && steps[(size_t) lo + 1].position <= position)
        ++lo;
    const int hi = (lo + 1 < count && position > steps[(size_t) lo].position) ? lo + 1 : lo;

    // 2. What runs this buffer: the bracket, plus up to maxRunningSteps in all, the next steps the knob is
    //    heading to (a step joins once the target is past the one before it). Anything else stops, and
    //    starts cold (warmed = 0) if it's needed again: NAM's WaveNet is a finite-memory network, so a model
    //    that has run on live input for its receptive field (warmupSamples) has exactly the output it would
    //    have had running all along, and until then it's heard nowhere.
    int runLo = lo, runHi = hi;
    if (target > position)
        while (runHi + 1 < count && runHi - runLo + 1 < maxRunningSteps && target > steps[(size_t) runHi].position)
            ++runHi;
    else if (target < position)
        while (runLo > 0 && runHi - runLo + 1 < maxRunningSteps && target < steps[(size_t) runLo].position)
            --runLo;

    for (int k = 0; k < count; ++k)
    {
        auto& step = steps[(size_t) k];
        step.running = k >= runLo && k <= runHi;
        if (! step.running)
            step.warmed = 0;
    }

    // 3. How far the position may go: through the steps around the bracket that are warm. While the next
    //    step is still warming, the position waits at the last warm one (a pause of up to warmupSamples,
    //    85 ms for a standard WaveNet, at most once per move: later steps warm while it travels).
    int first = lo, last = hi;
    while (first > runLo && steps[(size_t) first - 1].warmed >= model.warmupSamples)
        --first;
    while (last < runHi && steps[(size_t) last + 1].warmed >= model.warmupSamples)
        ++last;
    const auto reachable = juce::jlimit (steps[(size_t) first].position, steps[(size_t) last].position, target);

    for (int n = 0; n < numSamples; ++n)
    {
        position = slewTarget (position, reachable);
        positions[(size_t) n] = position;
    }

    // 4. Run every running step on the (calibrated) input, warm or warming.
    juce::FloatVectorOperations::multiply (scaledInput.data(), input, model.inputGain, numSamples);
    for (int k = runLo; k <= runHi; ++k)
    {
        auto& step = steps[(size_t) k];
        runModel (*step.dsp, scaledInput.data(), stepOutputs.getWritePointer (k), numSamples);
        step.warmed = (int) juce::jmin ((juce::int64) maxWarmed, (juce::int64) step.warmed + numSamples);
    }

    // 5. The blend. Between steps i and i + 1, at a = (p - s_i) / (s_{i+1} - s_i):
    //        y = c(a) ((1 - a) m_i(x) + a m_{i+1}(x)),
    //    a linear crossfade of the two normalized outputs, because neighbouring steps of one amp are highly
    //    correlated (measured on the built-ins: rho = 0.88 to 0.99), with c(a) the level correction for
    //    their measured correlation (blendCorrection), so the level holds mid-blend too. On a step, a is 0
    //    and only that step is read.
    int segment = juce::jmin (lo, count - 2);
    for (int n = 0; n < numSamples; ++n)
    {
        const auto p = positions[(size_t) n];
        while (segment + 1 < count - 1 && p >= steps[(size_t) segment + 1].position)
            ++segment;
        while (segment > 0 && p < steps[(size_t) segment].position)
            --segment;

        const auto& s0 = steps[(size_t) segment];
        const auto& s1 = steps[(size_t) segment + 1];
        const auto a = juce::jlimit (0.0f, 1.0f, (p - s0.position) / (s1.position - s0.position));

        if (a <= 0.0f)
            output[n] = s0.normalizationGain * stepOutputs.getSample (segment, n);
        else if (a >= 1.0f)
            output[n] = s1.normalizationGain * stepOutputs.getSample (segment + 1, n);
        else
            output[n] = blendCorrection (a, model.correlation[(size_t) segment])
                        * ((1.0f - a) * s0.normalizationGain * stepOutputs.getSample (segment, n)
                           + a * s1.normalizationGain * stepOutputs.getSample (segment + 1, n));
    }

    model.position = position;
}

} // namespace ampsim

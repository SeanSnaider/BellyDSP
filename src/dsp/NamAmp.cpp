#include "NamAmp.h"
#include "Loudness.h"
#include "ReferenceSignals.h"

#include <NAM/get_dsp.h>

#include <cmath>
#include <filesystem>

namespace ampsim
{

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
        return { false, "Couldn't load " + name + ": " + juce::String::fromUTF8 (e.what()) };
    }

    if (dsp == nullptr)
        return { false, "Couldn't load " + name };

    if (dsp->NumInputChannels() != 1 || dsp->NumOutputChannels() != 1)
        return { false, name + " has " + juce::String (dsp->NumInputChannels()) + " input and "
                            + juce::String (dsp->NumOutputChannels())
                            + " output channels, but amp slots need a mono model" };

    const auto trainedRate = dsp->GetExpectedSampleRate();
    if (trainedRate > 0.0 && std::abs (trainedRate - requiredSampleRate) > 0.5
        && ! dsp->SupportsArbitrarySampleRate())
        return { false, name + " was trained at " + juce::String (juce::roundToInt (trainedRate))
                            + " Hz, but amp slots run at 48000 Hz" };

    // Reset() sizes the model's internal buffers for our largest block, then prewarms it: runs
    // silence through until its receptive field (the window of past samples each output sample
    // depends on) holds settled history, so the model doesn't start with a burst.
    const auto maxBlock = loaderMaxBlockSize.load();
    dsp->Reset (requiredSampleRate, maxBlock);

    auto model = std::make_unique<Model>();
    LoadResult result { true, file.getFileNameWithoutExtension() };
    result.hasInputLevel = dsp->HasInputLevel();
    result.captureInputDbu = result.hasInputLevel ? dsp->GetInputLevel() : 0.0;

    if (calibration.enabled && result.hasInputLevel)
    {
        result.calibrationDb = calibration.interfaceInputDbu - result.captureInputDbu;
        model->inputGain = juce::Decibels::decibelsToGain ((float) result.calibrationDb, -1000.0f);
    }

    // Measure the model's loudness (BUILD_PLAN "Loudness matching"): render the reference guitar DI
    // through it and K-weight and gate the result (BS.1770). Every model gets the same signal and the
    // same method, so captures land at the same perceived level. A file's own loudness field can't
    // promise that, because it depends on whatever signal its trainer used. Then reset again so the
    // model starts clean.
    static const auto referenceDI = referenceGuitarDI ((int) (loudnessProbeSeconds * requiredSampleRate));
    std::vector<float> input (referenceDI), rendered (referenceDI.size());
    juce::FloatVectorOperations::multiply (input.data(), model->inputGain, (int) input.size()); // as process() will

    for (size_t start = 0; start < input.size(); start += (size_t) maxBlock)
    {
        const auto len = (int) std::min ((size_t) maxBlock, input.size() - start);
        NAM_SAMPLE* in = input.data() + start;
        NAM_SAMPLE* out = rendered.data() + start;
        dsp->process (&in, &out, len);
    }

    dsp->Reset (requiredSampleRate, maxBlock);

    result.measuredLufs = loudness::integratedMono (rendered.data(), (int) rendered.size(), requiredSampleRate);
    juce::StringArray notes;

    if (calibration.enabled && result.hasInputLevel)
        notes.add ("input calibrated " + juce::String (result.calibrationDb, 1) + " dB");

    if (normalize && std::isfinite (result.measuredLufs))
    {
        result.normalizationDb = targetLoudnessLufs - result.measuredLufs;
        model->normalizationGain = juce::Decibels::decibelsToGain ((float) result.normalizationDb);
        notes.add (juce::String (result.measuredLufs, 1) + " LUFS on the reference DI, normalized " + juce::String (result.normalizationDb, 1) + " dB");
    }
    else if (normalize)
    {
        notes.add ("silent on the reference DI, so not normalized");
    }

    if (! notes.isEmpty())
        result.message << " (" << notes.joinIntoString ("; ") << ")";

    model->dsp = std::move (dsp);
    handoff.publish (std::move (model));
    return result;
}

void NamAmp::clearModel()
{
    handoff.publish (std::make_unique<Model>()); // no dsp: renders as passthrough
}

void NamAmp::prepare (double sampleRate, int maxBlockSize)
{
    inputCopy.assign ((size_t) maxBlockSize, 0.0f);
    outgoing.assign ((size_t) maxBlockSize, 0.0f);
    scaledInput.assign ((size_t) maxBlockSize, 0.0f);
    fadeLength = juce::jmax (1, juce::roundToInt (sampleRate * switchFadeSeconds));
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

    // Resize the model's buffers for this block size and settle its history again.
    if (current != nullptr && current->dsp != nullptr)
        current->dsp->Reset (requiredSampleRate, maxBlockSize);
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

void NamAmp::render (Model* model, const float* input, float* output, int numSamples)
{
    if (model == nullptr || model->dsp == nullptr)
    {
        std::copy (input, input + numSamples, output); // no model: pass the input through
        return;
    }

    auto& dsp = *model->dsp;

    // The input calibration. Each model has its own, and during a switch two models share one input,
    // so scale a copy rather than the input itself.
    juce::FloatVectorOperations::multiply (scaledInput.data(), input, model->inputGain, numSamples);

    // A model queued before the most recent prepare() can have buffers sized for a smaller block,
    // so never hand it more frames than it was sized for.
    const int chunk = juce::jmax (1, dsp.GetMaxBufferSize());

    for (int start = 0; start < numSamples; start += chunk)
    {
        NAM_SAMPLE* in = scaledInput.data() + start;
        NAM_SAMPLE* out = output + start;
        dsp.process (&in, &out, juce::jmin (chunk, numSamples - start));
    }

    juce::FloatVectorOperations::multiply (output, model->normalizationGain, numSamples);
}

} // namespace ampsim

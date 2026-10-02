#include "DriveEngine.h"

#include <algorithm>
#include <cmath>

namespace ampsim
{

namespace
{
int validFactor (int f) { return f >= 8 ? 8 : f >= 4 ? 4 : f >= 2 ? 2 : 1; }
int stagesOf (int f) { return f >= 8 ? 3 : f >= 4 ? 2 : f >= 2 ? 1 : 0; }
} // namespace

void DriveEngine::addCircuit (std::unique_ptr<drive::Circuit> circuit)
{
    jassert (numCircuits < maxCircuits);
    circuits[(size_t) numCircuits++] = std::move (circuit);
}

void DriveEngine::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    for (int s = 0; s < 4; ++s)
        groupDelays[(size_t) s] = oversampling::groupDelaySamples (1 << s);

    for (auto* s : { &drive, &tone, &mix })
        s->reset (sampleRate, smoothingSeconds);
    level.reset (sampleRate, smoothingSeconds);
    tightHz.reset (sampleRate, smoothingSeconds);
    factorGainStep = 1.0 / (factorFadeSeconds * sampleRate);
    factorGain = 1.0;
    warmupSamples = juce::roundToInt (warmupSeconds * sampleRate);

    setSettings (settings);
    applyFactor (validFactor (settings.oversampling));
}

void DriveEngine::applyFactor (int newFactor) noexcept
{
    factor = newFactor;
    upsampler.setFactor (factor);
    wetDownsampler.setFactor (factor);
    dryDownsampler.setFactor (factor);
    for (int c = 0; c < numCircuits; ++c)
        circuits[(size_t) c]->setSampleRate (sampleRate * factor);
    positionStep = 1.0 / (switchSeconds * sampleRate * factor);
    reset();
}

void DriveEngine::reset()
{
    upsampler.reset();
    wetDownsampler.reset();
    dryDownsampler.reset();
    tight.reset();

    // Start on the current settings, no ramps: the selected circuit fully on, the others idle.
    for (auto* s : { &drive, &tone, &mix })
        s->setCurrentAndTargetValue (s->getTargetValue());
    level.setCurrentAndTargetValue (level.getTargetValue());
    tightHz.setCurrentAndTargetValue (tightHz.getTargetValue());
    for (int c = 0; c < numCircuits; ++c)
    {
        const bool selected = c == settings.circuit;
        position[(size_t) c] = target[(size_t) c] = selected ? 1.0 : 0.0;
        running[(size_t) c] = selected;
        circuits[(size_t) c]->reset();
    }
    warming = -1;
    warmupRemaining = 0;
    updateControls (true);
}

void DriveEngine::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    settings.circuit = juce::jlimit (0, juce::jmax (0, numCircuits - 1), settings.circuit);

    drive.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.drive));
    tone.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.tone));
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));
    level.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-60.0, 24.0, (double) settings.levelDb)));
    tightHz.setTargetValue (juce::jlimit ((double) tightOffHz, (double) maxTightHz, (double) settings.tightHz));

    if (numCircuits == 0)
        return;

    // Switching circuits. One that's silent starts from rest and first runs unheard for warmupSeconds,
    // so its coupling capacitors settle on the signal (the slowest in the audio path, the Distortion's
    // 22 nF input cap into 1M, has a 22 ms time constant); then the 20 ms crossfade. One still audible
    // (switching back mid-fade) is warm, so the ramps just turn around.
    const auto wanted = settings.circuit;
    if (wanted == warming)
        return;
    if (juce::exactlyEqual (target[(size_t) wanted], 1.0))
    {
        abandonWarmup(); // back to where we were heading while another circuit warmed up
        return;
    }
    if (running[(size_t) wanted] && position[(size_t) wanted] > 0.0)
    {
        abandonWarmup();
        beginFade (wanted);
        return;
    }
    abandonWarmup();
    startCircuit (wanted);
    warming = wanted;
    warmupRemaining = warmupSamples;
}

void DriveEngine::abandonWarmup() noexcept
{
    if (warming >= 0 && position[(size_t) warming] <= 0.0)
        running[(size_t) warming] = false;
    warming = -1;
}

void DriveEngine::beginFade (int index) noexcept
{
    for (int c = 0; c < numCircuits; ++c)
        target[(size_t) c] = c == index ? 1.0 : 0.0;
    warming = -1;
}

void DriveEngine::startCircuit (int index) noexcept
{
    auto& c = *circuits[(size_t) index];
    c.reset();
    c.setControls (drive.getCurrentValue(), tone.getCurrentValue());
    running[(size_t) index] = true;
    position[(size_t) index] = 0.0;
}

bool DriveEngine::isSwitching() const noexcept
{
    if (warming >= 0)
        return true;
    for (int c = 0; c < numCircuits; ++c)
        if (running[(size_t) c] && ! juce::exactlyEqual (position[(size_t) c], target[(size_t) c]))
            return true;
    return validFactor (settings.oversampling) != factor || factorGain < 1.0;
}

double DriveEngine::groupDelaySamples() const noexcept
{
    return groupDelays[(size_t) stagesOf (factor)];
}

void DriveEngine::updateControls (bool force) noexcept
{
    const auto d = drive.getCurrentValue(), t = tone.getCurrentValue();
    if (force || ! juce::exactlyEqual (d, appliedDrive) || ! juce::exactlyEqual (t, appliedTone))
    {
        for (int c = 0; c < numCircuits; ++c)
            if (running[(size_t) c] || force)
                circuits[(size_t) c]->setControls (d, t);
        appliedDrive = d;
        appliedTone = t;
    }

    // The tight high-pass: a 12 dB/oct Butterworth at the oversampled rate, out of the path at 20 Hz.
    const auto f = tightHz.getCurrentValue();
    const bool on = f > tightOffHz + 1.0e-3;
    if (on && ! tightOn)
        tight.reset(); // coming back into the path: start from rest, not stale state
    tightOn = on;
    if (on && (force || ! juce::exactlyEqual (f, appliedTightHz)))
    {
        tight.setCoefficients (Svf::design (Svf::Type::highpass, f, 0.7071067811865476, 0.0, sampleRate * factor));
        appliedTightHz = f;
    }
}

void DriveEngine::process (float* samples, int numSamples) noexcept
{
    for (int start = 0; start < numSamples; start += coefficientInterval)
        processChunk (samples + start, juce::jmin (coefficientInterval, numSamples - start));
}

void DriveEngine::processChunk (float* x, int n) noexcept
{
    // An oversampling change waits until the fade-out has reached silence.
    const auto wantedFactor = validFactor (settings.oversampling);
    if (wantedFactor != factor && factorGain <= 0.0)
        applyFactor (wantedFactor);

    // Knobs: advance the smoothers over this chunk and redesign from where they land (every 32 samples).
    drive.skip (n);
    tone.skip (n);
    tightHz.skip (n);
    updateControls (false);

    const auto voltsPerUnit = settings.voltsAtFullScale;
    for (int i = 0; i < n; ++i)
        volts[(size_t) i] = (double) x[i] * voltsPerUnit;

    const int m = n * factor;
    upsampler.process (volts.data(), high.data(), n);
    dryDownsampler.process (high.data(), dry.data(), n); // the dry with the wet path's exact linear response

    if (tightOn)
        for (int j = 0; j < m; ++j)
            high[(size_t) j] = tight.processSample (high[(size_t) j]);

    int active = 0, only = 0;
    for (int c = 0; c < numCircuits; ++c)
        if (running[(size_t) c])
        {
            ++active;
            only = c;
        }

    if (active == 1 && juce::exactlyEqual (position[(size_t) only], 1.0) && juce::exactlyEqual (target[(size_t) only], 1.0))
    {
        circuits[(size_t) only]->process (high.data(), m);
        wetDownsampler.process (high.data(), wet.data(), n);
    }
    else
    {
        std::fill (wetHigh.begin(), wetHigh.begin() + m, 0.0);
        for (int c = 0; c < numCircuits; ++c)
        {
            if (! running[(size_t) c])
                continue;
            std::copy (high.begin(), high.begin() + m, scratch.begin());
            circuits[(size_t) c]->process (scratch.data(), m);

            // Equal-power crossfade from independent linear ramps: sin(pi/2 p).
            auto p = position[(size_t) c];
            const auto goal = target[(size_t) c];
            for (int j = 0; j < m; ++j)
            {
                p = goal > p ? juce::jmin (goal, p + positionStep) : juce::jmax (goal, p - positionStep);
                wetHigh[(size_t) j] += std::sin (juce::MathConstants<double>::halfPi * p) * scratch[(size_t) j];
            }
            position[(size_t) c] = p;
            if (juce::exactlyEqual (p, 0.0) && juce::exactlyEqual (goal, 0.0) && c != warming)
                running[(size_t) c] = false;
        }
        wetDownsampler.process (wetHigh.data(), wet.data(), n);
    }

    if (warming >= 0 && (warmupRemaining -= n) <= 0)
        beginFade (warming);

    // Blend, level, and back to samples.
    const auto unitsPerVolt = 1.0 / voltsPerUnit;
    const bool fadingOut = validFactor (settings.oversampling) != factor;
    for (int i = 0; i < n; ++i)
    {
        const auto w = mix.getNextValue();
        const auto g = level.getNextValue();
        factorGain = fadingOut ? juce::jmax (0.0, factorGain - factorGainStep) : juce::jmin (1.0, factorGain + factorGainStep);
        x[i] = (float) (factorGain * g * ((1.0 - w) * dry[(size_t) i] + w * wet[(size_t) i]) * unitsPerVolt);
    }
}

} // namespace ampsim

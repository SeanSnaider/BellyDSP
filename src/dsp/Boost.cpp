#include "Boost.h"

#include <algorithm>
#include <cmath>

namespace ampsim
{

namespace
{
constexpr double butterworthQ = 0.7071067811865476;
}

Boost::Boost()
{
    screamer.addCircuit (std::make_unique<drive::MidDriveCircuit>());
}

std::complex<double> Boost::cleanResponse (double tilt, double frequency, double fs)
{
    const auto shelf = Svf::design (Svf::Type::highShelf, tiltPivotHz, tiltQ, tilt, fs);
    return std::pow (10.0, -tilt / 40.0) * Svf::responseAt (shelf, frequency, fs);
}

std::complex<double> Boost::tightResponse (double hz, double mid, double frequency, double fs)
{
    const auto hp = Svf::design (Svf::Type::highpass, hz, butterworthQ, 0.0, fs);
    const auto bell = Svf::design (Svf::Type::peak, midPushHz, midPushQ, mid, fs);
    return Svf::responseAt (hp, frequency, fs) * Svf::responseAt (bell, frequency, fs);
}

void Boost::setSettings (const Settings& s)
{
    settings = s;
    level.setTargetValue (juce::Decibels::decibelsToGain (juce::jlimit (-24.0, 24.0, (double) s.levelDb)));
    // Rounded to 0.01 dB so a centred knob is exactly flat (and the block exactly transparent).
    tiltDb.setTargetValue (std::round (juce::jlimit (-12.0, 12.0, (double) s.tiltDb) * 100.0) / 100.0);
    tightHz.setTargetValue (juce::jlimit (20.0, 1000.0, (double) s.tightHz));
    midDb.setTargetValue (juce::jlimit (0.0, 12.0, (double) s.midDb));

    DriveEngine::Settings e;
    e.circuit = 0;
    e.drive = (float) screamerDrive;
    e.tone = (float) screamerTone;
    e.oversampling = s.oversampling;
    e.voltsAtFullScale = s.voltsAtFullScale;
    screamer.setSettings (e);

    // Switching modes works as in DriveEngine: a silent mode starts from rest and runs unheard for
    // DriveEngine::warmupSeconds before its 20 ms crossfade; an audible one just turns its ramp around.
    const auto wanted = (int) s.mode;
    if (wanted == warming)
        return;
    if (juce::exactlyEqual (target[(size_t) wanted], 1.0))
    {
        abandonWarmup();
        return;
    }
    if (running[(size_t) wanted] && position[(size_t) wanted] > 0.0)
    {
        abandonWarmup();
        beginFade (wanted);
        return;
    }
    abandonWarmup();
    startPath (wanted);
    warming = wanted;
    warmupRemaining = warmupSamples;
}

void Boost::abandonWarmup() noexcept
{
    if (warming >= 0 && position[(size_t) warming] <= 0.0)
        running[(size_t) warming] = false;
    warming = -1;
}

void Boost::beginFade (int path) noexcept
{
    for (int p = 0; p < numModes; ++p)
        target[(size_t) p] = p == path ? 1.0 : 0.0;
    warming = -1;
}

void Boost::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    for (auto* s : { &tiltDb, &midDb })
        s->reset (sampleRate, smoothingSeconds);
    level.reset (sampleRate, smoothingSeconds);
    tightHz.reset (sampleRate, smoothingSeconds);
    positionStep = 1.0 / (switchSeconds * sampleRate);
    warmupSamples = juce::roundToInt (DriveEngine::warmupSeconds * sampleRate);
    screamer.prepare (sampleRate, maxBlockSize);
    setSettings (settings);
    reset();
}

void Boost::reset()
{
    for (auto* s : { &tiltDb, &midDb })
        s->setCurrentAndTargetValue (s->getTargetValue());
    level.setCurrentAndTargetValue (level.getTargetValue());
    tightHz.setCurrentAndTargetValue (tightHz.getTargetValue());
    tilt.reset();
    tightHighPass.reset();
    midPush.reset();
    screamer.reset();
    for (size_t p = 0; p < (size_t) numModes; ++p)
    {
        const bool selected = p == (size_t) settings.mode;
        position[p] = target[p] = selected ? 1.0 : 0.0;
        running[p] = selected;
    }
    warming = -1;
    warmupRemaining = 0;
    updateFilters (true);
}

void Boost::startPath (int path) noexcept
{
    switch ((Mode) path)
    {
        case Mode::clean:    tilt.reset(); break;
        case Mode::tight:    tightHighPass.reset(); midPush.reset(); break;
        case Mode::screamer: screamer.reset(); break;
    }
    running[(size_t) path] = true;
    position[(size_t) path] = 0.0;
}

void Boost::updateFilters (bool force) noexcept
{
    const auto t = tiltDb.getCurrentValue();
    if (force || ! juce::exactlyEqual (t, appliedTilt))
    {
        tilt.setCoefficients (Svf::design (Svf::Type::highShelf, tiltPivotHz, tiltQ, t, sampleRate));
        tiltGain = std::pow (10.0, -t / 40.0); // -tilt/2 dB overall, so the pivot stays at 0 dB
        appliedTilt = t;
    }
    const auto hz = tightHz.getCurrentValue();
    if (force || ! juce::exactlyEqual (hz, appliedTight))
    {
        tightHighPass.setCoefficients (Svf::design (Svf::Type::highpass, hz, butterworthQ, 0.0, sampleRate));
        appliedTight = hz;
    }
    const auto mid = midDb.getCurrentValue();
    if (force || ! juce::exactlyEqual (mid, appliedMid))
    {
        midPush.setCoefficients (Svf::design (Svf::Type::peak, midPushHz, midPushQ, mid, sampleRate));
        appliedMid = mid;
    }
}

void Boost::runPath (int path, float* y, int n) noexcept
{
    switch ((Mode) path)
    {
        case Mode::clean:
        {
            // The shelf always runs, so its state is current when the tilt knob leaves 0.
            const bool flat = juce::exactlyEqual (tiltDb.getCurrentValue(), 0.0) && ! tiltDb.isSmoothing();
            for (int i = 0; i < n; ++i)
            {
                const auto shaped = tiltGain * tilt.processSample ((double) y[i]);
                if (! flat)
                    y[i] = (float) shaped;
            }
            break;
        }
        case Mode::tight:
            for (int i = 0; i < n; ++i)
                y[i] = (float) midPush.processSample (tightHighPass.processSample ((double) y[i]));
            break;
        case Mode::screamer:
            screamer.process (y, n);
            break;
    }
}

void Boost::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    auto* x = block.getChannelPointer (0);
    const auto numSamples = (int) block.getNumSamples();
    for (int start = 0; start < numSamples; start += coefficientInterval)
        processChunk (x + start, juce::jmin (coefficientInterval, numSamples - start));
}

void Boost::processChunk (float* x, int n) noexcept
{
    tiltDb.skip (n);
    midDb.skip (n);
    tightHz.skip (n);
    updateFilters (false);

    int active = 0, only = 0;
    for (int p = 0; p < numModes; ++p)
        if (running[(size_t) p])
        {
            ++active;
            only = p;
        }

    if (active == 1 && juce::exactlyEqual (position[(size_t) only], 1.0) && juce::exactlyEqual (target[(size_t) only], 1.0))
    {
        runPath (only, x, n);
    }
    else
    {
        std::copy (x, x + n, input.begin());
        std::fill (mixed.begin(), mixed.begin() + n, 0.0f);
        for (int path = 0; path < numModes; ++path)
        {
            if (! running[(size_t) path])
                continue;
            std::copy (input.begin(), input.begin() + n, pathOut.begin());
            runPath (path, pathOut.data(), n);
            auto p = position[(size_t) path];
            const auto goal = target[(size_t) path];
            for (int i = 0; i < n; ++i)
            {
                p = goal > p ? juce::jmin (goal, p + positionStep) : juce::jmax (goal, p - positionStep);
                mixed[(size_t) i] += (float) std::sin (juce::MathConstants<double>::halfPi * p) * pathOut[(size_t) i];
            }
            position[(size_t) path] = p;
            if (juce::exactlyEqual (p, 0.0) && juce::exactlyEqual (goal, 0.0) && path != warming)
                running[(size_t) path] = false;
        }
        std::copy (mixed.begin(), mixed.begin() + n, x);
    }

    if (warming >= 0 && (warmupRemaining -= n) <= 0)
        beginFade (warming);

    if (level.isSmoothing() || ! juce::exactlyEqual (level.getCurrentValue(), 1.0))
        for (int i = 0; i < n; ++i)
            x[i] = (float) (x[i] * level.getNextValue());
}

} // namespace ampsim

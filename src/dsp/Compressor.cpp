#include "Compressor.h"

#include <cmath>

namespace ampsim
{

namespace
{
constexpr double minimumLevel = 1.0e-9; // -180 dB: keeps log10 finite in silence

double toDb (double linear) { return 20.0 * std::log10 (std::max (linear, minimumLevel)); }
} // namespace

double Compressor::staticCurveDb (double x, double t, double r, double w)
{
    const auto over = x - t;

    if (w < 1.0e-9) // hard knee
        return over <= 0.0 ? x : t + over / r;
    if (2.0 * over < -w)
        return x;
    if (2.0 * over > w)
        return t + over / r;

    // Inside the knee: a quadratic that meets both straight lines with matching slopes.
    const auto k = over + 0.5 * w;
    return x + (1.0 / r - 1.0) * k * k / (2.0 * w);
}

double Compressor::feedbackReductionDb (double y, double t, double r, double w)
{
    // The feed-forward reduction's shape (x - y(x)) scaled from slope (1 - 1/R) to (R - 1), so that
    // a feedback loop around it lands on ratio R in steady state.
    const auto over = y - t;

    if (w < 1.0e-9)
        return over <= 0.0 ? 0.0 : (r - 1.0) * over;
    if (2.0 * over < -w)
        return 0.0;
    if (2.0 * over > w)
        return (r - 1.0) * over;

    const auto k = over + 0.5 * w;
    return (r - 1.0) * k * k / (2.0 * w);
}

double Compressor::autoMakeupDb (double t, double r, double w)
{
    return autoMakeupReferenceDb - staticCurveDb (autoMakeupReferenceDb, t, r, w);
}

double Compressor::coefficient (double milliseconds) const
{
    // One-pole smoothing: y += (1 - alpha)(x - y) with alpha = exp(-1 / (tau fs)) settles 63% of a
    // step in tau.
    return std::exp (-1.0 / (std::max (0.01, milliseconds) * 0.001 * sampleRate));
}

void Compressor::updateTimeConstants()
{
    attack = coefficient (settings.mode == Mode::pedal ? pedalAttackMs : settings.attackMs);
    release = coefficient (settings.autoRelease ? autoFastReleaseMs : settings.releaseMs);
    slowAttack = coefficient (autoSlowAttackMs);
    slowRelease = coefficient (autoSlowReleaseMs);
    rmsCoefficient = coefficient (rmsWindowMs);
}

void Compressor::updateSidechainFilter()
{
    sidechainDesignedHz = settings.sidechainHz;
    const auto c = Svf::design (Svf::Type::highpass, settings.sidechainHz, 0.7071067811865476, 0.0, sampleRate);
    for (auto& f : sidechain)
        f.setCoefficients (c);
}

void Compressor::setSettings (const Settings& newSettings)
{
    const bool timesChanged = newSettings.mode != settings.mode || newSettings.autoRelease != settings.autoRelease
                              || std::abs (newSettings.attackMs - settings.attackMs) > 1.0e-6f
                              || std::abs (newSettings.releaseMs - settings.releaseMs) > 1.0e-6f;
    settings = newSettings;

    thresholdDb.setTargetValue (juce::jlimit (-80.0, 0.0, (double) settings.thresholdDb));
    ratio.setTargetValue (juce::jlimit (1.0, 100.0, (double) settings.ratio));
    kneeDb.setTargetValue (juce::jlimit (0.0, 36.0, (double) settings.kneeDb));
    makeupDb.setTargetValue (juce::jlimit (-24.0, 36.0, (double) settings.makeupDb));
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));

    if (timesChanged)
        updateTimeConstants();
    if (std::abs (settings.sidechainHz - sidechainDesignedHz) > 1.0e-3f)
        updateSidechainFilter();
}

void Compressor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    // Start on the current settings with no ramps: nothing is playing yet.
    for (auto* s : { &thresholdDb, &kneeDb, &makeupDb, &mix })
        s->reset (sampleRate, smoothingSeconds);
    ratio.reset (sampleRate, smoothingSeconds);

    updateTimeConstants();
    updateSidechainFilter();
    reset();
}

void Compressor::reset()
{
    for (auto& f : sidechain)
        f.reset();
    meanSquare = 0.0;
    reduction = 0.0;
    slowReduction = 0.0;
    lastOutput = {};
    meterReduction = 0.0f;
}

void Compressor::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numChannels = (stereo && block.getNumChannels() > 1) ? 2 : 1;
    const auto numSamples = (int) block.getNumSamples();
    float* const channels[2] = { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

    const bool pedal = settings.mode == Mode::pedal;
    const bool rms = settings.detector == Detector::rms;
    double peakIn = 0.0, peakOut = 0.0, mostReduction = 0.0;

    for (int n = 0; n < numSamples; ++n)
    {
        const auto t = thresholdDb.getNextValue();
        const auto r = ratio.getNextValue();
        const auto w = kneeDb.getNextValue();
        const auto wetAmount = mix.getNextValue();
        const auto manualMakeup = makeupDb.getNextValue();

        // 1-2. The detector's level: this sample's input (Studio) or the previous output (Pedal),
        // through the sidechain high-pass, the louder channel when stereo.
        double level = 0.0;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto v = pedal ? lastOutput[(size_t) ch] : (double) channels[ch][n];
            if (settings.sidechainHighPass)
                v = sidechain[(size_t) ch].processSample (v);
            level = std::max (level, rms ? v * v : std::abs (v));
            peakIn = std::max (peakIn, std::abs ((double) channels[ch][n]));
        }

        double levelDb;
        if (rms)
        {
            meanSquare = rmsCoefficient * meanSquare + (1.0 - rmsCoefficient) * level;
            levelDb = 10.0 * std::log10 (std::max (meanSquare, minimumLevel * minimumLevel));
        }
        else
        {
            levelDb = toDb (level);
        }

        // 3. The gain computer's reduction for this level.
        const auto target = pedal ? feedbackReductionDb (levelDb, t, r, w) : levelDb - staticCurveDb (levelDb, t, r, w);

        // 4. Smooth it: attack while it rises, release while it falls (branching one-pole).
        const auto coefficientFor = [] (double current, double goal, double up, double down) { return goal > current ? up : down; };
        const auto a = coefficientFor (reduction, target, attack, release);
        reduction = a * reduction + (1.0 - a) * target;

        auto applied = reduction;
        if (settings.autoRelease)
        {
            const auto b = coefficientFor (slowReduction, target, slowAttack, slowRelease);
            slowReduction = b * slowReduction + (1.0 - b) * target;
            applied = std::max (reduction, slowReduction);
        }
        mostReduction = std::max (mostReduction, applied);

        // 5. Gain, makeup, and the parallel mix.
        const auto makeup = settings.autoMakeup ? autoMakeupDb (t, r, w) : manualMakeup;
        const auto compressorGain = std::pow (10.0, -applied / 20.0);
        const auto wetGain = compressorGain * std::pow (10.0, makeup / 20.0);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto x = (double) channels[ch][n];
            lastOutput[(size_t) ch] = x * compressorGain; // what a feedback detector hears next sample
            const auto y = (1.0 - wetAmount) * x + wetAmount * x * wetGain;
            channels[ch][n] = (float) y;
            peakOut = std::max (peakOut, std::abs (y));
        }
    }

    meterReduction.store ((float) mostReduction, std::memory_order_relaxed);
    meterInput.store ((float) toDb (peakIn), std::memory_order_relaxed);
    meterOutput.store ((float) toDb (peakOut), std::memory_order_relaxed);
}

} // namespace ampsim

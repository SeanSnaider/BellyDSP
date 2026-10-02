#include "ModulatedDelay.h"

#include <cmath>

namespace ampsim
{

namespace
{
constexpr double minRateHz = 1.0e-3; // multiplicative smoothing can't reach 0
} // namespace

void ModulatedDelay::prepare (double newSampleRate, double maxDelayMs, double smoothingSeconds)
{
    sampleRate = newSampleRate;

    // tap() reads the line at d - 1 <= maxDelay - 1; DelayLine keeps room for Hermite's neighbours.
    maxDelay = std::max (minDelaySamples + 2.0, std::ceil (maxDelayMs * sampleRate / 1000.0));
    line.prepare ((int) maxDelay);

    for (auto& st : voices)
    {
        st.lfo.prepare (sampleRate);
        st.base.reset (sampleRate, smoothingSeconds);
        st.depth.reset (sampleRate, smoothingSeconds);
        st.rate.reset (sampleRate, smoothingSeconds);
        st.level.reset (sampleRate, voiceFadeSeconds);
        st.running = false;
    }
    feedback.reset (sampleRate, smoothingSeconds);

    setSettings (settings); // the targets in samples at this rate
    reset();
}

void ModulatedDelay::reset()
{
    line.reset();
    restart (0.0);
}

double ModulatedDelay::getPhase() const noexcept
{
    for (const auto& st : voices)
    {
        if (st.running)
        {
            const auto p = st.lfo.getPhase() - st.offset;
            return p - std::floor (p);
        }
    }
    return restartPhase;
}

void ModulatedDelay::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    const auto numVoices = juce::jlimit (0, maxVoices, settings.numVoices);
    feedback.setTargetValue (juce::jlimit (-maxFeedback, maxFeedback, settings.feedback));
    const auto reference = getPhase();

    for (int v = 0; v < maxVoices; ++v)
    {
        const auto& in = settings.voices[(size_t) v];
        auto& st = voices[(size_t) v];
        const auto base = juce::jlimit (minDelaySamples, maxDelay, in.baseDelayMs * sampleRate / 1000.0);
        const auto depth = juce::jlimit (0.0, maxDelay, in.depthMs * sampleRate / 1000.0);
        const auto rate = std::max (minRateHz, in.rateHz);

        st.offset = in.phase;
        st.inverted = in.inverted;
        st.seed = in.seed;
        st.lfo.setShape (in.shape);
        st.wanted = v < numVoices && in.level > 0.0;

        if (st.wanted && ! st.running)
        {
            // A voice starting from silence joins in step with the others, with no ramp but its fade-in.
            st.lfo.setSeed (st.seed);
            st.lfo.setPhase (reference + st.offset);
            st.base.setCurrentAndTargetValue (base);
            st.depth.setCurrentAndTargetValue (depth);
            st.rate.setCurrentAndTargetValue (rate);
            st.lfo.setRate (rate);
            st.level.setCurrentAndTargetValue (0.0);
            st.level.setTargetValue (in.level);
            st.running = true;
        }
        else
        {
            st.base.setTargetValue (base);
            st.depth.setTargetValue (depth);
            st.rate.setTargetValue (rate);
            if (! st.rate.isSmoothing())
                st.lfo.setRate (rate);
            st.level.setTargetValue (st.wanted ? in.level : 0.0);
        }
    }
}

void ModulatedDelay::restart (double phase)
{
    restartPhase = phase - std::floor (phase);

    for (auto& st : voices)
    {
        st.base.setCurrentAndTargetValue (st.base.getTargetValue());
        st.depth.setCurrentAndTargetValue (st.depth.getTargetValue());
        st.rate.setCurrentAndTargetValue (st.rate.getTargetValue());
        st.lfo.setRate (st.rate.getTargetValue());
        st.lfo.setSeed (st.seed);
        st.lfo.setPhase (phase + st.offset);
        st.level.setCurrentAndTargetValue (st.wanted ? st.level.getTargetValue() : 0.0);
        st.running = st.wanted;
        st.delay = st.base.getTargetValue();
    }
    feedback.setCurrentAndTargetValue (feedback.getTargetValue());
}

float ModulatedDelay::read (double delaySamples) const noexcept
{
    return (float) tap (juce::jlimit (minDelaySamples, maxDelay, delaySamples));
}

float ModulatedDelay::processSample (float x) noexcept
{
    double wet = 0.0;

    for (auto& st : voices)
    {
        if (! st.running)
            continue;

        const auto gain = st.level.getNextValue();
        if (st.rate.isSmoothing())
            st.lfo.setRate (st.rate.getNextValue());

        // d = base + depth * m, the voice's read position for this sample.
        const auto m = st.inverted ? -(double) st.lfo.next() : (double) st.lfo.next();
        const auto base = st.base.getNextValue();
        const auto depth = st.depth.getNextValue();
        st.delay = juce::jlimit (minDelaySamples, maxDelay, base + depth * m);
        wet += gain * tap (st.delay);

        if (! st.wanted && ! st.level.isSmoothing())
            st.running = false; // finished fading out
    }

    // Feedback through a soft clip: |tanh| < 1 keeps the loop bounded at any setting.
    auto input = (double) x;
    const auto fb = feedback.getNextValue();
    if (fb != 0.0)
        input += std::tanh (fb * wet);

    line.write ((float) input);
    return (float) wet;
}

} // namespace ampsim

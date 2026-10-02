#include "Delay.h"

#include <cmath>

namespace ampsim
{

namespace
{
constexpr double analogColourHz = 3500.0, tapeColourHz = 5000.0;
constexpr double wowRateHz = 0.5, wowDepthMs = 1.0, flutterRateHz = 6.0, flutterDepthMs = 0.08;
constexpr float analogDrive = 1.5f, tapeDrive = 1.2f;
constexpr int coefficientInterval = 32;
constexpr float silenceLevel = 1.0e-5f; // -100 dBFS
} // namespace

float Delay::softLimit (float x) noexcept
{
    constexpr float knee = 0.7f;
    const auto a = std::abs (x);
    if (a <= knee)
        return x;

    // Above the knee: knee + (1 - knee) tanh((a - knee) / (1 - knee)). Its slope at the joint is
    // tanh'(0) = 1, matching the straight line below, and it approaches 1 but never reaches it.
    return std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), x);
}

double Delay::timeSamples (float ms) const noexcept
{
    return juce::jlimit (minMs, maxSeconds * 1000.0, (double) ms) * 0.001 * sampleRate;
}

void Delay::setSettings (const Settings& newSettings)
{
    const auto oldMode = settings.mode;
    settings = newSettings;

    feedback.setTargetValue (juce::jlimit (0.0f, (float) maxFeedback, settings.feedback));
    const auto angle = juce::jlimit (0.0f, 1.0f, settings.mix) * juce::MathConstants<float>::halfPi;
    if (! bypassed)
        dryGain.setTargetValue (std::cos (angle));
    wetGain.setTargetValue (std::sin (angle));
    duckDb.setTargetValue (juce::jmax (0.0f, settings.duckDb));
    lowCutHz.setTargetValue (juce::jlimit (20.0f, 2000.0f, settings.lowCutHz));
    highCutHz.setTargetValue (juce::jlimit (500.0f, 20000.0f, settings.highCutHz));
    lowCutOff = settings.lowCutHz <= 20.0f;     // at the end of its range the cut is out of the loop:
    highCutOff = settings.highCutHz >= 20000.0f; // pristine digital repeats
    modulation.setRate (settings.modRateHz);
    if (settings.mode != oldMode)
        designColour();

    // Switching between the crossfading head (digital) and the gliding head (analog, tape): carry the
    // read position over so the repeats don't jump.
    if ((oldMode == Mode::digital) != (settings.mode == Mode::digital))
    {
        for (auto& head : heads)
        {
            if (settings.mode == Mode::digital)
                head.current = head.glided;
            else
                head.glided = head.fading ? head.next : head.current;
            head.fading = false;
        }
    }
}

void Delay::setBypassed (bool shouldBeBypassed)
{
    if (shouldBeBypassed == bypassed)
        return;

    bypassed = shouldBeBypassed;
    inputGain.setTargetValue (bypassed ? 0.0f : 1.0f);
    dryGain.setTargetValue (bypassed ? 1.0f : std::cos (juce::jlimit (0.0f, 1.0f, settings.mix) * juce::MathConstants<float>::halfPi));
    if (! bypassed)
        tailSilent = false;
}

void Delay::designLoopFilters()
{
    const auto low = Svf::design (Svf::Type::highpass, lowCutHz.getCurrentValue(), 0.7071067811865476, 0.0, sampleRate);
    const auto high = Svf::design (Svf::Type::lowpass, highCutHz.getCurrentValue(), 0.7071067811865476, 0.0, sampleRate);
    for (int ch = 0; ch < 2; ++ch)
    {
        lowCut[(size_t) ch].setCoefficients (low);
        highCut[(size_t) ch].setCoefficients (high);
    }
}

void Delay::designColour()
{
    const auto c = Svf::design (Svf::Type::lowpass, settings.mode == Mode::analog ? analogColourHz : tapeColourHz, 0.7071067811865476, 0.0, sampleRate);
    for (auto& f : colour)
        f.setCoefficients (c);
}

void Delay::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    const auto maxModulation = juce::roundToInt ((5.0 + wowDepthMs + flutterDepthMs) * 0.001 * sampleRate);
    for (auto& line : lines)
        line.prepare ((int) std::ceil (maxSeconds * sampleRate) + maxModulation + 8);

    headFadeLength = juce::roundToInt (headFadeSeconds * sampleRate);
    glideCoefficient = std::exp (-1.0 / (glideSeconds * sampleRate));
    envelopeAttack = std::exp (-1.0 / (0.005 * sampleRate));
    envelopeRelease = std::exp (-1.0 / (0.100 * sampleRate));

    feedback.reset (sampleRate, 0.020);
    wetGain.reset (sampleRate, 0.020);
    duckDb.reset (sampleRate, 0.020);
    lowCutHz.reset (sampleRate, 0.025);
    highCutHz.reset (sampleRate, 0.025);
    dryGain.reset (sampleRate, bypassFadeSeconds);
    inputGain.reset (sampleRate, bypassFadeSeconds);
    inputGain.setCurrentAndTargetValue (bypassed ? 0.0f : 1.0f);

    for (auto* lfo : { &modulation, &wow, &flutter })
        lfo->prepare (sampleRate);
    modulation.setRate (settings.modRateHz);
    wow.setRate (wowRateHz);
    flutter.setShape (Lfo::Shape::random);
    flutter.setRate (flutterRateHz);

    designLoopFilters();
    designColour();
    reset();
}

void Delay::reset()
{
    for (auto& line : lines)
        line.reset();
    for (auto* filters : { &lowCut, &highCut, &colour })
        for (auto& f : *filters)
            f.reset();

    // Start at the current times with no fades or glides.
    const double times[2] = { timeSamples (settings.timeMs),
                              settings.stereoMode == StereoMode::dual ? timeSamples (settings.rightTimeMs)
                              : settings.stereoMode == StereoMode::stereo ? timeSamples (settings.timeMs + settings.offsetMs)
                                                                          : timeSamples (settings.timeMs) };
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& head = heads[(size_t) ch];
        head.current = head.next = head.glided = times[ch];
        head.fading = false;
        head.fadePosition = 0;
    }

    envelope = 0.0;
    tailSilent = true;
    silentSamples = 0;
}

float Delay::readHead (int channel, Head& head, double target, double modulationSamples) noexcept
{
    const auto& line = lines[(size_t) channel];

    // The line is read before this sample is written, so read(0) is one sample old: a delay of d
    // samples reads position d - 1.
    const auto at = [&] (double position) { return line.read (juce::jmax (1.0, position + modulationSamples) - 1.0); };

    if (settings.mode != Mode::digital)
    {
        // Analog and tape: the read position glides (one-pole), bending the pitch of the repeats, at
        // no more than maxGlideRate samples per sample.
        const auto step = (1.0 - glideCoefficient) * (target - head.glided);
        head.glided += juce::jlimit (-maxGlideRate, maxGlideRate, step);
        return at (head.glided);
    }

    // Digital: a new time starts an equal-power crossfade to a second head; a time that keeps changing
    // is picked up when the current fade ends (target arrives every sample).
    if (! head.fading && std::abs (target - head.current) > 0.5)
    {
        head.next = target;
        head.fading = true;
        head.fadePosition = 0;
    }

    const auto current = at (head.current);
    if (! head.fading)
        return current;

    const auto p = (float) head.fadePosition / (float) headFadeLength * juce::MathConstants<float>::halfPi;
    const auto mixed = current * std::cos (p) + at (head.next) * std::sin (p);
    if (++head.fadePosition >= headFadeLength)
    {
        head.current = head.next;
        head.fading = false;
    }
    return mixed;
}

float Delay::colourAndSaturate (int channel, float x) noexcept
{
    auto v = (double) x;
    if (! lowCutOff)
        v = lowCut[(size_t) channel].processSample (v);
    if (! highCutOff)
        v = highCut[(size_t) channel].processSample (v);

    switch (settings.mode)
    {
        case Mode::analog:
            v = colour[(size_t) channel].processSample (v);
            return std::tanh (analogDrive * (float) v) / analogDrive;
        case Mode::tape:
            v = colour[(size_t) channel].processSample (v);
            return std::tanh (tapeDrive * (float) v) / tapeDrive;
        case Mode::digital:
            break;
    }
    return softLimit ((float) v);
}

void Delay::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numSamples = (int) block.getNumSamples();
    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);

    // Bypassed with the repeats gone: nothing to do. The dry passes untouched (its gain is 1).
    if (bypassed && tailSilent && ! inputGain.isSmoothing() && ! dryGain.isSmoothing())
        return;

    const auto tLeft = timeSamples (settings.timeMs);
    const auto tRight = settings.stereoMode == StereoMode::dual ? timeSamples (settings.rightTimeMs)
                        : settings.stereoMode == StereoMode::stereo ? timeSamples (settings.timeMs + settings.offsetMs)
                                                                    : tLeft;
    const auto modDepth = juce::jlimit (0.0, 5.0, (double) settings.modDepthMs) * 0.001 * sampleRate;
    const auto wowDepth = wowDepthMs * 0.001 * sampleRate, flutterDepth = flutterDepthMs * 0.001 * sampleRate;
    float loudestWet = 0.0f;

    for (int n = 0; n < numSamples; ++n)
    {
        // The loop's cut knobs ramp; redesign every 32 samples while they move.
        if (samplesUntilUpdate-- <= 0)
        {
            samplesUntilUpdate = coefficientInterval - 1;
            if (lowCutHz.isSmoothing() || highCutHz.isSmoothing())
            {
                lowCutHz.skip (coefficientInterval);
                highCutHz.skip (coefficientInterval);
                designLoopFilters();
            }
        }

        const auto inL = left[n], inR = right[n];

        // Ducking: follow the input's peak level and pull the repeats down while it's loud.
        const auto level = (double) juce::jmax (std::abs (inL), std::abs (inR));
        const auto k = level > envelope ? envelopeAttack : envelopeRelease;
        envelope = k * envelope + (1.0 - k) * level;
        const auto amount = (float) juce::jmin (1.0, envelope / duckReference);
        const auto duck = juce::Decibels::decibelsToGain (-duckDb.getNextValue() * amount);

        auto m = modDepth > 0.0 ? modDepth * modulation.next() : 0.0;
        if (settings.mode == Mode::tape)
            m += wowDepth * wow.next() + flutterDepth * flutter.next();

        const auto wetL = colourAndSaturate (0, readHead (0, heads[0], tLeft, m));
        const auto wetR = colourAndSaturate (1, readHead (1, heads[1], tRight, m));

        auto fb = feedback.getNextValue();
        if (bypassed)
            fb = juce::jmin (fb, 0.99f); // let even a self-oscillating loop die away
        const auto in = inputGain.getNextValue();

        if (settings.stereoMode == StereoMode::pingPong)
        {
            // Input (as mono) into the left line only; each side feeds the other.
            lines[0].write (in * 0.5f * (inL + inR) + fb * wetR);
            lines[1].write (fb * wetL);
        }
        else
        {
            lines[0].write (in * inL + fb * wetL);
            lines[1].write (in * inR + fb * wetR);
        }

        const auto dry = dryGain.getNextValue();
        const auto wet = wetGain.getNextValue() * duck;
        left[n] = dry * inL + wet * wetL;
        right[n] = dry * inR + wet * wetR;
        loudestWet = juce::jmax (loudestWet, std::abs (wetL), std::abs (wetR));
    }

    // Bypassed: once the repeats have stayed below -100 dBFS for a second, stop processing.
    if (bypassed)
    {
        silentSamples = loudestWet < silenceLevel ? silentSamples + numSamples : 0;
        tailSilent = silentSamples >= (int) sampleRate;
    }
}

} // namespace ampsim

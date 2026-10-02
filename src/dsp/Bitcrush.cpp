#include "Bitcrush.h"

#include "Fade.h"

namespace ampsim
{

double Bitcrush::quantize (double x, double bits) noexcept
{
    // Mid-tread, two's complement range: the levels k step with step = 2^(1 - bits), clipped to [-1, 1 - step].
    // std::round goes half away from zero, so the quantizer is odd-symmetric below the clip points. At integer
    // bits the step is a power of two, so x / step and step * k are exact and the levels are exact binary
    // fractions (and exactly representable as floats up to 16 bits).
    const auto step = stepSize (bits);
    return juce::jlimit (-1.0, 1.0 - step, step * std::round (x / step));
}

void Bitcrush::designTone (double frequency)
{
    const auto c = Svf::design (Svf::Type::lowpass, frequency, butterworthQ, 0.0, sampleRate);
    for (auto& f : tone)
        f.setCoefficients (c);
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Bitcrush::setSettings (const Settings& newSettings)
{
    settings = newSettings;

    bits.setTargetValue (juce::jlimit (minBits, maxBits, (double) settings.bits));
    rate.setTargetValue (juce::jlimit (minRateHz, sampleRate, (double) settings.rateHz));
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));
    dither.setTargetValue (settings.dither ? 1.0 : 0.0);

    // The tone low-pass: at the top of its range it leaves the path. Switched in from fully out, it starts
    // clean at its new frequency (its state is stale and its cutoff hasn't been gliding).
    const auto hz = juce::jlimit (minToneHz, maxToneHz, (double) settings.toneHz);
    const bool filtered = hz < maxToneHz;
    if (filtered && toneOn.getCurrentValue() <= 0.0 && ! toneOn.isSmoothing())
    {
        toneHz.setCurrentAndTargetValue (hz);
        designTone (hz);
        for (auto& f : tone)
            f.reset();
    }
    toneOn.setTargetValue (filtered ? 1.0 : 0.0);
    toneHz.setTargetValue (hz);

    // On from fully off: the held values and the filter are stale, and nothing was audible, so the next
    // buffer starts from clean state with every knob at its target (only the on fade itself ramps).
    if (settings.on && isFullyOff())
        wakePending = true;
    onGain.setTargetValue (settings.on ? 1.0 : 0.0);
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Bitcrush::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    for (auto* s : { &bits, &mix, &dither, &toneOn })
        s->reset (sampleRate, smoothingSeconds);
    rate.reset (sampleRate, smoothingSeconds);
    toneHz.reset (sampleRate, toneSmoothingSeconds);
    onGain.reset (sampleRate, onFadeSeconds);
    setSettings (settings); // targets at this rate (the rate's ceiling is the host rate)
    reset();
}

void Bitcrush::clearState()
{
    phase = 1.0; // the first sample captures
    held = {};
    for (auto& f : tone)
        f.reset();
    samplesUntilUpdate = 0;
}

void Bitcrush::jumpToTargets()
{
    for (auto* s : { &bits, &mix, &dither, &toneOn })
        s->setCurrentAndTargetValue (s->getTargetValue());
    rate.setCurrentAndTargetValue (rate.getTargetValue());
    toneHz.setCurrentAndTargetValue (toneHz.getTargetValue());
    designTone (toneHz.getTargetValue());
}

void Bitcrush::reset()
{
    jumpToTargets();
    onGain.setCurrentAndTargetValue (onGain.getTargetValue());
    clearState();
    wakePending = false;
}

void Bitcrush::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numChannels = std::min (2, (int) block.getNumChannels());
    const auto numSamples = (int) block.getNumSamples();
    if (numChannels == 0 || numSamples == 0 || isFullyOff())
        return;

    if (wakePending)
    {
        jumpToTargets();
        clearState();
        wakePending = false;
    }

    float* const channels[2] = { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

    for (int n = 0; n < numSamples; ++n)
    {
        // The tone's cutoff glides: redesign every 32 samples while it moves.
        if (samplesUntilUpdate-- <= 0)
        {
            samplesUntilUpdate = coefficientInterval - 1;
            if (toneHz.isSmoothing())
                designTone (toneHz.skip (coefficientInterval));
        }

        const auto amount = mix.getNextValue() * sCurve (onGain.getNextValue());
        const auto depth = bits.getNextValue();
        const auto increment = rate.getNextValue() / sampleRate;
        const auto noiseAmount = dither.getNextValue();
        const auto filtered = toneOn.getNextValue();

        // 1. The hold: a capture whenever the accumulator passes 1.
        phase += increment;
        const bool capture = phase >= 1.0;
        if (capture)
            phase -= std::floor (phase);

        const auto step = capture ? stepSize (depth) : 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto x = (double) channels[ch][n];

            // 2-3. At a capture: TPDF dither of +-1 step (two uniforms), then the quantizer.
            if (capture)
            {
                auto in = x;
                if (noiseAmount > 0.0)
                {
                    auto& r = noise[(size_t) ch];
                    in += noiseAmount * step * (r.nextDouble() + r.nextDouble() - 1.0);
                }
                held[(size_t) ch] = quantize (in, depth);
            }

            // 4. The tone low-pass, crossfaded out at the top of its range.
            auto crushed = held[(size_t) ch];
            if (filtered > 0.0)
                crushed += filtered * (tone[(size_t) ch].processSample (crushed) - crushed);

            // 5. The linear mix: exact dry at 0, exact crush at 1.
            channels[ch][n] = (float) ((1.0 - amount) * x + amount * crushed);
        }
    }
}

} // namespace ampsim

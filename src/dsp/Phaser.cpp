// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Phaser.h"

#include "Fade.h"

namespace ampsim
{

// ---- The math (static) -------------------------------------------------------------------------

int Phaser::snapStages (int stages) noexcept
{
    // The nearest allowed count; a tie goes to the larger.
    int best = stageChoices[0];
    for (auto choice : stageChoices)
        if (std::abs (choice - stages) <= std::abs (best - stages))
            best = choice;
    return best;
}

Phaser::Spec Phaser::specFor (const Settings& s) noexcept
{
    switch (s.mode)
    {
        case Mode::modern:
            // Lfo::shapeAt has sine and triangle; the random shape isn't offered here.
            return { Mode::modern, snapStages (s.stages), s.shape == Lfo::Shape::triangle ? Lfo::Shape::triangle : Lfo::Shape::sine };
        case Mode::classic:
            return { Mode::classic, 4, Lfo::Shape::triangle };
        case Mode::vibe:
            return { Mode::vibe, 4, Lfo::Shape::sine };
    }
    return {};
}

double Phaser::modernCorner (double lowHz, double highHz, double depth, double m) noexcept
{
    // Exponential: p moves linearly with the LFO, and the corner moves the same number of octaves per step.
    const auto p = 0.5 + 0.5 * depth * m;
    return lowHz * std::pow (highHz / lowHz, p);
}

double Phaser::classicCorner (double depth, double m) noexcept
{
    // Linear in Hz: the JFET's conductance is linear in its gate voltage (square-law model, ohmic region).
    const auto v = 0.5 + 0.5 * depth * m;
    return classicLowHz + (classicHighHz - classicLowHz) * v;
}

double Phaser::notchFrequency (int stages, int k, double cornerHz, double sampleRate) noexcept
{
    // N matched stages: the phase is -2N atan(W), W = tan(pi f / fs) / g, so it passes -(2k + 1) 180 degrees
    // at W = tan((2k + 1) pi / 2N).
    const auto pi = juce::MathConstants<double>::pi;
    const auto g = std::tan (pi * juce::jlimit (1.0, 0.49 * sampleRate, cornerHz) / sampleRate);
    return sampleRate / pi * std::atan (g * std::tan ((2.0 * k + 1.0) * pi / (2.0 * stages)));
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Phaser::setSettings (const Settings& newSettings)
{
    settings = newSettings;

    rate.setTargetValue (juce::jlimit (minRateHz, maxRateHz, (double) settings.rateHz));
    depth.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.depth));
    offset.setTargetValue (juce::jlimit (0.0, 0.5, (double) settings.stereoOffset));

    auto lo = juce::jlimit (minRangeHz, maxRangeHz, (double) settings.lowHz);
    auto hi = juce::jlimit (minRangeHz, maxRangeHz, (double) settings.highHz);
    if (lo > hi)
        std::swap (lo, hi);
    low.setTargetValue (lo);
    high.setTargetValue (hi);

    modernFeedback.setTargetValue (juce::jlimit (0.0, maxFeedback, (double) settings.feedback));
    classicFeedback.setTargetValue (settings.classicFeedback ? classicBlockFeedback : 0.0);
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));

    // On from fully off: start from clean state with every knob at its target (nothing was audible).
    if (settings.on && isFullyOff())
        wakePending = true;
    onGain.setTargetValue (settings.on ? 1.0 : 0.0);
}

double Phaser::channelPhase (int channel, double spread) const noexcept
{
    const auto p = phase + (channel == 1 ? spread : 0.0);
    return p - std::floor (p);
}

void Phaser::startBank (int index, const Spec& spec)
{
    // The incoming bank starts from rest, on the shared LFO, with its lamps warm.
    auto& bank = banks[(size_t) index];
    bank.spec = spec;
    for (int ch = 0; ch < 2; ++ch)
    {
        bank.state[(size_t) ch].fill (0.0);
        bank.lamps[(size_t) ch].warmStart ((double) Lfo::shapeAt (spec.shape, channelPhase (ch, offset.getCurrentValue())), depth.getCurrentValue());
    }
}

void Phaser::select (int index)
{
    selected = index;
    banks[(size_t) index].gain.setTargetValue (1.0);
    banks[(size_t) (1 - index)].gain.setTargetValue (0.0);
}

void Phaser::applyModeRequest()
{
    const auto wanted = specFor (settings);
    if (banks[(size_t) selected].spec == wanted)
        return;

    const auto other = 1 - selected;
    if (audible (banks[(size_t) other]))
    {
        // Still fading out. If it's what's being asked for, fade back to it; otherwise wait until it's silent.
        if (banks[(size_t) other].spec == wanted)
            select (other);
        return;
    }

    startBank (other, wanted);
    select (other);
}

bool Phaser::isSwitching() const noexcept
{
    return banks[0].gain.isSmoothing() || banks[1].gain.isSmoothing() || banks[(size_t) selected].spec != specFor (settings);
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Phaser::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    for (auto* s : { &rate, &low, &high })
        s->reset (sampleRate, modulationSmoothingSeconds);
    for (auto* s : { &depth, &offset })
        s->reset (sampleRate, modulationSmoothingSeconds);
    modernFeedback.reset (sampleRate, feedbackSmoothingSeconds);
    classicFeedback.reset (sampleRate, feedbackSmoothingSeconds);
    mix.reset (sampleRate, smoothingSeconds);
    onGain.reset (sampleRate, onFadeSeconds);

    for (auto& bank : banks)
    {
        bank.gain.reset (sampleRate, modeFadeSeconds);
        for (auto& lamp : bank.lamps)
            lamp.prepare (sampleRate);
    }

    selected = 0;
    reset();
}

void Phaser::jumpToTargets()
{
    for (auto* s : { &rate, &low, &high })
        s->setCurrentAndTargetValue (s->getTargetValue());
    for (auto* s : { &depth, &offset, &modernFeedback, &classicFeedback, &mix })
        s->setCurrentAndTargetValue (s->getTargetValue());
}

void Phaser::clearState()
{
    // Silent: finish any bank fade at the requested spec.
    banks[(size_t) selected].gain.setCurrentAndTargetValue (1.0);
    banks[(size_t) (1 - selected)].gain.setCurrentAndTargetValue (0.0);
    for (auto& bank : banks)
        for (auto& corners : bank.corners)
            corners.fill (0.0);
    startBank (selected, specFor (settings));
    banks[(size_t) (1 - selected)].state = {};
}

void Phaser::reset()
{
    jumpToTargets();
    onGain.setCurrentAndTargetValue (onGain.getTargetValue());
    phase = 0.0;
    clearState();
    wakePending = false;
}

double Phaser::feedbackFor (const Bank& bank, const Sweep& sweep) const noexcept
{
    switch (bank.spec.mode)
    {
        case Mode::modern:  return sweep.modernFeedback;
        case Mode::classic: return sweep.classicFeedback;
        case Mode::vibe:    return 0.0;
    }
    return 0.0;
}

double Phaser::runBank (Bank& bank, int channel, double channelPhase, double x, double fb, const Sweep& sweep) noexcept
{
    const auto stages = bank.spec.stages;
    const auto m = (double) Lfo::shapeAt (bank.spec.shape, channelPhase);
    auto& corners = bank.corners[(size_t) channel];

    // The stages' corner frequencies for this sample.
    switch (bank.spec.mode)
    {
        case Mode::modern:
        {
            const auto fc = modernCorner (sweep.low, sweep.high, sweep.depth, m);
            for (int i = 0; i < stages; ++i)
                corners[(size_t) i] = fc;
            break;
        }
        case Mode::classic:
        {
            const auto fc = classicCorner (sweep.depth, m);
            for (int i = 0; i < stages; ++i)
                corners[(size_t) i] = fc;
            break;
        }
        case Mode::vibe:
        {
            const auto reference = vibeReferenceHz (bank.lamps[(size_t) channel].next (m, sweep.depth));
            for (int i = 0; i < stages; ++i)
                corners[(size_t) i] = reference * vibeStageRatios[(size_t) i];
            break;
        }
    }

    // TPT one-pole coefficients, G = g / (1 + g) with g = tan(pi fc / fs). Modern's and Classic's stages are
    // matched, so they share one tan(); the Vibe's four each need their own.
    const auto coefficient = [this] (double fc)
    {
        const auto g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (1.0, 0.49 * sampleRate, fc) / sampleRate);
        return g / (1.0 + g);
    };
    std::array<double, maxStages> G {};
    if (bank.spec.mode == Mode::vibe)
        for (int i = 0; i < stages; ++i)
            G[(size_t) i] = coefficient (corners[(size_t) i]);
    else
        G.fill (coefficient (corners[0]));

    // The delay-free loop: chain the stages' affine responses, out = a u + b, into y = A u0 + B, then solve
    // u0 = x + fb y for u0.
    auto& s = bank.state[(size_t) channel];
    double A = 1.0, B = 0.0;
    for (int i = 0; i < stages; ++i)
    {
        const auto a = 2.0 * G[(size_t) i] - 1.0;
        A = a * A;
        B = a * B + 2.0 * (1.0 - G[(size_t) i]) * s[(size_t) i];
    }
    auto u = (x + fb * B) / (1.0 - fb * A);

    // Run the stages for real, updating their states: v = (u - s) G, lp = v + s, s <- lp + v, out = 2 lp - u.
    for (int i = 0; i < stages; ++i)
    {
        const auto v = (u - s[(size_t) i]) * G[(size_t) i];
        const auto lp = v + s[(size_t) i];
        s[(size_t) i] = lp + v;
        u = 2.0 * lp - u;
    }
    return u;
}

void Phaser::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
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
    applyModeRequest(); // a change that was waiting for the last fade to end

    float* const channels[2] = { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

    for (int n = 0; n < numSamples; ++n)
    {
        const auto amount = mix.getNextValue() * sCurve (onGain.getNextValue());
        const Sweep sweep { depth.getNextValue(), low.getNextValue(), high.getNextValue(), modernFeedback.getNextValue(),
                            classicFeedback.getNextValue() };
        const auto spread = offset.getNextValue();
        const auto increment = rate.getNextValue() / sampleRate;

        // Each audible bank's crossfade gain for this sample, and its feedback compensation.
        bool active[2];
        double gains[2], fbs[2], compensation[2];
        for (size_t b = 0; b < 2; ++b)
        {
            active[b] = audible (banks[b]);
            gains[b] = active[b] ? sCurve (banks[b].gain.getNextValue()) : 0.0;
            fbs[b] = feedbackFor (banks[b], sweep);
            compensation[b] = feedbackCompensation (fbs[b]);
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto x = (double) channels[ch][n];
            const auto ph = channelPhase (ch, spread);
            double wet = 0.0;
            for (size_t b = 0; b < 2; ++b)
                if (active[b])
                    wet += gains[b] * runBank (banks[b], ch, ph, x, fbs[b], sweep) * compensation[b];

            channels[ch][n] = (float) ((1.0 - amount) * x + amount * wet);
        }

        phase += increment;
        if (phase >= 1.0)
            phase -= std::floor (phase);
    }
}

} // namespace ampsim

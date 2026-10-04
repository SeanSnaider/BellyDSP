// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Flanger.h"

#include "Fade.h"

namespace ampsim
{

ModulatedDelay::Settings Flanger::engineSettings (const Settings& s, int channel, bool throughZeroOn)
{
    ModulatedDelay::Settings e;
    e.numVoices = 1;

    // d(t) = manual (1 + 0.9 depth m(t)): base = manual, depth = 0.9 depth manual (the engine's d = base + depth m).
    const auto manual = juce::jlimit (minManualMs, maxManualMs, (double) s.manualMs);
    auto& voice = e.voices[0];
    voice.baseDelayMs = manual;
    voice.depthMs = maxSwing * juce::jlimit (0.0, 1.0, (double) s.depth) * manual;
    voice.shape = s.shape;
    voice.rateHz = juce::jlimit (minRateHz, maxRateHz, (double) s.rateHz);
    voice.phase = channel == 1 ? juce::jlimit (0.0, 0.5, (double) s.stereoPhase) : 0.0;
    voice.inverted = false;
    voice.seed = 11; // both sides wander through one random sequence, offset by the stereo phase
    voice.level = 1.0;

    // Through-zero has no loop; otherwise the loop's soft clip (the engine's tanh) and its 150 Hz high-pass, with
    // the feedback's sign following the polarity.
    const auto amount = juce::jlimit (0.0, maxFeedback, (double) s.feedback);
    e.feedback = throughZeroOn ? 0.0 : (s.negative ? -amount : amount);
    e.feedbackHighPassHz = loopHighPassHz;
    return e;
}

// ---- Settings (audio thread) ------------------------------------------------------------------

double Flanger::requestedStereoPhase() const noexcept
{
    return juce::jlimit (0.0, 0.5, (double) settings.stereoPhase);
}

void Flanger::configure (Bank& bank)
{
    // A pair runs its own shape and stereo phase (a change of either crossfades pairs); everything else is
    // the current setting, gliding inside the engines.
    auto s = settings;
    s.shape = bank.shape;
    s.stereoPhase = (float) bank.stereoPhase;
    for (int ch = 0; ch < 2; ++ch)
        bank.engines[(size_t) ch].setSettings (engineSettings (s, ch, throughZero));
}

void Flanger::startBank (int index)
{
    // A silent pair takes the new shape and stereo phase and starts where the audible pair's LFO cycle is.
    auto& bank = banks[(size_t) index];
    bank.shape = settings.shape;
    bank.stereoPhase = requestedStereoPhase();
    startEngines (bank, banks[(size_t) selected].engines[0].getPhase());
}

void Flanger::startEngines (Bank& bank, double phase)
{
    // A pair that was only being written holds the plain input in its lines, with nothing recirculating. Switching
    // its loop on at full feedback would write a step into the line (input plus a sudden feedback term) that the
    // wet plays back a few milliseconds later, a click. So the engines restart (every glide at its target, the
    // LFO at `phase`) with feedback 0, and then ramp to the setting over the 100 ms glide, building the loop up.
    auto s = settings;
    s.shape = bank.shape;
    s.stereoPhase = (float) bank.stereoPhase;
    s.feedback = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& engine = bank.engines[(size_t) ch];
        engine.setSettings (engineSettings (s, ch, throughZero));
        engine.restart (phase);
    }
    configure (bank);
}

void Flanger::select (int index)
{
    selected = index;
    banks[(size_t) index].gain.setTargetValue (1.0);
    banks[(size_t) (1 - index)].gain.setTargetValue (0.0);
}

void Flanger::applyBankRequest()
{
    const auto matches = [this] (const Bank& bank)
    {
        return bank.shape == settings.shape && juce::exactlyEqual (bank.stereoPhase, requestedStereoPhase());
    };
    if (matches (banks[(size_t) selected]))
        return;

    const auto other = 1 - selected;
    if (audible (banks[(size_t) other]))
    {
        // Still fading out: fade back to it if it's what's being asked for, otherwise wait until it's silent.
        if (matches (banks[(size_t) other]))
            select (other);
        return;
    }

    startBank (other);
    select (other);
}

bool Flanger::isSwitching() const noexcept
{
    const auto& current = banks[(size_t) selected];
    return banks[0].gain.isSmoothing() || banks[1].gain.isSmoothing() || current.shape != settings.shape
           || ! juce::exactlyEqual (current.stereoPhase, requestedStereoPhase());
}

void Flanger::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));
    polarity.setTargetValue (settings.negative ? -1.0 : 1.0);

    // On from fully off: the lines are current, but knobs moved while off would glide in (a delay glide is a
    // pitch bend), so the next buffer jumps them to their targets first.
    if (settings.on && isFullyOff())
        wakePending = true;
    onGain.setTargetValue (settings.on ? 1.0 : 0.0);

    for (auto& bank : banks)
        configure (bank);
}

void Flanger::wake()
{
    // Nothing was audible: take the requested LFO pair at once and jump every glide, LFOs where they were.
    const auto phase = banks[(size_t) selected].engines[0].getPhase();
    for (auto& bank : banks)
    {
        bank.shape = settings.shape;
        bank.stereoPhase = requestedStereoPhase();
        startEngines (bank, phase);
    }
    banks[(size_t) selected].gain.setCurrentAndTargetValue (1.0);
    banks[(size_t) (1 - selected)].gain.setCurrentAndTargetValue (0.0);
    mix.setCurrentAndTargetValue (mix.getTargetValue());
    polarity.setCurrentAndTargetValue (polarity.getTargetValue());
}

void Flanger::setThroughZero (bool shouldBeOn)
{
    throughZero = shouldBeOn;

    // The output is silent, so every glide jumps to its target, keeping each LFO where it is; the loop starts
    // empty and its feedback (none in through-zero mode) ramps in.
    for (auto& bank : banks)
        startEngines (bank, bank.engines[0].getPhase());
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Flanger::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    throughZeroSamples = juce::roundToInt (throughZeroMs * sampleRate / 1000.0);

    for (auto& bank : banks)
    {
        for (auto& engine : bank.engines)
            engine.prepare (sampleRate, maxDelayMs, modulationSmoothingSeconds);
        bank.gain.reset (sampleRate, bankFadeSeconds);
    }
    for (auto& line : dryLines)
        line.prepare (throughZeroSamples + 4);

    mix.reset (sampleRate, smoothingSeconds);
    polarity.reset (sampleRate, smoothingSeconds);
    onGain.reset (sampleRate, onFadeSeconds);

    selected = 0;
    reset();
}

void Flanger::reset()
{
    // Silent: the requested through-zero state and LFO pair take effect at once, with no ramps.
    throughZero = settings.throughZero;
    for (auto& bank : banks)
    {
        bank.shape = settings.shape;
        bank.stereoPhase = requestedStereoPhase();
        configure (bank);
        for (auto& engine : bank.engines)
            engine.reset();
    }
    banks[(size_t) selected].gain.setCurrentAndTargetValue (1.0);
    banks[(size_t) (1 - selected)].gain.setCurrentAndTargetValue (0.0);

    for (auto& line : dryLines)
        line.reset();
    for (auto* s : { &mix, &polarity, &onGain })
        s->setCurrentAndTargetValue (s->getTargetValue());
    wakePending = false;
}

void Flanger::writeLines (int channel, float x) noexcept
{
    dryLines[(size_t) channel].write (x);
    for (auto& bank : banks)
        bank.engines[(size_t) channel].write (x);
}

void Flanger::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numChannels = std::min (2, (int) block.getNumChannels());
    const auto numSamples = (int) block.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    float* const channels[2] = { block.getChannelPointer (0), numChannels > 1 ? block.getChannelPointer (1) : nullptr };

    if (isFullyOff())
    {
        // Off: keep the lines current, and in through-zero mode keep the dry where the latency says it is.
        for (int n = 0; n < numSamples; ++n)
        {
            const auto left = channels[0][n];
            writeLines (0, left);
            writeLines (1, numChannels > 1 ? channels[1][n] : left);
            if (throughZero)
                for (int ch = 0; ch < numChannels; ++ch)
                    channels[ch][n] = dryLines[(size_t) ch].readInteger (throughZeroSamples);
        }
        return;
    }

    if (wakePending)
    {
        wake();
        wakePending = false;
    }
    applyBankRequest(); // a change that was waiting for the last fade to end

    for (int n = 0; n < numSamples; ++n)
    {
        const auto amount = mix.getNextValue() * sCurve (onGain.getNextValue());
        const auto hold = settings.holdLevel ? mixLevelHold (amount) : 1.0; // the mix's lost power back (Fade.h)
        const auto sign = polarity.getNextValue();

        bool active[2];
        double gains[2];
        for (size_t b = 0; b < 2; ++b)
        {
            active[b] = audible (banks[b]);
            gains[b] = active[b] ? sCurve (banks[b].gain.getNextValue()) : 0.0;
        }

        // The right engines run even on a mono block (on a copy of the left), so both stay in step.
        float outputs[2] {};
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto in = channels[ch < numChannels ? ch : 0][n];
            const auto x = (double) in;

            auto& dryLine = dryLines[(size_t) ch];
            dryLine.write (in);
            const auto dry = throughZero ? (double) dryLine.readInteger (throughZeroSamples) : x;

            double wet = 0.0;
            for (size_t b = 0; b < 2; ++b)
            {
                auto& engine = banks[b].engines[(size_t) ch];
                if (! active[b])
                {
                    engine.write (in);
                    continue;
                }
                const auto tap = (double) engine.processSample (in);
                wet += gains[b] * tap * feedbackCompensation (engine.getFeedback());
            }

            outputs[ch] = (float) (hold * ((1.0 - amount) * dry + amount * sign * wet));
        }

        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][n] = outputs[ch];
    }
}

} // namespace ampsim

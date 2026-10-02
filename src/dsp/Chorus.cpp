#include "Chorus.h"

#include <cmath>

namespace ampsim
{

namespace
{
// Uniform noise in [-1, 1] has an RMS of 1/sqrt(3), so this scale gives noiseRmsDb.
const double noiseGain = std::pow (10.0, Chorus::noiseRmsDb / 20.0) * std::sqrt (3.0);
} // namespace

ModulatedDelay::Settings Chorus::voiceSettings (Mode mode, Lfo::Shape shape, int channel, double rateHz, double depth)
{
    const auto& spec = modeSpecs[(size_t) mode];
    ModulatedDelay::Settings s;
    s.numVoices = spec.voicesPerChannel;
    s.feedback = 0.0;

    const auto depthMs = juce::jlimit (0.0, 1.0, depth) * spec.maxDepthMs;
    const auto rate = juce::jlimit (minRateHz, maxRateHz, rateHz);

    for (int v = 0; v < spec.voicesPerChannel; ++v)
    {
        auto& voice = s.voices[(size_t) v];
        voice.baseDelayMs = spec.baseDelayMs;
        voice.depthMs = depthMs;
        voice.shape = shape;
        voice.rateHz = rate;

        if (mode == Mode::tri)
        {
            // Voices 0 and 1 on the left, 1 and 2 on the right, 120 degrees apart. The centre voice (1) is
            // the same in both engines: same phase, same random sequence.
            const auto index = channel + v;
            voice.phase = index / 3.0;
            voice.inverted = false;
            voice.seed = 3 + index;
            voice.level = index == 1 ? triCentreLevel : triSideLevel;
        }
        else
        {
            // Classic and Dimension: the right voice is the left one inverted, sharing its random sequence,
            // so the pair is in exact antiphase whatever the shape.
            voice.phase = 0.0;
            voice.inverted = channel == 1;
            voice.seed = mode == Mode::classic ? 1 : 2;
            voice.level = 1.0;
        }
    }
    return s;
}

double Chorus::highPassQ (int section)
{
    // Butterworth section k = section + 1 of order N = 4: Q_k = 1 / (2 sin((2k - 1) pi / (2N))).
    return 1.0 / (2.0 * std::sin ((2.0 * section + 1.0) * juce::MathConstants<double>::pi / 8.0));
}

Chorus::MixGains Chorus::mixGains (double mix) noexcept
{
    // Linear: the dry falls as the wet rises. (Equal power, cos and sin of pi/2 mix, would hold the level
    // of the chorused band, where dry and wet are uncorrelated; linear dips it 3 dB at 50%.)
    return { 1.0 - mix, mix };
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Chorus::configure (Bank& bank)
{
    for (int ch = 0; ch < 2; ++ch)
        bank.engines[(size_t) ch].setSettings (voiceSettings (bank.mode, bank.shape, ch, settings.rateHz, settings.depth));
}

void Chorus::startBank (int index, Mode mode, Lfo::Shape shape)
{
    // A silent set takes the new mode and starts where the audible set's LFO cycle is.
    auto& bank = banks[(size_t) index];
    bank.mode = mode;
    bank.shape = shape;
    configure (bank);

    const auto phase = banks[(size_t) selected].engines[0].getPhase();
    for (auto& engine : bank.engines)
        engine.restart (phase);
}

void Chorus::select (int index)
{
    selected = index;
    banks[(size_t) index].gain.setTargetValue (1.0);
    banks[(size_t) (1 - index)].gain.setTargetValue (0.0);
}

void Chorus::applyModeRequest()
{
    const auto& current = banks[(size_t) selected];
    if (current.mode == settings.mode && current.shape == settings.shape)
        return;

    const auto other = 1 - selected;
    const auto& next = banks[(size_t) other];

    if (audible (next))
    {
        // Still fading out. If it's what's being asked for, fade back to it; otherwise wait until it's silent.
        if (next.mode == settings.mode && next.shape == settings.shape)
            select (other);
        return;
    }

    startBank (other, settings.mode, settings.shape);
    select (other);
}

bool Chorus::isSwitching() const noexcept
{
    const auto& current = banks[(size_t) selected];
    return banks[0].gain.isSmoothing() || banks[1].gain.isSmoothing() || current.mode != settings.mode
           || current.shape != settings.shape;
}

double Chorus::getVoiceDelaySamples (int channel, int voice) const noexcept
{
    return banks[(size_t) selected].engines[(size_t) channel].getVoiceDelay (voice);
}

void Chorus::designCrossover (double frequency)
{
    for (int s = 0; s < 2; ++s)
    {
        const auto c = Svf::design (Svf::Type::highpass, frequency, highPassQ (s), 0.0, sampleRate);
        for (auto& sections : wetHighPass)
            sections[(size_t) s].setCoefficients (c);
    }
    const auto low = Svf::design (Svf::Type::lowpass, frequency, butterworthQ, 0.0, sampleRate);
    for (auto& f : lowBand)
        f.setCoefficients (low);
}

void Chorus::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    const auto hz = juce::jlimit (minHighPassHz, maxHighPassHz, (double) settings.wetHighPassHz);

    mix.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.mix));
    width.setTargetValue (juce::jlimit (0.0, 1.0, (double) settings.width));

    // A filter switched on from fully off starts clean: its state is stale.
    if (settings.analog && analog.getCurrentValue() <= 0.0 && ! analog.isSmoothing())
        for (auto& f : analogLowPass)
            f.reset();
    analog.setTargetValue (settings.analog ? 1.0 : 0.0);
    noise.setTargetValue (settings.analog && settings.noise ? 1.0 : 0.0);

    if (settings.wetHighPass && crossoverOn.getCurrentValue() <= 0.0 && ! crossoverOn.isSmoothing())
    {
        crossoverHz.setCurrentAndTargetValue (hz);
        designCrossover (hz);
        resetCrossover();
    }
    crossoverOn.setTargetValue (settings.wetHighPass ? 1.0 : 0.0);
    crossoverHz.setTargetValue (hz);

    for (auto& bank : banks)
        configure (bank);
    applyModeRequest();
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Chorus::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    for (auto& bank : banks)
    {
        for (auto& engine : bank.engines)
            engine.prepare (sampleRate, maxDelayMs, modulationSmoothingSeconds);
        bank.gain.reset (sampleRate, modeFadeSeconds);
    }

    for (auto* s : { &mix, &width, &analog, &noise, &crossoverOn })
        s->reset (sampleRate, smoothingSeconds);
    crossoverHz.reset (sampleRate, crossoverSmoothingSeconds);

    const auto lowPass = Svf::design (Svf::Type::lowpass, analogLowPassHz, butterworthQ, 0.0, sampleRate);
    for (auto& f : analogLowPass)
        f.setCoefficients (lowPass);

    // Start on the current settings with no ramps or fades: nothing is playing yet.
    selected = 0;
    reset();
}

void Chorus::reset()
{
    // The block is silent, so finish any mode fade at the requested mode and jump every ramp to its target.
    banks[(size_t) selected].mode = settings.mode;
    banks[(size_t) selected].shape = settings.shape;

    for (auto& bank : banks)
    {
        configure (bank);
        for (auto& engine : bank.engines)
            engine.reset();
    }
    banks[(size_t) selected].gain.setCurrentAndTargetValue (1.0);
    banks[(size_t) (1 - selected)].gain.setCurrentAndTargetValue (0.0);

    for (auto* s : { &mix, &width, &analog, &noise, &crossoverOn })
        s->setCurrentAndTargetValue (s->getTargetValue());
    crossoverHz.setCurrentAndTargetValue (crossoverHz.getTargetValue());
    designCrossover (crossoverHz.getTargetValue());

    resetCrossover();
    for (auto& f : analogLowPass)
        f.reset();
    samplesUntilUpdate = 0;
}

void Chorus::resetCrossover()
{
    for (auto& sections : wetHighPass)
        for (auto& f : sections)
            f.reset();
    for (auto& f : lowBand)
        f.reset();
}

void Chorus::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numChannels = std::min (2, (int) block.getNumChannels());
    const auto numSamples = (int) block.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    // A mono block runs the right side on a copy of the left and keeps only the left.
    float* const left = block.getChannelPointer (0);
    float* const right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;

    applyModeRequest(); // a mode change that was waiting for the last fade to end

    for (int n = 0; n < numSamples; ++n)
    {
        // The crossover frequency glides: redesign every 32 samples while it moves.
        if (samplesUntilUpdate-- <= 0)
        {
            samplesUntilUpdate = coefficientInterval - 1;
            if (crossoverHz.isSmoothing())
                designCrossover (crossoverHz.skip (coefficientInterval));
        }

        const auto split = crossoverOn.getNextValue();
        const auto character = analog.getNextValue();
        const auto hiss = noise.getNextValue();
        const auto amount = mix.getNextValue();
        const auto spread = width.getNextValue();
        const double x[2] = { (double) left[n], right != nullptr ? (double) right[n] : (double) left[n] };

        // 1. Low-end protection: the 24 dB/oct high-pass feeds the lines, the 12 dB/oct low-pass skips them.
        double low[2] = { 0.0, 0.0 };
        double lineIn[2] = { x[0], x[1] };
        if (split > 0.0 || crossoverOn.isSmoothing())
        {
            for (size_t ch = 0; ch < 2; ++ch)
            {
                const auto high = wetHighPass[ch][1].processSample (wetHighPass[ch][0].processSample (x[ch]));
                lineIn[ch] = x[ch] + split * (high - x[ch]);
                low[ch] = split * lowBand[ch].processSample (x[ch]);
            }
        }

        // 2a. Analog: light saturation on the way into the lines.
        if (character > 0.0)
            for (auto& v : lineIn)
                v += character * (std::tanh (v) - v);

        // The voices of whichever engine sets can be heard; a silent set only keeps its lines current.
        double wet[2] = { 0.0, 0.0 };
        for (auto& bank : banks)
        {
            if (! audible (bank))
            {
                bank.engines[0].write ((float) lineIn[0]);
                bank.engines[1].write ((float) lineIn[1]);
                continue;
            }

            const auto g = bank.gain.getNextValue();
            const auto l = (double) bank.engines[0].processSample ((float) lineIn[0]);
            const auto r = (double) bank.engines[1].processSample ((float) lineIn[1]);

            if (bank.mode == Mode::dimension)
            {
                // The antiphase pair's difference, opposite on the two sides (SDD-320 matrix).
                const auto difference = g * dimensionGain * (l - r);
                wet[0] += difference;
                wet[1] -= difference;
            }
            else
            {
                wet[0] += g * l;
                wet[1] += g * r;
            }
        }

        // 2b. Analog: faint noise, then the 7 kHz low-pass, on the wet.
        for (size_t ch = 0; ch < 2; ++ch)
        {
            if (hiss > 0.0)
                wet[ch] += hiss * noiseGain * (2.0 * (double) noiseSources[ch].nextFloat() - 1.0);
            if (character > 0.0)
                wet[ch] += character * (analogLowPass[ch].processSample (wet[ch]) - wet[ch]);
        }

        // 3. Width: mid/side on the wet. 4. The mix, with the protected lows.
        const auto mid = 0.5 * (wet[0] + wet[1]);
        const auto side = 0.5 * (wet[0] - wet[1]) * spread;
        const auto gains = mixGains (amount);
        left[n] = (float) (gains.dry * x[0] + (1.0 - gains.dry) * low[0] + gains.wet * (mid + side));
        if (right != nullptr)
            right[n] = (float) (gains.dry * x[1] + (1.0 - gains.dry) * low[1] + gains.wet * (mid - side));
    }
}

} // namespace ampsim

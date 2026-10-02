#include "Multivoicer.h"

#include <cmath>

namespace ampsim
{

namespace
{
constexpr double sqrt2 = 1.4142135623730951;
constexpr double ln2Over1200 = 0.00057762265046662105; // ln(2) / 1200: one cent as a ratio, to first order
} // namespace

Multivoicer::Settings Multivoicer::startingPoint (StartingPoint point)
{
    Settings s;
    const auto voice = [] (double semitones, double cents, double delayMs, double pan, double levelDb, double drift)
    {
        Voice v;
        v.semitones = semitones;
        v.cents = cents;
        v.delayMs = delayMs;
        v.pan = pan;
        v.levelDb = levelDb;
        v.drift = drift;
        return v;
    };

    // Unused slots hold a neutral voice, so raising the voice count brings in something sensible.
    for (auto& v : s.voices)
        v = voice (0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

    switch (point)
    {
        case StartingPoint::unisonDouble:
            s.voiceCount = 2;
            s.voices[0] = voice (0.0, 8.0, 0.0, -1.0, 0.0, 1.0);
            s.voices[1] = voice (0.0, -10.0, 7.0, 1.0, 0.0, 1.0);
            break;
        case StartingPoint::octaveStack:
            s.voiceCount = 2;
            s.voices[0] = voice (-12.0, 0.0, 0.0, -0.3, 0.0, 0.0);
            s.voices[1] = voice (12.0, 0.0, 0.0, 0.3, 0.0, 0.0);
            break;
        case StartingPoint::fifthsStack:
            s.voiceCount = 2;
            s.voices[0] = voice (7.0, 0.0, 0.0, -0.35, 0.0, 0.0);
            s.voices[1] = voice (19.0, 0.0, 0.0, 0.35, -6.0, 0.0);
            break;
        case StartingPoint::doubleOctaves:
            s.voiceCount = 4;
            s.voices[0] = voice (0.0, 8.0, 0.0, -1.0, 0.0, 1.0);
            s.voices[1] = voice (0.0, -10.0, 7.0, 1.0, 0.0, 1.0);
            s.voices[2] = voice (-12.0, 0.0, 0.0, -0.4, -6.0, 0.0);
            s.voices[3] = voice (12.0, 0.0, 0.0, 0.4, -6.0, 0.0);
            break;
    }
    return s;
}

Multivoicer::MixGains Multivoicer::mixGains (double mix) noexcept
{
    // Equal power: the wet is decorrelated from the dry. cos(0) is exactly 1 (mix 0 is bit-exact), and the
    // far end is pinned because cos(pi/2) in floating point is 6e-17.
    if (mix <= 0.0)
        return { 1.0, 0.0 };
    if (mix >= 1.0)
        return { 0.0, 1.0 };
    const auto angle = mix * juce::MathConstants<double>::halfPi;
    return { std::cos (angle), std::sin (angle) };
}

std::pair<double, double> Multivoicer::panGains (double pan) noexcept
{
    const auto theta = (juce::jlimit (-1.0, 1.0, pan) + 1.0) * juce::MathConstants<double>::pi / 4.0;
    return { sqrt2 * std::cos (theta), sqrt2 * std::sin (theta) };
}

// ---- Settings (audio thread) ------------------------------------------------------------------

void Multivoicer::configureVoice (int index, bool restart) noexcept
{
    auto& st = voices[(size_t) index];
    const auto& v = settings.voices[(size_t) index];

    GranularVoice::Settings g;
    g.ratio = ratioOf (v);
    g.delayMs = juce::jlimit (0.0, maxDelayMs, v.delayMs);
    g.glideMs = glideMs;
    st.granular.setSettings (g);

    const auto [l, r] = panGains (v.pan * juce::jlimit (0.0, 1.0, settings.spread));
    const auto level = v.levelDb <= minLevelDb ? 0.0 : juce::Decibels::decibelsToGain (juce::jmin (maxLevelDb, v.levelDb));
    const auto drift = juce::jlimit (0.0, 1.0, v.drift);

    if (restart)
    {
        // Starting from silence: a fresh voice at the new settings with no glides, faded in from zero.
        st.granular.reset();
        st.left.setCurrentAndTargetValue (l);
        st.right.setCurrentAndTargetValue (r);
        st.drift.setCurrentAndTargetValue (drift);
        st.level.setCurrentAndTargetValue (0.0);
    }
    else
    {
        st.left.setTargetValue (l);
        st.right.setTargetValue (r);
        st.drift.setTargetValue (drift);
    }
    st.level.setTargetValue (st.wanted ? level : 0.0);
}

void Multivoicer::setSettings (const Settings& newSettings) noexcept
{
    settings = newSettings;
    const auto count = juce::jlimit (0, maxVoices, settings.voiceCount);

    // The wet bus is divided by sqrt(sum of the wanted voices' squared levels).
    double power = 0.0;
    for (int i = 0; i < maxVoices; ++i)
    {
        auto& st = voices[(size_t) i];
        const auto& v = settings.voices[(size_t) i];
        st.wanted = i < count && v.levelDb > minLevelDb;
        const auto restart = st.wanted && ! st.running;
        configureVoice (i, restart);
        if (st.wanted)
        {
            st.running = true;
            const auto g = juce::Decibels::decibelsToGain (juce::jmin (maxLevelDb, v.levelDb));
            power += g * g;
        }
    }
    normalization.setTargetValue (power > 0.0 ? 1.0 / std::sqrt (power) : 1.0);

    mix.setTargetValue (juce::jlimit (0.0, 1.0, settings.mix));

    const auto hz = juce::jlimit (minHighPassHz, maxHighPassHz, settings.wetHighPassHz);
    if (settings.wetHighPass && highPassOn.getCurrentValue() <= 0.0 && ! highPassOn.isSmoothing())
    {
        // Switched on from fully off: start clean at the asked frequency.
        highPassHz.setCurrentAndTargetValue (hz);
        designHighPass (hz);
        for (auto& f : highPass)
            f.reset();
    }
    highPassOn.setTargetValue (settings.wetHighPass ? 1.0 : 0.0);
    highPassHz.setTargetValue (hz);
}

void Multivoicer::designHighPass (double frequency) noexcept
{
    const auto c = Svf::design (Svf::Type::highpass, frequency, 0.70710678118654752, 0.0, sampleRate);
    for (auto& f : highPass)
        f.setCoefficients (c);
}

// ---- Processing (audio thread) ----------------------------------------------------------------

void Multivoicer::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    input.prepare (sampleRate, GranularVoice::maxDelayMs (maxDelayMs, driftDelayMs), GranularVoice::searchMarginMs);

    for (int i = 0; i < maxVoices; ++i)
    {
        auto& st = voices[(size_t) i];
        st.granular.prepare (sampleRate);
        for (auto* s : { &st.level, &st.left, &st.right, &st.drift })
            s->reset (sampleRate, smoothingSeconds);

        // Each voice wanders on its own random sequences.
        st.pitchDrift = Lfo (1000 + 2 * i);
        st.timeDrift = Lfo (1001 + 2 * i);
        for (auto* lfo : { &st.pitchDrift, &st.timeDrift })
        {
            lfo->prepare (sampleRate);
            lfo->setShape (Lfo::Shape::random);
        }
        st.pitchDrift.setRate (driftPitchHz * (1.0 + 0.07 * i));
        st.timeDrift.setRate (driftTimeHz * (1.0 + 0.11 * i));
        st.running = false;
    }

    for (auto* s : { &mix, &normalization, &highPassOn })
        s->reset (sampleRate, smoothingSeconds);
    highPassHz.reset (sampleRate, highPassSmoothingSeconds);

    reset();
}

void Multivoicer::reset()
{
    input.reset();
    for (int i = 0; i < maxVoices; ++i)
    {
        auto& st = voices[(size_t) i];
        st.running = false;
        st.pitchDrift.setSeed (1000 + 2 * i);
        st.timeDrift.setSeed (1001 + 2 * i);
        st.pitchDrift.setPhase (0.0);
        st.timeDrift.setPhase (0.0);
    }
    setSettings (settings);

    // Silent, so every ramp jumps to its target.
    for (auto& st : voices)
        for (auto* s : { &st.level, &st.left, &st.right, &st.drift })
            s->setCurrentAndTargetValue (s->getTargetValue());
    for (auto* s : { &mix, &normalization, &highPassOn })
        s->setCurrentAndTargetValue (s->getTargetValue());
    highPassHz.setCurrentAndTargetValue (highPassHz.getTargetValue());
    designHighPass (highPassHz.getTargetValue());
    for (auto& f : highPass)
        f.reset();
    samplesUntilUpdate = 0;
}

void Multivoicer::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numChannels = std::min (2, (int) block.getNumChannels());
    const auto numSamples = (int) block.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    float* const left = block.getChannelPointer (0);
    float* const right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;
    const auto driftSamples = driftDelayMs * sampleRate / 1000.0;

    for (int n = 0; n < numSamples; ++n)
    {
        if (samplesUntilUpdate-- <= 0)
        {
            samplesUntilUpdate = coefficientInterval - 1;
            if (highPassHz.isSmoothing())
                designHighPass (highPassHz.skip (coefficientInterval));
        }

        const double x[2] = { (double) left[n], right != nullptr ? (double) right[n] : (double) left[n] };
        const auto mono = 0.5 * (x[0] + x[1]);

        // The voices read the input before this sample is pushed (read before write).
        double wet[2] = { 0.0, 0.0 };
        for (auto& st : voices)
        {
            if (! st.running)
                continue;

            const auto gain = st.level.getNextValue();
            const auto l = st.left.getNextValue(), r = st.right.getNextValue();
            const auto amount = st.drift.getNextValue();

            // Drift: +-3 cents of pitch (one cent is a ratio of 1 + ln2/1200 to first order; the error at 3
            // cents is 0.003 cents) and 0 to 2 ms of extra delay, both on smoothed random LFOs.
            const auto ratioScale = 1.0 + amount * driftCents * ln2Over1200 * (double) st.pitchDrift.next();
            const auto extraDelay = amount * driftSamples * 0.5 * (1.0 + (double) st.timeDrift.next());

            const auto y = (double) st.granular.process (input, ratioScale, extraDelay);
            wet[0] += gain * l * y;
            wet[1] += gain * r * y;

            if (! st.wanted && ! st.level.isSmoothing())
                st.running = false; // faded out
        }
        input.push ((float) mono);

        const auto norm = normalization.getNextValue();
        wet[0] *= norm;
        wet[1] *= norm;

        // Optional wet high-pass, crossfaded in and out.
        const auto hp = highPassOn.getNextValue();
        if (hp > 0.0 || highPassOn.isSmoothing())
            for (size_t ch = 0; ch < 2; ++ch)
                wet[ch] += hp * (highPass[ch].processSample (wet[ch]) - wet[ch]);

        const auto gains = mixGains (mix.getNextValue());
        left[n] = (float) (gains.dry * x[0] + gains.wet * wet[0]);
        if (right != nullptr)
            right[n] = (float) (gains.dry * x[1] + gains.wet * wet[1]);
    }
}

} // namespace ampsim

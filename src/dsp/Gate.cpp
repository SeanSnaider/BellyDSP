// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Gate.h"

#include <algorithm>
#include <cmath>

namespace ampsim
{

namespace
{
/// 24 dB/oct Butterworth as two second-order sections: Q_k = 1 / (2 sin((2k - 1) pi / 8)), k = 1, 2.
constexpr double butterworthQ[2] = { 1.3065629648763766, 0.5411961001461970 };

double toDb (double linear) { return 20.0 * std::log10 (std::max (linear, Gate::minimumLevel)); }
} // namespace

double Gate::adaptiveReleaseMs (double gapDb, double knobMs)
{
    const auto slow = knobMs, fast = std::min (adaptiveFastReleaseMs, knobMs);
    if (gapDb <= adaptiveSlowGapDb)
        return slow;
    if (gapDb >= adaptiveFastGapDb)
        return fast;

    // Between the two: interpolate the logarithm of the time, so equal steps of gap are equal ratios
    // of release time.
    const auto t = (gapDb - adaptiveSlowGapDb) / (adaptiveFastGapDb - adaptiveSlowGapDb);
    return std::exp (std::log (slow) + t * (std::log (fast) - std::log (slow)));
}

double Gate::releaseCoefficient (double ms, double rate)
{
    // Falling 60 dB in N = ms fs samples: a^N = 10^(-60/20), so a = 10^(-3 / N).
    return std::pow (10.0, -releaseRangeDb / 20.0 / (ms * 0.001 * rate));
}

float Gate::thresholdForNoiseFloor (double noiseFloorDb, double hysteresis)
{
    return (float) juce::jlimit ((double) minThresholdDb, (double) maxThresholdDb, noiseFloorDb + learnMarginDb + hysteresis);
}

double Gate::floorFor (float rangeDb)
{
    return rangeDb <= muteDb ? 0.0 : std::pow (10.0, (double) rangeDb / 20.0);
}

void Gate::setSettings (const Settings& newSettings)
{
    auto s = newSettings;
    s.thresholdDb = juce::jlimit (minThresholdDb, maxThresholdDb, s.thresholdDb);
    s.hysteresisDb = juce::jlimit (0.0f, maxHysteresisDb, s.hysteresisDb);
    s.holdMs = juce::jlimit (0.0f, maxHoldMs, s.holdMs);
    s.attackMs = juce::jlimit (minAttackMs, maxAttackMs, s.attackMs);
    s.releaseMs = juce::jlimit (minReleaseMs, maxReleaseMs, s.releaseMs);
    s.rangeDb = juce::jlimit (muteDb, 0.0f, s.rangeDb);
    s.sidechainHz = juce::jlimit (minSidechainHz, maxSidechainHz, s.sidechainHz);

    const bool timesChanged = ! juce::exactlyEqual (s.holdMs, settings.holdMs) || ! juce::exactlyEqual (s.attackMs, settings.attackMs)
                              || ! juce::exactlyEqual (s.releaseMs, settings.releaseMs) || s.releaseMode != settings.releaseMode;
    settings = s;

    thresholdDb.setTargetValue (s.thresholdDb);
    hysteresisDb.setTargetValue (s.hysteresisDb);
    floorGain.setTargetValue (floorFor (s.rangeDb));
    sidechainMix.setTargetValue (s.sidechainHighPass ? 1.0 : 0.0);
    sourceMix.setTargetValue (s.detector == DetectorSource::ownInput ? 1.0 : 0.0);
    sidechainHz.setTargetValue (s.sidechainHz);

    if (timesChanged)
        updateTimes();
}

void Gate::updateTimes()
{
    holdSamples = juce::roundToInt ((double) settings.holdMs * 0.001 * sampleRate);
    attackStep = 1.0 / std::max (1.0, (double) settings.attackMs * 0.001 * sampleRate);
    releaseDirty = true; // the release factor depends on the knob and the mode
}

void Gate::designSidechain (double hz)
{
    for (size_t i = 0; i < sidechain.size(); ++i)
        sidechain[i].setCoefficients (Svf::design (Svf::Type::highpass, hz, butterworthQ[i], 0.0, sampleRate));
}

void Gate::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    gainCurve.assign ((size_t) std::max (1, maxBlockSize), 1.0f);
    curveLength = 0;
    chunkLength = std::max (1, juce::roundToInt (chunkSeconds * sampleRate));
    slowFall = std::pow (10.0, -slowFallDbPerSecond / 20.0 / sampleRate); // falls 100 dB per second
    fadeLength = std::max (1, juce::roundToInt (linkFadeSeconds * sampleRate));
    learnTotal = juce::roundToInt (learnSeconds * sampleRate);

    // Start on the current settings with no ramps.
    for (auto* s : { &thresholdDb, &hysteresisDb, &floorGain, &sidechainMix, &sourceMix })
        s->reset (sampleRate, smoothingSeconds);
    sidechainHz.reset (sampleRate, smoothingSeconds);
    designSidechain (sidechainHz.getTargetValue());
    samplesSinceDesign = 0;

    updateTimes();
    resetState();
}

void Gate::resetDetector()
{
    for (auto& f : sidechain)
        f.reset();
    chunks.fill (0.0);
    chunkFill = chunkPosition = 0;
    chunkMax = chunksMax = 0.0;
    slowFollower = 0.0;
}

void Gate::resetState()
{
    resetDetector();

    // Open, with the hold armed: if nothing is playing the gate closes through its release, and if
    // something is, it stays open. Either way it starts from exactly the dry signal, which is what
    // the chain's bypass crossfade fades in from.
    state = State::open;
    openness = attackStart = 1.0;
    attackPhase = 0.0;
    holdLeft = holdSamples;
    belowClose = false;
    gapPeak = 0.0;
    releaseDirty = true;

    lastSource = CurveSource::none;
    lastApplied = fadeFrom = 1.0f;
    fadeLeft = 0;
    ownEnvelopeStale = false;
    keptWarm = false;

    // A Learn measurement cut off here (a device restart, or the chain re-enabling the gate) starts
    // over at the next buffer rather than reporting a partial histogram, or never finishing.
    if (learnLeft > 0)
        learnRequested.store (true, std::memory_order_release);
    learnLeft = 0;

    meterDetector.store ((float) toDb (0.0), std::memory_order_relaxed);
    meterReduction.store (0.0f, std::memory_order_relaxed);
    meterOpenState.store (true, std::memory_order_relaxed);
}

void Gate::reset()
{
    // A gate whose curve computeGainCurve() kept current while the chain skipped its process() (Gate A
    // switched off while Gate B follows it) keeps its state: starting it over would make the Gate B
    // that's applying its curve jump.
    if (keptWarm)
    {
        keptWarm = false;
        return;
    }

    resetState();
}

void Gate::startLearn() noexcept
{
    learnProgress.store (0.0f, std::memory_order_relaxed);
    learning.store (true, std::memory_order_relaxed);
    learnRequested.store (true, std::memory_order_release);
}

bool Gate::isLearning() const noexcept
{
    return learnRequested.load (std::memory_order_acquire) || learning.load (std::memory_order_relaxed);
}

void Gate::learnSample (double levelDb) noexcept
{
    const auto bin = (int) std::floor ((levelDb - learnFloorDb) / learnBinDb);
    ++learnHistogram[(size_t) juce::jlimit (0, learnBins - 1, bin)];

    if (--learnLeft == 0)
        finishLearn();
}

void Gate::finishLearn() noexcept
{
    // The noise floor: the 95th percentile of the detector level, at its bin's upper edge.
    const auto needed = (int) std::ceil (learnPercentile * learnTotal);
    int running = 0, bin = 0;
    for (; bin < learnBins - 1; ++bin)
    {
        running += learnHistogram[(size_t) bin];
        if (running >= needed)
            break;
    }

    const auto noise = learnFloorDb + (bin + 1) * learnBinDb;
    learnedNoiseFloor.store ((float) noise, std::memory_order_relaxed);
    learnedThreshold.store (thresholdForNoiseFloor (noise, settings.hysteresisDb), std::memory_order_relaxed);
    learnProgress.store (1.0f, std::memory_order_relaxed);
    learning.store (false, std::memory_order_relaxed);
    learnCount.fetch_add (1, std::memory_order_release); // publishes the two values above
}

double Gate::windowedPeak (double magnitude) noexcept
{
    // The largest |x| over the chunk being filled plus the last numChunks completed chunks. A sample
    // counts the moment it arrives; a chunk leaves the window numChunks chunks after it completed.
    if (magnitude > chunkMax)
        chunkMax = magnitude;
    const auto peak = chunkMax > chunksMax ? chunkMax : chunksMax;

    if (++chunkFill == chunkLength)
    {
        chunks[(size_t) chunkPosition] = chunkMax;
        chunkPosition = (chunkPosition + 1) % numChunks;
        chunksMax = *std::max_element (chunks.begin(), chunks.end());
        chunkMax = 0.0;
        chunkFill = 0;
    }

    return peak;
}

double Gate::detectorSample (double di, double own) noexcept
{
    // The source: crossfaded for 20 ms when it's switched, so the detector never sees a step.
    double v;
    if (sourceMix.isSmoothing())
    {
        const auto m = sourceMix.getNextValue();
        v = di + m * (own - di);
    }
    else
    {
        v = sourceMix.getTargetValue() > 0.5 ? own : di;
    }

    // The sidechain high-pass. Its frequency glides (redesigned every 32 samples); switching it on or
    // off crossfades with the raw signal. It always runs, so it's warm when switched back on.
    if (sidechainHz.isSmoothing())
    {
        const auto hz = sidechainHz.getNextValue();
        if (++samplesSinceDesign >= coefficientInterval || ! sidechainHz.isSmoothing())
        {
            designSidechain (hz);
            samplesSinceDesign = 0;
        }
    }

    const auto filtered = sidechain[1].processSample (sidechain[0].processSample (v));
    double u;
    if (sidechainMix.isSmoothing())
    {
        const auto m = sidechainMix.getNextValue();
        u = v + m * (filtered - v);
    }
    else
    {
        u = sidechainMix.getTargetValue() > 0.5 ? filtered : v;
    }

    return windowedPeak (std::abs (u));
}

void Gate::resumeOwnEnvelope() noexcept
{
    // An external curve was applied since this gate last ran its own, so its detector state is stale:
    // start the detector fresh, and pick the envelope up from the gain last applied, so the gain
    // carries on from where it was.
    resetDetector();
    const auto floor = floorGain.getCurrentValue();
    openness = floor < 1.0 ? juce::jlimit (0.0, 1.0, ((double) lastApplied - floor) / (1.0 - floor)) : 1.0;

    if (openness >= 1.0)
    {
        state = State::open;
        holdLeft = holdSamples;
    }
    else
    {
        state = State::releasing;
    }

    belowClose = false;
    gapPeak = 0.0;
    releaseDirty = true;
    ownEnvelopeStale = false;
}

void Gate::computeCurve (const float* di, const float* own, int numSamples) noexcept
{
    jassert (numSamples <= (int) gainCurve.size());
    numSamples = std::min (numSamples, (int) gainCurve.size());

    if (learnRequested.exchange (false, std::memory_order_acq_rel))
    {
        learnHistogram.fill (0);
        learnLeft = learnTotal;
        learning.store (true, std::memory_order_relaxed);
        learnProgress.store (0.0f, std::memory_order_relaxed);
    }

    if (ownEnvelopeStale)
        resumeOwnEnvelope();

    double loudest = 0.0, lowestGain = 1.0;

    for (int n = 0; n < numSamples; ++n)
    {
        const auto openDb = thresholdDb.getNextValue();
        const auto closeDb = openDb - hysteresisDb.getNextValue();
        const auto floor = floorGain.getNextValue();

        // 1-2. The detector's peak level, and the slow follower beside it.
        const auto peak = detectorSample ((double) di[n], (double) own[n]);
        const auto levelDb = toDb (peak);
        const auto held = slowFollower * slowFall;
        slowFollower = peak > held ? peak : held;
        loudest = std::max (loudest, peak);

        if (learnLeft > 0)
            learnSample (levelDb);

        // The gap between the slow follower and the level: near 0 dB while a note rings out, large
        // after a stop. Only needed while the level is below the close threshold and the gate is
        // still (partly) open.
        const auto gap = [&] { return toDb (slowFollower) - levelDb; };

        // 3. The decision, with hysteresis: opening needs the open threshold, staying open only the
        // close threshold.
        if (state == State::releasing)
        {
            if (levelDb >= openDb)
            {
                state = State::opening;
                attackPhase = 0.0;
                attackStart = openness;
                holdLeft = holdSamples;
                belowClose = false;
            }
            else if (openness > 0.0)
            {
                gapPeak = std::max (gapPeak, gap());
            }
        }
        else if (levelDb >= closeDb)
        {
            holdLeft = holdSamples;
            belowClose = false;
        }
        else
        {
            if (! belowClose)
            {
                belowClose = true;
                gapPeak = gap();
            }
            else
            {
                gapPeak = std::max (gapPeak, gap());
            }

            if (holdLeft > 0)
                --holdLeft;
            else
                state = State::releasing;
        }

        // 4. The envelope.
        if (state == State::opening)
        {
            attackPhase += attackStep;
            if (attackPhase >= 1.0)
            {
                state = State::open;
                openness = 1.0;
            }
            else
            {
                // Raised cosine from where it was: zero slope at both ends.
                openness = attackStart + (1.0 - attackStart) * 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * attackPhase));
            }
        }
        else if (state == State::releasing && openness > 0.0)
        {
            // 5. The release: the knob, or in adaptive mode whatever the largest gap since the level fell
            // below the close threshold calls for. Redesigned only when that changes.
            const auto gapUsed = settings.releaseMode == ReleaseMode::adaptive ? gapPeak : 0.0;
            if (releaseDirty || ! juce::exactlyEqual (gapUsed, coefficientGap))
            {
                coefficientGap = gapUsed;
                releaseDirty = false;
                releaseFactor = releaseCoefficient (adaptiveReleaseMs (gapUsed, settings.releaseMs), sampleRate);
            }

            openness *= releaseFactor;
            if (openness < snapToClosed)
                openness = 0.0;
        }

        const auto gain = 1.0 - (1.0 - floor) * (1.0 - openness);
        gainCurve[(size_t) n] = (float) gain;
        lowestGain = std::min (lowestGain, gain);
    }

    curveLength = numSamples;

    if (learnLeft > 0)
        learnProgress.store (1.0f - (float) learnLeft / (float) learnTotal, std::memory_order_relaxed);

    const auto openNow = thresholdDb.getCurrentValue();
    meterDetector.store ((float) toDb (loudest), std::memory_order_relaxed);
    meterOpen.store ((float) openNow, std::memory_order_relaxed);
    meterClose.store ((float) (openNow - hysteresisDb.getCurrentValue()), std::memory_order_relaxed);
    meterReduction.store (lowestGain > 1.0e-5 ? std::max (0.0f, (float) -toDb (lowestGain)) : 100.0f, std::memory_order_relaxed);
    meterOpenState.store (state != State::releasing, std::memory_order_relaxed);
}

void Gate::applyCurve (float* audio, const float* gain, int numSamples, CurveSource source) noexcept
{
    // Switching between this gate's own curve and an external one: fade from the last gain applied
    // to the new curve over 10 ms, so linking or unlinking never steps the gain.
    if (source != lastSource)
    {
        if (lastSource != CurveSource::none)
        {
            fadeFrom = lastApplied;
            fadeLeft = fadeLength;
        }
        lastSource = source;
    }

    auto lowest = 1.0f;
    for (int n = 0; n < numSamples; ++n)
    {
        auto g = gain[n];
        if (fadeLeft > 0)
        {
            const auto w = (float) (fadeLength - fadeLeft) / (float) fadeLength;
            g = fadeFrom + w * (g - fadeFrom);
            --fadeLeft;
        }

        audio[n] *= g;
        lowest = std::min (lowest, g);
        lastApplied = g;
    }

    meterReduction.store (lowest > 1.0e-5f ? std::max (0.0f, (float) -toDb (lowest)) : 100.0f, std::memory_order_relaxed);
}

void Gate::process (juce::dsp::AudioBlock<float> block, const BlockContext& context)
{
    const auto numSamples = (int) block.getNumSamples();
    auto* audio = block.getChannelPointer (0);

    // The whole curve is computed from the untouched input before any of it is applied.
    computeCurve (context.di != nullptr ? context.di : audio, audio, numSamples);
    applyCurve (audio, gainCurve.data(), curveLength, CurveSource::own);
    keptWarm = false;
}

void Gate::computeGainCurve (const float* detector, int numSamples) noexcept
{
    computeCurve (detector, detector, numSamples);
    keptWarm = true;
}

void Gate::processWithGain (juce::dsp::AudioBlock<float> block, const float* gain) noexcept
{
    applyCurve (block.getChannelPointer (0), gain, (int) block.getNumSamples(), CurveSource::external);
    ownEnvelopeStale = true;
    keptWarm = false;

    // This gate's own detector isn't running: its level meter reads silence, and "open" means the
    // applied gain is.
    meterDetector.store ((float) toDb (0.0), std::memory_order_relaxed);
    meterOpenState.store (lastApplied > 0.5f, std::memory_order_relaxed);
}

} // namespace ampsim

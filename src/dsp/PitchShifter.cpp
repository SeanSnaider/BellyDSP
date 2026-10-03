// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "PitchShifter.h"

#include <cmath>

namespace ampsim
{

double pitchShifterDot (const float* a, const float* b, int n) noexcept
{
    // A float times a float is exact in a double, so only the additions round; four running sums let the
    // CPU overlap them (as PitchDetector's dot does).
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        s0 += (double) a[i] * (double) b[i];
        s1 += (double) a[i + 1] * (double) b[i + 1];
        s2 += (double) a[i + 2] * (double) b[i + 2];
        s3 += (double) a[i + 3] * (double) b[i + 3];
    }
    for (; i < n; ++i)
        s0 += (double) a[i] * (double) b[i];
    return (s0 + s1) + (s2 + s3);
}

namespace
{
constexpr double tiny = 1.0e-30; // below this a window counts as silent

double normalized (double c, double ea, double eb) noexcept
{
    const auto e = ea * eb;
    return e > tiny ? c / std::sqrt (e) : 0.0;
}
} // namespace

// ---- RatioGlide --------------------------------------------------------------------------------

void RatioGlide::setTarget (double value) noexcept
{
    if (juce::exactlyEqual (value, target))
        return;
    target = value;
    if (glideSamples <= 0 || current <= 0.0)
    {
        current = target;
        remaining = 0;
        return;
    }
    // A constant factor per sample: a straight line in log frequency (cents per sample).
    remaining = glideSamples;
    step = std::exp (std::log (target / current) / (double) remaining);
}

// ---- PitchShifterInput -------------------------------------------------------------------------

void PitchShifterInput::prepare (double newSampleRate, double maxDelayMs, double searchMarginMs)
{
    sampleRate = newSampleRate;
    line.prepare (sampleRate, maxDelayMs, 0.05);

    const auto reach = (int) std::ceil ((maxDelayMs + searchMarginMs) * sampleRate / 1000.0) + 8;
    history.prepare (reach);
    decimated.prepare (reach / decimation + 8);

    // 4th-order Butterworth: two sections with Q_k = 1 / (2 sin((2k - 1) pi / 8)), 1.307 and 0.541.
    for (int s = 0; s < 2; ++s)
    {
        const auto q = 1.0 / (2.0 * std::sin ((2.0 * s + 1.0) * juce::MathConstants<double>::pi / 8.0));
        lowpass[(size_t) s].setCoefficients (Svf::design (Svf::Type::lowpass, lowpassHz, q, 0.0, sampleRate));
    }
    reset();
}

void PitchShifterInput::reset() noexcept
{
    line.reset();
    history.reset();
    decimated.reset();
    for (auto& section : lowpass)
        section.reset();
    phase = 0;
    time = 0;
}

// ---- GranularVoice -----------------------------------------------------------------------------

void GranularVoice::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    const auto samples = [this] (double ms) { return ms * sampleRate / 1000.0; };

    alignRange = juce::roundToInt (samples (alignRangeMs));
    window = juce::roundToInt (samples (windowMs));
    coarseWindow = window / PitchShifterInput::decimation;
    jumpFloor = samples (jumpFloorMs);
    fadeTravel = samples (fadeTravelMs);
    minFade = samples (minFadeMs);
    maxFade = samples (maxFadeMs);
    floorDelay = samples (floorMs);
    bandWidth = 2.0 * alignRange + fadeTravel + jumpFloor;
    replaceThreshold = samples (replaceThresholdMs);
    earlyReach = samples (earlyRangeMs) + fadeTravel + jumpFloor;

    setSettings (settings);
    reset();
}

void GranularVoice::reset() noexcept
{
    ratio.jumpTo (juce::jlimit (minRatio, maxRatio, settings.ratio));
    targetDelay = placedDelay = std::max (0.0, settings.delayMs) * sampleRate / 1000.0;
    drift = 0.0;
    bandLow = placedDelay + floorDelay;

    heads[0] = { bandLow + 0.5 * bandWidth, true };
    heads[1] = { 0.0, false };
    current = 0;
    fading = false;
    earlyTried = false;
    fadePosition = fadeStep = fadeCorrelation = 0.0;
    splices = 0;
    lastSplice = {};
}

void GranularVoice::setSettings (const Settings& newSettings) noexcept
{
    settings = newSettings;
    ratio.setGlideSamples (juce::roundToInt (std::max (0.0, settings.glideMs) * sampleRate / 1000.0));
    ratio.setTarget (juce::jlimit (minRatio, maxRatio, settings.ratio));
    targetDelay = std::max (0.0, settings.delayMs) * sampleRate / 1000.0;
}

double GranularVoice::fadeLength (double slope) const noexcept
{
    // The old head travels |slope| per sample while it fades: at most fadeTravel in all, so 3 ms / |1 - r|,
    // between 1 and 6 ms.
    const auto speed = std::abs (slope);
    if (speed * maxFade <= fadeTravel)
        return maxFade;
    return std::max (minFade, fadeTravel / speed);
}

void GranularVoice::startFade (double newDelay, double correlation, double fadeSamples) noexcept
{
    auto& incoming = heads[(size_t) (1 - current)];
    incoming.delay = newDelay;
    incoming.active = true;
    fading = true;
    earlyTried = false;
    fadePosition = 0.0;
    fadeStep = 1.0 / std::max (1.0, fadeSamples);
    fadeCorrelation = juce::jlimit (0.0, 1.0, correlation);
}

void GranularVoice::maybeSplice (const PitchShifterInput& input, double slope) noexcept
{
    const auto delayA = heads[(size_t) current].delay;

    // A changed voice delay: one crossfade to a head at exactly the new delay, same place in the band.
    if (std::abs (targetDelay - placedDelay) >= replaceThreshold)
    {
        const auto jump = targetDelay - placedDelay;
        placedDelay = targetDelay;
        bandLow = placedDelay + floorDelay + drift;
        ++splices;
        lastSplice = { jump, correlationAt (input, delayA, juce::roundToInt (jump)), true };
        startFade (delayA + jump, lastSplice.correlation, maxFade);
        return;
    }

    const auto fade = fadeLength (slope);
    const auto lo = bandLow, hi = bandLow + bandWidth;

    // r < 1, early: once the head has climbed earlyReach above the bottom, one search for a jump that lands
    // anywhere from the bottom up to a jump floor below the head, as low as a good match allows. A single note
    // (period up to 12 ms) always has one, so its head never climbs the whole band and the trail stays short;
    // a chord that needs a longer jump fails the quality bar and climbs on to the full search at the top.
    if (slope > 0.0 && ! earlyTried && delayA + slope * fade >= lo + earlyReach && delayA + slope * fade < hi)
    {
        earlyTried = true;
        const auto early = findSplice (input, delayA, lo - delayA, -jumpFloor, lo - delayA);
        if (early.matched && early.correlation >= earlyQuality)
        {
            lastSplice = early;
            ++splices;
            startFade (delayA + early.jump, correlationAt (input, delayA, juce::roundToInt (early.jump), window), fade);
            return;
        }
    }

    bool need = false;

    if (slope < 0.0) // r > 1: the delay falls; splice before the fading head would leave the band
        need = delayA + slope * fade <= lo || delayA > hi;
    else if (slope > 0.0) // r < 1: the delay rises
        need = delayA + slope * fade >= hi || delayA < lo;
    else
        need = delayA < lo || delayA > hi;

    if (! need)
        return;

    // Settle any small pending delay change with this splice.
    placedDelay = targetDelay;
    bandLow = placedDelay + floorDelay + drift;
    const auto low = bandLow, high = bandLow + bandWidth, middle = bandLow + 0.5 * bandWidth;

    // Where the new head may land: the stretch of the band it is about to sweep across.
    double targetLo, targetHi;
    if (slope < 0.0)
    {
        targetLo = high - 2.0 * alignRange;
        targetHi = high;
    }
    else if (slope > 0.0)
    {
        targetLo = low;
        targetHi = low + 2.0 * alignRange;
    }
    else
    {
        targetLo = middle - alignRange;
        targetHi = middle + alignRange;
    }

    // Near-ties go to the landing that keeps the trail short: the middle of the stretch for r > 1 (the head
    // falls from there), the bottom for r < 1 (it climbs from there through the band). Without a match
    // (silence) the new head goes to the middle of its stretch, placed absolutely, so the result doesn't
    // depend on the old head's delay down to its last bit.
    const auto preferred = (slope > 0.0 ? targetLo : 0.5 * (targetLo + targetHi)) - delayA;
    lastSplice = findSplice (input, delayA, targetLo - delayA, targetHi - delayA, preferred);
    const auto newDelay = lastSplice.matched ? delayA + lastSplice.jump : 0.5 * (targetLo + targetHi);
    lastSplice.jump = newDelay - delayA;
    const auto rho = lastSplice.matched ? correlationAt (input, delayA, juce::roundToInt (lastSplice.jump), window) : 0.0;
    ++splices;
    startFade (newDelay, rho, fade);
}

float GranularVoice::process (const PitchShifterInput& input, double ratioScale, double extraDelaySamples) noexcept
{
    // Every head moves by 1 - r this sample (the pitch), plus any change in the drift offset, which also
    // moves the band, so drift never causes a splice.
    const auto slope = 1.0 - ratio.next() * ratioScale;
    const auto move = slope + (extraDelaySamples - drift);
    drift = extraDelaySamples;
    bandLow = placedDelay + floorDelay + drift;

    for (auto& head : heads)
        if (head.active)
            head.delay += move;

    if (! fading)
        maybeSplice (input, slope);

    const auto& a = heads[(size_t) current];
    if (! fading)
        return input.read (a.delay);

    // Correlation-matched crossfade: cos and sin of (pi/2) u, both divided by sqrt(1 + 2 rho sin cos), so
    // a^2 + b^2 + 2 rho a b = 1 at every point of the fade.
    const auto& b = heads[(size_t) (1 - current)];
    const auto angle = juce::MathConstants<double>::halfPi * fadePosition;
    const auto s = std::sin (angle), c = std::cos (angle);
    const auto norm = 1.0 / std::sqrt (1.0 + 2.0 * fadeCorrelation * s * c);
    const auto y = norm * (c * (double) input.read (a.delay) + s * (double) input.read (b.delay));

    fadePosition += fadeStep;
    if (fadePosition >= 1.0)
    {
        heads[(size_t) current].active = false;
        current = 1 - current;
        fading = false;
    }
    return (float) y;
}

double GranularVoice::correlationAt (const PitchShifterInput& input, double delayA, int jump, int windowOffset) const noexcept
{
    const auto a = (int) std::ceil (delayA) + windowOffset;
    const auto length = a + std::max (0, jump) + window;
    if (a + std::min (0, jump) < 1 || length > input.getHistoryCapacity())
        return 0.0;

    // Element `length - k` of newest(length) is the sample k back; A's window is delays a .. a + window - 1.
    const auto* base = input.newest (length);
    const auto* pa = base + (length - a - window + 1);
    const auto* pb = pa - jump;
    const auto rho = normalized (pitchShifterDot (pa, pb, window), pitchShifterDot (pa, pa, window), pitchShifterDot (pb, pb, window));
    return juce::jlimit (0.0, 1.0, rho);
}

GranularVoice::Splice GranularVoice::findSplice (const PitchShifterInput& input, double delayA, double jumpLo, double jumpHi,
                                                double preferred) const noexcept
{
    const auto middle = 0.5 * (jumpLo + jumpHi);
    const auto jLo = (int) std::ceil (jumpLo), jHi = (int) std::floor (jumpHi);
    const auto a = (int) std::ceil (delayA);
    if (jHi < jLo || a + jLo < 1)
        return { middle, 0.0, false };

    // ---- Coarse: jumps of 4 m samples on the 12 kHz copy --------------------------------------
    // Decimated sample j back sits at full-rate delay offset + 4 j, so A's window starts (newest end) at the
    // first j with offset + 4 j >= a.
    int fineLo = jLo, fineHi = jHi;
    const auto mLo = (int) std::ceil (jLo / (double) PitchShifterInput::decimation);
    const auto mHi = (int) std::floor (jHi / (double) PitchShifterInput::decimation);

    if (mLo <= mHi)
    {
        const auto offset = input.getDecimatedOffset();
        const auto ja = std::max (0, (int) std::ceil ((a - offset) / (double) PitchShifterInput::decimation));
        const auto wd = coarseWindow;
        const auto length = ja + std::max (0, mHi) + wd;
        if (ja + std::min (0, mLo) < 0 || length > input.getDecimatedCapacity())
            return { middle, 0.0, false };

        // Element length - 1 - j is decimated sample j back: A's window [ja, ja + wd - 1] starts (oldest end)
        // at element length - ja - wd, and B's, m samples older, at that minus m.
        const auto* base = input.newestDecimated (length);
        const auto* pa = base + (length - ja - wd);
        const auto ea = pitchShifterDot (pa, pa, wd);
        if (ea <= tiny)
            return { middle, 0.0, false };

        const auto span = std::max (1.0, jumpHi - jumpLo);
        auto bestScore = -1.0e300, bestRho = 0.0;
        auto bestM = mLo;

        for (int m = mLo; m <= mHi; ++m)
        {
            // Each window's energy is summed afresh: sliding it (add the new square, subtract the old) leaves
            // rounding residue from loud samples that have left the window, which swamps a window that has
            // decayed into silence and flips the choice.
            const auto* pb = pa - m;
            const auto rho = normalized (pitchShifterDot (pa, pb, wd), ea, pitchShifterDot (pb, pb, wd));
            // Near-ties (every multiple of a period fits a steady note) go to the jump nearest `preferred`.
            const auto score = rho - tieBreak * std::abs (PitchShifterInput::decimation * m - preferred) / span;
            if (score > bestScore)
            {
                bestScore = score;
                bestM = m;
            }
            bestRho = std::max (bestRho, rho);
        }
        if (bestRho <= 0.0) // nothing resembles the old head's input (silence, or nothing but anti-phase)
            return { middle, 0.0, false };
        fineLo = std::max (jLo, PitchShifterInput::decimation * bestM - 3);
        fineHi = std::min (jHi, PitchShifterInput::decimation * bestM + 3);
    }

    // ---- Fine: every whole-sample jump around the coarse winner, then a parabola ----------------
    constexpr int maxCandidates = 16;
    if (fineHi - fineLo + 1 > maxCandidates) // only when the coarse stage was skipped and the range is wide
        fineHi = fineLo + maxCandidates - 1;

    const auto length = a + std::max (0, fineHi) + window;
    if (a + std::min (0, fineLo) < 1 || length > input.getHistoryCapacity())
        return { middle, 0.0, false };

    const auto* base = input.newest (length);
    const auto* pa = base + (length - a - window + 1);
    const auto ea = pitchShifterDot (pa, pa, window);
    if (ea <= tiny)
        return { middle, 0.0, false };

    std::array<double, maxCandidates> rho {};
    int best = 0;
    for (int j = fineLo; j <= fineHi; ++j)
    {
        const auto* pb = pa - j;
        const auto k = (size_t) (j - fineLo);
        rho[k] = normalized (pitchShifterDot (pa, pb, window), ea, pitchShifterDot (pb, pb, window));
        if (rho[k] > rho[(size_t) best])
            best = (int) k;
    }

    if (rho[(size_t) best] <= 0.0)
        return { middle, 0.0, false };

    // Parabola through the winner and its neighbours: vertex delta = (y0 - y2) / (2 (y0 - 2 y1 + y2)),
    // height y1 - (y0 - y2) delta / 4 (as in PitchDetector).
    auto jump = (double) (fineLo + best);
    auto peak = rho[(size_t) best];
    if (best > 0 && best < fineHi - fineLo)
    {
        const auto y0 = rho[(size_t) best - 1], y1 = rho[(size_t) best], y2 = rho[(size_t) best + 1];
        const auto curvature = y0 - 2.0 * y1 + y2;
        if (curvature < 0.0)
        {
            const auto delta = juce::jlimit (-1.0, 1.0, 0.5 * (y0 - y2) / curvature);
            jump += delta;
            peak = y1 - 0.25 * (y0 - y2) * delta;
        }
    }
    return { juce::jlimit (jumpLo, jumpHi, jump), juce::jlimit (0.0, 1.0, peak), true };
}

} // namespace ampsim

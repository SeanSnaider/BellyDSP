#include "Psola.h"

#include <cmath>

namespace ampsim
{

namespace
{
double centsBetween (double a, double b) noexcept { return 1200.0 * std::log2 (a / b); }
} // namespace

// ---- PsolaAnalysis ---------------------------------------------------------------------------------

void PsolaAnalysis::prepare (double newSampleRate, double minFrequency)
{
    sampleRate = newSampleRate;
    auto s = PitchDetector::Settings::harmonizer();
    s.minFrequency = minFrequency;
    detector.prepare (sampleRate, s);
    confidenceStep = 1.0 / std::max (1.0, confidenceRampMs * sampleRate / 1000.0);
    reset();
}

void PsolaAnalysis::reset() noexcept
{
    detector.reset();
    hopCounter = 0;
    lastEstimate = {};
    previousConfident = 0.0;
    unconfident = 0;
    tracking = lost = false;
    period = 0.0;
    noteCount = 0;
    firstMark = numMarks = 0;
    confidence = 0.0;
}

void PsolaAnalysis::addMark (const Mark& m) noexcept
{
    if (numMarks < maxMarks)
    {
        marks[(size_t) ((firstMark + numMarks) % maxMarks)] = m;
        ++numMarks;
    }
    else
    {
        marks[(size_t) firstMark] = m;
        firstMark = (firstMark + 1) % maxMarks;
    }
}

const PsolaAnalysis::Mark& PsolaAnalysis::nearestMark (double time) const noexcept
{
    // Marks are in time order: walk back from the newest while the distance shrinks.
    auto best = numMarks - 1;
    auto bestDistance = std::abs (getMark (best).position - time);
    for (int i = numMarks - 2; i >= 0; --i)
    {
        const auto d = std::abs (getMark (i).position - time);
        if (d >= bestDistance)
            break;
        best = i;
        bestDistance = d;
    }
    return getMark (best);
}

void PsolaAnalysis::process (float di, const PitchShifterInput& input) noexcept
{
    detector.pushSample (di);

    if (++hopCounter >= hop)
    {
        hopCounter = 0;
        if (overridePeriod > 0.0)
            onEstimate ({ sampleRate / overridePeriod, 1.0 }, input);
        else
            onEstimate (detector.detect(), input);
    }

    if (tracking)
        trackNextMark (input);

    const auto target = isConfident() ? 1.0 : 0.0;
    confidence = target > confidence ? std::min (target, confidence + confidenceStep) : std::max (target, confidence - confidenceStep);
}

void PsolaAnalysis::onEstimate (const PitchDetector::Estimate& e, const PitchShifterInput& input) noexcept
{
    lastEstimate = e;
    const auto confident = e.frequency > 0.0 && e.clarity >= clarityThreshold;
    if (! confident)
    {
        previousConfident = 0.0;
        if (++unconfident >= 2)
            tracking = false; // two unsure readings in a row: the note is over (or it's a chord)
        return;
    }

    unconfident = 0;
    const auto agrees = previousConfident > 0.0 && std::abs (centsBetween (e.frequency, previousConfident)) <= agreeCents;
    previousConfident = e.frequency;
    if (! agrees)
        return; // one reading alone never acts

    // A confirmed pitch. A different note (or nothing tracked, or a track that broke) starts a new track.
    const auto newPeriod = sampleRate / e.frequency;
    if (! tracking || lost || std::abs (centsBetween (period, newPeriod)) > newNoteCents)
        anchor (newPeriod, input);
}

void PsolaAnalysis::anchor (double newPeriod, const PitchShifterInput& input) noexcept
{
    // The first mark: the largest sample (by magnitude) in the latest period. Element i of newest(p) is the
    // sample at absolute index time - p + i. Ties go to the earliest.
    const auto p = juce::jlimit (8, input.getHistoryCapacity() / 2, (int) std::lround (newPeriod));
    const auto* x = input.newest (p);
    int best = 0;
    auto bestMagnitude = -1.0f;
    for (int i = 0; i < p; ++i)
    {
        if (std::abs (x[i]) > bestMagnitude)
        {
            bestMagnitude = std::abs (x[i]);
            best = i;
        }
    }

    firstMark = numMarks = 0;
    addMark ({ (double) (input.getTime() - p + best), newPeriod, 1.0 });
    period = newPeriod;
    tracking = true;
    lost = false;
    ++noteCount;
}

void PsolaAnalysis::trackNextMark (const PitchShifterInput& input) noexcept
{
    // The next mark ends a period that best matches the period ending at the last mark: candidates tau
    // samples after the last mark's sample, within +-1/8 period of the predicted spacing. All the samples
    // it needs (up to the last candidate) must be in.
    const auto& last = newestMark();
    const auto range = std::max (2, (int) std::ceil (searchFraction * period));
    const auto anchorIndex = (int64_t) std::llround (last.position);
    const auto tauLo = std::max (8, (int) std::floor (period) - range);
    const auto tauHi = (int) std::ceil (period) + range;
    if (input.getTime() - 1 < anchorIndex + tauHi)
        return;

    const auto w = std::max (16, (int) std::lround (period)); // one period
    const auto length = (int) (input.getTime() - (anchorIndex - w + 1));
    double tau = period, quality = 0.0;

    if (length <= input.getHistoryCapacity())
    {
        // Element i of newest(length) is the sample at index anchorIndex - w + 1 + i, so the window ending at
        // the last mark starts at element 0 and the one ending tau later at element tau.
        const auto* pa = input.newest (length);
        const auto ea = pitchShifterDot (pa, pa, w);
        if (ea > 1.0e-30)
        {
            constexpr int maxCandidates = 512;
            std::array<double, maxCandidates> rho {};
            const auto count = std::min (maxCandidates, tauHi - tauLo + 1);
            int best = 0;
            for (int k = 0; k < count; ++k)
            {
                const auto* pb = pa + tauLo + k;
                const auto eb = pitchShifterDot (pb, pb, w);
                rho[(size_t) k] = ea * eb > 1.0e-30 ? pitchShifterDot (pa, pb, w) / std::sqrt (ea * eb) : 0.0;
                if (rho[(size_t) k] > rho[(size_t) best])
                    best = k;
            }

            // Parabola through the winner and its neighbours (as in PitchDetector).
            tau = (double) (tauLo + best);
            quality = rho[(size_t) best];
            if (best > 0 && best < count - 1)
            {
                const auto y0 = rho[(size_t) best - 1], y1 = rho[(size_t) best], y2 = rho[(size_t) best + 1];
                const auto curvature = y0 - 2.0 * y1 + y2;
                if (curvature < 0.0)
                {
                    const auto delta = juce::jlimit (-1.0, 1.0, 0.5 * (y0 - y2) / curvature);
                    tau += delta;
                    quality = y1 - 0.25 * (y0 - y2) * delta;
                }
            }
        }
    }

    if (quality >= trackQuality)
    {
        lost = false;
        period = tau;
    }
    else
    {
        lost = true;
        tau = period; // keep marks coming at the last good period, for voices still fading out
    }
    addMark ({ last.position + tau, tau, quality });
}

// ---- PsolaVoice ------------------------------------------------------------------------------------

double PsolaVoice::levelCompensation (double r) noexcept
{
    if (r >= 1.0)
        return 1.0; // half-length P/r at spacing P/r: Hann windows overlapping by half sum to exactly 1

    // Hann grains of half-length h = P at spacing s = P/r (q = s/h = 1/r): the mean square of their sum over
    // one spacing is (integral of w^2 + 2 x integral of w times its neighbour) / s, with w^2 integrating to
    // 3h/4 and the overlap term C (nonzero while q < 2) worked out in Psola.h.
    const auto pi = juce::MathConstants<double>::pi;
    const auto q = 1.0 / r;
    double m;
    if (q < 2.0)
    {
        const auto c = 0.25 * ((2.0 - q) * (1.0 + 0.5 * std::cos (pi * q)) + 1.5 / pi * std::sin (pi * q));
        m = r * (0.75 + 2.0 * c);
    }
    else
    {
        m = 0.75 * r;
    }
    return 1.0 / std::sqrt (m);
}

double PsolaVoice::windowedMean (const PitchShifterInput& input, double centre, double half) noexcept
{
    // Whole samples k in [-h, h] around round(centre), weights 0.5 (1 + cos(pi k / half)); the cosines by
    // the Chebyshev recurrence cos((k + 1) t) = 2 cos(t) cos(k t) - cos((k - 1) t).
    const auto h = (int) std::floor (half);
    const auto middle = (int64_t) std::llround (centre);
    const auto length = (int) (input.getTime() - (middle - h));
    if (h < 1 || middle + h > input.getTime() - 1 || length > input.getHistoryCapacity())
        return 0.0;

    const auto* x = input.newest (length); // element 0 is the sample at middle - h
    const auto theta = juce::MathConstants<double>::pi / half;
    const auto twoCos = 2.0 * std::cos (theta);
    auto previous = std::cos (theta * (double) (-h - 1)), current = std::cos (theta * (double) -h);
    double sw = 0.0, swx = 0.0;
    for (int k = 0; k <= 2 * h; ++k)
    {
        const auto w = 0.5 * (1.0 + current);
        sw += w;
        swx += w * (double) x[k];
        const auto next = twoCos * current - previous;
        previous = current;
        current = next;
    }
    return sw > 0.0 ? swx / sw : 0.0;
}

void PsolaVoice::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    setSettings (settings);
    reset();
}

void PsolaVoice::reset() noexcept
{
    ratio.jumpTo (juce::jlimit (minRatio, maxRatio, settings.ratio));
    fade.reset (sampleRate, std::max (0.0, settings.fadeMs) / 1000.0);
    fade.setCurrentAndTargetValue (settings.active ? 1.0 : 0.0);
    for (auto& g : grains)
        g.active = false;
    nextCentre = -1.0e18;
    noteCount = -1;
    lastDelay = 0.0;
    launched = 0;
}

void PsolaVoice::setSettings (const Settings& newSettings) noexcept
{
    settings = newSettings;
    ratio.setGlideSamples (juce::roundToInt (std::max (0.0, settings.glideMs) * sampleRate / 1000.0));
    ratio.setTarget (juce::jlimit (minRatio, maxRatio, settings.ratio));
    fade.setTargetValue (settings.active ? 1.0 : 0.0);
}

float PsolaVoice::process (const PsolaAnalysis& analysis, const PitchShifterInput& input, double ratioScale, double extraDelaySamples) noexcept
{
    const auto r = juce::jlimit (minRatio, maxRatio, ratio.next() * ratioScale);
    const auto gain = fade.getNextValue();
    const auto now = (double) input.getTime();

    if (analysis.getNumMarks() > 0)
    {
        const auto& newest = analysis.newestMark();
        const auto halfEstimate = std::min (newest.period, newest.period / r);

        // A new note: start its grains now instead of waiting out the old note's synthesis period. A voice
        // that has been idle restarts its schedule now.
        if (analysis.getNoteCount() != noteCount)
        {
            noteCount = analysis.getNoteCount();
            nextCentre = std::min (nextCentre, now + halfEstimate);
        }
        if (nextCentre < now)
            nextCentre = now + halfEstimate;

        if (now >= nextCentre - halfEstimate)
        {
            // The grain for this synthesis mark: the analysis mark nearest to it minus the lag, two of its
            // periods long (two synthesis periods for r > 1), read at a fixed delay for its whole length.
            const auto lag = std::max (0.0, settings.delayMs) * sampleRate / 1000.0 + extraDelaySamples;
            const auto& mark = analysis.nearestMark (nextCentre - lag);
            const auto p = mark.period;

            auto* slot = &grains[0];
            for (auto& g : grains)
            {
                if (! g.active)
                {
                    slot = &g;
                    break;
                }
                if (g.centre < slot->centre)
                    slot = &g; // all busy (can't happen with spacing >= half-length): reuse the oldest
            }
            slot->centre = nextCentre;
            slot->half = std::min (p, p / r);
            slot->delay = nextCentre - mark.position;
            slot->gain = levelCompensation (r);
            slot->offset = slot->half < p ? windowedMean (input, mark.position - p, slot->half) : 0.0;
            slot->active = true;
            lastDelay = slot->delay;
            ++launched;

            nextCentre += p / r;
        }
    }

    // Overlap-add: each grain is the input at its fixed delay under a Hann window over its length.
    double y = 0.0;
    for (auto& g : grains)
    {
        if (! g.active)
            continue;
        const auto u = (now - g.centre) / g.half;
        if (u >= 1.0)
        {
            g.active = false;
            continue;
        }
        if (u <= -1.0)
            continue;
        const auto w = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * u));
        y += g.gain * w * ((double) input.read (g.delay) - g.offset);
    }
    return (float) (gain * y);
}

} // namespace ampsim

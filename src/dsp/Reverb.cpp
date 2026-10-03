// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "Reverb.h"

#include <algorithm>
#include <cmath>

namespace ampsim
{

namespace
{
constexpr double butterworthQ = 0.7071067811865476; // the shelves' Q: monotonic, no overshoot
constexpr double pi = juce::MathConstants<double>::pi;
constexpr int coefficientInterval = 32; // knob-driven filters are redesigned every 32 samples
constexpr double plateSmoothingSeconds = 0.05;

/// Dattorro's two-multiplier lattice allpass (Fig. 1), with its delay line holding w up to the
/// previous sample:
///     w[n] = x[n] - g w[n - N]          the top summing node, written into the line
///     y[n] = w[n - N] + g w[n]          the bottom summing node
/// Y/X = (g + z^-N) / (1 + g z^-N), an allpass for |g| < 1: its impulse response is g, then
/// (1 - g^2), -g (1 - g^2), g^2 (1 - g^2), ... at N, 2N, 3N, ... It conserves x^2 - y^2 =
/// (1 - g^2)(w^2 - w[n - N]^2), so a loop of these and plain delays with unity gains is lossless.
/// Arithmetic in double; the line stores float.
inline double lattice (DelayLine& line, int length, double x, double g) noexcept
{
    const double delayed = line.readInteger (length - 1); // w[n - N]
    const double w = x - g * delayed;
    line.write ((float) w);
    return delayed + g * w;
}

/// The same lattice with both signs reversed, as Fig. 1 marks the "decay diffusion 1" lattices
/// ("note sign"): w = x + g d, y = d - g w, i.e. the allpass (-g + z^-N) / (1 - g z^-N). Its delay is
/// fractional and moving (the modulated tap), read with Hermite interpolation.
inline double latticeReversed (DelayLine& line, double delay, double x, double g) noexcept
{
    const double delayed = line.read (delay - 1.0); // w[n - D(n)]
    const double w = x + g * delayed;
    line.write ((float) w);
    return delayed - g * w;
}

/// The sum of squares of the last `count` samples written to a line.
double windowEnergy (const DelayLine& line, int count)
{
    double sum = 0.0;
    for (int k = 0; k < count; ++k)
    {
        const double v = line.readInteger (k);
        sum += v * v;
    }
    return sum;
}

double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
} // namespace

// =================================================================================================
// DattorroPlate
// =================================================================================================

int DattorroPlate::scaledLength (int paperSamples, double rate)
{
    return (int) std::floor (paperSamples * rate / paperSampleRate + 0.5);
}

void DattorroPlate::prepare (double rate)
{
    sampleRate = rate;

    // Fig. 1's lengths at 29761 Hz, scaled to this rate.
    const int inputPaper[4] = { 142, 107, 379, 277 };
    for (size_t i = 0; i < 4; ++i)
        inputLength[i] = scaledLength (inputPaper[i], rate);
    leftAllpass1 = scaledLength (672, rate);
    leftDelay1 = scaledLength (4453, rate);
    leftAllpass2 = scaledLength (1800, rate);
    leftDelay2 = scaledLength (3720, rate);
    rightAllpass1 = scaledLength (908, rate);
    rightDelay1 = scaledLength (4217, rate);
    rightAllpass2 = scaledLength (2656, rate);
    rightDelay2 = scaledLength (3163, rate);
    loopSamples = leftAllpass1 + leftDelay1 + leftAllpass2 + leftDelay2 + rightAllpass1 + rightDelay1 + rightAllpass2 + rightDelay2;

    // Table 2's taps, in the order the table lists them.
    const int left[7] = { 266, 2974, 1913, 1996, 1990, 187, 1066 };
    const int right[7] = { 353, 3627, 1228, 2673, 2111, 335, 121 };
    for (size_t i = 0; i < 7; ++i)
    {
        tapsLeft[i] = scaledLength (left[i], rate);
        tapsRight[i] = scaledLength (right[i], rate);
    }

    // The modulated lines need room for the full excursion plus Hermite's neighbours.
    const auto maxExcursion = (int) std::ceil (16.0 * rate / paperSampleRate) + 4;
    for (size_t i = 0; i < 4; ++i)
        inputLines[i].prepare (inputLength[i] + 4);
    leftAllpass1Line.prepare (leftAllpass1 + maxExcursion);
    rightAllpass1Line.prepare (rightAllpass1 + maxExcursion);
    leftDelay1Line.prepare (leftDelay1 + 4);
    leftAllpass2Line.prepare (leftAllpass2 + 4);
    leftDelay2Line.prepare (leftDelay2 + 4);
    rightDelay1Line.prepare (rightDelay1 + 4);
    rightAllpass2Line.prepare (rightAllpass2 + 4);
    rightDelay2Line.prepare (rightDelay2 + 4);

    lfoLeft.prepare (rate);
    lfoRight.prepare (rate);
    lfoLeft.setShape (Lfo::Shape::sine);
    lfoRight.setShape (Lfo::Shape::sine);

    for (auto* s : { &smoothed.decay, &smoothed.decayDiffusion1, &smoothed.inputDiffusion1, &smoothed.inputDiffusion2,
                     &smoothed.bandwidth, &smoothed.damping, &smoothed.excursion, &smoothed.lowShelfDb, &smoothed.highShelfDb })
        s->reset (rate, plateSmoothingSeconds);

    snapToTargets();
    reset();
}

void DattorroPlate::reset()
{
    for (auto& line : inputLines)
        line.reset();
    for (auto* line : { &leftAllpass1Line, &leftDelay1Line, &leftAllpass2Line, &leftDelay2Line, &rightAllpass1Line,
                        &rightDelay1Line, &rightAllpass2Line, &rightDelay2Line })
        line->reset();

    bandwidthState = dampLeft = dampRight = 0.0;
    for (auto* shelf : { &lowShelfLeft, &lowShelfRight, &highShelfLeft, &highShelfRight })
        shelf->reset();

    // The paper's quadrature pair: the right LFO a quarter cycle ahead.
    lfoLeft.setPhase (0.0);
    lfoRight.setPhase (0.25);
}

void DattorroPlate::setParameters (const Parameters& p)
{
    target = p;
    smoothed.decay.setTargetValue (juce::jlimit (0.0, 1.0, p.decay));
    smoothed.decayDiffusion1.setTargetValue (juce::jlimit (0.0, 0.95, p.decayDiffusion1));
    smoothed.inputDiffusion1.setTargetValue (juce::jlimit (0.0, 0.95, p.inputDiffusion1));
    smoothed.inputDiffusion2.setTargetValue (juce::jlimit (0.0, 0.95, p.inputDiffusion2));
    smoothed.bandwidth.setTargetValue (juce::jlimit (0.0, 1.0, p.bandwidth));
    smoothed.damping.setTargetValue (juce::jlimit (0.0, 0.999, p.damping));
    smoothed.excursion.setTargetValue (juce::jlimit (0.0, 16.0, p.excursion));
    smoothed.lowShelfDb.setTargetValue (p.lowShelfDb);
    smoothed.highShelfDb.setTargetValue (p.highShelfDb);
    lfoLeft.setRate (p.modRateHz);
    lfoRight.setRate (p.modRateHz);
}

void DattorroPlate::snapToTargets()
{
    for (auto* s : { &smoothed.decay, &smoothed.decayDiffusion1, &smoothed.inputDiffusion1, &smoothed.inputDiffusion2,
                     &smoothed.bandwidth, &smoothed.damping, &smoothed.excursion, &smoothed.lowShelfDb, &smoothed.highShelfDb })
        s->setCurrentAndTargetValue (s->getTargetValue());
    updateShelves (false);
    samplesUntilShelfUpdate = coefficientInterval;
}

void DattorroPlate::updateShelves (bool glide)
{
    const auto low = Svf::design (Svf::Type::lowShelf, Reverb::lowCrossoverHz, butterworthQ, smoothed.lowShelfDb.getCurrentValue(), sampleRate);
    const auto high = Svf::design (Svf::Type::highShelf, Reverb::highCrossoverHz, butterworthQ, smoothed.highShelfDb.getCurrentValue(), sampleRate);
    for (auto* shelf : { &lowShelfLeft, &lowShelfRight })
    {
        if (glide)
            shelf->glideTo (low, coefficientInterval);
        else
            shelf->snapTo (low);
    }
    for (auto* shelf : { &highShelfLeft, &highShelfRight })
    {
        if (glide)
            shelf->glideTo (high, coefficientInterval);
        else
            shelf->snapTo (high);
    }
}

void DattorroPlate::process (const float* input, float* left, float* right, int numSamples) noexcept
{
    const auto excursionScale = sampleRate / paperSampleRate; // Table 1 counts samples at 29761 Hz

    for (int n = 0; n < numSamples; ++n)
    {
        // The shelves follow their smoothed gains, redesigned every 32 samples and glided in between.
        if (--samplesUntilShelfUpdate <= 0)
        {
            samplesUntilShelfUpdate = coefficientInterval;
            if (smoothed.lowShelfDb.isSmoothing() || smoothed.highShelfDb.isSmoothing())
            {
                smoothed.lowShelfDb.skip (coefficientInterval);
                smoothed.highShelfDb.skip (coefficientInterval);
                updateShelves (true);
            }
        }

        const auto decay = smoothed.decay.getNextValue();
        const auto dd1 = smoothed.decayDiffusion1.getNextValue();
        const auto dd2 = decayDiffusion2 (decay);
        const auto id1 = smoothed.inputDiffusion1.getNextValue();
        const auto id2 = smoothed.inputDiffusion2.getNextValue();
        const auto bandwidth = smoothed.bandwidth.getNextValue();
        const auto damping = smoothed.damping.getNextValue();
        const auto excursion = smoothed.excursion.getNextValue() * excursionScale;
        const double modLeft = lfoLeft.next(), modRight = lfoRight.next();

        // Input bandwidth: y = bandwidth x + (1 - bandwidth) y[n-1].
        bandwidthState = bandwidth * (double) input[n] + (1.0 - bandwidth) * bandwidthState;

        // The four input diffusers, 142 and 107 at input diffusion 1, 379 and 277 at input diffusion 2.
        auto diffused = lattice (inputLines[0], inputLength[0], bandwidthState, id1);
        diffused = lattice (inputLines[1], inputLength[1], diffused, id1);
        diffused = lattice (inputLines[2], inputLength[2], diffused, id2);
        diffused = lattice (inputLines[3], inputLength[3], diffused, id2);

        // The tank. Each half's input this sample is the other half's last delay output, which comes
        // from earlier samples, so both are read before anything is written.
        const double fromRight = rightDelay2Line.readInteger (rightDelay2 - 1); // z^-3163 out (node 63)
        const double fromLeft = leftDelay2Line.readInteger (leftDelay2 - 1);    // z^-3720 out (node 39)

        // Left half: 672 (modulated, signs reversed) -> z^-4453 -> damping (-> our shelves) -> decay
        // -> 1800 -> z^-3720.
        {
            auto x = diffused + decay * fromRight;
            x = latticeReversed (leftAllpass1Line, leftAllpass1 + excursion * modLeft, x, dd1);
            const double delayed = leftDelay1Line.readInteger (leftDelay1 - 1); // node 30
            leftDelay1Line.write ((float) x);
            dampLeft = (1.0 - damping) * delayed + damping * dampLeft;
            x = decay * highShelfLeft.processSample (lowShelfLeft.processSample (dampLeft));
            x = lattice (leftAllpass2Line, leftAllpass2, x, dd2);
            leftDelay2Line.write ((float) x);
        }

        // Right half: 908 (modulated, signs reversed) -> z^-4217 -> damping (-> our shelves) -> decay
        // -> 2656 -> z^-3163.
        {
            auto x = diffused + decay * fromLeft;
            x = latticeReversed (rightAllpass1Line, rightAllpass1 + excursion * modRight, x, dd1);
            const double delayed = rightDelay1Line.readInteger (rightDelay1 - 1); // node 54
            rightDelay1Line.write ((float) x);
            dampRight = (1.0 - damping) * delayed + damping * dampRight;
            x = decay * highShelfRight.processSample (lowShelfRight.processSample (dampRight));
            x = lattice (rightAllpass2Line, rightAllpass2, x, dd2);
            rightDelay2Line.write ((float) x);
        }

        // Table 2. nodeA_B[k] is the line from node A to node B, k samples after its input; every line
        // has just been written, so readInteger(k) is exactly that.
        //   node24_30: z^-4453    node31_33: the 1800 lattice's line    node33_39: z^-3720
        //   node48_54: z^-4217    node55_59: the 2656 lattice's line    node59_63: z^-3163
        double acc = 0.6 * rightDelay1Line.readInteger (tapsLeft[0]);
        acc += 0.6 * rightDelay1Line.readInteger (tapsLeft[1]);
        acc -= 0.6 * rightAllpass2Line.readInteger (tapsLeft[2]);
        acc += 0.6 * rightDelay2Line.readInteger (tapsLeft[3]);
        acc -= 0.6 * leftDelay1Line.readInteger (tapsLeft[4]);
        acc -= 0.6 * leftAllpass2Line.readInteger (tapsLeft[5]);
        left[n] = (float) (acc - 0.6 * leftDelay2Line.readInteger (tapsLeft[6]));

        acc = 0.6 * leftDelay1Line.readInteger (tapsRight[0]);
        acc += 0.6 * leftDelay1Line.readInteger (tapsRight[1]);
        acc -= 0.6 * leftAllpass2Line.readInteger (tapsRight[2]);
        acc += 0.6 * leftDelay2Line.readInteger (tapsRight[3]);
        acc -= 0.6 * rightDelay1Line.readInteger (tapsRight[4]);
        acc -= 0.6 * rightAllpass2Line.readInteger (tapsRight[5]);
        right[n] = (float) (acc - 0.6 * rightDelay2Line.readInteger (tapsRight[6]));
    }
}

double DattorroPlate::storedEnergy() const
{
    const auto dd1 = smoothed.decayDiffusion1.getCurrentValue();
    const auto dd2 = decayDiffusion2 (smoothed.decay.getCurrentValue());
    return windowEnergy (leftDelay1Line, leftDelay1) + windowEnergy (leftDelay2Line, leftDelay2)
           + windowEnergy (rightDelay1Line, rightDelay1) + windowEnergy (rightDelay2Line, rightDelay2)
           + (1.0 - dd1 * dd1) * (windowEnergy (leftAllpass1Line, leftAllpass1) + windowEnergy (rightAllpass1Line, rightAllpass1))
           + (1.0 - dd2 * dd2) * (windowEnergy (leftAllpass2Line, leftAllpass2) + windowEnergy (rightAllpass2Line, rightAllpass2));
}

// =================================================================================================
// FeedbackDelayNetwork
// =================================================================================================

FeedbackDelayNetwork::FeedbackDelayNetwork (int lineCount) : numLines (juce::jlimit (2, maxLines, lineCount))
{
    // Injection and output patterns. The left input feeds the even lines and the right the odd ones,
    // each at sqrt(2/N) so the injected power doesn't depend on N. The outputs take two rows of a
    // Hadamard matrix, (+ - + - ...) and (+ + - - ...), both orthogonal to each other and to the
    // all-ones vector, so left and right are different, roughly uncorrelated mixes of the same lines.
    const auto inject = std::sqrt (2.0 / numLines);
    const auto tap = 1.0 / std::sqrt ((double) numLines);
    for (int i = 0; i < numLines; ++i)
    {
        const auto sign = ((i / 2) % 2 == 0) ? 1.0 : -1.0;
        injectLeft[(size_t) i] = (i % 2 == 0) ? sign * inject : 0.0;
        injectRight[(size_t) i] = (i % 2 == 1) ? sign * inject : 0.0;
        tapLeft[(size_t) i] = (i % 2 == 0 ? 1.0 : -1.0) * tap;
        tapRight[(size_t) i] = ((i / 2) % 2 == 0 ? 1.0 : -1.0) * tap;
    }

    // Four modulated lines spread across the set, each with its own random LFO (different seeds,
    // rates a little apart, phases a quarter cycle apart) so they never move together.
    for (int k = 0; k < numModulatedLines; ++k)
    {
        modulatedLine[(size_t) k] = k * numLines / numModulatedLines + 1;
        lfos[(size_t) k] = Lfo (101 + 7 * k + numLines);
        lfos[(size_t) k].setShape (Lfo::Shape::random);
    }

    // Early reflections, at Size scale 1: ten taps per side between 4 and 85 ms, the sides interleaved,
    // with gains falling as 1 / (1 + t / 15 ms) and mixed signs, normalized to unit energy per side.
    const double times[2][numEarlyTaps] = { { 4.3, 9.7, 14.9, 21.1, 27.6, 35.2, 43.9, 54.8, 66.1, 79.7 },
                                            { 5.9, 11.3, 17.6, 23.8, 31.2, 38.7, 48.3, 59.4, 71.6, 84.5 } };
    const double signs[2][numEarlyTaps] = { { 1, -1, 1, 1, -1, 1, -1, 1, 1, -1 }, { 1, 1, -1, 1, 1, -1, 1, -1, 1, 1 } };
    for (size_t side = 0; side < 2; ++side)
    {
        double energy = 0.0;
        for (size_t k = 0; k < (size_t) numEarlyTaps; ++k)
        {
            earlyTapMs[side][k] = times[side][k];
            earlyTapGain[side][k] = signs[side][k] / (1.0 + times[side][k] / 15.0);
            energy += earlyTapGain[side][k] * earlyTapGain[side][k];
        }
        for (auto& g : earlyTapGain[side])
            g /= std::sqrt (energy);
    }
}

void FeedbackDelayNetwork::feedbackMatrix (double* x, int n) noexcept
{
    // Householder: y = (I - (2/n) 1 1^T) x = x - (2/n) sum(x).
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        sum += x[i];
    const auto offset = 2.0 * sum / n;

    // Then the rotation: line i's mixed output goes to line i + 1 (the last wraps to line 0).
    const auto last = x[n - 1] - offset;
    for (int i = n - 1; i > 0; --i)
        x[i] = x[i - 1] - offset;
    x[0] = last;
}

void FeedbackDelayNetwork::lengthsFor (double scale, std::array<int, maxLines>& out) const
{
    // The nearest prime to each base length x scale, each one above the previous line's, so every
    // line ends up with a different prime (pairwise coprime).
    int previous = 1;
    for (int i = 0; i < numLines; ++i)
    {
        const auto wanted = baseLengths[(size_t) i] * scale;
        const auto above = std::upper_bound (primes.begin(), primes.end(), previous);
        auto next = std::lower_bound (above, primes.end(), (int) std::ceil (wanted));
        if (next == primes.end())
            --next;

        int best = *next;
        if (next != above && wanted - *(next - 1) <= *next - wanted)
            best = *(next - 1);

        out[(size_t) i] = best;
        previous = best;
    }
}

void FeedbackDelayNetwork::prepare (double rate)
{
    sampleRate = rate;
    const auto toSamples = rate / 1000.0;

    // Shimmer: the shifter, and the balanced +-1 direction (it sums to zero over the lines in use).
    shimmerShifter.prepare (rate);
    shimmerShifter.setSettings ({ shimmerRatio, 0.0, 30.0 });
    shimmerLowPass.setCoefficients (Svf::design (Svf::Type::lowpass, shimmerLowPassHz, 0.7071067811865476, 0.0, rate));
    shimmerLowPass.reset();
    shimmerAmount.reset (rate, 0.05);
    shimmerAverage = 1.0 - std::exp (-1.0 / (0.05 * rate));
    shimmerPowerIn = shimmerPowerOut = 0.0;
    shimmerAmount.setCurrentAndTargetValue (juce::jlimit (0.0, maxShimmer, target.shimmer));
    for (int i = 0; i < numLines; ++i)
        shimmerDirection[(size_t) i] = ((i % 4 == 0 || i % 4 == 3) ? 1.0 : -1.0) / std::sqrt ((double) numLines);

    // Base lengths: geometric from 30 to 100 ms, so neighbouring lines are a constant ratio apart.
    for (int i = 0; i < numLines; ++i)
        baseLengths[(size_t) i] = shortestLineMs * std::pow (longestLineMs / shortestLineMs, (double) i / (numLines - 1)) * toSamples;

    // Primes up to the longest line at the largest Size, by the sieve of Eratosthenes.
    const auto limit = (int) (longestLineMs * maxSizeScale * toSamples) + 200;
    std::vector<bool> composite ((size_t) limit + 1, false);
    primes.clear();
    for (int p = 2; p <= limit; ++p)
    {
        if (composite[(size_t) p])
            continue;
        primes.push_back (p);
        for (long m = (long) p * p; m <= limit; m += p)
            composite[(size_t) m] = true;
    }

    const auto maxExcursion = (int) std::ceil (maxExcursionMs * toSamples);
    for (int i = 0; i < numLines; ++i)
    {
        lines[(size_t) i].prepare (limit + maxExcursion + 4);
        lengths[(size_t) i].prepare (lengthGlideSeconds, rate, maxGlideSpeed, 1.0e-4);
    }

    for (auto& lfo : lfos)
        lfo.prepare (rate);

    for (size_t side = 0; side < 2; ++side)
    {
        const auto longestTap = *std::max_element (earlyTapMs[side].begin(), earlyTapMs[side].end());
        earlyLines[side].prepare ((int) std::ceil (longestTap * maxSizeScale * toSamples) + 4);

        // Diffuser lengths (samples at 48 kHz, primes, a little different per side).
        const int base[2][numDiffusers] = { { 113, 163, 241, 353 }, { 127, 179, 257, 373 } };
        for (size_t k = 0; k < (size_t) numDiffusers; ++k)
        {
            diffuserLength[side][k] = juce::jmax (2, (int) std::floor (base[side][k] * rate / 48000.0 + 0.5));
            diffusers[side][k].prepare (diffuserLength[side][k] + 4);
        }
    }

    for (auto* s : { &rateMid, &rateLow, &rateHigh, &excursion })
        s->reset (rate, rateSmoothingSeconds);
    // The early pattern's scale glides like the lines, with its speed limit in taps' terms: the longest
    // tap (84.5 ms at scale 1) moves at most maxGlideSpeed samples per sample.
    const auto longestTapSamples = 84.5 * toSamples;
    earlyScale.prepare (lengthGlideSeconds, rate, maxGlideSpeed / longestTapSamples, 1.0e-4 / longestTapSamples);
    for (auto* s : { &diffusion, &earlyGain, &lateGain })
        s->reset (rate, 0.02);

    prepared = true;
    lengthsDesignedFor = -1.0;
    setParameters (target);
    snapToTargets();
    reset();
}

void FeedbackDelayNetwork::reset()
{
    shimmerShifter.reset();
    shimmerLowPass.reset();
    shimmerPowerIn = shimmerPowerOut = 0.0;
    for (auto& line : lines)
        line.reset();
    for (auto& line : earlyLines)
        line.reset();
    for (auto& side : diffusers)
        for (auto& line : side)
            line.reset();
    for (int i = 0; i < numLines; ++i)
    {
        lowShelves[(size_t) i].reset();
        highShelves[(size_t) i].reset();
    }
    for (size_t k = 0; k < lfos.size(); ++k)
        lfos[k].setPhase (0.25 * (double) k);
}

void FeedbackDelayNetwork::setParameters (const Parameters& p)
{
    target = p;
    if (! prepared)
        return;

    shimmerAmount.setTargetValue (juce::jlimit (0.0, maxShimmer, p.shimmer));
    if (const auto ratio = juce::jlimit (0.25, 4.0, p.shimmerRatio); std::abs (ratio - shimmerRatio) > 1.0e-12)
    {
        shimmerRatio = ratio;
        shimmerShifter.setSettings ({ ratio, 0.0, 30.0 }); // glides over 30 ms
    }

    const auto scale = juce::jlimit (0.05, maxSizeScale, p.sizeScale);
    if (std::abs (scale - lengthsDesignedFor) > 1.0e-12)
    {
        lengthsDesignedFor = scale;
        lengthsFor (scale, targetLengths);
        double total = 0.0;
        for (int i = 0; i < numLines; ++i)
        {
            lengths[(size_t) i].setTarget ((double) targetLengths[(size_t) i]);
            total += targetLengths[(size_t) i];
        }
        earlyScale.setTarget (scale);

        // Late level. A network holding total length L_sum (samples) and losing 60 dB in T60 stores about
        // P_in T60 fs / (13.8 L_sum) of energy per sample of line, so each line's output, and the late
        // output, has a power proportional to T60 / L_sum: a bigger room at the same decay would be
        // quieter. Scaling the output by sqrt(L_sum / fs) cancels that, so Size changes the space and
        // only Decay changes the loudness (+3 dB per doubling, as in a real room).
        lateNormalization = std::sqrt (total / sampleRate);
    }

    rateMid.setTargetValue (juce::jmax (0.0, p.rateMid));
    rateLow.setTargetValue (juce::jmax (0.0, p.rateLow));
    rateHigh.setTargetValue (juce::jmax (0.0, p.rateHigh));
    excursion.setTargetValue (juce::jlimit (0.0, 1.0, p.modDepth) * maxExcursionMs * sampleRate / 1000.0);
    diffusion.setTargetValue (juce::jlimit (0.0, 1.0, p.diffusion));
    earlyGain.setTargetValue (p.earlyGain);
    lateGain.setTargetValue (p.lateGain * lateNormalization);

    // Slightly different rates per line so the modulations never lock together.
    const double rateSpread[numModulatedLines] = { 0.83, 1.0, 1.19, 1.37 };
    for (size_t k = 0; k < lfos.size(); ++k)
        lfos[k].setRate (juce::jmax (0.0, p.modRateHz) * rateSpread[k]);
}

void FeedbackDelayNetwork::snapToTargets()
{
    for (int i = 0; i < numLines; ++i)
        lengths[(size_t) i].snap();
    earlyScale.snap();
    for (auto* s : { &rateMid, &rateLow, &rateHigh, &excursion, &diffusion, &earlyGain, &lateGain })
        s->setCurrentAndTargetValue (s->getTargetValue());
    updateDecayFilters (false);
    samplesUntilFilterUpdate = coefficientInterval;
}

void FeedbackDelayNetwork::updateDecayFilters (bool glide)
{
    // Jot's proportional decay: line i loses -L_i x rate / fs dB per pass in each band, so every path
    // loses the same dB per second. Mid as a plain gain, low and high as shelves relative to it. While
    // something moves, the new values are reached by gliding over the next 32 samples, never by a jump.
    const auto mid = rateMid.getCurrentValue(), low = rateLow.getCurrentValue(), high = rateHigh.getCurrentValue();
    const auto interval = (double) coefficientInterval;

    for (int i = 0; i < numLines; ++i)
    {
        const auto seconds = lengths[(size_t) i].getValue() / sampleRate;
        const auto lowShelf = Svf::design (Svf::Type::lowShelf, Reverb::lowCrossoverHz, butterworthQ, -(low - mid) * seconds, sampleRate);
        const auto highShelf = Svf::design (Svf::Type::highShelf, Reverb::highCrossoverHz, butterworthQ, -(high - mid) * seconds, sampleRate);
        gainTargets[(size_t) i] = dbToGain (-mid * seconds);

        if (glide)
        {
            gainSteps[(size_t) i] = (gainTargets[(size_t) i] - gains[(size_t) i]) / interval;
            lowShelves[(size_t) i].glideTo (lowShelf, coefficientInterval);
            highShelves[(size_t) i].glideTo (highShelf, coefficientInterval);
        }
        else
        {
            gains[(size_t) i] = gainTargets[(size_t) i];
            gainSteps[(size_t) i] = 0.0;
            lowShelves[(size_t) i].snapTo (lowShelf);
            highShelves[(size_t) i].snapTo (highShelf);
        }
    }
    glideRemaining = glide ? coefficientInterval : 0;
}

namespace
{
/// Identity below 0.7, then a tanh knee that never exceeds 1, with matching slope at the joint (the same
/// curve as the delay's loop limiter, Delay::softLimit).
float shimmerLimit (float x) noexcept
{
    constexpr float knee = 0.7f, headroom = 1.0f - knee;
    const auto a = std::abs (x);
    if (a <= knee)
        return x;
    return std::copysign (knee + headroom * std::tanh ((a - knee) / headroom), x);
}
} // namespace

void FeedbackDelayNetwork::process (const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples) noexcept
{
    const auto n = numLines;
    const auto toSamples = sampleRate / 1000.0;

    bool lengthsMoving = false;
    for (int i = 0; i < n; ++i)
        lengthsMoving = lengthsMoving || lengths[(size_t) i].isMoving();

    std::array<double, maxLines> s {};
    std::array<double, maxLines> position {};
    std::array<bool, maxLines> fractional {};

    for (int sample = 0; sample < numSamples; ++sample)
    {
        // Decay filters follow the smoothed rates and the gliding lengths, every 32 samples.
        if (--samplesUntilFilterUpdate <= 0)
        {
            samplesUntilFilterUpdate = coefficientInterval;
            if (rateMid.isSmoothing() || rateLow.isSmoothing() || rateHigh.isSmoothing() || lengthsMoving)
            {
                rateMid.skip (coefficientInterval);
                rateLow.skip (coefficientInterval);
                rateHigh.skip (coefficientInterval);
                updateDecayFilters (true);
            }
            lengthsMoving = false;
            for (int i = 0; i < n; ++i)
                lengthsMoving = lengthsMoving || lengths[(size_t) i].isMoving();
        }

        // The mid gains glide to their latest targets (the shelves glide inside GlidingSvf).
        if (glideRemaining > 0)
        {
            if (--glideRemaining == 0)
                gains = gainTargets;
            else
                for (int i = 0; i < n; ++i)
                    gains[(size_t) i] += gainSteps[(size_t) i];
        }

        // Read positions: the (gliding) length, plus the modulation on the modulated lines. A line
        // that's neither gliding nor modulated is read at its whole-sample length, exactly.
        const auto depth = excursion.getNextValue();
        for (int i = 0; i < n; ++i)
        {
            position[(size_t) i] = lengths[(size_t) i].next();
            fractional[(size_t) i] = lengthsMoving;
        }
        if (depth > 0.0)
        {
            for (size_t k = 0; k < (size_t) numModulatedLines; ++k)
            {
                const auto line = (size_t) modulatedLine[k];
                position[line] += depth * (double) lfos[k].next();
                fractional[line] = true;
            }
        }
        else
        {
            for (auto& lfo : lfos)
                lfo.next(); // keep the LFOs turning, so the depth can come back smoothly
        }

        // Early reflections: write this sample, then tap the pattern (scaled by Size, gliding with it).
        earlyLines[0].write (inLeft[sample]);
        earlyLines[1].write (inRight[sample]);
        const auto scale = earlyScale.next() * toSamples;
        std::array<double, 2> early {};
        for (size_t side = 0; side < 2; ++side)
            for (size_t k = 0; k < (size_t) numEarlyTaps; ++k)
                early[side] += earlyTapGain[side][k] * earlyLines[side].read (earlyTapMs[side][k] * scale);

        // Input diffusion: four lattice allpasses per side (0.75 d, 0.75 d, 0.625 d, 0.625 d).
        const auto d = diffusion.getNextValue();
        std::array<double, 2> diffused {};
        for (size_t side = 0; side < 2; ++side)
        {
            auto v = early[side];
            for (size_t k = 0; k < (size_t) numDiffusers; ++k)
                v = lattice (diffusers[side][k], diffuserLength[side][k], v, (k < 2 ? 0.75 : 0.625) * d);
            diffused[side] = v;
        }

        // The network: every line's output (written at least one line length ago), through its decay
        // filter; the two output mixes; the feedback matrix; then this sample's writes.
        double yLeft = 0.0, yRight = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto& line = lines[(size_t) i];
            const double r = fractional[(size_t) i] ? line.read (position[(size_t) i] - 1.0)
                                                     : line.readInteger ((int) position[(size_t) i] - 1);
            const auto v = gains[(size_t) i] * highShelves[(size_t) i].processSample (lowShelves[(size_t) i].processSample (r));
            s[(size_t) i] = v;
            yLeft += tapLeft[(size_t) i] * v;
            yRight += tapRight[(size_t) i] * v;
        }

        // Shimmer: the projection on u, shifted, crossfaded back in along u (see the class comment).
        if (const auto g = shimmerAmount.getNextValue(); g > 0.0 || shimmerAmount.isSmoothing())
        {
            double m = 0.0;
            for (int i = 0; i < n; ++i)
                m += shimmerDirection[(size_t) i] * s[(size_t) i];
            const auto shifted = (double) shimmerLimit (shimmerShifter.processSample ((float) shimmerLowPass.processSample (m)));
            const auto replacement = std::sqrt (1.0 - g * g) * m + g * shifted;
            shimmerPowerIn += shimmerAverage * (m * m - shimmerPowerIn);
            shimmerPowerOut += shimmerAverage * (replacement * replacement - shimmerPowerOut);
            const auto allowed = shimmerPowerMargin * shimmerPowerIn;
            const auto k = shimmerPowerOut > allowed && shimmerPowerOut > 1.0e-30 ? std::sqrt (allowed / shimmerPowerOut) : 1.0;
            const auto change = k * replacement - m; // the u component's new value, minus the old
            for (int i = 0; i < n; ++i)
                s[(size_t) i] += change * shimmerDirection[(size_t) i];
        }

        feedbackMatrix (s.data(), n);

        for (int i = 0; i < n; ++i)
            lines[(size_t) i].write ((float) (s[(size_t) i] + injectLeft[(size_t) i] * diffused[0] + injectRight[(size_t) i] * diffused[1]));

        const auto e = earlyGain.getNextValue(), l = lateGain.getNextValue();
        outLeft[sample] = (float) (e * early[0] + l * yLeft);
        outRight[sample] = (float) (e * early[1] + l * yRight);
    }
}

double FeedbackDelayNetwork::storedEnergy() const
{
    double sum = 0.0;
    for (int i = 0; i < numLines; ++i)
        sum += windowEnergy (lines[(size_t) i], (int) std::lround (lengths[(size_t) i].getValue()));
    return sum;
}

// =================================================================================================
// Reverb
// =================================================================================================

Reverb::Rates Reverb::decayRates (const Settings& s)
{
    if (s.freeze)
        return { 0.0, 0.0, 0.0 };

    const auto t60 = juce::jlimit (0.1, 30.0, (double) s.decaySeconds);
    const auto low = juce::jlimit (0.25, 4.0, (double) s.lowDecayMultiplier);
    const auto high = juce::jlimit (0.1, 2.0, (double) s.highDecayMultiplier);
    return { 60.0 / t60, 60.0 / (t60 * low), 60.0 / (t60 * high) };
}

DattorroPlate::Parameters Reverb::plateParameters (const Settings& s, double tau)
{
    const auto r = decayRates (s);
    DattorroPlate::Parameters p; // Table 1 for everything not mapped here

    // Four decay multipliers per trip of tau seconds: 80 log10(decay) = -r_mid tau.
    p.decay = std::pow (10.0, -r.mid * tau / 80.0);

    // Two of each shelf per trip (one per half) share the low and high bands' extra (or reduced) loss.
    p.lowShelfDb = -(r.low - r.mid) * tau / 2.0;
    p.highShelfDb = -(r.high - r.mid) * tau / 2.0;

    // The paper's damping one-pole is off ("no damping = 0.0", Table 1): fitted to the high band at
    // 4 kHz it would also cut the mid band (at a 3 s decay with high x0.3 it measured a 1.8 s mid T60),
    // so the high shelf does that job instead.
    p.damping = 0.0;

    const auto diffusion = juce::jlimit (0.0, 1.0, (double) s.diffusion);
    p.inputDiffusion1 = 0.75 * diffusion;
    p.inputDiffusion2 = 0.625 * diffusion;
    p.excursion = s.freeze ? 0.0 : 16.0 * juce::jlimit (0.0, 1.0, (double) s.modDepth);
    p.modRateHz = juce::jlimit (0.0, 10.0, (double) s.modRateHz);
    return p;
}

Reverb::Reverb()
{
    selected = settings.engine;
}

void Reverb::setSettings (const Settings& newSettings)
{
    settings = newSettings;
    if (prepared)
        applySettings (false);
}

void Reverb::applySettings (bool snap)
{
    const auto& s = settings;
    selected = s.engine;

    inputGain.setTargetValue ((bypassed || s.freeze) ? 0.0f : 1.0f);
    bypassBlend.setTargetValue (bypassed ? 1.0f : 0.0f);

    // Equal-power mix: wet and dry are decorrelated, so cos^2 + sin^2 = 1 keeps the power steady.
    const auto mix = juce::jlimit (0.0, 1.0, (double) s.mix);
    dryGain.setTargetValue ((float) std::cos (mix * pi / 2.0));
    wetGain.setTargetValue ((float) std::sin (mix * pi / 2.0));
    width.setTargetValue (juce::jlimit (0.0f, 1.0f, s.width));
    ducking.setTargetValue (juce::jlimit (0.0f, 1.0f, s.ducking));

    // Pre-delay: process() crossfades to a new value (after any fade already running).
    preDelayRequested = (int) std::lround (juce::jlimit (0.0, maxPreDelayMs, (double) s.preDelayMs) * sampleRate / 1000.0);
    if (snap)
    {
        preDelayCurrent = preDelayTarget = preDelayRequested;
        preDelayFading = false;
    }

    lowCut.set (s.lowCutHz > 20.5f, s.lowCutHz, CutFilter::Slope::db12);
    highCut.set (s.highCutHz < 19999.5f, s.highCutHz, CutFilter::Slope::db12);

    // Engines: each one gets its targets, running or not (a starting engine snaps to them).
    const auto rates = decayRates (s);
    const auto balance = juce::jlimit (0.0, 1.0, (double) s.earlyLate);
    FeedbackDelayNetwork::Parameters f;
    f.sizeScale = sizeScale (s.size);
    f.rateMid = rates.mid;
    f.rateLow = rates.low;
    f.rateHigh = rates.high;
    f.diffusion = juce::jlimit (0.0, 1.0, (double) s.diffusion);
    f.modDepth = s.freeze ? 0.0 : juce::jlimit (0.0, 1.0, (double) s.modDepth);
    f.modRateHz = juce::jlimit (0.0, 10.0, (double) s.modRateHz);
    f.earlyGain = std::min (1.0, 2.0 * (1.0 - balance)); // the balance knob: both full at the centre
    f.lateGain = std::min (1.0, 2.0 * balance) * fdnLateLevel;
    f.shimmer = FeedbackDelayNetwork::maxShimmer * juce::jlimit (0.0, 1.0, (double) s.shimmer);
    f.shimmerRatio = std::exp2 (juce::jlimit (-12.0, 24.0, (double) s.shimmerSemitones) / 12.0);
    room.setParameters (f);
    hall.setParameters (f);
    plate.setParameters (plateParameters (s, plate.loopSeconds()));

    if (snap)
    {
        for (auto* v : { &inputGain, &bypassBlend, &dryGain, &wetGain, &width, &ducking })
            v->setCurrentAndTargetValue (v->getTargetValue());
        room.snapToTargets();
        hall.snapToTargets();
        plate.snapToTargets();
    }
}

void Reverb::prepare (double rate, int maxBlockSize)
{
    sampleRate = rate;
    maxBlock = maxBlockSize;

    room.prepare (rate);
    hall.prepare (rate);
    plate.prepare (rate);

    for (auto& line : preDelay)
        line.prepare ((int) std::ceil (maxPreDelayMs * rate / 1000.0) + 4);

    pre.setSize (2, maxBlockSize);
    engineIn.setSize (2, maxBlockSize);
    engineOut.setSize (2, maxBlockSize);
    wet.setSize (2, maxBlockSize);
    ramp.assign ((size_t) maxBlockSize, 0.0f);
    duckGains.assign ((size_t) maxBlockSize, 1.0f);
    monoScratch.assign ((size_t) maxBlockSize, 0.0f);

    inputGain.reset (rate, inputFadeSeconds);
    bypassBlend.reset (rate, inputFadeSeconds);
    for (auto* v : { &dryGain, &wetGain, &width, &ducking })
        v->reset (rate, mixSmoothingSeconds);

    // Ducking envelope: branching one-pole, 63% of a step in the attack or release time.
    duckAttack = std::exp (-1.0 / (duckAttackMs * 0.001 * rate));
    duckRelease = std::exp (-1.0 / (duckReleaseMs * 0.001 * rate));

    prepared = true;
    applySettings (true);
    lowCut.prepare (rate, maxBlockSize); // after set(): snaps to the settings
    highCut.prepare (rate, maxBlockSize);
    reset();
}

void Reverb::reset()
{
    // Everything at its setting with no glides, then every state cleared.
    if (prepared)
        applySettings (true);
    selected = settings.engine;

    for (auto& line : preDelay)
        line.reset();
    room.reset();
    hall.reset();
    plate.reset();
    lowCut.reset();
    highCut.reset();

    preDelayCurrent = preDelayTarget = preDelayRequested;
    preDelayFading = false;
    preDelayFade = 0.0f;
    duckEnvelope = 0.0;
    idle = false;
    silentSamples = 0;

    for (size_t e = 0; e < engines.size(); ++e)
    {
        const bool on = (Engine) e == selected;
        engines[e].running = on;
        engines[e].position = on ? 1.0f : 0.0f;
    }
}

void Reverb::setBypassed (bool shouldBeBypassed)
{
    if (shouldBeBypassed == bypassed)
        return;

    bypassed = shouldBeBypassed;
    inputGain.setTargetValue ((bypassed || settings.freeze) ? 0.0f : 1.0f);
    bypassBlend.setTargetValue (bypassed ? 1.0f : 0.0f);
}

bool Reverb::isSwitchingEngine() const noexcept
{
    // Switching while the selected engine is still fading in, or another one is still fading out.
    for (size_t e = 0; e < engines.size(); ++e)
    {
        if (! engines[e].running)
            continue;
        if ((Engine) e != selected || engines[e].position < 1.0f)
            return true;
    }
    return false;
}

double Reverb::storedEnergy() const
{
    switch (selected)
    {
        case Engine::room:  return room.storedEnergy();
        case Engine::hall:  return hall.storedEnergy();
        case Engine::plate: return plate.storedEnergy();
    }
    return 0.0;
}

void Reverb::startEngine (Engine engine)
{
    // A fresh start: no old tail, and the coefficients at their targets with no glide.
    auto& state = engines[(size_t) engine];
    switch (engine)
    {
        case Engine::room:  room.reset();  room.snapToTargets();  break;
        case Engine::hall:  hall.reset();  hall.snapToTargets();  break;
        case Engine::plate: plate.reset(); plate.snapToTargets(); break;
    }
    state.running = true;
    state.position = 0.0f;
}

void Reverb::runEngine (Engine engine, const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples)
{
    switch (engine)
    {
        case Engine::room:
            room.process (inLeft, inRight, outLeft, outRight, numSamples);
            break;
        case Engine::hall:
            hall.process (inLeft, inRight, outLeft, outRight, numSamples);
            break;
        case Engine::plate:
        {
            // Fig. 1's input: (xL + xR) / 2.
            for (int n = 0; n < numSamples; ++n)
                monoScratch[(size_t) n] = 0.5f * (inLeft[n] + inRight[n]);
            plate.process (monoScratch.data(), outLeft, outRight, numSamples);
            if (plateLevel != 1.0)
            {
                for (int n = 0; n < numSamples; ++n)
                {
                    outLeft[n] *= (float) plateLevel;
                    outRight[n] *= (float) plateLevel;
                }
            }
            break;
        }
    }
}


void Reverb::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    const auto numSamples = (int) block.getNumSamples();
    if (numSamples == 0 || block.getNumChannels() < 2)
        return;
    jassert (numSamples <= maxBlock);

    float* const io[2] = { block.getChannelPointer (0), block.getChannelPointer (1) };

    // Bypassed and silent: there's nothing inside but silence and the dry is at unity, so the buffer
    // (which is the dry) is left alone and nothing runs.
    if (idle)
    {
        if (bypassed)
            return;

        // Switched back on: wake up. Whatever moved while asleep jumps to its setting (the reverb only
        // holds silence, so there's nothing to glide); the input fades back in and the dry back down.
        idle = false;
        silentSamples = 0;
        duckEnvelope = 0.0;
        for (auto* v : { &wetGain, &width, &ducking, &dryGain })
            v->setCurrentAndTargetValue (v->getTargetValue());
        room.snapToTargets();
        hall.snapToTargets();
        plate.snapToTargets();
    }

    // ---- Input: the bypass and freeze fade, the ducking envelope, the pre-delay ----------------
    if (! preDelayFading && preDelayRequested != preDelayCurrent)
    {
        preDelayTarget = preDelayRequested; // latched until this fade is done
        preDelayFading = true;
        preDelayFade = 0.0f;
    }
    const auto fadeStep = (float) (1.0 / (preDelayFadeSeconds * sampleRate));
    const bool ducked = ducking.isSmoothing() || ducking.getTargetValue() > 0.0f;
    auto* preLeft = pre.getWritePointer (0);
    auto* preRight = pre.getWritePointer (1);

    for (int n = 0; n < numSamples; ++n)
    {
        const auto g = inputGain.getNextValue();
        const auto left = io[0][n], right = io[1][n];

        // Ducking: a peak envelope of the dry input (branching one-pole), then the wet's attenuation,
        // none below -50 dBFS, the full 18 dB x amount above -30 dBFS, linear in dB between.
        const double level = std::max (std::abs (left), std::abs (right));
        const auto coefficient = level > duckEnvelope ? duckAttack : duckRelease;
        duckEnvelope = coefficient * duckEnvelope + (1.0 - coefficient) * level;
        const auto amount = ducking.getNextValue();
        if (ducked)
        {
            const auto envelopeDb = 20.0 * std::log10 (std::max (1.0e-9, duckEnvelope));
            const auto depth = juce::jlimit (0.0, 1.0, (envelopeDb - duckFloorDb) / (duckFullDb - duckFloorDb));
            duckGains[(size_t) n] = (float) dbToGain (-duckRangeDb * amount * depth);
        }
        else
        {
            duckGains[(size_t) n] = 1.0f;
        }

        // Pre-delay (wet only). A change crossfades two read heads with weights that sum to 1 (they
        // carry the same signal a little apart, so they're correlated, and amplitude-complementary
        // weights keep the level), along a half cosine so the fade has no corners.
        preDelay[0].write (left * g);
        preDelay[1].write (right * g);
        if (! preDelayFading)
        {
            preLeft[n] = preDelay[0].readInteger (preDelayCurrent);
            preRight[n] = preDelay[1].readInteger (preDelayCurrent);
        }
        else
        {
            preDelayFade = std::min (1.0f, preDelayFade + fadeStep);
            const auto b = smoothFade (preDelayFade), a = 1.0f - b;
            preLeft[n] = a * preDelay[0].readInteger (preDelayCurrent) + b * preDelay[0].readInteger (preDelayTarget);
            preRight[n] = a * preDelay[1].readInteger (preDelayCurrent) + b * preDelay[1].readInteger (preDelayTarget);
            if (preDelayFade >= 1.0f)
            {
                preDelayCurrent = preDelayTarget;
                preDelayFading = false;
            }
        }
    }

    // ---- Engines: the selected one, plus any still fading out ------------------------------------
    wet.clear (0, numSamples);
    auto* wetLeft = wet.getWritePointer (0);
    auto* wetRight = wet.getWritePointer (1);
    const auto engineStep = (float) (1.0 / (engineFadeSeconds * sampleRate));

    for (size_t e = 0; e < engines.size(); ++e)
    {
        const auto engine = (Engine) e;
        auto& state = engines[e];
        const bool isSelected = engine == selected;

        if (isSelected && ! state.running)
            startEngine (engine);
        if (! state.running)
            continue;

        auto* outLeft = engineOut.getWritePointer (0);
        auto* outRight = engineOut.getWritePointer (1);

        if (isSelected && state.position >= 1.0f)
        {
            // Steady: the selected engine at full level.
            runEngine (engine, preLeft, preRight, outLeft, outRight, numSamples);
            for (int n = 0; n < numSamples; ++n)
            {
                wetLeft[n] += outLeft[n];
                wetRight[n] += outRight[n];
            }
        }
        else
        {
            // Fading. With s = the half-cosine shape of p: the input is scaled by s (a starting engine's
            // input fades in instead of starting with a step) and the output by sin(s pi/2). Two engines'
            // tails are uncorrelated, so the outputs crossfade at equal power: the outgoing engine's
            // gain is sin((1 - s) pi/2) = cos(s pi/2).
            auto* inLeft = engineIn.getWritePointer (0);
            auto* inRight = engineIn.getWritePointer (1);
            for (int n = 0; n < numSamples; ++n)
            {
                state.position = isSelected ? std::min (1.0f, state.position + engineStep) : std::max (0.0f, state.position - engineStep);
                const auto shape = smoothFade (state.position);
                ramp[(size_t) n] = shape;
                inLeft[n] = preLeft[n] * shape;
                inRight[n] = preRight[n] * shape;
            }
            runEngine (engine, inLeft, inRight, outLeft, outRight, numSamples);
            for (int n = 0; n < numSamples; ++n)
            {
                const auto gain = (float) std::sin (0.5 * pi * ramp[(size_t) n]);
                wetLeft[n] += gain * outLeft[n];
                wetRight[n] += gain * outRight[n];
            }
        }

        if (! isSelected && state.position <= 0.0f)
            state.running = false; // faded out: stop spending CPU on it
    }

    // How loud the tail is, for the spillover idle state.
    float peak = 0.0f;
    for (int n = 0; n < numSamples; ++n)
        peak = std::max (peak, std::max (std::abs (wetLeft[n]), std::abs (wetRight[n])));

    // ---- Width: mid/side, side scaled by the width (1 = as the engine made it, 0 = mono) ----------
    if (width.isSmoothing() || width.getTargetValue() < 1.0f)
    {
        for (int n = 0; n < numSamples; ++n)
        {
            const auto w = width.getNextValue();
            const auto mid = 0.5f * (wetLeft[n] + wetRight[n]);
            const auto side = 0.5f * (wetLeft[n] - wetRight[n]) * w;
            wetLeft[n] = mid + side;
            wetRight[n] = mid - side;
        }
    }

    // ---- Wet low cut and high cut ----------------------------------------------------------------
    float* wetChannels[2] = { wetLeft, wetRight };
    lowCut.process (wetChannels, 2, numSamples);
    highCut.process (wetChannels, 2, numSamples);

    // ---- Mix: dry x (cos, or unity while bypassed) + wet x sin x ducking -------------------------
    for (int n = 0; n < numSamples; ++n)
    {
        const auto mixDry = dryGain.getNextValue();
        const auto b = bypassBlend.getNextValue();
        const auto dry = (1.0f - b) * mixDry + b; // exactly 1 once bypassed
        const auto wetAmount = wetGain.getNextValue() * duckGains[(size_t) n];
        io[0][n] = io[0][n] * dry + wetLeft[n] * wetAmount;
        io[1][n] = io[1][n] * dry + wetRight[n] * wetAmount;
    }

    // ---- Spillover: bypassed, input gone, tail below -120 dBFS for a whole second -> idle --------
    // A second is longer than any path through the reverb (pre-delay 500 ms, early taps 170 ms, the
    // diffusers, a 200 ms line), so anything still in flight would have shown up by then.
    if (bypassed && ! inputGain.isSmoothing() && inputGain.getCurrentValue() == 0.0f && ! isSwitchingEngine())
    {
        silentSamples = peak < (float) silenceThreshold ? silentSamples + numSamples : 0;
        if (silentSamples >= (int) (silenceHoldSeconds * sampleRate))
            idle = true;
    }
    else
    {
        silentSamples = 0;
    }
}

} // namespace ampsim

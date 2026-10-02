#include "TunerAnalysis.h"

#include <cmath>

namespace ampsim
{

namespace
{
constexpr double gateHysteresisDb = 6.0; // once a note is on, the gate closes 6 dB below where it opened
constexpr double levelSeconds = 0.05;
constexpr double onsetStepSeconds = 0.005;
constexpr double onsetAssociationSeconds = 0.15; // a pluck this recent belongs to a newly detected note
constexpr double graceSeconds = 0.1;             // a note survives this long without a confident reading
constexpr double minimumFineSeconds = 0.02;
} // namespace

void TunerAnalysis::prepare (double newSampleRate, const Settings& newSettings)
{
    sampleRate = newSampleRate;
    settings = newSettings;
    settings.medianLength = juce::jlimit (1, (int) medianRing.size(), settings.medianLength);
    settings.referenceA4 = juce::jlimit (minReferenceA4, maxReferenceA4, settings.referenceA4);

    detector.prepare (sampleRate, PitchDetector::Settings::tuner());

    // The longest fine span: settle + window at the lowest frequency the detector can report (a period of
    // maxLag + 1 decimated samples), about 0.78 s at 48 kHz. Rounded up to a power of two: 1.37 s.
    const auto lowest = detector.getDecimatedRate() / (detector.getMaxLag() + 1);
    const auto longest = fineSpan (lowest, INT64_MAX / 2, sampleRate);
    history.prepare (longest.settle + longest.window + 1);

    levelSamples = std::max (1, (int) (levelSeconds * sampleRate));
    onsetStep = std::max (1, (int) (onsetStepSeconds * sampleRate));
    associationSamples = (int64_t) (onsetAssociationSeconds * sampleRate);
    graceSamples = (int64_t) (graceSeconds * sampleRate);

    // The fine stage waits until the coarse detector's whole frame lies after the pluck (before that, the
    // coarse estimate can still be the previous note's) and there's at least 20 ms to look at.
    minimumFineSamples = std::max ((int) (minimumFineSeconds * sampleRate),
                                   juce::roundToInt (detector.getFrameSeconds() * sampleRate));
    reset();
}

void TunerAnalysis::reset() noexcept
{
    detector.reset();
    history.reset();
    lastAnalysis = 0;
    nextOnsetCheck = 0;
    lastOnset = -1;
    lastConfident = 0;
    noteStart = 0;
    noteActive = false;
    noteHasFine = false;
    trackedCoarse = 0.0;
    medianCount = 0;
    medianNext = 0;
    heldFrequency = 0.0;
    needle = 0.0;
    strobePhase = 0.0;
    shownNote = -1;
    snapNeedle = true;
    coarse = {};
    fine = 0.0;
    reading = {};
    reading.referenceA4 = settings.referenceA4;
}

void TunerAnalysis::markDiscontinuity() noexcept
{
    noteActive = false;
    lastOnset = history.getTotal(); // whatever comes next starts after the gap
}

TunerAnalysis::FineSpan TunerAnalysis::fineSpan (double coarseHz, int64_t available, double rate) noexcept
{
    // One band-pass stage's envelope decays with time constant Q / (pi f) (its poles sit at a distance
    // pi f / Q from the j omega axis, in rad/s). Ten of those leave about 1e-4 of the start-up transient,
    // and 0.15 s is the floor the study started from. 12 periods or 0.25 s of crossings, whichever is longer.
    const auto tau = fineQ / (juce::MathConstants<double>::pi * coarseHz);
    const auto settle = (int) (std::max (0.15, 10.0 * tau) * rate);
    const auto window = (int) (std::max (0.25, 12.0 / coarseHz) * rate);

    if (available >= settle + window)
        return { settle, window };

    // A young note: the same proportion, but at least 6 time constants of settling (the transient is then
    // about 2%; with the 3 a plain proportion gives early on, it was 20% and a full cent on the first reading).
    const auto proportional = (int) ((double) available * settle / (double) (settle + window));
    const auto youngSettle = (int) std::min<int64_t> (std::min (settle, std::max ((int) (6.0 * tau * rate), proportional)), available);
    return { youngSettle, (int) available - youngSettle };
}

double TunerAnalysis::refineFrequency (const float* x, int length, int settle, double coarseHz, double rate) noexcept
{
    // Two identical band-passes in series: k s / (s^2 + k s + 1) each, k = 1/Q, unit gain at the centre.
    // Together they're down 26 dB an octave above (the 2nd partial) and 41 dB at the 3rd, so what's
    // left crosses zero once per period of the first partial.
    Svf first, second;
    const auto c = Svf::design (Svf::Type::bandpass, coarseHz, fineQ, 0.0, rate);
    first.setCoefficients (c);
    second.setCoefficients (c);

    const auto countFrom = std::max (1, settle + 1);
    double previous = 0.0, firstCrossing = 0.0, lastCrossing = 0.0;
    int crossings = 0;

    for (int i = 0; i < length; ++i)
    {
        const auto y = second.processSample (first.processSample ((double) x[i]));

        // An upward crossing between samples i - 1 and i. The line through (i - 1, previous) and (i, y)
        // hits zero at i - 1 + previous / (previous - y).
        if (i >= countFrom && previous < 0.0 && y >= 0.0)
        {
            const auto t = (i - 1) + previous / (previous - y);

            if (crossings == 0)
                firstCrossing = t;

            lastCrossing = t;
            ++crossings;
        }

        previous = y;
    }

    if (crossings < minFinePeriods + 1)
        return 0.0;

    return rate * (crossings - 1) / (lastCrossing - firstCrossing);
}

void TunerAnalysis::scanOnsets (int64_t now) noexcept
{
    // Compare the energy of the newest period with the period before, every 5 ms. Over exactly one
    // period the energy of a steady note doesn't depend on where the window starts, so a decaying or
    // steady note never doubles; a pluck does. Before a note is known the window is 33 ms (a period of
    // 30 Hz): that's at least 0.9 periods of any note the tuner reads, and the energy of a sinusoid over
    // L seconds wobbles with the window's position by at most 1 / (2 pi f L) of itself, under 20%, so
    // there too only an attack can double it.
    const auto period = juce::jlimit (onsetStep, history.getCapacity() / 8,
                                      (int) (sampleRate / (noteActive ? trackedCoarse : 30.0)));
    const auto gatePower = std::pow (10.0, settings.gateDb / 10.0) * period;
    const auto refractory = std::max ((int64_t) (0.05 * sampleRate), (int64_t) 2 * period);

    nextOnsetCheck = std::max (nextOnsetCheck, now - history.getCapacity() + 2 * period);

    for (; nextOnsetCheck <= now; nextOnsetCheck += onsetStep)
    {
        const auto t = nextOnsetCheck;

        if (t < 2 * period)
            continue;

        const auto* x = history.newest ((int) (now - (t - 2 * period))); // x[0] is sample t - 2 period
        double before = 0.0, after = 0.0;

        for (int i = 0; i < period; ++i)
            before += (double) x[i] * (double) x[i];

        for (int i = period; i < 2 * period; ++i)
            after += (double) x[i] * (double) x[i];

        if (after > settings.onsetRatio * before && after >= gatePower && t - lastOnset >= refractory)
            lastOnset = t;
    }
}

void TunerAnalysis::pushMedian (double log2Frequency) noexcept
{
    medianRing[(size_t) medianNext] = log2Frequency;
    medianNext = (medianNext + 1) % settings.medianLength;
    medianCount = std::min (medianCount + 1, settings.medianLength);
}

double TunerAnalysis::median() const noexcept
{
    std::array<double, 9> sorted {};
    std::copy (medianRing.begin(), medianRing.begin() + medianCount, sorted.begin());
    std::sort (sorted.begin(), sorted.begin() + medianCount);
    const auto middle = medianCount / 2;
    return medianCount % 2 == 1 ? sorted[(size_t) middle] : 0.5 * (sorted[(size_t) middle - 1] + sorted[(size_t) middle]);
}

const TunerReading& TunerAnalysis::analyse() noexcept
{
    const auto now = history.getTotal();
    const auto dt = (double) (now - lastAnalysis) / sampleRate;
    lastAnalysis = now;

    // Input level over the newest 50 ms.
    const auto levelLength = (int) std::min<int64_t> (now, levelSamples);
    double sumSquares = 0.0;
    const auto* recent = history.newest (levelLength);
    for (int i = 0; i < levelLength; ++i)
        sumSquares += (double) recent[i] * (double) recent[i];
    const auto levelDb = levelLength > 0 ? 10.0 * std::log10 (sumSquares / levelLength + 1.0e-30) : -300.0;

    const auto onsetBefore = lastOnset;
    scanOnsets (now);

    // The same note picked again: count only what comes after the pick. This has to happen even when the
    // pick itself makes this analysis unconfident, or the next fine window would straddle it.
    if (noteActive && lastOnset != onsetBefore)
        noteStart = lastOnset;

    coarse = detector.detect();
    const auto gateDb = noteActive ? settings.gateDb - gateHysteresisDb : settings.gateDb;
    const auto confident = coarse.frequency > 0.0 && coarse.clarity >= settings.clarityThreshold && levelDb >= gateDb;
    fine = 0.0;

    if (confident)
    {
        lastConfident = now;
        const auto newNote = ! noteActive || std::abs (1200.0 * std::log2 (coarse.frequency / trackedCoarse)) > settings.newNoteCents;

        if (newNote)
        {
            noteActive = true;
            noteHasFine = false;
            medianCount = 0;
            medianNext = 0;
            noteStart = (lastOnset >= 0 && now - lastOnset <= associationSamples) ? lastOnset : now;
        }

        trackedCoarse = coarse.frequency;
        const auto available = std::min<int64_t> (now - noteStart, history.getCapacity() - 1);

        if (available >= minimumFineSamples)
        {
            const auto span = fineSpan (coarse.frequency, available, sampleRate);
            const auto length = span.settle + span.window;
            fine = refineFrequency (history.newest (length), length, span.settle, coarse.frequency, sampleRate);
        }

        if (fine > 0.0)
        {
            if (! noteHasFine)
            {
                shownNote = -1; // a new note is shown by plain rounding; hysteresis applies within a note
                snapNeedle = true;
            }

            noteHasFine = true;
            pushMedian (std::log2 (fine));
        }
    }
    else if (noteActive && now - lastConfident > graceSamples)
    {
        noteActive = false;
    }

    const auto live = confident && noteHasFine;

    if (live)
        heldFrequency = std::exp2 (median());

    updateDisplay (live, dt, levelDb);
    return reading;
}

void TunerAnalysis::updateDisplay (bool live, double dt, double levelDb) noexcept
{
    reading.live = live;
    reading.clarity = coarse.clarity;
    reading.levelDb = levelDb;
    reading.referenceA4 = settings.referenceA4;

    if (heldFrequency <= 0.0)
    {
        reading.strobeVelocity = 0.0;
        return;
    }

    // Note number relative to the reference: 12 semitones per octave, A4 = 69. The shown note only moves
    // when the pitch is more than 50 + hysteresis cents away from it.
    const auto note = 12.0 * std::log2 (heldFrequency / settings.referenceA4) + 69.0;

    if (shownNote < 0 || std::abs (note - shownNote) > 0.5 + settings.hysteresisCents / 100.0)
    {
        shownNote = juce::roundToInt (note);
        snapNeedle = true;
    }

    const auto rawCents = 100.0 * (note - shownNote);

    // Needle: one-pole smoothing toward the reading, exact for any update interval: after dt the
    // remaining distance is multiplied by exp(-dt / tau). Holding, it shows the held reading exactly.
    if (snapNeedle || ! live)
        needle = rawCents;
    else
        needle += (1.0 - std::exp (-dt / settings.needleSeconds)) * (rawCents - needle);

    snapNeedle = false;

    // Strobe: the pattern's phase is the integral of its speed, which is proportional to the offset.
    reading.strobeVelocity = live ? settings.strobePeriodsPerSecondPerCent * rawCents : 0.0;
    strobePhase += reading.strobeVelocity * dt;
    strobePhase -= std::floor (strobePhase);

    reading.hasReading = true;
    reading.frequency = heldFrequency;
    reading.midiNote = shownNote;
    reading.cents = needle;
    reading.rawCents = rawCents;
    reading.strobePhase = strobePhase;
}

} // namespace ampsim

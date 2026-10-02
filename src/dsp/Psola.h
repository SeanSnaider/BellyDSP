#pragma once

#include "PitchDetector.h"
#include "PitchShifter.h"

#include <array>
#include <cstdint>

namespace ampsim
{

/// The PSOLA engine (Mono) of the shared pitch shifter: pitch-synchronous overlap-add (Moulines and
/// Charpentier, "Pitch-synchronous waveform processing techniques for text-to-speech synthesis using
/// diphones", Speech Communication 9, 1990; the real-time form follows Zolzer (ed.), "DAFX", 2nd ed., the
/// time-segment processing chapter). Excellent on single notes, octave down above all, because it cuts the
/// signal at its own period: every grain holds whole periods, so there is no splice to hide. It needs one
/// period, so it fails on chords; PsolaAnalysis says how sure it is, and callers fade out (the harmonizer)
/// or fall back to the granular engine (the multivoicer's Mono mode).
///
/// One analysis drives any number of voices at independent ratios (the harmonizer runs up to 4, the
/// multivoicer up to 8), all reading one PitchShifterInput.

/// The shared analysis: when there is one pitch, and where its periods are in the signal being shifted.
///
/// 1. The period. The shared McLeod detector (PitchDetector, harmonizer preset with the floor given to
///    prepare) reads the clean DI, never the processed signal, every 64 samples. A reading counts if its
///    clarity is 0.9 or more. Two agreeing readings in a row (within 50 cents) confirm a pitch, and two
///    unconfident readings in a row end it, so no single outlier does anything (the detector can report
///    the previous note for up to 2.7 ms after a pluck and a stray reading in the first millisecond).
///
/// 2. Pitch marks, on the signal actually being shifted (post amp and cab, so its waveform differs from
///    the DI's, but its period doesn't). A confirmed pitch that isn't the one being tracked (50 cents away,
///    or nothing tracked) starts a new track: the first mark goes on the largest sample of the latest
///    period. Each next mark is where the period ending there best matches the period ending at the previous
///    mark: normalized cross-correlation over one period, searched over +-1/8 period around the predicted
///    spot, refined by a parabola. So marks sit at the same point of every period (they can't jump to
///    another peak), and their spacing is the local period, measured on the waveform to a fraction of a
///    sample (the detector's coarse estimate only seeds the prediction). A match below 0.7 means the period
///    broke (a new pluck, a chord): the track is marked lost until the detector confirms a pitch again.
///    Marks keep coming at the last period while lost, so voices fading out still have grains.
///
/// 3. Confidence: 1 while a pitch is confirmed and its marks match, else 0, ramped over 2 ms.
///
/// Per sample, after PitchShifterInput::push() of the same sample: process(di, input). For deterministic
/// tests, setPeriodOverride() replaces the detector's readings with a fixed period.
class PsolaAnalysis
{
public:
    static constexpr int hop = 64;
    static constexpr double clarityThreshold = 0.9;
    static constexpr double agreeCents = 50.0;   // two readings this close confirm a pitch
    static constexpr double newNoteCents = 50.0; // a confirmed pitch this far from the track starts a new one
    static constexpr double trackQuality = 0.7;  // a mark matching worse than this loses the track
    static constexpr double searchFraction = 0.125;
    static constexpr double confidenceRampMs = 2.0;
    static constexpr int maxMarks = 256;
    static constexpr double defaultMinFrequency = 80.0; // the low E (82.4 Hz) with a little room

    struct Mark
    {
        double position = 0.0; // absolute sample index (fractional)
        double period = 0.0;   // the spacing from the previous mark: the local period, samples
        double quality = 0.0;  // the correlation it was found with (1 for an anchor)
    };

    /// Allocates (the detector). minFrequency is the detector's floor: lower reaches lower notes but slows
    /// detection a little (BUILD_PLAN "Harmonizer").
    void prepare (double sampleRate, double minFrequency = defaultMinFrequency);
    void reset() noexcept;

    /// > 0: use this period (samples) as every reading, with full clarity, instead of the detector.
    void setPeriodOverride (double periodSamples) noexcept { overridePeriod = periodSamples; }

    /// One sample, after input.push(): the DI sample for the detector; marks are tracked on `input`.
    void process (float di, const PitchShifterInput& input) noexcept;

    double getConfidence() const noexcept { return confidence; }
    bool isConfident() const noexcept { return tracking && ! lost; }
    bool isTracking() const noexcept { return tracking; }
    double getPeriod() const noexcept { return tracking ? period : 0.0; }
    int getNoteCount() const noexcept { return noteCount; }

    int getNumMarks() const noexcept { return numMarks; }
    /// Mark i, 0 the oldest kept, getNumMarks() - 1 the newest.
    const Mark& getMark (int i) const noexcept { return marks[(size_t) ((firstMark + i) % maxMarks)]; }
    const Mark& newestMark() const noexcept { return getMark (numMarks - 1); }

    /// The kept mark nearest to `time` (they are in time order). Only call with getNumMarks() > 0.
    const Mark& nearestMark (double time) const noexcept;

    /// The last readings, for tests.
    PitchDetector::Estimate getLastEstimate() const noexcept { return lastEstimate; }

private:
    void onEstimate (const PitchDetector::Estimate& e, const PitchShifterInput& input) noexcept;
    void anchor (double newPeriod, const PitchShifterInput& input) noexcept;
    void trackNextMark (const PitchShifterInput& input) noexcept;
    void addMark (const Mark& m) noexcept;

    double sampleRate = 48000.0;
    PitchDetector detector;
    double overridePeriod = 0.0;
    int hopCounter = 0;

    PitchDetector::Estimate lastEstimate;
    double previousConfident = 0.0; // Hz of the last confident reading, 0 if the last one wasn't
    int unconfident = 0;

    bool tracking = false, lost = false;
    double period = 0.0;
    int noteCount = 0;

    std::array<Mark, maxMarks> marks {};
    int firstMark = 0, numMarks = 0;

    double confidence = 0.0, confidenceStep = 1.0 / 96.0;
};

/// One PSOLA synthesis voice. Grains are Hann-windowed segments of the input centred on analysis marks,
/// laid down at synthesis marks P/r apart, so the output repeats every P/r: pitch r f, with the input's
/// spectral envelope (its formants, here the amp and cab's colour) kept. For r < 1 grains are skipped, for
/// r > 1 repeated; whole periods either way.
///
/// - Each grain is two periods long for r <= 1 (half-length P, as Moulines and Charpentier) and two
///   synthesis periods (half-length P/r) for r > 1. With two analysis periods at r > 1, an octave up of a
///   sine cancels to silence: the grains, laid half a period apart, alternate in sign (measured -273 dB);
///   with two synthesis periods they overlap by exactly half, sum to a constant window, and the same sine
///   comes out 6 dB down, a harmonic tone within 0.3 to 1.6 dB (prototypes/pitch_shifter.py --study psola,
///   and the tests). Bright tones lose a little more two octaves up (3 to 6 dB, against 1 dB with the
///   standard window), which is the price of never going silent.
///   A window shorter than two periods doesn't average a periodic signal to zero, though (the Hann of
///   two periods has a spectral zero at f0; a shorter one doesn't), so each such grain would add the same
///   offset, a DC as big as the fundamental (measured with marks on the waveform's peaks: 35% of an octave
///   up's energy on a harmonic tone, 78% on a sine). Each short grain subtracts its Hann-weighted mean, taken over the same window one
///   period earlier (already in the history when the grain starts; equal to its own for a periodic signal).
///   The windows sum to exactly 1 at r > 1, so the subtracted means add up to a constant that cancels the
///   DC and nothing else.
/// - Level: for r < 1, grains two periods long laid more than a period apart don't sum to a constant, so
///   the output's power falls with r. Each grain is scaled by 1/sqrt(M(r)), M the mean square of the sum of
///   the windows, in closed form: with h the half-length and s = h/r the spacing,
///       M = r (3/4 + 2 C/h),  C/h = (1/4)[(2 - s/h)(1 + cos(pi s/h)/2) + (3/(2 pi)) sin(pi s/h)]
///   for s <= 2h (overlapping neighbours), and M = 3r/4 beyond. M(1) = 1, M(1/2) = 3/8, M(1/4) = 3/16.
///   For r > 1 the windows sum to exactly 1, so no gain is needed.
/// - The grain for a synthesis mark s uses the kept analysis mark nearest to s - lag (lag = the voice's
///   delay). With no delay that is the newest mark, which makes the trail (s minus the mark) 1.4 to 1.8
///   periods of the note for r <= 1 (half a grain, plus the analysis' 1/8 period, plus up to a period
///   between marks): measured 5.2 ms on the high E, 13.1 ms on the open A, 17.1 ms on the low E, on top of
///   the detection time (9 to 25 ms from the high E to the low E at the 80 Hz floor).
/// - The ratio glides in the log domain over glideMs (taken at each grain), and the voice fades in and out
///   over fadeMs when `active` changes (the harmonizer's onsets; the multivoicer leaves it on).
class PsolaVoice
{
public:
    static constexpr double minRatio = 0.25, maxRatio = 4.0;
    static constexpr int maxGrains = 8;

    struct Settings
    {
        double ratio = 1.0;
        double delayMs = 0.0; // the lag: grains come from the mark nearest to (synthesis time - this)
        double glideMs = 30.0;
        double fadeMs = 2.0;
        bool active = true;
    };

    /// 1/sqrt(M(r)): the grain gain that holds the level for r < 1 (1 for r >= 1).
    static double levelCompensation (double ratio) noexcept;

    void prepare (double sampleRate);
    void reset() noexcept;
    void setSettings (const Settings& settings) noexcept;

    /// One output sample, read from `input` before its push() of this sample, using `analysis` as it stood
    /// after the previous sample. ratioScale and extraDelaySamples as in GranularVoice::process().
    float process (const PsolaAnalysis& analysis, const PitchShifterInput& input, double ratioScale = 1.0, double extraDelaySamples = 0.0) noexcept;

    // For tests and meters.
    double getLastGrainDelay() const noexcept { return lastDelay; }
    int getGrainCount() const noexcept { return launched; }
    double getFade() const noexcept { return fade.getCurrentValue(); }

private:
    struct Grain
    {
        double centre = 0.0, half = 1.0, delay = 0.0, gain = 1.0, offset = 0.0;
        bool active = false;
    };

    /// The Hann-weighted mean of the input over the window of half-length `half` centred one period before
    /// the mark (whole samples): the offset a grain shorter than two periods carries.
    static double windowedMean (const PitchShifterInput& input, double centre, double half) noexcept;

    double sampleRate = 48000.0;
    Settings settings;
    RatioGlide ratio;
    juce::SmoothedValue<double> fade { 1.0 };
    std::array<Grain, maxGrains> grains {};
    double nextCentre = -1.0e18;
    int noteCount = -1;
    double lastDelay = 0.0;
    int launched = 0;
};

} // namespace ampsim

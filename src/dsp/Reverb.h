#pragma once

#include "Block.h"
#include "CutFilter.h"
#include "DelayLine.h"
#include "Lfo.h"
#include "Svf.h"

#include <array>
#include <vector>

namespace ampsim
{

/// A TPT SVF (Svf.h) whose coefficients glide to each new design instead of jumping. Knob-driven
/// filters are redesigned every 32 samples (foundation rules), and in a one-pass filter the small jump
/// at each redesign is harmless. Inside a reverb tank the signal recirculates, so a train of jumps
/// leaves broadband splatter (measured before this: about -73 dB re the signal under a fast decay
/// sweep on the plate). Here every coefficient moves linearly over the interval, so the filter never
/// steps; each intermediate set lies between two nearby stable designs, and the last step lands
/// exactly on the target, so a flat shelf ends up exactly flat (freeze relies on that).
class GlidingSvf
{
public:
    void reset() noexcept { filter.reset(); }

    void snapTo (const Svf::Coefficients& c) noexcept
    {
        current = target = c;
        remaining = 0;
        filter.setCoefficients (c);
    }

    void glideTo (const Svf::Coefficients& c, int samples) noexcept
    {
        target = c;
        remaining = samples;
        const auto n = (double) samples;
        step.g = (c.g - current.g) / n;
        step.k = (c.k - current.k) / n;
        step.a1 = (c.a1 - current.a1) / n;
        step.a2 = (c.a2 - current.a2) / n;
        step.a3 = (c.a3 - current.a3) / n;
        step.m0 = (c.m0 - current.m0) / n;
        step.m1 = (c.m1 - current.m1) / n;
        step.m2 = (c.m2 - current.m2) / n;
    }

    double processSample (double x) noexcept
    {
        if (remaining > 0)
        {
            if (--remaining == 0)
            {
                current = target;
            }
            else
            {
                current.g += step.g;
                current.k += step.k;
                current.a1 += step.a1;
                current.a2 += step.a2;
                current.a3 += step.a3;
                current.m0 += step.m0;
                current.m1 += step.m1;
                current.m2 += step.m2;
            }
            filter.setCoefficients (current);
        }
        return filter.processSample (x);
    }

private:
    Svf filter;
    Svf::Coefficients current, target, step;
    int remaining = 0;
};

/// The half-cosine fade shape, 0 at x = 0 to 1 at x = 1 with zero slope at both ends: a linear ramp's
/// corners splatter a little broadband energy (about -78 dB under a tone for a 30 ms fade); this has
/// no corners.
inline float smoothFade (float x) noexcept
{
    return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * x);
}

/// A delay length (or a scale of delay lengths) gliding to a new target: a one-pole approach,
///     speed = (target - value) / (tau fs), capped at maxSpeed, value += speed,
/// landing exactly on the target once within snapDistance. A delay that changes at speed v plays its
/// contents at rate 1 - v (a Doppler pitch shift), so the speed must never jump: a linear ramp starts
/// and stops at full speed, an instant pitch jump of the whole tail (measured: -77 dB of broadband
/// splatter re the signal at the end of a fast Size sweep). Here the speed is proportional to the
/// remaining distance, so it changes continuously, even when the target moves every buffer, and the
/// cap limits the pitch shift of a big jump.
class Glide
{
public:
    void prepare (double timeConstantSeconds, double sampleRate, double maxSpeedPerSample, double snapWithin)
    {
        coefficient = 1.0 - std::exp (-1.0 / (timeConstantSeconds * sampleRate));
        maxSpeed = maxSpeedPerSample;
        snapDistance = snapWithin;
    }

    void setTarget (double newTarget) noexcept { target = newTarget; }
    void snap() noexcept { value = target; }
    double getTarget() const noexcept { return target; }
    double getValue() const noexcept { return value; }
    bool isMoving() const noexcept { return std::abs (target - value) > 0.0; }

    double next() noexcept
    {
        const auto distance = target - value;
        if (std::abs (distance) <= snapDistance)
            value = target;
        else
            value += juce::jlimit (-maxSpeed, maxSpeed, distance * coefficient);
        return value;
    }

private:
    double value = 0.0, target = 0.0, coefficient = 1.0, maxSpeed = 1.0, snapDistance = 0.0;
};

// =================================================================================================
// Plate engine
// =================================================================================================

/// Jon Dattorro's plate reverberator: "Effect Design, Part 1: Reverberator and Other Filters", JAES
/// 45(9), 1997, Fig. 1, Table 1 (parameters), Table 2 (output taps). Built straight from the figure:
///
///   x = (xL + xR) / 2 ─► predelay ─► bandwidth one-pole ─► 4 input diffusers (142, 107, 379, 277)
///     ─► the tank, a figure eight of two halves, each fed the diffused input plus the other half's
///        output times decay:
///        left:  diffuser 672 (modulated, signs reversed) ─► z^-4453 ─► damping one-pole
///               (─► our shelves) ─► x decay ─► diffuser 1800 ─► z^-3720 ─► x decay ─► right half
///        right: diffuser 908 (modulated, signs reversed) ─► z^-4217 ─► damping (─► our shelves)
///               ─► x decay ─► diffuser 2656 ─► z^-3163 ─► x decay ─► left half
///   yL, yR = seven taps each at +-0.6 on the tank's delay lines (Table 2), all wet.
///
/// Every diffuser is Dattorro's two-multiplier lattice allpass (lattice() and latticeReversed() in
/// Reverb.cpp). The paper's lengths are samples at 29761 Hz; here every length and tap is scaled by
/// fs / 29761 and rounded, so the reverb keeps the paper's timing at 48 kHz. Table 1's coefficients are
/// used as they are: bandwidth 0.9995 and damping 0.0005 are one-pole coefficients, which would depend
/// on the rate, but at those values both filters are within 0.01 dB of flat at any rate.
///
/// One addition, ours: a 300 Hz low shelf and a 4 kHz high shelf after each damping filter, so the
/// plate's three decay bands work like the FDN's (Reverb::plateParameters). At 0 dB they pass the
/// signal through bit for bit, which recovers the paper.
///
/// Modulation (paper section 1.3.7): the read taps of the two "decay diffusion 1" lines move with a
/// pair of sine LFOs in quadrature, D(n) = N + excursion x sin(...), read with Hermite interpolation.
/// Table 1's EXCURSION = 16 samples is the peak; the paper suggests about 8 at about 1 Hz.
class DattorroPlate
{
public:
    static constexpr double paperSampleRate = 29761.0;

    /// Table 1's control parameters, plus the modulation rate and our shelves.
    struct Parameters
    {
        double decay = 0.50;            // the four "decay" multipliers
        double decayDiffusion1 = 0.70;  // tank lattices 672 and 908 (signs reversed, "note sign")
        double inputDiffusion1 = 0.750; // input lattices 142 and 107
        double inputDiffusion2 = 0.625; // input lattices 379 and 277
        double bandwidth = 0.9995;      // input one-pole: y = bandwidth x + (1 - bandwidth) y[n-1]
        double damping = 0.0005;        // tank one-poles: y = (1 - damping) x + damping y[n-1]
        double excursion = 16.0;        // peak tap excursion, samples at 29761 Hz
        double modRateHz = 1.0;         // quadrature sine LFOs
        double lowShelfDb = 0.0;        // ours, per half; 0 = the paper's tank
        double highShelfDb = 0.0;       // ours, per half; 0 = the paper's tank
    };

    /// Table 1: decay diffusion 2 = decay + 0.15, floored at 0.25 and capped at 0.50.
    static double decayDiffusion2 (double decay) { return juce::jlimit (0.25, 0.50, decay + 0.15); }

    /// The paper's length n (samples at 29761 Hz) at sampleRate, rounded to the nearest sample.
    static int scaledLength (int paperSamples, double sampleRate);

    void prepare (double sampleRate);
    void reset();

    /// Audio thread. New targets: the coefficients glide there over 50 ms.
    void setParameters (const Parameters& parameters);

    /// Jumps to the targets with no glide (used when the engine starts from silence).
    void snapToTargets();

    /// Audio thread. input is the mono plate input (already (xL + xR) / 2 and predelayed).
    void process (const float* input, float* left, float* right, int numSamples) noexcept;

    /// Seconds for one trip around the whole figure eight (all eight tank delays): the time over which
    /// the signal meets the decay multiplier four times.
    double loopSeconds() const noexcept { return loopSamples / sampleRate; }

    /// The tank's stored energy: every sample a delay or a lattice holds. A lattice's state counts with
    /// weight (1 - g^2), the quantity it conserves (x^2 - y^2 = (1 - g^2)(w^2 - w[n-N]^2)). Tests use it
    /// to show freeze holds the tail exactly.
    double storedEnergy() const;

private:
    struct Smoothed
    {
        juce::SmoothedValue<double> decay { 0.5 }, decayDiffusion1 { 0.7 }, inputDiffusion1 { 0.75 },
            inputDiffusion2 { 0.625 }, bandwidth { 0.9995 }, damping { 0.0005 }, excursion { 16.0 }, lowShelfDb { 0.0 },
            highShelfDb { 0.0 };
    };

    void updateShelves (bool glide);

    double sampleRate = 48000.0;
    Parameters target;
    Smoothed smoothed;
    int samplesUntilShelfUpdate = 0;

    // Lengths at the running rate.
    std::array<int, 4> inputLength {};          // 142, 107, 379, 277
    int leftAllpass1 = 0, leftDelay1 = 0, leftAllpass2 = 0, leftDelay2 = 0;     // 672, 4453, 1800, 3720
    int rightAllpass1 = 0, rightDelay1 = 0, rightAllpass2 = 0, rightDelay2 = 0; // 908, 4217, 2656, 3163
    std::array<int, 7> tapsLeft {}, tapsRight {};
    double loopSamples = 1.0;

    std::array<DelayLine, 4> inputLines;
    DelayLine leftAllpass1Line, leftDelay1Line, leftAllpass2Line, leftDelay2Line;
    DelayLine rightAllpass1Line, rightDelay1Line, rightAllpass2Line, rightDelay2Line;
    Lfo lfoLeft { 11 }, lfoRight { 12 };

    double bandwidthState = 0.0, dampLeft = 0.0, dampRight = 0.0;
    GlidingSvf lowShelfLeft, lowShelfRight, highShelfLeft, highShelfRight;
};

// =================================================================================================
// Room and Hall engine
// =================================================================================================

/// A feedback delay network after Jean-Marc Jot and Antoine Chaigne, "Digital Delay Networks for
/// Designing Artificial Reverberators" (AES 90th Convention, 1991), with the decay filters of Jot,
/// "An Analysis/Synthesis Approach to Real-Time Artificial Reverberation" (ICASSP 1992). Room uses 8
/// lines, Hall 16.
///
///   in L/R ─► early reflections (a 10-tap delay per side, pattern scaled by Size) ─► early out
///                │
///                └─► 4 series allpass diffusers per side ─► left feeds the even lines, right the odd
///   lines: delay L_i ─► decay filter h_i ─► feedback matrix ─► back into the lines; late out = two
///          different +-1 combinations of the filtered line outputs
///
/// Lines: N lengths spread geometrically over 30 to 100 ms at Size scale 1, each moved to the nearest
/// prime (all different), so no two lines share a factor and their echoes never line up (mutually
/// prime lengths keep the modes from piling up on common frequencies). Size changes glide every length
/// to its new prime (Glide: a one-pole approach, 80 ms time constant, at most 0.25 samples per sample),
/// read with Hermite interpolation: a pitch bend whose speed never jumps, and never a step. The early
/// pattern glides with them.
///
/// Feedback matrix: the Householder reflection A = I - (2/N) 1 1^T, which costs O(N) (y = x - (2/N)
/// sum(x)) and is orthogonal (A A^T = I), so the matrix itself neither adds nor removes energy and
/// the decay is set by the filters alone. For N = 16 its diagonal is 0.875: on its own each line would
/// mostly feed itself and ring as a flutter echo at its own period. So the mixed vector is also
/// rotated by one line (line i's output feeds line i + 1), a permutation, which keeps the matrix
/// orthogonal and O(N) but sends that dominant path around all N lines.
///
/// Decay (Jot 1992): every line i gets gain and filters whose dB response is proportional to its
/// length, h_i(f) [dB] = -L_i x 60 / (fs T60(f)), so every path through the network loses the same dB
/// per second and all modes decay at T60(f). Here T60(f) has three bands: a broadband gain for the mid
/// band, a low shelf (300 Hz) for the low multiplier, and a high shelf (4 kHz) for the high multiplier,
/// both TPT SVF shelves (Svf.h, Q 0.707, monotonic) with double state:
///     g_i      = 10^(-3 L_i / (fs T60_mid))                (the mid gain, linear)
///     low_i    = -60 L_i / fs (1/T60_low - 1/T60_mid) dB  (relative to mid)
///     high_i   = -60 L_i / fs (1/T60_high - 1/T60_mid) dB
/// "Below 300 Hz" and "above 4 kHz" mean well below and above: these shelves reach 99.3% of their dB
/// gain at 88 Hz and at 11.3 kHz (95.5% at 8 kHz), and the tests measure the low band at 44 to 88 Hz
/// and the high band at 8 to 16 kHz. While a knob moves, the gains and shelves are redesigned every 32
/// samples and glide in between (GlidingSvf).
/// Freeze sets all three rates to zero: unity gain, flat shelves (an exact pass-through), and with the
/// modulation also faded to zero the network is lossless and holds its energy indefinitely.
///
/// The late output is normalized for the total line length (setParameters()), so Size doesn't change
/// the loudness.
///
/// Modulation: four lines' lengths wander by up to +-0.5 ms (depth) with the smoothed random LFO
/// (Lfo.h), at slightly different rates, read with Hermite interpolation. That smears the modes so
/// long tails don't ring metallically. Hermite's gain never exceeds 1 (checked numerically: max
/// |H| = 1.000), so it can't destabilize the loop; it does lose a little treble (about 0.1 dB per pass
/// at 8 kHz), which shortens the high band's decay slightly when modulation is on.
class FeedbackDelayNetwork
{
public:
    static constexpr int maxLines = 16;
    static constexpr int numModulatedLines = 4;
    static constexpr int numDiffusers = 4;   // per side
    static constexpr int numEarlyTaps = 10;  // per side
    static constexpr double shortestLineMs = 30.0, longestLineMs = 100.0; // at Size scale 1
    static constexpr double maxSizeScale = 2.0;
    static constexpr double maxExcursionMs = 0.5;
    static constexpr double lengthGlideSeconds = 0.08; // Glide time constant for Size changes
    static constexpr double maxGlideSpeed = 0.25;      // samples per sample: at most a 25% pitch shift
    static constexpr double rateSmoothingSeconds = 0.05;

    struct Parameters
    {
        double sizeScale = 1.0;                              // line lengths and early pattern
        double rateMid = 30.0, rateLow = 30.0, rateHigh = 60.0; // decay rates, dB per second (60 / T60); 0 holds
        double diffusion = 0.8;                              // allpass coefficients 0.75 d and 0.625 d
        double modDepth = 0.3;                               // 0..1 of +-0.5 ms
        double modRateHz = 0.5;
        double earlyGain = 1.0, lateGain = 1.0;
    };

    explicit FeedbackDelayNetwork (int numLines);

    int getNumLines() const noexcept { return numLines; }

    /// The feedback matrix, applied in place to x[0..n): y = x - (2/n) sum(x), then rotated one place
    /// (y[i] goes to line i + 1). Orthogonal; the tests check M M^T = I.
    static void feedbackMatrix (double* x, int n) noexcept;

    /// The line lengths (samples) for a Size scale: the nearest distinct primes to the base lengths
    /// times the scale. Allocation-free once prepared.
    void lengthsFor (double sizeScale, std::array<int, maxLines>& out) const;

    void prepare (double sampleRate);
    void reset();
    void setParameters (const Parameters& parameters);
    void snapToTargets();
    void process (const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples) noexcept;

    /// The energy held in the lines: the sum of squares of the last L_i samples of every line, the
    /// quantity an orthogonal matrix conserves. For the freeze test.
    double storedEnergy() const;

    /// The current (target) length of line i in samples.
    int lineLength (int i) const noexcept { return targetLengths[(size_t) i]; }

private:
    void updateDecayFilters (bool glide);

    const int numLines;
    double sampleRate = 48000.0;
    Parameters target;
    bool prepared = false;

    std::array<double, maxLines> baseLengths {}; // samples at scale 1 (not yet prime)
    std::vector<int> primes;                     // every prime up to the longest possible line
    std::array<int, maxLines> targetLengths {};
    double lengthsDesignedFor = -1.0;
    double lateNormalization = 1.0; // sqrt(total line length / fs), see setParameters()

    std::array<DelayLine, maxLines> lines;
    std::array<Glide, maxLines> lengths;
    std::array<GlidingSvf, maxLines> lowShelves, highShelves;
    std::array<double, maxLines> gains {}, gainSteps {}, gainTargets {}; // the mid gains glide like the shelves
    int glideRemaining = 0;
    std::array<double, maxLines> injectLeft {}, injectRight {}, tapLeft {}, tapRight {};
    std::array<int, numModulatedLines> modulatedLine {};
    std::array<Lfo, numModulatedLines> lfos;
    juce::SmoothedValue<double> rateMid { 30.0 }, rateLow { 30.0 }, rateHigh { 60.0 }, excursion { 0.0 };
    int samplesUntilFilterUpdate = 0;

    // Early reflections and diffusion.
    std::array<DelayLine, 2> earlyLines;
    std::array<std::array<double, numEarlyTaps>, 2> earlyTapMs {}, earlyTapGain {};
    Glide earlyScale; // the Size scale of the early pattern, gliding like the lines
    std::array<std::array<DelayLine, numDiffusers>, 2> diffusers;
    std::array<std::array<int, numDiffusers>, 2> diffuserLength {};
    juce::SmoothedValue<double> diffusion { 0.8 }, earlyGain { 1.0 }, lateGain { 1.0 };
};

// =================================================================================================
// The block
// =================================================================================================

/// The reverb (BUILD_PLAN "Reverb", design review round 8): a stereo post-FX block with three engines,
/// Room and Hall (FeedbackDelayNetwork with 8 and 16 lines) and Plate (DattorroPlate).
///
///   in ─► x input gain (fades to 0 for spillover bypass and freeze) ─► pre-delay ─► engine(s)
///      ─► width (mid/side) ─► wet low cut and high cut ─► ducking ─► x wet gain ─┐
///   in ─────────────────────────────────────────────────────────────► x dry gain ─┴─► out
///
/// Mix law: wet and dry are decorrelated (a reverb tail shares almost nothing sample by sample with
/// its input), so their powers add, and an equal-power crossfade keeps the loudness steady across the
/// knob: dry gain cos(mix pi/2), wet gain sin(mix pi/2), dry^2 + wet^2 = 1. Both gains are smoothed
/// over 20 ms. The dry path is never delayed (zero latency).
///
/// Spillover (BUILD_PLAN "Bypass and spillover"): the block handles its own bypass. Bypassed, the
/// reverb's input fades out over 10 ms and the dry fades to unity, while whatever is already inside
/// (pre-delay, early reflections, the tail) keeps ringing on top. Re-enabled, the input fades back in;
/// the tail is never reset. Once bypassed and silent (the wet output below -120 dBFS for a whole
/// second, longer than any path through the reverb), the block skips its engines entirely and leaves
/// the dry untouched, until it's switched back on.
///
/// Engine switch: each engine has a position p that ramps linearly to 1 (selected) or 0 over 30 ms,
/// shaped by the half cosine s = smoothFade(p). An engine runs while p > 0; its input is scaled by s
/// (so a newly started engine's input fades in rather than starting with a step) and its output by
/// sin(s pi/2), an equal-power crossfade (the outgoing engine gets cos), since two engines' tails are
/// uncorrelated. A starting engine is cleared first, so it never replays an old tail, and an engine
/// that has faded out stops running.
///
/// Freeze (footswitchable): the input fades out (as for bypass) and every decay rate glides to zero
/// over 50 ms, with the modulation depth: unity gains, flat filters, integer-sample reads, so the
/// FDN and the plate both become lossless and hold the tail indefinitely. What's already in the
/// pre-delay still reaches the tank, so a note played just before the stomp is held too.
///
/// Ducking (off by default): a peak envelope of the input (10 ms attack, 250 ms release) lowers the
/// wet by up to 18 dB x ducking, fully above -30 dBFS and not at all below -50 dBFS, so the tail
/// blooms in the gaps.
///
/// Denormals: reverb tails are the main denormal risk in the project. The processor flushes them to
/// zero (juce::ScopedNoDenormals), so a dying tail's float delay lines and double filter states reach
/// exact zero instead of crawling through subnormal numbers; the tests check there's no CPU spike as
/// a tail decays to silence.
class Reverb : public Block
{
public:
    enum class Engine
    {
        room,
        hall,
        plate
    };

    static constexpr int numEngines = 3;

    struct Settings
    {
        Engine engine = Engine::hall;
        float mix = 0.25f;                 // 0 dry .. 1 wet, equal power
        float preDelayMs = 10.0f;          // 0 .. 500, wet only
        float size = 0.5f;                 // 0 .. 1 (Room and Hall): line scale 2^(3 size - 2)
        float decaySeconds = 2.0f;         // T60 of the mid band, 0.1 .. 30
        float lowDecayMultiplier = 1.2f;   // T60 below 300 Hz = decay x this, 0.25 .. 4
        float highDecayMultiplier = 0.5f;  // T60 above 4 kHz = decay x this, 0.1 .. 2
        float diffusion = 0.8f;            // 0 .. 1: input allpasses 0.75 x and 0.625 x this (1 = Dattorro's)
        float modDepth = 0.3f;             // 0 .. 1 (FDN +-0.5 ms, plate +-16 samples at 29761 Hz)
        float modRateHz = 0.5f;            // 0.05 .. 5
        float width = 1.0f;                // 0 mono .. 1 full
        float earlyLate = 0.5f;            // 0 early only, 0.5 both, 1 late only (Room and Hall)
        float lowCutHz = 100.0f;           // wet low cut, 12 dB/oct; 20 or below is off
        float highCutHz = 8000.0f;         // wet high cut, 12 dB/oct; 20000 or above is off
        float ducking = 0.0f;              // 0 off .. 1 (18 dB)
        bool freeze = false;
    };

    static constexpr double lowCrossoverHz = 300.0;
    static constexpr double highCrossoverHz = 4000.0;
    static constexpr double maxPreDelayMs = 500.0;
    static constexpr double mixSmoothingSeconds = 0.020;
    static constexpr double inputFadeSeconds = 0.010;
    static constexpr double engineFadeSeconds = 0.030;
    static constexpr double preDelayFadeSeconds = 0.030;
    static constexpr double silenceThreshold = 1.0e-6;  // -120 dBFS
    static constexpr double silenceHoldSeconds = 1.0;
    static constexpr double duckRangeDb = 18.0;
    static constexpr double duckAttackMs = 10.0, duckReleaseMs = 250.0;
    static constexpr double duckFloorDb = -50.0, duckFullDb = -30.0;

    /// Output level of each engine, so they sit at the same wet loudness: steady white noise into a 2 s
    /// decay comes out of the late reverb at about the input's level on every engine (measured in the
    /// tests). The FDN's late output is also normalized for Size (FeedbackDelayNetwork::setParameters).
    /// The early reflections are normalized to unit energy, so the balance knob's centre adds them on top.
    static constexpr double fdnLateLevel = 3.0;
    static constexpr double plateLevel = 1.0;

    /// The Size knob's line-length multiplier: 2^(3 size - 2), 0.25x at 0, 1x (30 to 100 ms) at 2/3,
    /// 2x at 1.
    static double sizeScale (double size) { return std::pow (2.0, 3.0 * juce::jlimit (0.0, 1.0, size) - 2.0); }

    /// The plate's coefficients for the settings: the paper's Table 1 apart from these mappings.
    ///   decay: four multipliers per trip around the tank (loop time tau), so a T60 needs
    ///          20 log10(decay^4) = -60 tau / T60, decay = 10^(-0.75 tau / T60).
    ///   shelves: one low and one high shelf per half, two of each per trip, so each takes half the
    ///          band's extra loss: -(r_low - r_mid) tau / 2 and -(r_high - r_mid) tau / 2 dB, where
    ///          r = 60 / T60 is the decay rate in dB per second.
    ///   damping: 0 (Table 1's "no damping"). The one-pole can only hit the high band's T60 at one
    ///          frequency, and fitted at 4 kHz it drags the mid band down with it.
    static DattorroPlate::Parameters plateParameters (const Settings& settings, double loopSeconds);

    /// Decay rates (dB per second) for the settings: 60 / T60 per band, all zero in freeze.
    struct Rates
    {
        double mid, low, high;
    };
    static Rates decayRates (const Settings& settings);

    Reverb();

    /// Audio thread, once per buffer (or before prepare()).
    void setSettings (const Settings& settings);
    const Settings& getSettings() const noexcept { return settings; }

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }
    bool handlesOwnBypass() const override { return true; }
    void setBypassed (bool shouldBeBypassed) override;

    // ---- For tests -------------------------------------------------------------------------------
    /// Bypassed, silent, and skipping the engines.
    bool isIdle() const noexcept { return idle; }
    bool isSwitchingEngine() const noexcept;
    /// Whether an engine is running (selected, or fading out).
    bool isEngineRunning (Engine engine) const noexcept { return engines[(size_t) engine].running; }
    /// The selected engine's stored energy (FeedbackDelayNetwork / DattorroPlate::storedEnergy()).
    double storedEnergy() const;

private:
    struct EngineState
    {
        float position = 0.0f; // 0 = silent .. 1 = selected
        bool running = false;
    };

    void applySettings (bool snap);
    void startEngine (Engine engine);
    void runEngine (Engine engine, const float* inLeft, const float* inRight, float* outLeft, float* outRight, int numSamples);

    double sampleRate = 48000.0;
    int maxBlock = 0;
    bool prepared = false;
    Settings settings;

    FeedbackDelayNetwork room { 8 }, hall { 16 };
    DattorroPlate plate;
    std::array<EngineState, numEngines> engines;
    Engine selected = Engine::hall;

    // Input side.
    juce::SmoothedValue<float> inputGain { 1.0f };  // 0 while bypassed or frozen
    juce::SmoothedValue<float> bypassBlend { 0.0f }; // 1 = bypassed: dry at unity
    bool bypassed = false;
    std::array<DelayLine, 2> preDelay;
    // Pre-delay in samples. A change crossfades from the current read head to the target head; a
    // request that arrives mid-fade waits for the fade to finish, then starts the next one.
    int preDelayCurrent = 0, preDelayTarget = 0, preDelayRequested = 0;
    float preDelayFade = 0.0f; // 0 = current head .. 1 = target head
    bool preDelayFading = false;
    double duckEnvelope = 0.0, duckAttack = 0.0, duckRelease = 0.0;

    // Output side.
    juce::SmoothedValue<float> dryGain { 1.0f }, wetGain { 0.0f }, width { 1.0f }, ducking { 0.0f };
    CutFilter lowCut { CutFilter::Kind::lowCut }, highCut { CutFilter::Kind::highCut };

    // Silence detection for the spillover idle state.
    bool idle = false;
    int silentSamples = 0;

    // Scratch, sized in prepare().
    juce::AudioBuffer<float> pre, engineIn, engineOut, wet;
    std::vector<float> ramp, duckGains, monoScratch;
};

} // namespace ampsim

#pragma once

#include "Block.h"
#include "Lfo.h"

#include <array>
#include <cmath>

namespace ampsim
{

/// The phaser, one of Bloom's three effects (BUILD_PLAN "Bloom (modulation container)", design review round
/// 12). Stereo; zero latency. prototypes/vibe.py is its reference, sample for sample (golden tests).
///
/// Signal path, per channel:
///   1. A chain of first-order allpass stages, each a zero-delay-feedback TPT one-pole (Zavalishin, "The Art
///      of VA Filter Design", ch. 3). With g = tan(pi fc / fs) and G = g / (1 + g):
///          v = (u - s) G,  lp = v + s,  s <- lp + v,  out = 2 lp - u
///      the bilinear transform of H(s) = (1 - s/wc) / (1 + s/wc): unity gain at every frequency, a phase lag
///      of 2 atan(f / fc) that reaches 90 degrees at the corner fc (prewarped, so exact there). The state is
///      the integrator's, so sweeping fc every sample doesn't jolt the output.
///   2. Feedback (Modern, and Classic's later version): the chain's input is u0 = x + fb y, with y the last
///      stage's output. That's a delay-free loop, solved exactly instead of with a one-sample delay: each
///      stage's output is affine in its input, out = a u + b with a = 2G - 1 and b = 2 (1 - G) s, so the chain
///      gives y = A u0 + B (A the product of the a's, B carried along), and
///          u0 = (x + fb B) / (1 - fb A)
///      Every |a| < 1, so |fb A| < 1 and the division is always safe. The discrete loop is the bilinear
///      transform of the analog one, which the small-gain theorem makes stable for |fb| < 1 (|fb A| < 1 on the
///      whole jw axis), at any corner: stable at the maximum feedback, 0.9, however fast the sweep.
///   3. Wet: y sqrt(1 - fb^2). The loop's resonances raise the wet's average power to 1 / (1 - fb^2) (the
///      mean of the Poisson kernel 1 / |1 - fb e^(j phi)|^2 over the phase), so this holds the level while
///      feedback colours the sound. With no feedback the wet is the allpass chain itself: unity magnitude.
///   4. Mix, linear (the wet is the dry with its phase turned, so they're correlated): out = (1 - mix) x +
///      mix wet. At mix 1/2 with no feedback |out| = |cos(phi / 2)|: a total notch wherever the chain's phase
///      phi is an odd multiple of 180 degrees, so N matched stages give N/2 notches, at
///          f_k = (fs / pi) atan(g tan((2k + 1) pi / 2N)),   k = 0 .. N/2 - 1
///      (the analog fc tan((2k + 1) pi / 2N), prewarped: notchFrequency()). Mix 1 is pure phase modulation.
///
/// Sweep: one LFO for the block, its phase shared; the right channel reads it plus the stereo offset (0 to 180
/// degrees). The stages' corner frequencies, by mode:
///   Modern   any of 2, 4, 6, 8, or 12 stages, sine or triangle LFO, an exponential sweep (even in octaves)
///            across a set range of the corner, by default 100 Hz to 4 kHz:
///                fc = low (high / low)^p,   p = 1/2 + depth m / 2
///            and feedback from -0.9 to 0.9.
///   Classic  Phase 90-style: four matched stages whose resistors are JFETs, swept by a triangle. In the
///            ohmic region the square-law JFET's channel conductance is linear in the gate voltage above
///            pinch-off, g_ds = 2 beta (V_gs - V_p), so its resistance 1 / g_ds is hyperbolic in it: the JFET's
///            nonlinear resistance. The corner follows the conductance (plus a fixed resistor's), so a
///            linearly moving gate sweeps it linearly in Hz:
///                fc = 160 + (1600 - 160) v,   v = 1/2 + depth m / 2
///            which in octaves rushes through the bottom and lingers at the top (56% of the cycle in the top
///            octave, against 30% for an exponential sweep over the same range). Two notches, 66 Hz to 3.8 kHz
///            between them. Feedback off is the original pedal (script logo); on adds the later version's
///            (block logo) feedback resistor, as a loop gain of 0.35. Range and gain are estimates, not taken
///            from a schematic.
///   Vibe     Uni-Vibe-style: four stages with deliberately mismatched corners, all following one photocell
///            conductance, swept by a sine through a lamp (LampPhotocell below). The stage ratios are those of
///            the four capacitors usually quoted for the original, 15 nF, 220 nF, 470 pF, and 4.7 nF (not
///            checked against an original schematic): fc_i = r_i (400 + 3100 c) with r = (1, 15/220,
///            15000/470, 15/4.7), so the 15 nF stage spans 400 Hz to 3.5 kHz and the notches fall irregularly:
///            one sweeping the low mids (about 110 to 810 Hz), and a second near 5 to 7 kHz only while the lamp
///            is dim. No feedback. Mix 1 is the original's vibrato.
///
/// Smoothing: rate (multiplicatively), depth, and the range glide over 100 ms, the stereo offset over 100 ms,
/// feedback over 50 ms, mix over 20 ms. A change of mode, Modern's stage count, or Modern's LFO shape
/// crossfades over 30 ms to a second bank of stages that starts from rest (with a warm lamp) on the same LFO,
/// as the chorus does; switching back mid-fade reverses it, and a third choice waits for the fade to end. On/off
/// fades the effect's mix over 10 ms. Both fades follow an S-curve (Fade.h). Switched on from fully off, it
/// starts from clean state.
class Phaser : public Block
{
public:
    enum class Mode
    {
        classic,
        modern,
        vibe
    };

    static constexpr int maxStages = 12;
    static constexpr std::array<int, 5> stageChoices { 2, 4, 6, 8, 12 };
    static constexpr double minRateHz = 0.05, maxRateHz = 10.0;
    static constexpr double maxFeedback = 0.9;
    static constexpr double minRangeHz = 20.0, maxRangeHz = 20000.0;
    static constexpr double classicLowHz = 160.0, classicHighHz = 1600.0;
    static constexpr double classicBlockFeedback = 0.35;
    static constexpr std::array<double, 4> vibeStageRatios { 1.0, 15.0 / 220.0, 15000.0 / 470.0, 15.0 / 4.7 };
    static constexpr double vibeDarkHz = 400.0, vibeLightHz = 3500.0;

    static constexpr double modulationSmoothingSeconds = 0.100;
    static constexpr double feedbackSmoothingSeconds = 0.050;
    static constexpr double smoothingSeconds = 0.020;
    static constexpr double modeFadeSeconds = 0.030;
    static constexpr double onFadeSeconds = 0.010;

    struct Settings
    {
        bool on = false;
        Mode mode = Mode::classic;
        int stages = 4;                       // Modern: 2, 4, 6, 8, or 12 (others snap to the nearest)
        float rateHz = 0.5f;                  // 0.05 to 10
        float depth = 1.0f;                   // 0 to 1 of the sweep
        Lfo::Shape shape = Lfo::Shape::sine;  // Modern: sine or triangle (Classic is triangle, Vibe sine)
        float lowHz = 100.0f, highHz = 4000.0f; // Modern: the corner's sweep range
        float feedback = 0.0f;                // Modern: -0.9 to 0.9
        bool classicFeedback = false;         // Classic: the later (block logo) version's feedback
        float stereoOffset = 0.25f;           // the right LFO's lead in cycles, 0 to 0.5 (0 to 180 degrees)
        float mix = 0.5f;                     // 0 dry to 1 wet; 0.5 gives the deepest notches
    };

    /// What one bank of stages runs: a change of any of these crossfades to the other bank.
    struct Spec
    {
        Mode mode = Mode::classic;
        int stages = 4;
        Lfo::Shape shape = Lfo::Shape::triangle;

        bool operator== (const Spec& other) const noexcept
        {
            return mode == other.mode && stages == other.stages && shape == other.shape;
        }
        bool operator!= (const Spec& other) const noexcept { return ! (*this == other); }
    };

    /// The Vibe's lamp and photocell, one per channel: an LFO value m in [-1, 1] in, the photocell's
    /// normalized conductance c in [0, 1] out (prototypes/vibe.py has the derivation). Per sample:
    ///     drive    d = 0.25 + 0.75 depth (1 + m) / 2     a bias keeps the lamp glowing dimly
    ///     power    p = d^1.55                            tungsten's resistance rises with temperature, so
    ///                                                    P ~ V^1.55 (lamp rule of thumb; light ~ V^3.4)
    ///     filament theta += (p - theta)(1 - e^(-1 / (0.025 fs)))   25 ms of thermal lag
    ///     light    L = theta^(3.4 / 1.55)                light ~ V^3.4 = P^(3.4/1.55) in steady state
    ///     cell     c* = L^0.8                            CdS conductance ~ illuminance^0.8
    ///              c += (c* - c)(1 - e^(-1 / (tau fs)))  tau 4 ms while c* > c, 60 ms otherwise
    /// The photocell's resistance falls fast when the light rises and recovers slowly in the dark (the CdS
    /// "light memory" that opto compressors rely on for a fast attack and slow release; Vactrol datasheets
    /// give turn-on times of a few milliseconds and turn-off times of tens), so the sweep rises faster than
    /// it falls, more so at higher rates (rise takes 43% of the cycle at 1 Hz, 31% at 4 Hz), and its depth
    /// shrinks as the rate climbs: the throb. The time constants are estimates to tune by ear.
    class LampPhotocell
    {
    public:
        static constexpr double bias = 0.25;
        static constexpr double powerExponent = 1.55;
        static constexpr double lightExponent = 3.4 / 1.55;
        static constexpr double gamma = 0.8;
        static constexpr double lampSeconds = 0.025;
        static constexpr double onSeconds = 0.004, offSeconds = 0.060;

        static double drive (double m, double amount) noexcept
        {
            return juce::jlimit (0.0, 1.0, bias + (1.0 - bias) * amount * 0.5 * (1.0 + m));
        }

        void prepare (double hostRate) noexcept
        {
            thermal = 1.0 - std::exp (-1.0 / (lampSeconds * hostRate));
            on = 1.0 - std::exp (-1.0 / (onSeconds * hostRate));
            off = 1.0 - std::exp (-1.0 / (offSeconds * hostRate));
        }

        /// The steady state for a constant drive: a lamp that has been on (the state a bank starts from).
        void warmStart (double m, double amount) noexcept
        {
            theta = std::pow (drive (m, amount), powerExponent);
            c = std::pow (theta, lightExponent * gamma);
        }

        double next (double m, double amount) noexcept
        {
            const auto p = std::pow (drive (m, amount), powerExponent);
            theta += (p - theta) * thermal;
            const auto target = std::pow (theta, lightExponent * gamma);
            c += (target - c) * (target > c ? on : off);
            return c;
        }

        double getConductance() const noexcept { return c; }

    private:
        double thermal = 0.0, on = 0.0, off = 0.0;
        double theta = 0.0, c = 0.0;
    };

    static Spec specFor (const Settings& settings) noexcept;
    static int snapStages (int stages) noexcept;

    /// The stages' corner for an LFO value m in [-1, 1]: Modern and Classic (every stage alike), and the Vibe's
    /// 15 nF stage for a photocell conductance c (the others are vibeStageRatios times it).
    static double modernCorner (double lowHz, double highHz, double depth, double m) noexcept;
    static double classicCorner (double depth, double m) noexcept;
    static double vibeReferenceHz (double c) noexcept { return vibeDarkHz + (vibeLightHz - vibeDarkHz) * c; }

    /// Notch k (0 to stages/2 - 1) of N matched stages at corner fc, mixed 50/50 with no feedback.
    static double notchFrequency (int stages, int k, double cornerHz, double sampleRate) noexcept;

    /// The wet's level compensation for feedback fb: sqrt(1 - fb^2).
    static double feedbackCompensation (double fb) noexcept { return std::sqrt (1.0 - fb * fb); }

    /// Audio thread, once per buffer.
    void setSettings (const Settings& settings);

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::dsp::AudioBlock<float> block, const BlockContext& context) override;
    void reset() override;
    bool isStereo() const override { return true; }

    bool isFullyOff() const noexcept { return onGain.getCurrentValue() <= 0.0 && ! onGain.isSmoothing(); }

    /// For tests and meters: the selected bank's spec, whether a bank crossfade is running, and a stage's
    /// corner frequency at the last sample processed.
    Spec getActiveSpec() const noexcept { return banks[(size_t) selected].spec; }
    bool isSwitching() const noexcept;
    double getCornerHz (int channel, int stage) const noexcept { return banks[(size_t) selected].corners[(size_t) channel][(size_t) stage]; }

private:
    struct Bank
    {
        Spec spec;
        std::array<std::array<double, maxStages>, 2> state {};   // [channel][stage]: the integrators
        std::array<std::array<double, maxStages>, 2> corners {}; // [channel][stage]: the last corners (Hz)
        std::array<LampPhotocell, 2> lamps;
        juce::SmoothedValue<double> gain { 0.0 };
    };

    struct Sweep
    {
        double depth, low, high, modernFeedback, classicFeedback;
    };

    double runBank (Bank& bank, int channel, double channelPhase, double x, double fb, const Sweep& sweep) noexcept;
    double feedbackFor (const Bank& bank, const Sweep& sweep) const noexcept;
    void startBank (int index, const Spec& spec);
    void select (int index);
    void applyModeRequest();
    double channelPhase (int channel, double offset) const noexcept;
    void clearState();
    void jumpToTargets();
    static bool audible (const Bank& bank) noexcept { return bank.gain.getCurrentValue() > 0.0 || bank.gain.isSmoothing(); }

    double sampleRate = 48000.0;
    Settings settings;

    std::array<Bank, 2> banks;
    int selected = 0;
    double phase = 0.0; // the LFO's position in cycles, shared by both banks

    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> rate { 0.5 }, low { 100.0 }, high { 4000.0 };
    juce::SmoothedValue<double> depth { 1.0 }, offset { 0.25 }, modernFeedback { 0.0 }, classicFeedback { 0.0 };
    juce::SmoothedValue<double> mix { 0.5 }, onGain { 0.0 };
    bool wakePending = false;
};

} // namespace ampsim

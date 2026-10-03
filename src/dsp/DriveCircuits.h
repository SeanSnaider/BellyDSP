// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#pragma once

#include "Svf.h"

#include <array>
#include <cmath>
#include <complex>

namespace ampsim::drive
{

/// Physically informed pedal circuits for the drive engine (BUILD_PLAN "Boost and Overdrive"), after
/// David Yeh's approach (D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, physically-informed models of
/// distortion and overdrive guitar effects pedals", DAFx-07; Yeh's PhD thesis, Stanford 2009): split the
/// schematic where an op-amp or buffer output (a near-ideal voltage source) drives the next stage, so
/// the stages don't load each other, then
///   - the linear stages become filters whose transfer functions are derived from the component values
///     and discretized with the bilinear transform, and
///   - the nonlinear stages keep their circuit equations: each is reduced to one unknown per sample
///     (a diode or op-amp node voltage) and solved with the trapezoidal rule and Newton's method, which
///     is much more faithful than a static clipping curve because the capacitors around the diodes
///     (the "embedded low-pass" in Yeh's words) make the clipping frequency dependent.
///
/// Everything here runs at the oversampled rate, in volts, in double precision. The circuits are the
/// AC equivalents of the pedals: the 4.5 V bias rail is ground, so every node rests at 0 V.
///
/// The same equations (same parts, same diode and op-amp models) are simulated as complete netlists
/// by prototypes/circuits.py, a SPICE-style simulator at 16x, whose renders and AC analyses are the
/// golden references in tests/fixtures/drive. Component values and their sources are listed with each
/// circuit below and in that script.
///
/// Adding a mode means adding a Circuit subclass: the engine (DriveEngine) handles oversampling, the
/// tight pre-high-pass, mode crossfades, mix, and level for every circuit alike.

constexpr double thermalVoltage = 0.025865; // kT/q at 27 C, SPICE's nominal temperature (V)
constexpr double gmin = 1.0e-12;            // SPICE's minimum junction conductance (S)

/// +12 dBu RMS sine at 0 dBFS: the Scarlett Solo's instrument input at minimum gain (ASSUMPTIONS C9),
/// the same calibration the NAM slots use. 1.0 digital = 4.3611 V peak.
constexpr double defaultVoltsAtFullScale = 1.4142135623730951 * 0.7746 * 3.9810717055349722;

/// 1N914 / 1N4148 silicon switching diode: the Shockley parameters of the common SPICE model
/// (Is = 2.52 nA, N = 1.752); series resistance and capacitances are left out, as in the reference.
struct DiodeModel
{
    double saturationCurrent = 2.52e-9;
    double emissionCoefficient = 1.752;
};

/// Audio ("A", log) taper pot: (81^p - 1) / 80 of the full resistance at rotation p, which puts 10% of
/// the resistance at half rotation, the usual audio-taper midpoint.
double audioTaper (double position);

/// Solves  k v + a (2 Is sinh(v / vt) + 2 gmin v) = c  for v: the equation every antiparallel diode pair
/// here reduces to (k > 0, a > 0). The left side rises monotonically, so the root is unique, and since the
/// diode term has the sign of v the root lies between 0 and c / k. Newton from the previous sample's
/// value, on whichever of two equivalent forms is nearly linear at the current point (see solve()), with
/// that bracket as a safety net: about 3 iterations per sample at full drive, measured on a guitar DI.
class DiodePairSolver
{
public:
    explicit DiodePairSolver (DiodeModel model = {});

    double solve (double k, double a, double c, double guess) noexcept;

    /// d current / dv at 0: the pair's small-signal conductance at rest (both diodes with SPICE's gmin).
    double restConductance() const noexcept { return twoIs / vt + 2.0 * gmin; }

    long iterations = 0, solves = 0; // statistics for the tests

private:
    double twoIs, vt;
};

/// A first-order section from an analog prototype H(s) = (n0 + n1 s) / (d0 + d1 s), by the bilinear
/// transform s = c (1 - z^-1) / (1 + z^-1) with c = 2 / T (the trapezoidal rule, the same discretization
/// as the nonlinear stages). Transposed direct form II, double precision.
struct FirstOrderSection
{
    double b0 = 1.0, b1 = 0.0, a1 = 0.0, z1 = 0.0;

    void design (double n0, double n1, double d0, double d1, double c) noexcept;
    void reset() noexcept { z1 = 0.0; }
    double process (double x) noexcept
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y;
        return y;
    }
};

/// A second-order section, H(s) = (n0 + n1 s + n2 s^2) / (d0 + d1 s + d2 s^2), likewise (Yeh, Abel,
/// Smith, DAFx-07, eqs. 4-9).
struct SecondOrderSection
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;

    void design (double n0, double n1, double n2, double d0, double d1, double d2, double c) noexcept;
    void reset() noexcept { z1 = z2 = 0.0; }
    double process (double x) noexcept
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/// An analog biquad as a TPT state-variable filter (Svf.h), prewarped at its natural frequency. Used for
/// the knob-dependent linear stages, because the SVF's states stay valid while its coefficients move.
/// H = (n0 + n1 s + n2 s^2) / (d0 + d1 s + d2 s^2) with w0 = sqrt(d0 / d2), k = d1 / (d2 w0), and in the
/// normalized s' = s / w0 the SVF's band and low outputs are s' / D and 1 / D, D = s'^2 + k s' + 1, so
///     m0 = n2 / d2,  m1 = n1 / (d2 w0) - k m0,  m2 = n0 / (d2 w0^2) - m0.
Svf::Coefficients biquadToSvf (double n0, double n1, double n2, double d0, double d1, double d2, double sampleRate);

/// One pedal circuit at the oversampled rate: volts in, volts out.
class Circuit
{
public:
    virtual ~Circuit() = default;

    /// Sets the (oversampled) rate and redesigns everything. No allocation; the engine calls reset() after.
    virtual void setSampleRate (double sampleRate) = 0;
    virtual void reset() = 0;

    /// Knob positions, 0 to 1: drive (gain) and tone (1 = brightest). The engine calls this once per
    /// 32 base-rate samples with smoothed values.
    virtual void setControls (double drive, double tone) = 0;

    /// In place, at the rate given to setSampleRate().
    virtual void process (double* volts, int numSamples) noexcept = 0;

    /// The analog small-signal response at rest (diodes at their zero-bias conductance), from the same
    /// transfer functions the filters are designed from. For tests and plots.
    virtual std::complex<double> smallSignalResponse (double frequency) const = 0;

    /// Newton iterations per solve so far (all of this circuit's solvers).
    virtual double meanIterations() const = 0;
};

/// "Mid Drive": the Ibanez TS808 Tube Screamer. Component values from ElectroSmash's "Tube Screamer
/// circuit analysis" and Yeh, Abel, Smith (DAFx-07, Figs. 14 and 15):
///
///   Input buffer: R1 1k, C1 20 nF, R2 510k to bias, Q1 emitter follower (ideal):
///       H1(s) = s C1 R2 / (1 + s C1 (R1 + R2))                         15.6 Hz high-pass
///   Into IC1a: C2 1 uF, R5 10k to bias:  H2(s) = s C2 R5 / (1 + s C2 R5)   15.9 Hz
///   Clipping amplifier, IC1a (ideal op-amp, so v- = v+ = vp): the gain leg R4 4.7k + C3 47 nF carries
///       i = vp s C3 / (1 + s R4 C3)                                     720 Hz high-pass
///   and all of it flows through the feedback network: Rf = R6 51k + Drive (500k audio taper), C4 51 pF,
///   and D1/D2 (1N914, antiparallel). With V the voltage across that network (Yeh's eq. 22):
///       C4 dV/dt = i - V / Rf - 2 Is sinh(V / vt)           vout = vp + V
///   so the output is the clean input plus a clipped, high-passed copy: low frequencies (below 720 Hz)
///   pass at unity and never clip, which is the Tube Screamer's mid hump and tight bass. The trapezoidal
///   rule turns the ODE into one equation per sample (a = T / (2 C4), h = V[n-1] + a f[n-1]):
///       (1 + a / Rf) V + a 2 Is sinh(V / vt) = a i[n] + h          (DiodePairSolver)
///   and afterwards h = 2 V[n] - h, since a f[n] = V[n] - h.
///   Tone stage, IC1b: R7 1k and C5 0.22 uF low-pass, Ri 10k bias, Tone 20k (linear) between the inputs
///   (Rl = T 20k on the + side, Rr = (1 - T) 20k on the - side), wiper to ground through R8 220 and
///   C6 0.22 uF, feedback 1k. Ideal op-amp analysis (Yeh's eq. 24; derived again and checked against AC
///   analysis in prototypes/circuits.py):
///       H3(s) = (1 / (Rs Cs)) ((1 + K) s + wz) / ((s + wp)(s + wz) + X s)
///       wz = 1 / (Cz (Rz + Rl||Rr)),  wp = 1 / (Cs (Rs||Ri)),  K = Rf Rl / ((Rl + Rr)(Rz + Rl||Rr)),
///       X = (Rr / (Rl + Rr)) / ((Rz + Rl||Rr) Cs)
///   Output: C7 1 uF, 1k, Level 100k (at maximum; the block's Level knob is a gain after it), C8 0.1 uF
///   into the 510k bias of the output buffer Q3 (ideal), RB 100, C9 10 uF, RC 10k, 1M load:
///       H4(s) = s^2 C7 C8 R12 Rp / (1 + s (C7 (Rp + R11) + C8 R12 + C8 Rp) + s^2 C7 C8 (R12 (Rp + R11) + Rp R11))
///       H5(s) = s C9 Ro / (1 + s C9 (RB + Ro)),  Ro = RC || 1M
class MidDriveCircuit final : public Circuit
{
public:
    MidDriveCircuit();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void setControls (double drive, double tone) override;
    void process (double* volts, int numSamples) noexcept override;
    std::complex<double> smallSignalResponse (double frequency) const override;
    double meanIterations() const override;

    /// The feedback resistance R6 + Drive for a drive knob position.
    static double feedbackResistance (double drive);

private:
    struct ToneCoefficients
    {
        double n0, n1, d0, d1, d2; // H3 = (n0 + n1 s) / (d0 + d1 s + d2 s^2)
    };
    static ToneCoefficients toneStage (double tone);
    void designFixedSections();

    double sampleRate = 192000.0, a = 0.0;
    double drive = 0.5, tone = 0.5, rf = 0.0;
    FirstOrderSection inputHighPass1, inputHighPass2, gainLeg, outputHighPass;
    SecondOrderSection outputNetwork;
    Svf toneFilter;
    DiodePairSolver diodes;
    double v = 0.0, history = 0.0; // the feedback network's voltage and its trapezoidal history
};

/// "Distortion": the ProCo RAT. Component values from ElectroSmash's "ProCo RAT analysis" (parts list).
///
///   Input: C1 22 nF, R2 1M to bias, R3 1k, C2 1 nF:
///       Hin(s) = s C1 R2 / ((1 + s R3 C2)(1 + s C1 R2) + s C2 R2)
///   Gain stage, LM308 in a non-inverting amplifier: Distortion 100k (audio taper) with C4 100 pF across
///   it from the output to (-), and two gain legs from (-) to ground, R4 47 + C5 2.2 uF (1.5 kHz) and
///   R5 560 + C6 4.7 uF (60 Hz): up to 1 + 100k / (47 || 560) = 2305 (67 dB).
///   The LM308 isn't ideal, and its slowness is the RAT's character, so it's modeled the way its slew
///   rate and bandwidth arise: the input pair is a transconductor feeding the 30 pF compensation
///   capacitor as an integrator,
///       Cc dvo/dt = It tanh((vp - vm) / (2 VT)) - vo / Ro - Iclamp(vo)
///   (tanh is exact for a BJT differential pair), with It = SR Cc for the 0.3 V/us slew rate (ElectroSmash),
///   which gives GBW = It / (2 VT 2 pi Cc) = 0.92 MHz, Ro for the datasheet's 300 V/mV DC gain, and clamp
///   diodes stopping the output 1.5 V short of the 9 V rails (+-3 V). At high gain the closed-loop
///   bandwidth (GBW / gain, ~400 Hz at full drive) and slewing shape the distortion.
///   The feedback network is linear, so with trapezoidal companions for its capacitors, vm = alpha vo +
///   beta each sample, and the integrator becomes one monotonic equation in vo (Newton, bracketed).
///   Clipper and tone: C7 4.7 uF and R6 1k from the op-amp to D1/D2 (1N914) to ground, then R7 1.5k +
///   Filter (100k audio taper, clockwise darker) into C8 3.3 nF and R8 1M (the JFET's gate bias). The
///   tone network hangs on the diode node, so it's solved with it: the linear parts reduce to a Thevenin
///   source and the diode node gives  k vd + 2 Is sinh(vd / vt) = c  (DiodePairSolver).
///   Output: JFET source follower (ideal), C10 1 uF, Volume 100k (at maximum), C9 22 nF, 1M load:
///       Hout(s) = s^2 C10 C9 RL Rv / ((1 + s C9 RL)(1 + s C10 Rv) + s C9 Rv)
class DistortionCircuit final : public Circuit
{
public:
    DistortionCircuit();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void setControls (double drive, double tone) override;
    void process (double* volts, int numSamples) noexcept override;
    std::complex<double> smallSignalResponse (double frequency) const override;
    double meanIterations() const override;

    static double distortionResistance (double drive);
    static double filterResistance (double tone);

    // The LM308 macromodel (prototypes/circuits.py, LM308).
    static constexpr double compensation = 30.0e-12, slewRate = 0.3e6, openLoopGain = 3.0e5, saturation = 3.0;
    static constexpr double clampSaturationCurrent = 1.0e-14;

private:
    void designNetwork();
    double opAmpStage (double vp) noexcept;
    double clipperStage (double vo) noexcept;

    double sampleRate = 192000.0, T = 1.0 / 192000.0;
    double drive = 0.5, tone = 0.5;
    SecondOrderSection inputNetwork, outputNetwork;
    DiodePairSolver diodes;

    // Op-amp macromodel constants.
    double tailCurrent = 0.0, inputScale = 0.0, outputConductance = 0.0, clampVoltage = 0.0;
    double ac = 0.0; // T / (2 Cc)

    // Feedback network: companion conductances and history currents (J), see opAmpStage(). gLegC5 and
    // gLegC6 are the gain legs, R4 + C5 and R5 + C6, as single companions.
    double gFeedback = 0.0, gLegC5 = 0.0, gLegC6 = 0.0, gC4 = 0.0, gC5 = 0.0, gC6 = 0.0, sumM = 0.0;
    double jC4 = 0.0, jC5 = 0.0, jC6 = 0.0;
    double vo = 0.0, opAmpHistory = 0.0;
    long opAmpIterations = 0, opAmpSolves = 0;


    // Clipper and tone network.
    double gSeries7 = 0.0, gC7 = 0.0, gTone = 0.0, gC8 = 0.0, sumF = 0.0;
    double jC7 = 0.0, jC8 = 0.0, vd = 0.0;
};

/// A capacitor as its trapezoidal companion: the current from its first node to its second is
///     i = G v - J,   G = 2C / T,   v = v1 - v2,
/// and after each step J <- 2 G v - J (from i[n] = G (v[n] - v[n-1]) - i[n-1]). With a resistor R in
/// series the branch is one companion, G' = G / (1 + R G) and J' = J / (1 + R G), and the capacitor's own
/// voltage afterwards is (i + J) / G.
struct Companion
{
    double c = 0.0, r = 0.0;       // capacitance, series resistance
    double g = 0.0, gs = 0.0, j = 0.0;

    void design (double capacitance, double seriesResistance, double timeStep) noexcept
    {
        c = capacitance;
        r = seriesResistance;
        g = 2.0 * c / timeStep;
        gs = g / (1.0 + r * g);
    }
    double js() const noexcept { return j / (1.0 + r * g); }
    /// After the step: the branch carried current i, so the capacitor's voltage was (i + J) / G and
    /// J <- 2 G (i + J) / G - J = 2 i + J.
    void updateFromCurrent (double i) noexcept { j = 2.0 * i + j; }
    /// After the step, for a capacitor without series resistance: its voltage was v.
    void updateFromVoltage (double v) noexcept { j = 2.0 * g * v - j; }
    void reset() noexcept { j = 0.0; }
};

/// "Transparent": the Klon Centaur. Topology and values from ElectroSmash's "Klon Centaur Analysis"
/// (schematic and stage drawings); the 4.5 V bias is ground. The Gain pot (dual 100k, linear) is Drive,
/// the Treble pot (10k, linear) is Tone. Stages, each driven by an op-amp output, so they're solved one
/// after another each sample:
///
///   Input buffer U1A (ideal follower): R1 10k, C1 0.1 uF, R2 1M:  Hb(s) = s C1 R2 / (1 + s C1 (R1 + R2)).
///   Front network from the buffer (nodes a, p, g; companions for C3, C5, C16): C3 0.1 uF into a; from a,
///     R6 10k || C5 68 nF into the gain stage's (+) at p, which the Gain pot's first gang returns to the
///     bias through `upper` = Drive x 100k; and feed-forward network 1, R7 1.5k into C16 1 uF, then
///     R19 15k into the summing node: the clean, low-passed (106 Hz) path.
///   Gain stage U1B, a TL072 on 9 V, non-inverting: gain leg R11 15k || C7 82 nF, then R10 2k plus the
///     gang's other part `lower` = (1 - Drive) x 100k; feedback R12 422k || C8 390 pF. Up to 40 dB at
///     1 kHz, so at high gain it hits its rails: a static macromodel (prototypes/circuits.py, TL072),
///         It tanh((vp - vm) / 2VT) = vo / Ro + Is (e^((vo - Vc)/VT) - e^((-Vc - vo)/VT)) + 2 gmin vo,
///     DC gain 200 V/mV, clamps reaching +-3.0 V (1.5 V short of the 9 V rails) when they carry the input
///     stage's whole 200 uA. With the feedback network as companions, vm = alpha vo + beta, and that is
///     one monotonic equation in vo (Newton, bracketed).
///   Clipper and feed-forward network 2 (a ladder b - f - e - d): C9 1 uF + R13 1k from the gain stage
///     to d, where D2/D3 (1N34A germanium, antiparallel, each with its 7 ohm series resistance) clip to
///     the bias; C10 1 uF on to e, R16 47k into the summing node, and C11 2.2 nF + R15 22k across to f.
///     Network 2 comes from the buffer: R5 5.1k || C4 68 nF into b, R8 1.5k and C6 390 nF + R9 1k to the
///     bias, and the Gain pot's second gang from b to the bias with its wiper at f (`upper` above the
///     wiper, so turning the gain up turns this clean path down); f feeds the summing node through
///     R17 27k || (C12 27 nF + R18 12k). A linear chain around one diode node: Gaussian elimination
///     from b toward d leaves k vd + Id(vd) = c, and with the series resistance Rs that is
///     k' vj + Id(vj) = c',  k' = k / (1 + k Rs),  c' = c / (1 + k Rs)   (DiodePairSolver).
///   Summing amplifier U2A (inverting, on the charge pump's +16.2 / -8.6 V): the three paths' currents
///     into R20 392k || C13 820 pF, which also low-passes at 495 Hz.
///   Treble control U2B (inverting active shelf): R22 = R24 = 100k, C14 3.9 nF from the pot's wiper to
///     (-); the pot (10k) sits between R21 1.8k from the input (Ra = R21 + (1 - Tone) 10k) and R23 4.7k
///     from the output (Rb = R23 + Tone x 10k):
///         H(s) = -(Y / R22 + s C14 / Ra) / (Y / R24 + s C14 / Rb),   Y = 1/Ra + 1/Rb + s C14,
///     unity below about 400 Hz, from +17 dB to -7.5 dB above it.
///   Output: C15 4.7 uF + R25 560 into the Volume pot (10k, at maximum), R28 100k, the 1M load, and the
///     bypass line's bleed through R243 68k (from the buffer through C2 4.7 uF, R3 100k, R4 560), 42 dB
///     under the dry signal.
/// The U2 op-amps are ideal: their outputs stay under 10 V in every fixture (prototypes/circuits.py
/// prints them), inside the charge pump's rails.
class TransparentCircuit final : public Circuit
{
public:
    TransparentCircuit();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void setControls (double drive, double tone) override;
    void process (double* volts, int numSamples) noexcept override;
    std::complex<double> smallSignalResponse (double frequency) const override;
    double meanIterations() const override;

    // The TL072 macromodel and the 1N34A (prototypes/circuits.py, TL072 and DIODE_1N34A).
    static constexpr double openLoopGain = 2.0e5, inputStageCurrent = 200.0e-6, swing = 3.0;
    static constexpr double clampSaturationCurrent = 1.0e-14;
    static constexpr double diodeSaturationCurrent = 2.0e-7, diodeEmission = 1.3, diodeSeriesResistance = 7.0;

private:
    void designNetworks() noexcept;
    double gainStage (double vp) noexcept;

    double sampleRate = 192000.0, T = 1.0 / 192000.0;
    double drive = 0.5, tone = 0.5;
    FirstOrderSection inputBuffer;
    DiodePairSolver diodes;

    // Front network.
    Companion c3, c5, c16;
    double g6 = 0.0, aP = 0.0, aG = 0.0, kA = 0.0;
    // Gain stage: macromodel constants, feedback network.
    double gm = 0.0, outputConductance = 0.0, clampVoltage = 0.0, inputScale = 0.0;
    Companion c7, c8;
    double g11 = 0.0, g12 = 0.0, gH = 0.0, kM = 0.0, vo = 0.0;
    long opAmpIterations = 0, opAmpSolves = 0;
    // Clipper and feed-forward network 2.
    Companion c4, c6, c9, c10, c11, c12;
    double g4 = 0.0, gUpper = 0.0, gLower = 0.0, g17 = 0.0;
    double aB = 0.0, aF2 = 0.0, aE2 = 0.0, kD = 0.0, kJ = 0.0, vj = 0.0;
    // Summing amplifier, treble control, output.
    Companion c13, c14, c2, c15;
    double ra = 0.0, rb = 0.0, yT = 0.0, aX = 0.0, kO = 0.0;
};

/// "Fuzz": the Electro-Harmonix Big Muff Pi, American version 3 (1977, red and black), as ElectroSmash's
/// "Big Muff Pi Analysis" draws it (schematic, parts list, bias drawing; its designators, with Q4 the
/// input stage as on EHX's boards). Collector resistors 10k: Kit Rae's BigMuffPage notes them on some
/// V3s, against 15k on the most common one. Sustain (100k linear) is Drive, Tone (100k linear) is Tone.
/// Transistors 2N5088, Ebers-Moll in SPICE's transport form with Fairchild's Is, Bf, Br:
///     Ibe = Is (e^(vbe/VT) - 1) + gmin vbe,   Ibc = Is (e^(vbc/VT) - 1) + gmin vbc,
///     Ic = Ibe - Ibc (1 + 1/Br),   Ib = Ibe / Bf + Ibc / Br,
/// diodes 1N914 (DiodeModel). The circuit runs on its real 9 V supply with its real bias.
///
///   Q4, input booster: R2 39k and C1 1 uF from the input to the base, R14 47k base to ground, shunt
///     feedback R9 470k || C10 470 pF, R13 10k collector load, R22 100 emitter. Biased at 7.1 V (not half
///     the supply: R14 is low), so it cuts off (Ic to 0) above a few hundred mV of input and saturates
///     (both junctions forward) near 1 V.
///   Sustain: C4 1 uF into the pot (wiper toward the top as Drive rises) over R23 1k, the wiper through
///     C5 100 nF and R19 10k to Q3's base.
///   Q3 and Q2, clipping stages: the same shunt-feedback stage (R20 / R16 100k, R17 / R15 470k || C12 /
///     C11 470 pF, R18 / R11 10k, R21 / R10 150) with C6 / C7 1 uF in series with D3/D4 and D1/D2 from base
///     to collector: once the collector swings more than a diode drop from the base, the diodes close the
///     loop and clip it. C13 100 nF and R12 10k couple them.
///   Tone stack: R8 39k into C8 10 nF (low-pass) and C9 4 nF into R5 22k (high-pass), the pot between
///     them (wiper at the high-pass end for Tone 1), the wiper through C3 100 nF to Q1. Its DC path,
///     R8 + pot + R5 = 161k, loads Q2's collector; the wiper's DC voltage moves with the knob, and C3
///     passes the change to Q1, as on the real pedal (turning Tone while playing thumps a little).
///   Q1, output booster: R7 430k / R3 100k bias, R6 15k collector, R4 3.3k emitter (gain about 4.5);
///     C2 100 nF into the Volume pot (100k, at maximum) and the 1M load.
///
/// Solving it. Each transistor stage's own elements are trapezoidal companions around its nodes (base,
/// emitter, collector, and the diode node on the clipping stages: 3 or 4 unknowns). The networks between
/// stages (Sustain; C13 + R12; the tone stack) are linear, so their inner nodes are eliminated and each
/// becomes a 2-port between one stage's collector and the next stage's base. The stages load each other
/// noticeably (in the fixtures each base swings 0.5 to 7% of the collector before it, and Q1's base up to
/// 120% of Q2's collector), so all four are solved together: Newton on the 14 unknowns, with SPICE3's
/// pnjlim on both junctions of every transistor and on the diodes, as in prototypes/circuits.py. The
/// Jacobian is block-tridiagonal (a stage touches only its neighbours, through one collector-base
/// pair), so each Newton step is block Gaussian elimination from the input stage to the output stage
/// and back: four small dense solves. The bias is the same Newton with the capacitors open, solved once.
class FuzzCircuit final : public Circuit
{
public:
    FuzzCircuit();

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void setControls (double drive, double tone) override;
    void process (double* volts, int numSamples) noexcept override;
    std::complex<double> smallSignalResponse (double frequency) const override;
    double meanIterations() const override;

    static constexpr int numUnknowns = 14;
    /// The bias (DC operating point), by unknown: Q4 b e c, Q3 b e c d, Q2 b e c d, Q1 b e c.
    const std::array<double, numUnknowns>& bias() const noexcept { return operatingPoint; }
    /// Newton solves that hit the iteration cap (they keep the last iterate). For the tests.
    long unconverged() const noexcept { return failures; }

    // 2N5088: the Ebers-Moll part of Fairchild's SPICE model (prototypes/circuits.py, BJT_2N5088).
    static constexpr double transistorSaturationCurrent = 5.911e-15, forwardBeta = 1122.0, reverseBeta = 1.271;
    static constexpr double supply = 9.0;
    static constexpr int maxNewtonIterations = 100;
    /// Newton stops once no unknown moves more than this in a step (and nothing was limited). Near the
    /// root it converges quadratically, with the junctions' curvature 1/VT, so the error left after a
    /// 1 uV step is about 40 x (1e-6)^2 = 4e-11 V.
    static constexpr double newtonTolerance = 1.0e-6;

    /// One stage's block of the Newton system (row-major, stride 4) and its right-hand side.
    template <typename Value>
    struct Block
    {
        int n = 3;
        std::array<Value, 16> a {};
        std::array<Value, 4> r {};
    };

    /// All the linear admittances, for one time step (companions, 2C/T), for DC (capacitors open), or
    /// at one frequency (s C, complex) for the small-signal response.
    template <typename Value>
    struct Network
    {
        std::array<std::array<Value, 16>, 4> g {};  // each stage's own elements and its 2-ports' self terms
        std::array<Value, 3> upper {}, lower {};     // A[collector k, base k+1] and A[base k+1, collector k]
        Value gIn {}, gA {}, gB {}, gG {}, sigma {}, gK {}, gOut {};
        std::array<Value, 9> toneInverse {};         // the tone stack's inner block, inverted
        std::array<Value, 6> tonePi {};              // its port-to-inner admittances (2 x 3)
    };

private:
    template <typename Value, typename Admittance>
    void designNetwork (Network<Value>& net, Admittance capacitor) const noexcept;
    void linearRhs (double vin, std::array<double, numUnknowns>& rhs) const noexcept;
    int newton (const Network<double>& net, const std::array<double, numUnknowns>& rhs, std::array<double, numUnknowns>& v) noexcept;
    void limitFrom (const std::array<double, numUnknowns>& v) noexcept;
    void predict() noexcept;
    void updateCompanions (double vin) noexcept;
    void solveBias();

    double sampleRate = 192000.0, T = 1.0 / 192000.0;
    double drive = 0.5, tone = 0.5;
    double sustainUpper = 0.0, sustainLower = 0.0, toneUpper = 0.0, toneLower = 0.0;
    Network<double> network, dcNetwork;
    DiodeModel diode;
    double diodeVt = 0.0, diodeVcrit = 0.0, transistorVcrit = 0.0;

    Companion cIn, c10, c4, c5, c12, c6, cK, c11, c7, c8, c9, c3, cOut;
    std::array<double, numUnknowns> x {}, previous {}, operatingPoint {};
    std::array<double, 4> vbeLimited {}, vbcLimited {};
    std::array<double, 2> diodeLimited {};
    double output = 0.0;
    long iterations = 0, solves = 0, failures = 0;
};

} // namespace ampsim::drive

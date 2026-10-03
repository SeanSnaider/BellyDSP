// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "DriveCircuits.h"

#include <juce_core/juce_core.h>

#include <algorithm>

namespace ampsim::drive
{

namespace
{
constexpr double pi = juce::MathConstants<double>::pi;
constexpr double minimumPot = 1.0e-3; // a pot at the end of its travel: 1 milliohm, as in the reference
constexpr int maxIterations = 60;
constexpr double maxExponent = 700.0;

double pot (double resistance) { return std::max (resistance, minimumPot); }

/// SPICE3's junction voltage limiting (pnjlim) for a junction whose voltage would go from vOld to vNew.
double pnjlim (double vNew, double vOld, double vt, double vcrit)
{
    if (vNew > vcrit && std::abs (vNew - vOld) > 2.0 * vt)
    {
        if (vOld > 0.0)
        {
            const auto arg = 1.0 + (vNew - vOld) / vt;
            return arg > 0.0 ? vOld + vt * std::log (arg) : vcrit;
        }
        return vt * std::log (vNew / vt);
    }
    return vNew;
}
} // namespace

double audioTaper (double position)
{
    const auto p = juce::jlimit (0.0, 1.0, position);
    return (std::pow (81.0, p) - 1.0) / 80.0;
}

// ---- DiodePairSolver ------------------------------------------------------------------------

DiodePairSolver::DiodePairSolver (DiodeModel model)
    : twoIs (2.0 * model.saturationCurrent),
      vt (model.emissionCoefficient * thermalVoltage)
{
}

double DiodePairSolver::solve (double k, double a, double c, double v) noexcept
{
    // With gmin folded into the linear term (k' = k + 2 a gmin), the root satisfies
    //     F(v) = k' v + 2 a Is sinh(v / vt) - c = 0,   equivalently   v = vt asinh((c - k' v) / (2 a Is)).
    // Newton on F is fast while the diodes carry little current (F nearly linear) but overshoots up the
    // exponential and crawls back when they conduct; Newton on G(v) = v - vt asinh(z), z = (c - k' v) /
    // (2 a Is), is the other way around (G' ~ 1 when |z| is large). So each step uses whichever is nearly
    // linear there: G when |z| > Q = k' vt / (2 a Is), the point where the diodes' conductance matches
    // k', and F otherwise. Both rise monotonically with one root between 0 and c / k': a step that
    // leaves that bracket bisects instead, so it always converges.
    const auto kk = k + 2.0 * a * gmin;
    const auto scale = 1.0 / (a * twoIs);
    const auto q = kk * vt * scale;
    double lo = std::min (0.0, c / kk), hi = std::max (0.0, c / kk);
    v = juce::jlimit (lo, hi, v);

    int i = 0;
    for (; i < maxIterations; ++i)
    {
        const auto z = (c - kk * v) * scale;
        double f, df;
        if (std::abs (z) > q)
        {
            f = v - vt * std::asinh (z);
            df = 1.0 + q / std::sqrt (1.0 + z * z);
        }
        else
        {
            const auto e = std::exp (juce::jlimit (-maxExponent, maxExponent, v / vt));
            const auto ei = 1.0 / e;
            f = kk * v + 0.5 * a * twoIs * (e - ei) - c;
            df = kk + 0.5 * a * twoIs * (e + ei) / vt;
        }
        if (f > 0.0)
            hi = v;
        else if (f < 0.0)
            lo = v;
        else
            break;

        // Converged: take the last Newton step and stop. (Tested before the bracket check, because a
        // converged step lands on the bracket end it just set.)
        auto next = v - f / df;
        if (std::abs (next - v) <= 1.0e-15 + 1.0e-9 * std::abs (next))
        {
            v = next;
            break;
        }
        if (! (next > lo && next < hi))
            next = 0.5 * (lo + hi);
        v = next;
    }

    iterations += i + 1;
    ++solves;
    return v;
}

// ---- Linear sections ----------------------------------------------------------------------------

void FirstOrderSection::design (double n0, double n1, double d0, double d1, double c) noexcept
{
    const auto a0 = d0 + d1 * c;
    b0 = (n0 + n1 * c) / a0;
    b1 = (n0 - n1 * c) / a0;
    a1 = (d0 - d1 * c) / a0;
}

void SecondOrderSection::design (double n0, double n1, double n2, double d0, double d1, double d2, double c) noexcept
{
    const auto c2 = c * c;
    const auto a0 = d0 + d1 * c + d2 * c2;
    b0 = (n0 + n1 * c + n2 * c2) / a0;
    b1 = (2.0 * n0 - 2.0 * n2 * c2) / a0;
    b2 = (n0 - n1 * c + n2 * c2) / a0;
    a1 = (2.0 * d0 - 2.0 * d2 * c2) / a0;
    a2 = (d0 - d1 * c + d2 * c2) / a0;
}

Svf::Coefficients biquadToSvf (double n0, double n1, double n2, double d0, double d1, double d2, double sampleRate)
{
    const auto w0 = std::sqrt (d0 / d2);
    Svf::Coefficients c;
    c.k = d1 / (d2 * w0);
    c.m0 = n2 / d2;
    c.m1 = n1 / (d2 * w0) - c.k * c.m0;
    c.m2 = n0 / (d2 * w0 * w0) - c.m0;
    // Prewarp: the digital filter matches the analog one exactly at w0.
    c.g = std::tan (std::min (w0 / (2.0 * sampleRate), 0.49 * pi));
    c.a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
    c.a2 = c.g * c.a1;
    c.a3 = c.g * c.a2;
    return c;
}

namespace
{
std::complex<double> firstOrder (double n0, double n1, double d0, double d1, std::complex<double> s)
{
    return (n0 + n1 * s) / (d0 + d1 * s);
}

std::complex<double> secondOrder (double n0, double n1, double n2, double d0, double d1, double d2, std::complex<double> s)
{
    return (n0 + n1 * s + n2 * s * s) / (d0 + d1 * s + d2 * s * s);
}
} // namespace

// ---- Mid Drive (TS808) --------------------------------------------------------------------------

namespace
{
namespace ts
{
constexpr double r1 = 1.0e3, c1 = 0.02e-6, r2 = 510.0e3;                 // input buffer
constexpr double c2 = 1.0e-6, r5 = 10.0e3;                               // into IC1a
constexpr double r4 = 4.7e3, c3 = 0.047e-6;                              // gain leg
constexpr double r6 = 51.0e3, drivePot = 500.0e3, c4 = 51.0e-12;         // feedback
constexpr double rs = 1.0e3, cs = 0.22e-6, ri = 10.0e3;                  // tone stage low-pass and bias
constexpr double tonePot = 20.0e3, rz = 220.0, cz = 0.22e-6, rfb = 1.0e3;
constexpr double c7 = 1.0e-6, r11 = 1.0e3, levelPot = 100.0e3;           // output
constexpr double c8 = 0.1e-6, r12 = 510.0e3, rb = 100.0, c9 = 10.0e-6, rc = 10.0e3, load = 1.0e6;
constexpr double ro = rc * load / (rc + load);
} // namespace ts
} // namespace

MidDriveCircuit::MidDriveCircuit() : diodes (DiodeModel {})
{
    setSampleRate (sampleRate);
    reset();
}

double MidDriveCircuit::feedbackResistance (double driveKnob)
{
    return ts::r6 + ts::drivePot * audioTaper (driveKnob);
}

MidDriveCircuit::ToneCoefficients MidDriveCircuit::toneStage (double toneKnob)
{
    using namespace ts;
    const auto t = juce::jlimit (0.0, 1.0, toneKnob);
    const auto ra = pot (t * tonePot), rbPot = pot ((1.0 - t) * tonePot); // Rl (+ side), Rr (- side)
    const auto rab = ra * rbPot / (ra + rbPot);
    const auto wz = 1.0 / (cz * (rz + rab));
    const auto wp = 1.0 / (cs * rs * ri / (rs + ri));
    const auto K = rfb * ra / ((ra + rbPot) * (rz + rab));
    const auto X = (rbPot / (ra + rbPot)) / ((rz + rab) * cs);
    // H3 = (1 / (Rs Cs)) ((1 + K) s + wz) / (s^2 + (wp + wz + X) s + wp wz)
    return { wz / (rs * cs), (1.0 + K) / (rs * cs), wp * wz, wp + wz + X, 1.0 };
}

void MidDriveCircuit::designFixedSections()
{
    using namespace ts;
    const auto c = 2.0 * sampleRate;
    inputHighPass1.design (0.0, c1 * r2, 1.0, c1 * (r1 + r2), c);
    inputHighPass2.design (0.0, c2 * r5, 1.0, c2 * r5, c);
    gainLeg.design (0.0, c3, 1.0, r4 * c3, c); // volts in, amps out
    outputNetwork.design (0.0, 0.0, c7 * c8 * r12 * levelPot, 1.0, c7 * (levelPot + r11) + c8 * r12 + c8 * levelPot,
                          c7 * c8 * (r12 * (levelPot + r11) + levelPot * r11), c);
    outputHighPass.design (0.0, c9 * ro, 1.0, c9 * (rb + ro), c);
}

void MidDriveCircuit::setSampleRate (double newSampleRate)
{
    sampleRate = newSampleRate;
    a = 1.0 / (sampleRate * 2.0 * ts::c4); // T / (2 C4)
    designFixedSections();
    setControls (drive, tone);
}

void MidDriveCircuit::reset()
{
    for (auto* s : { &inputHighPass1, &inputHighPass2, &gainLeg, &outputHighPass })
        s->reset();
    outputNetwork.reset();
    toneFilter.reset();
    v = 0.0;
    history = 0.0;
}

void MidDriveCircuit::setControls (double newDrive, double newTone)
{
    drive = juce::jlimit (0.0, 1.0, newDrive);
    tone = juce::jlimit (0.0, 1.0, newTone);
    rf = feedbackResistance (drive);
    const auto t = toneStage (tone);
    toneFilter.setCoefficients (biquadToSvf (t.n0, t.n1, 0.0, t.d0, t.d1, t.d2, sampleRate));
}

void MidDriveCircuit::process (double* volts, int numSamples) noexcept
{
    const auto k = 1.0 + a / rf;
    for (int n = 0; n < numSamples; ++n)
    {
        // vp: the clipping amplifier's input. i: the gain leg's current, which the feedback network carries.
        const auto vp = inputHighPass2.process (inputHighPass1.process (volts[n]));
        const auto i = gainLeg.process (vp);

        // One trapezoidal step of C4 dV/dt = i - V/Rf - Id(V), solved for V.
        v = diodes.solve (k, a, a * i + history, v);
        history = 2.0 * v - history;

        volts[n] = outputHighPass.process (outputNetwork.process (toneFilter.processSample (vp + v)));
    }
}

std::complex<double> MidDriveCircuit::smallSignalResponse (double frequency) const
{
    using namespace ts;
    const std::complex<double> s (0.0, 2.0 * pi * frequency);
    const auto h1 = firstOrder (0.0, c1 * r2, 1.0, c1 * (r1 + r2), s);
    const auto h2 = firstOrder (0.0, c2 * r5, 1.0, c2 * r5, s);
    const auto zf = 1.0 / (1.0 / rf + diodes.restConductance() + s * c4);
    const auto zg = r4 + 1.0 / (s * c3);
    const auto t = toneStage (tone);
    const auto h3 = secondOrder (t.n0, t.n1, 0.0, t.d0, t.d1, t.d2, s);
    const auto h4 = secondOrder (0.0, 0.0, c7 * c8 * r12 * levelPot, 1.0, c7 * (levelPot + r11) + c8 * r12 + c8 * levelPot,
                                 c7 * c8 * (r12 * (levelPot + r11) + levelPot * r11), s);
    const auto h5 = firstOrder (0.0, c9 * ro, 1.0, c9 * (rb + ro), s);
    return h1 * h2 * (1.0 + zf / zg) * h3 * h4 * h5;
}

double MidDriveCircuit::meanIterations() const
{
    return diodes.solves > 0 ? (double) diodes.iterations / (double) diodes.solves : 0.0;
}

// ---- Distortion (RAT) ---------------------------------------------------------------------------

namespace
{
namespace rat
{
constexpr double c1 = 22.0e-9, r2 = 1.0e6, r3 = 1.0e3, c2 = 1.0e-9;                       // input
constexpr double distortionPot = 100.0e3, c4 = 100.0e-12;                                 // feedback
constexpr double r4 = 47.0, c5 = 2.2e-6, r5 = 560.0, c6 = 4.7e-6;                         // gain legs
constexpr double c7 = 4.7e-6, r6 = 1.0e3, r7 = 1.5e3, filterPot = 100.0e3, c8 = 3.3e-9, r8 = 1.0e6; // clipper, tone
constexpr double c10 = 1.0e-6, volumePot = 100.0e3, c9 = 22.0e-9, load = 1.0e6;          // output
} // namespace rat
} // namespace

DistortionCircuit::DistortionCircuit() : diodes (DiodeModel {})
{
    tailCurrent = slewRate * compensation;              // It = SR Cc: 9 uA
    inputScale = 1.0 / (2.0 * thermalVoltage);           // tanh(vd / 2VT)
    const auto gm = tailCurrent * inputScale;            // small-signal transconductance
    outputConductance = gm / openLoopGain;               // Ro = A0 / gm
    // The clamps reach `saturation` when they carry the whole input-stage current.
    clampVoltage = saturation - thermalVoltage * std::log (tailCurrent / clampSaturationCurrent + 1.0);
    setSampleRate (sampleRate);
    reset();
}

double DistortionCircuit::distortionResistance (double driveKnob)
{
    return pot (rat::distortionPot * audioTaper (driveKnob));
}

double DistortionCircuit::filterResistance (double toneKnob)
{
    // The Filter pot darkens clockwise; our tone knob brightens clockwise, so it turns the other way.
    return pot (rat::filterPot * audioTaper (1.0 - juce::jlimit (0.0, 1.0, toneKnob)));
}

void DistortionCircuit::designNetwork()
{
    using namespace rat;
    // Trapezoidal companions: a capacitor C is a conductance 2C/T plus a history current J.
    gC4 = 2.0 * c4 / T;
    gC5 = 2.0 * c5 / T;
    gC6 = 2.0 * c6 / T;
    gC7 = 2.0 * c7 / T;
    gC8 = 2.0 * c8 / T;
    // A resistor in series with a capacitor's companion: 1 / (R + T / 2C).
    gLegC5 = gC5 / (1.0 + r4 * gC5);
    gLegC6 = gC6 / (1.0 + r5 * gC6);
    gSeries7 = gC7 / (1.0 + r6 * gC7);
    ac = T / (2.0 * compensation);

    gFeedback = 1.0 / distortionResistance (drive) + gC4;
    sumM = gFeedback + gLegC5 + gLegC6;
    gTone = 1.0 / (r7 + filterResistance (tone));
    sumF = gTone + gC8 + 1.0 / r8;
}

void DistortionCircuit::setSampleRate (double newSampleRate)
{
    using namespace rat;
    sampleRate = newSampleRate;
    T = 1.0 / sampleRate;
    const auto c = 2.0 * sampleRate;
    // Hin = s C1 R2 / ((1 + s R3 C2)(1 + s C1 R2) + s C2 R2)
    inputNetwork.design (0.0, c1 * r2, 0.0, 1.0, r3 * c2 + c1 * r2 + c2 * r2, r3 * c2 * c1 * r2, c);
    // Hout = s^2 C10 C9 RL Rv / ((1 + s C9 RL)(1 + s C10 Rv) + s C9 Rv)
    outputNetwork.design (0.0, 0.0, c10 * c9 * load * volumePot, 1.0, c9 * load + c10 * volumePot + c9 * volumePot, c9 * load * c10 * volumePot, c);
    designNetwork();
}

void DistortionCircuit::reset()
{
    inputNetwork.reset();
    outputNetwork.reset();
    jC4 = jC5 = jC6 = jC7 = jC8 = 0.0;
    vo = opAmpHistory = vd = 0.0;
}

void DistortionCircuit::setControls (double newDrive, double newTone)
{
    drive = juce::jlimit (0.0, 1.0, newDrive);
    tone = juce::jlimit (0.0, 1.0, newTone);
    gFeedback = 1.0 / distortionResistance (drive) + gC4;
    sumM = gFeedback + gLegC5 + gLegC6;
    gTone = 1.0 / (rat::r7 + filterResistance (tone));
    sumF = gTone + gC8 + 1.0 / rat::r8;
}

double DistortionCircuit::opAmpStage (double vp) noexcept
{
    using namespace rat;
    // The feedback network gives the (-) input as vm = alpha vo + beta: KCL at (-) with the companions,
    //   (vo - vm)(1/Rd + G4) - J4 = G5' vm - J5' + G6' vm - J6'   (G' and J' for the series R-C legs).
    const auto jLegC5 = jC5 / (1.0 + r4 * gC5), jLegC6 = jC6 / (1.0 + r5 * gC6);
    const auto alpha = gFeedback / sumM;
    const auto beta = (jLegC5 + jLegC6 - jC4) / sumM;

    // One trapezoidal step of Cc dvo/dt = g(vo):  F(vo) = vo - ac g(vo) - H = 0, H = vo[n-1] + ac g[n-1].
    // F rises monotonically, and since |It tanh| <= It the root lies in [min(0, H - ac It), max(0, H + ac It)].
    const auto h = opAmpHistory;
    double lo = std::min (0.0, h - ac * tailCurrent), hi = std::max (0.0, h + ac * tailCurrent);
    auto v = juce::jlimit (lo, hi, vo);
    const auto clampVt = thermalVoltage;
    const auto clampVcrit = clampVt * std::log (clampVt / (std::sqrt (2.0) * clampSaturationCurrent));

    int i = 0;
    for (; i < maxIterations; ++i)
    {
        const auto th = std::tanh ((vp - alpha * v - beta) * inputScale);
        const auto eUp = std::exp (std::min ((v - clampVoltage) / clampVt, maxExponent));
        const auto eDown = std::exp (std::min (-(v + clampVoltage) / clampVt, maxExponent));
        const auto clamp = clampSaturationCurrent * (eUp - eDown) + 2.0 * gmin * v;
        const auto dClamp = clampSaturationCurrent * (eUp + eDown) / clampVt + 2.0 * gmin;
        const auto g = tailCurrent * th - outputConductance * v - clamp;
        const auto f = v - ac * g - h;
        const auto df = 1.0 + ac * (tailCurrent * inputScale * alpha * (1.0 - th * th) + outputConductance + dClamp);
        if (f > 0.0)
            hi = v;
        else if (f < 0.0)
            lo = v;
        else
            break;

        auto next = v - f / df;
        if (std::abs (next - v) <= 1.0e-15 + 1.0e-9 * std::abs (next)) // converged (see DiodePairSolver)
        {
            v = next;
            break;
        }

        // Limiting on the input pair's tanh, as in prototypes/circuits.py: linearized deep in
        // saturation the pair looks like a constant current, and Newton can fly to the opposite side
        // and back. From saturation a step may return at most to the knee (u = +-2) on its own side;
        // near the linear region the argument moves at most 2 per step.
        const auto uOld = (vp - alpha * v - beta) * inputScale;
        const auto uNew = (vp - alpha * next - beta) * inputScale;
        const auto maxMove = std::max (2.0, std::abs (uOld) - 2.0);
        if (std::abs (uNew - uOld) > maxMove)
            next = (vp - beta - (uOld + std::copysign (maxMove, uNew - uOld)) / inputScale) / alpha;

        // Junction limiting on the rail clamp the step drives forward.
        const auto side = next >= 0.0 ? 1.0 : -1.0;
        next = side * (clampVoltage + pnjlim (side * next - clampVoltage, side * v - clampVoltage, clampVt, clampVcrit));

        if (! (next > lo && next < hi))
            next = 0.5 * (lo + hi);
        v = next;
    }
    opAmpIterations += i + 1;
    ++opAmpSolves;

    vo = v;
    opAmpHistory = 2.0 * v - h; // ac g[n] = vo[n] - H

    // Update the feedback network's companions: J <- 2 G v - J for each capacitor's voltage v.
    const auto vm = alpha * v + beta;
    jC4 = 2.0 * gC4 * (v - vm) - jC4;
    const auto i5 = gLegC5 * vm - jLegC5;
    jC5 = 2.0 * gC5 * ((i5 + jC5) / gC5) - jC5;
    const auto i6 = gLegC6 * vm - jLegC6;
    jC6 = 2.0 * gC6 * ((i6 + jC6) / gC6) - jC6;
    return v;
}

double DistortionCircuit::clipperStage (double v) noexcept
{
    // Node f (the tone cap): vf = (Gt vd + J8) / (Gt + G8 + 1/R8). The series C7-R6 branch from the
    // op-amp: i7 = G7' (vo - vd) - J7'. KCL at the diode node, i7 = Id(vd) + Gt (vd - vf), gives
    //   (G7' + Gt (1 - Gt / sumF)) vd + Id(vd) = G7' vo - J7' + Gt J8 / sumF.
    const auto jSeries7 = jC7 / (1.0 + rat::r6 * gC7);
    const auto k = gSeries7 + gTone * (1.0 - gTone / sumF);
    const auto c = gSeries7 * v - jSeries7 + gTone * jC8 / sumF;
    vd = diodes.solve (k, 1.0, c, vd);

    const auto vf = (gTone * vd + jC8) / sumF;
    const auto i7 = gSeries7 * (v - vd) - jSeries7;
    jC7 = 2.0 * gC7 * ((i7 + jC7) / gC7) - jC7;
    jC8 = 2.0 * gC8 * vf - jC8;
    return vf;
}

void DistortionCircuit::process (double* volts, int numSamples) noexcept
{
    for (int n = 0; n < numSamples; ++n)
    {
        const auto vp = inputNetwork.process (volts[n]);
        const auto out = opAmpStage (vp);
        const auto vf = clipperStage (out); // the JFET buffer follows vf exactly
        volts[n] = outputNetwork.process (vf);
    }
}

std::complex<double> DistortionCircuit::smallSignalResponse (double frequency) const
{
    using namespace rat;
    const std::complex<double> s (0.0, 2.0 * pi * frequency);
    const auto hin = s * c1 * r2 / ((1.0 + s * r3 * c2) * (1.0 + s * c1 * r2) + s * c2 * r2);

    // Single-pole op-amp: gm into Cc, with Ro and the reverse-biased rail clamps (gmin each) at rest.
    const auto gm = tailCurrent * inputScale;
    const auto gOut = outputConductance + 2.0 * (clampSaturationCurrent / thermalVoltage * std::exp (-clampVoltage / thermalVoltage) + gmin);
    const auto A = gm / (gOut + s * compensation);
    const auto zf = 1.0 / (1.0 / distortionResistance (drive) + s * c4);
    const auto zg = 1.0 / (1.0 / (r4 + 1.0 / (s * c5)) + 1.0 / (r5 + 1.0 / (s * c6)));
    const auto amp = A / (1.0 + A * zg / (zg + zf));

    const auto rt = r7 + filterResistance (tone);
    const auto z8 = 1.0 / (s * c8 + 1.0 / r8);
    const auto yd = diodes.restConductance() + 1.0 / (rt + z8);
    const auto z7 = r6 + 1.0 / (s * c7);
    const auto clip = (1.0 / z7) / (1.0 / z7 + yd) * (z8 / (rt + z8));

    const auto hout = s * s * c10 * c9 * load * volumePot / ((1.0 + s * c9 * load) * (1.0 + s * c10 * volumePot) + s * c9 * volumePot);
    return hin * amp * clip * hout;
}

double DistortionCircuit::meanIterations() const
{
    const auto solves = (double) (opAmpSolves + diodes.solves);
    return solves > 0.0 ? (double) (opAmpIterations + diodes.iterations) / solves : 0.0;
}

// ---- Transparent (Klon Centaur) ---------------------------------------------------------------

namespace
{
namespace klon
{
constexpr double r1 = 10.0e3, c1 = 0.1e-6, r2 = 1.0e6;                              // input buffer
constexpr double c3 = 0.1e-6, r6 = 10.0e3, c5 = 68.0e-9, gainPot = 100.0e3;        // into the gain stage
constexpr double r7 = 1.5e3, c16 = 1.0e-6, r19 = 15.0e3;                           // feed-forward 1
constexpr double r11 = 15.0e3, c7 = 82.0e-9, r10 = 2.0e3, r12 = 422.0e3, c8 = 390.0e-12; // gain stage
constexpr double c9 = 1.0e-6, r13 = 1.0e3, c10 = 1.0e-6, r16 = 47.0e3, c11 = 2.2e-9, r15 = 22.0e3; // clipper
constexpr double r5 = 5.1e3, c4 = 68.0e-9, r8 = 1.5e3, c6 = 390.0e-9, r9 = 1.0e3;  // feed-forward 2
constexpr double r17 = 27.0e3, c12 = 27.0e-9, r18 = 12.0e3;
constexpr double r20 = 392.0e3, c13 = 820.0e-12;                                   // summing amplifier
constexpr double r22 = 100.0e3, r24 = 100.0e3, r21 = 1.8e3, r23 = 4.7e3, treblePot = 10.0e3, c14 = 3.9e-9; // treble
constexpr double c15 = 4.7e-6, r25 = 560.0, volumePot = 10.0e3, r28 = 100.0e3, load = 1.0e6; // output
constexpr double c2 = 4.7e-6, r3 = 100.0e3, rBleed = 560.0 + 68.0e3;               // bypass line: R4 + R243
constexpr double gOut = 1.0 / volumePot + 1.0 / r28 + 1.0 / load;
} // namespace klon
} // namespace

TransparentCircuit::TransparentCircuit() : diodes (DiodeModel { diodeSaturationCurrent, diodeEmission })
{
    // The macromodel's constants: the input pair's tanh(u / 2VT), Ro for the DC gain, and clamps that
    // reach `swing` when they carry the pair's whole current (prototypes/circuits.py, Netlist.tl072).
    inputScale = 1.0 / (2.0 * thermalVoltage);
    gm = inputStageCurrent * inputScale;
    outputConductance = gm / openLoopGain;
    clampVoltage = swing - thermalVoltage * std::log (inputStageCurrent / clampSaturationCurrent + 1.0);
    setSampleRate (sampleRate);
    reset();
}

void TransparentCircuit::setSampleRate (double newSampleRate)
{
    using namespace klon;
    sampleRate = newSampleRate;
    T = 1.0 / sampleRate;
    inputBuffer.design (0.0, c1 * r2, 1.0, c1 * (r1 + r2), 2.0 * sampleRate);
    c3.design (klon::c3, 0.0, T);
    c5.design (klon::c5, 0.0, T);
    c16.design (klon::c16, 0.0, T);
    c7.design (klon::c7, 0.0, T);
    c8.design (klon::c8, 0.0, T);
    c4.design (klon::c4, 0.0, T);
    c6.design (klon::c6, r9, T);
    c9.design (klon::c9, r13, T);
    c10.design (klon::c10, 0.0, T);
    c11.design (klon::c11, r15, T);
    c12.design (klon::c12, r18, T);
    c13.design (klon::c13, 0.0, T);
    c14.design (klon::c14, 0.0, T);
    c2.design (klon::c2, 0.0, T);
    c15.design (klon::c15, r25, T);
    designNetworks();
}

void TransparentCircuit::reset()
{
    inputBuffer.reset();
    for (auto* c : { &c3, &c5, &c16, &c7, &c8, &c4, &c6, &c9, &c10, &c11, &c12, &c13, &c14, &c2, &c15 })
        c->reset();
    vo = vj = 0.0;
}

void TransparentCircuit::setControls (double newDrive, double newTone)
{
    drive = juce::jlimit (0.0, 1.0, newDrive);
    tone = juce::jlimit (0.0, 1.0, newTone);
    designNetworks();
}

void TransparentCircuit::designNetworks() noexcept
{
    using namespace klon;
    // The Gain pot's gangs: `upper` (the + input to the bias; b to the wiper f) grows with the gain.
    const auto upper = pot (drive * gainPot), lower = pot ((1.0 - drive) * gainPot);

    // Front network, nodes a, p, g (see process()): p and g eliminated into a.
    g6 = 1.0 / r6 + c5.g;
    aP = g6 + 1.0 / upper;
    aG = 1.0 / r7 + c16.g + 1.0 / r19;
    kA = c3.g + g6 + 1.0 / r7 - g6 * g6 / aP - 1.0 / (r7 * r7 * aG);

    // Gain stage feedback, nodes m (the (-) input) and h: vm = alpha vo + beta.
    g12 = 1.0 / r12 + c8.g;
    g11 = 1.0 / r11 + c7.g;
    gH = 1.0 / (r10 + (1.0 - drive) * gainPot);
    kM = g12 + g11 * gH / (g11 + gH);

    // The ladder b - f - e - d, eliminated from b toward the diode node.
    g4 = 1.0 / r5 + c4.g;
    gUpper = 1.0 / upper;
    gLower = 1.0 / lower;
    g17 = 1.0 / r17 + c12.gs;
    aB = g4 + 1.0 / r8 + c6.gs + gUpper;
    aF2 = gUpper + gLower + g17 + c11.gs - gUpper * gUpper / aB;
    aE2 = c11.gs + 1.0 / r16 + c10.g - c11.gs * c11.gs / aF2;
    kD = c10.g + c9.gs - c10.g * c10.g / aE2;
    kJ = kD / (1.0 + kD * diodeSeriesResistance);

    // Treble control and output.
    ra = r21 + (1.0 - tone) * treblePot;
    rb = r23 + tone * treblePot;
    yT = 1.0 / ra + 1.0 / rb + c14.g;
    aX = c2.g + 1.0 / r3 + 1.0 / rBleed;
    kO = 1.0 / rBleed + c15.gs + gOut - 1.0 / (rBleed * rBleed * aX);
}

double TransparentCircuit::gainStage (double vp) noexcept
{
    // KCL at m and h with the companions of C8 (|| R12, from the output) and C7 (|| R11, to h):
    //   g12 (vo - vm) - J8 = g11 (vm - vh) - J7,   g11 (vm - vh) - J7 = gH vh
    // gives vm = alpha vo + beta.
    const auto alpha = g12 / kM;
    const auto beta = (c7.j * gH / (g11 + gH) - c8.j) / kM;

    // The macromodel's node: F(v) = Go v + Iclamp(v) - It tanh((vp - vm(v)) / 2VT) = 0 rises
    // monotonically, and since |It tanh| <= It and the clamps carry It at +-swing, the root lies in
    // [-swing, swing]. Newton from the last sample's output, with the RAT's safeguards (limiting on the
    // tanh's argument and on the clamp junctions, bisection when a step leaves the bracket).
    double lo = -swing, hi = swing;
    auto v = juce::jlimit (lo, hi, vo);
    const auto vcrit = thermalVoltage * std::log (thermalVoltage / (std::sqrt (2.0) * clampSaturationCurrent));
    int i = 0;
    for (; i < maxIterations; ++i)
    {
        const auto u = (vp - alpha * v - beta) * inputScale;
        const auto th = std::tanh (u);
        const auto eUp = std::exp (std::min ((v - clampVoltage) / thermalVoltage, maxExponent));
        const auto eDown = std::exp (std::min ((-clampVoltage - v) / thermalVoltage, maxExponent));
        const auto f = outputConductance * v + clampSaturationCurrent * (eUp - eDown) + 2.0 * gmin * v - inputStageCurrent * th;
        const auto df = outputConductance + clampSaturationCurrent * (eUp + eDown) / thermalVoltage + 2.0 * gmin
                        + inputStageCurrent * inputScale * alpha * (1.0 - th * th);
        if (f > 0.0)
            hi = v;
        else if (f < 0.0)
            lo = v;
        else
            break;

        auto next = v - f / df;
        if (std::abs (next - v) <= 1.0e-15 + 1.0e-9 * std::abs (next))
        {
            v = next;
            break;
        }
        const auto uNew = (vp - alpha * next - beta) * inputScale;
        const auto maxMove = std::max (2.0, std::abs (u) - 2.0);
        if (std::abs (uNew - u) > maxMove)
            next = (vp - beta - (u + std::copysign (maxMove, uNew - u)) / inputScale) / alpha;
        const auto side = next >= 0.0 ? 1.0 : -1.0;
        next = side * (clampVoltage + pnjlim (side * next - clampVoltage, side * v - clampVoltage, thermalVoltage, vcrit));
        if (! (next > lo && next < hi))
            next = 0.5 * (lo + hi);
        v = next;
    }
    opAmpIterations += i + 1;
    ++opAmpSolves;
    vo = v;

    const auto vm = alpha * v + beta;
    const auto vh = (g11 * vm - c7.j) / (g11 + gH);
    c8.updateFromVoltage (v - vm);
    c7.updateFromVoltage (vm - vh);
    return v;
}

void TransparentCircuit::process (double* volts, int numSamples) noexcept
{
    using namespace klon;
    const auto g7 = 1.0 / r7, gBleed = 1.0 / rBleed, rs = diodeSeriesResistance;
    for (int n = 0; n < numSamples; ++n)
    {
        const auto buf = inputBuffer.process (volts[n]);

        // Front network. KCL at a, p, g with C3 (from the buffer), R6 || C5 (a to p), R7 (a to g),
        // the gang's `upper` (p to the bias), C16 and R19 (g to ground and to the summing node):
        //   (G3 + g6 + g7) va - g6 vp - g7 vg = G3 buf - J3 + J5
        //   vp = (g6 va - J5) / aP,   vg = (g7 va + J16) / aG
        const auto va = (c3.g * buf - c3.j + c5.j * (1.0 - g6 / aP) + g7 * c16.j / aG) / kA;
        const auto vp = (g6 * va - c5.j) / aP;
        const auto vg = (g7 * va + c16.j) / aG;
        c3.updateFromVoltage (buf - va);
        c5.updateFromVoltage (va - vp);
        c16.updateFromVoltage (vg);

        const auto out1 = gainStage (vp);

        // The ladder, KCL at b, f, e, d (series R-C branches as one companion each):
        //   aB vb - gU vf = g4 buf - J4 + J6'
        //   -gU vb + aF vf - G11' ve = J12' - J11'
        //   -G11' vf + aE ve - G10 vd = J11' - J10
        //   -G10 ve + aD vd + Id(vd) = G9' out1 - J9' + J10
        // eliminated forward (aF2, aE2, kD from designNetworks()), then the diodes, then back.
        const auto rB = g4 * buf - c4.j + c6.js();
        const auto rF = c12.js() - c11.js() + gUpper * rB / aB;
        const auto rE = c11.js() - c10.j + c11.gs * rF / aF2;
        const auto c = c9.gs * out1 - c9.js() + c10.j + c10.g * rE / aE2;
        vj = diodes.solve (kJ, 1.0, c / (1.0 + kD * rs), vj); // the junctions, behind their series resistance
        const auto vd = vj + rs * (c / (1.0 + kD * rs) - kJ * vj);
        const auto ve = (rE + c10.g * vd) / aE2;
        const auto vf = (rF + c11.gs * ve) / aF2;
        const auto vb = (rB + gUpper * vf) / aB;

        // Into the summing amplifier's virtual ground: R16 from e, R17 || (C12 + R18) from f, R19 from g.
        const auto i17 = g17 * vf - c12.js();
        const auto iS = ve / r16 + i17 + vg / r19;

        c4.updateFromVoltage (buf - vb);
        c6.updateFromCurrent (c6.gs * vb - c6.js());
        c12.updateFromCurrent (i17 - vf / r17);
        c11.updateFromCurrent (c11.gs * (ve - vf) - c11.js());
        c10.updateFromVoltage (vd - ve);
        c9.updateFromCurrent (c9.gs * (out1 - vd) - c9.js());

        // Summing amplifier: iS + vsum / R20 + G13 vsum - J13 = 0.
        const auto vsum = (c13.j - iS) / (1.0 / r20 + c13.g);
        c13.updateFromVoltage (vsum);

        // Treble control: KCL at the wiper w and at (-) (a virtual ground), with C14's companion:
        //   vw = (vsum / Ra + vt / Rb + J14) / yT,   vsum / R22 + vt / R24 + G14 vw - J14 = 0.
        const auto vt = (-vsum * (1.0 / r22 + c14.g / (ra * yT)) + c14.j * (1.0 - c14.g / yT)) / (1.0 / r24 + c14.g / (rb * yT));
        c14.updateFromVoltage ((vsum / ra + vt / rb + c14.j) / yT);

        // Output: node x on the bypass line (C2 from the buffer, R3, R4 + R243 to the output) and the
        // output node (C15 + R25 from the treble stage, Volume || R28 || load).
        const auto rX = c2.g * buf - c2.j;
        const auto vout = (c15.gs * vt - c15.js() + gBleed * rX / aX) / kO;
        const auto vx = (rX + gBleed * vout) / aX;
        c2.updateFromVoltage (buf - vx);
        c15.updateFromCurrent (c15.gs * (vt - vout) - c15.js());
        volts[n] = vout;
    }
}

std::complex<double> TransparentCircuit::smallSignalResponse (double frequency) const
{
    // The same stages at s = j 2 pi f, the diodes at their rest conductance (each behind its Rs) and the
    // macromodel at its rest gain (prototypes/circuits.py, transparent_small_signal).
    using namespace klon;
    using C = std::complex<double>;
    const C s (0.0, 2.0 * pi * frequency);
    const auto upper = pot (drive * gainPot), lower = pot ((1.0 - drive) * gainPot);
    const auto hBuf = s * c1 * r2 / (1.0 + s * c1 * (r1 + r2));

    const C y3 = s * klon::c3, y6 = 1.0 / r6 + s * klon::c5, g7 = 1.0 / r7, y16 = s * klon::c16, g19 = 1.0 / r19;
    const auto ap = y6 + 1.0 / upper, ag = g7 + y16 + g19;
    const auto va = y3 / (y3 + y6 + g7 - y6 * y6 / ap - g7 * g7 / ag);
    const auto vp = y6 * va / ap, vg = g7 * va / ag;

    const auto gClamps = 2.0 * (clampSaturationCurrent / thermalVoltage * std::exp (-clampVoltage / thermalVoltage) + gmin);
    const auto A = gm / (outputConductance + gClamps);
    const auto zf = 1.0 / (1.0 / r12 + s * klon::c8);
    const auto zg = 1.0 / (1.0 / r11 + s * klon::c7) + r10 + (1.0 - drive) * gainPot;
    const auto vo1 = A * vp / (1.0 + A * zg / (zg + zf));

    const C y4 = 1.0 / r5 + s * klon::c4, y6b = 1.0 / (r9 + 1.0 / (s * klon::c6));
    const auto gu = 1.0 / upper, gl = 1.0 / lower;
    const auto y17 = 1.0 / r17 + 1.0 / (r18 + 1.0 / (s * klon::c12));
    const auto y11 = 1.0 / (r15 + 1.0 / (s * klon::c11)), y10 = s * klon::c10, y9 = 1.0 / (r13 + 1.0 / (s * klon::c9));
    const auto g0 = diodeSaturationCurrent / (diodeEmission * thermalVoltage) + gmin;
    const auto gd = 2.0 * g0 / (1.0 + g0 * diodeSeriesResistance);
    const auto ab = y4 + 1.0 / r8 + y6b + gu;
    const auto af = gu + gl + y17 + y11 - gu * gu / ab;
    const auto rf = gu * y4 / ab;
    const auto ae = y11 + 1.0 / r16 + y10 - y11 * y11 / af;
    const auto re = y11 * rf / af;
    const auto vd = (y9 * vo1 + y10 * re / ae) / (y10 + y9 + gd - y10 * y10 / ae);
    const auto ve = (re + y10 * vd) / ae;
    const auto vf = (rf + y11 * ve) / af;

    const auto vsum = -(ve / r16 + vf * y17 + vg * g19) / (1.0 / r20 + s * klon::c13);
    const auto y14 = s * klon::c14;
    const auto Y = 1.0 / ra + 1.0 / rb + y14;
    const auto vt = -(Y / r22 + y14 / ra) / (Y / r24 + y14 / rb) * vsum;

    const C y2 = s * klon::c2, gbl = 1.0 / rBleed, y15 = 1.0 / (r25 + 1.0 / (s * klon::c15));
    const auto a11 = y2 + 1.0 / r3 + gbl, a22 = gbl + y15 + gOut;
    const auto vout = (y15 * vt + gbl * y2 / a11) / (a22 - gbl * gbl / a11);
    return hBuf * vout;
}

double TransparentCircuit::meanIterations() const
{
    const auto solves = (double) (opAmpSolves + diodes.solves);
    return solves > 0.0 ? (double) (opAmpIterations + diodes.iterations) / solves : 0.0;
}

// ---- Fuzz (Big Muff Pi) -----------------------------------------------------------------------

namespace
{
namespace muff
{
constexpr double r2 = 39.0e3, c1 = 1.0e-6, r14 = 47.0e3, r9 = 470.0e3, c10 = 470.0e-12, r13 = 10.0e3, r22 = 100.0; // Q4
constexpr double c4 = 1.0e-6, sustainPot = 100.0e3, r23 = 1.0e3, c5 = 0.1e-6, r19 = 10.0e3;                          // sustain
constexpr double r20 = 100.0e3, r17 = 470.0e3, c12 = 470.0e-12, c6 = 1.0e-6, r18 = 10.0e3, r21 = 150.0;              // Q3
constexpr double c13 = 0.1e-6, r12 = 10.0e3;                                                                          // coupling
constexpr double r16 = 100.0e3, r15 = 470.0e3, c11 = 470.0e-12, c7 = 1.0e-6, r11 = 10.0e3, r10 = 150.0;              // Q2
constexpr double r8 = 39.0e3, c8 = 10.0e-9, c9 = 4.0e-9, r5 = 22.0e3, tonePot = 100.0e3, c3 = 0.1e-6;                // tone stack
constexpr double r7 = 430.0e3, r3 = 100.0e3, r6 = 15.0e3, r4 = 3.3e3;                                                 // Q1
constexpr double c2 = 0.1e-6, volumePot = 100.0e3, load = 1.0e6, rOut = volumePot * load / (volumePot + load);      // output

// Unknowns: Q4 (b, e, c), Q3 (b, e, c, d), Q2 (b, e, c, d), Q1 (b, e, c); local indices within a stage.
constexpr std::array<size_t, 4> start { 0, 3, 7, 11 };
constexpr std::array<int, 4> size { 3, 4, 4, 3 };
constexpr size_t B = 0, E = 1, C = 2, D = 3;
} // namespace muff

/// Solves the n x n block a (row-major, stride 4) for two right-hand sides at once, by Gaussian
/// elimination with partial pivoting. a, y, and z are overwritten (y and z with the solutions).
template <typename T>
void solveSmall (int n, std::array<T, 16>& a, std::array<T, 4>& y, std::array<T, 4>& z) noexcept
{
    for (int col = 0; col < n; ++col)
    {
        int p = col;
        for (int r = col + 1; r < n; ++r)
            if (std::abs (a[(size_t) (r * 4 + col)]) > std::abs (a[(size_t) (p * 4 + col)]))
                p = r;
        if (p != col)
        {
            for (int k = col; k < n; ++k)
                std::swap (a[(size_t) (p * 4 + k)], a[(size_t) (col * 4 + k)]);
            std::swap (y[(size_t) p], y[(size_t) col]);
            std::swap (z[(size_t) p], z[(size_t) col]);
        }
        const auto pivot = T (1) / a[(size_t) (col * 4 + col)];
        for (int r = col + 1; r < n; ++r)
        {
            const auto f = a[(size_t) (r * 4 + col)] * pivot;
            for (int k = col + 1; k < n; ++k)
                a[(size_t) (r * 4 + k)] -= f * a[(size_t) (col * 4 + k)];
            y[(size_t) r] -= f * y[(size_t) col];
            z[(size_t) r] -= f * z[(size_t) col];
        }
    }
    for (int r = n - 1; r >= 0; --r)
    {
        auto sy = y[(size_t) r], sz = z[(size_t) r];
        for (int k = r + 1; k < n; ++k)
        {
            sy -= a[(size_t) (r * 4 + k)] * y[(size_t) k];
            sz -= a[(size_t) (r * 4 + k)] * z[(size_t) k];
        }
        const auto pivot = T (1) / a[(size_t) (r * 4 + r)];
        y[(size_t) r] = sy * pivot;
        z[(size_t) r] = sz * pivot;
    }
}

/// The four stages' block-tridiagonal system: stage k reaches stage k + 1 only through
/// A[collector k, base k+1] = upper[k] and A[base k+1, collector k] = lower[k]. Block Gaussian
/// elimination from the input stage on: once stage k is eliminated, stage k + 1's block and
/// right-hand side change only at its base,
///     A'[B][B] -= lower[k] (A'_k^-1)[C][C] upper[k],   r'[B] -= lower[k] (A'_k^-1 r'_k)[C],
/// and back substitution gives x_k = y_k - upper[k] x_{k+1}[B] z_k, with y_k = A'_k^-1 r'_k and
/// z_k = A'_k^-1 e_C (the column the next stage couples through).
template <typename T>
void solveChain (std::array<FuzzCircuit::Block<T>, 4>& s, const std::array<T, 3>& upper, const std::array<T, 3>& lower,
                 std::array<T, FuzzCircuit::numUnknowns>& x) noexcept
{
    using namespace muff;
    std::array<std::array<T, 4>, 4> y {}, z {};
    for (size_t k = 0; k < 4; ++k)
    {
        auto& b = s[k];
        if (k > 0)
        {
            b.a[B * 4 + B] -= lower[k - 1] * z[k - 1][C] * upper[k - 1];
            b.r[B] -= lower[k - 1] * y[k - 1][C];
        }
        y[k] = b.r;
        z[k] = {};
        z[k][C] = T (1);
        solveSmall (b.n, b.a, y[k], z[k]);
    }
    T nextBase {};
    for (int k = 3; k >= 0; --k)
    {
        const auto kk = (size_t) k;
        const auto coupling = k < 3 ? upper[kk] * nextBase : T {};
        for (int i = 0; i < s[kk].n; ++i)
            x[start[kk] + (size_t) i] = y[kk][(size_t) i] - coupling * z[kk][(size_t) i];
        nextBase = x[start[kk] + B];
    }
}

template <typename T>
void invert3 (const std::array<T, 9>& m, std::array<T, 9>& inv) noexcept
{
    // The adjugate over the determinant (the tone stack's inner block, diagonally dominant).
    inv[0] = m[4] * m[8] - m[5] * m[7];
    inv[1] = m[2] * m[7] - m[1] * m[8];
    inv[2] = m[1] * m[5] - m[2] * m[4];
    inv[3] = m[5] * m[6] - m[3] * m[8];
    inv[4] = m[0] * m[8] - m[2] * m[6];
    inv[5] = m[2] * m[3] - m[0] * m[5];
    inv[6] = m[3] * m[7] - m[4] * m[6];
    inv[7] = m[1] * m[6] - m[0] * m[7];
    inv[8] = m[0] * m[4] - m[1] * m[3];
    const auto det = T (1) / (m[0] * inv[0] + m[1] * inv[3] + m[2] * inv[6]);
    for (auto& v : inv)
        v *= det;
}
} // namespace

FuzzCircuit::FuzzCircuit()
{
    diodeVt = diode.emissionCoefficient * thermalVoltage;
    diodeVcrit = diodeVt * std::log (diodeVt / (std::sqrt (2.0) * diode.saturationCurrent));
    transistorVcrit = thermalVoltage * std::log (thermalVoltage / (std::sqrt (2.0) * transistorSaturationCurrent));
    setSampleRate (sampleRate);
    solveBias();
    reset();
}

template <typename Value, typename Admittance>
void FuzzCircuit::designNetwork (Network<Value>& net, Admittance capacitor) const noexcept
{
    // capacitor (C) is the capacitor's admittance: 2C / T for the trapezoidal companions, 0 for DC (open),
    // or s C at one frequency. A resistor R in series with it: y / (1 + R y).
    using namespace muff;
    const auto series = [] (Value y, double r) { return y / (Value (1) + r * y); };
    net.gIn = series (capacitor (muff::c1), r2);
    net.gA = series (capacitor (muff::c4), sustainUpper);
    net.gG = Value (1.0 / (sustainLower + r23));
    net.gB = series (capacitor (muff::c5), r19);
    net.sigma = net.gA + net.gB + net.gG;
    net.gK = series (capacitor (muff::c13), r12);
    net.gOut = series (capacitor (muff::c2), rOut);

    // Sustain as a 2-port between Q4's collector and Q3's base: C4 + the pot's upper part to the
    // wiper (gA), its lower part + R23 to ground (gG), C5 + R19 on to the base (gB); the wiper eliminated.
    const auto s11 = net.gA - net.gA * net.gA / net.sigma, s12 = -net.gA * net.gB / net.sigma, s22 = net.gB - net.gB * net.gB / net.sigma;

    // Tone stack as a 2-port between Q2's collector and Q1's base. Inner nodes tl (R8 and C8, the
    // low-pass), th (C9 and R5, the high-pass), tw (the wiper, with C3 to Q1): Y = Ypp - Ypi Yii^-1 Yip.
    const auto y8 = capacitor (muff::c8), y9 = capacitor (muff::c9), y3 = capacitor (muff::c3);
    const auto gu = 1.0 / toneUpper, gl = 1.0 / toneLower;
    const std::array<Value, 9> inner { 1.0 / r8 + y8 + gl, Value {}, Value (-gl),
                                   Value {}, y9 + 1.0 / r5 + gu, Value (-gu),
                                   Value (-gl), Value (-gu), gu + gl + y3 };
    invert3 (inner, net.toneInverse);
    net.tonePi = { Value (-1.0 / r8), -y9, Value {}, Value {}, Value {}, -y3 }; // rows c2, b1; columns tl, th, tw
    const std::array<Value, 4> ports { 1.0 / r8 + y9, Value {}, Value {}, y3 };
    std::array<Value, 4> toneY {};
    for (size_t p = 0; p < 2; ++p)
        for (size_t q = 0; q < 2; ++q)
        {
            Value sum {};
            for (size_t i = 0; i < 3; ++i)
                for (size_t j = 0; j < 3; ++j)
                    sum += net.tonePi[p * 3 + i] * net.toneInverse[i * 3 + j] * net.tonePi[q * 3 + j];
            toneY[p * 2 + q] = ports[p * 2 + q] - sum;
        }

    // Each stage's own elements (KCL, currents leaving each node), plus its 2-ports' self terms.
    for (auto& g : net.g)
        g = {};
    const auto f9 = 1.0 / r9 + capacitor (muff::c10), f17 = 1.0 / r17 + capacitor (muff::c12), f15 = 1.0 / r15 + capacitor (muff::c11);
    const auto y6 = capacitor (muff::c6), y7 = capacitor (muff::c7);
    auto& q4 = net.g[0];
    q4[B * 4 + B] = 1.0 / r14 + f9 + net.gIn;
    q4[B * 4 + C] = -f9;
    q4[C * 4 + C] = 1.0 / r13 + f9 + s11;
    q4[C * 4 + B] = -f9;
    q4[E * 4 + E] = Value (1.0 / r22);
    auto& q3 = net.g[1];
    q3[B * 4 + B] = 1.0 / r20 + f17 + y6 + s22;
    q3[B * 4 + C] = -f17;
    q3[B * 4 + D] = -y6;
    q3[C * 4 + C] = 1.0 / r18 + f17 + net.gK;
    q3[C * 4 + B] = -f17;
    q3[D * 4 + D] = y6;
    q3[D * 4 + B] = -y6;
    q3[E * 4 + E] = Value (1.0 / r21);
    auto& q2 = net.g[2];
    q2[B * 4 + B] = 1.0 / r16 + f15 + y7 + net.gK;
    q2[B * 4 + C] = -f15;
    q2[B * 4 + D] = -y7;
    q2[C * 4 + C] = 1.0 / r11 + f15 + toneY[0];
    q2[C * 4 + B] = -f15;
    q2[D * 4 + D] = y7;
    q2[D * 4 + B] = -y7;
    q2[E * 4 + E] = Value (1.0 / r10);
    auto& q1 = net.g[3];
    q1[B * 4 + B] = 1.0 / r7 + 1.0 / r3 + toneY[3];
    q1[C * 4 + C] = 1.0 / r6 + net.gOut;
    q1[E * 4 + E] = Value (1.0 / r4);
    net.upper = { s12, -net.gK, toneY[1] };
    net.lower = { s12, -net.gK, toneY[2] };
}

void FuzzCircuit::setSampleRate (double newSampleRate)
{
    using namespace muff;
    sampleRate = newSampleRate;
    T = 1.0 / sampleRate;
    cIn.design (c1, r2, T);
    c10.design (muff::c10, 0.0, T);
    c4.design (muff::c4, sustainUpper, T);
    c5.design (muff::c5, r19, T);
    c12.design (muff::c12, 0.0, T);
    c6.design (muff::c6, 0.0, T);
    cK.design (c13, r12, T);
    c11.design (muff::c11, 0.0, T);
    c7.design (muff::c7, 0.0, T);
    c8.design (muff::c8, 0.0, T);
    c9.design (muff::c9, 0.0, T);
    c3.design (muff::c3, 0.0, T);
    cOut.design (c2, rOut, T);
    setControls (drive, tone);
}

void FuzzCircuit::setControls (double newDrive, double newTone)
{
    using namespace muff;
    drive = juce::jlimit (0.0, 1.0, newDrive);
    tone = juce::jlimit (0.0, 1.0, newTone);
    sustainUpper = pot ((1.0 - drive) * sustainPot); // C4 to the wiper: shrinks as sustain rises
    sustainLower = pot (drive * sustainPot);
    toneUpper = pot ((1.0 - tone) * tonePot);        // the high-pass end (C9, R5) to the wiper
    toneLower = pot (tone * tonePot);
    c4.design (muff::c4, sustainUpper, T);          // C4's series resistance is the pot's upper part
    const auto t = T;
    designNetwork (network, [t] (double c) { return 2.0 * c / t; });
    designNetwork (dcNetwork, [] (double) { return 0.0; });
}

void FuzzCircuit::limitFrom (const std::array<double, numUnknowns>& v) noexcept
{
    using namespace muff;
    for (size_t k = 0; k < 4; ++k)
    {
        const auto s = start[k];
        vbeLimited[k] = v[s + B] - v[s + E];
        vbcLimited[k] = v[s + B] - v[s + C];
    }
    for (size_t k = 1; k <= 2; ++k)
        diodeLimited[k - 1] = v[start[k] + D] - v[start[k] + C];
}

void FuzzCircuit::solveBias()
{
    // The bias: the same Newton with the capacitors open, the input at 0, the supply on. It doesn't
    // depend on the knobs (the coupling capacitors isolate every stage, and the tone stack's DC path is
    // the whole pot whatever the wiper does).
    using namespace muff;
    std::array<double, numUnknowns> rhs {};
    rhs[start[0] + C] = supply / r13;
    rhs[start[1] + C] = supply / r18;
    rhs[start[2] + C] = supply / r11;
    rhs[start[3] + B] = supply / r7;
    rhs[start[3] + C] = supply / r6;
    std::array<double, numUnknowns> v { 0.6, 0.02, 7.0, 0.7, 0.07, 4.3, 4.3, 0.7, 0.07, 4.3, 4.3, 1.7, 1.0, 4.3 };
    limitFrom (v);
    [[maybe_unused]] const auto saved = failures;
    newton (dcNetwork, rhs, v);
    jassert (failures == saved);
    operatingPoint = v;
}

void FuzzCircuit::reset()
{
    // Back to rest: the bias, every capacitor charged to its bias voltage and carrying no current
    // (J = G v; a series resistor drops nothing at zero current).
    using namespace muff;
    x = operatingPoint;
    previous = x;
    limitFrom (x);
    const auto at = [this] (size_t stage, size_t local) { return x[start[stage] + local]; };
    const auto charge = [] (Companion& c, double v) { c.j = c.g * v; };
    charge (cIn, -at (0, B));
    charge (c10, at (0, C) - at (0, B));
    charge (c4, at (0, C));        // the wiper rests at 0 V
    charge (c5, -at (1, B));
    charge (c12, at (1, C) - at (1, B));
    charge (c6, at (1, B) - at (1, D));
    charge (cK, at (1, C) - at (2, B));
    charge (c11, at (2, C) - at (2, B));
    charge (c7, at (2, B) - at (2, D));
    // Tone stack inner nodes at DC: v_inner = -Yii^-1 Yip v_ports (no history at DC).
    std::array<double, 3> inner {};
    const std::array<double, 2> ports { at (2, C), at (3, B) };
    for (size_t i = 0; i < 3; ++i)
    {
        double sum = 0.0;
        for (size_t j = 0; j < 3; ++j)
            for (size_t p = 0; p < 2; ++p)
                sum += dcNetwork.toneInverse[i * 3 + j] * dcNetwork.tonePi[p * 3 + j] * ports[p];
        inner[i] = -sum;
    }
    charge (c8, inner[0]);
    charge (c9, at (2, C) - inner[1]);
    charge (c3, inner[2] - at (3, B));
    charge (cOut, at (3, C));     // the output rests at 0 V
    output = 0.0;
}

void FuzzCircuit::linearRhs (double vin, std::array<double, numUnknowns>& rhs) const noexcept
{
    // The right-hand side of every node's KCL: the supply through the collector and bias resistors, the
    // input through R2 + C1, and the capacitors' history currents (a companion from node a to node b
    // puts -J on a's side and +J on b's), the 2-ports' through their elimination.
    using namespace muff;
    const auto& net = network;
    const auto ja = c4.js(), jb = c5.js();
    const auto sustain1 = -ja + net.gA * (ja - jb) / net.sigma; // leaving Q4's collector
    const auto sustain2 = jb + net.gB * (ja - jb) / net.sigma;  // leaving Q3's base
    const std::array<double, 2> jp { -c9.j, c3.j };              // ports c2, b1
    const std::array<double, 3> ji { -c8.j, c9.j, -c3.j };       // inner tl, th, tw
    std::array<double, 2> toneJ {};
    for (size_t p = 0; p < 2; ++p)
    {
        double sum = 0.0;
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = 0; j < 3; ++j)
                sum += net.tonePi[p * 3 + i] * net.toneInverse[i * 3 + j] * ji[j];
        toneJ[p] = jp[p] - sum;
    }
    const auto at = [] (size_t stage, size_t local) { return start[stage] + local; };
    rhs = {};
    rhs[at (0, B)] = net.gIn * vin - cIn.js() - c10.j;
    rhs[at (0, C)] = supply / r13 + c10.j - sustain1;
    rhs[at (1, B)] = -c12.j + c6.j - sustain2;
    rhs[at (1, C)] = supply / r18 + c12.j + cK.js();
    rhs[at (1, D)] = -c6.j;
    rhs[at (2, B)] = -c11.j + c7.j - cK.js();
    rhs[at (2, C)] = supply / r11 + c11.j - toneJ[0];
    rhs[at (2, D)] = -c7.j;
    rhs[at (3, B)] = supply / r7 - toneJ[1];
    rhs[at (3, C)] = supply / r6 + cOut.js();
}

int FuzzCircuit::newton (const Network<double>& net, const std::array<double, numUnknowns>& rhs, std::array<double, numUnknowns>& v) noexcept
{
    // Newton on the 14 unknowns, as SPICE does it: every device linearized at its (limited) junction
    // voltages, the linear system solved for the step, the new junction voltages limited with pnjlim,
    // until no step exceeds newtonTolerance and nothing was limited. The step is solved from the
    // residual (A dx = b - A v), so the solve's rounding scales with the step.
    //
    // The step's linear system in closed form: within a stage, the emitter's row (R_E and the
    // transistor) and the diode node's row (C6 / C7 and the diodes) touch only that stage, and their
    // diagonals are always positive (a conductance plus a junction's), so both are eliminated directly,
    // leaving a 2 x 2 block in (base, collector) per stage. The stages then form a chain of 2 x 2 blocks
    // coupled collector-to-next-base, solved by block Gaussian elimination as in solveChain(): forward
    // from the input stage, back from the output stage.
    using namespace muff;
    const auto is = transistorSaturationCurrent, kr = 1.0 + 1.0 / reverseBeta;
    int iteration = 0;
    for (; iteration < maxNewtonIterations; ++iteration)
    {
        struct Reduced
        {
            double bb, bc, cb, cc, rb, rc; // the (base, collector) block and right-hand side
            double eb, ec, re, ie;         // the emitter row (ie = 1 / its diagonal)
            double db, dc, rd, id;         // the diode node's row (id = 1 / its diagonal)
        };
        std::array<Reduced, 4> st {};
        for (size_t k = 0; k < 4; ++k)
        {
            const auto s = start[k];
            const auto& g = net.g[k];
            const bool clipper = k == 1 || k == 2;
            const auto vb = v[s + B], ve = v[s + E], vc = v[s + C], vd = clipper ? v[s + D] : 0.0;
            auto& t = st[k];

            // The linear part (each block's pattern: b-b, b-c, b-d, c-b, c-c, e-e, d-b, d-d) and the
            // couplings to the neighbouring stages, as the residual b - A v.
            t.bb = g[B * 4 + B];
            t.bc = g[B * 4 + C];
            t.cb = g[C * 4 + B];
            t.cc = g[C * 4 + C];
            auto ee = g[E * 4 + E];
            const auto bd = g[B * 4 + D], db = g[D * 4 + B];
            auto dd = g[D * 4 + D];
            t.rb = rhs[s + B] - t.bb * vb - t.bc * vc - bd * vd;
            t.rc = rhs[s + C] - t.cb * vb - t.cc * vc;
            t.re = rhs[s + E] - ee * ve;
            t.rd = clipper ? rhs[s + D] - db * vb - dd * vd : 0.0;
            if (k < 3)
                t.rc -= net.upper[k] * v[start[k + 1] + B];
            if (k > 0)
                t.rb -= net.lower[k - 1] * v[start[k - 1] + C];

            // The transistor: Ic and Ib (and -(Ic + Ib) at the emitter), linearized at the limited
            // junction voltages and evaluated at this iterate. Below vbc = -1 V, e^(vbc/VT) < 2e-17 is
            // under the rounding of the -Is next to it, so it isn't computed (exact in double precision).
            const auto lbe = vbeLimited[k], lbc = vbcLimited[k];
            const auto ebe = std::exp (std::min (lbe / thermalVoltage, maxExponent));
            const auto ebc = lbc > -1.0 ? std::exp (std::min (lbc / thermalVoltage, maxExponent)) : 0.0;
            const auto ibe = is * (ebe - 1.0) + gmin * lbe, ibc = is * (ebc - 1.0) + gmin * lbc;
            const auto gbe = is * ebe / thermalVoltage + gmin, gbc = is * ebc / thermalVoltage + gmin;
            const auto dbe = (vb - ve) - lbe, dbc = (vb - vc) - lbc;
            const auto ic = ibe - ibc * kr + gbe * dbe - gbc * kr * dbc;
            const auto ib = ibe / forwardBeta + ibc / reverseBeta + gbe / forwardBeta * dbe + gbc / reverseBeta * dbc;
            t.rc -= ic;
            t.rb -= ib;
            t.re += ic + ib;
            // d/d(vb, ve, vc), with vbe = vb - ve and vbc = vb - vc.
            const double dic[3] { gbe - gbc * kr, -gbe, gbc * kr };
            const double dib[3] { gbe / forwardBeta + gbc / reverseBeta, -gbe / forwardBeta, -gbc / reverseBeta };
            t.bb += dib[0];
            const auto be = dib[1];
            t.bc += dib[2];
            t.cb += dic[0];
            const auto ce = dic[1];
            t.cc += dic[2];
            t.eb = -(dic[0] + dib[0]);
            ee -= dic[1] + dib[1];
            t.ec = -(dic[2] + dib[2]);

            // The clipping stages' diode pairs, from d to the collector: 2 Is sinh(u / vt) + 2 gmin u.
            double cd = 0.0;
            t.db = db;
            t.dc = 0.0;
            if (clipper)
            {
                const auto l = diodeLimited[k - 1];
                const auto e = std::exp (juce::jlimit (-maxExponent, maxExponent, l / diodeVt));
                const auto ei = 1.0 / e;
                const auto gd = diode.saturationCurrent * (e + ei) / diodeVt + 2.0 * gmin;
                const auto id = diode.saturationCurrent * (e - ei) + 2.0 * gmin * l + gd * ((vd - vc) - l);
                t.rd -= id;
                t.rc += id;
                dd += gd;
                t.dc = -gd;
                cd = -gd;
                t.cc += gd;
            }

            // Eliminate the emitter, then the diode node.
            t.ie = 1.0 / ee;
            t.bb -= be * t.eb * t.ie;
            t.bc -= be * t.ec * t.ie;
            t.rb -= be * t.re * t.ie;
            t.cb -= ce * t.eb * t.ie;
            t.cc -= ce * t.ec * t.ie;
            t.rc -= ce * t.re * t.ie;
            if (clipper)
            {
                t.id = 1.0 / dd;
                t.bb -= bd * t.db * t.id;
                t.bc -= bd * t.dc * t.id;
                t.rb -= bd * t.rd * t.id;
                t.cb -= cd * t.db * t.id;
                t.cc -= cd * t.dc * t.id;
                t.rc -= cd * t.rd * t.id;
            }
        }

        // The chain of 2 x 2 blocks (as solveChain()).
        std::array<double, 4> yb {}, yc {}, zb {}, zc {};
        for (size_t k = 0; k < 4; ++k)
        {
            auto& t = st[k];
            if (k > 0)
            {
                t.bb -= net.lower[k - 1] * zc[k - 1] * net.upper[k - 1];
                t.rb -= net.lower[k - 1] * yc[k - 1];
            }
            const auto det = 1.0 / (t.bb * t.cc - t.bc * t.cb);
            yb[k] = (t.cc * t.rb - t.bc * t.rc) * det;
            yc[k] = (t.bb * t.rc - t.cb * t.rb) * det;
            zb[k] = -t.bc * det;
            zc[k] = t.bb * det;
        }
        std::array<double, numUnknowns> step {};
        double nextBase = 0.0;
        for (int kk = 3; kk >= 0; --kk)
        {
            const auto k = (size_t) kk;
            const auto coupling = k < 3 ? net.upper[k] * nextBase : 0.0;
            const auto db = yb[k] - coupling * zb[k], dc = yc[k] - coupling * zc[k];
            const auto s = start[k];
            const auto& t = st[k];
            step[s + B] = db;
            step[s + C] = dc;
            step[s + E] = (t.re - t.eb * db - t.ec * dc) * t.ie;
            if (k == 1 || k == 2)
                step[s + D] = (t.rd - t.db * db - t.dc * dc) * t.id;
            nextBase = db;
        }

        bool converged = true;
        for (size_t i = 0; i < numUnknowns; ++i)
        {
            v[i] += step[i];
            converged = converged && std::abs (step[i]) <= newtonTolerance;
        }

        bool limited = false;
        for (size_t k = 0; k < 4; ++k)
        {
            const auto s = start[k];
            const auto be = v[s + B] - v[s + E], bc = v[s + B] - v[s + C];
            vbeLimited[k] = pnjlim (be, vbeLimited[k], thermalVoltage, transistorVcrit);
            vbcLimited[k] = pnjlim (bc, vbcLimited[k], thermalVoltage, transistorVcrit);
            limited = limited || ! juce::exactlyEqual (vbeLimited[k], be) || ! juce::exactlyEqual (vbcLimited[k], bc);
        }
        for (size_t k = 1; k <= 2; ++k)
        {
            const auto u = v[start[k] + D] - v[start[k] + C];
            auto& l = diodeLimited[k - 1];
            l = u >= 0.0 ? pnjlim (u, l, diodeVt, diodeVcrit) : -pnjlim (-u, -l, diodeVt, diodeVcrit);
            limited = limited || ! juce::exactlyEqual (l, u);
        }
        if (converged && ! limited)
            return iteration + 1;
    }
    ++failures;
    limitFrom (v);
    return iteration;
}

void FuzzCircuit::updateCompanions (double vin) noexcept
{
    using namespace muff;
    const auto& net = network;
    const auto at = [this] (size_t stage, size_t local) { return x[start[stage] + local]; };
    cIn.updateFromCurrent (net.gIn * (vin - at (0, B)) - cIn.js());
    c10.updateFromVoltage (at (0, C) - at (0, B));
    const auto ja = c4.js(), jb = c5.js();
    const auto wiper = (net.gA * at (0, C) + net.gB * at (1, B) - ja + jb) / net.sigma;
    c4.updateFromCurrent (net.gA * (at (0, C) - wiper) - ja);
    c5.updateFromCurrent (net.gB * (wiper - at (1, B)) - jb);
    c12.updateFromVoltage (at (1, C) - at (1, B));
    c6.updateFromVoltage (at (1, B) - at (1, D));
    cK.updateFromCurrent (net.gK * (at (1, C) - at (2, B)) - cK.js());
    c11.updateFromVoltage (at (2, C) - at (2, B));
    c7.updateFromVoltage (at (2, B) - at (2, D));
    // Tone stack inner nodes: v_inner = -Yii^-1 (Yip v_ports + j_inner).
    const std::array<double, 2> ports { at (2, C), at (3, B) };
    const std::array<double, 3> ji { -c8.j, c9.j, -c3.j };
    std::array<double, 3> inner {};
    for (size_t i = 0; i < 3; ++i)
    {
        double sum = 0.0;
        for (size_t j = 0; j < 3; ++j)
            sum += net.toneInverse[i * 3 + j] * (net.tonePi[j] * ports[0] + net.tonePi[3 + j] * ports[1] + ji[j]);
        inner[i] = -sum;
    }
    c8.updateFromVoltage (inner[0]);
    c9.updateFromVoltage (at (2, C) - inner[1]);
    c3.updateFromVoltage (inner[2] - at (3, B));
    const auto iOut = net.gOut * at (3, C) - cOut.js();
    cOut.updateFromCurrent (iOut);
    output = iOut * rOut;
}

void FuzzCircuit::predict() noexcept
{
    // Newton starts from a linear extrapolation of the last two solutions, with the junctions limited
    // (pnjlim) from where they were, as SPICE's transient predictor does: on smooth signals the guess is
    // then off by the second difference instead of the first.
    using namespace muff;
    for (size_t i = 0; i < numUnknowns; ++i)
    {
        const auto guess = 2.0 * x[i] - previous[i];
        previous[i] = x[i];
        x[i] = guess;
    }
    for (size_t k = 0; k < 4; ++k)
    {
        const auto s = start[k];
        vbeLimited[k] = pnjlim (x[s + B] - x[s + E], vbeLimited[k], thermalVoltage, transistorVcrit);
        vbcLimited[k] = pnjlim (x[s + B] - x[s + C], vbcLimited[k], thermalVoltage, transistorVcrit);
    }
    for (size_t k = 1; k <= 2; ++k)
    {
        const auto u = x[start[k] + D] - x[start[k] + C];
        auto& l = diodeLimited[k - 1];
        l = u >= 0.0 ? pnjlim (u, l, diodeVt, diodeVcrit) : -pnjlim (-u, -l, diodeVt, diodeVcrit);
    }
}

void FuzzCircuit::process (double* volts, int numSamples) noexcept
{
    std::array<double, numUnknowns> rhs {};
    for (int n = 0; n < numSamples; ++n)
    {
        predict();
        linearRhs (volts[n], rhs);
        iterations += newton (network, rhs, x);
        ++solves;
        updateCompanions (volts[n]);
        volts[n] = output;
    }
}

std::complex<double> FuzzCircuit::smallSignalResponse (double frequency) const
{
    // The same blocks at s = j 2 pi f: capacitors as s C, the transistors and diodes linearized at the
    // bias, the input through R2 + C1 (prototypes/circuits.py, fuzz_small_signal).
    using namespace muff;
    using Cx = std::complex<double>;
    const Cx s (0.0, 2.0 * pi * frequency);
    Network<Cx> net;
    designNetwork (net, [s] (double c) { return s * c; });
    std::array<Block<Cx>, 4> blocks;
    for (size_t k = 0; k < 4; ++k)
    {
        blocks[k].n = size[k];
        blocks[k].a = net.g[k];
    }
    const auto is = transistorSaturationCurrent, kr = 1.0 + 1.0 / reverseBeta;
    for (size_t k = 0; k < 4; ++k)
    {
        const auto o = start[k];
        const auto vbe = operatingPoint[o + B] - operatingPoint[o + E], vbc = operatingPoint[o + B] - operatingPoint[o + C];
        const auto gbe = is * std::exp (vbe / thermalVoltage) / thermalVoltage + gmin, gbc = is * std::exp (vbc / thermalVoltage) / thermalVoltage + gmin;
        const double dic[3] { gbe - gbc * kr, -gbe, gbc * kr };
        const double dib[3] { gbe / forwardBeta + gbc / reverseBeta, -gbe / forwardBeta, -gbc / reverseBeta };
        for (size_t j = 0; j < 3; ++j)
        {
            blocks[k].a[C * 4 + j] += dic[j];
            blocks[k].a[B * 4 + j] += dib[j];
            blocks[k].a[E * 4 + j] -= dic[j] + dib[j];
        }
    }
    for (size_t k = 1; k <= 2; ++k)
    {
        const auto u = operatingPoint[start[k] + D] - operatingPoint[start[k] + C];
        const auto gd = 2.0 * diode.saturationCurrent * std::cosh (u / diodeVt) / diodeVt + 2.0 * gmin;
        blocks[k].a[D * 4 + D] += gd;
        blocks[k].a[D * 4 + C] -= gd;
        blocks[k].a[C * 4 + D] -= gd;
        blocks[k].a[C * 4 + C] += gd;
    }
    blocks[0].r[B] = net.gIn; // a 1 V source through R2 + C1
    std::array<Cx, numUnknowns> v {};
    solveChain (blocks, net.upper, net.lower, v);
    return v[start[3] + C] * net.gOut * rOut;
}

double FuzzCircuit::meanIterations() const
{
    return solves > 0 ? (double) iterations / (double) solves : 0.0;
}

} // namespace ampsim::drive

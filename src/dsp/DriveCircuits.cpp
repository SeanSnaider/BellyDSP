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

} // namespace ampsim::drive

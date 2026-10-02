"""
A small SPICE-style circuit simulator, the stand-in for ngspice (BUILD_PLAN "Boost and Overdrive",
Validation; ngspice isn't installed, ASSUMPTIONS W3). It simulates the drive pedals' full schematics and
renders the golden references the C++ drive models are tested against (tests/DriveTests.cpp).

Method, as in SPICE (L. W. Nagel, "SPICE2: A Computer Program to Simulate Semiconductor Circuits", 1975):
  - Modified nodal analysis (MNA). The unknowns are the node voltages, plus one branch current for every
    voltage source, voltage-controlled voltage source (VCVS), and ideal op-amp output. Each element
    "stamps" its equations into the matrix A and the right-hand side b of A x = b.
  - Capacitors use the trapezoidal companion model: i[n] = (2C/T)(v[n] - v[n-1]) - i[n-1], a conductance
    2C/T in parallel with a current source carrying the history. SPICE's default integration method.
  - Diodes are Shockley junctions, I = Is (exp(V / (n VT)) - 1) + GMIN V, linearized at every Newton
    iteration (conductance dI/dV plus an equivalent current source). Newton steps on each junction
    voltage are limited with SPICE3's pnjlim, which keeps the exponential from overshooting.
  - An ideal op-amp is a nullor: an extra equation v+ = v-, and an output current the solver chooses.
  - The LM308 (the RAT's op-amp) is a macromodel instead, because its slowness is part of the sound: its
    input pair is a transconductor I = It tanh((v+ - v-) / (2 VT)) (exact for a BJT differential pair)
    feeding the 30 pF compensation capacitor as an integrator. That one mechanism gives both the slew
    rate, It / Cc, and the gain-bandwidth product, It / (2 VT 2 pi Cc). A large resistor sets the DC gain,
    clamp diodes to +-Vclamp stop the output at the supply rails, and an ideal buffer drives the load.
    The Klon's TL072s use the same structure without the capacitor (a static macromodel: infinite
    bandwidth, finite gain, output clamps).
  - Bipolar transistors are Ebers-Moll in SPICE's transport form (Gummel-Poon with only Is, Bf, Br),
    stamped as conductances between their three nodes plus equivalent sources, with pnjlim on both
    junctions. The DC operating point starts them at SPICE's initial junction guess and falls back to
    source stepping.
  - Every time step iterates Newton until the node voltages move less than 1e-9 V + 1e-9 |v| and no
    junction was limited.
  - AC analysis linearizes every element at the operating point and solves the complex system at each
    frequency, like SPICE's .ac.

The op-amp pedals are simulated as their AC equivalents: the 4.5 V bias rail and the supply are signal
ground, so every node sits at 0 V at rest. Transistor buffers (the Tube Screamer's emitter followers, the
RAT's JFET source follower, the Klon's input buffer) are ideal unity-gain buffers. The Big Muff is
different: its transistors are the circuit, so it runs on its real 9 V supply with its real bias, found
by the operating point.

The renders run at 16x (768 kHz). The 48 kHz input is upsampled and the output decimated with a long
linear-phase Kaiser FIR (passband to 21 kHz, -140 dB from 27 kHz), so the reference is a band-limited,
alias-free, zero-phase rendering of the circuit.

Usage:
  uv run --with numpy --with scipy python prototypes/circuits.py --check        # validation only
  uv run --with numpy --with scipy python prototypes/circuits.py --golden tests/fixtures/drive
  ... --golden tests/fixtures/drive --circuits transparent,fuzz   # only these; cases.json is merged
"""

import argparse
import json
import math
import os
import time

import numpy as np
from scipy.io import wavfile
from scipy.signal import firwin, resample_poly

VT = 0.025865          # thermal voltage kT/q at 27 C (SPICE's nominal temperature), volts
GMIN = 1e-12           # SPICE's minimum junction conductance, siemens

# 1N914 / 1N4148 small-signal silicon diode: Shockley parameters of the widely used SPICE model
# (.model 1N914 D(Is=2.52n Rs=0.568 N=1.752 ...)); series resistance and capacitances left out.
DIODE_1N914 = dict(Is=2.52e-9, n=1.752)

# LM308 with the RAT's 30 pF compensation capacitor. Slew rate 0.3 V/us (ElectroSmash, "ProCo RAT
# analysis"; National's LM108/LM308 datasheet: "bandwidth and slew rate are proportional to 1/Cf").
# The tanh input pair then gives GBW = SR / (4 pi VT) = 0.92 MHz (ElectroSmash: at 67 dB of gain the
# response holds up to about 500 Hz, i.e. GBW ~ 1 MHz). Open-loop DC gain 300 V/mV (datasheet typical).
# Output swing: 1.5 V short of each 9 V rail (datasheet: within 1 to 2 V of the rails), +-3.0 V around
# the 4.5 V bias.
LM308 = dict(Cc=30e-12, slew=0.3e6, a0=3e5, vsat=3.0)

# 1N34A germanium point-contact diode (the Klon's clippers): the SPICE model that circulates in the
# LTspice user libraries, .model 1N34A D(IS=2E-7 RS=7 N=1.3 BV=75 IBV=18m CJO=0.5p VJ=0.1 M=0.27
# EG=0.67). Its forward voltage, N VT ln(I / Is + 1) + Rs I, is 0.29 V at 1 mA and 0.38 V at 5 mA,
# against ElectroSmash's VF = 0.35 V and measured 1N34As at 0.25 to 0.3 V near 1.2 mA. Against the
# 1N914 (0.58 V at 1 mA) that is half the clipping level, and N VT ln 10 = 77 mV per decade of current
# is 27% of the forward voltage against 18% for the 1N914: the softer knee. Breakdown (75 V) and
# capacitance (0.5 pF) are left out.
DIODE_1N34A = dict(Is=2.0e-7, n=1.3, rs=7.0)

# 2N5088 NPN (the Big Muff's transistors; ElectroSmash lists it as the BC239's equivalent): the
# Ebers-Moll part of Fairchild's SPICE model (.model 2N5088 NPN(Is=5.911f Bf=1.122K Br=1.271 Ne=1.394
# Ise=5.911f Ikf=14.92m Vaf=62.37 Rb=10 Rc=1.61 Cjc=4.017p Cje=4.973p ...)). Left out: the low-current
# recombination (Ise, Ne), high injection (Ikf), the Early effect (Vaf), the terminal resistances, and
# the junction capacitances, which add 4 to 5 pF to the circuit's 470 pF Miller capacitors.
BJT_2N5088 = dict(Is=5.911e-15, bf=1122.0, br=1.271)

# TL072 (the Klon's op-amps), static macromodel: the LM308's structure without its compensation
# capacitor, so infinite bandwidth, with the output clamped. The TL072's 13 V/us slew rate and 3 MHz
# gain-bandwidth are far beyond what the Klon asks of it (the fastest edge in any fixture is printed
# when rendering), but its gain stage runs on 9 V and saturates at high gain, so the clamps matter.
# Open-loop gain 200 V/mV (datasheet typical). The clamps stand for the output stage's saturation, so
# the stage feeding them is a BJT-style pair, It tanh(u / 2VT), that hands them its whole current with
# a few tens of mV of overdrive: the output follows the ideal value to within 0.05 V of the limit and
# bends over in the last ~0.15 V, independent of how hard it's overdriven (with the JFET pair's real
# 0.69 V scale the bend would start 0.5 V early and depend on the loop's gain, which the output stage's
# saturation doesn't). Output swing: 1.5 V short of each rail (datasheet: +-13.5 V on +-15 V into 10k),
# so +-3.0 V around the 4.5 V bias on the gain stage's 9 V supply.
TL072 = dict(a0=2e5, vs=2.0 * VT, it=200e-6)
TL072_VSAT_9V = 3.0

FS = 48000.0
VOLTS_AT_FULL_SCALE = math.sqrt(2.0) * 0.7746 * 10.0 ** (12.0 / 20.0)  # +12 dBu RMS sine at 0 dBFS = 4.3611 V peak

R_MIN = 1e-3  # a pot turned fully to one end: 1 milliohm instead of a singular 0 ohm


def taper_audio(p):
    """Audio (log, 'A') taper: 10% of the resistance at half rotation, (81^p - 1) / 80."""
    return (81.0 ** p - 1.0) / 80.0


# ---------------------------------------------------------------------------------------------------
# Netlist


class Netlist:
    """A circuit as a list of elements on named nodes; '0' is ground."""

    def __init__(self):
        self.nodes = {}
        self.elements = []

    def n(self, name):
        if name == "0":
            return -1
        if name not in self.nodes:
            self.nodes[name] = len(self.nodes)
        return self.nodes[name]

    def R(self, a, b, r):
        self.elements.append(("R", self.n(a), self.n(b), max(r, R_MIN)))

    def C(self, a, b, c):
        self.elements.append(("C", self.n(a), self.n(b), c))

    def VIN(self, a):
        """The input: an ideal voltage source from node a to ground, driven by the signal."""
        self.elements.append(("VIN", self.n(a), -1))

    def VDC(self, a, v):
        self.elements.append(("VDC", self.n(a), -1, v))

    def E(self, a, c, gain=1.0):
        """VCVS from node a to ground: v(a) = gain v(c). An ideal transistor buffer."""
        self.elements.append(("E", self.n(a), -1, self.n(c), -1, gain))

    def OPAMP(self, p, m, o):
        """Ideal op-amp (nullor): v(p) = v(m), output current free."""
        self.elements.append(("OPAMP", self.n(p), self.n(m), self.n(o)))

    def D(self, a, k, Is, n):
        self.elements.append(("D", self.n(a), self.n(k), Is, n))

    def GTANH(self, x, p, m, it, vs):
        """Current It tanh((v(p) - v(m)) / vs) flowing from ground into node x."""
        self.elements.append(("GTANH", self.n(x), self.n(p), self.n(m), it, vs))

    def DRS(self, a, k, Is, n, rs, name):
        """A diode with SPICE's series resistance: rs from a to an internal node, then the junction."""
        j = name + "_j"
        self.R(a, j, rs)
        self.D(j, k, Is, n)

    def Q(self, c, b, e, Is, bf, br):
        """NPN transistor, Ebers-Moll in SPICE's transport form (Gummel-Poon with only Is, Bf, Br):
            Ibe = Is (exp(vbe / VT) - 1) + GMIN vbe,   Ibc = Is (exp(vbc / VT) - 1) + GMIN vbc
            Ic = Ibe - Ibc (1 + 1 / Br),               Ib = Ibe / Bf + Ibc / Br,   Ie = -(Ic + Ib)
        (SPICE3's bjtload.c with qb = 1 and no leakage terms). Both junction voltages get pnjlim."""
        self.elements.append(("Q", self.n(c), self.n(b), self.n(e), Is, bf, br))

    def tl072(self, p, m, o, prefix, vsat=TL072_VSAT_9V):
        """The TL072 static macromodel described at the top: a tanh input pair into a resistor that sets
        the open-loop gain, clamp diodes that stop the output at +-vsat, and an ideal output buffer."""
        a0, vs, it = TL072["a0"], TL072["vs"], TL072["it"]
        gm = it / vs
        x = prefix + "_x"
        self.GTANH(x, p, m, it, vs)
        self.R(x, "0", a0 / gm)
        clamp_is, clamp_n = 1e-14, 1.0
        vclamp = vsat - clamp_n * VT * math.log(it / clamp_is + 1.0)
        self.VDC(prefix + "_vp", vclamp)
        self.VDC(prefix + "_vn", -vclamp)
        self.D(x, prefix + "_vp", clamp_is, clamp_n)
        self.D(prefix + "_vn", x, clamp_is, clamp_n)
        self.E(o, x)

    def lm308(self, p, m, o, prefix="u"):
        """The op-amp macromodel described at the top (slew, GBW, rails, output buffer)."""
        cc, sr, a0, vsat = LM308["Cc"], LM308["slew"], LM308["a0"], LM308["vsat"]
        it = sr * cc
        gm = it / (2.0 * VT)
        x = prefix + "_x"
        self.GTANH(x, p, m, it, 2.0 * VT)
        self.C(x, "0", cc)
        self.R(x, "0", a0 / gm)
        # Clamp diodes reach vsat when they carry the whole input-stage current It.
        clamp_is, clamp_n = 1e-14, 1.0
        vclamp = vsat - clamp_n * VT * math.log(it / clamp_is + 1.0)
        self.VDC(prefix + "_vp", vclamp)
        self.VDC(prefix + "_vn", -vclamp)
        self.D(x, prefix + "_vp", clamp_is, clamp_n)
        self.D(prefix + "_vn", x, clamp_is, clamp_n)
        self.E(o, x)


# ---------------------------------------------------------------------------------------------------
# The pedals (component values and sources in docs; see also src/dsp/DriveCircuits.h)


def mid_drive(drive, tone):
    """Ibanez TS808 Tube Screamer (the "Mid Drive" mode). Values: ElectroSmash, "Tube Screamer circuit
    analysis", and Yeh, Abel, Smith, DAFx-07 (tone stage topology, its Fig. 15 and eq. 24)."""
    c = Netlist()
    c.VIN("in")
    # Input buffer: R1 1k, C1 20 nF, R2 510k bias, Q1 emitter follower (ideal).
    c.R("in", "n1", 1e3)
    c.C("n1", "n2", 0.02e-6)
    c.R("n2", "0", 510e3)
    c.E("e1", "n2")
    # Clipping amplifier, IC1a: C2 1 uF and R5 10k into (+); gain leg R4 4.7k + C3 47 nF; feedback
    # R6 51k + Drive (500k audio taper), C4 51 pF, and D1/D2 (1N914) antiparallel.
    c.C("e1", "vp", 1e-6)
    c.R("vp", "0", 10e3)
    c.OPAMP("vp", "vm", "clip")
    c.R("vm", "g", 4.7e3)
    c.C("g", "0", 0.047e-6)
    c.R("vm", "clip", 51e3 + 500e3 * taper_audio(drive))
    c.C("vm", "clip", 51e-12)
    c.D("vm", "clip", **DIODE_1N914)
    c.D("clip", "vm", **DIODE_1N914)
    # Tone stage, IC1b: R7 1k and C5 0.22 uF low-pass with Ri 10k bias into (+); Tone 20k (linear) from
    # (+) to (-), its wiper to ground through R8 220 and C6 0.22 uF; feedback Rf 1k.
    c.R("clip", "tp", 1e3)
    c.C("tp", "0", 0.22e-6)
    c.R("tp", "0", 10e3)
    c.R("tp", "w", tone * 20e3)
    c.R("w", "tm", (1.0 - tone) * 20e3)
    c.R("w", "z", 220.0)
    c.C("z", "0", 0.22e-6)
    c.R("tout", "tm", 1e3)
    c.OPAMP("tp", "tm", "tout")
    # Output: C7 1 uF, 1k, Level 100k (at maximum: the output is the top of the pot), C8 0.1 uF into
    # the output buffer's 510k bias, Q3 emitter follower (ideal), RB 100, C9 10 uF, RC 10k, and a 1M load
    # (the amp's input).
    c.C("tout", "v1", 1e-6)
    c.R("v1", "vol", 1e3)
    c.R("vol", "0", 100e3)
    c.C("vol", "b3", 0.1e-6)
    c.R("b3", "0", 510e3)
    c.E("e3", "b3")
    c.R("e3", "o1", 100.0)
    c.C("o1", "out", 10e-6)
    c.R("out", "0", 10e3)
    c.R("out", "0", 1e6)
    return c


def distortion(drive, tone):
    """ProCo RAT (the "Distortion" mode). Values: ElectroSmash, "ProCo RAT analysis" (parts list)."""
    c = Netlist()
    c.VIN("in")
    # Input: R1 1M pull-down, C1 22 nF, R2 1M bias, R3 1k, C2 1 nF.
    c.R("in", "0", 1e6)
    c.C("in", "a", 22e-9)
    c.R("a", "0", 1e6)
    c.R("a", "p", 1e3)
    c.C("p", "0", 1e-9)
    # Gain stage: LM308 (macromodel), Distortion 100k audio taper with C4 100 pF across it, and two gain
    # legs to ground, R4 47 + C5 2.2 uF and R5 560 + C6 4.7 uF.
    c.lm308("p", "m", "o")
    c.R("o", "m", 100e3 * taper_audio(drive))
    c.C("o", "m", 100e-12)
    c.R("m", "l1", 47.0)
    c.C("l1", "0", 2.2e-6)
    c.R("m", "l2", 560.0)
    c.C("l2", "0", 4.7e-6)
    # Clipper: C7 4.7 uF, R6 1k, D1/D2 (1N914) to ground. Tone: R7 1.5k plus Filter 100k (audio taper;
    # clockwise is darker, so tone 1 = filter at 0 ohm) into C8 3.3 nF, R8 1M gate bias.
    c.C("o", "c7", 4.7e-6)
    c.R("c7", "d", 1e3)
    c.D("d", "0", **DIODE_1N914)
    c.D("0", "d", **DIODE_1N914)
    c.R("d", "t", 1.5e3)
    c.R("t", "f", 100e3 * taper_audio(1.0 - tone))
    c.C("f", "0", 3.3e-9)
    c.R("f", "0", 1e6)
    # Output: JFET source follower (ideal), C10 1 uF, Volume 100k at maximum, C9 22 nF, 1M load.
    c.E("j", "f")
    c.C("j", "g", 1e-6)
    c.R("g", "0", 100e3)
    c.C("g", "out", 22e-9)
    c.R("out", "0", 1e6)
    return c


def transparent(drive, tone):
    """Klon Centaur (the "Transparent" mode). Values and topology: ElectroSmash, "Klon Centaur Analysis"
    (its schematic and stage drawings). Drive is the dual-gang Gain pot (100k linear, both gangs), tone
    the Treble pot (10k linear, 1 = full boost). The 4.5 V bias is signal ground.

    Power: U1 (input buffer and gain stage) runs on 9 V, so its outputs swing +-3 V around the bias (the
    TL072 macromodel's clamps); U2 (summing amplifier and treble control) runs on the charge pump's
    +16.2 V / -8.6 V, about +-10 V of swing, and is ideal here: the renders print its peaks."""
    c = Netlist()
    c.VIN("in")
    # Input buffer U1A: R1 10k, C1 0.1 uF, R2 1M to the bias, unity follower (ideal).
    c.R("in", "n1", 10e3)
    c.C("n1", "p1", 0.1e-6)
    c.R("p1", "0", 1e6)
    c.E("buf", "p1")
    # Bypass line, which bleeds into the output through the anti-pop resistor R243 even with the effect
    # on: C2 4.7 uF, R3 100k, R4 560, R243 68k.
    c.C("buf", "x", 4.7e-6)
    c.R("x", "0", 100e3)
    c.R("x", "out", 560.0 + 68e3)
    # Gain stage U1B (TL072 on 9 V, non-inverting): C3 0.1 uF, R6 10k || C5 68 nF into (+); the Gain
    # pot's first gang from (+) to its wiper on the bias (that part grows with gain), its other part plus
    # R10 2k and R11 15k || C7 82 nF form the gain leg from (-); feedback R12 422k || C8 390 pF.
    c.C("buf", "a", 0.1e-6)
    c.R("a", "p", 10e3)
    c.C("a", "p", 68e-9)
    c.R("p", "0", drive * 100e3)
    c.tl072("p", "m", "o1", "u1b")
    c.R("m", "h", 15e3)
    c.C("m", "h", 82e-9)
    c.R("h", "0", 2e3 + (1.0 - drive) * 100e3)
    c.R("o1", "m", 422e3)
    c.C("o1", "m", 390e-12)
    # Clipper: C9 1 uF, R13 1k, D2/D3 (1N34A, antiparallel, each with its series resistance) to the bias;
    # then C10 1 uF and R16 47k into the summing node, and C11 2.2 nF + R15 22k across to node f.
    c.C("o1", "c9", 1e-6)
    c.R("c9", "d", 1e3)
    c.DRS("d", "0", DIODE_1N34A["Is"], DIODE_1N34A["n"], DIODE_1N34A["rs"], "d2")
    c.DRS("0", "d", DIODE_1N34A["Is"], DIODE_1N34A["n"], DIODE_1N34A["rs"], "d3")
    c.C("d", "e", 1e-6)
    c.R("e", "s", 47e3)
    c.C("e", "c11", 2.2e-9)
    c.R("c11", "f", 22e3)
    # Feed-forward network 1 (clean, low-passed): R7 1.5k, C16 1 uF, R19 15k into the summing node.
    c.R("a", "g", 1.5e3)
    c.C("g", "0", 1e-6)
    c.R("g", "s", 15e3)
    # Feed-forward network 2 (clean, through the Gain pot's second gang, which turns it down as the gain
    # goes up): R5 5.1k || C4 68 nF, R8 1.5k, C6 390 nF + R9 1k, the gang from b to the bias with its
    # wiper at f; then R17 27k || (C12 27 nF + R18 12k) into the summing node.
    c.R("buf", "b", 5.1e3)
    c.C("buf", "b", 68e-9)
    c.R("b", "0", 1.5e3)
    c.C("b", "c6", 390e-9)
    c.R("c6", "0", 1e3)
    c.R("b", "f", drive * 100e3)
    c.R("f", "0", (1.0 - drive) * 100e3)
    c.R("f", "s", 27e3)
    c.C("f", "c12", 27e-9)
    c.R("c12", "s", 12e3)
    # Summing amplifier U2A (inverting): R20 392k || C13 820 pF.
    c.OPAMP("0", "s", "sum")
    c.R("sum", "s", 392e3)
    c.C("sum", "s", 820e-12)
    # Treble control U2B (inverting active shelf): R22 100k in, R24 100k feedback, and C14 3.9 nF from
    # the Treble pot's wiper to (-); the pot sits between R21 1.8k (from the input) and R23 4.7k (from the
    # output). Tone 1 puts the wiper at R21: full boost.
    c.R("sum", "tm", 100e3)
    c.R("sum", "w", 1.8e3 + (1.0 - tone) * 10e3)
    c.R("w", "tout", 4.7e3 + tone * 10e3)
    c.C("w", "tm", 3.9e-9)
    c.R("tout", "tm", 100e3)
    c.OPAMP("0", "tm", "tout")
    # Output: C15 4.7 uF, R25 560, Volume 10k at maximum (the whole track to ground), R28 100k, and a
    # 1M load (the amp's input).
    c.C("tout", "c15", 4.7e-6)
    c.R("c15", "out", 560.0)
    c.R("out", "0", 10e3)
    c.R("out", "0", 100e3)
    c.R("out", "0", 1e6)
    return c


def fuzz(drive, tone):
    """Electro-Harmonix Big Muff Pi, American version 3 (1977, red and black), as ElectroSmash draws it
    in its "Big Muff Pi Analysis" (schematic, parts list, and bias drawing; collector resistors 10k, which
    Kit Rae's BigMuffPage notes on some V3s, against 15k on the most common one). Drive is the Sustain pot
    (100k linear), tone the Tone pot (100k linear, 1 = the high-pass end, brightest). Transistors 2N5088
    (Ebers-Moll), diodes 1N914. Real DC bias from the 9 V supply; the input is AC-coupled (C1), the output
    too (C2), so both rest at 0 V. Designators are ElectroSmash's (Q4 is the input stage, as on EHX's
    boards)."""
    c = Netlist()
    c.VIN("in")
    c.VDC("vcc", 9.0)
    q = BJT_2N5088
    # Input booster Q4: R2 39k, C1 1 uF, R14 47k base to ground, R9 470k || C10 470 pF collector to
    # base, R13 10k collector load, R22 100 emitter.
    c.R("in", "i1", 39e3)
    c.C("i1", "b4", 1e-6)
    c.R("b4", "0", 47e3)
    c.R("c4", "b4", 470e3)
    c.C("c4", "b4", 470e-12)
    c.R("vcc", "c4", 10e3)
    c.R("e4", "0", 100.0)
    c.Q("c4", "b4", "e4", **q)
    # Sustain: C4 1 uF into the 100k pot (wiper toward the top as sustain rises), R23 1k under it; the
    # wiper through C5 100 nF and R19 10k into Q3's base.
    c.C("c4", "s1", 1e-6)
    c.R("s1", "sw", (1.0 - drive) * 100e3)
    c.R("sw", "s2", drive * 100e3)
    c.R("s2", "0", 1e3)
    c.C("sw", "s3", 0.1e-6)
    c.R("s3", "b3", 10e3)
    # First clipping stage Q3: R20 100k, R17 470k || C12 470 pF, C6 1 uF in series with D3/D4 (1N914,
    # antiparallel) from base to collector, R18 10k, R21 150.
    c.R("b3", "0", 100e3)
    c.R("c3", "b3", 470e3)
    c.C("c3", "b3", 470e-12)
    c.C("b3", "d3", 1e-6)
    c.D("d3", "c3", **DIODE_1N914)
    c.D("c3", "d3", **DIODE_1N914)
    c.R("vcc", "c3", 10e3)
    c.R("e3", "0", 150.0)
    c.Q("c3", "b3", "e3", **q)
    # C13 100 nF and R12 10k into the second clipping stage Q2: R16 100k, R15 470k || C11 470 pF,
    # C7 1 uF with D1/D2, R11 10k, R10 150.
    c.C("c3", "k2", 0.1e-6)
    c.R("k2", "b2", 10e3)
    c.R("b2", "0", 100e3)
    c.R("c2", "b2", 470e3)
    c.C("c2", "b2", 470e-12)
    c.C("b2", "d2", 1e-6)
    c.D("d2", "c2", **DIODE_1N914)
    c.D("c2", "d2", **DIODE_1N914)
    c.R("vcc", "c2", 10e3)
    c.R("e2", "0", 150.0)
    c.Q("c2", "b2", "e2", **q)
    # Tone stack: low-pass R8 39k into C8 10 nF, high-pass C9 4 nF into R5 22k, the Tone pot between
    # them (its wiper at the high-pass end for tone 1), wiper through C3 100 nF to the output stage.
    c.R("c2", "tl", 39e3)
    c.C("tl", "0", 10e-9)
    c.C("c2", "th", 4e-9)
    c.R("th", "0", 22e3)
    c.R("th", "tw", (1.0 - tone) * 100e3)
    c.R("tw", "tl", tone * 100e3)
    c.C("tw", "b1", 0.1e-6)
    # Output booster Q1: R7 430k and R3 100k bias, R6 15k collector, R4 3.3k emitter (no feedback).
    c.R("vcc", "b1", 430e3)
    c.R("b1", "0", 100e3)
    c.R("vcc", "c1", 15e3)
    c.R("e1", "0", 3.3e3)
    c.Q("c1", "b1", "e1", **q)
    # Output: C2 100 nF, Volume 100k at maximum (the whole track to ground), 1M load.
    c.C("c1", "out", 0.1e-6)
    c.R("out", "0", 100e3)
    c.R("out", "0", 1e6)
    return c


CIRCUITS = {"mid_drive": mid_drive, "distortion": distortion, "transparent": transparent, "fuzz": fuzz}


# ---------------------------------------------------------------------------------------------------
# The simulator


def newton_update(A, b, x):
    """The linearized system A x_new = b solved as an update, x_new = x + A^-1 (b - A x): the same Newton
    step, but the solve's rounding scales with the step instead of the solution. A pot turned to its end
    (1 mOhm, R_MIN) next to microsiemens makes A ill-conditioned enough that solving for x_new directly
    leaves ~2e-9 V of noise on the fuzz's nodes every iteration, above the convergence tolerance."""
    return x + np.linalg.solve(A, (b - np.einsum("bnm,bm->bn", A, x))[..., None])[..., 0]


def pnjlim_array(vnew, vold, vt, vcrit):
    """SPICE3's junction voltage limiting (devsup.c, DEVpnjlim) on arrays; returns (limited, mask)."""
    limit = (vnew > vcrit) & (np.abs(vnew - vold) > 2.0 * vt)
    arg = 1.0 + (vnew - vold) / vt
    from_positive = np.where(arg > 0.0, vold + vt * np.log(np.maximum(arg, 1e-300)), vcrit)
    from_negative = vt * np.log(np.maximum(vnew / vt, 1e-300))
    return np.where(limit, np.where(vold > 0.0, from_positive, from_negative), vnew), limit


class Compiled:
    """A batch of netlists with identical topology (same elements in the same order, different values),
    turned into the matrices the solver needs. Batching runs several renders through one Python loop."""

    def __init__(self, netlists, T):
        first = netlists[0]
        self.batch = len(netlists)
        self.num_nodes = len(first.nodes)
        kinds = [e[0] for e in first.elements]
        for nl in netlists:
            assert [e[0] for e in nl.elements] == kinds and nl.nodes == first.nodes, "batch must share topology"

        # Extra unknowns: one branch current per VIN, VDC, E, OPAMP.
        extra = 0
        self.rows = []
        for e in first.elements:
            if e[0] in ("VIN", "VDC", "E", "OPAMP"):
                self.rows.append(self.num_nodes + extra)
                extra += 1
            else:
                self.rows.append(None)
        self.size = self.num_nodes + extra
        N, B = self.size, self.batch

        self.G = np.zeros((B, N, N))       # frequency-independent part (no capacitors)
        self.Cm = np.zeros((B, N, N))      # capacitance matrix, for AC
        self.b_static = np.zeros((B, N))
        self.vin_row = None
        caps, diodes, tanhs, bjts = [], [], [], []

        def stamp_g(M, a, b, g):
            if a >= 0:
                M[:, a, a] += g
            if b >= 0:
                M[:, b, b] += g
            if a >= 0 and b >= 0:
                M[:, a, b] -= g
                M[:, b, a] -= g

        for idx, e in enumerate(first.elements):
            values = [nl.elements[idx] for nl in netlists]
            kind = e[0]
            if kind == "R":
                stamp_g(self.G, e[1], e[2], np.array([1.0 / v[3] for v in values]))
            elif kind == "C":
                c = np.array([v[3] for v in values])
                stamp_g(self.Cm, e[1], e[2], c)
                caps.append((e[1], e[2], c))
            elif kind in ("VIN", "VDC"):
                k = self.rows[idx]
                a = e[1]
                self.G[:, a, k] += 1.0
                self.G[:, k, a] += 1.0
                if kind == "VIN":
                    self.vin_row = k
                else:
                    self.b_static[:, k] = e[3]
            elif kind == "E":
                k = self.rows[idx]
                a, c_node, gain = e[1], e[3], e[5]
                self.G[:, a, k] += 1.0
                self.G[:, k, a] += 1.0
                if c_node >= 0:
                    self.G[:, k, c_node] -= gain
            elif kind == "OPAMP":
                k = self.rows[idx]
                p, m, o = e[1], e[2], e[3]
                self.G[:, o, k] += 1.0
                if p >= 0:  # (+) may be grounded: an inverting stage on the bias rail
                    self.G[:, k, p] += 1.0
                if m >= 0:
                    self.G[:, k, m] -= 1.0
            elif kind == "D":
                diodes.append((e[1], e[2], e[3], e[4]))
            elif kind == "GTANH":
                tanhs.append((e[1], e[2], e[3], e[4], e[5]))
            elif kind == "Q":
                assert all(v[4:] == e[4:] for v in values), "transistor parameters must match across the batch"
                bjts.append((e[1], e[2], e[3], e[4], e[5], e[6]))

        # Capacitors as an incidence matrix (N x ncaps): +1 at the first node, -1 at the second.
        self.cap_inc = np.zeros((N, len(caps)))
        for j, (a, b, _) in enumerate(caps):
            if a >= 0:
                self.cap_inc[a, j] = 1.0
            if b >= 0:
                self.cap_inc[b, j] = -1.0
        self.cap_c = np.stack([c for _, _, c in caps], axis=1) if caps else np.zeros((B, 0))  # (B, ncaps)

        self.dio_inc = np.zeros((N, len(diodes)))
        for j, (a, k, _, _) in enumerate(diodes):
            if a >= 0:
                self.dio_inc[a, j] = 1.0
            if k >= 0:
                self.dio_inc[k, j] = -1.0
        self.dio_is = np.array([d[2] for d in diodes])
        self.dio_vt = np.array([d[3] * VT for d in diodes])
        self.dio_vcrit = self.dio_vt * np.log(self.dio_vt / (math.sqrt(2.0) * self.dio_is))
        self.tanhs = tanhs
        self.bjts = bjts
        self.bjt_vcrit = [VT * math.log(VT / (math.sqrt(2.0) * q[3])) for q in bjts]
        # The operating point puts GMIN from every node to ground, so a node joined only by capacitors
        # can't float. A circuit without such nodes can switch it off and get the exact bias (the fuzz,
        # whose C++ model solves its own bias without it).
        self.node_gmin = True
        self.T = T
        self.set_timestep(T)

    @staticmethod
    def node_v(x, n):
        """A node's voltage from the solution x (B, N); ground (-1) is 0."""
        return x[:, n] if n >= 0 else np.zeros(x.shape[0])

    def bjt_junctions(self, x):
        """(vbe, vbc) of every transistor at the node voltages x."""
        out = []
        for c, b, e, *_ in self.bjts:
            vb = self.node_v(x, b)
            out.append((vb - self.node_v(x, e), vb - self.node_v(x, c)))
        return out

    def bjt_stamp(self, A, b_vec, j, vbe, vbc):
        """Adds transistor j, linearized at the junction voltages (vbe, vbc), to the Newton system: its
        terminal currents (Ic into the collector, Ib into the base, -(Ic + Ib) into the emitter) as
        conductances between the nodes plus equivalent current sources. Returns (ic, ib)."""
        c, b, e, Is, bf, br = self.bjts[j]
        ebe = np.exp(np.minimum(vbe / VT, 700.0))
        ebc = np.exp(np.minimum(vbc / VT, 700.0))
        ibe = Is * (ebe - 1.0) + GMIN * vbe
        ibc = Is * (ebc - 1.0) + GMIN * vbc
        gbe = Is / VT * ebe + GMIN
        gbc = Is / VT * ebc + GMIN
        ic = ibe - ibc * (1.0 + 1.0 / br)
        ib = ibe / bf + ibc / br
        # d/d(vbe) and d/d(vbc) of the collector and base currents; vbe = vb - ve, vbc = vb - vc.
        dic = (gbe, -gbc * (1.0 + 1.0 / br))
        dib = (gbe / bf, gbc / br)
        for row, i0, d in ((c, ic, dic), (b, ib, dib), (e, -(ic + ib), (-(dic[0] + dib[0]), -(dic[1] + dib[1])))):
            if row < 0:
                continue
            for col, coef in ((b, d[0] + d[1]), (e, -d[0]), (c, -d[1])):
                if col >= 0:
                    A[:, row, col] += coef
            b_vec[:, row] -= i0 - d[0] * vbe - d[1] * vbc
        return ic, ib

    def bjt_limit(self, x_new, junctions):
        """SPICE3's pnjlim on both junctions of every transistor; returns the limited junction voltages
        and whether any was limited."""
        out, limited = [], np.zeros(x_new.shape[0], dtype=bool)
        for j, (vbe_new, vbc_new) in enumerate(self.bjt_junctions(x_new)):
            vbe_old, vbc_old = junctions[j]
            vcrit = self.bjt_vcrit[j]
            vbe, l1 = pnjlim_array(vbe_new, vbe_old, VT, vcrit)
            vbc, l2 = pnjlim_array(vbc_new, vbc_old, VT, vcrit)
            out.append((vbe, vbc))
            limited |= l1 | l2
        return out, limited

    def set_timestep(self, T):
        self.T = T
        self.cap_g = 2.0 * self.cap_c / T                                                  # (B, ncaps)
        self.A_lin = self.G + np.einsum("nj,bj,mj->bnm", self.cap_inc, self.cap_g, self.cap_inc)

    def diode_eval(self, vd):
        """Current and conductance of every diode at junction voltages vd (B, nd)."""
        e = np.exp(np.minimum(vd / self.dio_vt, 700.0))
        i = self.dio_is * (e - 1.0) + GMIN * vd
        g = self.dio_is / self.dio_vt * e + GMIN
        return i, g

    def pnjlim(self, vnew, vold):
        """SPICE3's junction voltage limiting (devsup.c, DEVpnjlim)."""
        vt, vcrit = self.dio_vt, self.dio_vcrit
        limit = (vnew > vcrit) & (np.abs(vnew - vold) > 2.0 * vt)
        arg = 1.0 + (vnew - vold) / vt
        from_positive = vold + vt * np.log(np.maximum(arg, 1e-300))
        from_positive = np.where(arg > 0.0, from_positive, vcrit)
        from_negative = vt * np.log(np.maximum(vnew / vt, 1e-300))
        out = np.where(limit, np.where(vold > 0.0, from_positive, from_negative), vnew)
        return out, limit

    @staticmethod
    def tanh_limit(u_new, u_old, vs):
        """Limiting for the tanh transconductor's controlling voltage, in the spirit of SPICE's device
        limiting: linearized deep in saturation, tanh looks like a constant current, so a Newton step can
        fly to the opposite saturated side and back forever. From saturation, a step may return at most to
        the knee (2 vs) on the same side; near the linear region, steps are capped at 2 vs."""
        delta = np.maximum(2.0 * vs, np.abs(u_old) - 2.0 * vs)
        u = np.clip(u_new, u_old - delta, u_old + delta)
        return u, u != u_new

    def transient(self, inputs, probe, max_iterations=100, stats=None):
        """inputs (B, L) volts at the simulation rate; returns the probe node's voltage (B, L), or
        (B, P, L) for a list of P probe nodes."""
        B, N = self.batch, self.size
        L = inputs.shape[1]
        # Start at rest: the DC operating point, capacitors charged to it, no current flowing.
        x, vd, u_op, qj = self.operating_point()
        cap_v = x[:, : self.num_nodes] @ self.cap_inc[: self.num_nodes]
        cap_i = np.zeros_like(self.cap_g)
        probes = list(probe) if isinstance(probe, (list, tuple)) else [probe]
        out = np.zeros((B, len(probes), L))
        nd = self.dio_inc.shape[1]
        iterations_total, iterations_max = 0, 0
        for step in range(L):
            # History currents of the capacitors' trapezoidal companions.
            J = self.cap_g * cap_v + cap_i                                                   # (B, ncaps)
            b0 = self.b_static + J @ self.cap_inc.T
            b0[:, self.vin_row] = inputs[:, step]
            for it in range(max_iterations):
                A = self.A_lin.copy()
                b = b0.copy()
                if nd:
                    i_d, g_d = self.diode_eval(vd)
                    A += np.einsum("nj,bj,mj->bnm", self.dio_inc, g_d, self.dio_inc)
                    b -= (i_d - g_d * vd) @ self.dio_inc.T
                for j, (xn, p, m, it_, vs) in enumerate(self.tanhs):
                    u = u_op[j]
                    th = np.tanh(u / vs)
                    gm = it_ / vs * (1.0 - th * th)
                    if p >= 0:
                        A[:, xn, p] -= gm
                    if m >= 0:
                        A[:, xn, m] += gm
                    b[:, xn] += it_ * th - gm * u
                for j in range(len(self.bjts)):
                    self.bjt_stamp(A, b, j, *qj[j])
                x_new = newton_update(A, b, x)
                limited = np.zeros(B, dtype=bool)
                if nd:
                    vd_new = x_new[:, : self.num_nodes] @ self.dio_inc[: self.num_nodes]
                    vd_lim, lim = self.pnjlim(vd_new, vd)
                    limited = lim.any(axis=1)
                    vd = vd_lim
                for j, (xn, p, m, it_, vs) in enumerate(self.tanhs):
                    u_new = (x_new[:, p] if p >= 0 else 0.0) - (x_new[:, m] if m >= 0 else 0.0)
                    u_op[j], lim = self.tanh_limit(u_new, u_op[j], vs)
                    limited |= lim
                if self.bjts:
                    qj, lim = self.bjt_limit(x_new, qj)
                    limited |= lim
                dv = np.abs(x_new[:, : self.num_nodes] - x[:, : self.num_nodes])
                converged = (dv <= 1e-9 + 1e-9 * np.abs(x_new[:, : self.num_nodes])).all() and not limited.any()
                x = x_new
                if converged:
                    break
            else:
                raise RuntimeError(f"Newton did not converge at step {step}")
            iterations_total += it + 1
            iterations_max = max(iterations_max, it + 1)
            if nd:
                vd = x[:, : self.num_nodes] @ self.dio_inc[: self.num_nodes]
            if self.bjts:
                qj = self.bjt_junctions(x)
            new_v = x[:, : self.num_nodes] @ self.cap_inc[: self.num_nodes]
            cap_i = self.cap_g * new_v - J
            cap_v = new_v
            out[:, :, step] = x[:, probes]
        if stats is not None:
            stats["mean_iterations"] = iterations_total / L
            stats["max_iterations"] = iterations_max
        return out if isinstance(probe, (list, tuple)) else out[:, 0, :]

    def operating_point(self, max_iterations=200):
        """DC operating point with the input at 0 V, as SPICE's .op: capacitors open, Newton with the
        same limiting, and GMIN from every node to ground so a node joined only by capacitors can't float.
        Transistors start from SPICE's initial junction guess (vbe at its critical voltage, vbc at 0), and
        if Newton fails, the DC sources are ramped up from 10% in steps (SPICE's source stepping).
        Returns the node solution, the junction voltages, the transconductors' controlling voltages, and
        the transistors' (vbe, vbc)."""
        B, N = self.batch, self.size
        nd = self.dio_inc.shape[1]
        state = (np.zeros((B, N)), np.zeros((B, nd)), [np.zeros(B) for _ in self.tanhs],
                 [(np.full(B, vc), np.zeros(B)) for vc in self.bjt_vcrit])
        try:
            return self._dc_newton(state, 1.0, max_iterations)
        except RuntimeError:
            for scale in np.linspace(0.1, 1.0, 10):
                state = self._dc_newton(state, scale, max_iterations)
            return state

    def _dc_newton(self, state, source_scale, max_iterations):
        B, N = self.batch, self.size
        nd = self.dio_inc.shape[1]
        x, vd, u_op, qj = state
        x, vd, u_op, qj = x.copy(), vd.copy(), [u.copy() for u in u_op], list(qj)
        for _ in range(max_iterations):
            A = self.G.copy()
            if self.node_gmin:
                idx = np.arange(self.num_nodes)
                A[:, idx, idx] += GMIN
            b = self.b_static.copy() * source_scale
            if nd:
                i_d, g_d = self.diode_eval(vd)
                A += np.einsum("nj,bj,mj->bnm", self.dio_inc, g_d, self.dio_inc)
                b -= (i_d - g_d * vd) @ self.dio_inc.T
            for j, (xn, p, m, it_, vs) in enumerate(self.tanhs):
                th = np.tanh(u_op[j] / vs)
                gm = it_ / vs * (1.0 - th * th)
                if p >= 0:
                    A[:, xn, p] -= gm
                if m >= 0:
                    A[:, xn, m] += gm
                b[:, xn] += it_ * th - gm * u_op[j]
            for j in range(len(self.bjts)):
                self.bjt_stamp(A, b, j, *qj[j])
            x_new = newton_update(A, b, x)
            limited = np.zeros(B, dtype=bool)
            if nd:
                vd, lim = self.pnjlim(x_new[:, : self.num_nodes] @ self.dio_inc[: self.num_nodes], vd)
                limited |= lim.any(axis=1)
            for j, (xn, p, m, it_, vs) in enumerate(self.tanhs):
                u_new = (x_new[:, p] if p >= 0 else 0.0) - (x_new[:, m] if m >= 0 else 0.0)
                u_op[j], lim = self.tanh_limit(u_new, u_op[j], vs)
                limited |= lim
            if self.bjts:
                qj, lim = self.bjt_limit(x_new, qj)
                limited |= lim
            converged = (np.abs(x_new - x)[:, : self.num_nodes] <= 1e-12 + 1e-9 * np.abs(x_new[:, : self.num_nodes])).all()
            x = x_new
            if converged and not limited.any():
                if self.bjts:
                    qj = self.bjt_junctions(x)
                return x, vd, u_op, qj
        raise RuntimeError("no DC operating point")

    def ac(self, freqs, probe):
        """Small-signal response from the input source to the probe node, linearized at the DC operating
        point (where the rail clamps' diodes sit reverse-biased by their sources, not at 0 V)."""
        B, N = self.batch, self.size
        _, vd, u_op, qj = self.operating_point()
        G = self.G.copy()
        if self.dio_inc.shape[1]:
            _, g0 = self.diode_eval(vd)
            G += np.einsum("nj,bj,mj->bnm", self.dio_inc, g0, self.dio_inc)
        for j, (xn, p, m, it_, vs) in enumerate(self.tanhs):
            th = np.tanh(u_op[j] / vs)
            gm = it_ / vs * (1.0 - th * th)
            if p >= 0:
                G[:, xn, p] -= gm
            if m >= 0:
                G[:, xn, m] += gm
        for j in range(len(self.bjts)):
            self.bjt_stamp(G, np.zeros((B, N)), j, *qj[j])  # only the conductances matter here
        b = np.zeros((B, N), dtype=complex)
        b[:, self.vin_row] = 1.0
        H = np.zeros((B, len(freqs)), dtype=complex)
        for i, f in enumerate(freqs):
            A = G + 2j * math.pi * f * self.Cm
            H[:, i] = np.linalg.solve(A, b[..., None])[..., 0][:, probe]
        return H


def compile_batch(netlists, T):
    return Compiled(netlists, T)


# ---------------------------------------------------------------------------------------------------
# Rate conversion around the simulation: a linear-phase Kaiser FIR, -140 dB.

def resampling_filter(factor):
    rate = FS * factor
    beta = 0.1102 * (140.0 - 8.7)
    taps = int(math.ceil((140.0 - 7.95) / (2.285 * 2.0 * math.pi * 6000.0 / rate))) | 1
    return firwin(taps, 24000.0, fs=rate, window=("kaiser", beta))


def render(netlists, inputs48, factor=16, stats=None, internal=(), node_gmin=True):
    """Run a batch of netlists on 48 kHz inputs (volts, (B, L)); returns 48 kHz outputs (B, L). Internal
    nodes named in `internal` come back in stats["internal"] at the simulation rate, (B, len, L * factor)."""
    h = resampling_filter(factor)
    up = np.stack([resample_poly(x, factor, 1, window=h) for x in inputs48])
    sim = compile_batch(netlists, 1.0 / (FS * factor))
    sim.node_gmin = node_gmin
    t0 = time.time()
    nodes = [netlists[0].nodes["out"]] + [netlists[0].nodes[n] for n in internal]
    y = sim.transient(up, nodes, stats=stats)
    if stats is not None:
        stats["seconds"] = time.time() - t0
        stats["internal"] = y[:, 1:, :]
    return np.stack([resample_poly(v, 1, factor, window=h) for v in y[:, 0, :]])


# ---------------------------------------------------------------------------------------------------
# Self-checks against known answers


def check_simulator():
    print("Simulator checks (known answers):")
    T = 1.0 / (FS * 16)

    # 1. RC low-pass, 1 kHz corner, driven by a 1 kHz sine: steady state |H| = 1/sqrt2, phase -45 deg.
    c = Netlist()
    c.VIN("in")
    c.R("in", "out", 1e3)
    c.C("out", "0", 1.0 / (2 * math.pi * 1e3 * 1e3))
    sim = compile_batch([c], T)
    n = np.arange(int(0.02 / T))
    x = np.sin(2 * math.pi * 1e3 * n * T)
    y = sim.transient(x[None, :], c.nodes["out"])[0]
    tail = slice(len(n) // 2, None)
    ref = np.sin(2 * math.pi * 1e3 * n * T - math.pi / 4) / math.sqrt(2)
    err = np.max(np.abs(y[tail] - ref[tail]))
    print(f"  RC low-pass at its 1 kHz corner, transient vs |H| = 0.7071 at -45 deg: max error {err:.2e} V")
    assert err < 1e-5
    H = sim.ac([1e3], c.nodes["out"])[0, 0]
    print(f"  RC low-pass, AC analysis at 1 kHz: |H| {abs(H):.6f}, phase {math.degrees(np.angle(H)):.3f} deg")
    assert abs(abs(H) - 1 / math.sqrt(2)) < 1e-9

    # 2. Diode clipper (2.2k into 1N914 pair) under a slow ramp: the DC curve of the diode equation.
    c = Netlist()
    c.VIN("in")
    c.R("in", "out", 2.2e3)
    c.D("out", "0", **DIODE_1N914)
    c.D("0", "out", **DIODE_1N914)
    sim = compile_batch([c], T)
    ramp = np.linspace(-6.0, 6.0, 20000)
    y = sim.transient(ramp[None, :], c.nodes["out"])[0]
    vt = DIODE_1N914["n"] * VT

    def static(vin):
        lo, hi = -abs(vin), abs(vin)
        for _ in range(200):
            v = 0.5 * (lo + hi)
            f = (vin - v) / 2.2e3 - 2 * DIODE_1N914["Is"] * math.sinh(v / vt) - 2 * GMIN * v
            lo, hi = (v, hi) if f > 0 else (lo, v)
        return 0.5 * (lo + hi)

    err = max(abs(y[i] - static(ramp[i])) for i in range(0, len(ramp), 97))
    print(f"  Diode clipper DC curve (ramp -6..6 V): max error vs. the diode equation {err:.2e} V; at 6 V the output is {y[-1]:.4f} V")
    assert err < 1e-6

    # 3. LM308 macromodel: closed-loop bandwidth at gain 101 (AC), and the slew rate (transient step).
    c = Netlist()
    c.VIN("in")
    c.lm308("in", "m", "out")
    c.R("out", "m", 100e3)
    c.R("m", "0", 1e3)
    sim = compile_batch([c], T)
    freqs = np.logspace(2, 5, 3000)
    H = np.abs(sim.ac(freqs, c.nodes["out"])[0])
    f3 = freqs[np.argmin(np.abs(H - H[0] / math.sqrt(2)))]
    gbw = LM308["slew"] / (4 * math.pi * VT)
    print(f"  LM308 at gain 101: DC {H[0]:.2f}, -3 dB at {f3:.0f} Hz (GBW / 101 = {gbw / 101:.0f} Hz; GBW {gbw / 1e6:.3f} MHz)")
    assert abs(f3 / (gbw / 101) - 1) < 0.02

    c = Netlist()
    c.VIN("in")
    c.lm308("in", "out", "out")
    sim = compile_batch([c], T)
    step = np.where(np.arange(4000) > 100, 2.0, 0.0)
    y = sim.transient(step[None, :], c.nodes["out"])[0]
    slope = np.max(np.diff(y)) / T
    print(f"  LM308 follower, 2 V step: fastest slope {slope / 1e6:.3f} V/us (slew rate {LM308['slew'] / 1e6} V/us)")
    assert abs(slope / LM308["slew"] - 1) < 0.01
    ramp = np.linspace(0, 5, 8000)
    y = sim.transient(ramp[None, :], c.nodes["out"])[0]
    print(f"  LM308 follower driven to 5 V: output stops at {y.max():.3f} V (rail clamp {LM308['vsat']} V)")
    assert abs(y.max() - LM308["vsat"]) < 0.05

    check_tl072()
    check_bjt()


def check_tl072():
    """The TL072 static macromodel: closed-loop gain against A / (1 + A beta), and the output clamp."""
    T = 1.0 / (FS * 16)
    a0, vs, it = TL072["a0"], TL072["vs"], TL072["it"]
    gm = it / vs
    vclamp = TL072_VSAT_9V - VT * math.log(it / 1e-14 + 1.0)
    g_clamp = 2.0 * (1e-14 / VT * math.exp(-vclamp / VT) + GMIN)  # both clamps, reverse-biased at rest
    A = gm / (gm / a0 + g_clamp)
    c = Netlist()
    c.VIN("in")
    c.tl072("in", "m", "out", "u")
    c.R("out", "m", 100e3)
    c.R("m", "0", 1e3)
    sim = compile_batch([c], T)
    H = sim.ac([1e3], c.nodes["out"])[0, 0]
    beta = 1e3 / 101e3
    predicted = A / (1.0 + A * beta)
    print(f"  TL072 at gain 101: AC {abs(H):.6f} vs A / (1 + A beta) = {predicted:.6f} (A = {A:.4g}; an ideal op-amp gives 101)")
    assert abs(abs(H) / predicted - 1.0) < 1e-9
    ramp = np.concatenate([np.linspace(0.0, 0.08, 4000), np.linspace(0.08, -0.08, 8000)])
    y = sim.transient(ramp[None, :], c.nodes["out"])[0]
    linear = np.abs(predicted * ramp) <= 0.5 * TL072_VSAT_9V
    knee = np.abs(predicted * ramp) <= TL072_VSAT_9V - 0.15
    print(f"  TL072 at gain 101 driven to +-8 V: output stops at {y.max():.4f} / {y.min():.4f} V (clamps at +-{TL072_VSAT_9V} V); "
          f"up to half that it is within {np.max(np.abs(y - predicted * ramp)[linear]):.1e} V of linear, up to 0.15 V short of it "
          f"within {np.max(np.abs(y - predicted * ramp)[knee]):.3f} V")
    assert abs(y.max() - TL072_VSAT_9V) < 0.01 and abs(y.min() + TL072_VSAT_9V) < 0.01
    assert np.max(np.abs(y - predicted * ramp)[linear]) < 1e-6


def ebers_moll(vbe, vbc, Is, bf, br):
    """The transport model written out on its own (with SPICE's GMIN across each junction), for checks."""
    ibe = Is * math.expm1(vbe / VT) + GMIN * vbe
    ibc = Is * math.expm1(vbc / VT) + GMIN * vbc
    return ibe - ibc * (1.0 + 1.0 / br), ibe / bf + ibc / br  # ic, ib


def solve_newton(f, x0, tol=1e-12, iterations=1000):
    """Newton with a finite-difference Jacobian, each step capped at 50 mV per unknown (the unknowns are
    junction voltages): an independent little solver for the checks' hand-derived equations, sharing
    nothing with the simulator's MNA."""
    x = np.array(x0, dtype=float)
    for _ in range(iterations):
        r = np.array(f(x))
        J = np.zeros((len(x), len(x)))
        for k in range(len(x)):
            h = 1e-8 * max(1.0, abs(x[k]))
            xp = x.copy()
            xp[k] += h
            J[:, k] = (np.array(f(xp)) - r) / h
        step = np.clip(np.linalg.solve(J, -r), -0.05, 0.05)
        x = x + step
        if np.max(np.abs(step)) < tol:
            return x
    raise RuntimeError("solve_newton did not converge")


def check_bjt():
    """The Ebers-Moll transistor against hand calculations on a common-emitter stage: the DC operating
    point, the small-signal gain, saturation, and a transient sweep from cutoff to saturation."""
    q = dict(Is=1e-14, bf=100.0, br=1.0)
    vcc, rb, rc, re = 9.0, 100e3, 4.7e3, 1e3

    def stage():
        """Base resistor from the input source, collector resistor from Vcc, emitter resistor."""
        c = Netlist()
        c.VIN("in")
        c.VDC("vcc", vcc)
        c.R("in", "b", rb)
        c.R("vcc", "c", rc)
        c.R("e", "0", re)
        c.Q("c", "b", "e", **q)
        return c

    def hand(vin):
        # KCL with the transport model, unknowns (vbe, vbc); GMIN from every node to ground as SPICE's .op.
        def f(v):
            vbe, vbc = v
            ic, ib = ebers_moll(vbe, vbc, **q)
            ve = (ic + ib) / (1.0 / re + GMIN)
            vb = ve + vbe
            vc = vb - vbc
            return [((vin - vb) / rb - ib - GMIN * vb) * 1e6, ((vcc - vc) / rc - ic - GMIN * vc) * 1e6]
        vbe, vbc = solve_newton(f, [0.6, -3.0])
        ic, ib = ebers_moll(vbe, vbc, **q)
        ve = (ic + ib) / (1.0 / re + GMIN)
        return dict(vb=ve + vbe, ve=ve, vc=ve + vbe - vbc, ic=ic, ib=ib, vbe=vbe, vbc=vbc)

    # 1. Operating point in the forward-active region (1.5 V on the base resistor; the input as a DC
    #    source so .op sees it).
    c = Netlist()
    c.VDC("in", 1.5)
    c.VDC("vcc", vcc)
    c.R("in", "b", rb)
    c.R("vcc", "c", rc)
    c.R("e", "0", re)
    c.Q("c", "b", "e", **q)
    x, _, _, _ = compile_batch([c], 1.0).operating_point()
    h = hand(1.5)
    err = max(abs(x[0, c.nodes["b"]] - h["vb"]), abs(x[0, c.nodes["e"]] - h["ve"]), abs(x[0, c.nodes["c"]] - h["vc"]))
    print(f"  Common-emitter stage (Is 10 fA, Bf 100, Br 1; 1.5 V through 100k, 4.7k / 1k): operating point "
          f"Vb {h['vb']:.4f}, Ve {h['ve']:.4f}, Vc {h['vc']:.4f} V, Ic {h['ic'] * 1e3:.4f} mA; simulator within {err:.1e} V")
    assert err < 1e-9

    # 2. Small-signal gain at that point: the hybrid-pi model (gm = dIbe/dvbe and the reverse junction's
    #    conductance) at the hand-solved operating point. The base resistor now goes to a 1.5 V source
    #    (AC ground) and the signal comes in through a 10 uF capacitor. Unknowns vb, ve, vc for vin = 1 V
    #    (Vcc is AC ground), KCL at each node, at 1 kHz.
    nl = Netlist()
    nl.VIN("in")
    nl.VDC("bias", 1.5)
    nl.VDC("vcc", vcc)
    nl.C("in", "b", 10e-6)
    nl.R("bias", "b", rb)
    nl.R("vcc", "c", rc)
    nl.R("e", "0", re)
    nl.Q("c", "b", "e", **q)
    gbe = q["Is"] / VT * math.exp(h["vbe"] / VT) + GMIN
    gbc = q["Is"] / VT * math.exp(h["vbc"] / VT) + GMIN
    dic = (gbe, -gbc * (1.0 + 1.0 / q["br"]))  # d Ic / d(vbe, vbc)
    dib = (gbe / q["bf"], gbc / q["br"])         # d Ib / d(vbe, vbc)
    die = (-(dic[0] + dib[0]), -(dic[1] + dib[1]))
    yc = 2j * math.pi * 1e3 * 10e-6
    M = np.diag([1.0 / rb + yc, 1.0 / re, 1.0 / rc]).astype(complex)
    for row, d in ((0, dib), (1, die), (2, dic)):
        M[row] += [d[0] + d[1], -d[0], -d[1]]  # vbe = vb - ve, vbc = vb - vc
    gain = np.linalg.solve(M, np.array([yc, 0.0, 0.0]))[2]
    # The textbook closed form, for orientation: the degenerated stage's gain -Rc gm / (1 + gm Re (1 + 1/Bf)).
    gm = gbe
    approx = -rc * gm / (1.0 + gm * re * (1.0 + 1.0 / q["bf"]))
    H = compile_batch([nl], 1.0).ac([1e3], nl.nodes["c"])[0, 0]
    print(f"  Its small-signal gain at 1 kHz: hybrid-pi {abs(gain):.6f} (closed form Rc gm / (1 + gm Re (1 + 1/Bf)) = {abs(approx):.4f}); "
          f"AC analysis {abs(H):.6f}, relative error {abs(H / gain - 1):.1e}")
    assert abs(H / gain - 1.0) < 1e-9

    # 3. Saturation: 9 V on the base resistor. Both junctions forward; the transport model gives
    #    Vce = VT ln((Ibe + Is) / (Ibc + Is)) with Ibc = (Bf Ib - Ic) / (1 + (Bf + 1) / Br), Ibe = Bf (Ib - Ibc / Br).
    c = Netlist()
    c.VDC("in", 9.0)
    c.VDC("vcc", vcc)
    c.R("in", "b", rb)
    c.R("vcc", "c", rc)
    c.R("e", "0", re)
    c.Q("c", "b", "e", **q)
    x, _, _, _ = compile_batch([c], 1.0).operating_point()
    vb, ve, vc = (x[0, c.nodes[n]] for n in ("b", "e", "c"))
    ib = (9.0 - vb) / rb - GMIN * vb
    ic = (vcc - vc) / rc - GMIN * vc
    ibc = (q["bf"] * ib - ic) / (1.0 + (q["bf"] + 1.0) / q["br"])
    ibe = q["bf"] * (ib - ibc / q["br"])
    vce = VT * math.log((ibe + q["Is"]) / (ibc + q["Is"]))
    print(f"  Saturated (9 V on the base resistor, forced beta {ic / ib:.2f}): Vce {vc - ve:.6f} V, the transport model's "
          f"VT ln((Ibe + Is) / (Ibc + Is)) = {vce:.6f} V (GMIN aside)")
    assert abs((vc - ve) - vce) < 1e-6

    # 4. Transient: a slow ramp of the base drive from 0 to 9 V crosses cutoff, the active region, and
    #    saturation; every point must sit on the hand-solved DC curve (no capacitors, so the transient is
    #    the DC solution at each step; the hand curve's GMIN to ground is 4e-8 V of difference at most).
    nl = stage()
    sim = compile_batch([nl], 1.0 / (FS * 16))
    ramp = np.linspace(0.0, 9.0, 6000)
    y = sim.transient(ramp[None, :], [nl.nodes["c"], nl.nodes["e"]])[0]
    worst = 0.0
    for i in range(0, len(ramp), 250):
        h = hand(ramp[i])
        worst = max(worst, abs(y[0, i] - h["vc"]), abs(y[1, i] - h["ve"]))
    print(f"  Ramp 0 to 9 V through cutoff, active, and saturation: transient within {worst:.1e} V of the hand-solved DC curve "
          f"(collector from {y[0, 0]:.3f} V down to {y[0, -1]:.3f} V)")
    assert worst < 1e-6


def check_transfer_functions():
    """The small-signal transfer functions the C++ models are built from, against AC analysis of the
    full netlists."""
    print("Derived transfer functions vs. AC analysis of the full netlist:")
    freqs = np.logspace(math.log10(10.0), math.log10(24000.0), 300)
    s = 2j * math.pi * freqs

    worst = 0.0
    for drive in (0.0, 0.5, 1.0):
        for tone in (0.0, 0.5, 1.0):
            # Mid Drive: input high-passes, 1 + Zf/Zg with the diodes' zero-bias conductance, tone, output.
            h1 = s * 0.02e-6 * 510e3 / (1 + s * 0.02e-6 * (1e3 + 510e3))
            h2 = s * 1e-6 * 10e3 / (1 + s * 1e-6 * 10e3)
            rf = 51e3 + 500e3 * taper_audio(drive)
            gd = 2 * (DIODE_1N914["Is"] / (DIODE_1N914["n"] * VT) + GMIN)
            zf = 1 / (1 / rf + gd + s * 51e-12)
            zg = 4.7e3 + 1 / (s * 0.047e-6)
            hc = 1 + zf / zg
            ra, rb = max(tone * 20e3, R_MIN), max((1 - tone) * 20e3, R_MIN)
            rs, cs, ri, rz, cz, rfb = 1e3, 0.22e-6, 10e3, 220.0, 0.22e-6, 1e3
            rab = ra * rb / (ra + rb)
            wz = 1 / (cz * (rz + rab))
            wp = 1 / (cs * rs * ri / (rs + ri))
            K = rfb * ra / ((ra + rb) * (rz + rab))
            X = (rb / (ra + rb)) / ((rz + rab) * cs)
            ht = (1 / (rs * cs)) * ((1 + K) * s + wz) / ((s + wp) * (s + wz) + X * s)
            c7, r11, rp, c8, r12 = 1e-6, 1e3, 100e3, 0.1e-6, 510e3
            h4 = (s * s * c7 * c8 * r12 * rp) / (1 + s * (c7 * (rp + r11) + c8 * r12 + c8 * rp) + s * s * (c8 * r12 * c7 * (rp + r11) + c8 * rp * r11 * c7))
            rout = 10e3 * 1e6 / (10e3 + 1e6)
            h5 = s * 10e-6 * rout / (1 + s * 10e-6 * (100 + rout))
            analytic = h1 * h2 * hc * ht * h4 * h5
            ac = compile_batch([mid_drive(drive, tone)], 1.0).ac(freqs, mid_drive(drive, tone).nodes["out"])[0]
            worst = max(worst, np.max(np.abs(20 * np.log10(np.abs(analytic / ac)))))
    print(f"  Mid Drive, 9 drive/tone settings, 10 Hz - 24 kHz: worst |analytic / AC| {worst:.2e} dB")
    assert worst < 1e-6

    worst = 0.0
    for drive in (0.0, 0.5, 1.0):
        for tone in (0.0, 0.5, 1.0):
            # Distortion: input network, closed-loop op-amp with a single-pole open loop, clipper+tone
            # network with the diodes' zero-bias conductance, output network.
            c1, r2, r3, c2 = 22e-9, 1e6, 1e3, 1e-9
            hin = s * c1 * r2 / ((1 + s * r3 * c2) * (1 + s * c1 * r2) + s * c2 * r2)
            cc, sr, a0 = LM308["Cc"], LM308["slew"], LM308["a0"]
            gm = sr * cc / (2 * VT)
            # The DC-gain resistor in parallel with the two reverse-biased rail clamps (GMIN each at rest).
            vclamp = LM308["vsat"] - VT * math.log(sr * cc / 1e-14 + 1.0)
            g_clamp = 2 * (1e-14 / VT * math.exp(-vclamp / VT) + GMIN)
            ro = 1 / (gm / a0 + g_clamp)
            A = gm * ro / (1 + s * ro * cc)
            rd = max(100e3 * taper_audio(drive), R_MIN)
            zf = 1 / (1 / rd + s * 100e-12)
            zg = 1 / (1 / (47 + 1 / (s * 2.2e-6)) + 1 / (560 + 1 / (s * 4.7e-6)))
            beta = zg / (zg + zf)
            hamp = A / (1 + A * beta)
            rt = 1.5e3 + max(100e3 * taper_audio(1 - tone), R_MIN)
            gd = 2 * (DIODE_1N914["Is"] / (DIODE_1N914["n"] * VT) + GMIN)
            # Node f: C8 || R8; node d: diodes, plus (Rt + Zf8) to ground; source: R6 + C7 from the op-amp.
            zf8 = 1 / (s * 3.3e-9 + 1 / 1e6)
            yd = gd + 1 / (rt + zf8)
            z7 = 1e3 + 1 / (s * 4.7e-6)
            vd_over_vo = (1 / z7) / (1 / z7 + yd)
            vf_over_vd = zf8 / (rt + zf8)
            c10, rv, c9, rl = 1e-6, 100e3, 22e-9, 1e6
            hout = s * s * c10 * c9 * rl * rv / ((1 + s * c9 * rl) * (1 + s * c10 * rv) + s * c9 * rv)
            analytic = hin * hamp * vd_over_vo * vf_over_vd * hout
            ac = compile_batch([distortion(drive, tone)], 1.0).ac(freqs, distortion(drive, tone).nodes["out"])[0]
            worst = max(worst, np.max(np.abs(20 * np.log10(np.abs(analytic / ac)))))
    print(f"  Distortion, 9 drive/tone settings, 10 Hz - 24 kHz: worst |analytic / AC| {worst:.2e} dB")
    assert worst < 1e-6

    worst = 0.0
    for drive in (0.0, 0.5, 1.0):
        for tone in (0.0, 0.5, 1.0):
            nl = transparent(drive, tone)
            ac = compile_batch([nl], 1.0).ac(freqs, nl.nodes["out"])[0]
            worst = max(worst, np.max(np.abs(20 * np.log10(np.abs(transparent_small_signal(drive, tone, freqs) / ac)))))
    print(f"  Transparent, 9 drive/tone settings, 10 Hz - 24 kHz: worst |analytic / AC| {worst:.2e} dB")
    assert worst < 1e-6

    # Fuzz: first the bias. Each stage's DC equations solved on their own (independent little Newton),
    # against the simulator's operating point.
    op_hand = fuzz_operating_point()
    nl = fuzz(0.5, 0.5)
    sim = compile_batch([nl], 1.0)
    sim.node_gmin = False
    x, _, _, _ = sim.operating_point()
    err = max(abs(x[0, nl.nodes[n]] - v) for n, v in op_hand.items())
    print("  Fuzz bias, each stage solved on its own: " + ", ".join(f"{n} {op_hand[n]:.3f}" for n in ("b4", "c4", "e4", "b3", "c3", "b2", "c2", "b1", "c1", "e1"))
          + f" V; simulator within {err:.1e} V (ElectroSmash's bias drawing: Q4 0.6 / 7 / 0.02, Q3 and Q2 0.7 / 4.4, Q1 1.6 / 4.4 / 1 V)")
    assert err < 1e-9
    # The pots' ends are 1 mOhm (R_MIN), so at tone 0 and 1 a 1000 S conductance sits in a 30-node
    # system with microsiemens and 60 dB of gain, and the netlist's own AC solve carries a few 1e-6 dB
    # of rounding there (with 1 Ohm ends both agree to 2e-9 dB at every setting). Hence 1e-5 dB here.
    worst_ends, worst_mid = 0.0, 0.0
    for drive in (0.0, 0.5, 1.0):
        for tone in (0.0, 0.5, 1.0):
            nl = fuzz(drive, tone)
            sim = compile_batch([nl], 1.0)
            sim.node_gmin = False
            ac = sim.ac(freqs, nl.nodes["out"])[0]
            err = np.max(np.abs(20 * np.log10(np.abs(fuzz_small_signal(drive, tone, freqs, op_hand) / ac))))
            if tone in (0.0, 1.0):
                worst_ends = max(worst_ends, err)
            else:
                worst_mid = max(worst_mid, err)
    print(f"  Fuzz, 9 drive/tone settings, 10 Hz - 24 kHz (stages linearized at that bias, coupling networks as 2-ports): worst |analytic / AC| "
          f"{worst_mid:.2e} dB at tone noon, {worst_ends:.2e} dB at the Tone pot's 1 mOhm ends (rounding in the full-netlist solve)")
    assert worst_mid < 1e-6 and worst_ends < 1e-5


def pot_halves(position, total):
    """A pot's two resistances at a rotation, each at least R_MIN as in the netlists."""
    return max(position * total, R_MIN), max((1.0 - position) * total, R_MIN)


def tl072_open_loop_gain():
    """The TL072 macromodel's small-signal gain at rest: gm into Ro and both reverse-biased clamps."""
    a0, vs, it = TL072["a0"], TL072["vs"], TL072["it"]
    gm = it / vs
    vclamp = TL072_VSAT_9V - VT * math.log(it / 1e-14 + 1.0)
    g_clamp = 2.0 * (1e-14 / VT * math.exp(-vclamp / VT) + GMIN)
    return gm / (gm / a0 + g_clamp)


def transparent_small_signal(drive, tone, freqs):
    """The Klon's small-signal response, stage by stage, as the C++ model computes it: every stage driven
    by an op-amp output (a voltage source), so the stages are solved one after another."""
    s = 2j * math.pi * np.asarray(freqs)
    upper, lower = pot_halves(drive, 100e3)  # each gang: the part that grows with gain, and the rest
    h_buf = s * 0.1e-6 * 1e6 / (1 + s * 0.1e-6 * (10e3 + 1e6))
    # Front network from the buffer (buf = 1): node a after C3, p the gain stage's (+), g in FF1.
    y3, g6, g7, y16, g19 = s * 0.1e-6, 1 / 10e3 + s * 68e-9, 1 / 1.5e3, s * 1e-6, 1 / 15e3
    a_p = g6 + 1 / upper
    a_g = g7 + y16 + g19
    va = y3 / (y3 + g6 + g7 - g6 * g6 / a_p - g7 * g7 / a_g)
    vp, vg = g6 * va / a_p, g7 * va / a_g
    # Gain stage: the macromodel's finite gain A around the feedback divider Zg / (Zg + Zf).
    A = tl072_open_loop_gain()
    zf = 1 / (1 / 422e3 + s * 390e-12)
    zg = 1 / (1 / 15e3 + s * 82e-9) + 2e3 + (1 - drive) * 100e3
    vo1 = A * vp / (1 + A * zg / (zg + zf))
    # Clipper and FF2 ladder b - f - e - d (diodes at their rest conductance, each with its Rs).
    g4, g8, y6 = 1 / 5.1e3 + s * 68e-9, 1 / 1.5e3, 1 / (1e3 + 1 / (s * 390e-9))
    gu, gl = 1 / upper, 1 / lower
    y17 = 1 / 27e3 + 1 / (12e3 + 1 / (s * 27e-9))
    y11, g16, y10, y9 = 1 / (22e3 + 1 / (s * 2.2e-9)), 1 / 47e3, s * 1e-6, 1 / (1e3 + 1 / (s * 1e-6))
    g0 = DIODE_1N34A["Is"] / (DIODE_1N34A["n"] * VT) + GMIN
    gd = 2 * g0 / (1 + g0 * DIODE_1N34A["rs"])
    a_b, a_f, a_e, a_d = g4 + g8 + y6 + gu, gu + gl + y17 + y11, y11 + g16 + y10, y10 + y9 + gd
    r_b, r_d = g4 * 1.0, y9 * vo1
    a_f2 = a_f - gu * gu / a_b
    r_f2 = gu * r_b / a_b
    a_e2 = a_e - y11 * y11 / a_f2
    r_e2 = y11 * r_f2 / a_f2
    vd = (r_d + y10 * r_e2 / a_e2) / (a_d - y10 * y10 / a_e2)
    ve = (r_e2 + y10 * vd) / a_e2
    vf = (r_f2 + y11 * ve) / a_f2
    # Summing amplifier: the three paths' currents into its virtual ground, R20 || C13.
    i_s = ve * g16 + vf * y17 + vg * g19
    v_sum = -i_s / (1 / 392e3 + s * 820e-12)
    # Treble: H = -(Y / R22 + s C14 / Ra) / (Y / R24 + s C14 / Rb), Y = 1/Ra + 1/Rb + s C14.
    ra, rb, y14 = 1.8e3 + (1 - tone) * 10e3, 4.7e3 + tone * 10e3, s * 3.9e-9
    Y = 1 / ra + 1 / rb + y14
    v_t = -(Y / 100e3 + y14 / ra) / (Y / 100e3 + y14 / rb) * v_sum
    # Output: node x (the bypass line) and the output node, two equations.
    y2, g3, gbl = s * 4.7e-6, 1 / 100e3, 1 / (560 + 68e3)
    y15, go = 1 / (560 + 1 / (s * 4.7e-6)), 1 / 10e3 + 1 / 100e3 + 1 / 1e6
    a11, a22 = y2 + g3 + gbl, gbl + y15 + go
    v_out = (y15 * v_t + gbl * y2 / a11) / (a22 - gbl * gbl / a11)
    return h_buf * v_out


FUZZ_STAGES = {  # per transistor stage: base-to-ground, collector-base feedback (R || C), collector, emitter
    "q4": dict(rbg=47e3, rf=470e3, cf=470e-12, rc=10e3, re=100.0),
    "q3": dict(rbg=100e3, rf=470e3, cf=470e-12, rc=10e3, re=150.0, cd=1e-6),
    "q2": dict(rbg=100e3, rf=470e3, cf=470e-12, rc=10e3, re=150.0, cd=1e-6),
}


def fuzz_operating_point():
    """The Big Muff's bias, each stage on its own (the coupling capacitors isolate them; the tone
    stack's DC path, R8 + Tone + R5 = 161k from Q2's collector to ground, is the only load that crosses
    a stage, and it doesn't depend on the knob). Unknowns per stage (vbe, vbc); the rest follows from
    the stage's KCL. Returns node voltages by netlist name."""
    q = BJT_2N5088
    out = {}
    for name, extra_load in (("q4", 0.0), ("q3", 0.0), ("q2", 1.0 / (39e3 + 100e3 + 22e3))):
        p = FUZZ_STAGES[name]

        def f(v, p=p, extra_load=extra_load):
            vbe, vbc = v
            ic, ib = ebers_moll(vbe, vbc, **q)
            ve = (ic + ib) * p["re"]
            vb = ve + vbe
            vc = vb - vbc
            return [(-vb / p["rbg"] + (vc - vb) / p["rf"] - ib) * 1e6,
                    ((9.0 - vc) / p["rc"] - (vc - vb) / p["rf"] - ic - vc * extra_load) * 1e6]
        vbe, vbc = solve_newton(f, [0.65, -3.0])
        ic, ib = ebers_moll(vbe, vbc, **q)
        ve = (ic + ib) * p["re"]
        k = name[1]
        out["e" + k], out["b" + k], out["c" + k] = ve, ve + vbe, ve + vbe - vbc
    # Output stage: R7 430k from 9 V and R3 100k to the base, R6 15k, R4 3.3k.
    def g(v):
        vbe, vbc = v
        ic, ib = ebers_moll(vbe, vbc, **q)
        ve = (ic + ib) * 3.3e3
        vb = ve + vbe
        vc = vb - vbc
        return [((9.0 - vb) / 430e3 - vb / 100e3 - ib) * 1e6, ((9.0 - vc) / 15e3 - ic) * 1e6]
    vbe, vbc = solve_newton(g, [0.6, -2.0])
    ic, ib = ebers_moll(vbe, vbc, **q)
    out["e1"] = (ic + ib) * 3.3e3
    out["b1"] = out["e1"] + vbe
    out["c1"] = out["b1"] - vbc
    # Nodes that follow: the diode nodes sit at their collectors (no DC through C6, C7), the tone stack
    # divides Q2's collector voltage.
    out["d3"], out["d2"] = out["c3"], out["c2"]
    out["tl"] = out["c2"] * (100e3 + 22e3) / (39e3 + 100e3 + 22e3)
    out["th"] = out["c2"] * 22e3 / (39e3 + 100e3 + 22e3)
    return out


def bjt_small_signal(vbe, vbc):
    """d(Ic, Ib, Ie_in) / d(vb, ve, vc) of the transport model at a bias point: rows c, b, e."""
    q = BJT_2N5088
    gbe = q["Is"] / VT * math.exp(vbe / VT) + GMIN
    gbc = q["Is"] / VT * math.exp(vbc / VT) + GMIN
    dic = (gbe, -gbc * (1 + 1 / q["br"]))
    dib = (gbe / q["bf"], gbc / q["br"])
    die = (-(dic[0] + dib[0]), -(dic[1] + dib[1]))
    return [np.array([d[0] + d[1], -d[0], -d[1]]) for d in (dic, dib, die)]


def fuzz_small_signal(drive, tone, freqs, op):
    """The Big Muff's small-signal response as the C++ model computes it: each transistor stage's own
    elements and the transistor linearized at the bias, the coupling networks between stages (Sustain;
    C13 and R12; the tone stack) reduced to 2-ports, and the four stages solved together (each stage's
    base moves under its predecessor's collector, so they load each other)."""
    freqs = np.asarray(freqs)
    su, sl = pot_halves(1.0 - drive, 100e3)   # Sustain: top-to-wiper, wiper-to-bottom
    tu, tl_ = pot_halves(1.0 - tone, 100e3)   # Tone: high-pass end to wiper, wiper to low-pass end
    names = ["b4", "e4", "c4", "b3", "e3", "c3", "d3", "b2", "e2", "c2", "d2", "b1", "e1", "c1"]
    ix = {n: i for i, n in enumerate(names)}
    H = np.zeros(len(freqs), dtype=complex)
    for fi, f in enumerate(freqs):
        s = 2j * math.pi * f
        M = np.zeros((14, 14), dtype=complex)
        rhs = np.zeros(14, dtype=complex)

        def y(a, b, val):  # an admittance between two unknowns (b = None: to AC ground)
            M[ix[a], ix[a]] += val
            if b is not None:
                M[ix[b], ix[b]] += val
                M[ix[a], ix[b]] -= val
                M[ix[b], ix[a]] -= val

        # Input: R2 + C1 from the source into Q4's base.
        y_in = 1 / (39e3 + 1 / (s * 1e-6))
        y("b4", None, y_in)
        rhs[ix["b4"]] += y_in
        for k, p in (("4", FUZZ_STAGES["q4"]), ("3", FUZZ_STAGES["q3"]), ("2", FUZZ_STAGES["q2"])):
            y("b" + k, None, 1 / p["rbg"])
            y("c" + k, "b" + k, 1 / p["rf"] + s * p["cf"])
            y("c" + k, None, 1 / p["rc"])
            y("e" + k, None, 1 / p["re"])
            if "cd" in p:  # the diode branch: C6 / C7 from the base to d, the pair from d to the collector
                y("b" + k, "d" + k, s * p["cd"])
                g0 = DIODE_1N914["Is"] / (DIODE_1N914["n"] * VT)
                u = op["d" + k] - op["c" + k]
                y("d" + k, "c" + k, 2 * g0 * math.cosh(u / (DIODE_1N914["n"] * VT)) + 2 * GMIN)
        # Output stage's own elements and its load: C2 into Volume (100k) || 1M.
        y("b1", None, 1 / 430e3 + 1 / 100e3)
        y("c1", None, 1 / 15e3 + 1 / (1 / (s * 0.1e-6) + 1 / (1 / 100e3 + 1 / 1e6)))
        y("e1", None, 1 / 3.3e3)
        # Couplings as 2-ports. Sustain: C4 + upper part from c4 to the wiper, lower part + R23 to ground,
        # C5 + R19 to Q3's base; the wiper eliminated.
        ya, yg, yb = 1 / (su + 1 / (s * 1e-6)), 1 / (sl + 1e3), 1 / (10e3 + 1 / (s * 0.1e-6))
        sigma = ya + yg + yb
        M[ix["c4"], ix["c4"]] += ya - ya * ya / sigma
        M[ix["b3"], ix["b3"]] += yb - yb * yb / sigma
        M[ix["c4"], ix["b3"]] -= ya * yb / sigma
        M[ix["b3"], ix["c4"]] -= ya * yb / sigma
        y("c3", "b2", 1 / (10e3 + 1 / (s * 0.1e-6)))  # C13 + R12
        # Tone stack: internal nodes tl, th, tw eliminated (Schur complement of a 5-node admittance).
        T = np.zeros((5, 5), dtype=complex)  # order: c2, b1, tl, th, tw

        def t(a, b, val):
            T[a, a] += val
            if b is not None:
                T[b, b] += val
                T[a, b] -= val
                T[b, a] -= val
        t(0, 2, 1 / 39e3)
        t(2, None, s * 10e-9)
        t(0, 3, s * 4e-9)
        t(3, None, 1 / 22e3)
        t(3, 4, 1 / tu)
        t(4, 2, 1 / tl_)
        t(4, 1, s * 0.1e-6)
        Y2 = T[:2, :2] - T[:2, 2:] @ np.linalg.solve(T[2:, 2:], T[2:, :2])
        for a, na in enumerate(("c2", "b1")):
            for b, nb in enumerate(("c2", "b1")):
                M[ix[na], ix[nb]] += Y2[a, b]
        # Transistors at the bias.
        for k in ("4", "3", "2", "1"):
            vbe, vbc = op["b" + k] - op["e" + k], op["b" + k] - op["c" + k]
            rows = bjt_small_signal(vbe, vbc)
            cols = [ix["b" + k], ix["e" + k], ix["c" + k]]
            for row_name, r in zip(("c", "b", "e"), rows):
                M[ix[row_name + k], cols] += r
        v = np.linalg.solve(M, rhs)
        # Output voltage: C2's current into Volume || 1M.
        r_out = 1 / (1 / 100e3 + 1 / 1e6)
        H[fi] = v[ix["c1"]] * r_out / (r_out + 1 / (s * 0.1e-6))
    return H


# ---------------------------------------------------------------------------------------------------
# Golden fixtures

SETTINGS_AC = [(d, t) for d in (0.0, 0.5, 1.0) for t in (0.0, 0.5, 1.0)]

DURATION = 0.2  # seconds per transient fixture


def test_signals():
    """48 kHz inputs in volts: Yeh's 220 Hz at 200 mV, a hard 110 Hz at 1 V, and a decaying power chord."""
    n = np.arange(int(DURATION * FS))
    t = n / FS
    fade = np.minimum(1.0, t / 0.002)  # 2 ms fade-in, so the start isn't a step
    sine220 = 0.2 * np.sin(2 * math.pi * 220.0 * t) * fade
    sine110 = 1.0 * np.sin(2 * math.pi * 110.0 * t) * fade
    chord = np.zeros_like(t)
    for f0 in (82.41, 123.47, 164.81):  # E2, B2, E3
        for k in range(1, 16):
            if k * f0 < 6000.0:
                chord += (0.35 / k) * np.exp(-t * (3.0 + 0.8 * k)) * np.sin(2 * math.pi * k * f0 * t + 0.7 * k)
    chord *= 0.6 / np.max(np.abs(chord)) * fade
    return {"sine220": sine220, "sine110": sine110, "chord": chord}


CASES = [
    # name, circuit, signal, drive, tone
    ("mid_drive_sine220_d0", "mid_drive", "sine220", 0.0, 0.5),
    ("mid_drive_sine220_d50", "mid_drive", "sine220", 0.5, 0.5),
    ("mid_drive_sine220_d100", "mid_drive", "sine220", 1.0, 0.5),
    ("mid_drive_sine110_d100", "mid_drive", "sine110", 1.0, 0.5),
    ("mid_drive_chord_d50_t0", "mid_drive", "chord", 0.5, 0.0),
    ("mid_drive_chord_d50_t100", "mid_drive", "chord", 0.5, 1.0),
    ("distortion_sine220_d0", "distortion", "sine220", 0.0, 0.5),
    ("distortion_sine220_d50", "distortion", "sine220", 0.5, 0.5),
    ("distortion_sine220_d100", "distortion", "sine220", 1.0, 0.5),
    ("distortion_sine110_d100", "distortion", "sine110", 1.0, 0.5),
    ("distortion_chord_d50_t0", "distortion", "chord", 0.5, 0.0),
    ("distortion_chord_d50_t100", "distortion", "chord", 0.5, 1.0),
    ("transparent_sine220_d0", "transparent", "sine220", 0.0, 0.5),
    ("transparent_sine220_d50", "transparent", "sine220", 0.5, 0.5),
    ("transparent_sine220_d100", "transparent", "sine220", 1.0, 0.5),
    ("transparent_sine110_d100", "transparent", "sine110", 1.0, 0.5),
    ("transparent_chord_d50_t0", "transparent", "chord", 0.5, 0.0),
    ("transparent_chord_d50_t100", "transparent", "chord", 0.5, 1.0),
    ("fuzz_sine220_d0", "fuzz", "sine220", 0.0, 0.5),
    ("fuzz_sine220_d50", "fuzz", "sine220", 0.5, 0.5),
    ("fuzz_sine220_d100", "fuzz", "sine220", 1.0, 0.5),
    ("fuzz_sine110_d100", "fuzz", "sine110", 1.0, 0.5),
    ("fuzz_chord_d50_t0", "fuzz", "chord", 0.5, 0.0),
    ("fuzz_chord_d50_t100", "fuzz", "chord", 0.5, 1.0),
]

# The fuzz's bias is solved exactly (no GMIN from its nodes to ground; it has no floating nodes), as its
# C++ model solves it.
NODE_GMIN = {"fuzz": False}

# Internal nodes recorded while rendering, for the checks printed with each case.
INTERNAL = {
    "mid_drive": ("clip", "tout"),
    "distortion": ("o", "d"),
    "transparent": ("o1", "p", "sum", "tout", "d"),
    "fuzz": ("b4", "e4", "c4", "b3", "e3", "c3", "b2", "e2", "c2", "b1", "e1", "c1"),
}


def write_wav(path, x):
    wavfile.write(path, int(FS), np.asarray(x, dtype=np.float32))


def describe_case(circuit, name, nodes, info):
    """Per-case checks on the internal nodes recorded while rendering (at 16x)."""
    rate = FS * 16
    if circuit == "mid_drive":
        # Both op-amps are ideal here; a JRC4558 on 9 V swings about +-3.5 V, so show they stay inside.
        info["opamp_peaks_volts"] = [float(np.max(np.abs(v))) for v in nodes]
        print(f"    {name}: op-amp outputs peak at {info['opamp_peaks_volts'][0]:.3f} V (clipping stage) and {info['opamp_peaks_volts'][1]:.3f} V (tone stage)")
    elif circuit == "distortion":
        slope = np.abs(np.diff(nodes[0])) * rate
        info["opamp_peak_volts"] = float(np.max(np.abs(nodes[0])))
        info["opamp_fastest_volts_per_us"] = float(np.max(slope) / 1e6)
        info["half_slew_fraction"] = float(np.mean(slope > 0.5 * LM308["slew"]))
        info["diode_peak_volts"] = float(np.max(np.abs(nodes[1])))
        print(f"    {name}: LM308 output peaks at {info['opamp_peak_volts']:.3f} V, fastest edge {info['opamp_fastest_volts_per_us']:.3f} V/us "
              f"(slew limit 0.3), above half the slew rate {100 * info['half_slew_fraction']:.2f}% of the time; diodes at {info['diode_peak_volts']:.3f} V")
    elif circuit == "transparent":
        o1, p, total, tone_out, d = nodes
        info["gain_stage_peak_volts"] = float(np.max(np.abs(o1)))
        info["gain_stage_saturated_fraction"] = float(np.mean(np.abs(o1) > TL072_VSAT_9V - 0.1))
        info["gain_stage_fastest_volts_per_us"] = float(np.max(np.abs(np.diff(o1))) * rate / 1e6)
        info["gain_stage_input_peak_volts"] = float(np.max(np.abs(p)))
        info["summing_peak_volts"] = float(np.max(np.abs(total)))
        info["treble_peak_volts"] = float(np.max(np.abs(tone_out)))
        info["diode_peak_volts"] = float(np.max(np.abs(d)))
        print(f"    {name}: gain stage (9 V) peaks at {info['gain_stage_peak_volts']:.3f} V, within 0.1 V of its +-3 V limit "
              f"{100 * info['gain_stage_saturated_fraction']:.1f}% of the time, fastest edge {info['gain_stage_fastest_volts_per_us']:.3f} V/us "
              f"(TL072: 13), its input at {info['gain_stage_input_peak_volts']:.3f} V; diodes at {info['diode_peak_volts']:.3f} V; "
              f"summing amp {info['summing_peak_volts']:.3f} V and treble stage {info['treble_peak_volts']:.3f} V (about +-10 V available)")
    elif circuit == "fuzz":
        v = dict(zip(INTERNAL["fuzz"], nodes))
        q = BJT_2N5088
        details = []
        for k in ("4", "3", "2", "1"):
            vbe, vbc = v["b" + k] - v["e" + k], v["b" + k] - v["c" + k]
            ic = q["Is"] * np.expm1(vbe / VT) - q["Is"] * np.expm1(vbc / VT) * (1 + 1 / q["br"])
            info["q" + k] = dict(vce_min=float(np.min(v["c" + k] - v["e" + k])), vbc_max=float(np.max(vbc)),
                                 ic_min_ma=float(np.min(ic) * 1e3), ic_max_ma=float(np.max(ic) * 1e3),
                                 collector_swing=float(np.max(v["c" + k]) - np.min(v["c" + k])),
                                 base_swing=float(np.max(v["b" + k]) - np.min(v["b" + k])))
            details.append(f"Q{k} Vce >= {info['q' + k]['vce_min']:.2f}, Vbc <= {info['q' + k]['vbc_max']:.2f}, Ic {info['q' + k]['ic_min_ma']:.3f} to "
                           f"{info['q' + k]['ic_max_ma']:.3f} mA")
        # How far each stage's base moves against the collector swing feeding it: the coupling the joint
        # solve keeps (a fixed load would ignore it).
        ratios = [info["q3"]["base_swing"] / info["q4"]["collector_swing"], info["q2"]["base_swing"] / info["q3"]["collector_swing"],
                  info["q1"]["base_swing"] / info["q2"]["collector_swing"]]
        info["base_to_previous_collector_swing"] = [float(r) for r in ratios]
        print(f"    {name}: " + "; ".join(details) + "; each base swings " + ", ".join(f"{100 * r:.1f}%" for r in ratios)
              + " of the collector before it")


def golden(folder, circuits):
    os.makedirs(folder, exist_ok=True)
    signals = test_signals()
    for name, x in signals.items():
        path = os.path.join(folder, f"input_{name}.wav")
        if not os.path.exists(path):
            write_wav(path, x / VOLTS_AT_FULL_SCALE)

    # AC responses: 1/12-octave points, 20 Hz to 20 kHz.
    freqs = 20.0 * 2.0 ** (np.arange(0, 120) / 12.0)
    freqs = freqs[freqs <= 20000.0]
    for circuit in circuits:
        build = CIRCUITS[circuit]
        nets = [build(d, t) for d, t in SETTINGS_AC]
        sim = compile_batch(nets, 1.0)
        sim.node_gmin = NODE_GMIN.get(circuit, True)
        H = sim.ac(freqs, nets[0].nodes["out"])
        with open(os.path.join(folder, f"ac_{circuit}.csv"), "w") as f:
            f.write("# Small-signal response of the full netlist (prototypes/circuits.py, AC analysis at rest), input volts to output volts.\n")
            f.write("# Columns: frequency_hz, then for each drive/tone setting: magnitude_db, phase_deg\n")
            f.write("frequency_hz," + ",".join(f"d{d}_t{t}_db,d{d}_t{t}_deg" for d, t in SETTINGS_AC) + "\n")
            for i, fr in enumerate(freqs):
                cols = []
                for j in range(len(SETTINGS_AC)):
                    cols += [f"{20 * math.log10(abs(H[j, i])):.9f}", f"{math.degrees(np.angle(H[j, i])):.6f}"]
                f.write(f"{fr:.6f}," + ",".join(cols) + "\n")
        print(f"  wrote ac_{circuit}.csv ({len(freqs)} frequencies x {len(SETTINGS_AC)} settings)")

    # Cases: rendered for the circuits asked for, merged into the existing cases.json (the others keep
    # their entries and their files untouched).
    path = os.path.join(folder, "cases.json")
    cases = json.load(open(path))["cases"] if os.path.exists(path) else {}
    for circuit in circuits:
        build = CIRCUITS[circuit]
        mine = [c for c in CASES if c[1] == circuit]
        nets = [build(d, t) for _, _, _, d, t in mine]
        inputs = np.stack([signals[sig] for _, _, sig, _, _ in mine])
        stats = {}
        outputs = render(nets, inputs, stats=stats, internal=INTERNAL[circuit], node_gmin=NODE_GMIN.get(circuit, True))
        print(f"  {circuit}: {len(mine)} renders at 16x in {stats['seconds']:.0f} s, Newton iterations per step: mean {stats['mean_iterations']:.2f}, max {stats['max_iterations']}")
        for j, ((name, _, sig, d, t), y) in enumerate(zip(mine, outputs)):
            write_wav(os.path.join(folder, f"expected_{name}.wav"), y / VOLTS_AT_FULL_SCALE)
            info = dict(circuit=circuit, input=f"input_{sig}.wav", drive=d, tone=t, expected=f"expected_{name}.wav",
                        peak_volts=float(np.max(np.abs(y))))
            describe_case(circuit, name, stats["internal"][j], info)
            cases[name] = info
    # The fuzz's bias (operating point), for checking the C++ model's own bias solve.
    previous = json.load(open(path)) if os.path.exists(path) else {}
    bias = previous.get("bias", {})
    if "fuzz" in circuits:
        nl = fuzz(0.5, 0.5)
        sim = compile_batch([nl], 1.0)
        sim.node_gmin = False
        x, _, _, _ = sim.operating_point()
        bias["fuzz"] = {n: float(x[0, nl.nodes[n]]) for n in ("b4", "e4", "c4", "b3", "e3", "c3", "d3", "b2", "e2", "c2", "d2", "b1", "e1", "c1")}
    with open(path, "w") as f:
        json.dump(dict(volts_at_full_scale=VOLTS_AT_FULL_SCALE, simulation_rate=FS * 16, cases=cases, bias=bias), f, indent=2)
    print(f"  wrote {len(cases)} cases to cases.json")


def convergence_check(circuits):
    """How close the 16x reference is to the circuit's true response: 16x against 32x."""
    print("Reference accuracy: 16x against 32x on the hardest cases:")
    signals = test_signals()
    for circuit in circuits:
        nets = [CIRCUITS[circuit](1.0, 0.5)]
        x = signals["sine110"][None, : int(0.05 * FS)]
        gmin = NODE_GMIN.get(circuit, True)
        y16 = render(nets, x, 16, node_gmin=gmin)[0]
        y32 = render(nets, x, 32, node_gmin=gmin)[0]
        err = 10 * math.log10(np.sum((y16 - y32) ** 2) / np.sum(y32 ** 2))
        print(f"  {circuit}, drive 1, 110 Hz at 1 V: 16x differs from 32x by {err:.1f} dB")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--golden")
    parser.add_argument("--circuits", default=",".join(CIRCUITS), help="comma-separated, for --golden (default: all)")
    args = parser.parse_args()
    check_simulator()
    check_transfer_functions()
    if args.golden:
        chosen = args.circuits.split(",")
        convergence_check(chosen)
        golden(args.golden, chosen)

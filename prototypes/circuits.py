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
  - Every time step iterates Newton until the node voltages move less than 1e-9 V + 1e-9 |v| and no
    junction was limited.
  - AC analysis linearizes every element at the operating point (here the all-zero rest state) and
    solves the complex system at each frequency, like SPICE's .ac.

The pedals are simulated as their AC equivalents: the 4.5 V bias rail and the supply are signal ground,
so every node sits at 0 V at rest. Transistor buffers (the Tube Screamer's emitter followers, the RAT's
JFET source follower) are ideal unity-gain buffers.

The renders run at 16x (768 kHz). The 48 kHz input is upsampled and the output decimated with a long
linear-phase Kaiser FIR (passband to 21 kHz, -140 dB from 27 kHz), so the reference is a band-limited,
alias-free, zero-phase rendering of the circuit.

Usage:
  uv run --with numpy --with scipy python prototypes/circuits.py --check        # validation only
  uv run --with numpy --with scipy python prototypes/circuits.py --golden tests/fixtures/drive
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


CIRCUITS = {"mid_drive": mid_drive, "distortion": distortion}


# ---------------------------------------------------------------------------------------------------
# The simulator


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
        caps, diodes, tanhs = [], [], []

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
                self.G[:, k, c_node] -= gain
            elif kind == "OPAMP":
                k = self.rows[idx]
                p, m, o = e[1], e[2], e[3]
                self.G[:, o, k] += 1.0
                self.G[:, k, p] += 1.0
                self.G[:, k, m] -= 1.0
            elif kind == "D":
                diodes.append((e[1], e[2], e[3], e[4]))
            elif kind == "GTANH":
                tanhs.append((e[1], e[2], e[3], e[4], e[5]))

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
        self.T = T
        self.set_timestep(T)

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
        x, vd, u_op = self.operating_point()
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
                x_new = np.linalg.solve(A, b[..., None])[..., 0]
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
        Returns the node solution, the junction voltages, and the transconductors' controlling voltages."""
        B, N = self.batch, self.size
        nd = self.dio_inc.shape[1]
        x = np.zeros((B, N))
        vd = np.zeros((B, nd))
        u_op = [np.zeros(B) for _ in self.tanhs]
        for _ in range(max_iterations):
            A = self.G.copy()
            idx = np.arange(self.num_nodes)
            A[:, idx, idx] += GMIN
            b = self.b_static.copy()
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
            x_new = np.linalg.solve(A, b[..., None])[..., 0]
            limited = np.zeros(B, dtype=bool)
            if nd:
                vd, lim = self.pnjlim(x_new[:, : self.num_nodes] @ self.dio_inc[: self.num_nodes], vd)
                limited |= lim.any(axis=1)
            for j, (xn, p, m, it_, vs) in enumerate(self.tanhs):
                u_new = (x_new[:, p] if p >= 0 else 0.0) - (x_new[:, m] if m >= 0 else 0.0)
                u_op[j], lim = self.tanh_limit(u_new, u_op[j], vs)
                limited |= lim
            converged = (np.abs(x_new - x)[:, : self.num_nodes] <= 1e-12 + 1e-9 * np.abs(x_new[:, : self.num_nodes])).all()
            x = x_new
            if converged and not limited.any():
                return x, vd, u_op
        raise RuntimeError("no DC operating point")

    def ac(self, freqs, probe):
        """Small-signal response from the input source to the probe node, linearized at the DC operating
        point (where the rail clamps' diodes sit reverse-biased by their sources, not at 0 V)."""
        B, N = self.batch, self.size
        _, vd, u_op = self.operating_point()
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


def render(netlists, inputs48, factor=16, stats=None, internal=()):
    """Run a batch of netlists on 48 kHz inputs (volts, (B, L)); returns 48 kHz outputs (B, L). Internal
    nodes named in `internal` come back in stats["internal"] at the simulation rate, (B, len, L * factor)."""
    h = resampling_filter(factor)
    up = np.stack([resample_poly(x, factor, 1, window=h) for x in inputs48])
    sim = compile_batch(netlists, 1.0 / (FS * factor))
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
]


def write_wav(path, x):
    wavfile.write(path, int(FS), np.asarray(x, dtype=np.float32))


def golden(folder):
    os.makedirs(folder, exist_ok=True)
    signals = test_signals()
    for name, x in signals.items():
        write_wav(os.path.join(folder, f"input_{name}.wav"), x / VOLTS_AT_FULL_SCALE)

    # AC responses: 1/12-octave points, 20 Hz to 20 kHz.
    freqs = 20.0 * 2.0 ** (np.arange(0, 120) / 12.0)
    freqs = freqs[freqs <= 20000.0]
    for circuit, build in CIRCUITS.items():
        nets = [build(d, t) for d, t in SETTINGS_AC]
        H = compile_batch(nets, 1.0).ac(freqs, nets[0].nodes["out"])
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

    cases = {}
    internal = {"mid_drive": ("clip", "tout"), "distortion": ("o", "d")}
    for circuit, build in CIRCUITS.items():
        mine = [c for c in CASES if c[1] == circuit]
        nets = [build(d, t) for _, _, _, d, t in mine]
        inputs = np.stack([signals[sig] for _, _, sig, _, _ in mine])
        stats = {}
        outputs = render(nets, inputs, stats=stats, internal=internal[circuit])
        print(f"  {circuit}: {len(mine)} renders at 16x in {stats['seconds']:.0f} s, Newton iterations per step: mean {stats['mean_iterations']:.2f}, max {stats['max_iterations']}")
        for j, ((name, _, sig, d, t), y) in enumerate(zip(mine, outputs)):
            write_wav(os.path.join(folder, f"expected_{name}.wav"), y / VOLTS_AT_FULL_SCALE)
            nodes = stats["internal"][j]
            info = dict(circuit=circuit, input=f"input_{sig}.wav", drive=d, tone=t, expected=f"expected_{name}.wav",
                        peak_volts=float(np.max(np.abs(y))))
            if circuit == "mid_drive":
                # Both op-amps are ideal here; a JRC4558 on 9 V swings about +-3.5 V, so show they stay inside.
                info["opamp_peaks_volts"] = [float(np.max(np.abs(v))) for v in nodes]
                print(f"    {name}: op-amp outputs peak at {info['opamp_peaks_volts'][0]:.3f} V (clipping stage) and {info['opamp_peaks_volts'][1]:.3f} V (tone stage)")
            else:
                slope = np.abs(np.diff(nodes[0])) * FS * 16
                info["opamp_peak_volts"] = float(np.max(np.abs(nodes[0])))
                info["opamp_fastest_volts_per_us"] = float(np.max(slope) / 1e6)
                info["half_slew_fraction"] = float(np.mean(slope > 0.5 * LM308["slew"]))
                info["diode_peak_volts"] = float(np.max(np.abs(nodes[1])))
                print(f"    {name}: LM308 output peaks at {info['opamp_peak_volts']:.3f} V, fastest edge {info['opamp_fastest_volts_per_us']:.3f} V/us "
                      f"(slew limit 0.3), above half the slew rate {100 * info['half_slew_fraction']:.2f}% of the time; diodes at {info['diode_peak_volts']:.3f} V")
            cases[name] = info
    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump(dict(volts_at_full_scale=VOLTS_AT_FULL_SCALE, simulation_rate=FS * 16, cases=cases), f, indent=2)
    print(f"  wrote {len(cases)} cases to cases.json")


def convergence_check():
    """How close the 16x reference is to the circuit's true response: 16x against 32x."""
    print("Reference accuracy: 16x against 32x on the hardest cases:")
    signals = test_signals()
    for circuit, build in CIRCUITS.items():
        nets = [build(1.0, 0.5)]
        x = signals["sine110"][None, : int(0.05 * FS)]
        y16 = render(nets, x, 16)[0]
        y32 = render(nets, x, 32)[0]
        err = 10 * math.log10(np.sum((y16 - y32) ** 2) / np.sum(y32 ** 2))
        print(f"  {circuit}, drive 1, 110 Hz at 1 V: 16x differs from 32x by {err:.1f} dB")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--golden")
    args = parser.parse_args()
    check_simulator()
    check_transfer_functions()
    if args.golden:
        convergence_check()
        golden(args.golden)

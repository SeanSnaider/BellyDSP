# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Five more gray-box amp channels: the sources of the built-in gain sets Forge, Basalt, Comet, Quartz, and
Lantern (tools/content/make_default_captures.py; docs/BUILD_PLAN.md "Amp gain", "More built-in amps").

prototypes/amp_sim.py's three channels (clean, crunch, lead) stay exactly as they were; these sit beside them
with the same interface, so the capture script can treat all eight alike:

    amp(x, fs, channel, gain=5, bass=5, mid=5, treble=5, master=5, cab=False, drive_db=None)
    gain_drive_db(channel, gain)       # the channel's gain taper: drive (dB) at a gain knob position

Amp only (cab=False is the only mode: BellyDSP has its own cab). Knobs are 0 to 10.

Why five new channels and not five new settings of the old ones: tone_bench (docs/TONE_MATCH.md) found the
built-ins can't reach high-gain and lead drive (the oracle's Gain at +24 dB in 17 of 50 cases) and that 45%
of the matcher's error is amp and cab coverage. More gain on amp_sim's lead channel would only make Monolith
fizzier; what's missing is other kinds of distortion. Each channel here is a different circuit idea, and
each is unlike both the built-ins (amp_sim: 1 to 3 non-inverting tanh stages behind a fixed pre-emphasis,
the passive FMV tone stack after them, a plain tanh power amp with no sag) and the benchmark's hidden
voicings (prototypes/tone_bench/rigs.py: 1 to 4 INVERTING triode stages with grid-conduction blocking,
passive FMV stacks with their own values or a Baxandall, and one push-pull power amp model with an explicit
crossover term and a single-time-constant sag). The benchmark only means something if these don't copy it.

  forge    Tight modern high gain. A 4th-order low cut (two 2nd-order high-passes at 100 and 180 Hz) and a
           +7 dB push at 1.4 kHz BEFORE any clipping, so the lows never reach the clippers and the upper mids
           drive them hardest; one soft first stage, then two HARD-clipping stages (the algebraic sigmoid at
           k = 8, nearly a hard clip with a rounded knee); an active contour after the preamp (a 500 Hz cut);
           a stiff power amp with no sag.
  basalt   Fat high gain. The lows pushed INTO the clippers (+3 dB shelf at 140 Hz, 20 Hz couplings), three
           arctangent stages (the smoothest of the sigmoids: its harmonics fall off slowest in level and it
           never flattens hard), a cathode follower that squashes only the positive swing before the tone
           stack (a passive FMV with our own values, the fattest of them), a power amp with 20 ms sag.
  comet    Saturated lead, pushed mids. +6 dB at 850 Hz and a 5.5 kHz roll-off before the gain, three
           biased soft stages, an envelope squash before the last stage (the stage's own signal, 4 ms, takes
           the edge off the pick attack), a mid-forward active stack, and a power amp driven into saturation
           with a TWO-SECTION supply sag (a fast 1 ms filter-cap droop for the softer attack plus a slower
           30 ms rectifier droop for the compressed, sustaining body).
  quartz   Saturated lead, scooped. The tone stack FIRST, before any gain (a passive FMV with our own values
           that cuts the lows hard), then a 6 dB cut at 550 Hz and a +5 dB treble shelf, all before the
           clippers; three SYMMETRIC cubic soft clippers (y = 1.5x - 0.5x^3, flat beyond +-1: odd harmonics
           only, every other channel here and in tone_bench is biased and makes even ones too); a scoop and
           a low "depth" lift after the preamp; a power amp with 10 ms sag.
  lantern  Power-amp-driven edge of breakup to crunch. The preamp is one nearly clean stage; the gain knob
           drives the POWER section (like a non-master-volume amp's volume): a phase inverter that clips
           late and soft, then the output pair modelled per half, each a softplus conduction curve that
           saturates (class AB: both halves conduct near zero, one cuts off on big swings), slightly
           mismatched for some even harmonics, under a 15 ms rail sag; the output transformer band-limits
           it. Picking softer stays clean, harder breaks up: the touch sensitivity of an amp whose
           distortion is in its power stage.

The whole nonlinear section of each channel runs at 4 x 48 kHz (one upsample before the first nonlinearity,
one downsample after the power amp; scipy's resample_poly, whose anti-aliasing filters are linear phase, so
the output leads its input by a few samples, which make_default_captures's simulated round trip covers).

Sources: the algebraic sigmoid x / (1 + |x|^k)^(1/k) and the arctangent and cubic clippers are standard
waveshapers (Pakarinen and Yeh, "A review of digital techniques for modeling vacuum-tube guitar amplifiers",
CMJ 33(2), 2009, sec. 3; the cubic is the classic soft clipper in J. O. Smith, "Physical Audio Signal
Processing", "Cubic Soft Clipper"); the FMV tone stack is Yeh and Smith (DAFx 2006, amp_sim.tonestack_analog);
the supply sag as a clipping level that droops with the recent output power is the usual behavioural model
(Pakarinen and Yeh sec. 4.3); the push-pull pair as the difference of two halves' conduction curves follows
the same paper's class AB discussion. Component values and every voicing number are ours.
"""

import numpy as np
from scipy.signal import bilinear, lfilter, resample_poly

import amp_sim

OS = 4

# ---------------------------------------------------------------- building blocks


def filt(x, kind, f0, fs, q=0.707, gain_db=0.0):
    """An RBJ cookbook biquad (amp_sim.biquad) at sample rate fs."""
    return amp_sim.filt(x, kind, f0, fs, q, gain_db)


def one_pole_lp(x, fc, fs):
    """y[n] = (1 - a) x[n] + a y[n-1], a = exp(-2 pi fc / fs): a first-order low-pass (an RC)."""
    a = np.exp(-2 * np.pi * fc / fs)
    return lfilter([1 - a], [1, -a], x)


def one_pole_hp(x, fc, fs):
    """y[n] = r (y[n-1] + x[n] - x[n-1]), r = exp(-2 pi fc / fs): a coupling capacitor into a grid resistor."""
    r = np.exp(-2 * np.pi * fc / fs)
    return lfilter([r, -r], [1, -r], x)


def envelope(x, tau_ms, fs):
    """sqrt of a one-pole average of x^2 with time constant tau: the recent RMS (a supply's RC sees power)."""
    return np.sqrt(np.maximum(one_pole_lp(x * x, 1000.0 / (2 * np.pi * tau_ms), fs), 0.0))


def algebraic(x, k):
    """The algebraic sigmoid y = x / (1 + |x|^k)^(1/k): slope 1 at 0, asymptotes +-1, and k sets the knee:
    k = 2 is x / sqrt(1 + x^2), a smooth clipper; as k grows it approaches a hard clip (k = 8 is within 9% of
    one at |x| = 1 and within 0.1% beyond |x| = 2)."""
    return x / np.power(1.0 + np.power(np.abs(x), k), 1.0 / k)


def atan_clip(x):
    """(2 / pi) atan(pi x / 2): slope 1 at 0, asymptotes +-1, approached as 1 - 4 / (pi^2 x): the slowest of
    the sigmoids here, so the smoothest clipping (its harmonics fall off the slowest)."""
    return (2.0 / np.pi) * np.arctan(0.5 * np.pi * x)


def cubic(x):
    """The cubic soft clipper: y = 1.5 x - 0.5 x^3 for |x| <= 1 and sign(x) beyond (continuous with slope 0
    at +-1). Odd, so a symmetric stage makes odd harmonics only."""
    xc = np.clip(x, -1.0, 1.0)
    return 1.5 * xc - 0.5 * xc ** 3


def biased(fn, x, bias, *args):
    """fn(x + bias) - fn(bias): an asymmetric stage (the bias moves the operating point, so the positive and
    negative swings clip at different levels: even harmonics) with silence still mapping to 0."""
    return fn(x + bias, *args) - fn(np.asarray(bias, dtype=np.float64), *args)


def softplus(x, k):
    """log(1 + exp(k x)) / k, computed without overflow: a conduction curve, 0 far below cutoff, x above."""
    return np.logaddexp(0.0, k * x) / k


# Passive FMV tone stacks (Yeh and Smith's network, amp_sim.tonestack_analog) with our own component values,
# chosen for each voicing and unlike amp_sim's two and tone_bench's three (their responses at noon,
# 60 Hz / 400 Hz / 1 kHz / 4 kHz / 8 kHz, dB: basalt -2.3 / -7.3 / -6.9 / -3.7 / -3.1, the fattest low end of
# any stack in the project; quartz -11.9 / -11.0 / -7.1 / -5.2 / -5.1, a steep low cut; lantern -6.8 /
# -12.4 / -10.0 / -5.1 / -4.5).
STACKS = {
    "basalt": dict(C1=330e-12, C2=68e-9, C3=33e-9, R1=200e3, R2=1e6, R3=50e3, R4=39e3),
    "quartz": dict(C1=820e-12, C2=10e-9, C3=47e-9, R1=250e3, R2=1e6, R3=25e3, R4=150e3),
    "lantern": dict(C1=560e-12, C2=27e-9, C3=27e-9, R1=150e3, R2=500e3, R3=30e3, R4=82e3),
}


def fmv(x, fs, stack, bass, mid, treble):
    """The passive FMV tone stack at fs (amp_sim.tonestack's network, our values), knobs 0 to 10."""
    t, m = np.clip(treble / 10, 0, 1), np.clip(mid / 10, 0, 1)
    l = amp_sim.log_taper(np.clip(bass / 10, 0, 1))
    b, a = amp_sim.tonestack_analog(t, m, l, **STACKS[stack])
    bz, az = bilinear(b, a, fs)
    return lfilter(bz, az, x)


def active_stack(x, fs, bass, mid, treble, bass_f, mid_f, mid_q, treble_f, mid_offset_db, span_db):
    """An active tone control (op-amp shelving and a bell): +-span dB per knob across 0 to 10, the mid bell
    offset by mid_offset_db at noon (the channel's own mid voicing)."""
    s = span_db / 5.0
    x = filt(x, "lowshelf", bass_f, fs, gain_db=s * (bass - 5.0))
    x = filt(x, "peak", mid_f, fs, q=mid_q, gain_db=mid_offset_db + s * (mid - 5.0))
    return filt(x, "highshelf", treble_f, fs, gain_db=s * (treble - 5.0))


def master_mult(master):
    """The master's drive into the power amp: 10^((master - 5) / 10), amp_sim's law (+-5 dB per 5)."""
    return 10 ** ((master - 5.0) / 10.0)


# ---------------------------------------------------------------- the channels
# Each takes the DI at fs and the first nonlinear stage's drive multiplier g (10^(drive_db / 20), from the
# gain knob's taper; for lantern it's the power section's drive), and returns the amp-only output at fs.


def _forge(x, fs, g, bass, mid, treble, master):
    # Pre-distortion voicing: a 4th-order low cut (two 2nd-order high-passes, Q 0.7 and 0.9, at 100 and 180 Hz:
    # 24 dB per octave below about 150 Hz), a +7 dB bell at 1.4 kHz (Q 1.1), and 4 dB less above 5 kHz.
    x = filt(x, "highpass", 100.0, fs, q=0.707)
    x = filt(x, "highpass", 180.0, fs, q=0.9)
    x = filt(x, "peak", 1400.0, fs, q=1.1, gain_db=7.0)
    x = filt(x, "highshelf", 5000.0, fs, gain_db=-4.0)
    f = fs * OS
    u = resample_poly(x, OS, 1)
    # Stage 1: soft (k = 2), slightly biased.
    u = biased(algebraic, 12.0 * g * u, 0.15, 2.0)
    u = one_pole_lp(one_pole_hp(u, 150.0, f), 7000.0, f)
    u = filt(u, "peak", 900.0, f, q=0.8, gain_db=4.0)     # the interstage mid voicing
    # Stages 2 and 3: hard clipping (k = 8), each followed by a 2nd-order low-pass that takes the fizz off.
    u = algebraic(8.0 * u, 8.0)
    u = filt(one_pole_hp(u, 110.0, f), "lowpass", 6000.0, f, q=0.707)
    u = biased(algebraic, 4.0 * u, 0.05, 8.0)
    u = filt(u, "lowpass", 5500.0, f, q=0.707)
    # The contour: an active stack with a 5 dB cut at 500 Hz at noon (Q 0.8), +-10 dB per knob.
    u = active_stack(u, f, bass, mid, treble, 120.0, 500.0, 0.8, 3000.0, -5.0, 10.0)
    # A stiff power amp (no sag): the algebraic sigmoid at k = 3.
    u = algebraic(1.2 * master_mult(master) * u, 3.0)
    y = resample_poly(u, 1, OS)
    y = filt(y, "peak", 3800.0, fs, q=0.7, gain_db=2.5)    # presence (the power amp's feedback, shelved)
    return one_pole_hp(y, 30.0, fs)


def _basalt(x, fs, g, bass, mid, treble, master):
    # Pre-distortion voicing: a gentle 45 Hz high-pass, the lows pushed (+3 dB below 140 Hz), a broad +3 dB at
    # 650 Hz (Q 0.6).
    x = filt(x, "highpass", 45.0, fs, q=0.707)
    x = filt(x, "lowshelf", 140.0, fs, gain_db=3.0)
    x = filt(x, "peak", 650.0, fs, q=0.6, gain_db=3.0)
    f = fs * OS
    u = resample_poly(x, OS, 1)
    # Two biased arctangent stages with large coupling capacitors (20 and 25 Hz: the lows go through).
    u = biased(atan_clip, 10.0 * g * u, 0.25)
    u = one_pole_lp(one_pole_hp(u, 20.0, f), 6000.0, f)
    u = biased(atan_clip, 4.0 * u, 0.2)
    u = one_pole_lp(one_pole_hp(u, 25.0, f), 5500.0, f)
    # A cathode follower driving the stack: on the positive swing the grid draws current and the follower
    # compresses, y = u / (1 + 0.6 u) for u > 0; the negative swing passes.
    u = np.where(u > 0.0, u / (1.0 + 0.6 * np.maximum(u, 0.0)), u)
    u = 4.0 * fmv(u, f, "basalt", bass, mid, treble)        # the recovery stage after the passive stack's loss
    # Stage 3: symmetric arctangent.
    u = one_pole_lp(atan_clip(2.5 * u), 6000.0, f)
    # The power amp with sag: the clipping level r droops with the recent output power (20 ms),
    # r = 1 / (1 + 0.5 e), y = r atan_clip(w / r): a harder-driven chord clips lower and compresses.
    w = 1.5 * master_mult(master) * u
    r = 1.0 / (1.0 + 0.5 * envelope(w, 20.0, f))
    u = r * atan_clip(w / r)
    y = resample_poly(u, 1, OS)
    y = filt(y, "peak", 85.0, fs, q=1.0, gain_db=3.0)      # resonance (the speaker's impedance peak, fed back)
    return one_pole_hp(y, 30.0, fs)


def _comet(x, fs, g, bass, mid, treble, master):
    # Pre-distortion voicing: +6 dB at 850 Hz (Q 0.7) and a 5.5 kHz roll-off: mids forward, a smooth top.
    x = filt(x, "highpass", 75.0, fs, q=0.707)
    x = filt(x, "peak", 850.0, fs, q=0.7, gain_db=6.0)
    x = one_pole_lp(x, 5500.0, fs)
    f = fs * OS
    u = resample_poly(x, OS, 1)
    u = biased(algebraic, 14.0 * g * u, 0.2, 2.0)
    u = one_pole_lp(one_pole_hp(u, 90.0, f), 6500.0, f)
    u = biased(algebraic, 5.0 * u, 0.15, 2.0)
    u = one_pole_lp(one_pole_hp(u, 110.0, f), 6000.0, f)
    # The attack squash: the signal divided by 1 + 1.5 e, e its own 4 ms RMS. A pick attack raises e within a
    # few ms, so its first transient is held down before the last stage; the sustain, steadier, is barely touched.
    u = u / (1.0 + 1.5 * envelope(u, 4.0, f))
    u = biased(algebraic, 3.0 * u, 0.1, 2.0)
    u = one_pole_lp(one_pole_hp(u, 70.0, f), 5500.0, f)
    # A mid-forward active stack: +3 dB at 750 Hz at noon (Q 0.7), +-8 dB per knob.
    u = active_stack(u, f, bass, mid, treble, 150.0, 750.0, 0.7, 2800.0, 3.0, 8.0)
    # The power amp, driven, with a two-section supply: a filter cap's fast droop (1 ms) and the rectifier's
    # slower one (30 ms), r = 1 / (1 + 0.4 e_fast + 0.4 e_slow), y = r s(w / r) with s the k = 2.5 sigmoid.
    # The fast section softens each attack, the slow one keeps the body compressed (the sustain).
    w = 2.0 * master_mult(master) * u
    r = 1.0 / (1.0 + 0.4 * envelope(w, 1.0, f) + 0.4 * envelope(w, 30.0, f))
    u = r * algebraic(w / r, 2.5)
    y = resample_poly(u, 1, OS)
    y = filt(y, "highshelf", 3500.0, fs, gain_db=1.5)
    return one_pole_hp(y, 30.0, fs)


def _quartz(x, fs, g, bass, mid, treble, master):
    # The tone stack first, then the scoop and a treble lift, all before any gain: what distorts is already
    # scooped and bright.
    x = filt(x, "highpass", 90.0, fs, q=0.707)
    x = 4.0 * fmv(x, fs, "quartz", bass, mid, treble)
    x = filt(x, "peak", 550.0, fs, q=0.7, gain_db=-6.0)
    x = filt(x, "highshelf", 2500.0, fs, gain_db=5.0)
    f = fs * OS
    u = resample_poly(x, OS, 1)
    # Three symmetric cubic stages: odd harmonics only.
    u = cubic(3.0 * g * u)
    u = one_pole_lp(one_pole_hp(u, 120.0, f), 9000.0, f)
    u = cubic(4.0 * u)
    u = filt(one_pole_hp(u, 80.0, f), "lowpass", 7000.0, f, q=0.707)
    u = cubic(2.0 * u)
    u = filt(u, "lowpass", 6500.0, f, q=0.707)
    # The scoop again after the clipping, and a low "depth" lift.
    u = filt(u, "peak", 500.0, f, q=0.7, gain_db=-4.0)
    u = filt(u, "lowshelf", 90.0, f, gain_db=3.0)
    # The power amp with a 10 ms sag (the k = 4 sigmoid).
    w = 1.6 * master_mult(master) * u
    r = 1.0 / (1.0 + 0.5 * envelope(w, 10.0, f))
    u = r * algebraic(w / r, 4.0)
    y = resample_poly(u, 1, OS)
    return one_pole_hp(y, 30.0, fs)


# The output pair: each half's plate current is a softplus conduction curve (sharpness PAIR_K, idle bias
# PAIR_BIAS: both halves conduct near zero, a class AB idle), saturating through tanh. The second half is
# PAIR_MISMATCH weaker (an unmatched pair: some even harmonics).
PAIR_K, PAIR_BIAS, PAIR_MISMATCH = 4.0, 0.1, 0.08


def push_pull(v):
    """The output pair's transfer: y = I(v) - (1 - m) I(-v), I(v) = tanh(softplus(v + bias)), minus its value
    at rest. Near zero both halves conduct (the slope is the sum of both: 2 sigmoid(k bias) sech^2(I(0)),
    about 1.1 here); a big swing cuts one half off and saturates the other."""
    half = lambda s: np.tanh(softplus(s + PAIR_BIAS, PAIR_K))
    rest = half(0.0) * PAIR_MISMATCH
    return half(v) - (1.0 - PAIR_MISMATCH) * half(-v) - rest


def _lantern(x, fs, g, bass, mid, treble, master):
    # The preamp: a bright cap (+3 dB above 2.5 kHz) and one nearly clean stage at a fixed drive.
    x = one_pole_hp(x, 60.0, fs)
    x = filt(x, "highshelf", 2500.0, fs, gain_db=3.0)
    f = fs * OS
    u = resample_poly(x, OS, 1)
    u = biased(algebraic, 1.2 * u, 0.05, 2.0)
    u = one_pole_lp(one_pole_hp(u, 40.0, f), 12000.0, f)
    u = 4.0 * fmv(u, f, "lantern", bass, mid, treble)
    # The power section, driven by the gain knob (g) and the master: the phase inverter clips late and softly
    # (1.6 tanh(v / 1.6)), then the output pair under a 15 ms rail sag, r = 1 / (1 + 0.6 e), y = r pp(v / r).
    v = 1.4 * g * master_mult(master) * u
    v = 1.6 * np.tanh(v / 1.6)
    r = 1.0 / (1.0 + 0.6 * envelope(v, 15.0, f))
    u = r * push_pull(v / r)
    y = resample_poly(u, 1, OS)
    # The output transformer: 2nd-order band limits at 65 Hz and 9.5 kHz (Q 0.6), then a 20 Hz coupling.
    y = filt(y, "highpass", 65.0, fs, q=0.6)
    y = filt(y, "lowpass", 9500.0, fs, q=0.6)
    return one_pole_hp(y, 20.0, fs)


TAPER_KNOBS = amp_sim.TAPER_KNOBS

# gain_taper_db: the drive (dB, relative to the design value above) at gain knob 0, 2.5, 5, 7.5, 10, linear in
# dB between, found by make_default_captures.py --voice so the five steps land on equal steps of the nonlinear
# energy ratio between the channel's two ends (its NL_RANGE there). For lantern the drive is the power
# section's; for the others, the first stage's.
CHANNELS = {
    "forge": dict(fn=_forge, gain_taper_db=[-33.8, -31.9, -29.3, -25.1, -10.3]),
    "basalt": dict(fn=_basalt, gain_taper_db=[-26.9, -24.3, -20.8, -15.5, 1.0]),
    "comet": dict(fn=_comet, gain_taper_db=[-30.0, -27.9, -24.8, -19.6, 1.1]),
    "quartz": dict(fn=_quartz, gain_taper_db=[-25.1, -23.2, -20.7, -16.4, -1.8]),
    "lantern": dict(fn=_lantern, gain_taper_db=[-8.8, -4.3, -0.4, 4.0, 10.1]),
}


def gain_drive_db(channel, gain):
    """The drive (dB, relative to the channel's design value) at a gain knob position."""
    return float(np.interp(gain, TAPER_KNOBS, CHANNELS[channel]["gain_taper_db"]))


def amp(x, fs, channel, gain=5.0, bass=5.0, mid=5.0, treble=5.0, master=5.0, cab=False, drive_db=None):
    """x (mono, fs) through the channel, amp only. drive_db, if given, replaces the gain knob's taper (how the
    tapers are voiced)."""
    if cab:
        raise ValueError("amp_voicings is amp only: BellyDSP has its own cab (use amp_sim for the old fake cab)")
    if drive_db is None:
        drive_db = gain_drive_db(channel, gain)
    return CHANNELS[channel]["fn"](np.asarray(x, dtype=np.float64), fs, 10 ** (drive_db / 20.0), bass, mid, treble, master)

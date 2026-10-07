# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""The hidden rigs: what made the benchmark's "records". None of their amps is in the matcher's search.

    DI -> [pedal] -> amp -> cab IR -> mic/channel EQ -> [bus compressor]      the rig's tone
       -> [time effect] -> [double] -> [band] -> [limiter] -> [lossy codec]   the production

Amps, two kinds:
  * NAM core's example models (third_party/NeuralAmpModelerCore/example_models, MIT licence, "Copyright (c)
    2023 Steven Atkinson"; LICENSE in that folder). Used only as hidden amps, never searched. Measured
    (docs/TONE_MATCH.md, "tone_bench"): lstm.nam is nearly linear (a clean amp), wavenet.nam goes from the
    edge of breakup to crunch with its input level, A2.nam, slimmable_container.nam and
    wavenet_a1_standard.nam are high gain. The other files there are generated test weights (outputs of +15 to
    +20 dBFS with a crest factor near 0 dB) and aren't used. Their provenance isn't documented; whether any
    includes a cab isn't either, so a cab IR follows every one of them as with any amp.
  * New gray-box voicings (graybox below), unlike the built-ins (prototypes/amp_sim.py's clean, crunch, and
    lead channels: 1, 2, and 3 non-inverting tanh stages, the tone stack after them, a plain tanh power amp):
    1 to 4 INVERTING triode stages (so their asymmetry alternates), each with its own coupling high-pass,
    Miller low-pass, and a bias that shifts with the signal (blocking); the tone stack can sit between
    stages; other tone stacks (three passive networks with our own component values, and an active one);
    a push-pull power amp with crossover, supply sag, and presence and resonance; an output transformer.

Each case's amp drive is calibrated so the rig's distortion lands where its style says, by the nonlinear
energy ratio (tools/content/make_default_captures.py, nl_ratio_db: the share of the output's power that no
linear filter of the input explains, from the magnitude-squared coherence) on the record's own DI.

Pedals (sometimes, in front): the app's own circuits through ampsim_render (the boost's Clean mode, the
overdrive's Mid Drive, Distortion, Transparent, and Fuzz, which are golden-tested against
prototypes/circuits.py, and the pre compressor in pedal mode, golden-tested against
prototypes/compressor.py), with random settings. They're BellyDSP's, so the oracle can put "the true pedal"
in BellyDSP; the matcher doesn't search pedals.

Cabs: a third of the 21 built-in IRs is held out of the matcher's search per split (HELD_OUT), and the hidden
cab is one of those, often modified into a new IR (novel_ir): its resonances moved, a mic-distance comb, an
off-axis darkening, a small-speaker or a 4x12 low end, its minimum-phase version.
"""

import math
import pathlib
import subprocess
import tempfile

import numpy as np
import scipy.signal as sps

import amp_sim  # the tone stack's circuit model (Yeh and Smith 2006)
from common import (CACHE, IRS, NAM_EXAMPLES, REPO, SR, ampsim_render, apply_eq, cached, db, filt, fit_length,
                    read_wav, rms_db, tm, write_wav)

# ---- Cabs --------------------------------------------------------------------------------------------

ALL_IRS = sorted(IRS.glob("*/*.wav"))
# Held out of the matcher's search, per split: 5 of the 15 Modern and 2 of the 6 Vintage each, disjoint. The
# factory presets' cab (Vintage 4x12, dynamic, upper, var. 2) stays searchable in both.
HELD_OUT = {
    "test": [1, 5, 9, 12, 13, 16, 19],
    "dev": [3, 4, 7, 10, 14, 15, 20],
}


def searchable_irs(split):
    return [p for i, p in enumerate(ALL_IRS) if i not in HELD_OUT[split]]


def load_ir(path):
    """The left channel, at most 1 s (as the cab plays a close mic), unit energy."""
    _, ir = read_wav(path)
    ir = (ir[:, 0] if ir.ndim == 2 else ir)[:SR]
    return ir / np.sqrt(np.sum(ir ** 2))


def minimum_phase(ir, n_fft=1 << 16):
    """The minimum-phase IR with the same magnitude response: the real cepstrum folded onto positive
    quefrencies (Oppenheim and Schafer, "Discrete-Time Signal Processing", the homomorphic method):
    c = IFFT(log |H|), c_min[0] = c[0], c_min[k] = 2 c[k] for 0 < k < N/2, c_min[N/2] = c[N/2], 0 above;
    H_min = exp(FFT(c_min)). Truncated to the IR's length with a 5 ms fade."""
    H = np.abs(np.fft.fft(ir, n_fft))
    c = np.fft.ifft(np.log(np.maximum(H, 1e-9 * H.max()))).real
    fold = np.zeros(n_fft)
    fold[0] = c[0]
    fold[1:n_fft // 2] = 2 * c[1:n_fft // 2]
    fold[n_fft // 2] = c[n_fft // 2]
    h = np.fft.ifft(np.exp(np.fft.fft(fold))).real[: len(ir)]
    fade = int(0.005 * SR)
    h[-fade:] *= np.linspace(1, 0, fade)
    return h


def warp_magnitude(ir, alpha, n_fft=1 << 15):
    """The IR's magnitude response moved in frequency, |H'(f)| = |H(f / alpha)| (every resonance alpha times
    higher), as a minimum-phase IR (a speaker's resonances moved: another cone, another cabinet size)."""
    H = np.abs(np.fft.rfft(ir, n_fft))
    f = np.arange(len(H))
    Hw = np.interp(f / alpha, f, H, right=H[-1])
    c = np.fft.irfft(np.log(np.maximum(Hw, 1e-9 * Hw.max())), n_fft)
    fold = np.zeros(n_fft)
    fold[0], fold[1:n_fft // 2], fold[n_fft // 2] = c[0], 2 * c[1:n_fft // 2], c[n_fft // 2]
    h = np.fft.irfft(np.exp(np.fft.rfft(fold)), n_fft)[: len(ir)]
    fade = int(0.005 * SR)
    h[-fade:] *= np.linspace(1, 0, fade)
    return h


def novel_ir(base, mods):
    """A new cab IR from a held-out one (module docstring). mods: list of [kind, params]."""
    h = load_ir(base)
    for kind, p in mods:
        if kind == "resonance_shift":
            h = warp_magnitude(h, p["alpha"])
        elif kind == "mic_distance":
            # A floor or baffle reflection d ms later, g down and darker: a comb with notches at odd multiples
            # of 1 / (2 d), and the direct sound's high end down a little (further away, more off the cone).
            d = int(p["delay_ms"] * SR / 1000.0)
            refl = filt(np.concatenate([np.zeros(d), h])[: len(h)], "lowpass", 4000.0)
            h = filt(h, "highshelf", 6000.0, gain_db=-p["hf_loss_db"]) + p["gain"] * refl
        elif kind == "off_axis":
            h = filt(h, "highshelf", p["f"], gain_db=-p["db"])
            h = filt(h, "peak", p["f"] * 0.8, q=1.2, gain_db=-0.4 * p["db"])
        elif kind == "small_speaker":
            h = filt(h, "highpass", p["hp"], q=0.9)
            h = filt(h, "peak", p["peak_f"], q=1.0, gain_db=p["peak_db"])
        elif kind == "big_cab":
            h = filt(h, "peak", p["f"], q=1.2, gain_db=p["db"])
        elif kind == "minimum_phase":
            h = minimum_phase(h)
        else:
            raise ValueError(kind)
    return h / np.sqrt(np.sum(h ** 2))


def cab_ir(spec):
    """The case's hidden cab IR (spec["cab"]: base index into ALL_IRS, mods)."""
    base = ALL_IRS[spec["base"]]
    return cached("ir", lambda: novel_ir(base, spec["mods"]), base.name, spec["mods"]) if spec["mods"] else load_ir(base)


# ---- Gray-box amps -------------------------------------------------------------------------------------

OS = 4                      # the nonlinear part runs at 4 x 48 kHz
FS_OS = SR * OS

# Passive tone stacks: Yeh and Smith's (DAFx 2006) three-knob network (amp_sim.tonestack_analog), with our
# own component values (three voices; not copied from any product).
STACKS = {
    "passive_scoop": dict(C1=250e-12, C2=47e-9, C3=22e-9, R1=250e3, R2=1e6, R3=25e3, R4=100e3),
    "passive_mid": dict(C1=680e-12, C2=100e-9, C3=47e-9, R1=100e3, R2=250e3, R3=10e3, R4=47e3),
    "passive_bright": dict(C1=1000e-12, C2=15e-9, C3=15e-9, R1=200e3, R2=500e3, R3=20e3, R4=68e3),
}

VOICINGS = {
    # Clean: two low-gain stages, bright cap, a scooped stack, a little sag.
    "clean_two_stage": dict(style="clean", in_hp=60, bright=(1800, 5.0), stages=[(2.0, 0.15, 2.2, 25, 14000, 0.05), (1.5, 0.1, 2.0, 20, 12000, 0.05)],
                            stack=("passive_scoop", 2), power=dict(drive=0.4, sag=0.15, sag_ms=80, crossover=0.0003, asym=0.0, presence=2.0, resonance=1.5)),
    # Clean: one stage into an active (Baxandall-style) tone control, a class-A-like asymmetric power amp.
    "clean_active": dict(style="clean", in_hp=40, bright=(2500, 2.0), stages=[(1.5, 0.2, 2.5, 15, 16000, 0.02)],
                         stack=("active", 1), power=dict(drive=0.5, sag=0.25, sag_ms=60, crossover=0.0, asym=0.25, presence=0.0, resonance=0.0)),
    # Edge of breakup: two stages, a mid-forward stack, the power amp pushed with sag (power-amp breakup).
    "edge_power": dict(style="edge", in_hp=70, bright=(2000, 3.0), stages=[(3.0, 0.2, 2.0, 40, 11000, 0.1), (2.0, 0.1, 1.8, 30, 10000, 0.05)],
                       stack=("passive_mid", 2), power=dict(drive=1.6, sag=0.45, sag_ms=70, crossover=0.0003, asym=0.1, presence=1.0, resonance=2.0)),
    # Edge of breakup: one hot stage straight into a class-A-like power amp, no master: the power amp is the sound.
    "edge_single": dict(style="edge", in_hp=50, bright=(3000, 4.0), stages=[(4.0, 0.3, 2.8, 20, 12000, 0.15)],
                        stack=("passive_bright", 1), power=dict(drive=2.0, sag=0.6, sag_ms=110, crossover=0.0, asym=0.35, presence=0.0, resonance=1.0)),
    # Crunch: three stages, the stack between the 2nd and 3rd, tight coupling.
    "crunch_three": dict(style="crunch", in_hp=90, bright=(2200, 2.0), stages=[(5.0, 0.25, 2.0, 120, 9000, 0.2), (2.5, 0.15, 1.8, 150, 8000, 0.15), (1.5, 0.1, 1.6, 60, 7000, 0.1)],
                         stack=("passive_mid", 2), power=dict(drive=1.2, sag=0.3, sag_ms=60, crossover=0.0003, asym=0.05, presence=2.5, resonance=2.0)),
    # Crunch: two hot stages and a saggy power amp.
    "crunch_sag": dict(style="crunch", in_hp=60, bright=(1500, 3.0), stages=[(6.0, 0.3, 2.4, 50, 8500, 0.25), (2.5, 0.2, 2.0, 40, 8000, 0.15)],
                       stack=("passive_scoop", 2), power=dict(drive=1.8, sag=0.55, sag_ms=90, crossover=0.0003, asym=0.1, presence=1.5, resonance=3.0)),
    # High gain: four stages, tight (200 Hz couplings), dark interstage filtering, a scooped stack, little sag.
    "hg_tight": dict(style="high_gain", in_hp=110, bright=(2500, 1.5), stages=[(8.0, 0.2, 1.8, 200, 7500, 0.15), (4.0, 0.15, 1.6, 220, 6500, 0.15), (2.5, 0.1, 1.5, 180, 6000, 0.1), (1.5, 0.05, 1.4, 80, 6000, 0.05)],
                     stack=("passive_scoop", 4), power=dict(drive=1.0, sag=0.1, sag_ms=50, crossover=0.0003, asym=0.0, presence=3.0, resonance=4.0)),
    # High gain: three stages with strong blocking (a fuzzier compression), a fat low end, a mid-forward stack.
    "hg_fat": dict(style="high_gain", in_hp=45, bright=(2000, 1.0), stages=[(8.0, 0.35, 2.4, 35, 8000, 0.4), (3.5, 0.25, 2.0, 30, 7000, 0.3), (2.0, 0.15, 1.8, 30, 6500, 0.15)],
                   stack=("passive_mid", 3), power=dict(drive=1.2, sag=0.35, sag_ms=80, crossover=0.0003, asym=0.05, presence=1.0, resonance=3.0)),
    # Saturated lead: four smooth stages, strong sag and compression, the stack early (after stage 1).
    "lead_smooth": dict(style="lead", in_hp=80, bright=(1800, 2.0), stages=[(8.0, 0.15, 1.3, 90, 7000, 0.2), (4.0, 0.1, 1.2, 120, 6500, 0.2), (3.0, 0.1, 1.2, 100, 6000, 0.15), (2.0, 0.05, 1.2, 60, 6000, 0.1)],
                        stack=("passive_mid", 1), power=dict(drive=1.5, sag=0.5, sag_ms=100, crossover=0.0003, asym=0.05, presence=1.5, resonance=2.0)),
    # Saturated lead: three stages, bright, the power amp saturating.
    "lead_bright": dict(style="lead", in_hp=70, bright=(3000, 4.0), stages=[(10.0, 0.25, 2.0, 70, 9000, 0.25), (4.0, 0.2, 1.8, 90, 8000, 0.2), (2.5, 0.1, 1.6, 50, 7500, 0.1)],
                        stack=("passive_bright", 3), power=dict(drive=2.0, sag=0.4, sag_ms=70, crossover=0.0003, asym=0.1, presence=3.0, resonance=1.5)),
}


def _one_pole_lp(x, fc, fs):
    a = math.exp(-2 * math.pi * fc / fs)
    return sps.lfilter([1 - a], [1, -a], x)


def _one_pole_hp(x, fc, fs):
    # y[n] = r (y[n-1] + x[n] - x[n-1]), r = exp(-2 pi fc / fs): a coupling capacitor into a grid resistor.
    r = math.exp(-2 * math.pi * fc / fs)
    return sps.lfilter([r, -r], [1, -r], x)


def triode(u, bias, cutoff_scale):
    """An inverting triode stage's transfer, asymmetric (Pakarinen and Yeh, "A review of digital techniques
    for modeling vacuum-tube guitar amplifiers", CMJ 33(2), 2009, sec. 3: grid conduction compresses the
    positive grid swing early and hard, cutoff limits the negative swing later and softer):
        v = u + bias;   y = tanh(v) for v >= 0,  c tanh(v / c) for v < 0   (c = cutoff_scale > 1)
    minus its value at rest (so silence gives 0), then inverted (a common-cathode stage inverts)."""
    v = u + bias
    y = np.where(v >= 0, np.tanh(v), cutoff_scale * np.tanh(v / cutoff_scale))
    rest = np.tanh(bias) if bias >= 0 else cutoff_scale * np.tanh(bias / cutoff_scale)
    return -(y - rest)


def active_stack(x, fs, bass, mid, treble):
    """An active (Baxandall-style) tone control: shelves at 150 Hz and 2.5 kHz and a mid bell at 700 Hz,
    +-10 dB each across the knob's 0 to 10."""
    x = filt(x, "lowshelf", 150.0, 0.7071, 2.0 * (bass - 5.0), fs)
    x = filt(x, "peak", 700.0, 0.7, 2.0 * (mid - 5.0), fs)
    return filt(x, "highshelf", 2500.0, 0.7071, 2.0 * (treble - 5.0), fs)


def graybox(x, name, drive_db, bass=5.0, mid=5.0, treble=5.0, master_db=0.0):
    """The DI through the gray-box voicing `name` (module docstring) with the first stage's gain moved by
    drive_db and the power amp's drive by master_db. Amp only (no cab)."""
    v = VOICINGS[name]
    x = filt(np.asarray(x, dtype=np.float64), "highpass", v["in_hp"], 0.7071)
    f_b, db_b = v["bright"]
    x = filt(x, "highshelf", f_b, 0.7071, db_b * float(np.clip(1.0 - drive_db / 24.0, 0.0, 1.0)))   # a bright cap fades as the gain rises
    u = sps.resample_poly(x, OS, 1)
    g0 = 10.0 ** (drive_db / 20.0)
    stack_name, stack_pos = v["stack"]

    def stack(u):
        if stack_name == "active":
            return active_stack(u, FS_OS, bass, mid, treble)
        t, m, l = np.clip(treble / 10, 0, 1), np.clip(mid / 10, 0, 1), amp_sim.log_taper(np.clip(bass / 10, 0, 1))
        b, a = amp_sim.tonestack_analog(t, m, l, **STACKS[stack_name])
        bz, az = sps.bilinear(b, a, FS_OS)
        return sps.lfilter(bz, az, u) * 5.0       # the recovery stage after a passive stack's loss

    for i, (gain, bias, cut, hp, lp, shift) in enumerate(v["stages"]):
        g = gain * (g0 if i == 0 else 1.0)
        # Blocking: a grid driven past its bias into conduction charges its coupling capacitor, which shifts
        # the bias by the recent conduction (nothing below about half the clipping level).
        env = _one_pole_lp(np.maximum(np.abs(g * u) - 0.5, 0.0), 30.0, FS_OS)
        u = triode(g * u - shift * env, bias, cut)
        u = _one_pole_hp(u, hp, FS_OS)
        u = _one_pole_lp(u, lp, FS_OS)
        if i + 1 == stack_pos:
            u = stack(u)
    if stack_pos > len(v["stages"]):
        u = stack(u)

    p = v["power"]
    k = p["drive"] * 10.0 ** (master_db / 20.0)
    # Supply sag: the rail droops with the recent output power (a rectifier's and filter cap's time constant),
    # lowering the clipping level and the gain: h = 1 / (1 + sag sqrt(<(k u)^2>)).
    env = np.sqrt(_one_pole_lp((k * u) ** 2, 1000.0 / (2 * math.pi * p["sag_ms"]), FS_OS))
    h = 1.0 / (1.0 + p["sag"] * env)
    w = k * u
    # Push-pull: symmetric soft clipping, a crossover notch (the output tubes hand over near zero), and some
    # class-A-like asymmetry (an unbalanced pair) if asked.
    # Crossover: within about +-c of zero the slope halves, w - c/2 tanh(w / c) (slope 1/2 at 0, 1 beyond c).
    if p["crossover"] > 0:
        w = w - 0.5 * p["crossover"] * np.tanh(w / p["crossover"])
    w = w + p["asym"] * w ** 2 / (1.0 + np.abs(w))
    y = h * np.tanh(w / h)
    y = sps.resample_poly(y, 1, OS)
    y = filt(y, "highshelf", 3500.0, 0.7071, p["presence"])
    y = filt(y, "peak", 100.0, 1.0, p["resonance"])
    y = _one_pole_hp(y, 40.0, SR)                 # the output transformer
    y = filt(y, "lowpass", 14000.0, 0.7071)
    return y


# ---- NAM amps -----------------------------------------------------------------------------------------

NAM_AMPS = {
    "nam_lstm": dict(style="clean", file="lstm.nam"),
    "nam_wavenet": dict(style="edge", file="wavenet.nam"),
    "nam_a2": dict(style="high_gain", file="A2.nam"),
    "nam_slimmable": dict(style="high_gain", file="slimmable_container.nam"),
    "nam_standard": dict(style="lead", file="wavenet_a1_standard.nam"),
}


def nam_amp(x, name, input_db):
    return ampsim_render(x, ["--model", NAM_EXAMPLES / NAM_AMPS[name]["file"], "--input-gain", f"{input_db:.3f}"], "nam")


def amp_out(x, amp):
    """amp: {"kind": "graybox"|"nam", "name", "drive_db", knobs...}."""
    if amp["kind"] == "graybox":
        return cached("gb", lambda: graybox(x, amp["name"], amp["drive_db"], **amp.get("knobs", {})), x, amp)
    return nam_amp(x, amp["name"], amp["drive_db"])


# ---- Pedals -------------------------------------------------------------------------------------------

def pedal_args(p):
    if p is None:
        return None
    if p["kind"] == "boost":
        return ["--boost", "clean", "--boost-level", f"{p['level']:.2f}", "--boost-tilt", f"{p['tilt']:.2f}"]
    if p["kind"] == "comp":
        return ["--pre-comp", f"pedal:peak:{p['threshold']:.1f}:{p['ratio']:.2f}:2:{p['release']:.0f}:{p['makeup']:.2f}:1"]
    return ["--od", f"{p['kind']}:{p['drive']:.3f}:{p['tone']:.3f}:{p['level']:.2f}:{p.get('tight', 20.0):.0f}"]


def pedal_out(x, p):
    args = pedal_args(p)
    return x if args is None else ampsim_render(x, args, "pedal")


# ---- The tone rig and the production --------------------------------------------------------------------

def post_comp(x, c):
    return ampsim_render(x, ["--post-comp", f"studio:rms:{c['threshold']:.2f}:{c['ratio']:.2f}:{c['attack']:.1f}:{c['release']:.0f}:0:1"], "comp")


def tone_rig(x, rig, stages=("pedal", "amp", "cab", "eq", "comp")):
    """The DI through the hidden rig's tone (no time effects). `stages` leaves parts out (the oracle's
    ablations)."""
    y = np.asarray(x, dtype=np.float64)
    if "pedal" in stages:
        y = pedal_out(y, rig["pedal"])
    y = amp_out(y, rig["amp"])
    if "cab" in stages:
        y = sps.fftconvolve(y, cab_ir(rig["cab"]))[: len(y)]
    if "eq" in stages:
        y = apply_eq(y, rig["mic_eq"])
    if "comp" in stages and rig.get("bus_comp"):
        y = post_comp(y, rig["bus_comp"])
    return y


def time_fx(y, fx, seed, stereo=False):
    """The record's time effect (fx: {"kind": plate|room|slap|dotted, ...}), mixed in at fx["mix_db"] (the wet
    RMS relative to the dry's). Returns the dry plus the wet, mono or (L, R)."""
    if not fx:
        return (y, y) if stereo else y
    rng = np.random.default_rng(seed)
    kind = fx["kind"]

    def tail_ir(rt60, pre_ms, early, nseed):
        r = np.random.default_rng(nseed)
        n = int(rt60 * 1.2 * SR)
        t = np.arange(n) / SR
        noise = r.normal(0, 1, n)
        # Frequency-dependent decay: three bands, the lows a little longer and the highs much shorter.
        lo = filt(noise, "lowpass", 500.0) * 10 ** (-3 * t / (1.2 * rt60))
        mid = filt(filt(noise, "highpass", 500.0), "lowpass", 4000.0) * 10 ** (-3 * t / rt60)
        hi = filt(noise, "highpass", 4000.0) * 10 ** (-3 * t / (0.55 * rt60))
        ir = lo + mid + 0.7 * hi
        ir *= np.clip(t / 0.01, 0, 1)                       # a 10 ms build-up
        for d, g in early:
            k = int(d * SR / 1000.0)
            if k < n:
                ir[k] += g * 30.0
        return np.concatenate([np.zeros(int(pre_ms * SR / 1000.0)), ir])

    def wet_of(nseed):
        if kind == "plate":
            return sps.fftconvolve(y, tail_ir(fx["rt60"], fx["predelay_ms"], [], nseed))[: len(y)]
        if kind == "room":
            early = [(float(d), float(g)) for d, g in zip(rng.uniform(4, 35, 8), rng.uniform(0.2, 0.6, 8))]
            return sps.fftconvolve(y, tail_ir(fx["rt60"], 2.0, early, nseed))[: len(y)]
        if kind == "slap":
            d = int(fx["delay_ms"] * SR / 1000.0)
            return filt(np.concatenate([np.zeros(d), y])[: len(y)], "lowpass", 3000.0)
        if kind == "dotted":
            # y[n - d] g + the delay's own output fed back: H(z) = z^-d / (1 - fb z^-d), the echoes darkened.
            d = int(fx["delay_s"] * SR)
            a = np.zeros(d + 1)
            a[0], a[d] = 1.0, -fx["feedback"]
            b = np.zeros(d + 1)
            b[d] = 1.0
            return filt(sps.lfilter(b, a, y), "lowpass", 3500.0)
        raise ValueError(kind)

    def mixed(wet):
        wet = wet * 10.0 ** (fx["mix_db"] / 20.0) * math.sqrt(np.mean(y ** 2) / (np.mean(wet ** 2) + 1e-20))
        return y + wet

    if not stereo:
        return mixed(wet_of(seed))
    return mixed(wet_of(seed)), mixed(wet_of(seed + 1) if kind in ("plate", "room") else wet_of(seed))


def limiter(x, push_db, release_ms, ceiling_db=-1.0, lookahead_ms=1.5):
    """A mastering-style brickwall limiter (x: (channels, n)): the input pushed by push_db into a ceiling.
    Offline, so the lookahead needs no delay: the reduction each sample needs, 1 - min(1, ceiling / peak),
    is held over +-lookahead (a running maximum, so the gain is already down when the peak arrives),
    released by a one-pole (the larger of the held and the released value), smoothed over the lookahead
    (a moving average, the attack ramp), and never less than the sample's own need; then a safety clip at the
    ceiling. Linked across channels."""
    from scipy.ndimage import maximum_filter1d, uniform_filter1d
    x = np.atleast_2d(x) * 10.0 ** (push_db / 20.0)
    ceil = 10.0 ** (ceiling_db / 20.0)
    la = max(1, int(lookahead_ms * SR / 1000.0))
    need = 1.0 - np.minimum(1.0, ceil / np.maximum(np.max(np.abs(x), axis=0), 1e-9))
    hold = maximum_filter1d(need, size=2 * la + 1)
    a = math.exp(-1.0 / (release_ms * 0.001 * SR))
    gr = np.maximum(hold, sps.lfilter([1 - a], [1, -a], hold))
    gr = np.maximum(uniform_filter1d(gr, la), need)
    return np.clip(x * (1.0 - gr), -ceil, ceil)


def codec(x, c):
    """x (channels, n) at 48 kHz, as a record: resampled to 44.1 kHz, encoded (AAC with macOS's afconvert, or
    MP3 with LAME through lameenc), decoded by macOS (as the app's CoreAudio decoder does), resampled back to
    48 kHz. Returns (channels, n), the encoder's delay left in as a real file has it, trimmed to n."""
    if not c:
        return x
    x = np.atleast_2d(x)

    def run():
        x441 = sps.resample_poly(x, 147, 160, axis=1)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            src = tmp / "in.wav"
            pcm = np.clip(np.round(x441.T * 32767.0), -32768, 32767).astype(np.int16)
            import scipy.io.wavfile as wavfile
            wavfile.write(src, 44100, pcm)
            if c["kind"] == "aac":
                enc = tmp / "out.m4a"
                subprocess.run(["afconvert", "-f", "m4af", "-d", "aac", "-b", str(int(c["kbps"] * 1000)), str(src), str(enc)], check=True,
                               capture_output=True)
            else:
                import lameenc
                e = lameenc.Encoder()
                e.set_bit_rate(int(c["kbps"]))
                e.set_in_sample_rate(44100)
                e.set_channels(pcm.shape[1])
                e.set_quality(2)
                enc = tmp / "out.mp3"
                enc.write_bytes(e.encode(pcm.tobytes()) + e.flush())
            dec = tmp / "dec.wav"
            subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEF32", str(enc), str(dec)], check=True, capture_output=True)
            _, y = read_wav(dec)
        y = np.atleast_2d(y.T if y.ndim == 2 else y[None])
        y48 = sps.resample_poly(y, 160, 147, axis=1)
        return np.stack([fit_length(ch, x.shape[1]) for ch in y48])

    return cached("codec", run, x.ravel(), c)


# ---- Calibration ------------------------------------------------------------------------------------------

def nl_ratio_db(x, y):
    """make_default_captures.nl_ratio_db: the share of y's power (60 Hz to 12 kHz) no linear filter of x
    explains, dB, from the magnitude-squared coherence C(f): sum (1 - C) Pyy / sum Pyy."""
    n = min(len(x), len(y))
    f, c = sps.coherence(x[:n], y[:n], fs=SR, nperseg=4096)
    _, pyy = sps.welch(y[:n], fs=SR, nperseg=4096)
    band = (f > 60) & (f < 12000)
    return float(10 * np.log10(np.sum((1 - c[band]) * pyy[band]) / np.sum(pyy[band])))


def calibrate_drive(x, amp, pedal, target_nl, lo=-30.0, hi=30.0, step=6.0, iters=6):
    """The amp's drive (dB) that puts the amp's own distortion at target_nl: the ratio between the amp's input
    (x through the pedal, if any) and its output, so a compressor pedal's gain changes, which the ratio counts
    as nonlinear, don't take the amp's share (a drive pedal adds its distortion on top, as it would). A grid every
    6 dB from lo to hi, then bisection in the bracket where the ratio crosses the target on its way up (the
    last such crossing: at very low drives a power amp's crossover can make the ratio rise again as the
    signal vanishes). Returns (drive_db, the ratio reached)."""
    xp = pedal_out(x, pedal)
    nl = lambda d: nl_ratio_db(xp, amp_out(xp, dict(amp, drive_db=float(d))))
    grid = np.arange(lo, hi + 1e-9, step)
    vals = [nl(d) for d in grid]
    a, b = grid[0], grid[0]
    for i in range(len(grid) - 1, 0, -1):
        if vals[i - 1] < target_nl <= vals[i]:
            a, b = grid[i - 1], grid[i]
            break
    else:
        d = grid[int(np.argmin(np.abs(np.array(vals) - target_nl)))]
        return float(d), float(nl(d))
    for _ in range(iters):
        m = 0.5 * (a + b)
        if nl(m) < target_nl:
            a = m
        else:
            b = m
    d = 0.5 * (a + b)
    return float(d), float(nl(d))

"""
Offline gray-box amp modeler (phase 1).

Signal chain:
  input HPF -> pre-emphasis (mid boost + bass cut) -> preamp gain
  -> OVERSAMPLED asymmetric tanh (tube-ish clipping) -> DC blocker
  -> tone stack (Yeh & Smith 2006 circuit model, knobs interact like a real amp)
  -> power amp (softer symmetric clipping)
  -> cab sim (real IR if you give one, crude filter fallback otherwise)

Usage:
  python amp_sim.py                         # no input: synthesizes a test riff
  python amp_sim.py my_di.wav               # process your own DI recording
  python amp_sim.py my_di.wav --ir cab.wav  # use a real cab impulse response
  python amp_sim.py my_di.wav --oversample 1   # turn off oversampling, hear aliasing
  python amp_sim.py my_di.wav --channel clean --bass 4 --mid 6 --treble 7
  python amp_sim.py my_di.wav --all-channels   # render every channel for A/B

Knobs are 0-10 like a real amp.
"""

import argparse
import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter, resample_poly, fftconvolve, bilinear


# ---------------------------------------------------------------- I/O

def load_wav(path):
    fs, x = wavfile.read(path)
    # Convert whatever integer format the file uses into floats in [-1, 1]
    if x.dtype == np.int16:
        x = x / 32768.0
    elif x.dtype == np.int32:
        x = x / 2147483648.0
    elif x.dtype == np.uint8:
        x = (x - 128) / 128.0
    x = x.astype(np.float64)
    if x.ndim > 1:          # stereo -> mono
        x = x.mean(axis=1)
    return fs, x


def save_wav(path, fs, x):
    x = x / (np.max(np.abs(x)) + 1e-12) * 0.9   # normalize so it never clips the file
    wavfile.write(path, fs, (x * 32767).astype(np.int16))


def synth_test_riff(fs, seconds=4.0):
    """Karplus-Strong plucked strings: fake DI guitar so you can run this immediately."""
    out = np.zeros(int(fs * seconds))
    # (start time s, frequency Hz): a little E power chord riff
    notes = [(0.0, 82.41), (0.0, 123.47), (0.5, 82.41), (0.5, 123.47),
             (1.0, 98.00), (1.0, 146.83), (1.5, 110.0), (1.5, 164.81),
             (2.0, 82.41), (2.0, 123.47), (2.0, 164.81)]
    rng = np.random.default_rng(0)
    for t0, f in notes:
        period = int(fs / f)
        buf = rng.uniform(-1, 1, period)
        n = int(fs * 1.5)
        note = np.zeros(n)
        for i in range(n):
            note[i] = buf[i % period]
            buf[i % period] = 0.996 * 0.5 * (buf[i % period] + buf[(i + 1) % period])
        start = int(t0 * fs)
        end = min(start + n, len(out))
        out[start:end] += note[: end - start]
    return out * 0.3


# ---------------------------------------------------------------- filters
# Biquad coefficients from the RBJ "Audio EQ Cookbook". Every filter below is
# b0 + b1 z^-1 + b2 z^-2 over a0 + a1 z^-1 + a2 z^-2, applied with lfilter.

def biquad(kind, f0, fs, q=0.707, gain_db=0.0):
    A = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / fs
    cw, sw = np.cos(w0), np.sin(w0)
    alpha = sw / (2 * q)

    if kind == "lowpass":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "highpass":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
        a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "peak":
        b = [1 + alpha * A, -2 * cw, 1 - alpha * A]
        a = [1 + alpha / A, -2 * cw, 1 - alpha / A]
    elif kind == "lowshelf":
        sa = 2 * np.sqrt(A) * alpha
        b = [A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw),
             A * ((A + 1) - (A - 1) * cw - sa)]
        a = [(A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw),
             (A + 1) + (A - 1) * cw - sa]
    elif kind == "highshelf":
        sa = 2 * np.sqrt(A) * alpha
        b = [A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw),
             A * ((A + 1) + (A - 1) * cw - sa)]
        a = [(A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw),
             (A + 1) - (A - 1) * cw - sa]
    else:
        raise ValueError(kind)
    b, a = np.array(b), np.array(a)
    return b / a[0], a / a[0]


def filt(x, kind, f0, fs, q=0.707, gain_db=0.0):
    b, a = biquad(kind, f0, fs, q, gain_db)
    return lfilter(b, a, x)


def dc_block(x, r=0.995):
    # y[n] = x[n] - x[n-1] + r*y[n-1]. Asymmetric clipping creates a DC offset; this kills it.
    return lfilter([1, -1], [1, -r], x)


# ---------------------------------------------------------------- nonlinear stages

def tube_clip(x, drive, bias=0.2):
    """Asymmetric soft clip. The bias makes positive and negative halves clip
    differently, which adds even harmonics (the 'warm' tube thing)."""
    return np.tanh(drive * x + bias) - np.tanh(bias)


def oversampled(fn, x, factor):
    """Upsample, apply the nonlinearity, downsample. resample_poly includes the
    anti-aliasing lowpass, so harmonics above the original Nyquist get removed
    instead of folding back down as inharmonic junk."""
    if factor == 1:
        return fn(x)
    up = resample_poly(x, factor, 1)
    return resample_poly(fn(up), 1, factor)


# ---------------------------------------------------------------- tone stack
# The classic "FMV" (Fender/Marshall/Vox) tone stack is ONE passive network of
# 3 caps, 4 resistors, and 3 pots. Every knob changes every coefficient, which
# is why the knobs interact on a real amp.
#
# Yeh & Smith (DAFx 2006) did nodal analysis on the circuit and got a 3rd-order
# analog transfer function:
#
#   H(s) = (b3 s^3 + b2 s^2 + b1 s) / (a3 s^3 + a2 s^2 + a1 s + a0)
#
# t, m, l are the treble, mid, bass pot positions in [0, 1]. We then use the
# bilinear transform (s -> 2*fs*(z-1)/(z+1)) to turn it into a digital filter.
# Being passive, it can only cut, never boost: max gain is 0 dB. That's why
# real amps put a "recovery" gain stage right after it.

TONESTACKS = {
    # '59 Fender Bassman
    "fender":   dict(C1=250e-12, C2=20e-9, C3=20e-9, R1=250e3, R2=1e6, R3=25e3, R4=56e3),
    # Marshall JCM800 (same topology, different values -> different voice)
    "marshall": dict(C1=470e-12, C2=22e-9, C3=22e-9, R1=220e3, R2=1e6, R3=22e3, R4=33e3),
}


def tonestack_analog(t, m, l, C1, C2, C3, R1, R2, R3, R4):
    b1 = t*C1*R1 + m*C3*R3 + l*(C1*R2 + C2*R2) + (C1*R3 + C2*R3)
    b2 = (t*(C1*C2*R1*R4 + C1*C3*R1*R4) - m*m*(C1*C3*R3**2 + C2*C3*R3**2)
          + m*(C1*C3*R1*R3 + C1*C3*R3**2 + C2*C3*R3**2)
          + l*(C1*C2*R1*R2 + C1*C2*R2*R4 + C1*C3*R2*R4)
          + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3)
          + (C1*C2*R1*R3 + C1*C2*R3*R4 + C1*C3*R3*R4))
    b3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4)
          - m*m*(C1*C2*C3*R1*R3**2 + C1*C2*C3*R3**2*R4)
          + m*(C1*C2*C3*R1*R3**2 + C1*C2*C3*R3**2*R4)
          + t*C1*C2*C3*R1*R3*R4 - t*m*C1*C2*C3*R1*R3*R4
          + t*l*C1*C2*C3*R1*R2*R4)
    a0 = 1.0
    a1 = (C1*R1 + C1*R3 + C2*R3 + C2*R4 + C3*R4) + m*C3*R3 + l*(C1*R2 + C2*R2)
    a2 = (m*(C1*C3*R1*R3 - C2*C3*R3*R4 + C1*C3*R3**2 + C2*C3*R3**2)
          + l*m*(C1*C3*R2*R3 + C2*C3*R2*R3)
          - m*m*(C1*C3*R3**2 + C2*C3*R3**2)
          + l*(C1*C2*R2*R4 + C1*C2*R1*R2 + C1*C3*R2*R4 + C2*C3*R2*R4)
          + (C1*C2*R1*R4 + C1*C3*R1*R4 + C1*C2*R3*R4 + C1*C2*R1*R3
             + C1*C3*R3*R4 + C2*C3*R3*R4))
    a3 = (l*m*(C1*C2*C3*R1*R2*R3 + C1*C2*C3*R2*R3*R4)
          - m*m*(C1*C2*C3*R1*R3**2 + C1*C2*C3*R3**2*R4)
          + m*(C1*C2*C3*R3**2*R4 + C1*C2*C3*R1*R3**2 - C1*C2*C3*R1*R3*R4)
          + l*C1*C2*C3*R1*R2*R4 + C1*C2*C3*R1*R3*R4)
    return [b3, b2, b1, 0.0], [a3, a2, a1, a0]


def log_taper(knob01):
    # Real bass pots are logarithmic ("audio taper"): most of the change
    # happens near the top of the rotation. Maps [0,1] -> [~0.03, 1].
    return np.exp((knob01 - 1.0) * 3.5)


def tonestack(x, fs, bass, mid, treble, voice="fender"):
    """bass/mid/treble are 0-10 knob positions."""
    t = np.clip(treble / 10, 0, 1)
    m = np.clip(mid / 10, 0, 1)
    l = log_taper(np.clip(bass / 10, 0, 1))
    b, a = tonestack_analog(t, m, l, **TONESTACKS[voice])
    bz, az = bilinear(b, a, fs)
    return lfilter(bz, az, x)


# ---------------------------------------------------------------- channels
# A channel is just a different set of circuit choices. Same building blocks,
# different values. This is how real multi-channel amps work too.

CHANNELS = {
    "clean": dict(
        voice="fender",
        low_cut_db=0.0, mid_push_db=0.0,     # flat pre-emphasis: full-range clean
        stages=[(1.5, 0.05)],                # one gentle stage: (drive, bias)
        interstage_lp=10000,
    ),
    "crunch": dict(
        voice="marshall",
        low_cut_db=-4.0, mid_push_db=4.0,
        stages=[(6.0, 0.2), (3.0, 0.1)],
        interstage_lp=7000,
    ),
    "lead": dict(
        voice="marshall",
        low_cut_db=-9.0, mid_push_db=8.0,    # tight: kill lows before heavy clipping
        stages=[(10.0, 0.25), (6.0, 0.15), (3.0, 0.1)],
        interstage_lp=6000,
    ),
}


# ---------------------------------------------------------------- the amp

def amp(x, fs, channel="crunch", gain=5.0, bass=5.0, mid=5.0, treble=5.0,
        master=5.0, oversample=4, ir=None):
    ch = CHANNELS[channel]

    # 1. Input: get rid of sub-bass rumble that would make distortion muddy
    x = filt(x, "highpass", 80, fs)

    # 2. Pre-emphasis: what gets boosted BEFORE the clipper distorts hardest.
    x = filt(x, "lowshelf", 250, fs, gain_db=ch["low_cut_db"])
    x = filt(x, "peak", 800, fs, q=0.8, gain_db=ch["mid_push_db"])

    # 3. Preamp. The gain knob (0-10) scales the first stage's drive
    #    exponentially, because volume is perceived logarithmically.
    gain_mult = 10 ** ((gain - 5) / 10)      # knob 0 -> 0.32x, 5 -> 1x, 10 -> 3.2x
    for i, (drive, bias) in enumerate(ch["stages"]):
        d = drive * gain_mult if i == 0 else drive
        x = oversampled(lambda s, d=d, bias=bias: tube_clip(s, d, bias), x, oversample)
        x = dc_block(x)
        x = filt(x, "lowpass", ch["interstage_lp"], fs)   # Miller-cap-ish rolloff

    # 4. Tone stack, then a recovery stage to make up the ~10-20 dB it eats
    x = tonestack(x, fs, bass, mid, treble, ch["voice"])
    x = x * 6.0

    # 5. Power amp: gentler, symmetric clipping
    master_mult = 10 ** ((master - 5) / 10)
    x = oversampled(lambda s: np.tanh(master_mult * s), x, oversample)

    # 6. Cabinet
    if ir is not None:
        x = fftconvolve(x, ir)[: len(x)]
    else:
        # Crude fake 4x12: speakers barely reproduce anything above ~5 kHz,
        # which is why raw distortion without a cab sounds like a fizzy bee.
        x = filt(x, "highpass", 90, fs)
        x = filt(x, "peak", 120, fs, q=1.2, gain_db=3)      # cab resonance thump
        x = filt(x, "peak", 400, fs, q=1.0, gain_db=-3)     # typical mid scoop
        x = filt(x, "peak", 2500, fs, q=1.5, gain_db=4)     # speaker presence peak
        x = filt(x, "lowpass", 5000, fs)
        x = filt(x, "lowpass", 5000, fs)                    # twice = steeper rolloff
    return x


# ---------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser()
    p.add_argument("input", nargs="?", help="DI wav file (omit to use a synth test riff)")
    p.add_argument("--out", default="out.wav")
    p.add_argument("--ir", help="cab impulse response wav")
    p.add_argument("--channel", choices=list(CHANNELS), default="crunch")
    p.add_argument("--all-channels", action="store_true",
                   help="render out_clean.wav, out_crunch.wav, out_lead.wav")
    for knob in ["gain", "bass", "mid", "treble", "master"]:
        p.add_argument(f"--{knob}", type=float, default=5.0, help="0-10")
    p.add_argument("--oversample", type=int, default=4)
    args = p.parse_args()

    if args.input:
        fs, x = load_wav(args.input)
    else:
        fs = 48000
        x = synth_test_riff(fs)
        save_wav("test_di.wav", fs, x)
        print("No input given, wrote synthetic DI to test_di.wav")

    ir = None
    if args.ir:
        ir_fs, ir = load_wav(args.ir)
        if ir_fs != fs:
            ir = resample_poly(ir, fs, ir_fs)
        ir = ir / np.sum(np.abs(ir))

    channels = list(CHANNELS) if args.all_channels else [args.channel]
    for ch in channels:
        y = amp(x, fs, ch, args.gain, args.bass, args.mid, args.treble,
                args.master, args.oversample, ir)
        out = args.out.replace(".wav", f"_{ch}.wav") if args.all_channels else args.out
        save_wav(out, fs, y)
        print(f"Wrote {out}")


if __name__ == "__main__":
    main()

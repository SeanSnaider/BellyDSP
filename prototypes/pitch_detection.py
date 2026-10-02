"""
Pitch detection study for the tuner and harmonizer (BUILD_PLAN "Tuner"): YIN against the McLeod Pitch
Method on synthetic guitar-like tones, through the same analysis path the C++ will use (low-pass and
decimate 4x to 12 kHz, a window of about two periods of the lowest note).

  YIN (de Cheveigne and Kawahara, JASA 2002):
    d(tau)  = sum_j (x_j - x_{j+tau})^2                       difference function
    d'(tau) = d(tau) / ((1/tau) sum_{k=1..tau} d(k))           cumulative mean normalized (d'(0) = 1)
    the first tau where d' dips below a threshold (0.1 to 0.15), then the minimum of that dip,
    refined by parabolic interpolation of d' around it.
  McLeod Pitch Method (McLeod and Wyvill, ICMC 2005):
    n(tau) = 2 r(tau) / m(tau),  r = autocorrelation,  m = sum of the squares of both overlapping parts
    (the normalized square difference function, in [-1, 1]); take the positive-lobe maxima ("key
    maxima"), choose the first one above k = 0.93 x the highest, refine by parabolic interpolation.

Test tones: sines, band-limited sawtooths, inharmonic stiff strings (partial n at n f0 sqrt(1 + B n^2),
B from 5e-5 to 3e-4, decaying faster with n), and a low "wound string" with a second harmonic 6 dB above
the fundamental (the classic octave-error trap), each with a little noise. Frequencies 30 Hz to 1.3 kHz.
Accuracy target (BUILD_PLAN): within 0.5 cent; no octave errors.

The "true" pitch of a stiff string is its first partial f0 sqrt(1 + B); that's what a tuner should show.

Result (this study): both detectors are octave-safe on these tones, but neither reaches 0.5 cent alone.
Pure sines are fine (YIN and MPM within ~0.1 cent below 500 Hz); at 12 kHz a high note's period is only
~9 samples, so parabolic interpolation costs YIN 10 cents at 1.3 kHz (MPM 1 cent); and on stiff strings
the stretched upper partials make the waveform's periodicity 10 to 14 cents sharp of the first partial.
So the design is two stages:
  1. coarse, octave-safe: MPM on the 12 kHz signal (better than YIN at high notes, equal elsewhere, and
     it's the detector the harmonizer will share);
  2. fine, for the tuner's readout: band-pass the 48 kHz signal around the coarse estimate (two SVF
     band-pass stages, Q 3, which leave mostly the first partial), then time its upward zero crossings
     across many periods (linear interpolation between samples) over a window of at least 12 periods.
With that, every tone here (30 Hz to 1.3 kHz, sines, saws, stiff strings, a dominant 2nd harmonic) reads
within 0.5 cent of its first partial.

Usage:  uv run --with numpy --with scipy python prototypes/pitch_detection.py
"""

import math

import numpy as np
from scipy.signal import resample_poly

FS = 48000
DECIMATION = 4
FSD = FS // DECIMATION


def stiff_string (f0, B, seconds, partials=30, fs=FS, second_boost_db=0.0, seed=0):
    rng = np.random.default_rng (seed)
    t = np.arange (int (seconds * fs)) / fs
    x = np.zeros_like (t)
    for n in range (1, partials + 1):
        fn = n * f0 * math.sqrt (1 + B * n * n)
        if fn > 0.45 * fs:
            break
        amp = 1.0 / n * (10 ** (second_boost_db / 20) if n == 2 else 1.0)
        decay = math.exp (-0.8 * n) * 0 + 1.0 / (0.6 + 0.15 * n)  # higher partials die faster
        x += amp * np.sin (2 * math.pi * fn * t + rng.uniform (0, 2 * math.pi)) * np.exp (-t / decay)
    return x / np.max (np.abs (x)) * 0.5, f0 * math.sqrt (1 + B)


def sawtooth (f0, seconds, fs=FS):
    t = np.arange (int (seconds * fs)) / fs
    x = np.zeros_like (t)
    n = 1
    while n * f0 < 0.45 * fs:
        x += np.sin (2 * math.pi * n * f0 * t) / n
        n += 1
    return 0.3 * x, f0


def sine (f0, seconds, fs=FS):
    t = np.arange (int (seconds * fs)) / fs
    return 0.5 * np.sin (2 * math.pi * f0 * t), f0


def decimate (x):
    return resample_poly (x, 1, DECIMATION)  # anti-aliased by a long FIR, as a stand-in for the C++ filter


def parabolic (y, i):
    if i <= 0 or i >= len (y) - 1:
        return float (i)
    a, b, c = y[i - 1], y[i], y[i + 1]
    denom = a - 2 * b + c
    return i + 0.5 * (a - c) / denom if denom != 0 else float (i)


def yin (frame, fs, fmin, fmax, threshold=0.12):
    w = len (frame) // 2
    tau_max = min (int (fs / fmin) + 2, w)
    tau_min = max (2, int (fs / fmax))
    d = np.array ([np.sum ((frame[:w] - frame[tau:tau + w]) ** 2) for tau in range (tau_max + 1)])
    cmnd = np.ones_like (d)
    running = 0.0
    for tau in range (1, len (d)):
        running += d[tau]
        cmnd[tau] = d[tau] * tau / running if running > 0 else 1.0
    tau = tau_min
    while tau < tau_max:
        if cmnd[tau] < threshold:
            while tau + 1 < tau_max and cmnd[tau + 1] < cmnd[tau]:
                tau += 1
            return fs / parabolic (cmnd, tau)
        tau += 1
    best = tau_min + int (np.argmin (cmnd[tau_min:tau_max]))
    return fs / parabolic (cmnd, best)


def mpm (frame, fs, fmin, fmax, k=0.93):
    w = len (frame) // 2
    tau_max = min (int (fs / fmin) + 2, w)
    n = np.zeros (tau_max + 1)
    for tau in range (tau_max + 1):
        a, b = frame[:w], frame[tau:tau + w]
        m = np.sum (a * a) + np.sum (b * b)
        n[tau] = 2 * np.sum (a * b) / m if m > 0 else 0.0
    # Key maxima: the highest point of each positive lobe after the first negative-going zero crossing.
    maxima = []
    tau = 1
    while tau < len (n) and n[tau] > 0:
        tau += 1
    while tau < len (n):
        while tau < len (n) and n[tau] <= 0:
            tau += 1
        start = tau
        while tau < len (n) and n[tau] > 0:
            tau += 1
        if start < tau:
            peak = start + int (np.argmax (n[start:tau]))
            maxima.append (peak)
    maxima = [m for m in maxima if fs / m <= fmax * 1.05]
    if not maxima:
        return 0.0
    highest = max (n[m] for m in maxima)
    for m in maxima:
        if n[m] >= k * highest:
            return fs / parabolic (n, m)
    return 0.0


def svf_bandpass (x, fc, q, fs=FS):
    """Simper SVF band-pass, unit gain at fc (the C++ Svf's bandpass type)."""
    g = math.tan (math.pi * fc / fs)
    k = 1 / q
    a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2
    ic1 = ic2 = 0.0
    y = np.zeros_like (x)
    for i, v0 in enumerate (x):
        v3 = v0 - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2 * v1 - ic1
        ic2 = 2 * v2 - ic2
        y[i] = k * v1
    return y


def refine (x48, coarse, q=3.0, stages=2, fs=FS):
    """The fine stage: band-pass around the coarse estimate, then time zero crossings over the last
    max(0.25 s, 12 periods) of the buffer (the filters settle on what comes before)."""
    y = x48
    for _ in range (stages):
        y = svf_bandpass (y, coarse, q, fs)
    window = int (max (0.25, 12.0 / coarse) * fs)
    seg = y[-window:]
    crossings = [i - 1 + seg[i - 1] / (seg[i - 1] - seg[i]) for i in range (1, len (seg)) if seg[i - 1] < 0 <= seg[i]]
    if len (crossings) < 2:
        return coarse
    return fs * (len (crossings) - 1) / (crossings[-1] - crossings[0])


def cents (measured, true):
    return 1200 * math.log2 (measured / true) if measured > 0 else float ("inf")


def evaluate():
    freqs = [30.0, 41.2, 55.0, 82.4, 110.0, 146.8, 196.0, 246.9, 329.6, 440.0, 659.3, 880.0, 1318.5]
    fmin, fmax = 28.0, 1400.0
    window = int (2.1 * FSD / 30.0)  # about two periods of 30 Hz at 12 kHz
    window += window % 2
    rng = np.random.default_rng (5)
    errors = { "YIN": [], "MPM": [], "MPM + refine": [] }
    octave_errors = { k: 0 for k in errors }
    worst = { k: (0.0, "") for k in errors }
    cases = []
    for f in freqs:
        cases.append (("sine", *sine (f, 1.0)))
        cases.append (("saw", *sawtooth (f, 1.0)))
        for B in (5e-5, 3e-4):
            cases.append ((f"stiff B={B:g}", *stiff_string (f, B, 1.0, seed=int (f))))
        if f < 200:
            cases.append (("2nd harmonic +6 dB", *stiff_string (f, 1e-4, 1.0, second_boost_db=6.0, seed=int (f) + 1)))
    for name, x, true in cases:
        x = x + 0.003 * rng.standard_normal (len (x))  # a quiet noise floor (-50 dB re the tone)
        buffer = x[: int ((0.15 + max (0.25, 12.0 / true)) * FS)]  # settling time plus the fine window
        frame = decimate (buffer)[-window:]
        coarse = { "YIN": yin (frame, FSD, fmin, fmax), "MPM": mpm (frame, FSD, fmin, fmax) }
        coarse["MPM + refine"] = refine (buffer, coarse["MPM"]) if coarse["MPM"] > 0 else 0.0
        for algo, est in coarse.items():
            c = cents (est, true)
            if abs (c) > 600:
                octave_errors[algo] += 1
                continue
            errors[algo].append (abs (c))
            if abs (c) > worst[algo][0]:
                worst[algo] = (abs (c), f"{name} at {true:.1f} Hz")
    for algo, r in errors.items():
        r = np.array (r)
        print (f"{algo:13s}: {len (cases)} tones, octave errors {octave_errors[algo]}, median {np.median (r):.3f} cents, "
               f"95th percentile {np.percentile (r, 95):.3f}, worst {worst[algo][0]:.3f} cents ({worst[algo][1]})")


if __name__ == "__main__":
    evaluate()

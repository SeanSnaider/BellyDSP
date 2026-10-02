"""
Pitch detection study and golden reference for the tuner and the harmonizer (BUILD_PLAN "Tuner" and
"Harmonizer"): YIN against the McLeod Pitch Method on synthetic guitar-like tones, through the same analysis
path the C++ uses (src/dsp/PitchDetector.cpp: an SVF low-pass, decimation 4x to 12 kHz, then MPM), plus the
tuner's fine stage (src/dsp/TunerAnalysis.cpp). The C++ is checked against this file by tests/TunerTests.cpp
on the fixture written with --golden.

  YIN (de Cheveigne and Kawahara, JASA 2002):
    d(tau)  = sum_j (x_j - x_{j+tau})^2                       difference function
    d'(tau) = d(tau) / ((1/tau) sum_{k=1..tau} d(k))           cumulative mean normalized (d'(0) = 1)
    the first tau where d' dips below a threshold (0.1 to 0.15), then the minimum of that dip,
    refined by parabolic interpolation of d' around it.
  McLeod Pitch Method (McLeod and Wyvill, "A Smarter Way to Find Pitch", ICMC 2005):
    n(tau) = 2 r(tau) / m(tau),  r = autocorrelation,  m = sum of the squares of both overlapping parts
    (the normalized square difference function, in [-1, 1]); take the positive-lobe maxima ("key
    maxima"), choose the first one above k = 0.93 x the highest, refine by parabolic interpolation.

The window. Each lag tau compares the newest W(tau) samples with the W(tau) samples tau earlier, with
    W(tau) = max(tau, W_min).
So a candidate period is judged on the newest max(2 tau, W_min + tau) samples. With W_min at least the
longest lag (the tuner: 35 ms, every lag gets the same window) this is the classic fixed-window form,
anchored at the newest sample instead of the oldest (same accuracy, shorter delay). With a small W_min
(the harmonizer: 3 ms) a short period only needs two of its own periods of signal, so a high note is found
long before a full window for the lowest note has passed. That's what the BUILD_PLAN's harmony start times
(high E about 6 to 8 ms ... low E about 25 to 30 ms) need; the fixed window takes about 12 ms even on the
high E (latency study below). The price is a little more variance at short lags: on 2 s of noise the
largest clarity rises from 0.46 to 0.72 (white) and from 0.78 to 0.88 (brown, a high-passed random walk),
still under the 0.9 a reading needs, but close enough that the harmonizer shouldn't act on one reading alone.
W_min 3 ms is the knee: 2 ms is no faster on the open strings (two periods of the high E are 6 ms) and lets
white noise reach 0.82; 4 ms costs the high E a millisecond (7.3 / 7.6 ms) for 0.68.

Decimation. The C++ low-passes with an 8th-order Butterworth at 3 kHz (four SVF sections, Q_k = 1 / (2
sin((2k - 1) pi / 16))) and keeps every 4th sample. Phase doesn't matter for pitch, and an IIR costs
almost nothing per sample. A 3 kHz cutoff also leaves out the stretched upper partials of a stiff string,
which pull the waveform's periodicity sharp: the coarse median error is 1.5 cents here against 3.3 with a
long linear-phase FIR (scipy's resample_poly, the first version of this study). Alias rejection: 48 dB at
6 kHz, 76 dB at 9 kHz.

Test tones: sines, band-limited sawtooths, inharmonic stiff strings (partial n at n f0 sqrt(1 + B n^2),
B from 5e-5 to 3e-4, decaying faster with n), and a low "wound string" with a second harmonic 6 dB above
the fundamental (the classic octave-error trap), each with a little noise. Frequencies 30 Hz to 1.3 kHz.
Accuracy target (BUILD_PLAN): within 0.5 cent; no octave errors.

The "true" pitch of a stiff string is its first partial f0 sqrt(1 + B); that's what a tuner should show.

Result (this study): both detectors are octave-safe on these tones, but neither reaches 0.5 cent alone.
Pure sines are fine (YIN and MPM within ~0.1 cent below 500 Hz); at 12 kHz a high note's period is only
~9 samples, so parabolic interpolation costs YIN 10 cents at 1.3 kHz (MPM 1 cent); and on stiff strings
the stretched upper partials make the waveform's periodicity up to 10 cents sharp of the first partial.
So the design is two stages:
  1. coarse, octave-safe: MPM on the 12 kHz signal (better than YIN at high notes, equal elsewhere, and
     it's the detector the harmonizer will share);
  2. fine, for the tuner's readout: band-pass the 48 kHz signal around the coarse estimate (two SVF
     band-pass stages, Q 3, which leave mostly the first partial), then time its upward zero crossings
     (linear interpolation between samples) across a window of max(0.25 s, 12 periods), after letting the
     filters settle for max(0.15 s, 10 time constants Q / (pi f)).
With that, every tone here (30 Hz to 1.3 kHz, sines, saws, stiff strings, a dominant 2nd harmonic) reads
within 0.5 cent of its first partial (median 0.006, worst 0.067 cent; the first version, with a fixed
0.15 s settling time, reached 0.47 cent on a 30 Hz saw).

Harmony start times with the per-lag window (W_min 3 ms, 110 Hz floor; 80 Hz for the low E), mean / worst:
high E 6.2 / 7.4 ms, G 10.2 / 11.4, A 17.0 / 18.3, low E 22.8 / 24.8, all inside the BUILD_PLAN budgets.
A fixed window needs 12.3 / 13.0 ms on the high E and 14.1 / 15.4 on the G.

Usage:
  uv run --with numpy --with scipy python prototypes/pitch_detection.py                 # the study
  uv run --with numpy --with scipy python prototypes/pitch_detection.py --golden tests/fixtures/tuner
"""

import argparse
import math
import os

import numpy as np
from scipy.io import wavfile

FS = 48000
DECIMATION = 4
FSD = FS // DECIMATION

DECIMATION_CUTOFF = 3000.0
DECIMATION_QS = [1.0 / (2.0 * math.sin ((2 * k - 1) * math.pi / 16.0)) for k in (4, 3, 2, 1)]  # 0.51, 0.60, 0.90, 2.56

# The C++ presets (PitchDetector::Settings::tuner() and ::harmonizer()); min_window in 12 kHz samples.
TUNER = dict (fmin=28.0, fmax=1400.0, min_window=420, k=0.93)      # W_min 35 ms: two periods of 30 Hz in all
HARMONIZER = dict (fmin=110.0, fmax=1400.0, min_window=36, k=0.93)  # W_min 3 ms
CONFIDENT = 0.9  # clarity a reading needs to count (the tuner's threshold; used for the latency study)

FINE_Q = 3.0


def stiff_string (f0, B, seconds, partials=30, fs=FS, second_boost_db=0.0, seed=0):
    rng = np.random.default_rng (seed)
    t = np.arange (int (seconds * fs)) / fs
    x = np.zeros_like (t)
    for n in range (1, partials + 1):
        fn = n * f0 * math.sqrt (1 + B * n * n)
        if fn > 0.45 * fs:
            break
        amp = 1.0 / n * (10 ** (second_boost_db / 20) if n == 2 else 1.0)
        decay = 1.0 / (0.6 + 0.15 * n)  # higher partials die faster
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


def pluck (f0, seconds, B=1e-4, fs=FS, seed=0):
    """A stiff string with a 0.5 ms attack and a 2 ms burst of pick noise, for the latency study."""
    x, true = stiff_string (f0, B, seconds, fs=fs, seed=seed)
    t = np.arange (len (x)) / fs
    x = x * (1 - np.exp (-t / 0.0005))
    burst = int (0.002 * fs)
    x[:burst] += 0.05 * np.random.default_rng (seed + 1000).standard_normal (burst) * np.linspace (1, 0, burst)
    return x, true


def svf (x, fc, q, kind, fs=FS):
    """Simper's trapezoidal SVF (src/dsp/Svf.h), double precision, from rest. kind: 'lowpass' returns the
    low output v2, 'bandpass' returns k v1 (unit gain at fc)."""
    g = math.tan (math.pi * fc / fs)
    k = 1 / q
    a1 = 1 / (1 + g * (g + k)); a2 = g * a1; a3 = g * a2
    ic1 = ic2 = 0.0
    y = np.zeros (len (x))
    band = kind == "bandpass"
    for i, v0 in enumerate (x):
        v3 = v0 - ic2
        v1 = a1 * ic1 + a2 * v3
        v2 = ic2 + a2 * ic1 + a3 * v3
        ic1 = 2 * v1 - ic1
        ic2 = 2 * v2 - ic2
        y[i] = k * v1 if band else v2
    return y


def decimate (x):
    """The C++ decimator: the 3 kHz Butterworth low-pass, then every 4th sample (the 4th, 8th, ...), stored
    as float32 like the C++ history."""
    y = np.asarray (x, dtype=np.float64)
    for q in DECIMATION_QS:
        y = svf (y, DECIMATION_CUTOFF, q, "lowpass")
    return y[DECIMATION - 1::DECIMATION].astype (np.float32).astype (np.float64)


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


def nsdf (x, tau, w):
    """McLeod and Wyvill's normalized square difference function at lag tau over w samples, anchored at the
    newest sample: x[L-w : L] against x[L-w-tau : L-tau]. 1 means perfectly periodic at tau."""
    L = len (x)
    a = x[L - w:]
    b = x[L - w - tau:L - tau]
    m = np.dot (a, a) + np.dot (b, b)
    return 2.0 * np.dot (a, b) / m if m > 0 else 0.0


def frame_length (fmin, min_window, fs=FSD):
    """Samples the analysis looks at: the longest lag plus its window, plus one for the interpolation."""
    tau_max = int (fs / fmin) + 2
    return max (tau_max + 1, min_window) + tau_max + 1


def mpm (x, fs, fmin, fmax, min_window, k=0.93):
    """McLeod Pitch Method on the newest samples of x (newest last; zeros in front when x is shorter than the
    frame, like the C++ history after a reset). Returns (frequency, clarity), or (0, 0) for no pitch."""
    tau_max = int (fs / fmin) + 2
    L = frame_length (fmin, min_window, fs)
    x = np.asarray (x, dtype=np.float64)
    if len (x) < L:
        x = np.concatenate ([np.zeros (L - len (x)), x])
    x = x[-L:]
    window = lambda tau: max (tau, min_window)
    n = np.array ([nsdf (x, tau, window (tau)) for tau in range (tau_max + 1)])
    # Key maxima: the highest point of each positive lobe after the first negative-going zero crossing.
    maxima = []
    tau = 1
    while tau <= tau_max and n[tau] > 0:
        tau += 1
    while tau <= tau_max:
        while tau <= tau_max and n[tau] <= 0:
            tau += 1
        start = tau
        while tau <= tau_max and n[tau] > 0:
            tau += 1
        if start < tau:
            maxima.append (start + int (np.argmax (n[start:tau])))
    maxima = [m for m in maxima if fs / m <= fmax * 1.05]
    if not maxima:
        return 0.0, 0.0
    highest = max (n[m] for m in maxima)
    chosen = next (m for m in maxima if n[m] >= k * highest)
    # Parabolic interpolation through the chosen lag and its neighbours, all three on the chosen lag's window.
    w = window (chosen)
    y0, y1, y2 = nsdf (x, chosen - 1, w), nsdf (x, chosen, w), nsdf (x, chosen + 1, w)
    den = y0 - 2 * y1 + y2
    delta = min (1.0, max (-1.0, 0.5 * (y0 - y2) / den)) if den < 0 else 0.0
    clarity = min (1.0, y1 - 0.25 * (y0 - y2) * delta)  # the parabola's peak height
    return fs / (chosen + delta), clarity


def crossing_frequency (y, settle, fs=FS, min_periods=4):
    """Upward zero crossings of y after the first `settle` samples, each placed by linear interpolation
    between the samples on either side; frequency = (crossings - 1) / span."""
    crossings = [i - 1 + y[i - 1] / (y[i - 1] - y[i]) for i in range (max (1, settle + 1), len (y)) if y[i - 1] < 0 <= y[i]]
    if len (crossings) < min_periods + 1:
        return 0.0
    return fs * (len (crossings) - 1) / (crossings[-1] - crossings[0])


def refine_segment (x, settle, coarse, fs=FS):
    """The fine stage on one stretch of the 48 kHz signal: two SVF band-passes (Q 3) around the coarse
    estimate, run from rest over all of x, and the zero crossings timed after the first `settle` samples."""
    y = np.asarray (x, dtype=np.float64)
    for _ in range (2):
        y = svf (y, coarse, FINE_Q, "bandpass", fs)
    return crossing_frequency (y, settle, fs)


def fine_spans (coarse, available, fs=FS):
    """(settle, window) in samples. Full size: settle max(0.15 s, 10 time constants), window max(0.25 s,
    12 periods), where one band-pass stage's envelope time constant is Q / (pi f). A young note with less
    signal than that splits what there is in the same proportion, but settles for at least 6 time constants
    (the start-up transient is then down to about 2%; with 3 it was 20%, a full cent on the first reading)."""
    tau = FINE_Q / (math.pi * coarse)
    settle = int (max (0.15, 10.0 * tau) * fs)
    window = int (max (0.25, 12.0 / coarse) * fs)
    if available >= settle + window:
        return settle, window
    young_settle = min (settle, available, max (int (6.0 * tau * fs), int (available * settle / (settle + window))))
    return young_settle, available - young_settle


def refine (x48, coarse):
    """The tuner's fine estimate from the newest samples of x48 (all of it the current note)."""
    settle, window = fine_spans (coarse, len (x48))
    return refine_segment (x48[len (x48) - settle - window:], settle, coarse)


def cents (measured, true):
    return 1200 * math.log2 (measured / true) if measured > 0 else float ("inf")


def evaluate():
    freqs = [30.0, 41.2, 55.0, 82.4, 110.0, 146.8, 196.0, 246.9, 329.6, 440.0, 659.3, 880.0, 1318.5]
    yin_window = int (2.1 * FSD / 30.0)  # YIN's classic frame: about two periods of 30 Hz at 12 kHz
    yin_window += yin_window % 2
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
        settle, window = fine_spans (true, 10 ** 9)
        buffer = x[: settle + window]  # look once the fine stage has its full settle and window
        xd = decimate (buffer)
        coarse = { "YIN": yin (xd[-yin_window:], FSD, 28.0, 1400.0), "MPM": mpm (xd, FSD, **TUNER)[0] }
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


def latency_study (seeds=range (6), hop=64):
    """Harmonizer use: MPM every 64 samples at a 110 Hz floor (80 Hz for the low E). Time from a pluck to the
    first reading with clarity >= 0.9 within 50 cents of the new note, after silence or another note (cut off
    over 1 ms, like a re-fretted string). Compares the per-lag window (W_min 3 ms) with a fixed one."""
    strings = [("high E", 329.63, 6, 8), ("G", 196.0, 10, 12), ("A", 110.0, 18, 20), ("low E", 82.41, 25, 30)]
    print ("Harmony start times (BUILD_PLAN budget in brackets), mean / worst over pluck timings and previous notes:")
    for label, f, lo, hi in strings:
        fmin = 110.0 if f >= 100 else 80.0
        row = []
        for name, min_window in (("W_min 3 ms", HARMONIZER["min_window"]), ("fixed window", int (1.05 * FSD / fmin))):
            times = []
            for s in seeds:
                for prev in (None, f * 2 ** (-5 / 12), f * 2 ** (7 / 12)):
                    rng = np.random.default_rng (100 + s)
                    lead = int (0.3 * FS) + int (rng.integers (0, hop))
                    if prev is None:
                        before = np.zeros (lead)
                    else:
                        before = pluck (prev, (lead + 10) / FS, seed=s + 50)[0][:lead]
                        before[-48:] *= np.linspace (1, 0, 48)
                    note, true = pluck (f, 0.1, seed=s)
                    x = np.concatenate ([before, note]) + 0.0005 * rng.standard_normal (lead + len (note))
                    xd = decimate (x)
                    found = float ("nan")
                    for end in range ((lead // hop + 1) * hop, lead + int (0.06 * FS), hop):
                        est, clarity = mpm (xd[: end // DECIMATION], FSD, fmin, 1400.0, min_window)
                        if est > 0 and clarity >= CONFIDENT and abs (cents (est, true)) < 50:
                            found = (end - lead) / FS * 1000
                            break
                    times.append (found)
            row.append (f"{name} {np.nanmean (times):.1f} / {np.nanmax (times):.1f} ms")
        print (f"  {label:6s} [{lo}-{hi} ms]: " + ", ".join (row))


def noise_study (seconds=2.0, hop=512):
    """The largest clarity MPM reports on noise (no pitch at all), at the harmonizer's 110 Hz floor, for the
    per-lag window and a fixed one: what a confidence threshold has to stay above."""
    rng = np.random.default_rng (9)
    n = int (seconds * FS)
    white = 0.01 * rng.standard_normal (n)
    brown = np.cumsum (rng.standard_normal (n))
    brown -= np.convolve (brown, np.ones (480) / 480, mode="same")  # a random walk, high-passed around 100 Hz
    brown *= 0.01 / np.std (brown)
    row = []
    for name, x in (("white", white), ("brown", brown)):
        xd = decimate (x)
        for label, min_window in (("W_min 3 ms", HARMONIZER["min_window"]), ("fixed", int (1.05 * FSD / 110.0))):
            peak = max (mpm (xd[: end // DECIMATION], FSD, 110.0, 1400.0, min_window)[1] for end in range (int (0.1 * FS), n, hop))
            row.append (f"{name} {label} {peak:.2f}")
    print ("Largest clarity on 2 s of noise: " + ", ".join (row))


def golden_signal():
    """About 3 s of material for the C++ golden test: the noise floor alone, every kind of tone the study
    uses, a legato note change, and both ends of the range."""
    rng = np.random.default_rng (11)
    parts = [np.zeros (int (0.15 * FS)),
             stiff_string (82.41, 1e-4, 0.4, seed=1)[0],
             sawtooth (196.0, 0.35)[0],
             stiff_string (329.63, 3e-4, 0.3, seed=2)[0],
             stiff_string (55.0, 1e-4, 0.4, second_boost_db=6.0, seed=3)[0],
             stiff_string (110.0, 1e-4, 0.25, seed=4)[0],
             stiff_string (146.83, 1e-4, 0.25, seed=5)[0],  # legato: no gap, a new pitch and phase
             stiff_string (1318.5, 3e-4, 0.25, seed=6)[0],
             sawtooth (30.0, 0.5)[0],
             np.zeros (int (0.1 * FS))]
    x = np.concatenate (parts)
    return (x + 0.001 * rng.standard_normal (len (x))).astype (np.float32), np.cumsum ([0] + [len (p) for p in parts])


def write_golden (folder):
    os.makedirs (folder, exist_ok=True)
    x, bounds = golden_signal()
    wavfile.write (os.path.join (folder, "input.wav"), FS, x)
    x = x.astype (np.float64)
    xd = decimate (x)
    hop = 480
    rows = ["kind,config,end,note_start,settle,coarse,frequency,clarity"]
    for config, settings in (("tuner", TUNER), ("harmonizer", HARMONIZER)):
        for end in range (hop, len (x) + 1, hop):
            f, c = mpm (xd[: end // DECIMATION], FSD, **settings)
            rows.append (f"coarse,{config},{end},0,0,0,{float (f)!r},{float (c)!r}")
    # The fine stage near the end of each tone, as the analysis would run it with the note starting at the
    # tone's first sample and the tuner's coarse estimate at that moment.
    for i in range (1, len (bounds) - 2):
        note_start, note_end = int (bounds[i]), int (bounds[i + 1])
        for end in (note_end - 4800, note_end):
            coarse, _ = mpm (xd[: end // DECIMATION], FSD, **TUNER)
            settle, window = fine_spans (coarse, end - note_start)
            f = refine_segment (x[end - settle - window:end], settle, coarse)
            rows.append (f"fine,tuner,{end},{note_start},{settle},{float (coarse)!r},{float (f)!r},0")
    with open (os.path.join (folder, "expected.csv"), "w") as out:
        out.write ("\n".join (rows) + "\n")
    print (f"wrote {folder}/input.wav ({len (x)} samples) and expected.csv ({len (rows) - 1} rows)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument ("--golden", help="write the C++ golden fixture into this folder")
    args = parser.parse_args()
    if args.golden:
        write_golden (args.golden)
    else:
        evaluate()
        latency_study()
        noise_study()

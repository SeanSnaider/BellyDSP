# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Tone match: prototype and synthetic study (docs/TONE_MATCH.md).

The task: given a target (a song section, or a guitar stem pulled out of one) and a DI of the player,
find BellyDSP settings that make the player's DI sound like the target. A song only holds the output of
the rig, mixed with everything else, so the real amp can't be recovered. What can be matched is the
tonal fingerprint, with the controls BellyDSP already has:

    amp slot (Glass, Ember, Monolith) -> Gain (the slot's input trim) -> five tone bands -> built-in cab
    -> the post EQ (fitted last, to the residual: the "match EQ")

The search splits along the chain's one nonlinearity. Everything after the amp model is linear and
time-invariant (the tone bands, the cab, the EQ), so for a fixed slot and Gain:

  * the long-term spectrum of the output is the amp output's long-term spectrum times |H(f)|^2, so
    every tone/cab/EQ candidate is scored without rendering anything (|H| comes from the SVF formulas
    in src/dsp/Svf.h and the cab IR's FFT);
  * the cab's IR is applied by FFT convolution (exactly what the Cab block does, up to its level
    normalization, which every comparison removes), so only the amp has to be rendered;
  * four features measured on every candidate describe distortion and compression rather than EQ: how
    much the level spreads and moves from frame to frame, how much the brightness moves, and the crest
    factor (Analysis, below; the "features" study picked them).

Only (slot, Gain) needs real renders, through the app's own chain (ampsim_render): a 6 dB grid, then
3 and 1.5 dB steps around the best two slots. The cabs are screened on predicted spectra, the best four per
(slot, Gain) are convolved and scored, and the tone knobs are fitted on spectra (least squares on the
linearized bands, then Nelder-Mead on the exact ones). Last, the post EQ's five parametric bands are fitted to
what's left: the match EQ.

Two modes:
  * "anything": the player played anything. Compare long-term statistics only.
  * "same part": the player played the same part as the target. Align the two in time with dynamic
    time warping (DTW) on chroma, then compare frame by frame: the per-band mean of the aligned dB
    differences is what the linear part has to fit, and the aligned level envelopes (which no filter
    changes) add to the distortion features.

Usage (Python through uv, see docs/TONE_MATCH.md):
  uv run --with numpy --with scipy python prototypes/tone_match.py match TARGET.wav DI.wav [--mode same|anything]
  uv run --with numpy --with scipy python prototypes/tone_match.py study [--quick]
  uv run --with numpy --with scipy python prototypes/tone_match.py features
  uv run --with numpy --with scipy python prototypes/tone_match.py golden tests/fixtures/tone_match
  uv run --python 3.11 --with demucs --with numpy --with scipy python prototypes/tone_match.py study --separation

The renders need the Release build of ampsim_render (cmake --build build -j). They're cached by content
in $TMPDIR/bellydsp_tone_match_cache (or $TONE_MATCH_CACHE).
"""

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
import time

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sps

REPO = pathlib.Path(__file__).resolve().parents[1]
RENDER = REPO / "build/ampsim_render_artefacts/Release/ampsim_render"
SR = 48000
AMPS = ["Glass", "Ember", "Monolith"]
MODEL_FILES = [REPO / "content/models" / (a + ".nam") for a in AMPS]
CAB_FILES = sorted((REPO / "content/irs").glob("*/*.wav"))
CACHE = pathlib.Path(os.environ.get("TONE_MATCH_CACHE", tempfile.gettempdir())) / "bellydsp_tone_match_cache"

# ---- Analysis constants (the C++ port, src/tonematch/, uses the same) ------------------------------
N_FFT = 8192                 # 5.9 Hz bins: the 80 Hz sixth-octave band still gets a bin
HOP = 2048                   # 43 ms
ACTIVE_RANGE_DB = 30.0       # frames within 30 dB of the loud end (95th percentile) count as playing
COARSE_LO, COARSE_HI = 80.0, 12000.0
COARSE_STEP = 1.0 / 6.0      # octaves: 1/6-octave bands, about the ear's resolution for timbre
BRIGHT_LOW = (150.0, 800.0)
BRIGHT_HIGH = (2000.0, 8000.0)
TONE_RANGE_DB = 12.0         # AmpTone::rangeDb
EQ_CAP_DB = 12.0             # the match EQ is capped at +-12 dB
GAIN_GRID = [-18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0]
GAIN_RANGE = (-24.0, 24.0)   # amp*_input_trim's range


# =====================================================================================================
# Filters: the chain's linear blocks, evaluated exactly (src/dsp/Svf.h, AmpTone.h, Equalizer.h)
# =====================================================================================================

def svf_response(kind, fc, q, gain_db, f):
    """Complex response of Svf::design(kind, fc, q, gain_db) at frequencies f (Hz).

    The TPT SVF is the bilinear transform of the analog SVF with the cutoff prewarped, so its digital
    response at f equals the analog prototype's at Omega = tan(pi f / fs) / g (Svf::responseAt):
        H = m0 + m1 s / (s^2 + k s + 1) + m2 / (s^2 + k s + 1),   s = j Omega
    with the mix (m0, m1, m2), k, and g set per type exactly as in Svf::design.
    """
    f = np.asarray(f, dtype=float)
    fc = min(max(fc, 1.0), 0.49 * SR)
    a = 10.0 ** (gain_db / 40.0)
    g = math.tan(math.pi * fc / SR)
    k = 1.0 / q
    if kind == "peak":
        k = 1.0 / (q * a)
        m0, m1, m2 = 1.0, k * (a * a - 1.0), 0.0
    elif kind == "lowShelf":
        g /= math.sqrt(a)
        m0, m1, m2 = 1.0, k * (a - 1.0), a * a - 1.0
    elif kind == "highShelf":
        g *= math.sqrt(a)
        m0, m1, m2 = a * a, k * (1.0 - a) * a, 1.0 - a * a
    elif kind == "notch":
        m0, m1, m2 = 1.0, -k, 0.0
    else:
        raise ValueError(kind)
    s = 1j * np.tan(np.pi * f / SR) / g
    den = s * s + k * s + 1.0
    return m0 + m1 * s / den + m2 / den


# AmpTone::bands: Depth bell 90 Hz Q 1.2, Bass low shelf 180 Hz, Mid bell 800 Hz Q 0.7, Treble high
# shelf 2.8 kHz, Presence bell 5 kHz Q 0.8.
TONE_BANDS = [("peak", 90.0, 1.2), ("lowShelf", 180.0, 0.7071), ("peak", 800.0, 0.7),
              ("highShelf", 2800.0, 0.7071), ("peak", 5000.0, 0.8)]
TONE_NAMES = ["Depth", "Bass", "Mid", "Treble", "Presence"]


def tone_db(gains, f):
    total = np.zeros(len(f))
    for (kind, fc, q), g in zip(TONE_BANDS, gains):
        if g != 0.0:
            total += 20.0 * np.log10(np.abs(svf_response(kind, fc, q, g, f)))
    return total


# The match EQ is the post EQ in parametric mode: a low shelf, three peaks, a high shelf.
EQ_KINDS = ["lowShelf", "peak", "peak", "peak", "highShelf"]
EQ_CODES = {"lowShelf": "ls", "peak": "pk", "highShelf": "hs", "notch": "notch"}
EQ_FREQ_RANGES = [(40.0, 400.0), (100.0, 1000.0), (300.0, 3000.0), (1000.0, 8000.0), (2000.0, 12000.0)]
EQ_Q_RANGE = (0.3, 3.0)
SHELF_Q = 0.7071


def eq_db(bands, f):
    """bands: list of (kind, freq, gain, q)."""
    total = np.zeros(len(f))
    for kind, fc, g, q in bands:
        if g != 0.0:
            total += 20.0 * np.log10(np.abs(svf_response(kind, fc, q, g, f)))
    return total


# =====================================================================================================
# Analysis: STFT, bands, features
# =====================================================================================================

def hann(n):
    """Periodic Hann window (the STFT convention)."""
    return 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)


WINDOW = hann(N_FFT)
FREQS = np.arange(N_FFT // 2 + 1) * SR / N_FFT


def stft_power(x):
    """|X_t(k)|^2 with a periodic Hann window, N_FFT long, HOP apart (frames_of). Shape (T, N_FFT/2 + 1)."""
    spec = np.fft.rfft(frames_of(x) * WINDOW, axis=1)
    return spec.real ** 2 + spec.imag ** 2


def band_matrix(centres, half_width_octaves):
    """(K, B): column b averages the bins whose frequency lies in [c 2^-h, c 2^h). A band too narrow
    to hold a bin takes the bin nearest its centre."""
    m = np.zeros((len(FREQS), len(centres)))
    for b, c in enumerate(centres):
        lo, hi = c * 2.0 ** -half_width_octaves, c * 2.0 ** half_width_octaves
        idx = np.nonzero((FREQS >= lo) & (FREQS < hi))[0]
        if len(idx) == 0:
            idx = [int(round(c * N_FFT / SR))]
        m[idx, b] = 1.0 / len(idx)
    return m


COARSE_CENTRES = COARSE_LO * 2.0 ** (np.arange(int(math.floor(math.log2(COARSE_HI / COARSE_LO) / COARSE_STEP)) + 1) * COARSE_STEP)
COARSE = band_matrix(COARSE_CENTRES, COARSE_STEP / 2.0)


def perceptual_weights(f):
    """Weight of each band's dB error: the A-weighting curve (IEC 61672) in dB, mapped to a factor and
    floored at 0.25, so a mismatch at 2 kHz counts about twice as much as one at 100 Hz, and nothing is
    ignored. A rough stand-in for loudness-weighted error; the ear is far more sensitive to timbre in the
    2-5 kHz region than at the bottom of the range."""
    f2 = np.asarray(f) ** 2
    ra = (12194.0 ** 2 * f2 ** 2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2))
    a_db = 20.0 * np.log10(ra) + 2.0
    return np.maximum(0.25, 10.0 ** (a_db / 20.0))


WEIGHTS = perceptual_weights(COARSE_CENTRES)
BRIGHT_LOW_BANDS = np.nonzero((COARSE_CENTRES >= BRIGHT_LOW[0]) & (COARSE_CENTRES < BRIGHT_LOW[1]))[0]
BRIGHT_HIGH_BANDS = np.nonzero((COARSE_CENTRES >= BRIGHT_HIGH[0]) & (COARSE_CENTRES < BRIGHT_HIGH[1]))[0]
TINY = 1e-20


def db(p):
    return 10.0 * np.log10(np.maximum(p, TINY))


CONFIDENT_DB, IGNORED_DB, MIN_CONFIDENCE = 20.0, 35.0, 0.05


def band_weights(ltas):
    """The weight of each band's error when matching a target with this long-term spectrum: the
    perceptual weight times a confidence. A band within 20 dB of the target's loudest band counts fully,
    one 35 dB or more below it counts 5%, linear in between. A lead line has next to nothing below its
    lowest note; a difference there says which notes were played, not what the rig does (the study
    found 16 dB "errors" at 300-450 Hz between two leads through identical settings). It also keeps the
    fit from chasing the cab's roll-off 50 dB down, which is masked anyway."""
    below = np.max(ltas) - ltas
    conf = np.clip((IGNORED_DB - below) / (IGNORED_DB - CONFIDENT_DB), MIN_CONFIDENCE, 1.0)
    return WEIGHTS * conf


def weighted_mean(v, w=WEIGHTS):
    return float(np.sum(v * w) / np.sum(w))


def weighted_rms_centred(r, w=WEIGHTS):
    """sqrt(sum w (r - mean_w r)^2 / sum w): the error left once the overall level is matched."""
    m = weighted_mean(r, w)
    return float(math.sqrt(np.sum(w * (r - m) ** 2) / np.sum(w)))


def smooth_bands(r):
    """[1/4, 1/2, 1/4] across neighbouring sixth-octave bands (the ends keep their value): roughly
    third-octave smoothing. Every spectral comparison goes through this, so a fit follows the tone's
    shape rather than the particular notes two performances happened to play."""
    r = np.asarray(r, dtype=float)
    out = r.copy()
    out[1:-1] = 0.25 * r[:-2] + 0.5 * r[1:-1] + 0.25 * r[2:]
    return out


def active_frames(band_db):
    """Frames that hold playing. Each band's dB is taken relative to that band's median over the whole
    signal (so a linear filter, which adds the same dB to a band in every frame, changes nothing), the
    frame's level is the power mean of those, and a frame counts if its level is within ACTIVE_RANGE_DB
    of the 95th percentile (not the maximum, so one loud click doesn't decide)."""
    rel = band_db - np.median(band_db, axis=0)
    level = 10.0 * np.log10(np.mean(10.0 ** (rel / 10.0), axis=1))
    return level > percentile(level, 95) - ACTIVE_RANGE_DB


def percentile(v, p):
    """Linear-interpolation percentile (numpy's default), written out so the C++ port matches."""
    s = np.sort(np.asarray(v, dtype=float))
    pos = (len(s) - 1) * p / 100.0
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(s) - 1)
    return float(s[lo] + (s[hi] - s[lo]) * (pos - lo))


def frames_of(x):
    """The STFT's framing of x: N_FFT samples every HOP, the end zero-padded. Shape (T, N_FFT)."""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < N_FFT:
        x = np.concatenate([x, np.zeros(N_FFT - len(x))])
    num = 1 + int(math.ceil((len(x) - N_FFT) / HOP))
    padded = np.concatenate([x, np.zeros((num - 1) * HOP + N_FFT - len(x))])
    return np.lib.stride_tricks.as_strided(padded, shape=(num, N_FFT), strides=(padded.strides[0] * HOP, padded.strides[0]))


class Analysis:
    """Everything the matcher needs from one signal.

    ltas_bins   long-term power spectrum per FFT bin (mean over playing frames)
    ltas        the same in 1/6-octave bands, dB
    frame_db    every frame's 1/6-octave band levels, dB (same-part mode compares these)
    Four features that track distortion and compression rather than EQ. The study ("features"
    command) chose them over spectral flatness, which mostly measured how many notes were sounding:
    dyn     spread of the frame level: its 90th minus 10th percentile over playing frames, dB. The level
            is measured on frames whitened by the long-term spectrum (each band relative to its long-term
            level, then the power mean), so the EQ doesn't decide it. Distortion compresses: it shrinks.
    flux    median change of that level from one frame to the next, dB. Clean notes rise and decay;
            distorted ones hold.
    bright  standard deviation over frames of the whitened high (2-8 kHz) minus low (150-800 Hz) band
            level, dB: how much the brightness moves.
    crest   median crest factor (peak over RMS) of the playing frames, dB. Clipping lowers it.
    """

    def __init__(self, x):
        p = stft_power(x)
        self.power = p
        bands = p @ COARSE
        self.frame_db = db(bands)                             # (T, B), every frame
        self.active = active_frames(self.frame_db)
        if self.active.sum() < 4:
            raise ValueError("not enough playing in the signal to analyse")
        act = p[self.active]
        self.ltas_bins = act.mean(axis=0)
        self.ltas = db(bands[self.active].mean(axis=0))
        self.weights = band_weights(smooth_bands(self.ltas))
        white = db(bands) - self.ltas                          # whitened, every frame

        level = 10.0 * np.log10(np.mean(10.0 ** (white / 10.0), axis=1))
        self.level = level
        self.dyn = percentile(level[self.active], 90) - percentile(level[self.active], 10)
        both = self.active[1:] & self.active[:-1]
        self.flux = float(np.median(np.abs(np.diff(level))[both])) if both.any() else 0.0
        tilt = white[self.active][:, BRIGHT_HIGH_BANDS].mean(axis=1) - white[self.active][:, BRIGHT_LOW_BANDS].mean(axis=1)
        self.bright = float(np.std(tilt))
        fr = frames_of(x)[self.active]
        rms = np.sqrt(np.mean(fr ** 2, axis=1))
        self.crest = float(np.median(20.0 * np.log10(np.max(np.abs(fr), axis=1) / np.maximum(rms, 1e-12))))

    def features(self):
        return np.array([self.dyn, self.flux, self.bright, self.crest])


# Scale of each distortion feature: a difference of one scale unit costs as much as 1 dB of spectral
# error. About twice each feature's spread across different performances at the same settings, in the
# feature study ("features" command): different playing alone mostly stays within one unit.
FEATURE_SCALES = np.array([2.0, 0.4, 1.5, 0.5])
FEATURE_NAMES = ["level spread (dB)", "level flux (dB/frame)", "brightness movement (dB)", "crest factor (dB)"]


def nonlinear_distance(ft, fc):
    return float(np.sum(np.abs(ft - fc) / FEATURE_SCALES))


# =====================================================================================================
# The chain: renders through the app's real DSP (ampsim_render)
# =====================================================================================================

def read_wav(path):
    sr, x = wavfile.read(path)
    if x.dtype.kind == "i":
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max + 1)
    x = np.asarray(x, dtype=np.float64)
    return sr, x


def write_wav(path, x, sr=SR):
    wavfile.write(path, sr, np.asarray(x, dtype=np.float32))


def to_mono_48k(path):
    sr, x = read_wav(path)
    if x.ndim == 2:
        x = x.mean(axis=1)
    if sr != SR:
        g = math.gcd(int(sr), SR)
        x = sps.resample_poly(x, SR // g, int(sr) // g)
    return x


def signal_key(x):
    return hashlib.sha1(np.asarray(x, dtype=np.float32).tobytes()).hexdigest()[:16]


def eq_arg(bands):
    return ",".join(f"{EQ_CODES[k]}:{f:.2f}:{g:.2f}:{q:.3f}" for k, f, g, q in bands)


def render(x, slot, gain_db, tone=(0.0,) * 5, cab=None, eq=None, cache=True):
    """x through slot `slot` at Gain `gain_db`, with tone, cab (a path, or None for none), and the post
    EQ (list of bands, or None for off). Returns the left output channel. Renders are cached on disk by
    content, since the study reuses them."""
    args = ["--model", str(MODEL_FILES[slot]), "--trim", f"{gain_db:.3f}",
            "--tone", ",".join(f"{t:.3f}" for t in tone)]
    if cab is not None:
        args += ["--ir", str(cab)]
    if eq is not None:
        args += ["--post-eq", eq_arg(eq)]
    key = hashlib.sha1((signal_key(x) + "|" + "|".join(args)).encode()).hexdigest()[:20]
    out = CACHE / (key + ".npy")
    if cache and out.exists():
        return np.load(out)
    CACHE.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = pathlib.Path(tmp) / "in.wav", pathlib.Path(tmp) / "out.wav"
        write_wav(src, x)
        r = subprocess.run([str(RENDER)] + args + [str(src), str(dst)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stdout + r.stderr)
        _, y = read_wav(dst)
    y = y[:, 0] if y.ndim == 2 else y
    np.save(out, y)
    return y


def render_many(jobs, workers=8):
    """jobs: list of (x, slot, gain). Renders in parallel (each is its own process)."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(lambda j: render(*j), jobs))


_cab_cache = {}


def cab_bands_db(path):
    """The cab IR's power response averaged into each 1/6-octave band, dB, and per FFT bin. The cab
    normalizes every IR's loudness at load (BUILD_PLAN "Cab"), and the matcher compares levels only after
    removing the mean, so the IR's own level doesn't matter. Close mics take the left channel."""
    if path in _cab_cache:
        return _cab_cache[path]
    sr, ir = read_wav(path)
    if ir.ndim == 2:
        ir = ir[:, 0]
    if sr != SR:
        raise ValueError("built-in IRs are 48 kHz")
    ir = ir[: SR]                                   # CabIR::maxIRSeconds = 1 s
    n = max(N_FFT, 1 << int(math.ceil(math.log2(len(ir)))))
    h = np.abs(np.fft.rfft(ir, n=n)) ** 2
    f = np.arange(len(h)) * SR / n
    hb = np.interp(FREQS, f, h)                     # on the analysis bins
    result = (db(hb @ COARSE), hb)
    _cab_cache[path] = result
    return result


# =====================================================================================================
# Optimizer: Nelder-Mead, written out (the C++ port is a line-by-line copy)
# =====================================================================================================

def nelder_mead(fn, x0, step, lo, hi, iters=400, tol=1e-5):
    """Nelder and Mead (1965), standard coefficients (reflect 1, expand 2, contract 0.5, shrink 0.5).
    Bounds by clamping: fn always sees a point inside [lo, hi]. Deterministic: same inputs, same path.
    Returns (x, f)."""
    clamp = lambda v: np.minimum(np.maximum(v, lo), hi)
    n = len(x0)
    pts = [clamp(np.array(x0, dtype=float))]
    for i in range(n):
        p = pts[0].copy()
        p[i] = p[i] + step[i] if p[i] + step[i] <= hi[i] else p[i] - step[i]
        pts.append(clamp(p))
    vals = [fn(p) for p in pts]
    for _ in range(iters):
        order = sorted(range(n + 1), key=lambda i: vals[i])
        pts = [pts[i] for i in order]
        vals = [vals[i] for i in order]
        if abs(vals[-1] - vals[0]) <= tol * (abs(vals[0]) + 1e-9):
            break
        centroid = np.mean(pts[:-1], axis=0)
        xr = clamp(centroid + (centroid - pts[-1]))
        fr = fn(xr)
        if fr < vals[0]:
            xe = clamp(centroid + 2.0 * (centroid - pts[-1]))
            fe = fn(xe)
            if fe < fr:
                pts[-1], vals[-1] = xe, fe
            else:
                pts[-1], vals[-1] = xr, fr
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            if fr < vals[-1]:
                xc = clamp(centroid + 0.5 * (xr - centroid))
            else:
                xc = clamp(centroid + 0.5 * (pts[-1] - centroid))
            fc = fn(xc)
            if fc < min(fr, vals[-1]):
                pts[-1], vals[-1] = xc, fc
            else:
                for i in range(1, n + 1):
                    pts[i] = clamp(pts[0] + 0.5 * (pts[i] - pts[0]))
                    vals[i] = fn(pts[i])
    best = int(np.argmin(vals))
    return pts[best], vals[best]


# =====================================================================================================
# Synthetic audio for the study: plucked-string DIs, a backing band, a reverb. No recorded music.
# =====================================================================================================

def midi_hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


def pluck(freq, length, rng, level=0.7, decay=0.996, brightness=0.6, muted=False):
    """Karplus-Strong (Karplus and Strong 1983; Jaffe and Smith 1983): a noise burst circulating in a
    one-period delay line through a two-point average and a loss factor:
        y[n] = x[n] + d (y[n-N] + y[n-N-1]) / 2
    run here as an IIR filter (scipy's lfilter) on the burst. The burst is low-passed (pick brightness)
    and comb-filtered (pick position, 1/7 of the string). A palm mute damps harder and darker."""
    period = max(2, int(round(SR / freq)))
    burst = rng.uniform(-1.0, 1.0, period)
    a = 1.0 - brightness
    burst = sps.lfilter([1.0 - a], [1.0, -a], burst)               # one-pole low-pass
    pick = max(1, period // 7)
    burst[pick:] -= burst[:-pick]                                  # pick position comb
    if muted:
        decay, burst = 0.975, sps.lfilter([0.5], [1.0, -0.5], burst)
    excitation = np.zeros(length)
    excitation[:period] = burst
    den = np.zeros(period + 2)
    den[0], den[period], den[period + 1] = 1.0, -0.5 * decay, -0.5 * decay
    y = sps.lfilter([1.0], den, excitation)
    release = min(length, int(0.012 * SR))                        # the string is damped at the end
    y[length - release:] *= np.linspace(1.0, 0.0, release)
    return level * y / (np.max(np.abs(y[:period * 4])) + 1e-9)


# Phrases: (beat, length in beats, MIDI notes, palm muted). 120 BPM, 16ths are 0.25 beats.
def _lead_a():
    run = [64, 67, 69, 71, 74, 76, 79, 81, 83, 81, 79, 76, 74, 71, 69, 67]
    notes = [(i * 0.25, 0.25, [m], False) for i, m in enumerate(run)]
    notes += [(4.0, 1.0, [76], False), (5.0, 0.5, [74], False), (5.5, 0.5, [76], False), (6.0, 2.0, [79], False)]
    run2 = [76, 74, 71, 74, 76, 79, 76, 74, 71, 69, 67, 69, 71, 74, 71, 69]
    notes += [(8.0 + i * 0.25, 0.25, [m], False) for i, m in enumerate(run2)]
    notes += [(12.0, 1.5, [64], False), (13.5, 0.5, [67], False), (14.0, 2.0, [69], False)]
    return notes, 16.0


def _lead_b():
    line = [(0, 1, 69), (1, 0.5, 72), (1.5, 0.5, 74), (2, 1, 76), (3, 1, 77), (4, 0.5, 76), (4.5, 0.5, 74),
            (5, 1, 72), (6, 2, 71), (8, 0.5, 81), (8.5, 0.5, 79), (9, 0.5, 77), (9.5, 0.5, 76), (10, 1, 74),
            (11, 1, 72), (12, 0.75, 74), (12.75, 0.25, 76), (13, 1, 77), (14, 2, 76)]
    return [(b, l, [m], False) for b, l, m in line], 16.0


def _riff_a():
    notes = []
    for bar in range(2):
        o = bar * 4.0
        notes += [(o + i * 0.25, 0.22, [40], True) for i in range(6)]
        notes += [(o + 1.5, 1.0, [40, 47, 52], False), (o + 2.5, 0.25, [43], True), (o + 2.75, 0.25, [45], True),
                  (o + 3.0, 1.0, [45, 52, 57], False)]
    return notes, 8.0


def _riff_b():
    notes = []
    for bar in range(2):
        o = bar * 4.0
        notes += [(o + i * 0.5, 0.45, [38], True) for i in range(3)]
        notes += [(o + 1.5, 0.75, [43, 50, 55], False), (o + 2.25, 0.25, [38], True), (o + 2.5, 1.5, [45, 52, 57], False)]
    return notes, 8.0


def _lead_c():
    line = [(0, 0.5, 71), (0.5, 0.5, 74), (1, 0.5, 76), (1.5, 0.5, 79), (2, 1.5, 81), (3.5, 0.5, 79),
            (4, 0.25, 76), (4.25, 0.25, 79), (4.5, 0.25, 76), (4.75, 0.25, 74), (5, 1, 76), (6, 2, 64),
            (8, 0.5, 67), (8.5, 0.5, 69), (9, 1, 71), (10, 0.5, 74), (10.5, 0.5, 71), (11, 1, 69),
            (12, 0.33, 76), (12.33, 0.33, 74), (12.67, 0.34, 71), (13, 1, 69), (14, 2, 71)]
    return [(b, l, [m], False) for b, l, m in line], 16.0


def _lead_d():
    line = [(0, 0.25, 76), (0.25, 0.25, 77), (0.5, 0.5, 79), (1, 0.5, 81), (1.5, 0.5, 79), (2, 1, 77),
            (3, 0.5, 76), (3.5, 0.5, 74), (4, 0.25, 72), (4.25, 0.25, 74), (4.5, 0.25, 76), (4.75, 0.25, 77),
            (5, 1.5, 79), (6.5, 0.5, 76), (7, 1, 72), (8, 0.5, 69), (8.5, 0.5, 72), (9, 0.5, 74), (9.5, 0.5, 76),
            (10, 1, 77), (11, 1, 74), (12, 0.25, 81), (12.25, 0.25, 79), (12.5, 0.25, 77), (12.75, 0.25, 76),
            (13, 0.5, 74), (13.5, 0.5, 72), (14, 2, 69)]
    return [(b, l, [m], False) for b, l, m in line], 16.0


PHRASES = {"lead_a": _lead_a, "lead_d": _lead_d, "lead_b": _lead_b, "lead_c": _lead_c, "riff_a": _riff_a, "riff_b": _riff_b}


def synth_di(parts, seed, tempo=1.0, jitter_ms=0.0, peak_db=-8.0):
    """A synthetic guitar DI: the named phrases one after another at 120 BPM x tempo, each note with a
    random level (+-2 dB) and, if asked, a random timing offset. Then a pickup's resonance (a 2-pole
    low-pass at 4.5 kHz, Q 1.5) and a peak level of peak_db dBFS."""
    rng = np.random.default_rng(seed)
    beat = 0.5 / tempo
    events, offset = [], 0.0
    for name in parts:
        notes, length = PHRASES[name]()
        events += [(offset + b, l, ms, mute) for b, l, ms, mute in notes]
        offset += length
    total = int((offset * beat + 1.0) * SR)
    out = np.zeros(total)
    for b, l, ms, mute in events:
        start = int(max(0.0, b * beat + rng.normal(0.0, jitter_ms / 1000.0)) * SR)
        length = min(int(l * beat * SR), total - start)
        level = 0.7 * 10.0 ** (rng.uniform(-2.0, 2.0) / 20.0) / math.sqrt(len(ms))
        for m in ms:
            out[start:start + length] += pluck(midi_hz(m), length, rng, level=level, muted=mute)
    w0 = 2 * math.pi * 4500.0 / SR
    alpha = math.sin(w0) / (2 * 1.5)                               # RBJ low-pass, Q 1.5: the resonance
    b = np.array([(1 - math.cos(w0)) / 2, 1 - math.cos(w0), (1 - math.cos(w0)) / 2])
    a = np.array([1 + alpha, -2 * math.cos(w0), 1 - alpha])
    out = sps.lfilter(b / a[0], a / a[0], out)
    return out * 10.0 ** (peak_db / 20.0) / np.max(np.abs(out))


def synth_backing(seconds, seed, tempo=1.0):
    """Drums (kick, snare, hats) and a bass line, synthesized: a busy band to bury the guitar in."""
    rng = np.random.default_rng(seed)
    n = int(seconds * SR)
    beat = 0.5 / tempo
    out = np.zeros(n)
    t = np.arange(int(0.4 * SR)) / SR
    kick = np.sin(2 * np.pi * (50 * t + 60 * (1 - np.exp(-t * 30)) / 30)) * np.exp(-t * 9)
    snare_t = np.arange(int(0.25 * SR)) / SR
    hat_t = np.arange(int(0.05 * SR)) / SR
    hp_b, hp_a = sps.butter(2, 6000, btype="highpass", fs=SR)
    bp_b, bp_a = sps.butter(2, [200, 8000], btype="bandpass", fs=SR)
    bass_roots = [40, 40, 45, 43]
    for i in range(int(seconds / beat)):
        s = int(i * beat * SR)
        if i % 2 == 0:
            seg = kick[: n - s]
            out[s:s + len(seg)] += 0.8 * seg
        else:
            snare = sps.lfilter(bp_b, bp_a, rng.normal(0, 1, len(snare_t))) * np.exp(-snare_t * 18)
            snare += 0.4 * np.sin(2 * np.pi * 185 * snare_t) * np.exp(-snare_t * 25)
            seg = 0.5 * snare[: n - s]
            out[s:s + len(seg)] += seg
        for h in range(2):
            hs = s + int(h * beat * SR / 2)
            hat = sps.lfilter(hp_b, hp_a, rng.normal(0, 1, len(hat_t))) * np.exp(-hat_t * 80)
            seg = 0.25 * hat[: max(0, n - hs)]
            out[hs:hs + len(seg)] += seg
        # bass: a low-passed sawtooth on the bar's root, eighth notes
        root = midi_hz(bass_roots[(i // 4) % 4] - 12)
        for h in range(2):
            bs = s + int(h * beat * SR / 2)
            bt = np.arange(int(beat * SR / 2 * 0.9)) / SR
            saw = 2 * ((bt * root) % 1.0) - 1
            seg = 0.35 * saw[: max(0, n - bs)] * np.exp(-bt[: max(0, n - bs)] * 3)
            out[bs:bs + len(seg)] += seg
    lp_b, lp_a = sps.butter(2, 4000, fs=SR)
    return sps.lfilter(lp_b, lp_a, out)


def synth_reverb(x, seed, rt60=1.4, wet_db=-12.0):
    """A plain synthetic room: exponentially decaying noise (60 dB in rt60 seconds), darkened, mixed in
    at wet_db relative to the dry signal's RMS."""
    rng = np.random.default_rng(seed)
    t = np.arange(int(rt60 * SR)) / SR
    ir = rng.normal(0, 1, len(t)) * 10.0 ** (-3.0 * t / rt60)
    b, a = sps.butter(1, 5000, fs=SR)
    ir = sps.lfilter(b, a, ir)
    ir[: int(0.02 * SR)] = 0.0                                      # 20 ms pre-delay
    wet = sps.fftconvolve(x, ir)[: len(x)]
    wet *= 10.0 ** (wet_db / 20.0) * np.sqrt(np.mean(x ** 2) / (np.mean(wet ** 2) + 1e-20))
    return x + wet


# =====================================================================================================
# Same-part alignment: chroma and dynamic time warping
# =====================================================================================================

CHROMA_LO, CHROMA_HI = 60.0, 2100.0
_CHROMA_BINS = np.nonzero((FREQS >= CHROMA_LO) & (FREQS < CHROMA_HI))[0]
_CHROMA_CLASS = (np.round(12.0 * np.log2(FREQS[_CHROMA_BINS] / 440.0)).astype(int) + 69) % 12


def chroma(power):
    """Pitch-class profile per frame (Fujishima 1999; Mueller, "Fundamentals of Music Processing", 2015,
    ch. 3): each bin from 60 Hz to 2.1 kHz adds its magnitude to its nearest pitch class, the 12 sums
    are log-compressed (log(1 + 100 c), which keeps quiet notes from vanishing next to loud ones) and
    scaled to unit length. Distortion and cabs change a note's timbre far more than its pitch classes,
    which is why the alignment uses these and not spectra. A silent frame gets an all-equal vector."""
    mag = np.sqrt(power[:, _CHROMA_BINS])
    c = np.zeros((power.shape[0], 12))
    for pc in range(12):
        c[:, pc] = mag[:, _CHROMA_CLASS == pc].sum(axis=1)
    c = c / (np.max(c) + TINY)
    c = np.log1p(100.0 * c)
    norm = np.linalg.norm(c, axis=1, keepdims=True)
    c = np.where(norm > 1e-9, c / np.maximum(norm, 1e-9), 1.0 / math.sqrt(12.0))
    return c


def dtw_path(cost):
    """Classic DTW (Sakoe and Chiba 1978) with steps (1,1), (1,0), (0,1), both ends anchored:
        D[i, j] = cost[i, j] + min(D[i-1, j-1], D[i-1, j], D[i, j-1]).
    Row by row; the horizontal term is a running min-plus scan: with A[j] = cost[i, j] +
    min(D[i-1, j-1], D[i-1, j]) and C the row's cumulative cost, D[i, j] = C[j] + min_{k<=j} (A[k] - C[k]).
    The backtrack prefers the diagonal, then (i-1, j), then (i, j-1), so ties go the same way every time.
    Returns the path as (i, j) pairs from (0, 0) to the end, and the mean cost along it."""
    n, m = cost.shape
    d = np.full((n, m), np.inf)
    d[0] = np.cumsum(cost[0])
    for i in range(1, n):
        prev = d[i - 1]
        best = prev.copy()
        best[1:] = np.minimum(prev[1:], prev[:-1])
        a = cost[i] + best
        c = np.cumsum(cost[i])
        d[i] = c + np.minimum.accumulate(a - c)
    i, j = n - 1, m - 1
    path = [(i, j)]
    while i > 0 or j > 0:
        if i == 0:
            j -= 1
        elif j == 0:
            i -= 1
        else:
            diag, up, left = d[i - 1, j - 1], d[i - 1, j], d[i, j - 1]
            if diag <= up and diag <= left:
                i, j = i - 1, j - 1
            elif up <= left:
                i -= 1
            else:
                j -= 1
        path.append((i, j))
    path.reverse()
    p = np.array(path)
    return p, float(d[-1, -1] / len(p))


def align(target_power, candidate_power):
    ct, cc = chroma(target_power), chroma(candidate_power)
    cost = 1.0 - ct @ cc.T                                          # cosine distance
    return dtw_path(cost)


# =====================================================================================================
# The matcher
# =====================================================================================================

ENVELOPE_SCALE_DB = 1.0      # same-part mode: 1 dB of envelope difference counts as one feature unit
LAMBDA = 0.5                 # weight of the distortion features against the spectral error (dB per unit)
CABS_PER_AMP = 4             # cabs kept per (slot, Gain) after the quick spectral screen
REFINE_SLOTS = 2             # slots whose Gain is refined
REFINE_STEPS = [3.0, 1.5]    # coarse-to-fine Gain steps after the 6 dB grid
SCORE_SCALE_DB = 6.0         # closeness = 100 exp(-E / 6 dB)
DEFAULT_CAB = REPO / "content/irs/Vintage 4x12/Vintage 4x12, dynamic, upper, var. 2.wav"  # 3 of 5 factory presets

_TONE_BASIS = None


def tone_basis():
    """dB response of each tone band per dB of gain (each at +6 dB, divided by 6): the linearized model
    used to fit the five knobs by least squares before polishing with Nelder-Mead."""
    global _TONE_BASIS
    if _TONE_BASIS is None:
        _TONE_BASIS = np.stack([tone_db([6.0 if i == j else 0.0 for j in range(5)], COARSE_CENTRES) / 6.0
                                for i in range(5)], axis=1)
    return _TONE_BASIS


TONE_RIDGE = 0.01            # dB^2 of error per dB^2 of knob: 10 dB on one knob costs as much as 1 dB of error


def tone_error(r, theta, w):
    """sqrt(weighted mean square of the level-matched residual + TONE_RIDGE |theta|^2), dB. The ridge
    term keeps the knobs from chasing what the error can't pin down (what notes were played)."""
    e = r - tone_db(theta, COARSE_CENTRES)
    m = weighted_mean(e, w)
    return float(math.sqrt(np.sum(w * (e - m) ** 2) / np.sum(w) + TONE_RIDGE * np.sum(np.square(theta))))


def fit_tone_linear(r, w):
    """Least-squares knobs for the dB residual r (target minus candidate) with the linearized bands:
    minimize sum_b w_b (r_b - c - (B theta)_b)^2 / sum w + TONE_RIDGE |theta|^2 over theta and a free
    level offset c (ridge regression, solved as an augmented least-squares problem), then clamp to +-12."""
    b = np.concatenate([tone_basis(), np.ones((len(r), 1))], axis=1)
    sw = np.sqrt(w / np.sum(w))
    a = np.concatenate([b * sw[:, None], np.concatenate([math.sqrt(TONE_RIDGE) * np.eye(5), np.zeros((5, 1))], axis=1)])
    y = np.concatenate([r * sw, np.zeros(5)])
    theta, *_ = np.linalg.lstsq(a, y, rcond=None)
    return np.clip(theta[:5], -TONE_RANGE_DB, TONE_RANGE_DB)


def fit_tone(r, w, polish=True):
    """Knobs that minimize tone_error: the linear fit, then Nelder-Mead on the exact SVF responses.
    Returns (theta, error in dB)."""
    theta = fit_tone_linear(r, w)
    err = lambda th: tone_error(r, th, w)
    if not polish:
        return theta, err(theta)
    lo, hi = np.full(5, -TONE_RANGE_DB), np.full(5, TONE_RANGE_DB)
    theta, e = nelder_mead(err, theta, np.full(5, 2.0), lo, hi, iters=300, tol=1e-6)
    return theta, e


class Target:
    def __init__(self, x):
        self.x = x
        self.analysis = Analysis(x)


class Candidate:
    """One (slot, Gain, cab): the amp render convolved with the cab IR, analysed. In same-part mode it
    also holds the aligned dB differences."""

    def __init__(self, slot, gain, cab, y, target, mode, path):
        self.slot, self.gain, self.cab = slot, gain, cab
        _, ir = read_wav(cab)
        ir = (ir[:, 0] if ir.ndim == 2 else ir)[:SR]
        self.y = sps.fftconvolve(y, ir)[: len(y)]                     # exactly what the cab does, up to level
        self.analysis = Analysis(self.y)
        ta = target.analysis
        self.nl = nonlinear_distance(ta.features(), self.analysis.features())
        if mode == "same":
            keep = ta.active[path[:, 0]] & self.analysis.active[path[:, 1]]
            d = ta.frame_db[path[keep, 0]] - self.analysis.frame_db[path[keep, 1]]
            self.residual = smooth_bands(d.mean(axis=0))              # what the linear part must fit
            # The same notes were played, so the level envelopes can be compared moment by moment: the
            # RMS difference of the whitened frame levels along the alignment, once the mean is
            # removed, in dB. Compression, sustain, and attack all show in it, and no filter changes it.
            e = ta.level[path[keep, 0]] - self.analysis.level[path[keep, 1]]
            self.envelope = float(np.sqrt(np.mean((e - e.mean()) ** 2)))
            self.nl += self.envelope / ENVELOPE_SCALE_DB
        else:
            self.residual = smooth_bands(ta.ltas - self.analysis.ltas)
        self.weights = ta.weights
        self.tone, self.spectral = fit_tone(self.residual, self.weights, polish=False)

    def total(self):
        return self.spectral + LAMBDA * self.nl


def screen_cabs(y_analysis, target_ltas_or_residual, w, count):
    """The quick screen: each cab's effect predicted from the amp render's long-term spectrum per bin
    times the cab's |H|^2, the tone knobs fitted linearly, the cabs ranked by spectral error."""
    scores = []
    for cab in CAB_FILES:
        _, hb = cab_bands_db(cab)
        pred = db((y_analysis.ltas_bins * hb) @ COARSE)
        r = smooth_bands(target_ltas_or_residual - pred)
        _, e = fit_tone(r, w, polish=False)
        scores.append((e, cab))
    scores.sort(key=lambda s: s[0])
    return [c for _, c in scores[:count]]


EQ_RIDGE = 0.005             # the same idea as TONE_RIDGE, per dB of band gain


def fit_match_eq(residual, w):
    """The match EQ: the post EQ's five parametric bands (low shelf, three peaks, high shelf) fitted to
    the residual left after the amp, Gain, tone, and cab (already smoothed, smooth_bands), capped at
    +-12 dB.
    Nelder-Mead on log2 frequency, gain, and log2 Q (the shelves keep Q 0.7071), weighted like the
    spectral error. Returns (bands, smoothed target curve)."""
    sm = np.clip(residual - weighted_mean(residual, w), -EQ_CAP_DB, EQ_CAP_DB)

    starts = [100.0, 400.0, 1000.0, 3000.0, 8000.0]

    # Parameter vector: per band log2 f, gain, and (peaks only) log2 q -> 2 + 3 + 3 + 3 + 2 = 13.
    layout = []
    for i, kind in enumerate(EQ_KINDS):
        layout.append((i, "f"))
        layout.append((i, "g"))
        if kind == "peak":
            layout.append((i, "q"))

    def decode(p):
        vals = {}
        for (i, what), v in zip(layout, p):
            vals[(i, what)] = v
        return [(kind, 2.0 ** vals[(i, "f")], vals[(i, "g")],
                 2.0 ** vals[(i, "q")] if kind == "peak" else SHELF_Q) for i, kind in enumerate(EQ_KINDS)]

    x0, lo, hi, step = [], [], [], []
    for i, what in layout:
        if what == "f":
            x0.append(math.log2(starts[i]))
            lo.append(math.log2(EQ_FREQ_RANGES[i][0])); hi.append(math.log2(EQ_FREQ_RANGES[i][1])); step.append(0.5)
        elif what == "g":
            x0.append(float(np.interp(math.log2(starts[i]), np.log2(COARSE_CENTRES), sm)))
            lo.append(-EQ_CAP_DB); hi.append(EQ_CAP_DB); step.append(2.0)
        else:
            x0.append(0.0); lo.append(math.log2(EQ_Q_RANGE[0])); hi.append(math.log2(EQ_Q_RANGE[1])); step.append(0.5)
    x0, lo, hi, step = map(np.array, (x0, lo, hi, step))
    gains = [k for k, (i, what) in enumerate(layout) if what == "g"]

    def err(p):
        e = sm - eq_db(decode(p), COARSE_CENTRES)
        m = weighted_mean(e, w)
        return math.sqrt(np.sum(w * (e - m) ** 2) / np.sum(w) + EQ_RIDGE * float(np.sum(np.square(p[gains]))))

    p, _ = nelder_mead(err, x0, step, lo, hi, iters=3000, tol=1e-8)
    p, _ = nelder_mead(err, p, step * 0.25, lo, hi, iters=3000, tol=1e-9)   # a restart, smaller simplex
    return decode(p), sm


def match(target_x, di_x, mode="anything", log=print, workers=8):
    """Find slot, Gain, tone, cab, and match EQ for di_x to sound like target_x. Returns a dict."""
    t0 = time.time()
    target = Target(target_x)
    renders, paths, cands = {}, {}, {}

    def amp(slot, gain):
        key = (slot, round(gain, 3))
        if key not in renders:
            renders[key] = render(di_x, slot, gain)
        return renders[key]

    # 1. The coarse grid: every slot at every 6 dB Gain step, rendered in parallel.
    jobs = [(di_x, s, g) for s in range(3) for g in GAIN_GRID]
    for (x, s, g), y in zip(jobs, render_many(jobs, workers)):
        renders[(s, round(g, 3))] = y
    log(f"  rendered {len(jobs)} grid points in {time.time() - t0:.1f} s")

    if mode == "same":
        for s in range(3):
            p, c = align(target.analysis.power, stft_power(amp(s, 0.0)))
            paths[s] = p
            log(f"  aligned against {AMPS[s]}: {len(p)} steps, mean chroma distance {c:.3f}")

    def evaluate(slot, gain, cabs=None):
        y = amp(slot, gain)
        if cabs is None:
            ya = Analysis(y)
            if mode == "same":
                p = paths[slot]
                keep = target.analysis.active[p[:, 0]] & ya.active[p[:, 1]]
                ref = (target.analysis.frame_db[p[keep, 0]] - ya.frame_db[p[keep, 1]]).mean(axis=0) + ya.ltas
            else:
                ref = target.analysis.ltas
            cabs = screen_cabs(ya, ref, target.analysis.weights, CABS_PER_AMP)
        out = []
        for cab in cabs:
            key = (slot, round(gain, 3), str(cab))
            if key not in cands:
                cands[key] = Candidate(slot, gain, cab, y, target, mode, paths.get(slot))
            out.append(cands[key])
        return out

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(lambda sg: evaluate(*sg), [(s, g) for s in range(3) for g in GAIN_GRID]))
    log(f"  scored {len(cands)} (slot, Gain, cab) candidates by {time.time() - t0:.1f} s")

    # 2. Coarse to fine on Gain, for the best slots, keeping each slot's best cabs.
    def best_for(slot):
        return min((c for c in cands.values() if c.slot == slot), key=Candidate.total)

    slot_order = sorted(range(3), key=lambda s: best_for(s).total())
    for s in slot_order[:REFINE_SLOTS]:
        cabs = sorted({str(c.cab) for c in cands.values() if c.slot == s},
                      key=lambda cb: min(c.total() for c in cands.values() if c.slot == s and str(c.cab) == cb))[:CABS_PER_AMP]
        cabs = [pathlib.Path(c) for c in cabs]
        for step in REFINE_STEPS:
            g0 = best_for(s).gain
            gains = [g for g in (g0 - step, g0 + step) if GAIN_RANGE[0] <= g <= GAIN_RANGE[1]]
            for y, g in zip(render_many([(di_x, s, g) for g in gains], workers), gains):
                renders[(s, round(g, 3))] = y
            for g in gains:
                evaluate(s, g, cabs)
    log(f"  refined Gain by {time.time() - t0:.1f} s ({len(renders)} renders)")

    ranked = sorted(cands.values(), key=Candidate.total)
    best = ranked[0]
    w = target.analysis.weights
    best.tone, best.spectral = fit_tone(best.residual, w, polish=True)

    # 3. The match EQ, on what the tone knobs couldn't fit.
    residual = best.residual - tone_db(best.tone, COARSE_CENTRES)
    eq, eq_target = fit_match_eq(residual, w)
    after_eq = weighted_rms_centred(residual - eq_db(eq, COARSE_CENTRES), w)
    elapsed = time.time() - t0

    result = {
        "mode": mode,
        "slot": best.slot, "amp": AMPS[best.slot], "gain_db": best.gain,
        "tone_db": [float(v) for v in best.tone], "cab": best.cab.name,
        "eq": [(k, float(f), float(g), float(q)) for k, f, g, q in eq],
        "spectral_error_db": best.spectral, "spectral_error_after_eq_db": after_eq,
        "distortion_distance": best.nl,
        "closeness": 100.0 * math.exp(-(after_eq + LAMBDA * best.nl) / SCORE_SCALE_DB),
        "runtime_s": elapsed, "renders": len(renders), "candidates": len(cands),
        "runner_up": [(AMPS[c.slot], c.gain, c.cab.name, round(c.total(), 2)) for c in ranked[1:4]],
        "eq_target": eq_target.tolist(), "residual": residual.tolist(),
    }
    return result


def verify(result, target_x, di_x, mode):
    """Renders the result through the real chain, every setting at once, and measures it against the
    target the same way the matcher did: the long-term spectral error (anything mode) or the aligned
    frame-wise one (same part), in dB. Checks the frequency-domain shortcuts end to end."""
    cab = next(c for c in CAB_FILES if c.name == result["cab"])
    y = render(di_x, result["slot"], result["gain_db"], tone=result["tone_db"], cab=cab, eq=result["eq"])
    return measure(target_x, y, mode)


def measure(target_x, y, mode):
    ta, ya = Analysis(target_x), Analysis(y)
    nl = nonlinear_distance(ta.features(), ya.features())
    if mode == "same":
        p, _ = align(ta.power, ya.power)
        keep = ta.active[p[:, 0]] & ya.active[p[:, 1]]
        d = ta.frame_db[p[keep, 0]] - ya.frame_db[p[keep, 1]]
        e = ta.level[p[keep, 0]] - ya.level[p[keep, 1]]
        nl += float(np.sqrt(np.mean((e - e.mean()) ** 2))) / ENVELOPE_SCALE_DB
        return weighted_rms_centred(smooth_bands(d.mean(axis=0)), ta.weights), nl
    return weighted_rms_centred(smooth_bands(ta.ltas - ya.ltas), ta.weights), nl


# =====================================================================================================
# The synthetic study: known settings in, recovered settings out
# =====================================================================================================

def cab_named(fragment):
    return next(c for c in CAB_FILES if c.stem == fragment)


# The cases the matcher's constants (LAMBDA, the feature scales) were tuned on. Kept apart from the
# reported test cases below, which use other settings, other phrases, and other random seeds.
TUNING_CASES = [
    (1, 4.0, (2, 0, -3, 1, 2), "Modern 4x12, dynamic, 75 W, var. 4", None),
    (2, 6.0, (3, -2, -4, 1, 0), "Modern 4x12, supercardioid, dark 60 W", None),
    (0, 8.0, (0, 2, -1, 3, 1), "Vintage 4x12, dynamic, upper, var. 2", None),
    (1, -6.0, (0, 0, 0, 0, 0), "Modern 4x12, blend, dark + bright 60 W, 1", None),
    (2, -9.0, (-2, 3, 0, -2, 4), "Modern 4x12, dynamic, 75 W, var. 1", None),
    (0, -4.0, (1, -1, 2, -3, 0), "Vintage 4x12, supercardioid, lower", None),
]

TEST_CASES = [
    (0, 10.0, (0, 2, -1, 3, 1), "Vintage 4x12, dynamic, upper, var. 3", None),
    (0, -6.0, (2, -1, 2, -3, 0), "Vintage 4x12, supercardioid, upper", [("peak", 2500.0, 4.0, 1.0)]),
    (0, 18.0, (0, 0, 0, 0, 0), "Vintage 4x12, dynamic, upper, var. 1", None),
    (1, 2.0, (1, 2, -2, 0, 3), "Modern 4x12, dynamic, 75 W, var. 3", None),
    (1, -8.0, (0, 0, 0, 0, 0), "Modern 4x12, blend, 75 W + bright 60 W, 2", [("highShelf", 6000.0, -4.0, SHELF_Q)]),
    (1, 14.0, (0, -3, 2, 0, -2), "Vintage 4x12, dynamic, lower", None),
    (2, 3.0, (4, -2, -5, 2, 1), "Modern 4x12, dynamic, dark 60 W", None),
    (2, -12.0, (-2, 3, 0, -2, 4), "Modern 4x12, dynamic, bright 60 W, var. 1", [("lowShelf", 120.0, 3.0, SHELF_Q)]),
    (2, 15.0, (0, 0, -6, 2, 0), "Modern 4x12, vocal mic, bright 60 W", None),
]


def study_signals(tuning=False):
    """The DIs: (reference for anything mode, reference for same part, target performance for each)."""
    if tuning:
        return {"anything": (synth_di(["lead_a", "lead_c"], 1), synth_di(["lead_b"], 7)),
                "same": (synth_di(["lead_b"], 21), synth_di(["lead_b"], 7, tempo=0.96, jitter_ms=8.0))}
    return {"anything": (synth_di(["lead_c", "lead_a"], 31), synth_di(["lead_d"], 41)),
            "same": (synth_di(["lead_d"], 51), synth_di(["lead_d"], 41, tempo=0.96, jitter_ms=8.0))}


def make_target(case, target_di, variant, seed):
    """Returns (what the matcher is given, the clean guitar it came from). Variants: dry (the guitar
    alone), reverb (with a room on it), mix (with the room, in a synthetic band), separated (the mix
    run through Demucs, the guitar stem kept)."""
    slot, gain, tone, cab, eq = case
    clean = render(target_di, slot, gain, tone=tone, cab=cab_named(cab), eq=eq)
    y = clean
    if variant in ("reverb", "mix", "separated"):
        y = synth_reverb(y, seed)
    if variant in ("mix", "separated"):
        y = mix_with_backing(y, seed)
    if variant == "separated":
        y = separate_guitar(y)
    return y, clean


def mix_with_backing(guitar, seed, guitar_db=0.0):
    """The guitar in a synthetic band: drums and bass at the same RMS as the guitar (guitar_db 0), the
    sum normalized to -1 dBFS peak. What the matcher sees when it's given a whole song."""
    band = synth_backing(len(guitar) / SR + 0.01, seed)[: len(guitar)]
    band *= math.sqrt(np.mean(guitar ** 2) / np.mean(band ** 2)) * 10.0 ** (-guitar_db / 20.0)
    mix = guitar + band
    return mix * 10.0 ** (-1.0 / 20.0) / np.max(np.abs(mix))


def default_baselines(target_x, di, mode):
    """'Leave it at default': each amp at Gain 0, tone flat, the factory presets' usual cab, no EQ."""
    out = []
    for s in range(3):
        y = render(di, s, 0.0, cab=DEFAULT_CAB)
        out.append(measure(target_x, y, mode))
    return out


def fmt_tone(t):
    return "/".join(f"{v:+.0f}" for v in t)


def fmt_eq(eq):
    if not eq:
        return "none"
    return ", ".join(f"{EQ_CODES[k]} {f:.0f} Hz {g:+.1f}" for k, f, g, q in eq if abs(g) >= 0.5) or "flat"


def run_case(case, mode, variant, signals, seed, log=lambda *a: None):
    reference, target_di = signals[mode]
    target_x, clean = make_target(case, target_di, variant, seed)
    r = match(target_x, reference, mode, log=log)
    spectral, nl = verify(r, target_x, reference, mode)          # against what the matcher was given
    spectral_clean, nl_clean = verify(r, clean, reference, mode)  # against the guitar alone
    base = default_baselines(clean, reference, mode)
    slot, gain, tone, cab, eq = case
    row = {
        "mode": mode, "variant": variant,
        "true": {"amp": AMPS[slot], "gain": gain, "tone": list(tone), "cab": cab, "eq": fmt_eq(eq)},
        "got": {"amp": r["amp"], "gain": r["gain_db"], "tone": [round(v, 1) for v in r["tone_db"]],
                "cab": r["cab"][:-4], "eq": fmt_eq(r["eq"])},
        "amp_right": r["slot"] == slot, "cab_right": r["cab"][:-4] == cab,
        "gain_error": r["gain_db"] - gain,
        "spectral_db": spectral_clean, "distortion": nl_clean, "combined": spectral_clean + LAMBDA * nl_clean,
        "spectral_input_db": spectral, "distortion_input": nl,
        "spectral_before_eq_db": r["spectral_error_db"],
        "default_best": min(b[0] + LAMBDA * b[1] for b in base),
        "default_mean": float(np.mean([b[0] + LAMBDA * b[1] for b in base])),
        "default_best_spectral": min(b[0] for b in base),
        "closeness": r["closeness"],
        "runtime_s": r["runtime_s"], "renders": r["renders"],
    }
    return row, r


_demucs = {}


def separate_guitar(x, device=None):
    """The guitar stem of x (48 kHz mono) from Demucs htdemucs_6s (Rouard, Massa, and Defossez, "Hybrid
    Transformers for Music Source Separation", ICASSP 2023), as the Demucs command line runs it: 44.1 kHz
    stereo in, normalized by the mix's mean and standard deviation, split into overlapping 7.8 s segments
    (overlap 0.25), one pass (no shift trick). Needs `uv run --with demucs`. On MPS when available."""
    import torch
    from demucs.apply import apply_model
    from demucs.pretrained import get_model

    if "model" not in _demucs:
        _demucs["model"] = get_model("htdemucs_6s")
        _demucs["model"].eval()
    model = _demucs["model"]
    if device is None:
        device = "mps" if torch.backends.mps.is_available() else "cpu"
    x441 = sps.resample_poly(x, 147, 160)                          # 48 kHz -> 44.1 kHz
    wav = torch.tensor(np.stack([x441, x441]), dtype=torch.float32)
    ref = wav.mean(0)
    mean, std = ref.mean(), ref.std()
    wav = (wav - mean) / std
    t0 = time.time()
    with torch.no_grad():
        out = apply_model(model, wav[None], device=device, shifts=0, split=True, overlap=0.25, progress=False)[0]
    _demucs["last_seconds"] = time.time() - t0
    out = out * std + mean
    guitar = out[model.sources.index("guitar")].mean(0).cpu().numpy().astype(np.float64)
    return sps.resample_poly(guitar, 160, 147)[: len(x)]


def print_rows(rows):
    print("| # | mode | variant | true: amp, Gain, tone D/B/M/T/P, cab, EQ | recovered: amp, Gain, tone, cab, match EQ |"
          " spectral err (dB) | distortion dist | combined | default best / mean | closeness | time (s) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for i, r in enumerate(rows):
        t, g = r["true"], r["got"]
        print(f"| {i + 1} | {r['mode']} | {r['variant']} | {t['amp']} {t['gain']:+.0f}, {fmt_tone(t['tone'])}, {t['cab']}, {t['eq']} |"
              f" {'**' if not r['amp_right'] else ''}{g['amp']}{'**' if not r['amp_right'] else ''} {g['gain']:+.1f}, {fmt_tone(g['tone'])}, {g['cab']}, {g['eq']} |"
              f" {r['spectral_db']:.2f} | {r['distortion']:.2f} | {r['combined']:.2f} | {r['default_best']:.2f} / {r['default_mean']:.2f} |"
              f" {r['closeness']:.0f} | {r['runtime_s']:.1f} |")


def summarize(rows):
    by = {}
    for r in rows:
        by.setdefault((r["mode"], r["variant"]), []).append(r)
    for (mode, variant), rs in by.items():
        amp = sum(r["amp_right"] for r in rs)
        cab = sum(r["cab_right"] for r in rs)
        beat = sum(r["combined"] < r["default_best"] for r in rs)
        print(f"{mode:8s} {variant:10s}: amp right {amp}/{len(rs)}, cab exact {cab}/{len(rs)}, "
              f"median |Gain error| {np.median([abs(r['gain_error']) for r in rs]):.1f} dB, "
              f"spectral error median {np.median([r['spectral_db'] for r in rs]):.2f} dB (max {max(r['spectral_db'] for r in rs):.2f}), "
              f"combined median {np.median([r['combined'] for r in rs]):.2f} vs best default {np.median([r['default_best'] for r in rs]):.2f}, "
              f"beat the best default {beat}/{len(rs)}, runtime median {np.median([r['runtime_s'] for r in rs]):.1f} s")


def study(args):
    signals = study_signals(tuning=args.tuning)
    cases = TUNING_CASES if args.tuning else TEST_CASES
    plan = []
    for mode in ("anything", "same"):
        for i, c in enumerate(cases):
            plan.append((c, mode, "dry", 100 + i))
    if not args.quick:
        for mode in ("anything", "same"):
            for i in (0, 4, 7):
                plan.append((cases[i], mode, "reverb", 200 + i))
        for mode in ("anything", "same"):
            for i in (0, 4, 7):
                plan.append((cases[i], mode, "mix", 300 + i))
    if args.separation:
        for mode in ("anything", "same"):
            for i in (0, 4, 7):
                plan.append((cases[i], mode, "separated", 300 + i))
    rows = []
    for c, mode, variant, seed in plan:
        row, _ = run_case(c, mode, variant, signals, seed)
        rows.append(row)
        print(f"  {mode} {variant} {row['true']['amp']} {row['true']['gain']:+.0f} -> {row['got']['amp']} {row['got']['gain']:+.1f} "
              f"spectral {row['spectral_db']:.2f} dist {row['distortion']:.2f} ({row['runtime_s']:.1f} s)", flush=True)
    print_rows(rows)
    summarize(rows)
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(rows, indent=1))


def feature_study(args):
    """How well each distortion feature separates settings, against how much it moves when only the
    playing changes: five lead performances through nine (slot, Gain) settings and one cab. For each
    feature, the spread of the per-setting means over the mean spread across performances (a Fisher-style
    ratio: higher is better). Spectral flatness of whitened frames (tried first) scored 0.4: it measured
    how many notes were sounding, not the amp."""
    perfs = [(["lead_a", "lead_c"], 1), (["lead_b"], 7), (["lead_c", "lead_b"], 3), (["lead_a"], 9), (["lead_c"], 13)]
    dis = [synth_di(p, sd) for p, sd in perfs]
    settings = [(s_, g) for s_ in range(3) for g in (-12.0, 0.0, 12.0)]
    _, ir = read_wav(cab_named("Modern 4x12, dynamic, 75 W, var. 4"))
    ir = ir[:, 0] if ir.ndim == 2 else ir

    def flatness(y):
        a = Analysis(y)
        bins = np.nonzero((FREQS >= 150.0) & (FREQS < 6000.0))[0]
        wb = np.maximum(a.power[a.active][:, bins] / np.maximum(a.ltas_bins[bins], TINY), TINY)
        return float(np.median(10.0 * np.log10(np.exp(np.mean(np.log(wb), axis=1)) / np.mean(wb, axis=1))))

    names = FEATURE_NAMES + ["whitened spectral flatness (dB), rejected"]
    table = np.zeros((len(settings), len(dis), len(names)))
    for i, (slot, gain) in enumerate(settings):
        for j, y in enumerate(render_many([(d, slot, gain) for d in dis])):
            y = sps.fftconvolve(y, ir)[: len(y)]
            table[i, j] = list(Analysis(y).features()) + [flatness(y)]
    for k, name in enumerate(names):
        means = table[:, :, k].mean(axis=1)
        within = table[:, :, k].std(axis=1).mean()
        print(f"{name:45s} ratio {means.std() / within:5.2f}   per-setting means {np.round(means, 2)}   spread across playing {within:.2f}")


def match_files(args):
    target = to_mono_48k(args.target)
    di = to_mono_48k(args.di)
    if args.start is not None or args.end is not None:
        a = int((args.start or 0.0) * SR)
        b = int(args.end * SR) if args.end is not None else len(target)
        target = target[a:b]
    r = match(target, di, args.mode)
    spectral, nl = verify(r, target, di, args.mode)
    print(json.dumps({k: v for k, v in r.items() if k not in ("eq_target", "residual")}, indent=1))
    print(f"verified through the chain: spectral error {spectral:.2f} dB, distortion distance {nl:.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("match", help="match a target file with a DI file")
    m.add_argument("target"); m.add_argument("di")
    m.add_argument("--mode", choices=["anything", "same"], default="anything")
    m.add_argument("--start", type=float); m.add_argument("--end", type=float)
    st = sub.add_parser("study", help="the synthetic ground-truth study")
    st.add_argument("--quick", action="store_true", help="dry cases only")
    st.add_argument("--tuning", action="store_true", help="the tuning cases instead of the test cases")
    st.add_argument("--json", help="write the rows here")
    st.add_argument("--separation", action="store_true", help="also the Demucs cases (needs uv run --with demucs)")
    sub.add_parser("features", help="the distortion feature study")
    args = ap.parse_args()
    if args.cmd == "features":
        feature_study(args)
    elif args.cmd == "match":
        match_files(args)
    elif args.cmd == "study":
        study(args)


if __name__ == "__main__":
    main()

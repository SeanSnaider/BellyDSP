# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["neural-amp-modeler==0.12.3", "numpy", "scipy", "demucs"]
# ///
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Learning a capture from the song itself: prototype and synthetic study (docs/TONE_MATCH.md, "Learning a
capture from the song (prototype)").

The current tone match chooses among three amp gain sets, their Gain and tone, 21 cabs, and a match EQ. That
can fit brightness but not feel: how loudness, compression, and harmonics change with picking strength, the
attack, the bloom. The idea here: train a small NAM WaveNet that turns the player's play-along DI take into
the record's guitar, so the dynamics come from the record itself.

The obstacle is that the two performances are never sample-identical. The player's notes land 10 to 40 ms
off, at other velocities, with other pick attacks, so NAM's usual loss, the error-to-signal ratio (ESR,
sample by sample), compares unrelated waveforms. What this does instead:

  1. Alignment, per note. The take's note onsets are found in the clean DI (spectral flux, refined to the
     sample by the energy rise). Each is mapped into the target's timeline through the chroma DTW path
     tone_match.py already computes, then moved to the nearest onset of THAT note in the target: the
     target's onset strength measured only on the bins near the note's harmonics (its pitch comes from the
     DI, McLeod's method), within +-60 ms of the DTW estimate. So drum hits and other guitars in a mix
     don't pull the onsets.
  2. A per-note loss (the brief's option b). The model runs on the take exactly as it was played (nothing is
     warped or stretched, so the input has no artefacts), and each note's output, from 5 ms before its onset
     to its end (the next onset in either performance, whichever comes first), is compared with the same
     stretch of the target from that note's onset there. The comparison is a multi-resolution STFT loss
     (MRSTFT; Yamamoto, Song, Kim 2020; Steinmetz and Reiss, auraloss, 2020): for FFT sizes 256 to 4096,
         L = sum_r ( || |S_r(y)| - |S_r(t)| ||_F / || |S_r(t)| ||_F  +  mean | log|S_r(y)| - log|S_r(t)| | ),
     the spectral convergence plus the log-magnitude distance. Magnitudes only: phase, which two
     performances never share, doesn't count. The smallest FFT (5 ms) still sees the attack, the largest
     (85 ms) the harmonics. No ESR term at all: on two different performances it only teaches the model to
     output less (the error is smallest for a silent model).
  3. The model: NAM's "feather" WaveNet (NAM 0.12.3's own architecture table, 2 layer arrays of 8 and 4
     channels, receptive field 4093 samples), built from nam.models.wavenet and exported with NAM's own
     exporter, so NAM core (and the app) loads the .nam as any capture. Adam, 4e-3 decaying to 4e-4, 15% of
     the notes held out to keep the best step.

Option (a), warping the DI to the target's timing, needs a time stretch where the gaps differ (a jitter of
40 ms on a 250 ms note is 16%), which either shifts the pitch or splices in the next note's attack; the
per-note loss gets the same alignment with an untouched input. A third variant, "latent velocity", adds one
learnable gain per note in front of the model (penalized, mean square, in dB): the player hit the note
softer or harder than the record's guitarist, and with the gain free the model doesn't have to learn that
difference as part of the rig. Why that helps the compression curve: comparing per-note levels, the target's
levels follow the original's velocities a_k, the take's are b_k = a_k + n_k with independent playing noise
n_k, so fitting the rig's level curve on b_k flattens it by var(a) / (var(a) + var(n)) (regression dilution,
errors in variables). With a per-note gain g_k free, every curve fits, and the smallest penalty sum g_k^2
picks g_k = -n_k, the true curve (since n is independent of a). It's in the study as "latent".

Usage (uv reads the dependencies from the header above; the first run builds a 1 GB environment):
  uv run prototypes/learn_tone.py <take folder>         # a take saved by the app's Match tone page, Save take
  uv run prototypes/learn_tone.py study [--quick] [--cases high_gain,crunch] [--json out.json]
  uv run prototypes/learn_tone.py probe <a.nam> ...     # the feel probe on captures (relative to the first)

The synthetic study needs the Release build of ampsim_render (cmake --build build -j): the current
matcher renders through it, and so does the export check. Renders are cached in
$TMPDIR/bellydsp_learn_tone_cache (or $LEARN_TONE_CACHE).
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
import scipy.signal as sps

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import amp_sim  # noqa: E402  the gray-box amp (the hidden rigs' circuits)
import compressor  # noqa: E402  the compressor's reference simulation (the hidden rigs' compressors)
import pitch_detection  # noqa: E402  McLeod's method (the informed mask's pitches)
import tone_match as tm  # noqa: E402  the current matcher, its analysis, its synthetic DIs

REPO = HERE.parent
SR = 48000
RENDER = tm.RENDER
CACHE = pathlib.Path(os.environ.get("LEARN_TONE_CACHE", tempfile.gettempdir())) / "bellydsp_learn_tone_cache"
IRS = REPO / "content/irs"
# The current matcher as the app runs it: the built-in amps are gain sets since 2026-10-04 (tone_match.py's
# own golden fixtures still use the old single captures).
GAIN_SETS = [REPO / "content/models" / a / "gainset.json" for a in tm.AMPS]


def db(x):
    return 10.0 * np.log10(np.maximum(x, 1e-20))


def rms_db(x):
    return float(db(np.mean(np.square(x))))


def key_of(*parts):
    h = hashlib.sha1()
    for p in parts:
        h.update(np.asarray(p, dtype=np.float32).tobytes() if isinstance(p, np.ndarray) else str(p).encode())
        h.update(b"|")
    return h.hexdigest()[:20]


def cached(name, fn, *parts):
    """fn() cached on disk under a key of `parts` (arrays by content)."""
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / f"{name}_{key_of(*parts)}.npy"
    if path.exists():
        return np.load(path)
    y = np.asarray(fn(), dtype=np.float64)
    np.save(path, y)
    return y


# =====================================================================================================
# Performances: one score, played by different players
# =====================================================================================================

# A fourth lead for the held-out DI (C): neither the target's guitarist nor the player played it.
def _lead_e():
    line = [(0, 0.5, 74), (0.5, 0.25, 76), (0.75, 0.25, 77), (1, 1, 79), (2, 0.5, 77), (2.5, 0.5, 76),
            (3, 1, 74), (4, 0.25, 71), (4.25, 0.25, 72), (4.5, 0.25, 74), (4.75, 0.25, 76), (5, 1.5, 77),
            (6.5, 0.5, 74), (7, 1, 72), (8, 0.75, 69), (8.75, 0.25, 71), (9, 0.5, 72), (9.5, 0.5, 74),
            (10, 1, 76), (11, 1, 79), (12, 0.25, 81), (12.25, 0.25, 79), (12.5, 0.5, 76), (13, 1, 74), (14, 2, 72)]
    return [(b, l, [m], False) for b, l, m in line], 16.0


PHRASES = dict(tm.PHRASES, lead_e=_lead_e)
TRAIN_SCORE = ["lead_a", "lead_d", "lead_b"]       # 48 beats at 120 BPM: 24 s, 81 notes
HELD_OUT_SCORE = ["lead_c", "lead_e"]               # 32 beats: 16 s, 48 notes, other melodies


def score(parts):
    """The phrases one after another: a list of (beat, length in beats, MIDI notes, palm muted)."""
    events, offset = [], 0.0
    for name in parts:
        notes, length = PHRASES[name]()
        events += [(offset + b, l, ms, mute) for b, l, ms, mute in notes]
        offset += length
    return events, offset


def contour_db(beat, depth=6.0):
    """The music's own dynamics, shared by every performance of the score: a slow swell over about three
    bars, plus accents on the downbeat (+2 dB) and the other beats (+1 dB). In dB of picking strength."""
    swell = depth * math.sin(2.0 * math.pi * beat / 13.0 + 0.7)
    accent = 2.0 if abs(beat % 4.0) < 1e-6 else 1.0 if abs(beat % 1.0) < 1e-6 else 0.0
    return swell + accent


def perform(parts, seed, *, tempo=1.0, jitter_ms=0.0, contour_scale=1.0, velocity_sd_db=2.0, brightness_shift=0.0,
            pickup_hz=4500.0, level_db=-16.0):
    """A plucked-string DI of the score (tone_match.pluck, Karplus-Strong) with note dynamics. Each note's
    picking strength v_k (dB) = contour_scale x contour_db(beat) + N(0, velocity_sd_db); it sets the
    note's peak level, level_db + v_k dBFS, and the pick's brightness (a harder pick is brighter:
    brightness 0.55 + 0.025 v_k + brightness_shift, +-0.04 at random). Each onset is the beat's time at
    120 BPM x tempo plus N(0, jitter_ms); a note rings until the next onset (legato). Then a pickup's
    resonance (2-pole low-pass, Q 1.5). Returns (di, onsets in samples, v_k)."""
    rng = np.random.default_rng(seed)
    events, length = score(parts)
    beat_s = 0.5 / tempo
    times = np.array([b * beat_s + rng.normal(0.0, jitter_ms / 1000.0) for b, _, _, _ in events])
    times[0] = max(times[0], 0.05)
    for i in range(1, len(times)):                     # a player never swaps two notes
        times[i] = max(times[i], times[i - 1] + 0.03)
    total = int((length * beat_s + 1.5) * SR)
    out = np.zeros(total)
    onsets, velocities = [], []
    for i, (b, l, ms, mute) in enumerate(events):
        start = int(round(times[i] * SR))
        end = int(round(times[i + 1] * SR)) if i + 1 < len(events) else min(total, start + int(l * beat_s * SR) + int(0.4 * SR))
        n = min(end - start, total - start)
        v = contour_scale * contour_db(b) + rng.normal(0.0, velocity_sd_db)
        bright = float(np.clip(0.55 + 0.025 * v + brightness_shift + rng.uniform(-0.04, 0.04), 0.15, 0.95))
        level = 10.0 ** ((level_db + v) / 20.0) / math.sqrt(len(ms))
        for m in ms:
            out[start:start + n] += tm.pluck(tm.midi_hz(m), n, rng, level=level, brightness=bright, muted=mute)
        onsets.append(start)
        velocities.append(v)
    w0 = 2 * math.pi * pickup_hz / SR
    alpha = math.sin(w0) / (2 * 1.5)
    b = np.array([(1 - math.cos(w0)) / 2, 1 - math.cos(w0), (1 - math.cos(w0)) / 2])
    a = np.array([1 + alpha, -2 * math.cos(w0), 1 - alpha])
    return sps.lfilter(b / a[0], a / a[0], out), np.array(onsets), np.array(velocities)


def note_pitches(parts):
    events, _ = score(parts)
    return np.array([tm.midi_hz(ms[0]) for _, _, ms, _ in events])


# =====================================================================================================
# Hidden rigs: what made the "record". None of them is one of the built-in amps.
# =====================================================================================================

# Gray-box channels the built-ins weren't trained from (the built-ins are amp_sim's clean, crunch, and lead
# channels at their own knobs). Other tone-stack voices, other stage counts and biases.
amp_sim.CHANNELS.update({
    "hidden_hg": dict(voice="fender", low_cut_db=-7.0, mid_push_db=5.0, stages=[(9.0, 0.3), (5.0, 0.05), (2.5, 0.1)],
                      interstage_lp=5500, gain_taper_db=[0.0] * 5),
    "hidden_crunch": dict(voice="fender", low_cut_db=-2.0, mid_push_db=2.0, stages=[(4.0, 0.35), (2.0, 0.0)],
                          interstage_lp=8000, gain_taper_db=[0.0] * 5),
    "hidden_edge": dict(voice="marshall", low_cut_db=0.0, mid_push_db=3.0, stages=[(2.2, 0.15), (1.6, 0.05)],
                        interstage_lp=9000, gain_taper_db=[0.0] * 5),
    "hidden_lead": dict(voice="marshall", low_cut_db=-10.0, mid_push_db=6.0, stages=[(12.0, 0.2), (4.0, 0.2)],
                        interstage_lp=6500, gain_taper_db=[0.0] * 5),
})

STUDIO = dict(mode="studio", detector="rms", knee_db=6.0, auto_release=False, auto_makeup=False, makeup_db=0.0, mix=1.0,
              sidechain_hz=80.0, sidechain_high_pass=False)

RIGS = {
    # High gain: a Screamer (the app's TS808 circuit model, ported from circuits.py) at +3 dB into a
    # three-stage channel with a Fender-style stack, a 75 W cab, a mid scoop.
    "high_gain": dict(boost=3.0, channel="hidden_hg", drive_db=-26.0, knobs=dict(bass=6.0, mid=3.5, treble=7.0, master=4.0),
                      cab="Modern 4x12/Modern 4x12, dynamic, 75 W, var. 2.wav", eq=[("peak", 700.0, -3.0, 0.8)],
                      comp=None, pre_comp=None, fx=None, tone_type="hi_gain", jitter_ms=25.0),
    # Crunch: two asymmetric stages (strong even harmonics), the power amp pushed (master 6), then a bus
    # compressor after the cab (threshold -12 dB, 3:1, 10 ms, 150 ms: about 7 dB of gain reduction on the
    # rig's level, so each note's first 10 ms stand out of a clamped sustain).
    "crunch": dict(boost=None, channel="hidden_crunch", drive_db=-4.0, knobs=dict(bass=5.0, mid=6.0, treble=6.0, master=6.0),
                   cab="Vintage 4x12/Vintage 4x12, supercardioid, lower.wav", eq=[("highshelf", 5000.0, -3.0, 0.7071)],
                   comp=dict(STUDIO, threshold_db=-12.0, ratio=3.0, attack_ms=10.0, release_ms=150.0), pre_comp=None,
                   fx=None, tone_type="crunch", jitter_ms=15.0),
    # Edge of breakup behind a pedal compressor (feedback, 3:1, the detector after the gain): the feel case.
    # It cleans up when picked softly and the compressor squashes the attack's level.
    "edge_comp": dict(boost=None, channel="hidden_edge", drive_db=4.0, knobs=dict(bass=5.0, mid=5.0, treble=6.5, master=5.0),
                      cab="Vintage 4x12/Vintage 4x12, dynamic, upper, var. 1.wav", eq=[("lowshelf", 150.0, 2.0, 0.7071)],
                      comp=None, pre_comp=dict(STUDIO, mode="pedal", detector="peak", threshold_db=-30.0, ratio=3.0,
                                               attack_ms=2.0, release_ms=200.0, makeup_db=8.0),
                      fx=None, tone_type="overdrive", jitter_ms=40.0),
    # High gain with a delay (360 ms, -10 dB, feedback 0.35) and a room (RT60 1.8 s, -14 dB) on the record. The
    # learned capture and the matcher are judged against the dry rig: the app has its own delay and reverb.
    "high_gain_fx": dict(boost=None, channel="hidden_lead", drive_db=-16.0, knobs=dict(bass=4.5, mid=6.5, treble=6.0, master=3.0),
                         cab="Modern 4x12/Modern 4x12, vocal mic, bright 60 W.wav", eq=[("peak", 2200.0, 3.0, 1.0)],
                         comp=None, pre_comp=None, fx=dict(delay_s=0.36, delay_db=-10.0, feedback=0.35, rt60=1.8, room_db=-14.0),
                         tone_type="hi_gain", jitter_ms=10.0),
}


def read_ir(rel):
    _, ir = tm.read_wav(IRS / rel)
    ir = ir[:, 0] if ir.ndim == 2 else ir
    return ir[:SR] / np.sqrt(np.sum(ir[:SR] ** 2))


def ampsim_render(x, args):
    """x through ampsim_render with these arguments; the left channel."""
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = pathlib.Path(tmp) / "in.wav", pathlib.Path(tmp) / "out.wav"
        tm.write_wav(src, x)
        r = subprocess.run([str(RENDER)] + [str(a) for a in args] + [str(src), str(dst)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stdout + r.stderr)
        _, y = tm.read_wav(dst)
    return y[:, 0] if y.ndim == 2 else y


def rig_dry(name, x):
    """The hidden rig without its time effects: (pre compressor) -> (Screamer) -> gray-box amp -> cab IR ->
    EQ -> (bus compressor). Cached."""
    spec = RIGS[name]

    def run():
        y = np.asarray(x, dtype=np.float64)
        if spec["pre_comp"] is not None:
            y = compressor.compress(y[None], spec["pre_comp"])[0]
        if spec["boost"] is not None:
            y = ampsim_render(y, ["--boost", "screamer", "--boost-level", spec["boost"]])
        y = amp_sim.amp(y, SR, spec["channel"], cab=False, drive_db=spec["drive_db"], **spec["knobs"])
        y = amp_sim.dc_block(y, r=0.998)
        y = sps.fftconvolve(y, read_ir(spec["cab"]))[: len(y)]
        for kind, f, g, q in spec["eq"]:
            y = amp_sim.filt(y, kind, f, SR, q=q, gain_db=g)
        if spec["comp"] is not None:
            y = compressor.compress(y[None], spec["comp"])[0]
        return y

    return cached("rig_" + name, run, x, json.dumps(spec, sort_keys=True))


def rig_fx(name, y, seed=11):
    """The record's time effects, if the rig has them: a feedback delay, then a synthetic room."""
    fx = RIGS[name]["fx"]
    if fx is None:
        return y
    d = int(fx["delay_s"] * SR)
    a = np.zeros(d + 1)
    a[0], a[d] = 1.0, -fx["feedback"]
    b = np.zeros(d + 1)
    b[d] = 10.0 ** (fx["delay_db"] / 20.0)
    wet = sps.lfilter(b, a, y)                         # y[n - d] g + feedback x the delay's own output
    return tm.synth_reverb(y + wet, seed, rt60=fx["rt60"], wet_db=fx["room_db"])


# =====================================================================================================
# The record: the lead in a band, and separated again
# =====================================================================================================

RHYTHM_SCORE = ["riff_a", "riff_b"] * 3              # 48 beats under the lead


def rhythm_guitar(seed):
    """A second guitar under the lead: palm-muted riffs and power chords, through the gray-box lead
    channel and a dark 60 W cab. Demucs returns it with the lead (both are "guitar")."""
    di, _, _ = perform(RHYTHM_SCORE, seed, jitter_ms=5.0, velocity_sd_db=1.0, level_db=-10.0)

    def run():
        y = amp_sim.amp(di, SR, "crunch", cab=False, drive_db=-6.0, bass=6.0, mid=4.0, treble=5.0, master=4.0)
        return sps.fftconvolve(amp_sim.dc_block(y, 0.998), read_ir("Modern 4x12/Modern 4x12, dynamic, dark 60 W.wav"))[: len(y)]

    return cached("rhythm", run, di)


def mix_song(lead, seed):
    """The lead with drums and bass at the lead's RMS and the rhythm guitar 3 dB under it, normalized to -1
    dBFS peak."""
    n = len(lead)
    band = tm.synth_backing(n / SR + 0.01, seed)[:n]
    band *= math.sqrt(np.mean(lead ** 2) / np.mean(band ** 2))
    rhythm = rhythm_guitar(seed)
    rhythm = np.concatenate([rhythm, np.zeros(max(0, n - len(rhythm)))])[:n]
    rhythm *= math.sqrt(np.mean(lead ** 2) / np.mean(rhythm ** 2)) * 10.0 ** (-3.0 / 20.0)
    mix = lead + band + rhythm
    return mix * 10.0 ** (-1.0 / 20.0) / np.max(np.abs(mix))


def separate(mix):
    """Demucs htdemucs_6s's guitar stem (tone_match.separate_guitar: what stage A used), cached."""
    return cached("demucs", lambda: tm.separate_guitar(mix), mix)


def si_sdr(reference, estimate):
    """Scale-invariant SDR (Le Roux et al., "SDR - half-baked or well done?", ICASSP 2019): the estimate's
    projection on the reference against what's left, dB. Insensitive to the stem's level."""
    n = min(len(reference), len(estimate))
    s, e = reference[:n], estimate[:n]
    alpha = np.dot(e, s) / np.dot(s, s)
    return float(db(np.sum((alpha * s) ** 2) / np.sum((alpha * s - e) ** 2)))


# =====================================================================================================
# Notes: onsets in the take, their partners in the target, their pitches
# =====================================================================================================

ONSET_FFT, ONSET_HOP = 1024, 120      # 2.5 ms hop


def spectral_flux(x, bins=None):
    """Onset strength (Bello et al., "A tutorial on onset detection in music signals", IEEE TSAP 2005):
    the positive part of each bin's log-magnitude rise since the last frame, summed over the bins (100 Hz
    to 10 kHz, or the bins given), per 2.5 ms frame. Frame t covers samples [t hop - fft/2, t hop + fft/2)."""
    padded = np.concatenate([np.zeros(ONSET_FFT // 2), x, np.zeros(ONSET_FFT)])
    frames = np.lib.stride_tricks.sliding_window_view(padded, ONSET_FFT)[::ONSET_HOP]
    mag = np.abs(np.fft.rfft(frames * np.hanning(ONSET_FFT), axis=1))
    logm = np.log(mag + 1e-4 * np.max(mag))
    f = np.fft.rfftfreq(ONSET_FFT, 1.0 / SR)
    if bins is None:
        bins = (f >= 100.0) & (f <= 10000.0)
    rise = np.maximum(0.0, np.diff(logm[:, bins], axis=0))
    return np.concatenate([[0.0], rise.sum(axis=1)])


def refine_onset(x, guess, radius=480):
    """The sample where the energy rises fastest near a guess: a 1 ms RMS envelope e, and the largest rise
    e[n] - e[n - 48] within +-10 ms, moved back half a window. A pluck's burst starts there."""
    lo, hi = max(48, guess - radius), min(len(x) - 1, guess + radius)
    seg = x[lo - 48:hi + 48] ** 2
    env = np.sqrt(np.convolve(seg, np.ones(48) / 48, mode="same"))
    rise = env[48:] - env[:-48]
    return int(lo + np.argmax(rise[: hi - lo]) - 24)


def flux_peaks(di, min_gap_s=0.045):
    """Spectral-flux peaks over a moving-median threshold (the local median plus 10% of the flux's 99th
    percentile; Bello et al. 2005, sec. III), at least 45 ms apart, in samples (frame times)."""
    sf = spectral_flux(di)
    med = sps.medfilt(sf, 81)                          # +-100 ms
    thr = med + 0.1 * np.percentile(sf, 99)
    peaks, _ = sps.find_peaks(sf, height=thr, distance=int(min_gap_s * SR / ONSET_HOP))
    return peaks * ONSET_HOP


def detect_onsets(di):
    """Note onsets in a clean DI: the flux peaks, each refined to the sample (refine_onset)."""
    return np.array([refine_onset(di, int(p)) for p in flux_peaks(di)])


def flux_lag(di, onsets):
    """How far a flux peak's frame time sits from the sample-exact onset, measured on the DI (median, in
    samples; negative: the flux peaks early, because a frame's time is its centre and the rise shows as
    soon as the window's leading half reaches the attack). The target's flux peaks are corrected by it,
    so the only constant offset left between take and target is the rig's own delay."""
    peaks = flux_peaks(di)
    d = [p - onsets[np.argmin(np.abs(onsets - p))] for p in peaks]
    return int(np.median(d))


def note_f0(di, onsets, ends):
    """Each note's pitch from the DI: McLeod's method (pitch_detection.mpm, the harmonizer's settings: 110 Hz
    to 1.4 kHz on a 12 kHz signal) every 10 ms from 20 ms after the onset to the note's end, the median of
    the readings with clarity 0.9 or more. 0 if none qualify."""
    b, a = sps.butter(8, 3000.0, fs=SR)
    lowpassed = sps.lfilter(b, a, di)[3::4]
    fsd = SR // 4
    s = pitch_detection.HARMONIZER
    length = pitch_detection.frame_length(s["fmin"], s["min_window"], fsd)
    out = []
    for o, e in zip(onsets, ends):
        readings = []
        for n in range((o + int(0.02 * SR)) // 4 + length, e // 4, fsd // 100):
            f, c = pitch_detection.mpm(lowpassed[n - length:n], fsd, s["fmin"], s["fmax"], s["min_window"], s["k"])
            if c >= 0.9:
                readings.append(f)
        out.append(float(np.median(readings)) if readings else 0.0)
    return np.array(out)


def harmonic_bins(f0, freqs, harmonics=12, tolerance=0.03):
    bins = np.zeros(len(freqs), dtype=bool)
    if f0 <= 0:
        return (freqs >= 100.0) & (freqs <= 10000.0)
    for h in range(1, harmonics + 1):
        bins |= np.abs(freqs - h * f0) <= tolerance * h * f0 + 0.6 * SR / ONSET_FFT
    return bins


def align_notes(di, target, f0=None, onsets=None, band_seconds=tm.PLAY_ALONG_BAND_SECONDS, search_s=0.06, prior_s=0.03):
    """The take's notes and their partners in the target. Returns a list of notes, each a dict with the
    take's onset o and next onset, the target's onset t and next onset, the pitch f0, and the length L
    both have before either one's next note (the stretch the loss compares).

    1. Onsets in the take (detect_onsets) and each note's pitch (note_f0).
    2. The coarse map: tone_match's chroma DTW (align) between the target and the take through a plain
       distortion (tanh(10 x), so its partials look like the target's), within the play-along band (0.5 s;
       None: unbanded). The band matters on a mix: unbanded, chroma DTW on the high-gain case's mix put 10% of
       the notes more than 0.7 s off; banded, 61 ms. Each take frame's mean partner, linearly interpolated, puts the onset in the target.
    3. The fine one: the target's onset strength over the bins near this note's first 12 harmonics, times
       a Gaussian prior (30 ms) around the DTW estimate, maximized within +-60 ms, minus the flux's own lag
       measured on the DI (flux_lag). Onsets are kept in order, at least 20 ms apart."""
    if onsets is None:
        onsets = detect_onsets(di)
    ends = np.append(onsets[1:], min(len(di), onsets[-1] + int(1.0 * SR)))
    if f0 is None:
        f0 = note_f0(di, onsets, ends)
    path, _ = tm.align(tm.stft_power(target), tm.stft_power(np.tanh(10.0 * di)), tm.band_frames(band_seconds))
    j_frames = path[:, 1].astype(float)
    i_frames = path[:, 0].astype(float)
    uj = np.unique(j_frames)
    mean_i = np.array([i_frames[j_frames == j].mean() for j in uj])
    centre = tm.N_FFT / 2
    est = np.interp((onsets - centre) / tm.HOP, uj, mean_i) * tm.HOP + centre
    freqs = np.fft.rfftfreq(ONSET_FFT, 1.0 / SR)
    lag = flux_lag(di, onsets)
    t_on = []
    for k, (o, e) in enumerate(zip(onsets, est)):
        bins = harmonic_bins(f0[k], freqs)
        lo, hi = int(e - search_s * SR), int(e + search_s * SR)
        lo, hi = max(0, lo), min(len(target) - 1, hi)
        if hi - lo < 4 * ONSET_HOP:
            t_on.append(int(np.clip(e, 0, len(target) - 1)))
            continue
        seg = target[max(0, lo - ONSET_FFT):hi + ONSET_FFT]
        sf = spectral_flux(seg, bins)
        times = max(0, lo - ONSET_FFT) + np.arange(len(sf)) * ONSET_HOP
        keep = (times >= lo) & (times <= hi)
        score_ = sf[keep] * np.exp(-0.5 * ((times[keep] - e) / (prior_s * SR)) ** 2)
        t = int(times[keep][np.argmax(score_)]) - lag if score_.size and score_.max() > 0 else int(e)
        t_on.append(t)
    t_on = np.array(t_on)
    for k in range(1, len(t_on)):
        t_on[k] = max(t_on[k], t_on[k - 1] + int(0.02 * SR))
    t_next = np.append(t_on[1:], min(len(target), t_on[-1] + int(1.0 * SR)))
    notes = []
    for k in range(len(onsets)):
        L = int(min(ends[k] - onsets[k], t_next[k] - t_on[k]))
        if L > int(0.03 * SR) and t_on[k] + L <= len(target):
            notes.append(dict(o=int(onsets[k]), t=int(t_on[k]), L=L, f0=float(f0[k]), o_next=int(ends[k]), t_next=int(t_next[k])))
    return notes


# =====================================================================================================
# Informed separation: a harmonic mask from the take's pitches
# =====================================================================================================

MASK_FFT, MASK_HOP = 4096, 512
MASK_CENTS = 35.0


def refine_pitch_in_target(target, note, freqs):
    """The note's pitch as the target plays it: the DI's pitch times 2^(c/1200), c in -60 .. +60 cents in
    5-cent steps, choosing the c whose first 8 harmonics hold the most magnitude in the target's note
    (an informed search: the record may be tuned a little differently)."""
    seg = target[note["t"] + int(0.02 * SR): note["t"] + note["L"]]
    if note["f0"] <= 0 or len(seg) < 2048:
        return note["f0"]
    mag = np.abs(np.fft.rfft(seg[:16384] * np.hanning(min(len(seg), 16384)), n=16384))
    f = np.fft.rfftfreq(16384, 1.0 / SR)
    best, best_c = -1.0, 0.0
    for c in np.arange(-60.0, 61.0, 5.0):
        f0 = note["f0"] * 2.0 ** (c / 1200.0)
        s = sum(np.interp(h * f0, f, mag) for h in range(1, 9))
        if s > best:
            best, best_c = s, c
    return note["f0"] * 2.0 ** (best_c / 1200.0)


def informed_mask(target, notes, harmonics=40, floor=0.1):
    """Keeps the target's energy near the played notes' harmonics: a soft mask on the STFT (4096 points,
    hop 512), M(t, f) = max over the notes sounding in frame t and their harmonics h of
        exp(-1/2 ((f - h f0) / sigma_h)^2),  sigma_h = max(h f0 (2^(35/1200) - 1), 1.5 bins),
    (35 cents, or the Hann window's main lobe for the low partials), floored at `floor` (0.1: what isn't
    near a harmonic is turned down 20 dB, not removed; with 0, the mask applied to the clean record alone
    moved its long-term spectrum by 11 to 23 dB in the study, because a distorted guitar has real energy
    between and below the harmonics, and all of it went); then the inverse
    STFT (overlap-add). A monophonic note through any distortion has only harmonics of its f0, so this
    keeps the lead and its distortion and drops the drums, the bass, and the other guitar between them
    (what's on top of a harmonic stays). Frames with no note are silenced. The notes' pitches come from the
    take (McLeod), each refined against the target (refine_pitch_in_target)."""
    f, t, Z = sps.stft(target, SR, nperseg=MASK_FFT, noverlap=MASK_FFT - MASK_HOP, boundary="zeros", padded=True)
    freqs = f
    mask = np.full(Z.shape, floor)
    bin_hz = SR / MASK_FFT
    for note in notes:
        f0 = refine_pitch_in_target(target, note, freqs)
        if f0 <= 0:
            continue
        start, end = note["t"], note["t_next"]
        # Frames whose window (centred on t, MASK_FFT long) overlaps the note's span.
        cols = np.nonzero((t * SR + MASK_FFT / 2 > start) & (t * SR - MASK_FFT / 2 < end))[0]
        m = np.zeros(len(freqs))
        for h in range(1, harmonics + 1):
            fh = h * f0
            if fh > 0.45 * SR:
                break
            sigma = max(fh * (2.0 ** (MASK_CENTS / 1200.0) - 1.0), 1.5 * bin_hz)
            m = np.maximum(m, np.exp(-0.5 * ((freqs - fh) / sigma) ** 2))
        mask[:, cols] = np.maximum(mask[:, cols], m[:, None])
    _, y = sps.istft(Z * mask, SR, nperseg=MASK_FFT, noverlap=MASK_FFT - MASK_HOP, boundary=True)
    return np.concatenate([y, np.zeros(max(0, len(target) - len(y)))])[: len(target)]


# =====================================================================================================
# Learning the capture
# =====================================================================================================

FFT_SIZES = [4096, 2048, 1024, 512, 256]


def nam_imports():
    import torch
    from nam._dependencies.auraloss.freq import MultiResolutionSTFTLoss
    from nam.models.wavenet import WaveNet
    from nam.train.core import Architecture, get_wavenet_config
    return torch, MultiResolutionSTFTLoss, WaveNet, Architecture, get_wavenet_config


def device_name():
    import torch
    return "mps" if torch.backends.mps.is_available() else "cpu"


def new_model(arch="feather"):
    torch, _, WaveNet, Architecture, get_wavenet_config = nam_imports()
    cfg = get_wavenet_config(Architecture(arch))
    return WaveNet(layers_configs=cfg["layers_configs"], head_scale=cfg["head_scale"], sample_rate=SR)


class FlooredMRSTFT:
    """The MRSTFT loss with the log-magnitude term floored: for each resolution r,
        SC_r  = || |Y_r| - |T_r| ||_F / || |T_r| ||_F                       (spectral convergence)
        LOG_r = mean | log(|Y_r| + e_r) - log(|T_r| + e_r) |,  e_r = 10^(floor/20) max |T_r|
    summed over r (per example's own maximum). auraloss's version adds 1e-8 instead of e_r, so the bins
    between the harmonics, 80 or 100 dB down, count as much as the harmonics: between two performances they
    hold unrelated noise (different pick noise, different decay), and a model can lower that term by filling
    them in. With the floor at -50 dB the valleys stop mattering."""

    def __init__(self, sizes, floor_db=-50.0):
        import torch
        self.torch, self.sizes, self.floor = torch, sizes, 10.0 ** (floor_db / 20.0)
        self.windows = {}

    def __call__(self, pred, target):
        torch = self.torch
        total = 0.0
        for n in self.sizes:
            if n not in self.windows:
                self.windows[n] = torch.hann_window(n, device=pred.device)
            a = torch.stft(pred, n, n // 4, window=self.windows[n], return_complex=True).abs()
            b = torch.stft(target, n, n // 4, window=self.windows[n], return_complex=True).abs()
            sc = torch.linalg.norm(a - b, dim=(1, 2)) / (torch.linalg.norm(b, dim=(1, 2)) + 1e-9)
            e = self.floor * b.amax(dim=(1, 2), keepdim=True) + 1e-9
            total = total + torch.mean(sc) + torch.mean(torch.abs(torch.log(a + e) - torch.log(b + e)))
        return total


def train_capture(x, y, notes, *, loss="note", arch="feather", steps=1500, batch=16, seg=16384, pre=240, lr=4e-3,
                  latent_weight=None, init_state=None, seed=0, log=print, val_fraction=0.15, spectral="auraloss",
                  teacher=None, warm_steps=0, anchor_weight=0.0):
    """Trains a WaveNet that turns x (the take) into y (the target). Returns (model, info).

    loss "note":   per-note MRSTFT (module docstring). Each example is one note: the model's output from
                   `pre` samples before the take's onset, and the target from `pre` before the target's
                   onset, both `seg` long and multiplied by the same mask (1 for pre + L samples, then a 5 ms
                   raised-cosine fade to 0), so only the stretch both performances share counts.
    loss "global": the take as recorded, no per-note alignment: random seg-long windows of x and the same
                   samples of y (a play-along take is lined up as a whole), MRSTFT. What alignment buys.
    loss "esr":    sample-aligned data (x and y the same performance): NAM's ESR, the upper bound, and the
                   distillation step of a warm start.
    latent_weight: if set, one learnable input gain per note (dB), penalized by latent_weight x mean(g^2).
    spectral:      "auraloss" (NAM's bundled auraloss MRSTFT, the default) or "floored" (FlooredMRSTFT; tried on
                   the high-gain case, it wasn't better: feel 3.43 against 2.94).
    teacher:       an output on the take's clock to start from (the current matcher's render of the take):
                   first warm_steps of ESR against it on random windows (distillation, sample-aligned since
                   it's the same performance), then, if anchor_weight > 0, that ESR stays in the loss with
                   that weight, so the model only moves from the matcher's sound where the target says so.
    """
    torch, MRSTFT, _, _, _ = nam_imports()
    dev = torch.device(device_name())
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = new_model(arch)
    if init_state is not None:
        model.load_state_dict(init_state)
    model.to(dev)
    rf = model.receptive_field
    X = torch.tensor(np.concatenate([np.zeros(rf + seg), x, np.zeros(seg)]), dtype=torch.float32, device=dev)
    Y = torch.tensor(np.concatenate([np.zeros(rf + seg), y, np.zeros(seg)]), dtype=torch.float32, device=dev)
    off = rf + seg                                    # where sample 0 of x and y sits in X and Y
    if spectral == "auraloss":
        aura = MRSTFT(fft_sizes=FFT_SIZES, hop_sizes=[n // 4 for n in FFT_SIZES], win_lengths=FFT_SIZES).to(dev)
        mrstft = lambda p, t: aura(p[:, None, :], t[:, None, :])
    else:
        mrstft = FlooredMRSTFT(FFT_SIZES)
    Tch = None
    if teacher is not None:
        Tch = torch.tensor(np.concatenate([np.zeros(rf + seg), teacher, np.zeros(seg)]), dtype=torch.float32, device=dev)
    ar = torch.arange(seg, device=dev)
    arx = torch.arange(seg + rf - 1, device=dev)

    order = rng.permutation(len(notes)) if notes else np.array([], dtype=int)
    n_val = max(1, int(round(val_fraction * len(notes)))) if notes else 0
    val_idx, train_idx = order[:n_val], order[n_val:]
    params = [{"params": model.parameters(), "lr": lr}]
    gains = None
    if latent_weight is not None:
        gains = torch.zeros(len(notes), device=dev, requires_grad=True)
        params.append({"params": [gains], "lr": 0.05})
    opt = torch.optim.Adam(params)
    sched = torch.optim.lr_scheduler.ExponentialLR(opt, gamma=0.1 ** (1.0 / steps))
    fade = int(0.005 * SR)
    lengths = np.array([n["L"] for n in notes]) if notes else np.zeros(0)

    def masks(idx):
        L = torch.tensor(pre + lengths[idx], device=dev, dtype=torch.float32)[:, None]
        u = torch.clamp((L - ar[None, :].float()) / fade, 0.0, 1.0)   # 1 inside, falling to 0 over the fade
        return 0.5 - 0.5 * torch.cos(math.pi * u)

    def note_batch(idx):
        idx = np.asarray(idx)
        xs = torch.tensor([off + notes[i]["o"] - pre - (rf - 1) for i in idx], device=dev)
        ys = torch.tensor([off + notes[i]["t"] - pre for i in idx], device=dev)
        xin = X[xs[:, None] + arx[None, :]]
        if gains is not None:
            xin = xin * torch.pow(10.0, gains[torch.tensor(idx, device=dev)] / 20.0)[:, None]
        return xin, Y[ys[:, None] + ar[None, :]], masks(idx)

    def window_batch(starts):
        s = torch.tensor(starts, device=dev)
        return X[off + s[:, None] - (rf - 1) + arx[None, :]], Y[off + s[:, None] + ar[None, :]], None

    def esr(pred, target):
        return torch.mean(torch.sum((pred - target) ** 2, dim=1) / (torch.sum(target ** 2, dim=1) + 1e-9))

    def loss_of(xin, tgt, m):
        pred = model(xin, pad_start=False)
        if loss == "esr":
            return esr(pred, tgt)
        if m is not None:
            pred, tgt = pred * m, tgt * m
        return mrstft(pred, tgt)

    def teacher_esr(batch_size):
        s0 = torch.tensor(rng.integers(0, len(x) - seg, batch_size), device=dev)
        xin = X[off + s0[:, None] - (rf - 1) + arx[None, :]]
        return esr(model(xin, pad_start=False), Tch[off + s0[:, None] + ar[None, :]])

    if Tch is not None and warm_steps > 0:
        warm = torch.optim.Adam(model.parameters(), lr=lr)
        for _ in range(warm_steps):
            v = teacher_esr(batch)
            warm.zero_grad()
            v.backward()
            warm.step()
        log(f"    distilled the teacher: {warm_steps} steps, ESR {float(v.detach()):.4f}")

    val_starts = None
    if loss in ("global", "esr"):
        span = len(x) - seg
        val_starts = rng.integers(0, span, 24)

    def validate():
        with torch.no_grad():
            if loss == "note":
                return float(loss_of(*note_batch(val_idx)))
            return float(loss_of(*window_batch(val_starts)))

    best, best_state, history = math.inf, None, []
    t0 = time.time()
    for step in range(steps):
        if loss == "note":
            xin, tgt, m = note_batch(rng.choice(train_idx, batch))
        else:
            xin, tgt, m = window_batch(rng.integers(0, len(x) - seg, batch))
        value = loss_of(xin, tgt, m)
        if Tch is not None and anchor_weight > 0:
            value = value + anchor_weight * teacher_esr(batch // 2)
        if gains is not None:
            value = value + latent_weight * torch.mean(gains ** 2)
        opt.zero_grad()
        value.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        sched.step()
        if step % 100 == 99 or step == steps - 1:
            v = validate()
            history.append((step + 1, float(value.detach()), v))
            if v < best:
                best, best_state = v, {k: t.detach().cpu().clone() for k, t in model.state_dict().items()}
    seconds = time.time() - t0
    model.load_state_dict(best_state)
    model.to("cpu").eval()
    info = dict(loss=loss, arch=arch, steps=steps, seconds=round(seconds, 1), best_val=best, history=history,
                device=device_name(), notes=len(notes), receptive_field=rf)
    if gains is not None:
        g = gains.detach().cpu().numpy()
        info["latent_gains_db"] = g.tolist()
        info["latent_train_idx"] = [int(i) for i in train_idx]
    log(f"    trained {arch} with the {loss} loss{' + latent velocity' if gains is not None else ''}: {steps} steps "
        f"in {seconds:.0f} s on {device_name()}, best validation {best:.3f}")
    return model, info


def note_loss(y, target, notes, seg=16384, pre=240, spectral="auraloss"):
    """The per-note MRSTFT loss (train_capture's "note" loss) of an output y on the take's clock against
    the target, over all the notes: what the training minimizes, for any method (the oracle's value is
    the floor two different performances leave)."""
    import torch
    _, MRSTFT, _, _, _ = nam_imports()
    if spectral == "auraloss":
        aura = MRSTFT(fft_sizes=FFT_SIZES, hop_sizes=[n // 4 for n in FFT_SIZES], win_lengths=FFT_SIZES)
        mrstft = lambda p, t: aura(p[:, None, :], t[:, None, :])
    else:
        mrstft = FlooredMRSTFT(FFT_SIZES)
    fade = int(0.005 * SR)
    yp = np.concatenate([y, np.zeros(seg)])
    tp = np.concatenate([target, np.zeros(seg)])
    preds, tgts = [], []
    for n in notes:
        u = np.clip((pre + n["L"] - np.arange(seg)) / fade, 0.0, 1.0)
        m = 0.5 - 0.5 * np.cos(np.pi * u)
        a, b = max(0, n["o"] - pre), max(0, n["t"] - pre)
        preds.append(yp[a:a + seg] * m)
        tgts.append(tp[b:b + seg] * m)
    with torch.no_grad():
        return float(mrstft(torch.tensor(np.array(preds), dtype=torch.float32), torch.tensor(np.array(tgts), dtype=torch.float32)))


def run_model(model, x):
    """The model on a whole signal (CPU, from silence)."""
    import torch
    with torch.no_grad():
        return model(torch.tensor(np.asarray(x), dtype=torch.float32), pad_start=True).numpy().astype(np.float64)


def export_model(model, folder, basename, name, tone_type):
    """Writes <folder>/<basename>.nam with NAM's own exporter (the format NAM core reads)."""
    from nam.models.metadata import GearType, ToneType, UserMetadata
    meta = UserMetadata(name=name, modeled_by="BellyDSP learn_tone prototype", gear_type=GearType("amp_cab"),
                        tone_type=ToneType(tone_type) if tone_type else None)
    model.export(pathlib.Path(folder), basename=basename, user_metadata=meta)
    return pathlib.Path(folder) / f"{basename}.nam"


def check_export(nam_file, model, x):
    """Renders x through the exported file in NAM core (ampsim_render, the chain's amp block, no
    normalization) and compares with PyTorch: the relative error energy (ESR) and the lag."""
    y_core = ampsim_render(x, ["--model", nam_file, "--no-normalize"])
    y_torch = run_model(model, x)
    n = min(len(y_core), len(y_torch))
    return float(np.sum((y_core[:n] - y_torch[:n]) ** 2) / np.sum(y_torch[:n] ** 2))


# =====================================================================================================
# The current matcher, as a method
# =====================================================================================================

def fit_post_eq(target, y):
    """The match EQ (tone_match.fit_match_eq: the post EQ's five parametric bands, +-12 dB) that takes y's
    long-term spectrum to the target's."""
    ta, ya = tm.Analysis(target), tm.Analysis(y)
    eq, _ = tm.fit_match_eq(tm.smooth_bands(ta.ltas - ya.ltas), ta.weights)
    return eq


def apply_post_eq(x, eq):
    """x through the app's post EQ with these bands (ampsim_render with no model: the chain's Equalizer)."""
    return ampsim_render(x, ["--post-eq", tm.eq_arg(eq)])


def use_gain_sets():
    tm.MODEL_FILES = GAIN_SETS


def current_matcher(target, take, mode="same", band_seconds=None):
    """tone_match.py's match (the C++ matcher's golden reference) with the built-in gain sets. Returns
    (result, a function rendering any DI through what Apply would set)."""
    use_gain_sets()
    r = tm.match(target, take, mode, log=lambda *a: None, band_seconds=band_seconds)
    cab = next(c for c in tm.CAB_FILES if c.name == r["cab"])

    def render(x):
        return tm.render(x, r["slot"], r["gain_db"], tone=r["tone_db"], cab=cab, eq=r["eq"])

    return r, render


def matcher_label(r):
    return f"{r['amp']} {r['gain_db']:+.1f}, {r['cab'][:-4]}"


# =====================================================================================================
# Metrics
# =====================================================================================================

# Input peak levels, dBFS: -30 to -6 in 3 dB steps, the range the synthetic takes' notes are played at
# (their peaks run from about -26 to -6 dBFS). Above that a capture learned from a take is extrapolating:
# nothing it learned from was played that hard.
PROBE_LEVELS = np.arange(-30.0, -5.9, 3.0)
PROBE_NOTE_S, PROBE_GAP_S = 1.3, 0.5
PROBE_PITCHES = {"note": [64], "chord": [45, 52, 57]}  # E4 alone; an A power chord (A2 E3 A3)


def probe_signal(kind):
    """The feel probe: the same pluck (one seed, so identical at every level) at each input peak level
    in PROBE_LEVELS, PROBE_GAP_S of silence before each. Returns (signal, onsets)."""
    pieces, onsets, pos = [], [], 0
    for lvl in PROBE_LEVELS:
        rng = np.random.default_rng(7)
        n = int(PROBE_NOTE_S * SR)
        note = sum(tm.pluck(tm.midi_hz(m), n, rng, level=1.0, brightness=0.6) for m in PROBE_PITCHES[kind])
        note = note * 10.0 ** (lvl / 20.0) / np.max(np.abs(note))
        gap = np.zeros(int(PROBE_GAP_S * SR))
        pieces += [gap, note]
        onsets.append(pos + len(gap))
        pos += len(gap) + len(note)
    return np.concatenate(pieces + [np.zeros(int(0.2 * SR))]), np.array(onsets)


def envelope_db(y, start, length, window, hop):
    idx = start + np.arange(0, length - window + 1, hop)
    return np.array([rms_db(y[i:i + window]) for i in idx])


def feel_measures(y, onsets, kind):
    """What the feel metric compares, for one method's output y of a probe:
    level     the output's loudness over each note's first 400 ms, dB (the compression curve against
              PROBE_LEVELS);
    harmonics (single note only) harmonics 2 to 8 relative to the fundamental, dB, from a Hann-windowed
              FFT of 100 to 500 ms after the onset (the steady part; floored at -80 dB);
    attack    the envelope (2 ms RMS, 1 ms hop) over the first 60 ms, dB relative to its own peak over the
              first 300 ms, floored at -40 dB: how the note speaks;
    decay     the envelope (20 ms RMS, 20 ms hop) from 60 ms to 1.2 s, dB relative to its first frame,
              floored at -60 dB: sustain and bloom."""
    out = dict(level=[], harmonics=[], attack=[], decay=[])
    f0 = tm.midi_hz(PROBE_PITCHES["note"][0])
    period = max(2, int(round(SR / f0)))
    f0 = SR / period                                   # the pluck's real pitch: Karplus-Strong's period is whole
    for o in onsets:
        out["level"].append(rms_db(y[o:o + int(0.4 * SR)]))
        peak = max(rms_db(y[i:i + 96]) for i in range(o, o + int(0.3 * SR), 48))
        att = envelope_db(y, o, int(0.06 * SR), 96, 48) - peak
        out["attack"].append(np.maximum(att, -40.0))
        dec = envelope_db(y, o + int(0.06 * SR), int(1.14 * SR), 960, 960)
        out["decay"].append(np.maximum(dec - dec[0], -60.0))
        if kind == "note":
            seg = y[o + int(0.1 * SR): o + int(0.5 * SR)]
            spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n=1 << 16))
            f = np.fft.rfftfreq(1 << 16, 1.0 / SR)

            def level(h):
                lo, hi = np.searchsorted(f, h * f0 * 0.98), np.searchsorted(f, h * f0 * 1.02)
                return 20.0 * np.log10(np.max(spec[lo:hi]) + 1e-12)

            h1 = level(1)
            out["harmonics"].append(np.maximum([level(h) - h1 for h in range(2, 9)], -80.0))
    return {k: np.array(v) for k, v in out.items()}


def feel_distance(m, ref):
    """The feel metric, method against the hidden rig, one number per facet (dB) and their mean:
    compression  RMS over the input levels of (level_m - level_ref) with its mean removed: the shape of the
                 output-level-against-input-level curve, not the volume;
    harmonics    RMS over levels and harmonics 2 to 8 of the difference in harmonic levels (relative to the
                 fundamental): how much and which distortion, at each picking strength;
    attack       RMS of the attack envelopes' difference, over levels and the first 60 ms;
    decay        RMS of the decay envelopes' difference, over levels and 60 ms to 1.2 s.
    Each facet is averaged over the probes that have it (the note and the chord)."""
    facets = {}
    for f in ("compression", "harmonics", "attack", "decay"):
        vals = []
        for kind in m:
            a, b = m[kind], ref[kind]
            if f == "compression":
                d = a["level"] - b["level"]
                vals.append(float(np.sqrt(np.mean((d - d.mean()) ** 2))))
            elif f == "harmonics" and len(a["harmonics"]):
                vals.append(float(np.sqrt(np.mean((a["harmonics"] - b["harmonics"]) ** 2))))
            elif f in ("attack", "decay"):
                vals.append(float(np.sqrt(np.mean((a[f] - b[f]) ** 2))))
        facets[f] = float(np.mean(vals))
    facets["feel"] = float(np.mean([facets[k] for k in ("compression", "harmonics", "attack", "decay")]))
    return facets


def compression_slope(meas):
    """dB of output per dB of input over the probe's levels (least squares), the note's."""
    return float(np.polyfit(PROBE_LEVELS, meas["note"]["level"], 1)[0])


def probe_all(fn):
    out = {}
    for kind in PROBE_PITCHES:
        x, onsets = probe_signal(kind)
        out[kind] = feel_measures(fn(x), onsets, kind)
    return out


def spectral_distance(ref, y):
    """tone_match's long-term spectral error (anything mode): the weighted RMS of the third-octave smoothed
    difference of the sixth-octave long-term spectra, mean removed, weights from the reference."""
    ra, ya = tm.Analysis(ref), tm.Analysis(y)
    return tm.weighted_rms_centred(tm.smooth_bands(ra.ltas - ya.ltas), ra.weights), tm.nonlinear_distance(ra.features(), ya.features())


NOTE_BANDS = tm.COARSE_CENTRES


def note_measures(y, starts, length=int(0.15 * SR)):
    """Per note: the level over its first 150 ms (dB) and its sixth-octave spectrum (dB, 2048-point frames)."""
    levels, spectra = [], []
    win = np.hanning(2048)
    f = np.fft.rfftfreq(2048, 1.0 / SR)
    edges = [(c * 2 ** (-1 / 12), c * 2 ** (1 / 12)) for c in NOTE_BANDS]
    for s in starts:
        seg = y[s:s + length]
        levels.append(rms_db(seg))
        frames = np.lib.stride_tricks.sliding_window_view(np.concatenate([seg, np.zeros(2048)]), 2048)[::512][: max(1, len(seg) // 512)]
        p = np.mean(np.abs(np.fft.rfft(frames * win, axis=1)) ** 2, axis=0)
        spectra.append([db(np.mean(p[(f >= lo) & (f < hi)]) if np.any((f >= lo) & (f < hi)) else p[np.argmin(abs(f - (lo + hi) / 2))]) for lo, hi in edges])
    return np.array(levels), np.array(spectra)


def take_feel(take, target, notes, outputs):
    """What can be measured on a real take, where no rig is known: per note, the take's level (its DI),
    the target's, and each method's output on the take (outputs: name -> y on the take's clock).
      slope        the per-note compression: d(output level) / d(DI level), least squares over the notes;
      note level   RMS over notes of (method's level - target's level), mean removed, dB;
      note spectrum the mean over notes of the weighted, mean-removed RMS difference of the note's
                   sixth-octave spectrum against the target's (tone_match's weights), dB.
    The target's slope is biased flat: its levels follow the original guitarist's picking, not the
    take's (regression dilution), so a method shouldn't be expected to reach it."""
    o = [n["o"] for n in notes]
    t = [n["t"] for n in notes]
    x_lvl, _ = note_measures(take, o)
    t_lvl, t_spec = note_measures(target, t)
    res = {"target": dict(slope=float(np.polyfit(x_lvl, t_lvl, 1)[0]))}
    w = tm.WEIGHTS
    for name, y in outputs.items():
        y_lvl, y_spec = note_measures(y, o)
        d = y_lvl - t_lvl
        spec_err = [tm.weighted_rms_centred(tm.smooth_bands(a - b), w) for a, b in zip(t_spec, y_spec)]
        res[name] = dict(slope=float(np.polyfit(x_lvl, y_lvl, 1)[0]), note_level=float(np.sqrt(np.mean((d - d.mean()) ** 2))),
                         note_spectrum=float(np.mean(spec_err)))
    return res


# =====================================================================================================
# The synthetic study
# =====================================================================================================

def case_signals(name, seed):
    """The target's guitarist (A), the player's take (B: same notes, own timing, velocities, and picks, 1%
    slower), and the held-out DI (C: other melodies, a third performance)."""
    jitter = RIGS[name]["jitter_ms"]
    a, a_on, a_v = perform(TRAIN_SCORE, seed)
    b, b_on, b_v = perform(TRAIN_SCORE, seed + 1000, tempo=0.99, jitter_ms=jitter, contour_scale=0.8, velocity_sd_db=2.5,
                           brightness_shift=-0.08)
    c, _, _ = perform(HELD_OUT_SCORE, seed + 2000, jitter_ms=8.0)
    return dict(a=a, a_on=a_on, a_v=a_v, b=b, b_on=b_on, b_v=b_v, c=c)


def onset_report(detected, truth, tol=480):
    """Precision and recall of detected onsets within 10 ms of the truth, and the median error (ms)."""
    hits = [np.min(np.abs(truth - d)) for d in detected]
    good = [h for h in hits if h <= tol]
    recall = sum(np.min(np.abs(detected - t)) <= tol for t in truth) / len(truth)
    return dict(precision=len(good) / max(1, len(detected)), recall=recall, median_ms=float(np.median(good)) * 1000 / SR if good else None)


def target_alignment_error(notes, sig):
    """The target onset found minus the original guitarist's true onset of the same note, ms: the median
    (a constant part is the rig's own delay, the cab IR's first arrival, which the model learns), and the
    median and 90th percentile of the distance from that median (the alignment's real error)."""
    errs = []
    for n in notes:
        k = int(np.argmin(np.abs(sig["b_on"] - n["o"])))
        if abs(sig["b_on"][k] - n["o"]) <= 480:
            errs.append((n["t"] - sig["a_on"][k]) * 1000 / SR)
    errs = np.array(errs)
    spread = np.abs(errs - np.median(errs))
    return float(np.median(errs)), float(np.median(spread)), float(np.percentile(spread, 90))


def evaluate(fn, sig, rig_out_c, rig_probe):
    """A method (fn: DI -> output) on the held-out DI and the probe, against the hidden rig."""
    yc = fn(sig["c"])
    spec, dist = spectral_distance(rig_out_c, yc)
    probe = probe_all(fn)
    feel = feel_distance(probe, rig_probe)
    return dict(spectral=spec, distortion=dist, slope=compression_slope(probe), **feel)


def run_case(name, args, log=print):
    seed = 300 + sorted(RIGS).index(name)
    sig = case_signals(name, seed)
    tone_type = RIGS[name]["tone_type"]
    dry_a = rig_dry(name, sig["a"])
    record = rig_fx(name, dry_a, seed)
    mix = mix_song(record, seed)
    stem = separate(mix) if "separated" in args.targets or "masked" in args.targets else None
    rig_c = rig_dry(name, sig["c"])
    rig_probe = probe_all(lambda x: rig_dry(name, x))
    oracle_b = rig_dry(name, sig["b"])
    row = dict(case=name, tone_type=tone_type)

    # The take's notes (found, not given), checked against the truth.
    onsets = detect_onsets(sig["b"])
    row["onsets"] = onset_report(onsets, sig["b_on"])
    log(f"  {name}: {len(onsets)} onsets in the take (truth {len(sig['b_on'])}), precision {row['onsets']['precision']:.2f} "
        f"recall {row['onsets']['recall']:.2f}, median {row['onsets']['median_ms']:.2f} ms")

    targets = {"stem": record}
    if stem is not None:
        targets["separated"] = stem
    row["targets"] = {}
    for tname in args.targets:
        if tname == "masked":
            notes_sep = align_notes(sig["b"], stem, onsets=onsets)
            targets["masked"] = informed_mask(stem, notes_sep)
        if tname == "mix_masked":
            notes_mix = align_notes(sig["b"], mix, onsets=onsets)
            targets["mix_masked"] = informed_mask(mix, notes_mix)
        if tname == "mix":
            targets["mix"] = mix
    # Separation quality against the unmixed record.
    row["separation"] = {k: dict(si_sdr=si_sdr(record, v), spectral=spectral_distance(record, v)[0])
                         for k, v in targets.items() if k != "stem"}
    for k, v in row["separation"].items():
        log(f"    separation {k}: SI-SDR {v['si_sdr']:.1f} dB, spectral distance to the record {v['spectral']:.2f} dB")

    oracle = dict(spectral=0.0, distortion=0.0, slope=compression_slope(rig_probe), compression=0.0, harmonics=0.0,
                  attack=0.0, decay=0.0, feel=0.0)
    for tname in args.targets:
        target = targets[tname]
        scale = 0.1 / math.sqrt(np.mean(target ** 2))
        target = target * scale
        t0 = time.time()
        notes = align_notes(sig["b"], target, onsets=onsets)
        align_err = target_alignment_error(notes, sig)
        log(f"    [{tname}] {len(notes)} notes aligned in {time.time() - t0:.1f} s; target onsets off the truth by "
            f"{align_err[0]:+.1f} ms (the rig's delay), spread {align_err[1]:.1f} ms median, {align_err[2]:.1f} ms 90th percentile")
        res = dict(notes=len(notes), alignment_ms=align_err, methods={"oracle": oracle})

        t0 = time.time()
        r, mrender = current_matcher(target, sig["b"], "same")
        res["matcher_seconds"] = round(time.time() - t0, 1)
        res["matcher"] = matcher_label(r)
        res["methods"]["matcher"] = evaluate(mrender, sig, rig_c, rig_probe)
        log(f"    [{tname}] matcher: {res['matcher']} in {res['matcher_seconds']} s: " + fmt_metrics(res["methods"]["matcher"]))
        if tname != "stem":
            r2, mrender2 = current_matcher(target, sig["b"], "anything")
            res["matcher_anything"] = matcher_label(r2)
            res["methods"]["matcher (anything)"] = evaluate(mrender2, sig, rig_c, rig_probe)
            log(f"    [{tname}] matcher, anything: {res['matcher_anything']}: " + fmt_metrics(res["methods"]["matcher (anything)"]))

        variants = [("learned", dict(loss="note"))]
        if tname == "stem" and not args.quick:
            variants += [("learned, latent", dict(loss="note", latent_weight=0.02)),
                         ("learned, no alignment", dict(loss="global")),
                         ("learned, same performance (ESR)", dict(loss="esr", paired=True))]
        res["training"] = {}
        outs_b = {"matcher": mrender(sig["b"])}
        for vname, kw in variants:
            kw = dict(kw)
            paired = kw.pop("paired", False)
            if paired:
                model, info = train_capture(sig["a"], dry_a * scale, [], steps=args.paired_steps, arch=args.arch, **kw, log=log)
            else:
                model, info = train_capture(sig["b"], target, notes, steps=args.steps, arch=args.arch, **kw, log=log)
            fn = lambda x, m=model: run_model(m, x)
            res["methods"][vname] = evaluate(fn, sig, rig_c, rig_probe)
            if "latent_gains_db" in info:
                # The latent gains against the truth: the take's velocity minus the original's, per note.
                g = np.array(info["latent_gains_db"])
                true_d = []
                for n in notes:
                    k = int(np.argmin(np.abs(sig["b_on"] - n["o"])))
                    true_d.append(sig["a_v"][k] - sig["b_v"][k])
                info["latent_corr"] = float(np.corrcoef(g, true_d)[0, 1])
                log(f"    latent gains vs the true velocity differences: correlation {info['latent_corr']:.2f}, "
                    f"spread {np.std(g):.2f} dB vs {np.std(true_d):.2f} dB")
                del info["latent_gains_db"], info["latent_train_idx"]
            res["training"][vname] = info
            log(f"    [{tname}] {vname}: " + fmt_metrics(res["methods"][vname]))
            if vname == "learned":
                outs_b[vname] = fn(sig["b"])
                # The learned capture followed by the app's own match EQ, fitted as the matcher fits it (the
                # post EQ's five bands, to the long-term spectra of the target and the capture on the take).
                eq = fit_post_eq(target, outs_b[vname])
                res["learned_eq"] = tm.fmt_eq(eq)
                fn_eq = lambda x, m=model, e=eq: apply_post_eq(run_model(m, x), e)
                res["methods"]["learned + match EQ"] = evaluate(fn_eq, sig, rig_c, rig_probe)
                outs_b["learned + match EQ"] = fn_eq(sig["b"])
                log(f"    [{tname}] learned + match EQ ({res['learned_eq']}): " + fmt_metrics(res["methods"]["learned + match EQ"]))
                if tname == "stem" and not args.skip_export:
                    folder = CACHE / "models"
                    folder.mkdir(parents=True, exist_ok=True)
                    nam = export_model(model, folder, f"{name}_{tname}", f"Learned {name}", tone_type)
                    res["export_esr"] = check_export(nam, model, sig["c"])
                    log(f"    exported {nam.name}: NAM core vs PyTorch on the held-out DI, ESR {res['export_esr']:.2e}")
        outs_b["oracle"] = oracle_b
        tf = take_feel(sig["b"], target, notes, outs_b)
        res["take"] = tf
        res["take_spectral"] = {k: spectral_distance(target, v)[0] for k, v in outs_b.items()}
        row["targets"][tname] = res
    return row


def fmt_metrics(m):
    return (f"spectral {m['spectral']:.2f} dB, feel {m['feel']:.2f} (compression {m['compression']:.2f}, harmonics "
            f"{m['harmonics']:.2f}, attack {m['attack']:.2f}, decay {m['decay']:.2f}), slope {m['slope']:.2f}")


def print_table(rows):
    print("\n| Case | Target | Method | Spectral (dB) | Compression | Harmonics | Attack | Decay | Feel | Slope (dB/dB) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for row in rows:
        for tname, res in row["targets"].items():
            for mname, m in res["methods"].items():
                label = mname if mname != "matcher" else f"matcher ({res['matcher']})"
                print(f"| {row['case']} | {tname} | {label} | {m['spectral']:.2f} | {m['compression']:.2f} | {m['harmonics']:.2f} | "
                      f"{m['attack']:.2f} | {m['decay']:.2f} | {m['feel']:.2f} | {m['slope']:.2f} |")
    print("\nGate (Sean's brief): the learned capture beats the current matcher clearly on feel AND isn't worse on")
    print("spectral distance, in most cases. Clearly: feel at least 20% lower. Not worse: spectral at most 0.25 dB")
    print("higher. Against the matcher's better mode on that target (lower feel; Same part, and Anything on a stem).")
    for lname in ("learned", "learned + match EQ"):
        wins = total = 0
        print(f"  {lname}:")
        for row in rows:
            for tname, res in row["targets"].items():
                ms = [(k, v) for k, v in res["methods"].items() if k.startswith("matcher")]
                mk, m = min(ms, key=lambda kv: kv[1]["feel"])
                l = res["methods"][lname]
                ok = l["feel"] <= 0.8 * m["feel"] and l["spectral"] <= m["spectral"] + 0.25
                wins += ok
                total += 1
                print(f"    {row['case']:13s} {tname:10s} feel {l['feel']:.2f} vs {m['feel']:.2f} ({mk}), spectral {l['spectral']:.2f} vs "
                      f"{m['spectral']:.2f}: {'pass' if ok else 'FAIL'}")
        print(f"    {wins}/{total} pass")


def report(args):
    print_table(json.loads(pathlib.Path(args.json).read_text()))


def study(args):
    rows = []
    cases = args.cases.split(",") if args.cases else list(RIGS)
    for name in cases:
        rows.append(run_case(name, args))
        if args.json:
            pathlib.Path(args.json).write_text(json.dumps(rows, indent=1, default=float))
    print_table(rows)


# =====================================================================================================
# A real take (saved by the app's "Save take")
# =====================================================================================================

def read_mono(path):
    return tm.to_mono_48k(path)


def take_folder(args):
    """Trains a capture from a saved take and compares it with the current matcher on the same take.
    The folder (docs/TONE_MATCH.md, "Save take"): target.wav (the selected section, 48 kHz), stem.wav (its
    separated guitar, if a separated match made one), di_raw.wav (the take from the song's first sample),
    di.wav (lined up with the section: di_raw from alignSamples on), take.json."""
    folder = pathlib.Path(args.folder)
    info = json.loads((folder / "take.json").read_text())
    stem_path = folder / "stem.wav"
    target = read_mono(stem_path) if stem_path.exists() and not args.full_song else read_mono(folder / "target.wav")
    take = read_mono(folder / "di.wav")
    n = min(len(target), len(take))
    target, take = target[:n], take[:n]
    print(f"{folder.name}: target {'stem' if stem_path.exists() and not args.full_song else 'section'} {n / SR:.1f} s, "
          f"take lined up by {info.get('align_samples', 0)} samples ({info.get('latency_ms', 0):.1f} ms)")
    if args.mask:
        target = informed_mask(target, align_notes(take, target, band_seconds=tm.PLAY_ALONG_BAND_SECONDS))
        print("  informed mask applied to the target")
    scale = 0.1 / math.sqrt(np.mean(target ** 2))
    target = target * scale
    notes = align_notes(take, target, band_seconds=tm.PLAY_ALONG_BAND_SECONDS)
    print(f"  {len(notes)} notes aligned")
    t0 = time.time()
    r, mrender = current_matcher(target, take, "same", band_seconds=tm.PLAY_ALONG_BAND_SECONDS)
    print(f"  current matcher ({time.time() - t0:.0f} s): {matcher_label(r)}, closeness {r['closeness']:.0f}")
    model, tinfo = train_capture(take, target, notes, steps=args.steps, latent_weight=args.latent)
    nam = export_model(model, folder, "learned", f"Learned from {folder.name}", None)
    outs = {"matcher": mrender(take), "learned": run_model(model, take)}
    eq = fit_post_eq(target, outs["learned"])
    outs["learned + match EQ"] = apply_post_eq(outs["learned"], eq)
    tm.write_wav(folder / "learned_take.wav", outs["learned"] / (np.max(np.abs(outs["learned"])) + 1e-9) * 0.5)
    tm.write_wav(folder / "matcher_take.wav", outs["matcher"] / (np.max(np.abs(outs["matcher"])) + 1e-9) * 0.5)
    spec = {k: spectral_distance(target, v)[0] for k, v in outs.items()}
    tf = take_feel(take, target, notes, outs)
    result = dict(notes=len(notes), matcher=matcher_label(r), matcher_closeness=r["closeness"], training=tinfo,
                  learned_eq=[[k, f, g, q] for k, f, g, q in eq],
                  spectral_vs_target=spec, take_feel=tf, nam=str(nam), export_esr=check_export(nam, model, take[: 10 * SR]))
    (folder / "learn_tone_results.json").write_text(json.dumps(result, indent=1, default=float))
    print(f"  wrote {nam.name} (NAM core vs PyTorch ESR {result['export_esr']:.1e}), learned_take.wav, matcher_take.wav")
    print("\n| Method | Spectral vs target (dB) | Per-note level (dB) | Per-note spectrum (dB) | Compression slope (target "
          f"{tf['target']['slope']:.2f}) |")
    print("|---|---|---|---|---|")
    for k in outs:
        print(f"| {k} | {spec[k]:.2f} | {tf[k]['note_level']:.2f} | {tf[k]['note_spectrum']:.2f} | {tf[k]['slope']:.2f} |")
    print(f"\nThe match EQ for the learned capture (the post EQ, parametric): {tm.fmt_eq(eq)}")
    print("The learned capture includes the cab (gear type amp_cab): load it in an amp slot with the Cab off, and the EQ "
          "above as the post EQ for the last row. How it sounds is for you to judge.")


def probe_files(args):
    """The feel probe on captures: each .nam rendered through NAM core, facets against the first."""
    outs = [probe_all(lambda x, f=f: ampsim_render(x, ["--model", f])) for f in args.files]
    for f, o in zip(args.files[1:], outs[1:]):
        print(f, feel_distance(o, outs[0]), "slope", compression_slope(o))


def main():
    if len(sys.argv) >= 2 and sys.argv[1] not in ("study", "probe", "report", "-h", "--help") and pathlib.Path(sys.argv[1]).is_dir():
        sys.argv.insert(1, "take")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("study", help="the synthetic ground-truth study")
    st.add_argument("--cases", help="comma-separated: " + ",".join(RIGS))
    st.add_argument("--targets", default="stem,separated,masked", help="stem, separated, masked, mix_masked, mix")
    st.add_argument("--quick", action="store_true", help="the main learned variant only")
    st.add_argument("--steps", type=int, default=3000, help="training steps for the take-trained captures")
    st.add_argument("--paired-steps", type=int, default=6000, help="training steps for the same-performance upper bound")
    st.add_argument("--arch", default="feather", choices=["feather", "lite", "standard"])
    st.add_argument("--skip-export", action="store_true")
    st.add_argument("--json")
    tk = sub.add_parser("take", help="a take folder saved by the app")
    tk.add_argument("folder")
    tk.add_argument("--steps", type=int, default=3000)
    tk.add_argument("--full-song", action="store_true", help="train on the section even if a stem exists")
    tk.add_argument("--mask", action="store_true", help="apply the informed harmonic mask to the target first")
    tk.add_argument("--latent", type=float, default=None, help="latent velocity weight (e.g. 0.02)")
    rp = sub.add_parser("report", help="the table and the gate from a study's --json")
    rp.add_argument("json")
    pr = sub.add_parser("probe", help="the feel probe on .nam files")
    pr.add_argument("files", nargs="+")
    args = ap.parse_args()
    if args.cmd == "study":
        args.targets = args.targets.split(",")
        study(args)
    elif args.cmd == "take":
        take_folder(args)
    elif args.cmd == "report":
        report(args)
    else:
        probe_files(args)


if __name__ == "__main__":
    main()

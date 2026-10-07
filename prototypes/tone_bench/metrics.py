# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""The benchmark's metrics: how far a method's rig is from the hidden rig, facet by facet.

Every method is judged by rendering ONE evaluation signal (eval_signal) through its rig and through the
hidden rig, so the two outputs are sample-aligned and every facet compares like with like. The signal:

    C (the held-out DI, another score in the case's style, the player's guitar)
    the feel probes: one pluck of E4 and an A power chord at input peaks -30 to -6 dBFS, 6 dB apart (learn_tone.py)
    sine steps: 110 Hz at -30, -20, -12 dBFS peak
    two-tone steps: 110 + 165 Hz (a power chord's root and fifth) at -24 and -14 dBFS peak
    staccato notes, each followed by 1.2 s of silence (the time effects' tails)

Facets (all in dB, 0 for the hidden rig itself):

  lt_erb     The loudness-weighted long-term spectral distance on C. Spectra on the ERB-number scale
             (Glasberg and Moore 1990: E(f) = 21.4 log10(1 + 0.00437 f), one band per ERB from 50 Hz to
             15 kHz, 39 bands), the power mean over C's playing frames (4096-point Hann frames, hop 1024; a
             frame plays if the reference's level is within 30 dB of its 95th percentile). With
             d_b = L_ref,b - L_m,b (dB), the distance is
                 D = sqrt( sum_b w_b (d_b - dbar)^2 ),  dbar = sum_b w_b d_b,  sum_b w_b = 1,
             the level removed (dbar), each band weighted by its share of the reference's specific loudness:
             w_b proportional to N'_b = max(0, 10^(0.023 (L_b + A_b + P - T_b)) - 1), Zwicker's power law
             (loudness grows as intensity^0.23) on the excitation after the outer and middle ear (A_b, the
             PEAQ weighting of ITU-R BS.1387: -2.184 f^-0.8 + 6.5 exp(-0.6 (f - 3.3)^2) - 0.001 f^3.6, f in
             kHz) above the threshold in quiet (T_b, Terhardt 1979: 3.64 f^-0.8 - 6.5 exp(-0.6 (f - 3.3)^2)
             + 0.001 f^4), at a playback level P that puts the reference's loudest band at 85 dB SPL. So a
             band counts by how loud it is, the treble region the ear is most sensitive to counts more, and
             what lies under the threshold doesn't count at all.
  note_erb   The same distance per note of C (each note's first 300 ms, or up to the next onset), then the
             mean over notes weighted by each note's loudness (its reference energy ^0.3): attack and
             per-note timbre, where a long-term average can hide differences that cancel.
  feel       learn_tone.feel_distance on the probes: the mean of compression (the output-level curve against
             the input level, mean removed), harmonics (2 to 8 against the fundamental, at every level),
             attack (the first 60 ms), and decay (60 ms to 1.2 s). Each facet is also reported alone.
  harm       The harmonic distribution: on the sine steps, the levels of harmonics 2 to 10 relative to the
             fundamental (dB, floored at -80), the RMS difference over harmonics and levels.
  imd        Intermodulation on the two-tone steps (a power chord): every product of 110 and 165 Hz lies on
             a multiple of 55 Hz; those that are multiples of neither (k 55 Hz, k not divisible by 2 or 3)
             are intermodulation. The IM share of the output power (dB) per level, the RMS difference.
  crest      The crest factor distribution on C: per 2048-sample playing frame, peak over RMS (dB); the mean
             absolute difference of its 10th, 50th, and 90th percentiles.
  mcd        Mel-cepstral distortion on C (Kubichek 1993): 40 mel bands (50 Hz to 16 kHz) of log power,
             each signal floored 60 dB under its loudest band, the DCT, coefficients 1 to 13 (0, the level, left out), (10 / ln 10) sqrt(2 sum_k (c_k -
             c'_k)^2) per playing frame, the mean. A perceptual proxy for checking the others, not in the
             combined score: no learned audio embedding was used (none with a clear licence installs
             without a large download; docs/TONE_MATCH.md, "tone_bench").
  legacy     tone_match's own long-term spectral error on C (the matcher's sixth-octave, A-weighted
             measure; learn_tone.spectral_distance), for comparison with the earlier studies' numbers.
  time_fx    The time-effect mismatch, against the hidden rig WITH its time effect (the others are against
             its tone without): after each staccato note, the output energy in ten 100 ms windows from 50
             ms to 1.05 s, relative to the note's own energy (floored at -50 dB), the RMS difference. A
             reverb's decay and a delay's echoes both show in it. Reported apart from the combined score.

The combined score is a weighted mean of the facets, each divided by its scale (SCALES, the median of
that facet for the deliberately wrong rig over the DEV cases: a facet's value 1.0 is "as far off as a
wrong rig typically is"), so 0 is the hidden rig and about 1 a wrong rig. Weights (WEIGHTS): the spectrum
0.5 (long-term 0.35, per note 0.15), the feel 0.25, the distortion character 0.25 (harmonics 0.1, IMD
0.05, crest 0.1). The spectral envelope is the first dimension of timbre in every multidimensional
scaling study of instrument timbre (Grey 1977; McAdams et al. 1995), so it gets half; feel (dynamics,
attack, sustain) is what Sean said was missing, so a quarter; the rest goes to the distortion facets the
spectra can't see.
"""

import math

import numpy as np
import scipy.fft as sfft

from common import SR, tm

import learn_tone as lt  # noqa: E402  the feel metric (its probes and distances)

# The feel probes at -30 to -6 dBFS in 6 dB steps (learn_tone's are 3 dB apart): half the length, the same
# range; the compression curve's shape over five levels is still well defined.
lt.PROBE_LEVELS = np.arange(-30.0, -5.9, 6.0)

# ---- The evaluation signal ---------------------------------------------------------------------------

GAP = int(0.6 * SR)


def _fade(x, ms=10.0):
    n = int(ms * SR / 1000.0)
    x = x.copy()
    x[:n] *= np.linspace(0, 1, n)
    x[-n:] *= np.linspace(1, 0, n)
    return x


def eval_signal(c, c_onsets):
    """The evaluation signal (module docstring) and where everything in it is."""
    parts, layout, pos = [], {}, 0

    def add(name, x, extra=None):
        nonlocal pos
        layout[name] = dict(start=pos, length=len(x), **(extra or {}))
        parts.append(x)
        pos += len(x)
        parts.append(np.zeros(GAP))
        pos += GAP

    add("c", np.asarray(c, dtype=np.float64), dict(onsets=[int(o) for o in c_onsets]))
    for kind in lt.PROBE_PITCHES:
        x, onsets = lt.probe_signal(kind)
        add("probe_" + kind, x, dict(onsets=[int(o) for o in onsets]))
    t = np.arange(int(0.6 * SR)) / SR
    sines = []
    for lvl in SINE_LEVELS:
        sines.append(_fade(10 ** (lvl / 20) * np.sin(2 * np.pi * 110.0 * t)))
        sines.append(np.zeros(int(0.3 * SR)))
    add("sines", np.concatenate(sines))
    tones = []
    for lvl in TWO_TONE_LEVELS:
        x = np.sin(2 * np.pi * 110.0 * t) + np.sin(2 * np.pi * 165.0 * t + 0.3)
        tones.append(_fade(10 ** (lvl / 20) * x / np.max(np.abs(x))))
        tones.append(np.zeros(int(0.3 * SR)))
    add("two_tone", np.concatenate(tones))
    stac, on = [], []
    rng = np.random.default_rng(5)
    for m in STACCATO:
        n = int(0.25 * SR)
        note = sum(tm.pluck(tm.midi_hz(p), n, rng, level=1.0, brightness=0.6) for p in m)
        note = 10 ** (-10 / 20) * note / np.max(np.abs(note))
        on.append(sum(len(s) for s in stac))
        stac += [note, np.zeros(int(1.2 * SR))]
    add("staccato", np.concatenate(stac), dict(onsets=on, note_len=int(0.25 * SR)))
    return np.concatenate(parts), layout


SINE_LEVELS = [-30.0, -20.0, -12.0]
TWO_TONE_LEVELS = [-24.0, -14.0]
STACCATO = [[52], [57], [40, 47, 52], [59]]


def seg(y, layout, name):
    s = layout[name]
    return y[s["start"]: s["start"] + s["length"]]


# ---- Psychoacoustic spectra ----------------------------------------------------------------------------

N_FFT, HOP = 4096, 1024
FREQS = np.fft.rfftfreq(N_FFT, 1.0 / SR)
WINDOW = np.hanning(N_FFT)


def erb_number(f):
    return 21.4 * np.log10(1.0 + 0.00437 * np.asarray(f, dtype=float))


def erb_hz(e):
    return (10 ** (np.asarray(e) / 21.4) - 1.0) / 0.00437


ERB_LO, ERB_HI = erb_number(50.0), erb_number(15000.0)
ERB_CENTRES = erb_hz(np.arange(ERB_LO, ERB_HI + 1e-9, 1.0))


def _band_matrix(centres):
    e = erb_number(FREQS)
    ec = erb_number(centres)
    m = np.zeros((len(FREQS), len(centres)))
    for b, c in enumerate(ec):
        idx = np.nonzero(np.abs(e - c) <= 0.5)[0]
        if len(idx) == 0:
            idx = [int(np.argmin(np.abs(e - c)))]
        m[idx, b] = 1.0 / len(idx)
    return m


ERB_MATRIX = _band_matrix(ERB_CENTRES)
_fk = ERB_CENTRES / 1000.0
OUTER_EAR_DB = -2.184 * _fk ** -0.8 + 6.5 * np.exp(-0.6 * (_fk - 3.3) ** 2) - 0.001 * _fk ** 3.6
THRESHOLD_DB = 3.64 * _fk ** -0.8 - 6.5 * np.exp(-0.6 * (_fk - 3.3) ** 2) + 0.001 * _fk ** 4


def frames(x, n=N_FFT, hop=HOP):
    x = np.asarray(x, dtype=np.float64)
    if len(x) < n:
        x = np.concatenate([x, np.zeros(n - len(x))])
    k = 1 + (len(x) - n) // hop
    return np.lib.stride_tricks.as_strided(x, shape=(k, n), strides=(x.strides[0] * hop, x.strides[0]))


def power_frames(x):
    spec = sfft.rfft(frames(x) * WINDOW, axis=1)
    return spec.real ** 2 + spec.imag ** 2


def loudness_weights(band_db):
    """w_b (sum 1) from the reference's band levels (module docstring, lt_erb)."""
    p = 85.0 - np.max(band_db + OUTER_EAR_DB)
    excess = band_db + OUTER_EAR_DB + p - THRESHOLD_DB
    n = np.maximum(0.0, 10 ** (0.023 * excess) - 1.0)
    return n / max(np.sum(n), 1e-12)


def weighted_distance(ref_db, m_db, w):
    d = ref_db - m_db
    dbar = float(np.sum(w * d))
    return float(math.sqrt(np.sum(w * (d - dbar) ** 2)))


def active_mask(level_db, range_db=30.0):
    return level_db > np.percentile(level_db, 95) - range_db


def erb_ltas(p, active):
    return 10 * np.log10(np.maximum(p[active].mean(axis=0) @ ERB_MATRIX, 1e-20))


# ---- Facets ------------------------------------------------------------------------------------------

def spectral_facets(ref, y, onsets):
    pr, pm = power_frames(ref), power_frames(y)
    lvl = 10 * np.log10(np.maximum(pr.sum(axis=1), 1e-20))
    act = active_mask(lvl)
    lr, lm = erb_ltas(pr, act), erb_ltas(pm, act)
    lt_erb = weighted_distance(lr, lm, loudness_weights(lr))

    dists, weights = [], []
    for k, o in enumerate(onsets):
        end = min(o + int(0.3 * SR), onsets[k + 1] if k + 1 < len(onsets) else len(ref))
        if end - o < 2048:
            continue
        a, b = ref[o:end], y[o:end]
        fa = np.abs(sfft.rfft(frames(a, 2048, 512) * np.hanning(2048), axis=1)) ** 2
        fb = np.abs(sfft.rfft(frames(b, 2048, 512) * np.hanning(2048), axis=1)) ** 2
        m = _NOTE_MATRIX
        ra = 10 * np.log10(np.maximum(fa.mean(axis=0) @ m, 1e-20))
        rb = 10 * np.log10(np.maximum(fb.mean(axis=0) @ m, 1e-20))
        dists.append(weighted_distance(ra, rb, loudness_weights(ra)))
        weights.append(np.sum(a ** 2) ** 0.3)
    note_erb = float(np.average(dists, weights=weights)) if dists else float("nan")

    # MCD on the playing frames.
    # Each signal's mel energies floored 60 dB under its own loudest band, so the empty top octave above a
    # cab's roll-off (100 dB down) doesn't dominate.
    er, em = pr @ MEL, pm @ MEL
    mel_r, mel_m = np.log(np.maximum(er, 1e-6 * er.max())), np.log(np.maximum(em, 1e-6 * em.max()))
    cr = sfft.dct(mel_r, type=2, norm="ortho", axis=1)[:, 1:14]
    cm = sfft.dct(mel_m, type=2, norm="ortho", axis=1)[:, 1:14]
    # log is natural here; in dB units the cepstra are (10 / ln 10) times these.
    mcd = float(np.mean((10.0 / math.log(10.0)) * np.sqrt(2.0 * np.sum((cr[act] - cm[act]) ** 2, axis=1))))

    # Crest factor distribution on 2048-sample frames.
    fr, fm = frames(ref, 2048, 1024), frames(y, 2048, 1024)
    rr = np.sqrt(np.mean(fr ** 2, axis=1))
    a2 = 20 * np.log10(np.maximum(rr, 1e-12))
    act2 = active_mask(a2)
    cref = 20 * np.log10(np.max(np.abs(fr[act2]), axis=1) / np.maximum(rr[act2], 1e-12))
    cmet = 20 * np.log10(np.max(np.abs(fm[act2]), axis=1) / np.maximum(np.sqrt(np.mean(fm[act2] ** 2, axis=1)), 1e-12))
    crest = float(np.mean([abs(np.percentile(cref, q) - np.percentile(cmet, q)) for q in (10, 50, 90)]))

    legacy = lt.spectral_distance(ref, y)[0]
    return dict(lt_erb=lt_erb, note_erb=note_erb, mcd=mcd, crest=crest, legacy=legacy)


def _mel_matrix(n_mels=40, lo=50.0, hi=16000.0):
    mel = lambda f: 2595.0 * np.log10(1.0 + f / 700.0)
    inv = lambda m: 700.0 * (10 ** (m / 2595.0) - 1.0)
    pts = inv(np.linspace(mel(lo), mel(hi), n_mels + 2))
    m = np.zeros((len(FREQS), n_mels))
    for i in range(n_mels):
        a, b, c = pts[i], pts[i + 1], pts[i + 2]
        m[:, i] = np.clip(np.minimum((FREQS - a) / (b - a), (c - FREQS) / (c - b)), 0, None)
    return m


MEL = _mel_matrix()
_NOTE_FREQS = np.fft.rfftfreq(2048, 1.0 / SR)


def _note_matrix():
    e = erb_number(_NOTE_FREQS)
    ec = erb_number(ERB_CENTRES)
    m = np.zeros((len(_NOTE_FREQS), len(ERB_CENTRES)))
    for b, c in enumerate(ec):
        idx = np.nonzero(np.abs(e - c) <= 0.5)[0]
        if len(idx) == 0:
            idx = [int(np.argmin(np.abs(e - c)))]
        m[idx, b] = 1.0 / len(idx)
    return m


_NOTE_MATRIX = _note_matrix()


def harmonic_levels(x, f0, count):
    """Harmonics 1..count of a steady tone x (Hann-windowed FFT, the peak within +-2% of each), dB."""
    n = 1 << 16
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n=n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    out = []
    for h in range(1, count + 1):
        lo, hi = np.searchsorted(f, h * f0 * 0.98), np.searchsorted(f, h * f0 * 1.02)
        out.append(20 * np.log10(np.max(spec[lo:hi]) + 1e-12))
    return np.array(out)


def sine_measures(y):
    """Per sine step (0.6 s on, 0.3 s off), harmonics 2..10 relative to the fundamental over 0.2 to 0.5 s."""
    step = int(0.9 * SR)
    out = []
    for i in range(len(SINE_LEVELS)):
        s = y[i * step + int(0.2 * SR): i * step + int(0.5 * SR)]
        h = harmonic_levels(s, 110.0, 10)
        out.append(np.maximum(h[1:] - h[0], -80.0))
    return np.array(out)


def imd_measures(y):
    """Per two-tone step, the intermodulation share of the power on the 55 Hz grid (dB)."""
    step = int(0.9 * SR)
    out = []
    n = 1 << 16
    f = np.fft.rfftfreq(n, 1.0 / SR)
    for i in range(len(TWO_TONE_LEVELS)):
        s = y[i * step + int(0.2 * SR): i * step + int(0.5 * SR)]
        spec = np.abs(np.fft.rfft(s * np.hanning(len(s)), n=n)) ** 2
        im = tot = 0.0
        for k in range(1, 91):
            lo, hi = np.searchsorted(f, k * 55.0 - 3.0), np.searchsorted(f, k * 55.0 + 3.0)
            p = float(np.max(spec[lo:hi]))
            tot += p
            if k % 2 and k % 3:
                im += p
        out.append(10 * np.log10(max(im, 1e-30) / max(tot, 1e-30)))
    return np.array(out)


def tail_measures(y, layout):
    s = layout["staccato"]
    out = []
    for o in s["onsets"]:
        a = s["start"] + o
        note = np.sum(y[a: a + s["note_len"]] ** 2) + 1e-20
        end = a + s["note_len"]
        rows = []
        for w in range(10):
            b = end + int((0.05 + 0.1 * w) * SR)
            e = np.sum(y[b: b + int(0.1 * SR)] ** 2) / note
            rows.append(max(10 * np.log10(e + 1e-20), TAIL_FLOOR_DB))
        out.append(rows)
    return np.array(out)


# Tails under -50 dB re the note don't count: a reverb or delay at a record's mix level is well above it (a
# plate 18 dB down with a 1.2 s RT60 is about -25 dB in the first window), while the amps' own low-level
# behaviour (a capture's hiss, a cab's last ringing) sits below it and shouldn't count as a time effect.
TAIL_FLOOR_DB = -50.0


class Reference:
    """The hidden rig's measurements on the evaluation signal, computed once per case."""

    def __init__(self, tone_y, full_y, layout):
        self.y, self.layout = tone_y, layout
        self.probe = {k: lt.feel_measures(seg(tone_y, layout, "probe_" + k), np.array(layout["probe_" + k]["onsets"]), k)
                      for k in lt.PROBE_PITCHES}
        self.sines = sine_measures(seg(tone_y, layout, "sines"))
        self.imd = imd_measures(seg(tone_y, layout, "two_tone"))
        self.tails = tail_measures(full_y, layout)
        self.tone_tails = tail_measures(tone_y, layout)


def facets(ref, y):
    """Every facet (module docstring) of a method's output y on the evaluation signal."""
    lay = ref.layout
    out = spectral_facets(seg(ref.y, lay, "c"), seg(y, lay, "c"), lay["c"]["onsets"])
    probe = {k: lt.feel_measures(seg(y, lay, "probe_" + k), np.array(lay["probe_" + k]["onsets"]), k) for k in lt.PROBE_PITCHES}
    feel = lt.feel_distance(probe, ref.probe)
    out.update({"feel": feel["feel"], "compression": feel["compression"], "harmonics_probe": feel["harmonics"],
                "attack": feel["attack"], "decay": feel["decay"]})
    out["harm"] = float(np.sqrt(np.mean((sine_measures(seg(y, lay, "sines")) - ref.sines) ** 2)))
    out["imd"] = float(np.sqrt(np.mean((imd_measures(seg(y, lay, "two_tone")) - ref.imd) ** 2)))
    out["time_fx"] = float(np.sqrt(np.mean((tail_measures(y, lay) - ref.tails) ** 2)))
    out["combined"] = combined(out)
    return out


# Each facet's scale: its median for the deliberately wrong rig over the DEV cases (run.py --calibrate
# measured these on 2026-10-06, 30 cases; docs/TONE_MATCH.md, "tone_bench").
SCALES = dict(lt_erb=4.75, note_erb=5.78, feel=5.29, harm=25.6, imd=18.17, crest=1.16)
WEIGHTS = dict(lt_erb=0.35, note_erb=0.15, feel=0.25, harm=0.10, imd=0.05, crest=0.10)
FACETS = ["lt_erb", "note_erb", "feel", "harm", "imd", "crest"]


def combined(f):
    return float(sum(WEIGHTS[k] * f[k] / SCALES[k] for k in FACETS))

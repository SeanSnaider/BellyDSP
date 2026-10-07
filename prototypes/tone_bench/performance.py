# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Synthetic guitar performances for the benchmark: scores per style, and three players of them.

A score is a list of events, each {beat, length (beats it sounds), notes (MIDI, one per string), art
(articulation), accent}. Five styles, each with the parts the tone matters most on:
  clean      arpeggiated chords that ring into each other, strummed chords, a melodic fill;
  edge       chord stabs, double stops, single-note fills with slides, vibrato on the long notes;
  crunch     power chords, strummed open chords, palm-muted eighths;
  high_gain  palm-muted chugs and gallops on the low root, accented power chords, a chromatic run;
  lead       single-note lines: sixteenth runs (legato notes softer and darker), bends, vibrato, slides.

Rendering. Every string is a Karplus-Strong pluck (tone_match.pluck: Karplus and Strong 1983; Jaffe and
Smith 1983), so a palm mute is its damped, darker form. Pitch changes within a note (bends, vibrato,
slides) are rendered by reading the pluck at a varying rate: with p(t) the pitch offset in semitones, the
output's sample n reads the pluck at phi[n] = sum_{k<n} 2^(p[k]/12) (linear interpolation). A string's
period is set by its length, so this is what a bend does to the waveform, except that the decay runs at
the read rate (a bent string decays a little faster; inaudible at these depths). Chords are strummed: each
string starts a few ms after the last (low to high on a down stroke). Velocity v (dB) sets each note's
peak level and its pick brightness (harder is brighter, as in learn_tone.perform). Then the guitar: a
pickup resonance (a 2-pole low-pass with Q, RBJ), its output level, and a noise floor (hiss at -84 dBFS
and mains hum harmonics at -90), which a high-gain amp brings up as real DIs do.

Players:
  A  the record's guitarist: the score in time (2 to 6 ms of jitter), the music's dynamics;
  A2 the record's double (stereo doubling): the same guitarist and guitar, another take;
  B  the player's play-along take (Sean): the same notes, timing jitter 8 to 30 ms on top of a slow drift
     of up to +-40 ms, his own velocities and attack (brightness), bends a little off (+-15 cents),
     vibrato at his own rate, his own guitar (another pickup resonance and output);
  C  held out: another score in the same style, played by B's player on B's guitar.
"""

import math

import numpy as np
import scipy.signal as sps

from common import SR, tm

STYLES = ["clean", "edge", "crunch", "high_gain", "lead"]


def midi_hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


# ---- Scores -----------------------------------------------------------------------------------------

MAJOR = [0, 2, 4, 5, 7, 9, 11]
MINOR = [0, 2, 3, 5, 7, 8, 10]
PENTA = [0, 3, 5, 7, 10]


def _triad(root, minor):
    return [root, root + (3 if minor else 4), root + 7]


def score_clean(rng, beats):
    """Arpeggios that ring (each note held 1.5 to 2.5 beats, so they overlap), a strummed chord at some bar
    starts, a short melody at the end."""
    key = int(rng.choice([40, 43, 45, 47]))
    prog = [(0, False), (9, True), (5, False), (7, False)] if rng.random() < 0.5 else [(0, True), (8, False), (3, False), (10, False)]
    ev = []
    for bar in range(int(beats // 4)):
        deg, minor = prog[bar % 4]
        root = key + deg + 12
        tri = _triad(root, minor)
        voicing = [tri[0], tri[2], tri[1] + 12, tri[0] + 12, tri[2] + 12, tri[1] + 12, tri[0] + 12, tri[2]]
        if rng.random() < 0.35:
            ev.append(dict(beat=bar * 4.0, length=3.5, notes=[tri[0] - 12, tri[0], tri[2], tri[1] + 12], art=dict(kind="strum", down=True), accent=1.0))
            continue
        for i, m in enumerate(voicing):
            ev.append(dict(beat=bar * 4.0 + 0.5 * i, length=float(rng.uniform(1.5, 2.5)), notes=[m], art=dict(kind="ring"), accent=1.0 if i == 0 else 0.0))
    return ev


def score_edge(rng, beats):
    key = int(rng.choice([40, 43, 45]))
    ev = []
    for bar in range(int(beats // 4)):
        o = bar * 4.0
        root = key + int(rng.choice([0, 5, 7, 3])) + 12
        chord = [root, root + 7, root + 12, root + 16 if rng.random() < 0.5 else root + 15]
        if bar % 2 == 0:
            for b in (0.0, 1.5, 2.5):
                ev.append(dict(beat=o + b, length=0.6, notes=chord, art=dict(kind="strum", down=b != 1.5), accent=1.0 if b == 0 else 0.0))
            ev.append(dict(beat=o + 3.0, length=1.0, notes=[root + 12, root + 16], art=dict(kind="ring"), accent=0.0))
        else:
            line = [root + 12 + MINOR[int(i)] for i in rng.choice(7, 5)]
            for i, m in enumerate(line):
                art = dict(kind="slide", semis=-2) if i == 0 else dict(kind="none")
                ev.append(dict(beat=o + 0.5 * i, length=0.5, notes=[int(m)], art=art, accent=0.0))
            ev.append(dict(beat=o + 2.5, length=1.5, notes=[int(line[-1]) + 2], art=dict(kind="vibrato", cents=25.0, rate=5.5), accent=1.0))
    return ev


def score_crunch(rng, beats):
    key = int(rng.choice([40, 42, 45]))
    ev = []
    roots = [0, 0, 5, 3, 7, 5, 10, 7]
    for bar in range(int(beats // 4)):
        o = bar * 4.0
        r = key + roots[(bar * 2) % 8]
        r2 = key + roots[(bar * 2 + 1) % 8]
        ev.append(dict(beat=o, length=1.0, notes=[r, r + 7, r + 12], art=dict(kind="strum", down=True), accent=2.0))
        ev += [dict(beat=o + 1.0 + 0.5 * i, length=0.45, notes=[r], art=dict(kind="mute"), accent=0.0) for i in range(2)]
        if bar % 2 == 1:
            ev.append(dict(beat=o + 2.0, length=2.0, notes=[r2 + 12, r2 + 19, r2 + 24, r2 + 28], art=dict(kind="strum", down=True), accent=1.0))
        else:
            ev.append(dict(beat=o + 2.0, length=1.0, notes=[r2, r2 + 7, r2 + 12], art=dict(kind="strum", down=True), accent=1.0))
            ev.append(dict(beat=o + 3.0, length=0.5, notes=[r2], art=dict(kind="mute"), accent=0.0))
            ev.append(dict(beat=o + 3.5, length=0.5, notes=[r2 + 2, r2 + 9], art=dict(kind="none"), accent=0.0))
    return ev


def score_high_gain(rng, beats):
    key = int(rng.choice([38, 40, 39]))
    ev = []
    for bar in range(int(beats // 4)):
        o = bar * 4.0
        pattern = int(rng.integers(0, 3))
        if pattern == 0:      # straight sixteenth chugs with an accented power chord
            for i in range(12):
                ev.append(dict(beat=o + 0.25 * i, length=0.22, notes=[key], art=dict(kind="mute"), accent=1.0 if i % 4 == 0 else 0.0))
            p = key + int(rng.choice([1, 3, 5, 6]))
            ev.append(dict(beat=o + 3.0, length=1.0, notes=[p, p + 7], art=dict(kind="strum", down=True), accent=2.0))
        elif pattern == 1:    # gallops
            for b in range(4):
                for d, l in ((0.0, 0.45), (0.5, 0.22), (0.75, 0.22)):
                    ev.append(dict(beat=o + b + d, length=l, notes=[key], art=dict(kind="mute"), accent=1.0 if d == 0 else 0.0))
        else:                 # a chromatic sixteenth run, picked, then an octave shape
            start = key + 12 + int(rng.integers(0, 5))
            for i in range(8):
                ev.append(dict(beat=o + 0.25 * i, length=0.25, notes=[start + (i % 4) - (i // 4)], art=dict(kind="none"), accent=0.0))
            ev.append(dict(beat=o + 2.0, length=2.0, notes=[key + 12, key + 24], art=dict(kind="vibrato", cents=18.0, rate=6.0), accent=1.5))
    return ev


def score_lead(rng, beats):
    key = int(rng.choice([57, 59, 62, 64]))
    scale = [key + s + 12 * o for o in (0, 1) for s in PENTA]
    ev = []
    pos = int(rng.integers(2, 6))
    b = 0.0
    while b < beats - 0.01:
        kind = rng.choice(["run", "bend", "vib", "slide"], p=[0.4, 0.25, 0.2, 0.15])
        if kind == "run":
            n = int(rng.choice([4, 6, 8]))
            for i in range(n):
                pos = int(np.clip(pos + rng.choice([-1, 1, 1, -2, 2]), 0, len(scale) - 1))
                legato = i % 2 == 1 and rng.random() < 0.7
                ev.append(dict(beat=b, length=0.25, notes=[scale[pos]], art=dict(kind="legato" if legato else "none"), accent=0.0))
                b += 0.25
        elif kind == "bend":
            semis = float(rng.choice([1.0, 2.0, 2.0]))
            length = float(rng.choice([1.0, 1.5, 2.0]))
            ev.append(dict(beat=b, length=length, notes=[scale[pos]], art=dict(kind="bend", semis=semis, rise=0.12, release=rng.random() < 0.4, cents=20.0, rate=5.5), accent=1.0))
            b += length
        elif kind == "vib":
            length = float(rng.choice([1.0, 1.5]))
            ev.append(dict(beat=b, length=length, notes=[scale[pos]], art=dict(kind="vibrato", cents=float(rng.uniform(25, 50)), rate=float(rng.uniform(5.0, 6.5))), accent=1.0))
            b += length
        else:
            target = int(np.clip(pos + 2, 0, len(scale) - 1))
            ev.append(dict(beat=b, length=1.0, notes=[scale[target]], art=dict(kind="slide", semis=float(scale[pos] - scale[target])), accent=0.5))
            pos = target
            b += 1.0
    return [e for e in ev if e["beat"] < beats]


SCORES = {"clean": score_clean, "edge": score_edge, "crunch": score_crunch, "high_gain": score_high_gain, "lead": score_lead}
TEMPI = {"clean": (80, 110), "edge": (90, 130), "crunch": (100, 140), "high_gain": (110, 160), "lead": (90, 130)}


def make_score(style, seed, seconds):
    rng = np.random.default_rng(seed)
    bpm = float(rng.uniform(*TEMPI[style]))
    beat_s = 60.0 / bpm
    beats = 4.0 * max(2, int(round(seconds / beat_s / 4.0)))
    return dict(style=style, bpm=bpm, beats=beats, events=SCORES[style](rng, beats))


# ---- Players -----------------------------------------------------------------------------------------

def player(kind, seed):
    """The performance parameters of a player (module docstring)."""
    rng = np.random.default_rng(seed)
    if kind in ("A", "A2"):
        return dict(jitter_ms=float(rng.uniform(2, 6)), drift_ms=0.0, contour_scale=1.0, velocity_sd_db=1.5,
                    brightness_shift=0.0, bend_error_cents=0.0, vib_rate_scale=1.0, level_db=-16.0)
    return dict(jitter_ms=float(rng.uniform(8, 30)), drift_ms=float(rng.uniform(10, 40)), contour_scale=float(rng.uniform(0.7, 1.0)),
                velocity_sd_db=float(rng.uniform(2.0, 3.0)), brightness_shift=float(rng.uniform(-0.1, 0.05)),
                bend_error_cents=15.0, vib_rate_scale=float(rng.uniform(0.85, 1.15)), level_db=-16.0)


def guitar(seed):
    """A guitar's pickup: resonance frequency and Q, and its output level (dB)."""
    rng = np.random.default_rng(seed)
    return dict(pickup_hz=float(rng.uniform(3200, 5500)), pickup_q=float(rng.uniform(1.0, 2.6)), output_db=float(rng.uniform(-3.0, 3.0)))


def contour_db(beat):
    swell = 4.0 * math.sin(2.0 * math.pi * beat / 13.0 + 0.7)
    return swell


def pitch_curve(art, n, rng, play):
    """Semitone offset per sample for a note of n samples (the articulation)."""
    t = np.arange(n) / SR
    p = np.zeros(n)
    kind = art.get("kind")
    err = rng.normal(0.0, play["bend_error_cents"] / 100.0) if play["bend_error_cents"] > 0 else 0.0
    if kind == "bend":
        rise = art["rise"]
        target = art["semis"] + err
        p = target * np.clip(t / rise, 0, 1) ** 0.7
        if art.get("release"):
            r0 = 0.6 * n / SR
            p = np.where(t > r0, target * np.clip(1 - (t - r0) / rise, 0, 1), p)
        vib_start = rise + 0.08
        depth = art.get("cents", 20.0) / 100.0
        p = p + np.where(t > vib_start, depth * np.sin(2 * np.pi * art.get("rate", 5.5) * play["vib_rate_scale"] * (t - vib_start)), 0.0)
    elif kind == "vibrato":
        start = 0.12
        depth = art["cents"] / 100.0 * np.clip((t - start) / 0.15, 0, 1)
        p = np.where(t > start, depth * np.sin(2 * np.pi * art["rate"] * play["vib_rate_scale"] * (t - start)), 0.0)
    elif kind == "slide":
        dur = 0.07
        p = art["semis"] * np.clip(1 - t / dur, 0, 1)
    return p


def string_note(freq, n, rng, level, brightness, muted, curve):
    """One string: a Karplus-Strong pluck read at the rate 2^(p/12) (module docstring)."""
    ratio = 2.0 ** (curve / 12.0)
    phi = np.concatenate([[0.0], np.cumsum(ratio[:-1])])
    n_src = int(phi[-1]) + 4
    src = tm.pluck(freq, max(n_src, 64), rng, level=level, brightness=brightness, muted=muted)
    y = np.interp(phi, np.arange(len(src)), src)
    rel = min(n, int(0.012 * SR))
    y[n - rel:] *= np.linspace(1.0, 0.0, rel)
    return y


def render(score, kind, seed, gtr):
    """A performance of the score by player `kind` on guitar `gtr`. Returns (di, onsets in samples, the
    velocities in dB, the notes' MIDI pitch)."""
    play = player(kind, seed)
    rng = np.random.default_rng(seed + 17)
    beat_s = 60.0 / score["bpm"]
    events = score["events"]
    total = int((score["beats"] * beat_s + 1.5) * SR)
    out = np.zeros(total)
    # The slow drift: two slow sinusoids with random phases, +-drift_ms at most (a play-along take wanders).
    ph = rng.uniform(0, 2 * np.pi, 2)

    def drift(t):
        return play["drift_ms"] / 1000.0 * 0.5 * (math.sin(2 * math.pi * t / 7.3 + ph[0]) + math.sin(2 * math.pi * t / 3.1 + ph[1]))

    onsets, vels, pitches = [], [], []
    last = -1.0
    for e in events:
        t = e["beat"] * beat_s
        t = max(0.05, t + drift(t) + rng.normal(0.0, play["jitter_ms"] / 1000.0))
        t = max(t, last + 0.03)
        last = t
        start = int(round(t * SR))
        n = min(int(e["length"] * beat_s * SR), total - start - 1)
        if n < 256:
            continue
        art = e["art"]
        v = play["contour_scale"] * contour_db(e["beat"]) + 2.0 * e.get("accent", 0.0) + rng.normal(0.0, play["velocity_sd_db"])
        bright = 0.55 + 0.025 * v + play["brightness_shift"] + rng.uniform(-0.04, 0.04)
        level_db = play["level_db"] + v
        if art.get("kind") == "legato":
            level_db -= 4.0
            bright -= 0.2
        bright = float(np.clip(bright, 0.15, 0.95))
        notes = e["notes"]
        strum_ms = float(rng.uniform(6, 14)) if art.get("kind") == "strum" else 0.0
        order = list(range(len(notes))) if art.get("down", True) else list(range(len(notes)))[::-1]
        level = 10.0 ** (level_db / 20.0) / math.sqrt(len(notes))
        for k, idx in enumerate(order):
            s = start + int(k * strum_ms * SR / 1000.0)
            m = min(n, total - s - 1)
            if m < 256:
                continue
            curve = pitch_curve(art, m, rng, play)
            out[s:s + m] += string_note(midi_hz(notes[idx]), m, rng, level, bright, art.get("kind") == "mute", curve)
        onsets.append(start)
        vels.append(v)
        pitches.append(notes[0])
    b, a = _pickup(gtr)
    out = sps.lfilter(b, a, out) * 10.0 ** (gtr["output_db"] / 20.0)
    out += noise_floor(len(out), seed)
    return out, np.array(onsets), np.array(vels), np.array(pitches)


def _pickup(gtr):
    w0 = 2 * math.pi * gtr["pickup_hz"] / SR
    alpha = math.sin(w0) / (2 * gtr["pickup_q"])
    b = np.array([(1 - math.cos(w0)) / 2, 1 - math.cos(w0), (1 - math.cos(w0)) / 2])
    a = np.array([1 + alpha, -2 * math.cos(w0), 1 - alpha])
    return b / a[0], a / a[0]


def noise_floor(n, seed):
    rng = np.random.default_rng(seed + 99)
    t = np.arange(n) / SR
    hiss = rng.normal(0.0, 10 ** (-84 / 20), n)
    hum = sum(10 ** (-90 / 20) / k * np.sin(2 * np.pi * 60.0 * k * t + k) for k in (1, 2, 3, 5))
    return hiss + hum

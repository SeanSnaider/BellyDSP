# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""
Pitch shifter reference (BUILD_PLAN "Pitch shifter" and "Multivoicer").

Two jobs:

1. The golden reference. The same granular engine as src/dsp/PitchShifter.cpp and the same PSOLA engine as
   src/dsp/Psola.cpp, sample by sample, with the same formulas, in double precision except where the C++
   rounds to float (the delay line and histories store floats, a Hermite read returns a float, the voice
   output is a float). The C++ has to match these renders within -100 dB: the golden tests in
   tests/PitchShifterTests.cpp. Settings change only at 128-sample buffer boundaries, as the C++ test
   applies them.

   Granular voice, per sample n (read before write: the input holds x[0 .. n-1]):
       r        = glided ratio (constant factor per sample: log-linear), slope = 1 - r
       heads   += slope                          every active head; output frequency f (1 - d') = r f
       splice   when not fading and the head would leave its 29 ms band before a fade could finish:
                r > 1: d + slope * fade <= low  -> the new head lands in [high - 24 ms, high]
                r < 1: d + slope * fade >= high -> the new head lands in [low, low + 24 ms]
                the jump J is the one with the best normalized correlation between the input around the
                old head and the input J samples away (coarse on a 12 kHz copy, fine at 48 kHz + parabola)
       fade     cos/sin of (pi/2) u divided by sqrt(1 + 2 rho sin cos), over 3 ms / |1 - r| in [1, 6] ms
       y[n]     = the heads' Hermite reads, faded

2. The design studies behind the choices (--study granular, --study psola): why two heads and not four,
   why the splices are aligned, and the PSOLA window and level law. Their numbers are quoted in
   src/dsp/PitchShifter.h, src/dsp/Psola.h, and docs/BUILD_PLAN.md.

Usage:
  uv run --with numpy --with scipy python prototypes/pitch_shifter.py --golden tests/fixtures/pitch_shifter
  uv run --with numpy --with scipy python prototypes/pitch_shifter.py --study granular
  uv run --with numpy --with scipy python prototypes/pitch_shifter.py --study psola
"""

import argparse
import json
import math
import os

import numpy as np
from scipy.io import wavfile

FS = 48000.0
BLOCK = 128


def f32(v):
    """Round to float, as the C++ does at the same points."""
    return float(np.float32(v))


# ---- Shared pieces (the same as prototypes/chorus.py) --------------------------------------------

def hermite(xm1, x0, x1, x2, t):
    """DelayLine.h's 4-point Hermite (Catmull-Rom), Niemitalo's form."""
    c1 = 0.5 * (x1 - xm1)
    c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2
    c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1)
    return ((c3 * t + c2) * t + c1) * t + x0


class DelayLine:
    def __init__(self, max_delay):
        size = 1
        while size < max_delay + 4:
            size <<= 1
        self.buf = [0.0] * size
        self.mask = size - 1
        self.w = 0
        self.max_delay = max_delay

    def write(self, x):
        self.w = (self.w + 1) & self.mask
        self.buf[self.w] = f32(x)

    def read_integer(self, k):
        return self.buf[(self.w - k) & self.mask]

    def read(self, delay):
        d = min(max(delay, 0.0), float(self.max_delay))
        whole = int(d)
        t = f32(d - whole)
        x0 = self.read_integer(whole)
        x1 = self.read_integer(whole + 1)
        xm1 = self.read_integer(whole - 1) if whole > 0 else x0
        x2 = self.read_integer(whole + 2)
        return f32(hermite(xm1, x0, x1, x2, t))


class Svf:
    """Svf.h's TPT state-variable filter, low-pass only."""

    def __init__(self, fc, q):
        fc = min(max(fc, 1.0), 0.49 * FS)
        g = math.tan(math.pi * fc / FS)
        self.k = 1.0 / q
        self.a1 = 1.0 / (1.0 + g * (g + self.k))
        self.a2 = g * self.a1
        self.a3 = g * self.a2
        self.ic1 = self.ic2 = 0.0

    def __call__(self, v0):
        v3 = v0 - self.ic2
        v1 = self.a1 * self.ic1 + self.a2 * v3
        v2 = self.ic2 + self.a2 * self.ic1 + self.a3 * v3
        self.ic1 = 2.0 * v1 - self.ic1
        self.ic2 = 2.0 * v2 - self.ic2
        return v2


def ms(v):
    return v * FS / 1000.0


# ---- PitchShifterInput -----------------------------------------------------------------------------

DECIMATION = 4
LOWPASS_HZ = 3000.0


class PitchShifterInput:
    """The shifted signal: a ModulatedDelay line (Hermite reads), the full-rate history, and the 12 kHz copy."""

    def __init__(self, max_delay_ms):
        self.max_delay = max(4.0, math.ceil(max_delay_ms * FS / 1000.0))  # ModulatedDelay::prepare
        self.line = DelayLine(int(self.max_delay))
        self.history = []
        self.decimated = []
        self.lowpass = [Svf(LOWPASS_HZ, 1.0 / (2.0 * math.sin((2.0 * s + 1.0) * math.pi / 8.0))) for s in (0, 1)]
        self.phase = 0
        self.time = 0

    def push(self, x):
        self.line.write(x)
        self.history.append(f32(x))
        y = float(x)
        for section in self.lowpass:
            y = section(y)
        self.phase += 1
        if self.phase == DECIMATION:
            self.phase = 0
            self.decimated.append(f32(y))
        self.time += 1

    def read(self, delay):
        """ModulatedDelay::read: clamp to [2, max], then the line at delay - 1."""
        d = min(max(delay, 2.0), self.max_delay)
        return self.line.read(d - 1.0)

    def newest(self, length):
        """The newest `length` samples, oldest first, zeros before the start."""
        h = self.history[-length:] if length <= len(self.history) else [0.0] * (length - len(self.history)) + self.history
        return np.array(h, dtype=np.float64)

    def newest_decimated(self, length):
        d = self.decimated[-length:] if length <= len(self.decimated) else [0.0] * (length - len(self.decimated)) + self.decimated
        return np.array(d, dtype=np.float64)

    def decimated_offset(self):
        return self.phase + 1


def normalized(c, ea, eb):
    e = ea * eb
    return c / math.sqrt(e) if e > 1.0e-30 else 0.0


# ---- RatioGlide --------------------------------------------------------------------------------------

class RatioGlide:
    def __init__(self, value=1.0):
        self.current = self.target = value
        self.step = 1.0
        self.glide = 0
        self.remaining = 0

    def jump_to(self, value):
        self.current = self.target = value
        self.remaining = 0

    def set_target(self, value):
        if value == self.target:
            return
        self.target = value
        if self.glide <= 0 or self.current <= 0.0:
            self.current = value
            self.remaining = 0
            return
        self.remaining = self.glide
        self.step = math.exp(math.log(self.target / self.current) / self.remaining)

    def next(self):
        if self.remaining > 0:
            self.current *= self.step
            self.remaining -= 1
            if self.remaining == 0:
                self.current = self.target
        return self.current


# ---- GranularVoice -----------------------------------------------------------------------------------

MIN_RATIO, MAX_RATIO = 0.25, 4.0
ALIGN_RANGE_MS = 16.0
WINDOW_MS = 12.0
JUMP_FLOOR_MS = 2.0
FADE_TRAVEL_MS = 3.0
MIN_FADE_MS, MAX_FADE_MS = 1.0, 6.0
FLOOR_MS = 1.0
BAND_MS = 2.0 * ALIGN_RANGE_MS + FADE_TRAVEL_MS + JUMP_FLOOR_MS
REPLACE_THRESHOLD_MS = 0.25
EARLY_RANGE_MS = 12.0
EARLY_QUALITY = 0.9
TIE_BREAK = 0.05
SEARCH_MARGIN_MS = 2.0 * WINDOW_MS + 1.0


def granular_max_delay_ms(max_voice_delay_ms, max_drift_ms=0.0):
    return max_voice_delay_ms + max_drift_ms + FLOOR_MS + BAND_MS + MAX_FADE_MS


class GranularVoice:
    def __init__(self, ratio=1.0, delay_ms=0.0, glide_ms=30.0):
        self.align_range = int(round(ms(ALIGN_RANGE_MS)))
        self.window = int(round(ms(WINDOW_MS)))
        self.coarse_window = self.window // DECIMATION
        self.jump_floor = ms(JUMP_FLOOR_MS)
        self.fade_travel = ms(FADE_TRAVEL_MS)
        self.min_fade = ms(MIN_FADE_MS)
        self.max_fade = ms(MAX_FADE_MS)
        self.floor = ms(FLOOR_MS)
        self.band = 2.0 * self.align_range + self.fade_travel + self.jump_floor
        self.replace_threshold = ms(REPLACE_THRESHOLD_MS)
        self.early_reach = ms(EARLY_RANGE_MS) + self.fade_travel + self.jump_floor
        self.ratio = RatioGlide()
        self.settings = dict(ratio=ratio, delay_ms=delay_ms, glide_ms=glide_ms)
        self.set_settings(ratio, delay_ms, glide_ms)
        self.reset()

    def reset(self):
        self.ratio.jump_to(min(max(self.settings["ratio"], MIN_RATIO), MAX_RATIO))
        self.target_delay = self.placed_delay = max(0.0, self.settings["delay_ms"]) * FS / 1000.0
        self.drift = 0.0
        self.band_low = self.placed_delay + self.floor
        self.heads = [[self.band_low + 0.5 * self.band, True], [0.0, False]]
        self.current = 0
        self.fading = False
        self.early_tried = False
        self.fade_pos = self.fade_step = self.fade_rho = 0.0
        self.splices = 0
        self.log = []  # (n, jump, rho) per splice, for the study

    def set_settings(self, ratio, delay_ms, glide_ms):
        self.settings = dict(ratio=ratio, delay_ms=delay_ms, glide_ms=glide_ms)
        self.ratio.glide = max(0, int(round(max(0.0, glide_ms) * FS / 1000.0)))
        self.ratio.set_target(min(max(ratio, MIN_RATIO), MAX_RATIO))
        self.target_delay = max(0.0, delay_ms) * FS / 1000.0

    def fade_length(self, slope):
        speed = abs(slope)
        if speed * self.max_fade <= self.fade_travel:
            return self.max_fade
        return max(self.min_fade, self.fade_travel / speed)

    def start_fade(self, new_delay, rho, fade):
        b = self.heads[1 - self.current]
        b[0] = new_delay
        b[1] = True
        self.fading = True
        self.early_tried = False
        self.fade_pos = 0.0
        self.fade_step = 1.0 / max(1.0, fade)
        self.fade_rho = min(max(rho, 0.0), 1.0)

    def correlation_at(self, inp, delay_a, jump, offset=0):
        a = int(math.ceil(delay_a)) + offset
        length = a + max(0, jump) + self.window
        if a + min(0, jump) < 1:
            return 0.0
        base = inp.newest(length)
        ia = length - a - self.window + 1
        pa = base[ia:ia + self.window]
        pb = base[ia - jump:ia - jump + self.window]
        rho = normalized(float(pa @ pb), float(pa @ pa), float(pb @ pb))
        return min(max(rho, 0.0), 1.0)

    def find_splice(self, inp, delay_a, jump_lo, jump_hi, preferred):
        middle = 0.5 * (jump_lo + jump_hi)
        j_lo, j_hi = int(math.ceil(jump_lo)), int(math.floor(jump_hi))
        a = int(math.ceil(delay_a))
        if j_hi < j_lo or a + j_lo < 1:
            return middle, 0.0, False

        fine_lo, fine_hi = j_lo, j_hi
        m_lo = int(math.ceil(j_lo / DECIMATION))
        m_hi = int(math.floor(j_hi / DECIMATION))
        if m_lo <= m_hi:
            offset = inp.decimated_offset()
            ja = max(0, int(math.ceil((a - offset) / DECIMATION)))
            wd = self.coarse_window
            length = ja + max(0, m_hi) + wd
            if ja + min(0, m_lo) < 0:
                return middle, 0.0, False
            base = inp.newest_decimated(length)
            ia = length - ja - wd
            pa = base[ia:ia + wd]
            ea = float(pa @ pa)
            if ea <= 1.0e-30:
                return middle, 0.0, False
            span = max(1.0, jump_hi - jump_lo)
            best_score, best_m, best_rho = -1.0e300, m_lo, 0.0
            for m in range(m_lo, m_hi + 1):
                pb = base[ia - m:ia - m + wd]
                rho = normalized(float(pa @ pb), ea, float(pb @ pb))
                score = rho - TIE_BREAK * abs(DECIMATION * m - preferred) / span
                if score > best_score:
                    best_score, best_m = score, m
                best_rho = max(best_rho, rho)
            if best_rho <= 0.0:
                return middle, 0.0, False
            fine_lo = max(j_lo, DECIMATION * best_m - 3)
            fine_hi = min(j_hi, DECIMATION * best_m + 3)

        if fine_hi - fine_lo + 1 > 16:
            fine_hi = fine_lo + 15
        length = a + max(0, fine_hi) + self.window
        if a + min(0, fine_lo) < 1:
            return middle, 0.0, False
        base = inp.newest(length)
        ia = length - a - self.window + 1
        pa = base[ia:ia + self.window]
        ea = float(pa @ pa)
        if ea <= 1.0e-30:
            return middle, 0.0, False
        rhos = []
        for j in range(fine_lo, fine_hi + 1):
            pb = base[ia - j:ia - j + self.window]
            rhos.append(normalized(float(pa @ pb), ea, float(pb @ pb)))
        best = 0
        for k in range(1, len(rhos)):
            if rhos[k] > rhos[best]:
                best = k
        if rhos[best] <= 0.0:
            return middle, 0.0, False
        jump = float(fine_lo + best)
        peak = rhos[best]
        if 0 < best < len(rhos) - 1:
            y0, y1, y2 = rhos[best - 1], rhos[best], rhos[best + 1]
            curvature = y0 - 2.0 * y1 + y2
            if curvature < 0.0:
                delta = min(max(0.5 * (y0 - y2) / curvature, -1.0), 1.0)
                jump += delta
                peak = y1 - 0.25 * (y0 - y2) * delta
        return min(max(jump, jump_lo), jump_hi), min(max(peak, 0.0), 1.0), True

    def maybe_splice(self, inp, slope):
        delay_a = self.heads[self.current][0]
        if abs(self.target_delay - self.placed_delay) >= self.replace_threshold:
            jump = self.target_delay - self.placed_delay
            self.placed_delay = self.target_delay
            self.band_low = self.placed_delay + self.floor + self.drift
            self.splices += 1
            rho = self.correlation_at(inp, delay_a, int(round(jump)))  # juce::roundToInt rounds half to even, as round() does
            self.log.append((inp.time, jump, rho))
            self.start_fade(delay_a + jump, rho, self.max_fade)
            return

        fade = self.fade_length(slope)
        lo, hi = self.band_low, self.band_low + self.band
        if slope > 0.0 and not self.early_tried and delay_a + slope * fade >= lo + self.early_reach and delay_a + slope * fade < hi:
            self.early_tried = True
            jump, rho, matched = self.find_splice(inp, delay_a, lo - delay_a, -self.jump_floor, lo - delay_a)
            if matched and rho >= EARLY_QUALITY:
                self.splices += 1
                self.log.append((inp.time, jump, rho))
                self.start_fade(delay_a + jump, self.correlation_at(inp, delay_a, int(round(jump)), self.window), fade)
                return
        if slope < 0.0:
            need = delay_a + slope * fade <= lo or delay_a > hi
        elif slope > 0.0:
            need = delay_a + slope * fade >= hi or delay_a < lo
        else:
            need = delay_a < lo or delay_a > hi
        if not need:
            return

        self.placed_delay = self.target_delay
        self.band_low = self.placed_delay + self.floor + self.drift
        low, high, middle = self.band_low, self.band_low + self.band, self.band_low + 0.5 * self.band
        if slope < 0.0:
            t_lo, t_hi = high - 2.0 * self.align_range, high
        elif slope > 0.0:
            t_lo, t_hi = low, low + 2.0 * self.align_range
        else:
            t_lo, t_hi = middle - self.align_range, middle + self.align_range
        preferred = (t_lo if slope > 0.0 else 0.5 * (t_lo + t_hi)) - delay_a
        jump, rho, matched = self.find_splice(inp, delay_a, t_lo - delay_a, t_hi - delay_a, preferred)
        new_delay = delay_a + jump if matched else 0.5 * (t_lo + t_hi)
        # The fade's correlation comes from the window before the search's (the search's own is biased up).
        fade_rho = self.correlation_at(inp, delay_a, int(round(new_delay - delay_a)), self.window) if matched else 0.0
        self.splices += 1
        self.log.append((inp.time, new_delay - delay_a, rho))
        self.start_fade(new_delay, fade_rho, fade)

    def process(self, inp, ratio_scale=1.0, extra_delay=0.0):
        slope = 1.0 - self.ratio.next() * ratio_scale
        move = slope + (extra_delay - self.drift)
        self.drift = extra_delay
        self.band_low = self.placed_delay + self.floor + self.drift
        for h in self.heads:
            if h[1]:
                h[0] += move
        if not self.fading:
            self.maybe_splice(inp, slope)
        a = self.heads[self.current]
        if not self.fading:
            return inp.read(a[0])
        b = self.heads[1 - self.current]
        angle = 0.5 * math.pi * self.fade_pos
        s, c = math.sin(angle), math.cos(angle)
        norm = 1.0 / math.sqrt(1.0 + 2.0 * self.fade_rho * s * c)
        y = norm * (c * inp.read(a[0]) + s * inp.read(b[0]))
        self.fade_pos += self.fade_step
        if self.fade_pos >= 1.0:
            a[1] = False
            self.current = 1 - self.current
            self.fading = False
        return f32(y)


def granular_render(x, ratio, delay_ms=0.0, glide_ms=30.0, max_voice_delay_ms=0.0, changes=None, voice_out=None):
    """GranularShifter over x. changes: {sample index (a multiple of 128): (ratio, delay_ms, glide_ms)}."""
    inp = PitchShifterInput(granular_max_delay_ms(max_voice_delay_ms))
    v = GranularVoice(ratio, delay_ms, glide_ms)
    y = np.zeros(len(x))
    for n in range(len(x)):
        if changes and n in changes:
            v.set_settings(*changes[n])
        y[n] = v.process(inp)
        inp.push(x[n])
    if voice_out is not None:
        voice_out.append(v)
    return y


# ---- PSOLA (Psola.cpp) -----------------------------------------------------------------------------
#
# PsolaAnalysis with a fixed period track standing in for the pitch detector (setPeriodOverride in the C++;
# the detector itself has its own golden test against prototypes/pitch_detection.py):
#   every 64 samples a reading (fs / period, clarity 1); two agreeing readings (within 50 cents) confirm a
#   pitch; a confirmed pitch 50 cents from the tracked one (or none tracked, or a lost track) anchors a new
#   track at the largest sample of the latest period
#   each next mark: candidates tau in [P - R, P + R] (R = ceil(P/8)) after the last mark's sample; the
#   window of one period ending at the last mark against the one ending tau later, normalized correlation,
#   best tau refined by a parabola; a match below 0.7 loses the track (marks continue at the last period)
#   confidence ramps toward 1 (tracking and not lost) or 0 over 2 ms
# PsolaVoice, per sample (before the input's push):
#   r glided; when now >= next centre - min(P, P/r): a grain at the next centre, from the mark nearest to
#   (centre - lag), half-length min(P, P/r), delay centre - mark, gain levelCompensation(r); next centre
#   += P/r. Output: sum of gain * Hann((now - centre) / half) * read(delay) over live grains.

PSOLA_HOP = 64
CLARITY_THRESHOLD = 0.9
AGREE_CENTS = 50.0
NEW_NOTE_CENTS = 50.0
TRACK_QUALITY = 0.7
SEARCH_FRACTION = 0.125
CONFIDENCE_RAMP_MS = 2.0
MAX_MARKS = 256


def lround(v):
    """std::lround / std::llround for the positive values used here: round half away from zero."""
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


class PsolaAnalysis:
    def __init__(self):
        self.override = 0.0
        self.hop_counter = 0
        self.previous_confident = 0.0
        self.unconfident = 0
        self.tracking = False
        self.lost = False
        self.period = 0.0
        self.note_count = 0
        self.marks = []  # (position, period, quality), oldest first
        self.confidence = 0.0
        self.confidence_step = 1.0 / max(1.0, CONFIDENCE_RAMP_MS * FS / 1000.0)

    def process(self, inp):
        self.hop_counter += 1
        if self.hop_counter >= PSOLA_HOP:
            self.hop_counter = 0
            if self.override > 0.0:
                self.on_estimate(FS / self.override, 1.0, inp)
        if self.tracking:
            self.track_next(inp)
        target = 1.0 if (self.tracking and not self.lost) else 0.0
        if target > self.confidence:
            self.confidence = min(target, self.confidence + self.confidence_step)
        else:
            self.confidence = max(target, self.confidence - self.confidence_step)

    def on_estimate(self, frequency, clarity, inp):
        confident = frequency > 0.0 and clarity >= CLARITY_THRESHOLD
        if not confident:
            self.previous_confident = 0.0
            self.unconfident += 1
            if self.unconfident >= 2:
                self.tracking = False
            return
        self.unconfident = 0
        agrees = self.previous_confident > 0.0 and abs(1200.0 * math.log2(frequency / self.previous_confident)) <= AGREE_CENTS
        self.previous_confident = frequency
        if not agrees:
            return
        new_period = FS / frequency
        if not self.tracking or self.lost or abs(1200.0 * math.log2(self.period / new_period)) > NEW_NOTE_CENTS:
            self.anchor(new_period, inp)

    def anchor(self, new_period, inp):
        p = max(8, lround(new_period))
        x = inp.newest(p)
        best, best_mag = 0, -1.0
        for i in range(p):
            if abs(x[i]) > best_mag:
                best_mag, best = abs(x[i]), i
        self.marks = [(float(inp.time - p + best), new_period, 1.0)]
        self.period = new_period
        self.tracking = True
        self.lost = False
        self.note_count += 1

    def track_next(self, inp):
        last = self.marks[-1]
        rng = max(2, int(math.ceil(SEARCH_FRACTION * self.period)))
        anchor_index = lround(last[0])
        tau_lo = max(8, int(math.floor(self.period)) - rng)
        tau_hi = int(math.ceil(self.period)) + rng
        if inp.time - 1 < anchor_index + tau_hi:
            return
        w = max(16, lround(self.period))
        length = inp.time - (anchor_index - w + 1)
        tau, quality = self.period, 0.0
        base = inp.newest(length)
        pa = base[0:w]
        ea = float(pa @ pa)
        if ea > 1.0e-30:
            count = min(512, tau_hi - tau_lo + 1)
            rho = []
            best = 0
            for k in range(count):
                pb = base[tau_lo + k:tau_lo + k + w]
                eb = float(pb @ pb)
                rho.append(float(pa @ pb) / math.sqrt(ea * eb) if ea * eb > 1.0e-30 else 0.0)
                if rho[k] > rho[best]:
                    best = k
            tau = float(tau_lo + best)
            quality = rho[best]
            if 0 < best < count - 1:
                y0, y1, y2 = rho[best - 1], rho[best], rho[best + 1]
                curvature = y0 - 2.0 * y1 + y2
                if curvature < 0.0:
                    delta = min(max(0.5 * (y0 - y2) / curvature, -1.0), 1.0)
                    tau += delta
                    quality = y1 - 0.25 * (y0 - y2) * delta
        if quality >= TRACK_QUALITY:
            self.lost = False
            self.period = tau
        else:
            self.lost = True
            tau = self.period
        self.marks.append((last[0] + tau, tau, quality))
        if len(self.marks) > MAX_MARKS:
            self.marks.pop(0)

    def nearest(self, time):
        best = len(self.marks) - 1
        best_distance = abs(self.marks[best][0] - time)
        for i in range(len(self.marks) - 2, -1, -1):
            d = abs(self.marks[i][0] - time)
            if d >= best_distance:
                break
            best, best_distance = i, d
        return self.marks[best]


def level_compensation(r):
    if r >= 1.0:
        return 1.0
    q = 1.0 / r
    if q < 2.0:
        c = 0.25 * ((2.0 - q) * (1.0 + 0.5 * math.cos(math.pi * q)) + 1.5 / math.pi * math.sin(math.pi * q))
        m = r * (0.75 + 2.0 * c)
    else:
        m = 0.75 * r
    return 1.0 / math.sqrt(m)


class PsolaVoice:
    def __init__(self, ratio=1.0, delay_ms=0.0, glide_ms=30.0):
        self.ratio = RatioGlide()
        self.set_settings(ratio, delay_ms, glide_ms)
        self.ratio.jump_to(min(max(ratio, MIN_RATIO), MAX_RATIO))
        self.grains = [[0.0, 1.0, 0.0, 1.0, False, 0.0] for _ in range(8)]  # centre, half, delay, gain, active, offset
        self.next_centre = -1.0e18
        self.note_count = -1
        self.delays = []

    def set_settings(self, ratio, delay_ms, glide_ms):
        self.delay_ms = delay_ms
        self.ratio.glide = max(0, int(round(max(0.0, glide_ms) * FS / 1000.0)))
        self.ratio.set_target(min(max(ratio, MIN_RATIO), MAX_RATIO))

    def process(self, ana, inp, ratio_scale=1.0, extra_delay=0.0):
        r = min(max(self.ratio.next() * ratio_scale, MIN_RATIO), MAX_RATIO)
        now = float(inp.time)
        if ana.marks:
            newest = ana.marks[-1]
            half_estimate = min(newest[1], newest[1] / r)
            if ana.note_count != self.note_count:
                self.note_count = ana.note_count
                self.next_centre = min(self.next_centre, now + half_estimate)
            if self.next_centre < now:
                self.next_centre = now + half_estimate
            if now >= self.next_centre - half_estimate:
                lag = max(0.0, self.delay_ms) * FS / 1000.0 + extra_delay
                mark = ana.nearest(self.next_centre - lag)
                p = mark[1]
                slot = self.grains[0]
                for g in self.grains:
                    if not g[4]:
                        slot = g
                        break
                    if g[0] < slot[0]:
                        slot = g
                slot[0] = self.next_centre
                slot[1] = min(p, p / r)
                slot[2] = self.next_centre - mark[0]
                slot[3] = level_compensation(r)
                slot[5] = windowed_mean(inp, mark[0] - p, slot[1]) if slot[1] < p else 0.0
                slot[4] = True
                self.delays.append(slot[2])
                self.next_centre += p / r
        y = 0.0
        for g in self.grains:
            if not g[4]:
                continue
            u = (now - g[0]) / g[1]
            if u >= 1.0:
                g[4] = False
                continue
            if u <= -1.0:
                continue
            w = 0.5 * (1.0 + math.cos(math.pi * u))
            y += g[3] * w * (inp.read(g[2]) - g[5])
        return f32(1.0 * y)


def windowed_mean(inp, centre, half):
    """PsolaVoice::windowedMean: the Hann-weighted mean over whole samples within half of round(centre), the
    cosines by the same Chebyshev recurrence."""
    h = int(math.floor(half))
    middle = lround(centre)
    if h < 1 or middle + h > inp.time - 1:
        return 0.0
    length = inp.time - (middle - h)
    x = inp.newest(length)
    theta = math.pi / half
    two_cos = 2.0 * math.cos(theta)
    previous, current = math.cos(theta * (-h - 1)), math.cos(theta * -h)
    sw = swx = 0.0
    for k in range(2 * h + 1):
        w = 0.5 * (1.0 + current)
        sw += w
        swx += w * x[k]
        previous, current = current, two_cos * current - previous
    return swx / sw if sw > 0.0 else 0.0


PSOLA_MAX_DELAY_MS = 40.0


def psola_render(x, track, ratio, delay_ms=0.0, glide_ms=30.0, changes=None, analysis_out=None):
    """One PSOLA voice with the period track `track` ({block start: period}) as the analysis' readings."""
    inp = PitchShifterInput(PSOLA_MAX_DELAY_MS)
    ana = PsolaAnalysis()
    v = PsolaVoice(ratio, delay_ms, glide_ms)
    y = np.zeros(len(x))
    for n in range(len(x)):
        if n in track:
            ana.override = track[n]
        if changes and n in changes:
            v.set_settings(*changes[n])
        y[n] = v.process(ana, inp)
        inp.push(x[n])
        ana.process(inp)
    if analysis_out is not None:
        analysis_out.append((ana, v))
    return y


def notes_signal(seconds=1.0, seed=11):
    """Single notes, each a decaying harmonic tone with a 2 ms attack, one after another with no gap: G3,
    D3, B3, then A2. Returns the signal and its period track (at 128-sample buffer starts)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    notes = [(0, 196.0), (128 * 150, 146.83), (128 * 240, 246.94), (128 * 310, 110.0)]
    out = np.zeros(n)
    track = {}
    for i, (start, f0) in enumerate(notes):
        end = notes[i + 1][0] if i + 1 < len(notes) else n
        tt = t[start:end] - t[start]
        env = np.exp(-tt / 0.35) * np.minimum(1.0, tt / 0.002)
        tone = sum(np.sin(2 * np.pi * f0 * k * tt + rng.uniform(0, 2 * np.pi)) / k ** 1.1 for k in range(1, 16) if f0 * k < 10000)
        out[start:end] += 0.25 * env * tone
        track[start] = FS / f0
    return out, track


PSOLA_CASES = {
    "down_octave": dict(ratio=0.5, delay_ms=0.0, glide_ms=30.0, changes=[]),
    "up_fifth": dict(ratio=2 ** (7 / 12), delay_ms=0.0, glide_ms=30.0, changes=[]),
    "up_two_octaves": dict(ratio=4.0, delay_ms=0.0, glide_ms=30.0, changes=[]),
    # Down an octave with a 4 ms lag, gliding to -5 semitones over 40 ms at 0.25 s and to +12 at 0.6 s.
    "glide": dict(ratio=0.5, delay_ms=4.0, glide_ms=40.0,
                  changes=[[128 * 94, 2 ** (-5 / 12), 4.0, 40.0], [128 * 225, 2.0, 4.0, 40.0]]),
}


# ---- Design study: granular ------------------------------------------------------------------------

def _read(x, pos):
    """Vectorized Hermite read of x at fractional positions (zeros outside)."""
    i = np.floor(pos).astype(np.int64)
    t = pos - i
    n = len(x)

    def at(k):
        v = x[np.clip(k, 0, n - 1)]
        return np.where((k >= 0) & (k < n), v, 0.0)
    return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), t)


def _four_heads(x, r, hop):
    """The classic design: four heads always overlapping, Hann windows summing to 1, each sweeping its
    delay by (1 - r) over its cycle of 4 hops."""
    t = np.arange(len(x), dtype=np.float64)
    period = 4 * hop
    centre = abs(1 - r) * period / 2 + 64
    y = np.zeros(len(x))
    for k in range(4):
        phi = np.mod(t / period + k / 4, 1.0)
        d = centre + (1 - r) * period * (phi - 0.5)
        y += 0.5 * np.sin(np.pi * phi) ** 2 * _read(x, t - d)
    return y


def _two_heads_unaligned(x, r, hop, xfade):
    """A plain sawtooth shifter: a new head every hop, jump (r - 1) hop, equal-power crossfade, no search."""
    n = len(x)
    t = np.arange(n, dtype=np.float64)
    grain = hop + xfade
    centre = abs(1 - r) * grain / 2 + 64
    y = np.zeros(n)
    for g in range(-1, int(n / hop) + 2):
        t0 = g * hop
        lo, hi = max(0, int(t0)), min(n, int(t0 + grain) + 1)
        if lo >= hi:
            continue
        tt = t[lo:hi]
        d = centre + (1 - r) * (tt - (t0 + grain / 2))
        u_in = np.clip((tt - t0) / xfade, 0, 1)
        u_out = np.clip((tt - t0 - hop) / xfade, 0, 1)
        y[lo:hi] += np.sin(0.5 * np.pi * u_in) * np.cos(0.5 * np.pi * u_out) * _read(x, tt - d)
    return y


def _envelope_db(y, skip):
    from scipy.signal import hilbert
    e = np.abs(hilbert(y))[skip:-skip]
    return 20 * np.log10(np.mean(e))


def _peak_frequency(y, f, start, order=16):
    seg = y[start:start + (1 << order)]
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    bin_hz = FS / (1 << order)
    lo, hi = int(f * 2 ** (-60 / 1200) / bin_hz), int(math.ceil(f * 2 ** (60 / 1200) / bin_hz))
    k = lo + int(np.argmax(mag[lo:hi + 1]))
    a, b, c = (np.log(mag[k + d] + 1e-30) for d in (-1, 0, 1))
    return (k + 0.5 * (a - c) / (a - 2 * b + c)) * bin_hz


def _off_partials_db(y, roots, r, start, order=16):
    seg = y[start:start + (1 << order)]
    p = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1 / FS)
    on = np.zeros_like(f, dtype=bool)
    for root in roots:
        for h in range(1, 9):
            on |= np.abs(f - h * root * r) <= 3.0
    return 10 * np.log10(p[~on].sum() / p.sum())


def _chord(roots, n, seed=3):
    rng = np.random.default_rng(seed)
    t = np.arange(n) / FS
    x = sum(np.sin(2 * np.pi * f0 * k * t + rng.uniform(0, 2 * np.pi)) / k ** 1.2 for f0 in roots for k in range(1, 9))
    return 0.5 * x / np.max(np.abs(x))


def _granular(x, r):
    inp = PitchShifterInput(granular_max_delay_ms(0.0) + 2 * ALIGN_RANGE_MS)
    v = GranularVoice(r)
    y = np.zeros(len(x))
    trail = 0.0
    for n in range(len(x)):
        y[n] = v.process(inp)
        inp.push(x[n])
        trail += v.heads[v.current][0]
    return y, trail / len(x) * 1000.0 / FS


def study_granular():
    global ALIGN_RANGE_MS
    t = np.arange(int(0.8 * FS)) / FS
    hop = 0.020 * FS

    print("1. Overlapping heads comb. A sine's level (dB) after an octave up, by input frequency:")
    freqs = [100, 112, 119, 126, 133, 150, 168, 178, 200, 224, 252, 283, 300, 378]
    four = [_envelope_db(_four_heads(np.sin(2 * np.pi * f * t), 2.0, hop), 9600) for f in freqs]
    two = [_envelope_db(_granular(np.sin(2 * np.pi * f * t), 2.0)[0], 9600) for f in freqs]
    print("   input Hz          " + " ".join(f"{f:6d}" for f in freqs))
    print("   four Hann heads   " + " ".join(f"{v:6.1f}" for v in four))
    print("   two heads (here)  " + " ".join(f"{v:6.1f}" for v in two))
    print("   (four heads 5 ms apart in delay are a comb with teeth every 50 Hz of input)")

    print("2. Splice alignment. A sine shifted up an octave, output frequency (target 2 f):")
    for f in [100.0, 112.5, 124.0]:
        x = np.sin(2 * np.pi * f * np.arange(int(2.0 * FS)) / FS)
        plain = _peak_frequency(_two_heads_unaligned(x, 2.0, hop, hop / 3), 2 * f, int(0.25 * FS))
        aligned = _peak_frequency(_granular(x, 2.0)[0], 2 * f, int(0.25 * FS))
        print(f"   {f:6.1f} Hz: sawtooth every 20 ms {plain:7.2f} Hz ({1200 * np.log2(plain / (2 * f)):+7.1f} cents), "
              f"aligned {aligned:7.2f} Hz ({1200 * np.log2(aligned / (2 * f)):+6.2f} cents)")

    print("3. Alignment range: each chord's worst note (cents) / energy off the partials (dB) / mean trail (ms):")
    chords = {"E5": [82.41, 123.47, 164.81], "C major": [130.81, 164.81, 196.0],
              "Gadd9": [98.0, 123.47, 146.83, 220.0, 293.66], "Am7": [110.0, 164.81, 196.0, 261.63]}
    saved = ALIGN_RANGE_MS
    n = int(2.8 * FS)
    for rng_ms in (12.0, 16.0, 20.0):
        ALIGN_RANGE_MS = rng_ms
        print(f"   +-{rng_ms:.0f} ms (band {2 * rng_ms + FADE_TRAVEL_MS + JUMP_FLOOR_MS:.0f} ms)")
        for name, roots in chords.items():
            x = _chord(roots, n)
            row = []
            for semis in (-12, 7, 12):
                r = 2 ** (semis / 12)
                y, trail = _granular(x, r)
                worst = max(abs(1200 * np.log2(_peak_frequency(y, f * r, int(1.2 * FS)) / (f * r))) for f in roots)
                row.append(f"{semis:+3d}: {worst:5.1f} c {_off_partials_db(y, roots, r, int(1.2 * FS)):6.1f} dB {trail:5.1f} ms")
            print(f"     {name:8s} " + " | ".join(row))
    ALIGN_RANGE_MS = saved


def _psola_ideal(x, period, r, short_window, dc_fix, marks_on_peaks=True):
    """TD-PSOLA with an exactly known integer period: marks every period (on the waveform's peak, as the
    engine's anchor puts them, or at an arbitrary phase), grains from the nearest mark one and a half
    periods back, Hann half-length P (or min(P, P/r) with short_window), the level law for r < 1, and
    optionally each short grain minus its windowed mean."""
    n = len(x)
    first = int(np.argmax(np.abs(x[period:2 * period]))) + period if marks_on_peaks else period
    marks = np.arange(first, n - period, period)
    y = np.zeros(n)
    s = 3.0 * period
    while s < n - 2 * period:
        a = marks[np.argmin(np.abs(marks - (s - 1.5 * period)))]
        h = min(period, period / r) if short_window else float(period)
        lo, hi = int(math.ceil(s - h)), int(math.floor(s + h))
        t = np.arange(lo, hi + 1, dtype=np.float64)
        w = 0.5 * (1 + np.cos(np.pi * (t - s) / h))
        seg = _read(x, t - (s - a))
        offset = 0.0
        if dc_fix and h < period:
            k = np.arange(-int(h), int(h) + 1)
            ww = 0.5 * (1 + np.cos(np.pi * k / h))
            offset = float(np.sum(ww * x[a - period + k]) / np.sum(ww))
        y[lo:hi + 1] += level_compensation(r) * w * (seg - offset)
        s += period / r
    return y


def _harmonic_report(y, f0, skip):
    seg = y[skip:-skip]
    p = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    f = np.fft.rfftfreq(len(seg), 1 / FS)
    on = np.zeros_like(f, dtype=bool)
    for k in range(1, 60):
        on |= np.abs(f - k * f0) <= 3.0
    off_db = 10 * np.log10(max(p[~on].sum(), 1e-30) / p.sum())
    dc_db = 10 * np.log10(max(p[f < 3.0].sum(), 1e-30) / p.sum())
    return off_db, dc_db, 10 * np.log10(np.mean(seg ** 2) + 1e-30)


def study_psola():
    t = np.arange(int(1.0 * FS)) / FS
    period = 436  # 110.09 Hz, a whole number of samples so the marks are exact
    f0 = FS / period
    signals = {"sine": np.sin(2 * np.pi * f0 * t),
               "harmonic": 0.5 * sum(np.sin(2 * np.pi * f0 * k * t + 0.3 * k * k) / k for k in range(1, 11))}
    print("PSOLA window and offset, marks on the waveform's peaks (level dB vs input / energy off the harmonics dB / DC share dB):")
    for name, x in signals.items():
        level_in = 10 * np.log10(np.mean(x ** 2))
        print(f"  {name}")
        for r in (0.25, 0.5, 2 ** (-5 / 12), 2 ** (7 / 12), 2.0, 4.0):
            row = []
            for label, short, fix in (("two periods", False, False), ("two synthesis periods", True, False), ("... minus the mean", True, True)):
                y = _psola_ideal(x, period, r, short, fix)
                off, dc, level = _harmonic_report(y, f0 * r, int(0.1 * FS))
                row.append(f"{label}: {level - level_in:7.1f} / {off:6.1f} / {dc:6.1f}")
            print(f"    r {r:5.3f}  " + " | ".join(row))
    print("Level law for r < 1 (1/sqrt(mean square of the summed windows)) against the harmonic tone's measured level:")
    for r in (0.25, 0.35, 0.5, 0.6, 2 ** (-5 / 12), 0.9):
        x = signals["harmonic"]
        y = _psola_ideal(x, period, r, True, True)
        print(f"    r {r:5.3f}: gain {20 * np.log10(level_compensation(r)):5.2f} dB, output level {10 * np.log10(np.mean(y[4800:-4800] ** 2) / np.mean(x ** 2)):6.2f} dB")


# ---- Golden renders ----------------------------------------------------------------------------------

def test_signal(seconds=1.0, seed=7):
    """1 s of mono guitar-like material: decaying harmonic plucks from a low E up to a high lead note, an
    E5 power chord, a short noise burst, and a gap of digital silence, so the searches see single notes,
    a chord, noise, and nothing."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = np.zeros(n)
    notes = [(0.00, [82.41], -8.0), (0.14, [196.0], -10.0), (0.26, [82.41, 123.47, 164.81], -9.0),
             (0.52, [329.6], -12.0), (0.64, [659.3], -14.0), (0.80, [110.0], -9.0)]
    for start, freqs, level in notes:
        env = np.where(t >= start, np.exp(-(t - start) / 0.12), 0.0)
        for f in freqs:
            tone = sum(np.sin(2 * np.pi * f * k * (t - start) + rng.uniform(0, 2 * np.pi)) / k ** 1.2
                       for k in range(1, 14) if f * k < 9000.0)
            out += 10 ** (level / 20) * env * tone / 2.0
    burst = (t >= 0.45) & (t < 0.48)
    out[burst] += 0.05 * rng.uniform(-1, 1, burst.sum())
    out[(t >= 0.76) & (t < 0.80)] = 0.0
    return out


GRANULAR_CASES = {
    "up_fifth": dict(ratio=2 ** (7 / 12), delay_ms=0.0, glide_ms=30.0, max_voice_delay_ms=0.0, changes=[]),
    "down_octave": dict(ratio=0.5, delay_ms=0.0, glide_ms=30.0, max_voice_delay_ms=0.0, changes=[]),
    "up_two_octaves": dict(ratio=4.0, delay_ms=0.0, glide_ms=30.0, max_voice_delay_ms=0.0, changes=[]),
    "detune": dict(ratio=2 ** (-12 / 1200), delay_ms=3.0, glide_ms=30.0, max_voice_delay_ms=10.0, changes=[]),
    # An octave up with a 5 ms voice delay, gliding to -5 semitones over 40 ms at 0.32 s, then to unison
    # with the delay moved to 12 ms (a re-placement) at 0.667 s.
    "glide_and_delay": dict(ratio=2.0, delay_ms=5.0, glide_ms=40.0, max_voice_delay_ms=20.0,
                            changes=[[128 * 120, 2 ** (-5 / 12), 5.0, 40.0], [128 * 250, 1.0, 12.0, 40.0]]),
}


def write_golden(folder):
    os.makedirs(folder, exist_ok=True)
    x = test_signal().astype(np.float32).astype(np.float64)  # the C++ reads these float32 values
    wavfile.write(os.path.join(folder, "input.wav"), int(FS), x.astype(np.float32))
    for name, c in GRANULAR_CASES.items():
        changes = {n: (r, d, g) for n, r, d, g in c["changes"]}
        voices = []
        y = granular_render(x, c["ratio"], c["delay_ms"], c["glide_ms"], c["max_voice_delay_ms"], changes, voices)
        wavfile.write(os.path.join(folder, f"expected_granular_{name}.wav"), int(FS), y.astype(np.float32))
        print(f"granular {name}: {voices[0].splices} splices, output RMS {20 * np.log10(np.sqrt(np.mean(y ** 2)) + 1e-30):.1f} dBFS")
    notes, track = notes_signal()
    notes = notes.astype(np.float32).astype(np.float64)
    wavfile.write(os.path.join(folder, "input_notes.wav"), int(FS), notes.astype(np.float32))
    for name, c in PSOLA_CASES.items():
        changes = {n: (r, d, g) for n, r, d, g in c["changes"]}
        out = []
        y = psola_render(notes, track, c["ratio"], c["delay_ms"], c["glide_ms"], changes, out)
        wavfile.write(os.path.join(folder, f"expected_psola_{name}.wav"), int(FS), y.astype(np.float32))
        ana, v = out[0]
        print(f"psola {name}: {ana.note_count} notes tracked, {len(v.delays)} grains, mean grain delay "
              f"{np.mean(v.delays) / FS * 1000:.2f} ms, output RMS {20 * np.log10(np.sqrt(np.mean(y ** 2)) + 1e-30):.1f} dBFS")
    psola = dict(track=[[int(k), v] for k, v in sorted(track.items())], max_delay_ms=PSOLA_MAX_DELAY_MS, cases=PSOLA_CASES)
    with open(os.path.join(folder, "cases.json"), "w") as f:
        json.dump(dict(granular=GRANULAR_CASES, psola=psola), f, indent=2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", metavar="FOLDER")
    parser.add_argument("--study", choices=["granular", "psola"])
    args = parser.parse_args()
    if args.golden:
        write_golden(args.golden)
    if args.study == "granular":
        study_granular()
    elif args.study == "psola":
        study_psola()

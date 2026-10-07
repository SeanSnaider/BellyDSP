# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
# /// script
# requires-python = ">=3.11,<3.13"
# dependencies = ["numpy", "scipy", "soundfile"]
# ///

"""Are the built-in amps' gray-box sources different from the benchmark's hidden voicings? (docs/TONE_MATCH.md,
"More built-in amps"). If a built-in copied a hidden voicing, the benchmark would be testing itself.

    uv run prototypes/tone_bench/distinct.py [Amp ...]      # every built-in, or just these

For every step of every built-in set (its gray-box source, prototypes/amp_sim.py or amp_voicings.py, through
make_default_captures.gray_box, amp only) and every hidden gray-box voicing (rigs.graybox) with its drive set by
bisection so ITS distortion (the nonlinear energy ratio, NL) equals the step's, on the tests' 8 s guitar DI:

  spectrum   the RMS difference (dB) of the two long-term spectra in 1/3-octave bands from 80 Hz to 10 kHz,
             each spectrum's mean (in dB, over the bands) removed first, so level doesn't count, only shape
             (a cab and EQ can still change shape; this is what's left for them to fix);
  harmonics  the RMS difference (dB) of the 2nd to 8th harmonic levels relative to the fundamental of a
             110 Hz sine at -12 dBFS (each floored at -80 dB): the distortion's own character;
  crest      the difference of the crest factors (dB) on the DI.

For each built-in step it prints the nearest hidden voicing by spectrum + harmonics, and the same distances
between the hidden voicings themselves (each against its nearest other one at the same NL), as the scale: a
built-in is "as different from the hidden set as the hidden voicings are from each other" if its nearest
distance is in that range. Writes build/tone_bench/distinct.json.
"""

import json
import pathlib
import sys

import numpy as np
import scipy.signal as sps

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "prototypes"))
sys.path.insert(0, str(REPO / "tools/content"))

import make_default_captures as mdc  # noqa: E402
import rigs  # noqa: E402

FS = 48000
BANDS = 80.0 * 2.0 ** (np.arange(0, 22) / 3.0)          # 80 Hz to about 10 kHz, 1/3 octave


def band_spectrum_db(y):
    f, p = sps.welch(y, fs=FS, nperseg=8192)
    out = []
    for fc in BANDS:
        sel = (f >= fc * 2 ** (-1 / 6)) & (f < fc * 2 ** (1 / 6))
        out.append(10 * np.log10(np.mean(p[sel]) + 1e-20))
    out = np.array(out)
    return out - out.mean()


def harmonics_db(y, f0=110.0):
    n = FS
    s = np.abs(np.fft.rfft(y[-n:] * np.hanning(n)))
    k0 = int(round(f0))
    power = lambda k: np.sum(s[k - 2: k + 3] ** 2)
    fund = power(k0)
    return np.array([max(10 * np.log10(power(k0 * h) / fund + 1e-20), -80.0) for h in range(2, 9)])


def features(fn_di, fn_sine):
    y = fn_di()
    return dict(spectrum=band_spectrum_db(y), harmonics=harmonics_db(fn_sine()), crest=mdc.crest_db(y), nl=mdc.nl_ratio_db(DI, y))


def distance(a, b):
    sp = float(np.sqrt(np.mean((a["spectrum"] - b["spectrum"]) ** 2)))
    hm = float(np.sqrt(np.mean((a["harmonics"] - b["harmonics"]) ** 2)))
    return dict(spectrum=sp, harmonics=hm, crest=float(abs(a["crest"] - b["crest"])), total=sp + hm)


def hidden_at(name, nl):
    """A hidden voicing with its drive bisected so its NL on the DI is nl (or as close as -40..+30 dB allows)."""
    lo, hi = -40.0, 30.0
    f = lambda d: mdc.nl_ratio_db(DI, rigs.graybox(DI, name, d))
    if f(hi) < nl:
        d = hi
    elif f(lo) > nl:
        d = lo
    else:
        for _ in range(12):
            mid = 0.5 * (lo + hi)
            lo, hi = (mid, hi) if f(mid) < nl else (lo, mid)
        d = 0.5 * (lo + hi)
    return d, features(lambda: rigs.graybox(DI, name, d), lambda: rigs.graybox(SINE, name, d))


DI = mdc.read_mono(mdc.VOICING_DI)
SINE = mdc.sine()


def main():
    graybox_hidden = list(rigs.VOICINGS)
    out = {"builtins": [], "hidden_scale": []}
    # The scale: each hidden voicing against the others at its style's middle NL.
    mid_nl = {"clean": -34.0, "edge": -21.5, "crunch": -12.5, "high_gain": -7.25, "lead": -4.75}
    hidden_feats = {}
    for name in graybox_hidden:
        nl = mid_nl[rigs.VOICINGS[name]["style"]]
        hidden_feats[name] = {nl: hidden_at(name, nl)[1]}
    for name in graybox_hidden:
        nl = mid_nl[rigs.VOICINGS[name]["style"]]
        a = hidden_feats[name][nl]
        best = min(((o, distance(a, hidden_at(o, nl)[1])) for o in graybox_hidden if o != name), key=lambda t: t[1]["total"])
        out["hidden_scale"].append(dict(voicing=name, nl=nl, nearest=best[0], **best[1]))
        print(f"hidden {name:16s} at NL {nl:6.1f}: nearest other hidden {best[0]:16s} spectrum {best[1]['spectrum']:5.2f} dB, "
              f"harmonics {best[1]['harmonics']:5.2f} dB, crest {best[1]['crest']:4.1f} dB", flush=True)

    only = sys.argv[1:]          # optional: just these built-ins (e.g. Basalt)
    for amp, spec in mdc.AMPS.items():
        if only and amp not in only:
            continue
        work = REPO / "build/default_captures" / amp
        work.mkdir(parents=True, exist_ok=True)
        x = mdc.boosted(mdc.VOICING_DI, spec, work) if spec["boost_db"] is not None else DI
        xs = SINE
        if spec["boost_db"] is not None:
            tone = work / "sine_110_-12.wav"
            import soundfile as sf
            sf.write(str(tone), SINE, FS, subtype="FLOAT")
            xs = mdc.boosted(tone, spec, work)
        for g in mdc.STEPS:
            a = features(lambda: mdc.gray_box(x, spec, gain=g), lambda: mdc.gray_box(xs, spec, gain=g))
            rows = []
            for name in graybox_hidden:
                d, b = hidden_at(name, a["nl"])
                rows.append((name, d, distance(a, b), b["nl"]))
            name, d, dist, nl_h = min(rows, key=lambda r: r[2]["total"])
            out["builtins"].append(dict(amp=amp, gain=g, nl=a["nl"], nearest=name, nearest_drive_db=d, nearest_nl=nl_h, **dist))
            print(f"{amp:8s} gain {g:4g} (NL {a['nl']:6.1f}): nearest hidden {name:16s} (NL {nl_h:6.1f}) spectrum {dist['spectrum']:5.2f} dB, "
                  f"harmonics {dist['harmonics']:5.2f} dB, crest {dist['crest']:4.1f} dB", flush=True)
    path = REPO / "build/tone_bench/distinct.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(out, indent=1))
    print(f"wrote {path}")


if __name__ == "__main__":
    main()

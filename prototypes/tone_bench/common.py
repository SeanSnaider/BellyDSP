# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Shared plumbing for the tone match benchmark (docs/TONE_MATCH.md, "tone_bench"): paths, the render cache,
WAV I/O, and the app's own renderer (ampsim_render) as a function.

Everything that renders is cached on disk by content (the input's samples and the arguments), under
build/tone_bench/cache (gitignored), or $TONE_BENCH_CACHE.
"""

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sps

HERE = pathlib.Path(__file__).resolve().parent
PROTOTYPES = HERE.parent
REPO = PROTOTYPES.parent
sys.path.insert(0, str(PROTOTYPES))

import tone_match as tm  # noqa: E402  the matcher prototype (the C++ matcher's golden reference)

SR = 48000
RENDER = REPO / "build/ampsim_render_artefacts/Release/ampsim_render"
OUT = REPO / "build/tone_bench"
CACHE = pathlib.Path(os.environ.get("TONE_BENCH_CACHE", OUT / "cache"))
# The built-in gain sets first (in slot order), then every other gain set in the content folder by name, as the app
# searches them (ToneMatchSession::contentGainSets): a new set joins the matcher (and the oracle) by being there.
_BUILT_IN = ["Glass", "Ember", "Monolith"]
_OTHERS = sorted(p.parent.name for p in (REPO / "content/models").glob("*/gainset.json") if p.parent.name not in _BUILT_IN)
tm.AMPS = _BUILT_IN + _OTHERS
GAIN_SETS = [REPO / "content/models" / a / "gainset.json" for a in tm.AMPS]
IRS = REPO / "content/irs"
NAM_EXAMPLES = REPO / "third_party/NeuralAmpModelerCore/example_models"

# The matcher as the app runs it: the built-in amps are gain sets (tone_match.py's own golden fixtures still
# use the old single captures), and its renders go to our cache.
tm.MODEL_FILES = GAIN_SETS
tm.CACHE = CACHE / "matcher_renders"

TONE_ORDER = ["clean", "overdrive", "crunch", "hi_gain", "fuzz"]


def use_all_amps():
    """Every built-in gain set in content/models, reordered as the capture menu lists them (run.py --amps all): the
    three slot defaults stay slots 0 to 2 and the others follow by tone type, then name (presets::builtInGainSets).
    Since the merge of amp-coverage the default list above already holds every set (by name), and the matcher
    searches all of them either way (tone_match.py's range(len(MODEL_FILES))); this only changes the order, which
    the oracle's coverage report (coverage.md) was written against. Returns the names."""
    extra = []
    for js in sorted((REPO / "content/models").glob("*/gainset.json")):
        name = js.parent.name
        if name in tm.AMPS[:3]:
            continue
        tone = json.loads(js.read_text()).get("tone_type", "")
        extra.append((TONE_ORDER.index(tone) if tone in TONE_ORDER else len(TONE_ORDER), name, js))
    extra.sort()
    tm.AMPS = list(tm.AMPS[:3]) + [n for _, n, _ in extra]
    tm.MODEL_FILES = list(GAIN_SETS[:3]) + [js for _, _, js in extra]
    return tm.AMPS


# The worker processes import this module afresh, so the choice travels in the environment (run.py --amps).
if os.environ.get("TONE_BENCH_AMPS") == "all":
    use_all_amps()


def key_of(*parts):
    h = hashlib.sha1()
    for p in parts:
        if isinstance(p, np.ndarray):
            h.update(np.ascontiguousarray(p, dtype=np.float32).tobytes())
        else:
            h.update(json.dumps(p, sort_keys=True, default=str).encode())
        h.update(b"|")
    return h.hexdigest()[:20]


def cached(name, fn, *parts):
    """fn() cached on disk under a key of `parts` (arrays by content). fn returns an array (or a tuple of
    arrays of the same length, stored stacked)."""
    folder = CACHE / name.split("_")[0]
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{name}_{key_of(*parts)}.npy"
    if path.exists():
        return np.load(path)
    y = np.asarray(fn(), dtype=np.float64)
    tmp = path.with_suffix(".tmp.npy")
    np.save(tmp, y)
    os.replace(tmp, path)
    return y


def read_wav(path):
    sr, x = wavfile.read(path)
    if x.dtype.kind == "i":
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max + 1)
    return sr, np.asarray(x, dtype=np.float64)


def write_wav(path, x, sr=SR):
    wavfile.write(path, sr, np.asarray(x, dtype=np.float32))


def ampsim_render(x, args, name="render"):
    """x (48 kHz mono) through the app's chain with these ampsim_render arguments; the left channel. Cached."""
    args = [str(a) for a in args]

    def run():
        with tempfile.TemporaryDirectory() as tmp:
            src, dst = pathlib.Path(tmp) / "in.wav", pathlib.Path(tmp) / "out.wav"
            write_wav(src, x)
            r = subprocess.run([str(RENDER)] + args + [str(src), str(dst)], capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(" ".join(args) + "\n" + r.stdout + r.stderr)
            _, y = read_wav(dst)
        return y[:, 0] if y.ndim == 2 else y

    return cached(name, run, x, args)


def db(p):
    return 10.0 * np.log10(np.maximum(p, 1e-20))


def rms_db(x):
    return float(db(np.mean(np.square(x))))


def fit_length(y, n):
    y = np.asarray(y, dtype=np.float64)
    return np.concatenate([y, np.zeros(max(0, n - len(y)))])[:n]


def biquad(kind, f0, q=0.7071, gain_db=0.0, fs=SR):
    """RBJ cookbook biquads (Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients"):
    the bilinear transform of the analog prototypes with the centre frequency prewarped. Returns (b, a)."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * np.pi * f0 / fs
    cw, sw = np.cos(w0), np.sin(w0)
    alpha = sw / (2.0 * q)
    if kind == "lowpass":
        b, a = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2], [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "highpass":
        b, a = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2], [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "peak":
        b, a = [1 + alpha * A, -2 * cw, 1 - alpha * A], [1 + alpha / A, -2 * cw, 1 - alpha / A]
    elif kind == "lowshelf":
        sa = 2 * np.sqrt(A) * alpha
        b = [A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa)]
        a = [(A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa]
    elif kind == "highshelf":
        sa = 2 * np.sqrt(A) * alpha
        b = [A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa)]
        a = [(A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa]
    else:
        raise ValueError(kind)
    b, a = np.array(b), np.array(a)
    return b / a[0], a / a[0]


def filt(x, kind, f0, q=0.7071, gain_db=0.0, fs=SR):
    b, a = biquad(kind, f0, q, gain_db, fs)
    return sps.lfilter(b, a, x)


def apply_eq(x, bands):
    """bands: list of [kind, f, gain_db, q] with RBJ kinds (lowpass, highpass, peak, lowshelf, highshelf)."""
    for kind, f, g, q in bands:
        x = filt(x, kind, f, q, g)
    return x

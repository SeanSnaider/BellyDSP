# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Ranks the bundled cab IRs by how flat their response is from 200 Hz to 5 kHz.

    uv run --with numpy --with soundfile python tools/content/ir_flatness.py

Used to pick the factory presets' default cabs (docs/ASSUMPTIONS.md DS27): within each cab folder, the IR
with the flattest midrange is the most neutral starting point, the one that colours an amp capture the
least. It's a measurement, not a listening test; Sean picks by ear.

Method: the magnitude response |H(f)| of the whole file (as the app plays it: at most 1 s), smoothed to
1/6 octave (the mean power in a band from f / 2^(1/12) to f * 2^(1/12)) at 40 log-spaced points from
200 Hz to 5 kHz. "Flatness" is the standard deviation of those levels in dB: 0 would be a straight line.
A cab's overall tilt counts against it too, which is intended (a strongly tilted cab isn't neutral).
Also printed: the level of four bands relative to the 200 Hz to 5 kHz mean, to show where each IR leans.
"""

import pathlib

import numpy as np
import soundfile as sf

REPO = pathlib.Path(__file__).resolve().parents[2]
BANDS = [(80, 200), (200, 800), (800, 3000), (3000, 6000)]


def measure(path: pathlib.Path):
    h, rate = sf.read(str(path), always_2d=True)
    h = h[: int(rate), 0]  # the first channel, at most 1 s (CabIR::maxIRSeconds)
    n = 1 << 17
    power = np.abs(np.fft.rfft(h, n)) ** 2
    freqs = np.fft.rfftfreq(n, 1.0 / rate)

    def level(lo, hi):
        band = (freqs >= lo) & (freqs < hi)
        return 10.0 * np.log10(np.mean(power[band]))

    centres = np.geomspace(200.0, 5000.0, 40)
    smoothed = np.array([level(c * 2 ** (-1 / 12), c * 2 ** (1 / 12)) for c in centres])
    reference = level(200.0, 5000.0)
    return float(np.std(smoothed)), [level(lo, hi) - reference for lo, hi in BANDS]


def main() -> None:
    root = REPO / "content" / "irs"
    for folder in sorted(p for p in root.iterdir() if p.is_dir()):
        rows = sorted((measure(f) + (f.name,) for f in folder.glob("*.wav")), key=lambda r: r[0])
        print(f"{folder.name}: flatness (std dev of 1/6-octave levels, 200 Hz to 5 kHz), flattest first")
        print(f"  {'dB':>5s}   " + "  ".join(f"{lo}-{hi}" for lo, hi in BANDS) + " Hz, dB re the 200-5k mean")
        for flatness, bands, name in rows:
            print(f"  {flatness:5.2f}   " + "  ".join(f"{b:+7.1f}" for b in bands) + f"   {name}")


if __name__ == "__main__":
    main()

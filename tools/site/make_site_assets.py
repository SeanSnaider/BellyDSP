# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=10", "fonttools>=4.47", "brotli>=1.1"]
# ///
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Makes the website's images and fonts from the app's own files. Run it by hand when the app's look
changes (docs/WEBSITE.md, "Updating screenshots"); its outputs are committed, so the Pages build needs
nothing but plain Python.

    uv run tools/site/make_site_assets.py [--proof build/proof]

Screenshots: the editor snapshots the test suite writes (`ampsim_tests --proof-dir build/proof`, at 2x:
2560 x 1520 for the 1280 x 760 canvas) become WebP files in two widths, site/assets/img/<name>-<width>.webp,
for the pages' srcset. Fonts: Geist (resources/fonts/Geist, SIL OFL 1.1) becomes WOFF2, unchanged apart
from the container (no subsetting, so it's still the Font Software as distributed), with its licence.
"""

import argparse
import shutil
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# site name: (proof snapshot, widths). The first is the overview's hero.
SHOTS = {
    "amp-glass": ("editor_amp_glass.png", (1280, 1920)),
    "amp-ember": ("editor_amp_ember.png", (640, 1280)),
    "amp-monolith": ("editor_amp_monolith.png", (640, 1280)),
    "cab": ("editor_cab_builtin.png", (640, 1280)),
    "harmonizer": ("editor_harmonizer.png", (640, 1280)),
    "bloom": ("editor_bloom.png", (640, 1280)),
    "delay": ("editor_delay.png", (640, 1280)),
    "reverb": ("editor_reverb.png", (640, 1280)),
    "tuner": ("editor_tuner_in_tune.png", (640, 1280)),
}
FONTS = ["Geist-Regular.ttf", "Geist-Medium.ttf", "Geist-SemiBold.ttf"]


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--proof", default=str(REPO_ROOT / "build" / "proof"))
    args = p.parse_args(argv[1:])
    proof = Path(args.proof)
    img_dir = REPO_ROOT / "site" / "assets" / "img"
    font_dir = REPO_ROOT / "site" / "assets" / "fonts"
    img_dir.mkdir(parents=True, exist_ok=True)
    font_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    for name, (src, widths) in SHOTS.items():
        im = Image.open(proof / src).convert("RGB")
        for w in widths:
            h = round(im.height * w / im.width)
            out = img_dir / f"{name}-{w}.webp"
            im.resize((w, h), Image.LANCZOS).save(out, "WEBP", quality=80, method=6)
            total += out.stat().st_size
            print(f"{out.relative_to(REPO_ROOT)}  {w} x {h}  {out.stat().st_size / 1024:.1f} KB  (from {src})")
    print(f"images: {total / 1024:.1f} KB")

    src_fonts = REPO_ROOT / "resources" / "fonts" / "Geist"
    for f in FONTS:
        font = TTFont(src_fonts / f)
        font.flavor = "woff2"
        out = font_dir / f.replace(".ttf", ".woff2")
        font.save(out)
        print(f"{out.relative_to(REPO_ROOT)}  {out.stat().st_size / 1024:.1f} KB  (ttf {(src_fonts / f).stat().st_size / 1024:.1f} KB)")
    shutil.copy(src_fonts / "OFL.txt", font_dir / "OFL.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

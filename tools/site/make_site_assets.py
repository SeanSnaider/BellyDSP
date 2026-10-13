# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=10", "fonttools>=4.47", "brotli>=1.1", "numpy>=1.26"]
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

import numpy as np
from fontTools.ttLib import TTFont
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# site name: (proof snapshot, widths).
SHOTS = {
    # The Amp page with its shelf of eight amps (the Built-in captures tests write it to amp_shelf/); also og:image.
    "amp-glass": ("amp_shelf/editor_amp_glass.png", (1280, 1920), None),
    # Match tone told as its three steps (the tone match page tests write them to tone_match/), each cropped to the
    # part that tells its step, at a size its text can be read: the song's card, the take recording against it, and
    # the whole result. Crops are (left, top, right, bottom) in the 2560 x 1520 snapshot.
    "tm-song": ("tone_match/20_play_target.png", (720, 1440), (78, 278, 1120, 824)),        # the Target card
    "tm-take": ("tone_match/22_recording.png", (480, 960), (1142, 278, 1712, 824)),        # the Your DI card, recording
    "tm-match": ("tone_match/11_compare_playing_match.png", (1280, 2400), (78, 278, 2480, 1344)),  # the three cards and the Result
    "tm-result": ("tone_match/11_compare_playing_match.png", (720, 1440), (78, 846, 1240, 1344)),   # the Result's text, for phones
    # The rest of the rig, each cropped to its subject: the cab's speaker and mics, the tuner's reading, the
    # harmonizer's voice table.
    "rig-cab": ("editor_cab_builtin.png", (720, 1440), (820, 120, 2540, 1366)),
    "rig-tuner": ("editor_tuner_in_tune.png", (720, 1440), (480, 240, 2100, 1320)),
    "rig-harmonizer": ("editor_harmonizer.png", (1280, 2400), (80, 240, 2540, 1080)),
}
# (folder in resources/fonts, file): Geist for everything, Fraunces SemiBold Italic for the amp names whose badges
# use it (Ember, Lantern), as the app's heads do.
FONTS = [("Geist", "Geist-Light.ttf"), ("Geist", "Geist-Regular.ttf"), ("Geist", "Geist-Medium.ttf"),
         ("Geist", "Geist-SemiBold.ttf"), ("Fraunces", "Fraunces-SemiBoldItalic.ttf")]

# The overview's amp bands show each amp's head as the app draws it, knobs included, on the band's own material.
# The test suite renders the head art alone with its transparency (amp_head_<material>.png, Editor tests) and the
# whole Amp page with each amp on the shelf (amp_shelf/editor_amp_<amp>.png, Built-in captures tests), both at 2x.
# The head is cut out of the page snapshot with the render's alpha: the render is aligned to the snapshot by the
# smallest mean difference over its opaque pixels (the knobs, which the render lacks, are a small part), the
# snapshot's pixels are kept where the head is opaque, and the render's own pixels (its shadow) everywhere else.
HEADS = ["Glass", "Ember", "Monolith", "Lantern", "Basalt", "Comet", "Forge", "Quartz"]
HEAD_WIDTHS = (800, 1400)


def cut_head(proof, amp):
    head = Image.open(proof / f"amp_head_{amp.lower()}.png").convert("RGBA")
    shot = Image.open(proof / "amp_shelf" / f"editor_amp_{amp.lower()}.png").convert("RGB")
    h = np.asarray(head).astype(np.float32)
    s = np.asarray(shot).astype(np.float32)
    alpha = h[..., 3]
    ys, xs = np.nonzero(alpha[::6, ::6] >= 254)
    ys, xs = ys * 6, xs * 6
    head_grey = h[ys, xs, :3].mean(axis=1)
    shot_grey = s.mean(axis=2)
    best = (float("inf"), 0, 0)
    for oy in range(0, shot.height - head.height + 1, 2):
        for ox in range(0, shot.width - head.width + 1, 2):
            d = np.abs(shot_grey[ys + oy, xs + ox] - head_grey).mean()
            if d < best[0]:
                best = (d, ox, oy)
    d, ox, oy = best
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            y, x = oy + dy, ox + dx
            if 0 <= y <= shot.height - head.height and 0 <= x <= shot.width - head.width:
                dd = np.abs(shot_grey[ys + y, xs + x] - head_grey).mean()
                if dd < d:
                    d, best = dd, (dd, x, y)
    _, ox, oy = best
    out = np.asarray(shot.crop((ox, oy, ox + head.width, oy + head.height)).convert("RGBA")).copy()
    solid = alpha >= 250
    out[~solid, :3] = h[~solid, :3].astype(np.uint8)
    out[..., 3] = alpha.astype(np.uint8)
    if d > 12:
        raise SystemExit(f"{amp}: the head render doesn't line up with the Amp page snapshot (mean difference {d:.1f})")
    return Image.fromarray(out, "RGBA"), (ox, oy, d)


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
    for name, (src, widths, crop) in SHOTS.items():
        im = Image.open(proof / src).convert("RGB")
        if crop:
            im = im.crop(crop)
        for w in widths:
            h = round(im.height * w / im.width)
            out = img_dir / f"{name}-{w}.webp"
            im.resize((w, h), Image.LANCZOS).save(out, "WEBP", quality=80, method=6)
            total += out.stat().st_size
            print(f"{out.relative_to(REPO_ROOT)}  {w} x {h}  {out.stat().st_size / 1024:.1f} KB  (from {src})")
    for amp in HEADS:
        im, (ox, oy, d) = cut_head(proof, amp)
        for w in HEAD_WIDTHS:
            h = round(im.height * w / im.width)
            out = img_dir / f"head-{amp.lower()}-{w}.webp"
            im.resize((w, h), Image.LANCZOS).save(out, "WEBP", quality=82, method=6, exact=True)
            total += out.stat().st_size
            print(f"{out.relative_to(REPO_ROOT)}  {w} x {h}  {out.stat().st_size / 1024:.1f} KB  (cut at {ox},{oy}, difference {d:.1f})")
    print(f"images: {total / 1024:.1f} KB")

    fonts_root = REPO_ROOT / "resources" / "fonts"
    for folder, f in FONTS:
        src = fonts_root / folder / f
        font = TTFont(src)
        font.flavor = "woff2"
        out = font_dir / f.replace(".ttf", ".woff2")
        font.save(out)
        print(f"{out.relative_to(REPO_ROOT)}  {out.stat().st_size / 1024:.1f} KB  (ttf {src.stat().st_size / 1024:.1f} KB)")
    shutil.copy(fonts_root / "Geist" / "OFL.txt", font_dir / "OFL.txt")
    shutil.copy(fonts_root / "Fraunces" / "OFL.txt", font_dir / "OFL-Fraunces.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=10", "fonttools>=4.47", "brotli>=1.1"]
# ///

"""Makes the website's images, tab icons and fonts from the app's own files. Run it by hand when the app's
look changes (docs/WEBSITE.md, "Updating the site"); its outputs are committed, so the Pages build needs
nothing but plain Python.

    uv run tools/site/make_site_assets.py [--proof build/proof]

Screenshots: the redesigned app's snapshots the test suite writes (`ampsim_tests --proof-dir build/proof`, at
2x: 2560 x 1520 for the 1280 x 760 canvas, in build/proof/redesign/) become WebP files in two widths,
site/assets/img/<name>-<width>.webp, for the pages' srcset, each cropped to its subject. Heads: each built-in
amp's head as the stage dresses it (its art, the lit jewel lamp and its seven knobs, on a transparent ground
with its shadow), which the Editor parts tests save alone at 2x (build/proof/site/head_<material>.png).
Tab icons: the app icon, the "B." mark (resources/icon). Fonts: Geist and Fraunces (resources/fonts, SIL OFL
1.1) become WOFF2, unchanged apart from the container (no subsetting, so they're still the Font Software as
distributed), with their licences.
"""

import argparse
import shutil
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# site name: (snapshot in build/proof, widths, crop). Crops are (left, top, right, bottom) in the 2x snapshot. The
# overlays' panels are cut at their own 1px borders (their rounded corners show the app's ground, which is the
# site's), so the page shows them without a frame of its own; the stage and the inspector, which have no border, are
# cut at the edges of their areas and framed. The widest width is the crop's own (never scaled up).
SHOTS = {
    # The main screen: the preset bar, the amp on the stage with the shelf of minis, the board, the inspector and the
    # scenes (Main screen tests). The rig section's big shot, and og:image.
    "main": ("redesign/main_amp.png", (1280, 1920), None),
    # Match tone told by its six-step page (the tone match tests copy each state to redesign/overlay_tone_match_*):
    # step 1, the song, playing its section; step 2, your DI, recording along with it; and steps 4 to 6 after a
    # match (the result, compare playing the match, apply), with the result's panel alone for phones.
    "tm-song": ("redesign/overlay_tone_match_play_target.png", (640, 878), (80, 232, 958, 824)),
    "tm-take": ("redesign/overlay_tone_match_take_recording.png", (480, 686), (984, 232, 1670, 824)),
    "tm-result": ("redesign/overlay_tone_match_compare_playing_match.png", (1280, 2400), (80, 844, 2480, 1332)),
    "tm-result-panel": ("redesign/overlay_tone_match_compare_playing_match.png", (640, 1176), (80, 844, 1256, 1332)),
    # The rest of the rig: the stage in Cab mode with the cab's inspector beside it (the stage's whole height, top bar
    # to board); the tuner overlay's display and its strings and tunings; the harmonizer's inspector down to its first
    # voice, at the tuner's height so the two sit side by side at one scale.
    "rig-cab": ("redesign/stage_cab.png", (1280, 2556), (2, 104, 2558, 804)),
    "rig-tuner": ("redesign/overlay_tuner_in_tune.png", (1280, 2400), (80, 248, 2480, 1332)),
    "rig-harmonizer": ("redesign/inspector_harmonizer.png", (291, 582), (0, 0, 582, 1084)),
}
# (folder in resources/fonts, file): Geist for everything, Fraunces SemiBold Italic for the amp names whose badges
# use it (Ember, Lantern), as the app's heads do.
FONTS = [("Geist", "Geist-Light.ttf"), ("Geist", "Geist-Regular.ttf"), ("Geist", "Geist-Medium.ttf"),
         ("Geist", "Geist-SemiBold.ttf"), ("Fraunces", "Fraunces-SemiBoldItalic.ttf")]

# The overview's amp bands show each amp's head as the stage draws it, on the band's own material.
HEADS = ["Glass", "Ember", "Monolith", "Lantern", "Basalt", "Comet", "Forge", "Quartz"]
HEAD_WIDTHS = (800, 1400)

# The tab icons, from the app icon: (file in site/, size in px).
ICON = REPO_ROOT / "resources" / "icon" / "BellyDSP_icon_1024.png"
TAB_ICONS = [("favicon-32.png", 32), ("favicon-192.png", 192), ("apple-touch-icon.png", 180)]


def save_widths(im, name, widths, img_dir, note, **webp):
    total = 0
    for w in widths:
        if w > im.width:
            raise SystemExit(f"{name}: {w} px is wider than its source ({im.width} px)")
        h = round(im.height * w / im.width)
        out = img_dir / f"{name}-{w}.webp"
        (im if w == im.width else im.resize((w, h), Image.LANCZOS)).save(out, "WEBP", method=6, **webp)
        total += out.stat().st_size
        print(f"{out.relative_to(REPO_ROOT)}  {w} x {h}  {out.stat().st_size / 1024:.1f} KB  ({note})")
    return total


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
        total += save_widths(im, name, widths, img_dir, f"from {src}", quality=80)
    for amp in HEADS:
        src = proof / "site" / f"head_{amp.lower()}.png"
        im = Image.open(src).convert("RGBA")
        if im.getpixel((0, 0))[3] != 0:
            raise SystemExit(f"{src} isn't transparent around the head")
        total += save_widths(im, f"head-{amp.lower()}", HEAD_WIDTHS, img_dir, f"from site/{src.name}", quality=82, exact=True)
    print(f"images: {total / 1024:.1f} KB")

    icon = Image.open(ICON).convert("RGBA")
    for f, size in TAB_ICONS:
        out = REPO_ROOT / "site" / f
        icon.resize((size, size), Image.LANCZOS).save(out, "PNG", optimize=True)
        print(f"{out.relative_to(REPO_ROOT)}  {size} x {size}  {out.stat().st_size / 1024:.1f} KB  (from {ICON.relative_to(REPO_ROOT)})")

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

# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider
#
# /// script
# dependencies = ["pillow"]
# ///
"""BellyDSP's app icon (the Dock on macOS, the taskbar on Windows): a white "B" in Geist SemiBold with the
emerald brand dot as its period, on the app's near-black, in macOS's icon grid (an 824 px rounded square in a
1024 px canvas, corner radius 185, transparent around it). Sean picked this "B." among three options on
2026-10-06. CMake hands resources/icon/BellyDSP_icon_1024.png to JUCE (ICON_BIG), which makes the .icns and
.ico from it.

    uv run tools/content/make_icon.py        (writes resources/icon/BellyDSP_icon_1024.png)

The colours are the UI tokens (docs/ui/amp-ui-handoff): bg #0b0c0c, line-2 #2a2f2d, ink #e6ebe9, accent
#34d399. Drawn at 4x and downsampled, so the edges are smooth. Geist is OFL (resources/fonts/Geist/OFL.txt),
which allows rendering it into an image like this.
"""

from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "resources" / "icon" / "BellyDSP_icon_1024.png"

SIZE, SUPERSAMPLE = 1024, 4
BG, EDGE, INK, ACCENT = (11, 12, 12), (42, 47, 45), (230, 235, 233), (52, 211, 153)


def main() -> None:
    w = SIZE * SUPERSAMPLE
    image = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    margin, radius = (1024 - 824) // 2 * SUPERSAMPLE, 185 * SUPERSAMPLE
    draw.rounded_rectangle([margin, margin, w - margin, w - margin], radius=radius, fill=BG + (255,),
                           outline=EDGE + (255,), width=3 * SUPERSAMPLE)

    # The "B", a little left of centre so the "B." pair sits centred.
    font = ImageFont.truetype(str(ROOT / "resources/fonts/Geist/Geist-SemiBold.ttf"), 520 * SUPERSAMPLE)
    box = draw.textbbox((0, 0), "B", font=font)
    x = (w - (box[2] - box[0])) // 2 - 60 * SUPERSAMPLE - box[0]
    y = (w - (box[3] - box[1])) // 2 - box[1] - 10 * SUPERSAMPLE
    draw.text((x, y), "B", font=font, fill=INK + (255,))

    # The dot: on the baseline, with a soft glow like the UI's pilot light.
    r = 52 * SUPERSAMPLE
    cx, cy = x + box[2] + 48 * SUPERSAMPLE + r, y + box[3] - r
    glow = Image.new("RGBA", (w, w), (0, 0, 0, 0))
    gr = int(r * 2.2)
    ImageDraw.Draw(glow).ellipse([cx - gr, cy - gr, cx + gr, cy + gr], fill=ACCENT + (80,))
    # The glow stays inside the rounded square: clip its alpha to the square's shape, or it hazes past the
    # icon's edge on a light desktop.
    glow = glow.filter(ImageFilter.GaussianBlur(gr * 0.6))
    shape = Image.new("L", (w, w), 0)
    ImageDraw.Draw(shape).rounded_rectangle([margin, margin, w - margin, w - margin], radius=radius, fill=255)
    glow.putalpha(ImageChops.multiply(glow.getchannel("A"), shape))
    image = Image.alpha_composite(image, glow)
    ImageDraw.Draw(image).ellipse([cx - r, cy - r, cx + r, cy + r], fill=ACCENT + (255,))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    image.resize((SIZE, SIZE), Image.LANCZOS).save(OUT)
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()

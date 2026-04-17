#!/usr/bin/env python3
"""Generate the Stitch logo PNG in a few sizes.

Renders a rounded-square icon with two overlapping "monitor" shapes
joined by a dashed stitch line — matching the mental model of the app.

Usage:
    python scripts/generate_logo.py
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"


def generate(size: int) -> Image.Image:
    s = size
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    pad = int(s * 0.031)
    radius = int(s * 0.22)

    # Subtle drop-shadow under the rounded tile.
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    sd.rounded_rectangle(
        (pad + 2, pad + 4, s - pad + 2, s - pad + 4),
        radius=radius, fill=(0, 0, 0, 120),
    )
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=max(2, s // 40)))
    img.alpha_composite(shadow)

    # Rounded tile with a gentle vertical gradient.
    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    for y in range(s):
        t = y / max(1, s - 1)
        r = int(0x1e * (1 - t) + 0x11 * t)
        g = int(0x21 * (1 - t) + 0x13 * t)
        b = int(0x3a * (1 - t) + 0x2a * t)
        ImageDraw.Draw(tile).line([(0, y), (s, y)], fill=(r, g, b, 255))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (pad, pad, s - pad, s - pad), radius=radius, fill=255,
    )
    img.paste(tile, (0, 0), mask)

    # Monitor 1 (coral red)
    m1 = (int(s * 0.18), int(s * 0.29), int(s * 0.62), int(s * 0.62))
    draw.rounded_rectangle(m1, radius=int(s * 0.04), fill=(233, 69, 96, 255))
    # Stand under monitor 1
    stand1 = (int(s * 0.31), int(s * 0.63), int(s * 0.49), int(s * 0.66))
    draw.rounded_rectangle(stand1, radius=max(1, s // 128),
                           fill=(255, 255, 255, 60))

    # Monitor 2 (cool blue) — offset to overlap monitor 1
    m2 = (int(s * 0.38), int(s * 0.42), int(s * 0.82), int(s * 0.75))
    draw.rounded_rectangle(m2, radius=int(s * 0.04), fill=(74, 144, 217, 255))
    stand2 = (int(s * 0.51), int(s * 0.76), int(s * 0.69), int(s * 0.79))
    draw.rounded_rectangle(stand2, radius=max(1, s // 128),
                           fill=(255, 255, 255, 60))

    # Stitch line connecting the two.
    def dashed_curve():
        import math
        p0 = (int(s * 0.23), int(s * 0.55))
        p1 = (int(s * 0.50), int(s * 0.40))  # control
        p2 = (int(s * 0.77), int(s * 0.61))
        steps = 96
        pts = []
        for i in range(steps + 1):
            t = i / steps
            x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t ** 2 * p2[0]
            y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t ** 2 * p2[1]
            pts.append((x, y))
        return pts

    pts = dashed_curve()
    on, off = 6, 5
    i = 0
    while i < len(pts) - 1:
        segment = pts[i: i + on]
        if len(segment) > 1:
            draw.line(segment, fill=(255, 255, 255, 235),
                      width=max(2, s // 64))
        i += on + off

    # Endpoint dots.
    for p in (pts[0], pts[-1]):
        r = max(2, s // 52)
        draw.ellipse((p[0] - r, p[1] - r, p[0] + r, p[1] + r),
                     fill=(255, 255, 255, 255))

    return img


def main():
    ASSETS.mkdir(exist_ok=True)
    for size in (16, 32, 48, 64, 128, 256, 512):
        out = ASSETS / f"logo_{size}.png"
        generate(size).save(out, "PNG", optimize=True)
        print(f"  {out.relative_to(ROOT)}  ({size}x{size})")
    # Default logo.png points at the 256px version.
    (ASSETS / "logo.png").write_bytes((ASSETS / "logo_256.png").read_bytes())
    print(f"  {(ASSETS / 'logo.png').relative_to(ROOT)}  (alias of 256px)")


if __name__ == "__main__":
    main()

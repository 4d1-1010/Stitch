"""Regenerate unIO's nav-rail icons in one lilac, outlined language.

Every icon (Activity, Layout, Settings, Access, Help) is rendered
here to keep the set coherent — same stroke weight, same colour,
same drawing idiom. The Access tab uses a key glyph (a small
round head, a shaft, two teeth at the tip).

Canvas: 28×28, 2 px outlined strokes, rounded joints, LILAC
(#8b7bff) colour, no fills. Run from the repo root (or anywhere,
it writes next to itself):

    python assets/generate_icons.py
"""

from __future__ import annotations

import math
import pathlib

from PIL import Image, ImageDraw


ASSETS = pathlib.Path(__file__).resolve().parent

SIZE = 28
STROKE = "#8b7bff"   # LILAC
STROKE_W = 2


def _canvas() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def _save(img: Image.Image, name: str) -> None:
    out = ASSETS / name
    img.save(out, "PNG")
    print(f"  {out.relative_to(ASSETS.parent)}  ({img.size[0]}×{img.size[1]})")


# ── Glyphs ──────────────────────────────────────────────────────

def activity() -> Image.Image:
    img, d = _canvas()
    pts = [(3, 14), (8, 14), (11, 9), (15, 19), (19, 12), (25, 14)]
    d.line(pts, fill=STROKE, width=STROKE_W, joint="curve")
    return img


def layout() -> Image.Image:
    img, d = _canvas()
    d.rounded_rectangle([3, 7, 13, 21], radius=2,
                        outline=STROKE, width=STROKE_W)
    d.rounded_rectangle([15, 7, 25, 21], radius=2,
                        outline=STROKE, width=STROKE_W)
    return img


def settings() -> Image.Image:
    img, d = _canvas()
    rows = [(7, 10), (14, 18), (21, 14)]  # (y, knob_x)
    for y, kx in rows:
        d.line([(3, y), (25, y)], fill=STROKE, width=STROKE_W)
        d.ellipse([kx - 3, y - 3, kx + 3, y + 3],
                  outline=STROKE, width=STROKE_W,
                  fill=(255, 255, 255, 255))
    return img


def access() -> Image.Image:
    """Key glyph — round head with a small inner hole, a shaft
    running to the right, and two teeth pointing down at the tip.
    Replaces the previous user-silhouette "Account" icon."""
    img, d = _canvas()
    # Head
    d.ellipse([3, 9, 13, 19], outline=STROKE, width=STROKE_W)
    # Inner hole
    d.ellipse([7, 13, 9, 15], fill=STROKE)
    # Shaft
    d.line([(13, 14), (25, 14)], fill=STROKE, width=STROKE_W)
    # Teeth
    d.line([(20, 14), (20, 18)], fill=STROKE, width=STROKE_W)
    d.line([(24, 14), (24, 17)], fill=STROKE, width=STROKE_W)
    return img


def help() -> Image.Image:
    img, d = _canvas()
    d.ellipse([3, 3, 25, 25], outline=STROKE, width=STROKE_W)
    d.arc([10, 7, 18, 15], start=180, end=360,
          fill=STROKE, width=STROKE_W)
    d.line([(17, 12), (14, 16)], fill=STROKE, width=STROKE_W,
           joint="curve")
    d.ellipse([13, 19, 15, 21], fill=STROKE)
    return img


# ── Entrypoint ──────────────────────────────────────────────────

ICONS = {
    "icon_tab_activity_28.png": activity,
    "icon_tab_layout_28.png":   layout,
    "icon_tab_settings_28.png": settings,
    "icon_access_28.png":       access,
    "icon_help_28.png":         help,
}


def main_entry() -> None:
    print("Generating lilac icons →", ASSETS)
    for name, fn in ICONS.items():
        _save(fn(), name)
    print("Done.")


if __name__ == "__main__":
    main_entry()

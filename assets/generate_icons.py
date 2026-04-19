"""Regenerate unIO's nav icons in one consistent Lucide-style language.

All six icons (three tabs + three mini-rail sections) share:

  * 28×28 PNG canvas with 2 px padding → 24×24 drawing grid
  * 2 px outlined strokes, rounded line joints/caps
  * No fills — stroke-only — so the eye reads the set as one family
  * Single colour (PAPER_MUTED). Active-state tint lives on the
    adjacent label text, not on the icon itself

The previous icons mixed three styles (filled tab set, half-filled
mini-rail set, thin-outlined "?"); redrawing them here gets them
all speaking one language.

Run from the repo root (or anywhere — the script writes next to
itself):

    python assets/generate_icons.py
"""

from __future__ import annotations

import math
import pathlib

from PIL import Image, ImageDraw


ASSETS = pathlib.Path(__file__).resolve().parent

SIZE = 28
STROKE = "#6d7286"   # PAPER_MUTED
STROKE_W = 2


def _canvas() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def _save(img: Image.Image, name: str) -> None:
    out = ASSETS / name
    img.save(out, "PNG")
    print(f"  {out.relative_to(ASSETS.parent)}  ({img.size[0]}×{img.size[1]})")


# ── Tab icons ───────────────────────────────────────────────────

def activity() -> Image.Image:
    """Heartbeat / pulse line. Classic activity metaphor."""
    img, d = _canvas()
    pts = [(3, 14), (8, 14), (11, 9), (15, 19), (19, 12), (25, 14)]
    d.line(pts, fill=STROKE, width=STROKE_W, joint="curve")
    return img


def layout() -> Image.Image:
    """Two monitors side-by-side — matches the 'arrange your
    displays' meaning of the Layout tab."""
    img, d = _canvas()
    d.rounded_rectangle([3, 7, 13, 21], radius=2,
                        outline=STROKE, width=STROKE_W)
    d.rounded_rectangle([15, 7, 25, 21], radius=2,
                        outline=STROKE, width=STROKE_W)
    return img


def settings() -> Image.Image:
    """Three horizontal sliders with knobs — reads as 'tune things'
    without needing to render a many-toothed gear at 28 px."""
    img, d = _canvas()
    rows = [(7, 10), (14, 18), (21, 14)]  # (y, knob_x) per row
    for y, kx in rows:
        d.line([(3, y), (25, y)], fill=STROKE, width=STROKE_W)
        d.ellipse([kx - 3, y - 3, kx + 3, y + 3],
                  outline=STROKE, width=STROKE_W,
                  fill=(255, 255, 255, 255))  # hollow
    return img


# ── Mini-rail icons ─────────────────────────────────────────────

def main() -> Image.Image:
    """2×2 squares — the 'workspace / all sections' section marker."""
    img, d = _canvas()
    for x in (3, 16):
        for y in (3, 16):
            d.rounded_rectangle([x, y, x + 9, y + 9], radius=2,
                                outline=STROKE, width=STROKE_W)
    return img


def account() -> Image.Image:
    """Lucide-style user glyph: head circle + shoulders arc.

    Drawn as outlines so it matches the rest of the set; the old PNG
    was a filled silhouette which made Account feel heavier than the
    tabs above it."""
    img, d = _canvas()
    # Head
    d.ellipse([10, 4, 18, 12], outline=STROKE, width=STROKE_W)
    # Shoulders — top half of an ellipse centred below the head.
    # PIL arc angles are degrees clockwise from 3 o'clock (east), so
    # 180→360 traces left → top → right = the upward shoulder curve.
    d.arc([5, 14, 23, 32], start=180, end=360,
          fill=STROKE, width=STROKE_W)
    return img


def help() -> Image.Image:
    """Question mark inside a circle."""
    img, d = _canvas()
    # Outer circle
    d.ellipse([3, 3, 25, 25], outline=STROKE, width=STROKE_W)
    # "?" hook — top half of a small circle, a short vertical tail,
    # and a dot below. Not a perfect Lucide ?, but at 28 px the
    # strokes read cleanly and match the rest of the icon set.
    d.arc([10, 7, 18, 15], start=180, end=360,
          fill=STROKE, width=STROKE_W)
    # The top-hook arc ends at its rightmost point (18, 11); step
    # inward and down to join the vertical stem so the ? doesn't
    # look truncated.
    d.line([(17, 12), (14, 16)], fill=STROKE, width=STROKE_W,
           joint="curve")
    # Dot
    d.ellipse([13, 19, 15, 21], fill=STROKE)
    return img


# ── Entrypoint ──────────────────────────────────────────────────

ICONS = {
    "icon_tab_activity_28.png": activity,
    "icon_tab_layout_28.png":   layout,
    "icon_tab_settings_28.png": settings,
    "icon_tab_main_28.png":     main,
    "icon_account_28.png":      account,
    "icon_help_28.png":         help,
}


def main_entry() -> None:
    print("Generating icons →", ASSETS)
    for name, fn in ICONS.items():
        _save(fn(), name)
    print("Done.")


if __name__ == "__main__":
    main_entry()

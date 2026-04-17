"""Shared styling helpers for the Stitch tkinter UI."""

from __future__ import annotations

import tkinter as tk
from pathlib import Path
from typing import Optional

from PIL import Image, ImageDraw, ImageFilter, ImageTk


# ── Palette ──────────────────────────────────────────────────────

BG_DARK = "#0e1024"
BG_MID = "#1a1d3a"
PANEL_TINT = (24, 28, 56, 165)
PANEL_BORDER = (255, 255, 255, 36)
TEXT = "#eef1ff"
TEXT_DIM = "#9ba0c7"
TEXT_FAINT = "#6d7295"
ACCENT = "#e94560"
ACCENT_2 = "#7aa2f7"
ACCENT_3 = "#9b8cff"

FONT = "Helvetica"


# ── Backdrop ─────────────────────────────────────────────────────

def render_backdrop(width: int, height: int) -> Image.Image:
    base = Image.new("RGB", (width, height), (14, 16, 36))
    draw = ImageDraw.Draw(base)
    for y in range(height):
        t = y / max(1, height - 1)
        r = int(0x18 * (1 - t) + 0x0a * t)
        g = int(0x1b * (1 - t) + 0x0b * t)
        b = int(0x3a * (1 - t) + 0x22 * t)
        draw.line([(0, y), (width, y)], fill=(r, g, b))

    orbs = [
        (int(width * 0.18), int(height * 0.25), 233, 69, 96, int(width * 0.32)),
        (int(width * 0.82), int(height * 0.30), 122, 162, 247, int(width * 0.36)),
        (int(width * 0.55), int(height * 0.85), 155, 140, 255, int(width * 0.42)),
    ]
    orb_layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    od = ImageDraw.Draw(orb_layer)
    for cx, cy, r, g, b, rad in orbs:
        od.ellipse((cx - rad, cy - rad, cx + rad, cy + rad),
                   fill=(r, g, b, 170))
    orb_layer = orb_layer.filter(
        ImageFilter.GaussianBlur(radius=max(30, width // 10)))
    base.paste(orb_layer, (0, 0), orb_layer)
    return base


def panel_image(
    width: int, height: int,
    radius: int = 22,
    tint: tuple[int, int, int, int] = PANEL_TINT,
    border: tuple[int, int, int, int] = PANEL_BORDER,
) -> Image.Image:
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, width - 1, height - 1),
                        radius=radius, fill=tint, outline=border, width=1)
    highlight = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    hd = ImageDraw.Draw(highlight)
    hd.rounded_rectangle((2, 2, width - 3, min(height - 3, radius * 2)),
                         radius=radius, fill=(255, 255, 255, 10))
    img.alpha_composite(highlight)
    return img


# ── Tk helpers ───────────────────────────────────────────────────

def set_window_icon(root: tk.Misc) -> None:
    """Attach the Stitch PNG icon to the given Tk window."""
    images = []
    for p in _icon_paths():
        if not p.exists():
            continue
        try:
            images.append(tk.PhotoImage(file=str(p), master=root))
        except tk.TclError:
            continue
    if not images:
        return
    # Keep references on the widget so Python GC doesn't free the PhotoImages
    # out from under Tk (which would make the icon disappear).
    existing = getattr(root, "_stitch_icon_refs", [])
    root._stitch_icon_refs = existing + images  # type: ignore[attr-defined]
    try:
        root.iconphoto(True, *images)
    except tk.TclError:
        pass


def make_root(title: str = "Stitch") -> tk.Tk:
    """Create a Tk root with the Stitch WM class + title baked in."""
    root = tk.Tk(className="Stitch")
    root.title(title)
    return root


def _icon_paths() -> list[Path]:
    assets = Path(__file__).resolve().parents[2] / "assets"
    sizes = (256, 128, 64, 48, 32, 16)
    return [assets / f"logo_{s}.png" for s in sizes]


class Backdrop:
    def __init__(self, parent: tk.Misc, canvas: tk.Canvas):
        self.canvas = canvas
        self.parent = parent
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._bg_id: Optional[int] = None
        self._last_size = (0, 0)
        canvas.bind("<Configure>", self._on_configure)

    def _on_configure(self, event):
        w, h = event.width, event.height
        if (w, h) == self._last_size or w < 10 or h < 10:
            return
        self._last_size = (w, h)
        img = render_backdrop(w, h)
        self._photo = ImageTk.PhotoImage(img)
        if self._bg_id is None:
            self._bg_id = self.canvas.create_image(
                0, 0, image=self._photo, anchor="nw",
            )
            self.canvas.tag_lower(self._bg_id)
        else:
            self.canvas.itemconfig(self._bg_id, image=self._photo)


class Panel:
    def __init__(self, canvas: tk.Canvas, *, x: int, y: int,
                 width: int, height: int, radius: int = 22,
                 tint: tuple[int, int, int, int] = PANEL_TINT):
        self.canvas = canvas
        img = panel_image(width, height, radius=radius, tint=tint)
        self._photo = ImageTk.PhotoImage(img)
        self._img_id = canvas.create_image(x, y, image=self._photo, anchor="nw")

        self.content = tk.Frame(canvas, bg=PANEL_SOLID)
        self._win_id = canvas.create_window(
            x + 16, y + 16, anchor="nw",
            window=self.content,
            width=width - 32, height=height - 32,
        )


PANEL_SOLID = "#1e2145"


def make_button(parent: tk.Widget, text: str, *, primary: bool = False,
                command=None) -> tk.Button:
    bg = ACCENT if primary else "#262a54"
    active = "#d63851" if primary else "#30346a"
    return tk.Button(
        parent, text=text, command=command,
        font=(FONT, 11, "bold"),
        fg="white", bg=bg, activebackground=active, activeforeground="white",
        relief=tk.FLAT, bd=0, padx=18, pady=10, cursor="hand2",
    )


def label(parent: tk.Widget, text: str, *, size: int = 11,
          bold: bool = False, italic: bool = False,
          fg: str = TEXT) -> tk.Label:
    weight = "bold" if bold else "normal"
    slant = "italic" if italic else "roman"
    return tk.Label(
        parent, text=text,
        font=(FONT, size, weight, slant) if italic or bold else (FONT, size),
        fg=fg, bg=PANEL_SOLID,
    )

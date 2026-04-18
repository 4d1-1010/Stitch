"""Shared styling helpers for the UnIO tkinter UI.

Two palettes coexist during the UI rework:

* The original **dark** palette (BG_DARK, ACCENT, Backdrop, Panel, …) —
  used by scripts/launcher.py's current Host/Join screens until the
  shell rewrite replaces them.

* The new **paper + lilac** palette (PAPER_* constants, PillButton,
  Card, StatusDot, RailButton, …) — the target look. Friendly,
  deliberately not Synergy-blue, one accent used sparingly.

New code should consume the PAPER_* tokens and the primitives at the
bottom of this file. Both sets are kept side by side so the app
stays runnable while the shell is being built out.
"""

from __future__ import annotations

import tkinter as tk
from pathlib import Path
from typing import Callable, Optional

from PIL import Image, ImageDraw, ImageFilter, ImageTk


# ── Legacy dark palette (current Host/Join screens) ──────────────

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


# ── Paper + lilac palette (new shell) ────────────────────────────
#
# Kept minimal on purpose: one accent (lilac), one positive (mint),
# one warn (amber), one danger (coral). Everything else is neutral
# so status dots and the single accent carry meaning on their own.

PAPER_BG = "#ffffff"          # app background
PAPER_SURFACE = "#f4f5f8"     # cards, content panels
PAPER_RAIL = "#eef0f5"        # left nav rail
PAPER_BORDER = "#e2e4ec"      # hairline separators
PAPER_TEXT = "#1f2024"        # primary text
PAPER_MUTED = "#6d7286"       # secondary text
PAPER_FAINT = "#9a9db0"       # tertiary / hint text

LILAC = "#8b7bff"             # primary accent
LILAC_HOVER = "#7a6af0"
LILAC_SOFT = "#eeeaff"        # tint for active tab background
MINT = "#5cc9a3"              # success / connected
AMBER = "#e8b04c"             # warn / high latency
CORAL = "#ff6b5b"             # danger / disconnected / error

# ── Typography ────────────────────────────────────────────────────

FONT_SANS = "Helvetica"
SIZE_XS = 9
SIZE_SM = 10
SIZE_BASE = 11
SIZE_LG = 13
SIZE_XL = 16
SIZE_TITLE = 22

# ── Spacing + radii ──────────────────────────────────────────────

SPACE_XS = 4
SPACE_SM = 8
SPACE_MD = 12
SPACE_LG = 18
SPACE_XL = 28

RADIUS_SM = 6
RADIUS_MD = 12
RADIUS_LG = 18


# ── Backdrop / Panel (legacy dark) ───────────────────────────────

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
    """Attach the UnIO PNG icon to the given Tk window."""
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


def make_root(title: str = "UnIO") -> tk.Tk:
    """Create a Tk root with the UnIO WM class + title baked in."""
    root = tk.Tk(className="UnIO")
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


# ── Paper + lilac primitives ─────────────────────────────────────

class PillButton(tk.Label):
    """Flat rounded button used throughout the new shell.

    tkinter doesn't support rounded backgrounds on native buttons, so
    this is a Label with a fixed padding and hover colours — good
    enough when the surface underneath is solid. Paper UI is all
    flat shapes anyway.
    """

    def __init__(self, parent: tk.Widget, text: str, *,
                 command: Optional[Callable[[], None]] = None,
                 variant: str = "primary",
                 size: int = SIZE_BASE):
        self._command = command
        self._variant = variant
        palettes = {
            "primary":   (LILAC, "#ffffff", LILAC_HOVER, "#ffffff"),
            "secondary": (PAPER_SURFACE, PAPER_TEXT, PAPER_BORDER, PAPER_TEXT),
            "ghost":     (PAPER_BG, PAPER_TEXT, PAPER_SURFACE, PAPER_TEXT),
            "danger":    (CORAL, "#ffffff", "#e85648", "#ffffff"),
        }
        bg, fg, hover_bg, hover_fg = palettes.get(variant, palettes["primary"])
        self._bg, self._fg = bg, fg
        self._hover_bg, self._hover_fg = hover_bg, hover_fg
        super().__init__(
            parent, text=text,
            font=(FONT_SANS, size, "bold"),
            bg=bg, fg=fg,
            padx=SPACE_LG, pady=SPACE_SM,
            cursor="hand2",
        )
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)
        self.bind("<Button-1>", self._on_click)

    def _on_enter(self, _e=None):
        self.configure(bg=self._hover_bg, fg=self._hover_fg)

    def _on_leave(self, _e=None):
        self.configure(bg=self._bg, fg=self._fg)

    def _on_click(self, _e=None):
        if self._command:
            self._command()


class Card(tk.Frame):
    """Neutral surface card with optional left-border accent strip."""

    def __init__(self, parent: tk.Widget, *,
                 accent: Optional[str] = None,
                 pad: int = SPACE_LG,
                 bg: str = PAPER_SURFACE):
        super().__init__(parent, bg=bg, bd=0, highlightthickness=0)
        if accent:
            strip = tk.Frame(self, bg=accent, width=3)
            strip.pack(side=tk.LEFT, fill=tk.Y)
        self.body = tk.Frame(self, bg=bg, padx=pad, pady=pad)
        self.body.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)


class StatusDot(tk.Canvas):
    """Small circle for connection state. 10px; semantic colors."""

    COLORS = {
        "ok": MINT,
        "warn": AMBER,
        "bad": CORAL,
        "idle": PAPER_FAINT,
    }

    def __init__(self, parent: tk.Widget, state: str = "idle",
                 *, size: int = 10, bg: str = PAPER_BG):
        super().__init__(parent, width=size, height=size,
                         bg=bg, highlightthickness=0, bd=0)
        self._size = size
        self._id = self.create_oval(
            1, 1, size - 1, size - 1,
            fill=self.COLORS.get(state, PAPER_FAINT), outline="",
        )
        self._state = state

    def set_state(self, state: str) -> None:
        self._state = state
        self.itemconfig(self._id, fill=self.COLORS.get(state, PAPER_FAINT))


class RailButton(tk.Frame):
    """Left-rail nav entry: icon (emoji for now) + label stacked.

    Active state: lilac-tinted background and lilac text. Passive
    state: neutral. Click cycles an underlying StringVar so multiple
    rail buttons share a "selected" value.
    """

    def __init__(self, parent: tk.Widget, *, label: str, glyph: str,
                 value: str, var: tk.StringVar,
                 on_select: Optional[Callable[[str], None]] = None):
        super().__init__(parent, bg=PAPER_RAIL, bd=0, highlightthickness=0,
                         cursor="hand2")
        self._value = value
        self._var = var
        self._on_select = on_select

        self._glyph_lbl = tk.Label(
            self, text=glyph, bg=PAPER_RAIL, fg=PAPER_MUTED,
            font=(FONT_SANS, SIZE_XL),
        )
        self._glyph_lbl.pack(pady=(SPACE_MD, 2))
        self._text_lbl = tk.Label(
            self, text=label, bg=PAPER_RAIL, fg=PAPER_MUTED,
            font=(FONT_SANS, SIZE_XS, "bold"),
        )
        self._text_lbl.pack(pady=(0, SPACE_MD))

        for w in (self, self._glyph_lbl, self._text_lbl):
            w.bind("<Button-1>", self._on_click)

        var.trace_add("write", lambda *_: self._refresh())
        self._refresh()

    def _on_click(self, _e=None):
        self._var.set(self._value)
        if self._on_select:
            self._on_select(self._value)

    def _refresh(self):
        active = self._var.get() == self._value
        bg = LILAC_SOFT if active else PAPER_RAIL
        fg = LILAC if active else PAPER_MUTED
        for w in (self, self._glyph_lbl, self._text_lbl):
            w.configure(bg=bg)
        self._glyph_lbl.configure(fg=fg)
        self._text_lbl.configure(fg=fg)


def hairline(parent: tk.Widget, *, axis: str = "x",
             color: str = PAPER_BORDER) -> tk.Frame:
    """1-px separator, used between list items and sections."""
    if axis == "x":
        f = tk.Frame(parent, bg=color, height=1)
    else:
        f = tk.Frame(parent, bg=color, width=1)
    return f

"""Source-side overlay for a monitor that's been projected elsewhere.

When a monitor on this PC is the source of a stream to some other PC,
the user's OS still treats it as a working screen — apps can still
open on it, the cursor can still land there, and the user can get
genuinely confused about "which window is on which panel?".

Phase 2 wants this to feel like the cable was unplugged: the monitor
becomes unreachable while the remote sink is claiming it. The plan
doc calls for calling SetDisplayConfig / xrandr --off to cut the OS
output. That is correct but risky — a crash leaves the user with a
permanently disabled display.

This module opts for the softer, reversible approach: cover the
source monitor with a borderless fullscreen overlay that absorbs
input, displays a badge explaining what's going on, and disappears
the moment the route releases. The OS still thinks the panel is on,
but the user can't actually use it — which is what "unreachable"
needs to mean from a usability standpoint.
"""

from __future__ import annotations

import logging
import tkinter as tk
from typing import Optional

log = logging.getLogger(__name__)


_OVERLAY_BG = "#0f1020"
_OVERLAY_ACCENT = "#8b7bff"
_OVERLAY_TEXT = "#ffffff"
_OVERLAY_MUTED = "#bcc1d9"


class SourceOverlay:
    """Borderless Toplevel that sits on top of a projected source
    monitor and tells the user where it went."""

    def __init__(self, root: tk.Tk, x: int, y: int,
                 width: int, height: int,
                 source_label: str,
                 destination_label: str):
        self.root = root
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self.source_label = source_label
        self.destination_label = destination_label
        self._destroyed = False

        self.top = tk.Toplevel(root)
        self.top.overrideredirect(True)
        self.top.geometry(f"{int(width)}x{int(height)}+{int(x)}+{int(y)}")
        self.top.configure(bg=_OVERLAY_BG)
        try:
            self.top.attributes("-topmost", True)
        except tk.TclError:
            pass

        # Absorb input — the whole surface is a single Frame that
        # consumes click / motion events so the user can't
        # accidentally drop a window on the projected panel.
        self._frame = tk.Frame(self.top, bg=_OVERLAY_BG)
        self._frame.pack(fill=tk.BOTH, expand=True)

        inner = tk.Frame(self._frame, bg=_OVERLAY_BG)
        inner.place(relx=0.5, rely=0.5, anchor="center")

        badge_size = max(18, min(40, int(min(width, height) / 24)))
        tk.Label(
            inner, text="▶ PROJECTED",
            font=("Helvetica", badge_size, "bold"),
            fg=_OVERLAY_ACCENT, bg=_OVERLAY_BG,
        ).pack(pady=(0, 8))

        dest_size = max(24, min(72, int(min(width, height) / 12)))
        tk.Label(
            inner,
            text=f"→ {destination_label}",
            font=("Helvetica", dest_size, "bold"),
            fg=_OVERLAY_TEXT, bg=_OVERLAY_BG,
        ).pack()

        detail_size = max(14, min(26, int(min(width, height) / 36)))
        tk.Label(
            inner,
            text=(f"{source_label} is showing on another computer.\n"
                  "Drag the route back to this display in Layout to reclaim it."),
            font=("Helvetica", detail_size),
            fg=_OVERLAY_MUTED, bg=_OVERLAY_BG,
            justify="center",
        ).pack(pady=(12, 0))

        # Swallow every mouse / key event so the monitor reads as
        # inert. A user who *really* wants to click through can move
        # to another monitor in their workspace — exactly the "cable
        # unplugged" experience Phase 2 is aiming at.
        for seq in ("<Button-1>", "<Button-2>", "<Button-3>",
                    "<Motion>", "<Key>"):
            self._frame.bind(seq, lambda _e: "break")

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        try:
            self.top.destroy()
        except tk.TclError:
            pass

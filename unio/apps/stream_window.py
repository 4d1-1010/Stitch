"""Borderless sink window for streamed displays.

One tk.Toplevel per routed sink monitor. The window covers the
physical sink rectangle and paints JPEG frames arriving from the
source peer via StreamSink. When the source drops (peer gone, TCP
broken, or the sink is explicitly detached) the window either shows
a placeholder card or destroys itself, depending on the reason.

Rendering path for Phase 1 is Tk-native: PhotoImage from in-memory
JPEG via Pillow.ImageTk. Slow (~80–150 ms) but cross-platform and
requires nothing beyond Pillow. Phase 5 replaces this with an
OpenGL/D3D11 surface and a hardware-decoded texture.
"""

from __future__ import annotations

import io
import logging
import threading
import tkinter as tk
from typing import Callable, Optional

log = logging.getLogger(__name__)


# Paper+lilac palette in sync with the main UI so the placeholder
# card looks like the rest of unIO, not a random fallback. Duplicated
# here (rather than imported from ui_theme) so this module stays
# isolated from the shell's import graph — the shell imports this
# module but not vice-versa.
_PLACEHOLDER_BG = "#111111"
_PLACEHOLDER_TEXT_PRIMARY = "#ffffff"
_PLACEHOLDER_TEXT_MUTED = "#9aa0b4"


class StreamWindow:
    """One borderless fullscreen Toplevel that renders a remote source.

    The window is ephemeral: it's created when a route gains a sink on
    this PC, destroyed when the route is released. Frames arrive from
    a StreamSink running on a background thread; they're dispatched to
    the Tk main loop via `root.after(0, ...)` so PhotoImage touches
    all happen on the UI thread as Tk requires.
    """

    def __init__(self, root: tk.Tk, x: int, y: int,
                 width: int, height: int,
                 source_label: str,
                 on_close: Optional[Callable[[], None]] = None):
        self.root = root
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self.source_label = source_label
        self._on_close = on_close

        self._frame_lock = threading.Lock()
        # latest_frame is (data, codec) — codec ∈ {"jpeg", "h264"}.
        # Only the most recent frame is kept; newer arrivals clobber
        # older ones before redraw so the window stays on the live
        # frame instead of lagging 2–3 frames behind the source.
        self._latest_frame: Optional[tuple[bytes, str]] = None
        self._redraw_scheduled = False
        self._destroyed = False
        # Hold refs to PhotoImage objects across redraw — Tk garbage-
        # collects them aggressively if the label's image field is the
        # only reference, which makes the window flicker black.
        self._photo_ref = None

        self.top = tk.Toplevel(root)
        # overrideredirect before geometry so the WM doesn't reposition
        # the borderless window somewhere inconvenient before we've
        # said where we want it.
        self.top.overrideredirect(True)
        self.top.geometry(f"{int(width)}x{int(height)}+{int(x)}+{int(y)}")
        self.top.configure(bg=_PLACEHOLDER_BG)
        try:
            self.top.attributes("-topmost", True)
        except tk.TclError:
            pass
        try:
            # Prevent the window from grabbing keyboard focus so the
            # user's typing still lands in whichever app they're
            # actually interacting with.
            self.top.attributes("-focusable", False)
        except tk.TclError:
            pass

        self._canvas = tk.Canvas(
            self.top, bg=_PLACEHOLDER_BG,
            highlightthickness=0, bd=0,
            width=width, height=height,
        )
        self._canvas.pack(fill=tk.BOTH, expand=True)
        self._image_id = self._canvas.create_image(
            0, 0, anchor="nw")
        self._show_placeholder("Waiting for frame…")

        self.top.protocol("WM_DELETE_WINDOW", self._handle_user_close)

    # ── Frame arrival (background thread) ────────────────────────────

    def push_frame(self, data: bytes, codec: str = "jpeg") -> None:
        """Called from the StreamSink's reader thread. Stashes the
        newest frame and asks the Tk loop to repaint on its next
        idle tick. Older frames are dropped intentionally — nobody
        wants a 3-frames-behind video of their own desktop."""
        with self._frame_lock:
            self._latest_frame = (data, codec)
            already = self._redraw_scheduled
            self._redraw_scheduled = True
        if already or self._destroyed:
            return
        try:
            self.root.after(0, self._redraw_on_ui_thread)
        except RuntimeError:
            # Tk loop has stopped — we're closing.
            pass

    def _redraw_on_ui_thread(self) -> None:
        with self._frame_lock:
            self._redraw_scheduled = False
            frame = self._latest_frame
            self._latest_frame = None
        if frame is None or self._destroyed:
            return
        data, codec = frame
        try:
            from PIL import Image, ImageTk
        except ImportError:
            log.warning("Pillow missing — can't render stream frames")
            self._show_placeholder("Pillow missing on this PC")
            return
        try:
            if codec == "h264":
                # Raw RGB frame from the HW decoder. Size is exactly
                # what negotiated during the handshake; the decoder
                # guarantees full-frame writes.
                img = Image.frombytes(
                    "RGB", (self.width, self.height), data)
            else:
                img = Image.open(io.BytesIO(data))
                img.load()
                if img.size != (self.width, self.height):
                    img = img.resize((self.width, self.height),
                                     Image.BILINEAR)
            photo = ImageTk.PhotoImage(img)
        except Exception:
            log.exception("frame decode failed (codec=%s)", codec)
            return
        try:
            self._canvas.itemconfigure(self._image_id, image=photo)
        except tk.TclError:
            return
        # Keep the PhotoImage alive — Tk's reference to it from the
        # Canvas item isn't enough for Python's GC.
        self._photo_ref = photo

    # ── Placeholder card (for "source disconnected" state) ───────────

    def show_placeholder(self, reason: str) -> None:
        """Called from the Tk thread only — external callers go
        through root.after."""
        self._show_placeholder(reason)

    def _show_placeholder(self, reason: str) -> None:
        try:
            self._canvas.delete("placeholder")
        except tk.TclError:
            return
        cx = self.width // 2
        cy = self.height // 2
        num_size = max(28, min(72, int(self.height / 10)))
        sub_size = max(14, min(28, int(self.height / 28)))
        self._canvas.create_text(
            cx, cy - num_size,
            text=self.source_label,
            anchor="center", tags="placeholder",
            font=("Helvetica", num_size, "bold"),
            fill=_PLACEHOLDER_TEXT_PRIMARY,
        )
        self._canvas.create_text(
            cx, cy + 6,
            text=reason,
            anchor="center", tags="placeholder",
            font=("Helvetica", sub_size),
            fill=_PLACEHOLDER_TEXT_MUTED,
        )

    # ── Lifecycle ────────────────────────────────────────────────────

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        try:
            self.top.destroy()
        except tk.TclError:
            pass

    def _handle_user_close(self) -> None:
        # Borderless windows don't usually get an X button, but a WM
        # can still send WM_DELETE_WINDOW via e.g. Alt+F4 or a
        # compositor gesture. Route that back to the owner so the
        # route's unsubscribe happens rather than us silently vanishing.
        if self._on_close:
            try:
                self._on_close()
            except Exception:
                log.exception("stream window on_close failed")
        else:
            self.destroy()

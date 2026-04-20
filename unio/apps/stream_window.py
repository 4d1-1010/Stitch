"""Borderless sink overlay for a routed display.

When the user tells unIO "show PC-A's display on PC-B's display 2",
a StreamWindow opens on PC-B's display 2, covering the panel, and
renders PC-A's pixels as they arrive on the wire. Nothing else lives
on this overlay — no labels, no error cards, no "waiting for frame"
text. The user sees exactly what the source PC is rendering, and
only that.

Input is absorbed, never falls through to the host PC. Cursor and
keyboard routing to the source PC is handled by the Peer's existing
passthrough layer; this window's only job with input events is to
prevent them from reaching PC-B's native desktop.

When the stream drops, the overlay goes black instead of showing an
error card — matches "directly what the PC provides". If the source
later reconnects, frames resume without any visual transition.
"""

from __future__ import annotations

import io
import logging
import threading
import tkinter as tk
from typing import Callable, Optional

log = logging.getLogger(__name__)


_BLACK = "#000000"


class StreamWindow:
    """Borderless fullscreen overlay that renders decoded frames
    from a StreamSink. Covers the sink monitor's physical rectangle
    on the Tk root's screen.

    All input is swallowed. Frames arrive from a background thread
    via `push_frame`; the Tk main loop repaints on its next idle tick.
    """

    def __init__(self, root: tk.Tk, x: int, y: int,
                 width: int, height: int,
                 source_label: str = "",
                 on_close: Optional[Callable[[], None]] = None):
        self.root = root
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self._on_close = on_close

        self._frame_lock = threading.Lock()
        self._latest_frame: Optional[tuple[bytes, str]] = None
        self._redraw_scheduled = False
        self._destroyed = False
        self._photo_ref = None    # keep PhotoImage alive across redraw

        self.top = tk.Toplevel(root)
        # Always map the window so the WM positions it correctly on
        # every platform. Start fully transparent so the user sees
        # their panel's native content right up until the first
        # decoded frame lands, then flip alpha to 1 atomically.
        # withdraw()+deiconify() is flaky on Windows for borderless
        # topmost toplevels; alpha-toggle behaves consistently.
        self._mapped = False
        self.top.geometry(f"{int(width)}x{int(height)}+{int(x)}+{int(y)}")
        self.top.configure(bg=_BLACK)
        try:
            self.top.attributes("-alpha", 0.0)
        except tk.TclError:
            pass
        dock_ok = False
        try:
            self.top.attributes("-type", "dock")
            dock_ok = True
        except tk.TclError:
            pass
        try:
            self.top.attributes("-topmost", True)
        except tk.TclError:
            pass
        try:
            self.top.attributes("-above", True)
        except tk.TclError:
            pass
        if not dock_ok:
            self.top.overrideredirect(True)
        # Hard-absorb every input event so nothing falls through to
        # the PC that physically owns this panel. Cursor / keyboard
        # routing to the source PC happens through the Peer's global
        # input hook; this window never generates a click on the
        # local desktop.
        self._canvas = tk.Canvas(
            self.top, bg=_BLACK,
            highlightthickness=0, bd=0,
            width=width, height=height,
            cursor="none",
        )
        self._canvas.pack(fill=tk.BOTH, expand=True)
        self._image_id = self._canvas.create_image(0, 0, anchor="nw")
        for seq in (
            "<ButtonPress-1>", "<ButtonPress-2>", "<ButtonPress-3>",
            "<ButtonRelease-1>", "<ButtonRelease-2>", "<ButtonRelease-3>",
            "<B1-Motion>", "<B2-Motion>", "<B3-Motion>",
            "<Motion>", "<MouseWheel>", "<Button-4>", "<Button-5>",
            "<KeyPress>", "<KeyRelease>",
            "<FocusIn>", "<FocusOut>",
        ):
            self._canvas.bind(seq, lambda _e: "break")
            self.top.bind(seq, lambda _e: "break")
        # Refuse to take keyboard focus — prevents typed keys from
        # landing here instead of the global keyboard hook.
        try:
            self.top.attributes("-focusable", False)
        except tk.TclError:
            pass
        self.top.protocol("WM_DELETE_WINDOW", self._handle_user_close)

        # Try to flag this window as invisible to the OS's screen-
        # capture APIs so the capture loop never sees our own
        # overlay pixels (the feedback-loop root cause). When this
        # succeeds, the shell's per-frame hide-during-capture
        # workaround skips us entirely — no flicker.
        self.auto_excluded = self._try_exclude_from_capture()

    def _try_exclude_from_capture(self) -> bool:
        """Platform-specific attempt to flag this overlay window as
        invisible to software screen capture. Returns True when the
        OS/compositor accepted the request — in that case the
        shell's hide-during-capture workaround skips us entirely.

        Windows: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE).
        Linux : _NET_WM_BYPASS_COMPOSITOR = 1. Modern compositors
        (Mutter, KWin, compton / picom) interpret this as "don't
        put me through the compositor; scan me out via a GPU
        overlay plane instead". Result: the window appears on the
        physical display but is NOT in the X root framebuffer that
        mss.grab reads. Compositors that ignore the hint fall
        through to the shell's fast XUnmap/XSync path.
        """
        import sys as _sys
        if _sys.platform == "win32":
            return self._try_exclude_win32()
        if _sys.platform.startswith("linux"):
            return self._try_exclude_linux_bypass_compositor()
        return False

    def _try_exclude_win32(self) -> bool:
        try:
            import ctypes
            user32 = ctypes.WinDLL("user32", use_last_error=True)
            hwnd = self.top.winfo_id()
            if not hwnd:
                return False
            WDA_EXCLUDEFROMCAPTURE = 0x00000011
            user32.SetWindowDisplayAffinity.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32,
            ]
            user32.SetWindowDisplayAffinity.restype = ctypes.c_int
            if user32.SetWindowDisplayAffinity(
                    hwnd, WDA_EXCLUDEFROMCAPTURE):
                log.info("StreamWindow excluded from capture via "
                         "SetWindowDisplayAffinity (hwnd=%d)", hwnd)
                return True
            log.info("SetWindowDisplayAffinity rejected — "
                     "requires Windows 10 build 19041+")
        except Exception:
            log.exception("SetWindowDisplayAffinity failed")
        return False

    def _try_exclude_linux_bypass_compositor(self) -> bool:
        try:
            import ctypes
            lib = ctypes.CDLL("libX11.so.6")
            lib.XOpenDisplay.restype = ctypes.c_void_p
            lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
            lib.XCloseDisplay.argtypes = [ctypes.c_void_p]
            lib.XInternAtom.restype = ctypes.c_ulong
            lib.XInternAtom.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
            lib.XChangeProperty.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                ctypes.c_void_p, ctypes.c_int,
            ]
            lib.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

            xid = int(self.top.winfo_id())
            if not xid:
                return False
            dpy = lib.XOpenDisplay(None)
            if not dpy:
                return False
            try:
                atom = lib.XInternAtom(
                    dpy, b"_NET_WM_BYPASS_COMPOSITOR", 0)
                cardinal = lib.XInternAtom(dpy, b"CARDINAL", 0)
                value = (ctypes.c_long * 1)(1)
                # PropModeReplace = 0; format = 32 bits per element;
                # nelements = 1.
                lib.XChangeProperty(
                    dpy, ctypes.c_ulong(xid), atom, cardinal, 32, 0,
                    ctypes.cast(value, ctypes.c_void_p), 1,
                )
                lib.XSync(dpy, 0)
                log.info("StreamWindow set _NET_WM_BYPASS_COMPOSITOR=1 "
                         "(xid=0x%x); mss.grab should skip us if the "
                         "compositor honours the hint", xid)
            finally:
                lib.XCloseDisplay(dpy)
            # We can't actually verify from here whether the
            # compositor honoured the hint — it's advisory. The
            # shell's hide-during-capture fallback stays active
            # for safety; if the compositor DID honour the hint,
            # the XUnmap/XMap cycle is a redundant no-op (the
            # pixels weren't in the framebuffer anyway). Reporting
            # False leaves the fallback on.
            return False
        except Exception:
            log.exception("_NET_WM_BYPASS_COMPOSITOR setup failed")
            return False

    # ── Frame push (background thread) ───────────────────────────

    def push_frame(self, data: bytes, codec: str = "jpeg") -> None:
        with self._frame_lock:
            self._latest_frame = (data, codec)
            already = self._redraw_scheduled
            self._redraw_scheduled = True
        if already or self._destroyed:
            return
        try:
            self.root.after(0, self._redraw_on_ui_thread)
        except RuntimeError:
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
            return
        try:
            if codec == "h264":
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
        self._photo_ref = photo
        if not self._mapped:
            # First real frame — flip alpha to 1.0 so the overlay
            # appears atomically over the panel. Because the window
            # was always mapped (just transparent), this works
            # consistently on both X11 and Win32.
            try:
                self.top.attributes("-alpha", 1.0)
                self._mapped = True
            except tk.TclError:
                pass

    # ── No-op APIs kept for compatibility with earlier code paths ─

    def show_placeholder(self, _reason: str = "") -> None:
        """Previously rendered a 'Source disconnected — …' card. We
        now show only what the source PC actually delivers — a blank
        black overlay when no frames are flowing. Keeping this method
        so the shell's existing calls stay no-ops rather than errors."""

    # ── Lifecycle ────────────────────────────────────────────────

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        try:
            self.top.destroy()
        except tk.TclError:
            pass

    def _handle_user_close(self) -> None:
        if self._on_close:
            try:
                self._on_close()
            except Exception:
                log.exception("stream window on_close failed")
        else:
            self.destroy()

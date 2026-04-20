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
            # On Windows, overrideredirect(True) rewrites the window's
            # style bits and strips WS_EX_LAYERED — undoing the
            # initial -alpha 0.0 call above. Re-apply the alpha now
            # so Tk re-sets WS_EX_LAYERED and our deferred click-
            # through can add WS_EX_TRANSPARENT safely on top of it.
            try:
                self.top.attributes("-alpha", 0.0)
            except tk.TclError:
                pass
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
        # Click-through on mouse events, deferred by 200 ms so Tk has
        # fully applied its own -alpha styling before we touch the
        # Win32 extended-style flags. Calling SetWindowLongW too early
        # races Tk and can leave WS_EX_LAYERED set without a matching
        # SetLayeredWindowAttributes call, which on Windows renders
        # the overlay invisible. Linux-side XShape has no such race
        # but we keep the deferral for symmetry.
        try:
            self.root.after(200, self._make_click_through)
        except Exception:
            self._make_click_through()
        self.top.protocol("WM_DELETE_WINDOW", self._handle_user_close)

        # Try to flag this window as invisible to the OS's screen-
        # capture APIs. In practice SetWindowDisplayAffinity /
        # _NET_WM_BYPASS_COMPOSITOR only help WHEN the source-side
        # capture backend routes through DWM / a composite-aware
        # X path — GDI BitBlt and XShmGetImage both ignore them.
        # Since mss (the backend that's actually in use today on
        # both platforms) uses those ignoring paths, we keep the
        # hide-during-capture fallback active for ALL windows.
        # `auto_excluded` stays False until the capture backend is
        # genuinely WDA-aware (DXGI Desktop Duplication / Windows.
        # Graphics.Capture, or an XComposite mini-compositor).
        self._try_exclude_from_capture()
        self.auto_excluded = False

    def _make_click_through(self) -> bool:
        """Tell the WM/OS that our overlay should not receive mouse
        input — events pass through to whatever window is under the
        cursor. Required for the display-routing path: the remote
        peer injects clicks at the cursor position, but our overlay
        covers the target monitor and would absorb them otherwise.

        Windows: set WS_EX_TRANSPARENT + WS_EX_LAYERED on the HWND.
        Linux  : set an empty XShape input region on the window.

        Returns True on success, False on other platforms / failure.
        """
        import sys as _sys
        if _sys.platform == "win32":
            return self._make_click_through_win32()
        if _sys.platform.startswith("linux"):
            return self._make_click_through_linux()
        return False

    def _make_click_through_win32(self) -> bool:
        """Add WS_EX_TRANSPARENT to the overlay HWND so injected
        clicks pass through to the real window underneath. We DON'T
        add WS_EX_LAYERED ourselves — Tk's -alpha handling owns that
        flag, and adding it from two places leaves the layered-attr
        state inconsistent and the window invisible. If Tk hasn't
        set WS_EX_LAYERED by the time we run (e.g. because
        overrideredirect stripped it and nobody re-applied -alpha),
        we skip the click-through rather than risk invisibility."""
        try:
            import ctypes
            user32 = ctypes.WinDLL("user32", use_last_error=True)
            GWL_EXSTYLE = -20
            WS_EX_LAYERED = 0x00080000
            WS_EX_TRANSPARENT = 0x00000020
            hwnd = self.top.winfo_id()
            if not hwnd:
                return False
            user32.GetWindowLongW.argtypes = [
                ctypes.c_void_p, ctypes.c_int]
            user32.GetWindowLongW.restype = ctypes.c_long
            user32.SetWindowLongW.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_long]
            user32.SetWindowLongW.restype = ctypes.c_long
            cur = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            if not (cur & WS_EX_LAYERED):
                log.info("StreamWindow click-through SKIPPED — "
                         "WS_EX_LAYERED is not set on hwnd=%d "
                         "(exstyle=0x%x). Tk's alpha path likely "
                         "got stripped by overrideredirect.",
                         hwnd, cur & 0xFFFFFFFF)
                return False
            new = cur | WS_EX_TRANSPARENT
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, new)
            log.info("StreamWindow click-through enabled "
                     "(hwnd=%d, exstyle 0x%x → 0x%x)",
                     hwnd, cur & 0xFFFFFFFF, new & 0xFFFFFFFF)
            return True
        except Exception:
            log.exception("WS_EX_TRANSPARENT failed")
            return False

    def _make_click_through_linux(self) -> bool:
        try:
            import ctypes
            # libXext — XShape extension
            lib = ctypes.CDLL("libXext.so.6")
            libx = ctypes.CDLL("libX11.so.6")
            libx.XOpenDisplay.argtypes = [ctypes.c_char_p]
            libx.XOpenDisplay.restype = ctypes.c_void_p
            libx.XCloseDisplay.argtypes = [ctypes.c_void_p]
            # XShapeCombineRectangles(dpy, win, kind, x, y, rects,
            #                         n_rects, op, ordering)
            # kind = 2 = ShapeInput
            # op   = 0 = ShapeSet
            lib.XShapeCombineRectangles.argtypes = [
                ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ]
            libx.XFlush.argtypes = [ctypes.c_void_p]
            xid = int(self.top.winfo_id())
            if not xid:
                return False
            dpy = libx.XOpenDisplay(None)
            if not dpy:
                return False
            try:
                # Empty rects array + n_rects=0 => accept no input.
                ShapeInput = 2
                ShapeSet = 0
                Unsorted = 0
                lib.XShapeCombineRectangles(
                    dpy, ctypes.c_ulong(xid), ShapeInput,
                    0, 0, None, 0, ShapeSet, Unsorted,
                )
                libx.XFlush(dpy)
                log.info("StreamWindow click-through enabled "
                         "via XShape ShapeInput (xid=0x%x)", xid)
                return True
            finally:
                libx.XCloseDisplay(dpy)
        except Exception:
            log.exception("XShape ShapeInput failed")
            return False

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

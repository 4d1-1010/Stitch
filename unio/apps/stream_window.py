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

        # Flag the overlay as invisible to OS screen-capture APIs.
        #
        # WINDOWS: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
        # has two well-documented foot-guns we MUST avoid:
        #   1. The window must have WS_EX_LAYERED applied BEFORE the
        #      affinity call, otherwise DWM silently falls back to
        #      WDA_MONITOR-style black-rect rendering even though a
        #      GetWindowDisplayAffinity readback reports the flag is
        #      set correctly (Electron bug).
        #   2. The call has to happen after ShowWindow / while the
        #      HWND is realized — running it from __init__ on an
        #      unmapped Tk Toplevel is one of the paths that lands
        #      us in the broken state above.
        # `_make_click_through_win32` runs ~200 ms after __init__,
        # by which time Tk has mapped the window and we've just
        # applied WS_EX_LAYERED + alpha. That's the correct place
        # for the WDA call — done there, not here.
        #
        # LINUX: _NET_WM_BYPASS_COMPOSITOR doesn't have this ordering
        # gotcha; set it eagerly from __init__.
        import sys as _sys
        if _sys.platform.startswith("linux"):
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
        """Add WS_EX_LAYERED + WS_EX_TRANSPARENT and immediately set
        layered alpha to 255 ourselves — don't rely on Tk's `-alpha`
        path here because overrideredirect(True) on Windows strips
        the layered flag and Tk doesn't reliably re-apply it. Own
        the layered attributes end to end: alpha starts at 255 so
        the overlay is visible as soon as the first frame paints.
        Alpha-based 'invisible until first frame' is abandoned on
        Windows — a brief black rect before the first JPEG is
        acceptable compared to a permanently-invisible overlay."""
        try:
            import ctypes
            user32 = ctypes.WinDLL("user32", use_last_error=True)
            GWL_EXSTYLE = -20
            WS_EX_LAYERED = 0x00080000
            WS_EX_TRANSPARENT = 0x00000020
            LWA_ALPHA = 0x02
            hwnd = self.top.winfo_id()
            if not hwnd:
                return False
            user32.GetWindowLongW.argtypes = [
                ctypes.c_void_p, ctypes.c_int]
            user32.GetWindowLongW.restype = ctypes.c_long
            user32.SetWindowLongW.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_long]
            user32.SetWindowLongW.restype = ctypes.c_long
            user32.SetLayeredWindowAttributes.argtypes = [
                ctypes.c_void_p, ctypes.c_uint,
                ctypes.c_ubyte, ctypes.c_uint,
            ]
            user32.SetLayeredWindowAttributes.restype = ctypes.c_int
            cur = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            new = cur | WS_EX_LAYERED | WS_EX_TRANSPARENT
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, new)
            # Alpha = 254, NOT 255. 255 would be fully opaque and
            # Tk/Windows drop the layered composition path, which in
            # turn causes BitBlt(SRCCOPY) to INCLUDE the overlay in
            # screen captures — that's the "black stream after first
            # frame" bug. 254 (99.6%) is visually identical (1/255
            # below full opacity, eye can't see it) but the window
            # stays layered so BitBlt keeps auto-excluding it.
            user32.SetLayeredWindowAttributes(hwnd, 0, 254, LWA_ALPHA)
            log.info("StreamWindow click-through enabled "
                     "(hwnd=%d, exstyle 0x%x → 0x%x, alpha=254)",
                     hwnd, cur & 0xFFFFFFFF, new & 0xFFFFFFFF)
            # WDA must come AFTER WS_EX_LAYERED is applied — see the
            # ordering comment on the __init__ call site for why.
            self._try_exclude_win32()
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
        """Apply WDA_EXCLUDEFROMCAPTURE + verify via round-trip.

        The flag has a documented corruption path: on some Windows
        builds, applying EXCLUDEFROMCAPTURE over a prior WDA_MONITOR
        state silently fails — the call returns success and
        GetWindowDisplayAffinity reports EXCLUDEFROMCAPTURE, but the
        compositor keeps rendering the window as a black rect in
        captures. The workaround is a defensive WDA_NONE reset
        before the real call. Tk never sets WDA_MONITOR explicitly
        but we do the reset anyway — the cost is one extra Win32
        call per overlay init."""
        try:
            import ctypes
            user32 = ctypes.WinDLL("user32", use_last_error=True)
            raw_hwnd = self.top.winfo_id()
            if not raw_hwnd:
                return False
            WDA_NONE = 0x00000000
            WDA_EXCLUDEFROMCAPTURE = 0x00000011
            user32.SetWindowDisplayAffinity.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32,
            ]
            user32.SetWindowDisplayAffinity.restype = ctypes.c_int
            user32.GetWindowDisplayAffinity.argtypes = [
                ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32),
            ]
            user32.GetWindowDisplayAffinity.restype = ctypes.c_int
            user32.GetAncestor.argtypes = [
                ctypes.c_void_p, ctypes.c_uint]
            user32.GetAncestor.restype = ctypes.c_void_p
            user32.IsWindow.argtypes = [ctypes.c_void_p]
            user32.IsWindow.restype = ctypes.c_int
            # Tk's winfo_id() on Windows sometimes hands back the
            # inner frame HWND rather than the Toplevel's root.
            # SetWindowDisplayAffinity requires a top-level HWND —
            # call it on GA_ROOT (the desktop-owned ancestor).
            GA_ROOT = 2
            root_hwnd = user32.GetAncestor(raw_hwnd, GA_ROOT)
            hwnd = root_hwnd or raw_hwnd
            is_win = bool(user32.IsWindow(hwnd))
            # Defensive reset to WDA_NONE before applying the real
            # affinity, avoiding the silent
            # WDA_MONITOR → EXCLUDEFROMCAPTURE upgrade bug.
            ctypes.set_last_error(0)
            reset_ok = bool(user32.SetWindowDisplayAffinity(
                hwnd, WDA_NONE))
            reset_err = ctypes.get_last_error()
            ctypes.set_last_error(0)
            ok = bool(user32.SetWindowDisplayAffinity(
                hwnd, WDA_EXCLUDEFROMCAPTURE))
            set_err = ctypes.get_last_error()
            ctypes.set_last_error(0)
            readback = ctypes.c_uint32(0)
            got = user32.GetWindowDisplayAffinity(
                hwnd, ctypes.byref(readback))
            get_err = ctypes.get_last_error()
            if ok and got and readback.value == WDA_EXCLUDEFROMCAPTURE:
                log.info("StreamWindow excluded from capture — "
                         "SetWindowDisplayAffinity ok, readback=0x%x "
                         "(raw=%d root=%d)",
                         readback.value, raw_hwnd, hwnd)
                return True
            log.info("SetWindowDisplayAffinity failed to stick — "
                     "raw=%d root=%d IsWindow=%s "
                     "reset_ok=%s(err=%d) set_ok=%s(err=%d) "
                     "readback_ok=%s(err=%d) readback=0x%x",
                     raw_hwnd, hwnd, is_win,
                     reset_ok, reset_err, ok, set_err,
                     bool(got), get_err, readback.value)
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
        except Exception:
            log.exception("frame decode failed (codec=%s)", codec)
            return
        # Reuse a single long-lived PhotoImage: ``paste`` writes the
        # new pixels into the existing Tk image buffer, the canvas
        # auto-refreshes, and we avoid ~5-10 ms of allocation + GC
        # pressure per frame that ``ImageTk.PhotoImage(img)`` would
        # otherwise incur. itemconfigure only needs to run once per
        # PhotoImage (on init); subsequent pastes don't re-bind the
        # canvas item.
        try:
            if self._photo_ref is None:
                self._photo_ref = ImageTk.PhotoImage(img)
                self._canvas.itemconfigure(
                    self._image_id, image=self._photo_ref)
            else:
                self._photo_ref.paste(img)
        except tk.TclError:
            return
        except Exception:
            log.exception("PhotoImage paste failed")
            return
        if not self._mapped:
            # First real frame — flip alpha up. Crucially NOT 1.0 on
            # Windows: Tk's native attr handler treats alpha==1.0 as
            # a signal to strip WS_EX_LAYERED entirely, which then
            # makes the overlay opaque and visible to BitBlt-based
            # screen captures (including our own Windows backend) —
            # that's what caused the "black stream after first frame"
            # symptom. Use 0.999 instead: visually indistinguishable
            # from fully opaque (1/255 translucent), but keeps the
            # WS_EX_LAYERED bit set so BitBlt(SRCCOPY) auto-excludes
            # us from captures.
            try:
                import sys as _sys
                if _sys.platform == "win32":
                    # Stay clearly below 1.0 so Tk's native handler
                    # keeps WS_EX_LAYERED set; BitBlt auto-excludes
                    # layered windows from the screen capture, which
                    # is how we avoid feedback on Windows.
                    self.top.attributes("-alpha", 0.95)
                else:
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

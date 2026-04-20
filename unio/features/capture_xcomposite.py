"""XComposite-based screen capture — Linux X11, exclusion-aware.

Replaces ``mss`` on Linux. mss uses ``XShmGetImage`` on the root
window, which reads the final composited screen — including our own
StreamWindow overlays. That's the feedback-loop source. ``_NET_WM_
BYPASS_COMPOSITOR`` is advisory and Mutter/KWin/GNOME-Shell don't
honour it for screen-capture purposes, so it doesn't save us.

XComposite lets us walk the root's top-level children and read
each window's backing pixmap individually via ``XCompositeName
WindowPixmap``. That:
  * Skips any window we decide to exclude (our StreamWindow xids).
  * Runs against the un-composited pixels — no alpha-blending
    artefacts from the compositor.
  * Works on every modern Linux desktop, because the system
    compositor (Mutter, KWin, picom, compton, mutter-x11-frames)
    already redirects every top-level window to a backing pixmap
    in manual mode. We don't need to request a redirect ourselves;
    we just read what the system compositor already has.

If XComposite isn't available or no compositor is running, ``open()``
returns False and the pipeline falls back to mss + hide-during-
capture.

Performance note: per-grab we enumerate children via ``XQueryTree``,
ask for each window's pixmap, and read its pixels via ``XGetImage``.
That's slower than mss's single-rect read — typical ~6-12 FPS on a
1080p desktop with 10-20 windows — but it's the price for real
exclusion. Phase 6's dirty-rect short-circuit keeps bandwidth down
regardless.
"""

from __future__ import annotations

import ctypes
import logging
import os
import sys
import threading
from typing import Iterable, Optional

log = logging.getLogger(__name__)


# ── X11 constants ───────────────────────────────────────────────────

IsViewable = 2
ZPixmap = 2
AllPlanes = (1 << 32) - 1


# ── X11 structures ──────────────────────────────────────────────────


class XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong),
        ("class_", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


class XImage(ctypes.Structure):
    # Only the leading fields we read. Xlib's real XImage has a
    # trailing funcs struct after bits_per_pixel; we never touch
    # those so laying them out is unnecessary.
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int),
        ("data", ctypes.c_void_p),
        ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
    ]


_ERRHANDLER_T = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)


# ── Capture class ───────────────────────────────────────────────────


class XCompositeCapture:
    """Per-window composite reader. Single-consumer; don't drive
    concurrently from multiple threads."""

    def __init__(self) -> None:
        self.dpy = None
        self.root = 0
        self.libx11 = None
        self.libxcomposite = None
        self._err_handler = None
        self._exclude_xids: set = set()
        self._lock = threading.Lock()

    # ── Exclusion list, thread-safe ─────────────────────────────

    def set_exclude_xids(self, xids: Iterable[int]) -> None:
        with self._lock:
            self._exclude_xids = {int(x) for x in xids if x}

    def add_exclude_xid(self, xid: int) -> None:
        with self._lock:
            if xid:
                self._exclude_xids.add(int(xid))

    def remove_exclude_xid(self, xid: int) -> None:
        with self._lock:
            self._exclude_xids.discard(int(xid))

    # ── Lifecycle ───────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "linux":
            return False
        try:
            return self._open_x()
        except Exception:
            log.exception("XCompositeCapture init failed")
            self.close()
            return False

    def _open_x(self) -> bool:
        try:
            self.libx11 = ctypes.CDLL("libX11.so.6")
        except OSError as e:
            log.info("libX11.so.6 not loadable: %s", e)
            return False
        try:
            self.libxcomposite = ctypes.CDLL("libXcomposite.so.1")
        except OSError as e:
            log.info("libXcomposite.so.1 not loadable: %s", e)
            return False

        x = self.libx11
        xc = self.libxcomposite
        ulong_p = ctypes.POINTER(ctypes.c_ulong)
        int_p = ctypes.POINTER(ctypes.c_int)
        uint_p = ctypes.POINTER(ctypes.c_uint)

        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XOpenDisplay.restype = ctypes.c_void_p
        x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x.XDefaultRootWindow.restype = ctypes.c_ulong
        x.XQueryTree.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ulong_p, ulong_p,
            ctypes.POINTER(ulong_p), uint_p,
        ]
        x.XQueryTree.restype = ctypes.c_int
        x.XFree.argtypes = [ctypes.c_void_p]
        x.XGetWindowAttributes.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.POINTER(XWindowAttributes),
        ]
        x.XGetWindowAttributes.restype = ctypes.c_int
        x.XTranslateCoordinates.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_int, ctypes.c_int,
            int_p, int_p, ulong_p,
        ]
        x.XTranslateCoordinates.restype = ctypes.c_int
        x.XGetImage.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.c_int, ctypes.c_int,
            ctypes.c_uint, ctypes.c_uint,
            ctypes.c_ulong, ctypes.c_int,
        ]
        x.XGetImage.restype = ctypes.POINTER(XImage)
        x.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
        x.XFreePixmap.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x.XSetErrorHandler.argtypes = [_ERRHANDLER_T]
        x.XSetErrorHandler.restype = _ERRHANDLER_T

        xc.XCompositeQueryExtension.argtypes = [
            ctypes.c_void_p, int_p, int_p]
        xc.XCompositeQueryExtension.restype = ctypes.c_int
        xc.XCompositeNameWindowPixmap.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong]
        xc.XCompositeNameWindowPixmap.restype = ctypes.c_ulong

        # Install a silent X error handler. XComposite calls race
        # with window destruction — transient BadWindow/BadPixmap/
        # BadDrawable errors are expected and must not abort the
        # process the way Xlib's default handler does. Return 0 to
        # tell Xlib to continue.
        def _silent(_dpy, _err):
            return 0

        self._err_handler = _ERRHANDLER_T(_silent)
        x.XSetErrorHandler(self._err_handler)

        display_name = os.environ.get("DISPLAY", "").encode() or None
        self.dpy = x.XOpenDisplay(display_name)
        if not self.dpy:
            log.info("XOpenDisplay(%r) failed", display_name)
            return False

        ev = ctypes.c_int()
        err = ctypes.c_int()
        if not xc.XCompositeQueryExtension(
                self.dpy, ctypes.byref(ev), ctypes.byref(err)):
            log.info("XComposite extension not available on this server")
            return False

        self.root = x.XDefaultRootWindow(self.dpy)
        if not self._probe_pixmap():
            log.info("XComposite pixmap probe failed — no compositor "
                     "has redirected subwindows, falling back to mss")
            return False

        log.info("XCompositeCapture ready (root=0x%x)", self.root)
        return True

    def _probe_pixmap(self) -> bool:
        """Test that ``XCompositeNameWindowPixmap`` actually returns
        a pixmap for at least one mapped top-level child of root. If
        every call returns 0 there's no compositor — NameWindow
        Pixmap only works on windows that some composite client has
        redirected (our code deliberately doesn't do this itself to
        avoid fighting the system compositor)."""
        root_ret = ctypes.c_ulong()
        parent_ret = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        nchildren = ctypes.c_uint()
        ok = self.libx11.XQueryTree(
            self.dpy, self.root,
            ctypes.byref(root_ret), ctypes.byref(parent_ret),
            ctypes.byref(children), ctypes.byref(nchildren),
        )
        if not ok or not nchildren.value:
            return False
        success = False
        try:
            for i in range(nchildren.value):
                xid = int(children[i])
                attrs = XWindowAttributes()
                if not self.libx11.XGetWindowAttributes(
                        self.dpy, xid, ctypes.byref(attrs)):
                    continue
                if attrs.map_state != IsViewable:
                    continue
                if attrs.width <= 1 or attrs.height <= 1:
                    continue
                pixmap = self.libxcomposite.XCompositeNameWindowPixmap(
                    self.dpy, xid)
                self.libx11.XSync(self.dpy, 0)
                if pixmap:
                    self.libx11.XFreePixmap(self.dpy, pixmap)
                    success = True
                    break
        finally:
            if children:
                self.libx11.XFree(children)
        return success

    def close(self) -> None:
        if self.dpy and self.libx11:
            try:
                self.libx11.XCloseDisplay(self.dpy)
            except Exception:
                pass
        self.dpy = None

    # ── Grab ────────────────────────────────────────────────────

    def grab(self, rect: dict):
        """Grab one frame of ``rect``. ``rect`` keys: x, y, width,
        height (all root-relative). Returns a PIL.Image in RGB mode
        or None on error.

        Starts from a black background and blits every mapped
        top-level window's pixmap in bottom-to-top stacking order,
        skipping any xid in ``_exclude_xids``. Cursor is not drawn.
        """
        from PIL import Image
        if not self.dpy:
            return None
        rx = int(rect.get("x", 0))
        ry = int(rect.get("y", 0))
        rw = int(rect.get("width", 0))
        rh = int(rect.get("height", 0))
        if rw <= 0 or rh <= 0:
            return None

        with self._lock:
            exclude = set(self._exclude_xids)

        root_ret = ctypes.c_ulong()
        parent_ret = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        nchildren = ctypes.c_uint()
        ok = self.libx11.XQueryTree(
            self.dpy, self.root,
            ctypes.byref(root_ret), ctypes.byref(parent_ret),
            ctypes.byref(children), ctypes.byref(nchildren),
        )
        if not ok:
            return None

        out = Image.new("RGB", (rw, rh), (0, 0, 0))
        try:
            # XQueryTree returns bottom-to-top stacking, which is
            # the order we want: paint each window on top of what's
            # already there.
            for i in range(nchildren.value):
                xid = int(children[i])
                if xid in exclude:
                    continue
                self._paint_one(xid, rx, ry, rw, rh, out)
        finally:
            if children:
                self.libx11.XFree(children)
        return out

    def _paint_one(self, xid: int, rx: int, ry: int,
                   rw: int, rh: int, out) -> None:
        """Paint one top-level window's backing pixmap into ``out``.
        All X calls go through the silent error handler, so transient
        failures (window destroyed mid-grab, server racing us) just
        skip the window instead of raising."""
        from PIL import Image
        x = self.libx11
        xc = self.libxcomposite
        attrs = XWindowAttributes()
        if not x.XGetWindowAttributes(
                self.dpy, xid, ctypes.byref(attrs)):
            return
        if attrs.map_state != IsViewable:
            return
        if attrs.width <= 0 or attrs.height <= 0:
            return

        wx_out = ctypes.c_int()
        wy_out = ctypes.c_int()
        child_ret = ctypes.c_ulong()
        if not x.XTranslateCoordinates(
                self.dpy, xid, self.root, 0, 0,
                ctypes.byref(wx_out), ctypes.byref(wy_out),
                ctypes.byref(child_ret)):
            return
        wx = wx_out.value
        wy = wy_out.value
        ww = attrs.width
        wh = attrs.height

        # Skip windows that don't intersect our capture rect.
        if (wx + ww <= rx or wy + wh <= ry
                or wx >= rx + rw or wy >= ry + rh):
            return

        pixmap = xc.XCompositeNameWindowPixmap(self.dpy, xid)
        # Sync so the silent error handler runs before we touch
        # the returned pixmap (which may be 0 if the request failed).
        x.XSync(self.dpy, 0)
        if not pixmap:
            return
        try:
            ximg_ptr = x.XGetImage(
                self.dpy, pixmap, 0, 0, ww, wh,
                AllPlanes, ZPixmap,
            )
            if not ximg_ptr:
                return
            try:
                ximg = ximg_ptr.contents
                bpl = ximg.bytes_per_line
                depth = ximg.bits_per_pixel
                data_addr = ximg.data
                if not data_addr or bpl <= 0:
                    return
                raw = bytes((ctypes.c_char * (bpl * wh))
                            .from_address(data_addr))
                # Most X servers give us BGRA (little-endian 32 bpp
                # ZPixmap) at depth 24/32. Try BGRX first; if the
                # window is 16 bpp or something unusual, skip it —
                # no compositor normally hands us that.
                if depth in (24, 32):
                    try:
                        img = Image.frombytes(
                            "RGB", (ww, wh), raw,
                            "raw", "BGRX", bpl, 1,
                        )
                    except Exception:
                        return
                else:
                    return
                # Paste relative to the capture rect. PIL's paste
                # auto-clips at output edges so we don't need to
                # manually crop the src.
                out.paste(img, (wx - rx, wy - ry))
            finally:
                x.XDestroyImage(ximg_ptr)
        finally:
            x.XFreePixmap(self.dpy, pixmap)


# ── Availability probe ──────────────────────────────────────────────


def available() -> bool:
    if sys.platform != "linux":
        return False
    try:
        ctypes.CDLL("libX11.so.6")
        ctypes.CDLL("libXcomposite.so.1")
        return True
    except OSError:
        return False

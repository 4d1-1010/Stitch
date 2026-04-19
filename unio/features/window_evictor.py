"""X11 window evictor — keep apps off routed-away panels.

When a monitor on this PC is being projected to a remote sink, we
cover it with a borderless overlay. The WM sees the overlay and
usually stacks above, but for mid-screen panels (not screen edges)
there's no standard X property that reserves the panel region from
window placement. New windows still land on it, just underneath our
overlay — exactly what the user wants to avoid.

This evictor polls X11's `_NET_CLIENT_LIST` every 500 ms, checks
each top-level client's geometry, and relocates anything that
overlaps our reserved panels to the primary (non-reserved) monitor.
Modern DEs sort this out on their own ~80% of the time via
stacking + focus-follows-mouse, but the evictor catches the long
tail (dialog boxes spawned at cursor, popups positioned from cached
coordinates, etc.).

Strictly Linux / X11; on Wayland the whole mechanism is a no-op
(Wayland compositors don't expose window positions to foreign
processes). On Windows we'd use `SetWinEventHook` + `MoveWindow`;
scaffolded but not wired in v1.
"""

from __future__ import annotations

import ctypes
import logging
import os
import sys
import threading
import time
from typing import Iterable, Optional

log = logging.getLogger(__name__)


# Rectangle in GLOBAL OS coordinates — same frame the X server uses
# for multi-monitor placement. Tuple form so it's hashable / small.
Rect = tuple[int, int, int, int]   # (x, y, w, h)


_POLL_INTERVAL_SEC = 0.5


class WindowEvictor:
    """Start / stop-able background thread that pushes apps off
    reserved panels. Thread exits on stop(); safe to restart.

    Usage:
        ev = WindowEvictor()
        ev.set_reserved([(100, 0, 1920, 1080)])
        ev.start()
        ...
        ev.stop()
    """

    def __init__(self) -> None:
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._reserved: list[Rect] = []
        self._lock = threading.Lock()
        self._xlib = None       # cached ctypes libX11 handle
        self._display = None    # X Display*
        self._root = 0          # Window id of the default root
        self._atom_client_list = 0
        self._user32 = None     # cached ctypes user32 handle (win)
        self._kernel32 = None   # for GetCurrentProcessId
        self._own_pid = 0
        self._impl: Optional[str] = None

    # ── Public API ───────────────────────────────────────────────

    def set_reserved(self, rects: Iterable[Rect]) -> None:
        rects = [tuple(r) for r in rects if r[2] > 0 and r[3] > 0]
        with self._lock:
            self._reserved = list(rects)

    def start(self) -> bool:
        """Returns True when the evictor comes up, False when the
        current platform can't host it. The caller can still call
        set_reserved + stop unconditionally — they're both no-ops
        when start() failed."""
        if sys.platform == "linux":
            if not os.environ.get("DISPLAY"):
                log.info("Window evictor: DISPLAY unset, skipping")
                return False
            if not self._load_xlib():
                return False
            self._impl = "x11"
        elif sys.platform == "win32":
            if not self._load_user32():
                return False
            self._impl = "win32"
        else:
            log.info("Window evictor: platform %s not supported",
                     sys.platform)
            return False
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run, daemon=True, name="unio-evictor",
        )
        self._thread.start()
        log.info("Window evictor started (%s)", self._impl)
        return True

    def stop(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None
        if self._xlib is not None and self._display is not None:
            try:
                self._xlib.XCloseDisplay(self._display)
            except Exception:
                pass
            self._display = None

    # ── xlib init ────────────────────────────────────────────────

    def _load_xlib(self) -> bool:
        try:
            lib = ctypes.CDLL("libX11.so.6")
        except OSError as e:
            log.info("Window evictor: libX11 not found: %s", e)
            return False
        # Minimal prototype declarations so ctypes doesn't truncate
        # the 64-bit Window / Atom values on 64-bit platforms.
        lib.XOpenDisplay.restype = ctypes.c_void_p
        lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
        lib.XDefaultRootWindow.restype = ctypes.c_ulong
        lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        lib.XInternAtom.restype = ctypes.c_ulong
        lib.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.c_int]
        lib.XGetWindowProperty.restype = ctypes.c_int
        lib.XGetWindowProperty.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_long, ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        lib.XFree.argtypes = [ctypes.c_void_p]
        lib.XGetGeometry.restype = ctypes.c_int
        lib.XGetGeometry.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.POINTER(ctypes.c_ulong),   # root
            ctypes.POINTER(ctypes.c_int),     # x
            ctypes.POINTER(ctypes.c_int),     # y
            ctypes.POINTER(ctypes.c_uint),    # width
            ctypes.POINTER(ctypes.c_uint),    # height
            ctypes.POINTER(ctypes.c_uint),    # border
            ctypes.POINTER(ctypes.c_uint),    # depth
        ]
        lib.XTranslateCoordinates.restype = ctypes.c_int
        lib.XTranslateCoordinates.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_ulong),
        ]
        lib.XMoveWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                    ctypes.c_int, ctypes.c_int]
        lib.XMoveWindow.restype = ctypes.c_int
        lib.XFlush.argtypes = [ctypes.c_void_p]
        lib.XCloseDisplay.argtypes = [ctypes.c_void_p]

        dpy = lib.XOpenDisplay(None)
        if not dpy:
            log.info("Window evictor: XOpenDisplay failed")
            return False
        self._xlib = lib
        self._display = dpy
        self._root = lib.XDefaultRootWindow(dpy)
        self._atom_client_list = lib.XInternAtom(
            dpy, b"_NET_CLIENT_LIST", 0)
        return True

    # ── Poll loop ────────────────────────────────────────────────

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                self._tick()
            except Exception:
                log.exception("Evictor tick failed")
            self._stop.wait(_POLL_INTERVAL_SEC)
        log.info("Window evictor loop exited")

    def _tick(self) -> None:
        with self._lock:
            reserved = list(self._reserved)
        if not reserved:
            return
        if self._impl == "win32":
            self._tick_win32(reserved)
            return
        for win_id, gx, gy, gw, gh in self._list_top_level_windows():
            if not self._intersects_any(gx, gy, gw, gh, reserved):
                continue
            safe_x, safe_y = self._pick_safe_spot(reserved)
            try:
                self._xlib.XMoveWindow(
                    self._display, ctypes.c_ulong(win_id),
                    safe_x, safe_y,
                )
                self._xlib.XFlush(self._display)
                log.info("evicted window %d from reserved panel to "
                         "(%d,%d)", win_id, safe_x, safe_y)
            except Exception:
                log.exception("XMoveWindow failed for win %d", win_id)

    def _list_top_level_windows(self):
        """Yield (win_id, global_x, global_y, width, height) for every
        entry in `_NET_CLIENT_LIST`. Using _NET_CLIENT_LIST instead of
        walking the window tree gives us just the 'real' app windows,
        not transient popups / tooltips / our own overlays."""
        XA_WINDOW = 33  # X.h constant
        actual_type = ctypes.c_ulong(0)
        actual_format = ctypes.c_int(0)
        nitems = ctypes.c_ulong(0)
        bytes_after = ctypes.c_ulong(0)
        prop = ctypes.c_void_p(0)
        rc = self._xlib.XGetWindowProperty(
            self._display, self._root, self._atom_client_list,
            0, (1 << 20), 0, XA_WINDOW,
            ctypes.byref(actual_type), ctypes.byref(actual_format),
            ctypes.byref(nitems), ctypes.byref(bytes_after),
            ctypes.byref(prop),
        )
        if rc != 0 or not prop or nitems.value == 0:
            return
        try:
            array_t = ctypes.c_ulong * nitems.value
            arr = array_t.from_address(prop.value)
            win_ids = [int(arr[i]) for i in range(nitems.value)]
        finally:
            self._xlib.XFree(prop)

        for wid in win_ids:
            geom = self._global_geometry(wid)
            if geom is None:
                continue
            yield (wid,) + geom

    def _global_geometry(self, wid: int):
        """Return (global_x, global_y, width, height) in root-window
        coordinates. Returns None if the window vanished mid-poll."""
        root = ctypes.c_ulong(0)
        x = ctypes.c_int(0); y = ctypes.c_int(0)
        w = ctypes.c_uint(0); h = ctypes.c_uint(0)
        bw = ctypes.c_uint(0); depth = ctypes.c_uint(0)
        rc = self._xlib.XGetGeometry(
            self._display, ctypes.c_ulong(wid),
            ctypes.byref(root), ctypes.byref(x), ctypes.byref(y),
            ctypes.byref(w), ctypes.byref(h),
            ctypes.byref(bw), ctypes.byref(depth),
        )
        if rc == 0:
            return None
        # XGetGeometry returns coords relative to the parent — we
        # need root-relative. Translate.
        gx = ctypes.c_int(0); gy = ctypes.c_int(0)
        child = ctypes.c_ulong(0)
        tr_rc = self._xlib.XTranslateCoordinates(
            self._display, ctypes.c_ulong(wid), self._root,
            0, 0,
            ctypes.byref(gx), ctypes.byref(gy),
            ctypes.byref(child),
        )
        if tr_rc == 0:
            return None
        return (gx.value, gy.value, int(w.value), int(h.value))

    @staticmethod
    def _intersects_any(x, y, w, h, reserved: list[Rect]) -> bool:
        for rx, ry, rw, rh in reserved:
            if (x < rx + rw and x + w > rx
                    and y < ry + rh and y + h > ry):
                return True
        return False

    @staticmethod
    def _pick_safe_spot(reserved: list[Rect]) -> tuple[int, int]:
        right_edge = max((r[0] + r[2] for r in reserved), default=0)
        return (right_edge + 20, 40)

    # ── Win32 backend ────────────────────────────────────────────

    def _load_user32(self) -> bool:
        try:
            self._user32 = ctypes.WinDLL("user32", use_last_error=True)
            self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        except (OSError, AttributeError) as e:
            log.info("Window evictor: user32 load failed: %s", e)
            return False

        user32 = self._user32
        kernel32 = self._kernel32

        user32.EnumWindows.restype = ctypes.c_int
        user32.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        user32.IsWindowVisible.restype = ctypes.c_int
        user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
        user32.IsIconic.restype = ctypes.c_int
        user32.IsIconic.argtypes = [ctypes.c_void_p]

        class RECT(ctypes.Structure):
            _fields_ = [("left", ctypes.c_long),
                        ("top", ctypes.c_long),
                        ("right", ctypes.c_long),
                        ("bottom", ctypes.c_long)]
        self._RECT = RECT
        user32.GetWindowRect.restype = ctypes.c_int
        user32.GetWindowRect.argtypes = [ctypes.c_void_p,
                                         ctypes.POINTER(RECT)]
        user32.SetWindowPos.restype = ctypes.c_int
        user32.SetWindowPos.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ctypes.c_uint,
        ]
        user32.GetWindowThreadProcessId.restype = ctypes.c_ulong
        user32.GetWindowThreadProcessId.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        user32.GetWindowLongW.restype = ctypes.c_long
        user32.GetWindowLongW.argtypes = [ctypes.c_void_p, ctypes.c_int]

        kernel32.GetCurrentProcessId.restype = ctypes.c_ulong
        self._own_pid = kernel32.GetCurrentProcessId()
        return True

    def _tick_win32(self, reserved: list[Rect]) -> None:
        user32 = self._user32
        RECT = self._RECT
        WNDENUMPROC = ctypes.WINFUNCTYPE(
            ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        )
        GWL_EXSTYLE = -20
        WS_EX_TOOLWINDOW = 0x00000080
        SWP_NOSIZE = 0x0001
        SWP_NOZORDER = 0x0004
        SWP_NOACTIVATE = 0x0010
        SWP_ASYNCWINDOWPOS = 0x4000
        flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE \
            | SWP_ASYNCWINDOWPOS
        safe_x, safe_y = self._pick_safe_spot(reserved)

        moved: list[int] = []

        def _cb(hwnd, _lparam):
            try:
                if not user32.IsWindowVisible(hwnd):
                    return 1
                if user32.IsIconic(hwnd):
                    return 1
                # Skip tool windows (tooltips, tray popups) so we
                # don't chase transient balloons around the screen.
                ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
                if ex_style & WS_EX_TOOLWINDOW:
                    return 1
                # Skip our own process windows.
                pid = ctypes.c_ulong(0)
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if int(pid.value) == self._own_pid:
                    return 1
                rect = RECT()
                if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                    return 1
                w = rect.right - rect.left
                h = rect.bottom - rect.top
                if w <= 0 or h <= 0:
                    return 1
                if not self._intersects_any(rect.left, rect.top, w, h,
                                            reserved):
                    return 1
                if user32.SetWindowPos(hwnd, None,
                                       safe_x, safe_y, 0, 0, flags):
                    moved.append(int(ctypes.cast(
                        hwnd, ctypes.c_void_p).value or 0))
            except Exception:
                # Callbacks must not raise back into win32.
                pass
            return 1

        user32.EnumWindows(WNDENUMPROC(_cb), None)
        if moved:
            log.info("evicted %d window(s) from reserved panel to (%d,%d)",
                     len(moved), safe_x, safe_y)

"""XDamage-based dirty rectangle watcher for Linux X11.

X11's Damage extension tells us *exactly* when a window's pixels
changed, with no polling overhead — the X server already knows which
regions got redrawn during the last render pass, so asking "has
anything changed since I last looked?" is a ~microsecond syscall.

We use it as a gate in front of the capture loop: if no damage has
arrived since the last grab, we skip the entire `mss.grab` +
encoder + fan-out cycle and just sleep until the next tick. Static
desktops produce zero bytes on the wire and near-zero CPU.

Falls through silently on Wayland / non-X11 backends; callers just
get `has_damage() == True` forever and the existing block-hash
dirty-rect detector handles frame skipping as before.
"""

from __future__ import annotations

import ctypes
import logging
import os
import sys
import threading
from typing import Optional

log = logging.getLogger(__name__)


# ── ctypes — libX11 + libXdamage ─────────────────────────────────────


# DAMAGE REPORT LEVELS (from damageproto.h). "BoundingBox" means we
# get events whenever the outer bounding rectangle of a change
# moves — cheap and sufficient for our "something changed" gate.
_DAMAGE_REPORT_BOUNDING_BOX = 0
# NOTIFY_AREA is what the X server adds to the event type returned by
# XDamageQueryExtension().event_base. We compute it below.


def _load_libs():
    try:
        x11 = ctypes.CDLL("libX11.so.6")
        xd = ctypes.CDLL("libXdamage.so.1")
    except OSError as e:
        log.info("XDamage unavailable: %s", e)
        return None
    return (x11, xd)


_libs = _load_libs()


class _XEventBlob(ctypes.Structure):
    """XEvent is a union of 192 bytes on 64-bit Linux. We don't need
    to introspect fields — we just poll `type` off the front."""
    _fields_ = [("_pad", ctypes.c_uint8 * 192)]


class XDamageWatcher:
    """Watches the root window for any pixel change. Thread-safe
    read/clear; caller owns both.

    Typical flow inside the capture loop:

        w = XDamageWatcher()
        if w.open():
            ...
            while running:
                if not w.has_damage():
                    await asyncio.sleep(period)
                    continue
                img = capture()
                ...
    """

    def __init__(self):
        self._dpy: Optional[int] = None
        self._root: int = 0
        self._damage: int = 0
        self._event_base: int = 0
        self._x11 = None
        self._xd = None
        self._event = _XEventBlob()
        self._lock = threading.Lock()

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "linux":
            return False
        if not os.environ.get("DISPLAY"):
            return False
        if _libs is None:
            return False
        x11, xd = _libs

        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XDefaultRootWindow.restype = ctypes.c_ulong
        x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x11.XFlush.argtypes = [ctypes.c_void_p]
        x11.XPending.restype = ctypes.c_int
        x11.XPending.argtypes = [ctypes.c_void_p]
        x11.XNextEvent.argtypes = [ctypes.c_void_p,
                                   ctypes.POINTER(_XEventBlob)]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]

        xd.XDamageQueryExtension.restype = ctypes.c_int
        xd.XDamageQueryExtension.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
        ]
        xd.XDamageCreate.restype = ctypes.c_ulong
        xd.XDamageCreate.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
        ]
        xd.XDamageDestroy.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        xd.XDamageSubtract.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.c_ulong, ctypes.c_ulong,   # XserverRegion = XID
        ]

        dpy = x11.XOpenDisplay(None)
        if not dpy:
            log.info("XDamage: XOpenDisplay failed")
            return False
        ev_base = ctypes.c_int(0)
        err_base = ctypes.c_int(0)
        if not xd.XDamageQueryExtension(
                dpy, ctypes.byref(ev_base), ctypes.byref(err_base)):
            log.info("XDamage: extension not available on this X server")
            x11.XCloseDisplay(dpy)
            return False
        root = x11.XDefaultRootWindow(dpy)
        damage = xd.XDamageCreate(dpy, root,
                                  _DAMAGE_REPORT_BOUNDING_BOX)
        if not damage:
            log.info("XDamage: XDamageCreate failed")
            x11.XCloseDisplay(dpy)
            return False
        self._dpy = dpy
        self._root = int(root)
        self._damage = int(damage)
        self._event_base = int(ev_base.value)
        self._x11 = x11
        self._xd = xd
        # Prime by subtracting existing damage so the first has_damage
        # call returns False unless something actually changed after
        # setup.
        self._xd.XDamageSubtract(self._dpy, self._damage, 0, 0)
        log.info("XDamage watcher live on root 0x%x", self._root)
        return True

    def close(self) -> None:
        with self._lock:
            if self._xd and self._dpy and self._damage:
                try:
                    self._xd.XDamageDestroy(self._dpy, self._damage)
                except Exception:
                    pass
            if self._x11 and self._dpy:
                try:
                    self._x11.XCloseDisplay(self._dpy)
                except Exception:
                    pass
            self._dpy = None
            self._damage = 0

    # ── Query ────────────────────────────────────────────────────

    def has_damage(self) -> bool:
        """Drain every queued X event. Return True if any of them
        were damage events, then subtract — so the next call only
        returns True when NEW damage arrives after this point."""
        if self._dpy is None:
            return True    # safe default — assume damage
        with self._lock:
            saw_damage = False
            damage_ev_type = self._event_base + 0  # DamageNotify
            while self._x11.XPending(self._dpy) > 0:
                self._x11.XNextEvent(self._dpy,
                                     ctypes.byref(self._event))
                ev_type = self._event._pad[0]
                if ev_type == damage_ev_type:
                    saw_damage = True
            # Even when we didn't see an event we subtract — the X
            # server accumulates damage continuously; subtracting
            # moves it into "done" so the next XPending loop only
            # surfaces genuinely new changes.
            if saw_damage:
                self._xd.XDamageSubtract(self._dpy, self._damage, 0, 0)
            return saw_damage


def available() -> bool:
    return _libs is not None and sys.platform == "linux"

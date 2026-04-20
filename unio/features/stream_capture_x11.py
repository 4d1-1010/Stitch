"""Dedicated libX11 connection for fast overlay unmap/map during
capture.

Why this exists: the feedback-loop-breaker in shell.py needs to
temporarily hide our StreamWindow overlays around every mss.grab
call (when a panel is both a source AND a sink of a swap). Going
through Tk's event queue adds ~15–20 ms per frame because the
capture thread has to post to the main loop, wait for the `after`
handler to run, wait for `update_idletasks` to flush, wake up.

That's a lot of jitter at 15–30 fps. Worse, Tk serialises each
ordering with its own mutex — simultaneous hide/show for multiple
overlays stacks linearly.

This module opens a second libX11 connection owned by the capture
thread and does XUnmapWindow / XMapWindow + XSync directly. Total
cost: ~0.5 ms per full hide-grab-show cycle, independent of Tk.

The Tk Display and our second Display both talk to the same X
server; X multiplexes them. Unmap/map operations on a window
created by a DIFFERENT client (Tk) work fine as long as the X
server's security model allows it — standard single-user desktop
sessions do.
"""

from __future__ import annotations

import ctypes
import logging
from typing import Optional

log = logging.getLogger(__name__)


class XCaptureHelper:
    """Thin ctypes wrapper around libX11 for unmap/map + sync."""

    def __init__(self) -> None:
        self._lib: Optional[ctypes.CDLL] = None
        self._dpy = None

    def open(self) -> bool:
        try:
            lib = ctypes.CDLL("libX11.so.6")
        except OSError:
            return False
        lib.XOpenDisplay.restype = ctypes.c_void_p
        lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
        lib.XCloseDisplay.argtypes = [ctypes.c_void_p]
        lib.XUnmapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        lib.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        lib.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        dpy = lib.XOpenDisplay(None)
        if not dpy:
            return False
        self._lib = lib
        self._dpy = dpy
        log.info("XCaptureHelper opened second X display for "
                 "capture-thread overlay toggling")
        return True

    def close(self) -> None:
        if self._lib and self._dpy:
            try:
                self._lib.XCloseDisplay(self._dpy)
            except Exception:
                pass
        self._lib = None
        self._dpy = None

    def unmap_sync(self, xid: int) -> None:
        if not self._lib or not self._dpy:
            return
        try:
            self._lib.XUnmapWindow(self._dpy, ctypes.c_ulong(xid))
            self._lib.XSync(self._dpy, 0)
        except Exception:
            log.exception("XUnmapWindow failed for xid %d", xid)

    def map_sync(self, xid: int) -> None:
        if not self._lib or not self._dpy:
            return
        try:
            self._lib.XMapWindow(self._dpy, ctypes.c_ulong(xid))
            self._lib.XSync(self._dpy, 0)
        except Exception:
            log.exception("XMapWindow failed for xid %d", xid)

"""mss-based screen capture for Windows with hide-during-capture.

Why this file exists instead of just using mss:
  * mss.grab reads the GDI framebuffer via BitBlt. It captures every
    pixel on screen — including our own StreamWindow overlay. Without
    any mitigation that feeds back into itself (feedback cascade on
    the sink peer).
  * PrintWindow with PW_RENDERFULLCONTENT returns all-black bitmaps
    for every window on modern Windows 10/11 because DirectComposition-
    backed windows (basically everything: Explorer shell, taskbar,
    modern apps) can't be rendered that way. That's what made
    PerWindowCapture unusable — diagnostics showed avg=0.0 for
    `Progman`, `Shell_TrayWnd`, `CabinetWClass`, and our own Tk
    overlay alike. Dead architecturally.

Solution: use mss, but set the registered overlay HWNDs to
``LWA_ALPHA 0`` (fully transparent) via SetLayeredWindowAttributes
right before each ``sct.grab``, restore alpha=255 right after.
On Windows the screen briefly shows whatever's underneath the overlay
— but the user running the swap is on the OTHER peer, so they only
see the stream of the post-hide frame, not the physical flicker on
this machine.

Hiding via alpha-toggle (rather than ShowWindow(SW_HIDE)) is much
cheaper because DWM just stops compositing the layered surface for
one frame; the HWND stays in Z-order, stays focused, doesn't repaint.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import sys
import threading
import time
from typing import Iterable

log = logging.getLogger(__name__)


GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
LWA_ALPHA = 0x02


class MssHideCapture:
    """mss.grab wrapped with an overlay-alpha dance so the overlay's
    pixels never end up in the streamed frame."""

    def __init__(self) -> None:
        self.user32 = None
        self.width = 0
        self.height = 0
        self.origin_x = 0
        self.origin_y = 0
        self._exclude_hwnds: set = set()
        self._exclude_lock = threading.Lock()
        self._tls = threading.local()

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        try:
            import mss  # noqa: F401  -- smoke test that it's importable
        except Exception:
            log.exception("mss not importable")
            return False
        try:
            user32 = ctypes.WinDLL("user32", use_last_error=True)
        except OSError:
            log.exception("user32 not loadable")
            return False
        user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
        user32.GetWindowLongW.restype = ctypes.c_long
        user32.SetWindowLongW.argtypes = [
            wt.HWND, ctypes.c_int, ctypes.c_long]
        user32.SetWindowLongW.restype = ctypes.c_long
        user32.SetLayeredWindowAttributes.argtypes = [
            wt.HWND, ctypes.c_uint, ctypes.c_ubyte, ctypes.c_uint]
        user32.SetLayeredWindowAttributes.restype = ctypes.c_int
        user32.GetSystemMetrics.argtypes = [ctypes.c_int]
        user32.GetSystemMetrics.restype = ctypes.c_int
        user32.IsWindow.argtypes = [wt.HWND]
        user32.IsWindow.restype = wt.BOOL
        self.user32 = user32
        self.origin_x = user32.GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
        self.origin_y = user32.GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
        self.width = user32.GetSystemMetrics(78)      # SM_CXVIRTUALSCREEN
        self.height = user32.GetSystemMetrics(79)     # SM_CYVIRTUALSCREEN
        log.info("MssHideCapture ready: virtual desktop %dx%d @ (%d,%d)",
                 self.width, self.height,
                 self.origin_x, self.origin_y)
        return True

    def close(self) -> None:
        pass

    # ── Exclusion list ──────────────────────────────────────────

    def set_exclude_hwnds(self, hwnds: Iterable[int]) -> None:
        as_set = {int(h) for h in (hwnds or ()) if h}
        with self._exclude_lock:
            self._exclude_hwnds = as_set
        log.info("mss_hide set_exclude_hwnds: %d HWND(s) = %s",
                 len(as_set),
                 ", ".join(f"0x{h:x}" for h in sorted(as_set)))

    set_exclude_xids = set_exclude_hwnds

    # ── Grab ────────────────────────────────────────────────────

    def _set_alpha(self, hwnd: int, alpha: int) -> bool:
        """Force the HWND to the requested layered alpha. Ensures
        WS_EX_LAYERED is set first; without it SetLayeredWindowAttributes
        silently fails. Returns True on apparent success."""
        try:
            cur = self.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            if not (cur & WS_EX_LAYERED):
                self.user32.SetWindowLongW(
                    hwnd, GWL_EXSTYLE, cur | WS_EX_LAYERED)
            return bool(self.user32.SetLayeredWindowAttributes(
                hwnd, 0, alpha & 0xFF, LWA_ALPHA))
        except Exception:
            log.exception("SetLayeredWindowAttributes failed hwnd=0x%x",
                          hwnd)
            return False

    def grab(self, rect: dict):
        """Hide every registered overlay HWND (alpha=0), grab the
        desktop via mss, restore alpha=255. Returns a PIL.Image
        cropped to ``rect``."""
        import mss
        from PIL import Image
        with self._exclude_lock:
            hide = list(self._exclude_hwnds)
        # Guard against HWNDs that got destroyed between runs.
        hide = [h for h in hide if self.user32.IsWindow(h)]
        # Flip to alpha=0.
        for h in hide:
            self._set_alpha(h, 0)
        try:
            sct = getattr(self._tls, "sct", None)
            if sct is None:
                sct = mss.mss()
                self._tls.sct = sct
            region = {
                "left": int(rect.get("x", self.origin_x)),
                "top": int(rect.get("y", self.origin_y)),
                "width": int(rect.get("width", self.width)),
                "height": int(rect.get("height", self.height)),
            }
            raw = sct.grab(region)
            img = Image.frombytes("RGB", raw.size, raw.rgb)
        finally:
            for h in hide:
                self._set_alpha(h, 255)
        # Cheap brightness telemetry, throttled to ~1 Hz.
        try:
            now = time.monotonic()
            last = getattr(self, "_last_log_at", 0.0)
            if now - last > 1.0:
                pixels = list(img.getdata())
                sample = pixels[::max(1, len(pixels) // 1000)]
                avg = (sum(p[0] + p[1] + p[2] for p in sample)
                       / max(1, 3 * len(sample)))
                log.info(
                    "mss_hide grab rect=%dx%d hidden=%d avg=%.1f",
                    img.width, img.height, len(hide), avg)
                self._last_log_at = now
        except Exception:
            pass
        return img


def available() -> bool:
    if sys.platform != "win32":
        return False
    try:
        import mss  # noqa: F401
        ctypes.WinDLL("user32")
        return True
    except Exception:
        return False

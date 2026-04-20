"""BitBlt(SRCCOPY) screen capture for Windows — auto-excludes layered windows.

Documented MSDN behavior:
  * When BitBlt copies from a screen DC with just ``SRCCOPY`` (no
    ``CAPTUREBLT`` flag), the resulting bitmap does NOT include
    layered windows (``WS_EX_LAYERED``).
  * When ``CAPTUREBLT`` is OR'd in, layered windows ARE included.

mss.windows uses ``SRCCOPY | CAPTUREBLT``, which is why every earlier
test saw our StreamWindow overlay feed back into its own stream —
the overlay is layered (Tk's ``-alpha`` sets ``WS_EX_LAYERED``).

Dropping the ``CAPTUREBLT`` bit gives us exactly the behaviour we
want: mss-style fullscreen capture that is genuinely oblivious to
any layered overlay we create. No WDA, no cloak, no hide-during-
capture, no native D3D workaround — just use the right flag.

On the shell side we still push overlay HWNDs to this backend's
``set_exclude_hwnds`` (plural) so each one can be re-confirmed
WS_EX_LAYERED before every grab — Tk sometimes drops the bit after
an ``overrideredirect`` call and we re-apply it defensively.
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


# BitBlt raster op — SRCCOPY alone, NO CAPTUREBLT.
SRCCOPY = 0x00CC0020

# SetLayeredWindowAttributes constants.
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
LWA_ALPHA = 0x02

# SM_ constants for virtual screen metrics.
SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD),
        ("biWidth", wt.LONG),
        ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD),
        ("biBitCount", wt.WORD),
        ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD),
        ("biXPelsPerMeter", wt.LONG),
        ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD),
        ("biClrImportant", wt.DWORD),
    ]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [
        ("bmiHeader", _BITMAPINFOHEADER),
        ("bmiColors", wt.DWORD * 3),
    ]


class BitBltCapture:
    """Screen capture via BitBlt(SRCCOPY) without CAPTUREBLT. Our
    Tk StreamWindow overlays (layered, alpha-controlled) are
    excluded from the capture at the OS level, no extra dance
    required."""

    def __init__(self) -> None:
        self.user32 = None
        self.gdi32 = None
        self.screen_dc = None
        self.mem_dc = None
        self.bitmap = None
        self.width = 0
        self.height = 0
        self.origin_x = 0
        self.origin_y = 0
        self._buffer = None
        self._exclude_hwnds: set = set()
        self._exclude_lock = threading.Lock()
        self._last_log_at = 0.0

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        try:
            return self._open_win32()
        except Exception:
            log.exception("BitBlt capture init failed")
            self.close()
            return False

    def _open_win32(self) -> bool:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

        user32.GetDC.argtypes = [wt.HWND]
        user32.GetDC.restype = wt.HDC
        user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
        user32.ReleaseDC.restype = ctypes.c_int
        user32.GetSystemMetrics.argtypes = [ctypes.c_int]
        user32.GetSystemMetrics.restype = ctypes.c_int
        user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
        user32.GetWindowLongW.restype = ctypes.c_long
        user32.SetWindowLongW.argtypes = [
            wt.HWND, ctypes.c_int, ctypes.c_long]
        user32.SetWindowLongW.restype = ctypes.c_long
        user32.SetLayeredWindowAttributes.argtypes = [
            wt.HWND, ctypes.c_uint, ctypes.c_ubyte, ctypes.c_uint]
        user32.SetLayeredWindowAttributes.restype = ctypes.c_int
        user32.IsWindow.argtypes = [wt.HWND]
        user32.IsWindow.restype = wt.BOOL

        gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
        gdi32.CreateCompatibleDC.restype = wt.HDC
        gdi32.CreateCompatibleBitmap.argtypes = [
            wt.HDC, ctypes.c_int, ctypes.c_int]
        gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
        gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
        gdi32.SelectObject.restype = wt.HGDIOBJ
        gdi32.DeleteDC.argtypes = [wt.HDC]
        gdi32.DeleteDC.restype = wt.BOOL
        gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
        gdi32.DeleteObject.restype = wt.BOOL
        gdi32.BitBlt.argtypes = [
            wt.HDC, ctypes.c_int, ctypes.c_int,
            ctypes.c_int, ctypes.c_int,
            wt.HDC, ctypes.c_int, ctypes.c_int,
            wt.DWORD,
        ]
        gdi32.BitBlt.restype = wt.BOOL
        gdi32.GetDIBits.argtypes = [
            wt.HDC, wt.HBITMAP, ctypes.c_uint, ctypes.c_uint,
            ctypes.c_void_p, ctypes.POINTER(_BITMAPINFO),
            ctypes.c_uint,
        ]
        gdi32.GetDIBits.restype = ctypes.c_int

        self.user32 = user32
        self.gdi32 = gdi32

        self.origin_x = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
        self.origin_y = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
        self.width = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
        self.height = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
        if self.width <= 0 or self.height <= 0:
            log.info("Invalid virtual screen metrics")
            return False

        self.screen_dc = user32.GetDC(None)   # screen DC
        if not self.screen_dc:
            log.info("GetDC(NULL) failed")
            return False
        self.mem_dc = gdi32.CreateCompatibleDC(self.screen_dc)
        if not self.mem_dc:
            log.info("CreateCompatibleDC failed")
            return False
        self.bitmap = gdi32.CreateCompatibleBitmap(
            self.screen_dc, self.width, self.height)
        if not self.bitmap:
            log.info("CreateCompatibleBitmap failed")
            return False
        gdi32.SelectObject(self.mem_dc, self.bitmap)
        self._buffer = (ctypes.c_ubyte
                        * (self.width * self.height * 4))()
        log.info("BitBltCapture ready: %dx%d @ (%d,%d) — "
                 "SRCCOPY without CAPTUREBLT excludes layered "
                 "overlays",
                 self.width, self.height,
                 self.origin_x, self.origin_y)
        return True

    def close(self) -> None:
        gdi32 = self.gdi32
        user32 = self.user32
        if gdi32 and self.bitmap:
            try:
                gdi32.DeleteObject(self.bitmap)
            except Exception:
                pass
            self.bitmap = None
        if gdi32 and self.mem_dc:
            try:
                gdi32.DeleteDC(self.mem_dc)
            except Exception:
                pass
            self.mem_dc = None
        if user32 and self.screen_dc:
            try:
                user32.ReleaseDC(None, self.screen_dc)
            except Exception:
                pass
            self.screen_dc = None

    # ── Exclusion list ──────────────────────────────────────────

    def set_exclude_hwnds(self, hwnds: Iterable[int]) -> None:
        as_set = {int(h) for h in (hwnds or ()) if h}
        with self._exclude_lock:
            self._exclude_hwnds = as_set
        log.info("bitblt set_exclude_hwnds: %d HWND(s) = %s",
                 len(as_set),
                 ", ".join(f"0x{h:x}" for h in sorted(as_set)))

    set_exclude_xids = set_exclude_hwnds

    # ── Grab ────────────────────────────────────────────────────

    def grab(self, rect: dict):
        """Return a PIL.Image cropped to ``rect``. The BitBlt call
        uses plain SRCCOPY — layered windows are automatically
        excluded by the OS. We still defensively re-assert
        WS_EX_LAYERED on each registered exclusion HWND before
        each grab, because Tk's overrideredirect(True) sometimes
        strips the bit."""
        if not self.mem_dc or not self.gdi32:
            return None
        # Defensive: make sure every excluded HWND is still layered
        # (Tk can strip WS_EX_LAYERED after style changes).
        with self._exclude_lock:
            hwnds = list(self._exclude_hwnds)
        for h in hwnds:
            if not self.user32.IsWindow(h):
                continue
            cur = self.user32.GetWindowLongW(h, GWL_EXSTYLE)
            if not (cur & WS_EX_LAYERED):
                self.user32.SetWindowLongW(
                    h, GWL_EXSTYLE, cur | WS_EX_LAYERED)
                self.user32.SetLayeredWindowAttributes(
                    h, 0, 255, LWA_ALPHA)

        # BitBlt the full virtual desktop into our mem DC, then
        # crop to the requested rect.
        try:
            ok = bool(self.gdi32.BitBlt(
                self.mem_dc, 0, 0, self.width, self.height,
                self.screen_dc, self.origin_x, self.origin_y,
                SRCCOPY,
            ))
        except Exception:
            log.exception("BitBlt raised")
            return None
        if not ok:
            log.info("BitBlt returned 0")
            return None

        bmi = _BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = self.width
        bmi.bmiHeader.biHeight = -self.height   # top-down DIB
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bmi.bmiHeader.biCompression = 0   # BI_RGB
        try:
            got = self.gdi32.GetDIBits(
                self.mem_dc, self.bitmap, 0, self.height,
                self._buffer, ctypes.byref(bmi), 0,
            )
        except Exception:
            log.exception("GetDIBits raised")
            return None
        if got <= 0:
            return None

        try:
            from PIL import Image
            img = Image.frombuffer(
                "RGBA", (self.width, self.height),
                bytes(self._buffer),
                "raw", "BGRA", 0, 1,
            ).convert("RGB")
            if rect:
                x = int(rect.get("x", 0)) - self.origin_x
                y = int(rect.get("y", 0)) - self.origin_y
                w = int(rect.get("width", self.width))
                h = int(rect.get("height", self.height))
                x = max(0, min(x, self.width))
                y = max(0, min(y, self.height))
                img = img.crop((x, y, x + w, y + h))
        except Exception:
            log.exception("PIL build / crop failed")
            return None

        # Telemetry throttled to ~1 Hz.
        try:
            now = time.monotonic()
            if now - self._last_log_at > 1.0:
                pixels = list(img.getdata())
                sample = pixels[::max(1, len(pixels) // 1000)]
                avg = (sum(p[0] + p[1] + p[2] for p in sample)
                       / max(1, 3 * len(sample)))
                log.info(
                    "bitblt grab %dx%d excluded=%d avg=%.1f",
                    img.width, img.height, len(hwnds), avg)
                self._last_log_at = now
        except Exception:
            pass
        return img


def available() -> bool:
    if sys.platform != "win32":
        return False
    try:
        ctypes.WinDLL("user32")
        ctypes.WinDLL("gdi32")
        return True
    except OSError:
        return False

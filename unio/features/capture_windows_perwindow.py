"""Per-window PrintWindow capture for Windows.

The Windows analogue of ``capture_xcomposite.py``. Instead of asking
DWM for the whole composed desktop (which would include our
StreamWindow overlay and feed back into itself), we enumerate every
top-level HWND in Z-order, skip the ones belonging to our own
overlays, ``PrintWindow`` each of the rest into a per-window bitmap,
and composite them bottom-to-top into a single output image.

This mirrors exactly what XComposite does on Linux: per-window reads
plus a manual composite, with user-controlled exclusion. No cloak
dance, no DwmFlush wait, no WDA shenanigans — just enumerate, read,
combine.

Limitations:
  * Full-screen DirectX / Vulkan games that present exclusively
    without going through DWM won't show up. For those we'd need
    DXGI Desktop Duplication, which doesn't honour WDA anyway.
  * Performance scales with the number of visible windows — for a
    desktop with 5–20 windows this is well under the 66 ms budget
    at 15 fps; a desktop with 200 windows would be too slow and
    we'd want a different strategy, but that's not the common case.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import os
import sys
import threading
import time
from typing import Iterable, Optional

log = logging.getLogger(__name__)


# PrintWindow flag — route through DWM and include per-window
# compositor effects (animations, shadows, DirectX content).
PW_RENDERFULLCONTENT = 0x00000002

# GetWindowLong indices.
GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_VISIBLE = 0x10000000
WS_MINIMIZE = 0x20000000

# DwmGetWindowAttribute indices.
DWMWA_CLOAKED = 14

# GetDIBits constants.
BI_RGB = 0
DIB_RGB_COLORS = 0


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


class PerWindowCapture:
    """PrintWindow per HWND + manual composite. Single consumer —
    drive from the capture-loop thread."""

    def __init__(self) -> None:
        self.user32 = None
        self.gdi32 = None
        self.dwmapi = None
        self.width = 0
        self.height = 0
        self.origin_x = 0
        self.origin_y = 0
        self._exclude_hwnds: set = set()
        self._exclude_lock = threading.Lock()
        self._my_pid = os.getpid()

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        try:
            return self._open_win32()
        except Exception:
            log.exception("PerWindow capture init failed")
            return False

    def _open_win32(self) -> bool:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        try:
            dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
        except OSError:
            dwmapi = None

        user32.EnumWindows.argtypes = [
            ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p),
            ctypes.c_void_p,
        ]
        user32.EnumWindows.restype = ctypes.c_int
        user32.IsWindowVisible.argtypes = [wt.HWND]
        user32.IsWindowVisible.restype = wt.BOOL
        user32.GetWindowRect.argtypes = [
            wt.HWND, ctypes.POINTER(wt.RECT)]
        user32.GetWindowRect.restype = wt.BOOL
        user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
        user32.GetWindowLongW.restype = ctypes.c_long
        user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
        user32.PrintWindow.restype = wt.BOOL
        user32.GetDC.argtypes = [wt.HWND]
        user32.GetDC.restype = wt.HDC
        user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
        user32.ReleaseDC.restype = ctypes.c_int
        user32.GetSystemMetrics.argtypes = [ctypes.c_int]
        user32.GetSystemMetrics.restype = ctypes.c_int
        user32.GetWindowThreadProcessId.argtypes = [
            wt.HWND, ctypes.POINTER(ctypes.c_ulong)]
        user32.GetWindowThreadProcessId.restype = ctypes.c_ulong
        user32.GetClassNameW.argtypes = [
            wt.HWND, ctypes.c_wchar_p, ctypes.c_int]
        user32.GetClassNameW.restype = ctypes.c_int
        user32.GetDesktopWindow.restype = wt.HWND

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
        gdi32.GetDIBits.argtypes = [
            wt.HDC, wt.HBITMAP, ctypes.c_uint, ctypes.c_uint,
            ctypes.c_void_p, ctypes.POINTER(_BITMAPINFO),
            ctypes.c_uint,
        ]
        gdi32.GetDIBits.restype = ctypes.c_int

        if dwmapi:
            dwmapi.DwmGetWindowAttribute.argtypes = [
                wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]
            dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long

        self.user32 = user32
        self.gdi32 = gdi32
        self.dwmapi = dwmapi

        # Virtual desktop span so we know the universe of coordinates.
        self.origin_x = user32.GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
        self.origin_y = user32.GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
        self.width = user32.GetSystemMetrics(78)      # SM_CXVIRTUALSCREEN
        self.height = user32.GetSystemMetrics(79)     # SM_CYVIRTUALSCREEN
        if self.width <= 0 or self.height <= 0:
            log.info("Invalid virtual screen metrics")
            return False

        log.info("PerWindowCapture ready: virtual desktop "
                 "%dx%d @ (%d,%d)",
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
        log.info("perwindow set_exclude_hwnds: %d HWND(s) = %s",
                 len(as_set),
                 ", ".join(f"0x{h:x}" for h in sorted(as_set)))

    # Generic alias for the display_stream wiring.
    set_exclude_xids = set_exclude_hwnds

    # ── Grab ────────────────────────────────────────────────────

    def grab(self, rect: dict):
        """Enumerate every visible top-level window in bottom-to-top
        Z-order, PrintWindow each one that isn't in the exclusion
        set, and paste the result into an output image cropped to
        ``rect``. Windows that don't intersect ``rect`` are skipped
        so we don't waste PrintWindow bandwidth on off-screen apps."""
        from PIL import Image
        if not self.user32:
            return None
        rx = int(rect.get("x", 0))
        ry = int(rect.get("y", 0))
        rw = int(rect.get("width", self.width))
        rh = int(rect.get("height", self.height))
        if rw <= 0 or rh <= 0:
            return None

        with self._exclude_lock:
            excludes = set(self._exclude_hwnds)

        # EnumWindows returns top-to-bottom Z-order; we want bottom-
        # to-top for painting so lower windows land underneath.
        hwnds_top_down: list[int] = []

        PROC = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)

        def enum_proc(hwnd, _lp):
            hwnds_top_down.append(int(hwnd))
            return 1

        proc = PROC(enum_proc)
        self.user32.EnumWindows(proc, None)

        out = Image.new("RGB", (rw, rh), (0, 0, 0))
        # Reverse — bottom first so each later paste lands on top.
        skipped_excluded = 0
        skipped_invisible = 0
        painted = 0
        painted_details: list[str] = []
        for hwnd in reversed(hwnds_top_down):
            if hwnd in excludes:
                skipped_excluded += 1
                continue
            if not self._should_capture(hwnd):
                skipped_invisible += 1
                continue
            ok, klass, ww, wh = self._paint_one_verbose(
                hwnd, rx, ry, rw, rh, out)
            if ok:
                painted += 1
                if len(painted_details) < 8:
                    painted_details.append(
                        f"0x{hwnd:x}({klass!r},{ww}x{wh})")
        # Throttle to once per second. No pixel avg — iterating
        # 2 M PIL pixels in Python costs hundreds of ms per grab
        # and was the dominant bottleneck in the capture path.
        now = time.monotonic()
        last = getattr(self, "_last_log_at", 0.0)
        if now - last > 1.0:
            log.info(
                "perwindow grab: enum=%d excluded=%d invisible=%d "
                "painted=%d  painted_list=[%s]",
                len(hwnds_top_down), skipped_excluded,
                skipped_invisible, painted,
                "; ".join(painted_details))
            self._last_log_at = now
        return out

    def _should_capture(self, hwnd: int) -> bool:
        user32 = self.user32
        if not user32.IsWindowVisible(hwnd):
            return False
        style = user32.GetWindowLongW(hwnd, GWL_STYLE)
        if style & WS_MINIMIZE:
            return False
        # Skip "cloaked" windows (virtual desktops parking invisible
        # apps, DWM-hidden UWP shells, etc.).
        if self.dwmapi:
            cloaked = ctypes.c_int(0)
            try:
                self.dwmapi.DwmGetWindowAttribute(
                    hwnd, DWMWA_CLOAKED,
                    ctypes.byref(cloaked),
                    ctypes.sizeof(cloaked),
                )
                if cloaked.value:
                    return False
            except Exception:
                pass
        return True

    def _paint_one(self, hwnd, rx, ry, rw, rh, out):
        ok, *_ = self._paint_one_verbose(hwnd, rx, ry, rw, rh, out)
        return ok

    def _paint_one_verbose(self, hwnd: int,
                           rx: int, ry: int, rw: int, rh: int,
                           out):
        """Return (painted, class_name, w, h). painted=True when the
        PrintWindow call succeeded and the bitmap was pasted. Error
        paths return (False, '', 0, 0) so the caller can distinguish
        'skipped' from 'painted' for diagnostics."""
        from PIL import Image
        user32 = self.user32
        gdi32 = self.gdi32
        klass_buf = (ctypes.c_wchar * 64)()
        try:
            user32.GetClassNameW(hwnd, klass_buf, 64)
        except Exception:
            pass
        klass_name = klass_buf.value
        wrect = wt.RECT()
        if not user32.GetWindowRect(hwnd, ctypes.byref(wrect)):
            return (False, klass_name, 0, 0)
        wx = int(wrect.left)
        wy = int(wrect.top)
        ww = int(wrect.right - wrect.left)
        wh = int(wrect.bottom - wrect.top)
        if ww <= 0 or wh <= 0:
            return (False, klass_name, ww, wh)
        # Reject windows way off screen (virtual desktop parking, etc.).
        if wx + ww <= rx or wy + wh <= ry or wx >= rx + rw or wy >= ry + rh:
            return (False, klass_name, ww, wh)

        # Create per-window DIB of the exact size of the window.
        screen_dc = user32.GetDC(0)
        if not screen_dc:
            return (False, klass_name, ww, wh)
        try:
            mem_dc = gdi32.CreateCompatibleDC(screen_dc)
            if not mem_dc:
                return (False, klass_name, ww, wh)
            try:
                bitmap = gdi32.CreateCompatibleBitmap(
                    screen_dc, ww, wh)
                if not bitmap:
                    return (False, klass_name, ww, wh)
                try:
                    gdi32.SelectObject(mem_dc, bitmap)
                    ok = False
                    try:
                        ok = bool(user32.PrintWindow(
                            hwnd, mem_dc, PW_RENDERFULLCONTENT))
                    except Exception:
                        return (False, klass_name, ww, wh)
                    if not ok:
                        return (False, klass_name, ww, wh)
                    bmi = _BITMAPINFO()
                    bmi.bmiHeader.biSize = ctypes.sizeof(
                        _BITMAPINFOHEADER)
                    bmi.bmiHeader.biWidth = ww
                    bmi.bmiHeader.biHeight = -wh  # top-down
                    bmi.bmiHeader.biPlanes = 1
                    bmi.bmiHeader.biBitCount = 32
                    bmi.bmiHeader.biCompression = BI_RGB
                    buf = (ctypes.c_ubyte * (ww * wh * 4))()
                    got = gdi32.GetDIBits(
                        mem_dc, bitmap, 0, wh,
                        buf, ctypes.byref(bmi), DIB_RGB_COLORS,
                    )
                    if got <= 0:
                        return (False, klass_name, ww, wh)
                    try:
                        img = Image.frombuffer(
                            "RGBA", (ww, wh), bytes(buf),
                            "raw", "BGRA", 0, 1,
                        ).convert("RGB")
                    except Exception:
                        return (False, klass_name, ww, wh)
                    # Paste at (wx - rx, wy - ry). PIL clips at
                    # the destination edges for us.
                    out.paste(img, (wx - rx, wy - ry))
                    return (True, klass_name, ww, wh)
                finally:
                    gdi32.DeleteObject(bitmap)
            finally:
                gdi32.DeleteDC(mem_dc)
        finally:
            user32.ReleaseDC(0, screen_dc)


def available() -> bool:
    if sys.platform != "win32":
        return False
    try:
        ctypes.WinDLL("user32")
        ctypes.WinDLL("gdi32")
        return True
    except OSError:
        return False

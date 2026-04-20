"""PrintWindow-based desktop capture for Windows.

mss.grab uses GDI ``BitBlt`` which reads the display buffer
directly and picks up every pixel on screen — including windows
flagged with ``WDA_EXCLUDEFROMCAPTURE``. That's what caused the
feedback loop on Diana.

``PrintWindow`` with ``PW_RENDERFULLCONTENT`` (flag 0x00000002)
routes the capture through DWM. Per the Microsoft docs,
windows with display affinity ``WDA_EXCLUDEFROMCAPTURE`` are
rendered BLACK or omitted from the resulting bitmap. Since
``StreamWindow._try_exclude_win32`` already sets that flag on
our overlays, they simply don't appear in the captured pixels —
no hide/show cycles, no flicker.

Simpler alternative to a full DXGI Desktop Duplication ctypes
wrapper. Falls back to mss when anything goes wrong.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import sys
from typing import Optional

log = logging.getLogger(__name__)


# PrintWindow flags
PW_RENDERFULLCONTENT = 0x00000002
# BitBlt raster op
SRCCOPY = 0x00CC0020
# GetDeviceCaps index for "this monitor in pixels"
DESKTOPHORZRES = 118
DESKTOPVERTRES = 117


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


class PrintWindowCapture:
    """Desktop-window capture via ``PrintWindow`` + ``GetDIBits``.

    Instance state is the cached device context + bitmap sized to
    the full virtual desktop. Grabbing is a blit + dib-read; no
    GPU buffer allocations in the hot path.
    """

    def __init__(self) -> None:
        self.user32 = None
        self.gdi32 = None
        self.dwmapi = None
        self.hwnd_desktop = 0
        self.screen_dc = None
        self.mem_dc = None
        self.bitmap = None
        self.width = 0
        self.height = 0
        self.origin_x = 0
        self.origin_y = 0
        self._buffer = None
        # Overlay HWNDs we must cloak out of the DWM composition
        # immediately before each PrintWindow grab, uncloak right
        # after. Without cloaking (and without WDA), our overlay's
        # pixels end up in the capture → feedback cascade. WDA does
        # exclude the overlay but renders the area as black in the
        # capture, which is equally useless. Cloaking removes the
        # window from DWM so the capture sees the real desktop
        # underneath.
        import threading as _threading
        self._exclude_hwnds: set = set()
        self._exclude_lock = _threading.Lock()

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        try:
            return self._open_win32()
        except Exception:
            log.exception("PrintWindow capture init failed")
            self.close()
            return False

    def _open_win32(self) -> bool:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

        user32.GetDesktopWindow.restype = wt.HWND
        user32.GetDC.argtypes = [wt.HWND]
        user32.GetDC.restype = wt.HDC
        user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
        user32.ReleaseDC.restype = ctypes.c_int
        user32.GetSystemMetrics.argtypes = [ctypes.c_int]
        user32.GetSystemMetrics.restype = ctypes.c_int
        user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, ctypes.c_uint]
        user32.PrintWindow.restype = wt.BOOL

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

        try:
            dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)
            dwmapi.DwmSetWindowAttribute.argtypes = [
                wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD]
            dwmapi.DwmSetWindowAttribute.restype = ctypes.c_long
            dwmapi.DwmFlush.restype = ctypes.c_long
            self.dwmapi = dwmapi
        except OSError:
            log.info("dwmapi unavailable — cloak-during-grab disabled")
            self.dwmapi = None

        self.user32 = user32
        self.gdi32 = gdi32
        self.hwnd_desktop = user32.GetDesktopWindow()
        if not self.hwnd_desktop:
            log.info("GetDesktopWindow returned NULL")
            return False

        # Virtual desktop spans all monitors on Windows.
        self.origin_x = user32.GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
        self.origin_y = user32.GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
        self.width = user32.GetSystemMetrics(78)      # SM_CXVIRTUALSCREEN
        self.height = user32.GetSystemMetrics(79)     # SM_CYVIRTUALSCREEN
        if self.width <= 0 or self.height <= 0:
            log.info("Invalid virtual screen metrics")
            return False

        self.screen_dc = user32.GetDC(self.hwnd_desktop)
        if not self.screen_dc:
            log.info("GetDC(desktop) failed")
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
        self._buffer = (ctypes.c_ubyte * (self.width * self.height * 4))()
        log.info("PrintWindowCapture ready: %dx%d @ (%d,%d)",
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
        if user32 and self.screen_dc and self.hwnd_desktop:
            try:
                user32.ReleaseDC(self.hwnd_desktop, self.screen_dc)
            except Exception:
                pass
            self.screen_dc = None

    # ── Exclude HWNDs (shell pushes overlay HWNDs here) ──────────

    def set_exclude_hwnds(self, hwnds) -> None:
        """Register HWNDs to cloak out of the DWM composition for
        the duration of every subsequent grab. The shell calls this
        each time a StreamWindow overlay is created or destroyed so
        the overlay's own pixels never make it into the capture."""
        with self._exclude_lock:
            self._exclude_hwnds = {int(h) for h in (hwnds or ()) if h}

    # Alias so the display_stream plumbing's generic `set_exclude_xids`
    # wiring finds the method regardless of platform naming.
    set_exclude_xids = set_exclude_hwnds

    def _cloak(self, hwnd: int, on: bool) -> None:
        if not self.dwmapi or not hwnd:
            return
        # DWMWA_CLOAK = 13. Value is a BOOL (int32).
        val = ctypes.c_int(1 if on else 0)
        try:
            self.dwmapi.DwmSetWindowAttribute(
                hwnd, 13, ctypes.byref(val), ctypes.sizeof(val))
        except Exception:
            log.exception("DwmSetWindowAttribute cloak=%s failed "
                          "(hwnd=%d)", on, hwnd)

    # ── Grab ─────────────────────────────────────────────────────

    def grab(self, rect: dict):
        """Return a PIL.Image cropped to ``rect``. Before calling
        PrintWindow, cloak every overlay HWND registered via
        ``set_exclude_hwnds`` out of DWM so the capture sees the
        real desktop underneath. Uncloak in the finally block so
        the overlays are visible again by the time DWM composes
        the next frame."""
        if not self.mem_dc or not self.gdi32:
            return None
        with self._exclude_lock:
            cloak_hwnds = list(self._exclude_hwnds)
        for h in cloak_hwnds:
            self._cloak(h, True)
        if cloak_hwnds and self.dwmapi:
            # Block until DWM has composed a frame without the
            # cloaked overlays — otherwise PrintWindow can still
            # see the stale frame that includes them.
            try:
                self.dwmapi.DwmFlush()
            except Exception:
                pass
        try:
            # PrintWindow into our memory DC.
            ok = False
            try:
                ok = bool(self.user32.PrintWindow(
                    self.hwnd_desktop, self.mem_dc,
                    PW_RENDERFULLCONTENT))
            except Exception:
                log.exception("PrintWindow raised")
            return self._grab_with_ok(ok, rect)
        finally:
            for h in cloak_hwnds:
                self._cloak(h, False)

    def _grab_with_ok(self, ok: bool, rect: dict):
        if not ok:
            # Fallback path. Still copies pixels so the user gets
            # something on screen while we diagnose. BitBlt bypasses
            # DWM so it doesn't honour cloak either, but the pixels
            # it copies are at least current.
            try:
                self.gdi32.BitBlt(
                    self.mem_dc, 0, 0, self.width, self.height,
                    self.screen_dc, self.origin_x, self.origin_y,
                    SRCCOPY,
                )
            except Exception:
                log.exception("BitBlt fallback raised")
                return None
        return self._read_dib_to_pil(rect)

    def _read_dib_to_pil(self, rect: dict):
        """Read the mem-DC bitmap back via GetDIBits and wrap it in
        a PIL.Image, cropped to ``rect``. Extracted from grab() so
        the cloak/uncloak dance around the PrintWindow call lives
        in one compact block."""
        # Read the full bitmap back.
        bmi = _BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = self.width
        # Negative biHeight = top-down DIB, so rows come back in
        # the natural order for PIL.
        bmi.bmiHeader.biHeight = -self.height
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
            return img
        except Exception:
            log.exception("frombuffer failed")
            return None


def available() -> bool:
    if sys.platform != "win32":
        return False
    try:
        ctypes.WinDLL("user32")
        ctypes.WinDLL("gdi32")
        return True
    except OSError:
        return False

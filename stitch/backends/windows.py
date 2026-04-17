"""
Windows input backend.

Uses:
  - user32.dll  for cursor, input injection (SendInput), low-level hooks
  - kernel32.dll for module handle
  - Win32 clipboard API

Requires no external dependencies — everything is via ctypes + windll.
"""

import ctypes
import ctypes.wintypes as wt
import logging
import struct
import sys
import threading
from typing import Callable, Optional

from . import InputBackend, MonitorRect
from .keycodes import vk_to_hid, hid_to_vk

log = logging.getLogger(__name__)

if sys.platform != "win32":
    raise ImportError("WindowsBackend is only available on Windows.")

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# Clipboard APIs return HANDLEs and BOOLs — without argtypes, ctypes
# truncates them to 32-bit ints, breaking the clipboard entirely on
# 64-bit Windows.
user32.OpenClipboard.argtypes = [wt.HWND]
user32.OpenClipboard.restype = wt.BOOL
user32.CloseClipboard.argtypes = []
user32.CloseClipboard.restype = wt.BOOL
user32.EmptyClipboard.argtypes = []
user32.EmptyClipboard.restype = wt.BOOL
user32.GetClipboardData.argtypes = [wt.UINT]
user32.GetClipboardData.restype = wt.HANDLE
user32.SetClipboardData.argtypes = [wt.UINT, wt.HANDLE]
user32.SetClipboardData.restype = wt.HANDLE
kernel32.GlobalAlloc.argtypes = [wt.UINT, ctypes.c_size_t]
kernel32.GlobalAlloc.restype = wt.HANDLE
kernel32.GlobalLock.argtypes = [wt.HANDLE]
kernel32.GlobalLock.restype = wt.LPVOID
kernel32.GlobalUnlock.argtypes = [wt.HANDLE]
kernel32.GlobalUnlock.restype = wt.BOOL
kernel32.GlobalFree.argtypes = [wt.HANDLE]
kernel32.GlobalFree.restype = wt.HANDLE

# ── Constants ────────────────────────────────────────────────────

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_HWHEEL = 0x01000
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008
WHEEL_DELTA = 120

WH_MOUSE_LL = 14
WH_KEYBOARD_LL = 13

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MBUTTONDOWN = 0x0207
WM_MBUTTONUP = 0x0208
WM_MOUSEWHEEL = 0x020A
WM_MOUSEHWHEEL = 0x020E
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
WM_QUIT = 0x0012

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

CF_UNICODETEXT = 13

GMEM_MOVEABLE = 0x0002

# ChangeDisplaySettingsEx flags
CDS_UPDATEREGISTRY = 0x00000001
CDS_NORESET = 0x10000000

DM_POSITION = 0x00000020
DM_PELSWIDTH = 0x00080000
DM_PELSHEIGHT = 0x00100000

ENUM_CURRENT_SETTINGS = -1


# ── Structures ───────────────────────────────────────────────────

class POINT(ctypes.Structure):
    _fields_ = [("x", wt.LONG), ("y", wt.LONG)]


class RECT(ctypes.Structure):
    _fields_ = [("left", wt.LONG), ("top", wt.LONG),
                ("right", wt.LONG), ("bottom", wt.LONG)]


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", RECT),
                ("rcWork", RECT), ("dwFlags", wt.DWORD)]


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", RECT),
                ("rcWork", RECT), ("dwFlags", wt.DWORD),
                ("szDevice", wt.WCHAR * 32)]


class DEVMODEW(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", wt.WCHAR * 32),
        ("dmSpecVersion", wt.WORD),
        ("dmDriverVersion", wt.WORD),
        ("dmSize", wt.WORD),
        ("dmDriverExtra", wt.WORD),
        ("dmFields", wt.DWORD),
        ("dmPositionX", wt.LONG),
        ("dmPositionY", wt.LONG),
        ("dmDisplayOrientation", wt.DWORD),
        ("dmDisplayFixedOutput", wt.DWORD),
        ("dmColor", wt.SHORT),
        ("dmDuplex", wt.SHORT),
        ("dmYResolution", wt.SHORT),
        ("dmTTOption", wt.SHORT),
        ("dmCollate", wt.SHORT),
        ("dmFormName", wt.WCHAR * 32),
        ("dmLogPixels", wt.WORD),
        ("dmBitsPerPel", wt.DWORD),
        ("dmPelsWidth", wt.DWORD),
        ("dmPelsHeight", wt.DWORD),
        ("dmDisplayFlags", wt.DWORD),
        ("dmDisplayFrequency", wt.DWORD),
        ("dmICMMethod", wt.DWORD),
        ("dmICMIntent", wt.DWORD),
        ("dmMediaType", wt.DWORD),
        ("dmDitherType", wt.DWORD),
        ("dmReserved1", wt.DWORD),
        ("dmReserved2", wt.DWORD),
        ("dmPanningWidth", wt.DWORD),
        ("dmPanningHeight", wt.DWORD),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG),
                ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class _INPUT_UNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("_input", _INPUT_UNION)]


# Signatures for the display-config APIs — pointer args must be sized correctly
# on 64-bit Windows, which the ctypes default (c_int) gets wrong.
user32.EnumDisplaySettingsExW.argtypes = [
    wt.LPCWSTR, wt.DWORD, ctypes.POINTER(DEVMODEW), wt.DWORD,
]
user32.EnumDisplaySettingsExW.restype = wt.BOOL
user32.ChangeDisplaySettingsExW.argtypes = [
    wt.LPCWSTR, ctypes.POINTER(DEVMODEW), wt.HWND, wt.DWORD, ctypes.c_void_p,
]
user32.ChangeDisplaySettingsExW.restype = ctypes.c_long


class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("pt", POINT), ("mouseData", wt.DWORD),
                ("flags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wt.DWORD), ("scanCode", wt.DWORD),
                ("flags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


HOOKPROC = ctypes.WINFUNCTYPE(
    wt.LPARAM, ctypes.c_int, wt.WPARAM, wt.LPARAM,
)
MONITORENUMPROC = ctypes.WINFUNCTYPE(
    wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(RECT), wt.LPARAM,
)


# ── Backend implementation ───────────────────────────────────────

class WindowsBackend(InputBackend):
    """Win32 API backend for Windows."""

    def __init__(self):
        self._grabbed = False
        self._hook_thread: Optional[threading.Thread] = None
        self._hook_thread_id: int = 0
        self._hooks_running = False
        self._mouse_hook = None
        self._kb_hook = None
        self._on_key: Optional[Callable] = None
        # Keep references to prevent GC of callback pointers
        self._mouse_proc = None
        self._kb_proc = None

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self):
        pass  # Win32 APIs are always available

    def close(self):
        self.stop_key_capture()
        if self._grabbed:
            self.ungrab_input()

    # ── Monitors ─────────────────────────────────────────────────

    def query_monitors(self) -> list[MonitorRect]:
        monitors = []

        def callback(hMonitor, hdcMonitor, lprcMonitor, dwData):
            info = MONITORINFOEXW()
            info.cbSize = ctypes.sizeof(MONITORINFOEXW)
            user32.GetMonitorInfoW(hMonitor, ctypes.byref(info))
            rc = info.rcMonitor
            monitors.append(MonitorRect(
                id=info.szDevice,
                x=rc.left, y=rc.top,
                width=rc.right - rc.left,
                height=rc.bottom - rc.top,
            ))
            return True

        proc = MONITORENUMPROC(callback)
        user32.EnumDisplayMonitors(None, None, proc, 0)
        return monitors

    def set_monitor_positions(
        self, positions: dict[str, tuple[int, int]],
    ) -> bool:
        """Reconfigure Windows display arrangement via ChangeDisplaySettingsEx."""
        norm = self._normalize_positions(positions)
        if norm is None:
            return False

        for device_name, (x, y) in norm.items():
            dm = DEVMODEW()
            dm.dmSize = ctypes.sizeof(DEVMODEW)
            # Seed with current settings so we don't overwrite width/height/refresh.
            if not user32.EnumDisplaySettingsExW(
                device_name, ENUM_CURRENT_SETTINGS, ctypes.byref(dm), 0,
            ):
                log.warning("EnumDisplaySettingsExW failed for %s", device_name)
                return False
            dm.dmFields = DM_POSITION
            dm.dmPositionX = x
            dm.dmPositionY = y
            rc = user32.ChangeDisplaySettingsExW(
                device_name, ctypes.byref(dm), None,
                CDS_UPDATEREGISTRY | CDS_NORESET, None,
            )
            if rc != 0:
                log.warning("ChangeDisplaySettingsExW(%s) returned %d",
                            device_name, rc)
                return False

        rc = user32.ChangeDisplaySettingsExW(None, None, None, 0, None)
        if rc != 0:
            log.warning("ChangeDisplaySettingsExW(commit) returned %d", rc)
            return False
        log.info("Applied OS monitor layout via ChangeDisplaySettingsEx.")
        return True

    @property
    def screen_width(self) -> int:
        return user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)

    @property
    def screen_height(self) -> int:
        return user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)

    # ── Cursor ───────────────────────────────────────────────────

    def get_cursor_pos(self) -> tuple[int, int]:
        pt = POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        return pt.x, pt.y

    def set_cursor_pos(self, x: int, y: int):
        user32.SetCursorPos(x, y)

    def get_button_mask(self) -> int:
        mask = 0
        if user32.GetAsyncKeyState(0x01) & 0x8000:  # VK_LBUTTON
            mask |= 1
        if user32.GetAsyncKeyState(0x04) & 0x8000:  # VK_MBUTTON
            mask |= 2
        if user32.GetAsyncKeyState(0x02) & 0x8000:  # VK_RBUTTON
            mask |= 4
        return mask

    # ── Grab ─────────────────────────────────────────────────────

    def grab_input(self) -> bool:
        # Hide the local cursor so the user doesn't see a ghost pointer
        # on this machine while the real cursor is on another. No
        # ClipCursor — that would also disable any physical mouse
        # attached to this PC.
        while user32.ShowCursor(False) >= 0:
            pass
        self._grabbed = True
        return True

    def ungrab_input(self):
        if self._grabbed:
            while user32.ShowCursor(True) < 0:
                pass
        self._grabbed = False

    @property
    def is_grabbed(self) -> bool:
        return self._grabbed

    # ── Injection ────────────────────────────────────────────────

    def _send_input(self, *inputs: INPUT):
        arr = (INPUT * len(inputs))(*inputs)
        user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))

    def inject_mouse_move(self, x: int, y: int):
        vx = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
        vy = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
        sw = self.screen_width
        sh = self.screen_height
        inp = INPUT()
        inp.type = INPUT_MOUSE
        inp._input.mi.dx = int((x - vx) * 65535 / (sw - 1))
        inp._input.mi.dy = int((y - vy) * 65535 / (sh - 1))
        inp._input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
        self._send_input(inp)

    def inject_mouse_move_rel(self, dx: int, dy: int):
        inp = INPUT()
        inp.type = INPUT_MOUSE
        inp._input.mi.dx = dx
        inp._input.mi.dy = dy
        inp._input.mi.dwFlags = MOUSEEVENTF_MOVE
        self._send_input(inp)

    def inject_mouse_button(self, button: int, pressed: bool):
        inp = INPUT()
        inp.type = INPUT_MOUSE
        flag_map = {
            (1, True): MOUSEEVENTF_LEFTDOWN,
            (1, False): MOUSEEVENTF_LEFTUP,
            (2, True): MOUSEEVENTF_MIDDLEDOWN,
            (2, False): MOUSEEVENTF_MIDDLEUP,
            (3, True): MOUSEEVENTF_RIGHTDOWN,
            (3, False): MOUSEEVENTF_RIGHTUP,
        }
        flag = flag_map.get((button, pressed))
        if flag is None:
            return
        inp._input.mi.dwFlags = flag
        self._send_input(inp)

    def inject_mouse_scroll(self, dx: int = 0, dy: int = 0):
        if dy:
            inp = INPUT()
            inp.type = INPUT_MOUSE
            inp._input.mi.dwFlags = MOUSEEVENTF_WHEEL
            inp._input.mi.mouseData = wt.DWORD(dy * WHEEL_DELTA)
            self._send_input(inp)
        if dx:
            inp = INPUT()
            inp.type = INPUT_MOUSE
            inp._input.mi.dwFlags = MOUSEEVENTF_HWHEEL
            inp._input.mi.mouseData = wt.DWORD(dx * WHEEL_DELTA)
            self._send_input(inp)

    def inject_key(self, scancode: int, pressed: bool):
        vk = hid_to_vk(scancode)
        if not vk:
            return
        inp = INPUT()
        inp.type = INPUT_KEYBOARD
        inp._input.ki.wVk = vk
        if not pressed:
            inp._input.ki.dwFlags = KEYEVENTF_KEYUP
        self._send_input(inp)

    # ── Keyboard capture (low-level hooks) ───────────────────────

    def start_key_capture(self, on_key: Callable[[int, bool], None]) -> bool:
        if self._hooks_running:
            return True
        self._on_key = on_key
        self._hooks_running = True
        self._hook_thread = threading.Thread(
            target=self._hook_loop, daemon=True, name="win-hooks",
        )
        self._hook_thread.start()
        return True

    def stop_key_capture(self):
        if not self._hooks_running:
            return
        self._hooks_running = False
        # Post WM_QUIT to the hook thread to break its message loop
        if self._hook_thread_id:
            ctypes.windll.user32.PostThreadMessageW(
                self._hook_thread_id, WM_QUIT, 0, 0,
            )
        self._on_key = None

    def _hook_loop(self):
        """Run in a dedicated thread: install hooks + message pump."""
        self._hook_thread_id = kernel32.GetCurrentThreadId()

        def kb_proc(nCode, wParam, lParam):
            if nCode >= 0 and self._on_key:
                kb = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT))
                vk = kb.contents.vkCode
                pressed = wParam in (WM_KEYDOWN, WM_SYSKEYDOWN)
                hid = vk_to_hid(vk)
                if hid:
                    self._on_key(hid, pressed)
                    # Non-zero return suppresses the keystroke from reaching
                    # the rest of the system, so the remote injection is the
                    # only place it lands.
                    return 1
            return user32.CallNextHookEx(None, nCode, wParam, lParam)

        self._kb_proc = HOOKPROC(kb_proc)
        self._kb_hook = user32.SetWindowsHookExW(
            WH_KEYBOARD_LL, self._kb_proc,
            kernel32.GetModuleHandleW(None), 0,
        )

        # Message pump — required for low-level hooks to work
        msg = wt.MSG()
        while self._hooks_running:
            ret = user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
            if ret <= 0:
                break
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

        # Cleanup hooks
        if self._kb_hook:
            user32.UnhookWindowsHookEx(self._kb_hook)
            self._kb_hook = None

    # ── Clipboard ────────────────────────────────────────────────

    def get_clipboard(self) -> str:
        if not user32.OpenClipboard(None):
            return ""
        try:
            handle = user32.GetClipboardData(CF_UNICODETEXT)
            if not handle:
                return ""
            ptr = kernel32.GlobalLock(handle)
            if not ptr:
                return ""
            try:
                return ctypes.wstring_at(ptr)
            finally:
                kernel32.GlobalUnlock(handle)
        finally:
            user32.CloseClipboard()

    def set_clipboard(self, text: str):
        if not user32.OpenClipboard(None):
            return
        try:
            user32.EmptyClipboard()
            encoded = text.encode("utf-16-le") + b"\x00\x00"
            handle = kernel32.GlobalAlloc(
                GMEM_MOVEABLE, len(encoded),
            )
            if not handle:
                return
            ptr = kernel32.GlobalLock(handle)
            if not ptr:
                kernel32.GlobalFree(handle)
                return
            ctypes.memmove(ptr, encoded, len(encoded))
            kernel32.GlobalUnlock(handle)
            user32.SetClipboardData(CF_UNICODETEXT, handle)
        finally:
            user32.CloseClipboard()

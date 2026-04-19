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

# Cursor-hiding APIs need explicit signatures: SetSystemCursor takes
# an HCURSOR (HANDLE) and an OCR_* DWORD, CopyIcon returns an HICON.
# Without these, 64-bit Windows truncates the handles and the calls
# silently succeed but produce garbage.
user32.CreateCursor.argtypes = [
    wt.HINSTANCE, ctypes.c_int, ctypes.c_int,
    ctypes.c_int, ctypes.c_int,
    ctypes.c_void_p, ctypes.c_void_p,
]
user32.CreateCursor.restype = wt.HANDLE
user32.CopyIcon.argtypes = [wt.HANDLE]
user32.CopyIcon.restype = wt.HANDLE
user32.DestroyCursor.argtypes = [wt.HANDLE]
user32.DestroyCursor.restype = wt.BOOL
user32.SetSystemCursor.argtypes = [wt.HANDLE, wt.DWORD]
user32.SetSystemCursor.restype = wt.BOOL
user32.SystemParametersInfoW.argtypes = [
    wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT,
]
user32.SystemParametersInfoW.restype = wt.BOOL

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

# Flags set by the kernel on injected events — inside our low-level
# hooks we must let these through untouched so the source PC's
# SendInput reaches local apps instead of being blocked by our own
# capture.
LLKHF_INJECTED = 0x10
LLMHF_INJECTED = 0x01

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

CF_UNICODETEXT = 13

GMEM_MOVEABLE = 0x0002

# SystemParametersInfo action to reload the default system cursor
# scheme — used to restore cursors after SetSystemCursor-based hiding.
SPI_SETCURSORS = 0x0057

# Every OCR_* system cursor id. SetSystemCursor takes one id per call,
# so the only way to fully hide the pointer is to replace every scheme
# entry with a blank cursor.
_OCR_IDS = (
    32512,  # OCR_NORMAL
    32513,  # OCR_IBEAM
    32514,  # OCR_WAIT
    32515,  # OCR_CROSS
    32516,  # OCR_UP
    32631,  # OCR_SIZE (legacy)
    32642,  # OCR_SIZENWSE
    32643,  # OCR_SIZENESW
    32644,  # OCR_SIZEWE
    32645,  # OCR_SIZENS
    32646,  # OCR_SIZEALL
    32648,  # OCR_NO
    32649,  # OCR_HAND
    32650,  # OCR_APPSTARTING
    32651,  # OCR_HELP
)

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

# Hook API signatures. Without these, on 64-bit Windows the HHOOK
# returned by SetWindowsHookExW gets truncated to 32 bits (same class
# of bug as the earlier clipboard/cursor issues), and CallNextHookEx
# returns a truncated value that Windows then treats as "block" when
# it shouldn't — the mouse-block hook appeared to do nothing because
# its pass-through path was randomly blocking or doing nothing.
user32.SetWindowsHookExW.argtypes = [
    ctypes.c_int, HOOKPROC, wt.HMODULE, wt.DWORD,
]
user32.SetWindowsHookExW.restype = wt.HANDLE
user32.UnhookWindowsHookEx.argtypes = [wt.HANDLE]
user32.UnhookWindowsHookEx.restype = wt.BOOL
user32.CallNextHookEx.argtypes = [
    wt.HANDLE, ctypes.c_int, wt.WPARAM, wt.LPARAM,
]
user32.CallNextHookEx.restype = wt.LPARAM
kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
kernel32.GetModuleHandleW.restype = wt.HMODULE
user32.PostThreadMessageW.argtypes = [
    wt.DWORD, wt.UINT, wt.WPARAM, wt.LPARAM,
]
user32.PostThreadMessageW.restype = wt.BOOL
user32.GetMessageW.argtypes = [
    ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT,
]
user32.GetMessageW.restype = wt.BOOL
user32.TranslateMessage.argtypes = [ctypes.POINTER(wt.MSG)]
user32.TranslateMessage.restype = wt.BOOL
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
user32.DispatchMessageW.restype = wt.LPARAM


# ── Backend implementation ───────────────────────────────────────

class WindowsBackend(InputBackend):
    """Win32 API backend for Windows."""

    def __init__(self):
        self._grabbed = False
        self._cursor_hidden = False
        self._hook_thread: Optional[threading.Thread] = None
        self._hook_thread_id: int = 0
        self._hooks_running = False
        self._mouse_hook = None
        self._kb_hook = None
        self._on_key: Optional[Callable] = None
        # True while the WH_MOUSE_LL hook should return 1 (block) for
        # every incoming mouse event. Set by start_pointer_block when
        # the PC is dormant and not the input source.
        self._mouse_block = False
        # Keep references to prevent GC of callback pointers
        self._mouse_proc = None
        self._kb_proc = None
        # Scroll-wheel events are dispatched through this callback
        # while the low-level mouse hook is active. Same shape as
        # _on_key: the backend surfaces what it sees, peer decides
        # whether to forward based on workspace + mode.
        self._on_scroll: Optional[Callable[[int, int], None]] = None

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self):
        pass  # Win32 APIs are always available

    def close(self):
        self.stop_key_capture()
        self.stop_pointer_block()
        if self._grabbed:
            self.ungrab_input()
        if self._cursor_hidden:
            self.show_cursor()

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
        # Cursor visibility is handled separately via hide_cursor —
        # grab just records the intent so ungrab_input has something
        # to clear. No ClipCursor; that would jail a physical mouse
        # attached to this PC.
        self._grabbed = True
        return True

    def ungrab_input(self):
        self._grabbed = False

    def hide_cursor(self) -> None:
        # ShowCursor(FALSE) only hides the cursor over windows owned by
        # the calling thread — the cursor still shows over every other
        # app on the desktop. SetSystemCursor replaces each cursor in
        # the user's scheme with a fully transparent one, which is how
        # Synergy / Barrier achieve a truly hidden pointer. Paired with
        # ShowCursor(FALSE) as a belt-and-suspenders.
        if self._cursor_hidden:
            return
        and_plane = b"\xff" * 128  # 32x32 mask: all 1s = transparent
        xor_plane = b"\x00" * 128  # 32x32 color: all 0s
        blank = user32.CreateCursor(None, 0, 0, 32, 32,
                                    and_plane, xor_plane)
        if not blank:
            return
        for ocr_id in _OCR_IDS:
            copy = user32.CopyIcon(blank)
            if copy:
                user32.SetSystemCursor(copy, ocr_id)
        user32.DestroyCursor(blank)
        while user32.ShowCursor(False) >= 0:
            pass
        self._cursor_hidden = True

    def show_cursor(self) -> None:
        if not self._cursor_hidden:
            return
        # SPI_SETCURSORS reloads the default cursor scheme from the
        # user's registry, undoing every SetSystemCursor we made.
        user32.SystemParametersInfoW(SPI_SETCURSORS, 0, None, 0)
        while user32.ShowCursor(True) < 0:
            pass
        self._cursor_hidden = False

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
        # SendInput(MOUSEEVENTF_MOVE) goes through Windows pointer
        # acceleration ("Enhance pointer precision"), which scales small
        # deltas down to near-zero pixels — the cursor feels stuck even
        # though our source PC is clearly moving. SetCursorPos(current+d)
        # moves the cursor by exactly dx,dy pixels so remote input is 1:1.
        pt = POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        user32.SetCursorPos(pt.x + dx, pt.y + dy)

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

    def _ensure_hook_thread(self) -> None:
        if self._hooks_running:
            return
        self._hooks_running = True
        self._hook_thread = threading.Thread(
            target=self._hook_loop, daemon=True, name="win-hooks",
        )
        self._hook_thread.start()

    def _shutdown_hook_thread(self) -> None:
        if not self._hooks_running:
            return
        # Only tear the thread down when nothing needs it. Keyboard
        # capture and pointer block both ride the same message pump —
        # one can stop without yanking the hook out from under the
        # other.
        if self._on_key is not None or self._mouse_block:
            return
        self._hooks_running = False
        if self._hook_thread_id:
            ctypes.windll.user32.PostThreadMessageW(
                self._hook_thread_id, WM_QUIT, 0, 0,
            )

    def start_key_capture(self, on_key: Callable[[int, bool], None]) -> bool:
        self._on_key = on_key
        self._ensure_hook_thread()
        return True

    def stop_key_capture(self):
        self._on_key = None
        self._shutdown_hook_thread()

    def start_scroll_capture(
            self, on_scroll: Callable[[int, int], None]) -> bool:
        """Route WM_MOUSEWHEEL / WM_MOUSEHWHEEL events to the caller.
        Piggybacks on the same low-level mouse hook the pointer-block
        already uses — on_scroll(dx, dy) fires with ±1-per-notch
        clicks (dy positive = scroll up, dx positive = scroll right)."""
        self._on_scroll = on_scroll
        self._ensure_hook_thread()
        return True

    def stop_scroll_capture(self) -> None:
        self._on_scroll = None
        self._shutdown_hook_thread()

    def start_pointer_block(self) -> None:
        if self._mouse_block:
            return
        self._mouse_block = True
        self._ensure_hook_thread()
        log.info("Pointer block engaged (WH_MOUSE_LL returning 1)")

    def stop_pointer_block(self) -> None:
        if not self._mouse_block:
            return
        self._mouse_block = False
        log.info("Pointer block released")
        self._shutdown_hook_thread()

    def _hook_loop(self):
        """Run in a dedicated thread: install hooks + message pump."""
        self._hook_thread_id = kernel32.GetCurrentThreadId()

        def kb_proc(nCode, wParam, lParam):
            if nCode >= 0:
                kb = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT))
                injected = bool(kb.contents.flags & LLKHF_INJECTED)
                if not injected and self._on_key:
                    vk = kb.contents.vkCode
                    pressed = wParam in (WM_KEYDOWN, WM_SYSKEYDOWN)
                    hid = vk_to_hid(vk)
                    if hid:
                        self._on_key(hid, pressed)
                        # Non-zero return suppresses real local typing
                        # so only the remote injection path lands.
                        return 1
                # Injected events (our own SendInput from the source
                # PC's forwarded keys) must pass through unchanged.
            return user32.CallNextHookEx(None, nCode, wParam, lParam)

        def mouse_proc(nCode, wParam, lParam):
            if nCode >= 0:
                mi = ctypes.cast(lParam, ctypes.POINTER(MSLLHOOKSTRUCT))
                injected = bool(mi.contents.flags & LLMHF_INJECTED)
                if (not injected and self._on_scroll and wParam
                        in (WM_MOUSEWHEEL, WM_MOUSEHWHEEL)):
                    # mouseData high word is a signed short with the
                    # wheel delta; positive = forward (away from user)
                    # = scroll up.
                    raw = mi.contents.mouseData
                    delta = (raw >> 16) & 0xFFFF
                    if delta & 0x8000:
                        delta -= 0x10000
                    # Collapse the 120-unit delta to "clicks" (±1 per
                    # notch) so remote OSes that expect a unit-count
                    # scroll don't receive wildly over-scaled values.
                    clicks = delta // WHEEL_DELTA
                    if clicks:
                        try:
                            if wParam == WM_MOUSEHWHEEL:
                                self._on_scroll(clicks, 0)
                            else:
                                self._on_scroll(0, clicks)
                        except Exception:
                            log.exception("on_scroll callback failed")
                    # Scroll capture is only enabled while we're in
                    # FORWARDING mode — the whole point is that the
                    # remote PC is the target. Always swallow the
                    # wheel event locally so it doesn't ALSO scroll
                    # apps on the forwarding PC. Previous gate on
                    # _mouse_block never fired because the capture
                    # backend never has pointer-block engaged.
                    return 1

                if self._mouse_block and not injected:
                    # Every other real mouse input on this PC — swallow.
                    return 1
                # Injected events (remote clicks from the source PC)
                # pass through so SendInput actually reaches apps.
            return user32.CallNextHookEx(None, nCode, wParam, lParam)

        self._kb_proc = HOOKPROC(kb_proc)
        self._mouse_proc = HOOKPROC(mouse_proc)
        # MSDN: for WH_KEYBOARD_LL and WH_MOUSE_LL, hMod may be NULL.
        # Avoids relying on GetModuleHandleW's truncation behaviour
        # and any module-path resolution quirks in the bundle.
        self._kb_hook = user32.SetWindowsHookExW(
            WH_KEYBOARD_LL, self._kb_proc, None, 0,
        )
        self._mouse_hook = user32.SetWindowsHookExW(
            WH_MOUSE_LL, self._mouse_proc, None, 0,
        )
        log.info("Win32 hooks installed: WH_KEYBOARD_LL=%s WH_MOUSE_LL=%s",
                 bool(self._kb_hook), bool(self._mouse_hook))

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
        if self._mouse_hook:
            user32.UnhookWindowsHookEx(self._mouse_hook)
            self._mouse_hook = None

    # ── Clipboard ────────────────────────────────────────────────

    def _open_clipboard(self, attempts: int = 10) -> bool:
        """OpenClipboard can fail if another process holds it — retry briefly."""
        import time as _time
        for i in range(attempts):
            if user32.OpenClipboard(None):
                return True
            _time.sleep(0.02)
        log.warning("OpenClipboard failed after %d attempts", attempts)
        return False

    def get_clipboard(self) -> str:
        if not self._open_clipboard():
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
        if not self._open_clipboard():
            return
        try:
            user32.EmptyClipboard()
            encoded = text.encode("utf-16-le") + b"\x00\x00"
            handle = kernel32.GlobalAlloc(
                GMEM_MOVEABLE, len(encoded),
            )
            if not handle:
                log.warning("GlobalAlloc for clipboard failed")
                return
            ptr = kernel32.GlobalLock(handle)
            if not ptr:
                log.warning("GlobalLock for clipboard failed")
                kernel32.GlobalFree(handle)
                return
            ctypes.memmove(ptr, encoded, len(encoded))
            kernel32.GlobalUnlock(handle)
            if not user32.SetClipboardData(CF_UNICODETEXT, handle):
                log.warning("SetClipboardData failed")
        finally:
            user32.CloseClipboard()

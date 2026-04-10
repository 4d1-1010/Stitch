"""
Low-level X11 bindings via ctypes.

Provides only the functions needed for input capture, injection,
cursor control, and monitor detection. No external Python dependencies.
"""

import ctypes
import ctypes.util
import logging
from dataclasses import dataclass
from typing import Optional

log = logging.getLogger(__name__)

# ── Load shared libraries ────────────────────────────────────────────

def _load(name: str):
    path = ctypes.util.find_library(name)
    if path is None:
        raise OSError(f"Cannot find lib{name}. Install it via your package manager.")
    return ctypes.cdll.LoadLibrary(path)


_x11 = _load("X11")
_xtst = _load("Xtst")

try:
    _xinerama = _load("Xinerama")
    HAS_XINERAMA = True
except OSError:
    _xinerama = None
    HAS_XINERAMA = False
    log.warning("Xinerama not available; will detect single screen only.")


# ── X11 type aliases ─────────────────────────────────────────────────

Display_p = ctypes.c_void_p      # Display*
Window = ctypes.c_ulong
Bool = ctypes.c_int
Atom = ctypes.c_ulong
Time = ctypes.c_ulong
KeyCode = ctypes.c_ubyte
Status = ctypes.c_int

CurrentTime = 0
GrabModeAsync = 1
GrabSuccess = 0
None_ = 0
KeyPressMask = (1 << 0)
KeyReleaseMask = (1 << 1)
ButtonPressMask = (1 << 2)
ButtonReleaseMask = (1 << 3)
PointerMotionMask = (1 << 6)
AllEventsMask = (KeyPressMask | KeyReleaseMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask)


# ── Xinerama structures ─────────────────────────────────────────────

class XineramaScreenInfo(ctypes.Structure):
    _fields_ = [
        ("screen_number", ctypes.c_int),
        ("x_org", ctypes.c_short),
        ("y_org", ctypes.c_short),
        ("width", ctypes.c_short),
        ("height", ctypes.c_short),
    ]


# ── Function signatures ─────────────────────────────────────────────

# Display management
_x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
_x11.XOpenDisplay.restype = Display_p

_x11.XCloseDisplay.argtypes = [Display_p]
_x11.XCloseDisplay.restype = ctypes.c_int

_x11.XDefaultRootWindow.argtypes = [Display_p]
_x11.XDefaultRootWindow.restype = Window

_x11.XDisplayWidth.argtypes = [Display_p, ctypes.c_int]
_x11.XDisplayWidth.restype = ctypes.c_int

_x11.XDisplayHeight.argtypes = [Display_p, ctypes.c_int]
_x11.XDisplayHeight.restype = ctypes.c_int

_x11.XDefaultScreen.argtypes = [Display_p]
_x11.XDefaultScreen.restype = ctypes.c_int

_x11.XFlush.argtypes = [Display_p]
_x11.XFlush.restype = ctypes.c_int

_x11.XSync.argtypes = [Display_p, Bool]
_x11.XSync.restype = ctypes.c_int

# Pointer
_x11.XQueryPointer.argtypes = [
    Display_p, Window,
    ctypes.POINTER(Window),   # root_return
    ctypes.POINTER(Window),   # child_return
    ctypes.POINTER(ctypes.c_int),  # root_x
    ctypes.POINTER(ctypes.c_int),  # root_y
    ctypes.POINTER(ctypes.c_int),  # win_x
    ctypes.POINTER(ctypes.c_int),  # win_y
    ctypes.POINTER(ctypes.c_uint), # mask
]
_x11.XQueryPointer.restype = Bool

_x11.XWarpPointer.argtypes = [
    Display_p, Window, Window,
    ctypes.c_int, ctypes.c_int,
    ctypes.c_uint, ctypes.c_uint,
    ctypes.c_int, ctypes.c_int,
]
_x11.XWarpPointer.restype = ctypes.c_int

_x11.XGrabPointer.argtypes = [
    Display_p, Window, Bool,
    ctypes.c_uint,     # event_mask
    ctypes.c_int,      # pointer_mode
    ctypes.c_int,      # keyboard_mode
    Window,            # confine_to
    ctypes.c_ulong,   # cursor
    Time,
]
_x11.XGrabPointer.restype = ctypes.c_int

_x11.XUngrabPointer.argtypes = [Display_p, Time]
_x11.XUngrabPointer.restype = ctypes.c_int

_x11.XGrabKeyboard.argtypes = [
    Display_p, Window, Bool,
    ctypes.c_int, ctypes.c_int, Time,
]
_x11.XGrabKeyboard.restype = ctypes.c_int

_x11.XUngrabKeyboard.argtypes = [Display_p, Time]
_x11.XUngrabKeyboard.restype = ctypes.c_int

# XTest
_xtst.XTestFakeMotionEvent.argtypes = [
    Display_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, Time,
]
_xtst.XTestFakeMotionEvent.restype = Status

_xtst.XTestFakeButtonEvent.argtypes = [
    Display_p, ctypes.c_uint, Bool, Time,
]
_xtst.XTestFakeButtonEvent.restype = Status

_xtst.XTestFakeKeyEvent.argtypes = [
    Display_p, ctypes.c_uint, Bool, Time,
]
_xtst.XTestFakeKeyEvent.restype = Status

_xtst.XTestFakeRelativeMotionEvent.argtypes = [
    Display_p, ctypes.c_int, ctypes.c_int, Time,
]
_xtst.XTestFakeRelativeMotionEvent.restype = Status

# Xinerama
if HAS_XINERAMA:
    _xinerama.XineramaIsActive.argtypes = [Display_p]
    _xinerama.XineramaIsActive.restype = Bool

    _xinerama.XineramaQueryScreens.argtypes = [
        Display_p, ctypes.POINTER(ctypes.c_int),
    ]
    _xinerama.XineramaQueryScreens.restype = ctypes.POINTER(XineramaScreenInfo)


# ── High-level wrapper ───────────────────────────────────────────────

@dataclass
class ScreenRect:
    """A physical monitor rectangle in local X coordinates."""
    id: str
    x: int
    y: int
    width: int
    height: int


class X11Display:
    """Wraps an X11 display connection with cursor and input operations."""

    def __init__(self, display_name: Optional[str] = None):
        name = display_name.encode() if display_name else None
        self._dpy = _x11.XOpenDisplay(name)
        if not self._dpy:
            raise RuntimeError(f"Cannot open X display: {display_name or '$DISPLAY'}")
        self._screen = _x11.XDefaultScreen(self._dpy)
        self._root = _x11.XDefaultRootWindow(self._dpy)
        self._grabbed = False
        self._kb_grabbed = False

    def close(self):
        if self._dpy:
            if self._grabbed:
                self.ungrab_pointer()
            if self._kb_grabbed:
                self.ungrab_keyboard()
            _x11.XCloseDisplay(self._dpy)
            self._dpy = None

    def __del__(self):
        self.close()

    # ── Pointer queries ──────────────────────────────────────────

    def query_pointer(self) -> tuple[int, int, int]:
        """Return (root_x, root_y, button_mask)."""
        root_ret = Window()
        child_ret = Window()
        rx, ry = ctypes.c_int(), ctypes.c_int()
        wx, wy = ctypes.c_int(), ctypes.c_int()
        mask = ctypes.c_uint()
        _x11.XQueryPointer(
            self._dpy, self._root,
            ctypes.byref(root_ret), ctypes.byref(child_ret),
            ctypes.byref(rx), ctypes.byref(ry),
            ctypes.byref(wx), ctypes.byref(wy),
            ctypes.byref(mask),
        )
        return rx.value, ry.value, mask.value

    def warp_pointer(self, x: int, y: int):
        """Move the cursor to (x, y) in root window coordinates."""
        _x11.XWarpPointer(self._dpy, None_, self._root, 0, 0, 0, 0, x, y)
        _x11.XFlush(self._dpy)

    # ── Grabs ────────────────────────────────────────────────────

    def grab_pointer(self) -> bool:
        """Grab the pointer. Returns True on success."""
        mask = (PointerMotionMask | ButtonPressMask | ButtonReleaseMask)
        result = _x11.XGrabPointer(
            self._dpy, self._root, Bool(True),
            mask, GrabModeAsync, GrabModeAsync,
            None_,   # don't confine
            None_,   # default cursor
            CurrentTime,
        )
        self._grabbed = (result == GrabSuccess)
        _x11.XFlush(self._dpy)
        return self._grabbed

    def ungrab_pointer(self):
        _x11.XUngrabPointer(self._dpy, CurrentTime)
        self._grabbed = False
        _x11.XFlush(self._dpy)

    def grab_keyboard(self) -> bool:
        result = _x11.XGrabKeyboard(
            self._dpy, self._root, Bool(True),
            GrabModeAsync, GrabModeAsync, CurrentTime,
        )
        self._kb_grabbed = (result == GrabSuccess)
        _x11.XFlush(self._dpy)
        return self._kb_grabbed

    def ungrab_keyboard(self):
        _x11.XUngrabKeyboard(self._dpy, CurrentTime)
        self._kb_grabbed = False
        _x11.XFlush(self._dpy)

    # ── XTest injection ──────────────────────────────────────────

    def fake_motion(self, x: int, y: int):
        """Inject an absolute pointer motion event."""
        _xtst.XTestFakeMotionEvent(self._dpy, self._screen, x, y, CurrentTime)
        _x11.XFlush(self._dpy)

    def fake_relative_motion(self, dx: int, dy: int):
        """Inject a relative pointer motion event."""
        _xtst.XTestFakeRelativeMotionEvent(self._dpy, dx, dy, CurrentTime)
        _x11.XFlush(self._dpy)

    def fake_button(self, button: int, pressed: bool):
        """Inject a mouse button press/release."""
        _xtst.XTestFakeButtonEvent(self._dpy, button, Bool(pressed), CurrentTime)
        _x11.XFlush(self._dpy)

    def fake_key(self, keycode: int, pressed: bool):
        """Inject a key press/release."""
        _xtst.XTestFakeKeyEvent(self._dpy, keycode, Bool(pressed), CurrentTime)
        _x11.XFlush(self._dpy)

    # ── Monitor detection ────────────────────────────────────────

    def query_monitors(self) -> list[ScreenRect]:
        """Detect physical monitors via Xinerama, fallback to single screen."""
        monitors = []

        if HAS_XINERAMA and _xinerama.XineramaIsActive(self._dpy):
            count = ctypes.c_int(0)
            info_p = _xinerama.XineramaQueryScreens(
                self._dpy, ctypes.byref(count),
            )
            for i in range(count.value):
                s = info_p[i]
                monitors.append(ScreenRect(
                    id=f"screen-{s.screen_number}",
                    x=s.x_org, y=s.y_org,
                    width=s.width, height=s.height,
                ))
            # XFree
            _x11.XFree(info_p)
        else:
            w = _x11.XDisplayWidth(self._dpy, self._screen)
            h = _x11.XDisplayHeight(self._dpy, self._screen)
            monitors.append(ScreenRect(
                id="screen-0", x=0, y=0, width=w, height=h,
            ))

        return monitors

    # ── Screen dimensions ────────────────────────────────────────

    @property
    def screen_width(self) -> int:
        return _x11.XDisplayWidth(self._dpy, self._screen)

    @property
    def screen_height(self) -> int:
        return _x11.XDisplayHeight(self._dpy, self._screen)

    def flush(self):
        _x11.XFlush(self._dpy)

    def sync(self):
        _x11.XSync(self._dpy, Bool(False))


# Set XFree signature (used for freeing Xinerama results)
_x11.XFree.argtypes = [ctypes.c_void_p]
_x11.XFree.restype = ctypes.c_int

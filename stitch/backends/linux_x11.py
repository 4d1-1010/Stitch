"""
Linux/X11 input backend.

Uses:
  - libX11 + libXtst (XTest) for cursor control and input injection
  - libXinerama for multi-monitor detection
  - /dev/input (evdev) for keyboard capture
  - xclip/xsel for clipboard
"""

import ctypes
import ctypes.util
import fcntl
import logging
import os
import re
import select
import shutil
import struct
import subprocess
import threading
from pathlib import Path
from typing import Callable, Optional

# _IOW('E', 0x90, int) — exclusively grab an evdev device so the OS
# doesn't also route its events to X.
EVIOCGRAB = 0x40044590

from . import InputBackend, MonitorRect
from .keycodes import evdev_to_hid, hid_to_x11

log = logging.getLogger(__name__)

_XRANDR_OUTPUT_RE = re.compile(
    r"^(?P<name>[\w\-]+) connected (?:primary )?"
    r"(?P<w>\d+)x(?P<h>\d+)\+(?P<x>-?\d+)\+(?P<y>-?\d+)",
)


def _parse_xrandr_outputs() -> list[tuple[str, int, int, int, int]]:
    """Return [(output_name, x, y, width, height), ...] for connected outputs.

    Returns [] if xrandr is missing or no outputs are connected.
    """
    if shutil.which("xrandr") is None:
        return []
    try:
        out = subprocess.run(
            ["xrandr", "--query"], capture_output=True, text=True, timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    if out.returncode != 0:
        return []

    results = []
    for line in out.stdout.splitlines():
        m = _XRANDR_OUTPUT_RE.match(line)
        if m:
            results.append((
                m.group("name"),
                int(m.group("x")), int(m.group("y")),
                int(m.group("w")), int(m.group("h")),
            ))
    return results


# ── Load shared libraries ────────────────────────────────────────

def _load(name: str):
    path = ctypes.util.find_library(name)
    if path is None:
        raise OSError(f"Cannot find lib{name}. Install it via your package manager.")
    return ctypes.cdll.LoadLibrary(path)


# ── X11 types ────────────────────────────────────────────────────

Display_p = ctypes.c_void_p
Window = ctypes.c_ulong
Bool = ctypes.c_int
Atom = ctypes.c_ulong
Time = ctypes.c_ulong
Status = ctypes.c_int

CurrentTime = 0
GrabModeAsync = 1
GrabSuccess = 0
None_ = 0
PointerMotionMask = (1 << 6)
ButtonPressMask = (1 << 2)
ButtonReleaseMask = (1 << 3)


class XineramaScreenInfo(ctypes.Structure):
    _fields_ = [
        ("screen_number", ctypes.c_int),
        ("x_org", ctypes.c_short),
        ("y_org", ctypes.c_short),
        ("width", ctypes.c_short),
        ("height", ctypes.c_short),
    ]


# ── evdev constants ──────────────────────────────────────────────

EVENT_FORMAT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)
EV_KEY = 0x01
KEY_RELEASE = 0
KEY_PRESS = 1
KEY_REPEAT = 2


# ── Backend implementation ───────────────────────────────────────

class LinuxX11Backend(InputBackend):
    """X11 + evdev + xclip backend for Linux."""

    def __init__(self):
        self._x11 = None
        self._xtst = None
        self._xinerama = None
        self._dpy: Optional[ctypes.c_void_p] = None
        self._screen: int = 0
        self._root: int = 0
        self._grabbed = False
        self._kb_grabbed = False

        # Keyboard capture (evdev)
        self._kbd_fds: list[int] = []
        self._kbd_thread: Optional[threading.Thread] = None
        self._kbd_running = False
        self._kbd_callback: Optional[Callable] = None

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self):
        self._x11 = _load("X11")
        self._xtst = _load("Xtst")
        try:
            self._xinerama = _load("Xinerama")
        except OSError:
            self._xinerama = None

        self._setup_signatures()

        self._dpy = self._x11.XOpenDisplay(None)
        if not self._dpy:
            raise RuntimeError("Cannot open X display. Is $DISPLAY set?")
        self._screen = self._x11.XDefaultScreen(self._dpy)
        self._root = self._x11.XDefaultRootWindow(self._dpy)

    def close(self):
        self.stop_key_capture()
        if self._dpy:
            if self._grabbed:
                self.ungrab_input()
            self._x11.XCloseDisplay(self._dpy)
            self._dpy = None

    def _setup_signatures(self):
        x = self._x11

        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XOpenDisplay.restype = Display_p
        x.XCloseDisplay.argtypes = [Display_p]
        x.XDefaultRootWindow.argtypes = [Display_p]
        x.XDefaultRootWindow.restype = Window
        x.XDisplayWidth.argtypes = [Display_p, ctypes.c_int]
        x.XDisplayWidth.restype = ctypes.c_int
        x.XDisplayHeight.argtypes = [Display_p, ctypes.c_int]
        x.XDisplayHeight.restype = ctypes.c_int
        x.XDefaultScreen.argtypes = [Display_p]
        x.XDefaultScreen.restype = ctypes.c_int
        x.XFlush.argtypes = [Display_p]
        x.XSync.argtypes = [Display_p, Bool]
        x.XFree.argtypes = [ctypes.c_void_p]

        x.XQueryPointer.argtypes = [
            Display_p, Window,
            ctypes.POINTER(Window), ctypes.POINTER(Window),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_uint),
        ]
        x.XQueryPointer.restype = Bool

        x.XWarpPointer.argtypes = [
            Display_p, Window, Window,
            ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
            ctypes.c_int, ctypes.c_int,
        ]

        x.XGrabPointer.argtypes = [
            Display_p, Window, Bool, ctypes.c_uint,
            ctypes.c_int, ctypes.c_int, Window, ctypes.c_ulong, Time,
        ]
        x.XGrabPointer.restype = ctypes.c_int
        x.XUngrabPointer.argtypes = [Display_p, Time]
        x.XGrabKeyboard.argtypes = [
            Display_p, Window, Bool, ctypes.c_int, ctypes.c_int, Time,
        ]
        x.XGrabKeyboard.restype = ctypes.c_int
        x.XUngrabKeyboard.argtypes = [Display_p, Time]

        t = self._xtst
        t.XTestFakeMotionEvent.argtypes = [
            Display_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, Time,
        ]
        t.XTestFakeButtonEvent.argtypes = [
            Display_p, ctypes.c_uint, Bool, Time,
        ]
        t.XTestFakeKeyEvent.argtypes = [
            Display_p, ctypes.c_uint, Bool, Time,
        ]
        t.XTestFakeRelativeMotionEvent.argtypes = [
            Display_p, ctypes.c_int, ctypes.c_int, Time,
        ]

        if self._xinerama:
            self._xinerama.XineramaIsActive.argtypes = [Display_p]
            self._xinerama.XineramaIsActive.restype = Bool
            self._xinerama.XineramaQueryScreens.argtypes = [
                Display_p, ctypes.POINTER(ctypes.c_int),
            ]
            self._xinerama.XineramaQueryScreens.restype = ctypes.POINTER(
                XineramaScreenInfo
            )

    # ── Monitors ─────────────────────────────────────────────────

    def query_monitors(self) -> list[MonitorRect]:
        # Prefer xrandr output names so OS reconfiguration can target them.
        xrandr = _parse_xrandr_outputs()
        if xrandr:
            return [
                MonitorRect(id=name, x=x, y=y, width=w, height=h)
                for name, x, y, w, h in xrandr
            ]
        monitors = []
        if (self._xinerama and
                self._xinerama.XineramaIsActive(self._dpy)):
            count = ctypes.c_int(0)
            info_p = self._xinerama.XineramaQueryScreens(
                self._dpy, ctypes.byref(count),
            )
            for i in range(count.value):
                s = info_p[i]
                monitors.append(MonitorRect(
                    id=f"screen-{s.screen_number}",
                    x=s.x_org, y=s.y_org,
                    width=s.width, height=s.height,
                ))
            self._x11.XFree(info_p)
        else:
            monitors.append(MonitorRect(
                id="screen-0", x=0, y=0,
                width=self.screen_width, height=self.screen_height,
            ))
        return monitors

    def set_monitor_positions(
        self, positions: dict[str, tuple[int, int]],
    ) -> bool:
        """Apply new positions to connected xrandr outputs.

        Keys must be xrandr output names (as returned by `query_monitors`).
        """
        norm = self._normalize_positions(positions)
        if norm is None:
            return False
        args = ["xrandr"]
        for name, (x, y) in norm.items():
            args += ["--output", name, "--pos", f"{x}x{y}"]
        try:
            result = subprocess.run(
                args, capture_output=True, text=True, timeout=10,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            log.warning("xrandr failed: %s", e)
            return False
        if result.returncode != 0:
            log.warning("xrandr returned %d: %s",
                        result.returncode, result.stderr.strip())
            return False
        log.info("Applied OS monitor layout via xrandr: %s", norm)
        return True

    @property
    def screen_width(self) -> int:
        return self._x11.XDisplayWidth(self._dpy, self._screen)

    @property
    def screen_height(self) -> int:
        return self._x11.XDisplayHeight(self._dpy, self._screen)

    # ── Cursor ───────────────────────────────────────────────────

    def get_cursor_pos(self) -> tuple[int, int]:
        root_ret, child_ret = Window(), Window()
        rx, ry = ctypes.c_int(), ctypes.c_int()
        wx, wy = ctypes.c_int(), ctypes.c_int()
        mask = ctypes.c_uint()
        self._x11.XQueryPointer(
            self._dpy, self._root,
            ctypes.byref(root_ret), ctypes.byref(child_ret),
            ctypes.byref(rx), ctypes.byref(ry),
            ctypes.byref(wx), ctypes.byref(wy),
            ctypes.byref(mask),
        )
        return rx.value, ry.value

    def set_cursor_pos(self, x: int, y: int):
        self._x11.XWarpPointer(self._dpy, None_, self._root,
                               0, 0, 0, 0, x, y)
        self._x11.XFlush(self._dpy)

    def get_button_mask(self) -> int:
        """Return normalized button mask: bit0=left, bit1=mid, bit2=right."""
        root_ret, child_ret = Window(), Window()
        rx, ry = ctypes.c_int(), ctypes.c_int()
        wx, wy = ctypes.c_int(), ctypes.c_int()
        mask = ctypes.c_uint()
        self._x11.XQueryPointer(
            self._dpy, self._root,
            ctypes.byref(root_ret), ctypes.byref(child_ret),
            ctypes.byref(rx), ctypes.byref(ry),
            ctypes.byref(wx), ctypes.byref(wy),
            ctypes.byref(mask),
        )
        raw = mask.value
        # X11: Button1Mask=1<<8, Button2Mask=1<<9, Button3Mask=1<<10
        result = 0
        if raw & (1 << 8):
            result |= 1  # left
        if raw & (1 << 9):
            result |= 2  # middle
        if raw & (1 << 10):
            result |= 4  # right
        return result

    # ── Grab ─────────────────────────────────────────────────────

    def grab_input(self) -> bool:
        mask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask
        r = self._x11.XGrabPointer(
            self._dpy, self._root, Bool(True),
            mask, GrabModeAsync, GrabModeAsync,
            None_, None_, CurrentTime,
        )
        self._grabbed = (r == GrabSuccess)
        r2 = self._x11.XGrabKeyboard(
            self._dpy, self._root, Bool(True),
            GrabModeAsync, GrabModeAsync, CurrentTime,
        )
        self._kb_grabbed = (r2 == GrabSuccess)
        self._x11.XFlush(self._dpy)
        return self._grabbed

    def ungrab_input(self):
        if self._grabbed:
            self._x11.XUngrabPointer(self._dpy, CurrentTime)
            self._grabbed = False
        if self._kb_grabbed:
            self._x11.XUngrabKeyboard(self._dpy, CurrentTime)
            self._kb_grabbed = False
        self._x11.XFlush(self._dpy)

    @property
    def is_grabbed(self) -> bool:
        return self._grabbed

    # ── Injection ────────────────────────────────────────────────

    def inject_mouse_move(self, x: int, y: int):
        self._xtst.XTestFakeMotionEvent(self._dpy, self._screen,
                                        x, y, CurrentTime)
        self._x11.XFlush(self._dpy)

    def inject_mouse_move_rel(self, dx: int, dy: int):
        self._xtst.XTestFakeRelativeMotionEvent(self._dpy, dx, dy,
                                                CurrentTime)
        self._x11.XFlush(self._dpy)

    def inject_mouse_button(self, button: int, pressed: bool):
        # Normalize: 1=left,2=mid,3=right maps directly to X11
        self._xtst.XTestFakeButtonEvent(self._dpy, button,
                                        Bool(pressed), CurrentTime)
        self._x11.XFlush(self._dpy)

    def inject_mouse_scroll(self, dx: int = 0, dy: int = 0):
        # X11: button 4=up, 5=down, 6=left, 7=right
        if dy > 0:
            for _ in range(dy):
                self._xtst.XTestFakeButtonEvent(self._dpy, 4,
                                                Bool(True), CurrentTime)
                self._xtst.XTestFakeButtonEvent(self._dpy, 4,
                                                Bool(False), CurrentTime)
        elif dy < 0:
            for _ in range(-dy):
                self._xtst.XTestFakeButtonEvent(self._dpy, 5,
                                                Bool(True), CurrentTime)
                self._xtst.XTestFakeButtonEvent(self._dpy, 5,
                                                Bool(False), CurrentTime)
        if dx > 0:
            for _ in range(dx):
                self._xtst.XTestFakeButtonEvent(self._dpy, 7,
                                                Bool(True), CurrentTime)
                self._xtst.XTestFakeButtonEvent(self._dpy, 7,
                                                Bool(False), CurrentTime)
        elif dx < 0:
            for _ in range(-dx):
                self._xtst.XTestFakeButtonEvent(self._dpy, 6,
                                                Bool(True), CurrentTime)
                self._xtst.XTestFakeButtonEvent(self._dpy, 6,
                                                Bool(False), CurrentTime)
        self._x11.XFlush(self._dpy)

    def inject_key(self, scancode: int, pressed: bool):
        x11_kc = hid_to_x11(scancode)
        if x11_kc:
            self._xtst.XTestFakeKeyEvent(self._dpy, x11_kc,
                                         Bool(pressed), CurrentTime)
            self._x11.XFlush(self._dpy)

    # ── Keyboard capture (evdev) ─────────────────────────────────

    def start_key_capture(self, on_key: Callable[[int, bool], None]) -> bool:
        self._kbd_callback = on_key
        devices = self._find_keyboard_devices()
        if not devices:
            log.error("Keyboard capture disabled: no /dev/input/event* "
                      "keyboards were discoverable. Add your user to the "
                      "'input' group (`sudo usermod -aG input $USER`) and "
                      "log back in.")
            return False

        opened: list[tuple[str, str, bool]] = []
        for dev_path, dev_name in devices:
            try:
                fd = os.open(dev_path, os.O_RDONLY | os.O_NONBLOCK)
            except (PermissionError, OSError) as e:
                log.error("Keyboard capture: cannot open %s (%s). "
                          "Add your user to the 'input' group.",
                          dev_path, e)
                continue
            grabbed = True
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 1)
            except OSError as e:
                grabbed = False
                log.warning("EVIOCGRAB %s failed (%s); key events will also "
                            "reach local apps on this machine.", dev_path, e)
            self._kbd_fds.append(fd)
            opened.append((dev_path, dev_name, grabbed))

        if not self._kbd_fds:
            log.error("Keyboard capture disabled: no input devices could "
                      "be opened.")
            return False
        log.info(
            "Keyboard capture active on %d device(s): %s",
            len(opened),
            ", ".join(
                f"{p} ({n or 'unknown'}){'' if g else ' [not grabbed]'}"
                for p, n, g in opened
            ),
        )

        self._kbd_running = True
        self._kbd_thread = threading.Thread(
            target=self._kbd_read_loop, daemon=True, name="kbd-evdev",
        )
        self._kbd_thread.start()
        return True

    def stop_key_capture(self):
        self._kbd_running = False
        for fd in self._kbd_fds:
            try:
                fcntl.ioctl(fd, EVIOCGRAB, 0)
            except OSError:
                pass
            try:
                os.close(fd)
            except OSError:
                pass
        self._kbd_fds.clear()

    def _kbd_read_loop(self):
        while self._kbd_running and self._kbd_fds:
            try:
                readable, _, _ = select.select(self._kbd_fds, [], [], 0.05)
            except (ValueError, OSError):
                break
            for fd in readable:
                try:
                    data = os.read(fd, EVENT_SIZE * 16)
                except OSError:
                    continue
                offset = 0
                while offset + EVENT_SIZE <= len(data):
                    _s, _us, ev_type, code, value = struct.unpack_from(
                        EVENT_FORMAT, data, offset,
                    )
                    offset += EVENT_SIZE
                    if ev_type != EV_KEY or value == KEY_REPEAT:
                        continue
                    hid = evdev_to_hid(code)
                    if hid and self._kbd_callback:
                        self._kbd_callback(hid, value == KEY_PRESS)

    @staticmethod
    def _has_keyboard_keys(key_caps: str) -> bool:
        """True if the device's key capability bitmap looks like a keyboard.

        /sys/class/input/eventN/device/capabilities/key is a whitespace-
        separated list of 64-bit hex words, most-significant-first. Real
        keyboard keys (KEY_ESC=1 through KEY_KPDOT=83) all live in the
        lowest 64-bit word; mouse buttons (BTN_MOUSE=0x110 = 272 and up)
        sit in higher words, leaving the low word empty on pure pointer
        devices. Require >=12 bits set in the low word to call something
        a keyboard.
        """
        words = key_caps.split()
        if not words:
            return False
        try:
            low = int(words[-1], 16)
        except ValueError:
            return False
        return bin(low).count("1") >= 12

    @staticmethod
    def _has_relative_axes(ev_caps: str) -> bool:
        """True if the device reports EV_REL (bit 2) — i.e. a pointing device.

        Combo devices like 'USB OPTICAL MOUSE Keyboard' (a mouse whose
        extra buttons expose a keyboard-looking interface) have letter
        keys AND relative axes on the same node. EVIOCGRAB on such a
        node steals mouse motion from the kernel input layer before X
        ever sees it — the symptom is a cursor that stays pinned at the
        warp center under Linux-source forwarding, producing zero
        deltas. We skip any node with EV_REL even if it also looks like
        a keyboard; real mouse motion events still come through the
        mouse's other event nodes.
        """
        try:
            return (int(ev_caps.strip(), 16) & (1 << 2)) != 0
        except ValueError:
            return False

    @staticmethod
    def _find_keyboard_devices() -> list[tuple[str, str]]:
        """Return [(device_path, human_name)] for devices that are keyboards."""
        devices: list[tuple[str, str]] = []
        input_dir = Path("/dev/input")
        if not input_dir.exists():
            return devices
        for entry in sorted(input_dir.iterdir()):
            if not entry.name.startswith("event"):
                continue
            sys_dev = Path(f"/sys/class/input/{entry.name}/device")
            key_caps_path = sys_dev / "capabilities/key"
            ev_caps_path = sys_dev / "capabilities/ev"
            if not key_caps_path.exists() or not ev_caps_path.exists():
                continue
            try:
                key_caps = key_caps_path.read_text().strip()
                ev_caps = ev_caps_path.read_text().strip()
            except (PermissionError, OSError):
                continue
            if not LinuxX11Backend._has_keyboard_keys(key_caps):
                continue
            if LinuxX11Backend._has_relative_axes(ev_caps):
                # Combo mouse+keyboard node — grabbing it would steal
                # pointer motion from X. Skip so the mouse stays usable.
                continue
            name = ""
            try:
                name = (sys_dev / "name").read_text().strip()
            except (PermissionError, OSError):
                pass
            devices.append((str(entry), name))
        return devices

    # ── Clipboard ────────────────────────────────────────────────

    def get_clipboard(self) -> str:
        tool = self._find_clip_tool()
        if not tool:
            return ""
        try:
            if tool == "xclip":
                r = subprocess.run(
                    ["xclip", "-selection", "clipboard", "-o"],
                    capture_output=True, text=True, timeout=2,
                )
            else:
                r = subprocess.run(
                    ["xsel", "--clipboard", "--output"],
                    capture_output=True, text=True, timeout=2,
                )
            return r.stdout if r.returncode == 0 else ""
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return ""

    def set_clipboard(self, text: str):
        tool = self._find_clip_tool()
        if not tool:
            return
        try:
            if tool == "xclip":
                subprocess.run(
                    ["xclip", "-selection", "clipboard", "-i"],
                    input=text, text=True, timeout=2,
                )
            else:
                subprocess.run(
                    ["xsel", "--clipboard", "--input"],
                    input=text, text=True, timeout=2,
                )
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass

    @staticmethod
    def _find_clip_tool() -> Optional[str]:
        for t in ("xclip", "xsel"):
            if shutil.which(t):
                return t
        return None

    def flush(self):
        if self._dpy:
            self._x11.XFlush(self._dpy)

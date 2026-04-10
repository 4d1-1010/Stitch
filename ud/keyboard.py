"""
Keyboard event capture using /dev/input (evdev).

This supplements the X11 pointer grab with actual keystroke capture.
Falls back to no keyboard forwarding if evdev isn't available.

Requires read access to /dev/input/event* devices (typically needs
the user to be in the 'input' group, or run as root).
"""

import ctypes
import ctypes.util
import logging
import os
import select
import struct
import threading
from pathlib import Path
from typing import Callable, Optional

log = logging.getLogger(__name__)

# Linux input event structure: struct input_event
# struct timeval { long sec; long usec; }  → 16 bytes on 64-bit
# __u16 type; __u16 code; __s32 value;
EVENT_FORMAT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)

# Event types
EV_KEY = 0x01

# Key values
KEY_RELEASE = 0
KEY_PRESS = 1
KEY_REPEAT = 2

# X11 keycode = evdev code + 8
EVDEV_TO_X11_OFFSET = 8


def find_keyboard_devices() -> list[str]:
    """Find keyboard input devices under /dev/input/."""
    devices = []
    input_dir = Path("/dev/input")
    if not input_dir.exists():
        return devices

    for entry in sorted(input_dir.iterdir()):
        if not entry.name.startswith("event"):
            continue
        # Check if this device has keyboard capabilities
        # by reading /sys/class/input/eventN/device/capabilities/key
        sysfs = Path(f"/sys/class/input/{entry.name}/device/capabilities/key")
        if sysfs.exists():
            try:
                caps = sysfs.read_text().strip()
                # A keyboard will have a non-trivial key capability bitmask
                # (at least several hex digits)
                if len(caps) > 10:
                    devices.append(str(entry))
            except (PermissionError, OSError):
                continue

    return devices


class KeyboardCapture:
    """
    Captures keyboard events from evdev.

    Calls `on_key(x11_keycode, pressed)` for each key press/release.
    """

    def __init__(self, on_key: Callable[[int, bool], None]):
        self._on_key = on_key
        self._fds: list[int] = []
        self._files: list = []
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def start(self) -> bool:
        """Start capturing. Returns False if no devices found."""
        devices = find_keyboard_devices()
        if not devices:
            log.warning("No keyboard input devices found. "
                        "Keyboard forwarding disabled. "
                        "Add your user to the 'input' group or run as root.")
            return False

        for dev_path in devices:
            try:
                fd = os.open(dev_path, os.O_RDONLY | os.O_NONBLOCK)
                self._fds.append(fd)
                log.info("Opened keyboard device: %s", dev_path)
            except PermissionError:
                log.warning("Cannot open %s (permission denied)", dev_path)
            except OSError as e:
                log.warning("Cannot open %s: %s", dev_path, e)

        if not self._fds:
            log.warning("No keyboard devices could be opened.")
            return False

        self._running = True
        self._thread = threading.Thread(
            target=self._read_loop, daemon=True, name="kbd-capture",
        )
        self._thread.start()
        return True

    def stop(self):
        self._running = False
        for fd in self._fds:
            try:
                os.close(fd)
            except OSError:
                pass
        self._fds.clear()

    def _read_loop(self):
        while self._running and self._fds:
            try:
                readable, _, _ = select.select(self._fds, [], [], 0.05)
            except (ValueError, OSError):
                break

            for fd in readable:
                try:
                    data = os.read(fd, EVENT_SIZE * 16)
                except OSError:
                    continue

                offset = 0
                while offset + EVENT_SIZE <= len(data):
                    _sec, _usec, ev_type, code, value = struct.unpack_from(
                        EVENT_FORMAT, data, offset,
                    )
                    offset += EVENT_SIZE

                    if ev_type != EV_KEY:
                        continue
                    if value == KEY_REPEAT:
                        continue  # ignore repeats, let target OS handle it

                    x11_keycode = code + EVDEV_TO_X11_OFFSET
                    pressed = (value == KEY_PRESS)
                    self._on_key(x11_keycode, pressed)

    @property
    def active(self) -> bool:
        return self._running and bool(self._fds)

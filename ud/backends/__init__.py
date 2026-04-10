"""
Platform abstraction layer for input capture, injection, and OS integration.

Each backend implements the InputBackend interface for its OS:
  - LinuxX11Backend  (X11 + evdev + xclip)
  - WindowsBackend   (Win32 hooks + SendInput)
  - MacOSBackend     (Quartz Event Services)

Usage:
    from ud.backends import create_backend
    backend = create_backend()
    backend.open()
"""

import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Callable, Optional


@dataclass
class MonitorRect:
    """A physical monitor in local OS screen coordinates."""
    id: str
    x: int
    y: int
    width: int
    height: int


class InputBackend(ABC):
    """
    Abstract interface for platform-specific input handling.

    Two instances are typically created: one for the input-polling thread
    (cursor queries, grabs, capture) and one for the asyncio thread
    (injection). This avoids thread-safety issues on platforms like X11
    where display connections aren't thread-safe.
    """

    # ── Lifecycle ────────────────────────────────────────────────

    @abstractmethod
    def open(self):
        """Initialize platform resources (display connection, etc.)."""

    @abstractmethod
    def close(self):
        """Release all resources."""

    # ── Monitor detection ────────────────────────────────────────

    @abstractmethod
    def query_monitors(self) -> list[MonitorRect]:
        """Return all physical monitors with local positions."""

    @property
    @abstractmethod
    def screen_width(self) -> int:
        """Total virtual screen width."""

    @property
    @abstractmethod
    def screen_height(self) -> int:
        """Total virtual screen height."""

    # ── Cursor ───────────────────────────────────────────────────

    @abstractmethod
    def get_cursor_pos(self) -> tuple[int, int]:
        """Return (x, y) in local screen coordinates."""

    @abstractmethod
    def set_cursor_pos(self, x: int, y: int):
        """Move cursor to (x, y) in local screen coordinates."""

    @abstractmethod
    def get_button_mask(self) -> int:
        """
        Return a bitmask of currently pressed mouse buttons.
        Bit layout: bit 0 = left, bit 1 = middle, bit 2 = right.
        """

    # ── Input grab (for forwarding mode) ─────────────────────────

    @abstractmethod
    def grab_input(self) -> bool:
        """
        Grab pointer + keyboard so all input is captured locally.
        Hides the cursor. Returns True on success.
        """

    @abstractmethod
    def ungrab_input(self):
        """Release pointer + keyboard grab and show cursor."""

    @property
    @abstractmethod
    def is_grabbed(self) -> bool:
        """Whether input is currently grabbed."""

    # ── Input injection (for receiving mode) ─────────────────────

    @abstractmethod
    def inject_mouse_move(self, x: int, y: int):
        """Inject an absolute mouse move to local coords (x, y)."""

    @abstractmethod
    def inject_mouse_move_rel(self, dx: int, dy: int):
        """Inject a relative mouse move delta."""

    @abstractmethod
    def inject_mouse_button(self, button: int, pressed: bool):
        """
        Inject mouse button press/release.
        button: 1=left, 2=middle, 3=right.
        """

    @abstractmethod
    def inject_mouse_scroll(self, dx: int = 0, dy: int = 0):
        """Inject scroll. dy>0 = scroll up, dy<0 = scroll down."""

    @abstractmethod
    def inject_key(self, scancode: int, pressed: bool):
        """
        Inject a key press/release.
        scancode: USB HID usage ID (universal across platforms).
        """

    # ── Keyboard capture (for forwarding mode) ───────────────────

    @abstractmethod
    def start_key_capture(
        self,
        on_key: Callable[[int, bool], None],
    ) -> bool:
        """
        Start capturing keyboard events.

        on_key(scancode, pressed): called for each key event.
            scancode is a USB HID usage ID.

        Returns True if capture started successfully.
        """

    @abstractmethod
    def stop_key_capture(self):
        """Stop keyboard capture."""

    # ── Clipboard ────────────────────────────────────────────────

    @abstractmethod
    def get_clipboard(self) -> str:
        """Read text from the system clipboard."""

    @abstractmethod
    def set_clipboard(self, text: str):
        """Write text to the system clipboard."""

    # ── Flush ────────────────────────────────────────────────────

    def flush(self):
        """Flush any pending operations (X11-specific, no-op on others)."""
        pass


def create_backend() -> InputBackend:
    """Auto-detect the platform and return the appropriate backend."""
    if sys.platform.startswith("linux"):
        from .linux_x11 import LinuxX11Backend
        return LinuxX11Backend()
    elif sys.platform == "win32":
        from .windows import WindowsBackend
        return WindowsBackend()
    elif sys.platform == "darwin":
        from .macos import MacOSBackend
        return MacOSBackend()
    else:
        raise RuntimeError(
            f"Unsupported platform: {sys.platform}. "
            "Supported: linux, win32, darwin."
        )

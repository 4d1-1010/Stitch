"""
Platform abstraction layer for input capture, injection, and OS integration.

Each backend implements the InputBackend interface for its OS:
  - LinuxX11Backend  (X11 + evdev + xclip)
  - WindowsBackend   (Win32 hooks + SendInput)
  - MacOSBackend     (Quartz Event Services)

Usage:
    from unio.backends import create_backend
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

    def get_modifier_mask(self) -> int:
        """
        Return a bitmask of currently pressed modifier keys.
        Bit 0 = Shift, bit 1 = Ctrl, bit 2 = Alt, bit 3 = Meta/Super.
        Used by the workspace "require Ctrl+Shift to cross" gate.
        Default 0 for backends that haven't wired it — the gate
        just refuses to let the cursor cross in that case.
        """
        return 0

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

    # ── Mouse-scroll capture (for forwarding mode) ───────────────

    def start_scroll_capture(
        self,
        on_scroll: Callable[[int, int], None],
    ) -> bool:
        """
        Start capturing scroll-wheel events for forwarding. Default
        impl is a no-op for backends that haven't wired it up yet —
        callers should treat a False return as "scroll forwarding
        isn't available on this platform."

        on_scroll(dx, dy): called once per wheel notch.
            dy positive = scroll up, dx positive = scroll right.
        """
        return False

    def stop_scroll_capture(self) -> None:
        """Stop scroll capture. Default no-op."""

    # ── Clipboard ────────────────────────────────────────────────

    @abstractmethod
    def get_clipboard(self) -> str:
        """Read text from the system clipboard."""

    @abstractmethod
    def set_clipboard(self, text: str):
        """Write text to the system clipboard."""

    # ── OS display configuration ─────────────────────────────────

    def set_monitor_positions(
        self, positions: dict[str, tuple[int, int]],
    ) -> bool:
        """Reconfigure the OS display arrangement.

        positions: {monitor_id: (x, y)} — monitor_id must match the id
        returned by query_monitors(). Coordinates are in the OS-local
        virtual screen space; the implementation may normalize them so
        the top-left monitor ends up at (0, 0).

        Returns True if the OS layout was changed, False if unsupported
        or the call failed. Default implementation is a no-op.
        """
        return False

    @staticmethod
    def _normalize_positions(
        positions: dict[str, tuple[int, int]],
    ) -> Optional[dict[str, tuple[int, int]]]:
        """Shift positions so the top-left monitor sits at (0, 0)."""
        if not positions:
            return None
        min_x = min(x for x, _ in positions.values())
        min_y = min(y for _, y in positions.values())
        return {k: (x - min_x, y - min_y) for k, (x, y) in positions.items()}

    # ── Cursor visibility ────────────────────────────────────────

    def hide_cursor(self) -> None:
        """Hide the local mouse cursor. Default: no-op.

        Independent of grab_input — used on dormant machines so the
        user doesn't see a second cursor parked on a screen that isn't
        the active one.
        """

    def show_cursor(self) -> None:
        """Restore the local mouse cursor. Default: no-op."""

    # ── Local input block ────────────────────────────────────────

    def start_pointer_block(self) -> None:
        """Stop the local mouse/touchpad from reaching local apps.

        Used on dormant machines that are NOT the input source — so
        clicks and motion never actually do anything locally. On the
        source machine we must NOT block: _poll_forwarding reads the
        OS cursor position to compute deltas, and blocking would pin
        the cursor just like EVIOCGRAB on the keyboard device pinned
        the X cursor in the earlier bug. Default: no-op.
        """

    def stop_pointer_block(self) -> None:
        """Release the block set by start_pointer_block. Default: no-op."""

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

"""
Clipboard synchronization using xclip/xsel subprocess calls.

Monitors the local clipboard for changes and provides methods to
get/set clipboard content. Avoids complex X11 selection protocol.
"""

import asyncio
import logging
import shutil
import subprocess
from typing import Optional, Callable, Awaitable

log = logging.getLogger(__name__)


def _find_tool() -> Optional[str]:
    """Find available clipboard CLI tool."""
    for tool in ("xclip", "xsel"):
        if shutil.which(tool):
            return tool
    return None


def get_clipboard() -> str:
    """Read the current CLIPBOARD selection contents."""
    tool = _find_tool()
    if tool is None:
        return ""

    try:
        if tool == "xclip":
            result = subprocess.run(
                ["xclip", "-selection", "clipboard", "-o"],
                capture_output=True, text=True, timeout=2,
            )
        else:
            result = subprocess.run(
                ["xsel", "--clipboard", "--output"],
                capture_output=True, text=True, timeout=2,
            )
        return result.stdout if result.returncode == 0 else ""
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ""


def set_clipboard(content: str):
    """Set the CLIPBOARD selection contents."""
    tool = _find_tool()
    if tool is None:
        log.warning("No clipboard tool (xclip/xsel) found.")
        return

    try:
        if tool == "xclip":
            proc = subprocess.run(
                ["xclip", "-selection", "clipboard", "-i"],
                input=content, text=True, timeout=2,
            )
        else:
            proc = subprocess.run(
                ["xsel", "--clipboard", "--input"],
                input=content, text=True, timeout=2,
            )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        log.warning("Failed to set clipboard.")


class ClipboardMonitor:
    """
    Polls the clipboard at an interval and fires a callback on change.
    """

    def __init__(self, on_change: Callable[[str], Awaitable[None]],
                 interval: float = 0.5):
        self._on_change = on_change
        self._interval = interval
        self._last_content: str = ""
        self._running = False
        self._suppress_next = False  # suppress echo of our own set

    async def start(self):
        self._running = True
        self._last_content = get_clipboard()
        while self._running:
            await asyncio.sleep(self._interval)
            try:
                current = get_clipboard()
            except Exception:
                continue
            if current != self._last_content:
                self._last_content = current
                if self._suppress_next:
                    self._suppress_next = False
                    continue
                await self._on_change(current)

    def stop(self):
        self._running = False

    def set_remote(self, content: str):
        """Set clipboard from remote, suppressing the change echo."""
        self._suppress_next = True
        self._last_content = content
        set_clipboard(content)

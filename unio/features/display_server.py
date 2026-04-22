"""Display server detection — X11 vs Wayland vs other.

Provides a single ``detect()`` call that returns one of:
  * ``"x11"``        — running under an X11 session (possibly with
    a Wayland compatibility layer via XWayland).
  * ``"wayland"``    — running under a native Wayland session.
  * ``"other"``      — detected display server we don't recognise.
  * ``None``         — no display server detected.

Detection strategy (in priority order):
  1. ``$WAYLAND_DISPLAY`` — set by the session manager for Wayland.
  2. ``$DISPLAY``        — set by the session manager for X11.
  3. ``$XDG_SESSION_TYPE`` — systemd login manager exposes this as
     ``x11``, ``wayland``, ``tty``, or ``unspecified``.

Each variable is checked independently; a session can have both set
(e.g. XWayland clients running under Wayland have both variables).
The most specific signal wins.
"""

from __future__ import annotations

import os
from typing import Optional


def detect() -> Optional[str]:
    """Return the detected display server type, or None."""
    session_type = os.environ.get("XDG_SESSION_TYPE", "").strip().lower()
    if session_type in ("wayland", "x11"):
        return session_type

    wayland_display = os.environ.get("WAYLAND_DISPLAY", "").strip()
    display = os.environ.get("DISPLAY", "").strip()

    if wayland_display:
        return "wayland"
    if display:
        return "x11"

    return None


def is_wayland() -> bool:
    """True when running under a Wayland session."""
    return detect() == "wayland"


def is_x11() -> bool:
    """True when running under an X11 session."""
    return detect() == "x11"
"""Abstract capture backend interface.

All capture backends (XComposite, WGC, PipeWire) implement this
protocol so ``display_stream`` can swap them interchangeably.
The shell pushes overlay exclusion IDs via ``set_exclude()``; the
backend is responsible for honouring them during ``grab()``.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Optional

from PIL import Image


class CaptureBackend(ABC):
    """Common interface for screen-capture backends."""

    # ── Lifecycle ───────────────────────────────────────────────

    @abstractmethod
    def open(self) -> bool:
        """Open the capture session. Returns True on success."""

    @abstractmethod
    def close(self) -> None:
        """Close and release all resources."""

    # ── Grab ────────────────────────────────────────────────────

    @abstractmethod
    def grab(self, rect: dict) -> Optional[Image.Image]:
        """Capture one frame of the given rect.

        ``rect`` keys: ``x``, ``y``, ``width``, ``height`` (in
        the backend's coordinate space). Returns a PIL.Image in
        RGB mode, or None on failure.
        """

    # ── Exclusion ───────────────────────────────────────────────

    @abstractmethod
    def set_exclude_xids(self, xids) -> None:
        """Tell the backend which window IDs to exclude from
        capture. Called by the shell before the capture loop
        starts, and whenever overlay windows change.

        Backends that don't support exclusion (e.g. WGC honours
        a system-level flag) may implement this as a no-op.
        """

    def set_exclude_hwnds(self, hwnds) -> None:
        """Windows-style exclusion handle. Default delegates to
        ``set_exclude_xids`` when the backend supports it."""
        self.set_exclude_xids(hwnds)
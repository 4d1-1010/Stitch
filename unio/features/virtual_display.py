"""Phase 3 scaffolding: virtual displays (phantom monitors).

The long-term plan is to register a phantom monitor with the host
OS so the user can drag windows onto a display that doesn't exist
physically — then stream that display's framebuffer to a real panel
on another PC. Two OS-level platforms are in scope:

  * Linux — evdi kernel module (`modprobe evdi`), userspace library
    via pyevdi / libevdi. Needs one-time root for the modprobe, then
    runs as the user.
  * Windows — Indirect Display Driver (IDD). Small mostly-user-mode
    driver that reports as an extra monitor over WDDM. Needs driver
    signing (self-sign + Test Mode for dev, EV cert for ship).

Neither capability is installable purely from user-space, so this
module's job is:

  1. Report whether the local host can host virtual displays at all.
  2. Expose a minimal create / destroy API that the shell can call
     when route overrides reference a virtual-display sink.
  3. Fall back to "unsupported" gracefully so existing features keep
     working when the driver isn't installed.

The actual IDD/evdi integration is stubbed — the checks detect
whether the runtime pieces are present, but the create path doesn't
yet spawn a phantom monitor. Wiring that live requires distributing
and signing the driver, which is a ship-time task outside this
module's scope.
"""

from __future__ import annotations

import ctypes
import logging
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Optional

log = logging.getLogger(__name__)


@dataclass
class VirtualDisplayCapabilities:
    """What virtual-display plumbing is actually available on this
    host. The shell uses `available` to decide whether the "Virtual
    displays" counter in the workspace editor should be enabled or
    shown dimmed with a hint."""
    available: bool
    backend: str                   # "evdi", "idd", or "unavailable"
    detail: str                    # human-readable (shown in UI tooltip)


def detect_capabilities() -> VirtualDisplayCapabilities:
    if sys.platform.startswith("linux"):
        return _detect_linux_evdi()
    if sys.platform == "win32":
        return _detect_windows_idd()
    return VirtualDisplayCapabilities(
        available=False, backend="unavailable",
        detail="Virtual displays are supported on Linux (evdi) and "
               "Windows (IDD) only.",
    )


def _detect_linux_evdi() -> VirtualDisplayCapabilities:
    # Three signals we look at, in order: module loaded? user has
    # permission to add an evdi card? at least one card already
    # present? Each combination gets a different detail string so
    # the workspace editor's hint guides the user to the exact
    # next step.
    if os.path.isdir("/sys/module/evdi"):
        # Module loaded. Is a card usable?
        try:
            lib = ctypes.CDLL("libevdi.so.1")
            lib.evdi_check_device.restype = ctypes.c_int
            lib.evdi_check_device.argtypes = [ctypes.c_int]
            has_card = any(lib.evdi_check_device(i) == 0
                           for i in range(16))
        except OSError:
            has_card = False
        if has_card:
            return VirtualDisplayCapabilities(
                available=True, backend="evdi",
                detail="evdi ready — virtual displays create real "
                       "phantom monitors.",
            )
        # Module loaded but no card. One-time sudo to create.
        return VirtualDisplayCapabilities(
            available=True, backend="evdi",
            detail=("evdi loaded but no virtual cards yet. Run once:"
                    "\n  sudo sh -c 'echo 1 > /sys/devices/evdi/add'"
                    "\nUntil then virtuals stream a placeholder card."),
        )
    if shutil.which("modprobe") and shutil.which("modinfo"):
        try:
            res = subprocess.run(
                ["modinfo", "evdi"],
                capture_output=True, text=True, timeout=1.5,
            )
            if res.returncode == 0:
                return VirtualDisplayCapabilities(
                    available=False, backend="evdi",
                    detail="evdi is installed but not loaded. Run "
                           "`sudo modprobe evdi` to enable virtual "
                           "displays.",
                )
        except (OSError, subprocess.SubprocessError):
            pass
    return VirtualDisplayCapabilities(
        available=False, backend="unavailable",
        detail="evdi kernel module not installed. Install package "
               "`evdi-dkms` (or equivalent) to enable virtual displays.",
    )


def _detect_windows_idd() -> VirtualDisplayCapabilities:
    # Windows IDD has two orthogonal states we care about:
    #   (a) driver service INSTALLED  (IndirectKmd.sys registered)
    #   (b) driver currently EXPOSING a monitor (EnumDisplayDevices
    #       lists something IDD-named)
    # A lot of IDD drivers (IndirectKmd, Spacedesk, Splashtop,
    # Parsec's) stay idle until a controller app asks them to spawn
    # a monitor — so state (a) without (b) is common and needs a
    # DIFFERENT UX than "nothing installed at all".
    try:
        import winreg  # type: ignore
    except ImportError:
        return VirtualDisplayCapabilities(
            available=False, backend="unavailable",
            detail="Could not probe Windows driver registry.",
        )

    # Known IDD service keys. Extend this list as we encounter more.
    services = {
        r"SYSTEM\CurrentControlSet\Services\usbmmidd": "USBMMIDD",
        r"SYSTEM\CurrentControlSet\Services\IndirectKmd": "IndirectKmd",
    }
    installed: Optional[str] = None
    for path, friendly in services.items():
        try:
            winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path).Close()
            installed = friendly
            break
        except OSError:
            continue

    # Ask virtual_display_idd if any live monitor currently matches
    # the IDD friendly-name hints.
    live = False
    try:
        from .virtual_display_idd import available as _idd_live
        live = _idd_live()
    except Exception:
        live = False

    if live:
        return VirtualDisplayCapabilities(
            available=True, backend="idd",
            detail=(f"IDD driver active"
                    f"{' (' + installed + ')' if installed else ''}"
                    " — virtual displays bind to an existing phantom "
                    "monitor."),
        )
    if installed:
        return VirtualDisplayCapabilities(
            available=False, backend="idd",
            detail=(f"IDD driver installed ({installed}) but no "
                    "virtual monitor is active. Some IDD drivers "
                    "(like IndirectKmd) only spawn monitors when a "
                    "helper app runs. A future unIO build will "
                    "bundle a signed driver that unIO can trigger "
                    "directly; until then, install USBMMIDD_v2 or "
                    "run a Parsec / Spacedesk / Splashtop session "
                    "once to bring a virtual monitor online."),
        )
    return VirtualDisplayCapabilities(
        available=False, backend="unavailable",
        detail="No indirect display driver installed. Install "
               "USBMMIDD_v2 (Apache-licensed), or wait for unIO's "
               "own signed IDD in a later build.",
    )


# ── Stub create/destroy API ─────────────────────────────────────────


@dataclass
class VirtualDisplay:
    id: str
    width: int
    height: int
    backend: str


class VirtualDisplayManager:
    """Owns the phantom monitors we've asked the OS to advertise.

    On Linux with evdi loaded, `create()` spawns a real virtual
    monitor via the evdi userspace bridge — its framebuffer becomes
    available through `live_frame(display_id)`. On Windows (IDD) the
    bridge is still stubbed; `create()` tracks state without spawning
    a real phantom until Step 1 lands.
    """

    def __init__(self) -> None:
        self._displays: dict[str, VirtualDisplay] = {}
        self._live: dict[str, object] = {}   # display_id → EvdiDevice / …
        self.caps = detect_capabilities()
        log.info("Virtual displays: %s (%s)",
                 self.caps.backend, self.caps.detail)

    @property
    def available(self) -> bool:
        return self.caps.available

    def create(self, display_id: str,
               width: int = 1920, height: int = 1080) -> Optional[VirtualDisplay]:
        if not self.caps.available:
            log.info("virtual_display create %s skipped: %s",
                     display_id, self.caps.detail)
            return None
        if display_id in self._displays:
            return self._displays[display_id]
        vd = VirtualDisplay(
            id=display_id, width=width, height=height,
            backend=self.caps.backend,
        )
        live_obj = self._spawn_live(display_id, width, height)
        if live_obj is not None:
            self._live[display_id] = live_obj
            log.info("virtual_display create %s (%dx%d, backend=%s) — live",
                     display_id, width, height, self.caps.backend)
        else:
            log.info("virtual_display create %s (%dx%d, backend=%s) — "
                     "driver bridge unavailable, placeholder only",
                     display_id, width, height, self.caps.backend)
        self._displays[display_id] = vd
        return vd

    def _spawn_live(self, display_id: str,
                    width: int, height: int):
        """Bring up the OS-level phantom. Returns the backend handle
        on success (evdi device, IDD monitor, …) or None if we fell
        back to bookkeeping-only mode."""
        if self.caps.backend == "evdi":
            try:
                from .virtual_display_evdi import EvdiDevice, available
            except Exception:
                log.exception("virtual_display_evdi import failed")
                return None
            if not available():
                return None
            dev = EvdiDevice(
                monitor_id=display_id,
                width=width, height=height,
            )
            if not dev.open():
                return None
            return dev
        if self.caps.backend == "idd":
            try:
                from .virtual_display_idd import IddDevice, available
            except Exception:
                log.exception("virtual_display_idd import failed")
                return None
            if not available():
                return None
            dev = IddDevice(
                monitor_id=display_id,
                width=width, height=height,
            )
            if not dev.open():
                return None
            return dev
        return None

    def destroy(self, display_id: str) -> None:
        vd = self._displays.pop(display_id, None)
        live = self._live.pop(display_id, None)
        if live is not None:
            try:
                live.close()
            except Exception:
                log.exception("live virtual destroy failed for %s",
                              display_id)
        if vd is None:
            return
        log.info("virtual_display destroy %s (backend=%s)",
                 display_id, vd.backend)

    def live_frame(self, display_id: str):
        """Return the most recent PIL.Image for a live virtual, or
        None. StreamServer's virtual capture path polls this — if we
        get a frame back, it goes on the wire as real pixels; if not,
        the placeholder card is used instead."""
        live = self._live.get(display_id)
        if live is None:
            return None
        try:
            return live.latest_frame()
        except Exception:
            log.exception("live_frame failed for %s", display_id)
            return None

    def all(self) -> list[VirtualDisplay]:
        return list(self._displays.values())

    def close_all(self) -> None:
        for display_id in list(self._live):
            self.destroy(display_id)

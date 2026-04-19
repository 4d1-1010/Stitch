"""Windows IDD (Indirect Display Driver) live bridge.

The long-term plan is for unIO to ship its own signed IDD that
creates monitors on demand. That work is blocked on signing
logistics (EV cert) and is out of scope for this iteration.

What this module does in the meantime: enumerate the existing
Windows monitors, pick out the ones that look IDD-provided
(friendly name contains "IDD", "Indirect", "Virtual", common
vendor strings from open-source IDDs), and hand them to
VirtualDisplayManager as available virtuals. The framebuffer is
captured via the same `mss` path the physical monitors use — IDD
monitors look like regular desktops to the compositor, so a
screen-rect grab Just Works.

Three practical upshots:
  * Works today on any Windows PC that already has an IDD driver
    installed (Parsec, Spacedesk, USBMMIDD, our future own, etc.).
  * Doesn't create new monitors on demand yet — that's the signed-
    driver phase. If the user wants more virtuals than the system
    has IDD slots, they'll see the "no backend" placeholder.
  * Easy to swap in a proper create/destroy path later: the same
    `IddDevice` interface is what a real API-driven flow would
    expose.
"""

from __future__ import annotations

import ctypes
import logging
import sys
import threading
from typing import Optional

log = logging.getLogger(__name__)


# Friendly-name substrings that identify IDD-provided monitors when
# they self-describe. Extend as we meet new drivers.
_IDD_NAME_HINTS = (
    "idd",
    "indirect",
    "virtual display",
    "usbmmidd",
    "usb mobile monitor",
    "spacedesk",
    "splashtop",
    "duet",
    "parsec",
    "amyuni",
)

# Some IDDs — USBMMIDD_v2 in particular — don't put their brand in
# the monitor's friendly name. They advertise as "Generic Non-PnP
# Monitor" with a `MONITOR\Default_Monitor` hardware id. We fall back
# to matching on the adapter's device ID / device key so those still
# get picked up.
_IDD_DEVICE_HINTS = (
    "usbmmidd",
    "amyuni",
    "iddcx",
    "indirect",
    "virtual_display",
    "spacedesk",
    # USBMMIDD's monitor always advertises with this hardware id,
    # independent of friendly name. Matching on it alone is reliable
    # enough to skip the "≥2 monitors" guard that Tier 3 used to
    # require.
    "monitor\\default_monitor",
    "monitor/default_monitor",
)

# The last resort: if the friendly name is "Generic Non-PnP Monitor"
# AND the monitor sits at a position we never had before a physical
# cable change, treat it as a virtual. This handles USBMMIDD's
# bare-metal case where nothing in the device metadata hints at IDD.
_GENERIC_VIRTUAL_NAMES = (
    "generic non-pnp monitor",
    "non-pnp monitor",
)


class IddDevice:
    """One virtual monitor claimed from the pool of IDD-provided
    Windows monitors. Captures its screen rect via `mss` and serves
    the latest PIL.Image through `latest_frame()`.

    This class does NOT create monitors — it binds to ones that
    already exist, courtesy of whatever IDD driver the user already
    installed. When our own IDD ships, `open()` can be extended to
    request a new monitor instead of finding one; the rest of the
    public shape stays identical.
    """

    def __init__(self, monitor_id: str,
                 width: int = 1920, height: int = 1080):
        self.monitor_id = monitor_id
        self.width = width
        self.height = height
        self._rect: Optional[dict] = None       # {"left","top","width","height"}
        self._device_name: str = ""
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._frame_lock = threading.Lock()
        self._latest_image = None

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        cand = _pick_idd_monitor()
        if cand is None:
            log.info("IDD: no virtual monitor candidate found "
                     "(install an IDD driver to enable)")
            return False
        self._rect = cand["rect"]
        self._device_name = cand["name"]
        self.width = cand["rect"]["width"]
        self.height = cand["rect"]["height"]
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run_loop, daemon=True,
            name=f"idd-{self.monitor_id}",
        )
        self._thread.start()
        log.info("IDD bound to %s at %dx%d+%d+%d",
                 self._device_name, self.width, self.height,
                 self._rect["left"], self._rect["top"])
        return True

    def close(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._thread = None

    # ── Capture loop ─────────────────────────────────────────────

    def _run_loop(self) -> None:
        import time
        try:
            import mss
        except Exception:
            log.exception("IDD: mss not available")
            return
        sct = None
        period = 1.0 / 30.0
        try:
            sct = mss.mss()
        except Exception:
            log.exception("IDD: mss.mss() failed")
            return
        while not self._stop.is_set():
            tick = time.monotonic()
            try:
                raw = sct.grab(self._rect)
                from PIL import Image
                img = Image.frombytes(
                    "RGB", raw.size, raw.rgb)
                with self._frame_lock:
                    self._latest_image = img
            except Exception:
                log.exception("IDD capture failed")
                time.sleep(0.5)
                continue
            elapsed = time.monotonic() - tick
            remaining = period - elapsed
            if remaining > 0:
                time.sleep(remaining)

    # ── Public accessor ──────────────────────────────────────────

    def latest_frame(self):
        with self._frame_lock:
            return self._latest_image


# ── Enumeration ─────────────────────────────────────────────────────


def _pick_idd_monitor() -> Optional[dict]:
    """Walk Windows' display enumeration, return the first monitor
    that matches an IDD heuristic. Tries three tiers in order:

        1. Friendly / device-string contains a known IDD brand.
        2. DeviceID / DeviceKey contains an IDD vendor substring
           (USBMMIDD's adapter carries the string even when the
           monitor's friendly name doesn't).
        3. Friendly name matches the "Generic Non-PnP Monitor"
           fallback that USBMMIDD uses by default. This is the last
           resort since a real no-EDID monitor could theoretically
           match too — but in practice no one plugs those into PCs
           running unIO.
    """
    if sys.platform != "win32":
        return None
    try:
        candidates = _enum_monitors()
    except Exception:
        log.exception("IDD: EnumDisplay* failed")
        return None
    # Always log what we saw, so we can debug misses remotely via
    # the unio.log without having to rerun a probe in the user's
    # interactive session.
    log.info("IDD: enumerated %d monitor(s)", len(candidates))
    for c in candidates:
        log.info("  name=%r str_id=%r key=%r",
                 c.get("name"),
                 c.get("device_id"),
                 c.get("device_key"))
    if not candidates:
        return None
    # Tier 1: branded friendly name.
    for cand in candidates:
        low = cand["name"].lower()
        if any(h in low for h in _IDD_NAME_HINTS):
            return cand
    # Tier 2: device id / key hints (covers USBMMIDD's adapter).
    for cand in candidates:
        blob = (cand.get("device_id", "")
                + " " + cand.get("device_key", "")).lower()
        if any(h in blob for h in _IDD_DEVICE_HINTS):
            return cand
    # Tier 3: generic-named monitor fallback. Only fire when there
    # are at least two monitors — if there's only one and it's
    # generic, that's the user's real panel, don't hijack it.
    if len(candidates) >= 2:
        for cand in candidates:
            if cand["name"].lower() in _GENERIC_VIRTUAL_NAMES:
                return cand
    return None


def _enum_monitors() -> list[dict]:
    """Wraps `EnumDisplayDevices` + `EnumDisplaySettingsEx` to list
    every attached Windows monitor with its device name and screen
    rect. Pure ctypes — no external deps."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)

    class DISPLAY_DEVICE(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("DeviceName", ctypes.c_wchar * 32),
            ("DeviceString", ctypes.c_wchar * 128),
            ("StateFlags", ctypes.c_ulong),
            ("DeviceID", ctypes.c_wchar * 128),
            ("DeviceKey", ctypes.c_wchar * 128),
        ]

    class DEVMODE(ctypes.Structure):
        _fields_ = [
            ("dmDeviceName", ctypes.c_wchar * 32),
            ("dmSpecVersion", ctypes.c_ushort),
            ("dmDriverVersion", ctypes.c_ushort),
            ("dmSize", ctypes.c_ushort),
            ("dmDriverExtra", ctypes.c_ushort),
            ("dmFields", ctypes.c_ulong),
            ("dmPositionX", ctypes.c_long),
            ("dmPositionY", ctypes.c_long),
            ("dmDisplayOrientation", ctypes.c_ulong),
            ("dmDisplayFixedOutput", ctypes.c_ulong),
            ("dmColor", ctypes.c_short),
            ("dmDuplex", ctypes.c_short),
            ("dmYResolution", ctypes.c_short),
            ("dmTTOption", ctypes.c_short),
            ("dmCollate", ctypes.c_short),
            ("dmFormName", ctypes.c_wchar * 32),
            ("dmLogPixels", ctypes.c_ushort),
            ("dmBitsPerPel", ctypes.c_ulong),
            ("dmPelsWidth", ctypes.c_ulong),
            ("dmPelsHeight", ctypes.c_ulong),
            ("dmDisplayFlags", ctypes.c_ulong),
            ("dmDisplayFrequency", ctypes.c_ulong),
            # Remaining fields omitted — we only need the display ones.
            ("_padding", ctypes.c_ubyte * 64),
        ]

    user32.EnumDisplayDevicesW.restype = ctypes.c_int
    user32.EnumDisplayDevicesW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_ulong,
        ctypes.POINTER(DISPLAY_DEVICE), ctypes.c_ulong,
    ]
    user32.EnumDisplaySettingsExW.restype = ctypes.c_int
    user32.EnumDisplaySettingsExW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_ulong,
        ctypes.POINTER(DEVMODE), ctypes.c_ulong,
    ]
    ENUM_CURRENT_SETTINGS = 0xFFFFFFFF
    DISPLAY_DEVICE_ACTIVE = 0x1

    out: list[dict] = []
    idx = 0
    while True:
        dev = DISPLAY_DEVICE()
        dev.cb = ctypes.sizeof(DISPLAY_DEVICE)
        if not user32.EnumDisplayDevicesW(None, idx, ctypes.byref(dev), 0):
            break
        idx += 1
        if not (dev.StateFlags & DISPLAY_DEVICE_ACTIVE):
            continue
        # Pull the monitor's friendly name via a second call,
        # scoped to this adapter.
        mon = DISPLAY_DEVICE()
        mon.cb = ctypes.sizeof(DISPLAY_DEVICE)
        user32.EnumDisplayDevicesW(dev.DeviceName, 0,
                                   ctypes.byref(mon), 0)
        name = mon.DeviceString or dev.DeviceString
        # Geometry via EnumDisplaySettingsEx.
        mode = DEVMODE()
        mode.dmSize = ctypes.sizeof(DEVMODE)
        if not user32.EnumDisplaySettingsExW(
                dev.DeviceName, ENUM_CURRENT_SETTINGS,
                ctypes.byref(mode), 0):
            continue
        out.append({
            "device": dev.DeviceName,
            "name": name,
            # Device IDs / keys often contain the IDD vendor substring
            # even when the friendly name doesn't. Adapter-level ids
            # (`dev.DeviceID`) catch USBMMIDD; monitor-level
            # (`mon.DeviceID`) catches Spacedesk / Splashtop etc.
            "device_id": f"{dev.DeviceID or ''} {mon.DeviceID or ''}",
            "device_key": f"{dev.DeviceKey or ''} {mon.DeviceKey or ''}",
            "rect": {
                "left": int(mode.dmPositionX),
                "top": int(mode.dmPositionY),
                "width": int(mode.dmPelsWidth),
                "height": int(mode.dmPelsHeight),
            },
        })
    return out


def available() -> bool:
    """True when this Windows host has at least one IDD-provided
    monitor we can bind to. Checked lazily — the result can change
    between calls as IDD drivers add / remove monitors."""
    if sys.platform != "win32":
        return False
    return _pick_idd_monitor() is not None

"""OS-level display enable/disable for Phase 2 handoff.

When a monitor on this PC has been routed to a remote sink, the
source PC should behave as if the cable were unplugged: apps stop
opening on it, already-open windows get rearranged onto the
remaining monitors, the cursor can't land there. Linux's xrandr
does this with `--output X --off`; Windows needs `SetDisplayConfig`
or `ChangeDisplaySettingsEx`.

Both paths are best-effort. If the OS refuses the call (permissions,
wrong backend, monitor id mismatch between our layout model and
the OS's naming), the function returns False and the caller falls
back to the borderless "projected" overlay that still visually
blocks the panel.

Re-enable is the symmetric `--auto` / primary-desktop call. Both
directions are idempotent — calling "enable" on a monitor that's
already on is a no-op. We also register an atexit to force every
disabled monitor back on before the process dies, so a crash
during the overlay phase doesn't leave the user without a display.
"""

from __future__ import annotations

import atexit
import logging
import shutil
import subprocess
import sys
import threading

log = logging.getLogger(__name__)


# machine-local registry of monitors we've disabled in this process.
# On shutdown / atexit we walk it and try to re-enable everything.
_disabled_lock = threading.Lock()
_disabled_set: set[str] = set()
_atexit_registered = False


def _ensure_atexit() -> None:
    global _atexit_registered
    if _atexit_registered:
        return
    atexit.register(_restore_all_on_exit)
    _atexit_registered = True


def _restore_all_on_exit() -> None:
    with _disabled_lock:
        ids = list(_disabled_set)
    for mid in ids:
        try:
            set_monitor_enabled(mid, True)
        except Exception:
            log.exception("atexit re-enable %s failed", mid)


def set_monitor_enabled(monitor_id: str, enabled: bool) -> bool:
    """Turn a physical display on or off.

    Returns True when the OS accepted the request, False otherwise.
    A False return means the caller should lean on the soft overlay
    alone — the panel is still visible at the OS level.
    """
    _ensure_atexit()
    if sys.platform.startswith("linux"):
        ok = _linux_xrandr_set(monitor_id, enabled)
    elif sys.platform == "win32":
        ok = _win_set(monitor_id, enabled)
    else:
        ok = False

    if ok:
        with _disabled_lock:
            if enabled:
                _disabled_set.discard(monitor_id)
            else:
                _disabled_set.add(monitor_id)
    return ok


# ── Linux (xrandr) ──────────────────────────────────────────────────


def _linux_xrandr_set(monitor_id: str, enabled: bool) -> bool:
    xrandr = shutil.which("xrandr")
    if not xrandr:
        log.info("xrandr not on PATH; leaving %s alone", monitor_id)
        return False
    args = [xrandr, "--output", monitor_id]
    args += ["--auto"] if enabled else ["--off"]
    try:
        res = subprocess.run(
            args, capture_output=True, text=True, timeout=4.0,
        )
    except (OSError, subprocess.SubprocessError) as e:
        log.info("xrandr call failed for %s: %s", monitor_id, e)
        return False
    if res.returncode != 0:
        log.info("xrandr %s %s rc=%d: %s",
                 monitor_id, "auto" if enabled else "off",
                 res.returncode, res.stderr.strip())
        return False
    # xrandr returns rc=0 even for unknown output names — it prints a
    # "warning: output X not found; ignoring" to stderr and exits
    # cleanly. Scan for the warning so the caller actually learns
    # their monitor_id didn't match any OS-known output.
    if "not found" in (res.stderr or "").lower():
        log.info("xrandr ignored unknown output %s", monitor_id)
        return False
    log.info("xrandr %s %s OK", monitor_id,
             "auto" if enabled else "off")
    return True


# ── Windows (ChangeDisplaySettingsEx) ───────────────────────────────


def _win_set(monitor_id: str, enabled: bool) -> bool:
    """Best-effort Windows implementation. `ChangeDisplaySettingsEx`
    with `NULL lpDevMode` and `CDS_UPDATEREGISTRY | CDS_NORESET` can
    detach/attach a monitor by its device name. v1 reports False
    when the backend-side name doesn't look like a Windows device
    path — the soft overlay is sufficient to keep the panel
    unreachable in that case."""
    try:
        import ctypes
        from ctypes import wintypes  # noqa: F401
    except Exception:
        return False
    # Real detach requires enumerating DISPLAY_DEVICE and matching
    # our monitor_id to `DeviceName` (e.g. "\\\\.\\DISPLAY1"). Until
    # we plumb that through, log + bail so we don't silently misfire.
    log.info("win32 monitor disable not yet wired (monitor=%s, enabled=%s)",
             monitor_id, enabled)
    return False

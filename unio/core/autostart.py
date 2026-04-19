"""Register / unregister unIO as an OS autostart entry.

Linux  ~/.config/autostart/unio.desktop  (XDG autostart spec)
macOS  ~/Library/LaunchAgents/io.unio.launch.plist  (launchd)
Windows HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run  entry 'unio'

The implementation is deliberately best-effort: if the OS-specific
step fails (missing directory, registry permission, etc.) the caller
gets False back and surfaces a gentle warning, rather than throwing.
"""

from __future__ import annotations

import logging
import os
import pathlib
import sys
from typing import Optional

log = logging.getLogger(__name__)


def current_executable_path() -> str:
    """Path that should be invoked to re-launch unIO on login.

    When frozen by PyInstaller the app's entry point is the bundle's
    EXE/ELF; otherwise we fall back to the interpreter + module form.
    """
    if getattr(sys, "frozen", False):
        return sys.executable
    return f"{sys.executable} -m unio.apps.shell"


def is_autostart_enabled() -> bool:
    try:
        if sys.platform == "win32":
            return _win_is_enabled()
        if sys.platform == "darwin":
            return _macos_plist_path().exists()
        return _linux_desktop_path().exists()
    except Exception:
        log.exception("autostart status check failed")
        return False


def set_autostart_enabled(enabled: bool) -> bool:
    try:
        if sys.platform == "win32":
            return _win_set(enabled)
        if sys.platform == "darwin":
            return _macos_set(enabled)
        return _linux_set(enabled)
    except Exception:
        log.exception("autostart set failed")
        return False


# ── Linux (XDG autostart) ────────────────────────────────────────

def _linux_autostart_dir() -> pathlib.Path:
    base = os.environ.get("XDG_CONFIG_HOME") \
        or os.path.expanduser("~/.config")
    return pathlib.Path(base) / "autostart"


def _linux_desktop_path() -> pathlib.Path:
    return _linux_autostart_dir() / "unio.desktop"


def _linux_set(enabled: bool) -> bool:
    path = _linux_desktop_path()
    if not enabled:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    entry = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=unIO\n"
        f"Exec={current_executable_path()}\n"
        "X-GNOME-Autostart-enabled=true\n"
        "Terminal=false\n"
    )
    path.write_text(entry, encoding="utf-8")
    return True


# ── macOS (launchd agent) ────────────────────────────────────────

def _macos_plist_path() -> pathlib.Path:
    return pathlib.Path(os.path.expanduser(
        "~/Library/LaunchAgents/io.unio.launch.plist"))


def _macos_set(enabled: bool) -> bool:
    path = _macos_plist_path()
    if not enabled:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    exe_parts = current_executable_path().split()
    prog_args = "\n".join(f"    <string>{p}</string>"
                          for p in exe_parts)
    plist = (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Label</key><string>io.unio.launch</string>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>ProgramArguments</key>\n"
        f"  <array>\n{prog_args}\n  </array>\n"
        "</dict>\n"
        "</plist>\n"
    )
    path.write_text(plist, encoding="utf-8")
    return True


# ── Windows (HKCU\...\Run) ───────────────────────────────────────

_WIN_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_WIN_VALUE = "unio"


def _win_open(write: bool = False):
    import winreg
    access = winreg.KEY_WRITE if write else winreg.KEY_READ
    return winreg.OpenKey(winreg.HKEY_CURRENT_USER, _WIN_RUN_KEY, 0, access)


def _win_is_enabled() -> bool:
    import winreg
    try:
        with _win_open() as key:
            winreg.QueryValueEx(key, _WIN_VALUE)
            return True
    except FileNotFoundError:
        return False


def _win_set(enabled: bool) -> bool:
    import winreg
    if enabled:
        with _win_open(write=True) as key:
            winreg.SetValueEx(key, _WIN_VALUE, 0, winreg.REG_SZ,
                              f'"{current_executable_path()}"')
        return True
    try:
        with _win_open(write=True) as key:
            winreg.DeleteValue(key, _WIN_VALUE)
    except FileNotFoundError:
        pass
    return True

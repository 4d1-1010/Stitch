"""Persistent per-user config for the unIO shell.

A single YAML file under the OS-conventional per-user config dir:
    Linux / *nix  ~/.config/unio/config.yaml
    macOS         ~/Library/Preferences/unio/config.yaml
    Windows       %APPDATA%\\unio\\config.yaml

Holds the values shown in the Settings tab's Local / Troubleshoot /
Network sections. Unknown keys are preserved on round-trip so
forward-compatibility isn't a concern when an older build opens a
newer file.
"""

from __future__ import annotations

import os
import pathlib
import sys
from typing import Any

try:
    import yaml
except ImportError:
    yaml = None  # type: ignore


def user_config_dir() -> pathlib.Path:
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return pathlib.Path(base) / "unio"
    if sys.platform == "darwin":
        return pathlib.Path(os.path.expanduser(
            "~/Library/Preferences/unio"))
    base = os.environ.get("XDG_CONFIG_HOME") or \
        os.path.expanduser("~/.config")
    return pathlib.Path(base) / "unio"


def config_path() -> pathlib.Path:
    return user_config_dir() / "config.yaml"


def load_config() -> dict[str, Any]:
    """Return a dict of persisted settings. Missing file, missing YAML
    library, or corrupted content all resolve to an empty dict so the
    caller can fall back to defaults cleanly."""
    path = config_path()
    if not path.exists() or yaml is None:
        return {}
    try:
        with path.open("r", encoding="utf-8") as fh:
            data = yaml.safe_load(fh) or {}
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def save_config(data: dict[str, Any]) -> bool:
    if yaml is None:
        return False
    path = config_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as fh:
            yaml.safe_dump(data, fh, default_flow_style=False,
                           sort_keys=True)
        return True
    except Exception:
        return False

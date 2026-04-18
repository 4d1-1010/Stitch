"""Persistent user-facing app settings (on-disk JSON).

Small scope for now: the Settings tab toggles that we want to
survive a restart. Lives next to the saved layout under the user's
XDG config dir (or %APPDATA% on Windows) so it travels with the rest
of UnIO's per-user state.
"""

from __future__ import annotations

import json
import logging
import os
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

log = logging.getLogger(__name__)


def _settings_path() -> Path:
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME",
                                    Path.home() / ".config"))
    return base / "unio" / "settings.json"


@dataclass
class AppSettings:
    clipboard_sync_enabled: bool = True


def load() -> AppSettings:
    p = _settings_path()
    if not p.exists():
        return AppSettings()
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        log.warning("settings: failed to read %s (%s) — using defaults",
                    p, e)
        return AppSettings()
    # Ignore unknown keys so a forward-incompatible file doesn't crash.
    known = {f: data[f] for f in AppSettings.__dataclass_fields__
             if f in data}
    return AppSettings(**known)


def save(s: AppSettings) -> None:
    p = _settings_path()
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(asdict(s), indent=2), encoding="utf-8")
    except OSError as e:
        log.warning("settings: failed to write %s: %s", p, e)

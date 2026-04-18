#!/usr/bin/env python3
"""Build a standalone UnIO binary for the current platform.

Usage (from the repo root):
    python packaging/build.py                # end-user build
    python packaging/build.py --dev-logs     # devs: enable log UI

The output lands in `dist/`:
  - Linux:    dist/unio            (ELF executable)
  - Windows:  dist/unio.exe        (PE executable)
  - macOS:    dist/unio            (Mach-O executable)
              dist/unio.app        (application bundle, if built with --app)

PyInstaller must be run on each target platform — there is no cross-compile.
To ship for all three OSes, run this script on each.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "packaging" / "unio.spec"
DIST = ROOT / "dist"
BUILD = ROOT / "build"
INIT_PY = ROOT / "unio" / "__init__.py"


@contextmanager
def _dev_logs_patched(enable: bool):
    """Temporarily bake DEV_LOGS=True into unio/__init__.py so the
    PyInstaller bundle ships with the dev log UI enabled. Restored on
    exit so a --dev-logs build doesn't leave the source tree dirty.
    """
    if not enable:
        yield
        return
    original = INIT_PY.read_text(encoding="utf-8")
    patched = re.sub(
        r"^DEV_LOGS\s*=.*$",
        "DEV_LOGS = True  # baked by build.py --dev-logs",
        original,
        count=1,
        flags=re.MULTILINE,
    )
    if patched == original:
        print("warning: could not patch DEV_LOGS in unio/__init__.py",
              file=sys.stderr)
    INIT_PY.write_text(patched, encoding="utf-8")
    try:
        yield
    finally:
        INIT_PY.write_text(original, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clean", action="store_true",
                        help="Wipe dist/ and build/ before building")
    parser.add_argument("--app", action="store_true",
                        help="Also build a macOS .app bundle (macOS only)")
    parser.add_argument("--dev-logs", action="store_true",
                        help="Bake DEV_LOGS=True into the binary so the "
                             "log buffer + 'View logs' UI are always on. "
                             "End-user builds should OMIT this flag.")
    args = parser.parse_args()

    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("PyInstaller is not installed. Run:", file=sys.stderr)
        print("    pip install pyinstaller", file=sys.stderr)
        return 1

    if args.clean:
        for d in (DIST, BUILD):
            if d.exists():
                shutil.rmtree(d)

    cmd = [
        sys.executable, "-m", "PyInstaller",
        str(SPEC),
        "--distpath", str(DIST),
        "--workpath", str(BUILD),
        "--noconfirm",
    ]
    print("+", " ".join(cmd))
    if args.dev_logs:
        print("  (DEV_LOGS baked in — diagnostic UI will be enabled)")
    with _dev_logs_patched(args.dev_logs):
        rc = subprocess.call(cmd, cwd=str(ROOT))
    if rc != 0:
        return rc

    if args.app and sys.platform == "darwin":
        app_cmd = cmd + ["--windowed", "--osx-bundle-identifier", "dev.unio.app"]
        print("+", " ".join(app_cmd))
        with _dev_logs_patched(args.dev_logs):
            rc = subprocess.call(app_cmd, cwd=str(ROOT))
        if rc != 0:
            return rc

    print(f"\nBuild complete. See: {DIST.relative_to(ROOT)}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build a standalone Stitch binary for the current platform.

Usage (from the repo root):
    python packaging/build.py

The output lands in `dist/`:
  - Linux:    dist/stitch            (ELF executable)
  - Windows:  dist/stitch.exe        (PE executable)
  - macOS:    dist/stitch            (Mach-O executable)
              dist/stitch.app        (application bundle, if built with --app)

PyInstaller must be run on each target platform — there is no cross-compile.
To ship for all three OSes, run this script on each.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "packaging" / "stitch.spec"
DIST = ROOT / "dist"
BUILD = ROOT / "build"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clean", action="store_true",
                        help="Wipe dist/ and build/ before building")
    parser.add_argument("--app", action="store_true",
                        help="Also build a macOS .app bundle (macOS only)")
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
    rc = subprocess.call(cmd, cwd=str(ROOT))
    if rc != 0:
        return rc

    if args.app and sys.platform == "darwin":
        app_cmd = cmd + ["--windowed", "--osx-bundle-identifier", "dev.stitch.app"]
        print("+", " ".join(app_cmd))
        rc = subprocess.call(app_cmd, cwd=str(ROOT))
        if rc != 0:
            return rc

    print(f"\nBuild complete. See: {DIST.relative_to(ROOT)}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())

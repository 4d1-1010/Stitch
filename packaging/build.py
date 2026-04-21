#!/usr/bin/env python3
"""Build a standalone unIO binary for the current platform.

Usage (from the repo root):
    python packaging/build.py                # end-user build
    python packaging/build.py --dev-logs     # devs: enable log UI

The output lands in `dist/`:
  - Linux:    dist/unio              (ELF executable)
              dist/unIO-x86_64.AppImage  (if appimagetool is on PATH)
  - Windows:  dist/unio.exe          (PE executable)
  - macOS:    dist/unio              (Mach-O executable)
              dist/unio.app          (application bundle, if built with --app)

PyInstaller must be run on each target platform — there is no cross-compile.
To ship for all three OSes, run this script on each.

On Linux the script also tries to wrap the binary in an AppImage so
Nautilus/KDE/etc. can show the unIO icon on the file itself (ELF
binaries can't carry an embedded icon). It needs `appimagetool` on
PATH — grab it from
https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
(chmod +x, drop in ~/.local/bin). If it's missing, the script just
skips the AppImage step with a warning so normal builds still work.
"""

from __future__ import annotations

import argparse
import os
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
DESKTOP_FILE = ROOT / "packaging" / "unio.desktop"
ICON_PNG = ROOT / "assets" / "logo_mark_256.png"


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

    # Native helper (unio-pipe) — builds the C++ control-plane
    # binary if cmake is available. Day-1 scope is just the
    # control socket + caps probe; no data plane yet, so the
    # Python pipeline keeps doing the actual streaming. A missing
    # cmake isn't fatal; the helper is optional until PR 6 lands.
    _try_build_native_helper()

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

    if sys.platform == "linux":
        _try_build_appimage()

    print(f"\nBuild complete. See: {DIST.relative_to(ROOT)}/")
    return 0


def _try_build_native_helper() -> None:
    """Configure + build unio-pipe if cmake is on PATH. Drops the
    resulting binary at dist/unio-pipe so the AppImage / PyInstaller
    onefile step can bundle it. Silent no-op when cmake is absent
    — PR 6 isn't required for the Python pipeline to function."""
    helper_src = ROOT / "unio-pipe"
    if not (helper_src / "CMakeLists.txt").exists():
        return
    cmake = shutil.which("cmake")
    if cmake is None:
        print("  (cmake not found — skipping unio-pipe native "
              "helper build)", file=sys.stderr)
        return
    build_dir = helper_src / "build"
    print(f"+ {cmake} -S {helper_src} -B {build_dir} "
          "-DCMAKE_BUILD_TYPE=Release")
    rc = subprocess.call(
        [cmake, "-S", str(helper_src), "-B", str(build_dir),
         "-DCMAKE_BUILD_TYPE=Release"],
        cwd=str(ROOT))
    if rc != 0:
        print(f"  (cmake configure exited {rc} — skipping helper)",
              file=sys.stderr)
        return
    print(f"+ {cmake} --build {build_dir} -j")
    rc = subprocess.call([cmake, "--build", str(build_dir), "-j"],
                         cwd=str(ROOT))
    if rc != 0:
        print(f"  (cmake build exited {rc} — skipping helper)",
              file=sys.stderr)
        return
    exe_name = "unio-pipe.exe" if sys.platform == "win32" else "unio-pipe"
    built = build_dir / exe_name
    if not built.exists():
        return
    DIST.mkdir(parents=True, exist_ok=True)
    dest = DIST / exe_name
    shutil.copy2(built, dest)
    dest.chmod(0o755)
    print(f"  unio-pipe: {dest.relative_to(ROOT)}")


def _try_build_appimage() -> None:
    """Wrap dist/unio in a self-contained AppImage so the file carries
    the unIO icon + .desktop metadata. No-op (with a warning) if
    appimagetool isn't on PATH — the raw binary still works without it.
    """
    tool = shutil.which("appimagetool") or shutil.which(
        "appimagetool-x86_64.AppImage")
    if tool is None:
        print("  (appimagetool not found — skipping AppImage step; "
              "install from https://github.com/AppImage/AppImageKit/releases "
              "to enable)",
              file=sys.stderr)
        return

    binary = DIST / "unio"
    if not binary.exists():
        print(f"  (no {binary} — PyInstaller output missing, "
              f"skipping AppImage)", file=sys.stderr)
        return

    appdir = BUILD / "AppDir"
    if appdir.exists():
        shutil.rmtree(appdir)
    (appdir / "usr" / "bin").mkdir(parents=True)
    shutil.copy2(binary, appdir / "usr" / "bin" / "unio")
    (appdir / "usr" / "bin" / "unio").chmod(0o755)

    # Desktop entry at AppDir root — appimagetool picks it up and
    # requires the Icon= value to match a PNG at the same level.
    shutil.copy2(DESKTOP_FILE, appdir / "unio.desktop")
    shutil.copy2(ICON_PNG, appdir / "unio.png")
    # Also place inside the standard icon-theme path so desktop
    # environments that install the AppImage can find the icon
    # without AppImage-specific magic.
    theme_icon_dir = appdir / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps"
    theme_icon_dir.mkdir(parents=True)
    shutil.copy2(ICON_PNG, theme_icon_dir / "unio.png")

    # AppRun — tiny shell shim appimagetool expects to find at the
    # AppDir root. It must be executable and `exec` the real binary
    # while forwarding all arguments.
    apprun = appdir / "AppRun"
    apprun.write_text(
        "#!/bin/sh\n"
        'HERE="$(dirname "$(readlink -f "$0")")"\n'
        'exec "${HERE}/usr/bin/unio" "$@"\n',
        encoding="utf-8",
    )
    apprun.chmod(0o755)

    output = DIST / "unIO-x86_64.AppImage"
    if output.exists():
        output.unlink()

    print(f"+ {tool} {appdir} {output}")
    # FUSE isn't available on most CI runners and some containers, so
    # force the extract-and-run path — appimagetool is itself an
    # AppImage, and this env var tells its runtime to unpack itself
    # into a tmp dir instead of mounting via FUSE. No-op elsewhere.
    env = {
        **os.environ,
        "APPIMAGE_EXTRACT_AND_RUN": "1",
        "ARCH": "x86_64",
    }
    rc = subprocess.call(
        [tool, str(appdir), str(output)],
        env=env,
    )
    if rc != 0:
        print(f"  (appimagetool exited {rc} — AppImage not produced)",
              file=sys.stderr)
        return

    output.chmod(0o755)
    print(f"  AppImage: {output.relative_to(ROOT)}")


if __name__ == "__main__":
    sys.exit(main())

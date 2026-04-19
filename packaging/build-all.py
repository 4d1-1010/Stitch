"""Build every supported platform in one command.

Runs `packaging/build.py` locally (Linux ELF + AppImage), then
`packaging/build-remote-win.py` against the Windows host over SSH
(EXE + _internal/ folder). macOS still has to come from the real Mac
— either wire a `build-remote-mac.py` twin or fall back to the CI
release path for that platform.

Usage:
    python packaging/build-all.py           # incremental
    python packaging/build-all.py --clean   # wipe build/dist on both

Env knobs for the Windows remote are shared with
`build-remote-win.py` (UNIO_WIN_SSH_HOST, _USER, _KEY, _REMOTE).
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
LINUX_BIN = REPO_ROOT / "dist" / "unio"


def _run(cmd: list[str]) -> None:
    print("\n==>", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, cwd=REPO_ROOT)


def kill_linux() -> None:
    """Stop any running copy of the freshly-built Linux binary so we
    can overwrite dist/unio. Harmless when nothing's running — pkill
    exits 1 in that case, which we swallow."""
    subprocess.run(["pkill", "-f", str(LINUX_BIN) + "$"], check=False)


def launch_linux() -> None:
    """Start the Linux binary detached from this shell so it survives
    the script exiting."""
    if not LINUX_BIN.exists():
        return
    env = os.environ.copy()
    # Pick the active X display — DISPLAY=:1 matches Adrian's setup;
    # fall back to whatever the invoking shell had.
    env.setdefault("DISPLAY", ":1")
    log = REPO_ROOT / "dist" / "unio-linux.log"
    with open(log, "ab") as f:
        subprocess.Popen(
            [str(LINUX_BIN)],
            stdout=f, stderr=f,
            start_new_session=True,
            env=env,
        )
    print(f"Linux binary launched → {LINUX_BIN} (log: {log})")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--clean", action="store_true",
                   help="Wipe build/dist on both platforms before building")
    p.add_argument("--skip-linux", action="store_true")
    p.add_argument("--skip-windows", action="store_true")
    p.add_argument("--bootstrap-windows", action="store_true",
                   help="Reinstall pip deps on the Windows remote")
    p.add_argument("--no-launch", action="store_true",
                   help="Skip starting the binaries after a successful "
                        "build (default behaviour launches both).")
    args = p.parse_args(argv)

    flags = ["--clean"] if args.clean else []

    if not args.skip_linux:
        kill_linux()
        _run([sys.executable, "packaging/build.py", *flags])

    if not args.skip_windows:
        win_cmd = [sys.executable, "packaging/build-remote-win.py", *flags]
        if args.bootstrap_windows:
            win_cmd.append("--bootstrap")
        if not args.no_launch:
            win_cmd.append("--launch")
        _run(win_cmd)

    print("\nBuild complete. Artifacts:")
    if not args.skip_linux:
        print("  Linux:   dist/unio, dist/unIO-x86_64.AppImage")
    if not args.skip_windows:
        print("  Windows: dist/win/unio/unio.exe + dist/win/unio/_internal/")

    if not args.no_launch and not args.skip_linux:
        launch_linux()

    return 0


if __name__ == "__main__":
    sys.exit(main())

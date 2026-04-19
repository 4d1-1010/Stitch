r"""Build the Windows binary on a remote Windows host over SSH.

Mirrors what `packaging/build.py` does locally, but for `windows-x64`
artifacts — streams the source tree to the remote box via `tar | ssh`,
runs PyInstaller there, streams `dist/unio` back. Faster than the
`windows-latest` CI runner and produces a honest native EXE.

Requirements on the remote:
  * Python 3.12 on PATH (`python --version`)
  * pip + pyinstaller + requirements.txt installed (this script will
    reinstall if you pass `--bootstrap` — useful after bumping deps)
  * Git for Windows (for its bundled `tar`, which we use both ways)

Env defaults can be overridden on the CLI:
  UNIO_WIN_SSH_HOST  →  --host    (default: 192.168.1.16)
  UNIO_WIN_SSH_USER  →  --user    (default: Diana)
  UNIO_WIN_SSH_KEY   →  --key     (default: ~/.ssh/id_ecdsa)
  UNIO_WIN_REMOTE    →  --remote  (default: %USERPROFILE%\unio)

Usage:
  python packaging/build-remote-win.py                # incremental
  python packaging/build-remote-win.py --bootstrap    # reinstall deps
  python packaging/build-remote-win.py --clean        # wipe build/dist
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

# Things that either don't belong on the remote at all, or that the
# remote build regenerates. Keeping the tarball lean makes each sync
# roundtrip quick even over slow WiFi.
SYNC_EXCLUDES = [
    ".git",
    "__pycache__",
    "*.pyc",
    "build",
    "dist",
    ".venv",
    ".mypy_cache",
    ".pytest_cache",
    ".claude",
]


def _ssh_args(args: argparse.Namespace) -> list[str]:
    return [
        "ssh",
        "-o", "IdentitiesOnly=yes",
        "-o", "BatchMode=yes",
        "-i", os.path.expanduser(args.key),
        f"{args.user}@{args.host}",
    ]


def _run(cmd: list[str], *, stdin=None, check: bool = True) -> int:
    print("+", " ".join(cmd), flush=True)
    result = subprocess.run(cmd, stdin=stdin, check=check)
    return result.returncode


def sync_source(args: argparse.Namespace) -> None:
    """Stream the working tree to the remote as a tar pipe.

    Runs the mkdir in a separate SSH call rather than chaining it with
    `&` in the piping shell — cmd.exe's `if not exist ... & tar -xzf -`
    has been seen to consume stdin before tar reads it, resulting in a
    partial archive and SIGPIPE on the local tar."""
    # 1. Ensure the target directory exists on the remote.
    _run(_ssh_args(args) + [
        f"if not exist {args.remote} mkdir {args.remote}",
    ])

    # 2. tar | ssh "cd /d <dst> & tar -xzf -" — only tar reads stdin.
    tar_cmd = ["tar", "-czf", "-"]
    for pat in SYNC_EXCLUDES:
        tar_cmd += ["--exclude", pat]
    tar_cmd += ["."]
    remote_cmd = f"cd /d {args.remote} & tar -xzf -"
    ssh_cmd = _ssh_args(args) + [remote_cmd]
    print("+", " ".join(tar_cmd), "|", " ".join(ssh_cmd), flush=True)
    with subprocess.Popen(tar_cmd, cwd=REPO_ROOT,
                          stdout=subprocess.PIPE) as tar:
        subprocess.check_call(ssh_cmd, stdin=tar.stdout)
        tar.stdout.close()
        if tar.wait() != 0:
            raise SystemExit("tar (local) failed")


def bootstrap(args: argparse.Namespace) -> None:
    """(Re)install pip deps + pyinstaller on the remote."""
    remote_cmd = (
        f"cd /d {args.remote} & "
        "python -m pip install --upgrade pip & "
        "python -m pip install -r requirements.txt pyinstaller"
    )
    _run(_ssh_args(args) + [remote_cmd])


def build(args: argparse.Namespace) -> None:
    flags = " --clean" if args.clean else ""
    remote_cmd = (
        f"cd /d {args.remote} & "
        f"python packaging\\build.py{flags}"
    )
    _run(_ssh_args(args) + [remote_cmd])


def pull_dist(args: argparse.Namespace) -> pathlib.Path:
    """Stream dist/unio back from the remote into dist/win/."""
    local_dst = REPO_ROOT / "dist" / "win"
    local_dst.mkdir(parents=True, exist_ok=True)
    remote_cmd = (
        f"cd /d {args.remote}\\dist & "
        "tar -czf - unio"
    )
    ssh_cmd = _ssh_args(args) + [remote_cmd]
    untar_cmd = ["tar", "-xzf", "-", "-C", str(local_dst)]
    print("+", " ".join(ssh_cmd), "|", " ".join(untar_cmd), flush=True)
    with subprocess.Popen(ssh_cmd, stdout=subprocess.PIPE) as ssh:
        subprocess.check_call(untar_cmd, stdin=ssh.stdout)
        ssh.stdout.close()
        if ssh.wait() != 0:
            raise SystemExit("ssh (remote tar) failed")
    exe = local_dst / "unio" / "unio.exe"
    if not exe.exists():
        raise SystemExit(f"expected {exe} to exist after pull")
    return exe


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--host", default=os.environ.get(
        "UNIO_WIN_SSH_HOST", "192.168.1.16"))
    p.add_argument("--user", default=os.environ.get(
        "UNIO_WIN_SSH_USER", "Diana"))
    p.add_argument("--key", default=os.environ.get(
        "UNIO_WIN_SSH_KEY", "~/.ssh/id_ecdsa"))
    p.add_argument("--remote", default=os.environ.get(
        "UNIO_WIN_REMOTE", r"%USERPROFILE%\unio"))
    p.add_argument("--bootstrap", action="store_true",
                   help="Reinstall pip deps + pyinstaller on the remote")
    p.add_argument("--clean", action="store_true",
                   help="Pass --clean to the remote build")
    p.add_argument("--skip-sync", action="store_true",
                   help="Skip the source push (e.g. when iterating on "
                        "only the build step)")
    args = p.parse_args(argv)

    if not args.skip_sync:
        sync_source(args)
    if args.bootstrap:
        bootstrap(args)
    build(args)
    exe = pull_dist(args)
    size_mb = exe.stat().st_size / (1024 * 1024)
    print(f"\nWindows EXE ready: {exe} ({size_mb:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

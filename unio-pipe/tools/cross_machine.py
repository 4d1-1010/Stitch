#!/usr/bin/env python3
"""Cross-machine end-to-end loopback between adi-pc (Linux) and
Diana (Windows). Companion to tools/loopback.py which handles the
single-host case.

Directions:
    lin2win    adi-pc src (VA-API / NVENC-Linux) → Diana sink (D3D11VA / NVDEC)
    win2lin    Diana src (NVENC)                 → adi-pc sink (VA-API / NVDEC)

Source + sink codec paths are selected by --src / --sink. The
script handles: killing stale helpers on both hosts, launching
fresh ones (Diana via ``schtasks /IT`` so WGC + DXGI get a real
user session), wiring them up over QUIC, running the stream for
--duration, tearing down, fetching the sink-side latency CSV
back to adi-pc for analysis.

Example runs:
    tools/cross_machine.py lin2win --sink d3d11va
    tools/cross_machine.py lin2win --sink nvdec --src nvenc-linux
    tools/cross_machine.py win2lin --sink vaapi
    tools/cross_machine.py win2lin --sink nvdec

Prerequisites:
  - Diana reachable over SSH with the ~/.ssh/id_ecdsa key
    (configure in ssh_config or pass --diana-host).
  - unio-pipe.exe already built on Diana at
    C:\\Users\\Diana\\unio\\unio-pipe\\build\\Release\\unio-pipe.exe
    (build via build-remote-win.py if missing).
  - Diana's Windows Firewall allows inbound on --port from adi-pc.
    For first-time runs, disable the firewall on the test LAN or
    add a rule for the unio-pipe.exe binary.

NTP caveat: cross-machine latency CSVs are only as tight as the
inter-host clock agreement. Run tools/sync-clocks.sh --win
before any measurement run that crosses clocks; see
unio-pipe/README.md latency-table section for the 490 ms skew
we hit against time.windows.com if the clocks aren't aligned.
"""

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

# ── Reuse helpers from loopback.py ──────────────────────────────

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from loopback import (  # noqa: E402
    rpc, kill_existing_helpers, spawn_helper, wait_for_socket,
    stop_stream, kill_helper, summarise_latency,
    SINK_SOCK, SRC_SOCK, SINK_LOG, SRC_LOG,
)

# ── Diana-side constants ────────────────────────────────────────

DEFAULT_DIANA_HOST = "Diana@192.168.1.18"
DIANA_KEY = os.path.expanduser("~/.ssh/id_ecdsa")
DIANA_EXE = r"C:\Users\Diana\unio\unio-pipe\build\Release\unio-pipe.exe"
DIANA_PIPE_SINK = r"unio-pipe-sink"
DIANA_PIPE_SRC  = r"unio-pipe-src"
DIANA_TASK_SINK = "unio-pipe-sink-task"
DIANA_TASK_SRC  = "unio-pipe-src-task"
DIANA_LOG_SINK  = r"C:\Users\Diana\unio-sink.log"
DIANA_LOG_SRC   = r"C:\Users\Diana\unio-src.log"
DIANA_CSV       = r"C:\Users\Diana\unio-lat.csv"
DIANA_DRIVER    = r"C:\Users\Diana\unio\unio-pipe\tools\drive_windows.ps1"

def ssh(cmd: str, host: str, check: bool = True, timeout: int = 30):
    """Run a cmd.exe command on Diana via SSH, return combined
    stdout+stderr. On check=True, non-zero raises."""
    full = ["ssh", "-o", "IdentitiesOnly=yes",
            "-o", "ConnectTimeout=5",
            "-i", DIANA_KEY, host, cmd]
    r = subprocess.run(full, capture_output=True, text=True,
                        timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(
            f"ssh failed ({r.returncode}): cmd={cmd!r}\n"
            f"stdout={r.stdout}\nstderr={r.stderr}"
        )
    return (r.stdout or "") + (r.stderr or "")


def diana_kill_all(host: str):
    ssh('taskkill /IM unio-pipe.exe /F 2>nul & del /F /Q C:\\Users\\Diana\\unio-sink.log '
        'C:\\Users\\Diana\\unio-src.log C:\\Users\\Diana\\unio-lat.csv 2>nul', host, check=False)
    for task in (DIANA_TASK_SINK, DIANA_TASK_SRC):
        ssh(f'schtasks /Delete /TN {task} /F 2>nul', host, check=False)


def diana_launch(host: str, role: str, pipe_name: str,
                 log_path: str, env: dict):
    """Launch unio-pipe.exe on Diana via schtasks /IT so it runs
    in the interactive user session (required for WGC + DXGI)."""
    task = DIANA_TASK_SINK if role == "sink" else DIANA_TASK_SRC
    # Build a single-line cmd that sets the env vars + redirects
    # stdout/stderr into the log.
    env_line = " & ".join(f'set {k}={v}' for k, v in env.items())
    tr = (
        f'cmd /c "'
        f'{env_line} && '
        f'{DIANA_EXE} --socket \\\\.\\pipe\\{pipe_name} '
        f'> {log_path} 2>&1"'
    )
    # Future date + /Z auto-deletes after the stop. /RL LIMITED
    # so we don't need UAC.
    ssh(f'schtasks /Create /TN {task} /TR "{tr}" /SC ONCE /ST 23:59 '
        f'/IT /RL LIMITED /F', host)
    ssh(f'schtasks /Run /TN {task}', host)


def diana_rpc(host: str, pipe_name: str, cmd: str, *args):
    """Drive Diana's unio-pipe via drive_windows.ps1 over SSH."""
    ps_args = " ".join(f'"{a}"' for a in args)
    return ssh(
        f'powershell -NoProfile -ExecutionPolicy Bypass -File '
        f'{DIANA_DRIVER} {pipe_name} {cmd} {ps_args}',
        host,
    ).strip()


# ── Direction-specific wiring ───────────────────────────────────


def run_lin2win(args, host: str) -> int:
    """adi-pc = src (VA-API / NVENC-Linux), Diana = sink."""
    print(f"[1/6] killing stale helpers on both hosts")
    kill_existing_helpers()
    diana_kill_all(host)

    print(f"[2/6] launching Diana sink (FORCE_DECODER={args.sink})")
    diana_launch(
        host, "sink", DIANA_PIPE_SINK, DIANA_LOG_SINK,
        {
            "UNIO_PIPE_FORCE_DECODER": args.sink,
            "UNIO_PIPE_LATENCY_CSV": DIANA_CSV,
        },
    )
    time.sleep(2.0)  # schtasks /Run is fire-and-forget

    print(f"[3/6] launching adi-pc src (FORCE_ENCODER={args.src})")
    src = spawn_helper(
        _exe(args), SRC_SOCK, SRC_LOG,
        {"UNIO_PIPE_FORCE_ENCODER": args.src},
    )
    if not wait_for_socket(SRC_SOCK):
        print("error: adi-pc src socket didn't appear", file=sys.stderr)
        diana_kill_all(host); return 3

    # adi-pc IP — Diana needs to dial back to us for start_outbound.
    # For lin2win the src dials Diana, so adi-pc's IP isn't needed
    # here; we use Diana's IP as the peer_addr.
    diana_ip = host.split("@")[-1]

    print(f"[4/6] wire up")
    r_in = diana_rpc(host, DIANA_PIPE_SINK, "in", args.port,
                      args.win_w, args.win_h, "cm1")
    print(f"  Diana start_inbound: {r_in}")
    time.sleep(0.3)
    r_out = rpc(SRC_SOCK, {
        "cmd": "start_outbound", "stream_id": "cm1",
        "peer_addr": diana_ip, "peer_port": args.port,
        "width": args.width, "height": args.height, "fps": args.fps,
        "capture_x": 0, "capture_y": 0, "monitor_source": ":1",
    })
    print(f"  adi-pc start_outbound: {r_out}")
    if "error" in r_out:
        diana_kill_all(host); kill_helper(src); return 4

    print(f"[5/6] streaming for {args.duration}s")
    time.sleep(args.duration)
    # status probes
    diana_status = diana_rpc(host, DIANA_PIPE_SINK, "status")
    print(f"  Diana status: {diana_status[:200]}")
    src_status = rpc(SRC_SOCK, {"cmd": "helper_status"})
    per = src_status.get("per_stream", [{}])[0]
    print(f"  adi-pc src: encoded={per.get('encoded')} "
          f"bytes={per.get('bytes_emitted')}")

    print(f"[6/6] teardown + fetch CSV")
    try:
        diana_rpc(host, DIANA_PIPE_SINK, "stop", "cm1")
    except Exception as e:
        print(f"  Diana stop: {e}", file=sys.stderr)
    stop_stream(SRC_SOCK, "cm1")
    kill_helper(src)
    diana_kill_all(host)

    local_csv = args.csv or f"/tmp/lat_cm_lin2win_{args.src}_{args.sink}.csv"
    subprocess.run([
        "scp", "-o", "IdentitiesOnly=yes", "-i", DIANA_KEY,
        f"{host}:{DIANA_CSV}", local_csv,
    ], check=False)
    summarise_latency(local_csv)
    return 0


def run_win2lin(args, host: str) -> int:
    """Diana = src (WGC + NVENC), adi-pc = sink (VA-API / NVDEC)."""
    print(f"[1/6] killing stale helpers on both hosts")
    kill_existing_helpers()
    diana_kill_all(host)

    # adi-pc IP seen from Diana. Figure out by asking the OS which
    # interface the Diana route uses.
    ip_cmd = subprocess.run(
        ["bash", "-c", f"ip -o route get {host.split('@')[-1]} "
                       "| awk '{for(i=1;i<=NF;i++)if($i==\"src\")print $(i+1)}'"],
        capture_output=True, text=True,
    )
    adi_ip = ip_cmd.stdout.strip()
    if not adi_ip:
        print("error: could not determine adi-pc IP visible to Diana",
              file=sys.stderr); return 2

    print(f"[2/6] launching adi-pc sink (FORCE_DECODER={args.sink})")
    sink = spawn_helper(
        _exe(args), SINK_SOCK, SINK_LOG,
        {
            "UNIO_PIPE_FORCE_DECODER": args.sink,
            "UNIO_PIPE_LATENCY_CSV": args.csv or
                f"/tmp/lat_cm_win2lin_{args.src}_{args.sink}.csv",
        },
    )
    if not wait_for_socket(SINK_SOCK):
        print("error: adi-pc sink socket didn't appear", file=sys.stderr)
        diana_kill_all(host); return 3

    print(f"[3/6] launching Diana src")
    # Windows encoder-forcing isn't wired yet (commit 7 added
    # Linux UNIO_PIPE_FORCE_ENCODER; Windows side is the default
    # NVENC). When WP 10 Part D / E (AMF / oneVPL) lands we'll
    # add a Windows force-hook parallel. For now args.src is
    # informational on the Windows side.
    diana_launch(host, "src", DIANA_PIPE_SRC, DIANA_LOG_SRC, {})
    time.sleep(2.0)

    print(f"[4/6] wire up")
    r_in = rpc(SINK_SOCK, {
        "cmd": "start_inbound", "stream_id": "cm1",
        "listen_port": args.port,
        "window_w": args.win_w, "window_h": args.win_h,
    })
    print(f"  adi-pc start_inbound: {r_in}")
    time.sleep(0.3)
    r_out = diana_rpc(host, DIANA_PIPE_SRC, "out", adi_ip, args.port,
                       args.width, args.height, "cm1")
    print(f"  Diana start_outbound: {r_out}")

    print(f"[5/6] streaming for {args.duration}s")
    time.sleep(args.duration)
    sink_status = rpc(SINK_SOCK, {"cmd": "helper_status"})
    per = sink_status.get("per_stream", [{}])[0]
    print(f"  adi-pc sink: decoder={per.get('decoder')} "
          f"decoded={per.get('frames_decoded')} "
          f"presented={per.get('frames_presented')}")

    print(f"[6/6] teardown")
    try:
        diana_rpc(host, DIANA_PIPE_SRC, "stop", "cm1")
    except Exception as e:
        print(f"  Diana stop: {e}", file=sys.stderr)
    stop_stream(SINK_SOCK, "cm1")
    kill_helper(sink)
    diana_kill_all(host)

    csv_path = args.csv or \
        f"/tmp/lat_cm_win2lin_{args.src}_{args.sink}.csv"
    summarise_latency(csv_path)
    return 0


def _exe(args) -> str:
    here = Path(__file__).resolve().parent
    repo_root = here.parent.parent
    exe = args.exe or str(
        repo_root / "unio-pipe" / "build" / "unio-pipe"
    )
    if not os.path.isfile(exe):
        raise FileNotFoundError(
            f"unio-pipe binary not found at {exe}"
        )
    return exe


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("direction", choices=("lin2win", "win2lin"))
    ap.add_argument("--src", default="vaapi",
                    choices=("vaapi", "nvenc-linux", "nvenc"),
                    help="(lin2win) force encoder on adi-pc")
    ap.add_argument("--sink", default=None,
                    help="force decoder: lin2win → d3d11va/nvdec; "
                          "win2lin → vaapi/nvdec. "
                          "Default matches the default per-OS.")
    ap.add_argument("--duration", type=float, default=15.0)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--port", type=int, default=5099)
    ap.add_argument("--win-w", type=int, default=320)
    ap.add_argument("--win-h", type=int, default=180)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--exe", default=None)
    ap.add_argument("--diana-host", default=DEFAULT_DIANA_HOST)
    args = ap.parse_args()

    # Per-direction sink default
    if not args.sink:
        args.sink = ("d3d11va" if args.direction == "lin2win"
                     else "vaapi")

    print("=" * 60)
    print(f"cross-machine loopback: {args.direction}")
    print(f"  src={args.src}  sink={args.sink}")
    print(f"  {args.width}x{args.height}@{args.fps}  {args.duration}s")
    print(f"  diana={args.diana_host}")
    print("=" * 60)

    if args.direction == "lin2win":
        return run_lin2win(args, args.diana_host)
    else:
        return run_win2lin(args, args.diana_host)


if __name__ == "__main__":
    sys.exit(main())

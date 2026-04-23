#!/usr/bin/env python3
"""Cross-machine end-to-end loopback between adi-pc (Linux) and
Diana (Windows). Companion to tools/loopback.py which handles the
single-host case.

Directions:
    lin2win    adi-pc src (VA-API / NVENC-Linux) → Diana sink (D3D11VA / NVDEC)
    win2lin    Diana src (NVENC / oneVPL)         → adi-pc sink (VA-API / NVDEC)

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
    tools/cross_machine.py win2lin --src onevpl --sink vaapi   # WP 10 Intel #26

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
# scp on the OpenSSH-Windows side needs forward slashes; backslashes
# get interpreted as shell escapes somewhere in the transport.
DIANA_CSV_SCP   = "C:/Users/Diana/unio-lat.csv"
DIANA_DRIVER    = r"C:\Users\Diana\unio\unio-pipe\tools\drive_windows.ps1"

def ssh(cmd: str, host: str, check: bool = True, timeout: int = 30):
    """Run a cmd.exe command on Diana via SSH, return combined
    stdout+stderr. On check=True, non-zero raises.

    Windows' default console codepage is CP1252-ish, not UTF-8.
    We read bytes + decode with errors='replace' so stray
    high-bit chars (Windows logo / NBSP / smart quotes in some
    error messages) don't crash the caller."""
    full = ["ssh", "-o", "IdentitiesOnly=yes",
            "-o", "ConnectTimeout=5",
            "-i", DIANA_KEY, host, cmd]
    r = subprocess.run(full, capture_output=True, timeout=timeout)
    out = r.stdout.decode("utf-8", errors="replace") if r.stdout else ""
    err = r.stderr.decode("utf-8", errors="replace") if r.stderr else ""
    if check and r.returncode != 0:
        raise RuntimeError(
            f"ssh failed ({r.returncode}): cmd={cmd!r}\n"
            f"stdout={out}\nstderr={err}"
        )
    return out + err


def diana_kill_all(host: str):
    ssh('taskkill /IM unio-pipe.exe /F 2>nul & del /F /Q C:\\Users\\Diana\\unio-sink.log '
        'C:\\Users\\Diana\\unio-src.log C:\\Users\\Diana\\unio-lat.csv 2>nul', host, check=False)
    for task in (DIANA_TASK_SINK, DIANA_TASK_SRC):
        ssh(f'schtasks /Delete /TN {task} /F 2>nul', host, check=False)


def diana_launch(host: str, role: str, pipe_name: str,
                 log_path: str, env: dict):
    """Launch unio-pipe.exe on Diana by writing a .cmd file to
    disk + running it through schtasks. The .cmd-file approach
    sidesteps the quoting nightmare of nested cmd / PowerShell /
    ssh / schtasks double-quotes.

      role="sink" — schtasks /Run without /IT is sufficient.
        D3D11VA decoder runs fine in Session 0; DXGI presenter
        fails ("inbound runs headless") but that's non-fatal for
        measurement — the latency CSV is written from the decode
        callback, before the presenter would paint.

      role="src"  — schtasks /IT. WGC capture needs a logged-on
        user session to grab the real desktop.
    """
    # Build the .cmd body.
    set_lines = [f'set "{k}={v}"' for k, v in env.items()]
    launch = (f'{DIANA_EXE} --socket \\\\.\\pipe\\{pipe_name} '
              f'> {log_path} 2>&1')
    cmd_body = "@echo off\r\n"
    for ln in set_lines:
        cmd_body += ln + "\r\n"
    cmd_body += launch + "\r\n"

    cmd_path = (r"C:\Users\Diana\unio-pipe-sink.cmd" if role == "sink"
                else r"C:\Users\Diana\unio-pipe-src.cmd")
    task = DIANA_TASK_SINK if role == "sink" else DIANA_TASK_SRC
    it_flag = "" if role == "sink" else "/IT"

    # Ship the .cmd over via base64 — safer than trying to quote
    # newlines through cmd.exe / ssh.
    import base64
    b64 = base64.b64encode(cmd_body.encode("utf-8")).decode("ascii")
    ssh(
        f"powershell -NoProfile -Command "
        f"\"[IO.File]::WriteAllBytes('{cmd_path}',"
        f"[Convert]::FromBase64String('{b64}'))\"",
        host,
    )
    ssh(
        f'schtasks /Create /TN {task} /TR "{cmd_path}" '
        f'/SC ONCE /ST 23:59 {it_flag} /RL LIMITED /F',
        host,
    )
    ssh(f"schtasks /Run /TN {task}", host)


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
    # Stop the streams first so the CSV stops being written to,
    # then fetch BEFORE the diana_kill_all wipes
    # C:\Users\Diana\unio-lat.csv. Bug caught on first run —
    # kill_all was deleting the CSV out from under scp.
    try:
        diana_rpc(host, DIANA_PIPE_SINK, "stop", "cm1")
    except Exception as e:
        print(f"  Diana stop: {e}", file=sys.stderr)
    stop_stream(SRC_SOCK, "cm1")

    local_csv = args.csv or f"/tmp/lat_cm_lin2win_{args.src}_{args.sink}.csv"
    scp_rc = subprocess.run([
        "scp", "-o", "IdentitiesOnly=yes", "-i", DIANA_KEY,
        f"{host}:{DIANA_CSV_SCP}", local_csv,
    ], capture_output=True).returncode
    if scp_rc != 0:
        print(f"  scp returncode={scp_rc} (CSV may not exist)",
              file=sys.stderr)

    kill_helper(src)
    diana_kill_all(host)

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

    # Windows encoder selection via UNIO_PIPE_FORCE_ENCODER. Maps
    # the user-facing --src name to what Diana's stream_manager
    # expects. "nvenc" and "onevpl" are the two wired paths
    # (#26 = oneVPL, existing NVENC default). Passing an empty
    # dict falls through to NVENC (the default on Windows).
    WIN_ENCODER_MAP = {
        "nvenc":  "nvenc",
        "onevpl": "onevpl",
    }
    diana_src_env: dict = {}
    if args.src in WIN_ENCODER_MAP:
        diana_src_env["UNIO_PIPE_FORCE_ENCODER"] = WIN_ENCODER_MAP[args.src]
    print(f"[3/6] launching Diana src (encoder={args.src},"
          f" env={diana_src_env or 'default-NVENC'})")
    diana_launch(host, "src", DIANA_PIPE_SRC, DIANA_LOG_SRC,
                 diana_src_env)
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
    # win2lin sink is on adi-pc so the CSV is local — no scp
    # needed. Stop cleanly + tear everything down + summarise.
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
                    choices=("vaapi", "nvenc-linux", "nvenc",
                             "onevpl"),
                    help="(lin2win) force encoder on adi-pc. "
                         "(win2lin) force encoder on Diana: "
                         "nvenc = NVENC, onevpl = Intel oneVPL")
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

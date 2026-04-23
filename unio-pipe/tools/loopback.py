#!/usr/bin/env python3
"""End-to-end loopback runner for ``unio-pipe`` on Linux.

Takes care of the full lifecycle that's otherwise six manual shell
commands: kill stale helpers, wipe stale sockets, spawn a fresh
sink + src with the requested force-env vars, send
start_inbound + start_outbound, run for N seconds, stop cleanly,
tear everything down, and print p50 / p95 / max latency from the
CSV.

Typical usage (defaults match the WP 10 Linux-NVIDIA loopback):

    tools/loopback.py --src vaapi --sink vaapi
    tools/loopback.py --src nvenc-linux --sink nvdec
    tools/loopback.py --src vaapi --sink nvdec --duration 30 --csv /tmp/run.csv

Prerequisites:
  - Helper built at ``unio-pipe/build/unio-pipe`` (or pass --exe).
  - DISPLAY + XAUTHORITY set correctly (script will auto-detect).
  - Nothing else listening on the chosen UDS names or port.

Flags:
  --src       Force encoder: vaapi / nvenc-linux. Default: vaapi.
  --sink      Force decoder: vaapi / nvdec. Default: vaapi.
  --duration  Seconds to keep the stream flowing. Default: 15.
  --width     Source capture width. Default: 1920.
  --height    Source capture height. Default: 1080.
  --fps       Stream fps hint. Default: 30.
  --port      Loopback QUIC port. Default: 5099.
  --win-w     Sink preview window width. Default: 320.
  --win-h     Sink preview window height. Default: 180.
  --csv       Path for the latency CSV (auto-parsed at end). Default: /tmp/lat_<src>_<sink>.csv.
  --exe       Path to the unio-pipe binary. Default: <repo>/unio-pipe/build/unio-pipe.
  --keep-alive  Don't tear down at the end; leave helpers running for manual inspection.
"""

import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

SINK_SOCK = "/tmp/unio-pipe-sink.sock"
SRC_SOCK  = "/tmp/unio-pipe-src.sock"
SINK_LOG  = "/tmp/unio-sink.log"
SRC_LOG   = "/tmp/unio-src.log"

# ── RPC ──────────────────────────────────────────────────────────


def rpc(sock_path: str, cmd: dict) -> dict:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    try:
        body = json.dumps(cmd).encode()
        s.sendall(struct.pack("<I", len(body)) + body)
        raw = _recv_exact(s, 4)
        n = struct.unpack("<I", raw)[0]
        return json.loads(_recv_exact(s, n).decode())
    finally:
        s.close()


def _recv_exact(s, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError(f"short read ({len(buf)}/{n})")
        buf += chunk
    return buf


# ── Lifecycle ────────────────────────────────────────────────────


def kill_existing_helpers():
    """Force-kill any unio-pipe --socket processes + remove the
    socket files the scripts re-use. Idempotent; safe to run
    even when nothing matches."""
    subprocess.run(
        ["pkill", "-9", "-f", "unio-pipe --socket"],
        stderr=subprocess.DEVNULL, check=False,
    )
    time.sleep(0.3)
    for p in (SINK_SOCK, SRC_SOCK, SINK_LOG, SRC_LOG):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


def detect_session_env() -> dict:
    """Return the subset of environment variables the helper needs
    for session-aware capture / presenter init, picked up from
    the caller's environment with sensible defaults for a
    graphical-login shell. Handles both X11 and Wayland sessions
    — essential now that Btrc4t's Wayland capture (#7) and
    EGL-Wayland presenter (#30) are in flight.

    On X11: DISPLAY + XAUTHORITY are the minimum. Default to
    :1 / the GDM keyring path since that's what adi-pc ships.

    On Wayland: WAYLAND_DISPLAY + XDG_SESSION_TYPE +
    XDG_RUNTIME_DIR + DBUS_SESSION_BUS_ADDRESS. Also pass DISPLAY
    + XAUTHORITY if present (for XWayland fallback). The
    capability probe inside the helper uses XDG_SESSION_TYPE to
    decide between x11 and wayland paths.
    """
    env = {}
    # X11 bits (harmless on a pure Wayland session — probe just
    # picks wayland if WAYLAND_DISPLAY is also set).
    env["DISPLAY"] = os.environ.get("DISPLAY", ":1")
    env["XAUTHORITY"] = os.environ.get(
        "XAUTHORITY", "/run/user/1000/gdm/Xauthority"
    )
    # Wayland bits — copy through when present.
    for k in (
        "WAYLAND_DISPLAY",
        "XDG_SESSION_TYPE",
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
    ):
        if k in os.environ:
            env[k] = os.environ[k]
    return env


def spawn_helper(exe: str, socket_path: str, log_path: str,
                 env_overrides: dict) -> subprocess.Popen:
    env = os.environ.copy()
    env.update(detect_session_env())
    env.update(env_overrides)
    log = open(log_path, "w")
    p = subprocess.Popen(
        [exe, "--socket", socket_path],
        stdin=subprocess.DEVNULL, stdout=log, stderr=log,
        env=env, start_new_session=True,
    )
    return p


def wait_for_socket(path: str, timeout_s: float = 3.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    return False


def stop_stream(sock: str, stream_id: str):
    try:
        rpc(sock, {"cmd": "stop", "stream_id": stream_id})
    except Exception as e:
        print(f"  stop({stream_id}) on {sock}: {e}", file=sys.stderr)


def kill_helper(p: subprocess.Popen):
    if p.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        p.wait(timeout=2)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        p.wait(timeout=1)


# ── Latency stats ───────────────────────────────────────────────


def summarise_latency(csv_path: str, warmup: int = 60):
    try:
        from csv import DictReader
        import statistics
    except ImportError:
        return
    try:
        rows = list(DictReader(open(csv_path)))
    except FileNotFoundError:
        print(f"  (no CSV at {csv_path})", file=sys.stderr)
        return
    print(f"\nLatency stats ({csv_path}, warmup {warmup} dropped):")
    print(f"  rows: {len(rows)}")
    for key in ("capture_to_decode_us", "decode_to_present_us",
                "capture_to_present_us"):
        vals = sorted(
            int(r[key]) for r in rows if int(r[key]) < 1_000_000_000
        )
        if len(vals) <= warmup:
            continue
        warm = vals[warmup:]
        n = len(warm)
        p50 = warm[n // 2]
        p95 = warm[int(n * 0.95)]
        mx = warm[-1]
        mean = statistics.mean(warm)
        print(
            f"    {key:30s} n={n:5d}  "
            f"p50={p50/1000:7.2f}ms  "
            f"p95={p95/1000:7.2f}ms  "
            f"max={mx/1000:7.2f}ms  "
            f"mean={mean/1000:7.2f}ms"
        )


# ── Main ────────────────────────────────────────────────────────


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--src", default="vaapi",
                    choices=("vaapi", "nvenc-linux", "nvenc",
                             "onevpl"),
                    help="force encoder (src side). "
                         "onevpl = Windows Intel oneVPL path (#26)")
    ap.add_argument("--sink", default="vaapi",
                    choices=("vaapi", "nvdec", "d3d11va"),
                    help="force decoder (sink side). "
                         "d3d11va = Windows D3D11VA path")
    # Wayland hook — the helper doesn't yet have a
    # UNIO_PIPE_FORCE_CAPTURE knob (Btrc4t's #7 will add it when
    # the PipeWire capture lands). When the knob exists, passing
    # --capture wayland-pipewire flows through as an env var; when
    # the knob isn't wired yet the helper auto-selects based on
    # XDG_SESSION_TYPE. Until #7 merges, --capture is a no-op
    # hint for us and a scaffold for the future flow.
    ap.add_argument("--capture", default=None,
                    choices=("xcomposite", "wayland-pipewire"),
                    help="force capture backend (requires #7 to "
                          "ship UNIO_PIPE_FORCE_CAPTURE; until "
                          "then the helper auto-selects from "
                          "XDG_SESSION_TYPE)")
    # Presenter force hook — mirror of --capture. Waits on #30
    # (EGL-Wayland presenter, Btrc4t).
    ap.add_argument("--presenter", default=None,
                    choices=("egl-x11", "egl-wayland", "dxgi-flip"),
                    help="force presenter backend (requires "
                          "UNIO_PIPE_FORCE_PRESENTER to ship; "
                          "until then the helper auto-selects)")
    ap.add_argument("--duration", type=float, default=15.0,
                    help="seconds to stream")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--port", type=int, default=5099,
                    help="loopback QUIC port")
    ap.add_argument("--win-w", type=int, default=320,
                    help="sink preview window width")
    ap.add_argument("--win-h", type=int, default=180,
                    help="sink preview window height")
    ap.add_argument("--csv", default=None,
                    help="latency CSV path (default: /tmp/lat_<src>_<sink>.csv)")
    ap.add_argument("--exe", default=None,
                    help="unio-pipe binary path")
    ap.add_argument("--keep-alive", action="store_true",
                    help="don't tear down at the end")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent
    repo_root = here.parent.parent
    exe = args.exe or str(repo_root / "unio-pipe" / "build" / "unio-pipe")
    if not os.path.isfile(exe):
        print(f"error: unio-pipe binary not found at {exe}\n"
              f"  build with: cmake --build {repo_root}/unio-pipe/build",
              file=sys.stderr)
        return 2

    csv_path = args.csv or f"/tmp/lat_{args.src}_{args.sink}.csv"
    sid = "loop1"

    print("=" * 60)
    print(f"unio-pipe loopback: {args.src} → {args.sink}")
    print(f"  {args.width}x{args.height}@{args.fps}  {args.duration}s")
    print(f"  csv: {csv_path}")
    print(f"  exe: {exe}")
    print("=" * 60)

    print("\n[1/6] kill stale helpers + clean sockets")
    kill_existing_helpers()
    try:
        os.unlink(csv_path)
    except FileNotFoundError:
        pass

    sink_env = {
        "UNIO_PIPE_FORCE_DECODER": args.sink,
        "UNIO_PIPE_LATENCY_CSV": csv_path,
    }
    src_env = {"UNIO_PIPE_FORCE_ENCODER": args.src}
    if args.capture:
        # Forward-compat scaffold: when Btrc4t's #7 lands the
        # helper will honour UNIO_PIPE_FORCE_CAPTURE. Until then
        # it's a no-op tolerated by getenv — costs nothing.
        src_env["UNIO_PIPE_FORCE_CAPTURE"] = args.capture
    if args.presenter:
        sink_env["UNIO_PIPE_FORCE_PRESENTER"] = args.presenter

    # Log the session type we picked — useful for debugging
    # "why is Wayland capture not selected" on a mixed session.
    session_env = detect_session_env()
    print(
        f"  session: XDG_SESSION_TYPE="
        f"{session_env.get('XDG_SESSION_TYPE', '<unset>')}"
        f" DISPLAY={session_env.get('DISPLAY')}"
        f" WAYLAND_DISPLAY="
        f"{session_env.get('WAYLAND_DISPLAY', '<unset>')}"
    )

    print(f"\n[2/6] spawn sink (FORCE_DECODER={args.sink}"
          f"{'; FORCE_PRESENTER=' + args.presenter if args.presenter else ''})")
    sink = spawn_helper(exe, SINK_SOCK, SINK_LOG, sink_env)
    print(f"\n[3/6] spawn src (FORCE_ENCODER={args.src}"
          f"{'; FORCE_CAPTURE=' + args.capture if args.capture else ''})")
    src = spawn_helper(exe, SRC_SOCK, SRC_LOG, src_env)

    if not wait_for_socket(SINK_SOCK) or not wait_for_socket(SRC_SOCK):
        print("error: helper socket didn't appear in 3s. Tail logs:",
              file=sys.stderr)
        for log in (SINK_LOG, SRC_LOG):
            print(f"--- {log} ---", file=sys.stderr)
            try:
                print(open(log).read(), file=sys.stderr)
            except Exception:
                pass
        kill_helper(sink); kill_helper(src)
        return 3

    print("\n[4/6] wire up: start_inbound (sink) + start_outbound (src)")
    r_in = rpc(SINK_SOCK, {
        "cmd": "start_inbound", "stream_id": sid,
        "listen_port": args.port,
        "window_w": args.win_w, "window_h": args.win_h,
    })
    print(f"  start_inbound: {r_in}")
    if "error" in r_in:
        kill_helper(sink); kill_helper(src); return 4
    time.sleep(0.3)
    r_out = rpc(SRC_SOCK, {
        "cmd": "start_outbound", "stream_id": sid,
        "peer_addr": "127.0.0.1", "peer_port": args.port,
        "width": args.width, "height": args.height, "fps": args.fps,
        "capture_x": 0, "capture_y": 0,
        "monitor_source": ":1",
    })
    print(f"  start_outbound: {r_out}")
    if "error" in r_out:
        kill_helper(sink); kill_helper(src); return 4

    print(f"\n[5/6] streaming for {args.duration}s ...")
    deadline = time.monotonic() + args.duration
    while time.monotonic() < deadline:
        time.sleep(min(5.0, deadline - time.monotonic()))

    status = rpc(SINK_SOCK, {"cmd": "helper_status"})
    per = status.get("per_stream", [{}])[0]
    print(
        f"  sink: decoder={per.get('decoder')} "
        f"decoded={per.get('frames_decoded')} "
        f"presented={per.get('frames_presented')}"
    )
    status = rpc(SRC_SOCK, {"cmd": "helper_status"})
    per = status.get("per_stream", [{}])[0]
    print(
        f"  src:  encoder={per.get('encoder')} "
        f"encoded={per.get('encoded')} "
        f"bytes={per.get('bytes_emitted')}"
    )

    if args.keep_alive:
        print("\n[6/6] --keep-alive: leaving helpers running")
        summarise_latency(csv_path)
        return 0

    print("\n[6/6] teardown")
    stop_stream(SRC_SOCK, sid)
    stop_stream(SINK_SOCK, sid)
    kill_helper(sink)
    kill_helper(src)
    kill_existing_helpers()  # belt-and-braces

    summarise_latency(csv_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())

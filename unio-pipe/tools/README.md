# `unio-pipe/tools/`

Dev-time scripts for driving the helper over its control socket and running
measurement experiments. None of these ship with the user-facing binary —
they're intended for interactive sessions (`bash` / `pwsh`) and for scripted
latency runs against a freshly-built `unio-pipe` on `adi-pc` or `Diana`.

## Inventory

| Tool | OS | Purpose |
|---|---|---|
| `loopback.py` | Linux | **One-shot end-to-end loopback runner**. Handles kill-stale + spawn-fresh + wire-up + run + teardown + stats. First tool to reach for when measuring a codec pair. |
| `cross_machine.py` | Linux + SSH to Windows | **Cross-machine end-to-end runner**. Linux↔Windows via SSH + `schtasks /IT` on Diana. Companion to `loopback.py` for the same-host case. |
| `drive_linux.py` | Linux | JSON-RPC client for the Unix-domain-socket control channel. Raw, lower-level — use when you want a single RPC, not a full run. |
| `drive_windows.ps1` | Windows | JSON-RPC client for the named-pipe control channel. Same verbs as `drive_linux.py`. |
| `latency_stats.py` | Any | Parses a `UNIO_PIPE_LATENCY_CSV` produced by the sink and prints p50 / p95 / max / mean for each phase. Standalone — `loopback.py` and `cross_machine.py` call it automatically after their runs. |
| `kill_all.sh` | Linux | Emergency: force-kill every `unio-pipe --socket` helper + wipe sockets / logs. Use when a prior run's teardown didn't land. |
| `sync-clocks.sh` | Linux (+ Windows via SSH) | Forces an NTP resync on one or both hosts so latency-CSV timestamps line up across machines. |

The helper binary itself ships in `unio-pipe/build/unio-pipe` (Linux) or
`unio-pipe\build\Release\unio-pipe.exe` (Windows); these scripts assume
you've already built and launched it.

## Shared concepts

- **Control socket**: the helper listens on a platform-specific IPC
  endpoint passed via `--socket <path>`:
  - Linux: `--socket /tmp/unio-pipe-sink.sock` → AF_UNIX
  - Windows: `--socket \\.\pipe\unio-pipe-sink` → named pipe
- **Wire format**: length-prefixed JSON. 4-byte little-endian length, then
  UTF-8 JSON body. Every RPC returns `{"error": "..."}` on failure or the
  natural shape on success.
- **Latency CSV**: set `UNIO_PIPE_LATENCY_CSV=/path` in the *sink* helper's
  environment before launch. Each decoded frame appends a row:
  `frame_id,capture_ns,decode_done_ns,present_done_ns,width,height,capture_to_decode_us,decode_to_present_us,capture_to_present_us`.
  See `src/latency_log.cpp` for the emitter + the README latency-table
  section for measurement caveats.
- **Force-path env vars** (testing knobs — not for production use):
  - `UNIO_PIPE_FORCE_ENCODER=vaapi` / `nvenc-linux` / `onevpl` (Windows Intel, #26)
  - `UNIO_PIPE_FORCE_DECODER=vaapi` / `nvdec` / `d3d11va`
  - `UNIO_PIPE_FORCE_NO_STREAMING=1` — forces the refusal path for UI
    testing on a host that actually has a hardware encoder
  - `UNIO_PIPE_DISABLE_PROBE=1` — skips the runtime capability probe,
    returns hardwired per-OS defaults

## `loopback.py` — the one-shot Linux runner

**Use this first.** It handles the full lifecycle (kill stale → spawn
fresh → start_inbound + start_outbound → stream → stop → teardown →
latency stats) so you don't have to remember six shell commands. Every
example in the rest of this README uses it.

```
tools/loopback.py [options]

Common options:
  --src {vaapi,nvenc-linux,nvenc,onevpl}    Force encoder (src side)
  --sink {vaapi,nvdec,d3d11va}              Force decoder (sink side)
  --capture {xcomposite,wayland-pipewire}   (forward-compat; see below)
  --presenter {egl-x11,egl-wayland}  (forward-compat; see below)
  --duration SECONDS                 Default 15
  --width / --height / --fps         Default 1920x1080@30
  --csv PATH                         Default /tmp/lat_<src>_<sink>.csv
  --keep-alive                       Don't tear down at the end
  --exe PATH                         Default <repo>/unio-pipe/build/unio-pipe
```

Live examples (actually ran on adi-pc during the WP 10 NVIDIA-Linux
work; numbers come out of the CSV summary the script prints):

```bash
# Pure Linux X11 + Intel VA-API loopback (shipped row)
tools/loopback.py
#   capture → present  p50 16.29 ms

# Linux X11 + NVIDIA — full NVENC-Linux → NVDEC (just landed)
tools/loopback.py --src nvenc-linux --sink nvdec
#   capture → present  p50 11.01 ms   (beats Intel baseline by ~5 ms)

# Mixed — Intel VA-API encode, NVIDIA NVDEC decode on the same host
tools/loopback.py --src vaapi --sink nvdec
#   capture → present  p50 16.44 ms

# Simulate a no-encoder refusal with --sink <junk> (falls back to vaapi)
tools/loopback.py --sink vaapi --duration 5
```

### Wayland prep (Btrc4t's track)

`loopback.py` auto-detects session type by passing through
`XDG_SESSION_TYPE` / `WAYLAND_DISPLAY` / `XDG_RUNTIME_DIR` /
`DBUS_SESSION_BUS_ADDRESS` when they're set in the caller's
environment. On a Wayland adi-pc session it will just work —
the helper's capability probe picks `wayland-pipewire` capture
and `egl-wayland` presenter over the X11 equivalents.

Two forward-compat flags are in place ahead of #7 (PipeWire
capture) and #30 (EGL-Wayland presenter):

  - `--capture {xcomposite|wayland-pipewire}` — sets
    `UNIO_PIPE_FORCE_CAPTURE` in the src's environment.
  - `--presenter {egl-x11|egl-wayland|dxgi-flip}` — sets
    `UNIO_PIPE_FORCE_PRESENTER` in the sink's environment.

These env vars aren't wired in the helper yet; until Btrc4t
adds `UNIO_PIPE_FORCE_*` handling for capture + presenter, the
flags are no-ops the helper silently ignores. When those hooks
ship, the flags start taking effect without any change to the
test scripts.

For measuring inside a Wayland session today (before #7 / #30):

```bash
# Wayland login shell — session type auto-detected
tools/loopback.py --src nvenc-linux --sink nvdec
#   (helper picks Wayland detection bits; actual capture path
#    still waits on #7. On current builds this falls back to X11
#    via XWayland if DISPLAY is also set.)
```

## `cross_machine.py` — Linux ↔ Windows runner

Mirror of `loopback.py` for the cross-machine case. One direction
per invocation:

```
tools/cross_machine.py lin2win --src vaapi --sink d3d11va
tools/cross_machine.py lin2win --src nvenc-linux --sink nvdec
tools/cross_machine.py win2lin --sink vaapi
tools/cross_machine.py win2lin --sink nvdec
tools/cross_machine.py win2lin --src onevpl --sink vaapi   # WP 10 Intel #26
```

- `lin2win`: adi-pc is the src (VA-API or NVENC-Linux), Diana is the sink
  (D3D11VA or NVDEC). Diana's sink launches via `schtasks /IT` so DXGI
  gets a user desktop.
- `win2lin`: Diana is the src (WGC + NVENC or oneVPL), adi-pc is the sink.
  Diana's src runs via `schtasks /IT` so WGC captures the actual desktop.
  Pass `--src onevpl` to exercise the Intel oneVPL encoder path on Diana.

After the run the sink-side latency CSV is scp'd back to adi-pc and
`latency_stats.py` prints p50 / p95 / max.

**Prerequisites**:
- Diana reachable via `Diana@192.168.1.18` using `~/.ssh/id_ecdsa`
  (override with `--diana-host`).
- `unio-pipe.exe` built on Diana at
  `C:\Users\Diana\unio\unio-pipe\build\Release\unio-pipe.exe`.
- `unio-pipe/tools/drive_windows.ps1` present on Diana at
  `C:\Users\Diana\unio\unio-pipe\tools\drive_windows.ps1` (sync the
  repo there via `packaging/build-remote-win.py` or manual
  `tar | ssh` if the tools folder is missing).
- Windows Firewall allows inbound on `--port` (default 5099) from the
  other side.
- Clocks synced: `tools/sync-clocks.sh --win Diana@192.168.1.18`.

## `drive_linux.py` and `drive_windows.ps1` — raw RPC clients

When you want to drive a *single* RPC against an already-running
helper (e.g. `helper_caps`, `helper_status`, `request_idr`) without
spawning / tearing down, use these.

```
drive_linux.py <socket-path> <subcommand> [args...]

subcommands:
  caps                                              — helper_caps RPC (JSON)
  status                                            — helper_status RPC (JSON)
  in   <listen_port> <win_w> <win_h> <stream_id>    — start_inbound
  out  <peer_addr> <peer_port> <w> <h> <stream_id>  — start_outbound
       [capture_x] [capture_y]
  stop <stream_id>
```

`drive_windows.ps1` takes the same verbs + args, plus an `idr <sid>`
for `request_idr`. First arg is the bare pipe name (no `\\.\pipe\`
prefix — that's added automatically).

## `drive_windows.ps1`

Same subcommand shape as `drive_linux.py`. The first arg is the bare
pipe name (without the `\\.\pipe\` prefix — that's added automatically).

```powershell
# Launch sink
$env:UNIO_PIPE_LATENCY_CSV = "C:\Users\Diana\lat.csv"
Start-Process -FilePath "unio-pipe\build\Release\unio-pipe.exe" `
              -ArgumentList "--socket \\.\pipe\unio-pipe-sink"

# Wire up + drive
unio-pipe\tools\drive_windows.ps1 unio-pipe-sink caps
unio-pipe\tools\drive_windows.ps1 unio-pipe-sink in 5099 320 180 loop1
unio-pipe\tools\drive_windows.ps1 unio-pipe-sink status
unio-pipe\tools\drive_windows.ps1 unio-pipe-sink stop loop1
```

## `latency_stats.py`

```
latency_stats.py <csv-path> [warmup-rows]
```

Reads the sink's latency CSV, drops the first `warmup-rows` samples
(default 60), filters rows whose diff exceeds 1000 s (cross-machine
NTP-skew artefact), and prints p50 / p95 / max / mean for each of
the three phase-deltas.

Example output:

```
rows (raw): 1281
warmup:     60
  capture_to_decode_us           n= 1221  p50=  10.09 ms  p95=  12.02 ms  max=  48.49 ms  mean=  10.27 ms
  decode_to_present_us           n= 1221  p50=   0.93 ms  p95=   1.61 ms  max=   9.22 ms  mean=   1.05 ms
  capture_to_present_us          n= 1221  p50=  11.01 ms  p95=  13.47 ms  max=  51.05 ms  mean=  11.31 ms
```

Cross-machine rows that overflow into giant uint64 values (from residual
NTP skew between hosts) are dropped before stats — see the README
latency section's "cross-machine needs a shared clock" caveat.

## `sync-clocks.sh`

```
./sync-clocks.sh                      # Linux only
./sync-clocks.sh --win user@host      # Linux + Windows via SSH
```

Forces `chrony`/`timedatectl` resync on the Linux side and (optionally)
`w32tm /resync /force` on the Windows side via SSH. Prints stratum +
dispersion per host. Run this before any cross-machine latency run that
crosses clocks — the numbers are only as tight as the NTP floor between
the two machines. See the README's discussion of the 490 ms skew we hit
against `time.windows.com`; a shared LAN stratum-1 tightens this.

The script deliberately does NOT estimate inter-host offset over SSH —
SSH RTT noise is too big to distinguish from the clock skew we're trying
to measure. For sub-ms cross-machine accuracy, use the app-level clock-
sync handshake (tracked as a follow-up; see the architecture doc).

## Common matrix coverage workflows

Every row in the coverage matrix (#31 on the tracker) has a one-liner
here. `loopback.py` handles single-host; `cross_machine.py` handles
Linux ↔ Windows.

| Matrix row | Command |
|---|---|
| Linux X11 + Intel / AMD (shipped) | `tools/loopback.py` |
| Linux X11 + NVIDIA (shipped ≙ WP 10 #21 + #27) | `tools/loopback.py --src nvenc-linux --sink nvdec` |
| Linux X11 mixed (Intel enc → NVIDIA dec) | `tools/loopback.py --src vaapi --sink nvdec` |
| Linux X11 mixed (NVIDIA enc → Intel dec) | `tools/loopback.py --src nvenc-linux --sink vaapi` |
| Linux Wayland + Intel / AMD (needs #7 + #30) | `tools/loopback.py --capture wayland-pipewire --presenter egl-wayland` |
| Linux Wayland + NVIDIA (needs #7 + #30 + #21 + #27) | `tools/loopback.py --src nvenc-linux --sink nvdec --capture wayland-pipewire --presenter egl-wayland` |
| Windows + NVIDIA ↔ Linux (any Linux decoder) | `tools/cross_machine.py lin2win --sink d3d11va`  /  `win2lin --sink vaapi`  /  `win2lin --sink nvdec` |
| Windows + NVIDIA loopback (same-host Diana) | *(future `tools/loopback_windows.ps1`; today drive via `drive_windows.ps1` from an SSH session on Diana)* |
| Windows + AMD (scoped, needs borrowed hw) | n/a until #25 ships an encoder |
| Windows + Intel (scoped, Diana has Intel UHD) | n/a until #26 ships oneVPL + `UNIO_PIPE_FORCE_ENCODER=onevpl` |
| No-encoder refusal UI test | `UNIO_PIPE_FORCE_NO_STREAMING=1 tools/loopback.py` |

The `UNIO_PIPE_DISABLE_PROBE=1` kill-switch on the sink returns hardwired
per-OS backends and bypasses the runtime probe entirely — useful when
debugging a suspected probe false-negative.

## Failure-mode playbook

- **"helper socket didn't appear in 3s"** — the spawned helper crashed
  at init. `tools/loopback.py` prints the `/tmp/unio-sink.log` and
  `/tmp/unio-src.log` contents automatically on this path. Usually
  means a forced vendor isn't actually available; drop the `--src` /
  `--sink` force or check `helper_caps` first.
- **Orphan helpers + stale sockets after a crash** — `tools/kill_all.sh`.
- **"QUIC connect: handshake failed"** on the src — sink's QUIC listener
  isn't up yet, or a firewall is blocking. Bump the sleep between
  `start_inbound` and `start_outbound` if you're on a slow box; check
  `ss -lun` (Linux) / firewall rules (Windows).
- **Cross-machine CSV shows `~1e18 µs` values** — NTP clock skew. Run
  `tools/sync-clocks.sh --win Diana@...` and retry. For sub-ms
  accuracy the app-level clock-sync handshake is the real fix
  (tracked separately; see README latency-table section).
- **Black override-redirect window covers the terminal** — happens when
  the sink's EGL-X11 presenter window is full-screen. `loopback.py`
  defaults to 320×180 to avoid this; `--win-w` / `--win-h` control it.

## Adding new tools

Keep them dev-only (never shipped in the installer). If a tool grows
stateful setup (spawns helpers, waits, tears down), prefer keeping the
launch / teardown in a *shell* script or the Python driver, not inside
these scripts — single-responsibility keeps debugging cheaper.

If you write a new script that's useful across sessions, add it here
with a short entry in the Inventory table above, a usage block in the
relevant section, and a commit message that names the matrix row or
issue it serves.

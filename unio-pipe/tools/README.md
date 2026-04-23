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
  - `UNIO_PIPE_FORCE_ENCODER=vaapi` / `nvenc-linux`
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
  --src {vaapi,nvenc-linux,nvenc}    Force encoder (src side)
  --sink {vaapi,nvdec}               Force decoder (sink side)
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
```

- `lin2win`: adi-pc is the src (VA-API or NVENC-Linux), Diana is the sink
  (D3D11VA or NVDEC). Diana's sink launches via `schtasks /IT` so DXGI
  gets a user desktop.
- `win2lin`: Diana is the src (WGC + NVENC), adi-pc is the sink. Diana's
  src runs via `schtasks /IT` so WGC captures the actual desktop.

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
| Windows + Intel loopback (same-host Diana, WP 10 #26) | `tools/cross_machine.py win2win` (default src/sink = `onevpl`) |
| Windows + Intel → Linux (WP 10 #26) | `tools/cross_machine.py win2lin --src onevpl --sink vaapi` |
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

## `matrix_test.py` — cross-host latency matrix runner

### What it does

Takes a declarative inventory of your lab (every machine, what
encoders/decoders each supports, how to reach it), expands that
into every (src_host × encoder × sink_host × decoder) tuple,
runs each combo end-to-end, and prints one table with p50/p95
latency for every cell.

Directions are derived from the two hosts' OS and identity — no
need to specify `lin2lin` / `lin2win` / etc. in the config:
loopback (same-host) falls out of `src_host == sink_host`,
cross-machine from differing OS.

### Quick start

```bash
# Copy the template and edit for your machines.
cp unio-pipe/tools/hosts.example.yaml unio-pipe/tools/hosts.yaml
$EDITOR unio-pipe/tools/hosts.yaml

# See what would run.
unio-pipe/tools/matrix_test.py --list

# Probe every declared host's capabilities.
unio-pipe/tools/matrix_test.py --probe

# Run everything.
unio-pipe/tools/matrix_test.py

# Run one cell.
unio-pipe/tools/matrix_test.py --direction lin2lin \
    --src vaapi --sink vaapi

# Run cross-machine, push source to every remote first + rebuild.
unio-pipe/tools/matrix_test.py --sync

# Capture a baseline for regression gating in CI.
unio-pipe/tools/matrix_test.py --out baseline.json

# Later: diff a run against that baseline.
unio-pipe/tools/matrix_test.py --baseline baseline.json \
    --fail-on-regression
```

### `hosts.yaml` — the single source of truth

Every machine in the lab gets an entry. One must be
`role: local` — that's the orchestrator (where you run
`matrix_test.py` from). Others are `role: remote`.

```yaml
hosts:
  adi-pc:
    role: local
    os: linux
    address: 192.168.1.23
    binary: ./unio-pipe/build/unio-pipe
    workdir: /tmp
    supports:
      encoders: [vaapi, nvenc-linux]
      decoders: [vaapi, nvdec]

  diana:
    role: remote
    os: windows
    address: 192.168.1.18
    ssh_host: Diana@192.168.1.18
    ssh_key: ~/.ssh/id_ecdsa
    binary: 'C:\Users\Diana\unio\unio-pipe\build\Release\unio-pipe.exe'
    workdir: 'C:\Users\Diana'
    # Optional: needed only for --sync.
    source_root: 'C:\Users\Diana\unio\unio-pipe'
    build_cmd: '"C:\Strawberry\c\bin\cmake.EXE" --build ...'
    supports:
      encoders: [nvenc, onevpl]
      decoders: [d3d11va, onevpl]

defaults:
  duration_s: 20
  width: 1920
  height: 1080
  fps: 30
  ntp_server: time.cloudflare.com
  max_clock_skew_ms: 50
```

Adding a third host is one YAML entry — no code change. See
`hosts.example.yaml` for the full commented template.

### Filters

```
--direction {lin2lin,win2win,lin2win,win2lin}  one direction only
--src ENCODER                                   one src encoder
--sink DECODER                                  one sink decoder
--src-host NAME                                 one src host
--sink-host NAME                                one sink host
--duration SECONDS                              default: hosts.yaml defaults
--csv-dir PATH                                  where per-combo CSVs land
```

### Pre-flight

Before running any combo, the tool verifies three things:

1. **Same code on every host** via `helper_caps.build_commit`
   (CMake embeds `git rev-parse HEAD` at configure time). Mixed
   commits → fail. Mixed "unknown" (old binary without the
   feature) vs a real commit → fail. Same commit everywhere →
   continue.
2. **Build freshness** via binary-mtime vs last `unio-pipe/`-touching
   commit. Warn on STALE; only fail under `--strict-version`.
3. **Inter-host clock skew** (cross-machine combos only). Skips
   loopback. Uses a UDP NTP-style probe for sub-ms precision;
   falls back to SSH-per-sample if UDP can't reach. Refuse to
   run if skew > `max_clock_skew_ms` (50 ms default); override
   with `--ignore-clock-skew`. Measured skew is **applied as a
   correction to cap→decode / cap→present phases** so huge
   drifts become transparent. The `ok*` tag in the report
   flags corrected rows.

### `--sync` — push source + rebuild across the network

```
matrix_test.py --sync
```

Before every combo:

1. Tar the orchestrator's `unio-pipe/` (excluding `build/`,
   `__pycache__`, etc.)
2. `ssh $remote 'cd $source_root && tar -xzf -'`
3. `ssh $remote 'cmake -S $source_root -B ... -DUNIO_BUILD_COMMIT=<sha>
   -DFETCHCONTENT_FULLY_DISCONNECTED=ON && cmake --build ...'`

The orchestrator's commit SHA is handed across as
`-DUNIO_BUILD_COMMIT=...` so the remote's extracted tree (no
`.git/`) still embeds the right commit. The disconnect flag
stops CMake's FetchContent from re-cloning msquic on every
configure.

Requires the remote to have a working toolchain
(cmake + VS Build Tools on Windows, cmake + gcc + the media
`-dev` packages on Linux).

### `--use-prebuilt` — dispatch binaries built in Docker on the orchestrator

```
matrix_test.py --use-prebuilt
```

Alternative to `--sync`: instead of rebuilding on every remote,
binaries are built **once on the orchestrator** (in Docker
containers via `packaging/docker/build-linux.sh` and
`packaging/docker/build-win.sh`) and scp'd out to each host.
Remotes need zero toolchain — only SSH access and write
permission at the `binary` path in `hosts.yaml`.

Workflow:

```bash
# Build both targets once.
packaging/docker/build-linux.sh      # → dist/linux-x64/
packaging/docker/build-win.sh        # → dist/win-x64/

# Dispatch and run:
tools/matrix_test.py --use-prebuilt
```

The pre-flight `build_commit` check still fires (each host's
binary reports the commit it was built from, they must match).
The binaries ship with everything they need:

- Linux: `unio-pipe` — single-file binary. msquic + openssl3
  are statically linked in (`QUIC_BUILD_SHARED=OFF`); system
  libs (libva / libEGL / libX11 / libcrypto / libnuma /
  libstdc++) come from the target's own package manager.
- Windows: `unio-pipe.exe` — single-file binary. msquic +
  openssl3 + libvpl are all statically linked; `/MT` links
  the MSVC C++ runtime statically too, so no VC++
  Redistributable is required on the target. The oneVPL
  dispatcher (now inside the exe) still runtime-loads Intel's
  real media runtime (`libmfxhw64.dll`) from the installed
  Intel driver — that DLL comes from the user's graphics
  driver, not from us.

See `packaging/docker/README.md` (post-#52 merge) for the image
recipes and cold-build times.

### JSON output + baseline

```
--out FILE.json                 write structured results
--baseline OLD.json             compare current vs. stored
--regression-pct 20             p50 regression threshold %
--regression-ms 5               p50 regression threshold ms
--fail-on-regression            exit 1 on any regression (CI)
```

Schema is documented at the top of `matrix_test.py`. Stable
within a major `schema_version` bump; adding fields never
breaks old consumers.

Baselines carry the pre-flight context — build commit, per-host
skew + method + RTT, NTP server used — so comparing runs
months apart still shows *why* the numbers are what they are.

### Sample output

```
[pre-flight] probing helper_caps.build_commit on 2 host(s)
[pre-flight] all reachable hosts @ 5550dbab459f (dirty working tree)
[pre-flight] adi-pc       fresh
[pre-flight] diana        fresh
[pre-flight] resyncing clocks against time.cloudflare.com
[pre-flight]   adi-pc       ok: chronyc makestep succeeded
[pre-flight]   diana        ok: w32tm resynced against time.cloudflare.com
[pre-flight]   adi-pc       skew=   0 ms (orchestrator)
[pre-flight]   diana        skew= +447 ms  (±1 ms, RTT 0 ms, method=udp) OVER

[1/4] lin2lin loop adi-pc:vaapi → adi-pc:vaapi
  ok: glass p50=15.19 ms p95=22.77 ms n=180
[2/4] lin2win xhost adi-pc:vaapi → diana:d3d11va
  ok: glass p50=14.89 ms p95=22.62 ms n=251
...

direction  src              sink                glass p50/p95   status
lin2lin    adi-pc:vaapi     adi-pc:vaapi         15.19/  22.77   ok
lin2win    adi-pc:vaapi     diana:d3d11va        14.89/  22.62   ok*

 * cap→* phases adjusted for measured inter-host clock skew
```

### Exit codes

| Code | Meaning |
|---|---|
| 0 | every attempted combo passed |
| 1 | at least one combo failed OR `--fail-on-regression` + regression |
| 2 | config error (`hosts.yaml` invalid) |
| 3 | build-freshness pre-flight failed (strict mode) |
| 4 | `--sync` failed |
| 5 | clock-skew pre-flight failed (no `--ignore-clock-skew`) |

## Typical workflows

### Regression hunt

```bash
# Snapshot known-good numbers from a clean main.
git checkout main && cmake --build unio-pipe/build
matrix_test.py --sync --out baseline-main.json

# After landing a suspect change:
matrix_test.py --sync --baseline baseline-main.json
# Red-highlighted rows are regressions. Each shows base vs.
# current p50 and the delta; use --regression-pct 10 for a
# tighter gate during investigation.
```

### Cross-machine sanity after an NTP outage

```bash
# Drift-blind "is streaming even working" run.
matrix_test.py --direction win2win         # single-clock, no skew
matrix_test.py --direction lin2lin         # single-clock, no skew

# Then re-establish cross-machine:
sudo systemctl restart systemd-timesyncd   # adi-pc
# (Diana auto-resyncs via matrix_test's w32tm /resync)
matrix_test.py --direction lin2win --direction win2lin
```

### Adding a new host

1. Edit `hosts.yaml` → add the host block + its `supports` list
2. `matrix_test.py --probe`  → confirms reachability and reports
   capabilities vs. what you declared
3. `matrix_test.py --sync --src-host <newhost>` — or just
   `--sync` to push + rebuild everywhere at once
4. Full matrix run

## Failure-mode playbook

- **"helper socket didn't appear in 6s"** — the spawned helper
  crashed at init. Check `/tmp/unio-matrix-*.log` (Linux) or
  `C:\Users\<user>\unio-matrix-*.log` (Windows). Usually means
  a forced backend isn't available on the host; the capability
  probe (`--probe`) will show which are.
- **"src substituted encoder: asked='nvenc-linux' got='vaapi'"** —
  the helper binary doesn't have `nvenc-linux` compiled in and
  silently fell back. Rebuild (or run with `--sync`). This
  fires even when the run superficially succeeded — without
  the check, the table would show a VA-API-speed p50 under an
  NVENC label.
- **"COMMIT MISMATCH across hosts"** — one host is running a
  different branch's binary. `--sync` fixes it. If you want to
  run anyway (e.g. measuring a known-mismatched A/B pair),
  you'd need to either rebuild both to the same commit or
  accept the mismatch consciously (no override yet — on
  purpose: silent code mixing is the failure mode this is
  preventing).
- **"clock skew exceeds threshold"** — inter-host drift larger
  than `max_clock_skew_ms`. The UDP probe gave you a precise
  measurement in the log; either raise the threshold, set up a
  shared LAN stratum-1 NTP source, or pass `--ignore-clock-skew`
  to run anyway (the tool still corrects cap→* phases using
  the measured drift).
- **"all were rejected by the raw >1000 s skew filter"** — a
  host-level timestamp bug, not NTP drift (e.g. Diana's
  `capture_wgc.cpp` QPC overflow, pre-PR-#49). The error points
  to the relevant bug if known.
- **Orphan helpers after a crash** — `pkill -9 unio-pipe` on
  Linux, `taskkill /IM unio-pipe.exe /F` on Windows. Or just
  rerun `matrix_test.py`; its pre-combo teardown will sweep
  stragglers.

## Why this exists

Issue #50. Before this tool, running the coverage matrix
(#31) meant ~16 invocations of three different scripts,
eyeballing console output, and hoping nothing drifted between
hosts. The runner productises that into one command with
structured output, cross-host consistency guarantees, and
regression gating that CI can call directly.

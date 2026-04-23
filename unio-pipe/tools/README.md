# `unio-pipe/tools/`

Dev-time scripts for driving the helper over its control socket and
running measurement experiments. None of these ship with the
user-facing binary.

## Inventory

| Tool | OS | Purpose |
|---|---|---|
| `matrix_test.py` | Linux (orchestrator) | **Cross-host latency matrix runner.** Reads `hosts.yaml`, enumerates every valid (src_host × encoder × sink_host × decoder) combo, runs them end-to-end, reports p50/p95 per phase. Handles loopback and cross-machine transparently. |
| `hosts.example.yaml` | — | Reference configuration. Copy to `hosts.yaml` and edit for your lab. `hosts.yaml` is gitignored. |
| `sync-clocks.sh` | Linux (+ Windows via SSH) | Manual NTP resync on one or both hosts. `matrix_test.py` does this automatically as a pre-flight, but the script is still useful for ad-hoc debugging. |

## `matrix_test.py` — the main tool

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

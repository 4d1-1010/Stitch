#!/usr/bin/env python3
"""Matrix latency test runner for unio-pipe.

Declare every machine in your lab + what it supports in
tools/hosts.yaml (see tools/hosts.example.yaml for the reference).
This runner enumerates every valid (src_host, src_encoder,
sink_host, sink_decoder) tuple the config allows, runs each combo
end-to-end, and prints a latency-focused report.

Directions are derived from the two hosts' OS and their identity:
  - src_host == sink_host + OS=linux   → lin2lin (same-host loopback)
  - src_host == sink_host + OS=windows → win2win (same-host loopback)
  - src.os=linux + sink.os=windows     → lin2win (cross-machine)
  - src.os=windows + sink.os=linux     → win2lin (cross-machine)

No hardcoded host names: point --config at a different file to run
on a different lab. No hardcoded direction dispatch: the same code
path drives every combo — only the Host subclass (LocalLinuxHost,
RemoteWindowsHost, ...) differs.

Phase 1a scope (this commit):
  - YAML config loader + schema validation
  - Host abstraction: launch / rpc / kill_all / fetch_latency_csv
  - LocalLinuxHost (orchestrator) + RemoteWindowsHost
  - Combo enumeration + CLI filters
  - Per-combo runner: launch → wire-up → stream → stats → teardown
  - Pretty-printed table

Phases 2+ (see issue #50) add baseline diff, pre-flight hardening,
and documentation.

Typical usage:
    tools/matrix_test.py                          # every combo
    tools/matrix_test.py --direction lin2lin      # only Linux loopback
    tools/matrix_test.py --src vaapi --sink nvdec # one cell
    tools/matrix_test.py --probe                  # capability probe only
    tools/matrix_test.py --duration 10 --out results.json
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shlex
import signal
import socket
import struct
import subprocess
import sys
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

try:
    import yaml
except ImportError:
    print("error: pyyaml is required (pip install pyyaml)",
          file=sys.stderr)
    sys.exit(2)


# ── Config ──────────────────────────────────────────────────────


CONFIG_REQUIRED_FIELDS = {
    "role", "os", "address", "binary", "supports",
}
SUPPORTS_KEYS = {"capture", "encoders", "decoders", "presenters"}
VALID_ROLES = {"local", "remote"}
VALID_OS = {"linux", "windows"}


def load_config(path: Path) -> dict:
    """Load + validate hosts.yaml. Returns the parsed dict."""
    if not path.exists():
        raise SystemExit(
            f"error: config not found at {path}\n"
            f"       copy {path.parent}/hosts.example.yaml "
            f"to {path.name} and edit for your lab."
        )
    with open(path) as f:
        cfg = yaml.safe_load(f)
    if not isinstance(cfg, dict) or "hosts" not in cfg:
        raise SystemExit("error: config must have a top-level 'hosts:' key")

    hosts = cfg["hosts"]
    if not hosts:
        raise SystemExit("error: 'hosts:' is empty")

    local_count = 0
    for name, h in hosts.items():
        missing = CONFIG_REQUIRED_FIELDS - set(h.keys())
        if missing:
            raise SystemExit(
                f"error: host '{name}' missing required fields: "
                f"{sorted(missing)}")
        if h["role"] not in VALID_ROLES:
            raise SystemExit(
                f"error: host '{name}' role must be one of "
                f"{sorted(VALID_ROLES)}")
        if h["os"] not in VALID_OS:
            raise SystemExit(
                f"error: host '{name}' os must be one of "
                f"{sorted(VALID_OS)}")
        if h["role"] == "local":
            local_count += 1
        if h["role"] == "remote":
            if "ssh_host" not in h:
                raise SystemExit(
                    f"error: remote host '{name}' needs ssh_host")
        for k in h["supports"]:
            if k not in SUPPORTS_KEYS:
                raise SystemExit(
                    f"error: host '{name}' supports.{k} not "
                    f"recognised (valid: {sorted(SUPPORTS_KEYS)})")

    if local_count != 1:
        raise SystemExit(
            f"error: exactly one host must have role: local "
            f"(orchestrator); found {local_count}")

    cfg.setdefault("defaults", {})
    for k, v in [
        ("duration_s", 25), ("width", 1920), ("height", 1080),
        ("fps", 30), ("warmup_frames", 60), ("port_base", 5099),
        ("cooldown_s", 2), ("max_clock_skew_ms", 50),
    ]:
        cfg["defaults"].setdefault(k, v)
    return cfg


# ── RPC wire (length-prefixed JSON, same format the helper speaks) ──


def _recv_exact(s: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError(f"short read ({len(buf)}/{n})")
        buf += chunk
    return buf


def uds_rpc(sock_path: str, cmd: dict, timeout_s: float = 5.0) -> dict:
    """AF_UNIX length-prefixed JSON RPC. Linux helpers only."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout_s)
    s.connect(sock_path)
    try:
        body = json.dumps(cmd).encode()
        s.sendall(struct.pack("<I", len(body)) + body)
        raw = _recv_exact(s, 4)
        n = struct.unpack("<I", raw)[0]
        return json.loads(_recv_exact(s, n).decode())
    finally:
        s.close()


# ── SSH helpers (used by RemoteWindowsHost) ─────────────────────


def _ssh(ssh_host: str, key: str, cmd: str,
         check: bool = True, timeout_s: int = 30) -> str:
    full = [
        "ssh", "-o", "IdentitiesOnly=yes",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=5",
        "-i", key, ssh_host, cmd,
    ]
    r = subprocess.run(full, capture_output=True, timeout=timeout_s)
    out = r.stdout.decode("utf-8", errors="replace") if r.stdout else ""
    err = r.stderr.decode("utf-8", errors="replace") if r.stderr else ""
    if check and r.returncode != 0:
        raise RuntimeError(
            f"ssh failed (rc={r.returncode}): {cmd}\n"
            f"stdout={out}\nstderr={err}")
    return out + err


def _scp_from(ssh_host: str, key: str, remote: str, local: str) -> int:
    # OpenSSH on Windows needs forward-slash remote paths.
    remote = remote.replace("\\", "/")
    r = subprocess.run(
        ["scp", "-o", "IdentitiesOnly=yes",
         "-i", key, f"{ssh_host}:{remote}", local],
        capture_output=True)
    return r.returncode


# ── Host abstraction ────────────────────────────────────────────


class Host(ABC):
    """One machine declared in hosts.yaml. Subclassed per
    (role, os) combo: LocalLinuxHost, RemoteWindowsHost, ...
    Adding a new combo = a new subclass; matrix_test.py doesn't
    care beyond the ABC interface."""

    def __init__(self, name: str, cfg: dict):
        self.name = name
        self.os = cfg["os"]
        self.role = cfg["role"]
        self.address = cfg["address"]
        self.binary = cfg["binary"]
        self.workdir = cfg.get("workdir",
            "/tmp" if self.os == "linux" else r"C:\Users\Default")
        self.supports = cfg["supports"]

    def encoders(self) -> list[str]:
        return list(self.supports.get("encoders", []))

    def decoders(self) -> list[str]:
        return list(self.supports.get("decoders", []))

    # Every host has two RPC endpoints — one for the src role,
    # one for the sink — because a single machine might host both
    # (win2win loopback). Subclasses pick platform-specific paths
    # (UDS socket vs. named pipe) for each.
    @abstractmethod
    def launch(self, role: str, env: dict) -> None: ...

    @abstractmethod
    def rpc(self, role: str, cmd: dict, timeout_s: float = 5.0) -> dict: ...

    @abstractmethod
    def kill_all(self) -> None: ...

    @abstractmethod
    def wait_ready(self, role: str, timeout_s: float = 6.0) -> bool:
        """Return True once the helper for `role` is listening."""

    @abstractmethod
    def fetch_latency_csv(self, local_path: str) -> bool:
        """Copy this host's sink-side latency CSV back to the
        orchestrator. Return False if missing (e.g., src host)."""

    def probe_caps(self) -> dict:
        """Launch a bare helper, call helper_caps, tear down.
        Returns the parsed helper_caps dict or {'error': ...}.
        Every failure mode — unreachable host, stale helper that
        won't die, helper that comes up but crashes — surfaces
        as a clean error string, never an exception upward."""
        try:
            self.kill_all()
            self.launch("probe", env={})
            if not self.wait_ready("probe", timeout_s=5.0):
                return {"error": "helper didn't come up"}
            return self.rpc("probe", {"cmd": "helper_caps"})
        except Exception as e:
            msg = str(e).splitlines()[0] if str(e) else type(e).__name__
            return {"error": msg}
        finally:
            try:
                self.kill_all()
            except Exception:
                pass


class LocalLinuxHost(Host):
    """The orchestrator — spawn unio-pipe directly with subprocess.
    One AF_UNIX socket per role: {workdir}/unio-matrix-{role}.sock."""

    def __init__(self, name: str, cfg: dict):
        super().__init__(name, cfg)
        self._procs: dict[str, subprocess.Popen] = {}

    def _sock(self, role: str) -> str:
        return f"{self.workdir}/unio-matrix-{role}.sock"

    def _log(self, role: str) -> str:
        return f"{self.workdir}/unio-matrix-{role}.log"

    def _session_env(self) -> dict:
        env = {}
        # X11 session bits the capture path needs. Default to the
        # running caller's values when set, else sensible defaults
        # for a graphical-login shell on adi-pc.
        env["DISPLAY"] = os.environ.get("DISPLAY", ":1")
        env["XAUTHORITY"] = os.environ.get(
            "XAUTHORITY", "/run/user/1000/gdm/Xauthority")
        for k in ("WAYLAND_DISPLAY", "XDG_SESSION_TYPE",
                  "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"):
            if k in os.environ:
                env[k] = os.environ[k]
        return env

    def launch(self, role: str, env: dict) -> None:
        sock = self._sock(role)
        log = self._log(role)
        for p in (sock, log):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        merged = os.environ.copy()
        merged.update(self._session_env())
        merged.update(env)
        logf = open(log, "w")
        p = subprocess.Popen(
            [self.binary, "--socket", sock],
            stdin=subprocess.DEVNULL, stdout=logf, stderr=logf,
            env=merged, start_new_session=True,
        )
        self._procs[role] = p

    def wait_ready(self, role: str, timeout_s: float = 6.0) -> bool:
        deadline = time.monotonic() + timeout_s
        sock = self._sock(role)
        while time.monotonic() < deadline:
            if os.path.exists(sock):
                return True
            time.sleep(0.05)
        return False

    def rpc(self, role: str, cmd: dict, timeout_s: float = 5.0) -> dict:
        return uds_rpc(self._sock(role), cmd, timeout_s)

    def kill_all(self) -> None:
        # Ours first (clean shutdown), then any stragglers.
        for role, p in list(self._procs.items()):
            if p.poll() is None:
                try:
                    os.killpg(os.getpgid(p.pid), signal.SIGTERM)
                    p.wait(timeout=2)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    try:
                        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            self._procs.pop(role, None)
        subprocess.run(
            ["pkill", "-9", "-f", f"{self.binary} --socket"],
            stderr=subprocess.DEVNULL, check=False)
        time.sleep(0.2)
        for role in ("src", "sink", "probe"):
            for p in (self._sock(role), self._log(role)):
                try:
                    os.unlink(p)
                except FileNotFoundError:
                    pass

    def fetch_latency_csv(self, local_path: str) -> bool:
        # The sink helper on this host wrote directly to local_path
        # via UNIO_PIPE_LATENCY_CSV — no fetch needed.
        return os.path.exists(local_path)


class RemoteWindowsHost(Host):
    """Reached via SSH. Helpers launch via schtasks so they
    inherit the user's desktop session (WGC + DXGI require it).
    One named pipe per role."""

    TASK_NAMES = {"src": "unio-matrix-src-task",
                  "sink": "unio-matrix-sink-task",
                  "probe": "unio-matrix-probe-task"}

    def __init__(self, name: str, cfg: dict):
        super().__init__(name, cfg)
        self.ssh_host = cfg["ssh_host"]
        self.ssh_key = os.path.expanduser(cfg["ssh_key"])
        # Where the helper writes its sink-side latency CSV.
        self._csv_remote = (self.workdir.rstrip("\\") +
                            r"\unio-matrix-lat.csv")

    def _pipe(self, role: str) -> str:
        return fr"unio-matrix-{role}"

    def _log(self, role: str) -> str:
        return self.workdir.rstrip("\\") + fr"\unio-matrix-{role}.log"

    def _cmd_path(self, role: str) -> str:
        return self.workdir.rstrip("\\") + fr"\unio-matrix-{role}.cmd"

    def launch(self, role: str, env: dict) -> None:
        # Build a .cmd file ferried over via base64 — escapes
        # cleanly through cmd.exe / ssh / schtasks quoting.
        set_lines = [f'set "{k}={v}"' for k, v in env.items()]
        body = "@echo off\r\n"
        for ln in set_lines:
            body += ln + "\r\n"
        body += (f'{self.binary} --socket \\\\.\\pipe\\{self._pipe(role)}'
                 f' > {self._log(role)} 2>&1\r\n')
        b64 = base64.b64encode(body.encode("utf-8")).decode("ascii")
        cmd_path = self._cmd_path(role)
        _ssh(self.ssh_host, self.ssh_key,
             f"powershell -NoProfile -Command "
             f"\"[IO.File]::WriteAllBytes('{cmd_path}',"
             f"[Convert]::FromBase64String('{b64}'))\"")
        task = self.TASK_NAMES[role]
        # /IT (interactive) only for src role: WGC needs the user
        # desktop. Sink + probe roles run in Session 0 fine.
        it_flag = "/IT" if role == "src" else ""
        _ssh(self.ssh_host, self.ssh_key,
             f'schtasks /Create /TN {task} /TR "{cmd_path}" '
             f'/SC ONCE /ST 23:59 {it_flag} /RL LIMITED /F')
        _ssh(self.ssh_host, self.ssh_key,
             f"schtasks /Run /TN {task}")

    def wait_ready(self, role: str, timeout_s: float = 6.0) -> bool:
        # Proxy: the helper creates its named pipe ~200 ms after
        # the process starts. Poll tasklist — once unio-pipe.exe
        # is running, a short extra sleep covers pipe creation.
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                out = _ssh(
                    self.ssh_host, self.ssh_key,
                    'tasklist /FI "IMAGENAME eq unio-pipe.exe" /NH',
                    check=False, timeout_s=5)
                if "unio-pipe.exe" in out:
                    time.sleep(0.4)
                    return True
            except Exception:
                pass
            time.sleep(0.3)
        return False

    def rpc(self, role: str, cmd: dict, timeout_s: float = 5.0) -> dict:
        # Speak the named-pipe protocol from inside PowerShell over
        # SSH — encode the JSON body as base64 so the quoting
        # survives the cmd.exe / ssh / powershell layers.
        body = json.dumps(cmd)
        body_b64 = base64.b64encode(body.encode("utf-8")).decode("ascii")
        pipe = self._pipe(role)
        ps = (
            "$ErrorActionPreference='Stop';"
            f"$body=[Convert]::FromBase64String('{body_b64}');"
            "$len=[BitConverter]::GetBytes([Int32]$body.Length);"
            f"$p=New-Object IO.Pipes.NamedPipeClientStream("
            f"'.','{pipe}',[IO.Pipes.PipeDirection]::InOut);"
            f"$p.Connect({int(timeout_s * 1000)});"
            "$p.Write($len,0,4); $p.Write($body,0,$body.Length);"
            "$hdr=New-Object byte[] 4; $n=0;"
            "while ($n -lt 4) {"
            " $r=$p.Read($hdr,$n,4-$n);"
            " if ($r -le 0) { throw 'short header' };"
            " $n+=$r };"
            "$rlen=[BitConverter]::ToInt32($hdr,0);"
            "$resp=New-Object byte[] $rlen; $n=0;"
            "while ($n -lt $rlen) {"
            " $r=$p.Read($resp,$n,$rlen-$n);"
            " if ($r -le 0) { throw 'short body' };"
            " $n+=$r };"
            "$p.Close();"
            "[Convert]::ToBase64String($resp)"
        )
        out = _ssh(
            self.ssh_host, self.ssh_key,
            f'powershell -NoProfile -Command "{ps}"',
            timeout_s=int(timeout_s) + 15)
        # Last non-empty line is the base64-encoded JSON reply.
        lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
        if not lines:
            raise RuntimeError(f"empty RPC response from {self.name}")
        reply = base64.b64decode(lines[-1]).decode("utf-8")
        return json.loads(reply)

    def kill_all(self) -> None:
        script = (
            "taskkill /IM unio-pipe.exe /F 2>nul & "
            f"del /F /Q {self._log('src')} {self._log('sink')} "
            f"{self._log('probe')} {self._csv_remote} 2>nul"
        )
        _ssh(self.ssh_host, self.ssh_key, script, check=False)
        for task in self.TASK_NAMES.values():
            _ssh(self.ssh_host, self.ssh_key,
                 f"schtasks /Delete /TN {task} /F 2>nul", check=False)

    def fetch_latency_csv(self, local_path: str) -> bool:
        return _scp_from(self.ssh_host, self.ssh_key,
                         self._csv_remote, local_path) == 0

    @property
    def latency_csv_path(self) -> str:
        """Remote path the sink writes to (for
        UNIO_PIPE_LATENCY_CSV)."""
        return self._csv_remote


def build_hosts(cfg: dict) -> dict[str, Host]:
    out: dict[str, Host] = {}
    for name, h in cfg["hosts"].items():
        role, os_ = h["role"], h["os"]
        if role == "local" and os_ == "linux":
            out[name] = LocalLinuxHost(name, h)
        elif role == "remote" and os_ == "windows":
            out[name] = RemoteWindowsHost(name, h)
        else:
            raise SystemExit(
                f"error: host '{name}' role/os combo "
                f"({role}/{os_}) not yet implemented. "
                f"Today: local+linux, remote+windows.")
    return out


# ── Combo enumeration ───────────────────────────────────────────


@dataclass
class Combo:
    src: Host
    src_encoder: str
    sink: Host
    sink_decoder: str

    @property
    def direction(self) -> str:
        if self.src is self.sink:
            return f"{self.src.os[:3]}2{self.sink.os[:3]}".replace(
                "win2win", "win2win").replace("lin2lin", "lin2lin")
        return f"{self.src.os[:3]}2{self.sink.os[:3]}"

    @property
    def is_loopback(self) -> bool:
        return self.src is self.sink

    @property
    def label(self) -> str:
        mode = "loop" if self.is_loopback else "xhost"
        return (f"{self.direction} {mode} "
                f"{self.src.name}:{self.src_encoder} "
                f"→ {self.sink.name}:{self.sink_decoder}")


def enumerate_combos(hosts: dict[str, Host]) -> list[Combo]:
    combos: list[Combo] = []
    for src in hosts.values():
        for enc in src.encoders():
            for sink in hosts.values():
                for dec in sink.decoders():
                    combos.append(Combo(src, enc, sink, dec))
    # Stable sort: direction, src, enc, sink, dec.
    combos.sort(key=lambda c: (
        c.direction, c.src.name, c.src_encoder,
        c.sink.name, c.sink_decoder))
    return combos


def filter_combos(combos: list[Combo], args) -> list[Combo]:
    def keep(c: Combo) -> bool:
        if args.direction and c.direction != args.direction:
            return False
        if args.src and c.src_encoder != args.src:
            return False
        if args.sink and c.sink_decoder != args.sink:
            return False
        if args.src_host and c.src.name != args.src_host:
            return False
        if args.sink_host and c.sink.name != args.sink_host:
            return False
        return True
    return [c for c in combos if keep(c)]


# ── Latency stats (standalone — no external deps) ───────────────


def summarise_csv(csv_path: str, warmup: int,
                  skew_filter_s: float = 1000.0) -> dict:
    """Parse the helper's latency CSV, drop warmup rows, filter
    cross-machine NTP-skew artefacts (> skew_filter_s apart).
    Returns a dict of {phase: {p50_us, p95_us, max_us, mean_us, n}}."""
    import csv
    import statistics
    out = {"rows_raw": 0, "rows_kept": 0, "phases": {}}
    try:
        rows = list(csv.DictReader(open(csv_path)))
    except FileNotFoundError:
        return out
    out["rows_raw"] = len(rows)
    skew_us = int(skew_filter_s * 1_000_000)
    for key in ("capture_to_decode_us", "decode_to_present_us",
                "capture_to_present_us"):
        vals = sorted(
            int(r[key]) for r in rows
            if int(r[key]) < skew_us and int(r[key]) >= 0)
        if len(vals) <= warmup:
            continue
        warm = vals[warmup:]
        n = len(warm)
        out["phases"][key] = {
            "p50_us": warm[n // 2],
            "p95_us": warm[int(n * 0.95)],
            "max_us": warm[-1],
            "mean_us": int(statistics.mean(warm)),
            "n": n,
        }
    out["rows_kept"] = max(
        (p["n"] for p in out["phases"].values()), default=0)
    return out


# ── Per-combo runner ────────────────────────────────────────────


@dataclass
class ComboResult:
    combo: Combo
    status: str = "pending"     # ok | skipped | failed
    reason: str = ""
    stats: dict = field(default_factory=dict)
    csv_path: str = ""


def _allocate_port(base: int, idx: int) -> int:
    return base + idx


def run_combo(combo: Combo, cfg: dict, args, idx: int) -> ComboResult:
    defs = cfg["defaults"]
    res = ComboResult(combo=combo)
    sid = f"m{idx}"
    port = _allocate_port(defs["port_base"], idx)
    csv_local = str(
        Path(args.csv_dir) /
        f"lat_{combo.direction}_"
        f"{combo.src.name}_{combo.src_encoder}_"
        f"{combo.sink.name}_{combo.sink_decoder}.csv")

    # Capability sanity (the config claimed the backend; if
    # --probe-strict was set, we also ran helper_caps earlier and
    # cached it). For v1 we trust the config; issue #50 Phase 1a
    # note: strict probe is a Phase 3 hardening item.
    if combo.src_encoder not in combo.src.encoders():
        res.status = "skipped"
        res.reason = (f"{combo.src.name} doesn't declare support "
                      f"for encoder={combo.src_encoder}")
        return res
    if combo.sink_decoder not in combo.sink.decoders():
        res.status = "skipped"
        res.reason = (f"{combo.sink.name} doesn't declare support "
                      f"for decoder={combo.sink_decoder}")
        return res

    # Teardown anything left over from a previous combo.
    combo.src.kill_all()
    if combo.sink is not combo.src:
        combo.sink.kill_all()
    try:
        os.unlink(csv_local)
    except FileNotFoundError:
        pass

    # Environment for each role.
    src_env = {"UNIO_PIPE_FORCE_ENCODER": combo.src_encoder}
    sink_env = {"UNIO_PIPE_FORCE_DECODER": combo.sink_decoder}
    # Latency CSV: always written on the sink host. If the sink is
    # remote, we scp it back in fetch_latency_csv.
    if isinstance(combo.sink, LocalLinuxHost):
        sink_env["UNIO_PIPE_LATENCY_CSV"] = csv_local
    else:
        sink_env["UNIO_PIPE_LATENCY_CSV"] = combo.sink.latency_csv_path

    # Launch order: sink first (needs to listen before src dials).
    combo.sink.launch("sink", sink_env)
    if not combo.sink.wait_ready("sink", timeout_s=6.0):
        res.status = "failed"
        res.reason = f"sink helper didn't come up on {combo.sink.name}"
        combo.sink.kill_all()
        return res

    # Loopback optimisation: if src and sink are the same host,
    # launch the src under a different role on the same machine.
    # (Two socket / pipe endpoints, one OS.)
    if combo.is_loopback:
        combo.src.launch("src", src_env)
    else:
        combo.src.launch("src", src_env)
    if not combo.src.wait_ready("src", timeout_s=6.0):
        res.status = "failed"
        res.reason = f"src helper didn't come up on {combo.src.name}"
        combo.src.kill_all()
        if combo.sink is not combo.src:
            combo.sink.kill_all()
        return res

    try:
        # Wire up. The peer address for start_outbound depends on
        # topology: loopback uses 127.0.0.1; cross-machine uses
        # the sink's declared address.
        peer_addr = ("127.0.0.1" if combo.is_loopback
                     else combo.sink.address)
        r_in = combo.sink.rpc("sink", {
            "cmd": "start_inbound", "stream_id": sid,
            "listen_port": port,
            "window_w": 320, "window_h": 180,
        })
        if "error" in r_in:
            res.status = "failed"
            res.reason = f"start_inbound error: {r_in['error']}"
            return res
        time.sleep(0.3)
        out_cmd = {
            "cmd": "start_outbound", "stream_id": sid,
            "peer_addr": peer_addr, "peer_port": port,
            "width": defs["width"], "height": defs["height"],
            "fps": defs["fps"],
            "capture_x": 0, "capture_y": 0,
        }
        # Linux source needs a monitor hint; harmless on Windows.
        if combo.src.os == "linux":
            out_cmd["monitor_source"] = os.environ.get("DISPLAY", ":1")
        r_out = combo.src.rpc("src", out_cmd)
        if "error" in r_out:
            res.status = "failed"
            res.reason = f"start_outbound error: {r_out['error']}"
            return res

        # Stream.
        time.sleep(args.duration)

        # Status probe (useful to confirm frames actually flowed).
        try:
            sink_status = combo.sink.rpc(
                "sink", {"cmd": "helper_status"}, timeout_s=5.0)
            per = sink_status.get("per_stream", [{}])[0]
            decoded = per.get("frames_decoded", 0)
            if decoded == 0:
                res.status = "failed"
                res.reason = "sink decoded 0 frames"
                return res
        except Exception as e:
            res.status = "failed"
            res.reason = f"sink helper_status RPC: {e}"
            return res

        # Stop streams, fetch CSV, summarise.
        try:
            combo.src.rpc("src", {"cmd": "stop", "stream_id": sid})
            combo.sink.rpc("sink", {"cmd": "stop", "stream_id": sid})
        except Exception:
            pass  # best-effort

        if not isinstance(combo.sink, LocalLinuxHost):
            combo.sink.fetch_latency_csv(csv_local)
        res.csv_path = csv_local
        res.stats = summarise_csv(csv_local, defs["warmup_frames"])
        if not res.stats.get("phases"):
            res.status = "failed"
            res.reason = "no latency rows parsed (CSV empty or all skew-filtered)"
            return res
        res.status = "ok"
        return res
    finally:
        combo.src.kill_all()
        if combo.sink is not combo.src:
            combo.sink.kill_all()


# ── Report ──────────────────────────────────────────────────────


def _fmt_ms(us: Optional[int]) -> str:
    if us is None:
        return "    -   "
    return f"{us/1000:7.2f}"


def print_table(results: list[ComboResult]) -> None:
    print("")
    print("=" * 110)
    print(f"{'direction':<8} {'src':<24} {'sink':<24} "
          f"{'cap→dec p50/p95':>18} {'dec→pre p50/p95':>18} "
          f"{'glass p50/p95':>18} {'status':>8}")
    print("-" * 110)
    for r in results:
        c = r.combo
        src = f"{c.src.name}:{c.src_encoder}"
        sink = f"{c.sink.name}:{c.sink_decoder}"
        if r.status == "ok":
            p = r.stats.get("phases", {})
            cd = p.get("capture_to_decode_us", {})
            dp = p.get("decode_to_present_us", {})
            cp = p.get("capture_to_present_us", {})
            cd_s = f"{_fmt_ms(cd.get('p50_us'))}/{_fmt_ms(cd.get('p95_us'))}"
            dp_s = f"{_fmt_ms(dp.get('p50_us'))}/{_fmt_ms(dp.get('p95_us'))}"
            cp_s = f"{_fmt_ms(cp.get('p50_us'))}/{_fmt_ms(cp.get('p95_us'))}"
            print(f"{c.direction:<8} {src:<24} {sink:<24} "
                  f"{cd_s:>18} {dp_s:>18} {cp_s:>18} {'ok':>8}")
        else:
            print(f"{c.direction:<8} {src:<24} {sink:<24} "
                  f"{'':>18} {'':>18} {'':>18} {r.status:>8}")
            if r.reason:
                print(f"         └─ {r.reason}")
    print("=" * 110)


# ── Main ────────────────────────────────────────────────────────


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    here = Path(__file__).resolve().parent
    ap.add_argument("--config", default=str(here / "hosts.yaml"),
                    help="hosts.yaml path (default: tools/hosts.yaml)")
    ap.add_argument("--direction",
                    choices=("lin2lin", "win2win", "lin2win", "win2lin"),
                    help="restrict to one direction")
    ap.add_argument("--src", help="restrict to one src encoder")
    ap.add_argument("--sink", help="restrict to one sink decoder")
    ap.add_argument("--src-host", help="restrict to one src host (by name)")
    ap.add_argument("--sink-host", help="restrict to one sink host (by name)")
    ap.add_argument("--duration", type=float, default=None,
                    help="seconds per combo (default: config)")
    ap.add_argument("--probe", action="store_true",
                    help="probe helper_caps on every host and exit")
    ap.add_argument("--list", action="store_true",
                    help="list the combos that would run, then exit")
    ap.add_argument("--csv-dir", default="/tmp",
                    help="where to drop per-combo latency CSVs")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    cfg = load_config(Path(args.config))
    hosts = build_hosts(cfg)

    if args.duration is None:
        args.duration = cfg["defaults"]["duration_s"]

    if args.probe:
        print("=" * 60)
        print("Capability probe — every declared host")
        print("=" * 60)
        rc = 0
        for name, h in hosts.items():
            print(f"\n[{name}] {h.role}/{h.os} ({h.address})")
            caps = h.probe_caps()
            if "error" in caps:
                print(f"  ERROR: {caps['error']}")
                rc = 1
                continue
            streaming = caps.get("streaming", {})
            print(f"  available={streaming.get('available')}, "
                  f"detected={streaming.get('detected_gpus')}")
            # Verify claims match probe.
            for k in ("encoders", "decoders"):
                claimed = set(h.supports.get(k, []))
                # The probe output is the capability *status*, not a
                # direct list; for v1 just trust the boolean.
            print(f"  config claims: "
                  f"encoders={h.encoders()}, decoders={h.decoders()}")
        return rc

    combos = filter_combos(enumerate_combos(hosts), args)
    if not combos:
        print("(no combos match the filter)", file=sys.stderr)
        return 2

    if args.list:
        print(f"{len(combos)} combo(s) would run:")
        for c in combos:
            print(f"  {c.label}")
        return 0

    print(f"Running {len(combos)} combo(s), "
          f"{args.duration:.0f}s each, "
          f"{cfg['defaults']['width']}x{cfg['defaults']['height']}@"
          f"{cfg['defaults']['fps']}")

    results: list[ComboResult] = []
    for i, combo in enumerate(combos):
        print(f"\n[{i+1}/{len(combos)}] {combo.label}")
        r = run_combo(combo, cfg, args, i)
        if r.status == "ok":
            cp = r.stats.get("phases", {}).get(
                "capture_to_present_us", {})
            print(f"  ok: glass p50={cp.get('p50_us',0)/1000:.2f} ms "
                  f"p95={cp.get('p95_us',0)/1000:.2f} ms "
                  f"n={cp.get('n', 0)}")
        else:
            print(f"  {r.status}: {r.reason}")
        results.append(r)
        if i < len(combos) - 1:
            time.sleep(cfg["defaults"]["cooldown_s"])

    print_table(results)
    fail = sum(1 for r in results if r.status == "failed")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

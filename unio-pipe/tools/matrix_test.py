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
import threading
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


def _ssh_common_args(key: str) -> list[str]:
    """SSH options common to every call. ControlMaster lets us
    reuse one TCP + TLS handshake across many short RPCs — drops
    per-call RTT from ~500 ms (fresh auth) to the real network
    round-trip (~10-20 ms on LAN). Matters a lot for the clock-
    skew measurement: measurement precision is ±RTT/2, and RTT
    noise is the error floor on the skew estimate.

    ControlPersist=5m keeps the master alive between matrix_test
    invocations within a ~5 min window, which makes iterative
    dev loops (edit → rerun → edit) fast."""
    here = Path(__file__).resolve().parent
    cm_dir = here / ".ssh-cm"
    cm_dir.mkdir(exist_ok=True)
    return [
        "-o", "IdentitiesOnly=yes",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=5",
        "-o", "ControlMaster=auto",
        "-o", f"ControlPath={cm_dir}/cm-%r@%h:%p",
        "-o", "ControlPersist=5m",
        "-i", key,
    ]


def _ssh(ssh_host: str, key: str, cmd: str,
         check: bool = True, timeout_s: int = 30) -> str:
    full = ["ssh"] + _ssh_common_args(key) + [ssh_host, cmd]
    r = subprocess.run(full, capture_output=True, timeout=timeout_s)
    out = r.stdout.decode("utf-8", errors="replace") if r.stdout else ""
    err = r.stderr.decode("utf-8", errors="replace") if r.stderr else ""
    if check and r.returncode != 0:
        raise RuntimeError(
            f"ssh failed (rc={r.returncode}): {cmd}\n"
            f"stdout={out}\nstderr={err}")
    return out + err


def _scp_from(ssh_host: str, key: str, remote: str, local: str) -> int:
    # OpenSSH on Windows needs forward-slash remote paths. scp
    # shares the SSH ControlMaster socket when present, so this
    # benefits from the same per-call RTT drop as _ssh.
    remote = remote.replace("\\", "/")
    r = subprocess.run(
        ["scp"] + _ssh_common_args(key)
        + [f"{ssh_host}:{remote}", local],
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

    @abstractmethod
    def binary_mtime_unix(self) -> Optional[int]:
        """Return the helper binary's mtime as Unix-epoch seconds,
        or None if the binary can't be stat'd (host unreachable,
        path wrong, etc.). Used by the pre-flight build-freshness
        check to verify every host is running code from at least
        the orchestrator's current git HEAD."""

    def sync_code(self, local_source_root: Path) -> tuple[bool, str]:
        """Push the orchestrator's unio-pipe/ tree to this host
        and rebuild. Called by --sync before the matrix runs so
        every host is guaranteed bit-identical. Default: no-op
        for local hosts. Remote hosts override.

        Returns (ok, message)."""
        return True, "local host (no sync needed)"

    def deploy_prebuilt(self, local_dist_root: Path
                        ) -> tuple[bool, str]:
        """Copy a pre-built binary + its runtime DLLs/.sos from
        the orchestrator's local_dist_root/<target> to this
        host's binary path. `target` is derived per subclass
        ('linux-x64' / 'win-x64'). Called by --use-prebuilt as an
        alternative to --sync: the orchestrator is responsible
        for producing dist/<target>/ (e.g. via
        packaging/docker/build-{linux,win}.sh on the
        `containerized-builds` branch); this method just
        dispatches the artefacts.

        Default: raises NotImplementedError — subclasses must
        override to say where the artefacts belong."""
        raise NotImplementedError(
            f"deploy_prebuilt not implemented for {type(self).__name__}")

    def sync_clock(self, ntp_server: str) -> tuple[bool, str]:
        """Trigger an NTP resync on this host against `ntp_server`.
        Remote hosts override; the orchestrator (local) also needs
        to resync its own clock so skew measurements are against
        a fresh time, not a drifted local reference.

        Returns (ok, message)."""
        return True, "local host clock sync not implemented here"

    def unix_time_ms(self) -> tuple[Optional[int], int]:
        """Return (remote_unix_time_ms, rtt_ms). For local hosts
        this is just time.time() * 1000 with RTT=0. For remote
        hosts, measured via SSH with the timestamp bracketed
        between local send + receive so we can bound the
        measurement error as ±RTT/2."""
        ms = int(time.time() * 1000)
        return ms, 0

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

    def binary_mtime_unix(self) -> Optional[int]:
        try:
            return int(os.stat(self.binary).st_mtime)
        except (OSError, FileNotFoundError):
            return None

    def deploy_prebuilt(self, local_dist_root: Path
                        ) -> tuple[bool, str]:
        """Copy dist/linux-x64/unio-pipe into the dir that holds
        this host's `binary` path. For the orchestrator this is
        usually ./unio-pipe/build/ (default hosts.yaml) — we
        overwrite the same file the source-rebuild path would have
        produced, so everything else (RPC socket naming, capability
        probe, etc.) keeps working.

        Ship list is exactly one file: our binary. msquic and
        openssl3 are statically linked (see unio-pipe/CMakeLists
        QUIC_BUILD_SHARED=OFF), so there's no libmsquic.so.2 to
        chase and no LD_LIBRARY_PATH dance."""
        src = local_dist_root / "linux-x64"
        src_bin = src / "unio-pipe"
        if not src_bin.exists():
            return False, (
                f"{src_bin} not found — run "
                f"packaging/docker/build-linux.sh first")
        dst_bin = Path(self.binary)
        dst_bin.parent.mkdir(parents=True, exist_ok=True)
        import shutil
        try:
            shutil.copy2(src_bin, dst_bin)
        except Exception as e:
            return False, f"copy failed: {e}"
        return True, f"deployed {src_bin.name} → {dst_bin.parent}"

    def sync_clock(self, ntp_server: str) -> tuple[bool, str]:
        # Linux has two common stacks: chrony (chronyc) or
        # systemd-timesyncd (timedatectl). Try both, silently.
        # Neither is required — timesyncd can't be forced to sync
        # without root, so we fall back to reporting what
        # timedatectl knows and let the actual skew measurement
        # decide whether the clock is close enough.
        import shutil
        if shutil.which("chronyc"):
            r = subprocess.run(
                ["chronyc", "-a", "makestep"],
                capture_output=True, text=True, timeout=5)
            if r.returncode == 0:
                return True, "chronyc makestep succeeded"
            return True, f"chronyc makestep rc={r.returncode} (likely non-priv; skew check will still run)"
        if shutil.which("timedatectl"):
            r = subprocess.run(
                ["timedatectl", "timesync-status"],
                capture_output=True, text=True, timeout=5)
            synced = "System clock synchronized: yes" in r.stdout
            if synced:
                return True, "systemd-timesyncd shows synchronized"
            # Try to restart the service to force a poll.
            restart = subprocess.run(
                ["sudo", "-n", "systemctl", "restart",
                 "systemd-timesyncd"],
                capture_output=True, timeout=5)
            if restart.returncode == 0:
                return True, "systemd-timesyncd restarted"
            return True, "systemd-timesyncd present, couldn't force resync"
        return False, "no chronyc or timedatectl found"

    def unix_time_ms(self) -> tuple[Optional[int], int]:
        return int(time.time() * 1000), 0


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
        self.source_root = cfg.get("source_root")
        self.build_cmd = cfg.get("build_cmd")
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

    def binary_mtime_unix(self) -> Optional[int]:
        # PowerShell: LastWriteTimeUtc.Ticks is 100-ns since 0001;
        # convert to Unix epoch seconds.
        try:
            out = _ssh(
                self.ssh_host, self.ssh_key,
                f"powershell -NoProfile -Command "
                f"\"(Get-Item '{self.binary}').LastWriteTimeUtc.Ticks\"",
                check=False, timeout_s=10)
            ticks = int(out.strip().splitlines()[-1])
            # 621355968000000000 ticks = 1970-01-01 in 0001-based
            # LastWriteTimeUtc.Ticks; every 10^7 ticks = 1 second.
            return int((ticks - 621355968000000000) // 10_000_000)
        except Exception:
            return None

    def deploy_prebuilt(self, local_dist_root: Path
                        ) -> tuple[bool, str]:
        """scp dist/win-x64/unio-pipe.exe to the directory that
        holds this host's `binary` path. Target: Windows remote
        via the SSH ControlMaster-reused connection.

        Single-file ship: msquic + openssl3 + libvpl are
        statically linked into the exe, MSVC CRT is /MT. The
        Intel oneVPL dispatcher code inside the exe runtime-
        loads libmfxhw64.dll from the installed Intel driver —
        the target has that DLL via its own graphics driver,
        not from us. The glob-for-*.dll path used to exist when
        libvpl shipped as a DLL; with libvpl now static there's
        nothing to scp alongside."""
        src = local_dist_root / "win-x64"
        src_bin = src / "unio-pipe.exe"
        if not src_bin.exists():
            return False, (
                f"{src_bin} not found — run "
                f"packaging/docker/build-win.sh first")
        # Remote destination directory (forward slashes for scp).
        dst_bin = self.binary.replace("\\", "/")
        dst_dir = dst_bin.rsplit("/", 1)[0]
        # Make sure remote dir exists.
        try:
            _ssh(self.ssh_host, self.ssh_key,
                 f'if not exist "{self.binary.rsplit(chr(92), 1)[0]}" '
                 f'mkdir "{self.binary.rsplit(chr(92), 1)[0]}"',
                 check=False, timeout_s=10)
        except Exception:
            pass
        r = subprocess.run(
            ["scp"] + _ssh_common_args(self.ssh_key)
            + [str(src_bin), f"{self.ssh_host}:{dst_dir}/"],
            capture_output=True)
        if r.returncode != 0:
            return False, (f"scp failed (rc={r.returncode}): "
                           f"{r.stderr.decode('utf-8','replace')[:200]}")
        return True, f"deployed {src_bin.name} → {dst_dir}/"

    def sync_clock(self, ntp_server: str) -> tuple[bool, str]:
        # Point Windows Time at the shared NTP server, resync, read
        # status. The /manualpeerlist change plus /syncfromflags=manual
        # pins this persistently — not ideal for the user's normal
        # use, but we don't reset it because Windows Time is tolerant
        # of re-pointing and the NTP server we pick (time.cloudflare.com)
        # is perfectly fine as a long-term source too.
        # Needs admin rights for /config; on a non-admin SSH session
        # the /config silently fails — /resync /force still works
        # though, just from whatever source was configured before.
        cmd = (
            "powershell -NoProfile -Command \""
            "Start-Service w32time -ErrorAction SilentlyContinue | Out-Null;"
            f"w32tm /config /manualpeerlist:{ntp_server} "
            "/syncfromflags:manual /update 2>&1 | Out-Null;"
            "w32tm /resync /force 2>&1 | Out-Null;"
            "Start-Sleep -Seconds 2;"
            "w32tm /query /status 2>&1\""
        )
        try:
            out = _ssh(self.ssh_host, self.ssh_key, cmd,
                      check=False, timeout_s=20)
        except Exception as e:
            return False, f"w32tm resync failed: {e}"
        # Success signature: "Last Successful Sync Time" present.
        if "Last Successful Sync" in out:
            return True, f"w32tm resynced against {ntp_server}"
        tail = " / ".join(
            ln.strip() for ln in out.splitlines()[-3:]
            if ln.strip())
        return False, f"w32tm status unclear: {tail[:200]}"

    def unix_time_ms(self) -> tuple[Optional[int], int]:
        # PowerShell DateTimeOffset.UtcNow.ToUnixTimeMilliseconds
        # — millisecond precision. Bracket the SSH call with local
        # monotonic timestamps so we can report RTT and thus the
        # precision bound on the skew estimate.
        cmd = ("powershell -NoProfile -Command "
               "\"[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()\"")
        t0 = time.monotonic_ns() // 1_000_000
        local_t0_wall = int(time.time() * 1000)
        try:
            out = _ssh(self.ssh_host, self.ssh_key, cmd,
                      check=False, timeout_s=10)
        except Exception:
            return None, 0
        t1 = time.monotonic_ns() // 1_000_000
        local_t1_wall = int(time.time() * 1000)
        rtt = t1 - t0
        try:
            remote_ms = int(out.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None, rtt
        # Return (remote time as it was at the midpoint of the RTT
        # window, RTT). Caller compares this to its own wall clock
        # midpoint for skew — our caller already holds its wall-time
        # bracket, so we hand back the raw remote timestamp and let
        # the measurement code do the comparison.
        _ = local_t0_wall, local_t1_wall  # kept for potential log
        return remote_ms, rtt

    def sync_code(self, local_source_root: Path) -> tuple[bool, str]:
        """tar-pipe the unio-pipe/ tree to Diana and rebuild.
        Excludes build/ (rebuilt) and the tools/hosts.yaml (per-
        host secret) but includes src/ / include/ / CMakeLists /
        the rest of tools/ so the remote has everything it needs
        to produce a binary that matches the orchestrator's HEAD."""
        if not self.source_root or not self.build_cmd:
            return False, (
                "hosts.yaml is missing source_root / build_cmd "
                "— add them or don't use --sync for this host")

        # Make sure the source_root exists (cmd.exe 'if not exist').
        try:
            _ssh(self.ssh_host, self.ssh_key,
                 f'if not exist "{self.source_root}" '
                 f'mkdir "{self.source_root}"')
        except Exception as e:
            return False, f"mkdir source_root failed: {e}"

        excludes = [
            "--exclude", "build",
            "--exclude", "__pycache__",
            "--exclude", "*.pyc",
            "--exclude", "tools/hosts.yaml",   # per-lab, not universal
            "--exclude", "tools/.ssh-cm",      # SSH ControlMaster sockets
            "--exclude", ".DS_Store",
        ]
        tar_cmd = ["tar", "-czf", "-"] + excludes + ["."]
        # Windows' libarchive tar (Win10 built-in) doesn't support
        # --overwrite. Clear the read-only bit across the tree
        # first — some files inherit it from the initial scp/build
        # and then libarchive refuses to unlink them on extract.
        remote_cmd = (
            f'cd /d "{self.source_root}" & '
            f'attrib -R * /S /D >nul 2>nul & '
            f'tar -xzf -')
        ssh_cmd = ["ssh"] + _ssh_common_args(self.ssh_key) + [
            self.ssh_host, remote_cmd,
        ]
        # Stream tar stdout → ssh stdin without buffering.
        try:
            with subprocess.Popen(
                    tar_cmd, cwd=str(local_source_root),
                    stdout=subprocess.PIPE) as tar:
                r = subprocess.run(ssh_cmd, stdin=tar.stdout,
                                   capture_output=True, timeout=120)
                tar.stdout.close()
                tar.wait()
                if r.returncode != 0:
                    return False, (
                        f"tar-over-ssh returncode={r.returncode}: "
                        f"{r.stderr.decode('utf-8', 'replace')[:300]}")
        except subprocess.TimeoutExpired:
            return False, "tar-over-ssh timed out (120 s)"
        except Exception as e:
            return False, f"tar-over-ssh failed: {e}"

        # Rebuild. cmake --build alone doesn't re-read CMakeLists
        # after an edit unless the source is newer than the cached
        # build files — with the tar sync that's inconsistent, so
        # explicitly reconfigure first, then build.
        #
        # FETCHCONTENT_FULLY_DISCONNECTED=ON stops a CMakeLists
        # edit from triggering a multi-minute msquic re-clone.
        #
        # UNIO_BUILD_COMMIT is handed across from the orchestrator
        # because the remote's extracted source tree has no .git/
        # metadata — cmake's git rev-parse there would fail and
        # leave the field "unknown", defeating the whole point of
        # the cross-host commit-consistency check.
        cmake_exe = '"C:\\Strawberry\\c\\bin\\cmake.EXE"'
        head = _git_head_info() or {}
        commit = head.get("sha", "unknown")
        # Mark dirty so the remote binary flags its provenance.
        dirty = subprocess.run(
            ["git", "status", "--porcelain"],
            capture_output=True, text=True).stdout.strip()
        if dirty:
            commit = f"{commit}-dirty"
        reconfigure = (
            f'{cmake_exe} -S "{self.source_root}" '
            f'-B "{self.source_root}\\build" '
            f'-DFETCHCONTENT_FULLY_DISCONNECTED=ON '
            f'-DUNIO_BUILD_COMMIT={commit}')
        full_cmd = f'{reconfigure} && {self.build_cmd}'
        ssh_cmd = ["ssh"] + _ssh_common_args(self.ssh_key) + [
            self.ssh_host, full_cmd,
        ]
        try:
            p = subprocess.Popen(
                ssh_cmd, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True)
            tail: list[str] = []
            for line in p.stdout:
                line = line.rstrip()
                tail.append(line)
                if len(tail) > 30:
                    tail.pop(0)
                if any(k in line for k in ("error C", "error MSB",
                                            "fatal error",
                                            "Linking", "Copying")):
                    print(f"    [remote build] {line}")
            rc = p.wait(timeout=900)
        except subprocess.TimeoutExpired:
            p.kill()
            return False, "remote build timed out (15 min)"
        except Exception as e:
            return False, f"remote build failed: {e}"
        if rc != 0:
            tail_s = "\n".join(tail[-10:])
            return False, f"remote build exit {rc}:\n{tail_s}"
        return True, "synced + rebuilt"

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
                  skew_filter_s: float = 1000.0,
                  correction_us: int = 0) -> dict:
    """Parse the helper's latency CSV, drop warmup rows, filter
    cross-machine NTP-skew artefacts (> skew_filter_s apart),
    apply an optional skew correction.

    correction_us: signed microseconds to ADD to every
    cap→dec / cap→pre row. For a cross-machine combo where the
    sink's clock is `+sink_skew` ahead of the orchestrator and
    the src's clock is `+src_skew` ahead, the measured
    decode_ns - capture_ns reads as (actual_latency + sink_skew
    - src_skew) — pass correction_us = (src_skew - sink_skew)
    in microseconds to recover the real latency. Loopback combos
    pass 0 (same clock both sides). dec→pre is NEVER corrected:
    it's always measured with a single host's clock.

    The caller also gets `correction_us` echoed back in the
    result so consumers know whether the phase numbers have been
    adjusted and by how much."""
    import csv
    import statistics
    out = {
        "rows_raw": 0, "rows_kept": 0,
        "phases": {}, "correction_us": correction_us,
    }
    try:
        rows = list(csv.DictReader(open(csv_path)))
    except FileNotFoundError:
        return out
    out["rows_raw"] = len(rows)
    # Raw skew filter (before correction) — reject rows that are
    # order-of-magnitude-nonsense regardless (prev run's CSV
    # leftover, broken timestamps, etc.).
    #
    # The helper writes latency deltas as unsigned 64-bit
    # microseconds. When the sink clock is behind the src clock,
    # `decode_ns - capture_ns` underflows to a huge positive
    # value. Interpret anything above 2^63 as the signed wrap-
    # around: this recovers the real (negative) delta so the
    # skew correction can do its job in both directions.
    raw_skew_us = int(skew_filter_s * 1_000_000)
    correcting = correction_us != 0

    # The helper writes deltas as uint64 microseconds. Two ways
    # a real-world negative shows up:
    #   1. ns wrap → divided by 1000 → value ≈ 2^64/1000 ≈ 1.844e16.
    #      Recover via  v - (1<<64)//1000.
    #   2. Full u64 wrap (rare — only if the original uint64 ns
    #      value was already near 2^64 when computed). Recover
    #      via  v - (1<<64).
    # Real frame-latency µs values never exceed ~10^9 (16 minutes),
    # so any v > 2^50 is unambiguously a wrap artifact and gets the
    # signed interpretation. The two wrap points are separated by
    # 3 orders of magnitude, so picking the closer one is exact.
    _US_WRAP_NS  = (1 << 64) // 1000  # ≈ 1.844e16
    _US_WRAP_U64 = 1 << 64            # ≈ 1.844e19

    def _signed_us(s: str) -> int:
        v = int(s)
        if v < (1 << 50):
            return v
        cand_ns_wrap = v - _US_WRAP_NS
        cand_u64     = v - _US_WRAP_U64
        return cand_ns_wrap if abs(cand_ns_wrap) < abs(cand_u64) else cand_u64

    for key in ("capture_to_decode_us", "decode_to_present_us",
                "capture_to_present_us"):
        adjust = correction_us if "capture_to_" in key else 0
        raw = [_signed_us(r[key]) for r in rows
               if abs(_signed_us(r[key])) < raw_skew_us]
        corrected = [v + adjust for v in raw]
        # Without correction: drop negatives (always jitter /
        # bad rows). With correction: keep everything — the
        # residual skew-measurement error (±RTT/2) can push
        # low-latency rows slightly negative, and discarding
        # them biases p50 / p95 upward by skipping the fastest
        # frames. Leaving them in keeps the median honest.
        if correcting:
            vals = sorted(corrected)
        else:
            vals = sorted(v for v in corrected if v >= 0)
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


def run_combo(combo: Combo, cfg: dict, args, idx: int,
              skew_ms_by_host: Optional[dict] = None) -> ComboResult:
    defs = cfg["defaults"]
    res = ComboResult(combo=combo)
    # Skew correction: measured inter-host drift relative to the
    # orchestrator. Loopback → 0 (same clock). Cross-machine →
    # (src_skew − sink_skew) because decode_ns - capture_ns reads
    # as (actual + sink_skew − src_skew), so adding
    # (src_skew − sink_skew) back recovers the real latency.
    skew_map = skew_ms_by_host or {}
    src_skew_ms = skew_map.get(combo.src.name, 0) or 0
    sink_skew_ms = skew_map.get(combo.sink.name, 0) or 0
    correction_us = (src_skew_ms - sink_skew_ms) * 1000
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

        # Status probe. Confirms frames actually flowed AND that
        # the encoder / decoder the helpers picked matches what we
        # asked for: on hosts where the requested backend isn't
        # compiled in, the helper silently falls back to a
        # different one (e.g. FORCE_ENCODER=nvenc-linux on main
        # today just uses VA-API — no warning). Catching that
        # divergence here turns a misleading "ok" into a visible
        # "backend-substituted" failure.
        try:
            sink_status = combo.sink.rpc(
                "sink", {"cmd": "helper_status"}, timeout_s=5.0)
            src_status = combo.src.rpc(
                "src", {"cmd": "helper_status"}, timeout_s=5.0)
            sink_per = sink_status.get("per_stream", [{}])[0]
            src_per  = src_status.get("per_stream", [{}])[0]
            decoded = sink_per.get("frames_decoded", 0)
            if decoded == 0:
                res.status = "failed"
                res.reason = "sink decoded 0 frames"
                return res
            actual_enc = src_per.get("encoder", "")
            actual_dec = sink_per.get("decoder", "")
            # Helper names are vendor-tagged (e.g. "vaapi-h264",
            # "nvenc-h264"); accept a prefix match so we don't
            # lock to exact strings that may evolve.
            if actual_enc and not actual_enc.startswith(
                    combo.src_encoder):
                res.status = "failed"
                res.reason = (f"src substituted encoder: "
                              f"asked={combo.src_encoder!r} "
                              f"got={actual_enc!r} "
                              f"(backend not compiled in?)")
                return res
            if actual_dec and not actual_dec.startswith(
                    combo.sink_decoder):
                res.status = "failed"
                res.reason = (f"sink substituted decoder: "
                              f"asked={combo.sink_decoder!r} "
                              f"got={actual_dec!r} "
                              f"(backend not compiled in?)")
                return res
        except Exception as e:
            res.status = "failed"
            res.reason = f"helper_status RPC: {e}"
            return res

        # Stop streams, fetch CSV, summarise.
        try:
            combo.src.rpc("src", {"cmd": "stop", "stream_id": sid})
            combo.sink.rpc("sink", {"cmd": "stop", "stream_id": sid})
        except Exception:
            pass  # best-effort

        if not isinstance(combo.sink, LocalLinuxHost):
            fetched = combo.sink.fetch_latency_csv(csv_local)
            if not fetched:
                res.status = "failed"
                res.reason = ("sink latency CSV didn't come back "
                              "(scp failed or file missing on remote)")
                return res
        res.csv_path = csv_local
        res.stats = summarise_csv(
            csv_local, defs["warmup_frames"],
            correction_us=correction_us)
        if res.stats.get("rows_raw", 0) == 0:
            res.status = "failed"
            res.reason = "latency CSV present but empty (decoder wrote 0 rows)"
            return res
        if not res.stats.get("phases"):
            res.status = "failed"
            res.reason = (
                f"{res.stats['rows_raw']} CSV rows but all were "
                f"rejected by the raw >1000 s skew filter — the "
                f"NTP skew correction fixes up to ~100 s drift, "
                f"beyond that indicates a host-level timestamp bug "
                f"(e.g. capture_wgc.cpp QPC overflow, fixed in "
                f"PR #49 but not yet merged to main)")
            return res
        res.status = "ok"
        return res
    finally:
        combo.src.kill_all()
        if combo.sink is not combo.src:
            combo.sink.kill_all()


# ── Report ──────────────────────────────────────────────────────


# JSON schema version. Bump on breaking changes to `results_to_json`
# so old baselines don't silently compare wrong against new runs.
SCHEMA_VERSION = 1


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
    any_correction = False
    for r in results:
        c = r.combo
        src = f"{c.src.name}:{c.src_encoder}"
        sink = f"{c.sink.name}:{c.sink_decoder}"
        if r.status == "ok":
            p = r.stats.get("phases", {})
            corr = r.stats.get("correction_us", 0)
            if corr:
                any_correction = True
            cd = p.get("capture_to_decode_us", {})
            dp = p.get("decode_to_present_us", {})
            cp = p.get("capture_to_present_us", {})
            cd_s = f"{_fmt_ms(cd.get('p50_us'))}/{_fmt_ms(cd.get('p95_us'))}"
            dp_s = f"{_fmt_ms(dp.get('p50_us'))}/{_fmt_ms(dp.get('p95_us'))}"
            cp_s = f"{_fmt_ms(cp.get('p50_us'))}/{_fmt_ms(cp.get('p95_us'))}"
            tag = "ok*" if corr else "ok"
            print(f"{c.direction:<8} {src:<24} {sink:<24} "
                  f"{cd_s:>18} {dp_s:>18} {cp_s:>18} {tag:>8}")
        else:
            print(f"{c.direction:<8} {src:<24} {sink:<24} "
                  f"{'':>18} {'':>18} {'':>18} {r.status:>8}")
            if r.reason:
                print(f"         └─ {r.reason}")
    print("=" * 110)
    if any_correction:
        print("  * cap→* phases adjusted for measured inter-host "
              "clock skew (see run.build_info.clock_sync)")


# ── JSON output ─────────────────────────────────────────────────


def results_to_json(results: list[ComboResult], hosts: dict[str, Host],
                    cfg: dict, args,
                    preflight_info: Optional[dict] = None) -> dict:
    """Structured dump of a run. Schema is stable across SCHEMA_VERSION
    bumps only — add fields freely, but renaming or removing
    breaks backwards compat with stored baselines.

    Key invariants consumers (CI, baseline diff) depend on:
      - combos[].key is a stable string ID for diff matching;
        changing it invalidates every stored baseline.
      - phases[].p50_us / p95_us units are microseconds (integer).
      - A "skipped" or "failed" combo omits phases entirely.
    """
    import datetime
    return {
        "schema_version": SCHEMA_VERSION,
        "run": {
            "started_at": datetime.datetime.now(
                datetime.timezone.utc).isoformat(timespec="seconds"),
            "duration_per_combo_s": args.duration,
            "width": cfg["defaults"]["width"],
            "height": cfg["defaults"]["height"],
            "fps": cfg["defaults"]["fps"],
            "warmup_frames": cfg["defaults"]["warmup_frames"],
            "config_path": str(Path(args.config).resolve()),
            "orchestrator_hostname": socket.gethostname(),
            "build_info": preflight_info or {},
        },
        "hosts": {
            name: {
                "role": h.role, "os": h.os, "address": h.address,
                "supports": h.supports,
            } for name, h in hosts.items()
        },
        "combos": [
            {
                "key": _combo_key(r.combo),
                "direction": r.combo.direction,
                "src_host": r.combo.src.name,
                "src_encoder": r.combo.src_encoder,
                "sink_host": r.combo.sink.name,
                "sink_decoder": r.combo.sink_decoder,
                "status": r.status,
                "reason": r.reason,
                "rows_raw": r.stats.get("rows_raw", 0),
                "rows_kept": r.stats.get("rows_kept", 0),
                "phases": r.stats.get("phases", {}),
            } for r in results
        ],
    }


def _combo_key(c: Combo) -> str:
    """Stable identifier for a combo, used to match current↔baseline."""
    return (f"{c.direction}/"
            f"{c.src.name}:{c.src_encoder}/"
            f"{c.sink.name}:{c.sink_decoder}")


def write_json(doc: dict, path: str) -> None:
    with open(path, "w") as f:
        json.dump(doc, f, indent=2, sort_keys=False)
    print(f"JSON: {path}")


# ── Baseline diff ───────────────────────────────────────────────


def load_baseline(path: str) -> dict:
    with open(path) as f:
        doc = json.load(f)
    ver = doc.get("schema_version")
    if ver != SCHEMA_VERSION:
        raise SystemExit(
            f"error: baseline schema {ver} != current {SCHEMA_VERSION}; "
            f"regenerate with `matrix_test.py --out {path}` on a "
            f"known-good build")
    return doc


def _is_regression(cur_us: Optional[int], base_us: Optional[int],
                   pct_threshold: float, ms_threshold: float) -> bool:
    """A regression is a p50 that grew by *both* > pct_threshold %
    AND the absolute delta is meaningful (> 0.5 ms OR > ms_threshold
    absolute regardless of %). Small absolute deltas below the noise
    floor aren't flagged even if they're a big percent."""
    if cur_us is None or base_us is None:
        return False
    delta_us = cur_us - base_us
    if delta_us <= 0:
        return False
    delta_ms = delta_us / 1000.0
    pct = (delta_us / base_us) * 100.0 if base_us > 0 else 0.0
    if delta_ms > ms_threshold:
        return True
    if pct > pct_threshold and delta_ms > 0.5:
        return True
    return False


def compare_to_baseline(current: dict, baseline: dict,
                        pct_threshold: float,
                        ms_threshold: float) -> tuple[list[dict], int]:
    """Returns (per-combo-diff, regression_count). Each diff row has
    enough info to render a colorised table."""
    base_by_key = {c["key"]: c for c in baseline.get("combos", [])}
    cur_by_key = {c["key"]: c for c in current.get("combos", [])}
    diffs: list[dict] = []
    regressions = 0
    all_keys = sorted(set(base_by_key) | set(cur_by_key))
    for key in all_keys:
        b = base_by_key.get(key)
        c = cur_by_key.get(key)
        row: dict = {"key": key}
        if b and not c:
            row["state"] = "missing"
            row["detail"] = "combo in baseline but not current"
            diffs.append(row); continue
        if c and not b:
            row["state"] = "new"
            row["detail"] = f"new combo: {c['status']}"
            diffs.append(row); continue

        row["state"] = "both"
        row["status_cur"] = c["status"]
        row["status_base"] = b["status"]
        if c["status"] != "ok" or b["status"] != "ok":
            # Can't compare numbers if either side didn't produce them.
            row["note"] = (f"cur={c['status']}, base={b['status']}; "
                           f"numeric diff skipped")
            diffs.append(row); continue

        # Only glass p50/p95 for the regression gate; we surface
        # cap→dec / dec→pre too for debugging.
        row["phases"] = {}
        for phase in ("capture_to_decode_us",
                      "decode_to_present_us",
                      "capture_to_present_us"):
            cp = c["phases"].get(phase, {}).get("p50_us")
            bp = b["phases"].get(phase, {}).get("p50_us")
            row["phases"][phase] = {
                "cur_p50_us": cp, "base_p50_us": bp,
                "delta_us": (cp - bp) if (cp is not None
                                           and bp is not None) else None,
            }
        glass = row["phases"]["capture_to_present_us"]
        if _is_regression(glass["cur_p50_us"], glass["base_p50_us"],
                          pct_threshold, ms_threshold):
            row["regression"] = True
            regressions += 1
        else:
            row["regression"] = False
        diffs.append(row)
    return diffs, regressions


def _ansi(code: str, s: str) -> str:
    """Wrap s in ANSI escape `code` when stdout is a TTY, else
    return s unchanged (so CI logs stay plain)."""
    if not sys.stdout.isatty():
        return s
    return f"\033[{code}m{s}\033[0m"


def print_baseline_diff(diffs: list[dict], pct_threshold: float,
                        ms_threshold: float) -> None:
    print("")
    print("=" * 110)
    print(f"Baseline diff "
          f"(regression gate: >+{pct_threshold:.0f}% p50 or "
          f">+{ms_threshold:.1f} ms absolute)")
    print("-" * 110)
    print(f"{'combo':<58} {'base glass p50':>14} "
          f"{'cur glass p50':>14} {'Δ':>10} {'':>8}")
    print("-" * 110)
    for d in diffs:
        key = d["key"]
        if d["state"] == "missing":
            print(f"{key:<58} "
                  f"{_ansi('33', 'MISSING (in baseline only)')}")
            continue
        if d["state"] == "new":
            print(f"{key:<58} "
                  f"{_ansi('36', 'NEW (not in baseline)')}")
            continue
        if "note" in d:
            print(f"{key:<58} "
                  f"{_ansi('33', d['note'])}")
            continue
        gl = d["phases"]["capture_to_present_us"]
        bp = gl["base_p50_us"]
        cp = gl["cur_p50_us"]
        bp_s = f"{bp/1000:10.2f} ms" if bp is not None else "    -"
        cp_s = f"{cp/1000:10.2f} ms" if cp is not None else "    -"
        if gl["delta_us"] is None:
            delta_s = "    -"
            tag = ""
        else:
            delta_ms = gl["delta_us"] / 1000.0
            sign = "+" if delta_ms >= 0 else ""
            delta_s = f"{sign}{delta_ms:6.2f} ms"
            if d["regression"]:
                delta_s = _ansi("31;1", delta_s)  # red bold
                tag = _ansi("31", "REGRESSION")
            elif delta_ms < -ms_threshold / 2:
                delta_s = _ansi("32", delta_s)    # green
                tag = _ansi("32", "improved")
            else:
                tag = ""
        print(f"{key:<58} {bp_s:>14} {cp_s:>14} "
              f"{delta_s:>10} {tag:>8}")
    print("=" * 110)


# ── Pre-flight: build freshness / code version consistency ──────


UNIO_PIPE_CODE_PATHS = [
    "unio-pipe/src",
    "unio-pipe/include",
    "unio-pipe/CMakeLists.txt",
]


def _git_head_info() -> Optional[dict]:
    """Return {'sha', 'time_unix', 'code_sha', 'code_time_unix'}
    for the current git HEAD and for the most recent commit that
    actually touched the unio-pipe C++ tree. The latter is what
    matters for build freshness — a tooling-only commit doesn't
    invalidate the binary, so comparing against HEAD's time would
    spuriously flag every host as STALE after this-file edits.

    Returns None if we're not in a git tree."""
    try:
        sha = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True
        ).stdout.strip()
        ts = subprocess.run(
            ["git", "log", "-1", "--format=%ct", "HEAD"],
            capture_output=True, text=True, check=True
        ).stdout.strip()
        code_sha = subprocess.run(
            ["git", "log", "-1", "--format=%h", "HEAD", "--"]
            + UNIO_PIPE_CODE_PATHS,
            capture_output=True, text=True, check=True
        ).stdout.strip()
        code_ts = subprocess.run(
            ["git", "log", "-1", "--format=%ct", "HEAD", "--"]
            + UNIO_PIPE_CODE_PATHS,
            capture_output=True, text=True, check=True
        ).stdout.strip()
        return {
            "sha": sha[:12], "time_unix": int(ts),
            "code_sha": code_sha or sha[:12],
            "code_time_unix": int(code_ts) if code_ts else int(ts),
        }
    except (subprocess.CalledProcessError, FileNotFoundError,
            ValueError):
        return None


def preflight_build_freshness(hosts: dict[str, Host],
                              strict: bool) -> tuple[bool, dict]:
    """Verify every host is running bit-identical code. Two
    checks, in order of strength:

      1. Authoritative: query helper_caps on each host, read the
         build_commit string the binary was compiled with (set by
         CMake from `git rev-parse HEAD`). If hosts disagree, the
         run is testing mixed code — fail immediately.

      2. Fallback heuristic: compare binary mtime against the
         orchestrator's last unio-pipe-code commit. Catches the
         "forgot to rebuild after pull" case even when the probe
         can't reach a host. mtime can't detect cross-branch
         builds — the build_commit check above does.

    Returns (ok, info_dict). ok=False means a commit mismatch OR
    (strict mode only) any host showed STALE / MISSING.
    """
    head = _git_head_info()
    info = {
        "git_head": head,
        "hosts": {},
    }

    # ── Phase 1: authoritative commit check via helper_caps ──
    commits: dict[str, str] = {}
    print("  [pre-flight] probing helper_caps.build_commit on "
          f"{len(hosts)} host(s)")
    for name, h in hosts.items():
        caps = h.probe_caps()
        if "error" in caps:
            commits[name] = f"(unreachable: {caps['error']})"
            info["hosts"].setdefault(name, {})["probe_error"] = caps["error"]
        else:
            bc = caps.get("build_commit", "unknown")
            commits[name] = bc
            info["hosts"].setdefault(name, {})["build_commit"] = bc

    reachable = {
        name: c for name, c in commits.items()
        if not c.startswith("(unreachable")
    }
    # "unknown" is not filtered out — it indicates a binary built
    # before the build_commit feature was added, which means we
    # genuinely can't verify that host against the others. That's
    # a mixed-code-capable state we must flag, not ignore.
    uniq_all = set(reachable.values())
    if len(uniq_all) > 1:
        # Mixed-code run — this is what we're trying to prevent.
        # Either different commits OR at least one host is
        # pre-build_commit-feature ("unknown").
        print(f"  [pre-flight] "
              f"{_ansi('31;1', 'COMMIT MISMATCH across hosts:')}")
        for name, bc in commits.items():
            print(f"  [pre-flight]   {name:<12} {bc}")
        if "unknown" in uniq_all:
            print(f"  [pre-flight]   (host reporting \"unknown\" "
                  f"was built before the build_commit feature — "
                  f"rebuild it to compare)")
        return False, info
    elif len(uniq_all) == 1:
        shared = next(iter(uniq_all))
        if shared == "unknown":
            print(f"  [pre-flight] "
                  f"{_ansi('33', 'all hosts report build_commit=unknown')} "
                  f"— binaries predate the feature; can't verify "
                  f"same-code beyond mtime")
        else:
            dirty = "-dirty" in shared
            tag = (f"{_ansi('33', shared + ' (dirty working tree)')}"
                   if dirty else _ansi('32', shared))
            print(f"  [pre-flight] all reachable hosts @ {tag}")
            if dirty and strict:
                print(f"  [pre-flight] "
                      f"{_ansi('31', 'refusing dirty build under --strict-version')}")
                return False, info

    # ── Phase 2: mtime heuristic ──
    if head is None:
        print("  [pre-flight] not in a git tree — skipping mtime check")
        return True, info

    # Compare against the most recent commit that touched C++ /
    # CMake — a tooling-only HEAD commit doesn't invalidate
    # binaries, so only flag STALE when actual build inputs moved.
    head_ts = head["code_time_unix"]
    print(f"  [pre-flight] last unio-pipe code commit "
          f"{head['code_sha']} @ "
          f"{time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime(head_ts))}"
          f" (HEAD is {head['sha']})")
    all_ok = True
    for name, h in hosts.items():
        mtime = h.binary_mtime_unix()
        host_info = info["hosts"].setdefault(name, {})
        host_info["binary_mtime_unix"] = mtime
        host_info["binary_path"] = h.binary
        if mtime is None:
            msg = f"{_ansi('31', 'MISSING')} ({h.binary} not found)"
            host_info["state"] = "missing"
            all_ok = False
        elif mtime < head_ts:
            age = head_ts - mtime
            msg = (f"{_ansi('33', 'STALE')} "
                   f"(binary built {age // 60}m "
                   f"before HEAD — rebuild on {name}?)")
            host_info["state"] = "stale"
            host_info["seconds_behind_head"] = age
            if strict:
                all_ok = False
        else:
            msg = f"{_ansi('32', 'fresh')}"
            host_info["state"] = "fresh"
        print(f"  [pre-flight] {name:<12} {msg}")
    return all_ok, info


# ── Pre-flight: inter-host clock skew gate ──────────────────────


# UDP-based inter-host skew measurement. The SSH-per-sample
# fallback gives ±125 ms precision on LAN (dominated by SSH
# handshake + shell startup) — too coarse to usefully correct
# latencies in the 10-30 ms range. Raw UDP drops to ~1 ms RTT
# → sub-ms skew precision.
#
# Direction: orchestrator opens the UDP echo server, remote
# runs a small probe client. This is deliberate — Windows
# Firewall blocks *inbound* UDP on ad-hoc ports by default, but
# *outbound* UDP is almost always allowed. If the orchestrator
# bound the server on the remote we'd fight the firewall; this
# way the remote just initiates outbound connections back to
# the orchestrator's known address.
#
# Protocol (same NTP-style 4-sample exchange):
#   client  ──t0──►  server  (client send)
#                    t1 (server receive)
#                    t2 (server send)
#   client  ◄──t3──  server  (client receive)
#   offset_server_ahead_of_client = ((t1 - t0) + (t2 - t3)) / 2
#   rtt                           = (t3 - t0) - (t2 - t1)
# We need skew = remote - orchestrator, so the remote is the
# *client* — skew_remote = -offset_server_ahead_of_client.

_UDP_SKEW_CLIENT = '''
import socket, struct, time, sys
srv_host = sys.argv[1]
srv_port = int(sys.argv[2])
samples = int(sys.argv[3])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(1.5)
best_rtt = 10**18
best_offset = None
for _ in range(samples):
    try:
        t0 = time.time_ns()
        s.sendto(b"P", (srv_host, srv_port))
        data, _ = s.recvfrom(64)
        t3 = time.time_ns()
        t1, t2 = struct.unpack("<QQ", data)
        rtt = (t3 - t0) - (t2 - t1)
        if rtt < 0 or rtt > 5 * 10**8:
            continue
        if rtt < best_rtt:
            best_rtt = rtt
            best_offset = ((t1 - t0) + (t2 - t3)) // 2
    except Exception:
        continue
s.close()
if best_offset is None:
    print("FAIL")
else:
    # Emit ms-resolved skew (client-perspective: server-ahead).
    # Orchestrator flips sign to get remote-perspective skew.
    print(f"OK {best_offset // 1_000_000} {best_rtt // 1_000_000}")
'''


def _start_udp_skew_server(port: int) -> tuple[socket.socket,
                                                threading.Thread]:
    """Run a tiny UDP echo server on the orchestrator. Replies
    with (serverRecv_ns, serverSend_ns) pairs until stopped."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError:
        pass
    srv.bind(("0.0.0.0", port))
    srv.settimeout(0.2)

    def loop():
        while not _srv_stop.is_set():
            try:
                pkt, addr = srv.recvfrom(64)
            except socket.timeout:
                continue
            except OSError:
                break
            t1 = time.time_ns()
            t2 = time.time_ns()
            try:
                srv.sendto(struct.pack("<QQ", t1, t2), addr)
            except OSError:
                break
    _srv_stop.clear()
    th = threading.Thread(target=loop, daemon=True)
    th.start()
    return srv, th


_srv_stop = threading.Event()


def _measure_skew_udp_from_remote(
        host: Host, orchestrator_addr: str, server_port: int,
        samples: int = 30) -> tuple[Optional[int], int, str]:
    """SSH to the remote, run the probe client against the
    orchestrator's UDP server. Returns (remote_skew_ms, rtt_ms,
    detail). The remote prints "OK <server_ahead_ms> <rtt_ms>";
    we flip the sign so skew_remote means remote-ahead-of-
    orchestrator, which is what the correction formula expects.
    """
    if host.role != "remote":
        return 0, 0, "orchestrator"
    b64 = base64.b64encode(
        _UDP_SKEW_CLIENT.encode("utf-8")).decode("ascii")
    py = "python3" if host.os == "linux" else "python"
    remote_cmd = (
        f'{py} -c "import base64,sys;'
        f'exec(base64.b64decode(\'{b64}\'))" '
        f'{orchestrator_addr} {server_port} {samples}')
    try:
        out = _ssh(host.ssh_host, host.ssh_key, remote_cmd,  # type: ignore[attr-defined]
                   check=False, timeout_s=20)
    except Exception as e:
        return None, 0, f"probe error: {e}"
    for line in out.splitlines()[::-1]:
        line = line.strip()
        if line.startswith("OK "):
            parts = line.split()
            try:
                server_ahead_ms = int(parts[1])
                rtt_ms = int(parts[2])
                return -server_ahead_ms, rtt_ms, "udp"
            except (ValueError, IndexError):
                pass
        if line == "FAIL":
            return None, 0, "udp probe failed (all samples timed out)"
    return None, 0, f"probe returned unexpected: {out.strip()[:200]}"


def _measure_skew_ssh_ms(orchestrator: Host,
                         remote: Host) -> tuple[Optional[int], int]:
    """Fallback: one SSH round-trip per sample. ±RTT/2 precision
    with RTT ~250 ms on LAN, so coarse (±125 ms)."""
    t0 = int(time.time() * 1000)
    remote_ms, rtt = remote.unix_time_ms()
    t1 = int(time.time() * 1000)
    if remote_ms is None:
        return None, rtt
    local_mid = (t0 + t1) // 2
    return remote_ms - local_mid, rtt


def _measure_skew_ssh_ms(orchestrator: Host,
                         remote: Host) -> tuple[Optional[int], int]:
    """Fallback: one SSH round-trip per sample. ±RTT/2 precision
    with RTT ~250 ms on LAN, so coarse (±125 ms)."""
    t0 = int(time.time() * 1000)
    remote_ms, rtt = remote.unix_time_ms()
    t1 = int(time.time() * 1000)
    if remote_ms is None:
        return None, rtt
    local_mid = (t0 + t1) // 2
    return remote_ms - local_mid, rtt


def preflight_clock_skew(hosts: dict[str, Host],
                         cfg: dict,
                         combos: list[Combo]) -> tuple[bool, dict]:
    """If the combo set contains any cross-machine runs, resync
    every involved host against the shared NTP server, then
    measure the actual skew between each remote and the
    orchestrator. Refuse to run if skew exceeds the threshold —
    otherwise cap→present numbers are dominated by clock drift,
    not pipeline latency.

    Loopback-only runs skip this entirely (same clock on both
    src and sink)."""
    info = {"hosts": {}, "threshold_ms": cfg["defaults"]["max_clock_skew_ms"]}

    cross = any(c.src is not c.sink for c in combos)
    if not cross:
        print("  [pre-flight] all combos are loopback — "
              "skipping clock-skew check")
        return True, info

    ntp_server = cfg["defaults"].get("ntp_server",
                                      "time.cloudflare.com")
    threshold = cfg["defaults"]["max_clock_skew_ms"]

    orchestrator = next(h for h in hosts.values() if h.role == "local")

    # Phase A: resync every host against the shared NTP source.
    print(f"  [pre-flight] resyncing clocks against {ntp_server}")
    for name, h in hosts.items():
        ok, msg = h.sync_clock(ntp_server)
        color = _ansi('32', 'ok') if ok else _ansi('33', 'warn')
        print(f"  [pre-flight]   {name:<12} {color}: {msg}")
        info["hosts"].setdefault(name, {})["sync_msg"] = msg
        info["hosts"][name]["sync_ok"] = ok

    # Phase B: measure inter-host skew. Preferred path is a
    # UDP echo server on the *orchestrator* + NTP-style probe
    # client launched on the remote via SSH (outbound UDP — no
    # inbound firewall rules needed). Drops precision from
    # ±125 ms (SSH-per-sample) to sub-ms (~1 ms LAN RTT).
    # Falls back to SSH-per-sample if UDP can't reach.
    udp_port = cfg["defaults"].get("port_base", 5099) + 100
    orch_addr = orchestrator.address
    srv, srv_thread = _start_udp_skew_server(udp_port)
    try:
        all_ok = True
        for name, h in hosts.items():
            if h is orchestrator:
                info["hosts"].setdefault(name, {})["skew_ms"] = 0
                info["hosts"][name]["rtt_ms"] = 0
                info["hosts"][name]["method"] = "orchestrator"
                print(f"  [pre-flight]   {name:<12} skew=   0 ms (orchestrator)")
                continue
            skew, rtt, detail = _measure_skew_udp_from_remote(
                h, orch_addr, udp_port, samples=30)
            method = detail
            if skew is None:
                # Fallback: per-sample SSH time queries.
                best_rtt = 10_000
                for _ in range(10):
                    s_skew, s_rtt = _measure_skew_ssh_ms(
                        orchestrator, h)
                    if s_skew is None:
                        continue
                    if s_rtt < best_rtt:
                        skew, best_rtt = s_skew, s_rtt
                rtt = best_rtt
                method = f"ssh-fallback ({detail})"
            info["hosts"].setdefault(name, {})["skew_ms"] = skew
            info["hosts"][name]["rtt_ms"] = rtt
            info["hosts"][name]["method"] = method
            if skew is None:
                print(f"  [pre-flight]   {name:<12} "
                      f"{_ansi('31', 'skew UNKNOWN')} "
                      f"({method})")
                all_ok = False
                continue
            abs_skew = abs(skew)
            precision = max(rtt // 2, 1)
            tag = (_ansi('32', 'ok') if abs_skew <= threshold
                   else _ansi('31', 'OVER'))
            print(f"  [pre-flight]   {name:<12} "
                  f"skew={skew:+5d} ms  (±{precision} ms, "
                  f"RTT {rtt} ms, method={method}) {tag}")
            if abs_skew > threshold:
                all_ok = False
    finally:
        _srv_stop.set()
        try:
            srv.close()
        except Exception:
            pass
        srv_thread.join(timeout=1)

    if not all_ok:
        print(f"  [pre-flight] {_ansi('31;1', 'clock skew exceeds threshold')} "
              f"({threshold} ms) — cross-machine latency numbers "
              f"would be dominated by clock drift.")
        print(f"  [pre-flight] fix: configure both hosts to use a "
              f"common low-stratum NTP source (LAN stratum-1 for "
              f"sub-ms) and rerun, or raise max_clock_skew_ms "
              f"in hosts.yaml if the drift is acceptable for "
              f"your measurement.")
    return all_ok, info


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
    ap.add_argument("--out", default=None, metavar="FILE.json",
                    help="write structured results to a JSON file "
                         "(schema is stable within SCHEMA_VERSION)")
    ap.add_argument("--baseline", default=None, metavar="FILE.json",
                    help="compare this run against a stored baseline "
                         "and print a diff table")
    ap.add_argument("--regression-pct", type=float, default=20.0,
                    help="p50 regression threshold, percent "
                         "(default 20)")
    ap.add_argument("--regression-ms", type=float, default=5.0,
                    help="p50 regression threshold, absolute ms "
                         "(default 5)")
    ap.add_argument("--fail-on-regression", action="store_true",
                    help="exit 1 if the baseline diff flags any "
                         "regression (CI gate mode)")
    ap.add_argument("--strict-version", action="store_true",
                    help="refuse to run any combo if any host's "
                         "binary is older than the orchestrator's "
                         "git HEAD commit time (detects stale "
                         "builds on remote hosts — otherwise a "
                         "warning only)")
    ap.add_argument("--sync", action="store_true",
                    help="before running the matrix, stream the "
                         "orchestrator's unio-pipe/ source tree "
                         "to every remote host and rebuild there. "
                         "Guarantees bit-identical code across "
                         "hosts. Adds 30 s–2 min per remote host "
                         "to the run.")
    ap.add_argument("--use-prebuilt", action="store_true",
                    help="instead of --sync (source push + rebuild), "
                         "dispatch pre-built binaries from "
                         "dist/<target>/ to every host. Produce them "
                         "first via packaging/docker/build-linux.sh "
                         "and packaging/docker/build-win.sh. Targets "
                         "need zero dev tools installed; the whole "
                         "build happens on the orchestrator.")
    ap.add_argument("--dist-dir", default=None,
                    help="where --use-prebuilt looks for dist/<target>/ "
                         "(default: <repo-root>/dist)")
    ap.add_argument("--ignore-clock-skew", action="store_true",
                    help="run cross-machine combos even when "
                         "inter-host clock skew exceeds "
                         "max_clock_skew_ms. Their cap→present "
                         "numbers will be unreliable; use only "
                         "when measuring decode→present in "
                         "isolation.")
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

    # Determine which hosts are actually exercised by the filter.
    only_hosts = {
        h for c in combos for h in (c.src, c.sink)
    }
    preflight_hosts = {
        name: h for name, h in hosts.items() if h in only_hosts
    }

    # --sync: push orchestrator's source tree to every remote host
    # and rebuild there BEFORE the pre-flight commit check. This is
    # the only way to guarantee bit-identical code across hosts
    # without manual cmake-on-each-box discipline.
    source_root = Path(__file__).resolve().parent.parent  # unio-pipe/
    if args.sync:
        # Local rebuild first — cmake caches UNIO_BUILD_COMMIT at
        # configure time, so a fresh commit made since the last
        # configure stays unembedded until we re-run cmake -S / -B.
        # Without this, --sync would push a fresh binary to every
        # remote and the orchestrator would still be running a
        # stale one.
        print(f"\n[sync] local reconfigure + rebuild to refresh "
              f"UNIO_BUILD_COMMIT ...")
        build_dir = source_root / "build"
        r = subprocess.run(
            ["cmake", "-S", str(source_root), "-B", str(build_dir)],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  [sync] local cmake reconfigure failed:\n"
                  f"  {r.stderr.splitlines()[-5:] if r.stderr else ''}",
                  file=sys.stderr)
            return 4
        r = subprocess.run(
            ["cmake", "--build", str(build_dir), "-j"],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  [sync] local cmake --build failed:\n"
                  f"  {''.join(r.stderr.splitlines()[-10:])}",
                  file=sys.stderr)
            return 4

        print(f"\n[sync] streaming source from {source_root} "
              f"to every remote host ...")
        sync_ok = True
        for name, h in preflight_hosts.items():
            if h.role == "local":
                continue
            print(f"  [sync] {name} ...")
            ok, msg = h.sync_code(source_root)
            print(f"  [sync] {name}: {msg}")
            if not ok:
                sync_ok = False
        if not sync_ok:
            print(f"\n{_ansi('31;1', 'sync FAILED — aborting')}",
                  file=sys.stderr)
            return 4

    # --use-prebuilt: push binaries from dist/<target>/ to every
    # host. Alternative to --sync: no rebuild happens on any host,
    # everything was already built on the orchestrator (usually via
    # packaging/docker/build-{linux,win}.sh). Remotes don't need
    # cmake / VS Build Tools / libvpl headers installed — only
    # SSH + permission to write the helper binary path declared
    # in hosts.yaml.
    if args.use_prebuilt:
        dist_root = (Path(args.dist_dir) if args.dist_dir
                     else source_root.parent / "dist")
        print(f"\n[prebuilt] deploying binaries from {dist_root} "
              f"to every host ...")
        deploy_ok = True
        for name, h in preflight_hosts.items():
            print(f"  [prebuilt] {name} ...")
            try:
                ok, msg = h.deploy_prebuilt(dist_root)
            except NotImplementedError as e:
                ok, msg = False, str(e)
            print(f"  [prebuilt] {name}: {msg}")
            if not ok:
                deploy_ok = False
        if not deploy_ok:
            print(f"\n{_ansi('31;1', 'prebuilt deploy FAILED — aborting')}",
                  file=sys.stderr)
            return 4

    # Pre-flight: verify every host is running a binary built from
    # at least the orchestrator's current git HEAD. Otherwise the
    # matrix numbers are literally incomparable — different hosts
    # would be running different code.
    ok, preflight_info = preflight_build_freshness(
        preflight_hosts, args.strict_version)
    if not ok:
        print(f"\n{_ansi('31;1', 'pre-flight FAILED — aborting')}",
              file=sys.stderr)
        if args.strict_version:
            print("  (remove --strict-version to continue anyway)",
                  file=sys.stderr)
        return 3

    # Pre-flight: inter-host clock skew. Only matters for
    # cross-machine combos — loopback runs share a single clock.
    clock_ok, clock_info = preflight_clock_skew(
        preflight_hosts, cfg, combos)
    preflight_info["clock_sync"] = clock_info
    if not clock_ok and not args.ignore_clock_skew:
        print(f"\n{_ansi('31;1', 'clock-skew pre-flight FAILED — aborting')}",
              file=sys.stderr)
        print("  (pass --ignore-clock-skew to run anyway; "
              "cross-machine cap→present numbers will be "
              "dominated by clock drift)", file=sys.stderr)
        return 5

    # Extract per-host skew so run_combo can correct cap→* numbers
    # for known clock drift. Orchestrator is 0 by definition.
    skew_ms_by_host = {
        name: info.get("skew_ms", 0) or 0
        for name, info in clock_info.get("hosts", {}).items()
    }
    if any(v for v in skew_ms_by_host.values()):
        print(f"  [pre-flight] applying skew correction to "
              f"cross-machine cap→* phases: {skew_ms_by_host}")

    results: list[ComboResult] = []
    for i, combo in enumerate(combos):
        print(f"\n[{i+1}/{len(combos)}] {combo.label}")
        r = run_combo(combo, cfg, args, i, skew_ms_by_host)
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

    current_doc = results_to_json(
        results, hosts, cfg, args, preflight_info)
    if args.out:
        write_json(current_doc, args.out)

    regression_rc = 0
    if args.baseline:
        baseline_doc = load_baseline(args.baseline)
        diffs, regressions = compare_to_baseline(
            current_doc, baseline_doc,
            args.regression_pct, args.regression_ms)
        print_baseline_diff(
            diffs, args.regression_pct, args.regression_ms)
        if regressions:
            print(f"\n{_ansi('31;1', f'{regressions} regression(s) flagged')}")
            if args.fail_on_regression:
                regression_rc = 1

    fail = sum(1 for r in results if r.status == "failed")
    if fail:
        return 1
    return regression_rc


if __name__ == "__main__":
    sys.exit(main())

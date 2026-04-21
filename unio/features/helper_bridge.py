"""Control-plane client for the (not-yet-implemented) unio-pipe
native helper.

The plan (see ``unio-pipe/README.md``) is to move every frame-path
stage — capture, encode, transport, decode, present — into a
separate C++ process, leaving Python in charge of only the control
plane (mesh, LWW, workspace routing, pairing, clipboard, file
transfer). That's PR 6 in the latency plan, an 8-week rewrite
that has not started.

This module is the Python-side scaffold for the control socket:
the constants, the command shape, and the not-yet-wired ``Bridge``
stub. When the C++ helper lands it connects to the socket this
module creates and receives the JSON commands below.

Shipping this stub now means:

  * the command vocabulary is in source, in one place, so any
    shell-side wiring we write toward the native helper can
    reference real names instead of inventing new ones when the
    C++ side catches up;
  * the "is the helper running?" check fails loud (helper absent)
    rather than silently, so the current Python pipeline keeps
    running unaffected;
  * packaging / distribution paths know the helper lives here.

Nothing in this file is active in the current build. The
``display_stream`` Python pipeline is still the data plane.
"""

from __future__ import annotations

import enum
import json
import logging
import socket
import struct
import sys
import threading
from typing import Optional

log = logging.getLogger(__name__)


# ── Protocol constants ──────────────────────────────────────────

# Transport for the control channel. UDS on Linux, named pipe on
# Windows — both give us framing + per-process namespacing without
# the TCP overhead of accept() + TLS. Frame data NEVER crosses
# this socket; commands are ≤ a few KB of JSON at ≤ 10 Hz.
UDS_SOCKET_PATH = "/tmp/unio-pipe.sock"
WIN_PIPE_NAME = r"\\.\pipe\unio-pipe"

# Wire framing: 4-byte little-endian length followed by UTF-8
# JSON payload. Same shape as the StreamServer handshake the
# display-stream module already uses — reading this module is a
# shortcut to understanding the helper's control plane.
CONTROL_HEADER_FMT = "<I"


class Command(str, enum.Enum):
    """Every JSON command the helper accepts. The values are the
    wire strings; enum membership is mostly for type hints on the
    Python side when the bridge actually gets wired."""
    START_OUTBOUND = "start_outbound"
    START_INBOUND = "start_inbound"
    STOP = "stop"
    REQUEST_IDR = "request_idr"
    HELPER_CAPS = "helper_caps"
    HELPER_STATUS = "helper_status"


# ── Client stub (not wired) ─────────────────────────────────────


def helper_socket_path() -> str:
    """Return the platform-appropriate socket path. Exposed for
    testing + packaging even though no consumer exists today."""
    if sys.platform == "win32":
        return WIN_PIPE_NAME
    return UDS_SOCKET_PATH


def helper_running() -> bool:
    """True when the native helper process is up and answering on
    its control socket. Opens a one-shot connection, runs
    helper_caps, closes. Called at startup by the shell to decide
    whether to use the native data plane (once implemented) or
    stay on the Python path."""
    b = Bridge()
    if not b.connect(timeout=0.5):
        return False
    try:
        return b.helper_caps() is not None
    finally:
        b.close()


class Bridge:
    """Synchronous RPC client for the unio-pipe C++ helper.

    Day-1 surface is intentionally minimal: connect, single-in-
    flight request/response, close. No cancellation, no streaming,
    no reconnect — the shell owns helper lifecycle (spawn on
    start, kill on stop) so there's no advantage to making this
    async yet. The native helper runs on a dedicated thread inside
    itself; the Python side is NOT on any frame path.

    Thread safety: one lock around every send/recv pair means
    multiple callers can share the instance without ordering bugs.
    Rate is bounded at ~10 Hz by the control-plane design.

    All RPCs return the parsed reply dict on success, or None on
    transport / parse failure. ``error`` inside the dict is a
    normal helper-side NACK (``{"error": "not implemented yet"}``)
    — the transport worked fine.
    """

    def __init__(self, socket_path: Optional[str] = None) -> None:
        self._path = socket_path or helper_socket_path()
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()

    # ── Lifecycle ──────────────────────────────────────────────

    def connect(self, timeout: float = 2.0) -> bool:
        """Open the UDS / named pipe. Returns True on success."""
        if sys.platform == "win32":
            # Named-pipe client is PR 6 week 2 material.
            log.info("helper_bridge: Windows named pipe not "
                     "supported yet — staying on Python pipeline")
            return False
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(timeout)
            s.connect(self._path)
            self._sock = s
            log.info("helper_bridge connected to %s", self._path)
            return True
        except OSError as e:
            log.info("helper_bridge connect(%s) failed: %s",
                     self._path, e)
            return False

    def close(self) -> None:
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                except Exception:
                    pass
                self._sock = None

    def is_connected(self) -> bool:
        return self._sock is not None

    # ── RPCs ───────────────────────────────────────────────────

    def _rpc(self, cmd: Command, **fields) -> Optional[dict]:
        """One request/response round-trip. Holds self._lock for
        the duration so multiple callers can't interleave frames.
        Returns None on transport failure; the caller treats that
        as "helper gone, fall back to Python pipeline"."""
        with self._lock:
            sock = self._sock
            if sock is None:
                return None
            req = {"cmd": cmd.value, **fields}
            body = json.dumps(req).encode("utf-8")
            header = struct.pack(CONTROL_HEADER_FMT, len(body))
            try:
                sock.sendall(header + body)
                hdr = _recv_exact(sock, 4)
                if hdr is None:
                    return None
                (rlen,) = struct.unpack(CONTROL_HEADER_FMT, hdr)
                if rlen <= 0 or rlen > (1 << 20):
                    log.info("helper_bridge: bogus reply len %d",
                             rlen)
                    return None
                reply = _recv_exact(sock, rlen)
                if reply is None:
                    return None
                return json.loads(reply.decode("utf-8",
                                               errors="replace"))
            except (OSError, ValueError, json.JSONDecodeError) as e:
                log.info("helper_bridge rpc %s failed: %s",
                         cmd.value, e)
                self._sock = None
                try:
                    sock.close()
                except Exception:
                    pass
                return None

    def helper_caps(self) -> Optional[dict]:
        return self._rpc(Command.HELPER_CAPS)

    def helper_status(self) -> Optional[dict]:
        return self._rpc(Command.HELPER_STATUS)

    def start_outbound(self, *, stream_id: str,
                       monitor_source: str,
                       peer_addr: str,
                       codec_hints: list) -> Optional[dict]:
        return self._rpc(
            Command.START_OUTBOUND,
            stream_id=stream_id,
            monitor_source=monitor_source,
            peer_addr=peer_addr,
            codec_hints=codec_hints)

    def start_inbound(self, *, stream_id: str,
                      sink_monitor_id: str,
                      source_hint: str) -> Optional[dict]:
        return self._rpc(
            Command.START_INBOUND,
            stream_id=stream_id,
            sink_monitor_id=sink_monitor_id,
            source_hint=source_hint)

    def stop(self, stream_id: str) -> Optional[dict]:
        return self._rpc(Command.STOP, stream_id=stream_id)

    def request_idr(self, stream_id: str) -> Optional[dict]:
        return self._rpc(Command.REQUEST_IDR, stream_id=stream_id)


def _recv_exact(sock: socket.socket, n: int) -> Optional[bytes]:
    """Read exactly ``n`` bytes from ``sock`` or return None on
    EOF / error. Loop because ``recv`` is free to return short."""
    out = bytearray()
    while len(out) < n:
        try:
            chunk = sock.recv(n - len(out))
        except OSError:
            return None
        if not chunk:
            return None
        out.extend(chunk)
    return bytes(out)

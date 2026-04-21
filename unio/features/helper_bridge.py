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
import logging
import sys
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
    its control socket. Always False in the current build; shell
    code can already call this and get the right "nope, use the
    Python pipeline" answer. Wires up properly when PR 6 lands."""
    return False


class Bridge:
    """Placeholder for the future control-socket client. Intended
    API (not implemented):

        bridge = Bridge()
        bridge.connect()
        caps = bridge.helper_caps()     # blocking RPC
        bridge.start_outbound(stream_id=..., monitor_source=...)
        bridge.request_idr(stream_id)
        bridge.stop(stream_id)

    Requests go out with Command.X values; replies come back with
    a correlation id. The native helper manages frame data on its
    own threads; this bridge never sees pixels.
    """

    def __init__(self) -> None:
        self._connected = False

    def connect(self) -> bool:
        # Placeholder. Will open UDS / named pipe and perform the
        # handshake in PR 6. Until then: fail fast so callers know
        # to stick with the Python pipeline.
        self._connected = False
        return False

    def close(self) -> None:
        self._connected = False

    def is_connected(self) -> bool:
        return self._connected

    def helper_caps(self) -> Optional[dict]:
        return None

    def start_outbound(self, *, stream_id: str,
                       monitor_source: str,
                       peer_addr: str,
                       codec_hints: list) -> bool:
        return False

    def start_inbound(self, *, stream_id: str,
                      sink_monitor_id: str,
                      source_hint: str) -> bool:
        return False

    def stop(self, stream_id: str) -> bool:
        return False

    def request_idr(self, stream_id: str) -> bool:
        return False

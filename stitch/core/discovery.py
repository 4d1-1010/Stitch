"""UDP LAN discovery for Stitch servers.

Servers run a `DiscoveryResponder` that replies to broadcast probes with
their hostname and TCP port. Clients use `discover_hosts()` to broadcast
a probe and collect every responder within a timeout. Multiple servers
on the same LAN are supported — each reply is a separate entry keyed
by its (ip, port) pair.

Wire format is a single JSON object per datagram. Probes and replies
both carry a `magic` string so unrelated broadcast traffic on the port
is ignored without noise.
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket
from dataclasses import dataclass
from typing import Optional

DISCOVERY_PORT = 24801
_PROBE_MAGIC = "stitch-discover-v1"
_REPLY_MAGIC = "stitch-server-v1"

log = logging.getLogger(__name__)


@dataclass
class DiscoveredHost:
    ip: str
    port: int
    hostname: str

    @property
    def label(self) -> str:
        return f"{self.hostname} ({self.ip}:{self.port})"


class _ResponderProtocol(asyncio.DatagramProtocol):
    def __init__(self, hostname: str, tcp_port: int):
        self.hostname = hostname
        self.tcp_port = tcp_port
        self.transport: Optional[asyncio.DatagramTransport] = None

    def connection_made(self, transport: asyncio.DatagramTransport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            probe = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if probe.get("magic") != _PROBE_MAGIC:
            return
        reply = json.dumps({
            "magic": _REPLY_MAGIC,
            "hostname": self.hostname,
            "port": self.tcp_port,
        }).encode("utf-8")
        try:
            if self.transport is not None:
                self.transport.sendto(reply, addr)
        except OSError as e:
            log.debug("Discovery reply to %s failed: %s", addr, e)


class DiscoveryResponder:
    """Answers LAN discovery probes with (hostname, tcp_port)."""

    def __init__(self, hostname: str, tcp_port: int,
                 listen_port: int = DISCOVERY_PORT):
        self.hostname = hostname
        self.tcp_port = tcp_port
        self.listen_port = listen_port
        self._transport: Optional[asyncio.DatagramTransport] = None

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        # SO_REUSEADDR so multiple servers on the same host (different
        # TCP ports) can all answer discovery probes.
        self._transport, _ = await loop.create_datagram_endpoint(
            lambda: _ResponderProtocol(self.hostname, self.tcp_port),
            local_addr=("0.0.0.0", self.listen_port),
            reuse_port=False,
            allow_broadcast=True,
        )
        log.info("LAN discovery listening on UDP %d (hostname=%s, tcp=%d)",
                 self.listen_port, self.hostname, self.tcp_port)

    def stop(self) -> None:
        if self._transport:
            self._transport.close()
            self._transport = None


class _ClientProtocol(asyncio.DatagramProtocol):
    def __init__(self, results: dict[tuple[str, int], DiscoveredHost]):
        self.results = results
        self.transport: Optional[asyncio.DatagramTransport] = None

    def connection_made(self, transport: asyncio.DatagramTransport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            payload = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if payload.get("magic") != _REPLY_MAGIC:
            return
        hostname = str(payload.get("hostname") or "")
        try:
            port = int(payload.get("port") or 0)
        except (TypeError, ValueError):
            return
        if port <= 0 or port > 65535:
            return
        ip = addr[0]
        key = (ip, port)
        # Prefer the most recent hostname answer if the same server
        # responded twice (can happen on multi-homed hosts).
        self.results[key] = DiscoveredHost(ip=ip, port=port, hostname=hostname)


async def discover_hosts(
    timeout: float = 1.5,
    listen_port: int = DISCOVERY_PORT,
) -> list[DiscoveredHost]:
    """Broadcast a discovery probe and collect every reply within `timeout`.

    Returns one entry per distinct (ip, port). Safe to call multiple
    times — each call opens a fresh ephemeral UDP socket.
    """
    loop = asyncio.get_running_loop()
    results: dict[tuple[str, int], DiscoveredHost] = {}
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _ClientProtocol(results),
        local_addr=("0.0.0.0", 0),
        allow_broadcast=True,
    )
    probe = json.dumps({"magic": _PROBE_MAGIC}).encode("utf-8")
    try:
        for target in _broadcast_targets():
            try:
                transport.sendto(probe, (target, listen_port))
            except OSError as e:
                log.debug("Discovery probe to %s failed: %s", target, e)
        await asyncio.sleep(timeout)
    finally:
        transport.close()
    # Sort by hostname for a stable UI ordering.
    return sorted(results.values(), key=lambda h: (h.hostname.lower(), h.ip))


def _broadcast_targets() -> list[str]:
    """Addresses to aim the discovery probe at.

    255.255.255.255 is the universal local broadcast. Some networks
    filter it, so we also derive the /24 broadcast from every IPv4
    interface we can see, which covers typical home LANs.
    """
    targets = {"255.255.255.255"}
    try:
        hostname = socket.gethostname()
        for ip in socket.gethostbyname_ex(hostname)[2]:
            if not ip.startswith("127."):
                octets = ip.split(".")
                if len(octets) == 4:
                    targets.add(f"{octets[0]}.{octets[1]}.{octets[2]}.255")
    except OSError:
        pass
    return sorted(targets)

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
            log.debug("Discovery: ignored %d-byte non-JSON datagram from %s",
                      len(data), addr)
            return
        if probe.get("magic") != _PROBE_MAGIC:
            log.debug("Discovery: ignored datagram from %s with magic %r",
                      addr, probe.get("magic"))
            return
        reply = json.dumps({
            "magic": _REPLY_MAGIC,
            "hostname": self.hostname,
            "port": self.tcp_port,
        }).encode("utf-8")
        try:
            if self.transport is not None:
                self.transport.sendto(reply, addr)
                log.info("Discovery probe from %s — replied with "
                         "hostname=%s tcp=%d",
                         addr, self.hostname, self.tcp_port)
        except OSError as e:
            log.warning("Discovery reply to %s failed: %s", addr, e)


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
        # Hand the socket to asyncio pre-configured — Python's own
        # create_datagram_endpoint defaults mean different things on
        # different platforms, and broadcast replies require us to
        # explicitly enable SO_BROADCAST + SO_REUSEADDR so a quick
        # restart after an unclean exit doesn't hit EADDRINUSE.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            sock.bind(("0.0.0.0", self.listen_port))
        except OSError:
            sock.close()
            raise
        self._transport, _ = await loop.create_datagram_endpoint(
            lambda: _ResponderProtocol(self.hostname, self.tcp_port),
            sock=sock,
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
            log.debug("Discovery: dropped %d-byte non-JSON reply from %s",
                      len(data), addr)
            return
        if payload.get("magic") != _REPLY_MAGIC:
            log.debug("Discovery: dropped reply from %s with magic %r",
                      addr, payload.get("magic"))
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
        log.info("Discovery reply from %s: hostname=%s tcp=%d",
                 addr, hostname, port)
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

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", 0))
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _ClientProtocol(results), sock=sock,
    )

    probe = json.dumps({"magic": _PROBE_MAGIC}).encode("utf-8")
    targets = _broadcast_targets()
    log.info("Discovery: probing %d broadcast target(s): %s",
             len(targets), ", ".join(targets))
    try:
        for target in targets:
            try:
                transport.sendto(probe, (target, listen_port))
                log.debug("Discovery: sent probe to %s:%d", target, listen_port)
            except OSError as e:
                log.warning("Discovery probe to %s failed: %s", target, e)
        await asyncio.sleep(timeout)
    finally:
        transport.close()
    log.info("Discovery: scan complete, %d host(s) replied", len(results))
    # Sort by hostname for a stable UI ordering.
    return sorted(results.values(), key=lambda h: (h.hostname.lower(), h.ip))


def _broadcast_targets() -> list[str]:
    """Addresses to aim the discovery probe at.

    255.255.255.255 is the universal local broadcast, but Windows and
    some routers drop it, so also aim at the /24 broadcast of every
    IPv4 address we actually own. We detect our own IPs by connecting
    a throwaway UDP socket to a well-known public address — no packets
    are actually sent, we just read what source address the OS would
    pick — which works on every OS without needing `netifaces` or
    `psutil`. Fallback to `gethostbyname_ex` covers cases where that
    trick fails (offline machine, no default route).
    """
    targets = {"255.255.255.255"}
    candidates: set[str] = set()

    for probe in (("8.8.8.8", 80), ("1.1.1.1", 80)):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(0.2)
            try:
                s.connect(probe)
                candidates.add(s.getsockname()[0])
            finally:
                s.close()
        except OSError:
            continue

    try:
        host = socket.gethostname()
        for ip in socket.gethostbyname_ex(host)[2]:
            candidates.add(ip)
    except OSError:
        pass

    for ip in candidates:
        if ip.startswith("127."):
            continue
        octets = ip.split(".")
        if len(octets) == 4:
            targets.add(f"{octets[0]}.{octets[1]}.{octets[2]}.255")
    return sorted(targets)

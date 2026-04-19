"""UDP LAN discovery.

Two protocols live in this module:

1. **Mesh announcements** — every peer broadcasts its presence every
   few seconds on every local interface, and a single listener
   collects what peers have announced. This is what the full-mesh
   architecture uses to maintain the peer list.

2. **On-demand probe / reply** (legacy) — clients send a probe, hosts
   reply. Still used by the shell's "Find hosts" dialog until the
   mesh replaces it entirely. Backwards compatible with prior builds.

Wire format for every datagram is a single JSON object carrying a
`magic` string so unrelated broadcast traffic on the port is ignored
without noise.
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional

try:
    import psutil
    _HAS_PSUTIL = True
except ImportError:
    _HAS_PSUTIL = False

DISCOVERY_PORT = 24801
_PROBE_MAGIC = "unio-discover-v1"
_REPLY_MAGIC = "unio-server-v1"
_ANNOUNCE_MAGIC = "unio-peer-v1"

# How often each peer re-broadcasts its presence, and how long an
# announcement is trusted before we consider the peer gone from the
# mesh. Interval must divide into TTL several times so a single
# dropped datagram doesn't evict a peer.
ANNOUNCE_INTERVAL = 2.0
ANNOUNCE_TTL = 10.0

log = logging.getLogger(__name__)


# ── Shared helpers ──────────────────────────────────────────────────


@dataclass
class DiscoveredHost:
    ip: str
    port: int
    hostname: str

    @property
    def label(self) -> str:
        return f"{self.hostname} ({self.ip}:{self.port})"


@dataclass
class MeshPeerAnnounce:
    """One live peer learned from an ANNOUNCE datagram."""
    machine_id: str
    hostname: str
    ip: str
    tcp_port: int
    last_seen_monotonic: float


def local_identity() -> tuple[str, set[str]]:
    """Return (hostname, {ipv4}) for the machine we're running on.

    Used by the UI to detect when a discovered host is actually this
    same PC, so we can disable connecting to ourselves.
    """
    ips: set[str] = {"127.0.0.1"}
    for iface in local_ipv4_interfaces():
        ips.add(iface.ip)
    if not ips - {"127.0.0.1"}:
        # Fallback: when psutil isn't available and enumeration fails,
        # the UDP-connect trick still tells us the default route IP.
        for probe in (("8.8.8.8", 80), ("1.1.1.1", 80)):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.settimeout(0.2)
                try:
                    s.connect(probe)
                    ips.add(s.getsockname()[0])
                finally:
                    s.close()
            except OSError:
                continue
    try:
        host = socket.gethostname()
        for ip in socket.gethostbyname_ex(host)[2]:
            ips.add(ip)
    except OSError:
        host = socket.gethostname() if hasattr(socket, "gethostname") else ""
    return (host, ips)


@dataclass(frozen=True)
class LocalInterface:
    name: str
    ip: str
    broadcast: str

    @property
    def subnet_broadcast(self) -> str:
        """Return the /24 broadcast derived from this interface's IP —
        falls back to what psutil reported if the subnet isn't /24."""
        if self.broadcast:
            return self.broadcast
        octets = self.ip.split(".")
        if len(octets) == 4:
            return f"{octets[0]}.{octets[1]}.{octets[2]}.255"
        return "255.255.255.255"


def local_ipv4_interfaces() -> list[LocalInterface]:
    """Enumerate every non-loopback IPv4 interface + its broadcast.

    psutil when available (all three OSes); falls back to the
    UDP-connect trick otherwise (catches the default-route interface
    only — multi-homed setups lose coverage without psutil)."""
    out: list[LocalInterface] = []
    if _HAS_PSUTIL:
        try:
            for name, addrs in psutil.net_if_addrs().items():
                for a in addrs:
                    if getattr(a, "family", None) != socket.AF_INET:
                        continue
                    ip = (a.address or "").strip()
                    if not ip or ip.startswith("127."):
                        continue
                    bcast = (a.broadcast or "").strip()
                    out.append(LocalInterface(name=name, ip=ip,
                                              broadcast=bcast))
        except Exception:
            # psutil can raise on exotic setups — fall through to the
            # single-interface fallback rather than returning nothing.
            out = []
    if not out:
        for probe in (("8.8.8.8", 80), ("1.1.1.1", 80)):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.settimeout(0.2)
                try:
                    s.connect(probe)
                    ip = s.getsockname()[0]
                    if ip and not ip.startswith("127."):
                        out.append(LocalInterface(
                            name="default", ip=ip, broadcast="",
                        ))
                finally:
                    s.close()
            except OSError:
                continue
    # Dedupe by IP so multiple aliases on the same interface only
    # send one datagram per announce tick.
    seen = set()
    deduped = []
    for iface in out:
        if iface.ip in seen:
            continue
        seen.add(iface.ip)
        deduped.append(iface)
    return deduped


# ── Mesh announce / listen ──────────────────────────────────────────


class _MeshListenProtocol(asyncio.DatagramProtocol):
    """Receives ANNOUNCE datagrams from every peer on the LAN and
    fans them into a callback. One instance per process."""

    def __init__(self, on_announce: Callable[[MeshPeerAnnounce], None]):
        self.on_announce = on_announce

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            payload = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if payload.get("magic") != _ANNOUNCE_MAGIC:
            return
        mid = str(payload.get("machine_id") or "")
        if not mid:
            return
        try:
            tcp_port = int(payload.get("tcp_port") or 0)
        except (TypeError, ValueError):
            return
        if tcp_port <= 0:
            return
        import time
        self.on_announce(MeshPeerAnnounce(
            machine_id=mid,
            hostname=str(payload.get("hostname") or ""),
            ip=addr[0],
            tcp_port=tcp_port,
            last_seen_monotonic=time.monotonic(),
        ))


class MeshDiscovery:
    """Continuously announce our presence + track every other peer.

    Opens one listening socket on UDP DISCOVERY_PORT (SO_REUSEADDR so
    a forked helper can share it) and one broadcast socket per local
    IPv4 interface. Every ANNOUNCE_INTERVAL we blast a JSON payload
    on each broadcast socket, so peers connected via cable and peers
    connected via WiFi both learn about us without the user having
    to pick an interface.

    Peers discovered via inbound ANNOUNCEs land in `self.peers`, keyed
    by machine_id. Peers idle for longer than ANNOUNCE_TTL are
    evicted by `_sweep_stale` so leavers clear out even if they
    didn't send a graceful goodbye.
    """

    def __init__(self, machine_id: str, hostname: str, tcp_port: int,
                 *,
                 listen_port: int = DISCOVERY_PORT,
                 on_peer_changed: Optional[Callable[[], None]] = None):
        self.machine_id = machine_id
        self.hostname = hostname
        self.tcp_port = tcp_port
        self.listen_port = listen_port
        self._on_peer_changed = on_peer_changed
        self.peers: dict[str, MeshPeerAnnounce] = {}

        self._listen_transport: Optional[asyncio.DatagramTransport] = None
        self._broadcast_socks: list[socket.socket] = []
        self._announce_task: Optional[asyncio.Task] = None
        self._sweep_task: Optional[asyncio.Task] = None
        self._running = False

    async def start(self) -> None:
        loop = asyncio.get_running_loop()

        # Listener — one socket on 0.0.0.0 grabs inbound datagrams on
        # every interface. SO_REUSEADDR so a previous crash's lingering
        # socket doesn't hit us with EADDRINUSE on restart.
        listen_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            listen_sock.bind(("0.0.0.0", self.listen_port))
        except OSError:
            listen_sock.close()
            raise
        self._listen_transport, _ = await loop.create_datagram_endpoint(
            lambda: _MeshListenProtocol(self._on_announce_received),
            sock=listen_sock,
        )

        # Broadcast sockets — one per interface. Binding each to the
        # interface's own IP forces the outbound packet onto that
        # NIC, so cable + WiFi both get announcements (binding to
        # 0.0.0.0 only would let the kernel pick one by routing).
        ifaces = local_ipv4_interfaces()
        log.info("Mesh announce: %d interface(s): %s",
                 len(ifaces),
                 ", ".join(f"{i.name}={i.ip}" for i in ifaces) or "<none>")
        for iface in ifaces:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((iface.ip, 0))
                self._broadcast_socks.append(s)
            except OSError as e:
                log.warning("Mesh announce: bind on %s (%s) failed: %s",
                            iface.name, iface.ip, e)

        self._running = True
        self._announce_task = asyncio.create_task(self._announce_loop())
        self._sweep_task = asyncio.create_task(self._sweep_loop())

    async def stop(self) -> None:
        self._running = False
        for task in (self._announce_task, self._sweep_task):
            if task is not None:
                task.cancel()
                try:
                    await task
                except (asyncio.CancelledError, Exception):
                    pass
        self._announce_task = None
        self._sweep_task = None
        if self._listen_transport is not None:
            self._listen_transport.close()
            self._listen_transport = None
        for s in self._broadcast_socks:
            try:
                s.close()
            except OSError:
                pass
        self._broadcast_socks = []

    # ── internals ────────────────────────────────────────────────

    async def _announce_loop(self) -> None:
        payload = json.dumps({
            "magic": _ANNOUNCE_MAGIC,
            "machine_id": self.machine_id,
            "hostname": self.hostname,
            "tcp_port": self.tcp_port,
        }).encode("utf-8")
        while self._running:
            for sock in self._broadcast_socks:
                try:
                    sock.sendto(payload,
                                ("255.255.255.255", self.listen_port))
                except OSError as e:
                    log.debug("announce send failed: %s", e)
            try:
                await asyncio.sleep(ANNOUNCE_INTERVAL)
            except asyncio.CancelledError:
                break

    async def _sweep_loop(self) -> None:
        import time
        while self._running:
            try:
                await asyncio.sleep(ANNOUNCE_INTERVAL)
            except asyncio.CancelledError:
                break
            now = time.monotonic()
            stale = [mid for mid, p in self.peers.items()
                     if now - p.last_seen_monotonic > ANNOUNCE_TTL]
            for mid in stale:
                del self.peers[mid]
                log.info("Mesh peer %s went stale, evicted", mid)
            # Fire on_peer_changed every sweep so the UI can refresh
            # its "seen in the last few seconds" filter even when no
            # peer joined or left — otherwise a just-disconnected
            # peer keeps showing up for the full 10 s TTL.
            if self._on_peer_changed:
                try:
                    self._on_peer_changed()
                except Exception:
                    log.exception("on_peer_changed callback raised")

    def _on_announce_received(self, announce: MeshPeerAnnounce) -> None:
        if announce.machine_id == self.machine_id:
            return  # our own echo
        prev = self.peers.get(announce.machine_id)
        self.peers[announce.machine_id] = announce
        if prev is None or prev.ip != announce.ip \
                or prev.tcp_port != announce.tcp_port:
            log.info("Mesh peer seen: %s@%s:%d",
                     announce.machine_id, announce.ip, announce.tcp_port)
            if self._on_peer_changed:
                try:
                    self._on_peer_changed()
                except Exception:
                    log.exception("on_peer_changed callback raised")


# ── Legacy probe/reply ──────────────────────────────────────────────


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
            log.warning("Discovery reply to %s failed: %s", addr, e)


class DiscoveryResponder:
    """Legacy — answers on-demand probes. Kept so old clients that
    don't speak the ANNOUNCE protocol can still find this PC."""

    def __init__(self, hostname: str, tcp_port: int,
                 listen_port: int = DISCOVERY_PORT):
        self.hostname = hostname
        self.tcp_port = tcp_port
        self.listen_port = listen_port
        self._transport: Optional[asyncio.DatagramTransport] = None

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
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
        self.results[(ip, port)] = DiscoveredHost(
            ip=ip, port=port, hostname=hostname,
        )


async def discover_hosts(
    timeout: float = 1.5,
    listen_port: int = DISCOVERY_PORT,
) -> list[DiscoveredHost]:
    """Broadcast a probe on every local interface and collect replies
    within `timeout`. Legacy API used by the shell's Find hosts
    dialog; the mesh uses `MeshDiscovery` instead."""
    loop = asyncio.get_running_loop()
    results: dict[tuple[str, int], DiscoveredHost] = {}

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("0.0.0.0", 0))
    transport, _ = await loop.create_datagram_endpoint(
        lambda: _ClientProtocol(results), sock=sock,
    )

    probe = json.dumps({"magic": _PROBE_MAGIC}).encode("utf-8")
    targets = {"255.255.255.255"}
    for iface in local_ipv4_interfaces():
        targets.add(iface.subnet_broadcast)
    try:
        for target in sorted(targets):
            try:
                transport.sendto(probe, (target, listen_port))
            except OSError as e:
                log.warning("Discovery probe to %s failed: %s", target, e)
        await asyncio.sleep(timeout)
    finally:
        transport.close()
    return sorted(results.values(), key=lambda h: (h.hostname.lower(), h.ip))

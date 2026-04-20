"""Full-mesh P2P peer.

Replaces `unio/apps/server.py` and `unio/apps/client.py`. Every PC
running unIO launches exactly one `Peer`, which:

- Listens on TCP 24800 for inbound mesh connections from other peers.
- Opens outbound connections to peers learned via `MeshDiscovery`,
  using a deterministic machine_id tiebreak so only one side dials.
- Captures local keyboard + mouse via the platform backend and
  forwards them, when FORWARDING, directly to whichever peer
  currently owns the shared cursor.
- Injects remote input when it's the active peer.
- Replicates shared state (layout, per-machine mute / clipboard
  toggles, active-machine pointer) via LWW (Last-Writer-Wins)
  gossip on every connected peer link.
- Hands off the cursor directly peer-to-peer via CURSOR_RELEASE,
  then gossips the new `active` so the rest of the mesh agrees.

There's no "server" — every peer keeps a full copy of the state,
and a peer dying just removes it from the mesh without taking the
others down.
"""

from __future__ import annotations

import asyncio
import atexit
import enum
import logging
import platform
import socket
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from ..backends import InputBackend, create_backend
from ..core.layout import LayoutManager, GlobalMonitor
from ..core.network import Connection
from ..core.protocol import (
    MsgType,
    ClipboardUpdateMsg, CursorReleaseMsg, EdgeHitMsg, SwapInputMsg,
    HeartbeatMsg, HelloMsg, IdentifyMsg,
    KeyEventMsg, MouseButtonMsg, MouseMoveRelMsg, MouseScrollMsg,
    SetStateMsg, StateSyncMsg,
)
from ..features.display_stream import StreamServer
from ..features.filetransfer import FileTransferReceiver

log = logging.getLogger(__name__)

POLL_HZ = 120
EDGE_MARGIN = 2
CLIPBOARD_POLL_INTERVAL = 0.5
HEARTBEAT_INTERVAL = 5.0
HEARTBEAT_TIMEOUT = 15.0


class Mode(enum.Enum):
    ACTIVE = "active"
    FORWARDING = "forwarding"


# ── LWW store ────────────────────────────────────────────────────────


@dataclass
class LWWEntry:
    ts_clock: int
    ts_machine: str
    value: Any

    @property
    def ts(self) -> tuple[int, str]:
        return (self.ts_clock, self.ts_machine)


class LWWStore:
    """Flat dict of Last-Writer-Wins registers keyed by string."""

    def __init__(self, local_machine_id: str):
        self._local = local_machine_id
        self._clock = 0
        self._entries: dict[str, LWWEntry] = {}

    def get(self, key: str, default: Any = None) -> Any:
        e = self._entries.get(key)
        return e.value if e else default

    def set_local(self, key: str, value: Any) -> LWWEntry:
        """Bump the clock past everything we've seen and write our own
        entry. Returns the new entry so callers can gossip it."""
        existing = self._entries.get(key)
        self._clock = max(
            self._clock,
            existing.ts_clock if existing else 0,
        ) + 1
        entry = LWWEntry(self._clock, self._local, value)
        self._entries[key] = entry
        return entry

    def apply_remote(self, key: str, entry: LWWEntry) -> bool:
        """Accept a remote entry if its ts wins; bump our clock so the
        next local write sits after anything we've seen."""
        existing = self._entries.get(key)
        if existing is None or entry.ts > existing.ts:
            self._entries[key] = entry
            self._clock = max(self._clock, entry.ts_clock)
            return True
        return False

    def iter_keys(self):
        return list(self._entries.keys())

    def dump(self) -> dict:
        return {
            k: [e.ts_clock, e.ts_machine, e.value]
            for k, e in self._entries.items()
        }

    def load(self, data: dict) -> list[str]:
        """Merge a remote dump; return keys whose value changed."""
        changed: list[str] = []
        for k, triplet in (data or {}).items():
            if not isinstance(triplet, (list, tuple)) or len(triplet) != 3:
                continue
            ts_clock, ts_machine, value = triplet
            try:
                clock = int(ts_clock)
            except (TypeError, ValueError):
                continue
            if self.apply_remote(k, LWWEntry(clock, str(ts_machine), value)):
                changed.append(k)
        return changed


# ── Helpers ──────────────────────────────────────────────────────────


def describe_platform() -> str:
    system = platform.system()
    if system == "Linux":
        try:
            info = platform.freedesktop_os_release()
            return info.get("PRETTY_NAME") or info.get("NAME", "Linux")
        except (AttributeError, OSError):
            return f"Linux {platform.release()}"
    if system == "Darwin":
        return f"macOS {platform.mac_ver()[0] or platform.release()}"
    if system == "Windows":
        ver = platform.win32_ver()
        return f"Windows {ver[0] or platform.release()}"
    return f"{system} {platform.release()}".strip()


@dataclass
class PeerLink:
    """One established mesh connection. Owns the Connection,
    tracks last-ack for the heartbeat watchdog, and stamps the
    counterparty's identity learned from HELLO."""
    machine_id: str
    hostname: str
    conn: Connection
    initiator: bool                           # we dialed out
    last_ack_monotonic: float = 0.0
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    async def send(self, msg_type: MsgType, payload) -> bool:
        try:
            async with self.send_lock:
                await self.conn.send(msg_type, payload)
            return True
        except Exception as e:
            log.debug("link %s: send %s failed: %s",
                      self.machine_id, msg_type.name, e)
            return False


# ── Peer ─────────────────────────────────────────────────────────────


class Peer:
    """Single process's full-mesh presence. All-in-one listener,
    client, input capture, injector, state store, and routing."""

    def __init__(self, machine_id: str, tcp_port: int = 24800):
        self.machine_id = machine_id
        self.tcp_port = tcp_port
        self.hostname = socket.gethostname() or machine_id

        # LWW replicated state
        self.lww = LWWStore(machine_id)

        # Mesh link table
        self.links: dict[str, PeerLink] = {}

        # Backend
        self._backend_main: Optional[InputBackend] = None
        self._backend_input: Optional[InputBackend] = None

        # Mode + local derived flags
        self.mode = Mode.ACTIVE
        self.is_muted = False
        self.clipboard_sync_enabled = True

        # Layout for cursor routing
        self.layout = LayoutManager()
        self.my_monitors: list = []

        # Input thread
        self._input_thread: Optional[threading.Thread] = None
        self._running = False
        self._lock = threading.Lock()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._warp_cx = 0
        self._warp_cy = 0
        self._prev_btn_mask = 0
        self._edge_hit_sent = False

        # Clipboard
        self._last_clipboard = ""
        self._pending_echo: Optional[str] = None

        # File transfer
        self._file_receiver = FileTransferReceiver()

        # Display streaming (Phase 1). Dedicated TCP server on its own
        # port that fans JPEG-encoded frames out to any sink that asks
        # for them. Comes up with the peer, dies with the peer.
        self.stream_server: Optional[StreamServer] = StreamServer(
            machine_id=self.machine_id,
        )

        # Hooks
        self.identify_sink: Optional[Callable[[list, int], None]] = None
        self.on_state_changed: Optional[Callable[[], None]] = None

        # ── Workspace gating ───────────────────────────────────────
        # Set of machine_ids this peer is actively sharing with — i.e.
        # the members of the currently-active workspace on this box,
        # including self. When empty, nothing is shared: cursor never
        # hands off to a non-member, mouse/key injections from a
        # non-member are dropped, clipboard stays local. Discovery +
        # LWW gossip still flow to every authed mesh peer (so every
        # PC still sees the list of workspaces), but user-visible
        # sharing only happens inside a workspace.
        self.allowed_peer_ids: set[str] = {self.machine_id}

        # Active workspace's behaviour knobs, pushed down from the
        # shell whenever the user flips the workspace or edits its
        # settings. Defaults preserve the previous constant
        # behaviour so a pre-schema-bump workspace still feels the
        # same as before.
        self.ws_edge_margin: int = EDGE_MARGIN
        self.ws_require_modifier: bool = False
        self.ws_block_os_hotkeys: bool = False
        self.ws_cb_max_bytes: int = 0  # 0 = no limit

        # Active workspace's route map: sink_key → source_key. Shell
        # pushes fresh values whenever the active workspace or the
        # LWW routes change. When the cursor crosses onto one of this
        # peer's own monitors and that monitor is a routed sink with
        # a remote source, we hand off to the source machine instead
        # of keeping the cursor local — the user's pixels are
        # actually on that remote, so the input should follow.
        self.active_routes: dict[str, str] = {}

        # Anti-bounce gate: monitor_id of the most recent swap-handoff
        # destination on this peer. Set either when we hand the
        # cursor off (so we don't immediately re-hand on the next
        # poll if the cursor happens to sit on another swap-routed
        # local monitor) or when we receive a cursor warp onto one
        # of our own swap-routed monitors (so we don't bounce the
        # cursor right back to the sender). Cleared whenever the
        # cursor moves off that monitor.
        self._last_swap_handoff_monitor: str | None = None

        # TCP listener + background task handles
        self._server = None
        self._background_tasks: list[asyncio.Task] = []

    # ── Lifecycle ────────────────────────────────────────────────

    async def start(self) -> None:
        """Open the TCP listener, seed our own presence, spin up the
        input thread + background tasks. Returns as soon as we're
        accepting connections — caller keeps the process alive by
        awaiting `serve_forever` (or by just leaving tasks running).
        Splitting start from serve lets the shell surface listener
        bind errors without hanging the UI."""
        self._loop = asyncio.get_running_loop()
        self._backend_main = create_backend()
        self._backend_main.open()
        atexit.register(self._restore_local_state)

        self.my_monitors = self._snapshot_local_monitors()
        self._init_self_in_state()

        self._server = await asyncio.start_server(
            self._handle_inbound, host="0.0.0.0", port=self.tcp_port,
        )
        log.info("Peer %s listening on TCP %d", self.machine_id, self.tcp_port)

        self._running = True
        self._input_thread = threading.Thread(
            target=self._input_loop, daemon=True, name="unio-input",
        )
        self._input_thread.start()

        self._background_tasks.append(asyncio.create_task(self._clipboard_poll_loop()))
        self._background_tasks.append(asyncio.create_task(self._heartbeat_loop()))

        if self.stream_server is not None:
            try:
                await self.stream_server.start()
                self.stream_server.set_monitors(self.my_monitors)
            except Exception:
                log.exception("stream server start failed")

    async def serve_forever(self) -> None:
        if self._server is None:
            raise RuntimeError("Peer.start() must be called first")
        try:
            await self._server.serve_forever()
        except asyncio.CancelledError:
            pass

    async def stop(self) -> None:
        self._running = False
        if self.stream_server is not None:
            try:
                await self.stream_server.stop()
            except Exception:
                log.exception("stream server stop failed")
        if self._server:
            self._server.close()
            try:
                await self._server.wait_closed()
            except Exception:
                pass
            self._server = None
        for task in self._background_tasks:
            task.cancel()
        self._background_tasks = []
        for link in list(self.links.values()):
            try:
                await link.conn.close()
            except Exception:
                pass
        self.links.clear()
        self._restore_local_state()
        if self._input_thread and self._input_thread.is_alive():
            self._input_thread.join(timeout=1.0)
            self._input_thread = None

    def _restore_local_state(self) -> None:
        if self._backend_main:
            try:
                self._backend_main.stop_pointer_block()
            except Exception:
                pass
            try:
                self._backend_main.show_cursor()
            except Exception:
                pass
        if self._backend_input:
            try:
                self._backend_input.stop_key_capture()
            except Exception:
                pass

    # ── Presence / initial state ─────────────────────────────────

    def _snapshot_local_monitors(self) -> list[dict]:
        if not self._backend_main:
            return []
        try:
            rects = self._backend_main.query_monitors()
        except Exception:
            log.exception("query_monitors failed")
            return []
        return [
            {"monitor_id": r.id, "width": r.width, "height": r.height,
             "local_x": r.x, "local_y": r.y}
            for r in rects
        ]

    def _init_self_in_state(self) -> None:
        """Seed our own presence + seed empty flags so newcomers
        have a deterministic starting view of us. Each monitor
        carries its local_x/y so a pre-Apply layout can fan a
        multi-monitor machine's displays out side-by-side rather
        than stacking them at the same fallback point."""
        self._lww_write(f"presence:{self.machine_id}",
                        self._own_presence_payload(), broadcast=False)
        if self.lww.get("active") is None:
            self._lww_write("active", self.machine_id, broadcast=False)

    def _reassert_own_presence_if_stomped(self) -> None:
        """If our presence register got overwritten by somebody else
        (a tombstone from a peer that saw our previous session end),
        rewrite it with a clock higher than theirs and gossip.
        No-op when our presence is still our own."""
        key = f"presence:{self.machine_id}"
        entry = self.lww._entries.get(key)
        if entry is None:
            return
        needs = entry.value is None or entry.ts_machine != self.machine_id
        if not needs:
            return
        log.info("Re-asserting own presence after stomp "
                 "(was ts=(%d,%s), value=%s)",
                 entry.ts_clock, entry.ts_machine,
                 "None" if entry.value is None else "remote")
        self._lww_write(key, self._own_presence_payload())

    def _own_presence_payload(self) -> dict:
        return {
            "hostname": self.hostname,
            "os": platform.system(),
            "platform_info": describe_platform(),
            "monitors": [
                {"monitor_id": m["monitor_id"],
                 "width": m["width"], "height": m["height"],
                 "local_x": int(m.get("local_x", 0)),
                 "local_y": int(m.get("local_y", 0))}
                for m in self.my_monitors
            ],
        }

    # ── Mesh connections ─────────────────────────────────────────

    async def _handle_inbound(self, reader, writer) -> None:
        conn = Connection(reader, writer, label="inbound")
        await self._handshake(conn, initiator=False)

    async def connect_to(self, ip: str, port: int) -> None:
        """Dial out to a peer. Skipped if we already have a live link
        to that machine_id (post-HELLO). Used by the shell whenever
        MeshDiscovery learns a peer we should initiate to."""
        try:
            reader, writer = await asyncio.open_connection(ip, port)
        except OSError as e:
            log.debug("dial %s:%d failed: %s", ip, port, e)
            return
        conn = Connection(reader, writer, label=f"out:{ip}:{port}")
        await self._handshake(conn, initiator=True)

    async def _handshake(self, conn: Connection, initiator: bool) -> None:
        """Exchange HELLO, register the link, pump its recv loop."""
        hello = HelloMsg(
            machine_id=self.machine_id,
            hostname=self.hostname,
            os=platform.system(),
            platform_info=describe_platform(),
            monitors=self.my_monitors,
            initiator=initiator,
        )
        try:
            await conn.send(MsgType.HELLO, hello)
        except Exception as e:
            log.debug("HELLO send failed: %s", e)
            await conn.close()
            return

        result = await conn.recv()
        if result is None:
            await conn.close()
            return
        mt, payload = result
        if mt != MsgType.HELLO or not isinstance(payload, HelloMsg):
            log.warning("Expected HELLO first, got %s; dropping link", mt)
            await conn.close()
            return
        their_id = payload.machine_id
        if their_id == self.machine_id:
            # Talking to ourselves via loopback — drop cleanly.
            await conn.close()
            return
        if their_id in self.links:
            # Duplicate: collision with a pre-existing link (both sides
            # dialed out around the same time). Keep whichever side
            # has the smaller machine_id as the "dialer" per the
            # deterministic rule; drop ours if it's the loser.
            existing = self.links[their_id]
            if existing.initiator == initiator:
                # Same-role duplicate — keep first, drop this.
                await conn.close()
                return
            # Different-role duplicate: keep the connection where
            # the smaller machine_id is the initiator.
            want_initiator_is_smaller = (self.machine_id < their_id)
            our_is_canonical = (initiator == want_initiator_is_smaller)
            if not our_is_canonical:
                await conn.close()
                return
            # Evict the old one.
            try:
                await existing.conn.close()
            except Exception:
                pass

        link = PeerLink(
            machine_id=their_id, hostname=payload.hostname,
            conn=conn, initiator=initiator,
            last_ack_monotonic=time.monotonic(),
        )
        self.links[their_id] = link
        log.info("Mesh link up: %s (initiator=%s)", their_id, initiator)

        # Absorb the counterpart's declared presence so it shows in
        # Activity + layout even before STATE_SYNC arrives.
        self._merge_hello_presence(payload)

        # BOTH sides push STATE_SYNC after HELLO. One-directional
        # (initiator-only) left the initiator blind to any tombstones
        # the counterparty was holding about them — e.g. a restart
        # where A still had presence:B=None from B's previous session
        # meant B's fresh presence lost the LWW compare and B stayed
        # invisible on A. Exchanging both dumps lets either side's
        # _reassert_own_presence_if_stomped fire and rebroadcast.
        await link.send(MsgType.STATE_SYNC,
                        StateSyncMsg(entries=self.lww.dump()))

        self._rebuild_global_layout()
        self._notify_state_changed()

        try:
            await self._recv_loop(link)
        finally:
            # Link closed — evict.
            self.links.pop(their_id, None)
            log.info("Mesh link down: %s", their_id)
            self._on_peer_gone(their_id)

    async def _recv_loop(self, link: PeerLink) -> None:
        while not link.conn.closed:
            result = await link.conn.recv()
            if result is None:
                break
            msg_type, payload = result
            try:
                await self._dispatch(link, msg_type, payload)
            except Exception:
                log.exception("dispatch error for %s from %s",
                              msg_type, link.machine_id)

    # ── Message dispatch ─────────────────────────────────────────

    async def _dispatch(self, link: PeerLink,
                        msg_type: MsgType, payload) -> None:
        link.last_ack_monotonic = time.monotonic()
        if msg_type == MsgType.HEARTBEAT:
            await link.send(MsgType.HEARTBEAT_ACK, payload)
        elif msg_type == MsgType.HEARTBEAT_ACK:
            pass
        elif msg_type == MsgType.STATE_SYNC:
            changed = self.lww.load(getattr(payload, "entries", {}) or {})
            # After a cold restart, the rest of the mesh may still have
            # our presence tombstoned (the peer that saw us go offline
            # wrote presence:{us}=None at a higher clock). Re-assert
            # our own presence now that we know the mesh's latest
            # clock, so peers learn we're alive again.
            self._reassert_own_presence_if_stomped()
            if changed:
                for k in changed:
                    self._apply_state_side_effect(k)
                self._rebuild_global_layout()
                self._notify_state_changed()
        elif msg_type == MsgType.SET_STATE:
            key = getattr(payload, "key", "")
            if key:
                entry = LWWEntry(
                    int(getattr(payload, "ts_clock", 0)),
                    str(getattr(payload, "ts_machine", "")),
                    getattr(payload, "value", None),
                )
                if self.lww.apply_remote(key, entry):
                    self._apply_state_side_effect(key)
                    # If a peer just stomped our presence (e.g. a
                    # late-arriving tombstone from our own previous
                    # session), immediately rebroadcast the truth.
                    if key == f"presence:{self.machine_id}":
                        self._reassert_own_presence_if_stomped()
                    self._rebuild_global_layout()
                    self._notify_state_changed()
        elif msg_type == MsgType.CURSOR_RELEASE:
            if link.machine_id not in self.allowed_peer_ids:
                return  # not in our active workspace — ignore
            if self.lww.get(f"muted:{link.machine_id}", False):
                # A muted peer shouldn't be able to fling the cursor
                # across to us. They self-block locally, but accept a
                # late CURSOR_RELEASE too just in case the mute hasn't
                # propagated to them yet.
                return
            await self._accept_cursor(payload)
        elif msg_type in (MsgType.MOUSE_MOVE_REL, MsgType.MOUSE_BUTTON,
                          MsgType.MOUSE_SCROLL):
            if link.machine_id not in self.allowed_peer_ids:
                return
            if self.lww.get(f"muted:{link.machine_id}", False):
                return
            await self._inject_mouse(msg_type, payload)
        elif msg_type == MsgType.KEY_EVENT:
            if link.machine_id not in self.allowed_peer_ids:
                return
            if self.lww.get(f"muted:{link.machine_id}", False):
                return
            await self._inject_key(payload)
        elif msg_type == MsgType.SWAP_INPUT:
            if link.machine_id not in self.allowed_peer_ids:
                return
            if self.lww.get(f"muted:{link.machine_id}", False):
                return
            await self._inject_swap_input(payload)
        elif msg_type == MsgType.CLIPBOARD_UPDATE:
            if link.machine_id not in self.allowed_peer_ids:
                return
            self._handle_clipboard_incoming(payload)
        elif msg_type == MsgType.IDENTIFY:
            self._show_identify(payload)
        elif msg_type == MsgType.EDGE_HIT:
            # In pure mesh, EDGE_HIT is computed locally and converted
            # straight into CURSOR_RELEASE. Any peer still sending
            # EDGE_HIT as a request is on an older build — ignore.
            pass
        elif msg_type in (MsgType.FILE_OFFER, MsgType.FILE_CHUNK,
                          MsgType.FILE_DONE):
            # File transfer is peer-to-peer in the mesh too: whichever
            # peer the sender targeted receives + saves. No relay.
            self._handle_file_message(msg_type, payload)
        else:
            log.debug("unhandled msg %s from %s",
                      msg_type, link.machine_id)

    def _handle_file_message(self, msg_type: MsgType, payload) -> None:
        # FileTransferReceiver wants plain dicts, but peer protocol
        # arrives as dataclasses.
        from dataclasses import asdict, is_dataclass
        data = asdict(payload) if is_dataclass(payload) \
            else (payload if isinstance(payload, dict) else {})
        try:
            if msg_type == MsgType.FILE_OFFER:
                self._file_receiver.handle_offer(data)
            elif msg_type == MsgType.FILE_CHUNK:
                self._file_receiver.handle_chunk(data)
            elif msg_type == MsgType.FILE_DONE:
                saved = self._file_receiver.handle_done(data)
                if saved:
                    log.info("File received and saved: %s", saved)
        except Exception:
            log.exception("file transfer handler failed")

    # ── LWW write + gossip ───────────────────────────────────────

    def _lww_write(self, key: str, value: Any, *,
                   broadcast: bool = True) -> None:
        entry = self.lww.set_local(key, value)
        self._apply_state_side_effect(key)
        self._rebuild_global_layout()
        if broadcast:
            self._gossip(key, entry)
        self._notify_state_changed()

    def _gossip(self, key: str, entry: LWWEntry) -> None:
        if not self._loop:
            return
        msg = SetStateMsg(
            key=key,
            ts_clock=entry.ts_clock,
            ts_machine=entry.ts_machine,
            value=entry.value,
        )
        for link in list(self.links.values()):
            asyncio.run_coroutine_threadsafe(
                link.send(MsgType.SET_STATE, msg), self._loop,
            )

    def _apply_state_side_effect(self, key: str) -> None:
        """Mirror LWW changes to the client-like runtime state so the
        input thread / apply_monitors / cursor hiding all pick up the
        new value without separate message handlers."""
        if key == f"muted:{self.machine_id}" or key == f"mute:{self.machine_id}":
            new_muted = bool(self.lww.get(f"muted:{self.machine_id}",
                                          False))
            if new_muted != self.is_muted:
                self.is_muted = new_muted
                self._apply_local_input_state()
                # In FORWARDING, mute toggles flip cursor visibility:
                # a muted forwarding PC shows its local cursor, a
                # non-muted forwarding PC hides (shared cursor is
                # elsewhere). ACTIVE is always visible.
                if self._backend_main and self.mode == Mode.FORWARDING:
                    try:
                        if self.is_muted:
                            self._backend_main.show_cursor()
                        else:
                            self._backend_main.hide_cursor()
                    except Exception:
                        pass
        elif key == f"cb_off:{self.machine_id}":
            off = bool(self.lww.get(f"cb_off:{self.machine_id}", False))
            self.clipboard_sync_enabled = not off
        elif key == "active":
            active = self.lww.get("active")
            # Mode transitions are gated on workspace membership: if
            # the "active" peer sits outside our current workspace,
            # their cursor ownership is none of our business — we stay
            # ACTIVE locally so the user doesn't see their cursor
            # vanish just because peer B happened to boot.
            should_be_active = (
                active == self.machine_id
                or not active
                or active not in self.allowed_peer_ids
            )
            if should_be_active:
                if self.mode != Mode.ACTIVE:
                    self._enter_active(-1, -1)
            else:
                if self.mode != Mode.FORWARDING:
                    self._enter_forwarding()
        elif key == "layout":
            # Someone applied a new layout — set_monitor_positions
            # for our own monitors if any entries target us.
            self._maybe_apply_own_monitor_positions()

    # ── Global layout assembly ───────────────────────────────────

    def _rebuild_global_layout(self) -> None:
        """Rebuild self.layout from LWW state so EDGE_HIT detection
        walks the agreed-upon global coord space. Pre-Apply fallback
        stacks machines end-to-end along x (sorted by machine_id so
        every peer agrees on the initial ordering), preserving each
        machine's own monitor arrangement within its block, so the
        Layout canvas shows every display right-next-to-the-previous-
        PC and the user just rearranges from there."""
        positions = self.lww.get("layout") or []
        by_mid_mon = {(p["machine_id"], p["monitor_id"]):
                      (p.get("global_x", 0), p.get("global_y", 0))
                      for p in positions if isinstance(p, dict)}

        self.layout.clear()
        known_mids = sorted(k for k in self.lww.iter_keys()
                            if k.startswith("presence:"))
        # Limit the layout to the workspace we're actively sharing in.
        # allowed_peer_ids always contains self, so a lone PC just sees
        # its own monitors (no edges cross to anyone else). An empty
        # workspace ("no active workspace") means allowed = {self}.
        allowed = self.allowed_peer_ids
        known_mids = [k for k in known_mids
                      if k.split(":", 1)[1] in allowed]
        cursor_x = 0
        for key in known_mids:
            mid = key.split(":", 1)[1]
            info = self.lww.get(key) or {}
            monitors = info.get("monitors") or []
            if not monitors:
                continue
            local_right = max(
                int(m.get("local_x", 0)) + int(m.get("width", 0))
                for m in monitors
            )
            by_mid: list = []
            origin_dx: Optional[int] = None
            origin_dy: Optional[int] = None
            for mon in monitors:
                local_x = int(mon.get("local_x", 0))
                local_y = int(mon.get("local_y", 0))
                gx, gy = by_mid_mon.get(
                    (mid, mon.get("monitor_id")),
                    (cursor_x + local_x, local_y),
                )
                by_mid.append(GlobalMonitor(
                    machine_id=mid,
                    monitor_id=mon.get("monitor_id"),
                    global_x=gx,
                    global_y=gy,
                    width=int(mon.get("width", 0)),
                    height=int(mon.get("height", 0)),
                ))
                # Origin = peer's virtual-screen (0, 0) in our global
                # coords. Derived as global - local for any of the
                # peer's monitors; they all agree when a peer's block
                # was placed as a rigid unit. Using the FIRST monitor
                # is sufficient.
                if origin_dx is None:
                    origin_dx = gx - local_x
                    origin_dy = gy - local_y
            self.layout.monitors.extend(by_mid)
            if origin_dx is not None:
                self.layout._machine_origin[mid] = (origin_dx, origin_dy)
            cursor_x += local_right
        self.layout._rebuild_adjacency()

    def _maybe_apply_own_monitor_positions(self) -> None:
        """If a layout apply changed our own monitor positions,
        push the OS-level reconfiguration via the backend."""
        if not self._backend_main:
            return
        positions = self.lww.get("layout") or []
        mine = {p["monitor_id"]: (p["global_x"], p["global_y"])
                for p in positions
                if isinstance(p, dict) and p.get("machine_id") == self.machine_id}
        if not mine:
            return
        # Normalize to origin-relative (OS APIs want a relative map).
        origin_x = min(x for x, _ in mine.values())
        origin_y = min(y for _, y in mine.values())
        rel = {mid: (gx - origin_x, gy - origin_y)
               for mid, (gx, gy) in mine.items()}

        def _apply():
            try:
                self._backend_main.set_monitor_positions(rel)
            except Exception:
                log.exception("set_monitor_positions failed")
        threading.Thread(target=_apply, daemon=True,
                         name="apply-monitors").start()

    # ── Presence merge from HELLO ────────────────────────────────

    def _merge_hello_presence(self, hello: HelloMsg) -> None:
        """HELLO carries the other side's presence snapshot. Don't
        overwrite their LWW entry though — we'll get the
        authoritative one via STATE_SYNC. But do populate enough so
        the Activity tile renders immediately."""
        key = f"presence:{hello.machine_id}"
        if self.lww.get(key) is None:
            # Install as a low-ts placeholder so their own newer
            # entry in STATE_SYNC takes over.
            self.lww.apply_remote(key, LWWEntry(
                ts_clock=0,
                ts_machine=hello.machine_id,
                value={
                    "hostname": hello.hostname,
                    "os": hello.os,
                    "platform_info": hello.platform_info,
                    "monitors": [
                        {"monitor_id": m.get("monitor_id"),
                         "width": m.get("width", 0),
                         "height": m.get("height", 0)}
                        for m in hello.monitors
                    ],
                },
            ))

    # ── Cursor handoff ───────────────────────────────────────────

    async def _accept_cursor(self, msg: CursorReleaseMsg) -> None:
        # If the sender told us WHICH of our monitors the entry
        # coords apply to, translate them into our local coord space
        # using that monitor's origin. Without this, a multi-monitor
        # receiver warps the cursor to an arbitrary position on its
        # primary monitor, which isn't where the edge-crossing
        # intended to land.
        ex = int(msg.entry_local_x)
        ey = int(msg.entry_local_y)
        target_mon = str(getattr(msg, "target_monitor_id", "") or "")
        if target_mon:
            for m in self.layout.get_monitors_for_machine(self.machine_id):
                if str(m.monitor_id) == target_mon:
                    mon_local = self.layout.global_to_local(
                        self.machine_id, m.global_x, m.global_y)
                    if mon_local is not None:
                        ex = mon_local[0] + max(0, ex)
                        ey = mon_local[1] + max(0, ey)
                    break
            # Anti-bounce: if we just got warped onto one of our own
            # monitors that is itself a swap-routed sink, the next
            # _poll_active tick would otherwise read it as "cursor on
            # a swap sink → hand off" and bounce straight back to the
            # sender. Remember the monitor so the interior-handoff
            # path skips it until the cursor moves elsewhere.
            self._last_swap_handoff_monitor = target_mon
        self._enter_active(ex, ey)
        self._lww_write("active", self.machine_id)
        if self._backend_main and ex >= 0 and ey >= 0:
            try:
                self._backend_main.set_cursor_pos(ex, ey)
                self._backend_main.flush()
            except Exception:
                log.exception("warp cursor on handoff failed")


    def _enter_active(self, entry_x: int, entry_y: int) -> None:
        self.mode = Mode.ACTIVE
        self._edge_hit_sent = True
        if self._backend_main:
            # Warp FIRST so show_cursor doesn't reveal the cursor at
            # wherever the OS last had it (often a different monitor
            # on a multi-display peer — that's the "cursor briefly
            # appears on the wrong display" flicker on handoff).
            if entry_x >= 0 and entry_y >= 0:
                try:
                    self._backend_main.set_cursor_pos(entry_x, entry_y)
                    self._backend_main.flush()
                except Exception:
                    log.exception("warp on enter_active failed")
            try:
                self._backend_main.show_cursor()
            except Exception:
                pass
        self._apply_local_input_state()

    def _enter_forwarding(self) -> None:
        self.mode = Mode.FORWARDING
        self._edge_hit_sent = False
        if self._backend_main and not self.is_muted:
            try:
                self._backend_main.hide_cursor()
            except Exception:
                pass
        self._apply_local_input_state()

    def _apply_local_input_state(self) -> None:
        """Local mouse + keyboard on a muted PC always drive only
        the muted PC itself — never blocked, never captured. Since
        the shared cursor never targets muted peers (see
        _poll_active's skip), the old "muted + ACTIVE" conflict
        case doesn't exist here.

        Non-muted PC: capture keys while FORWARDING so local apps
        don't also receive keys we're forwarding to the active peer.
        """
        if not self._backend_main:
            return
        try:
            self._backend_main.stop_pointer_block()
        except Exception:
            pass
        if self._backend_input:
            capture = (not self.is_muted
                       and self.mode == Mode.FORWARDING)
            try:
                if capture:
                    self._backend_input.start_key_capture(self._on_key_event)
                    # Scroll-wheel forwarding lives on the same hook
                    # window as key capture, so we (re-)start it here.
                    # Backends that don't implement it fall through
                    # silently via the base no-op.
                    self._backend_input.start_scroll_capture(
                        self._on_scroll_event)
                else:
                    self._backend_input.stop_key_capture()
                    self._backend_input.stop_scroll_capture()
            except Exception:
                pass

    # ── Input loop (capture + forward) ───────────────────────────

    def _input_loop(self) -> None:
        try:
            self._backend_input = create_backend()
            self._backend_input.open()
        except Exception:
            log.exception("input backend failed to open")
            return
        poll = 1.0 / POLL_HZ
        is_grabbed = False
        while self._running:
            want_grab = (self.mode == Mode.FORWARDING and not self.is_muted)
            try:
                if want_grab and not is_grabbed:
                    self._backend_input.grab_input()
                    sw = self._backend_input.screen_width
                    sh = self._backend_input.screen_height
                    self._warp_cx, self._warp_cy = sw // 2, sh // 2
                    self._backend_input.set_cursor_pos(
                        self._warp_cx, self._warp_cy,
                    )
                    is_grabbed = True
                elif not want_grab and is_grabbed:
                    if self._backend_input.is_grabbed:
                        self._backend_input.ungrab_input()
                    is_grabbed = False
            except Exception:
                log.exception("grab transition failed")
            try:
                if self.mode == Mode.ACTIVE:
                    self._poll_active()
                elif self.mode == Mode.FORWARDING:
                    self._poll_forwarding()
            except Exception:
                log.exception("input loop error")
            time.sleep(poll)
        with self._lock:
            if self._backend_input:
                try:
                    if self._backend_input.is_grabbed:
                        self._backend_input.ungrab_input()
                except Exception:
                    pass
                try:
                    self._backend_input.close()
                except Exception:
                    pass

    def _poll_active(self) -> None:
        with self._lock:
            if not self._backend_input:
                return
            cx, cy = self._backend_input.get_cursor_pos()
            btn_mask = self._backend_input.get_button_mask()
        # Swap-sink passthrough: if the cursor is sitting on one of
        # our own monitors whose overlay shows a peer's content, any
        # click the user makes should register on the source PC, not
        # locally (there's nothing meaningful under the overlay on
        # our desktop). Forward button transitions while the cursor
        # is over a sink; let the local click pass through to our
        # now-empty desktop. The overlay's invisible-cursor attribute
        # keeps our local arrow off the user's view of the streamed
        # content; the peer's own cursor, set by set_cursor_pos in
        # _inject_swap_input, is what shows up via the return stream.
        on_sink = self._is_on_local_swap_sink(cx, cy)
        self._sync_swap_sink_cursor_visibility(on_sink)
        self._poll_swap_sink_clicks(cx, cy, btn_mask)
        mine = self.layout.get_monitors_for_machine(self.machine_id)
        if not mine:
            return
        gl = self.layout.local_to_global(self.machine_id, cx, cy)
        if gl is None:
            return
        gx, gy = gl
        current = None
        for m in mine:
            if m.contains(gx, gy):
                current = m
                break
        if current is None:
            # Cursor wandered a pixel past an edge — snap to the
            # nearest of our own monitors so the edge-hit logic still
            # fires a handoff. Without this, crossings silently drop
            # when the cursor lands exactly on the seam between
            # machines. Ported back from pre-mesh client.
            min_dist = float("inf")
            for m in mine:
                clx = max(m.global_x, min(gx, m.right - 1))
                cly = max(m.global_y, min(gy, m.bottom - 1))
                d = abs(clx - gx) + abs(cly - gy)
                if d < min_dist:
                    min_dist = d
                    current = m
            if current is None:
                return

        edge = None
        margin = max(0, int(self.ws_edge_margin))
        if gx <= current.global_x + margin:
            edge = "left"
        elif gx >= current.right - 1 - margin:
            edge = "right"
        elif gy <= current.global_y + margin:
            edge = "top"
        elif gy >= current.bottom - 1 - margin:
            edge = "bottom"
        if edge is None:
            self._edge_hit_sent = False
            return
        if self._edge_hit_sent:
            return

        # Workspace "hold Ctrl+Shift to cross" gate. Checks modifier
        # state right at edge-hit time — the cursor only leaves this
        # PC when the user is explicitly asking for it. Backends that
        # haven't implemented get_modifier_mask default to 0, which
        # makes the cursor refuse to cross on that platform when the
        # gate is on (fail-safe — better than leaking the cursor).
        if self.ws_require_modifier and self._backend_input is not None:
            with self._lock:
                try:
                    mods = self._backend_input.get_modifier_mask()
                except Exception:
                    mods = 0
            # Bit 0 = Shift, bit 1 = Ctrl. Both required.
            if (mods & 1) == 0 or (mods & 2) == 0:
                return

        result = self.layout.find_crossing_target(edge, gx, gy)
        if not result:
            return
        target_mon, entry_gx, entry_gy = result
        if target_mon.machine_id == self.machine_id:
            # Local target — let the OS flow the cursor there. If
            # the target is a swap-sink (showing a peer's content),
            # we do nothing here: the cursor simply sits on the
            # sink display, the overlay's invisible-cursor attribute
            # keeps our local arrow out of view, and _forward_swap_input
            # picks up any click/scroll/key that happens while the
            # cursor sits on the sink.
            return
        # Remote target. If that target is itself a swap-sink and
        # the routed content comes from one of OUR monitors, the
        # user's visual flow is: cursor crosses onto peer's physical
        # display which is showing our content — the real apps for
        # that content live on us, so warp the cursor LOCALLY to
        # our source display instead of handing it to the peer.
        target_key = f"{target_mon.machine_id}:{target_mon.monitor_id}"
        target_source = self.active_routes.get(
            target_key, target_key)
        if target_source != target_key:
            peer_src_mid, _, peer_src_mon_id = (
                target_source.partition(":"))
            if peer_src_mid == self.machine_id:
                our_mon = None
                for m in self.layout.get_monitors_for_machine(
                        self.machine_id):
                    if str(m.monitor_id) == peer_src_mon_id:
                        our_mon = m
                        break
                if our_mon is not None:
                    sink_lx = max(
                        0, int(entry_gx - target_mon.global_x))
                    sink_ly = max(
                        0, int(entry_gy - target_mon.global_y))
                    sw = max(1, int(our_mon.width))
                    sh = max(1, int(our_mon.height))
                    tw = max(1, int(target_mon.width))
                    th = max(1, int(target_mon.height))
                    inset = 8
                    if edge == "right":
                        our_lx = inset
                        our_ly = int(sink_ly * sh / th)
                    elif edge == "left":
                        our_lx = sw - 1 - inset
                        our_ly = int(sink_ly * sh / th)
                    elif edge == "bottom":
                        our_ly = inset
                        our_lx = int(sink_lx * sw / tw)
                    elif edge == "top":
                        our_ly = sh - 1 - inset
                        our_lx = int(sink_lx * sw / tw)
                    else:
                        our_lx = int(sink_lx * sw / tw)
                        our_ly = int(sink_ly * sh / th)
                    our_lx = max(inset, min(our_lx, sw - 1 - inset))
                    our_ly = max(inset, min(our_ly, sh - 1 - inset))
                    mon_local = self.layout.global_to_local(
                        self.machine_id, our_mon.global_x,
                        our_mon.global_y)
                    if mon_local is not None:
                        warp_lx = mon_local[0] + our_lx
                        warp_ly = mon_local[1] + our_ly
                        with self._lock:
                            if self._backend_input is not None:
                                try:
                                    self._backend_input.set_cursor_pos(
                                        warp_lx, warp_ly)
                                    self._warp_cx = warp_lx
                                    self._warp_cy = warp_ly
                                except Exception:
                                    log.exception(
                                        "local-swap warp failed")
                        self._edge_hit_sent = True
                        self._last_swap_handoff_monitor = str(
                            our_mon.monitor_id)
                        log.info(
                            "local-swap warp: cursor to our %s "
                            "(peer %s was showing our content)",
                            peer_src_mon_id, target_mon.monitor_id)
                        return
        self._edge_hit_sent = True
        local = self.layout.global_to_local(
            target_mon.machine_id, entry_gx, entry_gy,
        )
        if local is None:
            local = (0, 0)
        self._send_cursor_release(
            target_mon.machine_id, local[0], local[1],
            target_monitor_id=str(target_mon.monitor_id),
        )

    def _poll_forwarding(self) -> None:
        if self.is_muted:
            return
        with self._lock:
            if not self._backend_input:
                return
            cx, cy = self._backend_input.get_cursor_pos()
            btn_mask = self._backend_input.get_button_mask()
        dx = cx - self._warp_cx
        dy = cy - self._warp_cy
        if dx or dy:
            self._send_to_active(MsgType.MOUSE_MOVE_REL,
                                 MouseMoveRelMsg(dx=dx, dy=dy))
            with self._lock:
                if self._backend_input:
                    try:
                        self._backend_input.set_cursor_pos(
                            self._warp_cx, self._warp_cy,
                        )
                    except Exception:
                        pass
        for btn, bit in ((1, 1), (2, 2), (3, 4)):
            was = bool(self._prev_btn_mask & bit)
            now = bool(btn_mask & bit)
            if was != now:
                active = self.lww.get("active")
                log.info("forwarding click btn=%d pressed=%s → %s",
                         btn, now, active)
                self._send_to_active(MsgType.MOUSE_BUTTON,
                                     MouseButtonMsg(button=btn, pressed=now))
        self._prev_btn_mask = btn_mask

    # USB HID usage IDs for the OS-hotkey block list. These map to
    # Win+L, Ctrl+Alt+Del-style system combos that we never want to
    # forward when the user has asked to keep them local. Forwarding
    # them also rarely makes sense — Win+L locks the REMOTE PC, not
    # the local one the user's eyes are on.
    _BLOCKED_HID_KEYS = frozenset({
        0xE3,  # Left GUI / Win / Cmd
        0xE7,  # Right GUI
        0x48,  # Pause/Break
        0x54,  # SysRq / Print Screen on some keyboards
    })

    def _on_key_event(self, scancode: int, pressed: bool) -> None:
        if self.is_muted and self.mode != Mode.ACTIVE:
            return
        if self.ws_block_os_hotkeys and scancode in self._BLOCKED_HID_KEYS:
            return
        if self.mode != Mode.FORWARDING:
            return
        self._send_to_active(MsgType.KEY_EVENT,
                             KeyEventMsg(keycode=scancode, pressed=pressed))

    def _on_scroll_event(self, dx: int, dy: int) -> None:
        """Backend-driven scroll callback. Wheel events happen while
        the user is driving the remote PC in FORWARDING mode; we
        relay them to the active peer using the same gate as other
        input messages (workspace membership + mute + active-in-
        workspace). On a muted PC we let scrolls drive the local
        machine directly — _on_scroll_event only fires at all when
        we're forwarding, so no local injection needed here."""
        if self.mode != Mode.FORWARDING:
            return
        if self.is_muted:
            return
        self._send_to_active(MsgType.MOUSE_SCROLL,
                             MouseScrollMsg(dx=int(dx), dy=int(dy)))

    # ── Send helpers ─────────────────────────────────────────────

    def _send_to_active(self, msg_type: MsgType, payload) -> None:
        active = self.lww.get("active")
        if not active or active == self.machine_id:
            return
        if active not in self.allowed_peer_ids:
            return  # owner of the cursor is outside our workspace
        link = self.links.get(active)
        if link is None or self._loop is None:
            return
        asyncio.run_coroutine_threadsafe(
            link.send(msg_type, payload), self._loop,
        )

    def _send_cursor_release(self, target_mid: str,
                             entry_x: int, entry_y: int,
                             target_monitor_id: str = "") -> None:
        if target_mid not in self.allowed_peer_ids:
            return  # cursor handoff stays inside the workspace
        link = self.links.get(target_mid)
        if link is None or self._loop is None:
            return
        # Flip our own mode optimistically so the grab engages.
        self._enter_forwarding()
        # Send CURSOR_RELEASE BEFORE gossiping the active-peer switch.
        # The target uses the CURSOR_RELEASE entry-coords to warp the
        # cursor into place; if SET_STATE(active=target) arrives first
        # it triggers _enter_active(-1,-1) which just reveals the
        # target's local cursor at whatever pre-warp position the OS
        # had (frequently on the wrong monitor on a multi-display
        # peer — that's the brief "cursor on wrong display" flicker).
        asyncio.run_coroutine_threadsafe(
            link.send(MsgType.CURSOR_RELEASE, CursorReleaseMsg(
                from_machine=self.machine_id,
                entry_local_x=entry_x,
                entry_local_y=entry_y,
                target_monitor_id=target_monitor_id,
            )),
            self._loop,
        )
        self._lww_write("active", target_mid)

    # ── Mouse / key injection (receiving from forwarders) ────────

    async def _inject_mouse(self, msg_type: MsgType, payload) -> None:
        if self.mode != Mode.ACTIVE:
            if msg_type == MsgType.MOUSE_BUTTON:
                log.info("click DROPPED (mode=%s not ACTIVE): btn=%s",
                         self.mode.value,
                         getattr(payload, "button", None))
            return
        if not self._backend_main:
            return
        try:
            if msg_type == MsgType.MOUSE_MOVE_REL:
                self._backend_main.inject_mouse_move_rel(
                    int(getattr(payload, "dx", 0)),
                    int(getattr(payload, "dy", 0)),
                )
            elif msg_type == MsgType.MOUSE_BUTTON:
                log.info("injecting click btn=%d pressed=%s",
                         int(getattr(payload, "button", 0)),
                         bool(getattr(payload, "pressed", False)))
                self._backend_main.inject_mouse_button(
                    int(getattr(payload, "button", 0)),
                    bool(getattr(payload, "pressed", False)),
                )
            elif msg_type == MsgType.MOUSE_SCROLL:
                self._backend_main.inject_mouse_scroll(
                    int(getattr(payload, "dx", 0)),
                    int(getattr(payload, "dy", 0)),
                )
            self._backend_main.flush()
        except Exception:
            log.exception("mouse inject failed")


    def _is_on_local_swap_sink(
            self, cx: int, cy: int) -> bool:
        """True when the cursor's local coords fall inside one of
        our monitors whose workspace route maps to a peer source."""
        gl = self.layout.local_to_global(self.machine_id, cx, cy)
        if gl is None:
            return False
        gx, gy = gl
        for m in self.layout.get_monitors_for_machine(self.machine_id):
            if not m.contains(gx, gy):
                continue
            sink_key = f"{self.machine_id}:{m.monitor_id}"
            src = self.active_routes.get(sink_key, sink_key)
            if src == sink_key:
                return False
            src_mid, _, _ = src.partition(":")
            return bool(src_mid and src_mid != self.machine_id)
        return False

    def _sync_swap_sink_cursor_visibility(
            self, on_sink: bool) -> None:
        """Hide the local OS cursor while it sits over a swap-sink
        overlay, restore it otherwise. Keeps the arrow off the
        user's view of streamed content without touching the
        overlay window's style bits."""
        prev = getattr(self, "_swap_sink_cursor_hidden", False)
        if on_sink == prev:
            return
        self._swap_sink_cursor_hidden = on_sink
        if not self._backend_main:
            return
        try:
            if on_sink:
                self._backend_main.hide_cursor()
            else:
                self._backend_main.show_cursor()
        except Exception:
            log.exception("swap-sink cursor visibility toggle failed")

    def _poll_swap_sink_clicks(
            self, cx: int, cy: int, btn_mask: int) -> None:
        """When the cursor sits over one of OUR swap-sink monitors,
        forward button transitions to the source PC. The click also
        fires locally (on the empty desktop under the overlay), but
        that's harmless — the user cares about the streamed content,
        not the desktop we hid under the overlay."""
        prev = getattr(self, "_prev_swap_btn_mask", 0)
        if btn_mask == prev:
            self._prev_swap_btn_mask = btn_mask
            return
        self._prev_swap_btn_mask = btn_mask
        gl = self.layout.local_to_global(
            self.machine_id, cx, cy)
        if gl is None:
            return
        gx, gy = gl
        current = None
        for m in self.layout.get_monitors_for_machine(self.machine_id):
            if m.contains(gx, gy):
                current = m
                break
        if current is None:
            return
        sink_key = f"{self.machine_id}:{current.monitor_id}"
        source_key = self.active_routes.get(sink_key, sink_key)
        if source_key == sink_key:
            return
        src_mid, _, src_mon_id = source_key.partition(":")
        if not src_mid or src_mid == self.machine_id:
            return
        src_mon = None
        for m in self.layout.get_monitors_for_machine(src_mid):
            if str(m.monitor_id) == src_mon_id:
                src_mon = m
                break
        if src_mon is None:
            return
        sw = max(1, int(src_mon.width))
        sh = max(1, int(src_mon.height))
        tw = max(1, int(current.width))
        th = max(1, int(current.height))
        src_lx = int((gx - current.global_x) * sw / tw)
        src_ly = int((gy - current.global_y) * sh / th)
        src_lx = max(0, min(src_lx, sw - 1))
        src_ly = max(0, min(src_ly, sh - 1))
        for btn, bit in ((1, 1), (2, 2), (3, 4)):
            was = bool(prev & bit)
            now = bool(btn_mask & bit)
            if was == now:
                continue
            link = self.links.get(src_mid)
            if link is None or self._loop is None:
                continue
            log.info(
                "swap-click forward: btn=%d pressed=%s on our %s "
                "(%d,%d) → %s:%s(%d,%d)",
                btn, now, current.monitor_id,
                gx - current.global_x, gy - current.global_y,
                src_mid, src_mon_id, src_lx, src_ly,
            )
            asyncio.run_coroutine_threadsafe(
                link.send(
                    MsgType.SWAP_INPUT,
                    SwapInputMsg(
                        from_machine=self.machine_id,
                        target_monitor_id=str(src_mon_id),
                        local_x=src_lx, local_y=src_ly,
                        button=btn, pressed=now,
                    )),
                self._loop)

    async def _inject_swap_input(self, msg: SwapInputMsg) -> None:
        """Apply a SWAP_INPUT event from a peer on our source
        display. Warp our cursor to the given local coords (so the
        event fires on the right target AND our captured frame
        shows the cursor there for the stream going back), inject
        the click / scroll / key, then restore the cursor so
        _poll_forwarding doesn't send phantom relative motion if
        we happen to be in FORWARDING mode."""
        if not self._backend_main:
            return
        target_mon_id = str(msg.target_monitor_id or "")
        if not target_mon_id:
            return
        target_mon = None
        for m in self.layout.get_monitors_for_machine(self.machine_id):
            if str(m.monitor_id) == target_mon_id:
                target_mon = m
                break
        if target_mon is None:
            return
        mon_local = self.layout.global_to_local(
            self.machine_id, target_mon.global_x,
            target_mon.global_y)
        if mon_local is None:
            return
        lx = mon_local[0] + max(0, int(msg.local_x))
        ly = mon_local[1] + max(0, int(msg.local_y))
        with self._lock:
            if self._backend_main is None:
                return
            try:
                if self._backend_input is not None:
                    try:
                        orig_x, orig_y = (
                            self._backend_input.get_cursor_pos())
                    except Exception:
                        orig_x, orig_y = self._warp_cx, self._warp_cy
                else:
                    orig_x, orig_y = self._warp_cx, self._warp_cy
                self._backend_main.set_cursor_pos(lx, ly)
                if msg.button:
                    log.info(
                        "swap-input inject btn=%d pressed=%s on %s "
                        "at (%d,%d)",
                        msg.button, msg.pressed, target_mon_id,
                        lx, ly)
                    self._backend_main.inject_mouse_button(
                        int(msg.button), bool(msg.pressed))
                elif msg.scroll_dx or msg.scroll_dy:
                    self._backend_main.inject_mouse_scroll(
                        int(msg.scroll_dx), int(msg.scroll_dy))
                elif msg.keycode:
                    self._backend_main.inject_key(
                        int(msg.keycode), bool(msg.key_pressed))
                self._backend_main.flush()
                if self.mode == Mode.FORWARDING:
                    # In FORWARDING the cursor is parked at
                    # _warp_cx/_warp_cy; restore so the input
                    # thread's next poll doesn't interpret the warp
                    # as real motion.
                    self._backend_main.set_cursor_pos(
                        int(orig_x), int(orig_y))
                    self._backend_main.flush()
            except Exception:
                log.exception("swap-input inject failed")

    async def _inject_key(self, payload) -> None:
        if self.mode != Mode.ACTIVE:
            return
        if not self._backend_main:
            return
        try:
            self._backend_main.inject_key(
                int(getattr(payload, "keycode", 0)),
                bool(getattr(payload, "pressed", False)),
            )
            self._backend_main.flush()
        except Exception:
            log.exception("key inject failed")

    # ── Clipboard ────────────────────────────────────────────────

    async def _clipboard_poll_loop(self) -> None:
        while self._running:
            try:
                await asyncio.sleep(CLIPBOARD_POLL_INTERVAL)
            except asyncio.CancelledError:
                break
            if not self._backend_main:
                continue
            if not self.clipboard_sync_enabled:
                try:
                    self._last_clipboard = self._backend_main.get_clipboard()
                except Exception:
                    pass
                continue
            try:
                current = self._backend_main.get_clipboard()
            except Exception:
                continue
            if current == self._last_clipboard:
                continue
            self._last_clipboard = current
            if current == self._pending_echo:
                self._pending_echo = None
                continue
            self._pending_echo = None
            # Respect the workspace's max-text-size gate. ws_cb_max_bytes
            # is 0 for "no limit"; a large clipboard entry (e.g. an
            # accidental giant paste) silently stays local.
            limit = int(self.ws_cb_max_bytes or 0)
            if limit > 0 and len(current.encode("utf-8", errors="replace")) > limit:
                log.info("clipboard entry %d bytes exceeds workspace "
                         "limit %d; not syncing",
                         len(current), limit)
                continue
            msg = ClipboardUpdateMsg(content=current,
                                     source_machine=self.machine_id)
            for link in list(self.links.values()):
                other_mid = link.machine_id
                # Only sync clipboard within the active workspace.
                # Outside the workspace, peers shouldn't see our
                # clipboard even if their link is open.
                if other_mid not in self.allowed_peer_ids:
                    continue
                if self.lww.get(f"cb_off:{other_mid}", False):
                    continue
                asyncio.create_task(link.send(MsgType.CLIPBOARD_UPDATE, msg))

    def _handle_clipboard_incoming(self, payload) -> None:
        if not self._backend_main or not self.clipboard_sync_enabled:
            return
        content = getattr(payload, "content", "")
        if content == self._last_clipboard:
            return
        self._pending_echo = content
        self._last_clipboard = content
        try:
            self._backend_main.set_clipboard(content)
        except Exception:
            log.exception("set_clipboard failed")

    # ── Identify ─────────────────────────────────────────────────

    def trigger_identify(self) -> None:
        """Entry point for the shell's Identify button. Fires the
        overlay locally and asks every peer to do the same."""
        # Number every monitor in the global layout, grouped per
        # machine, so overlays on different PCs stay consistent.
        number_map = self._number_all_monitors()
        local_defs = [
            {"monitor_id": m["monitor_id"],
             "number": number_map.get((self.machine_id, m["monitor_id"]), 0),
             "label": self.machine_id}
            for m in self.my_monitors
        ]
        self._show_identify(IdentifyMsg(displays=local_defs, duration=3))
        for link in list(self.links.values()):
            mid = link.machine_id
            presence = self.lww.get(f"presence:{mid}") or {}
            monitors = presence.get("monitors") or []
            their_defs = [
                {"monitor_id": m.get("monitor_id"),
                 "number": number_map.get((mid, m.get("monitor_id")), 0),
                 "label": mid}
                for m in monitors
            ]
            if their_defs and self._loop:
                asyncio.run_coroutine_threadsafe(
                    link.send(MsgType.IDENTIFY,
                              IdentifyMsg(displays=their_defs, duration=3)),
                    self._loop,
                )

    def _number_all_monitors(self) -> dict[tuple[str, str], int]:
        positions = self.lww.get("layout") or []
        by_machine: dict[str, list] = {}
        for p in positions:
            if not isinstance(p, dict):
                continue
            by_machine.setdefault(p["machine_id"], []).append(p)
        # Machines with presence but no layout entries (fresh peer)
        # still get numbered so Identify works before the first
        # Apply Layout click.
        for k in self.lww.iter_keys():
            if not k.startswith("presence:"):
                continue
            mid = k.split(":", 1)[1]
            by_machine.setdefault(mid, [])
            info = self.lww.get(k) or {}
            for m in info.get("monitors") or []:
                if not any(p.get("monitor_id") == m.get("monitor_id")
                           for p in by_machine[mid]):
                    by_machine[mid].append({
                        "machine_id": mid,
                        "monitor_id": m.get("monitor_id"),
                        "global_x": 0, "global_y": 0,
                    })
        # Stable ordering: machines alphabetically, monitors by
        # monitor_id within each machine. Doesn't depend on the
        # Layout canvas's current positions, so rearranging and
        # clicking Apply doesn't renumber monitors any more — the
        # user's report was that identify overlays on a given
        # monitor kept changing after each apply.
        machine_order = sorted(by_machine.keys())
        number_map: dict[tuple[str, str], int] = {}
        n = 1
        for mid in machine_order:
            for m in sorted(by_machine[mid],
                            key=lambda m: str(m.get("monitor_id"))):
                number_map[(mid, m.get("monitor_id"))] = n
                n += 1
        return number_map

    def _show_identify(self, msg) -> None:
        if not self._backend_main:
            return
        display_defs = getattr(msg, "displays", []) or []
        duration = int(getattr(msg, "duration", 3))
        try:
            screens = self._backend_main.query_monitors()
        except Exception:
            log.exception("query_monitors in identify failed")
            return
        screen_map = {s.id: s for s in screens}
        overlays = []
        for d in display_defs:
            mon_id = d.get("monitor_id")
            screen = screen_map.get(mon_id)
            if screen is None and screens:
                idx = display_defs.index(d)
                if idx < len(screens):
                    screen = screens[idx]
            if screen:
                overlays.append({
                    "x": screen.x, "y": screen.y,
                    "width": screen.width, "height": screen.height,
                    "number": d.get("number", 0),
                    "label": d.get("label", ""),
                })
        if overlays and self.identify_sink is not None:
            try:
                self.identify_sink(overlays, duration)
            except Exception:
                log.exception("identify_sink failed")

    # ── Shell-facing actions ─────────────────────────────────────

    def apply_layout(self, positions: list[dict]) -> None:
        """Commit a new monitor arrangement across every peer."""
        # Normalise keys so stale entries don't leak through.
        clean = []
        for p in positions:
            if not isinstance(p, dict):
                continue
            try:
                clean.append({
                    "machine_id": p["machine_id"],
                    "monitor_id": p["monitor_id"],
                    "global_x": int(p["global_x"]),
                    "global_y": int(p["global_y"]),
                })
            except (KeyError, ValueError, TypeError):
                continue
        self._lww_write("layout", clean)

    def set_input_muted(self, machine_id: str, muted: bool) -> None:
        self._lww_write(f"muted:{machine_id}", bool(muted))

    def set_clipboard_sync(self, machine_id: str, enabled: bool) -> None:
        self._lww_write(f"cb_off:{machine_id}", not bool(enabled))

    def set_allowed_peers(self, peer_ids) -> None:
        """Restrict user-visible sharing (cursor handoff, input
        injection, clipboard) to this set of machine_ids. Called by
        the shell whenever the active workspace or its membership
        changes. Always includes self — shell can pass an empty set
        to mean "no workspace active" and we'll normalise it."""
        new_set = {self.machine_id}
        for mid in (peer_ids or ()):
            if mid:
                new_set.add(mid)
        if new_set == self.allowed_peer_ids:
            return
        self.allowed_peer_ids = new_set
        log.info("allowed peers now: %s", sorted(new_set))
        # Layout + cursor routing need to re-derive from the new
        # member list. Rebuild now so the next edge-hit or cursor
        # release sees a layout containing only workspace members.
        try:
            self._rebuild_global_layout()
        except Exception:
            log.exception("rebuild layout after allowed_peers change failed")
        # Re-evaluate cursor ownership with the same gate as
        # _apply_state_side_effect("active") — leaving a workspace
        # where someone else owned the cursor should snap us back to
        # ACTIVE mode so the user's cursor reappears immediately.
        try:
            active = self.lww.get("active")
            should_be_active = (
                active == self.machine_id
                or not active
                or active not in self.allowed_peer_ids
            )
            if should_be_active and self.mode != Mode.ACTIVE:
                self._enter_active(-1, -1)
            elif not should_be_active and self.mode != Mode.FORWARDING:
                self._enter_forwarding()
        except Exception:
            log.exception("mode re-eval after allowed_peers change failed")

    def set_workspace_settings(self, settings: Optional[dict]) -> None:
        """Push the active workspace's behaviour knobs into the peer:
        edge margin, modifier gate, OS-hotkey block, clipboard max
        size. Called by the shell whenever the active workspace
        changes or its settings are edited. settings=None resets to
        defaults (no active workspace → conservative defaults)."""
        settings = settings or {}
        try:
            self.ws_edge_margin = int(
                settings.get("edge_margin", EDGE_MARGIN))
        except (TypeError, ValueError):
            self.ws_edge_margin = EDGE_MARGIN
        self.ws_require_modifier = bool(
            settings.get("require_modifier", False))
        self.ws_block_os_hotkeys = bool(
            settings.get("block_os_hotkeys", False))

        # Translate the dropdown string to a byte count. "Unlimited"
        # (or anything we can't parse) becomes 0 = no cap.
        size_str = str(settings.get("cb_max_size") or "").strip().lower()
        if size_str.endswith("kb"):
            try:
                self.ws_cb_max_bytes = int(
                    float(size_str[:-2]) * 1024)
            except ValueError:
                self.ws_cb_max_bytes = 0
        elif size_str.endswith("mb"):
            try:
                self.ws_cb_max_bytes = int(
                    float(size_str[:-2]) * 1024 * 1024)
            except ValueError:
                self.ws_cb_max_bytes = 0
        else:
            self.ws_cb_max_bytes = 0
        log.info("workspace settings: edge_margin=%d require_modifier=%s "
                 "block_os_hotkeys=%s cb_max_bytes=%d",
                 self.ws_edge_margin, self.ws_require_modifier,
                 self.ws_block_os_hotkeys, self.ws_cb_max_bytes)

    # ── Heartbeat ────────────────────────────────────────────────

    async def _heartbeat_loop(self) -> None:
        seq = 0
        while self._running:
            try:
                await asyncio.sleep(HEARTBEAT_INTERVAL)
            except asyncio.CancelledError:
                break
            seq += 1
            now = time.monotonic()
            for mid, link in list(self.links.items()):
                age = now - (link.last_ack_monotonic or now)
                if age > HEARTBEAT_TIMEOUT:
                    log.info("Peer %s went silent (%.1fs) — dropping", mid, age)
                    try:
                        await link.conn.close()
                    except Exception:
                        pass
                    continue
                asyncio.create_task(link.send(
                    MsgType.HEARTBEAT, HeartbeatMsg(seq=seq),
                ))

    # ── Peer disconnect cleanup ──────────────────────────────────

    def _on_peer_gone(self, mid: str) -> None:
        """Remove a peer's presence + any cursor ownership tied to
        them. Each peer reaches the same conclusion independently."""
        removed_presence = False
        # Purge this peer's presence so its monitors vanish from the
        # layout on every remaining peer. We mutate the LWW entry by
        # writing a higher-ts empty value owned by us.
        if self.lww.get(f"presence:{mid}") is not None:
            self._lww_write(f"presence:{mid}", None)
            removed_presence = True
        # If the gone peer owned the cursor, claim it ourselves.
        if self.lww.get("active") == mid:
            self._lww_write("active", self.machine_id)
        if removed_presence:
            self._rebuild_global_layout()
        self._notify_state_changed()

    # ── Notifications ────────────────────────────────────────────

    def _notify_state_changed(self) -> None:
        if self.on_state_changed is None:
            return
        try:
            self.on_state_changed()
        except Exception:
            log.exception("on_state_changed callback raised")

    # ── Shell-facing views ───────────────────────────────────────

    def machines_snapshot(self) -> dict[str, dict]:
        """Return {machine_id: {hostname, os, platform_info,
        monitors, muted, clipboard_sync}} for every peer currently
        known via LWW presence (including self). Used by the shell's
        Activity tab."""
        out: dict[str, dict] = {}
        for key in self.lww.iter_keys():
            if not key.startswith("presence:"):
                continue
            mid = key.split(":", 1)[1]
            info = self.lww.get(key)
            if info is None:
                continue
            out[mid] = {
                "hostname": info.get("hostname", mid),
                "os": info.get("os", ""),
                "platform_info": info.get("platform_info", ""),
                "monitors": info.get("monitors") or [],
                "muted": bool(self.lww.get(f"muted:{mid}", False)),
                "clipboard_sync": not bool(self.lww.get(f"cb_off:{mid}",
                                                        False)),
            }
        return out

    def global_monitors(self) -> list[dict]:
        """List of every monitor in the mesh for the Layout canvas.
        When we're alone (no other peer has an active presence), the
        canvas reads empty — the shared cursor system isn't doing
        anything yet, so there's nothing to arrange."""
        # Count peers other than ourselves that still have a
        # non-tombstoned presence entry.
        other_alive = any(
            mid != self.machine_id and self.lww.get(f"presence:{mid}")
            for mid in (k.split(":", 1)[1]
                        for k in self.lww.iter_keys()
                        if k.startswith("presence:"))
        )
        if not other_alive:
            return []

        positions = self.lww.get("layout") or []
        position_map = {(p["machine_id"], p["monitor_id"]):
                        (p["global_x"], p["global_y"])
                        for p in positions if isinstance(p, dict)}
        out: list[dict] = []
        sorted_mids = sorted(k for k in self.lww.iter_keys()
                             if k.startswith("presence:"))
        cursor_x = 0
        for key in sorted_mids:
            mid = key.split(":", 1)[1]
            info = self.lww.get(key)
            if info is None:
                continue
            monitors = info.get("monitors") or []
            if not monitors:
                continue
            local_right = max(
                int(m.get("local_x", 0)) + int(m.get("width", 0))
                for m in monitors
            )
            for m in monitors:
                local_x = int(m.get("local_x", 0))
                local_y = int(m.get("local_y", 0))
                gx, gy = position_map.get(
                    (mid, m.get("monitor_id")),
                    (cursor_x + local_x, local_y),
                )
                out.append({
                    "machine_id": mid,
                    "monitor_id": m.get("monitor_id"),
                    "global_x": gx,
                    "global_y": gy,
                    "width": int(m.get("width", 0)),
                    "height": int(m.get("height", 0)),
                })
            cursor_x += local_right
        return out

    def active_machine(self) -> str:
        return self.lww.get("active") or ""

    def peer_ip(self, machine_id: str) -> Optional[str]:
        """IP address of a live peer, derived from its mesh TCP link.
        Used by the display-streaming sink to open its dedicated
        video-transport connection to the source peer. Returns None
        when we don't have a live link to that machine_id (the sink
        should then show a placeholder card until the peer is back)."""
        link = self.links.get(machine_id)
        if link is None:
            return None
        try:
            peername = link.conn.writer.get_extra_info("peername")
        except Exception:
            return None
        if not peername:
            return None
        # peername is (host, port) for TCP, possibly more elements on
        # IPv6. The first element is always the address.
        try:
            return str(peername[0])
        except (IndexError, TypeError):
            return None


def _fallback_origin(mid: str) -> int:
    """Deterministic sort index for placing a peer whose layout
    position hasn't been set yet. Just hash-based — keeps two peers
    from stacking at 0,0 before the user clicks Apply."""
    import zlib
    return zlib.crc32(mid.encode("utf-8")) % 8

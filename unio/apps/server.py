"""
Central server (layout manager + event router).

Responsibilities:
- Accept client registrations
- Build and distribute the global monitor layout
- Track which machine is currently active (owns the cursor)
- Route input events from the active machine to the correct target
- Handle edge-crossing handoffs
- Broadcast clipboard updates
"""

import asyncio
import logging
import socket
import time
from dataclasses import dataclass, field
from typing import Optional

from ..core.protocol import (
    MsgType, RegisterMsg, RegisterAckMsg, LayoutUpdateMsg,
    EdgeHitMsg, ActivateMsg, DeactivateMsg, ClipboardUpdateMsg,
    MouseMoveRelMsg, MouseButtonMsg, MouseScrollMsg, KeyEventMsg,
    HeartbeatMsg, IdentifyMsg, ApplyMonitorsMsg,
    InputSourceStateMsg,
)
from ..core.discovery import DiscoveryResponder
from ..core.layout import LayoutManager
from ..core.network import Connection, serve
from .webui import load_saved_layout, save_layout

log = logging.getLogger(__name__)


def _is_loopback_conn(conn: Connection) -> bool:
    """True when the peer address is 127.0.0.1 / ::1 — i.e. the
    host's own Client or shell connection. Remote clients come in
    over the real LAN address."""
    try:
        addr = conn.writer.get_extra_info("peername")
    except Exception:
        return False
    if not addr:
        return False
    host = addr[0] if isinstance(addr, tuple) else str(addr)
    return host in ("127.0.0.1", "::1", "localhost")


@dataclass
class ClientState:
    """Per-client state tracked by the server."""
    machine_id: str
    conn: Connection
    monitors: list = field(default_factory=list)
    os: str = ""
    platform_info: str = ""
    # Wall-clock of the last HEARTBEAT_ACK we saw from this client.
    # Monotonic so a jump in system time doesn't falsely time it out.
    # 0 means "never acked" — bootstrapped to time.monotonic() at
    # registration so a freshly-joined client doesn't look stale
    # before the first heartbeat cycle runs.
    last_ack_monotonic: float = 0.0
    # True when this client is connected over loopback (the host's
    # own local Client / shell). Used to distinguish "only host
    # remains" from "someone else is still here" on disconnect, so
    # a remote's ghost monitors clear the moment nobody remote
    # is left.
    is_local: bool = False


class Server:
    """UnIO central server."""

    def __init__(self, host: str = "0.0.0.0", port: int = 24800,
                 layout_offsets: Optional[dict] = None):
        self.host = host
        self.port = port
        self.layout = LayoutManager()
        self.layout_offsets = layout_offsets  # {machine_id: (gx, gy)}
        self.clients: dict[str, ClientState] = {}  # machine_id → state
        self.active_machine: Optional[str] = None
        self.input_source: Optional[str] = None
        self._server: Optional[asyncio.Server] = None
        self._heartbeat_task: Optional[asyncio.Task] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._saved_layout = load_saved_layout()  # from layout.json
        self._hostname = socket.gethostname() or "unio-server"
        self._discovery: Optional[DiscoveryResponder] = None

    async def start(self):
        """Bind the listener and start background tasks. Returns once accepting."""
        self._loop = asyncio.get_running_loop()
        self._server = await serve(self.host, self.port, self._handle_client)
        self._heartbeat_task = asyncio.create_task(self._heartbeat_loop())
        self._discovery = DiscoveryResponder(
            hostname=self._hostname, tcp_port=self.port,
        )
        try:
            await self._discovery.start()
        except OSError as e:
            log.warning("LAN discovery disabled: %s", e)
            self._discovery = None
        log.info("Server ready. Waiting for clients...")

    async def serve_forever(self):
        """Block until the server is stopped."""
        if self._server is None:
            raise RuntimeError("Server.start() must be called first")
        await self._server.serve_forever()

    async def stop(self):
        if self._heartbeat_task:
            self._heartbeat_task.cancel()
        if self._discovery:
            self._discovery.stop()
            self._discovery = None
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        for cs in self.clients.values():
            await cs.conn.close()

    # ── Client handler ───────────────────────────────────────────

    async def _handle_client(self, conn: Connection):
        """Handle a single client connection lifecycle."""
        client_state: Optional[ClientState] = None

        try:
            while not conn.closed:
                result = await conn.recv()
                if result is None:
                    break
                msg_type, payload = result
                await self._dispatch(conn, msg_type, payload, client_state)
                # Update client_state ref after registration
                if msg_type == MsgType.REGISTER and isinstance(payload, RegisterMsg):
                    client_state = self.clients.get(payload.machine_id)
        except Exception:
            log.exception("Error in client handler")
        finally:
            if client_state:
                machine_id = client_state.machine_id
                log.info("Client %s disconnected.", machine_id)
                self.clients.pop(machine_id, None)
                # Always drop the disconnected client's monitors.
                self.layout.remove_machine(machine_id)
                # With no remote clients left, the layout has nothing
                # to arrange — clear the host's own monitors too so
                # the Layout tab goes empty rather than showing just
                # this PC's screens by themselves.
                if not self._any_remote_real_client():
                    self.layout.clear()
                if self.active_machine == machine_id:
                    # Transfer active to first remaining real client
                    real_clients = {k: v for k, v in self.clients.items()
                                    if v.monitors}
                    if real_clients:
                        new_active = next(iter(real_clients))
                        await self._activate(new_active)
                    else:
                        self.active_machine = None
                await self._broadcast_layout()

    async def _dispatch(self, conn: Connection, msg_type: MsgType,
                        payload, client_state: Optional[ClientState]):
        """Route an incoming message to the appropriate handler."""
        if msg_type == MsgType.REGISTER:
            await self._handle_register(conn, payload)

        elif msg_type == MsgType.EDGE_HIT:
            await self._handle_edge_hit(payload)

        elif msg_type == MsgType.CLAIM_FOCUS:
            await self._handle_claim_focus(payload, client_state)

        elif msg_type == MsgType.SET_INPUT_SOURCE:
            await self._handle_set_input_source(payload)

        elif msg_type in (MsgType.MOUSE_MOVE_REL, MsgType.MOUSE_BUTTON,
                          MsgType.MOUSE_SCROLL, MsgType.KEY_EVENT):
            await self._forward_input(msg_type, payload)

        elif msg_type == MsgType.CLIPBOARD_UPDATE:
            await self._handle_clipboard(payload, client_state)

        elif msg_type in (MsgType.FILE_OFFER, MsgType.FILE_CHUNK,
                          MsgType.FILE_DONE):
            await self._forward_file(msg_type, payload)

        elif msg_type == MsgType.LAYOUT_APPLY:
            await self._handle_layout_apply(payload)

        elif msg_type == MsgType.REQUEST_IDENTIFY:
            await self._trigger_identify()

        elif msg_type == MsgType.HEARTBEAT_ACK:
            if client_state is not None:
                client_state.last_ack_monotonic = time.monotonic()

    # ── Registration ─────────────────────────────────────────────

    async def _handle_register(self, conn: Connection, msg: RegisterMsg):
        machine_id = msg.machine_id
        log.info("Registering machine '%s' with %d monitor(s).",
                 machine_id, len(msg.monitors))

        # Remove old registration if reconnecting
        if machine_id in self.clients:
            old = self.clients[machine_id]
            await old.conn.close()
            self.layout.remove_machine(machine_id)

        cs = ClientState(
            machine_id=machine_id, conn=conn, monitors=msg.monitors,
            os=getattr(msg, "os", "") or "",
            platform_info=getattr(msg, "platform_info", "") or "",
            last_ack_monotonic=time.monotonic(),
            is_local=_is_loopback_conn(conn),
        )
        self.clients[machine_id] = cs

        # Configurator clients have no monitors — skip layout work
        if not msg.monitors:
            await conn.send(MsgType.REGISTER_ACK, RegisterAckMsg(
            ok=True, server_hostname=self._hostname,
        ))
            await self._broadcast_layout()
            return

        # When the host was alone we wiped its own monitors out of the
        # layout (layout has nothing to arrange with one machine).
        # Re-populate any other connected clients now that a peer is
        # joining, so the Layout tab sees every machine in the room.
        present = {m.machine_id for m in self.layout.monitors}
        for other_mid, other_cs in self.clients.items():
            if other_mid == machine_id or not other_cs.monitors:
                continue
            if other_mid in present:
                continue
            self.layout.add_monitors(other_mid, other_cs.monitors,
                                     self.layout_offsets)

        # Add monitors to layout
        self.layout.add_monitors(machine_id, msg.monitors, self.layout_offsets)

        # Apply saved layout positions if available
        if self._saved_layout:
            for saved in self._saved_layout:
                if saved.get("machine_id") == machine_id:
                    for m in self.layout.monitors:
                        if (m.machine_id == machine_id and
                                m.monitor_id == saved.get("monitor_id")):
                            m.global_x = saved["global_x"]
                            m.global_y = saved["global_y"]
            # Recompute origins and adjacency after applying saved positions
            by_machine: dict[str, list] = {}
            for m in self.layout.monitors:
                by_machine.setdefault(m.machine_id, []).append(m)
            for mid, mons in by_machine.items():
                min_gx = min(m.global_x for m in mons)
                min_gy = min(m.global_y for m in mons)
                self.layout._machine_origin[mid] = (min_gx, min_gy)
            self.layout._rebuild_adjacency()

        # ACK
        await conn.send(MsgType.REGISTER_ACK, RegisterAckMsg(
            ok=True, server_hostname=self._hostname,
        ))

        # First client to register becomes the active machine by
        # default. Every newly-registered real client is then told
        # explicitly whether it's active or dormant — ACTIVATE for the
        # cursor owner, DEACTIVATE for everyone else. Without the
        # DEACTIVATE branch the other clients stayed in their default
        # ACTIVE mode with their local cursor still drawn, producing a
        # ghost pointer on every screen that wasn't holding focus.
        if self.active_machine is None:
            self.active_machine = machine_id

        if machine_id == self.active_machine:
            await conn.send(MsgType.ACTIVATE, ActivateMsg(
                entry_local_x=-1, entry_local_y=-1,  # -1 means "keep current"
            ))
        else:
            await conn.send(MsgType.DEACTIVATE, DeactivateMsg())

        # First monitor-bearing client also becomes the input source.
        if self.input_source is None:
            self.input_source = machine_id
        # Tell this newly-registered client whether it is the input source.
        await conn.send(
            MsgType.INPUT_SOURCE_STATE,
            InputSourceStateMsg(is_input_source=(machine_id == self.input_source)),
        )

        await self._broadcast_layout()

    # ── Layout broadcast ─────────────────────────────────────────

    async def _broadcast_layout(self):
        info = self.layout.get_layout_info()
        machines = {
            mid: {"os": cs.os, "platform_info": cs.platform_info}
            for mid, cs in self.clients.items()
        }
        msg = LayoutUpdateMsg(
            monitors=info,
            active_machine=self.active_machine or "",
            machines=machines,
            input_source=self.input_source or "",
        )
        results = await asyncio.gather(
            *(cs.conn.send(MsgType.LAYOUT_UPDATE, msg)
              for cs in self.clients.values()),
            return_exceptions=True,
        )
        for cs, res in zip(self.clients.values(), results):
            if isinstance(res, Exception):
                log.warning("Failed to send layout to %s: %s",
                            cs.machine_id, res)

    # ── Layout apply (from configurator) ─────────────────────────

    async def _handle_layout_apply(self, msg):
        """Apply new monitor positions pushed by a configurator client."""
        displays = msg.displays if hasattr(msg, "displays") else \
            (msg.get("displays", []) if isinstance(msg, dict) else [])
        if not displays:
            return

        for pos in displays:
            mid = pos["machine_id"]
            mon_id = pos["monitor_id"]
            gx = pos["global_x"]
            gy = pos["global_y"]
            for m in self.layout.monitors:
                if m.machine_id == mid and m.monitor_id == mon_id:
                    m.global_x = gx
                    m.global_y = gy
                    break

        by_machine: dict[str, list] = {}
        for m in self.layout.monitors:
            by_machine.setdefault(m.machine_id, []).append(m)
        for mid, mons in by_machine.items():
            min_gx = min(m.global_x for m in mons)
            min_gy = min(m.global_y for m in mons)
            self.layout._machine_origin[mid] = (min_gx, min_gy)
        self.layout._rebuild_adjacency()

        await self._broadcast_layout()
        save_layout(self.layout)

        # Push OS-level reconfiguration to each affected client in parallel.
        apply_targets = []
        for mid, mons in by_machine.items():
            cs = self.clients.get(mid)
            if cs is None or not cs.monitors:
                continue
            origin_gx = min(m.global_x for m in mons)
            origin_gy = min(m.global_y for m in mons)
            positions = {
                m.monitor_id: [m.global_x - origin_gx, m.global_y - origin_gy]
                for m in mons
            }
            apply_targets.append((cs, positions))

        results = await asyncio.gather(
            *(cs.conn.send(MsgType.APPLY_MONITORS,
                           ApplyMonitorsMsg(positions=pos))
              for cs, pos in apply_targets),
            return_exceptions=True,
        )
        for (cs, _), res in zip(apply_targets, results):
            if isinstance(res, Exception):
                log.warning("Failed to send APPLY_MONITORS to %s: %s",
                            cs.machine_id, res)

    # ── Edge crossing ────────────────────────────────────────────

    async def _handle_edge_hit(self, msg: EdgeHitMsg):
        log.debug("Edge hit from %s: edge=%s at (%d,%d)",
                  msg.machine_id, msg.edge, msg.global_x, msg.global_y)

        result = self.layout.find_crossing_target(
            msg.edge, msg.global_x, msg.global_y,
        )
        if result is None:
            log.debug("No target monitor found for edge crossing.")
            return

        target_monitor, entry_gx, entry_gy = result
        target_machine = target_monitor.machine_id

        if target_machine == msg.machine_id:
            log.debug("Edge crossing is within the same machine, ignoring.")
            return

        if target_machine not in self.clients:
            log.warning("Target machine %s not connected.", target_machine)
            return

        log.info("Handoff: %s → %s at global (%d,%d)",
                 msg.machine_id, target_machine, entry_gx, entry_gy)

        # Deactivate current
        old_cs = self.clients.get(msg.machine_id)
        if old_cs:
            await old_cs.conn.send(MsgType.DEACTIVATE, DeactivateMsg())

        # Activate target
        local = self.layout.global_to_local(target_machine, entry_gx, entry_gy)
        if local is None:
            local = (0, 0)
        entry_lx, entry_ly = local

        new_cs = self.clients[target_machine]
        await new_cs.conn.send(MsgType.ACTIVATE, ActivateMsg(
            entry_local_x=entry_lx,
            entry_local_y=entry_ly,
        ))

        self.active_machine = target_machine

    # ── Input forwarding ─────────────────────────────────────────

    async def _forward_input(self, msg_type: MsgType, payload):
        """Forward input event to the active (receiving) machine."""
        if self.active_machine is None:
            return
        cs = self.clients.get(self.active_machine)
        if cs is None:
            return
        try:
            await cs.conn.send(msg_type, payload)
        except Exception:
            log.warning("Failed to forward input to %s", self.active_machine)

    # ── File transfer routing ───────────────────────────────────

    async def _forward_file(self, msg_type: MsgType, payload):
        """Route file transfer messages to the target machine."""
        if isinstance(payload, dict):
            target = payload.get("target_machine")
        else:
            target = getattr(payload, "target_machine", None)
        if not target or target not in self.clients:
            log.warning("File transfer target '%s' not found.", target)
            return
        cs = self.clients[target]
        try:
            await cs.conn.send(msg_type, payload)
        except Exception:
            log.warning("Failed to forward file data to %s", target)

    # ── Clipboard ────────────────────────────────────────────────

    async def _handle_clipboard(self, msg: ClipboardUpdateMsg,
                                sender: Optional[ClientState]):
        """Broadcast clipboard update to all other clients."""
        sender_id = msg.source_machine if isinstance(msg, ClipboardUpdateMsg) else ""
        for cs in self.clients.values():
            if cs.machine_id == sender_id:
                continue
            try:
                await cs.conn.send(MsgType.CLIPBOARD_UPDATE, msg)
            except Exception:
                pass

    # ── Activation helper ────────────────────────────────────────

    async def _activate(self, machine_id: str):
        self.active_machine = machine_id
        cs = self.clients.get(machine_id)
        if cs:
            await cs.conn.send(MsgType.ACTIVATE, ActivateMsg(
                entry_local_x=-1, entry_local_y=-1,
            ))

    async def _handle_set_input_source(self, msg):
        machine_id = getattr(msg, "machine_id", None)
        if not machine_id and isinstance(msg, dict):
            machine_id = msg.get("machine_id")
        if not machine_id:
            return
        if machine_id not in self.clients:
            log.warning("SET_INPUT_SOURCE: unknown machine %s", machine_id)
            return
        await self._set_input_source(machine_id)

    async def _set_input_source(self, machine_id: str) -> None:
        if self.input_source == machine_id:
            return
        prev = self.input_source
        self.input_source = machine_id
        log.info("Input source: %s → %s", prev, machine_id)
        # Notify every client of its new state so it can start/stop its
        # keyboard and mouse capture.
        sends = []
        for mid, cs in self.clients.items():
            if not cs.monitors:
                continue
            sends.append(cs.conn.send(
                MsgType.INPUT_SOURCE_STATE,
                InputSourceStateMsg(is_input_source=(mid == machine_id)),
            ))
        results = await asyncio.gather(*sends, return_exceptions=True)
        for (mid, cs), r in zip(
            ((m, c) for m, c in self.clients.items() if c.monitors),
            results,
        ):
            if isinstance(r, Exception):
                log.warning("Failed INPUT_SOURCE_STATE to %s: %s", mid, r)
        await self._broadcast_layout()

    async def _handle_claim_focus(self, msg, client_state):
        """A dormant client is claiming focus because its mouse moved."""
        machine_id = getattr(msg, "machine_id", None)
        if not machine_id and isinstance(msg, dict):
            machine_id = msg.get("machine_id")
        if not machine_id:
            return
        if machine_id == self.active_machine:
            return
        if machine_id not in self.clients:
            return
        cs = self.clients[machine_id]
        if not cs.monitors:
            return  # ignore configurator / monitor-less clients

        log.info("Focus claim: %s → %s", self.active_machine, machine_id)
        prev = self.active_machine
        if prev and prev in self.clients:
            try:
                await self.clients[prev].conn.send(
                    MsgType.DEACTIVATE, DeactivateMsg(),
                )
            except Exception as e:
                log.warning("Failed to DEACTIVATE %s: %s", prev, e)
        self.active_machine = machine_id
        try:
            await cs.conn.send(MsgType.ACTIVATE, ActivateMsg(
                entry_local_x=-1, entry_local_y=-1,
            ))
        except Exception as e:
            log.warning("Failed to ACTIVATE %s: %s", machine_id, e)

    # ── Display identification ──────────────────────────────────

    async def _trigger_identify(self):
        """Send IDENTIFY to all clients with numbered display info.

        Numbering matches the Layout panel: group monitors by machine
        (machines ordered by leftmost global_x), then monitors within
        each machine by (global_y, global_x). Keeps PC A's overlays
        consecutive (1..k) and PC B's right after (k+1..n) instead of
        the old pure-spatial sort that produced interleaved numbers
        (1,2,4 on one PC / 3 on another when the machines' monitors
        overlapped vertically).
        """
        by_machine: dict[str, list] = {}
        for m in self.layout.monitors:
            by_machine.setdefault(m.machine_id, []).append(m)
        machine_order = sorted(
            by_machine.keys(),
            key=lambda mid: (
                min(m.global_x for m in by_machine[mid]),
                min(m.global_y for m in by_machine[mid]),
                mid,
            ),
        )
        number_map = {}  # (machine_id, monitor_id) → number
        n = 1
        for mid in machine_order:
            for m in sorted(by_machine[mid],
                            key=lambda m: (m.global_y, m.global_x)):
                number_map[(m.machine_id, m.monitor_id)] = n
                n += 1

        # Group by machine and send each client its displays
        for cs in self.clients.values():
            displays = []
            for m in self.layout.monitors:
                if m.machine_id == cs.machine_id:
                    num = number_map.get((m.machine_id, m.monitor_id), 0)
                    displays.append({
                        "monitor_id": m.monitor_id,
                        "number": num,
                        "label": f"{cs.machine_id} - {m.monitor_id}",
                    })
            if displays:
                try:
                    await cs.conn.send(MsgType.IDENTIFY, IdentifyMsg(
                        displays=displays, duration=3,
                    ))
                except Exception:
                    log.warning("Failed to send identify to %s",
                                cs.machine_id)

        log.info("Identification triggered on %d machine(s).",
                 len(self.clients))

    # ── Heartbeat ────────────────────────────────────────────────

    # Seconds without a HEARTBEAT_ACK before we consider a client
    # dead and force its conn closed. Has to be > heartbeat interval
    # * 2 so a single dropped ping doesn't cull an otherwise-healthy
    # client; 15 s matches the client-side watchdog.
    _CLIENT_HEARTBEAT_TIMEOUT = 15.0

    def _any_remote_real_client(self) -> bool:
        """True if any currently-connected client is remote (non-
        loopback) and carries monitors. When False, the server is
        effectively alone and the layout has nothing to arrange."""
        return any(cs.monitors and not cs.is_local
                   for cs in self.clients.values())

    async def _heartbeat_loop(self):
        seq = 0
        while True:
            await asyncio.sleep(5)
            seq += 1
            now = time.monotonic()
            for cs in list(self.clients.values()):
                # Cull stale clients — the handle_client finally block
                # does the actual layout cleanup + broadcast once the
                # conn closes, so all we need here is to close it.
                age = now - cs.last_ack_monotonic
                if age > self._CLIENT_HEARTBEAT_TIMEOUT:
                    log.info("No HEARTBEAT_ACK from %s in %.1fs — "
                             "closing stale connection.",
                             cs.machine_id, age)
                    try:
                        await cs.conn.close()
                    except Exception:
                        pass
                    continue
                try:
                    await cs.conn.send(MsgType.HEARTBEAT, HeartbeatMsg(seq=seq))
                except Exception:
                    pass

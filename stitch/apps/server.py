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
from dataclasses import dataclass, field
from typing import Optional

from ..core.protocol import (
    MsgType, RegisterMsg, RegisterAckMsg, LayoutUpdateMsg,
    EdgeHitMsg, ActivateMsg, DeactivateMsg, ClipboardUpdateMsg,
    MouseMoveRelMsg, MouseButtonMsg, MouseScrollMsg, KeyEventMsg,
    HeartbeatMsg, IdentifyMsg, ApplyMonitorsMsg,
)
from ..core.layout import LayoutManager
from ..core.network import Connection, serve
from .webui import load_saved_layout, save_layout

log = logging.getLogger(__name__)


@dataclass
class ClientState:
    """Per-client state tracked by the server."""
    machine_id: str
    conn: Connection
    monitors: list = field(default_factory=list)
    os: str = ""
    platform_info: str = ""


class Server:
    """Stitch central server."""

    def __init__(self, host: str = "0.0.0.0", port: int = 24800,
                 layout_offsets: Optional[dict] = None):
        self.host = host
        self.port = port
        self.layout = LayoutManager()
        self.layout_offsets = layout_offsets  # {machine_id: (gx, gy)}
        self.clients: dict[str, ClientState] = {}  # machine_id → state
        self.active_machine: Optional[str] = None
        self._server: Optional[asyncio.Server] = None
        self._heartbeat_task: Optional[asyncio.Task] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._saved_layout = load_saved_layout()  # from layout.json

    async def start(self):
        """Bind the listener and start background tasks. Returns once accepting."""
        self._loop = asyncio.get_running_loop()
        self._server = await serve(self.host, self.port, self._handle_client)
        self._heartbeat_task = asyncio.create_task(self._heartbeat_loop())
        log.info("Server ready. Waiting for clients...")

    async def serve_forever(self):
        """Block until the server is stopped."""
        if self._server is None:
            raise RuntimeError("Server.start() must be called first")
        await self._server.serve_forever()

    async def stop(self):
        if self._heartbeat_task:
            self._heartbeat_task.cancel()
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
                self.layout.remove_machine(machine_id)
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
            pass  # client alive

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
        )
        self.clients[machine_id] = cs

        # Configurator clients have no monitors — skip layout work
        if not msg.monitors:
            await conn.send(MsgType.REGISTER_ACK, RegisterAckMsg(ok=True))
            await self._broadcast_layout()
            return

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
        await conn.send(MsgType.REGISTER_ACK, RegisterAckMsg(ok=True))

        # If first client, make it active
        if self.active_machine is None:
            self.active_machine = machine_id
            await conn.send(MsgType.ACTIVATE, ActivateMsg(
                entry_local_x=-1, entry_local_y=-1,  # -1 means "keep current"
            ))

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

    # ── Display identification ──────────────────────────────────

    async def _trigger_identify(self):
        """Send IDENTIFY to all clients with numbered display info."""
        # Number all displays globally, sorted left-to-right top-to-bottom
        sorted_monitors = sorted(
            self.layout.monitors,
            key=lambda m: (m.global_y, m.global_x),
        )
        number_map = {}  # (machine_id, monitor_id) → number
        for i, m in enumerate(sorted_monitors, 1):
            number_map[(m.machine_id, m.monitor_id)] = i

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

    async def _heartbeat_loop(self):
        seq = 0
        while True:
            await asyncio.sleep(5)
            seq += 1
            for cs in list(self.clients.values()):
                try:
                    await cs.conn.send(MsgType.HEARTBEAT, HeartbeatMsg(seq=seq))
                except Exception:
                    pass

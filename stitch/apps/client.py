"""
Client agent — runs on every PC in the Stitch.

Three modes of operation:
  ACTIVE:     Cursor is on this machine. Input works normally.
              A polling thread monitors cursor position for edge crossings.
  FORWARDING: Cursor left this machine. Pointer+keyboard are grabbed.
              All input events are captured and sent to the server.
  RECEIVING:  Cursor is on another machine and that machine is forwarding
              input here. We inject events via the platform backend.

The client is always in exactly one of these modes.

Platform-agnostic: all OS-specific input handling is delegated to the
InputBackend abstraction (see stitch.backends).
"""

import asyncio
import enum
import logging
import platform
import threading
import time
from typing import Optional

from ..core.protocol import (
    MsgType, RegisterMsg, ActivateMsg, DeactivateMsg,
    LayoutUpdateMsg, EdgeHitMsg, MouseMoveRelMsg, MouseButtonMsg,
    MouseScrollMsg, KeyEventMsg, ClipboardUpdateMsg, HeartbeatMsg,
    ClaimFocusMsg,
)
from ..core.layout import LayoutManager, GlobalMonitor
from ..core.network import Connection, connect
from ..backends import InputBackend, create_backend
from ..features.filetransfer import FileTransferReceiver

log = logging.getLogger(__name__)

POLL_HZ = 120               # cursor polling rate
EDGE_MARGIN = 2             # pixels from edge to trigger crossing
CLIPBOARD_POLL_INTERVAL = 0.5


def _describe_platform() -> str:
    """Human-readable OS description, e.g. 'Ubuntu 24.04' or 'macOS 14.3'."""
    system = platform.system()
    if system == "Linux":
        try:
            info = platform.freedesktop_os_release()
            name = info.get("PRETTY_NAME") or info.get("NAME", "Linux")
            return name
        except (AttributeError, OSError):
            return f"Linux {platform.release()}"
    if system == "Darwin":
        return f"macOS {platform.mac_ver()[0] or platform.release()}"
    if system == "Windows":
        ver = platform.win32_ver()
        return f"Windows {ver[0] or platform.release()} {ver[1]}".strip()
    return f"{system} {platform.release()}".strip()


class Mode(enum.Enum):
    ACTIVE = "active"
    FORWARDING = "forwarding"
    RECEIVING = "receiving"


class Client:
    """Stitch client agent (cross-platform)."""

    def __init__(self, machine_id: str, server_host: str,
                 server_port: int = 24800):
        self.machine_id = machine_id
        self.server_host = server_host
        self.server_port = server_port

        self.mode = Mode.ACTIVE
        self.conn: Optional[Connection] = None

        # Layout state (updated from server)
        self.layout = LayoutManager()
        self.global_monitors: list[dict] = []

        # Platform backends — two instances for thread safety.
        # _backend_main: used from the asyncio thread (injection, clipboard)
        # _backend_input: used from the input-polling thread (cursor, grab)
        self._backend_main: Optional[InputBackend] = None
        self._backend_input: Optional[InputBackend] = None

        # Input thread state
        self._input_thread: Optional[threading.Thread] = None
        self._running = False
        self._lock = threading.Lock()
        self._event_queue: asyncio.Queue = asyncio.Queue()

        # Cursor tracking (forwarding mode)
        self._warp_cx = 0
        self._warp_cy = 0
        self._prev_btn_mask = 0
        self._edge_hit_sent = False
        self._dormant_cursor: Optional[tuple[int, int]] = None

        # Clipboard state
        self._last_clipboard = ""
        self._clipboard_suppress = False

        # File transfer
        self._file_receiver = FileTransferReceiver()

    # ── Lifecycle ────────────────────────────────────────────────

    async def run(self):
        """High-level entry: connect, register, and run all tasks."""
        self._backend_main = create_backend()
        self._backend_main.open()

        # Detect local monitors
        screens = self._backend_main.query_monitors()
        monitors = [
            {"id": s.id, "local_x": s.x, "local_y": s.y,
             "width": s.width, "height": s.height}
            for s in screens
        ]

        log.info("Machine '%s' has %d monitor(s):", self.machine_id, len(monitors))
        for m in monitors:
            log.info("  %s: %dx%d at (%d,%d)",
                     m["id"], m["width"], m["height"], m["local_x"], m["local_y"])

        # Connect to server
        self.conn = await connect(self.server_host, self.server_port,
                                  label=self.machine_id)

        # Register
        await self.conn.send(MsgType.REGISTER, RegisterMsg(
            machine_id=self.machine_id,
            monitors=monitors,
            os=platform.system(),
            platform_info=_describe_platform(),
        ))

        # Start input thread
        self._running = True
        self._input_thread = threading.Thread(
            target=self._input_loop, daemon=True, name="input-poll",
        )
        self._input_thread.start()

        # Read initial clipboard
        self._last_clipboard = self._backend_main.get_clipboard()

        # Run all tasks concurrently
        try:
            await asyncio.gather(
                self._recv_loop(),
                self._drain_queue(),
                self._clipboard_poll_loop(),
            )
        except asyncio.CancelledError:
            pass
        finally:
            await self.stop()

    async def stop(self):
        self._running = False
        with self._lock:
            if self._backend_input:
                if self._backend_input.is_grabbed:
                    self._backend_input.ungrab_input()
                self._backend_input.stop_key_capture()
                self._backend_input.close()
                self._backend_input = None
        if self.conn:
            await self.conn.close()
        if self._backend_main:
            self._backend_main.close()
            self._backend_main = None

    # ── Network receive loop ─────────────────────────────────────

    async def _recv_loop(self):
        while self._running and self.conn and not self.conn.closed:
            result = await self.conn.recv()
            if result is None:
                log.warning("Server connection lost.")
                break
            msg_type, payload = result
            await self._handle(msg_type, payload)

    async def _handle(self, msg_type: MsgType, payload):
        if msg_type == MsgType.REGISTER_ACK:
            log.info("Registered successfully.")

        elif msg_type == MsgType.LAYOUT_UPDATE:
            self._handle_layout_update(payload)

        elif msg_type == MsgType.ACTIVATE:
            await self._handle_activate(payload)

        elif msg_type == MsgType.DEACTIVATE:
            await self._handle_deactivate(payload)

        elif msg_type == MsgType.MOUSE_MOVE_REL:
            self._inject_mouse_rel(payload)

        elif msg_type == MsgType.MOUSE_BUTTON:
            self._inject_mouse_button(payload)

        elif msg_type == MsgType.MOUSE_SCROLL:
            self._inject_mouse_scroll(payload)

        elif msg_type == MsgType.KEY_EVENT:
            self._inject_key(payload)

        elif msg_type == MsgType.CLIPBOARD_UPDATE:
            self._handle_clipboard_update(payload)

        elif msg_type == MsgType.FILE_OFFER:
            self._file_receiver.handle_offer(
                payload if isinstance(payload, dict) else {})

        elif msg_type == MsgType.FILE_CHUNK:
            self._file_receiver.handle_chunk(
                payload if isinstance(payload, dict) else {})

        elif msg_type == MsgType.FILE_DONE:
            saved = self._file_receiver.handle_done(
                payload if isinstance(payload, dict) else {})
            if saved:
                log.info("File received and saved: %s", saved)

        elif msg_type == MsgType.IDENTIFY:
            self._handle_identify(payload)

        elif msg_type == MsgType.APPLY_MONITORS:
            self._handle_apply_monitors(payload)

        elif msg_type == MsgType.HEARTBEAT:
            await self.conn.send(MsgType.HEARTBEAT_ACK, payload)

    # ── Layout handling ──────────────────────────────────────────

    def _handle_layout_update(self, msg: LayoutUpdateMsg):
        self.global_monitors = msg.monitors if isinstance(msg, LayoutUpdateMsg) else []
        self.layout.clear()
        by_machine: dict[str, list] = {}
        for m in self.global_monitors:
            mid = m["machine_id"]
            by_machine.setdefault(mid, []).append(m)

        for mid, mons in by_machine.items():
            for m in mons:
                gm = GlobalMonitor(
                    machine_id=mid,
                    monitor_id=m["monitor_id"],
                    global_x=m["global_x"],
                    global_y=m["global_y"],
                    width=m["width"],
                    height=m["height"],
                )
                self.layout.monitors.append(gm)
            min_gx = min(m["global_x"] for m in mons)
            min_gy = min(m["global_y"] for m in mons)
            self.layout._machine_origin[mid] = (min_gx, min_gy)

        self.layout._rebuild_adjacency()
        log.info("Layout updated: %d total monitors across %d machines.",
                 len(self.global_monitors), len(by_machine))

    # ── Display identification ──────────────────────────────────

    def _handle_identify(self, msg):
        """Show identification overlay on each local display."""
        from ..features.identify import show_identify
        import threading

        if not self._backend_main:
            return

        display_defs = msg.displays if hasattr(msg, 'displays') else msg.get("displays", [])
        duration = msg.duration if hasattr(msg, 'duration') else msg.get("duration", 3)

        # Get local monitor positions from our backend
        screens = self._backend_main.query_monitors()
        screen_map = {s.id: s for s in screens}

        overlays = []
        for d in display_defs:
            mon_id = d.get("monitor_id") if isinstance(d, dict) else d.monitor_id
            screen = screen_map.get(mon_id)
            if screen is None and screens:
                # Fallback: match by index
                idx = display_defs.index(d)
                if idx < len(screens):
                    screen = screens[idx]
            if screen:
                overlays.append({
                    "x": screen.x,
                    "y": screen.y,
                    "width": screen.width,
                    "height": screen.height,
                    "number": d.get("number", 0) if isinstance(d, dict) else getattr(d, "number", 0),
                    "label": d.get("label", "") if isinstance(d, dict) else getattr(d, "label", ""),
                })

        if overlays:
            log.info("Showing identification on %d display(s).", len(overlays))
            # Run in a thread so we don't block the asyncio loop
            threading.Thread(
                target=show_identify,
                args=(overlays, duration),
                daemon=True,
            ).start()

    def _handle_apply_monitors(self, msg):
        """Reconfigure the OS display arrangement as told by the server."""
        if not self._backend_main:
            return
        raw = msg.positions if hasattr(msg, "positions") else \
            (msg.get("positions", {}) if isinstance(msg, dict) else {})
        positions = {mid: (int(xy[0]), int(xy[1])) for mid, xy in raw.items()}
        if not positions:
            return

        # Offload to a thread — xrandr/ChangeDisplaySettings can block for
        # several seconds and we must not stall the asyncio loop.
        def _apply():
            if self._backend_main.set_monitor_positions(positions):
                log.info("OS layout reconfigured: %s", positions)
            else:
                log.info("OS reconfiguration not supported or backend failed.")
        threading.Thread(target=_apply, daemon=True,
                         name="apply-monitors").start()

    # ── Activation / Deactivation ────────────────────────────────

    async def _handle_activate(self, msg: ActivateMsg):
        """Server says: you now own the cursor."""
        log.info("ACTIVATE: cursor entering at local (%d,%d)",
                 msg.entry_local_x, msg.entry_local_y)
        old_mode = self.mode
        self.mode = Mode.ACTIVE
        self._edge_hit_sent = True

        # Leaving dormant state: restore local cursor and stop intercepting
        # the local keyboard so the user's typing lands natively.
        if old_mode == Mode.FORWARDING:
            with self._lock:
                if self._backend_input:
                    self._backend_input.stop_key_capture()
                    if self._backend_input.is_grabbed:
                        self._backend_input.ungrab_input()

        if msg.entry_local_x >= 0 and msg.entry_local_y >= 0:
            self._backend_main.set_cursor_pos(
                msg.entry_local_x, msg.entry_local_y,
            )
            self._backend_main.flush()

    async def _handle_deactivate(self, msg: DeactivateMsg):
        """Server says: cursor has left this machine."""
        log.info("DEACTIVATE: cursor has left this machine.")
        self.mode = Mode.FORWARDING
        self._edge_hit_sent = False

        # Hide the local cursor and start capturing the keyboard so any
        # input on this PC gets forwarded to whichever machine currently
        # owns the focus cursor.
        with self._lock:
            if self._backend_input:
                self._backend_input.grab_input()
                self._backend_input.start_key_capture(self._on_key_event)
                cx, cy = self._backend_input.get_cursor_pos()
                self._dormant_cursor = (cx, cy)

    def _on_key_event(self, scancode: int, pressed: bool):
        """Callback from backend keyboard capture. scancode = HID usage ID."""
        if self.mode == Mode.FORWARDING:
            self._send_async(MsgType.KEY_EVENT, KeyEventMsg(
                keycode=scancode, pressed=pressed,
            ))

    # ── Input injection (RECEIVING mode) ─────────────────────────

    def _inject_mouse_rel(self, msg):
        if self.mode != Mode.ACTIVE or not self._backend_main:
            return
        dx = msg.dx if hasattr(msg, 'dx') else msg.get("dx", 0)
        dy = msg.dy if hasattr(msg, 'dy') else msg.get("dy", 0)
        self._backend_main.inject_mouse_move_rel(dx, dy)

    def _inject_mouse_button(self, msg):
        if self.mode != Mode.ACTIVE or not self._backend_main:
            return
        button = msg.button if hasattr(msg, 'button') else msg.get("button", 1)
        pressed = msg.pressed if hasattr(msg, 'pressed') else msg.get("pressed", True)
        self._backend_main.inject_mouse_button(button, pressed)

    def _inject_mouse_scroll(self, msg):
        if self.mode != Mode.ACTIVE or not self._backend_main:
            return
        dx = msg.dx if hasattr(msg, 'dx') else msg.get("dx", 0)
        dy = msg.dy if hasattr(msg, 'dy') else msg.get("dy", 0)
        self._backend_main.inject_mouse_scroll(dx, dy)

    def _inject_key(self, msg):
        if self.mode != Mode.ACTIVE or not self._backend_main:
            return
        keycode = msg.keycode if hasattr(msg, 'keycode') else msg.get("keycode", 0)
        pressed = msg.pressed if hasattr(msg, 'pressed') else msg.get("pressed", True)
        self._backend_main.inject_key(keycode, pressed)

    # ── Input capture thread ─────────────────────────────────────

    def _input_loop(self):
        """Runs in a separate thread. Polls cursor and captures input."""
        self._backend_input = create_backend()
        self._backend_input.open()
        poll_interval = 1.0 / POLL_HZ

        while self._running:
            try:
                if self.mode == Mode.ACTIVE:
                    self._poll_active()
                elif self.mode == Mode.FORWARDING:
                    self._poll_dormant()
            except Exception:
                log.exception("Error in input loop")
            time.sleep(poll_interval)

        with self._lock:
            if self._backend_input:
                self._backend_input.close()
                self._backend_input = None

    def _poll_active(self):
        """In ACTIVE mode: check if cursor is at a screen edge."""
        with self._lock:
            if not self._backend_input:
                return
            cx, cy = self._backend_input.get_cursor_pos()

        my_monitors = self.layout.get_monitors_for_machine(self.machine_id)
        if not my_monitors:
            return

        gl = self.layout.local_to_global(self.machine_id, cx, cy)
        if gl is None:
            return
        gx, gy = gl

        # Find which of this machine's monitors contains the cursor
        current = None
        for m in my_monitors:
            if m.contains(gx, gy):
                current = m
                break
        if current is None:
            min_dist = float('inf')
            for m in my_monitors:
                clx = max(m.global_x, min(gx, m.right - 1))
                cly = max(m.global_y, min(gy, m.bottom - 1))
                d = abs(clx - gx) + abs(cly - gy)
                if d < min_dist:
                    min_dist = d
                    current = m
            if current is None:
                return

        edge = None
        if gx <= current.global_x + EDGE_MARGIN:
            edge = "left"
        elif gx >= current.right - 1 - EDGE_MARGIN:
            edge = "right"
        elif gy <= current.global_y + EDGE_MARGIN:
            edge = "top"
        elif gy >= current.bottom - 1 - EDGE_MARGIN:
            edge = "bottom"

        if edge is None:
            self._edge_hit_sent = False
            return

        if self._edge_hit_sent:
            return

        result = self.layout.find_crossing_target(edge, gx, gy)
        if result and result[0].machine_id != self.machine_id:
            self._edge_hit_sent = True
            self._send_async(MsgType.EDGE_HIT, EdgeHitMsg(
                edge=edge,
                global_x=gx,
                global_y=gy,
                machine_id=self.machine_id,
            ))

    def _poll_dormant(self):
        """Dormant mode: if the local user moves this PC's mouse, claim focus."""
        with self._lock:
            if not self._backend_input:
                return
            cx, cy = self._backend_input.get_cursor_pos()

        last = self._dormant_cursor
        if last is None:
            self._dormant_cursor = (cx, cy)
            return
        if (cx, cy) == last:
            return

        self._dormant_cursor = (cx, cy)
        self._send_async(
            MsgType.CLAIM_FOCUS,
            ClaimFocusMsg(machine_id=self.machine_id),
        )

    # ── Async bridge ─────────────────────────────────────────────

    def _send_async(self, msg_type: MsgType, payload):
        try:
            self._event_queue.put_nowait((msg_type, payload))
        except asyncio.QueueFull:
            pass

    async def _drain_queue(self):
        while self._running:
            try:
                msg_type, payload = await asyncio.wait_for(
                    self._event_queue.get(), timeout=0.01,
                )
                if self.conn and not self.conn.closed:
                    await self.conn.send(msg_type, payload)
            except asyncio.TimeoutError:
                pass
            except Exception:
                log.exception("Error draining event queue")

    # ── Clipboard ────────────────────────────────────────────────

    async def _clipboard_poll_loop(self):
        """Poll clipboard for changes and broadcast to other machines."""
        while self._running:
            await asyncio.sleep(CLIPBOARD_POLL_INTERVAL)
            if not self._backend_main:
                continue
            try:
                current = self._backend_main.get_clipboard()
            except Exception:
                continue
            if current != self._last_clipboard:
                self._last_clipboard = current
                if self._clipboard_suppress:
                    self._clipboard_suppress = False
                    continue
                if self.conn and not self.conn.closed:
                    await self.conn.send(MsgType.CLIPBOARD_UPDATE,
                                         ClipboardUpdateMsg(
                                             content=current,
                                             source_machine=self.machine_id,
                                         ))

    def _handle_clipboard_update(self, msg):
        if not self._backend_main:
            return
        content = msg.content if hasattr(msg, 'content') else msg.get("content", "")
        self._clipboard_suppress = True
        self._last_clipboard = content
        self._backend_main.set_clipboard(content)

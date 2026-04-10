"""
Client agent — runs on every PC in the unified desktop.

Three modes of operation:
  ACTIVE:     Cursor is on this machine. Input works normally.
              A polling thread monitors cursor position for edge crossings.
  FORWARDING: Cursor left this machine. Pointer+keyboard are grabbed.
              All input events are captured and sent to the server.
  RECEIVING:  Cursor is on another machine and that machine is forwarding
              input here. We inject events via XTest.

The client is always in exactly one of these modes.
"""

import asyncio
import enum
import logging
import threading
import time
from dataclasses import dataclass
from typing import Optional

from .protocol import (
    MsgType, RegisterMsg, MonitorInfo, ActivateMsg, DeactivateMsg,
    LayoutUpdateMsg, EdgeHitMsg, MouseMoveRelMsg, MouseButtonMsg,
    MouseScrollMsg, KeyEventMsg, ClipboardUpdateMsg, HeartbeatMsg,
)
from .layout import LayoutManager, GlobalMonitor
from .network import Connection, connect
from .clipboard import ClipboardMonitor
from .keyboard import KeyboardCapture
from .filetransfer import FileTransferReceiver

log = logging.getLogger(__name__)

POLL_HZ = 120               # cursor polling rate
EDGE_MARGIN = 2             # pixels from edge to trigger crossing
WARP_CENTER_MARGIN = 50     # keep center-warp away from edges


class Mode(enum.Enum):
    ACTIVE = "active"
    FORWARDING = "forwarding"
    RECEIVING = "receiving"


class Client:
    """Unified Desktop client agent."""

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

        # X11 display — created in the input thread (X11 requires
        # per-thread display connections for safety)
        self._x11_main = None      # for injection (asyncio thread)
        self._x11_input = None     # for capture (input thread)

        # Input thread state
        self._input_thread: Optional[threading.Thread] = None
        self._running = False
        self._lock = threading.Lock()
        self._event_queue: asyncio.Queue = asyncio.Queue()

        # Cursor tracking
        self._last_cx = 0
        self._last_cy = 0
        self._warp_cx = 0          # center X for delta computation
        self._warp_cy = 0
        self._prev_mask = 0
        self._edge_hit_sent = False  # debounce: only send one edge_hit per crossing

        # Clipboard
        self._clipboard: Optional[ClipboardMonitor] = None

        # Keyboard capture (evdev)
        self._kbd: Optional[KeyboardCapture] = None

        # File transfer
        self._file_receiver = FileTransferReceiver()

    # ── Lifecycle ────────────────────────────────────────────────

    async def start(self):
        """Connect to server and start the client."""
        from .x11 import X11Display
        self._x11_main = X11Display()

        # Detect local monitors
        screens = self._x11_main.query_monitors()
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
        ))

        # Start input thread
        self._running = True
        self._input_thread = threading.Thread(
            target=self._input_loop, daemon=True, name="input-poll",
        )
        self._input_thread.start()

        # Start clipboard monitor
        self._clipboard = ClipboardMonitor(self._on_clipboard_change)
        asyncio.create_task(self._clipboard_task())

        # Main receive loop
        await self._recv_loop()

    async def stop(self):
        self._running = False
        if self._clipboard:
            self._clipboard.stop()
        if self._x11_input:
            with self._lock:
                if self._x11_input._grabbed:
                    self._x11_input.ungrab_pointer()
                if self._x11_input._kb_grabbed:
                    self._x11_input.ungrab_keyboard()
        if self.conn:
            await self.conn.close()
        if self._x11_main:
            self._x11_main.close()

    # ── Network receive loop ─────────────────────────────────────

    async def _recv_loop(self):
        """Receive messages from the server."""
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

        elif msg_type == MsgType.HEARTBEAT:
            await self.conn.send(MsgType.HEARTBEAT_ACK, payload)

    # ── Layout handling ──────────────────────────────────────────

    def _handle_layout_update(self, msg: LayoutUpdateMsg):
        self.global_monitors = msg.monitors if isinstance(msg, LayoutUpdateMsg) else []
        # Rebuild layout from the monitor list
        self.layout.clear()
        # Group by machine
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
            # Derive machine origin: min global position of its monitors.
            # This works because global = origin + local, and for each
            # machine the minimum local position is (0,0) or the
            # Xinerama-reported offset of its leftmost/topmost monitor.
            min_gx = min(m["global_x"] for m in mons)
            min_gy = min(m["global_y"] for m in mons)
            self.layout._machine_origin[mid] = (min_gx, min_gy)

        self.layout._rebuild_adjacency()
        log.info("Layout updated: %d total monitors across %d machines.",
                 len(self.global_monitors), len(by_machine))

    # ── Activation / Deactivation ────────────────────────────────

    async def _handle_activate(self, msg: ActivateMsg):
        """Server says: you now own the cursor."""
        log.info("ACTIVATE: cursor entering this machine at local (%d,%d)",
                 msg.entry_local_x, msg.entry_local_y)

        old_mode = self.mode
        self.mode = Mode.ACTIVE
        self._edge_hit_sent = False

        # Release grabs if we were forwarding
        if old_mode == Mode.FORWARDING:
            # Stop keyboard capture
            if self._kbd:
                self._kbd.stop()
                self._kbd = None
            with self._lock:
                if self._x11_input:
                    self._x11_input.ungrab_pointer()
                    self._x11_input.ungrab_keyboard()

        # Warp cursor to entry point if specified
        if msg.entry_local_x >= 0 and msg.entry_local_y >= 0:
            self._x11_main.warp_pointer(msg.entry_local_x, msg.entry_local_y)
            self._x11_main.flush()

    async def _handle_deactivate(self, msg: DeactivateMsg):
        """Server says: release cursor, start forwarding input."""
        log.info("DEACTIVATE: cursor leaving this machine, entering forwarding mode.")
        self.mode = Mode.FORWARDING

        # Start keyboard capture via evdev
        if self._kbd is None:
            self._kbd = KeyboardCapture(self._on_key_event)
            self._kbd.start()

        # Grab pointer + keyboard so we capture all input
        with self._lock:
            if self._x11_input:
                self._x11_input.grab_pointer()
                self._x11_input.grab_keyboard()
                # Warp to center for delta tracking
                sw = self._x11_input.screen_width
                sh = self._x11_input.screen_height
                self._warp_cx = sw // 2
                self._warp_cy = sh // 2
                self._x11_input.warp_pointer(self._warp_cx, self._warp_cy)
                self._x11_input.sync()

    def _on_key_event(self, x11_keycode: int, pressed: bool):
        """Callback from evdev keyboard capture (runs in kbd thread)."""
        if self.mode == Mode.FORWARDING:
            self._send_async(MsgType.KEY_EVENT, KeyEventMsg(
                keycode=x11_keycode, pressed=pressed,
            ))

    # ── Input injection (RECEIVING mode) ─────────────────────────

    def _inject_mouse_rel(self, msg):
        if self.mode != Mode.ACTIVE:
            return
        if isinstance(msg, dict):
            dx, dy = msg.get("dx", 0), msg.get("dy", 0)
        else:
            dx, dy = msg.dx, msg.dy
        self._x11_main.fake_relative_motion(dx, dy)

    def _inject_mouse_button(self, msg):
        if self.mode != Mode.ACTIVE:
            return
        if isinstance(msg, dict):
            button, pressed = msg.get("button", 1), msg.get("pressed", True)
        else:
            button, pressed = msg.button, msg.pressed
        self._x11_main.fake_button(button, pressed)

    def _inject_mouse_scroll(self, msg):
        if self.mode != Mode.ACTIVE:
            return
        if isinstance(msg, dict):
            dy = msg.get("dy", 0)
        else:
            dy = msg.dy
        # X11 scroll: button 4 = up, button 5 = down
        if dy > 0:
            for _ in range(abs(dy)):
                self._x11_main.fake_button(4, True)
                self._x11_main.fake_button(4, False)
        elif dy < 0:
            for _ in range(abs(dy)):
                self._x11_main.fake_button(5, True)
                self._x11_main.fake_button(5, False)

    def _inject_key(self, msg):
        if self.mode != Mode.ACTIVE:
            return
        if isinstance(msg, dict):
            keycode, pressed = msg.get("keycode", 0), msg.get("pressed", True)
        else:
            keycode, pressed = msg.keycode, msg.pressed
        self._x11_main.fake_key(keycode, pressed)

    # ── Input capture thread ─────────────────────────────────────

    def _input_loop(self):
        """
        Runs in a separate thread. Polls cursor position and captures
        input when in FORWARDING mode.
        """
        from .x11 import X11Display

        self._x11_input = X11Display()
        poll_interval = 1.0 / POLL_HZ

        while self._running:
            try:
                if self.mode == Mode.ACTIVE:
                    self._poll_active()
                elif self.mode == Mode.FORWARDING:
                    self._poll_forwarding()
                else:
                    # RECEIVING: nothing to poll
                    pass
            except Exception:
                log.exception("Error in input loop")

            time.sleep(poll_interval)

        self._x11_input.close()
        self._x11_input = None

    def _poll_active(self):
        """In ACTIVE mode: check if cursor is at a screen edge."""
        with self._lock:
            if self._x11_input is None:
                return
            cx, cy, mask = self._x11_input.query_pointer()

        self._last_cx = cx
        self._last_cy = cy
        self._prev_mask = mask

        # Determine which of our monitors the cursor is on
        my_monitors = self.layout.get_monitors_for_machine(self.machine_id)
        if not my_monitors:
            return

        # Convert local X screen coords to global virtual coords.
        # local_to_global adds the machine's global origin offset.
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
            # Cursor at extreme edge — find closest monitor
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

        # Check if cursor is within EDGE_MARGIN of any edge of the
        # current monitor. Use elif so corners pick one direction
        # (prefer horizontal over vertical since that's more common).
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
            # Cursor moved away from edges — reset debounce
            self._edge_hit_sent = False
            return

        if self._edge_hit_sent:
            # Already sent an edge_hit for this crossing, wait for
            # server to process it (avoids flooding 120 msgs/sec)
            return

        # Check if there's a monitor on the other side that belongs
        # to a DIFFERENT machine. This correctly ignores internal
        # edges between monitors on the same PC.
        result = self.layout.find_crossing_target(edge, gx, gy)
        if result and result[0].machine_id != self.machine_id:
            self._edge_hit_sent = True
            self._send_async(MsgType.EDGE_HIT, EdgeHitMsg(
                edge=edge,
                global_x=gx,
                global_y=gy,
                machine_id=self.machine_id,
            ))

    def _poll_forwarding(self):
        """
        In FORWARDING mode: pointer is grabbed. Read cursor position,
        compute delta from center, warp back to center, send delta.
        Also detect button state changes.
        """
        with self._lock:
            if self._x11_input is None:
                return
            cx, cy, mask = self._x11_input.query_pointer()

        dx = cx - self._warp_cx
        dy = cy - self._warp_cy

        # Send mouse movement delta
        if dx != 0 or dy != 0:
            self._send_async(MsgType.MOUSE_MOVE_REL, MouseMoveRelMsg(dx=dx, dy=dy))
            # Warp back to center
            with self._lock:
                if self._x11_input:
                    self._x11_input.warp_pointer(self._warp_cx, self._warp_cy)

        # Detect button changes via mask
        # Button1 = bit 8, Button2 = bit 9, Button3 = bit 10
        for btn in range(1, 6):
            btn_mask = 1 << (7 + btn)
            was_pressed = bool(self._prev_mask & btn_mask)
            is_pressed = bool(mask & btn_mask)
            if is_pressed and not was_pressed:
                # Scroll wheel buttons: send as scroll events
                if btn == 4:
                    self._send_async(MsgType.MOUSE_SCROLL,
                                     MouseScrollMsg(dx=0, dy=1))
                elif btn == 5:
                    self._send_async(MsgType.MOUSE_SCROLL,
                                     MouseScrollMsg(dx=0, dy=-1))
                else:
                    self._send_async(MsgType.MOUSE_BUTTON,
                                     MouseButtonMsg(button=btn, pressed=True))
            elif was_pressed and not is_pressed:
                if btn not in (4, 5):
                    self._send_async(MsgType.MOUSE_BUTTON,
                                     MouseButtonMsg(button=btn, pressed=False))

        self._prev_mask = mask

    # ── Keyboard capture in FORWARDING mode ──────────────────────
    # Note: Keyboard events are harder to capture via polling since
    # XQueryPointer only returns modifier mask, not individual key
    # events. For the MVP, we use a separate approach.
    #
    # We'll use XRecord extension or a simpler fallback:
    # monitor /dev/input for key events. For the initial MVP,
    # keyboard forwarding uses the modifier mask changes + a
    # supplementary evdev reader.

    # ── Async bridge ─────────────────────────────────────────────

    def _send_async(self, msg_type: MsgType, payload):
        """Thread-safe: queue a message to send from the asyncio thread."""
        try:
            self._event_queue.put_nowait((msg_type, payload))
        except asyncio.QueueFull:
            pass  # drop if queue full (back-pressure)

    async def _drain_queue(self):
        """Drain queued events and send them to the server."""
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

    async def _on_clipboard_change(self, content: str):
        """Local clipboard changed — broadcast to other machines."""
        if self.conn and not self.conn.closed:
            await self.conn.send(MsgType.CLIPBOARD_UPDATE, ClipboardUpdateMsg(
                content=content,
                source_machine=self.machine_id,
            ))

    def _handle_clipboard_update(self, msg):
        if self._clipboard:
            content = msg.content if hasattr(msg, 'content') else msg.get("content", "")
            self._clipboard.set_remote(content)

    async def _clipboard_task(self):
        if self._clipboard:
            await self._clipboard.start()

    # ── Full run ─────────────────────────────────────────────────

    async def run(self):
        """High-level entry: connect, register, and run all tasks."""
        from .x11 import X11Display
        self._x11_main = X11Display()

        # Detect local monitors
        screens = self._x11_main.query_monitors()
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
        ))

        # Start input thread
        self._running = True
        self._input_thread = threading.Thread(
            target=self._input_loop, daemon=True, name="input-poll",
        )
        self._input_thread.start()

        # Start clipboard monitor
        self._clipboard = ClipboardMonitor(self._on_clipboard_change)

        # Run all tasks concurrently
        try:
            await asyncio.gather(
                self._recv_loop(),
                self._drain_queue(),
                self._clipboard_task(),
            )
        except asyncio.CancelledError:
            pass
        finally:
            await self.stop()

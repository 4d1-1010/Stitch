"""
Native display configuration application.

A tkinter GUI that connects to the Unified Desktop server and provides:
  - Live view of all connected machines and their displays
  - Drag-and-drop display arrangement matching physical placement
  - Snap-to-edge when positioning displays
  - "Identify" button to flash numbers on all physical displays
  - "Apply" to push the layout to the server and all clients

Run:
    python -m ud.configurator --server HOST [--port PORT]
"""

import asyncio
import json
import logging
import math
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox
from dataclasses import dataclass, field
from typing import Optional

from .protocol import (
    MsgType, RegisterMsg, RegisterAckMsg, LayoutUpdateMsg,
    IdentifyMsg, HeartbeatMsg, HEADER_SIZE,
    encode_message, decode_header, decode_payload,
)
from .network import Connection

log = logging.getLogger(__name__)

# ── Color palette for machines ───────────────────────────────────

MACHINE_COLORS = [
    "#e94560", "#4a90d9", "#6a5acd", "#2ecc71", "#e67e22",
    "#1abc9c", "#9b59b6", "#e74c3c", "#3498db", "#f39c12",
]

SNAP_THRESHOLD = 20  # pixels in canvas space
CANVAS_BG = "#1a1a2e"
DISPLAY_BORDER_ACTIVE = "#ffffff"


# ── Data ─────────────────────────────────────────────────────────

@dataclass
class DisplayInfo:
    machine_id: str
    monitor_id: str
    global_x: int
    global_y: int
    width: int
    height: int
    number: int = 0


# ── Main application ─────────────────────────────────────────────

class ConfiguratorApp:
    """Native tkinter display layout configurator."""

    def __init__(self, server_host: str, server_port: int = 24800):
        self.server_host = server_host
        self.server_port = server_port

        self.displays: list[DisplayInfo] = []
        self.original_displays: list[DisplayInfo] = []
        self.machines: dict[str, dict] = {}
        self.color_map: dict[str, str] = {}
        self._color_idx = 0

        # Connection state
        self.conn: Optional[Connection] = None
        self._connected = False
        self._net_thread: Optional[threading.Thread] = None
        self._running = True

        # Drag state
        self._drag_display: Optional[DisplayInfo] = None
        self._drag_offset = (0, 0)
        self._dirty = False

        # View transform
        self._scale = 0.15
        self._pan_x = 0.0
        self._pan_y = 0.0

        self._build_ui()

    def _get_color(self, machine_id: str) -> str:
        if machine_id not in self.color_map:
            self.color_map[machine_id] = MACHINE_COLORS[
                self._color_idx % len(MACHINE_COLORS)
            ]
            self._color_idx += 1
        return self.color_map[machine_id]

    # ── UI construction ──────────────────────────────────────────

    def _build_ui(self):
        self.root = tk.Tk()
        self.root.title("Unified Desktop - Display Configuration")
        self.root.geometry("1100x700")
        self.root.configure(bg="#16213e")
        self.root.minsize(800, 500)

        # ── Top bar ──────────────────────────────────
        top = tk.Frame(self.root, bg="#16213e", pady=8, padx=12)
        top.pack(fill=tk.X)

        tk.Label(top, text="Unified Desktop", font=("Helvetica", 16, "bold"),
                 fg="#e94560", bg="#16213e").pack(side=tk.LEFT)

        self._status_label = tk.Label(top, text="Connecting...",
                                      font=("Helvetica", 11),
                                      fg="#888888", bg="#16213e")
        self._status_label.pack(side=tk.RIGHT)

        # ── Main area (sidebar + canvas) ─────────────
        main = tk.Frame(self.root, bg=CANVAS_BG)
        main.pack(fill=tk.BOTH, expand=True)

        # Sidebar
        sidebar = tk.Frame(main, bg="#16213e", width=240)
        sidebar.pack(side=tk.LEFT, fill=tk.Y)
        sidebar.pack_propagate(False)

        tk.Label(sidebar, text="MACHINES", font=("Helvetica", 10, "bold"),
                 fg="#666666", bg="#16213e", anchor="w",
                 padx=12, pady=(12, 6)).pack(fill=tk.X)

        self._machine_frame = tk.Frame(sidebar, bg="#16213e")
        self._machine_frame.pack(fill=tk.BOTH, expand=True, padx=8)

        # Canvas
        self.canvas = tk.Canvas(main, bg=CANVAS_BG, highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.canvas.bind("<Configure>", lambda e: self._redraw())

        # ── Bottom toolbar ───────────────────────────
        toolbar = tk.Frame(self.root, bg="#16213e", pady=10)
        toolbar.pack(fill=tk.X)

        btn_frame = tk.Frame(toolbar, bg="#16213e")
        btn_frame.pack()

        self._btn_identify = tk.Button(
            btn_frame, text="  Identify Displays  ",
            font=("Helvetica", 11, "bold"),
            bg="#e94560", fg="white", activebackground="#d63851",
            relief=tk.FLAT, padx=16, pady=6,
            command=self._do_identify,
        )
        self._btn_identify.pack(side=tk.LEFT, padx=6)

        self._btn_apply = tk.Button(
            btn_frame, text="  Apply Layout  ",
            font=("Helvetica", 11, "bold"),
            bg="#0f3460", fg="white", activebackground="#1a4a7a",
            relief=tk.FLAT, padx=16, pady=6,
            command=self._do_apply,
            state=tk.DISABLED,
        )
        self._btn_apply.pack(side=tk.LEFT, padx=6)

        self._btn_reset = tk.Button(
            btn_frame, text="  Reset  ",
            font=("Helvetica", 11),
            bg="#333333", fg="#cccccc", activebackground="#444444",
            relief=tk.FLAT, padx=16, pady=6,
            command=self._do_reset,
        )
        self._btn_reset.pack(side=tk.LEFT, padx=6)

    # ── Network thread ───────────────────────────────────────────

    def _start_network(self):
        self._net_thread = threading.Thread(
            target=self._network_loop, daemon=True, name="config-net",
        )
        self._net_thread.start()

    def _network_loop(self):
        """Connect to the server and poll for layout updates."""
        loop = asyncio.new_event_loop()
        while self._running:
            try:
                loop.run_until_complete(self._connect_and_listen())
            except Exception as e:
                log.debug("Network error: %s", e)
            if self._running:
                time.sleep(2)  # reconnect delay

    async def _connect_and_listen(self):
        try:
            reader, writer = await asyncio.open_connection(
                self.server_host, self.server_port,
            )
        except (ConnectionRefusedError, OSError) as e:
            self.root.after(0, self._set_status, f"Cannot connect: {e}")
            return

        self.conn = Connection(reader, writer, label="configurator")
        self._connected = True
        self.root.after(0, self._set_status, "Connected")

        # Register as a special "configurator" client with no monitors
        await self.conn.send(MsgType.REGISTER, RegisterMsg(
            machine_id="__configurator__",
            monitors=[],
        ))

        try:
            while self._running and not self.conn.closed:
                result = await self.conn.recv()
                if result is None:
                    break
                msg_type, payload = result
                if msg_type == MsgType.LAYOUT_UPDATE:
                    self.root.after(0, self._on_layout_update, payload)
                elif msg_type == MsgType.REGISTER_ACK:
                    pass
                elif msg_type == MsgType.HEARTBEAT:
                    await self.conn.send(MsgType.HEARTBEAT_ACK, payload)
        except Exception:
            pass
        finally:
            self._connected = False
            self.conn = None
            self.root.after(0, self._set_status, "Disconnected")

    def _send_to_server(self, msg_type, payload):
        """Thread-safe send."""
        if self.conn and not self.conn.closed:
            loop = asyncio.new_event_loop()
            try:
                loop.run_until_complete(self.conn.send(msg_type, payload))
            except Exception as e:
                log.warning("Send failed: %s", e)
            finally:
                loop.close()

    # ── Layout update handling ───────────────────────────────────

    def _on_layout_update(self, msg):
        """Called on main thread when server sends layout."""
        monitors = msg.monitors if hasattr(msg, 'monitors') else msg.get('monitors', [])

        # Don't clobber user's drag-in-progress
        if self._drag_display is not None:
            return

        self.displays.clear()
        machine_set = set()
        for m in monitors:
            mid = m["machine_id"] if isinstance(m, dict) else m.machine_id
            if mid == "__configurator__":
                continue
            mon_id = m["monitor_id"] if isinstance(m, dict) else m.monitor_id
            gx = m["global_x"] if isinstance(m, dict) else m.global_x
            gy = m["global_y"] if isinstance(m, dict) else m.global_y
            w = m["width"] if isinstance(m, dict) else m.width
            h = m["height"] if isinstance(m, dict) else m.height
            self.displays.append(DisplayInfo(
                machine_id=mid, monitor_id=mon_id,
                global_x=gx, global_y=gy, width=w, height=h,
            ))
            machine_set.add(mid)

        # Number displays
        self.displays.sort(key=lambda d: (d.global_y, d.global_x))
        for i, d in enumerate(self.displays, 1):
            d.number = i

        if not self._dirty:
            self.original_displays = [
                DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})
                for d in self.displays
            ]

        self._update_status()
        self._update_sidebar()
        self._fit_view()
        self._redraw()

    def _set_status(self, text):
        self._status_label.config(text=text)

    def _update_status(self):
        mc = len(set(d.machine_id for d in self.displays))
        dc = len(self.displays)
        conn = "Connected" if self._connected else "Disconnected"
        self._status_label.config(
            text=f"{conn} | {mc} machine{'s' if mc != 1 else ''}, "
                 f"{dc} display{'s' if dc != 1 else ''}",
        )

    def _update_sidebar(self):
        for w in self._machine_frame.winfo_children():
            w.destroy()

        by_machine: dict[str, list[DisplayInfo]] = {}
        for d in self.displays:
            by_machine.setdefault(d.machine_id, []).append(d)

        for mid, mons in sorted(by_machine.items()):
            color = self._get_color(mid)
            card = tk.Frame(self._machine_frame, bg="#1a1a2e",
                            highlightbackground=color,
                            highlightthickness=2, pady=6, padx=8)
            card.pack(fill=tk.X, pady=4)

            tk.Label(card, text=mid, font=("Helvetica", 12, "bold"),
                     fg=color, bg="#1a1a2e", anchor="w").pack(fill=tk.X)
            tk.Label(card, text=f"{len(mons)} display{'s' if len(mons)!=1 else ''}",
                     font=("Helvetica", 9), fg="#666", bg="#1a1a2e",
                     anchor="w").pack(fill=tk.X)

            for m in mons:
                tk.Label(card,
                         text=f"  #{m.number}  {m.monitor_id}  {m.width}x{m.height}",
                         font=("Helvetica", 9), fg="#999", bg="#1a1a2e",
                         anchor="w").pack(fill=tk.X)

    # ── View transform ───────────────────────────────────────────

    def _fit_view(self):
        if not self.displays:
            return
        cw = self.canvas.winfo_width() or 800
        ch = self.canvas.winfo_height() or 500
        padding = 80

        min_x = min(d.global_x for d in self.displays)
        min_y = min(d.global_y for d in self.displays)
        max_x = max(d.global_x + d.width for d in self.displays)
        max_y = max(d.global_y + d.height for d in self.displays)

        bw = max(max_x - min_x, 1)
        bh = max(max_y - min_y, 1)

        self._scale = min((cw - padding * 2) / bw, (ch - padding * 2) / bh)
        self._scale = min(self._scale, 0.4)
        self._pan_x = (cw - bw * self._scale) / 2 - min_x * self._scale
        self._pan_y = (ch - bh * self._scale) / 2 - min_y * self._scale

    def _to_screen(self, gx, gy):
        return (gx * self._scale + self._pan_x,
                gy * self._scale + self._pan_y)

    def _to_global(self, sx, sy):
        return ((sx - self._pan_x) / self._scale,
                (sy - self._pan_y) / self._scale)

    # ── Canvas drawing ───────────────────────────────────────────

    def _redraw(self):
        c = self.canvas
        c.delete("all")
        cw = c.winfo_width()
        ch = c.winfo_height()

        # Grid lines
        if self._scale > 0.01:
            step_g = 200
            g0 = self._to_global(0, 0)
            start_gx = int(g0[0] // step_g) * step_g
            start_gy = int(g0[1] // step_g) * step_g
            gx = start_gx
            while True:
                sx = gx * self._scale + self._pan_x
                if sx > cw:
                    break
                if sx >= 0:
                    c.create_line(sx, 0, sx, ch, fill="#222244", width=1)
                gx += step_g
            gy = start_gy
            while True:
                sy = gy * self._scale + self._pan_y
                if sy > ch:
                    break
                if sy >= 0:
                    c.create_line(0, sy, cw, sy, fill="#222244", width=1)
                gy += step_g

        # Displays
        for d in self.displays:
            sx, sy = self._to_screen(d.global_x, d.global_y)
            sw = d.width * self._scale
            sh = d.height * self._scale
            color = self._get_color(d.machine_id)
            is_dragging = (d is self._drag_display)

            fill = color + ("40" if is_dragging else "20")
            border_w = 3 if is_dragging else 2

            c.create_rectangle(sx, sy, sx + sw, sy + sh,
                               fill=fill, outline=color, width=border_w)

            # Number (center)
            num_size = max(10, min(56, int(sh * 0.30)))
            c.create_text(sx + sw / 2, sy + sh / 2 - num_size * 0.15,
                          text=str(d.number),
                          font=("Helvetica", num_size, "bold"),
                          fill="white")

            # Machine name
            lbl_size = max(8, min(16, int(sh * 0.08)))
            c.create_text(sx + sw / 2, sy + sh / 2 + num_size * 0.35,
                          text=d.machine_id,
                          font=("Helvetica", lbl_size), fill="#aaaaaa")

            # Resolution (bottom)
            res_size = max(7, min(12, int(sh * 0.06)))
            c.create_text(sx + sw / 2, sy + sh - res_size - 4,
                          text=f"{d.width}x{d.height}",
                          font=("Helvetica", res_size), fill="#555555")

            # Monitor ID (top-left)
            c.create_text(sx + 5, sy + res_size + 3,
                          text=d.monitor_id, anchor="w",
                          font=("Helvetica", res_size), fill=color)

    # ── Mouse interaction ────────────────────────────────────────

    def _hit_test(self, sx, sy) -> Optional[DisplayInfo]:
        for d in reversed(self.displays):
            dx, dy = self._to_screen(d.global_x, d.global_y)
            dw = d.width * self._scale
            dh = d.height * self._scale
            if dx <= sx <= dx + dw and dy <= sy <= dy + dh:
                return d
        return None

    def _on_press(self, event):
        d = self._hit_test(event.x, event.y)
        if d:
            self._drag_display = d
            sx, sy = self._to_screen(d.global_x, d.global_y)
            self._drag_offset = (event.x - sx, event.y - sy)
            self.canvas.config(cursor="fleur")
            self._redraw()

    def _on_drag(self, event):
        if self._drag_display is None:
            return
        d = self._drag_display
        gx, gy = self._to_global(
            event.x - self._drag_offset[0],
            event.y - self._drag_offset[1],
        )
        d.global_x = round(gx)
        d.global_y = round(gy)
        self._snap(d)
        self._dirty = True
        self._btn_apply.config(state=tk.NORMAL)
        self._redraw()

    def _on_release(self, event):
        if self._drag_display:
            self._drag_display = None
            self.canvas.config(cursor="")
            self._redraw()

    def _snap(self, d: DisplayInfo):
        """Snap display edges to nearby edges of other displays."""
        threshold = SNAP_THRESHOLD / self._scale
        dr = d.global_x + d.width
        db = d.global_y + d.height

        best_dx, best_dy = float('inf'), float('inf')
        snap_x, snap_y = d.global_x, d.global_y

        for o in self.displays:
            if o is d:
                continue
            o_r = o.global_x + o.width
            o_b = o.global_y + o.height

            # Right → Left
            diff = abs(dr - o.global_x)
            if diff < abs(best_dx):
                best_dx = dr - o.global_x
                snap_x = o.global_x - d.width
            # Left → Right
            diff = abs(d.global_x - o_r)
            if diff < abs(best_dx):
                best_dx = d.global_x - o_r
                snap_x = o_r
            # Bottom → Top
            diff = abs(db - o.global_y)
            if diff < abs(best_dy):
                best_dy = db - o.global_y
                snap_y = o.global_y - d.height
            # Top → Bottom
            diff = abs(d.global_y - o_b)
            if diff < abs(best_dy):
                best_dy = d.global_y - o_b
                snap_y = o_b

            # Align tops
            diff = abs(d.global_y - o.global_y)
            if diff < threshold and diff < abs(best_dy):
                best_dy = d.global_y - o.global_y
                snap_y = o.global_y
            # Align bottoms
            diff = abs(db - o_b)
            if diff < threshold and diff < abs(best_dy):
                best_dy = db - o_b
                snap_y = o_b - d.height
            # Align lefts
            diff = abs(d.global_x - o.global_x)
            if diff < threshold and diff < abs(best_dx):
                best_dx = d.global_x - o.global_x
                snap_x = o.global_x
            # Align rights
            diff = abs(dr - o_r)
            if diff < threshold and diff < abs(best_dx):
                best_dx = dr - o_r
                snap_x = o_r - d.width

        if abs(best_dx) < threshold:
            d.global_x = round(snap_x)
        if abs(best_dy) < threshold:
            d.global_y = round(snap_y)

    # ── Actions ──────────────────────────────────────────────────

    def _do_identify(self):
        """Send identify request via the server API or protocol."""
        if not self._connected:
            messagebox.showwarning("Not Connected",
                                   "Not connected to the server.")
            return

        # Build identify messages for each machine
        by_machine: dict[str, list] = {}
        for d in self.displays:
            by_machine.setdefault(d.machine_id, []).append({
                "monitor_id": d.monitor_id,
                "number": d.number,
                "label": f"{d.machine_id} - {d.monitor_id}",
            })

        # We can't send IDENTIFY directly (we're a configurator client,
        # not the server). Instead, we'll use a special message.
        # For now, use HTTP API as fallback, or send a custom request.
        # Actually, let's add a simple HTTP call to trigger identify.
        import urllib.request
        try:
            # Try the web API if it's running
            req = urllib.request.Request(
                f"http://{self.server_host}:8080/api/identify",
                data=b"{}",
                headers={"Content-Type": "application/json"},
            )
            urllib.request.urlopen(req, timeout=2)
        except Exception:
            # Web UI might not be running; show on local displays only
            from .identify import show_identify
            screens = []
            try:
                from .backends import create_backend
                backend = create_backend()
                backend.open()
                local_screens = backend.query_monitors()
                backend.close()
                for i, s in enumerate(local_screens):
                    screens.append({
                        "x": s.x, "y": s.y,
                        "width": s.width, "height": s.height,
                        "number": i + 1, "label": "Local",
                    })
            except Exception:
                pass
            if screens:
                threading.Thread(
                    target=show_identify, args=(screens, 3), daemon=True,
                ).start()

    def _do_apply(self):
        """Push layout to server."""
        if not self._connected:
            messagebox.showwarning("Not Connected",
                                   "Not connected to the server.")
            return

        # Send layout via HTTP API
        layout = [
            {"machine_id": d.machine_id, "monitor_id": d.monitor_id,
             "global_x": d.global_x, "global_y": d.global_y}
            for d in self.displays
        ]

        import urllib.request
        try:
            req = urllib.request.Request(
                f"http://{self.server_host}:8080/api/layout",
                data=json.dumps({"displays": layout}).encode(),
                headers={"Content-Type": "application/json"},
            )
            urllib.request.urlopen(req, timeout=2)
            self._dirty = False
            self._btn_apply.config(state=tk.DISABLED)
            self.original_displays = [
                DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})
                for d in self.displays
            ]
        except Exception as e:
            messagebox.showerror("Error", f"Failed to apply layout:\n{e}")

    def _do_reset(self):
        """Reset to last-applied layout."""
        self.displays = [
            DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})
            for d in self.original_displays
        ]
        self._dirty = False
        self._btn_apply.config(state=tk.DISABLED)
        self._fit_view()
        self._redraw()

    # ── Run ──────────────────────────────────────────────────────

    def run(self):
        self._start_network()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()

    def _on_close(self):
        self._running = False
        if self.conn and not self.conn.closed:
            # Best-effort close
            try:
                loop = asyncio.new_event_loop()
                loop.run_until_complete(self.conn.close())
                loop.close()
            except Exception:
                pass
        self.root.destroy()


# ── CLI entry point ──────────────────────────────────────────────

def main():
    import argparse
    parser = argparse.ArgumentParser(
        description="Unified Desktop - Display Configuration",
    )
    parser.add_argument("--server", default="127.0.0.1",
                        help="Server hostname or IP")
    parser.add_argument("--port", type=int, default=24800,
                        help="Server TCP port")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    app = ConfiguratorApp(server_host=args.server, server_port=args.port)
    app.run()


if __name__ == "__main__":
    main()

"""Routing panel — patch bay for monitors & virtual displays.

Sibling of `layout_panel.LayoutPanel`. Where Layout's canvas is about
physical adjacency (where the cursor crosses), Routing's canvas is
about **what pixels go where**. The two views share the same LWW
state but render it with different metaphors:

  * Each PC shows as a **chassis**: a rounded frame containing a
    vertical stack of slot cards, one per monitor. Physical monitors
    are solid-filled; virtual displays are dashed. A dashed "+" slot
    at the bottom of every chassis creates a new virtual display.
  * Routes are **Bezier lines** between source slot → sink slot. The
    line is drawn in the source PC's colour, with an arrowhead on
    the sink end. Identity routes never draw — the canvas stays
    quiet when nothing is rewired.
  * Selecting a line turns it lilac; Delete / Backspace (or right-
    click → Cut) removes it.
  * Dragging a slot onto another slot patches a new route; dropping
    a slot onto itself or onto the same chassis clears the override.

Nothing here mutates layout. The Physical tab still owns monitor
positions for adjacency / cursor crossing; this tab leaves those
coordinates alone.
"""

from __future__ import annotations

import logging
import tkinter as tk
from dataclasses import dataclass
from typing import Callable, Optional

from .ui_theme import (
    FONT_SANS, LILAC, PAPER_BG, PAPER_BORDER, PAPER_MUTED,
    PAPER_SURFACE, PAPER_TEXT, SIZE_BASE, SIZE_LG, SIZE_SM, SIZE_XS,
    SPACE_LG, SPACE_MD, SPACE_SM, SPACE_XS,
)
from .layout_panel import _blend, machine_color

log = logging.getLogger(__name__)


CANVAS_BG = "#f8f9fc"


# Geometry of the patch-bay cards, all in CANVAS-PIXEL units (not
# global desktop coords). The routing view doesn't reuse the
# layout's scale/pan system — it's a schematic, not a to-scale map,
# so coordinates are just "which chassis at which column, which slot
# at which row within it". Keeps the view tidy even when a user has
# wildly different monitor sizes.
CHASSIS_W = 230
CHASSIS_HEADER = 44
SLOT_H = 70
SLOT_PAD = 10
CHASSIS_GAP = 80
TOP_MARGIN = 50
LEFT_MARGIN = 50
CHASSIS_BOTTOM_PAD = 14


@dataclass
class Slot:
    """One slot card inside a chassis. Carries the display info +
    the rectangle we rendered it at so hit testing can pair canvas
    coords back to the source/sink key."""
    key: str                   # "machine_id:monitor_id"
    machine_id: str
    monitor_id: str
    label: str
    virtual: bool
    # Filled on every redraw — canvas-space rectangle.
    rect: tuple[float, float, float, float] = (0, 0, 0, 0)


class RoutingPanel(tk.Frame):
    """Patch-bay canvas for routing + virtual displays."""

    def __init__(self, parent: tk.Widget, *,
                 on_reroute: Optional[Callable[[str, str], None]] = None,
                 on_add_virtual: Optional[Callable[[str], None]] = None,
                 on_remove_virtual: Optional[Callable[[str, str], None]] = None):
        super().__init__(parent, bg=PAPER_BG)
        self._on_reroute = on_reroute
        self._on_add_virtual = on_add_virtual
        self._on_remove_virtual = on_remove_virtual

        # Source-of-truth state. `physical` is a list of (machine_id,
        # monitor_id, label) tuples — one per real monitor. `virtual`
        # is the same shape for phantom monitors. Both get refreshed
        # in set_displays(); the panel computes chassis + slot
        # layout from them.
        self._physical: list[tuple[str, str, str]] = []
        self._virtual: list[tuple[str, str, str]] = []
        self._routes: dict[str, str] = {}

        # Layout cache filled during _redraw — used by hit testing to
        # match a canvas click back to a slot / + button / line
        # without re-running the chassis math.
        self._slots: dict[str, Slot] = {}
        self._add_slot_rects: dict[str, tuple[float, float, float, float]] = {}
        self._chassis_bounds: dict[str, tuple[float, float, float, float]] = {}

        self._drag_src: Optional[Slot] = None
        self._drag_cursor_xy: Optional[tuple[float, float]] = None
        self._drag_drop_slot: Optional[Slot] = None
        self._selected_route: Optional[str] = None

        self._build_ui()

    # ── UI construction ──────────────────────────────────────────

    def _build_ui(self) -> None:
        header = tk.Frame(self, bg=PAPER_BG)
        header.pack(fill=tk.X, padx=SPACE_LG, pady=(0, SPACE_SM))
        tk.Label(
            header,
            text="Patch a source onto a sink by dragging one slot "
                 "onto another. Click a line to select it, then "
                 "press Delete to cut. + adds a virtual display.",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
            wraplength=680, justify="left", anchor="w",
        ).pack(anchor="w")

        canvas_wrap = tk.Frame(self, bg=PAPER_BORDER, padx=1, pady=1)
        canvas_wrap.pack(fill=tk.BOTH, expand=True,
                         padx=SPACE_LG, pady=(0, SPACE_LG))
        self.canvas = tk.Canvas(canvas_wrap, bg=CANVAS_BG,
                                highlightthickness=0, bd=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.canvas.bind("<ButtonPress-3>", self._on_right_click)
        self.canvas.bind("<Configure>", lambda _e: self._redraw())
        # Delete the selected route line. Canvas needs focus for key
        # events — focus on click in _on_press.
        self.canvas.bind("<Key-Delete>", self._on_delete_key)
        self.canvas.bind("<Key-BackSpace>", self._on_delete_key)

        # Empty-state label covers the canvas while nothing's known.
        self._empty = tk.Label(
            self.canvas,
            text="Connect to a session to route monitors.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=CANVAS_BG,
        )
        self._empty.place(relx=0.5, rely=0.5, anchor="center")

    # ── Public API ───────────────────────────────────────────────

    def set_displays(self,
                     physical: list[dict],
                     virtual: list[dict]) -> None:
        """Replace the set of monitors shown on the patch bay.
        `physical` entries: {machine_id, monitor_id}. `virtual`
        entries same shape."""
        self._physical = [
            (str(m.get("machine_id") or ""),
             str(m.get("monitor_id") or ""),
             f"{m.get('machine_id')}:{m.get('monitor_id')}")
            for m in physical
            if m.get("machine_id") and m.get("monitor_id")
        ]
        self._virtual = [
            (str(m.get("machine_id") or ""),
             str(m.get("monitor_id") or ""),
             f"{m.get('machine_id')}:{m.get('monitor_id')}")
            for m in virtual
            if m.get("machine_id") and m.get("monitor_id")
        ]
        if self._physical or self._virtual:
            self._empty.place_forget()
        else:
            self._empty.place(relx=0.5, rely=0.5, anchor="center")
        self._redraw()

    def set_routes(self, routes: dict[str, str]) -> None:
        clean = {str(k): str(v) for k, v in (routes or {}).items()}
        if clean == self._routes:
            return
        self._routes = clean
        # Selected route no longer exists → drop the highlight.
        if (self._selected_route is not None
                and self._selected_route not in self._routes):
            self._selected_route = None
        self._redraw()

    # ── Chassis layout ───────────────────────────────────────────

    def _machines(self) -> list[str]:
        """Ordered list of every PC that owns at least one slot
        (physical or virtual). Stable alphabetical ordering so the
        chassis columns don't reshuffle when a peer reconnects."""
        seen: set[str] = set()
        for mid, _, _ in self._physical:
            seen.add(mid)
        for mid, _, _ in self._virtual:
            seen.add(mid)
        return sorted(seen)

    def _slots_for(self, machine_id: str) -> list[tuple[str, str, str, bool]]:
        """Return (mid, mon_id, label, virtual) tuples for every slot
        in the chassis, physicals first, then virtuals, preserving
        insertion order within each group."""
        phys = [(mid, mon, lbl, False)
                for mid, mon, lbl in self._physical
                if mid == machine_id]
        virt = [(mid, mon, lbl, True)
                for mid, mon, lbl in self._virtual
                if mid == machine_id]
        return phys + virt

    def _chassis_height(self, n_slots: int) -> float:
        # Header + one row per slot + one row for the + button.
        return (CHASSIS_HEADER
                + (n_slots + 1) * (SLOT_H + SLOT_PAD)
                + CHASSIS_BOTTOM_PAD)

    def _slot_rect(self, chassis_x: float, chassis_y: float,
                   slot_index: int) -> tuple[float, float, float, float]:
        x0 = chassis_x + SLOT_PAD
        y0 = chassis_y + CHASSIS_HEADER + slot_index * (SLOT_H + SLOT_PAD)
        return (x0, y0, x0 + CHASSIS_W - 2 * SLOT_PAD, y0 + SLOT_H)

    # ── Drawing ──────────────────────────────────────────────────

    def _redraw(self) -> None:
        c = self.canvas
        c.delete("all")
        self._slots.clear()
        self._add_slot_rects.clear()
        self._chassis_bounds.clear()

        machines = self._machines()
        if not machines:
            return

        x = LEFT_MARGIN
        y = TOP_MARGIN
        # Equal-column layout with per-chassis height so PCs with
        # many monitors don't squash the rest.
        for mid in machines:
            slots = self._slots_for(mid)
            chassis_h = self._chassis_height(len(slots))
            self._draw_chassis(mid, x, y, chassis_h, slots)
            x += CHASSIS_W + CHASSIS_GAP

        # Routing lines on top of chassis so the arrowheads read.
        self._draw_route_lines()
        # In-flight drag ghost line.
        if self._drag_src is not None and self._drag_cursor_xy is not None:
            self._draw_drag_ghost()

    def _draw_chassis(self, machine_id: str, x: float, y: float,
                      height: float, slots: list) -> None:
        color = machine_color(machine_id)
        # Background rectangle.
        bg = _blend(color, 0.08, bg_hex=CANVAS_BG)
        border = _blend(color, 0.45, bg_hex=CANVAS_BG)
        self.canvas.create_rectangle(
            x, y, x + CHASSIS_W, y + height,
            fill=bg, outline=border, width=2,
        )
        self._chassis_bounds[machine_id] = (x, y, x + CHASSIS_W, y + height)

        # Header: colour swatch + machine_id.
        self.canvas.create_oval(
            x + 14, y + 14, x + 26, y + 26,
            fill=color, outline="",
        )
        self.canvas.create_text(
            x + 34, y + 20,
            text=machine_id, anchor="w",
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fill=PAPER_TEXT,
        )

        for idx, (mid, mon, label, is_virtual) in enumerate(slots):
            rect = self._slot_rect(x, y, idx)
            slot = Slot(key=f"{mid}:{mon}", machine_id=mid,
                        monitor_id=mon, label=label,
                        virtual=is_virtual, rect=rect)
            self._slots[slot.key] = slot
            self._draw_slot(slot, color)

        # + affordance at the bottom.
        add_rect = self._slot_rect(x, y, len(slots))
        self._add_slot_rects[machine_id] = add_rect
        self._draw_add_slot(machine_id, add_rect, color)

    def _draw_slot(self, slot: Slot, color: str) -> None:
        x0, y0, x1, y1 = slot.rect
        sink_key = slot.key
        route_src = self._routes.get(sink_key)
        is_remote_sink = bool(route_src and route_src != sink_key
                              and not slot.virtual)
        is_projected = any(
            src == sink_key and sink != sink_key
            for sink, src in self._routes.items()
        )
        is_drop_target = (self._drag_drop_slot is slot
                          and slot is not self._drag_src)

        fill = _blend(color, 0.28 if is_drop_target else 0.18,
                      bg_hex=CANVAS_BG)
        outline = ("#4a9b6e" if is_drop_target
                   else _blend(color, 0.55, bg_hex=CANVAS_BG))
        dash = (6, 4) if slot.virtual else None

        self.canvas.create_rectangle(
            x0, y0, x1, y1,
            fill=fill, outline=outline, width=3,
            dash=dash,
        )

        # Primary label — the monitor id (HDMI-1 etc.) is the most
        # useful on-slot label; the machine id is already on the
        # chassis header. Keep it one line.
        self.canvas.create_text(
            (x0 + x1) / 2, (y0 + y1) / 2 - 8,
            text=slot.monitor_id,
            anchor="center",
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fill=PAPER_TEXT,
        )
        # Subtitle underneath: role / state.
        if slot.virtual:
            sub = "virtual display"
        elif is_remote_sink:
            sub = f"← {route_src.split(':', 1)[0]}"
        elif is_projected:
            for sink, src in self._routes.items():
                if src == sink_key and sink != sink_key:
                    sub = f"→ {sink.split(':', 1)[0]}"
                    break
            else:
                sub = "projected"
        else:
            sub = "native"
        self.canvas.create_text(
            (x0 + x1) / 2, (y0 + y1) / 2 + 12,
            text=sub, anchor="center",
            font=(FONT_SANS, SIZE_SM),
            fill=PAPER_MUTED,
        )

    def _draw_add_slot(self, machine_id: str,
                       rect: tuple[float, float, float, float],
                       color: str) -> None:
        x0, y0, x1, y1 = rect
        fill = _blend(color, 0.08, bg_hex=CANVAS_BG)
        border = _blend(color, 0.45, bg_hex=CANVAS_BG)
        self.canvas.create_rectangle(
            x0, y0, x1, y1,
            fill=fill, outline=border, width=2, dash=(3, 3),
            tags=("add-slot", f"add-slot:{machine_id}"),
        )
        cx = (x0 + x1) / 2
        cy = (y0 + y1) / 2
        self.canvas.create_text(
            cx, cy - 8,
            text="+", anchor="center",
            font=(FONT_SANS, SIZE_LG + 10, "bold"),
            fill=_blend(color, 0.7, bg_hex=CANVAS_BG),
            tags=("add-slot", f"add-slot:{machine_id}"),
        )
        self.canvas.create_text(
            cx, cy + 16,
            text="Add virtual display",
            anchor="center",
            font=(FONT_SANS, SIZE_XS),
            fill=PAPER_MUTED,
            tags=("add-slot", f"add-slot:{machine_id}"),
        )

    def _draw_route_lines(self) -> None:
        for sink_key, src_key in self._routes.items():
            if sink_key == src_key:
                continue
            sink = self._slots.get(sink_key)
            src = self._slots.get(src_key)
            if sink is None or src is None:
                continue
            self._draw_route_line(src, sink,
                                  selected=(sink_key == self._selected_route))

    def _draw_route_line(self, src: Slot, sink: Slot,
                         selected: bool) -> None:
        """Bezier curve from the right edge of `src` to the left edge
        of `sink` (or vice versa when they stack the other way)."""
        sx0, sy0, sx1, sy1 = src.rect
        tx0, ty0, tx1, ty1 = sink.rect
        # Anchor on the horizontal edge that faces the target.
        if (sx0 + sx1) / 2 < (tx0 + tx1) / 2:
            start = (sx1, (sy0 + sy1) / 2)
            end = (tx0, (ty0 + ty1) / 2)
            dx = 60
        else:
            start = (sx0, (sy0 + sy1) / 2)
            end = (tx1, (ty0 + ty1) / 2)
            dx = -60
        # Cubic control points push out horizontally so the curve
        # bows cleanly between chassis columns.
        c1 = (start[0] + dx, start[1])
        c2 = (end[0] - dx, end[1])

        color = machine_color(src.machine_id)
        width = 5 if selected else 3
        tag = f"route:{sink.key}"
        # Soft shadow for legibility against chassis fills.
        self.canvas.create_line(
            start[0] + 1, start[1] + 1,
            c1[0] + 1, c1[1] + 1,
            c2[0] + 1, c2[1] + 1,
            end[0] + 1, end[1] + 1,
            smooth=True, splinesteps=32,
            fill="#1f2340", width=width + 2,
            tags=("route-line", tag),
        )
        stroke = LILAC if selected else color
        self.canvas.create_line(
            start[0], start[1],
            c1[0], c1[1],
            c2[0], c2[1],
            end[0], end[1],
            smooth=True, splinesteps=32,
            fill=stroke, width=width,
            arrow=tk.LAST, arrowshape=(14, 16, 6),
            tags=("route-line", tag),
        )

    def _draw_drag_ghost(self) -> None:
        src = self._drag_src
        if src is None or self._drag_cursor_xy is None:
            return
        sx = (src.rect[0] + src.rect[2]) / 2
        sy = (src.rect[1] + src.rect[3]) / 2
        ex, ey = self._drag_cursor_xy
        color = machine_color(src.machine_id)
        dx = (ex - sx) / 2
        c1 = (sx + dx, sy)
        c2 = (ex - dx, ey)
        self.canvas.create_line(
            sx, sy, c1[0], c1[1], c2[0], c2[1], ex, ey,
            smooth=True, splinesteps=24,
            fill=color, width=3, dash=(6, 4),
        )

    # ── Hit testing ──────────────────────────────────────────────

    def _hit_slot(self, sx: float, sy: float) -> Optional[Slot]:
        for slot in self._slots.values():
            x0, y0, x1, y1 = slot.rect
            if x0 <= sx <= x1 and y0 <= sy <= y1:
                return slot
        return None

    def _hit_add_slot(self, sx: float, sy: float) -> Optional[str]:
        for mid, (x0, y0, x1, y1) in self._add_slot_rects.items():
            if x0 <= sx <= x1 and y0 <= sy <= y1:
                return mid
        return None

    def _hit_route_line(self, sx: float, sy: float) -> Optional[str]:
        items = self.canvas.find_overlapping(sx - 6, sy - 6,
                                             sx + 6, sy + 6)
        for item in reversed(items):
            for t in self.canvas.gettags(item):
                if t.startswith("route:"):
                    return t[len("route:"):]
        return None

    # ── Event handlers ───────────────────────────────────────────

    def _on_press(self, event):
        try:
            self.canvas.focus_set()
        except tk.TclError:
            pass

        add_mid = self._hit_add_slot(event.x, event.y)
        if add_mid is not None:
            if self._on_add_virtual is not None:
                self._on_add_virtual(add_mid)
            return

        slot = self._hit_slot(event.x, event.y)
        if slot is not None:
            # Starting a drag from a slot: it becomes the source of
            # the in-progress patch line.
            self._drag_src = slot
            self._drag_cursor_xy = (event.x, event.y)
            self._drag_drop_slot = None
            self.canvas.config(cursor="crosshair")
            if self._selected_route is not None:
                self._selected_route = None
            self._redraw()
            return

        # Route line?
        route_key = self._hit_route_line(event.x, event.y)
        if route_key is not None:
            self._selected_route = route_key
            self._redraw()
            return

        # Empty space — deselect.
        if self._selected_route is not None:
            self._selected_route = None
            self._redraw()

    def _on_drag(self, event):
        if self._drag_src is None:
            return
        self._drag_cursor_xy = (event.x, event.y)
        hover = self._hit_slot(event.x, event.y)
        if hover is self._drag_src:
            hover = None
        self._drag_drop_slot = hover
        self._redraw()

    def _on_release(self, event):
        src = self._drag_src
        self._drag_src = None
        self._drag_cursor_xy = None
        drop = self._drag_drop_slot
        self._drag_drop_slot = None
        self.canvas.config(cursor="")
        self._redraw()
        if src is None:
            return
        # Drop onto another slot = patch. src is the SOURCE (the PC
        # contributing pixels); drop is the SINK (the monitor that
        # will display them). Virtual-as-sink is meaningless — skip.
        if drop is None or drop.virtual:
            return
        if drop.key == src.key:
            return
        if self._on_reroute is not None:
            self._on_reroute(drop.key, src.key)

    def _on_delete_key(self, _event=None):
        sink_key = self._selected_route
        if not sink_key:
            return
        self._selected_route = None
        if self._on_reroute is not None:
            # Identity route = "no override". set_route treats this
            # as a delete, keeping the data-model side clean.
            self._on_reroute(sink_key, sink_key)
        self._redraw()

    def _on_right_click(self, event):
        route_key = self._hit_route_line(event.x, event.y)
        if route_key is not None:
            src = self._routes.get(route_key, "")
            menu = tk.Menu(self.canvas, tearoff=0,
                           bg=PAPER_BG, fg=PAPER_TEXT,
                           activebackground=LILAC,
                           activeforeground="#ffffff",
                           bd=1, relief=tk.SOLID)
            menu.add_command(
                label=f"{route_key.split(':', 1)[0]} ← "
                      f"{src.split(':', 1)[0]}"
                      if src else route_key,
                state=tk.DISABLED,
            )
            menu.add_separator()
            menu.add_command(
                label="Cut route",
                command=lambda k=route_key:
                    self._on_reroute(k, k) if self._on_reroute else None,
            )
            try:
                menu.tk_popup(event.x_root, event.y_root)
            finally:
                menu.grab_release()
            return

        slot = self._hit_slot(event.x, event.y)
        if slot is None:
            return
        menu = tk.Menu(self.canvas, tearoff=0,
                       bg=PAPER_BG, fg=PAPER_TEXT,
                       activebackground=LILAC,
                       activeforeground="#ffffff",
                       bd=1, relief=tk.SOLID)
        if slot.virtual:
            menu.add_command(label=f"Virtual · {slot.key}",
                             state=tk.DISABLED)
            menu.add_separator()
            if self._on_remove_virtual is not None:
                menu.add_command(
                    label="Remove virtual display",
                    command=lambda s=slot:
                        self._on_remove_virtual(s.machine_id,
                                                s.monitor_id),
                )
        else:
            menu.add_command(label=f"Show on {slot.key}",
                             state=tk.DISABLED)
            menu.add_separator()
            sink_key = slot.key
            current = self._routes.get(sink_key, sink_key)
            menu.add_command(
                label=("✓ " if current == sink_key else "   ")
                      + f"{slot.monitor_id} (native)",
                command=lambda k=sink_key:
                    self._on_reroute(k, k) if self._on_reroute else None,
            )
            for s in self._slots.values():
                if s.key == sink_key:
                    continue
                menu.add_command(
                    label=("✓ " if current == s.key else "   ") + s.key,
                    command=lambda k=sink_key, v=s.key:
                        self._on_reroute(k, v) if self._on_reroute else None,
                )
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

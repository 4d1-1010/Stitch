"""Two-row Layout canvas: PCs on top, physical displays on bottom.

The feature this panel drives is "show one PC's display on another
PC's display" — nothing more. No virtual displays, no hubs, no
extensions. Every physical panel in the mesh has exactly one PC
output driving it at any time. Editing the layout is exclusively
about WHICH PC output drives WHICH panel.

Model invariants enforced by the canvas:

  * total PC outputs == total physical displays
  * every display has exactly one incoming line
  * every PC output has exactly one outgoing line
  * default: each PC's k-th output drives its own k-th display
    ("identity")

Because total outputs and total displays are always equal, edits
are **swaps** — moving one line to a different sink automatically
pushes the displaced line back to the drag's origin. No cut, no
delete, no orphaned state.

Gestures:

  * Drag a physical rectangle in the bottom area → rearrange
    adjacency (what the cursor-crossing logic uses).
  * Drag a physical rectangle ONTO another physical rectangle with
    the right-click / alt-drag modifier → swap their sources.
  * Right-click a physical → "Show source from …" menu for
    keyboard-free swap.

Everything is pending until Apply.
"""

from __future__ import annotations

import colorsys
import logging
import tkinter as tk
import zlib
from dataclasses import dataclass
from typing import Callable, Optional

from .ui_theme import (
    FONT_SANS, LILAC, PAPER_BG, PAPER_BORDER, PAPER_MUTED, PAPER_SURFACE,
    PAPER_TEXT, SIZE_BASE, SIZE_LG, SIZE_SM, SIZE_XS,
    SPACE_LG, SPACE_MD, SPACE_SM, SPACE_XS,
    PillButton,
)


log = logging.getLogger(__name__)


# Procedural per-machine colour — same as before so colours stay
# consistent with Activity / Workspace UI.
_LILAC_HUE = 248.0 / 360.0
_HUE_SPAN = 60.0 / 360.0
_SAT_MIN, _SAT_MAX = 0.28, 0.62
_LIGHT_MIN, _LIGHT_MAX = 0.58, 0.80


def machine_color(machine_id: str) -> str:
    seed = zlib.crc32(machine_id.encode("utf-8")) & 0xFFFFFFFF
    h_part = ((seed >> 20) & 0xFFF) / 0xFFF
    s_part = ((seed >> 10) & 0x3FF) / 0x3FF
    l_part = (seed & 0x3FF) / 0x3FF
    hue = (_LILAC_HUE + (h_part - 0.5) * _HUE_SPAN) % 1.0
    sat = _SAT_MIN + s_part * (_SAT_MAX - _SAT_MIN)
    light = _LIGHT_MIN + l_part * (_LIGHT_MAX - _LIGHT_MIN)
    r, g, b = colorsys.hls_to_rgb(hue, light, sat)
    return f"#{int(r * 255):02x}{int(g * 255):02x}{int(b * 255):02x}"


SNAP_THRESHOLD = 20
CANVAS_BG = "#f8f9fc"


@dataclass
class DisplayInfo:
    """Physical display in the workspace. The `virtual` field is kept
    for dormant wire-format compatibility but never set True in the
    shipping build."""
    machine_id: str
    monitor_id: str
    global_x: int
    global_y: int
    width: int
    height: int
    number: int = 0
    virtual: bool = False


def _blend(fg_hex: str, alpha: float, bg_hex: str = CANVAS_BG) -> str:
    fg = bytes.fromhex(fg_hex.lstrip("#"))
    bg = bytes.fromhex(bg_hex.lstrip("#"))
    mixed = tuple(
        int(round(f * alpha + b * (1 - alpha)))
        for f, b in zip(fg, bg)
    )
    return f"#{mixed[0]:02x}{mixed[1]:02x}{mixed[2]:02x}"


# Band geometry. Canvas-local coords, unaffected by pan / zoom.
TOP_BAND_H = 84
TOP_BAND_Y = 16
BOTTOM_BAND_Y = TOP_BAND_Y + TOP_BAND_H + 32   # top + gap for labels

PC_NODE_W = 200
PC_NODE_H = 56
NODE_GAP = 28
BAND_PAD_X = 40

LINE_PICK_PX = 8

BOTTOM_SCALE_DEFAULT = 0.12


@dataclass
class _Node:
    """One hit-testable entry on the canvas."""
    kind: str              # "pc" | "physical"
    key: str               # "pc:<mid>" or "phys:<mid>:<mon>"
    rect: tuple[float, float, float, float]
    machine_id: str = ""
    monitor_id: str = ""
    display: Optional[DisplayInfo] = None


class LayoutPanel(tk.Frame):
    """Two-row canvas: PCs on top, physical displays on bottom.

    Public shape preserved so the shell keeps working with the same
    constructor signature and methods.
    """

    def __init__(self, parent: tk.Widget, *,
                 on_apply: Optional[Callable[[list[dict]], None]] = None,
                 on_identify: Optional[Callable[[], None]] = None,
                 on_reroute: Optional[Callable[[str, str], None]] = None,
                 # Deprecated callbacks — accepted for API compatibility
                 # but never invoked in this build. Will be removed or
                 # re-wired when virtual displays ship in a later unIO.
                 on_add_virtual: Optional[Callable[[str], None]] = None,
                 on_remove_virtual: Optional[Callable[[str, str], None]] = None,
                 on_add_hub: Optional[Callable[[], None]] = None,
                 on_remove_hub: Optional[Callable[[str], None]] = None,
                 virtuals_provider: Optional[Callable[[], list[dict]]] = None,
                 hubs_provider: Optional[Callable[[], list[dict]]] = None,
                 sources_provider: Optional[Callable[[], list[dict]]] = None):
        super().__init__(parent, bg=PAPER_BG)
        self._on_apply = on_apply
        self._on_identify = on_identify
        self._on_reroute = on_reroute
        self._sources_provider = sources_provider

        self.displays: list[DisplayInfo] = []
        self.original_displays: list[DisplayInfo] = []
        self._active_machine: str = ""

        # Committed routes (from LWW) and pending overrides (edits
        # staged until Apply). `_effective_routes()` merges them for
        # rendering.
        self._committed_routes: dict[str, str] = {}
        self._pending_routes: dict[str, str] = {}

        # Per-redraw hit-test cache.
        self._nodes: list[_Node] = []

        # Bottom-area pan / zoom for adjacency editing.
        self._scale = BOTTOM_SCALE_DEFAULT
        self._pan_x = 0.0
        self._pan_y = 0.0

        # Drag state.
        self._drag_physical: Optional[DisplayInfo] = None
        self._drag_offset = (0, 0)
        # Line-drag state: user grabbed the line at its display-end
        # and is moving it across the canvas. Drop on another display
        # = swap sources. Drop elsewhere = abort.
        self._line_drag_origin: Optional[DisplayInfo] = None
        self._line_drag_xy: Optional[tuple[int, int]] = None
        # Shift-drag also supports swap (keeps the older gesture in).
        self._swap_drag_from: Optional[DisplayInfo] = None
        self._swap_drop_target: Optional[DisplayInfo] = None
        self._pan_start: Optional[tuple[int, int, float, float]] = None

        self._dirty = False

        self._build_ui()

    # ── Compatibility shims ──────────────────────────────────────

    def set_mode(self, _mode: str) -> None:
        """No-op — this canvas has no mode toggle. Kept so older
        shell code can call it unconditionally."""

    # ── UI scaffolding ───────────────────────────────────────────

    def _build_ui(self) -> None:
        header = tk.Frame(self, bg=PAPER_BG)
        header.pack(fill=tk.X, padx=SPACE_LG, pady=(0, SPACE_SM))
        tk.Label(
            header,
            text="Top row: your PCs. Bottom row: every physical "
                 "display. Grab a line and drop it on another "
                 "display to swap which PC drives it. Drag a "
                 "display rectangle to rearrange adjacency.",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
            wraplength=720, justify="left", anchor="w",
        ).pack(anchor="w")

        wrap = tk.Frame(self, bg=PAPER_BORDER, padx=1, pady=1)
        wrap.pack(fill=tk.BOTH, expand=True,
                  padx=SPACE_LG, pady=(0, SPACE_MD))
        self.canvas = tk.Canvas(wrap, bg=CANVAS_BG,
                                highlightthickness=0, bd=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        self.canvas.bind("<ButtonPress-3>", self._on_right_click)
        # Alt/Shift-drag starts a swap gesture (drop on another
        # physical to swap sources). Without modifier, left-drag is
        # adjacency movement.
        self.canvas.bind("<Shift-ButtonPress-1>", self._on_swap_press)
        self.canvas.bind("<Shift-B1-Motion>", self._on_swap_drag)
        self.canvas.bind("<Shift-ButtonRelease-1>", self._on_swap_release)
        # Middle-click pan.
        self.canvas.bind("<ButtonPress-2>", self._on_pan_press)
        self.canvas.bind("<B2-Motion>", self._on_pan_motion)
        self.canvas.bind("<ButtonRelease-2>", self._on_pan_release)
        self.canvas.bind("<Configure>",
                         lambda e: self._on_canvas_configure())
        self.canvas.bind("<Enter>", self._on_canvas_enter)
        self.canvas.bind("<Leave>", self._on_canvas_leave)

        self._empty = tk.Label(
            self.canvas,
            text="Connect to a session to see your displays here.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=CANVAS_BG,
        )
        self._empty.place(relx=0.5, rely=0.5, anchor="center")

        actions = tk.Frame(self, bg=PAPER_BG)
        actions.pack(fill=tk.X, padx=SPACE_LG, pady=(0, SPACE_LG))

        self._btn_identify = PillButton(
            actions, "Identify displays",
            variant="secondary", command=self._do_identify,
        )
        self._btn_identify.pack(side=tk.LEFT)
        self._btn_apply = PillButton(
            actions, "Apply layout",
            variant="primary", command=self._do_apply,
        )
        self._btn_apply.pack(side=tk.LEFT, padx=SPACE_SM)
        self._btn_reset = PillButton(
            actions, "Reset",
            variant="secondary", command=self._do_reset,
        )
        self._btn_reset.pack(side=tk.LEFT)

        self._status_label = tk.Label(
            actions, text="", anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        )
        self._status_label.pack(side=tk.LEFT, padx=(SPACE_MD, 0))

        zoom_row = tk.Frame(actions, bg=PAPER_BG)
        zoom_row.pack(side=tk.RIGHT)
        self._btn_zoom_out = PillButton(
            zoom_row, "−", variant="ghost",
            command=lambda: self._zoom_centered(1 / 1.2),
        )
        self._btn_zoom_out.pack(side=tk.LEFT, padx=(0, SPACE_XS))
        self._btn_zoom_fit = PillButton(
            zoom_row, "Fit", variant="ghost",
            command=self._fit_view,
        )
        self._btn_zoom_fit.pack(side=tk.LEFT, padx=(0, SPACE_XS))
        self._btn_zoom_in = PillButton(
            zoom_row, "+", variant="ghost",
            command=lambda: self._zoom_centered(1.2),
        )
        self._btn_zoom_in.pack(side=tk.LEFT)

        self._set_session_controls_enabled(False)

    # ── Public API ───────────────────────────────────────────────

    def set_displays(self, monitors: list[dict]) -> None:
        self.displays = [
            DisplayInfo(
                machine_id=m["machine_id"],
                monitor_id=m["monitor_id"],
                global_x=int(m.get("global_x", 0)),
                global_y=int(m.get("global_y", 0)),
                width=int(m.get("width", 0)),
                height=int(m.get("height", 0)),
            )
            for m in monitors
            if not str(m.get("machine_id", "")).startswith("__")
        ]
        _number_displays(self.displays)
        if not self._dirty:
            self.original_displays = [_copy(d) for d in self.displays]
        self._set_session_controls_enabled(bool(self.displays))
        if self.displays:
            self._empty.place_forget()
        else:
            self._empty.place(relx=0.5, rely=0.5, anchor="center")
        self._fit_view()

    def set_routes(self, routes: dict[str, str]) -> None:
        clean = {str(k): str(v) for k, v in (routes or {}).items()}
        if clean == self._committed_routes:
            return
        self._committed_routes = clean
        self._redraw()
        self._refresh_status()

    def set_active_machine(self, machine_id: str) -> None:
        if machine_id == self._active_machine:
            return
        self._active_machine = machine_id
        self._redraw()

    def mark_clean(self) -> None:
        self.original_displays = [_copy(d) for d in self.displays]
        self._dirty = False
        self._set_apply_enabled(False)
        self._refresh_status()

    # ── Effective state helpers ──────────────────────────────────

    def _effective_routes(self) -> dict[str, str]:
        merged = dict(self._committed_routes)
        merged.update(self._pending_routes)
        return merged

    def _has_pending_edits(self) -> bool:
        return bool(self._dirty or self._pending_routes)

    def _machines(self) -> list[str]:
        seen: set[str] = set()
        for d in self.displays:
            if d.machine_id:
                seen.add(d.machine_id)
        return sorted(seen)

    def _display_by_key(self, key: str) -> Optional[DisplayInfo]:
        for d in self.displays:
            if f"{d.machine_id}:{d.monitor_id}" == key:
                return d
        return None

    # ── Button states ────────────────────────────────────────────

    def _set_session_controls_enabled(self, enabled: bool) -> None:
        for btn in (self._btn_identify, self._btn_reset):
            self._set_button_enabled(btn, enabled, active="secondary")
        for btn in (self._btn_zoom_in, self._btn_zoom_out,
                    self._btn_zoom_fit):
            self._set_button_enabled(btn, enabled, active="ghost")
        if not enabled:
            self._set_apply_enabled(False)

    def _set_apply_enabled(self, enabled: bool) -> None:
        self._set_button_enabled(self._btn_apply, enabled, active="primary")

    def _set_button_enabled(self, btn: PillButton, enabled: bool,
                            active: str = "primary") -> None:
        palettes = {
            "primary": ("#ffffff", LILAC),
            "ghost":   (PAPER_TEXT, PAPER_BG),
            "secondary": (PAPER_TEXT, PAPER_SURFACE),
        }
        fg, bg = palettes.get(active, palettes["primary"])
        if enabled:
            btn._bg, btn._fg = bg, fg
            btn.configure(fg=fg, bg=bg, cursor="hand2")
            btn._enabled = True
        else:
            btn._bg, btn._fg = PAPER_SURFACE, PAPER_TEXT
            btn.configure(fg=PAPER_TEXT, bg=PAPER_SURFACE, cursor="arrow")
            btn._enabled = False

    # ── Status line ──────────────────────────────────────────────

    def _refresh_status(self) -> None:
        if not hasattr(self, "_status_label"):
            return
        if not self._has_pending_edits():
            self._status_label.configure(text="", fg=PAPER_MUTED)
            return
        counts: list[str] = []
        moved_count = 0
        orig_by_key = {(d.machine_id, d.monitor_id): d
                       for d in self.original_displays}
        for d in self.displays:
            base = orig_by_key.get((d.machine_id, d.monitor_id))
            if base is None:
                continue
            if base.global_x != d.global_x or base.global_y != d.global_y:
                moved_count += 1
        if moved_count:
            counts.append(f"{moved_count} display"
                          f"{'' if moved_count == 1 else 's'} moved")
        if self._pending_routes:
            # Route swaps come in pairs — divide by two for display.
            pair_count = max(1, len(self._pending_routes) // 2)
            counts.append(f"{pair_count} source swap"
                          f"{'' if pair_count == 1 else 's'}")
        if counts:
            self._status_label.configure(
                text=" · ".join(counts) + "  — press Apply to commit",
                fg=LILAC,
            )
        else:
            self._status_label.configure(text="", fg=PAPER_MUTED)

    # ── Drawing ──────────────────────────────────────────────────

    def _redraw(self) -> None:
        c = self.canvas
        c.delete("all")
        self._nodes.clear()

        cw = c.winfo_width()
        ch = c.winfo_height()
        if cw < 50 or ch < 50:
            return

        # Row separator + labels.
        sep_y = BOTTOM_BAND_Y - 12
        c.create_line(20, sep_y, cw - 20, sep_y,
                      fill=PAPER_BORDER, width=1, dash=(2, 3))
        c.create_text(18, TOP_BAND_Y + TOP_BAND_H / 2,
                      text="PCs", anchor="w",
                      font=(FONT_SANS, SIZE_XS, "bold"),
                      fill=PAPER_MUTED)
        c.create_text(18, BOTTOM_BAND_Y - 4,
                      text="Physical displays", anchor="w",
                      font=(FONT_SANS, SIZE_XS, "bold"),
                      fill=PAPER_MUTED)

        self._draw_top_band(cw)
        self._draw_bottom_area(cw, ch)
        self._draw_lines()
        self._draw_drag_ghost()

    def _draw_top_band(self, cw: int) -> None:
        machines = self._machines()
        if not machines:
            return
        total_w = (len(machines) * PC_NODE_W
                   + max(0, len(machines) - 1) * NODE_GAP)
        x = max(BAND_PAD_X + 70, (cw - total_w) // 2)
        y = TOP_BAND_Y
        for mid in machines:
            color = machine_color(mid)
            rect = (x, y, x + PC_NODE_W, y + PC_NODE_H)
            self._nodes.append(_Node(
                kind="pc", key=f"pc:{mid}",
                rect=rect, machine_id=mid))
            fill = _blend(color, 0.22)
            active = (mid == self._active_machine)
            border = LILAC if active else color
            self.canvas.create_rectangle(
                *rect, fill=fill, outline=border, width=3,
            )
            self.canvas.create_oval(
                x + 14, y + PC_NODE_H / 2 - 8,
                x + 30, y + PC_NODE_H / 2 + 8,
                fill=color, outline="",
            )
            self.canvas.create_text(
                x + 40, y + PC_NODE_H / 2,
                text=mid, anchor="w",
                font=(FONT_SANS, SIZE_BASE, "bold"),
                fill=PAPER_TEXT,
            )
            x += PC_NODE_W + NODE_GAP

    def _draw_bottom_area(self, cw: int, ch: int) -> None:
        if not self.displays:
            return
        grid_y0 = BOTTOM_BAND_Y + 20
        step = 120
        for i in range(0, max(cw, 800), step):
            self.canvas.create_line(
                i, grid_y0, i, ch - 10,
                fill="#edeff4", width=1,
            )
        for j in range(grid_y0, ch - 10, step):
            self.canvas.create_line(
                20, j, cw - 20, j,
                fill="#edeff4", width=1,
            )

        effective = self._effective_routes()
        for d in self.displays:
            sx, sy = self._to_screen(d.global_x, d.global_y)
            sw = d.width * self._scale
            sh = d.height * self._scale
            if sy < grid_y0:
                sy = grid_y0
            color = machine_color(d.machine_id)
            is_active = (d.machine_id == self._active_machine
                         and self._active_machine != "")
            is_dragging = (d is self._drag_physical
                           or d is self._swap_drag_from)
            is_drop_target = (d is self._swap_drop_target)

            # Highlight the display's CURRENT SOURCE machine so users
            # can see at a glance if a panel is being driven by
            # somebody else.
            sink_key = f"{d.machine_id}:{d.monitor_id}"
            source_key = effective.get(sink_key, sink_key)
            source_mid = source_key.partition(":")[0]
            source_color = machine_color(source_mid) \
                if source_mid else color

            fill = _blend(source_color,
                          0.34 if is_active
                          else (0.28 if is_drop_target
                                else (0.26 if is_dragging else 0.18)))
            outline = "#4a9b6e" if is_drop_target else source_color
            border = 4 if (is_active or is_dragging or is_drop_target) else 3
            self.canvas.create_rectangle(
                sx, sy, sx + sw, sy + sh,
                fill=fill, outline=outline, width=border,
            )

            cx = sx + sw / 2
            cy = sy + sh / 2
            num_size = max(10, min(36, int(min(sw, sh) * 0.22)))
            lbl_size = max(7, min(12, int(sh * 0.09)))
            gap = 4
            total_h = num_size + gap + lbl_size
            self.canvas.create_text(
                cx, cy - total_h / 2 + num_size / 2,
                text=str(d.number),
                anchor="center",
                font=(FONT_SANS, num_size, "bold"),
                fill=PAPER_TEXT,
            )
            if sh > 34 and sw > 60:
                # Label reads: "machine:monitor" if identity;
                # "machine:monitor ← PC-X" if this panel is driven by
                # a different PC. Matches the mental model — the
                # panel belongs to a PC, and its CURRENT content
                # comes from whichever PC is wired to it.
                label = f"{d.machine_id}:{d.monitor_id}"
                if source_mid and source_mid != d.machine_id:
                    label += f"  ← {source_mid}"
                self.canvas.create_text(
                    cx, cy + total_h / 2 - lbl_size / 2,
                    text=label,
                    anchor="center",
                    font=(FONT_SANS, lbl_size),
                    fill=PAPER_MUTED,
                )

            self._nodes.append(_Node(
                kind="physical",
                key=f"phys:{d.machine_id}:{d.monitor_id}",
                rect=(sx, sy, sx + sw, sy + sh),
                machine_id=d.machine_id,
                monitor_id=d.monitor_id, display=d,
            ))

    def _draw_lines(self) -> None:
        node_map = {n.key: n for n in self._nodes}
        effective = self._effective_routes()
        for d in self.displays:
            sink_key = f"{d.machine_id}:{d.monitor_id}"
            source_key = effective.get(sink_key, sink_key)
            src_mid = source_key.partition(":")[0]
            sink_node = node_map.get(f"phys:{sink_key}")
            src_pc = node_map.get(f"pc:{src_mid}")
            if sink_node is None or src_pc is None:
                continue
            self._draw_bezier(src_pc, sink_node,
                              sink_key=sink_key,
                              source_mid=src_mid)

    def _draw_bezier(self, src: _Node, dst: _Node,
                     sink_key: str, source_mid: str) -> None:
        sx0 = (src.rect[0] + src.rect[2]) / 2
        sy0 = src.rect[3]
        sx1 = (dst.rect[0] + dst.rect[2]) / 2
        sy1 = dst.rect[1]
        dy = sy1 - sy0
        c1 = (sx0, sy0 + dy * 0.5)
        c2 = (sx1, sy1 - dy * 0.5)
        color = machine_color(source_mid) if source_mid else PAPER_MUTED
        # Non-identity routes get a bolder stroke so re-routed panels
        # are visually distinct even before the user reads the label.
        sink_mid = sink_key.partition(":")[0]
        is_identity = (source_mid == sink_mid)
        width = 2 if is_identity else 4
        stroke = color if is_identity else _blend(color, 0.85, bg_hex=PAPER_TEXT)
        tag = f"route:{sink_key}"
        # Shadow stroke so the line reads against the grid.
        self.canvas.create_line(
            sx0 + 1, sy0 + 1, c1[0] + 1, c1[1] + 1,
            c2[0] + 1, c2[1] + 1, sx1 + 1, sy1 + 1,
            smooth=True, splinesteps=32,
            fill="#1f2340", width=width + 2,
            tags=("bezier", tag),
        )
        self.canvas.create_line(
            sx0, sy0, c1[0], c1[1], c2[0], c2[1], sx1, sy1,
            smooth=True, splinesteps=32,
            fill=stroke, width=width,
            arrow=tk.LAST, arrowshape=(14, 16, 6),
            tags=("bezier", tag),
        )

    def _draw_drag_ghost(self) -> None:
        """Lilac preview line that follows the cursor during a
        line-drag or PC-drag. Gives users immediate feedback that a
        reroute is in flight; the real Bezier for the in-flight
        route stays visible underneath so the user can see the
        "from" end of the line."""
        if self._line_drag_origin is None or self._line_drag_xy is None:
            return
        origin = self._line_drag_origin
        node_map = {n.key: n for n in self._nodes}
        origin_node = node_map.get(
            f"phys:{origin.machine_id}:{origin.monitor_id}")
        if origin_node is None:
            return
        effective = self._effective_routes()
        sink_key = f"{origin.machine_id}:{origin.monitor_id}"
        src_key = effective.get(sink_key, sink_key)
        src_mid = src_key.partition(":")[0]
        src_pc = node_map.get(f"pc:{src_mid}")
        if src_pc is None:
            return
        # Start from the PC that owns this line; end at the cursor.
        sx0 = (src_pc.rect[0] + src_pc.rect[2]) / 2
        sy0 = src_pc.rect[3]
        ex, ey = self._line_drag_xy
        dy = ey - sy0
        c1 = (sx0, sy0 + dy * 0.5)
        c2 = (ex, ey - dy * 0.5)
        color = machine_color(src_mid) if src_mid else LILAC
        self.canvas.create_line(
            sx0, sy0, c1[0], c1[1], c2[0], c2[1], ex, ey,
            smooth=True, splinesteps=24,
            fill=color, width=3, dash=(6, 4),
        )

    # ── Canvas transform (bottom area only) ──────────────────────

    def _to_screen(self, gx: float, gy: float) -> tuple[float, float]:
        return (gx * self._scale + self._pan_x,
                gy * self._scale + self._pan_y + BOTTOM_BAND_Y + 40)

    def _to_global(self, sx: float, sy: float) -> tuple[float, float]:
        return ((sx - self._pan_x) / self._scale,
                (sy - self._pan_y - BOTTOM_BAND_Y - 40) / self._scale)

    def _on_canvas_configure(self) -> None:
        if self.displays:
            self._fit_view()
        else:
            self._redraw()

    def _fit_view(self) -> None:
        if not self.displays:
            self._redraw()
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        if cw < 50 or ch < 50:
            self.after(50, self._fit_view)
            return
        available_y = ch - BOTTOM_BAND_Y - 80
        if available_y < 120:
            return
        min_x = min(d.global_x for d in self.displays)
        min_y = min(d.global_y for d in self.displays)
        max_x = max(d.global_x + d.width for d in self.displays)
        max_y = max(d.global_y + d.height for d in self.displays)
        bw = max(max_x - min_x, 1)
        bh = max(max_y - min_y, 1)
        avail_w = max(cw - BAND_PAD_X * 2, 1)
        avail_h = max(available_y, 1)
        self._scale = min(avail_w / bw, avail_h / bh, 0.4)
        self._pan_x = (cw - bw * self._scale) / 2 - min_x * self._scale
        self._pan_y = (avail_h - bh * self._scale) / 2 - min_y * self._scale
        self._redraw()

    def _zoom_centered(self, factor: float) -> None:
        if not self.displays:
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        self._zoom_at(cw // 2, (BOTTOM_BAND_Y + ch) // 2, factor)

    def _zoom_at(self, sx: int, sy: int, factor: float) -> None:
        new_scale = max(0.01, min(self._scale * factor, 2.0))
        if new_scale == self._scale:
            return
        gx = (sx - self._pan_x) / self._scale
        gy = (sy - self._pan_y - BOTTOM_BAND_Y - 40) / self._scale
        self._scale = new_scale
        self._pan_x = sx - gx * self._scale
        self._pan_y = sy - gy * self._scale - BOTTOM_BAND_Y - 40
        self._redraw()

    def _on_canvas_enter(self, _e=None) -> None:
        self.canvas.bind_all("<MouseWheel>", self._on_wheel_zoom)
        self.canvas.bind_all("<Button-4>", self._on_wheel_up)
        self.canvas.bind_all("<Button-5>", self._on_wheel_down)

    def _on_canvas_leave(self, _e=None) -> None:
        self.canvas.unbind_all("<MouseWheel>")
        self.canvas.unbind_all("<Button-4>")
        self.canvas.unbind_all("<Button-5>")

    def _canvas_xy(self, event) -> tuple[int, int]:
        try:
            rx = self.canvas.winfo_rootx()
            ry = self.canvas.winfo_rooty()
            return event.x_root - rx, event.y_root - ry
        except tk.TclError:
            return event.x, event.y

    def _on_wheel_zoom(self, event) -> None:
        x, y = self._canvas_xy(event)
        if y < BOTTOM_BAND_Y:
            return
        factor = 1.1 if event.delta > 0 else 1 / 1.1
        self._zoom_at(x, y, factor)

    def _on_wheel_up(self, event) -> None:
        x, y = self._canvas_xy(event)
        if y < BOTTOM_BAND_Y:
            return
        self._zoom_at(x, y, 1.1)

    def _on_wheel_down(self, event) -> None:
        x, y = self._canvas_xy(event)
        if y < BOTTOM_BAND_Y:
            return
        self._zoom_at(x, y, 1 / 1.1)

    # ── Hit testing ──────────────────────────────────────────────

    def _hit_node(self, x: float, y: float) -> Optional[_Node]:
        for n in reversed(self._nodes):
            x0, y0, x1, y1 = n.rect
            if x0 <= x <= x1 and y0 <= y <= y1:
                return n
        return None

    def _hit_route_line(self, x: float, y: float) -> Optional[str]:
        """Click-near-line hit test. Returns the sink_key of a route
        whose Bezier passes within LINE_PICK_PX of (x, y), or None."""
        items = self.canvas.find_overlapping(
            x - LINE_PICK_PX, y - LINE_PICK_PX,
            x + LINE_PICK_PX, y + LINE_PICK_PX,
        )
        for item in reversed(items):
            for tag in self.canvas.gettags(item):
                if tag.startswith("route:"):
                    return tag[len("route:"):]
        return None

    # ── Left button: line-drag / PC-drag / adjacency drag / pan ──

    def _on_press(self, event):
        # Priority order:
        #   1. A ROUTE LINE under the cursor → start line-drag (swap).
        #   2. A PC CHASSIS under the cursor → start PC-drag (swap).
        #   3. A PHYSICAL DISPLAY rectangle → adjacency drag.
        #   4. Empty space → pan.
        route_sink_key = self._hit_route_line(event.x, event.y)
        if route_sink_key is not None:
            origin = self._display_by_key(route_sink_key)
            if origin is not None:
                self._line_drag_origin = origin
                self._line_drag_xy = (event.x, event.y)
                self.canvas.config(cursor="crosshair")
                self._redraw()
                return
        node = self._hit_node(event.x, event.y)
        if node is not None and node.kind == "pc":
            # PC-drag: the "line" we're visually moving is the one
            # currently attached to some arbitrary display — we pick
            # the display this PC is driving right now and move that
            # endpoint. Clean swap semantics.
            effective = self._effective_routes()
            my_sink: Optional[DisplayInfo] = None
            for d in self.displays:
                sink_key = f"{d.machine_id}:{d.monitor_id}"
                src = effective.get(sink_key, sink_key)
                if src.partition(":")[0] == node.machine_id:
                    my_sink = d
                    break
            if my_sink is None:
                return
            self._line_drag_origin = my_sink
            self._line_drag_xy = (event.x, event.y)
            self.canvas.config(cursor="crosshair")
            self._redraw()
            return
        if node is not None and node.kind == "physical":
            self._drag_physical = node.display
            sx, sy = self._to_screen(node.display.global_x,
                                     node.display.global_y)
            self._drag_offset = (event.x - sx, event.y - sy)
            self.canvas.config(cursor="fleur")
            self._redraw()
            return
        self._pan_start = (event.x, event.y, self._pan_x, self._pan_y)
        self.canvas.config(cursor="fleur")

    def _on_drag(self, event):
        if self._line_drag_origin is not None:
            self._line_drag_xy = (event.x, event.y)
            node = self._hit_node(event.x, event.y)
            hit = node.display if node and node.kind == "physical" else None
            if hit is self._line_drag_origin:
                hit = None
            if hit is not self._swap_drop_target:
                self._swap_drop_target = hit
            self._redraw()
            return
        if self._drag_physical is not None:
            d = self._drag_physical
            prev_x, prev_y = d.global_x, d.global_y
            gx, gy = self._to_global(
                event.x - self._drag_offset[0],
                event.y - self._drag_offset[1],
            )
            d.global_x = round(gx)
            d.global_y = round(gy)
            self._snap(d)
            if self._overlaps_other(d):
                d.global_x, d.global_y = prev_x, prev_y
            self._dirty_touched()
            self._redraw()
            return
        if self._pan_start is not None:
            sx, sy, base_px, base_py = self._pan_start
            self._pan_x = base_px + (event.x - sx)
            self._pan_y = base_py + (event.y - sy)
            self._redraw()

    def _on_release(self, event):
        if self._line_drag_origin is not None:
            origin = self._line_drag_origin
            drop = self._swap_drop_target
            self._line_drag_origin = None
            self._line_drag_xy = None
            self._swap_drop_target = None
            self.canvas.config(cursor="")
            self._redraw()
            if drop is not None and drop is not origin:
                self._swap_sources(origin, drop)
            return
        if self._drag_physical is not None:
            self._drag_physical = None
            self.canvas.config(cursor="")
            self._redraw()
        if self._pan_start is not None:
            self._pan_start = None
            self.canvas.config(cursor="")

    # ── Shift-drag: swap sources between displays ────────────────

    def _on_swap_press(self, event):
        node = self._hit_node(event.x, event.y)
        if node is None or node.kind != "physical":
            return
        self._swap_drag_from = node.display
        self._swap_drop_target = None
        self.canvas.config(cursor="crosshair")
        self._redraw()

    def _on_swap_drag(self, event):
        if self._swap_drag_from is None:
            return
        node = self._hit_node(event.x, event.y)
        hit = node.display if node and node.kind == "physical" else None
        if hit is self._swap_drag_from:
            hit = None
        if hit is not self._swap_drop_target:
            self._swap_drop_target = hit
            self._redraw()

    def _on_swap_release(self, event):
        src = self._swap_drag_from
        target = self._swap_drop_target
        self._swap_drag_from = None
        self._swap_drop_target = None
        self.canvas.config(cursor="")
        self._redraw()
        if src is None or target is None or src is target:
            return
        self._swap_sources(src, target)

    def _swap_sources(self, a: DisplayInfo, b: DisplayInfo) -> None:
        """Swap the source PC-output driving `a` with the one driving
        `b`. Preserves the invariant that every display has exactly
        one unique source."""
        effective = self._effective_routes()
        a_key = f"{a.machine_id}:{a.monitor_id}"
        b_key = f"{b.machine_id}:{b.monitor_id}"
        a_src = effective.get(a_key, a_key)
        b_src = effective.get(b_key, b_key)
        if a_src == b_src:
            return
        # Stage the swap as pending edits.
        self._stage_route(a_key, b_src)
        self._stage_route(b_key, a_src)
        self._dirty_touched()

    def _stage_route(self, sink_key: str, source_key: str) -> None:
        committed = self._committed_routes.get(sink_key, sink_key)
        if source_key == committed:
            # Edit would un-do a pending change, or is a no-op
            # against committed — clear it.
            self._pending_routes.pop(sink_key, None)
            return
        self._pending_routes[sink_key] = source_key

    def _dirty_touched(self) -> None:
        self._dirty = True
        self._set_apply_enabled(True)
        self._refresh_status()

    # ── Right-click menu: keyboard-free swap ─────────────────────

    def _on_right_click(self, event):
        node = self._hit_node(event.x, event.y)
        if node is None or node.kind != "physical":
            return
        sink_key = f"{node.machine_id}:{node.monitor_id}"
        effective = self._effective_routes()
        current = effective.get(sink_key, sink_key)
        menu = tk.Menu(self.canvas, tearoff=0,
                       bg=PAPER_BG, fg=PAPER_TEXT,
                       activebackground=LILAC,
                       activeforeground="#ffffff",
                       bd=1, relief=tk.SOLID)
        menu.add_command(
            label=f"Show on {node.machine_id}:{node.monitor_id}",
            state=tk.DISABLED,
        )
        menu.add_separator()
        for d in self.displays:
            src_key = f"{d.machine_id}:{d.monitor_id}"
            if src_key == sink_key:
                continue
            mark = "✓ " if current == src_key else "   "
            menu.add_command(
                label=f"{mark}from {d.machine_id}:{d.monitor_id}",
                command=lambda o=d, s=node.display:
                    self._swap_sources(s, o),
            )
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    # ── Middle-click pan ─────────────────────────────────────────

    def _on_pan_press(self, event):
        self._pan_start = (event.x, event.y, self._pan_x, self._pan_y)
        self.canvas.config(cursor="fleur")

    def _on_pan_motion(self, event):
        if self._pan_start is None:
            return
        sx, sy, base_px, base_py = self._pan_start
        self._pan_x = base_px + (event.x - sx)
        self._pan_y = base_py + (event.y - sy)
        self._redraw()

    def _on_pan_release(self, _event):
        self._pan_start = None
        self.canvas.config(cursor="")

    # ── Adjacency helpers ────────────────────────────────────────

    def _overlaps_other(self, d: DisplayInfo) -> bool:
        for o in self.displays:
            if o is d:
                continue
            if (d.global_x < o.global_x + o.width
                    and d.global_x + d.width > o.global_x
                    and d.global_y < o.global_y + o.height
                    and d.global_y + d.height > o.global_y):
                return True
        return False

    def _snap(self, d: DisplayInfo) -> None:
        threshold = SNAP_THRESHOLD / self._scale
        dr = d.global_x + d.width
        db = d.global_y + d.height
        best_dx, best_dy = float("inf"), float("inf")
        snap_x, snap_y = d.global_x, d.global_y
        for o in self.displays:
            if o is d:
                continue
            o_r = o.global_x + o.width
            o_b = o.global_y + o.height
            if abs(dr - o.global_x) < abs(best_dx):
                best_dx = dr - o.global_x
                snap_x = o.global_x - d.width
            if abs(d.global_x - o_r) < abs(best_dx):
                best_dx = d.global_x - o_r
                snap_x = o_r
            if abs(db - o.global_y) < abs(best_dy):
                best_dy = db - o.global_y
                snap_y = o.global_y - d.height
            if abs(d.global_y - o_b) < abs(best_dy):
                best_dy = d.global_y - o_b
                snap_y = o_b
            if abs(d.global_y - o.global_y) < threshold \
                    and abs(d.global_y - o.global_y) < abs(best_dy):
                best_dy = d.global_y - o.global_y
                snap_y = o.global_y
            if abs(db - o_b) < threshold and abs(db - o_b) < abs(best_dy):
                best_dy = db - o_b
                snap_y = o_b - d.height
            if abs(d.global_x - o.global_x) < threshold \
                    and abs(d.global_x - o.global_x) < abs(best_dx):
                best_dx = d.global_x - o.global_x
                snap_x = o.global_x
            if abs(dr - o_r) < threshold and abs(dr - o_r) < abs(best_dx):
                best_dx = dr - o_r
                snap_x = o_r - d.width
        if abs(best_dx) < threshold:
            d.global_x = round(snap_x)
        if abs(best_dy) < threshold:
            d.global_y = round(snap_y)

    # ── Actions ──────────────────────────────────────────────────

    def _do_apply(self) -> None:
        if not self._has_pending_edits():
            return
        # 1. Physical layout goes via on_apply (monitor positions).
        if self._on_apply is not None:
            layout = [
                {"machine_id": d.machine_id, "monitor_id": d.monitor_id,
                 "global_x": d.global_x, "global_y": d.global_y}
                for d in self.displays
            ]
            self._on_apply(layout)
        # 2. Route swaps go via on_reroute.
        if self._on_reroute is not None:
            for sink_key, source_key in self._pending_routes.items():
                try:
                    self._on_reroute(sink_key, source_key)
                except Exception:
                    log.exception("on_reroute failed")
        self._pending_routes.clear()
        self.mark_clean()
        self._refresh_status()

    def _do_identify(self) -> None:
        if self._on_identify:
            self._on_identify()

    def _do_reset(self) -> None:
        by_key = {(d.machine_id, d.monitor_id): d
                  for d in self.original_displays}
        for d in self.displays:
            o = by_key.get((d.machine_id, d.monitor_id))
            if o is None:
                continue
            d.global_x = o.global_x
            d.global_y = o.global_y
        self._pending_routes.clear()
        self._dirty = False
        self._set_apply_enabled(False)
        _number_displays(self.displays)
        self._refresh_status()
        self._fit_view()


def _copy(d: DisplayInfo) -> DisplayInfo:
    return DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})


def _number_displays(displays: list[DisplayInfo]) -> None:
    by_machine: dict[str, list[DisplayInfo]] = {}
    for d in displays:
        by_machine.setdefault(d.machine_id, []).append(d)
    order = sorted(by_machine.keys())
    n = 1
    for mid in order:
        for d in sorted(by_machine[mid], key=lambda d: d.monitor_id):
            d.number = n
            n += 1

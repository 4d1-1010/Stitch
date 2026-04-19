"""Three-row Layout canvas: PCs, virtuals, physicals.

Top band  — one PC chip per peer in the workspace.
Middle band — virtual displays. Empty by default; a "+" slot at the
  end creates a floating (unclaimed) virtual that the user then wires
  to an owning PC with a line.
Bottom area — every physical monitor in the workspace, positioned in
  2D for cursor-adjacency editing (stacks, L-shapes, whatever the
  user's real desk looks like).

Lines connect top→down:
  * PC → Virtual  (ownership — who provides the virtual's pixels)
  * PC → Physical (identity/route — what this physical renders)
  * Virtual → Physical (route — virtual's framebuffer shown here)

By default every physical has a line from its owning PC (identity
routing). Deleting that line means "no input source" — the panel
shows a placeholder card. Moving the top endpoint to another PC
routes this physical to that PC's primary. Adding a virtual in the
middle lets the user stage a remote monitor before wiring it.

The canvas is a pure widget. It fires callbacks on Apply / Identify /
Reset / Reroute / Add virtual / Remove virtual and expects the shell
to mutate state + hand back updated data via `set_displays` +
`set_routes`.
"""

from __future__ import annotations

import colorsys
import logging
import math
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


# Procedural per-machine colour. Everything lives in the lilac
# family so the Layout canvas reads as "nuances of the brand colour"
# rather than a rainbow. Three hash-derived knobs — hue offset around
# lilac (±30°), saturation, lightness — give enough variation that
# human eyes can always tell peers apart, without ever leaving the
# paper+lilac palette.
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


# ── Band geometry (canvas-local, unaffected by pan / zoom) ──────────

# Each band is a fixed-height strip anchored to the top of the canvas.
# The remaining vertical space below is the freeform adjacency area
# where physical monitors live.
TOP_BAND_H = 84
MID_BAND_H = 128
BAND_GAP = 18
TOP_BAND_Y = 16
MID_BAND_Y = TOP_BAND_Y + TOP_BAND_H + BAND_GAP
BOTTOM_BAND_Y = MID_BAND_Y + MID_BAND_H + BAND_GAP

# Node geometry inside the bands.
PC_NODE_W = 200
PC_NODE_H = 56
VIRTUAL_NODE_W = 200
VIRTUAL_NODE_H = 100
HUB_NODE_DIAMETER = 86
NODE_GAP = 28
BAND_PAD_X = 40

# Drag-line hit-test tolerance.
LINE_PICK_PX = 8

# Bottom-area adjacency constants — shared with the legacy canvas.
BOTTOM_SCALE_DEFAULT = 0.12
BOTTOM_PAN_DEFAULT = (0.0, 0.0)


@dataclass
class _Node:
    """One hit-testable entry on the canvas — a PC chip, a virtual
    card, a physical rectangle, or the "+" slot. Populated per
    redraw; hit testing walks this list."""
    kind: str              # "pc" | "virtual" | "physical" | "add"
    key: str               # "pc:<mid>" / "virt:<mid>:<mon>" / "phys:<mid>:<mon>" / "add"
    rect: tuple[float, float, float, float]  # canvas-space (x0,y0,x1,y1)
    machine_id: str = ""
    monitor_id: str = ""
    display: Optional[DisplayInfo] = None


class LayoutPanel(tk.Frame):
    """Three-row patch-bay layout canvas.

    Public shape is preserved so the shell keeps working unchanged:
    constructor callbacks, set_displays, set_routes, set_active_machine,
    mark_clean, set_mode.
    """

    def __init__(self, parent: tk.Widget, *,
                 on_apply: Optional[Callable[[list[dict]], None]] = None,
                 on_identify: Optional[Callable[[], None]] = None,
                 on_reroute: Optional[Callable[[str, str], None]] = None,
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
        self._on_add_virtual = on_add_virtual
        self._on_remove_virtual = on_remove_virtual
        self._on_add_hub = on_add_hub
        self._on_remove_hub = on_remove_hub
        self._virtuals_provider = virtuals_provider
        self._hubs_provider = hubs_provider
        self._sources_provider = sources_provider
        # Hubs are a distinct middle-row node for pure multicast /
        # duplication. Populated each redraw from hubs_provider; each
        # entry is {"id": "<hub_id>"}. Visually drawn as a round node
        # with a "⇉ ×N" fan-count badge that reflects how many
        # physicals currently route from this hub.
        self._hubs: list[str] = []

        self.displays: list[DisplayInfo] = []        # physicals + virtuals
        self.original_displays: list[DisplayInfo] = []
        self._active_machine: str = ""
        self._routes: dict[str, str] = {}

        # Per-redraw hit-test cache.
        self._nodes: list[_Node] = []

        # Bottom-area pan/zoom for physical adjacency editing.
        self._scale = BOTTOM_SCALE_DEFAULT
        self._pan_x, self._pan_y = BOTTOM_PAN_DEFAULT

        # Drag state.
        self._drag_physical: Optional[DisplayInfo] = None
        self._drag_offset = (0, 0)
        self._line_drag_from: Optional[_Node] = None
        self._line_drag_to_xy: Optional[tuple[int, int]] = None
        self._pan_start: Optional[tuple[int, int, float, float]] = None
        self._selected_route: Optional[str] = None

        self._dirty = False

        self._build_ui()

    # ── Compatibility shims ─────────────────────────────────────

    def set_mode(self, _mode: str) -> None:
        """No-op — the three-row canvas doesn't have modes. Kept so
        the shell can call it unconditionally on older builds."""

    # ── UI scaffolding ──────────────────────────────────────────

    def _build_ui(self) -> None:
        header = tk.Frame(self, bg=PAPER_BG)
        header.pack(fill=tk.X, padx=SPACE_LG, pady=(0, SPACE_SM))
        tk.Label(
            header,
            text="Top: your PCs. Middle: virtual displays (click + "
                 "to add one). Bottom: every physical display — drag "
                 "to arrange, draw a line onto one to change what it "
                 "shows.",
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
        self.canvas.bind("<ButtonPress-2>", self._on_pan_press)
        self.canvas.bind("<B2-Motion>", self._on_pan_motion)
        self.canvas.bind("<ButtonRelease-2>", self._on_pan_release)
        self.canvas.bind("<Configure>",
                         lambda e: self._on_canvas_configure())
        self.canvas.bind("<Key-Delete>", self._on_delete_key)
        self.canvas.bind("<Key-BackSpace>", self._on_delete_key)
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

    # ── Public API ─────────────────────────────────────────────

    def set_displays(self, monitors: list[dict]) -> None:
        """Replace the list of physical monitors. Virtuals come from
        `virtuals_provider` on each redraw so the shell can add /
        remove them without needing a separate setter here."""
        physical = [
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
        self.displays = physical + self._fetch_virtuals(physical)
        self._hubs = self._fetch_hubs()
        _number_physicals(self.displays)
        if not self._dirty:
            self.original_displays = [_copy(d) for d in self.displays
                                      if not d.virtual]
        self._set_session_controls_enabled(bool(physical))
        if self.displays:
            self._empty.place_forget()
        else:
            self._empty.place(relx=0.5, rely=0.5, anchor="center")
        self._fit_view()

    def set_routes(self, routes: dict[str, str]) -> None:
        clean = {str(k): str(v) for k, v in (routes or {}).items()}
        if clean == self._routes:
            return
        self._routes = clean
        if self._selected_route not in self._routes:
            self._selected_route = None
        self._redraw()

    def set_active_machine(self, machine_id: str) -> None:
        if machine_id == self._active_machine:
            return
        self._active_machine = machine_id
        self._redraw()

    def mark_clean(self) -> None:
        self.original_displays = [_copy(d) for d in self.displays
                                  if not d.virtual]
        self._dirty = False
        self._set_apply_enabled(False)

    # ── Internal state ─────────────────────────────────────────

    def _fetch_virtuals(self, physical: list[DisplayInfo]) -> list[DisplayInfo]:
        if self._virtuals_provider is None:
            return []
        try:
            raw = self._virtuals_provider() or []
        except Exception:
            log.exception("virtuals_provider failed")
            return []
        out: list[DisplayInfo] = []
        for v in raw:
            mid = str(v.get("machine_id") or "")
            mon = str(v.get("monitor_id") or "")
            if not mon:
                continue
            out.append(DisplayInfo(
                machine_id=mid, monitor_id=mon,
                global_x=0, global_y=0,
                width=int(v.get("width") or 1920),
                height=int(v.get("height") or 1080),
                virtual=True,
            ))
        return out

    def _fetch_hubs(self) -> list[str]:
        if self._hubs_provider is None:
            return []
        try:
            raw = self._hubs_provider() or []
        except Exception:
            log.exception("hubs_provider failed")
            return []
        out: list[str] = []
        for h in raw:
            hid = str(h.get("id") or "")
            if hid and hid not in out:
                out.append(hid)
        return out

    def _machines(self) -> list[str]:
        seen: set[str] = set()
        for d in self.displays:
            if d.machine_id:
                seen.add(d.machine_id)
        return sorted(seen)

    def _physicals(self) -> list[DisplayInfo]:
        return [d for d in self.displays if not d.virtual]

    def _virtuals(self) -> list[DisplayInfo]:
        return [d for d in self.displays if d.virtual]

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

    # ── Redraw: three bands ────────────────────────────────────

    def _redraw(self) -> None:
        c = self.canvas
        c.delete("all")
        self._nodes.clear()

        cw = c.winfo_width()
        ch = c.winfo_height()
        if cw < 50 or ch < 50:
            return

        # Row separators — faint horizontal hairlines so the three
        # roles read at a glance.
        for y in (TOP_BAND_Y + TOP_BAND_H + BAND_GAP / 2,
                  MID_BAND_Y + MID_BAND_H + BAND_GAP / 2):
            c.create_line(20, y, cw - 20, y,
                          fill=PAPER_BORDER, width=1, dash=(2, 3))

        # Zone labels on the left edge.
        for label, y in (("PCs", TOP_BAND_Y + TOP_BAND_H / 2),
                         ("Virtual", MID_BAND_Y + MID_BAND_H / 2),
                         ("Physical", BOTTOM_BAND_Y + 18)):
            c.create_text(
                18, y, text=label, anchor="w",
                font=(FONT_SANS, SIZE_XS, "bold"),
                fill=PAPER_MUTED,
            )

        self._draw_top_band(cw)
        self._draw_mid_band(cw)
        self._draw_bottom_area(cw, ch)
        self._draw_lines()
        self._draw_line_ghost()

    def _draw_top_band(self, cw: int) -> None:
        machines = self._machines()
        if not machines:
            return
        x = BAND_PAD_X + 70  # leave room for the "PCs" label
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
            # Colour dot + label.
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

    def _draw_mid_band(self, cw: int) -> None:
        x = BAND_PAD_X + 70
        y = MID_BAND_Y
        for v in self._virtuals():
            color = machine_color(v.machine_id) if v.machine_id \
                else PAPER_MUTED
            rect = (x, y, x + VIRTUAL_NODE_W, y + VIRTUAL_NODE_H)
            self._nodes.append(_Node(
                kind="virtual",
                key=f"virt:{v.machine_id}:{v.monitor_id}",
                rect=rect, machine_id=v.machine_id,
                monitor_id=v.monitor_id, display=v,
            ))
            fill = _blend(color, 0.12)
            self.canvas.create_rectangle(
                *rect, fill=fill, outline=color, width=2,
                dash=(6, 4),
            )
            title = v.monitor_id
            subtitle = (v.machine_id if v.machine_id else "unclaimed")
            self.canvas.create_text(
                x + VIRTUAL_NODE_W / 2, y + 28,
                text=title, anchor="center",
                font=(FONT_SANS, SIZE_BASE, "bold"),
                fill=PAPER_TEXT,
            )
            self.canvas.create_text(
                x + VIRTUAL_NODE_W / 2, y + 52,
                text=("virtual · " + subtitle),
                anchor="center",
                font=(FONT_SANS, SIZE_SM),
                fill=PAPER_MUTED,
            )
            x += VIRTUAL_NODE_W + NODE_GAP

        # Hubs: distinct circular nodes for duplication / fan-out.
        # Drawn after virtuals so they share the same row but read
        # as a different "thing" thanks to the round shape + the
        # "⇉ ×N" fan-count badge underneath.
        for hub_id in self._hubs:
            cx = x + HUB_NODE_DIAMETER / 2
            cy = y + VIRTUAL_NODE_H / 2
            d = HUB_NODE_DIAMETER
            rect = (x, cy - d / 2, x + d, cy + d / 2)
            self._nodes.append(_Node(
                kind="hub", key=f"hub:{hub_id}",
                rect=rect, monitor_id=hub_id,
            ))
            border = LILAC
            fill = _blend(LILAC, 0.14, bg_hex=CANVAS_BG)
            self.canvas.create_oval(
                rect[0] + 3, rect[1] + 3,
                rect[2] - 3, rect[3] - 3,
                fill=fill, outline=border, width=3,
            )
            # Broadcast glyph — two short arrows diverging from a
            # single stem — sits in the centre of the circle so the
            # hub never looks like a tiny moon.
            self.canvas.create_text(
                cx, cy - 4,
                text="⇉", anchor="center",
                font=(FONT_SANS, SIZE_LG + 8, "bold"),
                fill=border,
            )
            fan_count = sum(
                1 for sink, src in self._routes.items()
                if src == f"hub:{hub_id}"
            )
            self.canvas.create_text(
                cx, cy + 18,
                text=f"×{fan_count}" if fan_count else "hub",
                anchor="center",
                font=(FONT_SANS, SIZE_SM, "bold"),
                fill=PAPER_TEXT,
            )
            # Label below the circle so it doesn't clip the shape.
            self.canvas.create_text(
                cx, rect[3] - 2,
                text=hub_id,
                anchor="n",
                font=(FONT_SANS, SIZE_XS),
                fill=PAPER_MUTED,
            )
            x += HUB_NODE_DIAMETER + NODE_GAP

        # Two "+" slots so users know both options exist without
        # guessing. "+ Virtual" spawns a phantom monitor; "+ Hub"
        # spawns a duplication junction.
        if self._machines():
            slot_w = VIRTUAL_NODE_W // 2
            rect = (x, y, x + slot_w, y + VIRTUAL_NODE_H)
            self._nodes.append(_Node(
                kind="add_virtual", key="add_virtual", rect=rect,
            ))
            self.canvas.create_rectangle(
                *rect, fill=_blend(LILAC, 0.08),
                outline=_blend(LILAC, 0.45),
                width=2, dash=(3, 3),
            )
            cx = (rect[0] + rect[2]) / 2
            cy = (rect[1] + rect[3]) / 2
            self.canvas.create_text(
                cx, cy - 8, text="+", anchor="center",
                font=(FONT_SANS, SIZE_LG + 10, "bold"),
                fill=LILAC,
            )
            self.canvas.create_text(
                cx, cy + 18, text="Add virtual",
                anchor="center", font=(FONT_SANS, SIZE_XS),
                fill=PAPER_MUTED,
            )
            x += slot_w + NODE_GAP // 2
            hub_rect = (x, y, x + slot_w, y + VIRTUAL_NODE_H)
            self._nodes.append(_Node(
                kind="add_hub", key="add_hub", rect=hub_rect,
            ))
            self.canvas.create_oval(
                hub_rect[0] + 8, hub_rect[1] + 10,
                hub_rect[2] - 8, hub_rect[3] - 10,
                fill=_blend(LILAC, 0.08),
                outline=_blend(LILAC, 0.45), width=2, dash=(3, 3),
            )
            hx = (hub_rect[0] + hub_rect[2]) / 2
            hy = (hub_rect[1] + hub_rect[3]) / 2
            self.canvas.create_text(
                hx, hy - 6, text="+⇉", anchor="center",
                font=(FONT_SANS, SIZE_LG, "bold"),
                fill=LILAC,
            )
            self.canvas.create_text(
                hx, hy + 16, text="Add hub",
                anchor="center", font=(FONT_SANS, SIZE_XS),
                fill=PAPER_MUTED,
            )

    def _draw_bottom_area(self, cw: int, ch: int) -> None:
        phys = self._physicals()
        if not phys:
            return
        # Grid for the adjacency area so drags feel "on a grid".
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

        for d in phys:
            sx, sy = self._to_screen(d.global_x, d.global_y)
            sw = d.width * self._scale
            sh = d.height * self._scale
            # Clamp to bottom band so the physical doesn't collide
            # with the row labels above.
            if sy < grid_y0:
                offset = grid_y0 - sy
                sy += offset
            color = machine_color(d.machine_id)
            is_active = (d.machine_id == self._active_machine
                         and self._active_machine != "")
            is_dragging = (d is self._drag_physical)
            sink_key = f"{d.machine_id}:{d.monitor_id}"
            route_src = self._routes.get(sink_key, sink_key)
            # A physical with no route pointing at it IS detached,
            # unless the identity default applies. In this model the
            # default route is implicit — sink_key → sink_key — so
            # "detached" means we've set an explicit route to
            # something that doesn't exist OR the identity source (its
            # own PC) has been tombstoned.
            detached = False  # placeholder; plugged in below

            fill = _blend(color, 0.34 if is_active
                          else (0.26 if is_dragging else 0.18))
            outline = color
            border = 4 if (is_active or is_dragging) else 3
            self.canvas.create_rectangle(
                sx, sy, sx + sw, sy + sh,
                fill=fill, outline=outline, width=border,
            )

            # Number + machine_id centred on the rectangle.
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
                self.canvas.create_text(
                    cx, cy + total_h / 2 - lbl_size / 2,
                    text=f"{d.machine_id}:{d.monitor_id}",
                    anchor="center",
                    font=(FONT_SANS, lbl_size),
                    fill=PAPER_MUTED,
                )

            # "No input source" overlay when an explicit non-identity
            # route exists but points nowhere useful. In v1 we can
            # detect this by checking whether the source key matches
            # any known source (physical or virtual).
            all_source_keys = {
                f"{pd.machine_id}:{pd.monitor_id}"
                for pd in self.displays
            } | {
                f"{m}:__native__" for m in self._machines()
            }
            if (route_src != sink_key
                    and route_src not in all_source_keys):
                self._draw_no_source_overlay(sx, sy, sw, sh)

            self._nodes.append(_Node(
                kind="physical",
                key=f"phys:{d.machine_id}:{d.monitor_id}",
                rect=(sx, sy, sx + sw, sy + sh),
                machine_id=d.machine_id,
                monitor_id=d.monitor_id, display=d,
            ))

    def _draw_no_source_overlay(self, sx, sy, sw, sh) -> None:
        self.canvas.create_rectangle(
            sx, sy, sx + sw, sy + sh,
            fill="#2a2b36", outline="", stipple="gray50",
        )
        cx = sx + sw / 2
        cy = sy + sh / 2
        self.canvas.create_text(
            cx, cy - 8, text="no input source",
            anchor="center",
            font=(FONT_SANS, max(8, int(sh * 0.08)), "bold"),
            fill="#cfd2e3",
        )
        self.canvas.create_text(
            cx, cy + 12,
            text="draw a line to set one",
            anchor="center",
            font=(FONT_SANS, max(7, int(sh * 0.06))),
            fill="#8d91a8",
        )

    # ── Canvas coord transform (bottom area only) ──────────────

    def _to_screen(self, gx: float, gy: float) -> tuple[float, float]:
        return (gx * self._scale + self._pan_x,
                gy * self._scale + self._pan_y + BOTTOM_BAND_Y + 40)

    def _to_global(self, sx: float, sy: float) -> tuple[float, float]:
        return ((sx - self._pan_x) / self._scale,
                (sy - self._pan_y - BOTTOM_BAND_Y - 40) / self._scale)

    def _on_canvas_configure(self) -> None:
        if self._physicals():
            self._fit_view()
        else:
            self._redraw()

    def _fit_view(self) -> None:
        phys = self._physicals()
        if not phys:
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
        min_x = min(d.global_x for d in phys)
        min_y = min(d.global_y for d in phys)
        max_x = max(d.global_x + d.width for d in phys)
        max_y = max(d.global_y + d.height for d in phys)
        bw = max(max_x - min_x, 1)
        bh = max(max_y - min_y, 1)
        avail_w = max(cw - BAND_PAD_X * 2, 1)
        avail_h = max(available_y, 1)
        self._scale = min(avail_w / bw, avail_h / bh, 0.4)
        self._pan_x = (cw - bw * self._scale) / 2 - min_x * self._scale
        self._pan_y = (avail_h - bh * self._scale) / 2 - min_y * self._scale
        self._redraw()

    def _zoom_centered(self, factor: float) -> None:
        if not self._physicals():
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        self._zoom_at(cw // 2, (BOTTOM_BAND_Y + ch) // 2, factor)

    def _zoom_at(self, sx: int, sy: int, factor: float) -> None:
        new_scale = max(0.01, min(self._scale * factor, 2.0))
        if new_scale == self._scale:
            return
        # Keep (sx, sy) under the cursor.
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

    # ── Lines ──────────────────────────────────────────────────

    def _draw_lines(self) -> None:
        node_map = {n.key: n for n in self._nodes}

        def resolve_source_node(source_key: str) -> Optional[_Node]:
            # Hubs take the form "hub:<id>"; everything else is
            # "<machine>:<monitor>".
            if source_key.startswith("hub:"):
                return node_map.get(source_key)
            src_mid, _, src_mon = source_key.partition(":")
            if src_mon:
                virt = node_map.get(f"virt:{src_mid}:{src_mon}")
                if virt is not None:
                    return virt
                phys = node_map.get(f"phys:{src_mid}:{src_mon}")
                if phys is not None:
                    return phys
            return node_map.get(f"pc:{src_mid}")

        # Lines into physicals — identity default, plus any override.
        for phys in self._physicals():
            sink_key = f"{phys.machine_id}:{phys.monitor_id}"
            source_key = self._routes.get(sink_key, sink_key)
            sink_node = node_map.get(f"phys:{sink_key}")
            if sink_node is None:
                continue
            source_node = resolve_source_node(source_key)
            if source_node is None:
                continue
            self._draw_bezier(source_node, sink_node, sink_key)

        # Lines into hubs — the source that the hub will fan out.
        for hub_id in self._hubs:
            hub_key = f"hub:{hub_id}"
            source_key = self._routes.get(hub_key)
            if not source_key:
                continue
            sink_node = node_map.get(hub_key)
            if sink_node is None:
                continue
            source_node = resolve_source_node(source_key)
            if source_node is None:
                continue
            self._draw_bezier(source_node, sink_node, hub_key)

        # Ownership lines: PC → virtual (dashed).
        for v in self._virtuals():
            if not v.machine_id:
                continue
            pc = node_map.get(f"pc:{v.machine_id}")
            vn = node_map.get(f"virt:{v.machine_id}:{v.monitor_id}")
            if pc is None or vn is None:
                continue
            self._draw_bezier(pc, vn,
                              sink_key=f"own:{v.machine_id}:{v.monitor_id}",
                              ownership=True)

    def _draw_bezier(self, src: _Node, dst: _Node, sink_key: str = "",
                     *, ownership: bool = False) -> None:
        sx0 = (src.rect[0] + src.rect[2]) / 2
        sy0 = src.rect[3]
        sx1 = (dst.rect[0] + dst.rect[2]) / 2
        sy1 = dst.rect[1]
        dy = sy1 - sy0
        c1 = (sx0, sy0 + dy * 0.5)
        c2 = (sx1, sy1 - dy * 0.5)
        machine = src.machine_id or dst.machine_id
        color = machine_color(machine) if machine else PAPER_MUTED
        selected = (sink_key and sink_key == self._selected_route)
        if selected:
            stroke = LILAC
            width = 5
        elif ownership:
            stroke = _blend(color, 0.65)
            width = 2
        else:
            stroke = color
            width = 3
        dash = (8, 4) if ownership else None
        tag = (f"route:{sink_key}" if sink_key else f"own-line")
        if not ownership or selected:
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
            fill=stroke, width=width, dash=dash,
            arrow=tk.LAST, arrowshape=(14, 16, 6),
            tags=("bezier", tag),
        )

    def _draw_line_ghost(self) -> None:
        if self._line_drag_from is None or self._line_drag_to_xy is None:
            return
        src = self._line_drag_from
        sx0 = (src.rect[0] + src.rect[2]) / 2
        sy0 = (src.rect[1] + src.rect[3]) / 2
        ex, ey = self._line_drag_to_xy
        dy = ey - sy0
        c1 = (sx0, sy0 + dy * 0.5)
        c2 = (ex, ey - dy * 0.5)
        color = machine_color(src.machine_id) if src.machine_id else LILAC
        self.canvas.create_line(
            sx0, sy0, c1[0], c1[1], c2[0], c2[1], ex, ey,
            smooth=True, splinesteps=24,
            fill=color, width=3, dash=(6, 4),
        )

    # ── Hit testing ────────────────────────────────────────────

    def _hit_node(self, x: float, y: float) -> Optional[_Node]:
        # Walk backward — top-most drawn wins.
        for n in reversed(self._nodes):
            x0, y0, x1, y1 = n.rect
            if x0 <= x <= x1 and y0 <= y <= y1:
                return n
        return None

    def _hit_route_line(self, x: float, y: float) -> Optional[str]:
        items = self.canvas.find_overlapping(
            x - LINE_PICK_PX, y - LINE_PICK_PX,
            x + LINE_PICK_PX, y + LINE_PICK_PX)
        for item in reversed(items):
            for t in self.canvas.gettags(item):
                if t.startswith("route:"):
                    return t[len("route:"):]
        return None

    # ── Event handlers ─────────────────────────────────────────

    def _on_press(self, event):
        try:
            self.canvas.focus_set()
        except tk.TclError:
            pass
        node = self._hit_node(event.x, event.y)
        if node is not None:
            if node.kind == "add_virtual":
                # Virtual monitors always need an owning PC. Prompt
                # when ambiguous, auto-claim when only one peer is
                # in the workspace.
                machines = self._machines()
                if not machines:
                    return
                if len(machines) == 1:
                    if self._on_add_virtual is not None:
                        self._on_add_virtual(machines[0])
                    return
                self._pick_machine_for_new_virtual(event)
                return
            if node.kind == "add_hub":
                if self._on_add_hub is not None:
                    self._on_add_hub()
                return
            if node.kind in ("pc", "virtual", "hub"):
                # Start a new line drag from this node — hubs are
                # sources just like PCs and virtuals.
                self._line_drag_from = node
                self._line_drag_to_xy = (event.x, event.y)
                self.canvas.config(cursor="crosshair")
                if self._selected_route is not None:
                    self._selected_route = None
                self._redraw()
                return
            if node.kind == "physical":
                # Physical rect: drag to rearrange adjacency.
                self._drag_physical = node.display
                sx, sy = self._to_screen(node.display.global_x,
                                         node.display.global_y)
                self._drag_offset = (event.x - sx, event.y - sy)
                self.canvas.config(cursor="fleur")
                if self._selected_route is not None:
                    self._selected_route = None
                self._redraw()
                return

        # No node hit — try a route line (for selection + delete).
        route_key = self._hit_route_line(event.x, event.y)
        if route_key is not None:
            self._selected_route = route_key
            self._redraw()
            return
        # Empty space — pan.
        if self._selected_route is not None:
            self._selected_route = None
            self._redraw()
        self._pan_start = (event.x, event.y, self._pan_x, self._pan_y)
        self.canvas.config(cursor="fleur")

    def _on_drag(self, event):
        if self._line_drag_from is not None:
            self._line_drag_to_xy = (event.x, event.y)
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
            self._dirty = True
            self._set_apply_enabled(True)
            self._redraw()
            return
        if self._pan_start is not None:
            sx, sy, base_px, base_py = self._pan_start
            self._pan_x = base_px + (event.x - sx)
            self._pan_y = base_py + (event.y - sy)
            self._redraw()

    def _on_release(self, event):
        if self._line_drag_from is not None:
            src = self._line_drag_from
            drop = self._hit_node(event.x, event.y)
            self._line_drag_from = None
            self._line_drag_to_xy = None
            self.canvas.config(cursor="")
            self._redraw()
            if drop is None:
                return
            self._complete_line_drag(src, drop)
            return
        if self._drag_physical is not None:
            self._drag_physical = None
            self.canvas.config(cursor="")
            self._redraw()
        if self._pan_start is not None:
            self._pan_start = None
            self.canvas.config(cursor="")

    def _complete_line_drag(self, src: _Node, drop: _Node) -> None:
        """Decide what to do with a completed line drag based on the
        (src.kind, drop.kind) pair.

        PC → virtual : claim ownership (v1: no-op cross-PC).
        PC → hub     : hub subscribes to this PC's primary monitor.
        PC → physical: route that physical to this PC's primary.
        Virtual → hub: hub subscribes to this virtual.
        Virtual → physical: route the physical to show this virtual.
        Hub → physical: route the physical to show this hub.
        Physical → physical: route sink physical to show source physical.
        Anything → PC: no-op (PCs aren't sinks).
        """
        if drop.kind == "pc":
            return
        src_key = self._node_source_key(src)
        if src_key is None:
            return
        if drop.kind == "virtual":
            # Ownership transfer between PCs is a v2 concern.
            if src.kind == "pc" and drop.machine_id == src.machine_id:
                return
            if src.kind == "pc":
                log.info("Virtual ownership transfer not supported in v1")
            return
        if drop.kind == "hub":
            # Hub gets a source. Represented the same way as any route:
            # the hub_key is the sink.
            hub_key = f"hub:{drop.monitor_id}"
            if src_key == hub_key:
                return
            self._fire_reroute(hub_key, src_key)
            return
        if drop.kind == "physical":
            sink_key = f"{drop.machine_id}:{drop.monitor_id}"
            if src_key == sink_key:
                self._fire_reroute(sink_key, sink_key)
                return
            self._fire_reroute(sink_key, src_key)
            return

    def _node_source_key(self, src: _Node) -> Optional[str]:
        if src.kind == "pc":
            primary = self._pc_primary_monitor(src.machine_id)
            if not primary:
                return None
            return f"{src.machine_id}:{primary}"
        if src.kind == "virtual":
            return f"{src.machine_id}:{src.monitor_id}"
        if src.kind == "physical":
            return f"{src.machine_id}:{src.monitor_id}"
        if src.kind == "hub":
            return f"hub:{src.monitor_id}"
        return None

    def _pc_primary_monitor(self, machine_id: str) -> str:
        for d in self._physicals():
            if d.machine_id == machine_id:
                return d.monitor_id
        return ""

    def _fire_reroute(self, sink_key: str, source_key: str) -> None:
        if self._on_reroute is not None:
            self._on_reroute(sink_key, source_key)

    def _pick_machine_for_new_virtual(self, event) -> None:
        if self._on_add_virtual is None:
            return
        menu = tk.Menu(self.canvas, tearoff=0,
                       bg=PAPER_BG, fg=PAPER_TEXT,
                       activebackground=LILAC,
                       activeforeground="#ffffff",
                       bd=1, relief=tk.SOLID)
        menu.add_command(label="Create virtual display for…",
                         state=tk.DISABLED)
        menu.add_separator()
        for mid in self._machines():
            menu.add_command(
                label=mid,
                command=lambda m=mid: self._on_add_virtual(m),
            )
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

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

    def _on_right_click(self, event):
        # Route line → Cut shortcut.
        route_key = self._hit_route_line(event.x, event.y)
        if route_key is not None:
            menu = tk.Menu(self.canvas, tearoff=0,
                           bg=PAPER_BG, fg=PAPER_TEXT,
                           activebackground=LILAC,
                           activeforeground="#ffffff",
                           bd=1, relief=tk.SOLID)
            menu.add_command(
                label=f"{route_key} ← "
                      f"{self._routes.get(route_key, '?')}",
                state=tk.DISABLED)
            menu.add_separator()
            menu.add_command(
                label="Cut route",
                command=lambda k=route_key: self._fire_reroute(k, k),
            )
            try:
                menu.tk_popup(event.x_root, event.y_root)
            finally:
                menu.grab_release()
            return

        node = self._hit_node(event.x, event.y)
        if node is None:
            return
        if node.kind == "virtual":
            menu = tk.Menu(self.canvas, tearoff=0,
                           bg=PAPER_BG, fg=PAPER_TEXT,
                           activebackground=LILAC,
                           activeforeground="#ffffff",
                           bd=1, relief=tk.SOLID)
            menu.add_command(
                label=f"{node.machine_id}:{node.monitor_id}",
                state=tk.DISABLED)
            menu.add_separator()
            if self._on_remove_virtual is not None:
                menu.add_command(
                    label="Remove virtual",
                    command=lambda m=node.machine_id, mon=node.monitor_id:
                        self._on_remove_virtual(m, mon),
                )
            try:
                menu.tk_popup(event.x_root, event.y_root)
            finally:
                menu.grab_release()
        elif node.kind == "hub":
            menu = tk.Menu(self.canvas, tearoff=0,
                           bg=PAPER_BG, fg=PAPER_TEXT,
                           activebackground=LILAC,
                           activeforeground="#ffffff",
                           bd=1, relief=tk.SOLID)
            menu.add_command(
                label=f"Hub · {node.monitor_id}",
                state=tk.DISABLED)
            menu.add_separator()
            if self._on_remove_hub is not None:
                menu.add_command(
                    label="Remove hub",
                    command=lambda h=node.monitor_id:
                        self._on_remove_hub(h),
                )
            try:
                menu.tk_popup(event.x_root, event.y_root)
            finally:
                menu.grab_release()

    def _on_delete_key(self, _event=None):
        if self._selected_route:
            sink_key = self._selected_route
            self._selected_route = None
            self._fire_reroute(sink_key, sink_key)

    # ── Adjacency (bottom area only) ──────────────────────────

    def _overlaps_other(self, d: DisplayInfo) -> bool:
        for o in self._physicals():
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
        for o in self._physicals():
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

    # ── Actions ───────────────────────────────────────────────

    def _do_apply(self) -> None:
        if not self._dirty or self._on_apply is None:
            return
        layout = [
            {"machine_id": d.machine_id, "monitor_id": d.monitor_id,
             "global_x": d.global_x, "global_y": d.global_y}
            for d in self._physicals()
        ]
        self._on_apply(layout)

    def _do_identify(self) -> None:
        if self._on_identify:
            self._on_identify()

    def _do_reset(self) -> None:
        if not self.original_displays:
            return
        by_key = {(d.machine_id, d.monitor_id): d
                  for d in self.original_displays}
        for d in self.displays:
            if d.virtual:
                continue
            o = by_key.get((d.machine_id, d.monitor_id))
            if o is None:
                continue
            d.global_x = o.global_x
            d.global_y = o.global_y
        self._dirty = False
        self._set_apply_enabled(False)
        _number_physicals(self.displays)
        self._fit_view()


def _copy(d: DisplayInfo) -> DisplayInfo:
    return DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})


def _number_physicals(displays: list[DisplayInfo]) -> None:
    phys = [d for d in displays if not d.virtual]
    by_machine: dict[str, list[DisplayInfo]] = {}
    for d in phys:
        by_machine.setdefault(d.machine_id, []).append(d)
    order = sorted(by_machine.keys())
    n = 1
    for mid in order:
        for d in sorted(by_machine[mid], key=lambda d: d.monitor_id):
            d.number = n
            n += 1
    # Virtuals don't get numbers.
    for d in displays:
        if d.virtual:
            d.number = 0

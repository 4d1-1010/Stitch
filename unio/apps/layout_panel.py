"""Embeddable layout canvas — drag-and-drop monitor arrangement.

Extracted from configurator.py so the shell's Layout tab can embed
the same canvas + buttons without spinning up a separate top-level
window. The old ConfiguratorApp still exists for the current
launcher's Host path; it will be retired once the shell rewrite
replaces that flow end-to-end.

The panel is a pure widget: it renders what set_displays() is
handed and fires callbacks when the user clicks Apply / Identify /
Reset. Any networking lives in the shell.
"""

from __future__ import annotations

import logging
import tkinter as tk
from dataclasses import dataclass
from typing import Callable, Optional

from .ui_theme import (
    FONT_SANS, LILAC, PAPER_BG, PAPER_BORDER, PAPER_MUTED, PAPER_SURFACE,
    PAPER_TEXT, SIZE_BASE, SIZE_LG, SIZE_SM, SIZE_XS,
    SPACE_LG, SPACE_MD, SPACE_SM, SPACE_XS,
    PillButton,
)


# Eight distinct colors for up-to-eight connected machines. Paired to
# the paper background: saturated enough to pop on the light canvas,
# muted enough not to scream.
MACHINE_COLORS = [
    "#8b7bff",  # lilac (matches primary accent)
    "#5cc9a3",  # mint
    "#e8b04c",  # amber
    "#ff6b5b",  # coral
    "#4a90d9",  # cornflower
    "#9b59b6",  # plum
    "#1abc9c",  # teal
    "#e67e22",  # orange
]

SNAP_THRESHOLD = 20            # pixels, in canvas space
CANVAS_BG = "#f8f9fc"          # subtle off-white so displays have a seam

log = logging.getLogger(__name__)


@dataclass
class DisplayInfo:
    machine_id: str
    monitor_id: str
    global_x: int
    global_y: int
    width: int
    height: int
    number: int = 0


def _blend(fg_hex: str, alpha: float, bg_hex: str = CANVAS_BG) -> str:
    """Alpha-blend fg over bg at the given alpha. tkinter rectangles
    don't support an alpha channel, so we pre-mix to produce a solid
    fill that looks translucent against the canvas backdrop."""
    fg = bytes.fromhex(fg_hex.lstrip("#"))
    bg = bytes.fromhex(bg_hex.lstrip("#"))
    mixed = tuple(
        int(round(f * alpha + b * (1 - alpha)))
        for f, b in zip(fg, bg)
    )
    return f"#{mixed[0]:02x}{mixed[1]:02x}{mixed[2]:02x}"


class LayoutPanel(tk.Frame):
    """Drag-and-drop display arrangement canvas + action buttons."""

    def __init__(self, parent: tk.Widget,
                 *,
                 on_apply: Optional[Callable[[list[dict]], None]] = None,
                 on_identify: Optional[Callable[[], None]] = None):
        super().__init__(parent, bg=PAPER_BG)

        self._on_apply = on_apply
        self._on_identify = on_identify

        self.displays: list[DisplayInfo] = []
        self.original_displays: list[DisplayInfo] = []
        self._color_map: dict[str, str] = {}
        self._color_idx = 0
        self._active_machine: str = ""

        self._drag_display: Optional[DisplayInfo] = None
        self._drag_offset = (0, 0)
        self._dirty = False

        self._scale = 0.15
        self._pan_x = 0.0
        self._pan_y = 0.0

        self._build_ui()

    # ── UI construction ──────────────────────────────────────────

    def _build_ui(self) -> None:
        # Header above the canvas: short explainer.
        header = tk.Frame(self, bg=PAPER_BG)
        header.pack(fill=tk.X, padx=SPACE_LG, pady=(SPACE_LG, SPACE_SM))
        tk.Label(
            header, text="Layout",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(anchor="w")
        tk.Label(
            header,
            text="Drag each display so it matches how your monitors sit "
                 "physically. The cursor crosses where the edges touch.",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(anchor="w", pady=(2, 0))

        # Canvas — the drag surface.
        canvas_wrap = tk.Frame(self, bg=PAPER_BORDER,
                               padx=1, pady=1)
        canvas_wrap.pack(fill=tk.BOTH, expand=True,
                         padx=SPACE_LG, pady=(0, SPACE_MD))
        self.canvas = tk.Canvas(canvas_wrap, bg=CANVAS_BG,
                                highlightthickness=0, bd=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        self.canvas.bind("<ButtonPress-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        # <Configure> fires on every canvas resize — including the
        # first mapping. Re-fit when that happens so displays fed in
        # before the canvas had a real size still land in the view.
        self.canvas.bind("<Configure>",
                         lambda e: self._on_canvas_configure())

        # Zoom bindings — Tk delivers <MouseWheel> on Windows/macOS
        # with event.delta carrying the notch count, and separate
        # Button-4/Button-5 events on Linux X11. Cover all three so
        # the user can scroll to zoom on any platform.
        self.canvas.bind("<MouseWheel>", self._on_wheel_zoom)
        self.canvas.bind("<Button-4>", lambda e: self._zoom_at(e.x, e.y, 1.1))
        self.canvas.bind("<Button-5>", lambda e: self._zoom_at(e.x, e.y, 1 / 1.1))
        # Windows sends MouseWheel to the focused widget, not the
        # widget under the cursor. Grab focus whenever the pointer
        # enters the canvas so wheel events land here without the
        # user needing to click first.
        self.canvas.configure(takefocus=True)
        self.canvas.bind("<Enter>", lambda _e: self.canvas.focus_set())

        # Empty-state label shown when no displays are available yet.
        self._empty_label = tk.Label(
            self.canvas,
            text="Connect to a session to see displays here.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=CANVAS_BG,
        )

        # Action row — Identify / Apply / Reset.
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

        # Zoom cluster on the right — reset-to-fit + two step buttons.
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

        self._set_apply_enabled(False)

    # ── Public API ───────────────────────────────────────────────

    def set_displays(self, monitors: list[dict]) -> None:
        """Replace the displayed layout with the given monitor list."""
        if self._drag_display is not None:
            # User is mid-drag — don't clobber their work.
            return

        self.displays = [
            DisplayInfo(
                machine_id=m["machine_id"],
                monitor_id=m["monitor_id"],
                global_x=m["global_x"],
                global_y=m["global_y"],
                width=m["width"],
                height=m["height"],
            )
            for m in monitors
            if not str(m.get("machine_id", "")).startswith("__")
        ]
        log.info("LayoutPanel.set_displays: %d display(s) loaded",
                 len(self.displays))
        _number_displays(self.displays)
        self.displays.sort(key=lambda d: d.number)

        if not self._dirty:
            self.original_displays = [_copy(d) for d in self.displays]

        active_ids = {d.machine_id for d in self.displays}
        for stale in list(self._color_map.keys()):
            if stale not in active_ids:
                del self._color_map[stale]

        self._update_empty_state()
        self._fit_view()

    def mark_clean(self) -> None:
        """Called after a successful Apply — snapshot current as original."""
        self.original_displays = [_copy(d) for d in self.displays]
        self._dirty = False
        self._set_apply_enabled(False)

    def set_active_machine(self, machine_id: str) -> None:
        """Highlight the machine currently holding the shared cursor."""
        if machine_id == self._active_machine:
            return
        self._active_machine = machine_id
        self._redraw()

    def _on_canvas_configure(self) -> None:
        # Canvas just got a new size. If displays arrived while the
        # canvas was still un-mapped (width 1), their _fit_view retry
        # loop can stall — forcing another fit here guarantees the
        # display rectangles land inside the visible area on every
        # resize including the very first mapping.
        if self.displays:
            self._fit_view()
        else:
            self._redraw()

    # ── Internal helpers ─────────────────────────────────────────

    def _get_color(self, machine_id: str) -> str:
        if machine_id not in self._color_map:
            self._color_map[machine_id] = MACHINE_COLORS[
                self._color_idx % len(MACHINE_COLORS)
            ]
            self._color_idx += 1
        return self._color_map[machine_id]

    def _set_apply_enabled(self, enabled: bool) -> None:
        # PillButton doesn't have a disabled state yet; gate the
        # command and dim visually.
        if enabled:
            self._btn_apply.configure(fg="#ffffff", bg=LILAC)
        else:
            self._btn_apply.configure(fg=PAPER_MUTED, bg=PAPER_SURFACE)

    def _update_empty_state(self) -> None:
        if self.displays:
            self._empty_label.place_forget()
        else:
            self._empty_label.place(relx=0.5, rely=0.5, anchor="center")

    # ── View transform ───────────────────────────────────────────

    def _fit_view(self) -> None:
        if not self.displays:
            self._redraw()
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        if cw < 50 or ch < 50:
            log.debug("LayoutPanel._fit_view deferred — canvas %dx%d", cw, ch)
            self.after(50, self._fit_view)
            return
        log.info("LayoutPanel._fit_view: canvas %dx%d, %d display(s)",
                 cw, ch, len(self.displays))
        padding = 60

        min_x = min(d.global_x for d in self.displays)
        min_y = min(d.global_y for d in self.displays)
        max_x = max(d.global_x + d.width for d in self.displays)
        max_y = max(d.global_y + d.height for d in self.displays)

        bw = max(max_x - min_x, 1)
        bh = max(max_y - min_y, 1)

        avail_w = max(cw - padding * 2, 1)
        avail_h = max(ch - padding * 2, 1)
        self._scale = min(avail_w / bw, avail_h / bh, 0.4)
        self._pan_x = (cw - bw * self._scale) / 2 - min_x * self._scale
        self._pan_y = (ch - bh * self._scale) / 2 - min_y * self._scale
        self._redraw()

    def _on_wheel_zoom(self, event) -> None:
        # Tk reports event.delta as a signed multiple of 120 on
        # Windows and an arbitrary signed float on macOS; a positive
        # value is scroll-up (zoom in), negative is scroll-down.
        factor = 1.1 if event.delta > 0 else 1 / 1.1
        self._zoom_at(event.x, event.y, factor)

    def _zoom_centered(self, factor: float) -> None:
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        self._zoom_at(cw // 2, ch // 2, factor)

    def _zoom_at(self, sx: int, sy: int, factor: float) -> None:
        """Scale by factor, keeping the canvas point (sx, sy) stationary.
        Clamped to a sensible range so the user can't zoom so far the
        displays disappear or the canvas lockups chasing pixel math."""
        if not self.displays:
            return
        new_scale = self._scale * factor
        new_scale = max(0.01, min(new_scale, 2.0))
        if new_scale == self._scale:
            return
        # Keep (sx, sy) in screen space mapped to the same global
        # point before and after scaling.
        gx = (sx - self._pan_x) / self._scale
        gy = (sy - self._pan_y) / self._scale
        self._scale = new_scale
        self._pan_x = sx - gx * self._scale
        self._pan_y = sy - gy * self._scale
        self._redraw()

    def _to_screen(self, gx: float, gy: float) -> tuple[float, float]:
        return (gx * self._scale + self._pan_x,
                gy * self._scale + self._pan_y)

    def _to_global(self, sx: float, sy: float) -> tuple[float, float]:
        return ((sx - self._pan_x) / self._scale,
                (sy - self._pan_y) / self._scale)

    # ── Drawing ──────────────────────────────────────────────────

    def _redraw(self) -> None:
        c = self.canvas
        c.delete("all")
        cw = c.winfo_width()
        ch = c.winfo_height()

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
                    c.create_line(sx, 0, sx, ch, fill="#edeff4", width=1)
                gx += step_g
            gy = start_gy
            while True:
                sy = gy * self._scale + self._pan_y
                if sy > ch:
                    break
                if sy >= 0:
                    c.create_line(0, sy, cw, sy, fill="#edeff4", width=1)
                gy += step_g

        for d in self.displays:
            sx, sy = self._to_screen(d.global_x, d.global_y)
            sw = d.width * self._scale
            sh = d.height * self._scale
            color = self._get_color(d.machine_id)
            is_dragging = (d is self._drag_display)
            is_active = (d.machine_id == self._active_machine
                         and self._active_machine != "")

            # Fill alphas: 0.08 was too faint to distinguish from the
            # near-white canvas bg, making the tab look empty even
            # when displays were there. Bumped so rectangles actually
            # read as blocks.
            fill = _blend(color, 0.38 if is_active
                          else (0.30 if is_dragging else 0.20))
            border_w = 4 if (is_dragging or is_active) else 3

            c.create_rectangle(sx, sy, sx + sw, sy + sh,
                               fill=fill, outline=color, width=border_w)

            # Just number + PC name. Monitor id / resolution / "cursor
            # here" chip removed — too noisy per-box. Anchor the number
            # above center and the name below so they can't overlap at
            # any zoom.
            cx = sx + sw / 2
            cy = sy + sh / 2
            show_label = sh > 40 and sw > 60

            num_size = max(6, min(56, int(min(sh, sw) * 0.30)))
            c.create_text(cx, cy - (2 if show_label else 0),
                          text=str(d.number),
                          anchor="s" if show_label else "center",
                          font=(FONT_SANS, num_size, "bold"),
                          fill=PAPER_TEXT)

            if show_label:
                lbl_size = max(6, min(16, int(sh * 0.08)))
                c.create_text(cx, cy + 2,
                              text=d.machine_id, anchor="n",
                              font=(FONT_SANS, lbl_size), fill=PAPER_MUTED)

    # ── Drag handling ────────────────────────────────────────────

    def _hit_test(self, sx: float, sy: float) -> Optional[DisplayInfo]:
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

    def _on_release(self, _event):
        if self._drag_display:
            self._drag_display = None
            self.canvas.config(cursor="")
            self._redraw()

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

    # ── Button handlers ──────────────────────────────────────────

    def _do_apply(self) -> None:
        if not self._dirty or self._on_apply is None:
            return
        if self._isolated_displays():
            # Can't cross the cursor onto a display that isn't touching
            # any other — refuse the apply instead of shipping a layout
            # the cursor can't traverse.
            from tkinter import messagebox
            names = ", ".join(
                f"{d.machine_id} {d.monitor_id}"
                for d in self._isolated_displays()
            )
            messagebox.showwarning(
                "Isolated display",
                f"These displays aren't touching any other display:\n\n"
                f"{names}\n\n"
                "Drag them so an edge lines up with another display, "
                "then apply again.",
                parent=self,
            )
            return
        layout = [
            {"machine_id": d.machine_id, "monitor_id": d.monitor_id,
             "global_x": d.global_x, "global_y": d.global_y}
            for d in self.displays
        ]
        self._on_apply(layout)

    def _isolated_displays(self) -> list[DisplayInfo]:
        """Displays that share no edge with any other — the cursor
        can't cross on or off them. With a single display there's
        nothing to isolate from, so report none in that case."""
        if len(self.displays) < 2:
            return []
        return [d for d in self.displays if not self._has_neighbour(d)]

    # Minimum fraction of each edge that must overlap before we count
    # two displays as "touching". Corner-only contact is too thin for
    # the cursor to realistically cross, and synergy-style apps pick
    # a similar threshold to avoid accidentally-adjacent layouts.
    _MIN_EDGE_SHARE = 1 / 3

    def _has_neighbour(self, d: DisplayInfo) -> bool:
        for o in self.displays:
            if o is d:
                continue
            # Horizontal adjacency — one's right meets the other's
            # left. Threshold is 1/3 of the SHORTER edge so a tiny
            # display can still be a neighbour to a 4K without being
            # forced to cover an impossible fraction of it.
            if (d.global_x + d.width == o.global_x
                    or o.global_x + o.width == d.global_x):
                overlap = (min(d.global_y + d.height, o.global_y + o.height)
                           - max(d.global_y, o.global_y))
                if overlap >= min(d.height, o.height) * self._MIN_EDGE_SHARE:
                    return True
            # Vertical adjacency — symmetric in the other axis.
            if (d.global_y + d.height == o.global_y
                    or o.global_y + o.height == d.global_y):
                overlap = (min(d.global_x + d.width, o.global_x + o.width)
                           - max(d.global_x, o.global_x))
                if overlap >= min(d.width, o.width) * self._MIN_EDGE_SHARE:
                    return True
        return False

    def _do_identify(self) -> None:
        if self._on_identify:
            self._on_identify()


def _copy(d: DisplayInfo) -> DisplayInfo:
    return DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})


def _number_displays(displays: list[DisplayInfo]) -> None:
    """Assign display.number in-place. Groups monitors by machine so
    each PC's overlays are consecutive (1..k on PC A, k+1..n on PC B)
    rather than interleaved when the machines' monitors happen to
    overlap vertically. Machines are ordered by their leftmost
    monitor's global_x so the numbering still walks left-to-right
    across the arrangement.

    Matches the server's _trigger_identify numbering — change both
    together or the Layout canvas labels and the Identify overlays
    will fall out of sync.
    """
    by_machine: dict[str, list[DisplayInfo]] = {}
    for d in displays:
        by_machine.setdefault(d.machine_id, []).append(d)
    machine_order = sorted(
        by_machine.keys(),
        key=lambda mid: (
            min(d.global_x for d in by_machine[mid]),
            min(d.global_y for d in by_machine[mid]),
            mid,
        ),
    )
    n = 1
    for mid in machine_order:
        for d in sorted(by_machine[mid],
                        key=lambda d: (d.global_y, d.global_x)):
            d.number = n
            n += 1

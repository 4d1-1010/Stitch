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


# Procedural per-machine colour. Everything lives in the lilac
# family so the Layout canvas reads as "nuances of the brand colour"
# rather than a rainbow. Three hash-derived knobs — hue offset around
# lilac (±30°), saturation, lightness — give enough variation that
# human eyes can always tell peers apart, without ever leaving the
# paper+lilac palette.
_LILAC_HUE = 248.0 / 360.0     # matches #8b7bff
_HUE_SPAN = 60.0 / 360.0       # ±30° around lilac — cool purples only
_SAT_MIN, _SAT_MAX = 0.28, 0.62
_LIGHT_MIN, _LIGHT_MAX = 0.58, 0.80


def machine_color(machine_id: str) -> str:
    """Deterministic lilac-family colour for a machine_id. Same peer
    → same colour on every PC; adjacent machine_ids land on
    distinguishable points in (hue, saturation, lightness) but
    always stay as nuances of lilac / paper. No palette cap."""
    seed = zlib.crc32(machine_id.encode("utf-8")) & 0xFFFFFFFF
    # Split the 32-bit hash into three independent knobs so hue and
    # saturation vary together instead of all three tracking the
    # same LSBs.
    h_part = ((seed >> 20) & 0xFFF) / 0xFFF   # 0..1
    s_part = ((seed >> 10) & 0x3FF) / 0x3FF
    l_part = (seed & 0x3FF) / 0x3FF

    hue = (_LILAC_HUE + (h_part - 0.5) * _HUE_SPAN) % 1.0
    sat = _SAT_MIN + s_part * (_SAT_MAX - _SAT_MIN)
    light = _LIGHT_MIN + l_part * (_LIGHT_MAX - _LIGHT_MIN)
    r, g, b = colorsys.hls_to_rgb(hue, light, sat)
    return f"#{int(r * 255):02x}{int(g * 255):02x}{int(b * 255):02x}"


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
                 on_identify: Optional[Callable[[], None]] = None,
                 on_reroute: Optional[Callable[[str, str], None]] = None,
                 sources_provider: Optional[Callable[[], list[dict]]] = None):
        super().__init__(parent, bg=PAPER_BG)

        self._on_apply = on_apply
        self._on_identify = on_identify
        # Phase 4: called when the user picks a new source for a sink,
        # either via right-click menu or drag-onto-rectangle. Args are
        # (sink_key, source_key) using the "machine_id:monitor_id"
        # format shared with the shell's _workspace_routes.
        self._on_reroute = on_reroute
        # Phase 4: asked to enumerate every source in the mesh when
        # the right-click menu opens. Returning a fresh list per call
        # means the menu always reflects the current peer set.
        self._sources_provider = sources_provider

        self.displays: list[DisplayInfo] = []
        self.original_displays: list[DisplayInfo] = []
        self._active_machine: str = ""

        # Phase 4: per-sink_key -> source_key override map for the
        # currently-rendered workspace. Empty == identity routing for
        # every sink. Drawn as a corner badge on each rectangle.
        self._routes: dict[str, str] = {}
        # Hover hint used during drag-onto-rectangle: when a user drags
        # one display over another, the hovered one highlights as the
        # pending sink (it will receive the dragged display as its
        # source on drop).
        self._route_drop_target: Optional[DisplayInfo] = None

        self._drag_display: Optional[DisplayInfo] = None
        self._drag_offset = (0, 0)
        # Phase 4: distinguishes "moving the rectangle to rearrange
        # the layout" (the legacy drag) from "using the rectangle as a
        # source to drop onto another rectangle" (the new patch-matrix
        # gesture). Defaults to layout-move; Shift-drag switches to
        # patch mode so the user opts in per gesture.
        self._drag_mode: str = "layout"  # "layout" | "patch"
        self._dirty = False

        # Pan session: click on empty canvas (or middle-click) drags
        # the view. Holds (mouse_x, mouse_y, starting pan_x, pan_y).
        self._pan_start: Optional[tuple[int, int, float, float]] = None

        self._scale = 0.15
        self._pan_x = 0.0
        self._pan_y = 0.0

        self._build_ui()

    # ── UI construction ──────────────────────────────────────────

    def _build_ui(self) -> None:
        # No "Layout" page-title (the tab rail already says it), but
        # keep the short "drag each display…" hint above the canvas
        # so first-time users understand what the rectangles do.
        header = tk.Frame(self, bg=PAPER_BG)
        header.pack(fill=tk.X, padx=SPACE_LG, pady=(0, SPACE_SM))
        tk.Label(
            header,
            text="Drag each display so it matches how your monitors "
                 "sit physically. The cursor crosses where the edges "
                 "touch.",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
            wraplength=680, justify="left", anchor="w",
        ).pack(anchor="w")

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
        # Shift-drag starts a patch gesture (drop source onto sink to
        # rewire a route). Shift+Button-1 fires as <Shift-ButtonPress-1>
        # on every platform.
        self.canvas.bind("<Shift-ButtonPress-1>", self._on_patch_press)
        self.canvas.bind("<Shift-B1-Motion>", self._on_patch_drag)
        self.canvas.bind("<Shift-ButtonRelease-1>", self._on_patch_release)
        # Right-click opens the "Show here: …" menu on the rectangle
        # underneath. Button-3 on X11, also on Windows / macOS via Tk.
        self.canvas.bind("<ButtonPress-3>", self._on_right_click)
        # Middle-click always pans (matches graphics-app conventions).
        # Left-click on empty canvas space also pans — the press
        # handler routes based on whether the hit lands on a monitor.
        self.canvas.bind("<ButtonPress-2>", self._on_pan_press)
        self.canvas.bind("<B2-Motion>", self._on_pan_motion)
        self.canvas.bind("<ButtonRelease-2>", self._on_pan_release)
        # <Configure> fires on every canvas resize — including the
        # first mapping. Re-fit when that happens so displays fed in
        # before the canvas had a real size still land in the view.
        self.canvas.bind("<Configure>",
                         lambda e: self._on_canvas_configure())

        # Zoom bindings. Tk routes <MouseWheel> to the focused widget
        # on Windows, and we can't rely on the canvas having focus
        # while the user's cursor is over it. bind_all while the
        # pointer is inside the canvas grabs the wheel events
        # regardless of focus, then unbind on leave so the rest of
        # the app keeps its normal scroll behaviour.
        self.canvas.bind("<Enter>", self._on_canvas_enter)
        self.canvas.bind("<Leave>", self._on_canvas_leave)

        # Empty-state label shown when no displays are available yet.
        # Placed immediately so the tab reads as "waiting for a
        # session" instead of blank before any LAYOUT_UPDATE arrives.
        self._empty_label = tk.Label(
            self.canvas,
            text="Connect to a session to see your displays here.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=CANVAS_BG,
        )
        self._empty_label.place(relx=0.5, rely=0.5, anchor="center")

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

        self._btn_reset = PillButton(
            actions, "Reset",
            variant="secondary", command=self._do_reset,
        )
        self._btn_reset.pack(side=tk.LEFT)

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
        # Buttons start inert — they only make sense once we have
        # displays to act on. set_displays will re-enable them.
        self._set_session_controls_enabled(False)

    # ── Public API ───────────────────────────────────────────────

    def set_displays(self, monitors: list[dict]) -> None:
        """Replace the displayed layout with the given monitor list."""
        if self._drag_display is not None:
            # User is mid-drag — don't clobber their work.
            return

        # No-op guard: if the incoming monitor list is byte-identical to
        # what's already on the canvas, skip the fit + redraw. Without
        # this, every spurious "state changed" callback from the peer
        # mesh would refit and repaint the canvas, producing a visible
        # flicker even when nothing actually moved.
        sig = tuple(
            (str(m.get("machine_id", "")), str(m.get("monitor_id", "")),
             int(m.get("global_x", 0)), int(m.get("global_y", 0)),
             int(m.get("width", 0)), int(m.get("height", 0)))
            for m in monitors
            if not str(m.get("machine_id", "")).startswith("__")
        )
        if sig == getattr(self, "_last_displays_sig", None) and not self._dirty:
            return
        self._last_displays_sig = sig

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

        self._set_session_controls_enabled(bool(self.displays))
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

    def set_routes(self, routes: dict[str, str]) -> None:
        """Phase 4: set the current workspace's sink→source override
        map. Redraws badges but nothing else; the rectangles
        themselves are unaffected."""
        clean = {str(k): str(v) for k, v in (routes or {}).items()}
        if clean == self._routes:
            return
        self._routes = clean
        self._redraw()

    def _screen_key(self, d: DisplayInfo) -> str:
        return f"{d.machine_id}:{d.monitor_id}"

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
        return machine_color(machine_id)

    def _set_apply_enabled(self, enabled: bool) -> None:
        self._set_button_enabled(self._btn_apply, enabled, active="primary")

    def _set_session_controls_enabled(self, enabled: bool) -> None:
        """Identify / Reset / zoom buttons only make sense with
        displays. Dim + inhibit their commands until set_displays
        arrives."""
        # Identify / Reset share the same secondary paper look as
        # Apply-when-disabled so the action row reads consistently.
        for btn in (self._btn_identify, self._btn_reset):
            self._set_button_enabled(btn, enabled, active="secondary")
        for btn in (self._btn_zoom_in, self._btn_zoom_out,
                    self._btn_zoom_fit):
            self._set_button_enabled(btn, enabled, active="ghost")
        if not enabled:
            self._set_apply_enabled(False)

    def _set_button_enabled(self, btn: PillButton, enabled: bool,
                            active: str = "primary") -> None:
        palettes = {
            "primary": ("#ffffff", LILAC),
            "ghost":   (PAPER_TEXT, PAPER_BG),
            "secondary": (PAPER_TEXT, PAPER_SURFACE),
        }
        fg, bg = palettes.get(active, palettes["primary"])
        if enabled:
            btn._bg, btn._fg = bg, fg  # restore hover-out colors
            btn.configure(fg=fg, bg=bg, cursor="hand2")
            btn._enabled = True
        else:
            # Disabled state keeps the dark PAPER_TEXT so labels stay
            # legible; the subtle paper-surface fill is enough to read
            # as "inactive" without washing the text out.
            btn._bg, btn._fg = PAPER_SURFACE, PAPER_TEXT
            btn.configure(fg=PAPER_TEXT, bg=PAPER_SURFACE, cursor="arrow")
            btn._enabled = False

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

    def _on_canvas_enter(self, _e=None) -> None:
        # bind_all with add='' (replace) so multiple enters don't
        # stack handlers. The lambdas translate the pointer coords
        # into canvas-local space before dispatching.
        self.canvas.bind_all("<MouseWheel>", self._on_wheel_zoom)
        self.canvas.bind_all("<Button-4>", self._on_wheel_up)
        self.canvas.bind_all("<Button-5>", self._on_wheel_down)

    def _on_canvas_leave(self, _e=None) -> None:
        self.canvas.unbind_all("<MouseWheel>")
        self.canvas.unbind_all("<Button-4>")
        self.canvas.unbind_all("<Button-5>")

    def _canvas_xy(self, event) -> tuple[int, int]:
        # event.x / event.y are relative to whichever widget received
        # the event; translate to canvas-local coords so the zoom
        # focal point stays under the cursor.
        try:
            rx = self.canvas.winfo_rootx()
            ry = self.canvas.winfo_rooty()
            return event.x_root - rx, event.y_root - ry
        except tk.TclError:
            return event.x, event.y

    def _on_wheel_zoom(self, event) -> None:
        # Tk reports event.delta as a signed multiple of 120 on
        # Windows and an arbitrary signed float on macOS; a positive
        # value is scroll-up (zoom in), negative is scroll-down.
        x, y = self._canvas_xy(event)
        factor = 1.1 if event.delta > 0 else 1 / 1.1
        self._zoom_at(x, y, factor)

    def _on_wheel_up(self, event) -> None:
        x, y = self._canvas_xy(event)
        self._zoom_at(x, y, 1.1)

    def _on_wheel_down(self, event) -> None:
        x, y = self._canvas_xy(event)
        self._zoom_at(x, y, 1 / 1.1)

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
            is_drop_target = (d is self._route_drop_target)

            # Route state for this rectangle:
            #  * projected — this display is the SOURCE of a route
            #    that some remote sink is showing (hatched appearance,
            #    the user can't trust that their windows are visible
            #    here any more).
            #  * remote_sink — this display is the SINK of a route
            #    pointing at someone else's source (shows their PC's
            #    pixels, badged "← from X").
            sink_key = self._screen_key(d)
            route_src = self._routes.get(sink_key)
            is_remote_sink = bool(route_src and route_src != sink_key)
            is_projected = any(
                src == sink_key and sink != sink_key
                for sink, src in self._routes.items()
            )

            # Fill alphas: 0.08 was too faint to distinguish from the
            # near-white canvas bg, making the tab look empty even
            # when displays were there. Bumped so rectangles actually
            # read as blocks.
            fill = _blend(color, 0.38 if is_active
                          else (0.30 if is_dragging else 0.20))
            border_w = 4 if (is_dragging or is_active or is_drop_target) else 3
            outline = "#4a9b6e" if is_drop_target else color

            c.create_rectangle(sx, sy, sx + sw, sy + sh,
                               fill=fill, outline=outline, width=border_w)

            # Hatching for projected sources — a cheap stipple of
            # diagonal lines drawn on top of the fill so the user can
            # tell at a glance that a display isn't reachable.
            if is_projected:
                step = max(8, int(min(sw, sh) / 14))
                hatch_color = _blend(color, 0.55)
                x0 = int(sx)
                x1 = int(sx + sw)
                y0 = int(sy)
                y1 = int(sy + sh)
                # Only clip at edges — Tk happily draws off-screen,
                # but we stay tidy for visual reasons.
                for offset in range(-int(sh), int(sw), step):
                    lx0 = max(x0, x0 + offset)
                    ly0 = y0 + max(0, -offset)
                    lx1 = min(x1, x0 + offset + int(sh))
                    ly1 = y0 + min(int(sh),
                                   int(sh) - (lx1 - (x0 + offset)) + int(sh))
                    c.create_line(
                        x0 + offset, y0,
                        x0 + offset + int(sh), y0 + int(sh),
                        fill=hatch_color, width=1,
                    )

            # Number + PC name, centred as one block in the rectangle.
            # The previous "anchor=s / anchor=n around cy" variant put
            # the number's baseline above cy and the label's top below
            # cy — visually the big number pushed the stack upward,
            # which the user read as "not centered". Compute the
            # combined stack height and offset by half so the two
            # texts straddle the true centre.
            cx = sx + sw / 2
            cy = sy + sh / 2
            show_label = sh > 40 and sw > 60

            num_size = max(6, min(56, int(min(sh, sw) * 0.30)))
            if show_label:
                lbl_size = max(6, min(16, int(sh * 0.08)))
                gap = 4
                total_h = num_size + gap + lbl_size
                top_y = cy - total_h / 2
                num_y = top_y + num_size / 2
                lbl_y = top_y + num_size + gap + lbl_size / 2
                c.create_text(cx, num_y, text=str(d.number),
                              anchor="center",
                              font=(FONT_SANS, num_size, "bold"),
                              fill=PAPER_TEXT)
                c.create_text(cx, lbl_y, text=d.machine_id,
                              anchor="center",
                              font=(FONT_SANS, lbl_size),
                              fill=PAPER_MUTED)
            else:
                c.create_text(cx, cy, text=str(d.number),
                              anchor="center",
                              font=(FONT_SANS, num_size, "bold"),
                              fill=PAPER_TEXT)

            # Route badge — small pill in the top-right corner showing
            # what this display is currently doing re: streaming.
            # Rendered last so it sits above the hatching + number.
            badge_text = None
            badge_bg = None
            if is_remote_sink:
                src_label = route_src.split(":", 1)[-1] if route_src else "?"
                src_mid = (route_src or "").split(":", 1)[0]
                badge_text = f"← {src_mid}"
                badge_bg = _blend(PAPER_TEXT, 0.85, bg_hex=color)
            elif is_projected:
                # Use the first sink that points here as the label —
                # there's only one in v1 (no multicast), so this is
                # unambiguous.
                for sink, src in self._routes.items():
                    if src == sink_key and sink != sink_key:
                        sink_mid = sink.split(":", 1)[0]
                        badge_text = f"→ {sink_mid}"
                        break
                badge_bg = _blend(color, 0.75)
            if badge_text and sw > 80 and sh > 38:
                badge_font_size = max(9, min(12, int(sh * 0.06)))
                padding_x = 6
                padding_y = 3
                tid = c.create_text(
                    sx + sw - padding_x - 4,
                    sy + padding_y + 4,
                    text=badge_text,
                    anchor="ne",
                    font=(FONT_SANS, badge_font_size, "bold"),
                    fill="#ffffff",
                )
                # Tk's canvas doesn't support rounded rects out of the
                # box; draw a solid rectangle behind the text and
                # raise the text above it.
                bx0, by0, bx1, by1 = c.bbox(tid)
                c.create_rectangle(
                    bx0 - padding_x, by0 - padding_y,
                    bx1 + padding_x, by1 + padding_y,
                    fill=badge_bg, outline="",
                )
                c.tag_raise(tid)

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
        else:
            # Empty canvas space — start a pan session. We capture the
            # starting mouse + pan position so _on_drag can compute a
            # delta-based translation on every motion event.
            self._pan_start = (event.x, event.y,
                               self._pan_x, self._pan_y)
            self.canvas.config(cursor="fleur")

    def _on_drag(self, event):
        if self._drag_display is not None:
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
        elif self._pan_start is not None:
            sx, sy, base_px, base_py = self._pan_start
            self._pan_x = base_px + (event.x - sx)
            self._pan_y = base_py + (event.y - sy)
            self._redraw()

    def _on_release(self, _event):
        if self._drag_display:
            self._drag_display = None
            self.canvas.config(cursor="")
            self._redraw()
        if self._pan_start is not None:
            self._pan_start = None
            self.canvas.config(cursor="")

    # ── Patch-drag (route one display's source to another) ──────

    def _on_patch_press(self, event):
        d = self._hit_test(event.x, event.y)
        if d is None:
            return
        self._drag_display = d
        self._drag_mode = "patch"
        self._drag_offset = (0, 0)
        self._route_drop_target = None
        self.canvas.config(cursor="crosshair")
        self._redraw()

    def _on_patch_drag(self, event):
        if self._drag_display is None or self._drag_mode != "patch":
            return
        hit = self._hit_test(event.x, event.y)
        # Ignore hits on the source itself — can't patch a display
        # into itself (that's identity routing, set_route will just
        # clear the override).
        if hit is self._drag_display:
            hit = None
        if hit is not self._route_drop_target:
            self._route_drop_target = hit
            self._redraw()

    def _on_patch_release(self, event):
        if self._drag_display is None or self._drag_mode != "patch":
            self._drag_mode = "layout"
            return
        source = self._drag_display
        sink = self._hit_test(event.x, event.y)
        self._drag_display = None
        self._route_drop_target = None
        self._drag_mode = "layout"
        self.canvas.config(cursor="")
        self._redraw()
        if sink is None or sink is source:
            return
        if self._on_reroute is None:
            return
        self._on_reroute(self._screen_key(sink),
                         self._screen_key(source))

    # ── Right-click "Show here: …" menu ─────────────────────────

    def _on_right_click(self, event):
        hit = self._hit_test(event.x, event.y)
        if hit is None:
            return
        sink_key = self._screen_key(hit)
        sources = self._sources_provider() if self._sources_provider \
            else [{"machine_id": d.machine_id,
                   "monitor_id": d.monitor_id}
                  for d in self.displays]
        menu = tk.Menu(self.canvas, tearoff=0,
                       bg=PAPER_BG, fg=PAPER_TEXT,
                       activebackground=LILAC,
                       activeforeground="#ffffff",
                       bd=1, relief=tk.SOLID)
        header = f"Show on {hit.machine_id}:{hit.monitor_id}"
        menu.add_command(label=header, state=tk.DISABLED)
        menu.add_separator()
        current = self._routes.get(sink_key, sink_key)
        # Identity entry always present — lets the user clear an
        # override without deleting and recreating the workspace.
        menu.add_command(
            label=f"{'✓ ' if current == sink_key else '   '}"
                  f"{hit.machine_id}:{hit.monitor_id} (native)",
            command=lambda: self._fire_reroute(sink_key, sink_key),
        )
        menu.add_separator()
        for src in sources:
            mid = src.get("machine_id", "")
            mon = src.get("monitor_id", "")
            src_key = f"{mid}:{mon}"
            if src_key == sink_key:
                continue
            mark = "✓ " if current == src_key else "   "
            menu.add_command(
                label=f"{mark}{mid}:{mon}",
                command=lambda k=src_key: self._fire_reroute(sink_key, k),
            )
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    def _fire_reroute(self, sink_key: str, source_key: str) -> None:
        if self._on_reroute is None:
            return
        self._on_reroute(sink_key, source_key)

    # ── Middle-click pan (graphics-app convention) ──────────────

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

    def _do_reset(self) -> None:
        """Revert any in-progress drags back to the last-applied
        layout. Briefly flashes the button lilac as a visual ack."""
        self._flash_button(self._btn_reset, LILAC, "#ffffff", ms=250)
        if not self.original_displays:
            return
        self.displays = [_copy(d) for d in self.original_displays]
        self._dirty = False
        self._set_apply_enabled(False)
        _number_displays(self.displays)
        self.displays.sort(key=lambda d: d.number)
        self._fit_view()

    def _flash_button(self, btn: PillButton, bg: str, fg: str,
                      ms: int = 250) -> None:
        orig_bg, orig_fg = btn._bg, btn._fg
        orig_hover_bg, orig_hover_fg = btn._hover_bg, btn._hover_fg
        btn._bg, btn._fg = bg, fg
        btn._hover_bg, btn._hover_fg = bg, fg
        btn.configure(bg=bg, fg=fg)

        def _restore():
            btn._bg, btn._fg = orig_bg, orig_fg
            btn._hover_bg, btn._hover_fg = orig_hover_bg, orig_hover_fg
            btn.configure(bg=orig_bg, fg=orig_fg)

        try:
            btn.after(ms, _restore)
        except tk.TclError:
            _restore()


def _copy(d: DisplayInfo) -> DisplayInfo:
    return DisplayInfo(**{k: getattr(d, k) for k in d.__dataclass_fields__})


def _number_displays(displays: list[DisplayInfo]) -> None:
    """Assign display.number in-place. Ordered by machine_id then
    monitor_id so the number is *stable* across Apply cycles — the
    user was seeing numbers flip after rearranging because the old
    "leftmost global_x wins" rule made the numbering depend on the
    very positions the user was dragging around.

    Matches peer._identify_number_map so the Layout canvas labels
    and the Identify overlays keep agreeing. Change both together.
    """
    by_machine: dict[str, list[DisplayInfo]] = {}
    for d in displays:
        by_machine.setdefault(d.machine_id, []).append(d)
    machine_order = sorted(
        by_machine.keys(),
        key=lambda mid: (
            mid,
            min(d.global_x for d in by_machine[mid]),
            min(d.global_y for d in by_machine[mid]),
        ),
    )
    n = 1
    for mid in machine_order:
        for d in sorted(by_machine[mid],
                        key=lambda d: d.monitor_id):
            d.number = n
            n += 1

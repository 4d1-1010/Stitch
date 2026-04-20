"""Main shell window for the unIO UI.

Single top-level window that owns the whole session lifecycle:

  * Left rail (nav + hostname + connection status)
  * Content area that swaps per tab
  * Activity tab with an inline Host/Join empty state — no separate
    first-screen picker window anymore
  * Layout tab with the shared LayoutPanel fed by a configurator-
    role TCP connection the shell keeps open to the server
  * Session management: server (host mode), user-client, and the
    configurator-role connection are all spun up / torn down from
    this class

Run:

    python -m unio.apps.shell            # shell only, manual host/join
"""

from __future__ import annotations

import asyncio
import logging
import os
import pathlib
import platform
import re
import socket
import sys
import threading
import tkinter as tk
from dataclasses import dataclass
from tkinter import messagebox
from typing import Callable, Optional

import unio
from ..core.discovery import (
    MeshDiscovery, MeshPeerAnnounce, local_identity,
)
from ..core.protocol import MsgType  # noqa: F401 — kept for shortcuts
from ..features.display_stream import STREAM_PORT, StreamSink
from ..features.os_display import set_monitor_enabled
from ..features.virtual_display import VirtualDisplayManager
from ..features.window_evictor import WindowEvictor
from .layout_panel import LayoutPanel, machine_color
from .log_view import install_log_buffer, show_log_window
from .peer import Peer
from .source_overlay import SourceOverlay
from .stream_window import StreamWindow
from .ui_theme import (
    CORAL, FONT_SANS, LILAC, LILAC_HOVER, LILAC_SOFT, MINT,
    PAPER_BG, PAPER_BORDER,
    PAPER_FAINT, PAPER_MUTED, PAPER_RAIL, PAPER_RAIL_DEEP,
    PAPER_SURFACE, PAPER_TEXT,
    RADIUS_MD, SIZE_BASE, SIZE_LG, SIZE_SM, SIZE_TITLE, SIZE_XL, SIZE_XS,
    SPACE_LG, SPACE_MD, SPACE_SM, SPACE_XL, SPACE_XS,
    PillButton, RailButton, StatusDot, hairline, set_window_icon,
)

log = logging.getLogger(__name__)


RAIL_WIDTH = 108
MINI_RAIL_WIDTH = 64
MIN_WIDTH = 920
MIN_HEIGHT = 560
DEFAULT_PORT = 24800
# Every shell's configurator connection registers under a machine_id
# starting with this prefix, suffixed by the host's own machine_id so
# each PC's shell is unique to the server's clients dict. Before this
# was a bare "__unio_shell__", so the moment a second PC's shell
# registered it evicted the first — and all Identify / Apply sends
# from the host silently dropped because its conn had been closed
# server-side. See _handle_register's "already registered → kick old"
# branch in server.py.


# ── TEST-ONLY hardcoded account ───────────────────────────────────
#
# Bakes a single admin/admin credential into the binary so the mesh
# has something to gate on while we wire the real auth flow. DO NOT
# ship this to users — it WILL be replaced before any public build.
# TODO(auth): swap for real user + password handling.

_TEST_ACCOUNTS = {"admin": "admin"}


def _check_test_login(username: str, password: str) -> bool:
    return _TEST_ACCOUNTS.get(username.strip()) == password


# ── Small helpers ─────────────────────────────────────────────────


def _default_machine_id() -> str:
    raw = socket.gethostname() or "machine"
    clean = re.sub(r"[^A-Za-z0-9_-]", "-", raw).strip("-")
    return clean or "machine"


def _local_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def _describe_platform() -> str:
    system = platform.system()
    if system == "Linux":
        try:
            info = platform.freedesktop_os_release()
            return info.get("PRETTY_NAME") or info.get("NAME", "Linux")
        except (AttributeError, OSError):
            return f"Linux {platform.release()}"
    if system == "Darwin":
        return f"macOS {platform.mac_ver()[0] or platform.release()}"
    if system == "Windows":
        ver = platform.win32_ver()
        return f"Windows {ver[0] or platform.release()}"
    return f"{system} {platform.release()}".strip()


class _AsyncRunner:
    """Dedicated daemon thread running an asyncio loop."""

    def __init__(self) -> None:
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self._ready = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        if self._thread:
            return
        self._thread = threading.Thread(
            target=self._run, daemon=True, name="unio-asyncio",
        )
        self._thread.start()
        if not self._ready.wait(timeout=5):
            raise RuntimeError("asyncio loop failed to start")

    def _run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self._ready.set()
        self.loop.run_forever()

    def submit(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop)

    def stop(self) -> None:
        if self.loop and self.loop.is_running():
            self.loop.call_soon_threadsafe(self.loop.stop)


def _attach_tooltip(widget: tk.Widget, text: str,
                    delay_ms: int = 400) -> None:
    """Small hover tooltip — the only way a glyph-only button can
    still carry its meaning without a caption underneath."""
    state = {"tip": None, "after": None}

    def _show():
        if state["tip"] is not None:
            return
        try:
            x = widget.winfo_rootx() + widget.winfo_width() + 6
            y = widget.winfo_rooty() + widget.winfo_height() // 2 - 10
        except tk.TclError:
            return
        tip = tk.Toplevel(widget)
        tip.wm_overrideredirect(True)
        tip.wm_geometry(f"+{x}+{y}")
        tk.Label(
            tip, text=f" {text} ",
            bg=PAPER_TEXT, fg=PAPER_BG,
            font=(FONT_SANS, SIZE_XS),
            padx=6, pady=3,
        ).pack()
        state["tip"] = tip

    def _cancel(_e=None):
        if state["after"] is not None:
            try:
                widget.after_cancel(state["after"])
            except Exception:
                pass
            state["after"] = None
        if state["tip"] is not None:
            try:
                state["tip"].destroy()
            except Exception:
                pass
            state["tip"] = None

    def _enter(_e=None):
        _cancel()
        state["after"] = widget.after(delay_ms, _show)

    widget.bind("<Enter>", _enter, add="+")
    widget.bind("<Leave>", _cancel, add="+")
    widget.bind("<Button-1>", _cancel, add="+")


# ── Data ──────────────────────────────────────────────────────────


@dataclass
class Tab:
    key: str
    label: str
    glyph: str
    build: Callable[[tk.Widget], tk.Widget]


# ── Main window ───────────────────────────────────────────────────


class MainWindow:
    """The single top-level window that hosts every unIO UI surface."""

    def __init__(self) -> None:
        self.root = tk.Tk(className="unIO")
        self.root.title("unIO")
        self.root.configure(bg=PAPER_BG)
        self.root.minsize(MIN_WIDTH, MIN_HEIGHT)
        self.root.geometry(f"{MIN_WIDTH + 40}x{MIN_HEIGHT + 80}")
        set_window_icon(self.root)

        # Single-level nav: one _active_tab StringVar; a single rail
        # on the left hosts every tab (Activity, Layout, Settings,
        # Account, Help) and the logo lives in a top bar that spans
        # the full width of the window.
        self._active_tab = tk.StringVar(value="activity")
        self._tab_frames: dict[str, tk.Widget] = {}
        self.layout_panel: Optional[LayoutPanel] = None
        self._activity_frame: Optional[tk.Frame] = None
        # Pre-loaded lock icons. Using PNGs instead of emoji
        # characters because Tk's default font on Windows can
        # silently drop 🔒/🔓 glyphs, leaving the lock state
        # invisible even though the underlying LWW value has synced.
        self._lock_closed_img: Optional[tk.PhotoImage] = None
        self._lock_open_img: Optional[tk.PhotoImage] = None
        # Per-workspace card widget handles so remote lock flips can
        # patch an existing card in-place (new lock icon + show/hide
        # Edit) instead of destroying-and-rebuilding the whole
        # Activity tab on every incoming gossip. Repopulated on every
        # _rebuild_activity, cleared before each rebuild.
        self._workspace_card_refs: dict[str, dict] = {}

        # Session state — every PC runs one Peer on launch. The mesh
        # replaces the old server/client/config-conn split entirely.
        self._runner = _AsyncRunner()
        self._runner_started = False
        self._peer: Optional[Peer] = None
        self._peer_task = None
        self._machines_info: dict[str, dict] = {}
        self._active_machine: str = ""
        self._last_monitors: list[dict] = []
        self._machine_id = _default_machine_id()

        # Mesh discovery — starts on launch, runs for the entire app
        # lifetime. Populates self._mesh.peers with every other PC
        # that's broadcasting its presence, regardless of whether we
        # (or they) are hosting. Phase-1 surface area: just the
        # empty-Activity list. Phases 2+ will use this to auto-wire
        # TCP connections into a full mesh.
        self._mesh: Optional[MeshDiscovery] = None
        self._mesh_peers: dict[str, MeshPeerAnnounce] = {}
        # Test-only auth — gates mesh participation. Hardcoded
        # admin/admin login is enough to activate the whole mesh:
        # one PC signs in, every other PC on the LAN sees the
        # authed announce and activates itself. When the signed-in
        # PC logs out or disappears, the mesh winds down.
        self._local_login = False
        self._mesh_auth_listeners: list[Callable[[], None]] = []
        # Machine_id → {"input": PillButton, "clipboard": PillButton}.
        # Tiles are only destroyed on add/remove; toggles update the
        # pills in place so the Activity tab doesn't flash on every
        # mute / clipboard flip.
        self._tile_pills: dict[str, dict[str, "PillButton"]] = {}

        # ── Workspaces (UI-only stub for now) ──────────────────────
        # The data model + persistence + mesh plumbing lands in a
        # later phase; this first cut is purely UX so we can see how
        # the workspace picker + manager feels before committing to
        # the backend shape. No implicit "Default" workspace — users
        # explicitly create one via the Activity tab once they have
        # at least two PCs on the mesh.
        #
        # Each entry: {id, name, members: set[machine_id],
        #              locked_by: Optional[machine_id]}.
        # locked_by is the machine_id of the PC that last locked the
        # workspace while signed in. The lock persists after that PC
        # goes offline — unlock requires signing in locally on the
        # locker's machine.
        self._workspaces: dict[str, dict] = {}
        self._active_workspace: Optional[str] = None
        # Workspace bar references (filled by _build_layout_tab)
        self._workspace_pill: Optional[tk.Label] = None
        # Per-workspace routing table for the display-streaming feature
        # (Phase 0 — data model only; no UI, no pixels on the wire yet).
        # Maps workspace_id → {sink_key: source_key} where both keys
        # are "machine_id:monitor_id". Only NON-IDENTITY entries are
        # stored — a sink with no entry shows its own PC's matching
        # source. Keeps LWW payloads small and lets the default
        # behaviour match today's "every monitor shows its own PC"
        # shape.
        self._workspace_routes: dict[str, dict[str, str]] = {}
        # Per-workspace virtual-display registry. Shape:
        #   ws_id → {machine_id: [{monitor_id, width, height}, ...]}
        # Mirrored into LWW under "virtual_displays:<ws_id>" so every
        # peer agrees on the phantom-monitor list and can materialise
        # (or skip, if its backend is unavailable) consistently.
        self._workspace_virtual: dict[str, dict[str, list[dict]]] = {}
        # Per-workspace hub registry. Each entry is a list of hub ids
        # — hubs have no owner PC, they're pure multicast junctions.
        # Mirrored into LWW under "hubs:<ws_id>" like the other
        # workspace state.
        self._workspace_hubs: dict[str, list[str]] = {}
        # Active display-stream sinks on THIS PC — one entry per routed
        # sink monitor. Keyed by sink_key ("machine_id:monitor_id").
        # `windows` holds the borderless Toplevel painter; `sinks`
        # holds the background TCP reader thread. Both are created in
        # _sync_display_streams when a route lands; both are torn down
        # when the route releases or the source disappears.
        self._stream_windows: dict[str, "StreamWindow"] = {}
        self._stream_sinks: dict[str, "StreamSink"] = {}
        # Per-sink "current source_key" — compared against the latest
        # effective route on every sync so we only tear down / rebuild
        # when the target actually changes.
        self._stream_bound_source: dict[str, str] = {}
        # Phase 2 hand-off: when one of our own monitors is a source
        # for a remote sink, we cover it with a borderless "projected
        # to PC X" overlay so the user can't accidentally drop windows
        # onto a display they no longer see. Keyed by source monitor
        # id (our own machine only, no remote monitors in here).
        self._source_overlays: dict[str, "SourceOverlay"] = {}
        self._source_overlay_dest: dict[str, str] = {}
        # Phase 3 virtual-display manager. The detect_capabilities()
        # call inside its constructor is a one-shot sys-probe — cheap
        # and deterministic. available==False just means the workspace
        # editor's virtual-display counter stays disabled with a hint.
        self._virtual_displays = VirtualDisplayManager()
        # X11 evictor kicks in when any source overlay is live. Poll
        # loop pushes apps off the reserved panel(s); stops when all
        # overlays close so we don't spin on an idle box.
        self._window_evictor = WindowEvictor()
        self._window_evictor_started = False
        # Activity-tab view state. The Create / Edit / Delete-confirm
        # flows all render *inside* the Activity tab instead of spawning
        # new windows — the whole app lives in one Tk window. Values:
        #   "list"              → workspace cards + PC list (default)
        #   "create"            → inline "new workspace" form
        #   f"edit:{ws_id}"     → inline edit form for ws_id
        #   f"delete:{ws_id}"   → inline "are you sure?" confirm
        self._activity_mode: str = "list"
        # Inline banner, shown at the top of the Activity tab. Tuple
        # of (level, text) — level ∈ {"warn", "info"}. Set by
        # workspace actions that used to open a messagebox.
        self._activity_alert: Optional[tuple[str, str]] = None
        # Form state. Shared across Create and Edit renders so the
        # content persists if the tab is rebuilt (e.g. a new peer
        # joined) while the user was mid-edit.
        self._ws_form_name = tk.StringVar(master=self.root)
        self._ws_form_members: dict[str, tk.BooleanVar] = {}
        # New workspace-level settings. Stored locally on the form
        # across rebuilds; written into the workspace dict (and LWW)
        # on Save. Defaults track "behaves like it used to" so every
        # pre-existing workspace survives the schema bump intact.
        self._ws_form_cb_max_size = tk.StringVar(
            master=self.root, value="1 MB")
        self._ws_form_cb_rich = tk.BooleanVar(
            master=self.root, value=False)
        self._ws_form_cb_files = tk.BooleanVar(
            master=self.root, value=False)
        self._ws_form_edge_margin = tk.StringVar(
            master=self.root, value="6")
        self._ws_form_require_modifier = tk.BooleanVar(
            master=self.root, value=False)
        self._ws_form_block_hotkeys = tk.BooleanVar(
            master=self.root, value=False)
        self._ws_form_auto_unlock = tk.StringVar(
            master=self.root, value="Off")
        # Phase 4: per-workspace count of virtual displays to create
        # on every member PC. Defaults to 0 — the existing "identity
        # routing over physical monitors only" shape. Stored alongside
        # the other workspace settings and gossiped via LWW so all
        # members agree how many phantom monitors exist.
        self._ws_form_virtual_count = tk.StringVar(
            master=self.root, value="0")

        self._tabs: list[Tab] = [
            Tab("activity", "Activity", "",  self._build_activity_tab),
            Tab("layout",   "Layout",   "",  self._build_layout_tab),
            Tab("settings", "Settings", "",  self._build_settings_tab),
            Tab("account",  "Access",   "",  self._build_account_tab),
            Tab("help",     "Help",     "",  self._build_help_tab),
        ]
        if unio.DEV_LOGS:
            self._tabs.append(
                Tab("logs", "Logs", "≡", self._build_logs_placeholder),
            )

        self._build()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._bind_shortcuts()
        # Kick off mesh discovery as soon as the UI is alive so other
        # PCs on the LAN can see us (and we them) before any Host /
        # Join click happens.
        self.root.after(100, self._start_mesh_discovery)

    def _bind_shortcuts(self) -> None:
        # The shortcuts live on the root so they fire regardless of
        # which tab / widget has focus. All gate on there being an
        # active session — pressing them from the empty state is a
        # no-op rather than an error.
        self.root.bind_all("<Control-Shift-I>",
                           lambda _e: self._shortcut_identify())
        if unio.DEV_LOGS:
            self.root.bind_all("<Control-Shift-L>",
                               lambda _e: show_log_window(self.root))

    def _shortcut_identify(self) -> None:
        if self._peer is not None:
            self._request_identify()

    # ── Skeleton ─────────────────────────────────────────────────

    def _build(self) -> None:
        outer = tk.Frame(self.root, bg=PAPER_BG)
        outer.pack(fill=tk.BOTH, expand=True)

        # Left nav rail spans the full window height.
        rail = self._build_rail(outer)
        rail.pack(side=tk.LEFT, fill=tk.Y)
        hairline(outer, axis="y").pack(side=tk.LEFT, fill=tk.Y)

        # Right column: top bar + content, stacked. The top bar sits
        # only above the content area (not over the rail), so its
        # logo's horizontal centre lands in the middle of the content
        # column — matching the Activity page's alignment.
        right_col = tk.Frame(outer, bg=PAPER_BG)
        right_col.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        top = self._build_top_bar(right_col)
        top.pack(side=tk.TOP, fill=tk.X)
        # No hairline between top bar and content — the bar shares
        # PAPER_BG with the page, so the logo reads as floating above
        # the content rather than sitting in a separate chrome band.

        self._content = tk.Frame(right_col, bg=PAPER_BG)
        self._content.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        self._active_tab.trace_add(
            "write", lambda *_: self._on_tab_change())

        # Initial paint.
        self._show_tab(self._active_tab.get())

    def _build_top_bar(self, parent: tk.Widget) -> tk.Frame:
        """Top bar above the content area (NOT above the rail). Same
        background as the Activity page, so the bar blends into the
        content visually and the logo reads as a brand mark floating
        at the top of the page rather than a chrome element."""
        bar = tk.Frame(parent, bg=PAPER_BG, height=64)
        bar.pack_propagate(False)

        from pathlib import Path
        assets = Path(__file__).resolve().parents[2] / "assets"
        candidates = [
            assets / "logo_mark_48.png",
            assets / "logo_48.png",
            assets / "logo.png",
        ]
        self._top_bar_logo = None
        for p in candidates:
            if not p.exists():
                continue
            try:
                self._top_bar_logo = tk.PhotoImage(file=str(p))
                break
            except tk.TclError:
                continue
        if self._top_bar_logo is not None:
            tk.Label(bar, image=self._top_bar_logo,
                     bg=PAPER_BG).place(
                relx=0.5, rely=0.5, anchor="center")
        else:
            tk.Label(bar, text="unIO",
                     font=(FONT_SANS, SIZE_XL, "bold"),
                     fg=LILAC, bg=PAPER_BG).place(
                relx=0.5, rely=0.5, anchor="center")
        return bar

    def _build_rail(self, parent: tk.Widget) -> tk.Frame:
        """Main nav rail — just the tab buttons now. Logo and footer
        icons live on the outer mini rail."""
        rail = tk.Frame(parent, bg=PAPER_RAIL, width=RAIL_WIDTH)
        rail.pack_propagate(False)

        from pathlib import Path
        assets = Path(__file__).resolve().parents[2] / "assets"
        self._tab_icons: dict[str, Optional[tk.PhotoImage]] = {
            "activity": self._load_image(assets / "icon_tab_activity_28.png"),
            "layout":   self._load_image(assets / "icon_tab_layout_28.png"),
            "settings": self._load_image(assets / "icon_tab_settings_28.png"),
            "account":  self._load_image(assets / "icon_access_28.png"),
            "help":     self._load_image(assets / "icon_help_28.png"),
        }

        # Top cluster: Activity / Layout / Settings (+ Logs on dev).
        # Bottom cluster: Account / Help, pinned to the foot of the
        # rail. Packing the bottom group with side=BOTTOM first lets
        # Tk reserve its space before the top group fills the rest.
        primary_keys = {"activity", "layout", "settings", "logs"}
        primary = [t for t in self._tabs if t.key in primary_keys]
        secondary = [t for t in self._tabs if t.key not in primary_keys]

        if secondary:
            bottom_nav = tk.Frame(rail, bg=PAPER_RAIL)
            bottom_nav.pack(side=tk.BOTTOM, fill=tk.X,
                            pady=(0, SPACE_MD))
            for tab in secondary:
                icon = self._tab_icons.get(tab.key)
                RailButton(
                    bottom_nav, label=tab.label,
                    glyph=tab.glyph, image=icon,
                    value=tab.key, var=self._active_tab,
                ).pack(fill=tk.X, pady=0)

        top_nav = tk.Frame(rail, bg=PAPER_RAIL)
        top_nav.pack(side=tk.TOP, fill=tk.X)
        for tab in primary:
            icon = self._tab_icons.get(tab.key)
            RailButton(
                top_nav, label=tab.label,
                glyph=tab.glyph, image=icon,
                value=tab.key, var=self._active_tab,
            ).pack(fill=tk.X, pady=0)

        return rail

    def _load_image(self, path) -> Optional[tk.PhotoImage]:
        try:
            return tk.PhotoImage(file=str(path))
        except tk.TclError:
            return None

    def _lock_image(self, locked: bool) -> Optional[tk.PhotoImage]:
        """Return a PhotoImage for the lock glyph. Lazy-loaded and
        cached so we don't re-read the PNG on every card render."""
        if self._lock_closed_img is None or self._lock_open_img is None:
            from pathlib import Path
            assets = Path(__file__).resolve().parents[2] / "assets"
            self._lock_closed_img = self._load_image(
                assets / "icon_lock_closed_16.png")
            self._lock_open_img = self._load_image(
                assets / "icon_lock_open_16.png")
        return (self._lock_closed_img if locked
                else self._lock_open_img)

    def _build_account_tab(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._account_frame = frame
        self._rebuild_account()
        return frame

    def _rebuild_account(self) -> None:
        frame = getattr(self, "_account_frame", None)
        if frame is None:
            return
        for w in frame.winfo_children():
            w.destroy()

        # Column that fills the tab vertically, everything packed
        # top-down with generous leading padding so the form sits in
        # the upper third of the tab rather than collapsing to 0×0
        # like a place-managed centre frame would.
        column = tk.Frame(frame, bg=PAPER_BG)
        column.pack(fill=tk.BOTH, expand=True,
                    padx=SPACE_XL, pady=SPACE_XL)

        tk.Label(
            column, text="Access",
            font=(FONT_SANS, SIZE_TITLE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(anchor="center", pady=(0, SPACE_SM))

        if self._local_login:
            self._build_account_signed_in(column)
        elif self._mesh and self._mesh.any_peer_authed():
            self._build_account_mesh_authed(column)
        else:
            self._build_account_sign_in(column)

        # Temporary-state banner so anyone reading the app knows
        # admin/admin is a stopgap until real auth lands.
        tk.Label(
            column,
            text="Test account only — admin / admin is hardcoded. "
                 "Real sign-in is coming later.",
            font=(FONT_SANS, SIZE_XS, "italic"),
            fg=PAPER_FAINT, bg=PAPER_BG,
            wraplength=420, justify="center",
        ).pack(anchor="center", pady=(SPACE_XL, 0))

    def _build_account_sign_in(self, parent: tk.Widget) -> None:
        tk.Label(
            parent,
            text="Sign in on any computer to activate the mesh.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(anchor="center", pady=(0, SPACE_LG))

        form = tk.Frame(parent, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_LG)
        form.pack(anchor="center")

        def _entry_row(label: str, show: str = "") -> tk.Entry:
            tk.Label(form, text=label,
                     font=(FONT_SANS, SIZE_SM),
                     fg=PAPER_MUTED, bg=PAPER_SURFACE, anchor="w"
                     ).pack(anchor="w", pady=(0, 2))
            e = tk.Entry(form, font=(FONT_SANS, SIZE_BASE),
                         width=28, show=show,
                         relief=tk.FLAT, bg=PAPER_BG,
                         fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
                         highlightthickness=1,
                         highlightbackground=PAPER_BORDER,
                         highlightcolor=LILAC)
            e.pack(ipady=6, pady=(0, SPACE_SM))
            return e

        user_entry = _entry_row("Username")
        pass_entry = _entry_row("Password", show="•")

        error_var = tk.StringVar(value="")
        error_lbl = tk.Label(
            form, textvariable=error_var,
            font=(FONT_SANS, SIZE_SM),
            fg=CORAL, bg=PAPER_SURFACE,
        )
        error_lbl.pack(anchor="w")

        def _try_login():
            user = user_entry.get()
            pwd = pass_entry.get()
            if _check_test_login(user, pwd):
                error_var.set("")
                self._login(user.strip())
            else:
                error_var.set("Wrong username or password.")

        user_entry.bind("<Return>", lambda _e: _try_login())
        pass_entry.bind("<Return>", lambda _e: _try_login())

        PillButton(form, "Sign in", variant="primary",
                   command=_try_login,
                   ).pack(anchor="w", pady=(SPACE_SM, 0))

        user_entry.focus_set()

    def _build_account_signed_in(self, parent: tk.Widget) -> None:
        name = getattr(self, "_login_username", "admin")
        tk.Label(
            parent,
            text=f"Signed in as {name}.",
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(anchor="center", pady=(0, SPACE_SM))
        tk.Label(
            parent,
            text="Every other computer on your LAN can join the mesh\n"
                 "automatically while you stay signed in here.",
            wraplength=440, justify="center",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(anchor="center", pady=(0, SPACE_LG))
        PillButton(parent, "Sign out", variant="secondary",
                   command=self._logout).pack(anchor="center")

    def _build_account_mesh_authed(self, parent: tk.Widget) -> None:
        tk.Label(
            parent,
            text="Active via another computer on your network.",
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(anchor="center", pady=(0, SPACE_SM))
        tk.Label(
            parent,
            text="This machine is joining the mesh because a signed-in\n"
                 "peer is broadcasting. You don't need to sign in here.",
            wraplength=440, justify="center",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(anchor="center")

    def _build_help_tab(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._centered_text(
            frame,
            title="Help",
            body="Guides, shortcuts, and troubleshooting tips will "
                 "live here.\nComing soon.",
        )
        return frame

    def _show_tab(self, key: str) -> None:
        for k, frame in self._tab_frames.items():
            frame.pack_forget()
        if key not in self._tab_frames:
            tab = next((t for t in self._tabs if t.key == key), None)
            if tab is None:
                return
            self._tab_frames[key] = tab.build(self._content)
        self._tab_frames[key].pack(fill=tk.BOTH, expand=True)

    def _on_tab_change(self) -> None:
        self._show_tab(self._active_tab.get())

    # ── Activity tab ─────────────────────────────────────────────

    def _build_scrollable(self, parent: tk.Widget,
                          tab_key: str) -> tuple[tk.Frame, tk.Frame,
                                                 Callable[[tk.Widget],
                                                          None]]:
        """Returns (outer, inner, attach_pan). Wraps arbitrary content
        in a scroll canvas with:

          * Mouse-wheel scrolling, scoped to the given tab_key so it
            doesn't fight Layout's wheel-to-zoom when the user's on
            a different tab. Bound with add="+" so other bind_all
            handlers (e.g. Layout's own zoom) still fire.
          * Middle-click drag to pan the view. attach_pan(widget) lets
            callers re-attach the pan bindings onto dynamically-
            created children after a rebuild.
          * Inner height pinned to at least the visible canvas height
            so short pages (Welcome screen) still vertically centre
            via place(rely=0.5).

        Call after rendering content into `inner` to get scrolling
        that kicks in only when content actually overflows."""
        outer = tk.Frame(parent, bg=PAPER_BG)
        scroll_canvas = tk.Canvas(
            outer, bg=PAPER_BG, highlightthickness=0, bd=0,
        )
        inner = tk.Frame(scroll_canvas, bg=PAPER_BG)
        inner_id = scroll_canvas.create_window(
            (0, 0), window=inner, anchor="nw",
        )

        def _sync_inner_geometry(_e=None):
            try:
                scroll_canvas.update_idletasks()
                canvas_h = scroll_canvas.winfo_height()
                content_h = inner.winfo_reqheight()
                # When content fits, pad the inner frame to the full
                # canvas height so place(rely=0.5) can still centre a
                # short page. When content overflows, use the natural
                # content height — setting it taller than needed used
                # to trap winfo_reqheight and break scroll-after-
                # resize (full-screen → medium regressed because the
                # inflated height stayed cached after a resize).
                use_h = canvas_h if content_h < canvas_h else content_h
                scroll_canvas.itemconfig(inner_id, height=use_h)
                # Scrollregion pulled straight from the visible bbox
                # so it always reflects reality after any widget /
                # geometry change, rather than trusting our
                # computed content_h.
                bbox = scroll_canvas.bbox("all")
                if bbox:
                    scroll_canvas.configure(scrollregion=bbox)
                else:
                    scroll_canvas.configure(
                        scrollregion=(0, 0,
                                      inner.winfo_reqwidth(), content_h))
            except tk.TclError:
                pass

        def _on_canvas_configure(event):
            scroll_canvas.itemconfig(inner_id, width=event.width)
            _sync_inner_geometry()

        inner.bind("<Configure>", _sync_inner_geometry)
        scroll_canvas.bind("<Configure>", _on_canvas_configure)
        # When the outer tab frame resizes (window resize), both the
        # canvas and the inner frame might take a tick to reflow.
        # Rerunning sync on outer <Configure> as well guarantees the
        # scrollregion tracks the new size even if the inner-frame
        # event has already fired with a stale canvas height.
        outer.bind("<Configure>", _sync_inner_geometry)

        def _on_wheel(event):
            if self._active_tab.get() != tab_key:
                return
            try:
                scroll_canvas.update_idletasks()
                # Pull the scrollregion's actual height — if that
                # exceeds the visible canvas, we scroll. Much more
                # reliable than comparing reqheight against
                # winfo_height, which we saw go stale after a
                # fullscreen → medium resize.
                region = scroll_canvas.cget("scrollregion")
                if not region:
                    return
                parts = region.split()
                if len(parts) != 4:
                    return
                y1, y2 = float(parts[1]), float(parts[3])
                content_h = y2 - y1
                canvas_h = scroll_canvas.winfo_height()
            except (tk.TclError, ValueError):
                return
            if content_h <= canvas_h:
                return
            delta = -1 if getattr(event, "num", 0) == 5 else (
                1 if getattr(event, "num", 0) == 4 else
                (-1 if event.delta < 0 else 1))
            scroll_canvas.yview_scroll(-delta, "units")

        self.root.bind_all("<MouseWheel>", _on_wheel, add="+")
        self.root.bind_all("<Button-4>", _on_wheel, add="+")
        self.root.bind_all("<Button-5>", _on_wheel, add="+")

        pan_state = {"active": False}

        def _pan_start(event):
            scroll_canvas.scan_mark(event.x, event.y)
            pan_state["active"] = True
            scroll_canvas.configure(cursor="fleur")

        def _pan_move(event):
            if pan_state["active"]:
                scroll_canvas.scan_dragto(event.x, event.y, gain=1)

        def _pan_end(_event):
            pan_state["active"] = False
            scroll_canvas.configure(cursor="")

        scroll_canvas.bind("<Button-2>", _pan_start)
        scroll_canvas.bind("<B2-Motion>", _pan_move)
        scroll_canvas.bind("<ButtonRelease-2>", _pan_end)

        def _attach_pan_recursively(widget):
            widget.bind("<Button-2>", _pan_start, add="+")
            widget.bind("<B2-Motion>", _pan_move, add="+")
            widget.bind("<ButtonRelease-2>", _pan_end, add="+")
            for child in widget.winfo_children():
                _attach_pan_recursively(child)

        scroll_canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        return outer, inner, _attach_pan_recursively

    def _build_activity_tab(self, parent: tk.Widget) -> tk.Widget:
        outer, inner, attach_pan = self._build_scrollable(parent,
                                                          "activity")
        self._activity_scroll_canvas = outer.winfo_children()[0]
        self._activity_frame = inner
        self._activity_attach_pan = attach_pan
        self._rebuild_activity()
        return outer

    def _rebuild_activity(self) -> None:
        frame = self._activity_frame
        if frame is None:
            return
        # Drop stale widget refs — tiles we're about to destroy can't
        # be repainted in place anymore, and their replacements will
        # register fresh pills in _machine_tile.
        self._tile_pills = {}
        self._workspace_card_refs = {}
        for w in frame.winfo_children():
            w.destroy()

        def _after_build():
            # Rebind middle-click pan onto every freshly-created widget
            # so drag-to-scroll works anywhere over the Activity tab,
            # not just over whatever empty canvas is left.
            if hasattr(self, "_activity_attach_pan"):
                try:
                    self._activity_attach_pan(frame)
                except Exception:
                    pass

        # Peer always runs — there's no "start"/"stop" session. When
        # we have any other computer in the mesh, show the running
        # view; otherwise the "alone" view with live LAN discovery.
        other_machines = [
            mid for mid, info in self._machines_info.items()
            if mid != self._machine_id and info
        ]
        if other_machines:
            self._activity_running(frame)
        else:
            self._activity_alone_state(frame)
        _after_build()

    def _activity_alone_state(self, parent: tk.Widget) -> None:
        # Horizontally centred, anchored to the top of the content
        # area with a fixed gap from the top bar/logo above. Fully
        # centring vertically pushed the welcome down into dead
        # space on tall windows; fixed top-offset keeps the message
        # close to the logo on every window size without bumping
        # into it.
        center = tk.Frame(parent, bg=PAPER_BG)
        # Welcome block sits two logo-heights below the top of the
        # content area. The logo in the top bar is 48 px, and the
        # gap above it inside the bar is ~8 px; using 2×48 below the
        # bar creates a deliberate breathing-room-then-content rhythm
        # that echoes the logo's own vertical padding without
        # crowding it.
        logo_h = 48
        center.place(relx=0.5, y=2 * logo_h, anchor="n")
        # Three-state status progression:
        #   1. Just launched → "Looking for an activated unIO…" while
        #      we give discovery a couple of announce cycles to find
        #      a signed-in peer that would auto-activate us.
        #   2. Grace expired and nobody on the LAN is signed in →
        #      "Sign in (Access tab)…" prompt.
        #   3. This PC signed in OR auto-activated by a remote peer →
        #      "Searching for peers on your LAN…".
        signed_in_somewhere = self._mesh_is_authorised()
        searching_grace = not self._discovery_grace_elapsed()

        # "Welcome to unIO" with "un" in lilac and "IO" in the paper-
        # gray of the subtitle. Rendered via a Canvas so the three
        # pieces sit flush next to each other — separate tk.Label
        # widgets leave a thin border gap that makes "un" and "IO"
        # read as two words.
        import tkinter.font as tkfont
        title_font = (FONT_SANS, SIZE_TITLE, "bold")
        font_measure = tkfont.Font(
            root=self.root, family=FONT_SANS,
            size=SIZE_TITLE, weight="bold",
        )
        parts = [
            ("Welcome to ", PAPER_TEXT),
            ("un", LILAC),
            ("IO", PAPER_MUTED),
        ]
        total_w = sum(font_measure.measure(text) for text, _ in parts)
        height = font_measure.metrics("linespace")
        title_canvas = tk.Canvas(
            center, width=total_w, height=height,
            bg=PAPER_BG, bd=0, highlightthickness=0,
        )
        title_canvas.pack(pady=(0, SPACE_SM))
        cursor_x = 0
        for text, color in parts:
            title_canvas.create_text(
                cursor_x, 0, text=text, anchor="nw",
                font=title_font, fill=color,
            )
            cursor_x += font_measure.measure(text)

        tk.Label(
            center,
            text="unIO transforms the way you use multiple computers.\n\n\n"
                 "Launch unIO on another computer — they'll find each "
                 "other automatically.",
            wraplength=520, justify="center",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_XL))

        if signed_in_somewhere:
            status = "Searching for peers on your LAN…"
        elif searching_grace:
            status = "Looking for an activated unIO on your LAN…"
            # When the grace ends, re-render so the message falls
            # through to the Sign-in prompt if nobody has shown up.
            self._schedule_grace_rerender()
        else:
            status = ("Sign in (Access tab) on this or any other PC "
                      "to activate the mesh.")
        tk.Label(
            center, text=status,
            font=(FONT_SANS, SIZE_SM, "italic"),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack()

    def _activity_running(self, parent: tk.Widget) -> None:
        """Dispatch the Activity content based on _activity_mode. The
        Create / Edit / Delete-confirm flows render *inline* — the app
        never opens a new window for a workspace action, everything
        lives in this single tab."""
        wrap = tk.Frame(parent, bg=PAPER_BG,
                        padx=SPACE_LG, pady=SPACE_LG)
        wrap.pack(fill=tk.BOTH, expand=True)

        # Inline alert banner (replaces tkinter messagebox for every
        # workspace action). Renders at the top of the tab regardless
        # of which view is active.
        self._activity_alert_banner(wrap)

        mode = self._activity_mode
        if mode == "create":
            self._activity_workspace_form(wrap, ws_id=None)
            return
        if mode.startswith("edit:"):
            ws_id = mode.split(":", 1)[1]
            if ws_id in self._workspaces:
                self._activity_workspace_form(wrap, ws_id=ws_id)
                return
            self._activity_mode = "list"
        if mode.startswith("delete:"):
            ws_id = mode.split(":", 1)[1]
            if ws_id in self._workspaces:
                self._activity_delete_confirm(wrap, ws_id)
                return
            self._activity_mode = "list"

        self._activity_list_view(wrap)

    def _activity_list_view(self, parent: tk.Widget) -> None:
        self._activity_header(parent)

        assigned = self._pc_to_workspace_map()
        unassigned = sorted(
            mid for mid, info in self._machines_info.items()
            if mid and info and mid not in assigned
        )

        if self._workspaces:
            if unassigned:
                self._activity_pc_section(
                    parent, "Unassigned", unassigned,
                    empty_text=None,
                )
            self._activity_workspaces_section(parent)
        else:
            all_mids = sorted(
                mid for mid, info in self._machines_info.items()
                if mid and info
            )
            self._activity_pc_section(
                parent, "Connected computers", all_mids,
                empty_text="Waiting for computers to connect…",
            )
            self._activity_empty_workspaces_prompt(parent)

    def _activity_alert_banner(self, parent: tk.Widget) -> None:
        alert = self._activity_alert
        if not alert:
            return
        level, text = alert
        bg = {"warn": "#fdeaea", "info": LILAC_SOFT}.get(level, LILAC_SOFT)
        fg = {"warn": CORAL, "info": LILAC}.get(level, LILAC)
        row = tk.Frame(parent, bg=bg,
                       padx=SPACE_MD, pady=SPACE_SM)
        row.pack(fill=tk.X, pady=(0, SPACE_MD))
        tk.Label(
            row, text=text,
            font=(FONT_SANS, SIZE_SM, "bold"),
            fg=fg, bg=bg, anchor="w",
            wraplength=600, justify="left",
        ).pack(side=tk.LEFT, fill=tk.X, expand=True)
        close = tk.Label(row, text="×",
                         font=(FONT_SANS, SIZE_BASE, "bold"),
                         fg=fg, bg=bg, cursor="hand2")
        close.pack(side=tk.RIGHT)
        close.bind("<Button-1>",
                   lambda _e: self._clear_activity_alert())

    def _set_activity_alert(self, level: str, text: str) -> None:
        self._activity_alert = (level, text)
        self._rebuild_activity()

    def _clear_activity_alert(self) -> None:
        self._activity_alert = None
        self._rebuild_activity()

    def _activity_header(self, parent: tk.Widget) -> None:
        header = tk.Frame(parent, bg=PAPER_BG)
        header.pack(fill=tk.X, pady=(0, SPACE_LG))

        peer_count = (
            sum(1 for mid in self._machines_info
                if mid != self._machine_id)
        )
        title_text = (
            f"Unify · {peer_count + 1} "
            f"computer{'s' if peer_count != 0 else ''}"
        )
        sub_text = self._machine_id

        tk.Label(
            header, text=title_text,
            font=(FONT_SANS, SIZE_TITLE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w")
        tk.Label(
            header, text=sub_text,
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w", pady=(2, 0))

    def _activity_pc_section(self, parent: tk.Widget,
                             title: str, machine_ids: list[str],
                             *, empty_text: Optional[str]) -> None:
        """A titled section that renders one machine tile per id. Used
        for both the flat pre-workspace view ("Connected computers")
        and the "Unassigned" bucket that sits above the workspace
        cards once any workspace exists."""
        section = tk.Frame(parent, bg=PAPER_BG)
        section.pack(fill=tk.X, pady=(0, SPACE_LG))

        tk.Label(
            section, text=title,
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w", pady=(0, SPACE_SM))

        if not machine_ids:
            if empty_text:
                tk.Label(
                    section,
                    text=empty_text,
                    font=(FONT_SANS, SIZE_BASE),
                    fg=PAPER_MUTED, bg=PAPER_BG,
                ).pack(anchor="w", pady=SPACE_SM)
            return

        for mid in machine_ids:
            info = self._machines_info.get(mid)
            if not info:
                continue
            self._machine_tile(section, mid, info).pack(
                fill=tk.X, pady=(0, SPACE_SM))

    def _activity_empty_workspaces_prompt(self, parent: tk.Widget) -> None:
        """Shown under the PC list when no workspaces exist yet. Gives
        the user a concrete next step once they have ≥2 PCs on the
        mesh: group them into a workspace."""
        section = tk.Frame(parent, bg=PAPER_BG)
        section.pack(fill=tk.X, pady=(SPACE_LG, 0))

        tk.Label(
            section, text="Workspaces",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w", pady=(0, SPACE_SM))

        # Workspace needs at least two PCs. Below that, show a hint
        # and suppress the button entirely — no "graded" disabled
        # button, we want the user to see why it isn't available.
        pc_count = sum(
            1 for mid, info in self._machines_info.items()
            if mid and info
        )
        if pc_count < 2:
            tk.Label(
                section,
                text="A workspace needs at least 2 computers. "
                     "Launch unIO on another computer to continue.",
                font=(FONT_SANS, SIZE_BASE),
                fg=PAPER_MUTED, bg=PAPER_BG, wraplength=440,
                justify="left", anchor="w",
            ).pack(anchor="w", pady=(0, SPACE_SM))
            return

        tk.Label(
            section,
            text="Group computers together to share a cursor, "
                 "keyboard, and clipboard between them.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG, wraplength=440,
            justify="left", anchor="w",
        ).pack(anchor="w", pady=(0, SPACE_SM))
        PillButton(section, "+ Create workspace",
                   variant="primary",
                   command=self._start_create_workspace,
                   ).pack(anchor="w", pady=(SPACE_SM, 0))

    def _activity_workspaces_section(self, parent: tk.Widget) -> None:
        """Render the workspace cards + "Create workspace" button when
        at least one workspace exists."""
        section = tk.Frame(parent, bg=PAPER_BG)
        section.pack(fill=tk.X, pady=(0, SPACE_LG))

        header = tk.Frame(section, bg=PAPER_BG)
        header.pack(fill=tk.X, pady=(0, SPACE_SM))
        tk.Label(
            header, text="Workspaces",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(side=tk.LEFT)

        # Only offer "+ Create" when at least 2 PCs are on the mesh —
        # a smaller mesh can't form a valid workspace anyway.
        pc_count = sum(
            1 for mid, info in self._machines_info.items()
            if mid and info
        )
        if pc_count >= 2:
            PillButton(header, "+ Create workspace",
                       variant="secondary",
                       command=self._start_create_workspace,
                       ).pack(side=tk.RIGHT)

        for ws_id in sorted(self._workspaces,
                            key=lambda k: self._workspaces[k]["name"]):
            self._workspace_card(section, ws_id).pack(
                fill=tk.X, pady=(0, SPACE_SM))

    # ── Workspace helpers ─────────────────────────────────────────

    def _pc_to_workspace_map(self) -> dict[str, str]:
        """Flat lookup: machine_id → workspace_id that claims it. A
        PC can only be in one workspace at a time (enforced by the
        dialog). When a lookup returns nothing, the PC is unassigned."""
        out: dict[str, str] = {}
        for ws_id, ws in self._workspaces.items():
            for mid in ws.get("members", ()):
                out[mid] = ws_id
        return out

    def _workspace_card(self, parent: tk.Widget, ws_id: str) -> tk.Widget:
        ws = self._workspaces[ws_id]
        locked_by = ws.get("locked_by")
        is_locked = bool(locked_by)
        locked_by_me = is_locked and locked_by == self._machine_id
        log.info("render workspace card: id=%s name=%r locked_by=%r "
                 "(self=%s) glyph=%s edit=%s",
                 ws_id, ws.get("name"), locked_by, self._machine_id,
                 "LOCKED" if is_locked else "open",
                 "shown" if (not is_locked or locked_by_me) else "hidden")

        # Full-width card with an outer tinted background. Members
        # render as regular _machine_tile rows inside — so Input /
        # Clipboard toggles still work while a PC is in a workspace.
        card = tk.Frame(parent, bg=PAPER_SURFACE)

        head = tk.Frame(card, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_MD)
        head.pack(fill=tk.X)

        tk.Label(
            head, text=ws.get("name") or "Workspace",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_SURFACE, anchor="w",
        ).pack(side=tk.LEFT)

        lock_img = self._lock_image(is_locked)
        if lock_img is not None:
            lock_label = tk.Label(
                head, image=lock_img, bg=PAPER_SURFACE, cursor="hand2",
            )
        else:
            lock_label = tk.Label(
                head, text=("[L]" if is_locked else "[U]"),
                font=(FONT_SANS, SIZE_SM),
                fg=PAPER_MUTED, bg=PAPER_SURFACE, cursor="hand2",
            )
        lock_label.pack(side=tk.LEFT, padx=(SPACE_SM, 0))
        lock_label.bind(
            "<Button-1>",
            lambda _e, wid=ws_id: self._toggle_workspace_lock(wid),
        )

        # Edit is available when we're allowed to modify the
        # workspace: either it's unlocked, or we're the one who
        # locked it. Remote peers seeing someone else's lock have
        # no button — the lock icon tells the story.
        can_edit = (not is_locked) or locked_by_me
        edit_btn: Optional[PillButton] = None
        if can_edit:
            edit_btn = PillButton(head, "Edit", variant="secondary",
                                  size=SIZE_XS,
                                  command=lambda wid=ws_id:
                                      self._start_edit_workspace(wid))
            edit_btn.pack(side=tk.RIGHT)

        # Stash widget handles for in-place lock updates. Lets a
        # remote lock-only change patch the glyph + Edit button
        # without destroying every tile in the card.
        self._workspace_card_refs[ws_id] = {
            "head": head,
            "lock_label": lock_label,
            "edit_btn": edit_btn,
        }

        # Members render as full tiles (same as Connected computers)
        # so Input / Clipboard toggles are available per-PC inside the
        # workspace. Workspace-membership is just a grouping — the
        # per-PC mute / clipboard state is unchanged.
        members = sorted(ws.get("members", ()))
        inner = tk.Frame(card, bg=PAPER_SURFACE,
                         padx=SPACE_LG, pady=SPACE_MD)
        inner.pack(fill=tk.X)
        if not members:
            tk.Label(
                inner,
                text="No computers yet. Click Edit to add some.",
                font=(FONT_SANS, SIZE_SM, "italic"),
                fg=PAPER_FAINT, bg=PAPER_SURFACE, anchor="w",
            ).pack(fill=tk.X)
        else:
            for mid in members:
                info = self._machines_info.get(mid) or {
                    "hostname": mid, "platform_info": "(offline)",
                }
                self._machine_tile(inner, mid, info).pack(
                    fill=tk.X, pady=(0, SPACE_SM))

        return card

    # ── Workspace actions ─────────────────────────────────────────

    def _toggle_workspace_lock(self, ws_id: str) -> None:
        """Lock model: any PC can lock an unlocked workspace. Only
        the locker can unlock it (or edit it while locked). Sign-in
        is no longer part of the gate — the lock itself is the
        authority, and it's tied to the machine that set it."""
        ws = self._workspaces.get(ws_id)
        if ws is None:
            return
        locked_by = ws.get("locked_by")
        if not locked_by:
            ws["locked_by"] = self._machine_id
        else:
            if locked_by != self._machine_id:
                self._set_activity_alert(
                    "warn",
                    f"This workspace is locked by {locked_by}. "
                    "Only that computer can unlock it.",
                )
                return
            ws["locked_by"] = None
        self._write_workspace_to_lww(ws_id, ws)
        self._activity_alert = None
        # In-place update of the card — avoids the full-tab flicker
        # that every lock toggle used to cause.
        if not self._update_workspace_card_lock(ws_id):
            self._rebuild_activity()

    def _update_workspace_card_lock(self, ws_id: str) -> bool:
        """Patch an existing workspace card's lock icon + Edit button
        to match self._workspaces[ws_id]. Returns False when the card
        isn't tracked (caller falls back to a full rebuild)."""
        refs = self._workspace_card_refs.get(ws_id)
        if refs is None:
            return False
        ws = self._workspaces.get(ws_id)
        if ws is None:
            return False
        locked_by = ws.get("locked_by")
        is_locked = bool(locked_by)
        locked_by_me = is_locked and locked_by == self._machine_id

        lock_label = refs.get("lock_label")
        if lock_label is not None and lock_label.winfo_exists():
            img = self._lock_image(is_locked)
            if img is not None:
                lock_label.configure(image=img)
            else:
                lock_label.configure(
                    text=("[L]" if is_locked else "[U]"))

        head = refs.get("head")
        edit_btn = refs.get("edit_btn")
        can_edit = (not is_locked) or locked_by_me
        if can_edit and (edit_btn is None or not edit_btn.winfo_exists()):
            if head is not None and head.winfo_exists():
                new_btn = PillButton(
                    head, "Edit", variant="secondary", size=SIZE_XS,
                    command=lambda wid=ws_id:
                        self._start_edit_workspace(wid))
                new_btn.pack(side=tk.RIGHT)
                refs["edit_btn"] = new_btn
        elif not can_edit and edit_btn is not None \
                and edit_btn.winfo_exists():
            edit_btn.destroy()
            refs["edit_btn"] = None
        return True

    def _delete_workspace(self, ws_id: str) -> None:
        """Raw delete — the caller is expected to have already checked
        the lock state via _start_delete_confirm. Returns the members
        to the Unassigned list on every PC via an LWW tombstone."""
        ws = self._workspaces.pop(ws_id, None)
        if ws is None:
            return
        self._delete_workspace_from_lww(ws_id)
        if self._active_workspace == ws_id:
            self._active_workspace = None
        self._activity_mode = "list"
        self._activity_alert = None
        self._rebuild_activity()
        self._refresh_workspace_pill()
        self._refresh_layout_empty_state()
        self._refresh_layout_display()
        self._sync_allowed_peers_to_peer()

    # ── Workspace ↔ LWW plumbing ──────────────────────────────────

    def _write_workspace_to_lww(self, ws_id: str, ws: dict) -> None:
        """Push a workspace into the peer's replicated LWW store. No-op
        when the peer isn't running yet (the user is still pre-auth
        and the change stays local — next peer start will push it)."""
        if self._peer is None:
            log.info("workspace %s write skipped — peer not running", ws_id)
            return
        value = {
            "name": ws.get("name", ""),
            # Stored as a sorted list for deterministic LWW sigs and
            # JSON-serialisable gossip payloads.
            "members": sorted(ws.get("members") or []),
            "locked_by": ws.get("locked_by"),
        }
        for key in self.WORKSPACE_SETTING_KEYS:
            if key in ws:
                value[key] = ws[key]
        log.info("workspace write → LWW: %s = %s", ws_id, value)
        try:
            self._peer._lww_write(f"workspace:{ws_id}", value)
        except Exception:
            log.exception("lww write for workspace %s failed", ws_id)

    def _delete_workspace_from_lww(self, ws_id: str) -> None:
        if self._peer is None:
            return
        try:
            self._peer._lww_write(f"workspace:{ws_id}", None)
            # Tombstone the matching route entry so a future workspace
            # with the same id (unlikely but possible) doesn't inherit
            # stale routing from the old one.
            self._peer._lww_write(f"route:{ws_id}", None)
        except Exception:
            log.exception("lww delete for workspace %s failed", ws_id)

    # ── Source / sink routing (Phase 0 — data model only) ─────────
    #
    # Sources and sinks are both addressed as "machine_id:monitor_id"
    # — physical displays today, virtual displays once IDD / evdi
    # ships. A route maps a sink (what the viewer's eyes land on) to
    # a source (what's displayed there). Per-workspace so different
    # clusters can have different patch matrices.

    @staticmethod
    def _screen_key(machine_id: str, monitor_id: str) -> str:
        return f"{machine_id}:{monitor_id}"

    def effective_route(self, ws_id: str, sink_key: str) -> str:
        """Resolve a sink_key to its effective source_key for the
        given workspace. Returns the sink_key itself when no explicit
        route is set — identity routing is the implicit default, so a
        brand-new workspace looks exactly like today's behaviour."""
        override = self._workspace_routes.get(ws_id, {}).get(sink_key)
        return override or sink_key

    def set_route(self, ws_id: str, sink_key: str,
                  source_key: str) -> None:
        """Set a route override and replicate to peers. source_key
        == sink_key (identity) clears the override entry to keep the
        stored dict minimal."""
        routes = dict(self._workspace_routes.get(ws_id) or {})
        if source_key == sink_key:
            routes.pop(sink_key, None)
        else:
            routes[sink_key] = source_key
        if routes == self._workspace_routes.get(ws_id, {}):
            return
        if routes:
            self._workspace_routes[ws_id] = routes
        else:
            self._workspace_routes.pop(ws_id, None)
        self._write_route_to_lww(ws_id, routes)
        # Locally-initiated route edits need to reshape our stream
        # windows + source overlays immediately; waiting for the
        # gossip round-trip would leave the user staring at the old
        # panel for 100-200 ms.
        try:
            self._sync_display_streams()
            self._sync_source_overlays()
        except Exception:
            log.exception("sync display streams after set_route failed")

    def _write_route_to_lww(self, ws_id: str,
                            routes: dict[str, str]) -> None:
        if self._peer is None:
            return
        log.info("route write → LWW: %s = %s", ws_id, routes)
        try:
            # Empty dict means "back to full identity routing" — we
            # send the empty value explicitly (rather than tombstoning)
            # so a peer can't get stuck on stale overrides when the
            # user clears them.
            self._peer._lww_write(f"route:{ws_id}", routes or {})
        except Exception:
            log.exception("lww write for route %s failed", ws_id)

    @staticmethod
    def _resolve_route_chain(routes: dict[str, str],
                             source_key: str,
                             _seen: Optional[set] = None) -> Optional[str]:
        """Walk hub → hub (or virtual → virtual) routes until we land
        on an actual source (machine:monitor). Returns None for a
        cycle or a dead-end hub with no input. Used by both the
        display-stream sync and the cursor passthrough so both agree
        on where pixels / input actually come from."""
        if _seen is None:
            _seen = set()
        if source_key in _seen:
            return None
        _seen.add(source_key)
        if source_key.startswith("hub:"):
            nxt = routes.get(source_key)
            if not nxt:
                return None
            return MainWindow._resolve_route_chain(routes, nxt, _seen)
        return source_key

    def _sync_display_streams(self) -> None:
        """Reconcile active StreamWindows/StreamSinks against the
        current active workspace's route map.

        For every sink monitor that belongs to THIS PC in the active
        workspace, check whether the effective route points somewhere
        else in the mesh. If so, make sure a StreamWindow + StreamSink
        pair exists for it, pointing at the right source peer. If not
        (identity route, or no active workspace), tear any existing
        pair down. Source-side (our own captured monitors) is handled
        by the Peer's StreamServer on demand — we don't touch it here.

        Latency-wise this only runs on state changes (route edit or
        peer comes/goes), not per frame, so the reconciliation cost
        never sits in the video hot path.
        """
        if self._peer is None:
            self._teardown_all_streams()
            return
        ws_id = self._active_workspace
        routes = self._workspace_routes.get(ws_id or "", {}) if ws_id else {}
        my_mid = self._machine_id
        wanted: dict[str, tuple[str, dict]] = {}
        # Only this PC's monitors can be sinks locally — we never try
        # to render a stream on another machine's screen.
        my_info = self._machines_info.get(my_mid) or {}
        for m in my_info.get("monitors") or []:
            mon_id = str(m.get("monitor_id") or "")
            if not mon_id:
                continue
            sink_key = self._screen_key(my_mid, mon_id)
            raw_src = routes.get(sink_key, sink_key)
            # Resolve hubs / virtual chains to a concrete source
            # before deciding to stream. If the chain is broken
            # (hub with no input, cycle, etc.) skip the sink so it
            # shows its "no input source" card instead.
            src_key = self._resolve_route_chain(routes, raw_src)
            if not src_key or src_key == sink_key:
                continue
            src_mid, _, src_mon = src_key.partition(":")
            if not src_mid or not src_mon:
                continue
            # Same-PC routing (e.g. Linux display A → Linux display B)
            # is allowed: we stream from our own StreamServer over
            # localhost. The capture cost is one extra mss-grab per
            # route, but the UX is identical to cross-PC routing.
            # Pick the sink rectangle in LOCAL (OS) coords so the
            # borderless Toplevel lands on the correct physical panel
            # even in a multi-monitor setup.
            wanted[sink_key] = (src_key, {
                "x": int(m.get("local_x", 0)),
                "y": int(m.get("local_y", 0)),
                "width": int(m.get("width", 0)),
                "height": int(m.get("height", 0)),
                "source_label": src_key,
            })

        # Tear down anything no longer wanted — either the route went
        # identity, the source disappeared, or we switched workspace.
        for sink_key in list(self._stream_windows):
            if sink_key not in wanted:
                self._teardown_stream(sink_key)
                continue
            # Re-bind when the target source changed (user dragged the
            # route in Layout, etc.) — cheaper than a full refresh.
            src_key, _ = wanted[sink_key]
            if self._stream_bound_source.get(sink_key) != src_key:
                self._teardown_stream(sink_key)

        # Build anything newly wanted.
        for sink_key, (src_key, geom) in wanted.items():
            if sink_key in self._stream_windows:
                continue
            self._start_stream(sink_key, src_key, geom)

    def _start_stream(self, sink_key: str, src_key: str,
                      geom: dict) -> None:
        """Open a StreamWindow for `sink_key` and connect a StreamSink
        to the source peer. The placeholder card is shown immediately;
        real frames replace it as soon as JPEGs arrive from the source."""
        src_mid, _, src_mon = src_key.partition(":")
        if not src_mid or not src_mon:
            return
        window = StreamWindow(
            root=self.root,
            x=geom["x"], y=geom["y"],
            width=geom["width"], height=geom["height"],
            source_label=geom["source_label"],
        )
        self._stream_windows[sink_key] = window
        self._stream_bound_source[sink_key] = src_key

        # Same-PC routing streams over the loopback interface since
        # there's no mesh link to self.
        if src_mid == self._machine_id:
            ip = "127.0.0.1"
        else:
            ip = self._peer.peer_ip(src_mid) if self._peer else None
        if ip is None:
            # Remote source PC not reachable yet. Overlay stays black
            # until a later _sync_display_streams tick finds it.
            return

        def _on_frame(data: bytes, codec: str, w=window) -> None:
            w.push_frame(data, codec)

        def _on_error(_msg: str) -> None:
            # No user-visible error surface — the overlay goes back
            # to black; the next reconnect attempt brings frames
            # back seamlessly.
            pass

        sink = StreamSink(
            host=ip, port=STREAM_PORT,
            monitor_id=src_mon,
            sink_machine_id=self._machine_id,
            on_frame=_on_frame,
            on_error=_on_error,
        )
        self._stream_sinks[sink_key] = sink
        sink.start()
        log.info("display stream started: %s ← %s (@%s)",
                 sink_key, src_key, ip)

    def _teardown_stream(self, sink_key: str) -> None:
        sink = self._stream_sinks.pop(sink_key, None)
        if sink is not None:
            try:
                sink.stop()
            except Exception:
                log.exception("stream sink stop failed")
        window = self._stream_windows.pop(sink_key, None)
        if window is not None:
            try:
                window.destroy()
            except Exception:
                log.exception("stream window destroy failed")
        self._stream_bound_source.pop(sink_key, None)

    def _teardown_all_streams(self) -> None:
        for key in list(self._stream_windows):
            self._teardown_stream(key)
        self._teardown_all_source_overlays()

    def _sync_source_overlays(self) -> None:
        """Legacy hook kept as a no-op. With swap-based routing every
        display is always showing SOMEONE's pixels (swap pairs keep
        the matching perfect), so there's no "projected away —
        source is dark" state to cover with an overlay. StreamWindow
        handles the "show remote PC's pixels on this panel" side;
        nothing to do here.

        Will be revived if we add an explicit "cover source" per-
        route toggle later. Until then, tear down any stale overlays
        that predate the shift to swap semantics.
        """
        # Evictor no longer needed either — with swap-based routing,
        # nothing is "reserved" (every panel has pixels), so there's
        # no empty region for apps to accidentally land on.
        self._sync_evictor()

    def _format_destination_label(self, sink_mid: str, sink_mon: str) -> str:
        info = self._machines_info.get(sink_mid) or {}
        hostname = info.get("hostname") or sink_mid
        if sink_mon:
            return f"{hostname}:{sink_mon}"
        return hostname

    def _teardown_source_overlay(self, mon_id: str) -> None:
        overlay = self._source_overlays.pop(mon_id, None)
        self._source_overlay_dest.pop(mon_id, None)
        if overlay is not None:
            try:
                overlay.destroy()
            except Exception:
                log.exception("source overlay destroy failed")
        self._sync_evictor()

    def _teardown_all_source_overlays(self) -> None:
        for mon_id in list(self._source_overlays):
            self._teardown_source_overlay(mon_id)
        self._sync_evictor()

    def _sync_evictor(self) -> None:
        """Reconcile the X11 window evictor's reserved-rect set with
        whatever source overlays we have live. Starts lazily and
        tears down when the overlay count hits zero."""
        rects: list[tuple[int, int, int, int]] = []
        my_info = self._machines_info.get(self._machine_id) or {}
        geom_by_mon = {
            str(m.get("monitor_id")): m
            for m in (my_info.get("monitors") or [])
        }
        for mon_id in self._source_overlays:
            g = geom_by_mon.get(mon_id)
            if not g:
                continue
            rects.append((
                int(g.get("local_x", 0)), int(g.get("local_y", 0)),
                int(g.get("width", 0)), int(g.get("height", 0)),
            ))
        self._window_evictor.set_reserved(rects)
        if rects and not self._window_evictor_started:
            if self._window_evictor.start():
                self._window_evictor_started = True
        elif not rects and self._window_evictor_started:
            self._window_evictor.stop()
            self._window_evictor_started = False

    # ── Phase 4: Layout canvas hooks ─────────────────────────────

    def _on_layout_reroute(self, sink_key: str, source_key: str) -> None:
        """Called by LayoutPanel when the user drags one rectangle
        onto another or picks a source from the right-click menu.
        Writes the override into the active workspace's route map via
        the existing set_route path, which gossips through LWW and
        triggers the stream-window reconciliation below."""
        ws_id = self._active_workspace
        if not ws_id:
            return
        self.set_route(ws_id, sink_key, source_key)
        # Reflect the change immediately in the panel so the badge
        # updates before the gossip round-trip lands.
        self._refresh_layout_routes()

    def _collect_mesh_sources(self) -> list[dict]:
        """Enumerate every known source in the mesh — every monitor
        on every peer, restricted to the active workspace's members.
        Phase 3 will extend this to include virtual displays."""
        ws_id = self._active_workspace
        if not ws_id or ws_id not in self._workspaces:
            return []
        members = self._workspaces[ws_id].get("members", set())
        out: list[dict] = []
        for mid, info in self._machines_info.items():
            if mid not in members:
                continue
            for m in info.get("monitors") or []:
                out.append({
                    "machine_id": mid,
                    "monitor_id": m.get("monitor_id"),
                })
        out.sort(key=lambda s: (s["machine_id"], str(s["monitor_id"])))
        return out

    def _refresh_layout_routes(self) -> None:
        """Push the active workspace's route map into the LayoutPanel
        so badges reflect the current state. Also push a HUB-RESOLVED
        version into the peer so its cursor-passthrough logic handles
        hub chains without having to understand hub semantics."""
        ws_id = self._active_workspace
        routes = self._workspace_routes.get(ws_id or "", {}) if ws_id else {}
        if self.layout_panel is not None:
            self.layout_panel.set_routes(routes)
        if self._peer is not None:
            # Strip hub sink entries and flatten any hub sources to
            # their ultimate machine:monitor before handing off to
            # the peer. Peer stays hub-unaware.
            resolved: dict[str, str] = {}
            for sink, src in routes.items():
                if sink.startswith("hub:"):
                    continue
                eff = self._resolve_route_chain(routes, src)
                if eff and not eff.startswith("hub:"):
                    resolved[sink] = eff
            self._peer.active_routes = resolved

    # ── Virtual displays (per-workspace LWW + Routing panel) ────

    def _on_add_virtual_display(self, machine_id: str) -> None:
        """Fired from the Routing canvas's "+" slot. Appends a new
        phantom monitor to the active workspace's virtual-display
        map and gossips via LWW so every member PC sees it."""
        ws_id = self._active_workspace
        if not ws_id:
            return
        per_machine = dict(self._workspace_virtual.get(ws_id) or {})
        existing = list(per_machine.get(machine_id) or [])
        next_index = len(existing) + 1
        while any(e.get("monitor_id") == f"V-{next_index}"
                  for e in existing):
            next_index += 1
        existing.append({
            "monitor_id": f"V-{next_index}",
            "width": 1920, "height": 1080,
        })
        per_machine[machine_id] = existing
        self._workspace_virtual[ws_id] = per_machine
        self._write_virtual_to_lww(ws_id, per_machine)
        self._sync_own_virtuals_to_stream_server()
        self._refresh_layout_display()

    def _on_remove_virtual_display(self, machine_id: str,
                                   monitor_id: str) -> None:
        ws_id = self._active_workspace
        if not ws_id:
            return
        per_machine = dict(self._workspace_virtual.get(ws_id) or {})
        existing = [e for e in per_machine.get(machine_id, [])
                    if e.get("monitor_id") != monitor_id]
        if existing:
            per_machine[machine_id] = existing
        else:
            per_machine.pop(machine_id, None)
        if per_machine:
            self._workspace_virtual[ws_id] = per_machine
        else:
            self._workspace_virtual.pop(ws_id, None)
        self._write_virtual_to_lww(ws_id, per_machine)
        self._sync_own_virtuals_to_stream_server()
        # Also tombstone any route that referenced the removed
        # virtual, so sinks don't end up pointing at a phantom that
        # no longer exists.
        key_removed = f"{machine_id}:{monitor_id}"
        routes = dict(self._workspace_routes.get(ws_id) or {})
        changed = False
        for sink, src in list(routes.items()):
            if src == key_removed:
                routes.pop(sink, None)
                changed = True
        if changed:
            if routes:
                self._workspace_routes[ws_id] = routes
            else:
                self._workspace_routes.pop(ws_id, None)
            self._write_route_to_lww(ws_id, routes)
        self._refresh_layout_display()

    def _write_virtual_to_lww(self, ws_id: str,
                              per_machine: dict[str, list[dict]]) -> None:
        if self._peer is None:
            return
        try:
            self._peer._lww_write(
                f"virtual_displays:{ws_id}", per_machine or {})
        except Exception:
            log.exception("lww write virtual_displays:%s failed", ws_id)

    def _sync_own_virtuals_to_stream_server(self) -> None:
        """Tell THIS PC's StreamServer which virtual displays it
        owns in the active workspace AND spawn any live driver-level
        phantoms (evdi / IDD). Live frames flow through the stream
        server's virtual_frame_provider callback; missing backends
        fall through to the placeholder card automatically."""
        if self._peer is None or self._peer.stream_server is None:
            return
        ws_id = self._active_workspace
        per_machine = (self._workspace_virtual.get(ws_id or "") or {}) \
            if ws_id else {}
        mine = list(per_machine.get(self._machine_id) or [])
        hostname = socket.gethostname() or self._machine_id
        # Build a host-specific placeholder hint. If the backend is
        # "detected but idle" we want a different message than "not
        # installed at all" — saves users hunting for install steps
        # they already completed. The detail text already captures
        # this nuance from detect_capabilities.
        hint = self._virtual_displays.caps.detail or ""
        try:
            self._peer.stream_server.set_virtuals(
                mine, owner_label=hostname,
                placeholder_hint=hint)
            self._peer.stream_server.virtual_frame_provider = \
                self._virtual_displays.live_frame
        except Exception:
            log.exception("set_virtuals failed")
        # Reconcile the live phantom set: create for new virtuals,
        # destroy for ones that went away. Safe to call repeatedly —
        # VirtualDisplayManager.create is idempotent on id.
        wanted_ids = {str(v.get("monitor_id") or "") for v in mine}
        current_ids = {vd.id for vd in self._virtual_displays.all()}
        for v in mine:
            mon = str(v.get("monitor_id") or "")
            if not mon:
                continue
            try:
                self._virtual_displays.create(
                    mon,
                    width=int(v.get("width") or 1920),
                    height=int(v.get("height") or 1080),
                )
            except Exception:
                log.exception("virtual display create %s failed", mon)
        for gone in current_ids - wanted_ids:
            try:
                self._virtual_displays.destroy(gone)
            except Exception:
                log.exception("virtual display destroy %s failed", gone)

    def _refresh_virtual_from_lww(self) -> bool:
        if self._peer is None:
            return False
        new_map: dict[str, dict[str, list[dict]]] = {}
        for key in self._peer.lww.iter_keys():
            if not key.startswith("virtual_displays:"):
                continue
            value = self._peer.lww.get(key)
            if not isinstance(value, dict):
                continue
            ws_id = key.split(":", 1)[1]
            clean: dict[str, list[dict]] = {}
            for mid, lst in value.items():
                if not isinstance(lst, list):
                    continue
                entries = []
                for e in lst:
                    if not isinstance(e, dict):
                        continue
                    mon_id = str(e.get("monitor_id") or "")
                    if not mon_id:
                        continue
                    entries.append({
                        "monitor_id": mon_id,
                        "width": int(e.get("width") or 1920),
                        "height": int(e.get("height") or 1080),
                    })
                if entries:
                    clean[str(mid)] = entries
            if clean:
                new_map[ws_id] = clean
        if new_map == self._workspace_virtual:
            return False
        self._workspace_virtual = new_map
        return True

    def _collect_hubs(self) -> list[dict]:
        ws_id = self._active_workspace
        if not ws_id:
            return []
        return [{"id": h} for h in self._workspace_hubs.get(ws_id, [])]

    def _on_add_hub(self) -> None:
        ws_id = self._active_workspace
        if not ws_id:
            return
        existing = list(self._workspace_hubs.get(ws_id) or [])
        # Pick a stable id — hub-1, hub-2, … — that avoids collisions
        # with any pre-existing ids even after deletions.
        n = 1
        taken = set(existing)
        while f"hub-{n}" in taken:
            n += 1
        existing.append(f"hub-{n}")
        self._workspace_hubs[ws_id] = existing
        self._write_hubs_to_lww(ws_id, existing)
        self._refresh_layout_display()

    def _on_remove_hub(self, hub_id: str) -> None:
        ws_id = self._active_workspace
        if not ws_id:
            return
        existing = [h for h in self._workspace_hubs.get(ws_id, [])
                    if h != hub_id]
        if existing:
            self._workspace_hubs[ws_id] = existing
        else:
            self._workspace_hubs.pop(ws_id, None)
        self._write_hubs_to_lww(ws_id, existing)
        # Clean routes that referenced the removed hub — either as
        # sink (hub:<id>) or as source (hub:<id> on any physical).
        hub_key = f"hub:{hub_id}"
        routes = dict(self._workspace_routes.get(ws_id) or {})
        touched = False
        for sink_key in list(routes):
            if sink_key == hub_key or routes.get(sink_key) == hub_key:
                routes.pop(sink_key, None)
                touched = True
        if touched:
            if routes:
                self._workspace_routes[ws_id] = routes
            else:
                self._workspace_routes.pop(ws_id, None)
            self._write_route_to_lww(ws_id, routes)
        self._refresh_layout_display()

    def _write_hubs_to_lww(self, ws_id: str, hubs: list[str]) -> None:
        if self._peer is None:
            return
        try:
            self._peer._lww_write(f"hubs:{ws_id}", list(hubs))
        except Exception:
            log.exception("lww write hubs:%s failed", ws_id)

    def _refresh_hubs_from_lww(self) -> bool:
        if self._peer is None:
            return False
        new_map: dict[str, list[str]] = {}
        for key in self._peer.lww.iter_keys():
            if not key.startswith("hubs:"):
                continue
            value = self._peer.lww.get(key)
            if not isinstance(value, list):
                continue
            ws_id = key.split(":", 1)[1]
            clean = [str(h) for h in value if isinstance(h, str)]
            if clean:
                new_map[ws_id] = clean
        if new_map == self._workspace_hubs:
            return False
        self._workspace_hubs = new_map
        return True

    def _collect_virtual_displays(self) -> list[dict]:
        """LayoutPanel's virtuals_provider. Returns the active
        workspace's phantom monitors, restricted to member PCs, with
        enough geometry for the canvas to place them next to each
        chassis."""
        ws_id = self._active_workspace
        if not ws_id or ws_id not in self._workspaces:
            return []
        members = self._workspaces[ws_id].get("members", set())
        per_machine = self._workspace_virtual.get(ws_id) or {}
        out: list[dict] = []
        for mid, entries in per_machine.items():
            if mid not in members:
                continue
            for e in entries:
                out.append({
                    "machine_id": mid,
                    "monitor_id": e.get("monitor_id"),
                    "width": int(e.get("width") or 1920),
                    "height": int(e.get("height") or 1080),
                })
        return out

    def _refresh_routes_from_lww(self) -> bool:
        """Pull every route:<ws_id> entry from the peer's LWW store
        into self._workspace_routes. Returns True when anything
        changed — caller uses this to trigger re-render of any
        routing-aware UI."""
        if self._peer is None:
            return False
        new_map: dict[str, dict[str, str]] = {}
        for key in self._peer.lww.iter_keys():
            if not key.startswith("route:"):
                continue
            value = self._peer.lww.get(key)
            if value is None:
                continue  # tombstoned (workspace deleted)
            if not isinstance(value, dict):
                continue
            ws_id = key.split(":", 1)[1]
            # Filter to string→string pairs; ignore anything else so a
            # malformed payload can't crash the decoder.
            clean = {str(k): str(v) for k, v in value.items()
                     if isinstance(k, str) and isinstance(v, str)}
            if clean:
                new_map[ws_id] = clean
        if new_map == self._workspace_routes:
            return False
        self._workspace_routes = new_map
        log.info("workspace routes changed: %s",
                 {wid: r for wid, r in new_map.items()})
        return True

    @staticmethod
    def _ws_shape_sig(ws_map: dict) -> tuple:
        """Workspace-set sig that changes ONLY when the list of
        workspaces, their names, or their member lists move —
        everything that actually needs the Activity tab rebuilt.
        Lock state is deliberately NOT here: lock flips go through
        an in-place path that patches only the affected card."""
        return tuple(sorted(
            (ws_id, ws.get("name", ""),
             tuple(sorted(ws.get("members", ()) or ())))
            for ws_id, ws in ws_map.items()
        ))

    @staticmethod
    def _ws_lock_sig(ws_map: dict) -> tuple:
        return tuple(sorted(
            (ws_id, ws.get("locked_by"))
            for ws_id, ws in ws_map.items()
        ))

    def _refresh_workspaces_from_lww(self) -> tuple[bool, bool]:
        """Rebuild self._workspaces from the peer's LWW store. Returns
        (shape_changed, lock_only_changed):
          * shape_changed   – workspaces / names / members moved.
                              Activity tab needs a full rebuild.
          * lock_only_changed – the workspace set is the same, only
                              locked_by values shifted. Activity tab
                              can be patched in-place via
                              _update_workspace_card_lock(ws_id)
                              which avoids the widget-destroy flicker
                              that was visible on every lock toggle.
        Both False when nothing changed."""
        if self._peer is None:
            return (False, False)
        new_map: dict[str, dict] = {}
        for key in self._peer.lww.iter_keys():
            if not key.startswith("workspace:"):
                continue
            value = self._peer.lww.get(key)
            if value is None:
                continue
            if not isinstance(value, dict):
                continue
            ws_id = key.split(":", 1)[1]
            entry = {
                "id": ws_id,
                "name": str(value.get("name") or ""),
                "members": set(value.get("members") or ()),
                "locked_by": value.get("locked_by"),
            }
            for k in self.WORKSPACE_SETTING_KEYS:
                if k in value:
                    entry[k] = value[k]
            new_map[ws_id] = entry

        old_shape = self._ws_shape_sig(self._workspaces)
        new_shape = self._ws_shape_sig(new_map)
        old_locks = self._ws_lock_sig(self._workspaces)
        new_locks = self._ws_lock_sig(new_map)
        shape_changed = old_shape != new_shape
        lock_only_changed = (not shape_changed) and (old_locks != new_locks)
        if not shape_changed and not lock_only_changed:
            return (False, False)

        log.info("workspaces changed from LWW (shape=%s locks=%s): %s",
                 shape_changed, lock_only_changed,
                 [(wid, ws.get("name"), ws.get("locked_by"))
                  for wid, ws in new_map.items()])
        self._workspaces = new_map

        # Active workspace may have been deleted remotely.
        if (self._active_workspace
                and self._active_workspace not in self._workspaces):
            self._active_workspace = None

        # Auto-activate a workspace we're a member of when nothing is
        # active. Critical for the remote peer: when Linux creates a
        # workspace and LWW-gossips it to Windows, Windows would
        # otherwise stay on _active_workspace=None and leave its
        # allowed_peer_ids={self}, blocking every cursor handoff from
        # Linux. Picking the first workspace we're in matches what
        # _create_workspace does locally for the creator.
        if self._active_workspace is None:
            for ws_id, ws in sorted(self._workspaces.items(),
                                    key=lambda kv: kv[1].get("name", "")):
                if self._machine_id in ws.get("members", set()):
                    self._active_workspace = ws_id
                    log.info("auto-activated workspace %r (%s) — "
                             "first one containing this PC",
                             ws.get("name"), ws_id)
                    break
        return (shape_changed, lock_only_changed)

    def _workspaces_signature(self) -> tuple:
        return self._signature_for_workspaces(self._workspaces)

    @staticmethod
    def _signature_for_workspaces(ws_map: dict[str, dict]) -> tuple:
        return tuple(sorted(
            (ws_id,
             ws.get("name", ""),
             tuple(sorted(ws.get("members", ()) or ())),
             ws.get("locked_by"))
            for ws_id, ws in ws_map.items()
        ))

    def _create_workspace(self, name: str,
                          members: set[str],
                          *,
                          settings: Optional[dict] = None
                          ) -> Optional[str]:
        """Add a new workspace. Persists through the peer's LWW so
        every other PC on the mesh sees it on the next gossip tick.
        Returns the new id on success, None if the name was empty."""
        name = name.strip()
        if not name:
            return None
        import uuid
        ws_id = uuid.uuid4().hex[:8]
        ws = {
            "id": ws_id,
            "name": name,
            "members": set(members),
            "locked_by": None,
        }
        if settings:
            ws.update(settings)
        self._workspaces[ws_id] = ws
        self._write_workspace_to_lww(ws_id, ws)
        if self._active_workspace is None:
            self._active_workspace = ws_id
        self._rebuild_activity()
        self._refresh_workspace_pill()
        self._refresh_layout_empty_state()
        self._refresh_layout_display()
        self._sync_allowed_peers_to_peer()
        return ws_id

    def _update_workspace(self, ws_id: str, name: str,
                          members: set[str],
                          *,
                          settings: Optional[dict] = None
                          ) -> None:
        ws = self._workspaces.get(ws_id)
        if ws is None:
            return
        ws["name"] = name.strip() or ws["name"]
        ws["members"] = set(members)
        if settings:
            ws.update(settings)
        self._write_workspace_to_lww(ws_id, ws)
        self._rebuild_activity()
        self._refresh_workspace_pill()
        self._refresh_layout_empty_state()
        self._refresh_layout_display()
        self._sync_allowed_peers_to_peer()

    # Keys we persist alongside name / members / locked_by on every
    # workspace LWW entry. Centralised so the read (LWW → dict) and
    # the write (dict → LWW) stay in lockstep and so every new
    # workspace gets a predictable default set.
    WORKSPACE_SETTING_KEYS = (
        "cb_max_size", "cb_rich", "cb_files",
        "edge_margin", "require_modifier", "block_os_hotkeys",
        "auto_unlock",
    )

    def _collect_form_settings(self) -> dict:
        """Read the current values out of the workspace-edit form
        StringVar/BooleanVars into a dict ready to merge onto the
        workspace object and gossip via LWW."""
        try:
            edge_margin = int(self._ws_form_edge_margin.get().strip() or 6)
        except ValueError:
            edge_margin = 6
        edge_margin = max(0, min(edge_margin, 64))
        return {
            "cb_max_size": self._ws_form_cb_max_size.get(),
            "cb_rich": bool(self._ws_form_cb_rich.get()),
            "cb_files": bool(self._ws_form_cb_files.get()),
            "edge_margin": edge_margin,
            "require_modifier": bool(
                self._ws_form_require_modifier.get()),
            "block_os_hotkeys": bool(
                self._ws_form_block_hotkeys.get()),
            "auto_unlock": self._ws_form_auto_unlock.get(),
        }

    def _refresh_workspace_pill(self) -> None:
        """Kept for compatibility with older call sites — the chip row
        now carries the workspace switcher, so we just re-render it."""
        self._render_workspace_chips()

    # ── Workspace inline form ─────────────────────────────────────

    def _start_create_workspace(self) -> None:
        """Switch Activity into the inline "new workspace" view.
        Resets the form state so previous in-progress input doesn't
        leak between sessions."""
        pc_count = sum(
            1 for mid, info in self._machines_info.items()
            if mid and info
        )
        if pc_count < 2:
            self._set_activity_alert(
                "warn",
                "A workspace needs at least 2 computers on the mesh.",
            )
            return
        self._activity_alert = None
        self._activity_mode = "create"
        self._ws_form_name.set("")
        self._ws_form_members = {}
        # Reset the settings block to "behaves like the old default".
        self._ws_form_cb_max_size.set("1 MB")
        self._ws_form_cb_rich.set(False)
        self._ws_form_cb_files.set(False)
        self._ws_form_edge_margin.set("6")
        self._ws_form_require_modifier.set(False)
        self._ws_form_block_hotkeys.set(False)
        self._ws_form_auto_unlock.set("Off")
        self._ws_form_virtual_count.set("0")
        self._rebuild_activity()

    def _start_edit_workspace(self, ws_id: str) -> None:
        ws = self._workspaces.get(ws_id)
        if ws is None:
            return
        if ws.get("locked_by") and ws["locked_by"] != self._machine_id:
            self._set_activity_alert(
                "warn",
                f"'{ws['name']}' is locked by {ws['locked_by']}. "
                "Unlock it from that computer to edit.",
            )
            return
        self._activity_alert = None
        self._activity_mode = f"edit:{ws_id}"
        self._ws_form_name.set(ws["name"])
        # Seed a BooleanVar for every currently-known PC, pre-checked
        # when the PC is already in this workspace.
        members = ws.get("members", set())
        self._ws_form_members = {
            mid: tk.BooleanVar(master=self.root,
                               value=(mid in members))
            for mid in self._machines_info
            if mid and self._machines_info.get(mid)
        }
        # Seed settings from the existing workspace (defaults when
        # the workspace pre-dates the schema bump).
        self._ws_form_cb_max_size.set(ws.get("cb_max_size") or "1 MB")
        self._ws_form_cb_rich.set(bool(ws.get("cb_rich")))
        self._ws_form_cb_files.set(bool(ws.get("cb_files")))
        self._ws_form_edge_margin.set(str(ws.get("edge_margin") or 6))
        self._ws_form_require_modifier.set(
            bool(ws.get("require_modifier")))
        self._ws_form_block_hotkeys.set(
            bool(ws.get("block_os_hotkeys")))
        self._ws_form_auto_unlock.set(ws.get("auto_unlock") or "Off")
        self._ws_form_virtual_count.set(
            str(ws.get("virtual_displays_per_pc") or 0))
        self._rebuild_activity()

    def _cancel_workspace_form(self) -> None:
        self._activity_mode = "list"
        self._activity_alert = None
        self._rebuild_activity()

    def _activity_workspace_form(self, parent: tk.Widget, *,
                                 ws_id: Optional[str]) -> None:
        ws = self._workspaces.get(ws_id) if ws_id else None
        existing = ws is not None

        # Header: back link + title
        header = tk.Frame(parent, bg=PAPER_BG)
        header.pack(fill=tk.X, pady=(0, SPACE_LG))

        back = tk.Label(header, text="‹ Back",
                        fg=LILAC, bg=PAPER_BG,
                        font=(FONT_SANS, SIZE_SM, "bold"),
                        cursor="hand2")
        back.pack(side=tk.LEFT)
        back.bind("<Button-1>",
                  lambda _e: self._cancel_workspace_form())
        title = ("New workspace" if not existing
                 else f"Edit '{ws['name']}'")
        tk.Label(header, text=title,
                 font=(FONT_SANS, SIZE_TITLE, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
                 ).pack(side=tk.LEFT, padx=(SPACE_LG, 0))

        # Name field
        tk.Label(parent, text="Name",
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_BG, anchor="w"
                 ).pack(anchor="w")
        name_entry = tk.Entry(
            parent, textvariable=self._ws_form_name,
            font=(FONT_SANS, SIZE_BASE), width=40,
            relief=tk.FLAT, bg=PAPER_SURFACE,
            fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
            highlightthickness=1,
            highlightbackground=PAPER_BORDER,
            highlightcolor=LILAC,
        )
        name_entry.pack(anchor="w", ipady=6, pady=(2, SPACE_MD))

        # Members checklist
        tk.Label(parent, text="Members  (at least 2 required)",
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_BG, anchor="w"
                 ).pack(anchor="w")

        assigned_map = self._pc_to_workspace_map()
        all_mids = sorted(
            mid for mid, info in self._machines_info.items()
            if mid and info
        )
        # Lazily create vars for any PC that joined after the form
        # opened — so a new peer announcement while the form is up
        # shows up as a selectable row.
        for mid in all_mids:
            if mid not in self._ws_form_members:
                self._ws_form_members[mid] = tk.BooleanVar(
                    master=self.root, value=False)

        checklist = tk.Frame(parent, bg=PAPER_BG)
        checklist.pack(fill=tk.X, pady=(2, SPACE_MD))
        if not all_mids:
            tk.Label(
                checklist,
                text="No computers are connected yet.",
                font=(FONT_SANS, SIZE_BASE),
                fg=PAPER_MUTED, bg=PAPER_BG,
            ).pack(anchor="w")

        for mid in all_mids:
            other_ws_id = assigned_map.get(mid)
            other_ws = (self._workspaces.get(other_ws_id)
                        if other_ws_id else None)
            other_locked = bool(other_ws and other_ws.get("locked_by"))
            locked_elsewhere = (other_locked
                                and other_ws_id != ws_id)
            var = self._ws_form_members[mid]
            if locked_elsewhere and var.get():
                var.set(False)  # can't claim a locked PC — force off

            row = tk.Frame(checklist, bg=PAPER_BG)
            row.pack(fill=tk.X, pady=2)
            tk.Checkbutton(
                row, variable=var,
                bg=PAPER_BG, fg=PAPER_TEXT,
                activebackground=PAPER_BG,
                selectcolor=PAPER_BG,
                state=tk.DISABLED if locked_elsewhere else tk.NORMAL,
                highlightthickness=0, bd=0,
            ).pack(side=tk.LEFT)

            label_parts = [mid]
            if other_ws_id and other_ws_id != ws_id:
                other_name = (other_ws["name"] if other_ws
                              else "another workspace")
                if other_locked:
                    label_parts.append(
                        f"(in {other_name} — locked)")
                else:
                    label_parts.append(f"(in {other_name})")
            tk.Label(
                row, text=" · ".join(label_parts),
                font=(FONT_SANS, SIZE_SM),
                fg=(PAPER_FAINT if locked_elsewhere else PAPER_TEXT),
                bg=PAPER_BG, anchor="w",
            ).pack(side=tk.LEFT, padx=(SPACE_SM, 0))

        # ── Clipboard ───────────────────────────────────────────
        self._form_section_header(parent, "Clipboard")
        self._form_dropdown(
            parent, "Max text size",
            self._ws_form_cb_max_size,
            ("100 KB", "1 MB", "5 MB", "10 MB", "Unlimited"),
        )
        self._form_checkbox(
            parent, "Include rich text / images",
            self._ws_form_cb_rich,
        )
        self._form_checkbox(
            parent, "Include files",
            self._ws_form_cb_files,
        )

        # ── Cursor ──────────────────────────────────────────────
        self._form_section_header(parent, "Cursor")
        self._form_text_entry(
            parent, "Edge margin (px)",
            self._ws_form_edge_margin, width=6,
        )
        self._form_checkbox(
            parent, "Hold Ctrl+Shift to move cursor to another "
                    "computer",
            self._ws_form_require_modifier,
        )
        self._form_checkbox(
            parent, "Block OS hotkeys from forwarding "
                    "(Win+L, Ctrl+Alt+Del, …)",
            self._ws_form_block_hotkeys,
        )

        # ── Auto-unlock ─────────────────────────────────────────
        self._form_section_header(parent, "Auto-unlock")
        self._form_dropdown(
            parent, "After idle",
            self._ws_form_auto_unlock,
            ("Off", "5 min", "15 min", "1 hour"),
        )

        # Virtual-display configuration used to live here as a
        # per-workspace count; it moved to the Layout tab's Routing
        # canvas where each PC's "+" slot creates a phantom monitor
        # directly and lines route them onto physical sinks. Keeping
        # the hint in Settings so the user can still see their host's
        # driver status.
        self._form_section_header(parent, "Virtual displays")
        hint_color = (PAPER_MUTED if self._virtual_displays.available
                      else PAPER_FAINT)
        tk.Label(
            parent,
            text=f"{self._virtual_displays.caps.detail}\n"
                 "Add or remove virtual displays in Layout → Routing.",
            font=(FONT_SANS, SIZE_XS),
            fg=hint_color, bg=PAPER_BG,
            wraplength=420, justify="left", anchor="w",
        ).pack(fill=tk.X, pady=(0, SPACE_XS))

        # Footer buttons
        btn_row = tk.Frame(parent, bg=PAPER_BG)
        btn_row.pack(fill=tk.X, pady=(SPACE_LG, 0))
        PillButton(btn_row, "Cancel", variant="secondary",
                   command=self._cancel_workspace_form,
                   ).pack(side=tk.RIGHT, padx=(SPACE_SM, 0))
        PillButton(btn_row,
                   "Create" if not existing else "Save",
                   variant="primary",
                   command=self._submit_workspace_form,
                   ).pack(side=tk.RIGHT)
        if existing:
            PillButton(btn_row, "Delete", variant="danger",
                       command=lambda wid=ws_id:
                           self._start_delete_confirm(wid),
                       ).pack(side=tk.LEFT)

        name_entry.focus_set()

    # ── Form helpers ──────────────────────────────────────────────

    def _form_section_header(self, parent: tk.Widget, text: str) -> None:
        tk.Label(parent, text=text,
                 font=(FONT_SANS, SIZE_LG, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
                 ).pack(fill=tk.X, pady=(SPACE_LG, SPACE_XS))

    def _form_checkbox(self, parent: tk.Widget, text: str,
                       var: tk.BooleanVar) -> None:
        row = tk.Frame(parent, bg=PAPER_BG)
        row.pack(fill=tk.X, pady=2)
        tk.Checkbutton(
            row, variable=var,
            bg=PAPER_BG, fg=PAPER_TEXT,
            activebackground=PAPER_BG, selectcolor=PAPER_BG,
            highlightthickness=0, bd=0,
        ).pack(side=tk.LEFT)
        tk.Label(
            row, text=text,
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
            wraplength=440, justify="left",
        ).pack(side=tk.LEFT, padx=(SPACE_SM, 0))

    def _form_dropdown(self, parent: tk.Widget, label: str,
                       var: tk.StringVar, values) -> None:
        row = tk.Frame(parent, bg=PAPER_BG)
        row.pack(fill=tk.X, pady=2)
        tk.Label(
            row, text=label, width=22, anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(side=tk.LEFT)
        self._paper_dropdown(row, var, values,
                             bg_base=PAPER_BG).pack(side=tk.LEFT)

    def _form_text_entry(self, parent: tk.Widget, label: str,
                         var: tk.StringVar, *, width: int = 40) -> None:
        row = tk.Frame(parent, bg=PAPER_BG)
        row.pack(fill=tk.X, pady=2)
        tk.Label(
            row, text=label, width=22, anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(side=tk.LEFT)
        tk.Entry(
            row, textvariable=var,
            font=(FONT_SANS, SIZE_SM), width=width,
            relief=tk.FLAT, bg=PAPER_SURFACE,
            fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
            highlightthickness=1,
            highlightbackground=PAPER_BORDER,
            highlightcolor=LILAC,
        ).pack(side=tk.LEFT, ipady=4)

    def _submit_workspace_form(self) -> None:
        mode = self._activity_mode
        ws_id = mode.split(":", 1)[1] if mode.startswith("edit:") else None

        name = self._ws_form_name.get().strip()
        if not name:
            self._set_activity_alert("warn",
                                     "Give your workspace a name.")
            return
        selected = {mid for mid, var in self._ws_form_members.items()
                    if var.get()}
        if len(selected) < 2:
            self._set_activity_alert(
                "warn",
                "A workspace must have at least 2 computers.",
            )
            return

        # Claim members from other UNLOCKED workspaces. Each member can
        # only belong to one workspace, so selecting it here strips it
        # from its previous home. Locked memberships were filtered out
        # of the checklist earlier so we don't need to re-validate.
        assigned_map = self._pc_to_workspace_map()
        affected_src_ids: set[str] = set()
        for mid in selected:
            src_id = assigned_map.get(mid)
            if src_id and src_id != ws_id:
                src = self._workspaces.get(src_id)
                if src and not src.get("locked_by"):
                    src["members"].discard(mid)
                    affected_src_ids.add(src_id)

        form_settings = self._collect_form_settings()

        # Create or update the target workspace first so it exists in
        # LWW before we potentially auto-delete a source that the user
        # had been viewing — keeps the Layout tab from flickering
        # through a "no active workspace" state.
        if ws_id is None:
            target_id = self._create_workspace(
                name, selected, settings=form_settings)
        else:
            self._update_workspace(
                ws_id, name, selected, settings=form_settings)
            target_id = ws_id

        # Resolve sources: push the reduced member list to LWW, OR
        # auto-delete the whole workspace if it's dropped below the
        # two-computer minimum. "A workspace with one PC" would be a
        # degenerate state we don't want to render — better to melt
        # it and return its last member to Unassigned.
        for src_id in list(affected_src_ids):
            src = self._workspaces.get(src_id)
            if src is None:
                continue
            if len(src.get("members") or ()) < 2:
                self._delete_workspace(src_id)
            else:
                self._write_workspace_to_lww(src_id, src)

        # Source-deletion might have cleared the active workspace.
        # Fall back to the one we just created / edited so Layout
        # never sits on a stale "no workspace" cover right after the
        # user explicitly set up a workspace.
        if (self._active_workspace is None
                or self._active_workspace not in self._workspaces):
            if target_id and target_id in self._workspaces:
                self._switch_active_workspace(target_id)

        self._activity_mode = "list"
        self._activity_alert = None
        self._rebuild_activity()

    def _start_delete_confirm(self, ws_id: str) -> None:
        ws = self._workspaces.get(ws_id)
        if ws is None:
            return
        if ws.get("locked_by") and ws["locked_by"] != self._machine_id:
            self._set_activity_alert(
                "warn",
                f"'{ws['name']}' is locked by {ws['locked_by']}. "
                "Unlock it from that computer to delete.",
            )
            return
        self._activity_mode = f"delete:{ws_id}"
        self._activity_alert = None
        self._rebuild_activity()

    def _activity_delete_confirm(self, parent: tk.Widget,
                                 ws_id: str) -> None:
        ws = self._workspaces[ws_id]

        header = tk.Frame(parent, bg=PAPER_BG)
        header.pack(fill=tk.X, pady=(0, SPACE_LG))
        back = tk.Label(header, text="‹ Back",
                        fg=LILAC, bg=PAPER_BG,
                        font=(FONT_SANS, SIZE_SM, "bold"),
                        cursor="hand2")
        back.pack(side=tk.LEFT)
        back.bind("<Button-1>",
                  lambda _e: self._cancel_workspace_form())

        card = tk.Frame(parent, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_LG)
        card.pack(fill=tk.X)
        tk.Label(
            card, text=f"Delete '{ws['name']}'?",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_SURFACE, anchor="w",
        ).pack(fill=tk.X)
        tk.Label(
            card,
            text="Its computers will return to the Unassigned list.",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_SURFACE, anchor="w",
        ).pack(fill=tk.X, pady=(SPACE_XS, SPACE_MD))

        row = tk.Frame(card, bg=PAPER_SURFACE)
        row.pack(fill=tk.X)
        PillButton(row, "Cancel", variant="secondary",
                   command=self._cancel_workspace_form,
                   ).pack(side=tk.RIGHT, padx=(SPACE_SM, 0))
        PillButton(row, "Delete", variant="danger",
                   command=lambda wid=ws_id:
                       self._delete_workspace(wid),
                   ).pack(side=tk.RIGHT)

    def _machine_tile(self, parent: tk.Widget,
                      machine_id: str, info: dict) -> tk.Widget:
        is_muted = bool(info.get("muted"))
        # Default True so servers that haven't shipped the toggle yet
        # still read as "sync on" rather than "off".
        clipboard_on = bool(info.get("clipboard_sync", True))
        # Static per-machine accent colour, matching the same colour
        # the Layout canvas uses for this machine's displays. Doesn't
        # flip when the shared cursor moves — that "active" signal
        # lives only on the Layout canvas.
        accent = machine_color(machine_id)

        card = tk.Frame(parent, bg=PAPER_SURFACE)
        strip = tk.Frame(card, bg=accent, width=4)
        strip.pack(side=tk.LEFT, fill=tk.Y)

        # Toggles parked on the right of the card and vertically
        # centered against the text block on the left. The wrap frame
        # takes the full card height; the inner pill_row uses
        # expand=True without fill to land centered in that space.
        toggles_wrap = tk.Frame(card, bg=PAPER_SURFACE,
                                padx=SPACE_LG, pady=SPACE_MD)
        toggles_wrap.pack(side=tk.RIGHT, fill=tk.Y)
        pill_row = tk.Frame(toggles_wrap, bg=PAPER_SURFACE)
        pill_row.pack(expand=True)
        # Click callbacks look up the live state via _machines_info
        # instead of a snapshot captured here — so when _refresh_tile_pills
        # updates the pill in place (without rebuilding the tile) the
        # click still flips to the correct new value.
        input_pill = self._state_pill(
            pill_row, label="Input", on=not is_muted,
            on_click=lambda mid=machine_id:
                self._set_input_muted(
                    mid,
                    not bool(self._machines_info.get(mid, {}).get(
                        "muted", False)),
                ),
        )
        input_pill.pack(side=tk.LEFT, padx=(0, SPACE_XS))
        clipboard_pill = self._state_pill(
            pill_row, label="Clipboard", on=clipboard_on,
            on_click=lambda mid=machine_id:
                self._set_clipboard_sync(
                    mid,
                    not bool(self._machines_info.get(mid, {}).get(
                        "clipboard_sync", True)),
                ),
        )
        clipboard_pill.pack(side=tk.LEFT)
        self._tile_pills[machine_id] = {
            "input": input_pill,
            "clipboard": clipboard_pill,
        }

        body = tk.Frame(card, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_MD)
        body.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        row = tk.Frame(body, bg=PAPER_SURFACE)
        row.pack(fill=tk.X)
        tk.Label(
            row, text=machine_id,
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)

        os_label = info.get("platform_info") or info.get("os") or ""
        if os_label:
            tk.Label(
                body, text=os_label,
                font=(FONT_SANS, SIZE_SM),
                fg=PAPER_MUTED, bg=PAPER_SURFACE, anchor="w",
            ).pack(fill=tk.X, pady=(2, 0))

        return card

    def _state_pill(self, parent: tk.Widget, *,
                    label: str, on: bool,
                    on_click: Callable[[], None]) -> "PillButton":
        pill = PillButton(parent, "", variant="primary", size=SIZE_XS)
        self._paint_state_pill(pill, label=label, on=on)
        pill._command = on_click
        return pill

    def _paint_state_pill(self, pill: "PillButton", *,
                          label: str, on: bool) -> None:
        """Update an existing pill's text + colours in place, used by
        _refresh_tile_pills so mute / clipboard toggles don't rebuild
        the Activity tile frame. No-op if the pill's already showing
        the requested state — avoids a tk.configure round-trip (and
        the hover flash it can cause) on every spurious refresh."""
        state = (label, bool(on))
        if getattr(pill, "_unio_state", None) == state:
            return
        pill._unio_state = state
        text = f"{label} · {'ON' if on else 'OFF'}"
        pill.configure(text=text)
        if on:
            pill._bg, pill._fg = LILAC, "#ffffff"
            pill._hover_bg, pill._hover_fg = LILAC, "#ffffff"
        else:
            pill._bg, pill._fg = PAPER_BG, PAPER_MUTED
            pill._hover_bg, pill._hover_fg = PAPER_BG, PAPER_MUTED
        pill.configure(bg=pill._bg, fg=pill._fg)

    def _set_input_muted(self, machine_id: str, muted: bool) -> None:
        if self._peer is not None:
            self._peer.set_input_muted(machine_id, muted)

    def _set_clipboard_sync(self, machine_id: str, enabled: bool) -> None:
        if self._peer is not None:
            self._peer.set_clipboard_sync(machine_id, enabled)

    # ── Layout + Settings + Logs (placeholders for this commit) ──

    def _build_layout_tab(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)

        # Workspace bar across the top of the Layout tab.
        self._build_workspace_bar(frame)

        # Single canvas with an Arrange ↔ Route mode toggle built into
        # LayoutPanel itself. Both concerns (physical adjacency and
        # routing) render on the same surface — mode changes which
        # gestures are active and how prominent the routing lines are.
        self.layout_panel = LayoutPanel(
            frame,
            on_apply=self._apply_layout,
            on_identify=self._request_identify,
            on_reroute=self._on_layout_reroute,
            on_add_virtual=self._on_add_virtual_display,
            on_remove_virtual=self._on_remove_virtual_display,
            on_add_hub=self._on_add_hub,
            on_remove_hub=self._on_remove_hub,
            virtuals_provider=self._collect_virtual_displays,
            hubs_provider=self._collect_hubs,
            sources_provider=self._collect_mesh_sources,
        )
        self.layout_panel.pack(fill=tk.BOTH, expand=True)

        # If no workspace is active, cover the panel with an empty
        # state — Layout only makes sense in the context of a
        # workspace, and the user's next step is creating one.
        self._layout_empty_state: Optional[tk.Frame] = None
        self._refresh_layout_empty_state()

        if self._last_monitors:
            self.root.after_idle(self._refresh_layout_display)
        return frame

    def _workspace_filtered_monitors(
            self, monitors: list[dict]) -> list[dict]:
        """Restrict the monitor list to the PCs that belong to the
        active workspace. Keeps the Layout canvas focused on the
        cluster you're arranging, not every discovered PC. Returns an
        empty list when no workspace is active — the canvas already
        has the "pick a workspace" cover in that case."""
        if not self._active_workspace:
            return []
        ws = self._workspaces.get(self._active_workspace)
        if not ws:
            return []
        members = ws.get("members", set())
        return [m for m in monitors
                if m.get("machine_id") in members]

    def _refresh_layout_display(self) -> None:
        """Push the current _last_monitors (filtered by the active
        workspace) into the Layout canvas. Called when the active
        workspace changes or its membership is edited."""
        if self.layout_panel is None:
            return
        filtered = self._workspace_filtered_monitors(
            self._last_monitors)
        self.layout_panel.set_displays(filtered)
        self.layout_panel.set_active_machine(self._active_machine)

    def _sync_allowed_peers_to_peer(self) -> None:
        """Tell the Peer which machine_ids the user has opted into
        sharing with right now — i.e. the members of the active
        workspace. Outside that set: no cursor handoff, no keyboard
        forwarding, no clipboard sync. Two PCs on the mesh that
        share no workspace behave as if the other doesn't exist,
        even though discovery + LWW gossip still flow between them."""
        if self._peer is None:
            return
        if (self._active_workspace
                and self._active_workspace in self._workspaces):
            ws = self._workspaces[self._active_workspace]
            members = ws.get("members", set())
            settings = {k: ws[k] for k in self.WORKSPACE_SETTING_KEYS
                        if k in ws}
        else:
            members = set()
            settings = None
        self._peer.set_allowed_peers(members)
        try:
            self._peer.set_workspace_settings(settings)
        except Exception:
            log.exception("set_workspace_settings failed")

    def _build_workspace_bar(self, parent: tk.Widget) -> tk.Frame:
        """Layout-tab header: page title + inline workspace chips.

        Replaces the old `[pill ▾]` dropdown — the tk.Menu popup used
        OS-default styling which fought the paper + lilac look, and
        clicking it spawned a separate window the user didn't want.
        Chips render inline, match the PillButton style, and make the
        whole switcher a single one-click gesture."""
        # Asymmetric pady MUST go on pack(), not the Frame constructor
        # (TclError "bad screen distance" on a tuple aborts the tab
        # mid-build — same class of bug as 6273da9).
        bar = tk.Frame(parent, bg=PAPER_BG, padx=SPACE_LG)
        bar.pack(fill=tk.X, pady=(SPACE_LG, SPACE_SM))

        # No page title here — the rail already says "Layout", and
        # the user felt the duplicate heading was wasted vertical
        # space. Workspace chips are the entire header now.
        chips = tk.Frame(bar, bg=PAPER_BG)
        chips.pack(anchor="w", fill=tk.X)
        self._workspace_chip_row = chips
        self._workspace_pill = None
        self._render_workspace_chips()
        return bar

    def _render_workspace_chips(self) -> None:
        row = getattr(self, "_workspace_chip_row", None)
        if row is None or not row.winfo_exists():
            return
        for w in row.winfo_children():
            w.destroy()

        if not self._workspaces:
            # No hint needed here — the Activity tab in the left
            # rail is right there; the empty chip row alone is a
            # quiet signal that there's nothing to pick.
            return

        # "Workspace:" is now the subtitle-ish label sitting under the
        # page title. Plain black at the same size as the sibling hint
        # text above — no Manage link on the right, the user just
        # clicks the Activity tab to manage.
        tk.Label(
            row, text="Workspace:",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(side=tk.LEFT, padx=(0, SPACE_MD))

        for ws_id in sorted(self._workspaces,
                            key=lambda k: self._workspaces[k]["name"]):
            ws = self._workspaces[ws_id]
            active = ws_id == self._active_workspace
            # The Activity-tab card carries the authoritative lock
            # glyph; the chip stays text-only so switching stays a
            # clean one-click gesture without emoji-font quirks on
            # Windows eating the character silently.
            chip = self._workspace_chip(
                row, ws["name"],
                active=active,
                on_click=lambda wid=ws_id:
                    self._switch_active_workspace(wid),
            )
            chip.pack(side=tk.LEFT, padx=(0, SPACE_SM))

    def _workspace_chip(self, parent: tk.Widget, text: str, *,
                        active: bool,
                        on_click) -> tk.Label:
        """Pill-shaped workspace chip matching the app's PillButton
        language. Active chip = filled lilac; inactive = paper surface
        with a border. Swapping on click flips the two without any
        popup."""
        if active:
            bg, fg = LILAC, "#ffffff"
            hover_bg, hover_fg = LILAC_HOVER, "#ffffff"
        else:
            bg, fg = PAPER_SURFACE, PAPER_TEXT
            hover_bg, hover_fg = LILAC_SOFT, LILAC
        chip = tk.Label(
            parent, text=text,
            bg=bg, fg=fg,
            font=(FONT_SANS, SIZE_SM, "bold"),
            padx=SPACE_MD, pady=5,
            cursor="hand2",
        )
        chip.bind("<Button-1>", lambda _e: on_click())
        chip.bind("<Enter>",
                  lambda _e: chip.configure(bg=hover_bg, fg=hover_fg))
        chip.bind("<Leave>",
                  lambda _e: chip.configure(bg=bg, fg=fg))
        return chip

    def _switch_active_workspace(self, ws_id: str) -> None:
        if ws_id not in self._workspaces:
            return
        self._active_workspace = ws_id
        self._refresh_workspace_pill()
        self._refresh_layout_empty_state()
        self._refresh_layout_display()
        self._sync_allowed_peers_to_peer()
        self._sync_own_virtuals_to_stream_server()
        # Route map is per-workspace, so flipping workspaces means
        # every active stream window AND source overlay needs
        # re-evaluating — some will close, some will start.
        try:
            self._sync_display_streams()
            self._sync_source_overlays()
        except Exception:
            log.exception("sync display streams after workspace switch failed")
        self._refresh_layout_routes()

    def _jump_to_activity_for_workspaces(self) -> None:
        self._active_tab.set("activity")

    def _refresh_layout_empty_state(self) -> None:
        """Show or hide the "pick / create a workspace" cover over the
        Layout canvas depending on whether a workspace is active."""
        existing = getattr(self, "_layout_empty_state", None)
        has_active = (
            self._active_workspace is not None
            and self._active_workspace in self._workspaces
        )
        if has_active:
            if existing is not None and existing.winfo_exists():
                existing.place_forget()
                existing.destroy()
            self._layout_empty_state = None
            return
        if existing is not None and existing.winfo_exists():
            # Already showing — nothing to do.
            return
        if self.layout_panel is None:
            return
        cover = tk.Frame(self.layout_panel, bg=PAPER_BG)
        cover.place(relx=0, rely=0, relwidth=1, relheight=1)
        inner = tk.Frame(cover, bg=PAPER_BG)
        inner.place(relx=0.5, rely=0.5, anchor="center")
        tk.Label(
            inner, text="No workspace created",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_SM))
        tk.Label(
            inner,
            text=("Create a workspace in the Activity tab to arrange "
                  "the displays of the computers inside it."),
            wraplength=420, justify="center",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack()
        self._layout_empty_state = cover

    def _build_settings_tab(self, parent: tk.Widget) -> tk.Widget:
        """Four-section Settings: Local / Troubleshoot / Network /
        Diagnostics. Pure UI in this pass — every toggle and value
        persists via _settings_state so the form state survives a
        rebuild, but the actual behaviour wiring (autostart,
        interface filter, port, log folder) lands in a follow-up."""
        # Lazily create the state store + its StringVar/BooleanVars
        # the first time Settings is built, so defaults land without
        # having to hand-wire them inside __init__.
        if not hasattr(self, "_settings_vars"):
            from ..core import autostart as _autostart
            from ..core import user_config as _user_config
            saved = _user_config.load_config()
            self._settings_vars = {
                "autostart":    tk.BooleanVar(
                    master=self.root,
                    value=bool(saved.get("autostart",
                                         _autostart.is_autostart_enabled()))),
                "troubleshoot_enabled": tk.BooleanVar(
                    master=self.root,
                    value=bool(saved.get("troubleshoot_enabled", True))),
                "log_folder":   tk.StringVar(
                    master=self.root,
                    value=saved.get("log_folder",
                                    str(_user_log_dir()))),
                "log_max_size": tk.StringVar(
                    master=self.root,
                    value=saved.get("log_max_size", "300 KB")),
                "tcp_port":     tk.StringVar(
                    master=self.root,
                    value=str(saved.get("tcp_port", DEFAULT_PORT))),
            }
            # Interface checklist — one BooleanVar per discovered IPv4
            # interface. Re-evaluated on every rebuild so newly-added
            # NICs show up.
            self._settings_iface_vars: dict[str, tk.BooleanVar] = {}
            saved_ifaces = saved.get("disabled_interfaces") or []
            self._settings_disabled_ifaces: set[str] = set(saved_ifaces)
            # Every Settings var writes back to config.yaml on change.
            # StringVar traces fire on .set(...) so the dropdown /
            # entry / path-picker all trigger persistence automatically.
            for var in self._settings_vars.values():
                var.trace_add("write",
                              lambda *_: self._on_settings_changed())

        # Settings is long enough to want scrolling — use the same
        # scroll-canvas + mouse-wheel + middle-click-pan helper as
        # Activity. tab_key="settings" makes sure wheel scroll only
        # engages while Settings is visible.
        frame, inner, attach_pan = self._build_scrollable(parent,
                                                          "settings")
        self._settings_attach_pan = attach_pan
        content = tk.Frame(inner, bg=PAPER_BG,
                           padx=SPACE_XL, pady=SPACE_LG)
        content.pack(fill=tk.BOTH, expand=True)

        # ── Local Settings ─────────────────────────────────────
        self._settings_heading(content, "Local Settings",
                               "These apply to this computer only.")
        local = self._settings_card(content)
        self._settings_checkbox(
            local, "Autostart at login",
            self._settings_vars["autostart"],
            hint="Launch unIO when this computer starts, so the mesh "
                 "rejoins without opening the app manually.",
        )

        # ── Troubleshoot ───────────────────────────────────────
        self._settings_heading(
            content, "Troubleshoot",
            "unIO only records peer IDs, timestamps, and app events — "
            "no clipboard content, keystrokes, window titles, or user "
            "data.",
        )
        trouble = self._settings_card(content)
        self._settings_checkbox(
            trouble, "Enable troubleshooting logs",
            self._settings_vars["troubleshoot_enabled"],
            hint="When off, unIO writes nothing to disk. Turn on "
                 "only to diagnose an issue, then send the zipped "
                 "logs to support.",
        )
        self._settings_path_row(
            trouble, "Log folder",
            self._settings_vars["log_folder"],
        )
        self._settings_dropdown_row(
            trouble, "Max log file size",
            self._settings_vars["log_max_size"],
            ("100 KB", "300 KB", "1 MB", "5 MB"),
        )
        self._settings_action_row(
            trouble, "Package logs for support",
            "Zip & open folder",
            self._settings_package_logs,
        )

        # ── Network ────────────────────────────────────────────
        self._settings_heading(
            content, "Network",
            "Interfaces and port used by discovery and peer links.",
        )
        net = self._settings_card(content)
        self._settings_interface_checklist(net)
        self._settings_entry_row(
            net, "TCP port",
            self._settings_vars["tcp_port"], width=8,
        )

        # ── Diagnostics ────────────────────────────────────────
        self._settings_heading(
            content, "Diagnostics",
            "Live status of this computer on the mesh.",
        )
        diag = tk.Frame(content, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_LG)
        diag.pack(fill=tk.X, pady=(0, SPACE_LG))
        self._kv_row(diag, "Version", unio.__version__)
        self._kv_row(diag, "Hostname", socket.gethostname() or "—")
        self._kv_row(diag, "Machine ID", self._machine_id)
        self._kv_row(diag, "Platform", _describe_platform())
        try:
            from ..core.discovery import local_ipv4_interfaces
            ifaces = local_ipv4_interfaces()
            self._kv_row(
                diag, "Interfaces",
                ", ".join(f"{i.name}={i.ip}" for i in ifaces) or "—",
            )
        except Exception:
            self._kv_row(diag, "Interfaces", "—")

        if unio.DEV_LOGS:
            self._settings_heading(
                content, "Developer",
                "Only visible in dev builds (UNIO_DEV_LOGS=1).",
            )
            dev = self._settings_card(content)
            PillButton(dev, "Open log viewer",
                       command=lambda: show_log_window(self.root),
                       variant="secondary"
                       ).pack(anchor="w")

        # Attach middle-click pan bindings recursively, so drag-to-
        # scroll works anywhere over the Settings page (not only on
        # the empty canvas margin).
        try:
            attach_pan(content)
        except Exception:
            pass

        return frame

    # ── Settings helpers ──────────────────────────────────────────

    def _settings_card(self, parent: tk.Widget) -> tk.Frame:
        card = tk.Frame(parent, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_LG)
        card.pack(fill=tk.X, pady=(0, SPACE_LG))
        return card

    def _settings_checkbox(self, parent: tk.Widget, label: str,
                           var: tk.BooleanVar, *,
                           hint: Optional[str] = None) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=2)
        tk.Checkbutton(
            row, variable=var,
            bg=PAPER_SURFACE, fg=PAPER_TEXT,
            activebackground=PAPER_SURFACE,
            selectcolor=PAPER_SURFACE,
            highlightthickness=0, bd=0,
        ).pack(side=tk.LEFT)
        tk.Label(
            row, text=label,
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_SURFACE, anchor="w",
        ).pack(side=tk.LEFT, padx=(SPACE_SM, 0))
        if hint:
            tk.Label(
                parent, text=hint,
                font=(FONT_SANS, SIZE_XS),
                fg=PAPER_MUTED, bg=PAPER_SURFACE, anchor="w",
                wraplength=520, justify="left",
            ).pack(fill=tk.X, padx=(24, 0), pady=(0, SPACE_SM))

    def _settings_dropdown_row(self, parent: tk.Widget, label: str,
                               var: tk.StringVar, values) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=4)
        tk.Label(
            row, text=label, width=22, anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)
        self._paper_dropdown(row, var, values,
                             bg_base=PAPER_SURFACE).pack(side=tk.LEFT)

    # ── Paper-themed dropdown ─────────────────────────────────────

    def _paper_dropdown(self, parent: tk.Widget, var: tk.StringVar,
                        values, *,
                        bg_base: str = PAPER_SURFACE) -> tk.Widget:
        """Drop-in replacement for tk.OptionMenu that respects the
        paper + lilac palette. Rendered as a pill-shaped trigger with
        a " ▾" caret; on click it pops an overrideredirect Toplevel
        containing the options, each with lilac-on-hover feedback.

        bg_base should match the parent background so the trigger's
        outer 1-px border reads clean."""
        trigger = tk.Label(
            parent,
            text=f"{var.get()}  ▾",
            bg=PAPER_BG, fg=PAPER_TEXT,
            font=(FONT_SANS, SIZE_SM),
            padx=SPACE_MD, pady=5,
            cursor="hand2",
            highlightthickness=1,
            highlightbackground=PAPER_BORDER,
            bd=0, relief=tk.FLAT,
        )

        def _refresh_label(*_):
            trigger.configure(text=f"{var.get()}  ▾")
        var.trace_add("write", _refresh_label)

        state = {"popup": None}

        def _close_popup():
            if state["popup"] is not None:
                try:
                    state["popup"].destroy()
                except tk.TclError:
                    pass
                state["popup"] = None

        def _open_popup(_e=None):
            if state["popup"] is not None:
                _close_popup()
                return
            trigger.update_idletasks()
            x = trigger.winfo_rootx()
            y = trigger.winfo_rooty() + trigger.winfo_height()
            w = trigger.winfo_width()

            popup = tk.Toplevel(trigger)
            popup.overrideredirect(True)
            try:
                popup.attributes("-topmost", True)
            except tk.TclError:
                pass
            popup.configure(bg=PAPER_BORDER)

            inner = tk.Frame(popup, bg=PAPER_BG)
            inner.pack(padx=1, pady=1, fill=tk.BOTH, expand=True)

            for val in values:
                active = (str(val) == var.get())
                item = tk.Label(
                    inner, text=str(val),
                    font=(FONT_SANS, SIZE_SM),
                    bg=(LILAC_SOFT if active else PAPER_BG),
                    fg=(LILAC if active else PAPER_TEXT),
                    padx=SPACE_MD, pady=6,
                    cursor="hand2", anchor="w",
                )
                item.pack(fill=tk.X)

                def _enter(_e, w=item):
                    w.configure(bg=LILAC_SOFT, fg=LILAC)

                def _leave(_e, w=item, a=active):
                    w.configure(bg=(LILAC_SOFT if a else PAPER_BG),
                                fg=(LILAC if a else PAPER_TEXT))

                def _pick(_e, v=val):
                    var.set(str(v))
                    _close_popup()

                item.bind("<Enter>", _enter)
                item.bind("<Leave>", _leave)
                item.bind("<Button-1>", _pick)

            popup.update_idletasks()
            req_w = max(popup.winfo_reqwidth(), w)
            popup.geometry(f"{req_w}x{popup.winfo_reqheight()}+{x}+{y}")
            state["popup"] = popup

            # Close when the user clicks anywhere else.
            def _maybe_close(ev):
                if state["popup"] is None:
                    return
                widget = ev.widget
                while widget is not None:
                    if widget is popup:
                        return
                    try:
                        widget = widget.master
                    except AttributeError:
                        break
                _close_popup()
            popup._close_binding = self.root.bind_all(
                "<Button-1>", _maybe_close, add="+")

        def _enter(_e):
            trigger.configure(highlightbackground=LILAC)

        def _leave(_e):
            trigger.configure(highlightbackground=PAPER_BORDER)

        trigger.bind("<Button-1>", _open_popup)
        trigger.bind("<Enter>", _enter)
        trigger.bind("<Leave>", _leave)
        return trigger

    def _settings_entry_row(self, parent: tk.Widget, label: str,
                            var: tk.StringVar,
                            *, width: int = 40) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=4)
        tk.Label(
            row, text=label, width=22, anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)
        tk.Entry(
            row, textvariable=var,
            font=(FONT_SANS, SIZE_SM), width=width,
            relief=tk.FLAT, bg=PAPER_BG,
            fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
            highlightthickness=1,
            highlightbackground=PAPER_BORDER,
            highlightcolor=LILAC,
        ).pack(side=tk.LEFT, ipady=4)

    def _settings_path_row(self, parent: tk.Widget, label: str,
                           var: tk.StringVar) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=4)
        tk.Label(
            row, text=label, width=22, anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)
        tk.Entry(
            row, textvariable=var,
            font=(FONT_SANS, SIZE_SM), width=40,
            relief=tk.FLAT, bg=PAPER_BG,
            fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
            highlightthickness=1,
            highlightbackground=PAPER_BORDER,
            highlightcolor=LILAC,
        ).pack(side=tk.LEFT, ipady=4, padx=(0, SPACE_SM))
        PillButton(row, "Change…", variant="secondary", size=SIZE_XS,
                   command=lambda v=var: self._settings_pick_folder(v),
                   ).pack(side=tk.LEFT)

    def _settings_action_row(self, parent: tk.Widget, label: str,
                             button_text: str, command) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=4)
        tk.Label(
            row, text=label, width=22, anchor="w",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)
        PillButton(row, button_text, variant="secondary", size=SIZE_XS,
                   command=command).pack(side=tk.LEFT)

    def _settings_interface_checklist(self, parent: tk.Widget) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=4)
        tk.Label(
            row, text="Interfaces", width=22, anchor="nw",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT, anchor="n")
        stack = tk.Frame(row, bg=PAPER_SURFACE)
        stack.pack(side=tk.LEFT, fill=tk.X, expand=True)
        try:
            from ..core.discovery import local_ipv4_interfaces
            ifaces = local_ipv4_interfaces()
        except Exception:
            ifaces = []
        if not ifaces:
            tk.Label(
                stack, text="No interfaces detected yet.",
                font=(FONT_SANS, SIZE_SM),
                fg=PAPER_MUTED, bg=PAPER_SURFACE,
            ).pack(anchor="w")
            return
        for i in ifaces:
            if i.name not in self._settings_iface_vars:
                # Initial value reflects persisted state — if the
                # interface was previously unchecked we honour that.
                initial = i.name not in self._settings_disabled_ifaces
                var = tk.BooleanVar(master=self.root, value=initial)
                self._settings_iface_vars[i.name] = var

                def _on_iface_change(name=i.name, v=var):
                    if v.get():
                        self._settings_disabled_ifaces.discard(name)
                    else:
                        self._settings_disabled_ifaces.add(name)
                    self._on_settings_changed()
                var.trace_add("write", lambda *_, cb=_on_iface_change: cb())
            var = self._settings_iface_vars[i.name]
            sub = tk.Frame(stack, bg=PAPER_SURFACE)
            sub.pack(fill=tk.X, anchor="w", pady=1)
            tk.Checkbutton(
                sub, variable=var,
                bg=PAPER_SURFACE, fg=PAPER_TEXT,
                activebackground=PAPER_SURFACE,
                selectcolor=PAPER_SURFACE,
                highlightthickness=0, bd=0,
            ).pack(side=tk.LEFT)
            tk.Label(
                sub, text=f"{i.name}  ·  {i.ip}",
                font=(FONT_SANS, SIZE_SM),
                fg=PAPER_TEXT, bg=PAPER_SURFACE, anchor="w",
            ).pack(side=tk.LEFT, padx=(SPACE_SM, 0))

    def _on_settings_changed(self) -> None:
        """Persist the current Settings vars to config.yaml and apply
        the knobs that can be re-applied live (autostart registration,
        log-handler path + size). TCP port + interface checklist are
        saved but take effect on the next launch — changing them
        mid-session would need a peer restart."""
        from ..core import autostart as _autostart
        from ..core import user_config as _user_config
        data = {
            "autostart":    bool(self._settings_vars["autostart"].get()),
            "troubleshoot_enabled": bool(
                self._settings_vars["troubleshoot_enabled"].get()),
            "log_folder":   self._settings_vars["log_folder"].get(),
            "log_max_size": self._settings_vars["log_max_size"].get(),
            "tcp_port":     self._settings_vars["tcp_port"].get(),
            "disabled_interfaces": sorted(
                self._settings_disabled_ifaces),
        }
        _user_config.save_config(data)

        # Apply autostart side-effect (best-effort; failures log but
        # don't roll back the UI state — user can flip again).
        try:
            _autostart.set_autostart_enabled(data["autostart"])
        except Exception:
            log.exception("autostart toggle failed")

        # Live-reconfigure the log handler. When troubleshooting is
        # off, the handler is detached and unIO stops writing to
        # disk until the user turns it back on.
        self._apply_log_handler_config(
            data["log_folder"], data["log_max_size"],
            enabled=data["troubleshoot_enabled"])

    def _apply_log_handler_config(self, folder: str,
                                  max_size_label: str,
                                  *, enabled: bool = True) -> None:
        from logging.handlers import RotatingFileHandler
        root_logger = logging.getLogger()
        # Always drop any existing RotatingFileHandler first — we
        # either replace it (enabled=True) or remove it entirely
        # (enabled=False, troubleshoot toggled off).
        for h in list(root_logger.handlers):
            if isinstance(h, RotatingFileHandler):
                root_logger.removeHandler(h)
                try:
                    h.close()
                except Exception:
                    pass

        if not enabled:
            log.info("troubleshooting disabled — no log file handler")
            return

        size_map = {
            "100 KB": 100_000,
            "300 KB": 300_000,
            "1 MB":   1_000_000,
            "5 MB":   5_000_000,
        }
        max_bytes = size_map.get(max_size_label, 300_000)

        target_dir = pathlib.Path(folder).expanduser()
        try:
            target_dir.mkdir(parents=True, exist_ok=True)
        except Exception:
            log.exception("log folder %s couldn't be created", folder)
            return
        target_path = target_dir / "unio.log"

        new_handler = RotatingFileHandler(
            str(target_path), maxBytes=max_bytes, backupCount=1,
            encoding="utf-8", errors="replace",
        )
        new_handler.setFormatter(logging.Formatter(
            "%(asctime)s [%(levelname)s] %(name)s: %(message)s"))
        root_logger.addHandler(new_handler)
        log.info("log handler now writing to %s (max %d bytes)",
                 target_path, max_bytes)

    def _settings_pick_folder(self, var: tk.StringVar) -> None:
        # Native folder picker — no Toplevel created by us, the OS
        # owns the dialog so this doesn't violate the "everything in
        # one window" rule.
        try:
            from tkinter import filedialog
            path = filedialog.askdirectory(
                initialdir=var.get() or str(_user_log_dir()),
                title="Choose log folder",
            )
            if path:
                var.set(path)
        except tk.TclError:
            pass

    def _settings_package_logs(self) -> None:
        """Zip the log folder + open a file manager at the zip. The
        user attaches it manually to an email — logs never leave the
        device over the network."""
        import zipfile
        import tempfile
        import subprocess
        log_dir = pathlib.Path(self._settings_vars["log_folder"].get())
        if not log_dir.is_dir():
            return
        import time as _time
        stamp = _time.strftime("%Y%m%d-%H%M%S")
        out = log_dir / f"unio-logs-{stamp}.zip"
        try:
            with zipfile.ZipFile(out, "w",
                                 zipfile.ZIP_DEFLATED) as zf:
                for p in log_dir.glob("unio.log*"):
                    zf.write(p, p.name)
            if sys.platform == "win32":
                subprocess.Popen(["explorer", "/select,", str(out)])
            elif sys.platform == "darwin":
                subprocess.Popen(["open", "-R", str(out)])
            else:
                subprocess.Popen(["xdg-open", str(log_dir)])
        except Exception:
            log.exception("packaging logs failed")

    def _settings_heading(self, parent: tk.Widget,
                          title: str, sub: str) -> None:
        tk.Label(parent, text=title,
                 font=(FONT_SANS, SIZE_LG, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_BG, anchor="w"
                 ).pack(fill=tk.X)
        tk.Label(parent, text=sub,
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_BG, anchor="w"
                 ).pack(fill=tk.X, pady=(0, SPACE_SM))

    def _kv_row(self, parent: tk.Widget, key: str, value: str) -> None:
        row = tk.Frame(parent, bg=PAPER_SURFACE)
        row.pack(fill=tk.X, pady=2)
        tk.Label(row, text=key, width=14, anchor="w",
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_SURFACE
                 ).pack(side=tk.LEFT)
        tk.Label(row, text=value, anchor="w",
                 font=(FONT_SANS, SIZE_BASE),
                 fg=PAPER_TEXT, bg=PAPER_SURFACE
                 ).pack(side=tk.LEFT, fill=tk.X, expand=True)

    def _build_logs_placeholder(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._centered_text(
            frame,
            title="Logs (developer)",
            body="Open the log viewer.",
            button=("Open log viewer",
                    lambda: show_log_window(self.root)),
        )
        return frame

    def _centered_text(self, parent: tk.Widget, *, title: str, body: str,
                       button: Optional[tuple[str, Callable[[], None]]] = None
                       ) -> None:
        center = tk.Frame(parent, bg=PAPER_BG)
        center.place(relx=0.5, rely=0.5, anchor="center")
        tk.Label(center, text=title,
                 font=(FONT_SANS, SIZE_TITLE, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_BG).pack(pady=(0, SPACE_SM))
        tk.Label(center, text=body, wraplength=420, justify="center",
                 font=(FONT_SANS, SIZE_BASE),
                 fg=PAPER_MUTED, bg=PAPER_BG).pack(pady=(0, SPACE_LG))
        if button:
            PillButton(center, button[0], command=button[1],
                       variant="secondary").pack()

    # ── Mesh actions ─────────────────────────────────────────────

    def _apply_layout(self, layout: list[dict]) -> None:
        if self._peer is None:
            return
        self._peer.apply_layout(layout)
        if self.layout_panel is not None:
            self.layout_panel.mark_clean()

    def _request_identify(self) -> None:
        if self._peer is not None:
            self._peer.trigger_identify()

    # ── Identify overlay sink (in-process) ───────────────────────

    def _identify_sink(self, overlays: list, duration: int) -> None:
        """Render each identify overlay as a Toplevel off the shell's
        root. Called from the Client's asyncio thread — hops back to
        the Tk thread via root.after(0, ...) before touching any
        widgets. Avoids multiprocessing spawn (fast on both Linux and
        Windows; the old fork+multiprocessing path paid a fresh Tk/Tcl
        init cost per display that was visible on both OSes).

        `overlays` items carry: x, y, width, height, number, label.
        `duration` is seconds before auto-dismiss.
        """
        import time
        log.info("identify_sink: %d overlay(s), dur=%ss (queued at %.3f)",
                 len(overlays), duration, time.monotonic())
        self.root.after(0, self._render_identify, list(overlays), duration)

    def _render_identify(self, overlays: list, duration: int) -> None:
        import time
        t0 = time.monotonic()
        windows: list[tk.Toplevel] = []
        for d in overlays:
            try:
                top = tk.Toplevel(self.root)
            except tk.TclError:
                continue
            # overrideredirect MUST come before geometry(), otherwise
            # the WM intervenes with automatic placement — we saw
            # three overlays piling up on a single monitor on GNOME
            # because Mutter was treating the decoration-less windows
            # as unpositioned dialogs. With overrideredirect(True) Tk
            # honours the absolute coords we pass, including negative
            # X/Y for monitors to the left of the primary.
            top.overrideredirect(True)
            top.geometry(
                f"{int(d['width'])}x{int(d['height'])}"
                f"+{int(d['x'])}+{int(d['y'])}"
            )
            top.configure(bg="#111111")
            try:
                top.attributes("-topmost", True)
            except tk.TclError:
                pass
            try:
                top.attributes("-alpha", 0.85)
            except tk.TclError:
                pass

            frame = tk.Frame(top, bg="#111111")
            frame.place(relx=0.5, rely=0.5, anchor="center")
            num_size = max(48, min(300, int(d["height"]) // 3))
            tk.Label(frame, text=str(d.get("number", 0)),
                     font=("Helvetica", num_size, "bold"),
                     fg="#FFFFFF", bg="#111111").pack()
            sub_size = max(16, min(60, int(d["height"]) // 15))
            tk.Label(frame, text=str(d.get("label", "")),
                     font=("Helvetica", sub_size),
                     fg="#888888", bg="#111111").pack(pady=(10, 0))

            def _dismiss(_e=None, w=top):
                try:
                    w.destroy()
                except tk.TclError:
                    pass
            top.bind("<Key>", _dismiss)
            top.bind("<Button>", _dismiss)
            windows.append(top)

        # Force a single flush after all overlays are built so Tk
        # doesn't coalesce the window mappings with the next idle tick
        # — on Linux that shaved off the perceived "hang" before the
        # numbers appeared.
        try:
            self.root.update_idletasks()
        except tk.TclError:
            pass

        log.info("identify rendered %d window(s) in %.1f ms",
                 len(windows), (time.monotonic() - t0) * 1000)

        def _close_all():
            for w in windows:
                try:
                    w.destroy()
                except tk.TclError:
                    pass
        self.root.after(max(500, int(duration * 1000)), _close_all)

    # ── Lifecycle ────────────────────────────────────────────────

    def _ensure_runner(self) -> None:
        if not self._runner_started:
            self._runner.start()
            self._runner_started = True

    def _start_mesh_discovery(self) -> None:
        """Start MeshDiscovery on launch. UDP announce/listen runs
        for the entire app lifetime so we can always hear whether a
        signed-in peer is on the LAN. The actual Peer (TCP listener +
        mesh links) only spins up once we consider the mesh
        authorised — see _sync_peer_lifecycle."""
        if self._mesh is not None:
            return
        self._ensure_runner()

        mesh = MeshDiscovery(
            machine_id=self._machine_id,
            hostname=socket.gethostname() or self._machine_id,
            tcp_port=DEFAULT_PORT,
            on_peer_changed=lambda: self.root.after(0, self._on_mesh_changed),
        )
        mesh.set_authed(self._local_login)
        self._mesh = mesh
        # Stamp when discovery came up so the Activity tab's three-state
        # status can distinguish "we just launched, still listening" from
        # "nobody's home, prompt sign-in".
        import time as _time
        self._discovery_started_ts = _time.monotonic()

        async def _start_mesh():
            try:
                await mesh.start()
                log.info("Mesh discovery started")
            except OSError as e:
                log.warning("Mesh discovery failed to start: %s", e)
                self._mesh = None
        self._runner.submit(_start_mesh())

        # The Activity tab paints during _build(), BEFORE this method
        # runs (we're on a 100 ms after()). That first paint had no
        # timestamp yet, so _schedule_grace_rerender was a no-op and
        # the "Looking for an activated unIO…" status would stay stuck
        # forever. Force a fresh render now that the timestamp is set;
        # the rebuild re-enters _activity_alone_state which correctly
        # schedules the grace-window fall-through.
        if self._activity_frame is not None:
            self._rebuild_activity()

    # Two announce intervals is long enough for a signed-in peer on
    # the LAN to have registered with us, short enough that a truly
    # alone PC isn't left staring at "Looking for…" for ages.
    _DISCOVERY_GRACE_SECONDS = 4.0

    def _discovery_grace_elapsed(self) -> bool:
        start = getattr(self, "_discovery_started_ts", None)
        if start is None:
            return False
        import time as _time
        return (_time.monotonic() - start) >= self._DISCOVERY_GRACE_SECONDS

    def _schedule_grace_rerender(self) -> None:
        """One-shot: when the discovery grace window ends, re-render the
        Activity tab so the 'Looking for…' status falls through to the
        Sign-in prompt if no authed peer has surfaced."""
        if getattr(self, "_grace_rerender_scheduled", False):
            return
        start = getattr(self, "_discovery_started_ts", None)
        if start is None:
            return
        import time as _time
        remaining = self._DISCOVERY_GRACE_SECONDS - (
            _time.monotonic() - start)
        delay_ms = max(100, int(remaining * 1000) + 50)
        self._grace_rerender_scheduled = True

        def _fire():
            self._grace_rerender_scheduled = False
            if self._activity_frame is not None:
                self._rebuild_activity()
        self.root.after(delay_ms, _fire)

        # Evaluate auth after start so peer comes up if the user
        # signs in before the first announce round-trips.
        self.root.after(500, self._sync_peer_lifecycle)

    # ── Auth-gated peer lifecycle ────────────────────────────────

    def _mesh_is_authorised(self) -> bool:
        """Mesh is active when anyone on the LAN is signed in — us or
        a peer we heard an authed announce from."""
        if self._local_login:
            return True
        return bool(self._mesh and self._mesh.any_peer_authed())

    def _sync_peer_lifecycle(self) -> None:
        """Make the Peer's running state match the current auth:
        start it when authorised, stop it when the mesh loses auth.
        Safe to call at any point; it's a no-op when already in sync."""
        authorised = self._mesh_is_authorised()
        prev = getattr(self, "_prev_lifecycle_state", None)
        local = self._local_login
        if authorised and self._peer is None:
            self._start_peer()
        elif not authorised and self._peer is not None:
            self._stop_peer()
        self._rebuild_account()
        # The Activity tab's "alone" view now reads _local_login for
        # its status text. Rebuild when either auth bit (local login
        # or mesh auth) flips — _on_peer_state_changed's old_ids/new_ids
        # guard skips this on a still-alone PC.
        if (prev != (authorised, local)
                and self._activity_frame is not None):
            self._rebuild_activity()
        self._prev_lifecycle_state = (authorised, local)

    def _start_peer(self) -> None:
        if self._peer is not None:
            return
        self._ensure_runner()
        peer = Peer(machine_id=self._machine_id, tcp_port=DEFAULT_PORT)
        peer.identify_sink = self._identify_sink
        peer.on_state_changed = lambda: self.root.after(
            0, self._on_peer_state_changed)
        self._peer = peer

        async def _start():
            try:
                await peer.start()
                self._peer_task = asyncio.create_task(peer.serve_forever())
                log.info("Peer %s started (auth active)", self._machine_id)
            except OSError as e:
                log.warning("Peer failed to start: %s", e)
                self._peer = None
        self._runner.submit(_start())
        self.root.after(300, self._on_peer_state_changed)
        # Seed the peer with the active workspace's members so input
        # / clipboard sharing is scoped to the workspace from the
        # very first tick, not only after the user next toggles.
        self.root.after(400, self._sync_allowed_peers_to_peer)

    def _stop_peer(self) -> None:
        peer = self._peer
        if peer is None:
            return
        self._peer = None
        self._peer_task = None
        self._machines_info = {}
        self._active_machine = ""
        self._last_monitors = []
        self._last_state_sig = None
        self._teardown_all_streams()
        # Tear down any live evdi / IDD phantoms — a later sign-in
        # will recreate them from the workspace state.
        try:
            self._virtual_displays.close_all()
        except Exception:
            log.exception("virtual_displays close_all failed")
        if self.layout_panel is not None:
            self.layout_panel.set_displays([])
        if self._activity_frame is not None:
            self._rebuild_activity()

        async def _stop():
            try:
                await peer.stop()
                log.info("Peer stopped (auth gone)")
            except Exception:
                log.exception("Peer.stop failed")
        self._runner.submit(_stop())

    # ── Login / logout ───────────────────────────────────────────

    def _login(self, username: str) -> None:
        self._local_login = True
        self._login_username = username
        if self._mesh is not None:
            self._mesh.set_authed(True)
        self._sync_peer_lifecycle()

    def _logout(self) -> None:
        self._local_login = False
        if self._mesh is not None:
            self._mesh.set_authed(False)
        self._sync_peer_lifecycle()

    def _on_mesh_changed(self) -> None:
        """UDP heard a new peer (or one went stale). Re-evaluate
        auth (a freshly-authed peer activates us; a departed
        authed peer deactivates us) and auto-dial any mesh peer
        we should initiate to (smaller machine_id wins)."""
        if self._mesh is None:
            return
        self._mesh_peers = dict(self._mesh.peers)
        log.info("mesh changed: %d peer(s), authed=%s",
                 len(self._mesh_peers),
                 [p.machine_id for p in self._mesh_peers.values() if p.authed])
        self._sync_peer_lifecycle()
        # _auto_dial_missing_peers handles the dial loop; no need to
        # replicate it here. Skip the trailing _on_peer_state_changed
        # too — the Peer's own on_state_changed fires whenever LWW
        # actually mutates, and running the expensive layout/Activity
        # refresh on every 2 s announce was making the UI flicker.
        self._auto_dial_missing_peers()

    def _on_peer_state_changed(self) -> None:
        """Peer's shared-state changed. Refresh the Layout canvas on
        every call. Activity gets a FULL rebuild only when the set of
        machines OR workspaces changes; mute/clipboard toggle flips
        just repaint the pills in place so the tile frames don't flash."""
        if self._peer is None:
            return
        new_machines_info = self._peer.machines_snapshot()
        new_active = self._peer.active_machine()
        new_monitors = self._peer.global_monitors()

        # Pull replicated workspace state from the LWW store first so
        # anything downstream (sig, rebuild, layout filter) sees the
        # latest cluster list.
        shape_changed, lock_only_changed = \
            self._refresh_workspaces_from_lww()
        workspaces_changed = shape_changed or lock_only_changed
        # Routes live in their own LWW key per workspace — refresh so
        # Phase 1 streaming pipelines below pick up the latest patch.
        self._refresh_routes_from_lww()
        self._refresh_virtual_from_lww()
        self._refresh_hubs_from_lww()
        self._sync_own_virtuals_to_stream_server()

        sig = (
            new_active,
            tuple(sorted(
                (m.get("machine_id"), m.get("monitor_id"),
                 m.get("global_x"), m.get("global_y"),
                 m.get("width"), m.get("height"))
                for m in new_monitors
            )),
            tuple(sorted(
                (mid,
                 bool(info.get("muted", False)),
                 bool(info.get("clipboard_sync", True)),
                 len(info.get("monitors") or []))
                for mid, info in new_machines_info.items()
            )),
            self._workspaces_signature(),
        )
        if sig == getattr(self, "_last_state_sig", None):
            self._auto_dial_missing_peers()
            return
        log.info("peer state changed: active=%s, %d monitor(s), %d machine(s), %d workspace(s)",
                 new_active, len(new_monitors),
                 len(new_machines_info), len(self._workspaces))
        self._last_state_sig = sig

        self._active_machine = new_active
        self._last_monitors = new_monitors
        if self.layout_panel is not None:
            self.layout_panel.set_displays(
                self._workspace_filtered_monitors(new_monitors))
            self.layout_panel.set_active_machine(new_active)

        old_ids = set(self._machines_info.keys())
        new_ids = set(new_machines_info.keys())
        self._machines_info = new_machines_info
        if (old_ids != new_ids or shape_changed) \
                and self._activity_frame is not None:
            # Structural change (peer joined/left, workspace
            # created/renamed/members moved) — a full rebuild is the
            # only way to pick up new widgets.
            self._rebuild_activity()
        elif lock_only_changed and self._activity_frame is not None:
            # Lock-only delta → patch affected cards in place. Falls
            # back to a full rebuild if any card ref is missing (can
            # happen for a workspace that just appeared in LWW but
            # hasn't been rendered yet).
            needs_full = False
            for ws_id in self._workspaces:
                if not self._update_workspace_card_lock(ws_id):
                    needs_full = True
                    break
            if needs_full:
                self._rebuild_activity()
        elif self._activity_frame is not None:
            self._refresh_tile_pills()
        if shape_changed:
            # Remote create/delete/rename/member-move → refresh the
            # Layout chip row and empty-state cover too, and re-sync
            # the allowed-peer set so input/clipboard routing picks
            # up the new membership immediately. Lock-only flips
            # don't affect any of these, so we skip this block when
            # only locks moved.
            self._render_workspace_chips()
            self._refresh_layout_empty_state()
            self._refresh_layout_display()
            self._sync_allowed_peers_to_peer()
        # Reconcile display-stream pairs every time state changes —
        # a new peer coming online, routes being edited, or our own
        # monitors reshaping can all affect which sinks need windows.
        try:
            self._sync_display_streams()
        except Exception:
            log.exception("sync display streams failed")
        # Source-side overlays mirror the same reconciliation: if one
        # of our own monitors is being projected to a peer we cover it
        # with a borderless "projected" card so the user can't drop
        # windows onto a panel they can no longer see.
        try:
            self._sync_source_overlays()
        except Exception:
            log.exception("sync source overlays failed")
        # Keep the Layout canvas's route-badge overlay in sync with
        # the same LWW state used by the streaming pipeline.
        self._refresh_layout_routes()
        self._refresh_layout_display()
        self._auto_dial_missing_peers()

    def _refresh_tile_pills(self) -> None:
        """Repaint each existing tile's Input + Clipboard pills from
        the latest _machines_info without touching the tile frames."""
        for mid, pills in self._tile_pills.items():
            info = self._machines_info.get(mid)
            if info is None:
                continue
            self._paint_state_pill(
                pills["input"], label="Input",
                on=not bool(info.get("muted", False)),
            )
            self._paint_state_pill(
                pills["clipboard"], label="Clipboard",
                on=bool(info.get("clipboard_sync", True)),
            )

    def _auto_dial_missing_peers(self) -> None:
        if self._peer is None or self._mesh is None:
            return
        for mid, info in self._mesh.peers.items():
            if mid == self._machine_id or mid in self._peer.links:
                continue
            # Deterministic tiebreak: the peer with the smaller
            # machine_id dials out. The other side accepts inbound.
            if self._machine_id < mid:
                self._runner.submit(
                    self._peer.connect_to(info.ip, info.tcp_port))

    def _on_close(self) -> None:
        # Peer runs for the whole app lifetime; closing tears down the
        # mesh and releases the backend. A short "Leaving mesh…" modal
        # keeps the UI responsive until cleanup finishes.
        if not self._runner_started:
            self.root.destroy()
            return

        progress = self._show_closing_modal()

        def _worker():
            async def _close_all():
                tasks = []
                if self._peer is not None:
                    tasks.append(self._peer.stop())
                if self._mesh is not None:
                    tasks.append(self._mesh.stop())
                if tasks:
                    await asyncio.wait_for(
                        asyncio.gather(*tasks, return_exceptions=True),
                        timeout=3.0,
                    )

            try:
                self._runner.submit(_close_all()).result(timeout=4)
            except Exception:
                pass
            self._runner.stop()

            def _finish():
                try:
                    progress.destroy()
                except Exception:
                    pass
                self.root.destroy()

            self.root.after(0, _finish)

        threading.Thread(target=_worker, daemon=True,
                         name="unio-close").start()

    def _show_closing_modal(self) -> tk.Toplevel:
        dlg = tk.Toplevel(self.root)
        dlg.title("Closing unIO")
        dlg.configure(bg=PAPER_BG)
        dlg.transient(self.root)
        dlg.resizable(False, False)
        try:
            dlg.grab_set()
        except tk.TclError:
            pass
        set_window_icon(dlg)

        body = tk.Frame(dlg, bg=PAPER_BG, padx=SPACE_XL, pady=SPACE_LG)
        body.pack(fill=tk.BOTH, expand=True)
        tk.Label(
            body, text="Leaving mesh…",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(anchor="w")
        tk.Label(
            body,
            text="unIO will close as soon as connections close.",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(anchor="w", pady=(SPACE_SM, 0))

        dlg.update_idletasks()
        rw = self.root.winfo_rootx() + self.root.winfo_width() // 2
        rh = self.root.winfo_rooty() + self.root.winfo_height() // 2
        w, h = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        dlg.geometry(f"{w}x{h}+{rw - w // 2}+{rh - h // 2}")
        dlg.protocol("WM_DELETE_WINDOW", lambda: None)
        return dlg

    # ── Run ──────────────────────────────────────────────────────

    def run(self) -> None:
        self.root.mainloop()


def _restore_system_cursors_on_startup() -> None:
    """On Windows, ensure the OS cursor scheme is in its default state
    before Tk creates any widgets.

    The hide_cursor path in the Windows backend uses SetSystemCursor,
    which replaces every entry in the user's cursor scheme with a
    transparent image. We call SPI_SETCURSORS on show_cursor + atexit
    to undo it, but any abrupt exit (SIGKILL, taskkill, power loss)
    skips that path and leaves the system cursors blank. When the
    next unIO run starts, Tk caches cursor handles at widget-creation
    time — if the system is still in the blank state, Tk's handles
    are blank, and even a later SPI_SETCURSORS doesn't refresh them
    because the class-cursor cache is window-class-scoped. The user
    sees "cursor invisible over unIO" while every other app works.

    Calling SPI_SETCURSORS here, before any tk.Tk() is constructed,
    makes sure Tk loads real cursor handles.
    """
    if sys.platform != "win32":
        return
    try:
        import ctypes
        SPI_SETCURSORS = 0x0057
        ctypes.windll.user32.SystemParametersInfoW(
            SPI_SETCURSORS, 0, None, 0)
    except Exception:
        pass


def main() -> None:
    _restore_system_cursors_on_startup()
    # Honour the persisted Troubleshoot toggle and log-folder choice
    # before touching the root logger. When troubleshooting is off,
    # no file handler is installed at all — unIO writes nothing to
    # disk for the whole session unless the user re-enables it in
    # Settings mid-run (which live-adds the handler via
    # _apply_log_handler_config).
    from ..core import user_config as _user_config
    import pathlib
    from logging.handlers import RotatingFileHandler
    saved = _user_config.load_config()
    troubleshoot_on = bool(saved.get("troubleshoot_enabled", True))
    size_map = {
        "100 KB": 100_000,
        "300 KB": 300_000,
        "1 MB":   1_000_000,
        "5 MB":   5_000_000,
    }
    max_bytes = size_map.get(saved.get("log_max_size", "300 KB"),
                             300_000)
    log_folder = saved.get("log_folder") or str(_user_log_dir())
    level = logging.INFO

    handlers = []
    if troubleshoot_on:
        log_dir = pathlib.Path(log_folder).expanduser()
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / "unio.log"
        handlers.append(RotatingFileHandler(
            str(log_path), maxBytes=max_bytes, backupCount=1,
            encoding="utf-8", errors="replace",
        ))
    if unio.DEV_LOGS:
        handlers.append(logging.StreamHandler())
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        handlers=handlers,
        force=True,
    )
    if troubleshoot_on:
        log.info("unIO log file: %s/unio.log", log_folder)
    else:
        log.info("Troubleshooting disabled — no log file handler")
    if unio.DEV_LOGS:
        install_log_buffer()
    MainWindow().run()


def _user_log_dir() -> "pathlib.Path":
    import pathlib
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return pathlib.Path(base) / "unio"
    if sys.platform == "darwin":
        return pathlib.Path(
            os.path.expanduser("~/Library/Logs/unio"))
    # Linux / *nix — XDG cache is the conventional home for logs that
    # the user may clear without consequence.
    base = os.environ.get("XDG_CACHE_HOME") or \
        os.path.expanduser("~/.cache")
    return pathlib.Path(base) / "unio"


if __name__ == "__main__":
    main()

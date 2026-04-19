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
from .layout_panel import LayoutPanel, machine_color
from .log_view import install_log_buffer, show_log_window
from .peer import Peer
from .ui_theme import (
    FONT_SANS, LILAC, LILAC_SOFT, MINT, PAPER_BG, PAPER_BORDER,
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

        # Two-level nav: the outer mini rail picks a section
        # ("main" / "account" / "help"); inside "main" the inner
        # rail's _active_tab picks the visible tab.
        self._active_section = tk.StringVar(value="main")
        self._active_tab = tk.StringVar(value="activity")
        self._tab_frames: dict[str, tk.Widget] = {}
        self.layout_panel: Optional[LayoutPanel] = None
        self._activity_frame: Optional[tk.Frame] = None
        # Placeholders populated by _build_rail / _build_mini_rail.
        self._rail_frame: Optional[tk.Frame] = None
        self._rail_hairline: Optional[tk.Frame] = None

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

        self._tabs: list[Tab] = [
            Tab("activity", "Activity", "",  self._build_activity_tab),
            Tab("layout",   "Layout",   "",  self._build_layout_tab),
            Tab("settings", "Settings", "",  self._build_settings_tab),
        ]
        if unio.DEV_LOGS:
            self._tabs.append(
                Tab("logs", "Logs", "≡", self._build_logs_placeholder),
            )
        # Footer "tabs" — rendered as icon-only buttons at the bottom
        # of the rail, but behave like any other tab: clicking swaps
        # the content area via the same _active_tab StringVar, no
        # popup windows.
        self._footer_tabs: list[Tab] = [
            Tab("account", "Account", "", self._build_account_tab),
            Tab("help",    "Help",    "", self._build_help_tab),
        ]

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

        # Outer identity rail — logo + section icons (Main / Account
        # / Help) with a darker bg. Always visible.
        mini = self._build_mini_rail(outer)
        mini.pack(side=tk.LEFT, fill=tk.Y)
        hairline(outer, axis="y").pack(side=tk.LEFT, fill=tk.Y)

        # Inner nav rail — only visible when section="main", hosts
        # Activity / Layout / Settings (+ Logs on dev builds). Packed
        # / unpacked in _on_section_change. Paired hairline kept in
        # sync so we don't end up with a stray vertical line when the
        # rail is hidden.
        self._rail_frame = self._build_rail(outer)
        self._rail_hairline = hairline(outer, axis="y")

        self._content = tk.Frame(outer, bg=PAPER_BG)
        self._content.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # Subscribe: section changes show/hide the inner rail and
        # swap content; tab changes within "main" re-render the
        # chosen tab.
        self._active_section.trace_add(
            "write", lambda *_: self._on_section_change())
        self._active_tab.trace_add(
            "write", lambda *_: self._on_tab_change())

        # Initial paint.
        self._on_section_change()

    def _build_rail(self, parent: tk.Widget) -> tk.Frame:
        """Main nav rail — just the tab buttons now. Logo and footer
        icons live on the outer mini rail."""
        rail = tk.Frame(parent, bg=PAPER_RAIL, width=RAIL_WIDTH)
        rail.pack_propagate(False)

        # Tab icons as PNGs so rendering is identical on every OS.
        from pathlib import Path
        assets = Path(__file__).resolve().parents[2] / "assets"
        self._tab_icons: dict[str, Optional[tk.PhotoImage]] = {
            "activity": self._load_image(assets / "icon_tab_activity_28.png"),
            "layout":   self._load_image(assets / "icon_tab_layout_28.png"),
            "settings": self._load_image(assets / "icon_tab_settings_28.png"),
        }

        # No top padding here — the rail's very first pixel should
        # belong to the Activity button so its active-state tint
        # extends flush to the top edge. RailButton's internal
        # glyph/label padding already gives the button its height.
        nav = tk.Frame(rail, bg=PAPER_RAIL)
        nav.pack(fill=tk.X)
        for tab in self._tabs:
            icon = self._tab_icons.get(tab.key)
            RailButton(
                nav, label=tab.label,
                glyph=tab.glyph, image=icon,
                value=tab.key, var=self._active_tab,
            ).pack(fill=tk.X, pady=0)

        return rail

    def _build_mini_rail(self, parent: tk.Widget) -> tk.Frame:
        """Outer identity rail on the far left.

        Logo up top, then a Main section button under it, Account
        and Help pinned to the bottom. Selected section's button
        tints to PAPER_RAIL — the same colour as the inner nav rail
        — so the selected icon reads as "attached" to whatever's
        visible to the right of it.
        """
        mini = tk.Frame(parent, bg=PAPER_RAIL_DEEP, width=MINI_RAIL_WIDTH)
        mini.pack_propagate(False)

        # Logo up top.
        from pathlib import Path
        assets = Path(__file__).resolve().parents[2] / "assets"
        candidates = [
            assets / "logo_mark_48.png",
            assets / "logo_48.png",
            assets / "logo.png",
        ]
        self._rail_logo_img = None
        for p in candidates:
            if not p.exists():
                continue
            try:
                self._rail_logo_img = tk.PhotoImage(file=str(p))
                break
            except tk.TclError:
                continue
        if self._rail_logo_img is not None:
            tk.Label(
                mini, image=self._rail_logo_img, bg=PAPER_RAIL_DEEP,
            ).pack(pady=(SPACE_LG, SPACE_SM))
        else:
            tk.Label(
                mini, text="●", bg=PAPER_RAIL_DEEP,
                fg=LILAC, font=(FONT_SANS, SIZE_XL, "bold"),
            ).pack(pady=(SPACE_LG, SPACE_SM))

        # Main section icon: represents the Activity / Layout /
        # Settings bundle. Clicking shows the inner rail.
        self._icon_main = self._load_image(assets / "icon_tab_main_28.png")
        self._section_button(
            mini, self._icon_main, "Workspace", "main",
        ).pack(fill=tk.X, side=tk.TOP)

        # Account / Help near the bottom of the rail — not flush
        # against the bottom edge. SPACE_LG below Help gives breathing
        # room; SPACE_SM between Account and Help keeps them visually
        # paired without blending into one block.
        bottom = tk.Frame(mini, bg=PAPER_RAIL_DEEP)
        bottom.pack(side=tk.BOTTOM, fill=tk.X, pady=(0, SPACE_LG))

        self._icon_account = self._load_image(assets / "icon_account_28.png")
        self._icon_help = self._load_image(assets / "icon_help_28.png")
        self._section_button(
            bottom, self._icon_account, "Account", "account",
        ).pack(fill=tk.X, side=tk.TOP, pady=(0, SPACE_SM))
        self._section_button(
            bottom, self._icon_help, "Help", "help",
        ).pack(fill=tk.X, side=tk.TOP)

        return mini

    def _load_image(self, path) -> Optional[tk.PhotoImage]:
        try:
            return tk.PhotoImage(file=str(path))
        except tk.TclError:
            return None

    def _section_button(self, parent: tk.Widget,
                        image: Optional[tk.PhotoImage],
                        tooltip: str,
                        section_key: str) -> tk.Frame:
        """Full-width icon button on the mini rail. Selected state
        tints to PAPER_RAIL (same as the inner nav rail) so the
        icon visually merges with what's to its right."""
        btn = tk.Frame(parent, bg=PAPER_RAIL_DEEP, cursor="hand2")
        if image is not None:
            lbl = tk.Label(btn, image=image, bg=PAPER_RAIL_DEEP,
                           pady=SPACE_MD)
        else:
            lbl = tk.Label(btn, text="?", bg=PAPER_RAIL_DEEP,
                           fg=PAPER_MUTED, font=(FONT_SANS, SIZE_XL),
                           pady=SPACE_MD)
        lbl.pack()

        def _refresh(*_):
            active = self._active_section.get() == section_key
            bg = PAPER_RAIL if active else PAPER_RAIL_DEEP
            btn.configure(bg=bg)
            lbl.configure(bg=bg)

        def _click(_e=None):
            self._active_section.set(section_key)

        for w in (btn, lbl):
            w.bind("<Button-1>", _click)
        self._active_section.trace_add("write", _refresh)
        _refresh()
        _attach_tooltip(lbl, tooltip)
        return btn

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

        center = tk.Frame(frame, bg=PAPER_BG)
        center.place(relx=0.5, rely=0.5, anchor="center")

        tk.Label(
            center, text="Account",
            font=(FONT_SANS, SIZE_TITLE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_SM))

        if self._local_login:
            self._build_account_signed_in(center)
        elif self._mesh and self._mesh.any_peer_authed():
            self._build_account_mesh_authed(center)
        else:
            self._build_account_sign_in(center)

        # Temporary-state banner so anyone reading the app knows
        # admin/admin is a stopgap until real auth lands.
        tk.Label(
            center,
            text="Test account only — admin / admin is hardcoded. "
                 "Real sign-in is coming later.",
            font=(FONT_SANS, SIZE_XS, "italic"),
            fg=PAPER_FAINT, bg=PAPER_BG,
            wraplength=420, justify="center",
        ).pack(pady=(SPACE_XL, 0))

    def _build_account_sign_in(self, parent: tk.Widget) -> None:
        tk.Label(
            parent,
            text="Sign in on any computer to activate the mesh.",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_LG))

        form = tk.Frame(parent, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_LG)
        form.pack()

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
        ).pack(pady=(0, SPACE_SM))
        tk.Label(
            parent,
            text="Every other computer on your LAN can join the mesh\n"
                 "automatically while you stay signed in here.",
            wraplength=440, justify="center",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_LG))
        PillButton(parent, "Sign out", variant="secondary",
                   command=self._logout).pack()

    def _build_account_mesh_authed(self, parent: tk.Widget) -> None:
        tk.Label(
            parent,
            text="Active via another computer on your network.",
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_SM))
        tk.Label(
            parent,
            text="This machine is joining the mesh because a signed-in\n"
                 "peer is broadcasting. You don't need to sign in here.",
            wraplength=440, justify="center",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack()

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
            tab = next(
                (t for t in self._tabs + self._footer_tabs if t.key == key),
                None,
            )
            if tab is None:
                return
            self._tab_frames[key] = tab.build(self._content)
        self._tab_frames[key].pack(fill=tk.BOTH, expand=True)

    def _on_section_change(self) -> None:
        """Switch between 'main' (show inner rail + active tab) and
        'account' / 'help' (hide inner rail, show that section's
        content directly)."""
        section = self._active_section.get()
        # Hairline and rail always move together so we don't leave a
        # stray vertical line when the rail is hidden.
        if self._rail_frame is not None and self._rail_hairline is not None:
            self._rail_frame.pack_forget()
            self._rail_hairline.pack_forget()
        if section == "main":
            if self._rail_frame is not None and self._rail_hairline is not None:
                # Re-pack the rail + hairline right after the mini
                # rail's hairline and before the content area.
                self._rail_frame.pack(side=tk.LEFT, fill=tk.Y,
                                      before=self._content)
                self._rail_hairline.pack(side=tk.LEFT, fill=tk.Y,
                                         before=self._content)
            self._show_tab(self._active_tab.get())
        else:
            self._show_tab(section)

    def _on_tab_change(self) -> None:
        if self._active_section.get() == "main":
            self._show_tab(self._active_tab.get())

    # ── Activity tab ─────────────────────────────────────────────

    def _build_activity_tab(self, parent: tk.Widget) -> tk.Widget:
        self._activity_frame = tk.Frame(parent, bg=PAPER_BG)
        self._rebuild_activity()
        return self._activity_frame

    def _rebuild_activity(self) -> None:
        frame = self._activity_frame
        if frame is None:
            return
        # Drop stale widget refs — tiles we're about to destroy can't
        # be repainted in place anymore, and their replacements will
        # register fresh pills in _machine_tile.
        self._tile_pills = {}
        for w in frame.winfo_children():
            w.destroy()

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

    def _activity_alone_state(self, parent: tk.Widget) -> None:
        center = tk.Frame(parent, bg=PAPER_BG)
        center.place(relx=0.5, rely=0.5, anchor="center")
        authorised = self._mesh_is_authorised()

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

        status = (
            "Searching for peers on your LAN…" if authorised
            else "Sign in (Account tab) on this or any other PC "
                 "to activate the mesh."
        )
        tk.Label(
            center, text=status,
            font=(FONT_SANS, SIZE_SM, "italic"),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack()

    def _activity_running(self, parent: tk.Widget) -> None:
        wrap = tk.Frame(parent, bg=PAPER_BG,
                        padx=SPACE_LG, pady=SPACE_LG)
        wrap.pack(fill=tk.BOTH, expand=True)

        self._activity_header(wrap)
        self._activity_machines_grid(wrap)

    def _activity_header(self, parent: tk.Widget) -> None:
        header = tk.Frame(parent, bg=PAPER_BG)
        header.pack(fill=tk.X, pady=(0, SPACE_LG))

        peer_count = (
            sum(1 for mid in self._machines_info
                if mid != self._machine_id)
        )
        title_text = (
            f"Mesh · {peer_count + 1} "
            f"computer{'s' if peer_count != 0 else ''}"
        )
        sub_text = f"{self._machine_id} · {_local_ip()}:{DEFAULT_PORT}"

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

    def _activity_machines_grid(self, parent: tk.Widget) -> None:
        wrap = tk.Frame(parent, bg=PAPER_BG)
        wrap.pack(fill=tk.BOTH, expand=True)

        tk.Label(
            wrap, text="Connected computers",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w", pady=(0, SPACE_SM))

        tiles = tk.Frame(wrap, bg=PAPER_BG)
        tiles.pack(fill=tk.BOTH, expand=True)

        real_machines = {
            mid: info
            for mid, info in self._machines_info.items()
            if mid and info
        }
        if not real_machines:
            tk.Label(
                tiles,
                text="Waiting for computers to connect…",
                font=(FONT_SANS, SIZE_BASE),
                fg=PAPER_MUTED, bg=PAPER_BG,
            ).pack(anchor="w", pady=SPACE_SM)
            return

        for mid, info in sorted(real_machines.items()):
            self._machine_tile(tiles, mid, info).pack(
                fill=tk.X, pady=(0, SPACE_SM))

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
        StatusDot(row, state="ok", bg=PAPER_SURFACE).pack(
            side=tk.LEFT, padx=(0, SPACE_SM), pady=(3, 0))
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
        the Activity tile frame."""
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
        self.layout_panel = LayoutPanel(
            parent,
            on_apply=self._apply_layout,
            on_identify=self._request_identify,
        )
        # Replay the latest snapshot AFTER this tab is packed +
        # mapped — otherwise LayoutPanel's canvas has winfo_width=1
        # and _fit_view's 50 ms retry loop sometimes never lands on a
        # real size (the canvas' <Configure> fires before the retry
        # re-queues). root.after_idle guarantees the widget tree has
        # settled before we push data.
        if self._last_monitors:
            monitors = list(self._last_monitors)
            active = self._active_machine
            panel = self.layout_panel

            def _apply():
                panel.set_displays(monitors)
                panel.set_active_machine(active)
            self.root.after_idle(_apply)
        return self.layout_panel

    def _build_settings_tab(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)

        scroll_wrap = tk.Frame(frame, bg=PAPER_BG,
                               padx=SPACE_XL, pady=SPACE_LG)
        scroll_wrap.pack(fill=tk.BOTH, expand=True)

        self._settings_heading(scroll_wrap, "About",
                               "What's running on this PC.")
        about = tk.Frame(scroll_wrap, bg=PAPER_SURFACE,
                         padx=SPACE_LG, pady=SPACE_LG)
        about.pack(fill=tk.X, pady=(0, SPACE_LG))
        self._kv_row(about, "Version", unio.__version__)
        self._kv_row(about, "Hostname", socket.gethostname() or "—")
        self._kv_row(about, "Machine ID", self._machine_id)
        self._kv_row(about, "Platform", _describe_platform())

        self._settings_heading(
            scroll_wrap, "Keyboard shortcuts",
            "Coming in a follow-up commit — these are the planned ones.",
        )
        shortcuts = tk.Frame(scroll_wrap, bg=PAPER_SURFACE,
                             padx=SPACE_LG, pady=SPACE_LG)
        shortcuts.pack(fill=tk.X, pady=(0, SPACE_LG))
        self._kv_row(shortcuts, "Identify displays",
                     "Ctrl+Shift+I")
        if unio.DEV_LOGS:
            self._kv_row(shortcuts, "Open log viewer",
                         "Ctrl+Shift+L")

        if unio.DEV_LOGS:
            self._settings_heading(
                scroll_wrap, "Developer",
                "Diagnostic tools — only visible in dev builds.",
            )
            dev = tk.Frame(scroll_wrap, bg=PAPER_SURFACE,
                           padx=SPACE_LG, pady=SPACE_LG)
            dev.pack(fill=tk.X, pady=(0, SPACE_LG))
            PillButton(dev, "Open log viewer",
                       command=lambda: show_log_window(self.root),
                       variant="secondary"
                       ).pack(anchor="w")

        return frame

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

        async def _start_mesh():
            try:
                await mesh.start()
                log.info("Mesh discovery started")
            except OSError as e:
                log.warning("Mesh discovery failed to start: %s", e)
                self._mesh = None
        self._runner.submit(_start_mesh())

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
        if authorised and self._peer is None:
            self._start_peer()
        elif not authorised and self._peer is not None:
            self._stop_peer()
        self._rebuild_account()

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

    def _stop_peer(self) -> None:
        peer = self._peer
        if peer is None:
            return
        self._peer = None
        self._peer_task = None
        self._machines_info = {}
        self._active_machine = ""
        self._last_monitors = []
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
        # Remote peer logged in/out? Might need to flip our own
        # mesh-active state.
        self._sync_peer_lifecycle()
        if self._peer is not None:
            for mid, info in self._mesh_peers.items():
                if mid in self._peer.links:
                    continue
                if self._machine_id < mid:
                    log.info("Dialing mesh peer %s at %s:%d",
                             mid, info.ip, info.tcp_port)
                    self._runner.submit(
                        self._peer.connect_to(info.ip, info.tcp_port))
        self._on_peer_state_changed()

    def _on_peer_state_changed(self) -> None:
        """Peer's shared-state changed. Refresh the Layout canvas on
        every call. Activity gets a FULL rebuild only when the set of
        machines changes (a peer joined or left); mute/clipboard
        toggle flips just repaint the pills in place so the tile
        frames don't flash."""
        if self._peer is None:
            return
        new_machines_info = self._peer.machines_snapshot()
        self._active_machine = self._peer.active_machine()
        self._last_monitors = self._peer.global_monitors()
        if self.layout_panel is not None:
            self.layout_panel.set_displays(self._last_monitors)
            self.layout_panel.set_active_machine(self._active_machine)

        old_ids = set(self._machines_info.keys())
        new_ids = set(new_machines_info.keys())
        self._machines_info = new_machines_info
        if old_ids != new_ids and self._activity_frame is not None:
            self._rebuild_activity()
        elif self._activity_frame is not None:
            self._refresh_tile_pills()
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


def main() -> None:
    if unio.DEV_LOGS:
        logging.basicConfig(
            level=logging.INFO,
            format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        )
        install_log_buffer()
    else:
        logging.basicConfig(level=logging.CRITICAL)
    MainWindow().run()


if __name__ == "__main__":
    main()

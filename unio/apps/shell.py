"""Main shell window for the UnIO UI.

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
import threading
import tkinter as tk
from dataclasses import dataclass
from tkinter import messagebox
from typing import Callable, Optional

import unio
from ..core.discovery import (
    DiscoveredHost, discover_hosts, local_identity,
)
from ..core.network import Connection
from ..core.protocol import (
    MsgType, RegisterMsg, LayoutApplyMsg, RequestIdentifyMsg,
    SetInputSourceMsg,
)
from .client import Client
from .layout_panel import LayoutPanel
from .log_view import install_log_buffer, show_log_window
from .server import Server
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
SHELL_MACHINE_ID = "__unio_shell__"


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


async def _run_local_client_safe(client: Client) -> None:
    try:
        await client.run()
    except asyncio.CancelledError:
        raise
    except (OSError, RuntimeError) as e:
        log.warning("Local client disabled (%s: %s)", type(e).__name__, e)
    finally:
        try:
            await client.stop()
        except OSError:
            pass


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
    """The single top-level window that hosts every UnIO UI surface."""

    def __init__(self) -> None:
        self.root = tk.Tk(className="UnIO")
        self.root.title("UnIO")
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

        # Session state.
        self._runner = _AsyncRunner()
        self._runner_started = False
        self._session: Optional[str] = None     # "host" | "join" | None
        self._server: Optional[Server] = None
        self._local_client: Optional[Client] = None
        self._config_conn: Optional[Connection] = None
        self._config_task = None
        self._session_host: Optional[str] = None
        self._session_port: int = DEFAULT_PORT
        self._session_server_hostname: str = ""
        self._machines_info: dict[str, dict] = {}
        self._active_machine: str = ""
        self._input_source: str = ""
        self._last_monitors: list[dict] = []
        self._stopping: bool = False
        self._machine_id = _default_machine_id()

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

    def _bind_shortcuts(self) -> None:
        # The shortcuts live on the root so they fire regardless of
        # which tab / widget has focus. All gate on there being an
        # active session — pressing them from the empty state is a
        # no-op rather than an error.
        self.root.bind_all("<Control-Shift-S>",
                           lambda _e: self._shortcut_cycle_source())
        self.root.bind_all("<Control-Shift-I>",
                           lambda _e: self._shortcut_identify())
        if unio.DEV_LOGS:
            self.root.bind_all("<Control-Shift-L>",
                               lambda _e: show_log_window(self.root))

    def _shortcut_cycle_source(self) -> None:
        if self._config_conn is None:
            return
        # Cycle through connected real machines.
        candidates = [
            mid for mid, info in sorted(self._machines_info.items())
            if info and mid != SHELL_MACHINE_ID
        ]
        if len(candidates) < 2:
            return
        try:
            idx = candidates.index(self._input_source)
        except ValueError:
            idx = -1
        nxt = candidates[(idx + 1) % len(candidates)]
        self._set_input_source(nxt)

    def _shortcut_identify(self) -> None:
        if self._config_conn is not None:
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
        self._centered_text(
            frame,
            title="Account",
            body="Account settings and sign-in will live here.\n"
                 "Coming soon.",
        )
        return frame

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
        for w in frame.winfo_children():
            w.destroy()

        if self._session is None:
            self._activity_empty_state(frame)
        else:
            self._activity_running(frame)

    def _activity_empty_state(self, parent: tk.Widget) -> None:
        center = tk.Frame(parent, bg=PAPER_BG)
        center.place(relx=0.5, rely=0.5, anchor="center")

        tk.Label(
            center, text="Welcome to UnIO",
            font=(FONT_SANS, SIZE_TITLE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_SM))

        tk.Label(
            center,
            text="One keyboard and mouse across every PC on your network.\n"
                 "Pick how you'd like to start.",
            wraplength=460, justify="center",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_XL))

        card_row = tk.Frame(center, bg=PAPER_BG)
        card_row.pack()

        self._action_card(
            card_row,
            title="Host on this PC",
            body="Start a session others can join. Your displays show up "
                 "first in the layout.",
            button="Start hosting",
            variant="primary",
            command=self._do_host,
        ).pack(side=tk.LEFT, padx=SPACE_SM)

        self._action_card(
            card_row,
            title="Join another host",
            body="Connect to a PC already running UnIO. We'll scan your "
                 "LAN for one.",
            button="Find hosts",
            variant="secondary",
            command=self._do_join,
        ).pack(side=tk.LEFT, padx=SPACE_SM)

    def _action_card(self, parent: tk.Widget, *, title: str, body: str,
                     button: str, variant: str,
                     command: Callable[[], None]) -> tk.Widget:
        card = tk.Frame(parent, bg=PAPER_SURFACE,
                        width=280, height=180)
        card.pack_propagate(False)
        inner = tk.Frame(card, bg=PAPER_SURFACE,
                         padx=SPACE_LG, pady=SPACE_LG)
        inner.pack(fill=tk.BOTH, expand=True)
        tk.Label(inner, text=title,
                 font=(FONT_SANS, SIZE_LG, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_SURFACE, anchor="w"
                 ).pack(fill=tk.X)
        tk.Label(inner, text=body, wraplength=240, justify="left",
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_SURFACE, anchor="w"
                 ).pack(fill=tk.X, pady=(SPACE_SM, SPACE_LG))
        PillButton(inner, button, command=command, variant=variant
                   ).pack(anchor="w")
        return card

    def _activity_running(self, parent: tk.Widget) -> None:
        wrap = tk.Frame(parent, bg=PAPER_BG,
                        padx=SPACE_LG, pady=SPACE_LG)
        wrap.pack(fill=tk.BOTH, expand=True)

        self._activity_header(wrap)
        self._activity_source_strip(wrap)
        self._activity_machines_grid(wrap)

    def _activity_header(self, parent: tk.Widget) -> None:
        header = tk.Frame(parent, bg=PAPER_BG)
        header.pack(fill=tk.X, pady=(0, SPACE_LG))

        left = tk.Frame(header, bg=PAPER_BG)
        left.pack(side=tk.LEFT, anchor="w")

        if self._session == "host":
            title_text = f"Hosting as {self._machine_id}"
            sub_text = (
                f"{_local_ip()}:{self._session_port} · "
                "share this address with other machines"
            )
        else:
            server = (self._session_server_hostname
                      or self._session_host or "")
            title_text = f"Connected to {server}"
            sub_text = f"Server at {self._session_host}:{self._session_port}"

        tk.Label(
            left, text=title_text,
            font=(FONT_SANS, SIZE_TITLE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w")
        tk.Label(
            left, text=sub_text,
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w", pady=(2, 0))

        PillButton(
            header,
            "Stop session" if self._session == "host" else "Disconnect",
            command=self._do_stop, variant="danger",
        ).pack(side=tk.RIGHT, anchor="e")

    def _activity_source_strip(self, parent: tk.Widget) -> None:
        strip = tk.Frame(parent, bg=PAPER_SURFACE)
        strip.pack(fill=tk.X, pady=(0, SPACE_LG))
        inner = tk.Frame(strip, bg=PAPER_SURFACE,
                         padx=SPACE_LG, pady=SPACE_MD)
        inner.pack(fill=tk.X)

        tk.Label(
            inner, text="INPUT SOURCE",
            font=(FONT_SANS, SIZE_XS, "bold"),
            fg=PAPER_MUTED, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)

        StatusDot(inner, state="ok" if self._input_source else "idle",
                  bg=PAPER_SURFACE).pack(
            side=tk.LEFT, padx=(SPACE_MD, SPACE_XS))

        tk.Label(
            inner, text=self._input_source or "(none yet)",
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)

        tk.Label(
            inner,
            text="  — the PC whose keyboard and mouse drive everything",
            font=(FONT_SANS, SIZE_SM),
            fg=PAPER_MUTED, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)

    def _activity_machines_grid(self, parent: tk.Widget) -> None:
        wrap = tk.Frame(parent, bg=PAPER_BG)
        wrap.pack(fill=tk.BOTH, expand=True)

        tk.Label(
            wrap, text="Connected machines",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG, anchor="w",
        ).pack(anchor="w", pady=(0, SPACE_SM))

        tiles = tk.Frame(wrap, bg=PAPER_BG)
        tiles.pack(fill=tk.BOTH, expand=True)

        real_machines = {
            mid: info
            for mid, info in self._machines_info.items()
            if mid and info and mid != SHELL_MACHINE_ID
        }
        if not real_machines:
            tk.Label(
                tiles,
                text="Waiting for machines to connect…",
                font=(FONT_SANS, SIZE_BASE),
                fg=PAPER_MUTED, bg=PAPER_BG,
            ).pack(anchor="w", pady=SPACE_SM)
            return

        for mid, info in sorted(real_machines.items()):
            self._machine_tile(tiles, mid, info).pack(
                fill=tk.X, pady=(0, SPACE_SM))

    def _machine_tile(self, parent: tk.Widget,
                      machine_id: str, info: dict) -> tk.Widget:
        is_source = machine_id == self._input_source
        is_active = machine_id == self._active_machine
        accent = LILAC if is_source else (MINT if is_active else PAPER_BORDER)

        card = tk.Frame(parent, bg=PAPER_SURFACE)
        strip = tk.Frame(card, bg=accent, width=4)
        strip.pack(side=tk.LEFT, fill=tk.Y)
        body = tk.Frame(card, bg=PAPER_SURFACE,
                        padx=SPACE_LG, pady=SPACE_MD)
        body.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # Row 1: hostname + badges.
        row = tk.Frame(body, bg=PAPER_SURFACE)
        row.pack(fill=tk.X)

        StatusDot(row, state="ok", bg=PAPER_SURFACE).pack(
            side=tk.LEFT, padx=(0, SPACE_SM), pady=(3, 0))

        tk.Label(
            row, text=machine_id,
            font=(FONT_SANS, SIZE_BASE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_SURFACE,
        ).pack(side=tk.LEFT)

        if is_source:
            self._badge(row, "Source", LILAC).pack(
                side=tk.LEFT, padx=(SPACE_SM, 0))
        if is_active:
            self._badge(row, "Cursor here", MINT).pack(
                side=tk.LEFT, padx=(SPACE_SM, 0))

        # Right side: "Make source" action when not already source.
        if not is_source:
            PillButton(
                row, "Make source",
                command=lambda mid=machine_id: self._set_input_source(mid),
                variant="ghost",
                size=SIZE_XS,
            ).pack(side=tk.RIGHT)

        # Row 2: OS / platform info.
        os_label = info.get("platform_info") or info.get("os") or ""
        if os_label:
            tk.Label(
                body, text=os_label,
                font=(FONT_SANS, SIZE_SM),
                fg=PAPER_MUTED, bg=PAPER_SURFACE, anchor="w",
            ).pack(fill=tk.X, pady=(2, 0))

        return card

    def _badge(self, parent: tk.Widget, text: str, color: str) -> tk.Widget:
        return tk.Label(
            parent, text=f" {text} ",
            font=(FONT_SANS, SIZE_XS, "bold"),
            fg="white", bg=color, padx=SPACE_XS, pady=1,
        )

    # ── Layout + Settings + Logs (placeholders for this commit) ──

    def _build_layout_tab(self, parent: tk.Widget) -> tk.Widget:
        self.layout_panel = LayoutPanel(
            parent,
            on_apply=self._apply_layout,
            on_identify=self._request_identify,
        )
        # A layout might already have arrived before the user opened
        # this tab — replay the latest snapshot so it doesn't look empty.
        if self._last_monitors:
            self.layout_panel.set_displays(self._last_monitors)
            self.layout_panel.set_active_machine(self._active_machine)
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
        self._kv_row(shortcuts, "Cycle input source",
                     "Ctrl+Shift+S")
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

    # ── Session actions ──────────────────────────────────────────

    def _do_host(self) -> None:
        self._ensure_runner()
        self._session = "host"
        self._session_host = "127.0.0.1"
        self._session_port = DEFAULT_PORT

        server = Server(host="0.0.0.0", port=DEFAULT_PORT)
        self._server = server
        ready = threading.Event()
        err: dict = {}

        async def _startup():
            try:
                await server.start()
            except OSError as e:
                err["e"] = e
            finally:
                ready.set()
            if "e" in err:
                return
            await server.serve_forever()

        self._runner.submit(_startup())
        if not ready.wait(timeout=8):
            messagebox.showerror("Startup failed",
                                 "Server did not come up in time.",
                                 parent=self.root)
            self._server = None
            self._session = None
            return
        if "e" in err:
            messagebox.showerror(
                "Port in use",
                f"Couldn't bind port {DEFAULT_PORT}:\n{err['e']}\n\n"
                "Another UnIO instance may already be running.",
                parent=self.root,
            )
            self._server = None
            self._session = None
            return

        self._start_local_client(self._session_host, self._session_port)
        self._connect_configurator(self._session_host, self._session_port)
        self._rebuild_activity()

    def _do_join(self) -> None:
        self._show_discover_dialog()

    def _join_to(self, host: str, port: int) -> None:
        self._ensure_runner()
        self._session = "join"
        self._session_host = host
        self._session_port = port
        self._start_local_client(host, port)
        self._connect_configurator(host, port)
        self._rebuild_activity()

    def _do_stop(self) -> None:
        # Don't block the UI waiting for the server / client / config
        # conn to drain. Flip state immediately so the user gets
        # feedback, then tear down on a background thread.
        if self._stopping or self._session is None:
            return
        self._stopping = True
        # Hide the running view right now so the user doesn't stare
        # at stale machine tiles while teardown happens.
        self._session = None
        self._machines_info = {}
        self._active_machine = ""
        self._input_source = ""
        self._last_monitors = []
        if self.layout_panel is not None:
            self.layout_panel.set_displays([])
        self._rebuild_activity()
        threading.Thread(
            target=self._teardown_session_worker,
            daemon=True, name="unio-teardown",
        ).start()

    def _teardown_session_worker(self) -> None:
        """Runs off the UI thread so stop buttons feel instant."""
        # Parallel close — each drain has its own short timeout; no
        # one peer gone AWOL can hold the whole teardown for 9s.
        async def _close_all():
            tasks = []
            if self._config_conn is not None and not self._config_conn.closed:
                tasks.append(self._config_conn.close())
            if self._local_client is not None:
                tasks.append(self._local_client.stop())
            if self._server is not None:
                tasks.append(self._server.stop())
            if tasks:
                await asyncio.wait_for(
                    asyncio.gather(*tasks, return_exceptions=True),
                    timeout=2.5,
                )

        try:
            if self._runner_started:
                fut = self._runner.submit(_close_all())
                try:
                    fut.result(timeout=3.0)
                except Exception:
                    pass
        finally:
            self._server = None
            self._local_client = None
            self._config_conn = None

        self.root.after(0, self._on_teardown_complete)

    def _on_teardown_complete(self) -> None:
        self._stopping = False
        self._session_server_hostname = ""

    def _start_local_client(self, host: str, port: int) -> None:
        client = Client(machine_id=self._machine_id,
                        server_host=host, server_port=port)
        self._local_client = client

        async def _wrap():
            try:
                await _run_local_client_safe(client)
            finally:
                # Server/network dropped us — reflect in UI.
                self.root.after(0, self._on_client_disconnected)

        self._runner.submit(_wrap())

    def _on_client_disconnected(self) -> None:
        # Triggered when the user-Client's run() ends (usually due to
        # the server going away or heartbeat watchdog firing). Without
        # a status bar there's nothing passive to update, so just
        # tear the session down — the Activity tab flips back to its
        # Host/Join empty state, which is unambiguous feedback.
        if self._session is None:
            return
        self._do_stop()

    def _connect_configurator(self, host: str, port: int) -> None:
        async def _run():
            try:
                reader, writer = await asyncio.open_connection(host, port)
            except OSError as e:
                log.warning("Configurator connection failed: %s", e)
                return
            conn = Connection(reader, writer, label="shell-config")
            self._config_conn = conn
            try:
                await conn.send(MsgType.REGISTER, RegisterMsg(
                    machine_id=SHELL_MACHINE_ID,
                    monitors=[], os="", platform_info="",
                ))
                while not conn.closed:
                    result = await conn.recv()
                    if result is None:
                        break
                    mt, payload = result
                    if mt == MsgType.LAYOUT_UPDATE:
                        self._handle_layout_update(payload)
                    elif mt == MsgType.HEARTBEAT:
                        await conn.send(MsgType.HEARTBEAT_ACK, payload)
                    elif mt == MsgType.REGISTER_ACK:
                        name = getattr(payload, "server_hostname", "") or ""
                        if name:
                            self._session_server_hostname = name
                            # Activity tab reads _session_server_hostname
                            # in its header, so a rebuild is enough.
                            if self._session is not None:
                                self.root.after(0, self._rebuild_activity)
            finally:
                self._config_conn = None

        self._config_task = self._runner.submit(_run())

    def _handle_layout_update(self, payload) -> None:
        # Late arrivals during teardown shouldn't flicker the UI back
        # to a "connected" state.
        if self._stopping or self._session is None:
            return

        monitors = getattr(payload, "monitors", None)
        if monitors is None and isinstance(payload, dict):
            monitors = payload.get("monitors", [])
        active = getattr(payload, "active_machine", "") or ""
        source = getattr(payload, "input_source", "") or ""
        machines = getattr(payload, "machines", {}) or {}
        monitors = monitors or []

        self._active_machine = active
        self._input_source = source
        self._machines_info = machines
        self._last_monitors = monitors

        def _apply_update():
            if self._stopping or self._session is None:
                return
            if self.layout_panel is not None:
                self.layout_panel.set_displays(monitors)
                self.layout_panel.set_active_machine(active)
            if "activity" in self._tab_frames:
                self._rebuild_activity()
        self.root.after(0, _apply_update)

    def _apply_layout(self, layout: list[dict]) -> None:
        if self._config_conn is None or self._config_conn.closed:
            return
        self._runner.submit(self._config_conn.send(
            MsgType.LAYOUT_APPLY, LayoutApplyMsg(displays=layout),
        ))
        if self.layout_panel is not None:
            self.layout_panel.mark_clean()

    def _request_identify(self) -> None:
        if self._config_conn is None or self._config_conn.closed:
            return
        self._runner.submit(self._config_conn.send(
            MsgType.REQUEST_IDENTIFY, RequestIdentifyMsg(),
        ))

    def _set_input_source(self, machine_id: str) -> None:
        if self._config_conn is None or self._config_conn.closed:
            return
        self._runner.submit(self._config_conn.send(
            MsgType.SET_INPUT_SOURCE,
            SetInputSourceMsg(machine_id=machine_id),
        ))

    # ── Discovery dialog (embedded) ──────────────────────────────

    def _show_discover_dialog(self) -> None:
        dlg = tk.Toplevel(self.root)
        dlg.title("UnIO — Find hosts on your network")
        dlg.geometry("440x360")
        dlg.configure(bg=PAPER_BG)
        dlg.transient(self.root)
        set_window_icon(dlg)

        tk.Label(dlg, text="Hosts on your network",
                 font=(FONT_SANS, SIZE_LG, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_BG,
                 pady=SPACE_SM).pack(anchor="w", padx=SPACE_LG,
                                     pady=(SPACE_LG, 0))

        status_var = tk.StringVar(value="Scanning…")
        tk.Label(dlg, textvariable=status_var,
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_BG).pack(
            anchor="w", padx=SPACE_LG, pady=(0, SPACE_SM))

        list_frame = tk.Frame(dlg, bg=PAPER_BORDER, padx=1, pady=1)
        list_frame.pack(fill=tk.BOTH, expand=True,
                        padx=SPACE_LG, pady=(0, SPACE_SM))
        listbox = tk.Listbox(
            list_frame, font=(FONT_SANS, SIZE_BASE),
            bg=PAPER_SURFACE, fg=PAPER_TEXT,
            selectbackground=LILAC, selectforeground="white",
            relief=tk.FLAT, highlightthickness=0, activestyle="none",
        )
        listbox.pack(fill=tk.BOTH, expand=True)

        hosts: list[DiscoveredHost] = []
        self_flags: list[bool] = []
        local_host, local_ips = local_identity()

        def is_self(h: DiscoveredHost) -> bool:
            return (
                h.ip in local_ips
                or (h.hostname != ""
                    and h.hostname.lower() == local_host.lower())
            )

        def do_pick():
            sel = listbox.curselection()
            if not sel:
                return
            idx = sel[0]
            if self_flags[idx]:
                messagebox.showinfo(
                    "This is the current PC",
                    "That's the machine you're on — pick a different host.",
                    parent=dlg,
                )
                return
            h = hosts[idx]
            dlg.destroy()
            self._join_to(h.ip, h.port)

        listbox.bind("<Double-1>", lambda _e: do_pick())

        def _guard(_e=None):
            sel = listbox.curselection()
            if sel and self_flags[sel[0]]:
                listbox.selection_clear(0, tk.END)
        listbox.bind("<<ListboxSelect>>", _guard)

        btn_row = tk.Frame(dlg, bg=PAPER_BG)
        btn_row.pack(fill=tk.X, padx=SPACE_LG, pady=(0, SPACE_LG))

        def run_scan():
            status_var.set("Scanning…")
            listbox.delete(0, tk.END)
            hosts.clear()
            self_flags.clear()

            def worker():
                try:
                    found = asyncio.run(discover_hosts(timeout=1.5))
                except Exception as e:
                    log.exception("Discovery failed: %s", e)
                    found = []
                dlg.after(0, apply_results, found)

            threading.Thread(target=worker, daemon=True,
                             name="unio-discover").start()

        def apply_results(found: list[DiscoveredHost]):
            hosts[:] = found
            self_flags[:] = [is_self(h) for h in hosts]
            listbox.delete(0, tk.END)
            remote = 0
            first_remote: Optional[int] = None
            for idx, (h, me) in enumerate(zip(hosts, self_flags)):
                if me:
                    listbox.insert(tk.END, f"  {h.label}  — this PC")
                    listbox.itemconfig(
                        idx, fg=PAPER_FAINT,
                        selectforeground=PAPER_FAINT,
                        selectbackground=PAPER_SURFACE)
                else:
                    listbox.insert(tk.END, f"  {h.label}")
                    remote += 1
                    if first_remote is None:
                        first_remote = idx
            if not hosts:
                status_var.set(
                    "No hosts responded. Check firewall / UDP 24801.")
            elif remote == 0:
                status_var.set(
                    "Only this PC responded — start UnIO on another "
                    "machine and Rescan.")
            else:
                status_var.set(
                    f"Found {remote} other host"
                    f"{'s' if remote != 1 else ''}. "
                    "Double-click one to connect.")
                if first_remote is not None:
                    listbox.selection_set(first_remote)

        PillButton(btn_row, "Rescan", command=run_scan,
                   variant="secondary").pack(side=tk.LEFT)
        PillButton(btn_row, "Connect", command=do_pick,
                   variant="primary").pack(side=tk.LEFT, padx=SPACE_SM)
        PillButton(btn_row, "Cancel", command=dlg.destroy,
                   variant="ghost").pack(side=tk.RIGHT)

        # Manual-IP link at the bottom.
        def manual_entry():
            dlg.destroy()
            self._show_manual_join()
        tk.Label(
            dlg, text="Or enter an address manually",
            font=(FONT_SANS, SIZE_XS, "underline"),
            fg=PAPER_MUTED, bg=PAPER_BG, cursor="hand2",
        ).pack(pady=(0, SPACE_MD))

        run_scan()

    def _show_manual_join(self) -> None:
        dlg = tk.Toplevel(self.root)
        dlg.title("UnIO — Connect to a host")
        dlg.geometry("380x240")
        dlg.configure(bg=PAPER_BG)
        dlg.transient(self.root)
        set_window_icon(dlg)

        tk.Label(dlg, text="Enter host address",
                 font=(FONT_SANS, SIZE_LG, "bold"),
                 fg=PAPER_TEXT, bg=PAPER_BG).pack(
            anchor="w", padx=SPACE_LG, pady=(SPACE_LG, SPACE_SM))

        host_entry = tk.Entry(dlg, font=(FONT_SANS, SIZE_BASE),
                              relief=tk.FLAT, bg=PAPER_SURFACE,
                              fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
                              highlightthickness=1,
                              highlightbackground=PAPER_BORDER,
                              highlightcolor=LILAC)
        host_entry.pack(fill=tk.X, padx=SPACE_LG, ipady=6)

        tk.Label(dlg, text="Port",
                 font=(FONT_SANS, SIZE_SM),
                 fg=PAPER_MUTED, bg=PAPER_BG).pack(
            anchor="w", padx=SPACE_LG, pady=(SPACE_MD, 2))
        port_entry = tk.Entry(dlg, font=(FONT_SANS, SIZE_BASE),
                              relief=tk.FLAT, bg=PAPER_SURFACE,
                              fg=PAPER_TEXT, insertbackground=PAPER_TEXT,
                              highlightthickness=1,
                              highlightbackground=PAPER_BORDER,
                              highlightcolor=LILAC)
        port_entry.insert(0, str(DEFAULT_PORT))
        port_entry.pack(fill=tk.X, padx=SPACE_LG, ipady=6)

        def connect():
            host = host_entry.get().strip()
            if not host:
                messagebox.showerror("Host required",
                                     "Enter a host or IP.", parent=dlg)
                return
            try:
                port = int(port_entry.get().strip())
            except ValueError:
                messagebox.showerror("Invalid port",
                                     "Port must be a number.", parent=dlg)
                return
            dlg.destroy()
            self._join_to(host, port)

        host_entry.bind("<Return>", lambda _e: connect())
        port_entry.bind("<Return>", lambda _e: connect())
        host_entry.focus_set()

        row = tk.Frame(dlg, bg=PAPER_BG)
        row.pack(fill=tk.X, padx=SPACE_LG, pady=SPACE_LG)
        PillButton(row, "Connect", command=connect, variant="primary"
                   ).pack(side=tk.LEFT)
        PillButton(row, "Cancel", command=dlg.destroy, variant="ghost"
                   ).pack(side=tk.RIGHT)

    # ── Lifecycle ────────────────────────────────────────────────

    def _ensure_runner(self) -> None:
        if not self._runner_started:
            self._runner.start()
            self._runner_started = True

    def _on_close(self) -> None:
        # Synchronous teardown on window close — no point leaving UI
        # responsive at this point, and we want the asyncio loop shut
        # down before Python exits so backends get their close()s.
        async def _close_all():
            tasks = []
            if self._config_conn is not None and not self._config_conn.closed:
                tasks.append(self._config_conn.close())
            if self._local_client is not None:
                tasks.append(self._local_client.stop())
            if self._server is not None:
                tasks.append(self._server.stop())
            if tasks:
                await asyncio.wait_for(
                    asyncio.gather(*tasks, return_exceptions=True),
                    timeout=3.0,
                )

        if self._runner_started:
            try:
                self._runner.submit(_close_all()).result(timeout=4)
            except Exception:
                pass
            self._runner.stop()
        self.root.destroy()

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

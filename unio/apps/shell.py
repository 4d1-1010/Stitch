"""Main shell window for the UnIO UI.

Everything users see after launch lives here: a narrow left rail
with tab nav, a main content area that swaps per tab, and a small
status block pinned to the bottom of the rail (hostname / role /
connection count).

This commit lays out the shell scaffolding with empty tab bodies —
future commits populate each tab (Activity, Layout, Settings) and
reroute the launcher through this window.

Run standalone for visual preview:

    python -m unio.apps.shell
"""

from __future__ import annotations

import socket
import tkinter as tk
from dataclasses import dataclass
from typing import Callable, Optional

import unio
from .ui_theme import (
    FONT_SANS, LILAC, MINT, PAPER_BG, PAPER_BORDER, PAPER_FAINT,
    PAPER_MUTED, PAPER_RAIL, PAPER_SURFACE, PAPER_TEXT,
    RADIUS_MD, SIZE_BASE, SIZE_LG, SIZE_SM, SIZE_TITLE, SIZE_XL, SIZE_XS,
    SPACE_LG, SPACE_MD, SPACE_SM, SPACE_XL, SPACE_XS,
    PillButton, RailButton, StatusDot, hairline, set_window_icon,
)


RAIL_WIDTH = 92
MIN_WIDTH = 860
MIN_HEIGHT = 560


@dataclass
class Tab:
    key: str
    label: str
    glyph: str
    build: Callable[[tk.Widget], tk.Widget]


class MainWindow:
    """The single top-level window that hosts every UnIO UI surface."""

    def __init__(self) -> None:
        self.root = tk.Tk(className="UnIO")
        self.root.title("UnIO")
        self.root.configure(bg=PAPER_BG)
        self.root.minsize(MIN_WIDTH, MIN_HEIGHT)
        self.root.geometry(f"{MIN_WIDTH + 40}x{MIN_HEIGHT + 80}")
        set_window_icon(self.root)

        self._active_tab = tk.StringVar(value="activity")
        self._tab_frames: dict[str, tk.Widget] = {}

        self._tabs: list[Tab] = [
            Tab("activity", "Activity", "◉", self._build_activity_placeholder),
            Tab("layout",   "Layout",   "▦", self._build_layout_placeholder),
            Tab("settings", "Settings", "⚙", self._build_settings_placeholder),
        ]
        if unio.DEV_LOGS:
            self._tabs.append(
                Tab("logs", "Logs", "≡", self._build_logs_placeholder),
            )

        self._build()

    # ── Skeleton ─────────────────────────────────────────────────

    def _build(self) -> None:
        outer = tk.Frame(self.root, bg=PAPER_BG)
        outer.pack(fill=tk.BOTH, expand=True)

        rail = self._build_rail(outer)
        rail.pack(side=tk.LEFT, fill=tk.Y)

        # Hairline between rail and content for a subtle seam.
        hairline(outer, axis="y").pack(side=tk.LEFT, fill=tk.Y)

        self._content = tk.Frame(outer, bg=PAPER_BG)
        self._content.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # Show the initial tab.
        self._active_tab.trace_add("write", lambda *_: self._show_tab(
            self._active_tab.get()))
        self._show_tab(self._active_tab.get())

    def _build_rail(self, parent: tk.Widget) -> tk.Frame:
        rail = tk.Frame(parent, bg=PAPER_RAIL, width=RAIL_WIDTH)
        rail.pack_propagate(False)

        # Brand mark at the top — just a wordmark for now.
        brand = tk.Frame(rail, bg=PAPER_RAIL, pady=SPACE_LG)
        brand.pack(fill=tk.X)
        tk.Label(
            brand, text="UnIO",
            font=(FONT_SANS, SIZE_LG, "bold"),
            fg=PAPER_TEXT, bg=PAPER_RAIL,
        ).pack()

        hairline(rail, axis="x").pack(fill=tk.X, padx=SPACE_MD)

        # Tab buttons.
        nav = tk.Frame(rail, bg=PAPER_RAIL)
        nav.pack(fill=tk.X, pady=(SPACE_SM, 0))
        for tab in self._tabs:
            RailButton(
                nav, label=tab.label, glyph=tab.glyph,
                value=tab.key, var=self._active_tab,
            ).pack(fill=tk.X, pady=1)

        # Bottom status block.
        status_wrap = tk.Frame(rail, bg=PAPER_RAIL)
        status_wrap.pack(side=tk.BOTTOM, fill=tk.X,
                         padx=SPACE_SM, pady=SPACE_SM)
        hairline(status_wrap, axis="x").pack(fill=tk.X, pady=(0, SPACE_SM))

        row = tk.Frame(status_wrap, bg=PAPER_RAIL)
        row.pack(fill=tk.X, padx=SPACE_XS)
        self._status_dot = StatusDot(row, state="idle", bg=PAPER_RAIL)
        self._status_dot.pack(side=tk.LEFT, pady=(3, 0))
        self._status_text = tk.StringVar(value="Not connected")
        tk.Label(
            row, textvariable=self._status_text,
            font=(FONT_SANS, SIZE_XS, "bold"),
            fg=PAPER_TEXT, bg=PAPER_RAIL, anchor="w",
        ).pack(side=tk.LEFT, padx=(SPACE_XS, 0), fill=tk.X, expand=True)

        self._hostname_text = tk.StringVar(value=socket.gethostname() or "unio")
        tk.Label(
            status_wrap, textvariable=self._hostname_text,
            font=(FONT_SANS, SIZE_XS),
            fg=PAPER_MUTED, bg=PAPER_RAIL, anchor="w",
        ).pack(fill=tk.X, padx=SPACE_XS)

        return rail

    def _show_tab(self, key: str) -> None:
        for k, frame in self._tab_frames.items():
            frame.pack_forget()
        if key not in self._tab_frames:
            tab = next((t for t in self._tabs if t.key == key), None)
            if tab is None:
                return
            frame = tab.build(self._content)
            self._tab_frames[key] = frame
        self._tab_frames[key].pack(fill=tk.BOTH, expand=True)

    # ── Public status API (wired up by later commits) ────────────

    def set_status(self, *, state: str, text: str,
                   hostname: Optional[str] = None) -> None:
        """Update the rail's bottom status block.

        state: "ok" / "warn" / "bad" / "idle" — drives the dot color.
        text:  short line shown next to the dot.
        hostname: optional override for the second line.
        """
        self._status_dot.set_state(state)
        self._status_text.set(text)
        if hostname is not None:
            self._hostname_text.set(hostname)

    # ── Placeholder tab builders (filled in later commits) ───────

    def _build_activity_placeholder(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._empty_state(
            frame,
            title="No session running",
            body="Start hosting on this PC, or join another host on your "
                 "network.",
            primary=("Host on this PC", lambda: self.set_status(
                state="ok", text="Hosting")),
            secondary=("Join another host", lambda: self.set_status(
                state="ok", text="Joining…")),
        )
        return frame

    def _build_layout_placeholder(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._empty_state(
            frame,
            title="Layout",
            body="The drag-and-drop display arrangement lives here. It "
                 "becomes available once you're connected to a session.",
        )
        return frame

    def _build_settings_placeholder(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._empty_state(
            frame,
            title="Settings",
            body=f"UnIO {unio.__version__} — settings land here.",
        )
        return frame

    def _build_logs_placeholder(self, parent: tk.Widget) -> tk.Widget:
        frame = tk.Frame(parent, bg=PAPER_BG)
        self._empty_state(
            frame,
            title="Logs (developer)",
            body="Diagnostic log viewer. Visible only in dev builds "
                 "or with UNIO_DEV_LOGS=1.",
        )
        return frame

    def _empty_state(self, parent: tk.Widget, *,
                     title: str, body: str,
                     primary: Optional[tuple[str, Callable[[], None]]] = None,
                     secondary: Optional[tuple[str, Callable[[], None]]] = None,
                     ) -> None:
        # Simple centered block. Good enough until each tab gets its
        # real contents.
        center = tk.Frame(parent, bg=PAPER_BG)
        center.place(relx=0.5, rely=0.5, anchor="center")

        tk.Label(
            center, text=title,
            font=(FONT_SANS, SIZE_TITLE, "bold"),
            fg=PAPER_TEXT, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_SM))

        tk.Label(
            center, text=body, wraplength=420, justify="center",
            font=(FONT_SANS, SIZE_BASE),
            fg=PAPER_MUTED, bg=PAPER_BG,
        ).pack(pady=(0, SPACE_LG))

        if primary or secondary:
            btn_row = tk.Frame(center, bg=PAPER_BG)
            btn_row.pack()
            if primary:
                PillButton(btn_row, primary[0], command=primary[1],
                           variant="primary").pack(
                    side=tk.LEFT, padx=SPACE_SM)
            if secondary:
                PillButton(btn_row, secondary[0], command=secondary[1],
                           variant="secondary").pack(
                    side=tk.LEFT, padx=SPACE_SM)

    # ── Run ──────────────────────────────────────────────────────

    def run(self) -> None:
        self.root.mainloop()


def main() -> None:
    MainWindow().run()


if __name__ == "__main__":
    main()

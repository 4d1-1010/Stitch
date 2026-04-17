#!/usr/bin/env python3
"""Stitch — single UI entry point.

Launching this script opens a window that asks whether to host or join
a Stitch session. Everything (server startup, client connection,
display-layout configuration) is driven from the UI — no command-line
flags required.
"""

from __future__ import annotations

import asyncio
import logging
import multiprocessing
import platform
import re
import socket
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox
from typing import Optional

# Required before anything else: when the PyInstaller bundle uses
# multiprocessing (e.g. the identify overlays), child processes re-invoke
# the executable and fall through to this line — without freeze_support
# they'd start a second launcher window instead of running the child.
multiprocessing.freeze_support()

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from stitch.apps.server import Server
from stitch.apps.client import Client
from stitch.apps.configurator import ConfiguratorApp
from stitch.apps.log_view import install_log_buffer, show_log_window
from stitch.apps.ui_theme import (
    ACCENT, ACCENT_2, BG_DARK, FONT, TEXT, TEXT_DIM, TEXT_FAINT,
    Backdrop, Panel,
    label, make_button, make_root, set_window_icon,
)
from stitch.core.discovery import DiscoveredHost, discover_hosts

log = logging.getLogger("stitch")

DEFAULT_PORT = 24800


# ── Utilities ────────────────────────────────────────────────────

def _default_machine_id() -> str:
    raw = socket.gethostname() or "machine"
    clean = re.sub(r"[^A-Za-z0-9_-]", "-", raw).strip("-")
    return clean or "machine"


def _local_ip() -> str:
    """Best-effort non-loopback IP."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


class AsyncRunner:
    """Runs an asyncio loop in a daemon thread."""

    def __init__(self) -> None:
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self._ready = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        self._thread = threading.Thread(
            target=self._run, daemon=True, name="stitch-asyncio",
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


# ── Launcher ─────────────────────────────────────────────────────

class Launcher:
    """Initial role-selection window."""

    def __init__(self) -> None:
        self.choice: Optional[dict] = None

        self.root = make_root("Stitch")
        self.root.geometry("460x520")
        self.root.configure(bg=BG_DARK)
        self.root.resizable(False, False)
        set_window_icon(self.root)

        self.canvas = tk.Canvas(self.root, bg=BG_DARK,
                                highlightthickness=0, bd=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        Backdrop(self.root, self.canvas)

        self.root.after(30, self._draw_home)

    # Drawing helpers ------------------------------------------------

    def _draw_home(self) -> None:
        self.canvas.delete("content")
        self.canvas.update_idletasks()
        w = self.canvas.winfo_width() or 460
        h = self.canvas.winfo_height() or 520

        self._logo = self._load_logo(96)
        if self._logo is not None:
            self.canvas.create_image(w // 2, 90, image=self._logo,
                                     tags="content", anchor="center")

        self.canvas.create_text(
            w // 2, 180, text="Stitch", fill=TEXT, tags="content",
            font=(FONT, 30, "bold"),
        )
        self.canvas.create_text(
            w // 2, 215, text="Shared cursor across machines",
            fill=TEXT_DIM, tags="content", font=(FONT, 11),
        )

        panel = Panel(self.canvas, x=w // 2 - 170, y=260,
                           width=340, height=200, radius=22)
        inner = panel.content

        label(inner, "Start a session", bold=True, size=13).pack(pady=(6, 4))
        label(inner, "Pick how you'd like to run Stitch on this machine.",
              size=9, fg=TEXT_FAINT).pack(pady=(0, 14))

        make_button(inner, "Host on this machine", primary=True,
                    command=self._pick_host).pack(fill=tk.X, pady=4)
        make_button(inner, "Join an existing host",
                    command=self._pick_join).pack(fill=tk.X, pady=4)

        self._panel_ref = panel  # keep reference

    def _draw_join_form(self) -> None:
        self.canvas.delete("content")
        self._panel_ref = None
        w = self.canvas.winfo_width() or 460
        h = self.canvas.winfo_height() or 520

        self._logo = self._load_logo(64)
        if self._logo is not None:
            self.canvas.create_image(w // 2, 80, image=self._logo,
                                     tags="content", anchor="center")

        self.canvas.create_text(
            w // 2, 150, text="Join a host", fill=TEXT, tags="content",
            font=(FONT, 22, "bold"),
        )
        self.canvas.create_text(
            w // 2, 180,
            text="Enter the address of the machine hosting the session.",
            fill=TEXT_DIM, tags="content", font=(FONT, 10),
        )

        panel = Panel(self.canvas, x=w // 2 - 180, y=220,
                           width=360, height=320, radius=22)
        inner = panel.content

        label(inner, "Server address", bold=True, size=11).pack(
            anchor="w", pady=(4, 4))
        host_entry = tk.Entry(inner, font=(FONT, 11),
                              bg="#141734", fg=TEXT, relief=tk.FLAT,
                              insertbackground=TEXT,
                              highlightthickness=1,
                              highlightbackground="#2a2e5a",
                              highlightcolor=ACCENT_2)
        host_entry.pack(fill=tk.X, ipady=6)

        label(inner, "Port", bold=True, size=11).pack(
            anchor="w", pady=(12, 4))
        port_entry = tk.Entry(inner, font=(FONT, 11),
                              bg="#141734", fg=TEXT, relief=tk.FLAT,
                              insertbackground=TEXT,
                              highlightthickness=1,
                              highlightbackground="#2a2e5a",
                              highlightcolor=ACCENT_2)
        port_entry.insert(0, str(DEFAULT_PORT))
        port_entry.pack(fill=tk.X, ipady=6)

        def back():
            self._draw_home()

        def connect():
            host = host_entry.get().strip()
            if not host:
                messagebox.showerror("Host required",
                                     "Enter a host or IP.", parent=self.root)
                return
            try:
                port = int(port_entry.get().strip())
            except ValueError:
                messagebox.showerror("Invalid port",
                                     "Port must be a number.", parent=self.root)
                return
            self.choice = {"mode": "join", "host": host, "port": port}
            self.root.destroy()

        def open_discover():
            _show_discover_dialog(
                self.root,
                lambda ip, port: (
                    host_entry.delete(0, tk.END),
                    host_entry.insert(0, ip),
                    port_entry.delete(0, tk.END),
                    port_entry.insert(0, str(port)),
                ),
            )

        make_button(inner, "Find hosts on LAN",
                    command=open_discover).pack(fill=tk.X, pady=(16, 0))

        btn_row = tk.Frame(inner, bg=inner.cget("bg"))
        btn_row.pack(fill=tk.X, pady=(12, 0))

        make_button(btn_row, "Back", command=back).pack(
            side=tk.LEFT, expand=True, fill=tk.X, padx=(0, 6))
        make_button(btn_row, "Connect", primary=True, command=connect).pack(
            side=tk.LEFT, expand=True, fill=tk.X, padx=(6, 0))

        host_entry.bind("<Return>", lambda _e: connect())
        port_entry.bind("<Return>", lambda _e: connect())
        host_entry.focus_set()
        self._panel_ref = panel

    def _load_logo(self, size: int):
        candidate = (Path(__file__).resolve().parent.parent
                     / "assets" / f"logo_{size}.png")
        if not candidate.exists():
            candidate = (Path(__file__).resolve().parent.parent
                         / "assets" / "logo.png")
        if not candidate.exists():
            return None
        try:
            return tk.PhotoImage(file=str(candidate))
        except tk.TclError:
            return None

    # Event handlers -------------------------------------------------

    def _pick_host(self):
        self.choice = {"mode": "host"}
        self.root.destroy()

    def _pick_join(self):
        self._draw_join_form()

    def run(self) -> Optional[dict]:
        self.root.mainloop()
        return self.choice


def _show_discover_dialog(parent: tk.Misc, on_pick) -> None:
    """Open a small Toplevel that scans the LAN and lists responders."""
    dlg = tk.Toplevel(parent)
    dlg.title("Stitch — Discover hosts")
    dlg.geometry("420x340")
    dlg.configure(bg=BG_DARK)
    dlg.transient(parent)
    set_window_icon(dlg)

    tk.Label(dlg, text="Hosts on your network",
             font=(FONT, 13, "bold"), fg=TEXT, bg=BG_DARK,
             pady=10).pack(anchor="w", padx=14)

    status_var = tk.StringVar(value="Scanning…")
    tk.Label(dlg, textvariable=status_var,
             font=(FONT, 10), fg=TEXT_DIM, bg=BG_DARK).pack(
        anchor="w", padx=14, pady=(0, 8))

    list_frame = tk.Frame(dlg, bg=BG_DARK)
    list_frame.pack(fill=tk.BOTH, expand=True, padx=14, pady=(0, 4))
    listbox = tk.Listbox(list_frame, font=(FONT, 11), bg="#141734",
                         fg=TEXT, selectbackground=ACCENT_2,
                         selectforeground="white", relief=tk.FLAT,
                         highlightthickness=0, activestyle="none")
    listbox.pack(fill=tk.BOTH, expand=True)

    hosts: list[DiscoveredHost] = []

    def do_pick():
        sel = listbox.curselection()
        if not sel:
            return
        h = hosts[sel[0]]
        on_pick(h.ip, h.port)
        dlg.destroy()

    listbox.bind("<Double-1>", lambda _e: do_pick())

    btn_row = tk.Frame(dlg, bg=BG_DARK)
    btn_row.pack(fill=tk.X, padx=14, pady=10)

    def run_scan():
        status_var.set("Scanning…")
        listbox.delete(0, tk.END)
        hosts.clear()

        def worker():
            try:
                found = asyncio.run(discover_hosts(timeout=1.5))
            except Exception as e:
                log.exception("Discovery failed: %s", e)
                found = []
            dlg.after(0, apply_results, found)

        threading.Thread(target=worker, daemon=True,
                         name="stitch-discover").start()

    def apply_results(found: list[DiscoveredHost]):
        hosts[:] = found
        listbox.delete(0, tk.END)
        for h in hosts:
            listbox.insert(tk.END, f"  {h.label}")
        if hosts:
            status_var.set(f"Found {len(hosts)} host"
                           f"{'s' if len(hosts) != 1 else ''}. "
                           "Pick one and click Use.")
            listbox.selection_set(0)
        else:
            status_var.set("No hosts responded. Check firewall / UDP 24801.")

    make_button(btn_row, "Rescan", command=run_scan).pack(
        side=tk.LEFT, padx=(0, 6))
    make_button(btn_row, "Use selected", primary=True,
                command=do_pick).pack(side=tk.LEFT, padx=(0, 6))
    make_button(btn_row, "Close", command=dlg.destroy).pack(side=tk.RIGHT)

    run_scan()


# ── Host mode ────────────────────────────────────────────────────

def run_host_mode(port: int = DEFAULT_PORT) -> None:
    runner = AsyncRunner()
    runner.start()

    server = Server(host="0.0.0.0", port=port)
    machine_id = _default_machine_id()
    local_client = Client(machine_id=machine_id,
                          server_host="127.0.0.1", server_port=port)
    ready = threading.Event()
    startup_error: dict = {}

    async def _host_startup():
        try:
            await server.start()
        except OSError as e:
            startup_error["err"] = e
            ready.set()
            return
        ready.set()
        asyncio.create_task(_run_local_client_safe(local_client))
        await server.serve_forever()

    runner.submit(_host_startup())

    if not ready.wait(timeout=8):
        messagebox.showerror("Startup failed",
                             "Server did not come up in time.")
        runner.stop()
        return

    if "err" in startup_error:
        err = startup_error["err"]
        messagebox.showerror(
            "Port in use",
            f"Couldn't bind port {port}:\n{err}\n\n"
            "Another Stitch instance may already be running.",
        )
        runner.stop()
        return

    ip = _local_ip()
    app = ConfiguratorApp(server_host="127.0.0.1", server_port=port)
    app.root.title(f"Stitch — Hosting at {ip}:{port}")
    set_window_icon(app.root)
    _install_host_header(app, ip, port)
    app.run()

    try:
        runner.submit(server.stop()).result(timeout=3)
    except Exception:
        pass
    runner.stop()


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


def _install_host_header(app: ConfiguratorApp, ip: str, port: int) -> None:
    first = app.root.winfo_children()[0] if app.root.winfo_children() else None
    header = tk.Frame(app.root, bg="#0f1230", pady=6)
    if first is not None:
        header.pack(fill=tk.X, side=tk.TOP, before=first)
    else:
        header.pack(fill=tk.X, side=tk.TOP)

    hostname = socket.gethostname() or "stitch"

    tk.Label(header, text="  Hosting  ", bg=ACCENT, fg="white",
             font=(FONT, 9, "bold"), padx=2).pack(side=tk.LEFT, padx=(12, 8))
    tk.Label(header, text=f"{hostname} ({ip}:{port})", bg="#0f1230",
             fg=TEXT, font=(FONT, 12, "bold")).pack(side=tk.LEFT)
    tk.Label(header, text="  •  Clients can also Find hosts on LAN",
             bg="#0f1230", fg=TEXT_FAINT, font=(FONT, 9)).pack(side=tk.LEFT)

    tk.Button(
        header, text="View logs",
        font=(FONT, 9, "bold"), fg="white", bg="#262a54",
        activebackground="#30346a", relief=tk.FLAT, bd=0,
        padx=10, pady=4, cursor="hand2",
        command=lambda: show_log_window(app.root),
    ).pack(side=tk.RIGHT, padx=(8, 12))


# ── Join mode ────────────────────────────────────────────────────

def run_join_mode(host: str, port: int) -> None:
    runner = AsyncRunner()
    runner.start()

    machine_id = _default_machine_id()
    client = Client(machine_id=machine_id, server_host=host, server_port=port)

    root = make_root(f"Stitch — Connected to {host}:{port}")
    root.geometry("460x420")
    root.configure(bg=BG_DARK)
    root.resizable(False, False)
    set_window_icon(root)

    canvas = tk.Canvas(root, bg=BG_DARK, highlightthickness=0, bd=0)
    canvas.pack(fill=tk.BOTH, expand=True)
    Backdrop(root, canvas)

    w, h = 460, 420

    logo_img = _try_load_logo(64)
    if logo_img is not None:
        canvas.create_image(w // 2, 70, image=logo_img, anchor="center")
        canvas._logo_ref = logo_img  # type: ignore

    canvas.create_text(w // 2, 140, text="Connected", fill=TEXT,
                       font=(FONT, 22, "bold"))

    panel = Panel(canvas, x=w // 2 - 180, y=170,
                       width=360, height=220, radius=22)
    inner = panel.content

    row = tk.Frame(inner, bg=inner.cget("bg"))
    row.pack(fill=tk.X, pady=(6, 2))
    label(row, "SERVER", size=9, fg=TEXT_FAINT).pack(anchor="w")
    server_var = tk.StringVar(value=f"{host}:{port}")
    tk.Label(row, textvariable=server_var, font=(FONT, 13, "bold"),
             fg=TEXT, bg=inner.cget("bg"), anchor="w").pack(
        anchor="w", fill=tk.X)

    def poll_hostname():
        name = getattr(client, "server_hostname", "") or ""
        if name:
            server_var.set(f"{name} ({host}:{port})")
            return
        if root.winfo_exists():
            root.after(500, poll_hostname)
    root.after(500, poll_hostname)

    row2 = tk.Frame(inner, bg=inner.cget("bg"))
    row2.pack(fill=tk.X, pady=(12, 2))
    label(row2, "THIS MACHINE", size=9, fg=TEXT_FAINT).pack(anchor="w")
    label(row2, machine_id, size=13, bold=True).pack(anchor="w")

    row3 = tk.Frame(inner, bg=inner.cget("bg"))
    row3.pack(fill=tk.X, pady=(12, 2))
    label(row3, "OS", size=9, fg=TEXT_FAINT).pack(anchor="w")
    label(row3, _describe_platform(), size=11).pack(anchor="w")

    status_var = tk.StringVar(value="● Running — cursor will follow the layout")
    status_lbl = tk.Label(inner, textvariable=status_var,
                          font=(FONT, 10, "italic"),
                          fg=ACCENT_2, bg=inner.cget("bg"))
    status_lbl.pack(anchor="w", pady=(16, 0))

    def on_close():
        runner.submit(client.stop())
        runner.stop()
        root.destroy()

    def on_disconnect():
        status_var.set("● Disconnected — server closed or unreachable")
        status_lbl.config(fg="#e94560")

    btn_row = tk.Frame(inner, bg=inner.cget("bg"))
    btn_row.pack(anchor="center", pady=(18, 0))
    make_button(btn_row, "View logs",
                command=lambda: show_log_window(root)).pack(
        side=tk.LEFT, padx=(0, 8))
    make_button(btn_row, "Disconnect", command=on_close).pack(side=tk.LEFT)

    root.protocol("WM_DELETE_WINDOW", on_close)

    async def _watch_client():
        try:
            await _run_local_client_safe(client)
        finally:
            root.after(0, on_disconnect)

    runner.submit(_watch_client())
    root.mainloop()


def _try_load_logo(size: int):
    p = Path(__file__).resolve().parent.parent / "assets" / f"logo_{size}.png"
    if not p.exists():
        p = Path(__file__).resolve().parent.parent / "assets" / "logo.png"
    if not p.exists():
        return None
    try:
        return tk.PhotoImage(file=str(p))
    except tk.TclError:
        return None


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


# ── Entrypoint ───────────────────────────────────────────────────

def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )
    install_log_buffer()

    choice = Launcher().run()
    if not choice:
        return
    if choice["mode"] == "host":
        run_host_mode(DEFAULT_PORT)
    elif choice["mode"] == "join":
        run_join_mode(choice["host"], choice["port"])


if __name__ == "__main__":
    main()

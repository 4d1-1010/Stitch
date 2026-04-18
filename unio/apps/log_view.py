"""In-memory log buffer and a Tk viewer window.

Attaches a `logging.Handler` to the root logger that keeps the last N
formatted lines in a ring buffer. A small Toplevel window renders the
buffer and auto-refreshes so users can inspect diagnostics without
rerunning from a terminal.
"""

from __future__ import annotations

import logging
import threading
import tkinter as tk
from collections import deque
from tkinter import scrolledtext
from typing import Optional

from .ui_theme import (
    ACCENT, ACCENT_2, BG_DARK, FONT, TEXT, TEXT_DIM,
    make_button, set_window_icon,
)

_BUFFER_SIZE = 2000

_log_buffer: Optional["LogBuffer"] = None


class LogBuffer(logging.Handler):
    """Ring-buffer logging handler used by the in-app log viewer."""

    def __init__(self, capacity: int = _BUFFER_SIZE):
        super().__init__()
        self._lines: deque[str] = deque(maxlen=capacity)
        self._lock = threading.Lock()
        self._seq = 0

    def emit(self, record: logging.LogRecord) -> None:
        try:
            line = self.format(record)
        except Exception:
            return
        with self._lock:
            self._lines.append(line)
            self._seq += 1

    def snapshot(self) -> tuple[int, list[str]]:
        """Return (seq, [lines]). seq lets callers skip redraws."""
        with self._lock:
            return self._seq, list(self._lines)


def install_log_buffer() -> LogBuffer:
    """Attach the ring-buffer handler to the root logger (idempotent)."""
    global _log_buffer
    if _log_buffer is not None:
        return _log_buffer
    buf = LogBuffer()
    buf.setFormatter(logging.Formatter(
        fmt="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    ))
    buf.setLevel(logging.DEBUG)
    root_logger = logging.getLogger()
    # Make sure we see DEBUG in the buffer even if the root stayed at INFO.
    if root_logger.level > logging.DEBUG or root_logger.level == logging.NOTSET:
        root_logger.setLevel(logging.DEBUG)
    root_logger.addHandler(buf)
    _log_buffer = buf
    return buf


def get_log_buffer() -> Optional[LogBuffer]:
    return _log_buffer


def show_log_window(parent: tk.Misc, *, title: str = "UnIO — Logs") -> None:
    """Open (or raise) the log viewer window."""
    buf = _log_buffer
    if buf is None:
        buf = install_log_buffer()

    existing = getattr(parent, "_stitch_log_window", None)
    if existing is not None and existing.winfo_exists():
        existing.deiconify()
        existing.lift()
        existing.focus_force()
        return

    win = tk.Toplevel(parent)
    win.title(title)
    win.geometry("880x520")
    win.configure(bg=BG_DARK)
    set_window_icon(win)

    header = tk.Frame(win, bg=BG_DARK, pady=8, padx=12)
    header.pack(fill=tk.X)

    tk.Label(header, text="Live logs", font=(FONT, 13, "bold"),
             fg=TEXT, bg=BG_DARK).pack(side=tk.LEFT)
    tk.Label(header,
             text=f"  (last {_BUFFER_SIZE} lines — updates live)",
             font=(FONT, 10), fg=TEXT_DIM, bg=BG_DARK).pack(side=tk.LEFT)

    autoscroll_var = tk.BooleanVar(value=True)
    tk.Checkbutton(
        header, text="Auto-scroll", variable=autoscroll_var,
        font=(FONT, 10), fg=TEXT_DIM, bg=BG_DARK,
        activebackground=BG_DARK, activeforeground=TEXT,
        selectcolor=BG_DARK, bd=0, highlightthickness=0,
    ).pack(side=tk.RIGHT)

    text = scrolledtext.ScrolledText(
        win, font=("Monospace", 10), bg="#0a0c20", fg=TEXT,
        insertbackground=TEXT, relief=tk.FLAT, bd=0,
        padx=8, pady=6, wrap=tk.NONE,
    )
    text.pack(fill=tk.BOTH, expand=True, padx=12, pady=(0, 8))
    text.configure(state=tk.DISABLED)

    btn_row = tk.Frame(win, bg=BG_DARK, pady=8, padx=12)
    btn_row.pack(fill=tk.X)

    def do_copy():
        _, lines = buf.snapshot()
        payload = "\n".join(lines)
        win.clipboard_clear()
        win.clipboard_append(payload)

    def do_clear():
        # Only clears what the user sees — keep the underlying buffer
        # so a reopen still shows recent history.
        text.configure(state=tk.NORMAL)
        text.delete("1.0", tk.END)
        text.configure(state=tk.DISABLED)

    make_button(btn_row, "Copy all", command=do_copy).pack(
        side=tk.LEFT, padx=(0, 8))
    make_button(btn_row, "Clear view", command=do_clear).pack(side=tk.LEFT)
    make_button(btn_row, "Close", command=win.destroy).pack(side=tk.RIGHT)

    last_seq = -1

    def refresh():
        nonlocal last_seq
        seq, lines = buf.snapshot()
        if seq != last_seq:
            last_seq = seq
            text.configure(state=tk.NORMAL)
            text.delete("1.0", tk.END)
            text.insert(tk.END, "\n".join(lines))
            if autoscroll_var.get():
                text.see(tk.END)
            text.configure(state=tk.DISABLED)
        if win.winfo_exists():
            win.after(500, refresh)

    refresh()

    def on_close():
        parent._stitch_log_window = None  # type: ignore[attr-defined]
        win.destroy()

    win.protocol("WM_DELETE_WINDOW", on_close)
    parent._stitch_log_window = win  # type: ignore[attr-defined]

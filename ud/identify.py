"""
Display identification overlay.

Shows a large number on each monitor so the user can visually identify
which physical display corresponds to which entry in the layout tool.

Uses tkinter (ships with Python on all platforms) to create fullscreen
overlays. Each monitor gets its own overlay in a separate process to
avoid tkinter's single-mainloop limitation.
"""

import multiprocessing
import sys
import time
from typing import Optional


def _show_overlay(x: int, y: int, w: int, h: int,
                  number: int, label: str, duration: int):
    """
    Show a fullscreen overlay on a single monitor.
    Runs in its own process (tkinter requires one mainloop per process).
    """
    try:
        import tkinter as tk
    except ImportError:
        print(f"[identify] tkinter not available, cannot show overlay "
              f"for display {number}", file=sys.stderr)
        return

    root = tk.Tk()
    root.title(f"Display {number}")
    root.overrideredirect(True)
    root.geometry(f"{w}x{h}+{x}+{y}")
    root.configure(bg="#111111")

    # Try to set transparency and always-on-top
    try:
        root.attributes("-alpha", 0.85)
    except tk.TclError:
        pass
    try:
        root.attributes("-topmost", True)
    except tk.TclError:
        pass

    frame = tk.Frame(root, bg="#111111")
    frame.place(relx=0.5, rely=0.5, anchor="center")

    # Large display number
    num_size = max(48, min(300, h // 3))
    num_label = tk.Label(
        frame, text=str(number),
        font=("Helvetica", num_size, "bold"),
        fg="#FFFFFF", bg="#111111",
    )
    num_label.pack()

    # Machine + monitor label
    sub_size = max(16, min(60, h // 15))
    sub_label = tk.Label(
        frame, text=label,
        font=("Helvetica", sub_size),
        fg="#888888", bg="#111111",
    )
    sub_label.pack(pady=(10, 0))

    # Resolution info
    res_size = max(12, min(36, h // 25))
    res_label = tk.Label(
        frame, text=f"{w} x {h}",
        font=("Helvetica", res_size),
        fg="#555555", bg="#111111",
    )
    res_label.pack(pady=(8, 0))

    # Close after duration
    root.after(duration * 1000, root.destroy)

    # Also close on any key press or mouse click
    root.bind("<Key>", lambda e: root.destroy())
    root.bind("<Button>", lambda e: root.destroy())

    root.mainloop()


def show_identify(displays: list[dict], duration: int = 3):
    """
    Show identification overlays on multiple monitors.

    Args:
        displays: list of dicts with keys:
            x, y, width, height: monitor position/size in local coords
            number: display number to show
            label: text label (e.g. "PC1 - HDMI-1")
        duration: seconds to show (default 3)
    """
    processes = []
    for d in displays:
        p = multiprocessing.Process(
            target=_show_overlay,
            args=(
                d["x"], d["y"], d["width"], d["height"],
                d["number"], d.get("label", ""), duration,
            ),
            daemon=True,
        )
        p.start()
        processes.append(p)

    # Wait for all overlays to close
    for p in processes:
        p.join(timeout=duration + 2)
        if p.is_alive():
            p.terminate()

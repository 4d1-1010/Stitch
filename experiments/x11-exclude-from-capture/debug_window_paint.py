#!/usr/bin/env python3
"""
Diagnostic: confirm that creating + mapping + filling an
override_redirect window actually writes the sentinel colour into
the window's own pixmap.

Expected:
  - win.get_image(...) returns the sentinel colour inside the
    window — that means the paint chain works; any downstream
    "capture doesn't see the overlay" result is about framebuffer
    vs. pixmap, not about whether the window has content.
  - If the window's own pixels are ALSO black, the paint step
    failed and the window never held sentinel-coloured pixels
    in the first place — we'd need to fix that before worrying
    about any capture.

Run inside whatever display the harness sets DISPLAY to.
"""

import array
import time

from Xlib import X, Xatom, display

from harness import (
    EXCLUDE_ATOM_NAME,
    OVERLAY_H,
    OVERLAY_W,
    OVERLAY_X,
    OVERLAY_Y,
    SENTINEL_RGB,
    detect_compositor,
)


def _pack_rgb(rgb):
    r, g, b = rgb
    return (r << 16) | (g << 8) | b


def main():
    dpy = display.Display()
    compositor = detect_compositor()
    screen = dpy.screen()
    root = screen.root
    bg = _pack_rgb(SENTINEL_RGB)

    win = root.create_window(
        OVERLAY_X, OVERLAY_Y, OVERLAY_W, OVERLAY_H, 0,
        screen.root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=bg,
        override_redirect=True,
        event_mask=X.ExposureMask,
    )

    exclude_atom = dpy.intern_atom(
        EXCLUDE_ATOM_NAME, only_if_exists=False)
    win.change_property(
        exclude_atom, Xatom.INTEGER, 32,
        array.array('i', [1]).tolist(),
        mode=X.PropModeReplace)

    win.map()
    dpy.sync()
    time.sleep(0.3)

    # Force-paint with three different primitives so we can tell
    # which one actually leaves pixels in the window.
    gc = win.create_gc(foreground=bg, background=bg)
    win.fill_rectangle(gc, 0, 0, OVERLAY_W, OVERLAY_H)
    dpy.flush()
    dpy.sync()
    time.sleep(0.3)

    # Check the window's own pixels via get_image. This is different
    # from reading the root framebuffer — a success here proves the
    # window has sentinel-coloured bytes even if the root capture
    # comes back empty.
    try:
        img = win.get_image(0, 0, OVERLAY_W, OVERLAY_H,
                            X.ZPixmap, 0xffffffff)
        raw = img.data
        if isinstance(raw, str):
            raw = raw.encode("latin1")
        # Sample the centre pixel. BGRX layout on 24-bit TrueColor.
        off = (OVERLAY_H // 2) * OVERLAY_W * 4 + (OVERLAY_W // 2) * 4
        b, g, r, x = raw[off], raw[off+1], raw[off+2], raw[off+3]
        print(f"window pixels via win.get_image (compositor={compositor}):")
        print(f"  centre pixel (BGRX)       = ({b}, {g}, {r}, {x})")
        print(f"  expected sentinel (R,G,B) = {SENTINEL_RGB}")
        print(f"  match? -> {(r, g, b) == SENTINEL_RGB}")
    except Exception as e:
        print(f"win.get_image raised: {e}")

    # Also check what the root reports at the same screen coordinates.
    try:
        rimg = root.get_image(
            OVERLAY_X + OVERLAY_W // 2,
            OVERLAY_Y + OVERLAY_H // 2,
            1, 1, X.ZPixmap, 0xffffffff)
        rraw = rimg.data
        if isinstance(rraw, str):
            rraw = rraw.encode("latin1")
        b, g, r, _ = rraw[0], rraw[1], rraw[2], rraw[3]
        print(f"root pixel at ({OVERLAY_X + OVERLAY_W // 2}, "
              f"{OVERLAY_Y + OVERLAY_H // 2}) = ({r}, {g}, {b})")
    except Exception as e:
        print(f"root.get_image raised: {e}")

    dpy.close()


if __name__ == "__main__":
    main()

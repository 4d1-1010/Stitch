#!/usr/bin/env python3
"""
Q3 — multi-window stacking order.

02_mini_compositor.py proved the *mechanism*: the mini-compositor
walks root's children, skips tagged windows, composes the rest.
Single overlay on its own only stresses the happy path though; Q3
adds pressure on z-order correctness under a realistic scenario
that matches UnIO's actual overlay model:

  A  (bottom, untagged)  — a background app  → blue
  B  (middle, untagged)  — a foreground app  → yellow
  C  (top, TAGGED)       — the UnIO overlay  → magenta sentinel

Stacking goes bottom-to-top A → B → C. All three overlap
geometrically inside the same region.

Naive framebuffer grab should see (pick a pixel covered by all
three):
  magenta at the centre (C on top), yellow where only A+B overlap
  but C doesn't cover, blue where only A covers.

Mini-compositor output should see:
  NO magenta anywhere — C is excluded.
  Yellow at the centre — B correctly composited ON TOP OF A after
  skipping C. This is the actual stacking-order assertion: if we
  walked top-to-bottom, or painted A last instead of first, we'd
  see blue at the centre instead of yellow.
  Blue only in the parts of A that B doesn't cover.

This mirrors UnIO in production — the overlay is always the
topmost window, and the capture needs to reconstruct what the
user's actual apps would be composing underneath so the remote
side gets a faithful view of "this user's desktop without the
overlay painted on top".
"""

import array
import sys
import time

from Xlib import X, Xatom, display

from harness import (
    EXCLUDE_ATOM_NAME,
    SENTINEL_RGB,
    capture_root_naive,
    capture_via_per_window_composite,
    detect_compositor,
    save_artefact,
    scan_for_sentinel,
)


# Three overlapping rectangles. All inside a 1280x800 Xephyr (or
# wherever the outer screen lands) — coords chosen so every
# pair-wise overlap region is non-empty, which is what makes the
# stacking assertion meaningful.
A_RECT = (100, 100, 500, 400)   # blue,    untagged  — bottom
B_RECT = (200, 200, 300, 250)   # yellow,  untagged  — middle
C_RECT = (250, 250, 200, 150)   # magenta, TAGGED    — top

A_RGB = (0x30, 0x80, 0xFF)       # saturated blue
B_RGB = (0xFF, 0xE0, 0x10)       # saturated yellow
C_RGB = SENTINEL_RGB             # the spike's magenta sentinel

# Sampling points for the stacking-order assertion. Chosen so each
# point exercises a specific z-order combination:
#   inside all three    → naive sees magenta (C), mini sees yellow (B)
#   A + B only          → both see yellow
#   A only              → both see blue
#   outside everything  → both see root background (grey50 under
#                         Xephyr harness, otherwise DE wallpaper)
INSIDE_ALL    = (C_RECT[0] + C_RECT[2] // 2, C_RECT[1] + C_RECT[3] // 2)
# Inside A and B, outside C. B's top-left corner is (200, 200) —
# pick a point inside B but before C (250, 250) starts.
A_AND_B_ONLY  = (B_RECT[0] + 20,              B_RECT[1] + 20)
A_ONLY        = (A_RECT[0] + 30,              A_RECT[1] + 30)


def _pack_rgb(rgb):
    r, g, b = rgb
    return (r << 16) | (g << 8) | b


def _make_window(dpy, rect, rgb, tag_it):
    """Create + map an override-redirect window painted a solid
    colour. Sets the exclude property iff `tag_it` is True."""
    x, y, w, h = rect
    screen = dpy.screen()
    root = screen.root
    bg = _pack_rgb(rgb)
    win = root.create_window(
        x, y, w, h, 0,
        screen.root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=bg,
        override_redirect=True,
        event_mask=X.ExposureMask,
    )
    if tag_it:
        atom = dpy.intern_atom(EXCLUDE_ATOM_NAME, only_if_exists=False)
        win.change_property(
            atom, Xatom.INTEGER, 32,
            array.array('i', [1]).tolist(),
            mode=X.PropModeReplace)
    win.map()
    dpy.sync()
    # Explicit paint so bare-X11 (no compositor) doesn't skip the
    # repaint pass. Same pattern as 02.
    gc = win.create_gc(foreground=bg, background=bg)
    win.fill_rectangle(gc, 0, 0, w, h)
    gc.free()
    dpy.flush()
    return win


def _sample(img, point, label):
    px = img.getpixel(point)
    return f"{label}={point}:{px}"


def main():
    dpy = display.Display()
    compositor = detect_compositor()

    # Create bottom-to-top: the X server's stacking order for
    # override_redirect children of root is "later map() wins the
    # top slot", which matches how query_tree returns them
    # bottom-to-top — which is the order our mini-compositor
    # iterates, which is what the stacking assertion depends on.
    a = _make_window(dpy, A_RECT, A_RGB, tag_it=False)
    b = _make_window(dpy, B_RECT, B_RGB, tag_it=False)
    c = _make_window(dpy, C_RECT, C_RGB, tag_it=True)
    dpy.sync()
    time.sleep(0.5)

    naive = capture_root_naive(dpy)
    mini  = capture_via_per_window_composite(dpy)

    # Scan for sentinel ONLY inside C's rectangle. On the outer
    # desktop session there are dozens of unrelated apps whose
    # framebuffers happen to contain a handful of magenta pixels
    # matching the sentinel exactly; a whole-image scan picks
    # them up as "leaks" even though they have nothing to do with
    # our overlay. The actual guarantee we care about is: inside
    # the tagged overlay's own rectangle, the mini-compositor
    # output must not carry the overlay's color.
    mini_found, mini_count = scan_for_sentinel(mini, region=C_RECT)
    naive_out = save_artefact(naive, "03_stacking_order_naive.png")
    mini_out  = save_artefact(mini,  "03_stacking_order_mini.png")

    # Raw sample-point dump for eyeballing.
    print(f"compositor = {compositor}")
    print(f"rects: A={A_RECT} blue  B={B_RECT} yellow  "
          f"C={C_RECT} magenta(tagged)")
    print("naive framebuffer grab (baseline):")
    print(f"  {_sample(naive, INSIDE_ALL,   'inside-all')}")
    print(f"  {_sample(naive, A_AND_B_ONLY, 'A+B-only')}")
    print(f"  {_sample(naive, A_ONLY,       'A-only')}")
    print("mini-compositor output:")
    print(f"  {_sample(mini, INSIDE_ALL,   'inside-all')}")
    print(f"  {_sample(mini, A_AND_B_ONLY, 'A+B-only')}")
    print(f"  {_sample(mini, A_ONLY,       'A-only')}")
    print(f"  sentinel_pixels_in_mini = {mini_count}")
    print(f"  artefacts: {naive_out.name}, {mini_out.name}")

    # Assertions. Exact-match is safe because the windows are solid
    # fills at our known RGB values; compositors don't gamma-shift
    # a plain background-pixel fill.
    checks = []

    # Q1 restated under three-window load: no sentinel leaks.
    checks.append(("Q1 no magenta in mini",
                   mini_count == 0))

    # Q3 proper: the stacking-order assertion.
    # At the centre where all three overlap, naive sees magenta (C
    # on top); mini should see YELLOW (B correctly composited on
    # top of A, after C was skipped). Blue there would mean the
    # compositor painted A AFTER B, i.e. got z-order backwards.
    checks.append(("Q3 stacking — inside-all shows yellow in mini",
                   mini.getpixel(INSIDE_ALL) == B_RGB))

    # A+B-only region (covered by both, not by C): both captures
    # should agree on yellow. Bonus: confirms B's naive fill
    # actually took and that mini-compositor doesn't tint it.
    checks.append(("Q3 stacking — A+B-only shows yellow in mini",
                   mini.getpixel(A_AND_B_ONLY) == B_RGB))

    # A-only region (outside B and C): blue in both.
    checks.append(("A-only region shows blue in mini",
                   mini.getpixel(A_ONLY) == A_RGB))

    all_pass = all(ok for _, ok in checks)
    print()
    for name, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print()
    print(f"VERDICT: {'PASS' if all_pass else 'FAIL'} on {compositor}")

    for w in (c, b, a):
        try:
            w.destroy()
        except Exception:
            pass
    dpy.close()
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()

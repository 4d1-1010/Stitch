#!/usr/bin/env python3
"""
Strategy 02 — custom property + mini-compositor capture.

Strategy 01 demonstrated that the property signal is correct. This
experiment keeps the signal the same but promotes the *mechanism*
from client-side post-processing to compositor-level selection:
instead of grabbing the framebuffer and zeroing out tagged
rectangles, we build the output frame ourselves from each window's
redirected offscreen pixmap, skipping tagged windows during the
walk.

Why this is better than the post-process zero-out of strategy 01:
  * The pixels where the overlay used to be are genuinely the
    content of the windows underneath. No black hole in the
    streamed frame.
  * No per-frame framebuffer grab *then* iterate-and-mask. One walk
    across root's direct children does both the skip check and
    the composition.
  * Matches the architecture we'd want for production: this is
    what `capture_xcomposite.cpp` would look like if it actually
    used the XComposite offscreen pixmaps its filename implies,
    instead of the current `XShmGetImage(dpy, root, ...)`
    framebuffer shortcut.

Why this isn't XCompositeUnredirectWindow:
  * Unredirect REMOVES the window from the compositing system.
    The X server stops keeping an offscreen pixmap for it and
    paints it directly to the screen. Our spike needs each window
    to stay redirected so we can walk + composite — unredirect
    defeats the entire mini-compositor approach.
  * Also: Unredirect requires the caller to have previously called
    RedirectWindow on the same window, which under Mutter/KWin we
    haven't (the compositor holds the redirect). BadValue every
    time.

Expected verdicts on an X11 session with an active compositor:
  * naive (XGetImage on root, what capture_xcomposite.cpp does now)
    — VISIBLE. Confirms the overlay is on screen.
  * mini-compositor (walk root's children, skip tagged) — HIDDEN.
    Confirms the property-aware composite pass excludes the
    overlay end-to-end.
"""

import array
import sys
import time

from Xlib import X, Xatom, display

from harness import (
    EXCLUDE_ATOM_NAME,
    OVERLAY_H,
    OVERLAY_W,
    OVERLAY_X,
    OVERLAY_Y,
    SENTINEL_RGB,
    capture_root_naive,
    capture_via_per_window_composite,
    detect_compositor,
    save_artefact,
    scan_for_sentinel,
)


def _pack_rgb(rgb):
    r, g, b = rgb
    return (r << 16) | (g << 8) | b


def _make_overlay(dpy):
    screen = dpy.screen()
    root = screen.root
    win = root.create_window(
        OVERLAY_X, OVERLAY_Y, OVERLAY_W, OVERLAY_H, 0,
        screen.root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=_pack_rgb(SENTINEL_RGB),
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
    return win


def main():
    dpy = display.Display()
    compositor = detect_compositor()

    win = _make_overlay(dpy)
    dpy.sync()
    # Let the compositor draw the overlay into its offscreen pixmap
    # before we start walking. Without the settle, the window's
    # offscreen backing can still be empty on the first frame.
    time.sleep(0.5)

    naive = capture_root_naive(dpy)
    mini  = capture_via_per_window_composite(dpy)

    naive_found, naive_count = scan_for_sentinel(naive)
    mini_found,  mini_count  = scan_for_sentinel(mini)

    naive_verdict = "VISIBLE" if naive_found else "HIDDEN"
    mini_verdict  = "VISIBLE" if mini_found  else "HIDDEN"
    naive_out = save_artefact(naive, "02_mini_compositor_naive.png")
    mini_out  = save_artefact(mini,  "02_mini_compositor_output.png")

    print(f"RESULT naive (framebuffer grab): {naive_verdict:<8} "
          f"(sentinel_pixels={naive_count}, compositor={compositor}, "
          f"capture={naive.size[0]}x{naive.size[1]}, "
          f"artefact={naive_out.name})")
    print(f"RESULT mini-compositor:          {mini_verdict:<8} "
          f"(sentinel_pixels={mini_count}, compositor={compositor}, "
          f"capture={mini.size[0]}x{mini.size[1]}, "
          f"artefact={mini_out.name})")

    # Validation Q2 (per the design discussion): when exclusion
    # truly replaced the overlay with windows-underneath pixels,
    # the overlay region in the mini-compositor output should
    # carry real RGB — not pure black (which would mean "the
    # property-aware composite silently left a hole and the test
    # still passed because sentinel_count == 0"). Sample the
    # centre + corners of the overlay rectangle and report what
    # we got. Operator eyeballs whether it looks plausible.
    samples = [
        (OVERLAY_X + OVERLAY_W // 2, OVERLAY_Y + OVERLAY_H // 2),
        (OVERLAY_X + 5,              OVERLAY_Y + 5),
        (OVERLAY_X + OVERLAY_W - 5,  OVERLAY_Y + OVERLAY_H - 5),
    ]
    naive_samples = [naive.getpixel(p) for p in samples]
    mini_samples  = [mini.getpixel(p)  for p in samples]
    print("VALIDATION Q2 (pixels-underneath): "
          "sampling inside the overlay rectangle in BOTH captures")
    for (x, y), n, m in zip(samples, naive_samples, mini_samples):
        print(f"  ({x},{y}): naive={n}, mini={m}")
    # Naive samples will be the sentinel colour (overlay was
    # painted there). Mini samples should be anything BUT the
    # sentinel — ideally real desktop content. Pure black is a
    # weak smell (possible black-hole artefact), but on a fresh
    # session over empty wallpaper it can be legitimately black —
    # hence "operator eyeballs" rather than a hard assertion.

    if naive_verdict == "VISIBLE" and mini_verdict == "HIDDEN":
        print(f"VERDICT: PASS — property-driven mini-compositor "
              f"excludes the overlay on {compositor}")
        rc = 0
    else:
        print(f"VERDICT: FAIL — expected (VISIBLE, HIDDEN), got "
              f"({naive_verdict}, {mini_verdict}) on {compositor}")
        rc = 1

    try:
        win.destroy()
    except Exception:
        pass
    dpy.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Strategy 01 — custom X11 property tags the overlay; capture respects
the tag.

Round trip:
  1. Create an overlay window painted solid magenta (SENTINEL_RGB).
  2. Set `UNIO_CAPTURE_EXCLUDE=1` on the overlay via
     `XChangeProperty`.
  3. Capture with the naive path (what capture_xcomposite.cpp does
     today) — expect VISIBLE (the overlay IS on screen; if this
     reads HIDDEN something's wrong with the setup).
  4. Capture with the property-aware path — expect HIDDEN. This is
     the protocol proof: the custom property carries the exclusion
     signal end-to-end.
  5. Verdict: PASS if (naive, property) == (VISIBLE, HIDDEN).

PASS on Mutter / KWin / xfwm / no-compositor = the property protocol
is the strategy to fold into `capture_xcomposite.cpp`.
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
    capture_root_property_aware,
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

    # The whole point of this spike: set a custom 32-bit INTEGER
    # property on the overlay window. Any capture path that knows the
    # atom name reads this and skips the window in its composition.
    exclude_atom = dpy.intern_atom(
        EXCLUDE_ATOM_NAME, only_if_exists=False)
    # python-xlib wants the 32-bit INTEGER value packed as a sequence
    # of ints; array.array('i', [1]) matches the C path exactly
    # (XA_INTEGER, format 32, 1 element, value 1).
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
    # Small settle wait so the compositor finishes its first repaint
    # cycle. Without it the first capture can fire before the overlay
    # lands in the composited root pixmap.
    time.sleep(0.5)

    naive = capture_root_naive(dpy)
    prop  = capture_root_property_aware(dpy)

    naive_found, naive_count = scan_for_sentinel(naive)
    prop_found,  prop_count  = scan_for_sentinel(prop)

    naive_verdict = "VISIBLE" if naive_found else "HIDDEN"
    prop_verdict  = "VISIBLE" if prop_found  else "HIDDEN"
    naive_out = save_artefact(naive, "01_property_exclude_naive.png")
    prop_out  = save_artefact(prop,  "01_property_exclude_property-aware.png")

    print(f"RESULT naive:    {naive_verdict:<8} "
          f"(sentinel_pixels={naive_count}, compositor={compositor}, "
          f"capture={naive.size[0]}x{naive.size[1]}, "
          f"artefact={naive_out.name})")
    print(f"RESULT property: {prop_verdict:<8} "
          f"(sentinel_pixels={prop_count}, compositor={compositor}, "
          f"capture={prop.size[0]}x{prop.size[1]}, "
          f"artefact={prop_out.name})")

    # Winning combo = naive sees the overlay, property-aware doesn't.
    # Any other combo means the spike didn't prove what we wanted.
    if naive_verdict == "VISIBLE" and prop_verdict == "HIDDEN":
        print(f"VERDICT: PASS — custom-property exclusion works on "
              f"{compositor}")
        rc = 0
    else:
        print(f"VERDICT: FAIL — expected (VISIBLE, HIDDEN), got "
              f"({naive_verdict}, {prop_verdict}) on {compositor}")
        rc = 1

    try:
        win.destroy()
    except Exception:
        pass
    dpy.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()

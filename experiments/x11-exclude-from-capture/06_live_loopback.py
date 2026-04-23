#!/usr/bin/env python3
"""
Strategy 01 live demo — a box on the desktop that shows the desktop
being captured in real time.

Without --strategy-01:
  The box's capture includes the box itself → tunnel recursion. After
  a few frames the nested-picture-in-picture-in-picture pattern is
  visible. Matches your bidirectional-swap feedback scenario.

With --strategy-01:
  The box is tagged with UNIO_CAPTURE_EXCLUDE=1. The capture loop
  reads the tag on each of root's children, zeros out the box's
  rectangle before rendering, and the box shows the clean desktop
  with a black square where it sits. No recursion.

Run side-by-side:
  # tunnel mode (no exclusion):
  .venv/bin/python 06_live_loopback.py

  # strategy 01 (with exclusion):
  .venv/bin/python 06_live_loopback.py --strategy-01

Press Ctrl-C in the launching terminal to quit.
"""

from __future__ import annotations

import argparse
import array
import sys
import time

import numpy as np
from PIL import Image

from Xlib import X, Xatom, display
from Xlib.ext import xinerama


EXCLUDE_ATOM_NAME = "UNIO_CAPTURE_EXCLUDE"

BOX_W, BOX_H = 640, 360
BOX_X, BOX_Y = 100, 100

FPS = 20


def _pack_rgb(r: int, g: int, b: int) -> int:
    return (r << 16) | (g << 8) | b


def _extract_int(value) -> int | None:
    try:
        return int(value[0])
    except Exception:
        pass
    try:
        if isinstance(value, (bytes, bytearray)) and len(value) >= 4:
            return int.from_bytes(bytes(value[:4]), "little", signed=True)
    except Exception:
        pass
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--strategy-01", action="store_true",
        help="Tag the preview box with UNIO_CAPTURE_EXCLUDE=1 and "
             "zero out tagged windows during capture. Off = tunnel "
             "recursion demo. On = clean capture with black square.")
    parser.add_argument(
        "--monitor", type=int, default=None,
        help="Xinerama monitor index to capture (default: the one "
             "that contains the box's top-left corner).")
    args = parser.parse_args()

    dpy = display.Display()
    screen = dpy.screen()
    root = screen.root

    # Pick a single physical monitor to capture, not the full root
    # framebuffer (which on adi-pc is 5760x1169 across three
    # side-by-side monitors — squishing that into a 640x360 preview
    # is a funhouse mirror). Xinerama gives us per-monitor
    # rectangles. Default: the monitor that contains the box's
    # top-left corner. --monitor N selects by index.
    mon_rects = []
    try:
        if dpy.has_extension("XINERAMA"):
            xinerama_info = xinerama.query_screens(dpy)
            for s in xinerama_info.screens:
                mon_rects.append(
                    (int(s.x), int(s.y),
                     int(s.width), int(s.height)))
    except Exception:
        pass
    if not mon_rects:
        # Fallback: single logical screen covers the whole root
        # framebuffer. Matches bare X11 / no-Xinerama setups.
        mon_rects.append(
            (0, 0, screen.width_in_pixels, screen.height_in_pixels))

    if args.monitor is not None:
        mon_idx = max(0, min(args.monitor, len(mon_rects) - 1))
    else:
        mon_idx = 0
        for i, (mx, my, mw, mh) in enumerate(mon_rects):
            if mx <= BOX_X < mx + mw and my <= BOX_Y < my + mh:
                mon_idx = i
                break

    MON_X, MON_Y, sw, sh = mon_rects[mon_idx]
    print(f"capturing monitor {mon_idx}: "
          f"{sw}x{sh}+{MON_X}+{MON_Y} "
          f"(of {len(mon_rects)} detected)")

    # The preview box. override_redirect so no WM decorations get in
    # the way of the demo; the capture loop sees it as a direct child
    # of root.
    win = root.create_window(
        BOX_X, BOX_Y, BOX_W, BOX_H, 0,
        screen.root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=_pack_rgb(0, 0, 0),
        override_redirect=True,
        event_mask=X.ExposureMask,
    )

    exclude_atom = dpy.intern_atom(
        EXCLUDE_ATOM_NAME, only_if_exists=False)

    if args.strategy_01:
        win.change_property(
            exclude_atom, Xatom.INTEGER, 32,
            array.array('i', [1]).tolist(),
            mode=X.PropModeReplace)
        print(f"strategy-01 ON — box tagged with "
              f"{EXCLUDE_ATOM_NAME}=1; capture loop will zero out "
              f"tagged rectangles.")
    else:
        print("strategy-01 OFF — capture includes the box; expect "
              "tunnel recursion.")

    win.map()
    dpy.sync()
    time.sleep(0.2)
    gc = win.create_gc()

    print(f"Box is {BOX_W}×{BOX_H} at ({BOX_X}, {BOX_Y}). "
          f"Screen is {sw}×{sh}. Target {FPS} fps. Ctrl-C to quit.")

    frame_interval = 1.0 / FPS
    next_frame = time.monotonic()
    frame_count = 0
    try:
        while True:
            now = time.monotonic()
            if now < next_frame:
                time.sleep(next_frame - now)
            next_frame += frame_interval

            # Capture the chosen monitor's rectangle of the root
            # framebuffer. Same primitive capture_xcomposite.cpp
            # uses in production (XShmGetImage → XGetImage in the
            # python bindings), but scoped to one monitor so the
            # preview isn't a three-monitor smush.
            img = root.get_image(
                MON_X, MON_Y, sw, sh, X.ZPixmap, 0xffffffff)
            raw = img.data
            if isinstance(raw, str):
                raw = raw.encode("latin1")
            # BGRX layout → numpy for fast manipulation.
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(sh, sw, 4)

            if args.strategy_01:
                # Walk root's direct children. Any mapped window
                # carrying UNIO_CAPTURE_EXCLUDE=1 gets its rectangle
                # zeroed out in the captured array. This is the
                # *entire* 10-line algorithm strategy 01 asks us to
                # put into production capture_xcomposite.cpp.
                #
                # Coordinates: geom.x / geom.y are in ROOT space,
                # but the captured array is in MONITOR-LOCAL space
                # (top-left = MON_X,MON_Y on root). Translate
                # before intersecting.
                tree = root.query_tree()
                for child in tree.children:
                    try:
                        attrs = child.get_attributes()
                        if attrs.map_state != X.IsViewable:
                            continue
                        prop = child.get_full_property(
                            exclude_atom, X.AnyPropertyType)
                        if prop is None:
                            continue
                        if _extract_int(prop.value) != 1:
                            continue
                        geom = child.get_geometry()
                        # Root-space child rect
                        rx0 = int(geom.x)
                        ry0 = int(geom.y)
                        rx1 = rx0 + int(geom.width)
                        ry1 = ry0 + int(geom.height)
                        # Translate into the captured monitor's
                        # local coordinates, then clamp to the
                        # captured buffer.
                        lx0 = max(0, rx0 - MON_X)
                        ly0 = max(0, ry0 - MON_Y)
                        lx1 = min(sw, rx1 - MON_X)
                        ly1 = min(sh, ry1 - MON_Y)
                        if lx1 > lx0 and ly1 > ly0:
                            arr[ly0:ly1, lx0:lx1, :] = 0
                    except Exception:
                        continue

            # BGRX → RGB for Pillow (drop X byte, reverse channel
            # order).
            pil = Image.fromarray(arr[:, :, :3][:, :, ::-1], mode="RGB")

            # Aspect-preserving fit into the box, then centre on a
            # black canvas. Raw resize to (BOX_W, BOX_H) squishes a
            # 5760×1169 triple-monitor framebuffer into 16:9 and
            # looks like a funhouse mirror. thumbnail() respects the
            # input aspect ratio, and paste() centres the result.
            fit_w = BOX_W
            fit_h = int(BOX_W * sh / sw)
            if fit_h > BOX_H:
                fit_h = BOX_H
                fit_w = int(BOX_H * sw / sh)
            fit = pil.resize((fit_w, fit_h), Image.BILINEAR)
            scaled = Image.new("RGB", (BOX_W, BOX_H), (0, 0, 0))
            scaled.paste(fit,
                         ((BOX_W - fit_w) // 2,
                          (BOX_H - fit_h) // 2))

            # Back to BGRX for put_image on a 24-bit screen.
            scaled_arr = np.array(scaled, dtype=np.uint8)
            out = np.zeros((BOX_H, BOX_W, 4), dtype=np.uint8)
            out[..., 0] = scaled_arr[..., 2]  # B
            out[..., 1] = scaled_arr[..., 1]  # G
            out[..., 2] = scaled_arr[..., 0]  # R
            # out[..., 3] stays 0 (X byte).

            # python-xlib's PutImage encodes the data length in a
            # uint16 field, so the per-request payload maxes out at
            # 65535 bytes. 640*360*4 = 921,600, well above. Send the
            # frame as a stack of horizontal strips, each sized to
            # fit. 24 rows × 640 × 4 = 61,440 bytes — comfortable.
            strip_rows = 24
            for y0 in range(0, BOX_H, strip_rows):
                y1 = min(BOX_H, y0 + strip_rows)
                win.put_image(
                    gc, 0, y0, BOX_W, y1 - y0,
                    X.ZPixmap, screen.root_depth, 0,
                    out[y0:y1, :, :].tobytes())
            dpy.flush()

            frame_count += 1
            if frame_count == 1:
                print("first frame rendered")
            # Keep the event queue drained so X doesn't throttle us.
            while dpy.pending_events():
                dpy.next_event()
    except KeyboardInterrupt:
        print("\nexiting")
    finally:
        try:
            win.destroy()
        except Exception:
            pass
        dpy.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())

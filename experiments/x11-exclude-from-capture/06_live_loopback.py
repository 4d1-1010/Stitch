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
import sys
import time

import numpy as np
from PIL import Image

from Xlib import X, display
from Xlib.ext import xinerama

from harness import (
    EXCLUDE_ATOM_NAME,
    WM_CLASS_INSTANCE,
    is_unio_overlay,
    tag_as_unio_overlay,
)


BOX_W, BOX_H = 640, 360
BOX_X, BOX_Y = 100, 100

FPS = 20


def _pack_rgb(r: int, g: int, b: int) -> int:
    return (r << 16) | (g << 8) | b


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode",
        choices=("naive", "zero-out", "tree-walk", "hybrid"),
        default="naive",
        help="Capture strategy:\n"
             "  naive     — no exclusion; tunnel recursion.\n"
             "  zero-out  — zero out the tagged rectangle.\n"
             "              Black square, works everywhere.\n"
             "  tree-walk — rebuild the whole frame from root's\n"
             "              children, skipping tagged. Overlay\n"
             "              genuinely absent, but Mutter chrome\n"
             "              (top bar / dock) is missing because\n"
             "              it isn't an X11 window.\n"
             "  hybrid    — framebuffer grab as the base, then\n"
             "              replace ONLY the tagged rectangles\n"
             "              with tree-walk output. Chrome is\n"
             "              preserved everywhere the overlay\n"
             "              isn't. Best of both when the overlay\n"
             "              is partial; equivalent to tree-walk\n"
             "              when the overlay is fullscreen.")
    parser.add_argument(
        "--strategy-01", action="store_true",
        help="Deprecated alias for --mode zero-out.")
    parser.add_argument(
        "--monitor", type=int, default=None,
        help="Xinerama monitor index to capture (default: the one "
             "that contains the box's top-left corner).")
    parser.add_argument(
        "--duration", type=float, default=10.0,
        help="How long to run before exiting (seconds). Default 10.")
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

    # Back-compat: --strategy-01 is an alias for --mode zero-out.
    if args.strategy_01 and args.mode == "naive":
        args.mode = "zero-out"

    tag_box = args.mode in ("zero-out", "tree-walk", "hybrid")
    if tag_box:
        tag_as_unio_overlay(dpy, win)
        print(f"mode={args.mode} — box tagged as UnIO overlay "
              f"(UNIO_CAPTURE_EXCLUDE=1 + WM_CLASS={WM_CLASS_INSTANCE!r} "
              f"+ _NET_WM_WINDOW_TYPE_NOTIFICATION + state flags).")
    else:
        print(f"mode={args.mode} — no exclusion; capture includes "
              f"the box; expect tunnel recursion.")

    win.map()
    dpy.sync()
    time.sleep(0.2)
    gc = win.create_gc()

    print(f"Box is {BOX_W}×{BOX_H} at ({BOX_X}, {BOX_Y}), "
          f"window id={hex(win.id)}. "
          f"Screen is {sw}×{sh}. Target {FPS} fps. "
          f"Running for {args.duration:.1f}s (Ctrl-C to quit sooner).")

    frame_interval = 1.0 / FPS
    next_frame = time.monotonic()
    start_time = time.monotonic()
    frame_count = 0
    try:
        while True:
            now = time.monotonic()
            if args.duration > 0 and (now - start_time) >= args.duration:
                print(f"duration {args.duration:.1f}s elapsed, exiting")
                break
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
            # BGRX layout → numpy for fast manipulation. `frombuffer`
            # returns a READ-ONLY view when the underlying buffer is
            # immutable (python-xlib's image.data is). `.copy()`
            # gives us a writable array we can zero rectangles into.
            arr = (np.frombuffer(raw, dtype=np.uint8)
                     .reshape(sh, sw, 4)
                     .copy())

            if args.mode in ("tree-walk", "hybrid"):
                # Walk root's children (bottom-to-top stacking
                # order), composite each non-tagged mapped
                # InputOutput window's offscreen pixmap into a
                # canvas. For `tree-walk` the canvas starts black;
                # the overlay's rectangle fills in with whatever
                # windows happened to be behind it, but regions
                # painted by the compositor outside the X11 tree
                # (Mutter top bar / dock) stay black. For `hybrid`
                # the canvas starts as a copy of the framebuffer
                # grab — chrome is preserved everywhere the
                # overlay doesn't cover — then only the tagged
                # rectangles are replaced with tree-walk content
                # (what's genuinely behind the overlay).
                from harness import _name_window_pixmap
                if args.mode == "hybrid":
                    tw_canvas = np.zeros_like(arr)
                else:
                    tw_canvas = np.zeros_like(arr)
                try:
                    tree = root.query_tree()
                except Exception:
                    tree = None
                composited_matched = 0
                composited_skipped = 0
                composited_failed = 0
                first_fail_msgs = []
                tagged_rects = []  # monitor-local (lx0, ly0, lx1, ly1)
                if tree is not None:
                    for child in tree.children:
                        try:
                            attrs = child.get_attributes()
                            if attrs.map_state != X.IsViewable:
                                continue
                            if is_unio_overlay(dpy, child):
                                composited_skipped += 1
                                # Remember the tagged geometry so
                                # hybrid knows which rectangles to
                                # replace from the tree-walk
                                # canvas.
                                try:
                                    tg = child.get_geometry()
                                    trx0 = int(tg.x)
                                    try0 = int(tg.y)
                                    trx1 = trx0 + int(tg.width)
                                    try1 = try0 + int(tg.height)
                                    tlx0 = max(0, trx0 - MON_X)
                                    tly0 = max(0, try0 - MON_Y)
                                    tlx1 = min(sw, trx1 - MON_X)
                                    tly1 = min(sh, try1 - MON_Y)
                                    if tlx1 > tlx0 and tly1 > tly0:
                                        tagged_rects.append(
                                            (tlx0, tly0, tlx1, tly1))
                                except Exception:
                                    pass
                                continue
                            geom = child.get_geometry()
                            rx0 = int(geom.x)
                            ry0 = int(geom.y)
                            cw  = int(geom.width)
                            ch  = int(geom.height)
                            depth = int(geom.depth)
                            if cw <= 0 or ch <= 0:
                                continue
                            if depth not in (24, 32):
                                continue
                            pix_xid = _name_window_pixmap(int(child.id))
                            if pix_xid is not None:
                                pix_obj = dpy.create_resource_object(
                                    "pixmap", pix_xid)
                                src = pix_obj.get_image(
                                    0, 0, cw, ch,
                                    X.ZPixmap, 0xffffffff)
                            else:
                                src = child.get_image(
                                    0, 0, cw, ch, X.ZPixmap, 0xffffffff)
                            sraw = src.data
                            if isinstance(sraw, str):
                                sraw = sraw.encode("latin1")
                            if depth == 24:
                                src_arr = (np.frombuffer(sraw, dtype=np.uint8)
                                             .reshape(ch, cw, 4))
                            else:
                                src_arr = (np.frombuffer(sraw, dtype=np.uint8)
                                             .reshape(ch, cw, 4))
                            # Translate into the captured monitor's
                            # local coords, clip to canvas bounds.
                            lx0 = max(0, rx0 - MON_X)
                            ly0 = max(0, ry0 - MON_Y)
                            lx1 = min(sw, rx0 - MON_X + cw)
                            ly1 = min(sh, ry0 - MON_Y + ch)
                            sx0 = lx0 - (rx0 - MON_X)
                            sy0 = ly0 - (ry0 - MON_Y)
                            if lx1 > lx0 and ly1 > ly0:
                                tw_canvas[ly0:ly1, lx0:lx1, :] = \
                                    src_arr[sy0:sy0+(ly1-ly0),
                                            sx0:sx0+(lx1-lx0), :]
                                composited_matched += 1
                        except Exception as e:
                            composited_failed += 1
                            if frame_count == 0 and len(first_fail_msgs) < 5:
                                first_fail_msgs.append(
                                    f"child={hex(child.id)} {type(e).__name__}: {e}")
                            continue
                if args.mode == "tree-walk":
                    # Output = canvas we just built. Chrome outside
                    # the X11 window tree is missing; that's the
                    # documented limitation of this mode.
                    arr = tw_canvas
                else:  # hybrid
                    # Output = framebuffer (preserves chrome
                    # everywhere), EXCEPT inside each tagged
                    # rectangle, which is replaced with tree-walk
                    # content (what's genuinely behind the overlay).
                    for (lx0, ly0, lx1, ly1) in tagged_rects:
                        arr[ly0:ly1, lx0:lx1, :] = \
                            tw_canvas[ly0:ly1, lx0:lx1, :]

                if frame_count == 0:
                    print(f"DEBUG: {args.mode} composited "
                          f"{composited_matched} windows, skipped "
                          f"{composited_skipped} tagged, "
                          f"{composited_failed} failed; "
                          f"tagged_rects={tagged_rects}.")
                    for msg in first_fail_msgs:
                        print(f"DEBUG:   fail: {msg}")

            elif args.mode == "zero-out":
                # Walk root's direct children. Any mapped window
                # that identifies as a UnIO overlay (via
                # `is_unio_overlay` — primary atom OR WM_CLASS
                # fallback) gets its rectangle zeroed out in the
                # captured array.
                tree = root.query_tree()
                matched_count = 0
                zeroed_count = 0
                first_match_debug = []
                for child in tree.children:
                    try:
                        attrs = child.get_attributes()
                        if attrs.map_state != X.IsViewable:
                            continue
                        if not is_unio_overlay(dpy, child):
                            continue
                        matched_count += 1
                        geom = child.get_geometry()
                        rx0 = int(geom.x)
                        ry0 = int(geom.y)
                        rx1 = rx0 + int(geom.width)
                        ry1 = ry0 + int(geom.height)
                        lx0 = max(0, rx0 - MON_X)
                        ly0 = max(0, ry0 - MON_Y)
                        lx1 = min(sw, rx1 - MON_X)
                        ly1 = min(sh, ry1 - MON_Y)
                        if frame_count == 0:
                            first_match_debug.append(
                                (hex(child.id),
                                 (rx0, ry0, int(geom.width), int(geom.height)),
                                 (lx0, ly0, lx1, ly1)))
                        if lx1 > lx0 and ly1 > ly0:
                            arr[ly0:ly1, lx0:lx1, :] = 0
                            zeroed_count += 1
                    except Exception as e:
                        if frame_count == 0:
                            first_match_debug.append(
                                (f"exception: {e}",))
                        continue
                if frame_count == 0:
                    print(f"DEBUG: root has {len(tree.children)} "
                          f"direct children; "
                          f"{matched_count} matched is_unio_overlay; "
                          f"{zeroed_count} rectangles zeroed.")
                    print(f"DEBUG: our own box id={hex(win.id)} "
                          f"expected geom=({BOX_X},{BOX_Y},{BOX_W},{BOX_H}) "
                          f"monitor offset=({MON_X},{MON_Y})")
                    for entry in first_match_debug:
                        print(f"DEBUG:   matched {entry}")

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

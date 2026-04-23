"""
Shared primitives for the X11 capture-exclude spike.

Provides:
- SENTINEL_RGB, OVERLAY_* constants — the distinctive colour + size
  every experiment uses so the harness can detect leaks reliably.
- EXCLUDE_ATOM_NAME — the custom X11 property every overlay sets on
  itself; every property-aware capture reads the same atom.
- capture_root_naive(dpy) — what capture_xcomposite.cpp does today.
- capture_root_property_aware(dpy, exclude_atom) — the proposed
  fix: after grabbing the composited root pixels, zero out every
  tagged window's rectangle.
- scan_for_sentinel(img) — exact-colour scan for the sentinel.
- detect_compositor() — best-effort tag for the result line.

Design notes on the capture helpers
-----------------------------------
Both helpers start from the same X11 primitive: `XGetImage` on the
root window. With XComposite's manual redirect in place (which the
desktop compositor keeps on for its own painting), this is the
composited output — precisely the stream of pixels UnIO's real
capture TU feeds into the encoder.

The property-aware variant then post-processes: walks each direct
child of the root, reads the `UNIO_CAPTURE_EXCLUDE` property via
`XGetWindowProperty`, and for every child whose value is 1 zeros
the pixels in the captured image inside that child's geometry.
That reproduces the effect a production implementation inside
`capture_xcomposite.cpp` would have — the difference in prod is
only *when* the skip happens (during composition vs after),
not whether the pixels make it into the encoded frame.

Earlier revisions of this harness also tried
`XCompositeNameWindowPixmap` for a root-level named pixmap that
would match the C TU more literally; python-xlib's wrapper for
that call returns a resource the XGetImage binding refuses with
`BadDrawable`, which looks like an ABI gap in the binding rather
than an X server issue (the C path works fine). Using
`root.get_image()` sidesteps the quirk without changing what
pixels we see.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import typing

from PIL import Image

from Xlib import X, Xatom, display
from Xlib.ext import composite


# The tag every overlay sets on itself, and the tag every
# property-aware capture path looks for. Production will use the same
# string; keeping it here as the single source of truth so experiment
# scripts and the real capture_xcomposite.cpp match exactly.
EXCLUDE_ATOM_NAME = "UNIO_CAPTURE_EXCLUDE"

# Sentinel colour chosen to be distinct from anything a typical
# desktop composes: saturated magenta-ish on a non-grey axis. If a
# leak exists the harness will see at least one of these exact
# pixels in the captured frame.
SENTINEL_RGB = (0xFF, 0x3F, 0x7F)
OVERLAY_W, OVERLAY_H = 200, 200
OVERLAY_X, OVERLAY_Y = 120, 120

ARTIFACTS_DIR = pathlib.Path(__file__).with_name("artifacts")


def detect_compositor() -> str:
    """Best-effort compositor detection for logging.

    Not load-bearing for correctness — the property-aware filter
    is compositor-agnostic — but useful for recording which session
    a PASS / FAIL came from.
    """
    env = os.environ
    if env.get("XDG_CURRENT_DESKTOP"):
        return env["XDG_CURRENT_DESKTOP"].lower()
    if env.get("DESKTOP_SESSION"):
        return env["DESKTOP_SESSION"].lower()
    try:
        r = subprocess.run(
            ["xprop", "-root", "_NET_WM_CM_S0"],
            capture_output=True, text=True, timeout=1)
        if "not found" not in r.stdout.lower():
            return "unknown-compositor"
    except Exception:
        pass
    return "none"


def _ensure_composite_extension(dpy: display.Display) -> None:
    if not dpy.has_extension("Composite"):
        print("FATAL: X server does not expose XComposite; this spike "
              "cannot run here.", file=sys.stderr)
        sys.exit(2)


def _get_root_image(dpy: display.Display) -> Image.Image:
    """`XGetImage` on the root window — the framebuffer read that
    `capture_xcomposite.cpp` does today via `XShmGetImage(dpy,
    root, …)`. Shared by both capture helpers so they start from
    the same raw pixels.

    Does NOT call `XCompositeRedirectSubwindows`. That's the
    running desktop compositor's job, and calling it ourselves has
    surprising semantics when no one else already holds the
    redirect — on Xephyr-bare and Xephyr+picom, taking the
    redirect ourselves made `root.get_image(...)` return an empty
    offscreen pixmap instead of the framebuffer, which broke the
    naive path for both scenarios. Under Mutter the call was
    silently BadAccess'd (Mutter already held manual redirect) so
    it didn't surface there. Cleanest fix: don't touch redirect
    state here — whichever compositor is running keeps its own.
    """
    root = dpy.screen().root
    geom = root.get_geometry()
    w, h = geom.width, geom.height
    img = root.get_image(0, 0, w, h, X.ZPixmap, 0xffffffff)
    raw = img.data
    if isinstance(raw, str):
        raw = raw.encode("latin1")
    pil = Image.frombytes("RGB", (w, h), raw, "raw", "BGRX")
    return pil


def capture_root_naive(dpy: display.Display) -> Image.Image:
    """What `capture_xcomposite.cpp` does today. The baseline
    broken-for-overlay behaviour — picks up every window on screen,
    including our tagged overlay."""
    return _get_root_image(dpy)


def capture_via_per_window_composite(
        dpy: display.Display,
        exclude_atom_name: str = EXCLUDE_ATOM_NAME,
        ) -> Image.Image:
    """The mini-compositor capture path.

    Instead of grabbing the framebuffer once (which includes every
    window the user sees, tagged or not), walk root's direct children
    in stacking order, read each one's redirected offscreen pixels
    via `XGetImage(window, ...)`, and composite onto a scratch image.
    Windows that carry `UNIO_CAPTURE_EXCLUDE=1` are *skipped entirely*
    during this pass — their pixmap never enters the output, and the
    windows behind them (also retrieved from their own offscreen
    pixmaps) show through correctly. No black hole, no post-process.

    This is what would replace the `XShmGetImage(dpy, root, ...)`
    call in production `capture_xcomposite.cpp`. The upside over the
    strategy-01 zero-out is that the excluded region is filled with
    genuine pixels from the windows underneath, not solid black.

    Assumptions baked in for the spike:
      * Only direct children of root are composited. Most desktops
        reparent application windows one level below root (under the
        WM's frame window); `query_tree` returns exactly those.
      * Only mapped (`IsViewable`) InputOutput windows contribute.
        InputOnly + unmapped windows have no pixels.
      * Composition order is bottom-to-top, matching the X stacking
        order `query_tree` returns.
      * Windows with depth 24 are read as BGRX, depth 32 as BGRA.
        Other depths are skipped (pathological 15/16-bit visuals
        aren't in scope).

    Production code would additionally: reuse pixmap allocations
    across frames, use XShmGetImage for zero-copy reads, handle
    cross-depth composition with proper alpha. The spike is a
    proof-of-signal, not a performance artefact.
    """
    screen = dpy.screen()
    root = screen.root
    sw = screen.width_in_pixels
    sh = screen.height_in_pixels
    exclude_atom = dpy.intern_atom(
        exclude_atom_name, only_if_exists=False)

    # Black canvas; every pixel gets overwritten by the bottom-most
    # mapped window that covers it (typically the root window's
    # wallpaper, drawn by the DE's background client).
    canvas = Image.new("RGB", (sw, sh), (0, 0, 0))

    try:
        tree = root.query_tree()
    except Exception as e:
        print(f"WARN: query_tree failed: {e}", file=sys.stderr)
        return canvas

    for child in tree.children:
        try:
            attrs = child.get_attributes()
        except Exception:
            continue
        if attrs.map_state != X.IsViewable:
            continue
        # InputOnly windows have no pixels and will fail the
        # `get_image` call further down — python-xlib's attribute
        # names vary across versions for the C `class` field (keyword
        # collision), so we let the exception handler below catch
        # those instead of probing the attribute name here.

        # The exclusion check — the whole point of the custom
        # property protocol. A tagged window is invisible to this
        # composite pass; whatever is underneath it (already blitted
        # earlier in the bottom-to-top walk) remains.
        try:
            prop = child.get_full_property(
                exclude_atom, X.AnyPropertyType)
            if prop is not None and _extract_first_int(prop.value) == 1:
                continue
        except Exception:
            pass  # property read failure is not a reason to skip

        try:
            geom = child.get_geometry()
        except Exception:
            continue
        x, y, w, h = int(geom.x), int(geom.y), int(geom.width), int(geom.height)
        depth = int(geom.depth)
        if w <= 0 or h <= 0:
            continue
        if depth not in (24, 32):
            continue

        # XGetImage on a redirected window returns its offscreen
        # backing — not the raw framebuffer slice at the window's
        # coordinates. That's the ingredient we need for a real
        # compositor pass: each window's *own* pixels regardless of
        # what's on top.
        try:
            img = child.get_image(
                0, 0, w, h, X.ZPixmap, 0xffffffff)
        except Exception:
            # Some server objects (e.g. freshly-unmapped, or
            # windows we don't have access to) fail the read.
            # Skip — this pass just gets imperfect, not wrong.
            continue
        raw = img.data
        if isinstance(raw, str):
            raw = raw.encode("latin1")

        if depth == 24:
            pil = Image.frombytes("RGB", (w, h), raw, "raw", "BGRX")
        else:
            pil = Image.frombytes(
                "RGBA", (w, h), raw, "raw", "BGRA")
            pil = pil.convert("RGB")

        # Clamp destination to the screen; windows positioned off
        # the edge (menu popups, dragging) would otherwise overflow.
        dst_x = max(x, 0)
        dst_y = max(y, 0)
        crop_x = dst_x - x
        crop_y = dst_y - y
        crop_w = min(w - crop_x, sw - dst_x)
        crop_h = min(h - crop_y, sh - dst_y)
        if crop_w <= 0 or crop_h <= 0:
            continue
        pil = pil.crop((crop_x, crop_y, crop_x + crop_w, crop_y + crop_h))
        canvas.paste(pil, (dst_x, dst_y))

    return canvas


def capture_root_property_aware(
        dpy: display.Display,
        exclude_atom_name: str = EXCLUDE_ATOM_NAME) -> Image.Image:
    """Capture, then zero out every tagged window's rectangle.

    Walks each direct child of the root and reads
    `exclude_atom_name` via `XGetWindowProperty`. When the property
    is present and its value is 1, the child's on-screen geometry
    gets filled with black in the captured image before we return.

    The simplest faithful implementation of the protocol. Prod may
    choose a more elaborate composition (fill with what was behind
    the overlay, via per-window offscreen pixmaps) but the signal
    — tagged windows don't reach the encoded frame — is the same.
    """
    img = _get_root_image(dpy)

    root = dpy.screen().root
    exclude_atom = dpy.intern_atom(exclude_atom_name, only_if_exists=False)

    # `query_tree` returns root's direct children in bottom-to-top
    # stacking order. Property-tagged windows near the top of the
    # stack are the ones whose rectangles show through into the
    # capture, so we check every child and zero whichever match.
    try:
        tree = root.query_tree()
    except Exception as e:
        print(f"WARN: query_tree failed: {e}", file=sys.stderr)
        return img

    canvas = img.load()
    for child in tree.children:
        try:
            prop = child.get_full_property(exclude_atom, X.AnyPropertyType)
        except Exception:
            continue
        if prop is None or not prop.value:
            continue

        # Property values come back as python-xlib's homegrown
        # types — Int32List / str / bytes depending on format. A
        # 32-bit INTEGER with a single element 1 is what
        # `01_property_exclude.py` sets, but be permissive: treat
        # any present + nonzero as "exclude".
        flag = _extract_first_int(prop.value)
        if flag != 1:
            continue

        # `get_geometry` gives coords relative to the window's
        # parent. For a direct child of root, parent == root, so
        # the x/y are already root-relative (= screen
        # coordinates), which is what we need to index into the
        # captured image.
        try:
            g = child.get_geometry()
        except Exception:
            continue
        x0, y0, w, h = int(g.x), int(g.y), int(g.width), int(g.height)

        iw, ih = img.size
        # Clamp to the captured image; override-redirect windows
        # placed at negative offsets (the rejected "offscreen"
        # strategy) harmlessly produce empty rectangles here.
        x0c = max(x0, 0)
        y0c = max(y0, 0)
        x1c = min(x0 + w, iw)
        y1c = min(y0 + h, ih)
        for y in range(y0c, y1c):
            for x in range(x0c, x1c):
                canvas[x, y] = (0, 0, 0)

    return img


def _extract_first_int(value) -> typing.Optional[int]:
    """python-xlib returns 32-bit INTEGER properties as a list-ish
    of ints — handle the common shapes without importing internals."""
    try:
        return int(value[0])
    except Exception:
        pass
    try:
        # Some paths return bytes; decode 4 little-endian bytes.
        if isinstance(value, (bytes, bytearray)) and len(value) >= 4:
            return int.from_bytes(bytes(value[:4]), "little", signed=True)
    except Exception:
        pass
    return None


def scan_for_sentinel(
        img: Image.Image,
        region: typing.Optional[typing.Tuple[int, int, int, int]] = None
        ) -> typing.Tuple[bool, int]:
    """Return `(found, matching_pixel_count)`.

    Restricts to `region = (x, y, w, h)` when given, else scans the
    whole image. Exact-colour match — compositors don't dither solid
    fills at this colour depth, so one exact match is enough signal
    to call the capture leaky.
    """
    if region is not None:
        x, y, w, h = region
        img = img.crop((x, y, x + w, y + h))
    count = 0
    for px in img.getdata():
        if px == SENTINEL_RGB:
            count += 1
    return (count > 0, count)


def save_artefact(img: Image.Image, name: str) -> pathlib.Path:
    """Drop a capture into ./artifacts/ for visual review."""
    ARTIFACTS_DIR.mkdir(exist_ok=True)
    out = ARTIFACTS_DIR / name
    img.save(out)
    return out

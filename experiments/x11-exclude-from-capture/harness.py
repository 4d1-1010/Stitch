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
    """XGetImage on the root window — the composited output under
    an active XComposite manual-redirect session. Shared by both
    capture helpers so they start from the same raw pixels."""
    root = dpy.screen().root
    geom = root.get_geometry()
    w, h = geom.width, geom.height
    # No-op if the desktop compositor already holds manual redirect,
    # which is the common case. Matches capture_xcomposite.cpp's
    # startup call.
    root.composite_redirect_subwindows(composite.RedirectManual)
    dpy.sync()
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

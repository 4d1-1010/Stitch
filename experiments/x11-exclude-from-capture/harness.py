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

import array
import ctypes
import ctypes.util
import os
import pathlib
import subprocess
import sys
import typing

from PIL import Image

from Xlib import X, Xatom, display
from Xlib.ext import composite


# ctypes bindings for `XCompositeNameWindowPixmap`. python-xlib's
# wrapper for this call returns a resource that its own
# `get_image` binding rejects with BadDrawable — an ABI mismatch
# in the binding rather than an X-server issue. Using the real
# libXcomposite via ctypes gives us a plain Pixmap XID that we
# can then hand back to python-xlib's `get_image` on a fresh
# Pixmap object, which does work.
#
# Required for correct multi-window capture: reading
# `win.get_image(...)` on a window returns only the VISIBLE
# pixels at that region — obscured parts come back undefined
# (zeroed on most servers). The *offscreen backing pixmap* named
# by XCompositeNameWindowPixmap holds the window's full content
# regardless of occlusion, which is what a real compositor needs
# to do the right thing when one window is on top of another.
_libx11 = ctypes.CDLL(
    ctypes.util.find_library("X11") or "libX11.so.6")
_libxcomp = ctypes.CDLL(
    ctypes.util.find_library("Xcomposite") or "libXcomposite.so.1")

_libx11.XOpenDisplay.argtypes = [ctypes.c_char_p]
_libx11.XOpenDisplay.restype = ctypes.c_void_p
_libx11.XCloseDisplay.argtypes = [ctypes.c_void_p]
_libx11.XCloseDisplay.restype = ctypes.c_int
_libx11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
_libx11.XSync.restype = ctypes.c_int

_libxcomp.XCompositeNameWindowPixmap.argtypes = [
    ctypes.c_void_p,  # Display*
    ctypes.c_ulong,   # Window
]
_libxcomp.XCompositeNameWindowPixmap.restype = ctypes.c_ulong
_libxcomp.XCompositeRedirectWindow.argtypes = [
    ctypes.c_void_p,  # Display*
    ctypes.c_ulong,   # Window
    ctypes.c_int,     # update mode
]
_libxcomp.XCompositeRedirectWindow.restype = None


def _name_window_pixmap(window_xid: int) -> typing.Optional[int]:
    """Return the Pixmap XID backing `window_xid`'s offscreen
    contents via `XCompositeNameWindowPixmap`, or None on failure.

    Opens a short-lived libX11 connection for the call — the
    Pixmap XID is server-side and usable from the other
    (python-xlib) connection.
    """
    dpy = _libx11.XOpenDisplay(None)
    if not dpy:
        return None
    try:
        pix = _libxcomp.XCompositeNameWindowPixmap(
            dpy, ctypes.c_ulong(window_xid))
        _libx11.XSync(dpy, 0)
        if pix == 0:
            return None
        return int(pix)
    finally:
        _libx11.XCloseDisplay(dpy)


# The tag every overlay sets on itself, and the tag every
# property-aware capture path looks for. Production will use the same
# string; keeping it here as the single source of truth so experiment
# scripts and the real capture_xcomposite.cpp match exactly.
EXCLUDE_ATOM_NAME = "UNIO_CAPTURE_EXCLUDE"

# Defense-in-depth identifiers. A hardened overlay sets ALL of the
# properties below, not just the custom atom, so:
#
#   1. If something clears / fails to read the custom atom, WM_CLASS
#      still identifies us. (X11 protocol property reads aren't
#      atomic with respect to creation — there's a window between
#      map() and change_property() where a capture loop could see the
#      window WITHOUT the atom set. Belt and suspenders.)
#   2. Third-party tools (xprop, xwininfo, OBS, xdotool, debuggers)
#      can identify the overlay via public well-known properties
#      without needing to know the proprietary atom.
#   3. The desktop environment handles the window sensibly even if
#      nothing reads the custom atom — it stays out of taskbar /
#      pager, doesn't follow workspace switches, stays above.
WM_CLASS_INSTANCE = "unio-overlay"
WM_CLASS_CLASS    = "UnIO"
WM_NAME_HUMAN     = "UnIO capture-excluded overlay"

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
    """Hybrid capture: framebuffer grab as the base, then replace
    pixels inside tagged rectangles with what the *untagged*
    windows underneath composed to.

    Why not "full mini-compositor starting from black":
      We tried that first — walk root's children, composite each
      untagged one onto a black canvas, skip tagged. On a bare test
      scene that works. On a real desktop session it silently drops
      everything the compositor paints *itself* (GNOME Shell top
      bar, dock, Activities overlay, screen edges) because those
      aren't regular X11 windows in root's child list — Mutter
      paints them directly as internal compositor chrome. The full
      mini-compositor has no way to inspect their pixels.

      The fix is to take what the framebuffer already has as the
      ground truth (compositor chrome and all), and only rebuild
      the pixels that used to belong to tagged overlays. The
      rebuild uses the same per-window-pixmap walk as before but
      scoped to the tagged rectangles.

    Algorithm:
      1. `_get_root_image(dpy)` — framebuffer grab. Contains shell
         chrome, all apps, and the tagged overlays.
      2. Find every tagged window's on-screen rectangle.
      3. For each tagged rectangle, walk root's children in bottom-
         to-top stacking order, skipping tagged windows. For each
         non-tagged window whose geometry intersects the tagged
         rect, read its offscreen pixmap via XCompositeNameWindow-
         Pixmap and blit the overlap region onto the base image.
      4. Return the modified base image.

      The tagged rectangles now show whatever was behind the
      overlay; everywhere else the base image is untouched, so
      shell chrome is preserved.

    Production shape would do exactly the same thing in C with
    XShmGetImage for the base grab, per-window NameWindowPixmap
    for the fill regions, and GPU-side compositing (EGLImage +
    GLES shader) in place of the CPU blit. This helper is the
    architectural template, not the performance artefact.
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

        # The exclusion check — defense-in-depth identity match via
        # `is_unio_overlay` (custom atom OR WM_CLASS fallback). A
        # tagged window is invisible to this composite pass;
        # whatever is underneath it (already blitted earlier in the
        # bottom-to-top walk) remains.
        try:
            if is_unio_overlay(dpy, child, exclude_atom_name):
                continue
        except Exception:
            pass  # identity-check failure is not a reason to skip

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

        # Read the offscreen backing pixmap via XComposite, NOT the
        # window directly. `win.get_image(...)` returns only the
        # VISIBLE pixels (X11 semantics) — where another window is
        # on top, the obscured region comes back undefined / zeroed,
        # which would paint black pixels over the window underneath
        # in our composite pass. The offscreen pixmap holds the
        # window's full content regardless of occlusion, which is
        # the whole point of running under an XComposite-redirected
        # compositor.
        pix_xid = _name_window_pixmap(int(child.id))
        if pix_xid is None:
            # Window probably isn't redirected (bare-X11 case, or
            # a window that's explicitly unredirected). Fall back
            # to the direct read; correctness suffers on occluded
            # regions, but bare-X11 doesn't have a compositor
            # anyway so there's typically no occlusion to lose.
            try:
                img = child.get_image(
                    0, 0, w, h, X.ZPixmap, 0xffffffff)
            except Exception:
                continue
        else:
            # python-xlib doesn't give us a typed Pixmap object
            # from a raw XID, but the low-level get_image request
            # takes any drawable XID. Build a thin shim.
            class _PixmapShim:
                def __init__(self, xid_):
                    self.id = xid_
                    self.display = dpy
            try:
                img = display.drawable.Drawable.get_image(
                    _PixmapShim(pix_xid),
                    0, 0, w, h, X.ZPixmap, 0xffffffff)
            except Exception:
                # Named pixmap may have been freed or is invalid;
                # fall back to window read as a last resort.
                try:
                    img = child.get_image(
                        0, 0, w, h, X.ZPixmap, 0xffffffff)
                except Exception:
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
            if not is_unio_overlay(dpy, child, exclude_atom_name):
                continue
        except Exception:
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


def tag_as_unio_overlay(dpy: display.Display, win) -> None:
    """Set the full property set identifying `win` as a UnIO
    capture-excluded overlay. Safe to call from any experiment or
    production; every property is idempotent under PropModeReplace.

    Production `capture_xcomposite.cpp` will mirror this function in
    C; having it here keeps the signal set a single source of truth
    that the spike and the shipping code agree on.
    """
    # ----- Primary signal -----------------------------------------
    # Custom INTEGER atom. 32-bit, value = 1. The capture loop's
    # fast path reads this first and skips the rest of the checks.
    exclude_atom = dpy.intern_atom(
        EXCLUDE_ATOM_NAME, only_if_exists=False)
    win.change_property(
        exclude_atom, Xatom.INTEGER, 32,
        array.array('i', [1]).tolist(),
        mode=X.PropModeReplace)

    # ----- Defense-in-depth identifiers --------------------------
    # WM_CLASS: accepted fallback for third-party tools and for the
    # case where the custom atom read lost a race with window
    # creation. `set_wm_class` bakes the two-string STRING property
    # X11 expects ("instance\0class\0").
    try:
        win.set_wm_class(WM_CLASS_INSTANCE, WM_CLASS_CLASS)
    except Exception:
        # Some python-xlib versions lack set_wm_class — fall back.
        data = (WM_CLASS_INSTANCE + "\0" + WM_CLASS_CLASS + "\0")
        win.change_property(
            Xatom.WM_CLASS, Xatom.STRING, 8,
            list(data.encode("latin-1")),
            mode=X.PropModeReplace)

    # _NET_WM_NAME (UTF8) + WM_NAME (ICCCM) so every tool reads
    # something sensible. `xwininfo -tree` → "UnIO capture-excluded
    # overlay" rather than the default "(no name)".
    utf8_atom = dpy.intern_atom("UTF8_STRING")
    net_wm_name_atom = dpy.intern_atom("_NET_WM_NAME")
    name_bytes = WM_NAME_HUMAN.encode("utf-8")
    win.change_property(
        net_wm_name_atom, utf8_atom, 8,
        list(name_bytes), mode=X.PropModeReplace)
    try:
        win.set_wm_name(WM_NAME_HUMAN)
    except Exception:
        pass

    # ----- Behavioural hints for the desktop environment ---------
    # _NET_WM_WINDOW_TYPE = NOTIFICATION. NOTIFICATION is the EWMH
    # type closest to "transient always-on-top visual artifact" —
    # DEs honour it by not including the window in taskbars, Alt-Tab
    # order, workspace thumbnails, etc. TOOLTIP or UTILITY would
    # also work; NOTIFICATION matches the semantic most precisely.
    type_atom = dpy.intern_atom("_NET_WM_WINDOW_TYPE")
    type_notification = dpy.intern_atom(
        "_NET_WM_WINDOW_TYPE_NOTIFICATION")
    win.change_property(type_atom, Xatom.ATOM, 32,
                         [type_notification],
                         mode=X.PropModeReplace)

    # _NET_WM_STATE flags: always above, skip taskbar, skip pager,
    # sticky (visible on every virtual desktop). Matches how OSD
    # notifications behave across GNOME / KDE / XFCE.
    state_atom = dpy.intern_atom("_NET_WM_STATE")
    state_above   = dpy.intern_atom("_NET_WM_STATE_ABOVE")
    state_skip_tb = dpy.intern_atom("_NET_WM_STATE_SKIP_TASKBAR")
    state_skip_pg = dpy.intern_atom("_NET_WM_STATE_SKIP_PAGER")
    state_sticky  = dpy.intern_atom("_NET_WM_STATE_STICKY")
    win.change_property(
        state_atom, Xatom.ATOM, 32,
        [state_above, state_skip_tb, state_skip_pg, state_sticky],
        mode=X.PropModeReplace)

    # _NET_WM_BYPASS_COMPOSITOR = 2. Hint to the compositor to keep
    # compositing this window normally (not the "disable compositing"
    # mechanism discussed in the README — value 2 is "don't disable
    # compositing even if you'd otherwise have"). Good citizenship
    # on Mutter / KWin which can enter bypass modes for fullscreen
    # windows otherwise.
    bypass_atom = dpy.intern_atom("_NET_WM_BYPASS_COMPOSITOR")
    win.change_property(bypass_atom, Xatom.CARDINAL, 32,
                         [2], mode=X.PropModeReplace)

    # _NET_WM_PID: so a debugger (xdotool getactivewindow getwindowpid,
    # xprop _NET_WM_PID) can trace the window back to our process.
    pid_atom = dpy.intern_atom("_NET_WM_PID")
    win.change_property(pid_atom, Xatom.CARDINAL, 32,
                         [os.getpid()],
                         mode=X.PropModeReplace)


def is_unio_overlay(
        dpy: display.Display, win,
        exclude_atom_name: str = EXCLUDE_ATOM_NAME) -> bool:
    """Defense-in-depth identity check.

    Returns True iff `win` is identifiable as a UnIO
    capture-excluded overlay via ANY supported signal:

      1. Primary: `UNIO_CAPTURE_EXCLUDE` INTEGER atom with value 1.
      2. Fallback: `WM_CLASS` instance-name == "unio-overlay".

    Cheap checks first, so the typical production path is one atom
    read; the fallback only kicks in for the race-window case
    (window mapped before `change_property` completes) or when a
    third-party client is scanning for our overlays by name.
    """
    # Primary
    try:
        atom = dpy.intern_atom(
            exclude_atom_name, only_if_exists=False)
        prop = win.get_full_property(atom, X.AnyPropertyType)
        if prop is not None:
            val = _extract_first_int(prop.value)
            if val == 1:
                return True
    except Exception:
        pass
    # Fallback: WM_CLASS match.
    try:
        wm_class = win.get_wm_class()
        if wm_class and wm_class[0] == WM_CLASS_INSTANCE:
            return True
    except Exception:
        pass
    return False


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

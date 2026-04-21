"""XComposite-based screen capture — Linux X11, exclusion-aware.

Replaces ``mss`` on Linux. mss uses ``XShmGetImage`` on the root
window, which reads the final composited screen — including our own
StreamWindow overlays. That's the feedback-loop source. ``_NET_WM_
BYPASS_COMPOSITOR`` is advisory and Mutter/KWin/GNOME-Shell don't
honour it for screen-capture purposes, so it doesn't save us.

XComposite lets us walk the root's top-level children and read
each window's backing pixmap individually via ``XCompositeName
WindowPixmap``. That:
  * Skips any window we decide to exclude (our StreamWindow xids).
  * Runs against the un-composited pixels — no alpha-blending
    artefacts from the compositor.
  * Works on every modern Linux desktop, because the system
    compositor (Mutter, KWin, picom, compton, mutter-x11-frames)
    already redirects every top-level window to a backing pixmap
    in manual mode. We don't need to request a redirect ourselves;
    we just read what the system compositor already has.

If XComposite isn't available or no compositor is running, ``open()``
returns False and the pipeline falls back to mss + hide-during-
capture.

Performance note: per-grab we enumerate children via ``XQueryTree``,
ask for each window's pixmap, and read its pixels via ``XGetImage``.
That's slower than mss's single-rect read — typical ~6-12 FPS on a
1080p desktop with 10-20 windows — but it's the price for real
exclusion. Phase 6's dirty-rect short-circuit keeps bandwidth down
regardless.
"""

from __future__ import annotations

import ctypes
import logging
import os
import sys
import threading
import time
from typing import Iterable, Optional

log = logging.getLogger(__name__)


# ── X11 constants ───────────────────────────────────────────────────

IsViewable = 2
ZPixmap = 2
AllPlanes = (1 << 32) - 1


# ── X11 structures ──────────────────────────────────────────────────


class XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong),
        ("class_", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


class XImage(ctypes.Structure):
    # Only the leading fields we read. Xlib's real XImage has a
    # trailing funcs struct after bits_per_pixel; we never touch
    # those so laying them out is unnecessary.
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int),
        ("data", ctypes.c_void_p),
        ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
    ]


_ERRHANDLER_T = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)


class XShmSegmentInfo(ctypes.Structure):
    """Xlib's MIT-SHM descriptor — one per shared-memory image."""
    _fields_ = [
        ("shmseg", ctypes.c_ulong),
        ("shmid", ctypes.c_int),
        ("shmaddr", ctypes.c_void_p),
        ("readOnly", ctypes.c_int),
    ]


# SysV shm constants we need to allocate + attach segments.
_IPC_CREAT = 0o1000
_IPC_RMID = 0
_IPC_PRIVATE = 0


class _XShmImage:
    """One shared-memory XImage of a fixed size. Holds the segment
    info, the XImage*, and the mapped address so ``_paint_one`` can
    fetch raw pixels without going through the X socket."""
    __slots__ = (
        "libx11", "libxext", "libc", "dpy",
        "width", "height", "stride", "size",
        "xshm_info", "xshm_image", "addr", "attached",
    )

    def __init__(self, libx11, libxext, libc, dpy,
                 width: int, height: int):
        self.libx11 = libx11
        self.libxext = libxext
        self.libc = libc
        self.dpy = dpy
        self.width = int(width)
        self.height = int(height)
        self.stride = self.width * 4   # BGRX / XRGB8888, 32 bpp
        self.size = self.stride * self.height
        self.xshm_info = XShmSegmentInfo()
        self.xshm_image = None
        self.addr = None
        self.attached = False

    def open(self, depth: int, visual, pixel_format: int) -> bool:
        # Allocate a fresh SysV shm segment sized to this image's
        # pixel buffer, create the XImage against it, then XShmAttach
        # so the X server gets a handle to the same memory.
        shmid = self.libc.shmget(
            _IPC_PRIVATE, self.size, _IPC_CREAT | 0o600)
        if shmid < 0:
            return False
        addr = self.libc.shmat(shmid, 0, 0)
        # shmat returns (void*)-1 on failure; it's 0xFFFFFFFFFFFFFFFF
        # on x64.
        if addr == -1 or addr == (2 ** 64) - 1:
            self.libc.shmctl(shmid, _IPC_RMID, 0)
            return False
        self.xshm_info.shmseg = 0
        self.xshm_info.shmid = shmid
        self.xshm_info.shmaddr = addr
        self.xshm_info.readOnly = 0
        self.addr = addr
        self.xshm_image = self.libxext.XShmCreateImage(
            self.dpy, visual, depth, pixel_format,
            addr, ctypes.byref(self.xshm_info),
            self.width, self.height,
        )
        if not self.xshm_image:
            self.libc.shmdt(addr)
            self.libc.shmctl(shmid, _IPC_RMID, 0)
            return False
        if not self.libxext.XShmAttach(
                self.dpy, ctypes.byref(self.xshm_info)):
            self.libx11.XDestroyImage(self.xshm_image)
            self.libc.shmdt(addr)
            self.libc.shmctl(shmid, _IPC_RMID, 0)
            return False
        # Mark the segment for deletion now. Under Linux SysV shm
        # semantics it actually disappears once no process still has
        # it mapped, so we're free to exit / crash without leaking.
        self.libc.shmctl(shmid, _IPC_RMID, 0)
        self.libx11.XSync(self.dpy, 0)
        self.attached = True
        return True

    def close(self) -> None:
        if not self.attached:
            return
        try:
            self.libxext.XShmDetach(
                self.dpy, ctypes.byref(self.xshm_info))
        except Exception:
            pass
        if self.xshm_image:
            try:
                self.libx11.XDestroyImage(self.xshm_image)
            except Exception:
                pass
            self.xshm_image = None
        if self.addr:
            try:
                self.libc.shmdt(self.addr)
            except Exception:
                pass
            self.addr = None
        self.attached = False


class _XShmPool:
    """Pool of ``_XShmImage`` keyed by (width, height). Each window
    size we encounter allocates one segment that's reused for every
    subsequent grab of that same-size window. The cache is bounded
    to prevent pathological cases (a rapidly-resizing window would
    otherwise leak an unbounded number of shm segments)."""

    _MAX_ENTRIES = 24

    def __init__(self, libx11, dpy):
        self.libx11 = libx11
        self.dpy = dpy
        self.libxext = None
        self.libc = None
        self.depth = 24
        self.visual = None
        self.pixel_format = 2   # ZPixmap
        self._cache: dict = {}

    def open(self) -> bool:
        try:
            self.libxext = ctypes.CDLL("libXext.so.6")
        except OSError:
            return False
        try:
            self.libc = ctypes.CDLL("libc.so.6", use_errno=True)
        except OSError:
            return False
        # MIT-SHM bindings.
        xe = self.libxext
        xe.XShmQueryExtension.argtypes = [ctypes.c_void_p]
        xe.XShmQueryExtension.restype = ctypes.c_int
        xe.XShmCreateImage.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
            ctypes.c_int, ctypes.c_void_p,
            ctypes.POINTER(XShmSegmentInfo),
            ctypes.c_uint, ctypes.c_uint,
        ]
        xe.XShmCreateImage.restype = ctypes.POINTER(XImage)
        xe.XShmAttach.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(XShmSegmentInfo)]
        xe.XShmAttach.restype = ctypes.c_int
        xe.XShmDetach.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(XShmSegmentInfo)]
        xe.XShmDetach.restype = ctypes.c_int
        xe.XShmGetImage.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.POINTER(XImage), ctypes.c_int, ctypes.c_int,
            ctypes.c_ulong,
        ]
        xe.XShmGetImage.restype = ctypes.c_int
        # SysV shm bindings (libc).
        self.libc.shmget.argtypes = [
            ctypes.c_int, ctypes.c_size_t, ctypes.c_int]
        self.libc.shmget.restype = ctypes.c_int
        self.libc.shmat.argtypes = [
            ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
        self.libc.shmat.restype = ctypes.c_void_p
        self.libc.shmdt.argtypes = [ctypes.c_void_p]
        self.libc.shmdt.restype = ctypes.c_int
        self.libc.shmctl.argtypes = [
            ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        self.libc.shmctl.restype = ctypes.c_int
        if not xe.XShmQueryExtension(self.dpy):
            return False
        # Pull the default visual from the display so XShmCreateImage
        # gets a matching XImage layout. libx11 exposes this as a
        # macro; it's screen 0 → visual on modern single-screen setups.
        self.libx11.XDefaultVisual.argtypes = [
            ctypes.c_void_p, ctypes.c_int]
        self.libx11.XDefaultVisual.restype = ctypes.c_void_p
        self.libx11.XDefaultScreen.argtypes = [ctypes.c_void_p]
        self.libx11.XDefaultScreen.restype = ctypes.c_int
        screen = self.libx11.XDefaultScreen(self.dpy)
        self.visual = self.libx11.XDefaultVisual(self.dpy, screen)
        if not self.visual:
            return False
        return True

    def acquire(self, width: int, height: int):
        key = (int(width), int(height))
        entry = self._cache.get(key)
        if entry is not None:
            return entry
        if len(self._cache) >= self._MAX_ENTRIES:
            # Evict an arbitrary entry; we don't see enough
            # resizing in practice to justify LRU bookkeeping.
            victim_key, victim = next(iter(self._cache.items()))
            victim.close()
            del self._cache[victim_key]
        entry = _XShmImage(
            self.libx11, self.libxext, self.libc, self.dpy,
            width, height)
        if not entry.open(self.depth, self.visual, self.pixel_format):
            entry.close()
            return None
        self._cache[key] = entry
        return entry

    def close(self) -> None:
        for entry in self._cache.values():
            entry.close()
        self._cache.clear()


# ── Capture class ───────────────────────────────────────────────────


class XCompositeCapture:
    """Per-window composite reader. Single-consumer; don't drive
    concurrently from multiple threads."""

    def __init__(self) -> None:
        self.dpy = None
        self.root = 0
        self.libx11 = None
        self.libxcomposite = None
        self._err_handler = None
        # Raw xids pushed in by the shell (Tk winfo_id values). Under
        # a reparenting WM like Mutter, these are usually NOT direct
        # children of root — the WM wraps them in a decoration frame.
        # We resolve each raw xid up to the root-child ancestor at
        # grab time (cached until the set changes).
        self._exclude_xids_raw: set = set()
        self._exclude_ancestors: set = set()
        self._exclude_cache_key: frozenset = frozenset()
        self._lock = threading.Lock()
        # Wallpaper base layer. Mutter/GNOME Shell paints the
        # wallpaper through Clutter/GSK, bypassing X11 framebuffers
        # entirely — XGetImage(root) and XGetImage(overlay_window)
        # both return 99.8% black for wallpaper-only regions, so the
        # per-window composite below has to synthesise the wallpaper
        # itself. We read the image file directly via gsettings and
        # apply GNOME's "zoom" scaling per monitor.
        self._wallpaper_path = ""
        self._wallpaper_full = None
        self._wallpaper_scaled_key: tuple = ()
        self._wallpaper_scaled_img = None
        # gsettings subprocess cadence. The wallpaper almost never
        # changes at runtime, and spawning gsettings every frame was
        # costing ~50-80 ms/grab — a wallpaper refresh every 10 s is
        # plenty in practice and caps the subprocess overhead at
        # ~1% of a 60 fps budget.
        self._wallpaper_check_interval = 10.0
        self._wallpaper_last_check = 0.0
        self._last_log_at = 0.0
        # Composite buffer — a single BGRX/BGRA bytes arena we
        # memmove window pixels into instead of paying PIL's
        # per-paste BGRX→RGB conversion + row loop. Allocated
        # lazily on the first grab sized to the capture rect.
        self._composite_buf = None
        self._composite_size: tuple = (0, 0)
        self._composite_stride = 0
        # Snapshot of the most recent grab's composite BGRX buffer,
        # exposed via ``last_bgra_bytes()`` so the H.264 encoder
        # can be fed the raw 4-byte-per-pixel buffer directly with
        # ``-pix_fmt bgra`` instead of us paying PIL's BGRX→RGB
        # conversion just to hand the bytes back to ffmpeg which
        # would undo it anyway.
        self._last_bgra_buf = None
        self._last_bgra_size: tuple = (0, 0)
        self._last_bgra_stride = 0
        # Pre-scaled wallpaper as raw BGRX bytes sized to the
        # capture rect. Keyed off (path, width, height) so a
        # wallpaper or monitor change re-renders exactly once.
        self._wallpaper_bgra_key: tuple = ()
        self._wallpaper_bgra = None

    # ── Exclusion list, thread-safe ─────────────────────────────

    def set_exclude_xids(self, xids: Iterable[int]) -> None:
        with self._lock:
            self._exclude_xids_raw = {int(x) for x in xids if x}

    def add_exclude_xid(self, xid: int) -> None:
        with self._lock:
            if xid:
                self._exclude_xids_raw.add(int(xid))

    def remove_exclude_xid(self, xid: int) -> None:
        with self._lock:
            self._exclude_xids_raw.discard(int(xid))

    # ── Lifecycle ───────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "linux":
            return False
        try:
            return self._open_x()
        except Exception:
            log.exception("XCompositeCapture init failed")
            self.close()
            return False

    def _open_x(self) -> bool:
        try:
            self.libx11 = ctypes.CDLL("libX11.so.6")
        except OSError as e:
            log.info("libX11.so.6 not loadable: %s", e)
            return False
        try:
            self.libxcomposite = ctypes.CDLL("libXcomposite.so.1")
        except OSError as e:
            log.info("libXcomposite.so.1 not loadable: %s", e)
            return False

        x = self.libx11
        xc = self.libxcomposite
        ulong_p = ctypes.POINTER(ctypes.c_ulong)
        int_p = ctypes.POINTER(ctypes.c_int)
        uint_p = ctypes.POINTER(ctypes.c_uint)

        x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x.XOpenDisplay.restype = ctypes.c_void_p
        x.XCloseDisplay.argtypes = [ctypes.c_void_p]
        x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        x.XDefaultRootWindow.restype = ctypes.c_ulong
        x.XQueryTree.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ulong_p, ulong_p,
            ctypes.POINTER(ulong_p), uint_p,
        ]
        x.XQueryTree.restype = ctypes.c_int
        x.XFree.argtypes = [ctypes.c_void_p]
        x.XGetWindowAttributes.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.POINTER(XWindowAttributes),
        ]
        x.XGetWindowAttributes.restype = ctypes.c_int
        x.XTranslateCoordinates.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_int, ctypes.c_int,
            int_p, int_p, ulong_p,
        ]
        x.XTranslateCoordinates.restype = ctypes.c_int
        x.XGetImage.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.c_int, ctypes.c_int,
            ctypes.c_uint, ctypes.c_uint,
            ctypes.c_ulong, ctypes.c_int,
        ]
        x.XGetImage.restype = ctypes.POINTER(XImage)
        x.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
        x.XFreePixmap.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        x.XSetErrorHandler.argtypes = [_ERRHANDLER_T]
        x.XSetErrorHandler.restype = _ERRHANDLER_T
        x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                  ctypes.c_int]
        x.XInternAtom.restype = ctypes.c_ulong
        x.XQueryPointer.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong,
            ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_uint),
        ]
        x.XQueryPointer.restype = ctypes.c_int
        x.XGetWindowProperty.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
            ctypes.c_long, ctypes.c_long, ctypes.c_int,
            ctypes.c_ulong,
            ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
        ]
        x.XGetWindowProperty.restype = ctypes.c_int

        xc.XCompositeQueryExtension.argtypes = [
            ctypes.c_void_p, int_p, int_p]
        xc.XCompositeQueryExtension.restype = ctypes.c_int
        xc.XCompositeNameWindowPixmap.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong]
        xc.XCompositeNameWindowPixmap.restype = ctypes.c_ulong

        # Install a silent X error handler. XComposite calls race
        # with window destruction — transient BadWindow/BadPixmap/
        # BadDrawable errors are expected and must not abort the
        # process the way Xlib's default handler does. Return 0 to
        # tell Xlib to continue.
        def _silent(_dpy, _err):
            return 0

        self._err_handler = _ERRHANDLER_T(_silent)
        x.XSetErrorHandler(self._err_handler)

        display_name = os.environ.get("DISPLAY", "").encode() or None
        self.dpy = x.XOpenDisplay(display_name)
        if not self.dpy:
            log.info("XOpenDisplay(%r) failed", display_name)
            return False

        ev = ctypes.c_int()
        err = ctypes.c_int()
        if not xc.XCompositeQueryExtension(
                self.dpy, ctypes.byref(ev), ctypes.byref(err)):
            log.info("XComposite extension not available on this server")
            return False

        self.root = x.XDefaultRootWindow(self.dpy)
        # Pre-compute the atoms we need for window-type filtering.
        # GNOME's DING extension and similar window-manager chrome
        # (docks, desktop backgrounds) use _NET_WM_WINDOW_TYPE_DESKTOP.
        # Those windows either cover the monitor with an empty pixmap
        # or contain icons rendered by the compositor outside of X —
        # either way compositing them over our wallpaper base paints
        # black over what we actually want to send.
        self._atom_wm_type = x.XInternAtom(
            self.dpy, b"_NET_WM_WINDOW_TYPE", 0)
        self._atom_type_desktop = x.XInternAtom(
            self.dpy, b"_NET_WM_WINDOW_TYPE_DESKTOP", 0)
        # EWMH client-list-stacking: the window manager publishes an
        # ordered list of its managed top-level windows on the root
        # window. Walking XQueryTree on root returns every child
        # window including sub-menus, IME helpers, transient popups,
        # etc. — on a busy desktop that's 700+ windows, and each one
        # costs two X roundtrips (attributes + translate-coords) in
        # _paint_one. Reading the EWMH list instead gives us ~10–50
        # top-level windows, already sorted bottom-to-top.
        self._atom_client_list_stacking = x.XInternAtom(
            self.dpy, b"_NET_CLIENT_LIST_STACKING", 0)
        self._atom_window = x.XInternAtom(self.dpy, b"WINDOW", 0)
        # Try to attach MIT-SHM. When available it replaces
        # XGetImage's ~8 MB-per-frame socket transfer with a
        # shared-memory map, typically 3-5× faster at 1080p.
        # Failure is non-fatal; we just fall back to XGetImage.
        self._shm = None
        try:
            self._shm = _XShmPool(x, self.dpy)
            if not self._shm.open():
                self._shm = None
        except Exception:
            log.exception("XShm init failed — falling back to XGetImage")
            self._shm = None
        if self._shm is not None:
            log.info("XShm attached: per-frame XGetImage avoided")
        if not self._probe_pixmap():
            log.info("XComposite pixmap probe failed — no compositor "
                     "has redirected subwindows, falling back to mss")
            return False

        log.info("XCompositeCapture ready (root=0x%x)", self.root)
        return True

    def _probe_pixmap(self) -> bool:
        """Test that ``XCompositeNameWindowPixmap`` actually returns
        a pixmap for at least one mapped top-level child of root. If
        every call returns 0 there's no compositor — NameWindow
        Pixmap only works on windows that some composite client has
        redirected (our code deliberately doesn't do this itself to
        avoid fighting the system compositor)."""
        root_ret = ctypes.c_ulong()
        parent_ret = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        nchildren = ctypes.c_uint()
        ok = self.libx11.XQueryTree(
            self.dpy, self.root,
            ctypes.byref(root_ret), ctypes.byref(parent_ret),
            ctypes.byref(children), ctypes.byref(nchildren),
        )
        if not ok or not nchildren.value:
            return False
        success = False
        try:
            for i in range(nchildren.value):
                xid = int(children[i])
                attrs = XWindowAttributes()
                if not self.libx11.XGetWindowAttributes(
                        self.dpy, xid, ctypes.byref(attrs)):
                    continue
                if attrs.map_state != IsViewable:
                    continue
                if attrs.width <= 1 or attrs.height <= 1:
                    continue
                pixmap = self.libxcomposite.XCompositeNameWindowPixmap(
                    self.dpy, xid)
                self.libx11.XSync(self.dpy, 0)
                if pixmap:
                    self.libx11.XFreePixmap(self.dpy, pixmap)
                    success = True
                    break
        finally:
            if children:
                self.libx11.XFree(children)
        return success

    def close(self) -> None:
        if self.dpy and self.libx11:
            try:
                self.libx11.XCloseDisplay(self.dpy)
            except Exception:
                pass
        self.dpy = None

    # ── Grab ────────────────────────────────────────────────────

    def grab(self, rect: dict):
        """Grab one frame of ``rect``. ``rect`` keys: x, y, width,
        height (all root-relative). Returns a PIL.Image in RGB mode
        or None on error.

        Starts from a black background and blits every mapped
        top-level window's pixmap in bottom-to-top stacking order,
        skipping any xid in ``_exclude_xids``. Cursor is not drawn.
        """
        from PIL import Image
        if not self.dpy:
            return None
        rx = int(rect.get("x", 0))
        ry = int(rect.get("y", 0))
        rw = int(rect.get("width", 0))
        rh = int(rect.get("height", 0))
        if rw <= 0 or rh <= 0:
            return None

        with self._lock:
            raw = set(self._exclude_xids_raw)
            cached_key = self._exclude_cache_key
            ancestors = set(self._exclude_ancestors)
        # Re-resolve ancestors whenever the raw set changes. Done on
        # the capture thread so all XQueryTree calls run on the same
        # X connection as the grab itself — Xlib isn't thread-safe by
        # default and we deliberately don't call XInitThreads.
        if frozenset(raw) != cached_key:
            ancestors = self._resolve_ancestors(raw)
            with self._lock:
                self._exclude_ancestors = set(ancestors)
                self._exclude_cache_key = frozenset(raw)
        exclude = ancestors

        t0 = time.monotonic()
        # Fast path: EWMH-managed top-level window list (≤50 entries
        # on a busy desktop). Falls back to XQueryTree on WMs that
        # don't publish the property.
        window_ids = self._client_list_stacking()
        children_ptr = None
        nchildren_value = 0
        if window_ids is None:
            root_ret = ctypes.c_ulong()
            parent_ret = ctypes.c_ulong()
            children = ctypes.POINTER(ctypes.c_ulong)()
            nchildren = ctypes.c_uint()
            ok = self.libx11.XQueryTree(
                self.dpy, self.root,
                ctypes.byref(root_ret), ctypes.byref(parent_ret),
                ctypes.byref(children), ctypes.byref(nchildren),
            )
            if not ok:
                return None
            window_ids = [int(children[i])
                          for i in range(nchildren.value)]
            children_ptr = children
            nchildren_value = nchildren.value

        t1 = time.monotonic()
        # Build a raw BGRX composite buffer via ctypes.memmove so we
        # skip PIL.paste's per-row BGRX→RGB conversion. Wallpaper is
        # pre-rendered to BGRX once per (path, size). Windows memmove
        # on top in stacking order.
        composite = self._composite_buffer(rw, rh)
        stride = self._composite_stride
        wallpaper_bgra = self._wallpaper_bgra_for(rw, rh)
        if wallpaper_bgra is not None:
            ctypes.memmove(composite, wallpaper_bgra, stride * rh)
            has_wallpaper = True
        else:
            ctypes.memset(composite, 0, stride * rh)
            has_wallpaper = False
        t2 = time.monotonic()
        # Read pointer position before walking children so a busy
        # server doesn't drift the cursor between layers. Coord space
        # is root-relative, matching our `rect`.
        ptr_x, ptr_y = self._query_pointer_root()
        painted = 0
        skipped_excluded = 0
        skipped_desktop = 0
        skipped_clip = 0
        try:
            # Walk bottom-to-top stacking, painting each window on
            # top of what's already there — as raw memmove into the
            # BGRX composite buffer, no PIL paste.
            for xid in window_ids:
                if xid in exclude:
                    skipped_excluded += 1
                    continue
                if self._is_desktop_type(xid):
                    skipped_desktop += 1
                    continue
                if self._paint_one_bgra(
                        xid, rx, ry, rw, rh, composite, stride):
                    painted += 1
                else:
                    skipped_clip += 1
        finally:
            if children_ptr:
                self.libx11.XFree(children_ptr)
        t3 = time.monotonic()
        # One-shot BGRX→RGB conversion via PIL's raw decoder — the
        # final pass from composite buffer to PIL, happens exactly
        # once per grab regardless of how many windows were pasted.
        out = Image.frombuffer(
            "RGB", (rw, rh), composite, "raw", "BGRX", stride, 1,
        )
        _draw_cursor_overlay(out, rx, ry, rw, rh, ptr_x, ptr_y)
        # Publish the raw BGRX composite buffer (pre-cursor-overlay)
        # so callers who want the 4-byte-per-pixel bytes can skip
        # the PIL roundtrip. The cursor overlay lives in PIL-land
        # only — acceptable for now because the H.264 stream already
        # carries the peer's real cursor via WGC / xinput relay.
        self._last_bgra_buf = composite
        self._last_bgra_size = (rw, rh)
        self._last_bgra_stride = stride
        t4 = time.monotonic()
        # Telemetry log throttled to ~1 Hz, but carrying per-stage
        # timings so we can actually see where the grab's millis go.
        now = time.monotonic()
        if now - self._last_log_at > 1.0:
            log.info(
                "xcomposite grab rect=(%d,%d,%dx%d) enum=%d painted=%d "
                "timings(ms): enum=%.1f wallpaper=%.1f paint_loop=%.1f "
                "cursor=%.1f total=%.1f",
                rx, ry, rw, rh, len(window_ids), painted,
                (t1 - t0) * 1000, (t2 - t1) * 1000,
                (t3 - t2) * 1000, (t4 - t3) * 1000,
                (t4 - t0) * 1000)
            self._last_log_at = now
        return out

    def last_bgra_bytes(self) -> Optional[bytes]:
        """Return the raw composite BGRX bytes from the most recent
        ``grab()`` — exactly ``width*height*4`` bytes, little-endian
        (B, G, R, X) per pixel, no row padding. Returns None before
        the first successful grab. One ``bytes()`` copy out of the
        ctypes arena (~8 MB at 1080p) — cheap next to the PIL
        BGRX→RGB conversion we're replacing, and it keeps the
        buffer safe if a grab lands mid-encode."""
        buf = self._last_bgra_buf
        if buf is None:
            return None
        w, h = self._last_bgra_size
        stride = self._last_bgra_stride
        if stride != w * 4:
            return None
        # The composite arena is sized exactly to ``stride * height``
        # (see _composite_buffer) so a single ``bytes(buf)`` copy is
        # already the right length — no slice needed.
        return bytes(buf)

    def _query_pointer_root(self) -> tuple[int, int]:
        """Return the pointer's root-relative (x, y). (-1, -1) on
        failure — draw_cursor_overlay treats that as 'skip'."""
        root_ret = ctypes.c_ulong()
        child_ret = ctypes.c_ulong()
        rx = ctypes.c_int()
        ry = ctypes.c_int()
        wx = ctypes.c_int()
        wy = ctypes.c_int()
        mask = ctypes.c_uint()
        ok = self.libx11.XQueryPointer(
            self.dpy, self.root,
            ctypes.byref(root_ret), ctypes.byref(child_ret),
            ctypes.byref(rx), ctypes.byref(ry),
            ctypes.byref(wx), ctypes.byref(wy),
            ctypes.byref(mask),
        )
        if not ok:
            return (-1, -1)
        return (int(rx.value), int(ry.value))

    def _client_list_stacking(self) -> Optional[list]:
        """Return the WM's ordered list of managed top-level windows
        (bottom-to-top stacking), or None when the property is missing
        or unreadable. Populated on every mainstream EWMH-compliant
        window manager (Mutter, KWin, Xfwm, Openbox, i3). One root
        XGetWindowProperty call returns the whole list — orders of
        magnitude faster than the XQueryTree walk because we don't
        visit compositor-internal child windows, IME scratch windows,
        menu popups, etc."""
        x = self.libx11
        actual_type = ctypes.c_ulong()
        actual_format = ctypes.c_int()
        nitems = ctypes.c_ulong()
        bytes_after = ctypes.c_ulong()
        prop_ptr = ctypes.POINTER(ctypes.c_ubyte)()
        # long_length in 32-bit units; 4096 atoms covers any sane
        # desktop (typical is 10–50 managed windows).
        AnyPropertyType = 0
        ok = x.XGetWindowProperty(
            self.dpy, self.root, self._atom_client_list_stacking,
            0, 4096, 0, AnyPropertyType,
            ctypes.byref(actual_type), ctypes.byref(actual_format),
            ctypes.byref(nitems), ctypes.byref(bytes_after),
            ctypes.byref(prop_ptr),
        )
        if ok != 0:
            return None
        try:
            if not prop_ptr or nitems.value == 0:
                return None
            if actual_format.value != 32:
                return None
            window_atoms = ctypes.cast(
                prop_ptr, ctypes.POINTER(ctypes.c_ulong))
            return [int(window_atoms[i]) for i in range(nitems.value)]
        finally:
            if prop_ptr:
                x.XFree(prop_ptr)

    def _is_desktop_type(self, xid: int) -> bool:
        """True when ``xid`` has ``_NET_WM_WINDOW_TYPE_DESKTOP``. We
        skip such windows because modern desktops (GNOME+DING, KDE
        Plasma desktop, …) keep their pixmap empty and render icons
        via Clutter / a separate QML layer; pasting their all-black
        X pixmap on top of our wallpaper base would hide it. XInternAtom
        results are cached at open time so this is just one property
        read per window per frame."""
        x = self.libx11
        actual_type = ctypes.c_ulong()
        actual_format = ctypes.c_int()
        nitems = ctypes.c_ulong()
        bytes_after = ctypes.c_ulong()
        prop_ptr = ctypes.POINTER(ctypes.c_ubyte)()
        # long_length=8 atoms = 32 bytes. _NET_WM_WINDOW_TYPE is a
        # list of atoms (window can have multiple types, spec-wise);
        # 8 is well more than anything reasonable.
        AnyPropertyType = 0
        ok = x.XGetWindowProperty(
            self.dpy, xid, self._atom_wm_type, 0, 8, 0,
            AnyPropertyType,
            ctypes.byref(actual_type), ctypes.byref(actual_format),
            ctypes.byref(nitems), ctypes.byref(bytes_after),
            ctypes.byref(prop_ptr),
        )
        if ok != 0:
            return False
        try:
            if not prop_ptr or nitems.value == 0:
                return False
            if actual_format.value != 32:
                return False
            atoms = ctypes.cast(
                prop_ptr, ctypes.POINTER(ctypes.c_ulong))
            for i in range(nitems.value):
                if atoms[i] == self._atom_type_desktop:
                    return True
            return False
        finally:
            if prop_ptr:
                x.XFree(prop_ptr)

    def _wallpaper_for_monitor(self, width: int, height: int):
        """Return a PIL.Image sized to (width, height), painted with
        the GNOME desktop wallpaper scaled per the 'zoom' option
        (fill monitor, preserve aspect, center-crop overflow). Falls
        back to None if gsettings / the file are unavailable, and the
        caller should use an all-black base in that case. Cached per
        (path, size) so repeated grabs don't re-scale."""
        from PIL import Image
        self._refresh_wallpaper_path()
        if self._wallpaper_full is None:
            return None
        key = (self._wallpaper_path, width, height)
        if self._wallpaper_scaled_key == key \
                and self._wallpaper_scaled_img is not None:
            return self._wallpaper_scaled_img.copy()
        src = self._wallpaper_full
        sw, sh = src.size
        if sw <= 0 or sh <= 0 or width <= 0 or height <= 0:
            return None
        # GNOME "zoom" == fill, preserving aspect, cropping overflow.
        scale = max(width / sw, height / sh)
        nw = max(1, int(round(sw * scale)))
        nh = max(1, int(round(sh * scale)))
        scaled = src.resize((nw, nh), Image.LANCZOS)
        cx = max(0, (nw - width) // 2)
        cy = max(0, (nh - height) // 2)
        img = scaled.crop((cx, cy, cx + width, cy + height))
        self._wallpaper_scaled_key = key
        self._wallpaper_scaled_img = img
        return img.copy()

    def _refresh_wallpaper_path(self) -> None:
        """Re-read the GNOME wallpaper URI from gsettings. Cached so
        we only reload the PNG/JPEG when the user actually changes
        the wallpaper. Silent on failure — this is a best-effort
        enhancement; XComposite capture still works (on a black
        base) when gsettings is missing.

        Throttled to once per ``_wallpaper_check_interval`` seconds
        because spawning gsettings on every grab was the dominant
        cost of the Linux capture path (tens of ms per frame)."""
        import subprocess
        import time as _time
        now = _time.monotonic()
        if (self._wallpaper_full is not None
                and now - self._wallpaper_last_check
                < self._wallpaper_check_interval):
            return
        self._wallpaper_last_check = now
        try:
            result = subprocess.run(
                ["gsettings", "get", "org.gnome.desktop.background",
                 "picture-uri"],
                capture_output=True, text=True, timeout=2,
                check=False,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return
        if result.returncode != 0:
            return
        raw = result.stdout.strip().strip("'").strip('"')
        if not raw.startswith("file://"):
            return
        path = raw[len("file://"):]
        if path == self._wallpaper_path and self._wallpaper_full is not None:
            return
        try:
            from PIL import Image
            img = Image.open(path).convert("RGB")
            img.load()
        except Exception:
            log.exception("wallpaper load failed for %s", path)
            return
        self._wallpaper_path = path
        self._wallpaper_full = img
        self._wallpaper_scaled_key = ()
        self._wallpaper_scaled_img = None
        log.info("wallpaper loaded: %s (%dx%d)", path, *img.size)

    def _resolve_ancestors(self, raw_xids: Iterable[int]) -> set:
        """Walk each raw xid's parent chain until we find a direct
        child of root, return that set. Under a reparenting window
        manager the Tk-reported xid for a Toplevel is typically 2–4
        levels below root (inside a WM decoration frame), so skipping
        only the Tk xid misses the frame — which then gets composited
        and drags our overlay's pixels back into the captured image.

        Capped at 16 levels per chain so a weirdly-deep hierarchy
        can't hang the grab."""
        x = self.libx11
        ulong_p = ctypes.POINTER(ctypes.c_ulong)
        out: set = set()
        for raw in raw_xids:
            current = int(raw)
            if not current:
                continue
            resolved = current
            for _ in range(16):
                root_ret = ctypes.c_ulong()
                parent_ret = ctypes.c_ulong()
                children = ctypes.POINTER(ctypes.c_ulong)()
                nchildren = ctypes.c_uint()
                ok = x.XQueryTree(
                    self.dpy, current,
                    ctypes.byref(root_ret), ctypes.byref(parent_ret),
                    ctypes.byref(children), ctypes.byref(nchildren),
                )
                if children:
                    x.XFree(children)
                if not ok:
                    break
                parent = int(parent_ret.value)
                if parent == int(self.root) or parent == 0:
                    resolved = current
                    break
                current = parent
                resolved = current
            out.add(resolved)
            if resolved != int(raw):
                log.info("XComposite exclude: raw xid 0x%x resolved "
                         "to root-child ancestor 0x%x", int(raw),
                         resolved)
        return out

    def _composite_buffer(self, width: int, height: int):
        """Lazy-allocate (or reuse) the BGRX composite arena. The
        buffer is ``width * height * 4`` bytes — 8.3 MB at 1080p —
        and lives as a ctypes array so ``ctypes.memmove`` can dump
        pixels into it directly from XShm segments without going
        through PIL.paste's per-row byte-swap + conversion."""
        stride = width * 4
        if (self._composite_buf is None
                or self._composite_size != (width, height)):
            self._composite_buf = (
                ctypes.c_char * (stride * height))()
            self._composite_size = (width, height)
            self._composite_stride = stride
        return self._composite_buf

    def _wallpaper_bgra_for(self, width: int, height: int):
        """Return raw BGRX bytes of the current wallpaper scaled to
        ``(width, height)``. Cached and lazily rebuilt on wallpaper
        path or size change. Used by ``grab`` to memmove the
        wallpaper into the composite buffer in one shot instead of
        going through PIL.paste each grab."""
        self._refresh_wallpaper_path()
        if self._wallpaper_full is None:
            return None
        key = (self._wallpaper_path, width, height)
        if (self._wallpaper_bgra_key == key
                and self._wallpaper_bgra is not None):
            return self._wallpaper_bgra
        img = self._wallpaper_for_monitor(width, height)
        if img is None:
            return None
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        # Re-channel RGBA → BGRA (B/R swap) to match the composite
        # buffer layout. PIL does this with a C loop when we call
        # tobytes on a rawmode'd image.
        bgra_bytes = img.tobytes("raw", "BGRA")
        buf = (ctypes.c_char * len(bgra_bytes))(*bgra_bytes)
        self._wallpaper_bgra_key = key
        self._wallpaper_bgra = buf
        return buf

    def _paint_one_bgra(self, xid: int, rx: int, ry: int,
                        rw: int, rh: int,
                        composite, stride: int) -> bool:
        """Paint one top-level window's pixmap into the composite
        BGRX buffer via ``ctypes.memmove``. Per-row copy so we can
        honour the window's (wx, wy) offset and clip at the buffer
        edges. Same overall behaviour as ``_paint_one`` but without
        allocating a PIL Image for every window or paying the per-
        pixel BGRX→RGB conversion that ``out.paste`` imposes."""
        x = self.libx11
        xc = self.libxcomposite
        attrs = XWindowAttributes()
        if not x.XGetWindowAttributes(
                self.dpy, xid, ctypes.byref(attrs)):
            return False
        if attrs.map_state != IsViewable:
            return False
        if attrs.width <= 0 or attrs.height <= 0:
            return False

        wx_out = ctypes.c_int()
        wy_out = ctypes.c_int()
        child_ret = ctypes.c_ulong()
        if not x.XTranslateCoordinates(
                self.dpy, xid, self.root, 0, 0,
                ctypes.byref(wx_out), ctypes.byref(wy_out),
                ctypes.byref(child_ret)):
            return False
        wx = wx_out.value
        wy = wy_out.value
        ww = attrs.width
        wh = attrs.height

        # Clip against our capture rect.
        if (wx + ww <= rx or wy + wh <= ry
                or wx >= rx + rw or wy >= ry + rh):
            return False

        # Visible rect inside the window, relative to the window.
        src_left = max(0, rx - wx)
        src_top = max(0, ry - wy)
        src_right = min(ww, rx + rw - wx)
        src_bottom = min(wh, ry + rh - wy)
        copy_w = src_right - src_left
        copy_h = src_bottom - src_top
        if copy_w <= 0 or copy_h <= 0:
            return False
        # Destination offset in the composite buffer.
        dst_x = max(0, wx - rx)
        dst_y = max(0, wy - ry)

        pixmap = xc.XCompositeNameWindowPixmap(self.dpy, xid)
        x.XSync(self.dpy, 0)
        if not pixmap:
            return False
        try:
            # Fast path: XShmGetImage into a cached shm XImage.
            if self._shm is not None:
                entry = self._shm.acquire(ww, wh)
                if entry is not None:
                    ok = self._shm.libxext.XShmGetImage(
                        self.dpy, pixmap, entry.xshm_image,
                        0, 0, AllPlanes)
                    x.XSync(self.dpy, 0)
                    if ok:
                        src_stride = entry.stride
                        src_row_bytes = copy_w * 4
                        src_addr = entry.addr
                        dst_addr = (
                            ctypes.addressof(composite)
                            + dst_y * stride + dst_x * 4)
                        # Full-width fast path: when src and dst
                        # row pitches match AND the visible rect
                        # covers the full window width + starts at
                        # row 0 with no left offset, rows are
                        # contiguous in BOTH buffers and a single
                        # memmove replaces 1080× Python-level
                        # ctypes.memmove calls (each of which costs
                        # ~5 µs of Python overhead). Saves ~15 ms
                        # per full-window paint vs per-row.
                        if (src_row_bytes == src_stride
                                == stride
                                and src_left == 0):
                            ctypes.memmove(
                                dst_addr,
                                src_addr + src_top * src_stride,
                                src_stride * copy_h,
                            )
                        else:
                            for row in range(copy_h):
                                ctypes.memmove(
                                    dst_addr + row * stride,
                                    src_addr
                                    + (src_top + row) * src_stride
                                    + src_left * 4,
                                    src_row_bytes,
                                )
                        return True
            # XGetImage fallback (no XShm).
            ximg_ptr = x.XGetImage(
                self.dpy, pixmap, 0, 0, ww, wh,
                AllPlanes, ZPixmap,
            )
            if not ximg_ptr:
                return False
            try:
                ximg = ximg_ptr.contents
                bpl = ximg.bytes_per_line
                depth = ximg.bits_per_pixel
                data_addr = ximg.data
                if not data_addr or bpl <= 0 or depth not in (24, 32):
                    return False
                src_row_bytes = copy_w * 4
                dst_addr = (
                    ctypes.addressof(composite)
                    + dst_y * stride + dst_x * 4)
                if (src_row_bytes == bpl == stride
                        and src_left == 0):
                    ctypes.memmove(
                        dst_addr,
                        data_addr + src_top * bpl,
                        bpl * copy_h,
                    )
                else:
                    for row in range(copy_h):
                        ctypes.memmove(
                            dst_addr + row * stride,
                            data_addr
                            + (src_top + row) * bpl
                            + src_left * 4,
                            src_row_bytes,
                        )
                return True
            finally:
                x.XDestroyImage(ximg_ptr)
        finally:
            x.XFreePixmap(self.dpy, pixmap)

    def _paint_one(self, xid: int, rx: int, ry: int,
                   rw: int, rh: int, out) -> bool:
        """Paint one top-level window's backing pixmap into ``out``.
        All X calls go through the silent error handler, so transient
        failures (window destroyed mid-grab, server racing us) just
        skip the window instead of raising. Returns True when we
        actually pasted pixels."""
        from PIL import Image
        x = self.libx11
        xc = self.libxcomposite
        attrs = XWindowAttributes()
        if not x.XGetWindowAttributes(
                self.dpy, xid, ctypes.byref(attrs)):
            return False
        if attrs.map_state != IsViewable:
            return False
        if attrs.width <= 0 or attrs.height <= 0:
            return False

        wx_out = ctypes.c_int()
        wy_out = ctypes.c_int()
        child_ret = ctypes.c_ulong()
        if not x.XTranslateCoordinates(
                self.dpy, xid, self.root, 0, 0,
                ctypes.byref(wx_out), ctypes.byref(wy_out),
                ctypes.byref(child_ret)):
            return False
        wx = wx_out.value
        wy = wy_out.value
        ww = attrs.width
        wh = attrs.height

        # Skip windows that don't intersect our capture rect.
        if (wx + ww <= rx or wy + wh <= ry
                or wx >= rx + rw or wy >= ry + rh):
            return False

        pixmap = xc.XCompositeNameWindowPixmap(self.dpy, xid)
        # Sync so the silent error handler runs before we touch
        # the returned pixmap (which may be 0 if the request failed).
        x.XSync(self.dpy, 0)
        if not pixmap:
            return False
        try:
            # Fast path: MIT-SHM. Reuses a cached shared-memory
            # XImage sized to this window; XShmGetImage writes the
            # pixmap into shm with no socket transfer (~2-3 ms vs
            # ~10 ms on a 1080p window).
            if self._shm is not None:
                entry = self._shm.acquire(ww, wh)
                if entry is not None:
                    ok = self._shm.libxext.XShmGetImage(
                        self.dpy, pixmap, entry.xshm_image,
                        0, 0, AllPlanes)
                    x.XSync(self.dpy, 0)
                    if ok:
                        try:
                            # Zero-copy handoff to PIL. ctypes array
                            # exposes the buffer protocol; PIL reads
                            # from shm directly without an 8 MB
                            # bytes() duplicate.
                            buf = (ctypes.c_char * entry.size)\
                                .from_address(entry.addr)
                            img = Image.frombuffer(
                                "RGB", (ww, wh),
                                buf, "raw", "BGRX",
                                entry.stride, 1,
                            )
                            out.paste(img, (wx - rx, wy - ry))
                            return True
                        except Exception:
                            log.exception("XShm paste failed")
                            # Fall through to the XGetImage path.
            # XGetImage fallback: the old socket-transfer path.
            ximg_ptr = x.XGetImage(
                self.dpy, pixmap, 0, 0, ww, wh,
                AllPlanes, ZPixmap,
            )
            if not ximg_ptr:
                return False
            try:
                ximg = ximg_ptr.contents
                bpl = ximg.bytes_per_line
                depth = ximg.bits_per_pixel
                data_addr = ximg.data
                if not data_addr or bpl <= 0:
                    return False
                # Zero-copy: wrap the XImage data in a ctypes buffer
                # and hand it to PIL via the buffer protocol. No
                # 8 MB bytes() duplicate.
                raw = (ctypes.c_char * (bpl * wh))\
                    .from_address(data_addr)
                # Most X servers give us BGRA (little-endian 32 bpp
                # ZPixmap) at depth 24/32. Try BGRX first; if the
                # window is 16 bpp or something unusual, skip it —
                # no compositor normally hands us that.
                if depth in (24, 32):
                    try:
                        img = Image.frombuffer(
                            "RGB", (ww, wh), raw,
                            "raw", "BGRX", bpl, 1,
                        )
                    except Exception:
                        return False
                else:
                    return False
                # Paste relative to the capture rect. PIL's paste
                # auto-clips at output edges so we don't need to
                # manually crop the src.
                out.paste(img, (wx - rx, wy - ry))
                return True
            finally:
                x.XDestroyImage(ximg_ptr)
        finally:
            x.XFreePixmap(self.dpy, pixmap)


# ── Cursor overlay (shared across capture backends) ─────────────────


def _draw_cursor_overlay(img, rect_x: int, rect_y: int,
                         rect_w: int, rect_h: int,
                         cursor_global_x: int,
                         cursor_global_y: int) -> None:
    """Paint a simple arrow cursor onto ``img`` at the cursor's
    position within the captured rect. No-op if the cursor is
    outside the rect or position couldn't be queried (cursor_*_x
    < 0). Shared by every capture backend — DXGI, XComposite,
    whatever — because X11 pixmap reads and DXGI Desktop Dup both
    exclude the OS cursor image and we have to composite it in
    ourselves or the routed sink shows a frozen view.

    Drawing a generic white-with-black-outline arrow rather than the
    real OS cursor: matches what the remote user expects to see
    visually and saves us from platform-specific cursor-shape
    capture. Can be upgraded per-platform later."""
    if cursor_global_x < 0 or cursor_global_y < 0:
        return
    if not (rect_x <= cursor_global_x < rect_x + rect_w
            and rect_y <= cursor_global_y < rect_y + rect_h):
        return
    from PIL import ImageDraw
    px = cursor_global_x - rect_x
    py = cursor_global_y - rect_y
    draw = ImageDraw.Draw(img)
    # Classic white arrow with a 1-pixel black outline. Coords are
    # the tip of the arrow + body + tail. Shape borrowed from the
    # standard X11 left_ptr.
    outline = [
        (px, py),
        (px + 11, py + 11),
        (px + 6, py + 11),
        (px + 9, py + 17),
        (px + 7, py + 18),
        (px + 4, py + 12),
        (px, py + 16),
    ]
    fill = [
        (px + 1, py + 2),
        (px + 10, py + 11),
        (px + 5, py + 11),
        (px + 8, py + 16),
        (px + 7, py + 17),
        (px + 4, py + 11),
        (px + 1, py + 14),
    ]
    try:
        draw.polygon(outline, fill=(0, 0, 0))
        draw.polygon(fill, fill=(255, 255, 255))
    except Exception:
        log.exception("cursor overlay paint failed")


# ── Availability probe ──────────────────────────────────────────────


def available() -> bool:
    if sys.platform != "linux":
        return False
    try:
        ctypes.CDLL("libX11.so.6")
        ctypes.CDLL("libXcomposite.so.1")
        return True
    except OSError:
        return False

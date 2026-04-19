"""Linux evdi userspace bridge.

evdi (Extensible Virtual Display Interface, DisplayLink open-source
kernel module) advertises a phantom monitor to the Linux compositor.
Apps can drag windows onto it; the framebuffer lives in user-space
memory that our code grabs at every vsync. That framebuffer feeds
into StreamServer as a real source — replacing the placeholder path
for virtuals owned by this PC.

We talk to the shipped libevdi.so.1 via ctypes — no need for
python-evdi or libevdi-dev. The surface area we use is small:

    evdi_add_device             → create a new /dev/dri/cardN
    evdi_open(idx)              → open it, get a handle
    evdi_connect(h, edid, ...)  → advertise a monitor with an EDID
    evdi_register_buffer(h, b)  → hand our pixel buffer to the driver
    evdi_request_update(h, id)  → "do you have new pixels for buffer id?"
    evdi_handle_events(h, ctx)  → pump events; triggers callbacks
    evdi_grab_pixels(h, rects)  → copy changed pixels into the buffer
    evdi_disconnect / _close    → tear down

The event loop runs in its own thread. The capture callback hands
the latest PIL.Image to whoever calls `latest_frame()` — usually the
StreamServer's virtual-source path.
"""

from __future__ import annotations

import ctypes
import logging
import os
import threading
import time
from typing import Optional

log = logging.getLogger(__name__)


# ── EDID (1920×1080 @ 60 Hz) ────────────────────────────────────────
#
# A canned Extended Display Identification Data block for a generic
# 1920×1080 panel. The driver checks basic sanity + advertises this
# to the compositor so apps see something believable to render to.
# Generated with `edid-decode`-friendly defaults; identical to the
# one evdi's reference test program ships.

_EDID_1080P = bytes([
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  # header
    0x36, 0xF0, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,  # mfg + product
    0x01, 0x1D, 0x01, 0x03, 0x80, 0x34, 0x20, 0x78,  # EDID version + video input
    0x2A, 0xEE, 0x95, 0xA3, 0x54, 0x4C, 0x99, 0x26,  # chroma
    0x0F, 0x50, 0x54, 0xA1, 0x08, 0x00, 0x31, 0x40,  # established
    0x45, 0x40, 0x61, 0x40, 0x71, 0x40, 0x81, 0x80,
    0x90, 0x40, 0x95, 0x00, 0xA9, 0x40, 0xB3, 0x00,
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,  # detailed 1080p@60
    0x58, 0x2C, 0x45, 0x00, 0xC4, 0x8E, 0x21, 0x00,
    0x00, 0x1E, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x75,  # serial string
    0x6E, 0x69, 0x6F, 0x56, 0x69, 0x72, 0x74, 0x31,
    0x0A, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC,  # monitor name
    0x00, 0x75, 0x6E, 0x49, 0x4F, 0x20, 0x56, 0x69,
    0x72, 0x74, 0x75, 0x61, 0x6C, 0x0A, 0x00, 0x00,
    0x00, 0xFD, 0x00, 0x32, 0x4B, 0x18, 0x53, 0x11,  # range limits
    0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x00, 0x57,                                       # ext count + checksum
])


# ── ctypes binding ───────────────────────────────────────────────────


class _EvdiRect(ctypes.Structure):
    _fields_ = [
        ("x1", ctypes.c_int),
        ("y1", ctypes.c_int),
        ("x2", ctypes.c_int),
        ("y2", ctypes.c_int),
    ]


class _EvdiBuffer(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_int),
        ("buffer", ctypes.c_void_p),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("stride", ctypes.c_int),
        ("rects", ctypes.POINTER(_EvdiRect)),
        ("rect_count", ctypes.c_int),
    ]


class _EvdiMode(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("refresh_rate", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("pixel_format", ctypes.c_uint),
    ]


# Callback typedefs — void returns, handful of arg shapes.
_DPMS_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)
_MODE_CB = ctypes.CFUNCTYPE(None, _EvdiMode, ctypes.c_void_p)
_UPDATE_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)
_CRTC_CB = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_void_p)
_CURSOR_SET_CB = ctypes.CFUNCTYPE(
    None, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int32,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p)
_CURSOR_MOVE_CB = ctypes.CFUNCTYPE(
    None, ctypes.c_int32, ctypes.c_int32, ctypes.c_void_p)
_DDCCI_CB = ctypes.CFUNCTYPE(
    None, ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8,
    ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint8, ctypes.c_void_p)


class _EvdiEventCtx(ctypes.Structure):
    _fields_ = [
        ("dpms_handler", _DPMS_CB),
        ("mode_changed_handler", _MODE_CB),
        ("update_ready_handler", _UPDATE_CB),
        ("crtc_state_handler", _CRTC_CB),
        ("cursor_set_handler", _CURSOR_SET_CB),
        ("cursor_move_handler", _CURSOR_MOVE_CB),
        ("ddcci_data_handler", _DDCCI_CB),
        ("user_data", ctypes.c_void_p),
    ]


def _load_libevdi():
    for name in ("libevdi.so.1", "libevdi.so.0", "libevdi.so"):
        try:
            lib = ctypes.CDLL(name)
            return lib
        except OSError:
            continue
    return None


_lib = _load_libevdi()
if _lib is not None:
    _lib.evdi_add_device.restype = ctypes.c_int
    _lib.evdi_add_device.argtypes = []
    _lib.evdi_open.restype = ctypes.c_void_p
    _lib.evdi_open.argtypes = [ctypes.c_int]
    _lib.evdi_close.restype = None
    _lib.evdi_close.argtypes = [ctypes.c_void_p]
    _lib.evdi_connect.restype = None
    _lib.evdi_connect.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                  ctypes.c_uint, ctypes.c_uint32]
    _lib.evdi_disconnect.restype = None
    _lib.evdi_disconnect.argtypes = [ctypes.c_void_p]
    _lib.evdi_register_buffer.restype = None
    _lib.evdi_register_buffer.argtypes = [ctypes.c_void_p, _EvdiBuffer]
    _lib.evdi_unregister_buffer.restype = None
    _lib.evdi_unregister_buffer.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _lib.evdi_request_update.restype = ctypes.c_bool
    _lib.evdi_request_update.argtypes = [ctypes.c_void_p, ctypes.c_int]
    _lib.evdi_grab_pixels.restype = None
    _lib.evdi_grab_pixels.argtypes = [ctypes.c_void_p,
                                      ctypes.POINTER(_EvdiRect),
                                      ctypes.POINTER(ctypes.c_int)]
    _lib.evdi_handle_events.restype = None
    _lib.evdi_handle_events.argtypes = [ctypes.c_void_p,
                                        ctypes.POINTER(_EvdiEventCtx)]
    _lib.evdi_get_event_ready.restype = ctypes.c_int
    _lib.evdi_get_event_ready.argtypes = [ctypes.c_void_p]


def available() -> bool:
    """True when libevdi is present AND the evdi kernel module is
    loaded. Caller should fall through to the placeholder path when
    False."""
    if _lib is None:
        return False
    return os.path.isdir("/sys/module/evdi")


def _try_add_device_as_root() -> bool:
    """evdi_add_device() writes to /sys/devices/evdi/add which is
    root-only. Calling it without permission crashes libevdi (the
    error path dereferences a NULL fd). So we only invoke it when
    the process is actually running as root — otherwise return False
    and let the caller surface a helpful message instead of dying."""
    try:
        if os.geteuid() != 0:
            return False
    except AttributeError:
        return False
    if _lib is None:
        return False
    try:
        rc = _lib.evdi_add_device()
    except Exception:
        log.exception("evdi_add_device raised")
        return False
    return rc >= 0


# ── EvdiDevice — one virtual monitor ────────────────────────────────


class EvdiDevice:
    """A single live evdi virtual monitor. Owns a pixel buffer and
    an event-loop thread that requests updates and grabs pixels when
    the driver says they're ready. `latest_frame()` returns the most
    recent PIL.Image — StreamServer polls this from its capture
    loop."""

    def __init__(self, monitor_id: str,
                 width: int = 1920, height: int = 1080):
        self.monitor_id = monitor_id
        self.width = width
        self.height = height
        self._handle: Optional[int] = None
        self._buffer_id = 1
        self._stride = width * 4        # BGRA, 32bpp
        self._pixel_bytes = self._stride * height
        self._pixel_buf = (ctypes.c_uint8 * self._pixel_bytes)()
        self._rects_buf = (_EvdiRect * 16)()      # evdi caps at 16
        self._rect_count = ctypes.c_int(0)
        self._evdi_buffer: Optional[_EvdiBuffer] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._frame_lock = threading.Lock()
        self._latest_image = None                  # PIL.Image

        # Keep strong refs to callback trampolines — if Python GCs
        # the CFUNCTYPE instances while evdi still holds them, you
        # get a beautiful segfault during teardown.
        self._cb_dpms = _DPMS_CB(self._on_dpms)
        self._cb_mode = _MODE_CB(self._on_mode_changed)
        self._cb_update = _UPDATE_CB(self._on_update_ready)
        self._cb_crtc = _CRTC_CB(self._on_crtc_state)
        self._cb_cursor_set = _CURSOR_SET_CB(self._on_cursor_set)
        self._cb_cursor_move = _CURSOR_MOVE_CB(self._on_cursor_move)
        self._cb_ddcci = _DDCCI_CB(self._on_ddcci)

        self._event_ctx = _EvdiEventCtx(
            dpms_handler=self._cb_dpms,
            mode_changed_handler=self._cb_mode,
            update_ready_handler=self._cb_update,
            crtc_state_handler=self._cb_crtc,
            cursor_set_handler=self._cb_cursor_set,
            cursor_move_handler=self._cb_cursor_move,
            ddcci_data_handler=self._cb_ddcci,
            user_data=None,
        )
        self._update_pending = False

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self) -> bool:
        if _lib is None:
            return False
        # evdi_check_device codes: 0 = AVAILABLE (is an evdi card),
        # 1 = UNRECOGNIZED (real GPU), 2 = NOT_PRESENT. We only open
        # AVAILABLE ones so we don't grab a real GPU's DRI node.
        free_idx: Optional[int] = None
        for idx in range(16):
            rc = _lib.evdi_check_device(idx)
            if rc == 0:
                free_idx = idx
                break
        if free_idx is None:
            if not _try_add_device_as_root():
                log.info(
                    "evdi: no virtual cards available. Run "
                    "`sudo sh -c 'echo 1 > /sys/devices/evdi/add'` "
                    "once (or add a udev rule) to enable virtual "
                    "displays. Falling back to placeholder stream.")
                return False
            # add_device succeeded — rescan.
            for idx in range(16):
                if _lib.evdi_check_device(idx) == 0:
                    free_idx = idx
                    break
            if free_idx is None:
                return False
        h = _lib.evdi_open(free_idx)
        if not h:
            log.info("evdi_open(%d) returned NULL", free_idx)
            return False
        self._handle = h

        # Advertise our EDID so the compositor enumerates this as a
        # real monitor. pixel_area_limit=0 means "no limit".
        edid = (ctypes.c_char * len(_EDID_1080P))(*_EDID_1080P)
        _lib.evdi_connect(self._handle, edid, len(_EDID_1080P), 0)

        # Register our pixel buffer so the driver knows where to
        # write.
        self._evdi_buffer = _EvdiBuffer(
            id=self._buffer_id,
            buffer=ctypes.addressof(self._pixel_buf),
            width=self.width,
            height=self.height,
            stride=self._stride,
            rects=self._rects_buf,
            rect_count=0,
        )
        _lib.evdi_register_buffer(self._handle, self._evdi_buffer)

        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run_loop, daemon=True,
            name=f"evdi-{self.monitor_id}",
        )
        self._thread.start()
        log.info("evdi monitor %s live at %dx%d",
                 self.monitor_id, self.width, self.height)
        return True

    def close(self) -> None:
        self._stop.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._thread = None
        if self._handle is not None and _lib is not None:
            try:
                _lib.evdi_unregister_buffer(
                    self._handle, self._buffer_id)
            except Exception:
                pass
            try:
                _lib.evdi_disconnect(self._handle)
            except Exception:
                pass
            try:
                _lib.evdi_close(self._handle)
            except Exception:
                pass
            self._handle = None

    # ── Event loop ───────────────────────────────────────────────

    def _run_loop(self) -> None:
        """evdi's API is request/response with callbacks. We ask for
        an update, then pump handle_events until update_ready fires,
        then grab pixels. Repeat at ~30 fps."""
        import select
        if _lib is None or self._handle is None:
            return
        fd = _lib.evdi_get_event_ready(self._handle)
        period = 1.0 / 30.0
        while not self._stop.is_set():
            tick = time.monotonic()
            # Ask the driver "any new pixels for buffer?".
            if not self._update_pending:
                ok = _lib.evdi_request_update(
                    self._handle, self._buffer_id)
                # True = there were pending pixels already; grab
                # them without waiting.
                if ok:
                    self._grab_and_publish()
                else:
                    self._update_pending = True
            # Pump events; any callbacks fire on THIS thread.
            try:
                r, _w, _x = select.select([fd], [], [], period)
            except (OSError, ValueError):
                r = []
            if r:
                _lib.evdi_handle_events(self._handle,
                                        ctypes.byref(self._event_ctx))
            # Frame cadence.
            elapsed = time.monotonic() - tick
            remaining = period - elapsed
            if remaining > 0:
                time.sleep(remaining)

    def _grab_and_publish(self) -> None:
        self._rect_count.value = 16
        _lib.evdi_grab_pixels(
            self._handle, self._rects_buf,
            ctypes.byref(self._rect_count))
        try:
            from PIL import Image
            img = Image.frombuffer(
                "RGBA", (self.width, self.height),
                bytes(self._pixel_buf),
                "raw", "BGRA", 0, 1,
            ).convert("RGB")
            with self._frame_lock:
                self._latest_image = img
        except Exception:
            log.exception("evdi pixel convert failed")

    # ── evdi callbacks ───────────────────────────────────────────

    def _on_update_ready(self, buffer_id, _user):
        self._update_pending = False
        if buffer_id == self._buffer_id:
            self._grab_and_publish()

    def _on_mode_changed(self, mode, _user):
        log.info("evdi mode changed: %dx%d @ %d Hz",
                 mode.width, mode.height, mode.refresh_rate)
        # Real drivers can change resolution here; v1 pins to our
        # advertised EDID's 1920x1080.

    def _on_dpms(self, dpms_mode, _user):
        log.debug("evdi dpms: %s", dpms_mode)

    def _on_crtc_state(self, state, _user):
        log.debug("evdi crtc state: %s", state)

    def _on_cursor_set(self, *_args):
        pass

    def _on_cursor_move(self, *_args):
        pass

    def _on_ddcci(self, *_args):
        pass

    # ── Public accessor ──────────────────────────────────────────

    def latest_frame(self):
        with self._frame_lock:
            return self._latest_image

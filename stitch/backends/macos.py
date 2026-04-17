"""
macOS input backend.

Uses:
  - CoreGraphics (Quartz) for cursor control, input injection, event taps
  - ApplicationServices for monitor detection
  - pbcopy/pbpaste for clipboard (avoids PyObjC dependency)

Accessibility permissions are required for input capture/injection.
Grant in: System Settings → Privacy & Security → Accessibility.
"""

import ctypes
import ctypes.util
import json
import logging
import subprocess
import threading
from typing import Callable, Optional

from . import InputBackend, MonitorRect
from .keycodes import mac_to_hid, hid_to_mac

log = logging.getLogger(__name__)

# ── Load CoreGraphics ────────────────────────────────────────────

_cg_path = ctypes.util.find_library("CoreGraphics")
if _cg_path is None:
    _cg_path = (
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
    )

_cg = ctypes.cdll.LoadLibrary(_cg_path)

# Also need ApplicationServices for some display functions
_as_path = ctypes.util.find_library("ApplicationServices")
if _as_path is None:
    _as_path = (
        "/System/Library/Frameworks/ApplicationServices.framework"
        "/ApplicationServices"
    )
try:
    _appserv = ctypes.cdll.LoadLibrary(_as_path)
except OSError:
    _appserv = _cg  # fallback: functions may be in CoreGraphics


# ── Types ────────────────────────────────────────────────────────

CGFloat = ctypes.c_double
CGError = ctypes.c_int32
CGDirectDisplayID = ctypes.c_uint32
CGEventRef = ctypes.c_void_p
CGEventSourceRef = ctypes.c_void_p
CGEventTapProxy = ctypes.c_void_p
CFMachPortRef = ctypes.c_void_p
CFRunLoopSourceRef = ctypes.c_void_p
CFRunLoopRef = ctypes.c_void_p


class CGPoint(ctypes.Structure):
    _fields_ = [("x", CGFloat), ("y", CGFloat)]


class CGSize(ctypes.Structure):
    _fields_ = [("width", CGFloat), ("height", CGFloat)]


class CGRect(ctypes.Structure):
    _fields_ = [("origin", CGPoint), ("size", CGSize)]


# Event types
kCGEventLeftMouseDown = 1
kCGEventLeftMouseUp = 2
kCGEventRightMouseDown = 3
kCGEventRightMouseUp = 4
kCGEventMouseMoved = 5
kCGEventLeftMouseDragged = 6
kCGEventRightMouseDragged = 7
kCGEventKeyDown = 10
kCGEventKeyUp = 11
kCGEventScrollWheel = 22
kCGEventOtherMouseDown = 25
kCGEventOtherMouseUp = 26
kCGEventOtherMouseDragged = 27

kCGScrollEventUnitPixel = 0

kCGHIDEventTap = 0
kCGHeadInsertEventTap = 0
kCGEventTapOptionDefault = 0

kCGEventKeyboardEventAutorepeat = 0x00000008

# Event field keys
kCGKeyboardEventKeycode = 9
kCGMouseEventDeltaX = 4
kCGMouseEventDeltaY = 5
kCGScrollWheelEventDeltaAxis1 = 11
kCGScrollWheelEventDeltaAxis2 = 12
kCGMouseEventButtonNumber = 3

# Event masks
kCGEventMaskForAllEvents = (1 << 64) - 1

# kCGDirectMainDisplay
kCGDirectMainDisplay = _cg.CGMainDisplayID()


# ── Function signatures ──────────────────────────────────────────

# Display detection
_cg.CGGetActiveDisplayList.argtypes = [
    ctypes.c_uint32,
    ctypes.POINTER(CGDirectDisplayID),
    ctypes.POINTER(ctypes.c_uint32),
]
_cg.CGGetActiveDisplayList.restype = CGError

_cg.CGDisplayBounds.argtypes = [CGDirectDisplayID]
_cg.CGDisplayBounds.restype = CGRect

_cg.CGDisplayPixelsWide.argtypes = [CGDirectDisplayID]
_cg.CGDisplayPixelsWide.restype = ctypes.c_size_t
_cg.CGDisplayPixelsHigh.argtypes = [CGDirectDisplayID]
_cg.CGDisplayPixelsHigh.restype = ctypes.c_size_t

# Cursor
_cg.CGWarpMouseCursorPosition.argtypes = [CGPoint]
_cg.CGWarpMouseCursorPosition.restype = CGError

_cg.CGAssociateMouseAndMouseCursorPosition.argtypes = [ctypes.c_bool]
_cg.CGAssociateMouseAndMouseCursorPosition.restype = CGError

_cg.CGDisplayHideCursor.argtypes = [CGDirectDisplayID]
_cg.CGDisplayShowCursor.argtypes = [CGDirectDisplayID]

# Display configuration
CGDisplayConfigRef = ctypes.c_void_p

_cg.CGBeginDisplayConfiguration.argtypes = [ctypes.POINTER(CGDisplayConfigRef)]
_cg.CGBeginDisplayConfiguration.restype = CGError

_cg.CGConfigureDisplayOrigin.argtypes = [
    CGDisplayConfigRef, CGDirectDisplayID, ctypes.c_int32, ctypes.c_int32,
]
_cg.CGConfigureDisplayOrigin.restype = CGError

_cg.CGCompleteDisplayConfiguration.argtypes = [
    CGDisplayConfigRef, ctypes.c_uint32,
]
_cg.CGCompleteDisplayConfiguration.restype = CGError

_cg.CGCancelDisplayConfiguration.argtypes = [CGDisplayConfigRef]
_cg.CGCancelDisplayConfiguration.restype = CGError

kCGConfigurePermanently = 2

# Events
_cg.CGEventCreateMouseEvent.argtypes = [
    CGEventSourceRef, ctypes.c_uint32, CGPoint, ctypes.c_uint32,
]
_cg.CGEventCreateMouseEvent.restype = CGEventRef

_cg.CGEventCreateKeyboardEvent.argtypes = [
    CGEventSourceRef, ctypes.c_uint16, ctypes.c_bool,
]
_cg.CGEventCreateKeyboardEvent.restype = CGEventRef

_cg.CGEventCreateScrollWheelEvent.argtypes = [
    CGEventSourceRef, ctypes.c_uint32, ctypes.c_uint32,
    ctypes.c_int32, ctypes.c_int32,
]
_cg.CGEventCreateScrollWheelEvent.restype = CGEventRef

_cg.CGEventPost.argtypes = [ctypes.c_uint32, CGEventRef]
_cg.CGEventPost.restype = None

_cg.CGEventSetIntegerValueField.argtypes = [
    CGEventRef, ctypes.c_uint32, ctypes.c_int64,
]

_cg.CGEventGetIntegerValueField.argtypes = [
    CGEventRef, ctypes.c_uint32,
]
_cg.CGEventGetIntegerValueField.restype = ctypes.c_int64

_cg.CGEventGetLocation.argtypes = [CGEventRef]
_cg.CGEventGetLocation.restype = CGPoint

_cg.CGEventGetType.argtypes = [CGEventRef]
_cg.CGEventGetType.restype = ctypes.c_uint32

# CFRelease
try:
    _cf = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreFoundation"))
except (OSError, TypeError):
    _cf = ctypes.cdll.LoadLibrary(
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
    )
_cf.CFRelease.argtypes = [ctypes.c_void_p]

# CGEventTap
CGEventTapCallBack = ctypes.CFUNCTYPE(
    CGEventRef,          # return
    CGEventTapProxy,     # proxy
    ctypes.c_uint32,     # type (CGEventType)
    CGEventRef,          # event
    ctypes.c_void_p,     # userInfo
)

_cg.CGEventTapCreate.argtypes = [
    ctypes.c_uint32,     # tap
    ctypes.c_uint32,     # place
    ctypes.c_uint32,     # options
    ctypes.c_uint64,     # eventsOfInterest
    CGEventTapCallBack,  # callback
    ctypes.c_void_p,     # userInfo
]
_cg.CGEventTapCreate.restype = CFMachPortRef

_cg.CGEventTapEnable.argtypes = [CFMachPortRef, ctypes.c_bool]

# CFMachPort → CFRunLoopSource
_cf.CFMachPortCreateRunLoopSource.argtypes = [
    ctypes.c_void_p, CFMachPortRef, ctypes.c_long,
]
_cf.CFMachPortCreateRunLoopSource.restype = CFRunLoopSourceRef

_cf.CFRunLoopGetCurrent.restype = CFRunLoopRef
_cf.CFRunLoopAddSource.argtypes = [
    CFRunLoopRef, CFRunLoopSourceRef, ctypes.c_void_p,
]
_cf.CFRunLoopRun.restype = None
_cf.CFRunLoopStop.argtypes = [CFRunLoopRef]

# kCFRunLoopCommonModes (needs to be loaded as a symbol)
try:
    _kCFRunLoopCommonModes = ctypes.c_void_p.in_dll(
        _cf, "kCFRunLoopCommonModes"
    )
except (ValueError, AttributeError):
    _kCFRunLoopCommonModes = ctypes.c_void_p()


# ── Current cursor position helper ───────────────────────────────

def _get_cursor_pos() -> tuple[int, int]:
    """Get cursor position via a temporary event."""
    event = _cg.CGEventCreate(None)
    if not event:
        return 0, 0
    loc = _cg.CGEventGetLocation(event)
    _cf.CFRelease(event)
    return int(loc.x), int(loc.y)


# Need CGEventCreate for position query
_cg.CGEventCreate.argtypes = [CGEventSourceRef]
_cg.CGEventCreate.restype = CGEventRef


# ── Backend implementation ───────────────────────────────────────

class MacOSBackend(InputBackend):
    """Quartz/CoreGraphics backend for macOS."""

    def __init__(self):
        self._grabbed = False
        self._on_key: Optional[Callable] = None
        self._tap_port: Optional[ctypes.c_void_p] = None
        self._tap_source: Optional[ctypes.c_void_p] = None
        self._tap_thread: Optional[threading.Thread] = None
        self._tap_running = False
        self._tap_runloop: Optional[ctypes.c_void_p] = None
        # Must prevent GC of the callback
        self._tap_callback_ref = None

    # ── Lifecycle ────────────────────────────────────────────────

    def open(self):
        pass  # CoreGraphics is always available on macOS

    def close(self):
        self.stop_key_capture()
        if self._grabbed:
            self.ungrab_input()

    # ── Monitors ─────────────────────────────────────────────────

    def query_monitors(self) -> list[MonitorRect]:
        max_displays = 32
        count = ctypes.c_uint32()
        display_ids = (CGDirectDisplayID * max_displays)()
        err = _cg.CGGetActiveDisplayList(max_displays, display_ids,
                                         ctypes.byref(count))
        if err != 0:
            log.warning("CGGetActiveDisplayList failed: %d", err)
            return [MonitorRect("display-0", 0, 0, 1920, 1080)]

        monitors = []
        for i in range(count.value):
            did = display_ids[i]
            bounds = _cg.CGDisplayBounds(did)
            monitors.append(MonitorRect(
                id=f"display-{did}",
                x=int(bounds.origin.x),
                y=int(bounds.origin.y),
                width=int(bounds.size.width),
                height=int(bounds.size.height),
            ))
        return monitors

    def set_monitor_positions(
        self, positions: dict[str, tuple[int, int]],
    ) -> bool:
        """Reconfigure macOS display arrangement via CGDisplayConfigure.

        Keys must be ids returned by query_monitors (e.g. "display-12345").
        """
        norm = self._normalize_positions(positions)
        if norm is None:
            return False

        parsed: dict[int, tuple[int, int]] = {}
        for mid, xy in norm.items():
            if not mid.startswith("display-"):
                log.warning("Unknown monitor id for macOS: %s", mid)
                return False
            try:
                parsed[int(mid.split("-", 1)[1])] = xy
            except ValueError:
                log.warning("Invalid macOS display id: %s", mid)
                return False

        config = CGDisplayConfigRef()
        err = _cg.CGBeginDisplayConfiguration(ctypes.byref(config))
        if err != 0:
            log.warning("CGBeginDisplayConfiguration failed: %d", err)
            return False

        for did, (x, y) in parsed.items():
            err = _cg.CGConfigureDisplayOrigin(config, did, x, y)
            if err != 0:
                log.warning("CGConfigureDisplayOrigin(%d) failed: %d",
                            did, err)
                _cg.CGCancelDisplayConfiguration(config)
                return False

        err = _cg.CGCompleteDisplayConfiguration(
            config, kCGConfigurePermanently,
        )
        if err != 0:
            log.warning("CGCompleteDisplayConfiguration failed: %d", err)
            return False
        log.info("Applied OS monitor layout via CGDisplayConfigure.")
        return True

    @property
    def screen_width(self) -> int:
        monitors = self.query_monitors()
        if not monitors:
            return 1920
        return max(m.x + m.width for m in monitors)

    @property
    def screen_height(self) -> int:
        monitors = self.query_monitors()
        if not monitors:
            return 1080
        return max(m.y + m.height for m in monitors)

    # ── Cursor ───────────────────────────────────────────────────

    def get_cursor_pos(self) -> tuple[int, int]:
        return _get_cursor_pos()

    def set_cursor_pos(self, x: int, y: int):
        _cg.CGWarpMouseCursorPosition(CGPoint(float(x), float(y)))

    def get_button_mask(self) -> int:
        # Use CGEventSourceButtonState
        mask = 0
        # 0=left, 1=right, 2=middle
        try:
            _cg.CGEventSourceButtonState.argtypes = [
                ctypes.c_uint32, ctypes.c_uint32,
            ]
            _cg.CGEventSourceButtonState.restype = ctypes.c_bool
            if _cg.CGEventSourceButtonState(0, 0):  # left
                mask |= 1
            if _cg.CGEventSourceButtonState(0, 2):  # middle
                mask |= 2
            if _cg.CGEventSourceButtonState(0, 1):  # right
                mask |= 4
        except Exception:
            pass
        return mask

    # ── Grab ─────────────────────────────────────────────────────

    def grab_input(self) -> bool:
        # Disassociate mouse from cursor movement (captures raw deltas)
        _cg.CGAssociateMouseAndMouseCursorPosition(False)
        _cg.CGDisplayHideCursor(kCGDirectMainDisplay)
        self._grabbed = True
        return True

    def ungrab_input(self):
        _cg.CGAssociateMouseAndMouseCursorPosition(True)
        _cg.CGDisplayShowCursor(kCGDirectMainDisplay)
        self._grabbed = False

    @property
    def is_grabbed(self) -> bool:
        return self._grabbed

    # ── Injection ────────────────────────────────────────────────

    def inject_mouse_move(self, x: int, y: int):
        pt = CGPoint(float(x), float(y))
        event = _cg.CGEventCreateMouseEvent(
            None, kCGEventMouseMoved, pt, 0,
        )
        if event:
            _cg.CGEventPost(kCGHIDEventTap, event)
            _cf.CFRelease(event)

    def inject_mouse_move_rel(self, dx: int, dy: int):
        # Get current position, add delta, post absolute move
        cx, cy = self.get_cursor_pos()
        nx, ny = cx + dx, cy + dy
        pt = CGPoint(float(nx), float(ny))
        event = _cg.CGEventCreateMouseEvent(
            None, kCGEventMouseMoved, pt, 0,
        )
        if event:
            # Set delta fields for apps that use raw input
            _cg.CGEventSetIntegerValueField(
                event, kCGMouseEventDeltaX, dx,
            )
            _cg.CGEventSetIntegerValueField(
                event, kCGMouseEventDeltaY, dy,
            )
            _cg.CGEventPost(kCGHIDEventTap, event)
            _cf.CFRelease(event)

    def inject_mouse_button(self, button: int, pressed: bool):
        cx, cy = self.get_cursor_pos()
        pt = CGPoint(float(cx), float(cy))

        type_map = {
            (1, True): kCGEventLeftMouseDown,
            (1, False): kCGEventLeftMouseUp,
            (2, True): kCGEventOtherMouseDown,
            (2, False): kCGEventOtherMouseUp,
            (3, True): kCGEventRightMouseDown,
            (3, False): kCGEventRightMouseUp,
        }
        etype = type_map.get((button, pressed))
        if etype is None:
            return

        # CGEventCreateMouseEvent needs the button number for "other"
        cg_button = {1: 0, 2: 2, 3: 1}.get(button, 0)
        event = _cg.CGEventCreateMouseEvent(None, etype, pt, cg_button)
        if event:
            _cg.CGEventPost(kCGHIDEventTap, event)
            _cf.CFRelease(event)

    def inject_mouse_scroll(self, dx: int = 0, dy: int = 0):
        # CGEventCreateScrollWheelEvent(source, units, wheelCount, dy, dx)
        event = _cg.CGEventCreateScrollWheelEvent(
            None, kCGScrollEventUnitPixel, 2,
            ctypes.c_int32(dy * 10),   # scale for reasonable speed
            ctypes.c_int32(dx * 10),
        )
        if event:
            _cg.CGEventPost(kCGHIDEventTap, event)
            _cf.CFRelease(event)

    def inject_key(self, scancode: int, pressed: bool):
        mac_kc = hid_to_mac(scancode)
        if mac_kc < 0:
            return
        event = _cg.CGEventCreateKeyboardEvent(None, mac_kc, pressed)
        if event:
            _cg.CGEventPost(kCGHIDEventTap, event)
            _cf.CFRelease(event)

    # ── Keyboard capture (CGEventTap) ────────────────────────────

    def start_key_capture(self, on_key: Callable[[int, bool], None]) -> bool:
        if self._tap_running:
            return True
        self._on_key = on_key

        mask = (
            (1 << kCGEventKeyDown) |
            (1 << kCGEventKeyUp)
        )

        def tap_callback(proxy, etype, event, user_info):
            if not self._on_key:
                return event
            if etype in (kCGEventKeyDown, kCGEventKeyUp):
                mac_kc = _cg.CGEventGetIntegerValueField(
                    event, kCGKeyboardEventKeycode,
                )
                hid = mac_to_hid(mac_kc)
                if hid:
                    pressed = (etype == kCGEventKeyDown)
                    self._on_key(hid, pressed)
            return event

        self._tap_callback_ref = CGEventTapCallBack(tap_callback)
        self._tap_port = _cg.CGEventTapCreate(
            kCGHIDEventTap,
            kCGHeadInsertEventTap,
            kCGEventTapOptionDefault,
            ctypes.c_uint64(mask),
            self._tap_callback_ref,
            None,
        )
        if not self._tap_port:
            log.error("Failed to create CGEventTap. "
                      "Grant Accessibility permissions in "
                      "System Settings → Privacy & Security.")
            return False

        _cg.CGEventTapEnable(self._tap_port, True)

        self._tap_source = _cf.CFMachPortCreateRunLoopSource(
            None, self._tap_port, 0,
        )

        self._tap_running = True
        self._tap_thread = threading.Thread(
            target=self._tap_loop, daemon=True, name="mac-eventap",
        )
        self._tap_thread.start()
        return True

    def stop_key_capture(self):
        self._tap_running = False
        self._on_key = None
        if self._tap_runloop:
            _cf.CFRunLoopStop(self._tap_runloop)
        if self._tap_port:
            _cg.CGEventTapEnable(self._tap_port, False)
            self._tap_port = None

    def _tap_loop(self):
        """Run the CFRunLoop for the event tap."""
        rl = _cf.CFRunLoopGetCurrent()
        self._tap_runloop = rl
        _cf.CFRunLoopAddSource(rl, self._tap_source,
                               _kCFRunLoopCommonModes)
        _cf.CFRunLoopRun()
        self._tap_runloop = None

    # ── Clipboard ────────────────────────────────────────────────

    def get_clipboard(self) -> str:
        try:
            r = subprocess.run(
                ["pbpaste"], capture_output=True, text=True, timeout=2,
            )
            return r.stdout if r.returncode == 0 else ""
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return ""

    def set_clipboard(self, text: str):
        try:
            subprocess.run(
                ["pbcopy"], input=text, text=True, timeout=2,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass

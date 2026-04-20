"""DXGI Desktop Duplication capture — Windows, exclusion-aware.

Replaces ``mss`` on Windows for the source-side capture loop.
``mss`` uses GDI ``BitBlt`` which ignores ``WDA_EXCLUDEFROMCAPTURE``
entirely — every window ends up in the captured pixels whether we
flagged it or not. DXGI Desktop Duplication, by contrast, reads
the DWM's composited surface through the modern display pipeline
AND honours the flag: any window whose HWND has been marked with
``SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`` is
rendered as transparent in the duplicated frame, so our own
StreamWindow overlays don't appear in what we send to sinks.

That closes the feedback loop we saw on Diana: Linux routed a
swap through, Windows's overlay showed Linux's desktop, Windows's
capture picked up its own overlay (via mss), streamed it back to
Linux, Linux's overlay showed that, cascaded to black.

Design:
  * ``DxgiCapture.open()`` bootstraps the DXGI factory → adapter →
    output chain and creates a D3D11 device + Desktop Duplication
    interface. Returns False and falls through to mss when any
    step fails (wrong platform, no D3D11, running in a session
    that can't access the desktop).
  * ``DxgiCapture.grab(rect)`` returns a PIL.Image cropped to
    ``rect`` (same contract as the existing mss callable the
    capture loop already uses).
  * Each grab acquires the next frame, copies it into a staging
    texture, maps it to CPU-accessible memory, and copies the
    bytes into a PIL.Image. The staging texture is re-used across
    frames so we're not reallocating GPU memory on every tick.
  * When no frame is ready within a short timeout we re-use the
    last successfully-grabbed image — DXGI only delivers frames
    when the desktop actually changes, which is what we want
    anyway (it's essentially free dirty-rect detection).

All WinAPI / D3D11 / DXGI calls go through ctypes + manually-
constructed COM vtables. No WinRT projection, no extra runtime
dependency beyond what's already on every Win10 2004+ install.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import sys
from ctypes import (
    HRESULT, POINTER, Structure, c_int, c_uint, c_uint32, c_ulong,
    c_void_p, byref,
)
from typing import Optional

log = logging.getLogger(__name__)


# ── COM + DXGI constants ────────────────────────────────────────────


def _guid(s: str) -> "GUID":
    """Parse a string-form GUID into the ctypes struct."""
    g = GUID()
    parts = s.replace("{", "").replace("}", "").split("-")
    g.Data1 = int(parts[0], 16)
    g.Data2 = int(parts[1], 16)
    g.Data3 = int(parts[2], 16)
    d4 = int(parts[3], 16)
    g.Data4[0] = (d4 >> 8) & 0xFF
    g.Data4[1] = d4 & 0xFF
    tail = int(parts[4], 16)
    for i in range(6):
        g.Data4[7 - i] = (tail >> (i * 8)) & 0xFF
    return g


class GUID(Structure):
    _fields_ = [
        ("Data1", c_uint),
        ("Data2", c_uint),
        ("Data3", c_uint),
        ("Data4", ctypes.c_ubyte * 8),
    ]


IID_IDXGIFactory1 = _guid("770aae78-f26f-4dba-a829-253c83d1b387")
IID_IDXGIOutput1 = _guid("00cddea8-939b-4b83-a340-a685226666cc")
IID_ID3D11Texture2D = _guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c")

# D3D11 driver types / feature levels
D3D_DRIVER_TYPE_HARDWARE = 1
D3D11_SDK_VERSION = 7
D3D_FEATURE_LEVEL_11_0 = 0xb000
D3D_FEATURE_LEVEL_10_1 = 0xa100
D3D_FEATURE_LEVEL_10_0 = 0xa000
D3D11_USAGE_STAGING = 3
D3D11_CPU_ACCESS_READ = 0x20000
DXGI_FORMAT_B8G8R8A8_UNORM = 87

# Error codes
DXGI_ERROR_WAIT_TIMEOUT = 0x887A0027 - 0x100000000   # signed
DXGI_ERROR_ACCESS_LOST = 0x887A0026 - 0x100000000


# ── Structures ──────────────────────────────────────────────────────


class DXGI_OUTPUT_DESC(Structure):
    _fields_ = [
        ("DeviceName", wt.WCHAR * 32),
        ("DesktopCoordinates", wt.RECT),
        ("AttachedToDesktop", wt.BOOL),
        ("Rotation", c_uint),
        ("Monitor", c_void_p),
    ]


class DXGI_OUTDUPL_FRAME_INFO(Structure):
    _fields_ = [
        ("LastPresentTime", ctypes.c_int64),
        ("LastMouseUpdateTime", ctypes.c_int64),
        ("AccumulatedFrames", c_uint),
        ("RectsCoalesced", wt.BOOL),
        ("ProtectedContentMaskedOut", wt.BOOL),
        ("PointerPosition", ctypes.c_byte * 24),
        ("TotalMetadataBufferSize", c_uint),
        ("PointerShapeBufferSize", c_uint),
    ]


class D3D11_TEXTURE2D_DESC(Structure):
    _fields_ = [
        ("Width", c_uint),
        ("Height", c_uint),
        ("MipLevels", c_uint),
        ("ArraySize", c_uint),
        ("Format", c_uint),
        ("SampleDesc_Count", c_uint),
        ("SampleDesc_Quality", c_uint),
        ("Usage", c_uint),
        ("BindFlags", c_uint),
        ("CPUAccessFlags", c_uint),
        ("MiscFlags", c_uint),
    ]


class D3D11_MAPPED_SUBRESOURCE(Structure):
    _fields_ = [
        ("pData", c_void_p),
        ("RowPitch", c_uint),
        ("DepthPitch", c_uint),
    ]


# ── Lightweight COM helpers ─────────────────────────────────────────


def _call_vtbl(obj: int, idx: int, argtypes, restype, *args):
    """Invoke vtable[idx] on a COM interface pointer. The pointer
    we got from CoCreateInstance / the DXGI / D3D factory is
    actually a pointer to a pointer to a vtable; each vtable entry
    is a function pointer we build with ctypes.CFUNCTYPE."""
    vtbl_ptr = ctypes.cast(obj, POINTER(c_void_p))[0]
    fn_ptr = ctypes.cast(vtbl_ptr, POINTER(c_void_p))[idx]
    fn = ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)(fn_ptr)
    return fn(obj, *args)


def _com_release(obj) -> None:
    if not obj:
        return
    try:
        _call_vtbl(obj, 2, (), c_ulong)
    except Exception:
        pass


# ── Public capture class ────────────────────────────────────────────


class DxgiCapture:
    """DXGI Desktop Duplication capture. Lives for the lifetime of
    one source monitor — each source (physical panel) keeps its
    own DxgiCapture instance so we don't re-enumerate DXGI outputs
    on every grab.

    Thread-model: meant to be driven from the capture-loop thread.
    The D3D11 device + duplication interface are free-threaded
    enough for a single consumer; don't share one instance across
    threads."""

    def __init__(self) -> None:
        self.device = None              # ID3D11Device*
        self.context = None             # ID3D11DeviceContext*
        self.duplication = None         # IDXGIOutputDuplication*
        self.staging = None             # ID3D11Texture2D* (staging)
        self.width = 0
        self.height = 0
        self.output_left = 0
        self.output_top = 0
        self._last_img = None           # PIL.Image cache

    # ── Open / close ────────────────────────────────────────────

    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        try:
            return self._open_dxgi()
        except Exception:
            log.exception("DXGI capture init failed")
            self.close()
            return False

    def _open_dxgi(self) -> bool:
        log.info("DXGI step 1: loading dxgi.dll + d3d11.dll")
        dxgi = ctypes.WinDLL("dxgi")
        d3d11 = ctypes.WinDLL("d3d11")

        # CreateDXGIFactory1
        log.info("DXGI step 2: CreateDXGIFactory1")
        dxgi.CreateDXGIFactory1.argtypes = [
            POINTER(GUID), POINTER(c_void_p)]
        dxgi.CreateDXGIFactory1.restype = HRESULT
        factory = c_void_p()
        hr = dxgi.CreateDXGIFactory1(byref(IID_IDXGIFactory1),
                                     byref(factory))
        log.info("DXGI step 2 result: hr=0x%x factory=0x%x",
                 hr & 0xFFFFFFFF, factory.value or 0)
        if hr != 0 or not factory.value:
            log.info("CreateDXGIFactory1 failed hr=0x%x", hr & 0xFFFFFFFF)
            return False
        try:
            log.info("DXGI step 3: EnumAdapters1(0)")
            adapter = c_void_p()
            # IDXGIFactory1::EnumAdapters1 == vtable index 12
            hr = _call_vtbl(
                factory.value, 12,
                (c_uint, POINTER(c_void_p)),
                HRESULT, 0, byref(adapter),
            )
            log.info("DXGI step 3 result: hr=0x%x adapter=0x%x",
                     hr & 0xFFFFFFFF, adapter.value or 0)
            if hr != 0 or not adapter.value:
                log.info("EnumAdapters1 failed hr=0x%x", hr & 0xFFFFFFFF)
                return False
            try:
                log.info("DXGI step 4: EnumOutputs(0) on adapter")
                # IDXGIAdapter1::EnumOutputs = vtable index 7
                output = c_void_p()
                hr = _call_vtbl(
                    adapter.value, 7,
                    (c_uint, POINTER(c_void_p)),
                    HRESULT, 0, byref(output),
                )
                log.info("DXGI step 4 result: hr=0x%x output=0x%x",
                         hr & 0xFFFFFFFF, output.value or 0)
                if hr != 0 or not output.value:
                    log.info("EnumOutputs failed hr=0x%x", hr & 0xFFFFFFFF)
                    return False
                try:
                    log.info("DXGI step 5: QueryInterface IDXGIOutput1")
                    # Upgrade to IDXGIOutput1 for DuplicateOutput.
                    output1 = c_void_p()
                    # IUnknown::QueryInterface = vtable index 0
                    hr = _call_vtbl(
                        output.value, 0,
                        (POINTER(GUID), POINTER(c_void_p)),
                        HRESULT,
                        byref(IID_IDXGIOutput1), byref(output1),
                    )
                    log.info("DXGI step 5 result: hr=0x%x output1=0x%x",
                             hr & 0xFFFFFFFF, output1.value or 0)
                    if hr != 0 or not output1.value:
                        log.info("QueryInterface IDXGIOutput1 "
                                 "failed hr=0x%x", hr & 0xFFFFFFFF)
                        return False
                    try:
                        log.info("DXGI step 6: IDXGIOutput::GetDesc")
                        # Get output desc so we know the rect.
                        desc = DXGI_OUTPUT_DESC()
                        # IDXGIOutput::GetDesc = vtable index 7
                        hr = _call_vtbl(
                            output1.value, 7,
                            (POINTER(DXGI_OUTPUT_DESC),),
                            HRESULT, byref(desc),
                        )
                        self.output_left = int(desc.DesktopCoordinates.left)
                        self.output_top = int(desc.DesktopCoordinates.top)
                        self.width = int(desc.DesktopCoordinates.right
                                         - desc.DesktopCoordinates.left)
                        self.height = int(desc.DesktopCoordinates.bottom
                                          - desc.DesktopCoordinates.top)
                        log.info("DXGI step 6 result: hr=0x%x "
                                 "rect=(%d,%d)+%dx%d",
                                 hr & 0xFFFFFFFF,
                                 self.output_left, self.output_top,
                                 self.width, self.height)

                        log.info("DXGI step 7: D3D11CreateDevice")
                        # Create D3D11 device.
                        d3d11.D3D11CreateDevice.argtypes = [
                            c_void_p, c_uint, c_void_p, c_uint,
                            POINTER(c_uint), c_uint, c_uint,
                            POINTER(c_void_p), POINTER(c_uint),
                            POINTER(c_void_p),
                        ]
                        d3d11.D3D11CreateDevice.restype = HRESULT
                        levels = (c_uint * 3)(
                            D3D_FEATURE_LEVEL_11_0,
                            D3D_FEATURE_LEVEL_10_1,
                            D3D_FEATURE_LEVEL_10_0,
                        )
                        device = c_void_p()
                        context = c_void_p()
                        chosen = c_uint()
                        hr = d3d11.D3D11CreateDevice(
                            adapter.value,
                            0,  # UNKNOWN — adapter dictates
                            None, 0,
                            levels, 3,
                            D3D11_SDK_VERSION,
                            byref(device),
                            byref(chosen),
                            byref(context),
                        )
                        log.info("DXGI step 7 result: hr=0x%x "
                                 "device=0x%x feature_level=0x%x",
                                 hr & 0xFFFFFFFF, device.value or 0,
                                 chosen.value)
                        if hr != 0 or not device.value:
                            log.info("D3D11CreateDevice failed hr=0x%x",
                                     hr & 0xFFFFFFFF)
                            return False
                        self.device = device.value
                        self.context = context.value

                        log.info("DXGI step 8: "
                                 "IDXGIOutput1::DuplicateOutput")
                        # DuplicateOutput: IDXGIOutput1::DuplicateOutput
                        # = vtable index 22
                        dup = c_void_p()
                        hr = _call_vtbl(
                            output1.value, 22,
                            (c_void_p, POINTER(c_void_p)),
                            HRESULT, self.device, byref(dup),
                        )
                        log.info("DXGI step 8 result: hr=0x%x dup=0x%x",
                                 hr & 0xFFFFFFFF, dup.value or 0)
                        if hr != 0 or not dup.value:
                            log.info("DuplicateOutput failed hr=0x%x "
                                     "(E_ACCESSDENIED=0x80070005, "
                                     "E_INVALIDARG=0x80070057, "
                                     "DXGI_ERROR_UNSUPPORTED=0x887A0004)",
                                     hr & 0xFFFFFFFF)
                            return False
                        self.duplication = dup.value

                        log.info("DXGI step 9: CreateTexture2D(staging)")
                        # Staging texture for CPU readback.
                        desc_t = D3D11_TEXTURE2D_DESC(
                            Width=self.width, Height=self.height,
                            MipLevels=1, ArraySize=1,
                            Format=DXGI_FORMAT_B8G8R8A8_UNORM,
                            SampleDesc_Count=1, SampleDesc_Quality=0,
                            Usage=D3D11_USAGE_STAGING,
                            BindFlags=0,
                            CPUAccessFlags=D3D11_CPU_ACCESS_READ,
                            MiscFlags=0,
                        )
                        staging = c_void_p()
                        # ID3D11Device::CreateTexture2D = vtable index 5
                        hr = _call_vtbl(
                            self.device, 5,
                            (POINTER(D3D11_TEXTURE2D_DESC),
                             c_void_p, POINTER(c_void_p)),
                            HRESULT,
                            byref(desc_t), None, byref(staging),
                        )
                        log.info("DXGI step 9 result: hr=0x%x staging=0x%x",
                                 hr & 0xFFFFFFFF, staging.value or 0)
                        if hr != 0 or not staging.value:
                            log.info("CreateTexture2D(staging) "
                                     "failed hr=0x%x", hr & 0xFFFFFFFF)
                            return False
                        self.staging = staging.value
                        log.info("DxgiCapture ready: %dx%d @ (%d,%d)",
                                 self.width, self.height,
                                 self.output_left, self.output_top)
                        return True
                    finally:
                        _com_release(output1.value)
                finally:
                    _com_release(output.value)
            finally:
                _com_release(adapter.value)
        finally:
            _com_release(factory.value)

    def close(self) -> None:
        for ref in ("duplication", "staging", "context", "device"):
            val = getattr(self, ref, None)
            if val:
                _com_release(val)
                setattr(self, ref, None)

    # ── Grab ─────────────────────────────────────────────────────

    def grab(self, rect: dict):
        """Grab one frame of the output, cropped to ``rect``
        (keys: x / y / width / height). Returns a PIL.Image in
        RGB mode or None on error."""
        from PIL import Image
        if not self.duplication:
            return None
        frame_info = DXGI_OUTDUPL_FRAME_INFO()
        resource = c_void_p()
        # IDXGIOutputDuplication::AcquireNextFrame = vtable idx 8
        hr = _call_vtbl(
            self.duplication, 8,
            (c_uint,
             POINTER(DXGI_OUTDUPL_FRAME_INFO),
             POINTER(c_void_p)),
            HRESULT,
            50,                           # timeout ms
            byref(frame_info), byref(resource),
        )
        if hr == DXGI_ERROR_WAIT_TIMEOUT or (hr & 0xFFFFFFFF) == \
                (DXGI_ERROR_WAIT_TIMEOUT & 0xFFFFFFFF):
            # No new frame; return the previous one so sinks keep
            # flowing at a steady rate.
            return self._last_img
        if hr != 0:
            log.info("AcquireNextFrame hr=0x%x", hr & 0xFFFFFFFF)
            if (hr & 0xFFFFFFFF) == (DXGI_ERROR_ACCESS_LOST & 0xFFFFFFFF):
                # Device lost — reopen on the next call.
                self.close()
            return self._last_img

        try:
            # QueryInterface resource → ID3D11Texture2D
            tex = c_void_p()
            qi = _call_vtbl(
                resource.value, 0,
                (POINTER(GUID), POINTER(c_void_p)),
                HRESULT,
                byref(IID_ID3D11Texture2D), byref(tex),
            )
            if qi != 0 or not tex.value:
                return self._last_img
            try:
                # CopyResource(staging, tex)
                # ID3D11DeviceContext::CopyResource = vtable index 47
                _call_vtbl(
                    self.context, 47,
                    (c_void_p, c_void_p),
                    None, self.staging, tex.value,
                )
            finally:
                _com_release(tex.value)

            # Map(staging)
            mapped = D3D11_MAPPED_SUBRESOURCE()
            # ID3D11DeviceContext::Map = vtable index 14
            hr = _call_vtbl(
                self.context, 14,
                (c_void_p, c_uint, c_uint, c_uint,
                 POINTER(D3D11_MAPPED_SUBRESOURCE)),
                HRESULT,
                self.staging, 0, 1, 0, byref(mapped),
            )
            if hr != 0 or not mapped.pData:
                return self._last_img
            try:
                stride = mapped.RowPitch
                buf = (ctypes.c_ubyte * (stride * self.height)) \
                    .from_address(mapped.pData)
                img = Image.frombuffer(
                    "RGBA", (self.width, self.height),
                    bytes(buf),
                    "raw", "BGRA", stride, 1,
                ).convert("RGB")
                if rect:
                    x = int(rect.get("x", 0)) - self.output_left
                    y = int(rect.get("y", 0)) - self.output_top
                    w = int(rect.get("width", self.width))
                    h = int(rect.get("height", self.height))
                    x = max(0, min(x, self.width))
                    y = max(0, min(y, self.height))
                    img = img.crop((x, y, x + w, y + h))
                self._last_img = img
                return img
            finally:
                # ID3D11DeviceContext::Unmap = vtable index 15
                _call_vtbl(self.context, 15,
                           (c_void_p, c_uint),
                           None, self.staging, 0)
        finally:
            _com_release(resource.value)
            # IDXGIOutputDuplication::ReleaseFrame = vtable index 14
            _call_vtbl(self.duplication, 14, (), HRESULT)


def available() -> bool:
    if sys.platform != "win32":
        return False
    try:
        ctypes.WinDLL("dxgi")
        ctypes.WinDLL("d3d11")
        return True
    except OSError:
        return False

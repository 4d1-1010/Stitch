"""Windows.Graphics.Capture (WGC) screen capture via WinRT/ctypes.

WGC is the only Windows capture API that honours
``SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`` as "see-through":
BitBlt and DXGI Desktop Duplication replace the excluded window's
pixels with black. Our ``StreamWindow`` overlay has that affinity set
on its HWND, so WGC captures the desktop pixels *underneath* the
overlay — exactly the feedback-free stream we need for display swap.

This module is a hand-rolled COM/WinRT binding using raw ``ctypes``
so we avoid pulling in ``comtypes`` / ``winrt`` / ``python-windows-
capture``. Interfaces used:

* ``IGraphicsCaptureItemInterop`` — activation factory for
  ``GraphicsCaptureItem``; we call ``CreateForMonitor`` on it.
* ``IGraphicsCaptureItem``       — the captured surface (a monitor).
* ``IDirect3D11CaptureFramePoolStatics2.CreateFreeThreaded`` — a
  frame pool that doesn't require a DispatcherQueue, so we can poll
  it from a plain worker thread.
* ``IDirect3D11CaptureFramePool``      — queues captured frames.
* ``IGraphicsCaptureSession`` (+2/+3)  — starts the capture; v2
  controls cursor capture, v3 controls the yellow border (22H2+).
* ``IDirect3DDxgiInterfaceAccess``     — unwraps the WinRT
  ``IDirect3DSurface`` into an ``ID3D11Texture2D`` we can copy into a
  staging texture and ``Map`` for CPU read.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import sys
import threading
import time

log = logging.getLogger(__name__)


# ─── GUIDs / IIDs ─────────────────────────────────────────────────
class _GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_ubyte * 8),
    ]


def _guid(s: str) -> _GUID:
    parts = s.split("-")
    g = _GUID()
    g.Data1 = int(parts[0], 16)
    g.Data2 = int(parts[1], 16)
    g.Data3 = int(parts[2], 16)
    rest = parts[3] + parts[4]
    for i in range(8):
        g.Data4[i] = int(rest[i * 2:i * 2 + 2], 16)
    return g


_IID_IGraphicsCaptureItemInterop = _guid(
    "3628E81B-3CAC-4C60-B7F4-23CE0E0C3356")
_IID_IGraphicsCaptureItem = _guid(
    "79C3F95B-31F7-4EC2-A464-632EF5D30760")
_IID_IGraphicsCaptureSessionStatics = _guid(
    "2224A540-5974-52EE-8147-B30CF83AEEB8")
_IID_IGraphicsCaptureSession = _guid(
    "814E42A9-F70F-4AD7-939B-FDDCC6EB880D")
_IID_IGraphicsCaptureSession2 = _guid(
    "2C39AE40-7D2E-5044-804E-8B6799D4CF9E")
_IID_IGraphicsCaptureSession3 = _guid(
    "F2CDD964-2824-5A3F-A375-E09EE3C3A6E1")
_IID_IDirect3D11CaptureFramePoolStatics2 = _guid(
    "589B103F-6BBC-5DF5-A991-02E28BE5F4BF")
_IID_IDirect3D11CaptureFramePool = _guid(
    "24EB6D22-1975-422E-82E7-780DBD8DDF24")
_IID_IDirect3D11CaptureFrame = _guid(
    "FA50C623-38DA-4B32-B466-CDA3A13DFF5D")
_IID_IDirect3DDevice = _guid(
    "A37624AB-8D5F-4650-9D3E-9EAE3D9BC416")
_IID_IDirect3DDxgiInterfaceAccess = _guid(
    "A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")
_IID_IDXGIDevice = _guid(
    "54EC77FA-1377-44E6-8C32-88FD5F44C84C")
_IID_ID3D11Texture2D = _guid(
    "6F15AAF2-D208-4E89-9AB4-489535D34F9C")


# ─── Win32 / D3D11 / WinRT constants ──────────────────────────────
RO_INIT_MULTITHREADED = 1
S_OK = 0

D3D_DRIVER_TYPE_HARDWARE = 1
D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20
D3D11_SDK_VERSION = 7
D3D11_USAGE_STAGING = 3
D3D11_CPU_ACCESS_READ = 0x20000
D3D11_MAP_READ = 1

DXGI_FORMAT_B8G8R8A8_UNORM = 87
DIRECTX_PIXEL_FORMAT_B8G8R8A8_UINT_NORMALIZED = 87

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79

MONITOR_DEFAULTTOPRIMARY = 1


# ─── Structs ──────────────────────────────────────────────────────
class _SizeInt32(ctypes.Structure):
    _fields_ = [("Width", ctypes.c_int32),
                ("Height", ctypes.c_int32)]


class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class _DXGI_SAMPLE_DESC(ctypes.Structure):
    _fields_ = [("Count", ctypes.c_uint32),
                ("Quality", ctypes.c_uint32)]


class _D3D11_TEXTURE2D_DESC(ctypes.Structure):
    _fields_ = [
        ("Width", ctypes.c_uint32),
        ("Height", ctypes.c_uint32),
        ("MipLevels", ctypes.c_uint32),
        ("ArraySize", ctypes.c_uint32),
        ("Format", ctypes.c_uint32),
        ("SampleDesc", _DXGI_SAMPLE_DESC),
        ("Usage", ctypes.c_uint32),
        ("BindFlags", ctypes.c_uint32),
        ("CPUAccessFlags", ctypes.c_uint32),
        ("MiscFlags", ctypes.c_uint32),
    ]


class _D3D11_MAPPED_SUBRESOURCE(ctypes.Structure):
    _fields_ = [
        ("pData", ctypes.c_void_p),
        ("RowPitch", ctypes.c_uint32),
        ("DepthPitch", ctypes.c_uint32),
    ]


# ─── DLL bindings (lazy) ──────────────────────────────────────────
_combase = None
_d3d11 = None
_user32 = None


def _load() -> bool:
    global _combase, _d3d11, _user32
    if _combase is not None and _d3d11 is not None:
        return True
    try:
        combase = ctypes.WinDLL("combase", use_last_error=True)
        d3d11 = ctypes.WinDLL("d3d11", use_last_error=True)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
    except OSError:
        return False

    combase.RoInitialize.argtypes = [ctypes.c_uint32]
    combase.RoInitialize.restype = ctypes.c_long
    combase.RoUninitialize.argtypes = []
    combase.RoUninitialize.restype = None
    combase.RoGetActivationFactory.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(_GUID),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    combase.RoGetActivationFactory.restype = ctypes.c_long
    combase.WindowsCreateString.argtypes = [
        ctypes.c_wchar_p, ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    combase.WindowsCreateString.restype = ctypes.c_long
    combase.WindowsDeleteString.argtypes = [ctypes.c_void_p]
    combase.WindowsDeleteString.restype = ctypes.c_long

    d3d11.D3D11CreateDevice.argtypes = [
        ctypes.c_void_p,                         # pAdapter
        ctypes.c_uint32,                         # DriverType
        ctypes.c_void_p,                         # Software
        ctypes.c_uint32,                         # Flags
        ctypes.c_void_p,                         # pFeatureLevels
        ctypes.c_uint32,                         # FeatureLevels
        ctypes.c_uint32,                         # SDKVersion
        ctypes.POINTER(ctypes.c_void_p),         # ppDevice
        ctypes.c_void_p,                         # pFeatureLevel (out)
        ctypes.POINTER(ctypes.c_void_p),         # ppImmediateContext
    ]
    d3d11.D3D11CreateDevice.restype = ctypes.c_long
    d3d11.CreateDirect3D11DeviceFromDXGIDevice.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    d3d11.CreateDirect3D11DeviceFromDXGIDevice.restype = ctypes.c_long

    user32.GetSystemMetrics.argtypes = [ctypes.c_int]
    user32.GetSystemMetrics.restype = ctypes.c_int
    user32.MonitorFromPoint.argtypes = [_POINT, ctypes.c_uint32]
    user32.MonitorFromPoint.restype = ctypes.c_void_p

    _combase = combase
    _d3d11 = d3d11
    _user32 = user32
    return True


# ─── COM vtable helpers ──────────────────────────────────────────
def _vcall(ptr, idx, restype, argtypes, *args):
    """Call method #idx on the COM vtable of ``ptr`` with ``(this,
    *args)``. Works for IUnknown / IInspectable / their derivatives
    on x64 Windows (stdcall / fastcall WINAPI)."""
    if not ptr:
        raise OSError("COM null pointer")
    vtable = ctypes.cast(
        ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    fn_addr = vtable[idx]
    proto = ctypes.WINFUNCTYPE(
        restype if restype is not None else None,
        ctypes.c_void_p, *argtypes)
    fn = proto(fn_addr)
    return fn(ptr, *args)


def _release(ptr) -> None:
    if not ptr:
        return
    try:
        _vcall(ptr, 2, ctypes.c_ulong, [])
    except Exception:
        pass


def _query(ptr, iid: _GUID):
    out = ctypes.c_void_p()
    hr = _vcall(
        ptr, 0, ctypes.c_long,
        [ctypes.POINTER(_GUID), ctypes.POINTER(ctypes.c_void_p)],
        ctypes.byref(iid), ctypes.byref(out),
    )
    if hr != S_OK:
        raise OSError(
            f"QueryInterface failed hr=0x{hr & 0xFFFFFFFF:08x}")
    return out.value


def _hstring(s: str) -> ctypes.c_void_p:
    out = ctypes.c_void_p()
    hr = _combase.WindowsCreateString(s, len(s), ctypes.byref(out))
    if hr != S_OK:
        raise OSError(
            f"WindowsCreateString failed hr=0x{hr & 0xFFFFFFFF:08x}")
    return out


def _activation_factory(class_name: str, iid: _GUID):
    hs = _hstring(class_name)
    try:
        out = ctypes.c_void_p()
        hr = _combase.RoGetActivationFactory(
            hs, ctypes.byref(iid), ctypes.byref(out))
        if hr != S_OK:
            raise OSError(
                f"RoGetActivationFactory({class_name}) hr="
                f"0x{hr & 0xFFFFFFFF:08x}")
        return out.value
    finally:
        _combase.WindowsDeleteString(hs)


# ─── Main capture class ──────────────────────────────────────────
class WGCCapture:
    """Screen capture via Windows.Graphics.Capture.

    ``WDA_EXCLUDEFROMCAPTURE`` windows are rendered as see-through in
    captured frames (unlike BitBlt/DXGI, which paint them black). We
    bind the capture to the primary monitor — multi-monitor selection
    can be added when display_stream starts requesting specific
    HMONITORs. The grab path pulls frames with ``TryGetNextFrame``,
    unwraps them to an ``ID3D11Texture2D``, copies to a reusable
    staging texture, and maps it for CPU read into a PIL image.
    """

    def __init__(self) -> None:
        self._ro_init_ok = False
        self._d3d_device = None          # ID3D11Device*
        self._d3d_context = None         # ID3D11DeviceContext*
        self._winrt_device = None        # IDirect3DDevice* (WinRT)
        self._item = None                # IGraphicsCaptureItem*
        self._frame_pool = None          # IDirect3D11CaptureFramePool*
        self._session = None             # IGraphicsCaptureSession*
        self._staging = None             # ID3D11Texture2D* (staging)
        self._staging_w = 0
        self._staging_h = 0
        self.width = 0
        self.height = 0
        self.origin_x = 0
        self.origin_y = 0
        self._last_image = None
        self._last_log_at = 0.0
        self._lock = threading.Lock()

    # ── Lifecycle ───────────────────────────────────────────────
    def open(self) -> bool:
        if sys.platform != "win32":
            return False
        if not _load():
            return False
        try:
            ok = self._open_impl()
            if not ok:
                self.close()
            return ok
        except Exception:
            log.exception("WGC capture init failed")
            self.close()
            return False

    def _open_impl(self) -> bool:
        # RoInitialize on this thread (capture loop thread). Already-
        # initialized threads return S_FALSE (non-negative = success).
        hr = _combase.RoInitialize(RO_INIT_MULTITHREADED)
        if hr < 0:
            log.info("RoInitialize failed hr=0x%x",
                     hr & 0xFFFFFFFF)
            return False
        self._ro_init_ok = True

        if not self._is_supported():
            log.info("GraphicsCaptureSession.IsSupported = false")
            return False

        self.origin_x = _user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
        self.origin_y = _user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
        self.width = _user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
        self.height = _user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)

        # D3D11 device + context. BGRA support is required for the
        # capture-item surface format.
        device_pp = ctypes.c_void_p()
        ctx_pp = ctypes.c_void_p()
        hr = _d3d11.D3D11CreateDevice(
            None, D3D_DRIVER_TYPE_HARDWARE, None,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            None, 0, D3D11_SDK_VERSION,
            ctypes.byref(device_pp), None, ctypes.byref(ctx_pp),
        )
        if hr != S_OK or not device_pp.value:
            log.info("D3D11CreateDevice hr=0x%x",
                     hr & 0xFFFFFFFF)
            return False
        self._d3d_device = device_pp.value
        self._d3d_context = ctx_pp.value

        # Wrap the D3D11 device in a WinRT IDirect3DDevice for WGC.
        dxgi_device = _query(self._d3d_device, _IID_IDXGIDevice)
        try:
            insp = ctypes.c_void_p()
            hr = _d3d11.CreateDirect3D11DeviceFromDXGIDevice(
                dxgi_device, ctypes.byref(insp))
            if hr != S_OK or not insp.value:
                log.info(
                    "CreateDirect3D11DeviceFromDXGIDevice hr=0x%x",
                    hr & 0xFFFFFFFF)
                return False
            try:
                self._winrt_device = _query(
                    insp.value, _IID_IDirect3DDevice)
            finally:
                _release(insp.value)
        finally:
            _release(dxgi_device)

        # Monitor the overlay's origin lives on (we capture the
        # primary monitor — multi-monitor picking happens upstream).
        pt = _POINT(self.origin_x + 1, self.origin_y + 1)
        hmonitor = _user32.MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY)
        if not hmonitor:
            log.info("MonitorFromPoint returned NULL")
            return False
        interop = _activation_factory(
            "Windows.Graphics.Capture.GraphicsCaptureItem",
            _IID_IGraphicsCaptureItemInterop,
        )
        try:
            item_p = ctypes.c_void_p()
            hr = _vcall(
                interop, 4, ctypes.c_long,
                [ctypes.c_void_p, ctypes.POINTER(_GUID),
                 ctypes.POINTER(ctypes.c_void_p)],
                hmonitor,
                ctypes.byref(_IID_IGraphicsCaptureItem),
                ctypes.byref(item_p),
            )
            if hr != S_OK or not item_p.value:
                log.info("Interop.CreateForMonitor hr=0x%x",
                         hr & 0xFFFFFFFF)
                return False
            self._item = item_p.value
        finally:
            _release(interop)

        # Monitor size from the capture item (matches physical pixels).
        size = _SizeInt32()
        hr = _vcall(
            self._item, 7, ctypes.c_long,
            [ctypes.POINTER(_SizeInt32)], ctypes.byref(size),
        )
        if hr != S_OK:
            log.info("item.get_Size hr=0x%x", hr & 0xFFFFFFFF)
            return False

        # FramePool.CreateFreeThreaded — no DispatcherQueue needed,
        # we poll TryGetNextFrame from the capture loop thread.
        pool_statics = _activation_factory(
            "Windows.Graphics.Capture.Direct3D11CaptureFramePool",
            _IID_IDirect3D11CaptureFramePoolStatics2,
        )
        try:
            pool_p = ctypes.c_void_p()
            hr = _vcall(
                pool_statics, 6, ctypes.c_long,
                [ctypes.c_void_p, ctypes.c_int32,
                 ctypes.c_int32, _SizeInt32,
                 ctypes.POINTER(ctypes.c_void_p)],
                self._winrt_device,
                DIRECTX_PIXEL_FORMAT_B8G8R8A8_UINT_NORMALIZED,
                2, size, ctypes.byref(pool_p),
            )
            if hr != S_OK or not pool_p.value:
                log.info("FramePool.CreateFreeThreaded hr=0x%x",
                         hr & 0xFFFFFFFF)
                return False
            self._frame_pool = pool_p.value
        finally:
            _release(pool_statics)

        # CreateCaptureSession(item) → IGraphicsCaptureSession.
        session_p = ctypes.c_void_p()
        hr = _vcall(
            self._frame_pool, 10, ctypes.c_long,
            [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)],
            self._item, ctypes.byref(session_p),
        )
        if hr != S_OK or not session_p.value:
            log.info("CreateCaptureSession hr=0x%x",
                     hr & 0xFFFFFFFF)
            return False
        self._session = session_p.value

        # Drop the hardware cursor (our peer layer forwards it
        # separately) and kill the yellow capture border on 22H2+.
        try:
            s2 = _query(self._session, _IID_IGraphicsCaptureSession2)
            try:
                _vcall(s2, 7, ctypes.c_long, [ctypes.c_byte], 0)
            finally:
                _release(s2)
        except Exception:
            pass
        try:
            s3 = _query(self._session, _IID_IGraphicsCaptureSession3)
            try:
                _vcall(s3, 7, ctypes.c_long, [ctypes.c_byte], 0)
            finally:
                _release(s3)
        except Exception:
            pass

        hr = _vcall(self._session, 6, ctypes.c_long, [])
        if hr != S_OK:
            log.info("StartCapture hr=0x%x", hr & 0xFFFFFFFF)
            return False

        log.info(
            "WGCCapture ready: %dx%d monitor @ (%d,%d) — "
            "WDA_EXCLUDEFROMCAPTURE overlays render see-through",
            size.Width, size.Height,
            self.origin_x, self.origin_y,
        )

        # Wait briefly for the first frame so probe().grab() doesn't
        # race the capture pipeline (first TryGetNextFrame often
        # returns S_OK with a null pointer until the GPU publishes).
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            if self._pull_one_frame():
                return True
            time.sleep(0.016)
        log.info("WGC first frame didn't arrive within 1s — "
                 "open() succeeds anyway; grab() will warm up")
        return True

    def _is_supported(self) -> bool:
        try:
            statics = _activation_factory(
                "Windows.Graphics.Capture.GraphicsCaptureSession",
                _IID_IGraphicsCaptureSessionStatics,
            )
        except Exception:
            return False
        try:
            supported = ctypes.c_byte(0)
            hr = _vcall(
                statics, 6, ctypes.c_long,
                [ctypes.POINTER(ctypes.c_byte)],
                ctypes.byref(supported),
            )
            return hr == S_OK and bool(supported.value)
        finally:
            _release(statics)

    def close(self) -> None:
        for attr in ("_staging", "_session", "_frame_pool",
                     "_item", "_winrt_device",
                     "_d3d_context", "_d3d_device"):
            p = getattr(self, attr, None)
            if p:
                _release(p)
                setattr(self, attr, None)
        self._staging_w = 0
        self._staging_h = 0
        if self._ro_init_ok:
            try:
                _combase.RoUninitialize()
            except Exception:
                pass
            self._ro_init_ok = False

    # ── Exclude API (parity with other backends; no-op for WGC) ─
    def set_exclude_hwnds(self, hwnds) -> None:
        # WGC honours the per-window WDA_EXCLUDEFROMCAPTURE flag set
        # by StreamWindow itself; no per-frame bookkeeping needed.
        pass

    set_exclude_xids = set_exclude_hwnds

    # ── Grab ────────────────────────────────────────────────────
    def grab(self, rect: dict):
        if not self._frame_pool:
            return None
        self._pull_one_frame()
        return self._crop(self._last_image, rect)

    def _pull_one_frame(self) -> bool:
        """Try to pull one frame from the pool; update ``_last_image``
        on success. Returns True when a new frame was decoded."""
        frame_p = ctypes.c_void_p()
        hr = _vcall(
            self._frame_pool, 7, ctypes.c_long,
            [ctypes.POINTER(ctypes.c_void_p)],
            ctypes.byref(frame_p),
        )
        if hr != S_OK or not frame_p.value:
            return False
        try:
            img = self._decode_frame(frame_p.value)
        finally:
            _release(frame_p.value)
        if img is None:
            return False
        self._last_image = img
        self._maybe_log(img)
        return True

    def _maybe_log(self, img) -> None:
        try:
            now = time.monotonic()
            if now - self._last_log_at <= 1.0:
                return
            pixels = list(img.getdata())
            sample = pixels[::max(1, len(pixels) // 1000)]
            avg = (sum(p[0] + p[1] + p[2] for p in sample)
                   / max(1, 3 * len(sample)))
            log.info(
                "wgc grab %dx%d avg=%.1f",
                img.width, img.height, avg)
            self._last_log_at = now
        except Exception:
            pass

    def _decode_frame(self, frame_ptr):
        # frame.get_Surface → IDirect3DSurface
        surface_p = ctypes.c_void_p()
        hr = _vcall(
            frame_ptr, 6, ctypes.c_long,
            [ctypes.POINTER(ctypes.c_void_p)],
            ctypes.byref(surface_p),
        )
        if hr != S_OK or not surface_p.value:
            return None
        try:
            dxgi_access = _query(
                surface_p.value, _IID_IDirect3DDxgiInterfaceAccess)
            try:
                tex_p = ctypes.c_void_p()
                hr = _vcall(
                    dxgi_access, 3, ctypes.c_long,
                    [ctypes.POINTER(_GUID),
                     ctypes.POINTER(ctypes.c_void_p)],
                    ctypes.byref(_IID_ID3D11Texture2D),
                    ctypes.byref(tex_p),
                )
                if hr != S_OK or not tex_p.value:
                    return None
                try:
                    return self._texture_to_image(tex_p.value)
                finally:
                    _release(tex_p.value)
            finally:
                _release(dxgi_access)
        finally:
            _release(surface_p.value)

    def _texture_to_image(self, tex):
        # ID3D11Texture2D.GetDesc is at vtable index 10 (3=IUnknown,
        # +4=ID3D11DeviceChild, +3=ID3D11Resource, +0=GetDesc).
        desc = _D3D11_TEXTURE2D_DESC()
        _vcall(
            tex, 10, None,
            [ctypes.POINTER(_D3D11_TEXTURE2D_DESC)],
            ctypes.byref(desc),
        )
        if (self._staging is None
                or desc.Width != self._staging_w
                or desc.Height != self._staging_h):
            if self._staging:
                _release(self._staging)
                self._staging = None
            self._staging = self._make_staging(desc.Width, desc.Height)
            if not self._staging:
                return None
            self._staging_w = desc.Width
            self._staging_h = desc.Height

        # context.CopyResource(dst=staging, src=tex) — index 47.
        _vcall(
            self._d3d_context, 47, None,
            [ctypes.c_void_p, ctypes.c_void_p],
            self._staging, tex,
        )
        # context.Map(staging, 0, D3D11_MAP_READ, 0, &mapped) — 14.
        mapped = _D3D11_MAPPED_SUBRESOURCE()
        hr = _vcall(
            self._d3d_context, 14, ctypes.c_long,
            [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint,
             ctypes.c_uint,
             ctypes.POINTER(_D3D11_MAPPED_SUBRESOURCE)],
            self._staging, 0, D3D11_MAP_READ, 0,
            ctypes.byref(mapped),
        )
        if hr != S_OK or not mapped.pData:
            return None
        try:
            return self._copy_pixels(
                mapped.pData, mapped.RowPitch,
                desc.Width, desc.Height,
            )
        finally:
            # context.Unmap(staging, 0) — index 15.
            _vcall(
                self._d3d_context, 15, None,
                [ctypes.c_void_p, ctypes.c_uint],
                self._staging, 0,
            )

    def _make_staging(self, w, h):
        desc = _D3D11_TEXTURE2D_DESC()
        desc.Width = w
        desc.Height = h
        desc.MipLevels = 1
        desc.ArraySize = 1
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM
        desc.SampleDesc.Count = 1
        desc.SampleDesc.Quality = 0
        desc.Usage = D3D11_USAGE_STAGING
        desc.BindFlags = 0
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ
        desc.MiscFlags = 0
        tex = ctypes.c_void_p()
        hr = _vcall(
            self._d3d_device, 5, ctypes.c_long,
            [ctypes.POINTER(_D3D11_TEXTURE2D_DESC),
             ctypes.c_void_p,
             ctypes.POINTER(ctypes.c_void_p)],
            ctypes.byref(desc), None, ctypes.byref(tex),
        )
        if hr != S_OK or not tex.value:
            log.info("CreateTexture2D(staging %dx%d) hr=0x%x",
                     w, h, hr & 0xFFFFFFFF)
            return None
        return tex.value

    def _copy_pixels(self, src_addr, row_pitch, w, h):
        """Copy BGRA pixels from a mapped staging texture into a PIL
        Image. DXGI may pad ``row_pitch`` beyond ``w*4`` for alignment
        — handle that explicitly."""
        from PIL import Image
        buf = (ctypes.c_ubyte * (w * h * 4))()
        src_stride = int(row_pitch)
        dst_stride = w * 4
        if src_stride == dst_stride:
            ctypes.memmove(buf, src_addr, w * h * 4)
        else:
            dst_addr = ctypes.addressof(buf)
            for y in range(h):
                ctypes.memmove(
                    dst_addr + y * dst_stride,
                    src_addr + y * src_stride,
                    dst_stride,
                )
        return Image.frombuffer(
            "RGBA", (w, h),
            bytes(buf), "raw", "BGRA", 0, 1,
        ).convert("RGB")

    def _crop(self, img, rect):
        if img is None:
            return None
        if not rect:
            return img
        try:
            x = int(rect.get("x", 0)) - self.origin_x
            y = int(rect.get("y", 0)) - self.origin_y
            w = int(rect.get("width", img.width))
            h = int(rect.get("height", img.height))
            x = max(0, min(x, img.width))
            y = max(0, min(y, img.height))
            return img.crop((x, y, x + w, y + h))
        except Exception:
            return img


def available() -> bool:
    if sys.platform != "win32":
        return False
    return _load()

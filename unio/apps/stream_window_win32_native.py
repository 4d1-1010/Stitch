"""Native D3D11 + DirectComposition StreamWindow overlay for Windows.

Why this exists:
  * Tk overlays end up in the GDI redirection surface. mss.grab /
    BitBlt / PrintWindow all see them; our own capture picks up the
    overlay's streamed pixels and feeds them back to the sink peer.
  * WDA_EXCLUDEFROMCAPTURE + any WDA-respecting capture API gives
    us a *black* rectangle where the overlay is — the overlay is
    "excluded" in the sense that it's blanked, not "seen through".
  * PerWindow PrintWindow on modern Windows returns black bitmaps
    for every DirectComposition-backed window (Progman,
    Shell_TrayWnd, every modern app). Dead architecturally.

This module creates a window with ``WS_EX_NOREDIRECTIONBITMAP`` so
it has no GDI redirection surface at all. Pixels are rendered via
DirectComposition, composited at scan-out time by DWM. mss, BitBlt,
PrintWindow, any GDI-based capture API → sees through the overlay
as if it didn't exist. No hide-during-capture, no flicker.

Each ``push_frame(jpeg_bytes, codec)`` uploads a decoded RGBA buffer
to a D3D11 texture, copies it to the current DXGI swap-chain back
buffer, and presents it. DirectComposition commits the change and
DWM shows it on the physical monitor at the next refresh.

Public surface mirrors ``StreamWindow`` so the shell doesn't need
a Windows-specific codepath: ``push_frame``, ``destroy``, and the
``top`` attribute (an object with ``winfo_id`` that returns the
HWND — the shell uses this as the xid to push to the capture
backend's exclusion list; harmless on bare-mss Windows because the
window has no redirection surface for mss to see anyway).
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import threading
from ctypes import (
    HRESULT, POINTER, Structure, byref, c_int, c_long, c_uint,
    c_ulong, c_void_p, c_ubyte, c_ushort,
)
from typing import Callable, Optional

log = logging.getLogger(__name__)


# ── Win32 constants ──────────────────────────────────────────────────

WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
WS_EX_TOPMOST = 0x00000008
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000

SW_HIDE = 0
SW_SHOWNOACTIVATE = 4

CS_HREDRAW = 0x0002
CS_VREDRAW = 0x0001

HWND_TOPMOST = -1
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040

# DXGI
DXGI_FORMAT_B8G8R8A8_UNORM = 87
DXGI_ALPHA_MODE_PREMULTIPLIED = 1
DXGI_SCALING_STRETCH = 0
DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3
DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x00000020

# D3D11
D3D_DRIVER_TYPE_HARDWARE = 1
D3D_DRIVER_TYPE_WARP = 5
D3D11_SDK_VERSION = 7
D3D_FEATURE_LEVEL_11_0 = 0xb000
D3D_FEATURE_LEVEL_10_1 = 0xa100
D3D_FEATURE_LEVEL_10_0 = 0xa000
D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20
D3D11_USAGE_DEFAULT = 0
D3D11_BIND_RENDER_TARGET = 0x20

# COM
COINIT_APARTMENTTHREADED = 0x2


# ── Structures ───────────────────────────────────────────────────────


class GUID(Structure):
    _fields_ = [
        ("Data1", wt.DWORD),
        ("Data2", wt.WORD),
        ("Data3", wt.WORD),
        ("Data4", c_ubyte * 8),
    ]


def _guid(s: str) -> GUID:
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


IID_IDXGIDevice = _guid("54ec77fa-1377-44e6-8c32-88fd5f44c84c")
IID_IDXGIFactory2 = _guid("50c83a1c-e072-4c48-87b0-3630fa36a6d0")
IID_IDCompositionDevice = _guid("c37ea93a-e7aa-450d-b16f-9746cb0407f3")
IID_ID3D11Texture2D = _guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c")


class DXGI_SAMPLE_DESC(Structure):
    _fields_ = [
        ("Count", c_uint),
        ("Quality", c_uint),
    ]


class DXGI_SWAP_CHAIN_DESC1(Structure):
    _fields_ = [
        ("Width", c_uint),
        ("Height", c_uint),
        ("Format", c_uint),
        ("Stereo", wt.BOOL),
        ("SampleDesc", DXGI_SAMPLE_DESC),
        ("BufferUsage", c_uint),
        ("BufferCount", c_uint),
        ("Scaling", c_uint),
        ("SwapEffect", c_uint),
        ("AlphaMode", c_uint),
        ("Flags", c_uint),
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


class D3D11_BOX(Structure):
    _fields_ = [
        ("left", c_uint), ("top", c_uint), ("front", c_uint),
        ("right", c_uint), ("bottom", c_uint), ("back", c_uint),
    ]


WNDPROC = ctypes.WINFUNCTYPE(
    c_long, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSEXW(Structure):
    _fields_ = [
        ("cbSize", c_uint),
        ("style", c_uint),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", c_int),
        ("cbWndExtra", c_int),
        ("hInstance", wt.HINSTANCE),
        ("hIcon", wt.HANDLE),
        ("hCursor", wt.HANDLE),
        ("hbrBackground", wt.HANDLE),
        ("lpszMenuName", wt.LPCWSTR),
        ("lpszClassName", wt.LPCWSTR),
        ("hIconSm", wt.HANDLE),
    ]


# ── COM vtable helper ────────────────────────────────────────────────


def _call_vtbl(obj: int, idx: int, argtypes, restype, *args):
    vtbl = ctypes.cast(obj, POINTER(c_void_p))[0]
    fn_ptr = ctypes.cast(vtbl, POINTER(c_void_p))[idx]
    fn = ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)(fn_ptr)
    return fn(obj, *args)


def _com_release(obj) -> None:
    if not obj:
        return
    try:
        _call_vtbl(obj, 2, (), c_ulong)
    except Exception:
        pass


# ── NativeStreamWindow ───────────────────────────────────────────────


class _HwndBox:
    """Duck type for compatibility with the Tk StreamWindow's
    ``self.top.winfo_id()`` call site in shell._push_overlay_xids_to_
    capture.  Returning the native HWND is still useful — the capture
    backend's exclusion list accepts it, even though bare mss doesn't
    need to skip a NOREDIRECTIONBITMAP window anyway."""

    def __init__(self, hwnd: int):
        self._hwnd = hwnd

    def winfo_id(self) -> int:
        return int(self._hwnd)


class NativeStreamWindow:
    """Windows-only stream overlay rendered via D3D11 + DirectComposition
    on a ``WS_EX_NOREDIRECTIONBITMAP`` window. Invisible to every GDI-
    based capture API (mss, BitBlt, PrintWindow).

    Public surface mirrors ``unio.apps.stream_window.StreamWindow`` so
    the shell can construct it through the same codepath: the
    constructor takes ``(root, x, y, width, height, source_label,
    on_close)`` and exposes ``push_frame`` + ``destroy`` + a ``top``
    attribute with a ``winfo_id`` method."""

    _class_registered = False
    _class_name = "unIONativeStreamWindow"
    _h_instance = None
    _wndproc_ref = None   # keep alive across the life of the app

    def __init__(self, root, x: int, y: int,
                 width: int, height: int,
                 source_label: str = "",
                 on_close: Optional[Callable[[], None]] = None):
        self.x = int(x)
        self.y = int(y)
        self.width = int(width)
        self.height = int(height)
        self._on_close = on_close
        self._destroyed = False
        self._frame_lock = threading.Lock()
        self._latest_frame: Optional[tuple[bytes, str]] = None
        self._redraw_scheduled = False
        # D3D11 / DComp state. Initialised lazily on the UI thread.
        self.hwnd = 0
        self.d3d_device = None
        self.d3d_ctx = None
        self.dxgi_factory = None
        self.swap_chain = None
        self.dcomp_device = None
        self.dcomp_target = None
        self.dcomp_visual = None
        # Python-side: a tkinter root is passed in so we can schedule
        # redraws on the Tk main thread via ``root.after``. We don't
        # create any Tk widgets — just reuse Tk's event loop.
        self.root = root
        self._init_all()

    # ── Tk-compatible attribute -------------------------------

    @property
    def top(self):
        return _HwndBox(self.hwnd)

    # ── Init sequence ----------------------------------------

    def _init_all(self) -> None:
        self._load_libs()
        self._ensure_class()
        self._create_window()
        self._create_d3d_device()
        self._create_swap_chain()
        self._create_dcomp()

    def _load_libs(self) -> None:
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.d3d11 = ctypes.WinDLL("d3d11", use_last_error=True)
        self.dxgi = ctypes.WinDLL("dxgi", use_last_error=True)
        self.dcomp = ctypes.WinDLL("dcomp", use_last_error=True)
        self.ole32 = ctypes.WinDLL("ole32", use_last_error=True)

        self.user32.RegisterClassExW.argtypes = [
            POINTER(WNDCLASSEXW)]
        self.user32.RegisterClassExW.restype = wt.ATOM
        self.user32.CreateWindowExW.argtypes = [
            wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD,
            c_int, c_int, c_int, c_int,
            wt.HWND, wt.HMENU, wt.HINSTANCE, c_void_p,
        ]
        self.user32.CreateWindowExW.restype = wt.HWND
        self.user32.DefWindowProcW.argtypes = [
            wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        self.user32.DefWindowProcW.restype = c_long
        self.user32.DestroyWindow.argtypes = [wt.HWND]
        self.user32.DestroyWindow.restype = wt.BOOL
        self.user32.ShowWindow.argtypes = [wt.HWND, c_int]
        self.user32.ShowWindow.restype = wt.BOOL
        self.user32.SetWindowPos.argtypes = [
            wt.HWND, wt.HWND, c_int, c_int, c_int, c_int, c_uint]
        self.user32.SetWindowPos.restype = wt.BOOL
        self.kernel32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
        self.kernel32.GetModuleHandleW.restype = wt.HINSTANCE
        self.ole32.CoInitializeEx.argtypes = [c_void_p, wt.DWORD]
        self.ole32.CoInitializeEx.restype = c_long

        self.d3d11.D3D11CreateDevice.argtypes = [
            c_void_p, c_uint, c_void_p, c_uint,
            POINTER(c_uint), c_uint, c_uint,
            POINTER(c_void_p), POINTER(c_uint),
            POINTER(c_void_p),
        ]
        self.d3d11.D3D11CreateDevice.restype = c_long

        self.dcomp.DCompositionCreateDevice.argtypes = [
            c_void_p, POINTER(GUID), POINTER(c_void_p)]
        self.dcomp.DCompositionCreateDevice.restype = c_long

        # CreateDXGIFactory2 takes flags + IID + **factory
        self.dxgi.CreateDXGIFactory2.argtypes = [
            c_uint, POINTER(GUID), POINTER(c_void_p)]
        self.dxgi.CreateDXGIFactory2.restype = c_long

        # STA COM init (DComp insists on an apartment).
        self.ole32.CoInitializeEx(None, COINIT_APARTMENTTHREADED)

    def _ensure_class(self) -> None:
        if NativeStreamWindow._class_registered:
            self._h_instance = NativeStreamWindow._h_instance
            return
        hinst = self.kernel32.GetModuleHandleW(None)
        NativeStreamWindow._h_instance = hinst
        self._h_instance = hinst

        def wndproc(hwnd, msg, wp, lp):
            return self.user32.DefWindowProcW(hwnd, msg, wp, lp)

        NativeStreamWindow._wndproc_ref = WNDPROC(wndproc)

        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.style = CS_HREDRAW | CS_VREDRAW
        wc.lpfnWndProc = NativeStreamWindow._wndproc_ref
        wc.cbClsExtra = 0
        wc.cbWndExtra = 0
        wc.hInstance = hinst
        wc.hIcon = 0
        wc.hCursor = 0
        wc.hbrBackground = 0
        wc.lpszMenuName = None
        wc.lpszClassName = NativeStreamWindow._class_name
        wc.hIconSm = 0
        atom = self.user32.RegisterClassExW(byref(wc))
        if not atom:
            raise OSError("RegisterClassExW failed "
                          f"(last error {ctypes.get_last_error()})")
        NativeStreamWindow._class_registered = True

    def _create_window(self) -> None:
        ex_style = (WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST
                    | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
                    | WS_EX_NOACTIVATE)
        style = WS_POPUP
        hwnd = self.user32.CreateWindowExW(
            ex_style,
            NativeStreamWindow._class_name,
            "unIO overlay",
            style,
            self.x, self.y, self.width, self.height,
            None, None, self._h_instance, None,
        )
        if not hwnd:
            raise OSError(
                "CreateWindowExW failed "
                f"(last error {ctypes.get_last_error()})")
        self.hwnd = int(hwnd)
        # Show without activating so we don't steal focus.
        self.user32.ShowWindow(self.hwnd, SW_SHOWNOACTIVATE)
        log.info("NativeStreamWindow created hwnd=%d at "
                 "(%d,%d,%dx%d) ex_style=0x%x (NOREDIRECTIONBITMAP)",
                 self.hwnd, self.x, self.y, self.width, self.height,
                 ex_style)

    def _create_d3d_device(self) -> None:
        device = c_void_p()
        context = c_void_p()
        chosen = c_uint()
        levels = (c_uint * 3)(
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        )
        hr = self.d3d11.D3D11CreateDevice(
            None,                       # pAdapter (default)
            D3D_DRIVER_TYPE_HARDWARE,
            None, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 3,
            D3D11_SDK_VERSION,
            byref(device), byref(chosen), byref(context),
        )
        if hr != 0 or not device.value:
            raise OSError(
                f"D3D11CreateDevice failed hr=0x{hr & 0xFFFFFFFF:x}")
        self.d3d_device = device.value
        self.d3d_ctx = context.value

    def _create_swap_chain(self) -> None:
        # Create DXGI factory.
        factory = c_void_p()
        hr = self.dxgi.CreateDXGIFactory2(
            0, byref(IID_IDXGIFactory2), byref(factory))
        if hr != 0 or not factory.value:
            raise OSError(
                f"CreateDXGIFactory2 failed hr=0x{hr & 0xFFFFFFFF:x}")
        self.dxgi_factory = factory.value

        desc = DXGI_SWAP_CHAIN_DESC1()
        desc.Width = self.width
        desc.Height = self.height
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM
        desc.Stereo = False
        desc.SampleDesc.Count = 1
        desc.SampleDesc.Quality = 0
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT
        desc.BufferCount = 2
        desc.Scaling = DXGI_SCALING_STRETCH
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED
        desc.Flags = 0

        swap = c_void_p()
        # IDXGIFactory2::CreateSwapChainForComposition = vtable 24
        hr = _call_vtbl(
            self.dxgi_factory, 24,
            (c_void_p, POINTER(DXGI_SWAP_CHAIN_DESC1),
             c_void_p, POINTER(c_void_p)),
            c_long,
            self.d3d_device, byref(desc), None, byref(swap),
        )
        if hr != 0 or not swap.value:
            raise OSError(
                f"CreateSwapChainForComposition failed "
                f"hr=0x{hr & 0xFFFFFFFF:x}")
        self.swap_chain = swap.value

    def _create_dcomp(self) -> None:
        # DCompositionCreateDevice(pDxgiDevice, riid, **dcomp_device).
        # Pass our D3D11 device queried as IDXGIDevice.
        dxgi_device = c_void_p()
        hr = _call_vtbl(
            self.d3d_device, 0,    # IUnknown::QueryInterface
            (POINTER(GUID), POINTER(c_void_p)),
            c_long,
            byref(IID_IDXGIDevice), byref(dxgi_device),
        )
        if hr != 0 or not dxgi_device.value:
            raise OSError(
                f"QI IDXGIDevice failed hr=0x{hr & 0xFFFFFFFF:x}")
        try:
            dcomp = c_void_p()
            hr = self.dcomp.DCompositionCreateDevice(
                dxgi_device,
                byref(IID_IDCompositionDevice), byref(dcomp))
            if hr != 0 or not dcomp.value:
                raise OSError(
                    f"DCompositionCreateDevice failed "
                    f"hr=0x{hr & 0xFFFFFFFF:x}")
            self.dcomp_device = dcomp.value
        finally:
            _com_release(dxgi_device.value)

        # CreateTargetForHwnd(hwnd, topmost, **target) = vtable 6.
        target = c_void_p()
        hr = _call_vtbl(
            self.dcomp_device, 6,
            (wt.HWND, wt.BOOL, POINTER(c_void_p)),
            c_long,
            self.hwnd, True, byref(target),
        )
        if hr != 0 or not target.value:
            raise OSError(
                f"CreateTargetForHwnd failed hr=0x{hr & 0xFFFFFFFF:x}")
        self.dcomp_target = target.value

        # CreateVisual = vtable 7.
        visual = c_void_p()
        hr = _call_vtbl(
            self.dcomp_device, 7,
            (POINTER(c_void_p),),
            c_long,
            byref(visual),
        )
        if hr != 0 or not visual.value:
            raise OSError(
                f"CreateVisual failed hr=0x{hr & 0xFFFFFFFF:x}")
        self.dcomp_visual = visual.value

        # Visual::SetContent(swap_chain) = vtable 15.
        _call_vtbl(
            self.dcomp_visual, 15,
            (c_void_p,),
            c_long,
            self.swap_chain,
        )
        # Target::SetRoot(visual) = vtable 3.
        _call_vtbl(
            self.dcomp_target, 3,
            (c_void_p,),
            c_long,
            self.dcomp_visual,
        )
        # Commit = vtable 3.
        _call_vtbl(self.dcomp_device, 3, (), c_long)

    # ── Frame push -------------------------------------------

    def push_frame(self, data: bytes, codec: str = "jpeg") -> None:
        with self._frame_lock:
            self._latest_frame = (data, codec)
            already = self._redraw_scheduled
            self._redraw_scheduled = True
        if already or self._destroyed:
            return
        try:
            self.root.after(0, self._redraw_on_ui_thread)
        except Exception:
            pass

    def _redraw_on_ui_thread(self) -> None:
        with self._frame_lock:
            self._redraw_scheduled = False
            frame = self._latest_frame
            self._latest_frame = None
        if frame is None or self._destroyed:
            return
        data, codec = frame
        try:
            from PIL import Image
            import io
            if codec == "h264":
                img = Image.frombytes(
                    "RGB", (self.width, self.height), data)
            else:
                img = Image.open(io.BytesIO(data))
                img.load()
                if img.size != (self.width, self.height):
                    img = img.resize(
                        (self.width, self.height), Image.BILINEAR)
            # DXGI back buffer is B8G8R8A8_UNORM. Convert RGBA with
            # premultiplied alpha = 255 so every pixel is fully
            # opaque.
            rgba = img.convert("RGBA")
            raw = rgba.tobytes("raw", "BGRA")
        except Exception:
            log.exception("frame decode failed")
            return
        self._blit_to_swap_chain(raw)

    def _blit_to_swap_chain(self, bgra_bytes: bytes) -> None:
        """Upload ``bgra_bytes`` (width*height*4) to the swap chain's
        back buffer and Present. Called on the Tk main thread after
        each decoded frame."""
        if not self.swap_chain:
            return
        back = c_void_p()
        # IDXGISwapChain::GetBuffer = vtable 9.
        hr = _call_vtbl(
            self.swap_chain, 9,
            (c_uint, POINTER(GUID), POINTER(c_void_p)),
            c_long,
            0, byref(IID_ID3D11Texture2D), byref(back),
        )
        if hr != 0 or not back.value:
            log.warning("GetBuffer failed hr=0x%x", hr & 0xFFFFFFFF)
            return
        try:
            box = D3D11_BOX(
                left=0, top=0, front=0,
                right=self.width, bottom=self.height, back=1)
            # ID3D11DeviceContext::UpdateSubresource = vtable 48.
            _call_vtbl(
                self.d3d_ctx, 48,
                (c_void_p, c_uint, POINTER(D3D11_BOX),
                 c_void_p, c_uint, c_uint),
                None,
                back.value, 0, byref(box),
                ctypes.c_char_p(bgra_bytes),
                self.width * 4, 0,
            )
        finally:
            _com_release(back.value)
        # IDXGISwapChain1::Present = vtable 8 (inherited from
        # IDXGISwapChain). Args: SyncInterval, Flags.
        _call_vtbl(
            self.swap_chain, 8,
            (c_uint, c_uint),
            c_long,
            1, 0,
        )
        # DComp Commit.
        _call_vtbl(self.dcomp_device, 3, (), c_long)

    # ── Lifecycle --------------------------------------------

    def show_placeholder(self, _reason: str = "") -> None:
        """Compatibility no-op. The Tk-era code showed a 'source
        disconnected' card; native overlay stays on its last frame
        until a new one arrives."""

    def destroy(self) -> None:
        if self._destroyed:
            return
        self._destroyed = True
        for ref_name in (
                "dcomp_visual", "dcomp_target", "dcomp_device",
                "swap_chain", "dxgi_factory",
                "d3d_ctx", "d3d_device"):
            val = getattr(self, ref_name, None)
            if val:
                _com_release(val)
                setattr(self, ref_name, None)
        if self.hwnd:
            try:
                self.user32.DestroyWindow(self.hwnd)
            except Exception:
                log.exception("DestroyWindow failed")
            self.hwnd = 0


def available() -> bool:
    import sys as _sys
    if _sys.platform != "win32":
        return False
    try:
        ctypes.WinDLL("d3d11")
        ctypes.WinDLL("dxgi")
        ctypes.WinDLL("dcomp")
        return True
    except OSError:
        return False

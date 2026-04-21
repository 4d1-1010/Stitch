# unio-pipe — native media helper (PR 6, not yet implemented)

Python wrapper code lives in `unio/features/helper_bridge.py`; this
directory is the home for the **native C++ helper binary** that
replaces the ffmpeg-subprocess + Tk-present pipeline. The rewrite
is the bulk of the PR 6 work in the latency plan. None of it is
shipping yet; this file is the starting scaffold so the
directory exists in source control with a clear charter.

## Why a separate process

The Python layer keeps the control plane (mesh, LWW, workspace
routing, pairing, clipboard, file transfer — everything that runs
at 0–10 Hz). Anything on the **frame path** leaves Python and goes
into this process. The goal is glass-to-glass ≤ 16 ms on GPU hosts
and ≤ 20 ms on no-GPU hosts, which is structurally impossible with
Python + ffmpeg subprocesses + PIL + Tk in the hot path.

## Scope (MVP — PR 6 minimum)

- **Transport**: msquic (MIT, Microsoft), one stream per direction,
  TLS 1.3 included.
- **Windows capture**: WGC v1 (Win10 22H2 via
  DispatcherQueueController) + WGC v2 (Win11 CreateFreeThreaded).
- **Windows encode**: NVENC only.
- **Windows decode**: D3D11VA.
- **Windows present**: DXGI flip-model swap chain (FLIP_DISCARD,
  SyncInterval=0) with DirectComposition.
- **Linux capture**: XComposite ported from
  `unio/features/capture_xcomposite.py` (keep the XShm + ancestor
  resolution + EWMH client-list-stacking logic; drop the PIL wrap).
- **Linux encode**: VA-API.
- **Linux decode**: VA-API.
- **Linux present**: EGL on an X11 override-redirect window.
- **Control plane**: Unix domain socket on Linux, named pipe on
  Windows. JSON commands at ≤ 10 Hz. Frames never cross.

## Scope (deferred — PR 7/8/9)

- AMF (Windows AMD) + oneVPL (Windows Intel) + NVENC-on-Linux
  encoder coverage (PR 7).
- NVDEC on Linux + D3D11VA completeness (PR 8).
- DXGI waitable swap chain + glXSwapBuffersMscOML vsync polish
  (PR 9).

## Control-plane protocol

See `unio/features/helper_bridge.py` for the Python side. The
wire is JSON-framed over a UDS (Linux) / named pipe (Windows):

```
start_outbound  { stream_id, monitor_source, peer_addr, codec_hints }
start_inbound   { stream_id, sink_monitor_id, source_hint }
stop            { stream_id }
request_idr     { stream_id }
helper_caps     -> { encoders: [...], decoders: [...], presenters: [...] }
helper_status   -> { per_stream: [...] }
```

Request rate is bounded at 10 Hz; the native side owns frame
cadence via WGC's FrameArrived and XDamage events.

## Data plane

```
Windows source (zero CPU touch):
  WGC FramePool::Frame
    -> ID3D11Texture2D (via IDirect3DDxgiInterfaceAccess)
    -> NVENC NvEncRegisterResource + NvEncEncodePicture
    -> NAL bytes -> SPSC ring -> send thread -> msquic

Linux source (one unavoidable H->D DMA):
  XComposite XShm XImage (BGRX, SHM segment)
    -> VA-API vaPutImage -> VASurface
    -> VAAPI encode -> NAL -> send thread -> msquic

Windows sink:
  msquic recv -> NAL bytes
    -> D3D11VA decode -> ID3D11Texture2D
    -> CopySubresourceRegion to DXGI backbuffer
    -> Present(SyncInterval=0)

Linux sink:
  msquic recv -> NAL bytes
    -> VA-API decode -> VASurface
    -> EGLImageKHR (EGL_LINUX_DMA_BUF_EXT)
    -> GL texture -> glDraw -> eglSwapBuffers(swap interval 0)
```

## Thread model

Per outbound stream: capture + encode + send threads, each with a
depth-2 SPSC ring to the next stage. Per inbound stream: recv +
decode + present. Backpressure is drop-oldest at the ring; never
stall the capture thread.

## Build / status

Not buildable yet. Directory + README + control-plane Python stub
are PR 6 Day 1 scaffolding. The full 8-week effort begins with
the msquic toolchain, then WGC capture, then NVENC encode on one
end and D3D11VA + DXGI present on the other, exactly in that
order.

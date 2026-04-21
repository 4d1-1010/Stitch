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

### Linux (complete — PR 6 Days 1–7b)

End-to-end source→sink validated: XComposite capture → VA-API
encode → msquic → VA-API decode → EGL/X11 presenter (DMA-BUF
zero-copy NV12). Build with:

```
apt install libva-dev libva-drm-dev libssl-dev \
            libx11-dev libxext-dev libegl-dev libgles2-mesa-dev
cmake -S unio-pipe -B unio-pipe/build
cmake --build unio-pipe/build -j
```

First configure pulls msquic (v2.4.5, MIT) and builds it from
source (~2 min submodule clone + ~40 s build with OpenSSL 3
backend). Subsequent incremental builds are seconds. A prebuilt
msquic tree can be passed via `-DMSQUIC_ROOT=/path/to/install`
or `MSQUIC_ROOT=/path/to/install` env var to skip the fetch.

### Windows (complete — PR 6 Day 8)

Every Day-8 subsystem is wired and validated end-to-end:
control-plane IPC (named pipe), msquic (OpenSSL3 backend), WGC
capture, NVENC encoder, D3D11VA H.264 decoder, DXGI flip-model
presenter. Cross-platform **Linux↔Windows video streaming over
QUIC works in both directions**.

Bidirectional validated end-to-end with **real desktop pixels
on both sides** on adi-pc (Linux, Intel UHD 630) + Diana
(Windows 10 22H2, NVIDIA GTX 1650 Ti):

- Linux source → Windows sink: XComposite → VA-API encode →
  msquic → D3D11VA decode → DXGI flip present — **visually
  confirmed** (Day 8e-b, Task 10). The DXGI presenter samples the
  decoder's NV12 texture directly via Y (R8_UNORM) + UV
  (R8G8_UNORM) SRVs, runs a BT.601 limited-range YUV→RGB pixel
  shader on a fullscreen triangle, and tear-presents at
  SyncInterval=0. One GPU-to-GPU `CopyResource` between the
  decoder-bound and shader-bound NV12 pools; no CPU readback.
- Windows source → Linux sink: WGC capture → NVENC encode →
  msquic → VA-API decode → EGL/X11 present — **visually
  confirmed**, Diana's Windows desktop renders on adi-pc at
  1080p/30 fps, 5.4 MB in 15 s for typical content.
- Linux → Linux loopback: same EGL presenter + VA-API decoder,
  **visually confirmed**.

The `start_outbound` RPC accepts optional `capture_x` /
`capture_y` alongside `width` / `height` so a client can target
an off-origin monitor (e.g. `(1920, 0, 1920, 1080)` to capture
the primary HDMI output of a three-monitor 5760-wide X display).
Omitted offsets default to `(0, 0)`.

Linux presenter uses zero-copy DMA-BUF import:
`vaExportSurfaceHandle(SEPARATE_LAYERS)` hands back per-plane
DMA-BUF fds → `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` with
`DRM_FORMAT_R8` (Y) and `DRM_FORMAT_GR88` (UV) → bound to GL
textures via `glEGLImageTargetTexture2DOES` → BT.601 limited
conversion in a GLES 2 fragment shader. EGLImages are cached
by `VASurfaceID`; steady-state cost per frame is two bind calls
+ one draw + one swap. No CPU copies.

The DMA-BUF path looked broken during Day 7b visual validation
— turned out the VA-API decoder itself was silently emitting
fill-value 128 surfaces, so the DMA-BUF import was carrying
through correct-but-empty data. After fixing the decoder, the
import works as designed. `DRM_FORMAT_GR88` maps Mesa's
R8G8_UNORM convention, so the NV12 (U, V)-interleaved pair
samples as `.r = U`, `.g = V`.

**Key VA-API decoder quirks found during bring-up** (documented
in `src/decoder_vaapi.cpp` so the next time someone touches it
doesn't burn the same hour):
- Intel iHD silently leaves the output surface at fill value
  128 (mid-gray) unless an IQMatrix buffer is supplied, even
  when no scaling lists are present. Every `VAStatus` returns
  `SUCCESS`.
- `vaRenderPicture` needs each buffer submitted in its own call
  — batching them into one `vaRenderPicture(bufs, 3)` runs
  cleanly but the driver skips the decode.
- `VASliceDataBufferType` wants the Annex-B start code
  (`00 00 00 01`) prepended, not just the NAL header byte.

**Key D3D11VA decoder quirks found during Day 8e-b** (documented
in `src/decoder_d3d11va.cpp`; NVIDIA GeForce 561.x on Win10 22H2):
- NVIDIA advertises nine H264_VLD_NOFGT configs; most are
  `ConfigBitstreamRaw=2`, a few are `raw=1`. Only `raw=2` decodes
  — `raw=1` returns `SUCCESS` from `SubmitDecoderBuffers` and
  leaves the NV12 surface zero-filled. FFmpeg prefers `raw=1` on
  Intel/AMD and `raw=2` on NVIDIA; taking the first advertised
  config picks the right one on every vendor we test.
- `DXVA_PicParams_H264::ContinuationFlag` must be 1. Without it
  the driver reads only the first half of the struct and treats
  the reference list, POC fields, and most flags as zero.
- `DXVA_PicParams_H264::RefPicFlag` (bit 6 of `wBitFields`) must
  be 1 for any picture used as a reference. Our IPPP encoder
  marks every frame as a reference, so set unconditionally.
- `DXVA_Slice_H264_Short::SliceBytesInBuffer` covers the bitstream
  buffer's trailing 128-byte padding, not just the start-code +
  NAL payload — the last (only) slice "owns" the pad bytes.
- The decoder output pool cannot carry both `D3D11_BIND_DECODER`
  and `D3D11_BIND_SHADER_RESOURCE` on NVIDIA; we keep two parallel
  NV12 pools (decoder-only, shader-only) and `CopyResource`
  between them on the same D3D11 device. No CPU touch.

**DXGI flip presenter gotcha**: the default D3D11 rasterizer
(`CullMode=BACK, FrontCounterClockwise=FALSE`) culls the classic
SV_VertexID fullscreen triangle because its projected winding is
counter-clockwise in screen space. Always bind a rasterizer with
`CullMode=NONE` or reorder the vertex IDs so the triangle is
clockwise.

Build with Visual Studio 2019/2022 + Strawberry Perl (needed by
msquic's OpenSSL3 sub-build) + CMake:

```
cmake -S unio-pipe -B unio-pipe\build -G "Visual Studio 16 2019" -A x64
cmake --build unio-pipe\build --config Release
```

msquic is built with the openssl3 TLS backend on both OSes — Win10
22H2 ships Schannel with TLS 1.3 disabled by default, so we'd
either need a registry flip at install time or bundle OpenSSL. We
bundle. The bundled `libmsquic.so` / `msquic.dll` statically links
OpenSSL; there's no runtime OpenSSL dep on Windows. First build
takes ~5 min because OpenSSL and msquic both compile from source.

Today's Windows build produces `unio-pipe.exe` that:
- opens a named pipe at `\\.\pipe\unio-pipe` (or the path passed
  via `--socket`);
- accepts helper_caps / helper_status / start_inbound /
  start_outbound;
- on `start_inbound`, stands up a QUIC listener (self-signed cert),
  a D3D11VA H.264 decoder, and a DXGI flip-model swap chain that
  samples the decoder's NV12 texture via Y/UV SRVs and renders at
  tear-present;
- on `start_outbound`, runs WGC capture → NVENC encode → msquic.

**One gotcha**: a helper launched over SSH runs in session 0, and
`IDXGIFactory2::CreateSwapChainForHwnd` fails outside an
interactive desktop session. The sink's decoder still counts
frames in that mode; visual validation needs a scheduled task with
`/IT` (see `packaging/build-remote-win.py --launch` for the
idiom). Also requires a one-time `New-NetFirewallRule
-DisplayName unio-pipe-quic -Direction Inbound -Protocol UDP
-LocalPort 5080-5090 -Action Allow` to let QUIC traffic reach
the helper.

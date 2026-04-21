# `unio-pipe` architecture

`unio-pipe` is the native C++ media helper that owns the frame
hot path for UnIO's display-streaming feature. Python drives the
control plane (mesh / LWW / pairing / workspace routing at
0–10 Hz); `unio-pipe` owns everything under that — capture,
encode, transport, decode, present — at display refresh rate
with pixels never crossing back into Python.

This document is the technical reference for the codebase at its
current state (PR 6 complete, PR 7 Day 2 shipped). It's
intentionally long; skip to the section that matches what you're
touching.

> Scope of this doc: ~8.2 KLoC across `src/` + `include/`, Linux
> + Windows. Measured end-to-end latency is at the bottom.

---

## 1. Process model

One `unio-pipe` process per machine. The Python app (`unio/…`)
spawns it at login, keeps it alive across the session, and
communicates with it over a Unix Domain Socket (Linux) or named
pipe (Windows). A singleton guard (`FILE_FLAG_FIRST_PIPE_INSTANCE`
on Windows, UDS bind-failure on Linux) makes a second helper
exit cleanly with a log line rather than fight the first for
GPU / capture / firewall resources.

```
 ┌────────────────────────────┐        control plane         ┌────────────────────┐
 │ Python unio (0–10 Hz)      │ ───────────────────────────► │ unio-pipe helper    │
 │                            │  JSON RPC over UDS / pipe    │ (per machine)       │
 │ mesh / LWW / pairing /     │ ◄─────────────────────────── │                    │
 │ workspace routing          │        replies + caps        │                    │
 └────────────────────────────┘                              └──────┬─────────────┘
                                                                    │ pixels never
                                                                    │ cross this line
                                                                    ▼
                                                        ┌──────────────────────────┐
                                                        │  data plane (QUIC)        │
                                                        │                          │
                                                        │  capture → encode → net  │
                                                        │  net → decode → present  │
                                                        └──────────────────────────┘
```

The process structure by TU:

| TU | Lines | Role |
|---|---|---|
| `main.cpp` | 125 | Arg parse, start `ControlSocket`, signal handling |
| `control_socket.cpp` | 596 | UDS/pipe server, length-prefixed JSON, dispatch |
| `json_mini.cpp` / `caps.cpp` | 220 + 86 | Dependency-free JSON, `helper_caps` builder |
| `stream_manager.cpp` | 479 | `OutboundStream` / `InboundStream` lifecycle + ring plumbing |
| `capture_xcomposite.cpp` | 295 | Linux source: XShm + XComposite |
| `capture_wgc.cpp` | 379 | Windows source: WGC (+ GPU zero-copy pool) |
| `encoder_vaapi.cpp` | 895 | Linux encode: VA-API H.264 with packed headers |
| `encoder_nvenc.cpp` | 450 | Windows encode: NVENC H.264 (CPU + GPU input) |
| `quic_transport.cpp` | 713 | msquic wrapper, self-signed cert, TLS 1.3 |
| `decoder_vaapi.cpp` | 535 | Linux decode: VA-API H.264, shared H.264 parser |
| `decoder_d3d11va.cpp` | 629 | Windows decode: D3D11VA H.264 |
| `presenter_egl_x11.cpp` | 586 | Linux present: EGL on X11 override-redirect, DMA-BUF zero-copy |
| `presenter_dxgi.cpp` | 493 | Windows present: DXGI flip-model, NV12 SRV shader |
| `h264_parse.cpp` | 513 | Shared H.264 parser / builder / latency SEI |
| `latency_log.cpp` | 67 | Per-frame CSV emitter |

---

## 2. Control plane

### 2.1 Transport

Length-prefixed JSON over a local socket:

```
  4 bytes little-endian uint32 length
  N bytes UTF-8 JSON body
```

Linux: `AF_UNIX SOCK_STREAM` at `/tmp/unio-pipe-<role>.sock`.
Windows: `\\.\pipe\unio-<role>` created with
`FILE_FLAG_FIRST_PIPE_INSTANCE` — a second helper on the same
pipe name gets `ERROR_ACCESS_DENIED` / `ERROR_PIPE_BUSY` and
exits cleanly.

Rate ceiling is ≤ 10 Hz by convention; nothing in the server
enforces it because the bytes are negligible. Frames never flow
over this channel.

### 2.2 Commands

All JSON objects. Replies return either an object with `error`
or the natural success shape (e.g. `{"started": true}`).

| cmd | params | effect |
|---|---|---|
| `helper_caps` | — | Returns `{encoders, decoders, presenters}` arrays — the build-time list, not the runtime-probed one (PR 10 adds runtime probe). |
| `helper_status` | — | Per-stream counters: captured / encoded / dropped / bytes_emitted / packets_received / frames_decoded / frames_presented / quic_connected. |
| `start_outbound` | `stream_id`, `peer_addr`, `peer_port`, `width`, `height`, `fps`, optional `capture_x`/`capture_y` | Open capture + encoder + QUIC client toward peer. |
| `start_inbound` | `stream_id`, `listen_port`, `window_w`, `window_h` | Open QUIC server + decoder + presenter window. |
| `stop` | `stream_id` | Tears down in either direction. Destructors join threads first, so `stop` returns when the data plane has quiesced. |
| `request_idr` | `stream_id` | Flips `force_idr_` atomic on the encoder; next `Encode` / `EncodeGpu` call emits an IDR with fresh SPS/PPS. Cheap enough to fire at subscribe or loss detection at up to 10 Hz. |

### 2.3 Dispatch

```
ControlSocket accept loop ──► JSON parse (json_mini.cpp)
                                     │
                                     ▼
                             HandleCommand()
                                     │
                        ┌────────────┼────────────┐
                        ▼            ▼            ▼
              StreamManager::Start*/Stop    caps builder   RequestIdr
                        │
                        ▼
                  OutboundStream / InboundStream
                  (capture + encoder + quic threads)
```

Each RPC runs synchronously on the accept thread — we never
touch frames here, so it's fine for the RPC to block while the
stream boots. The UDS/pipe closes when the RPC reply finishes.

---

## 3. Data plane — Linux source (`DISPLAY=:…` → QUIC)

```
┌─────────────────────────────────────────────────────────────────────┐
│  XServer root window                                                 │
│  (XComposite redirection, XShm attached for fast readback)            │
└──────────────┬──────────────────────────────────────────────────────┘
               │  XShmGetImage → BGRX in shared memory
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  XCompositeCapture (capture_xcomposite.cpp)                          │
│  - Captures CaptureRect (x, y, w, h)                                 │
│  - Resolves ancestor window chain, honors EWMH _NET_CLIENT_LIST_STACKING│
│  - Emits CpuFrame {BGRX pixels, width, height, frame_id, capture_ns}  │
└──────────────┬──────────────────────────────────────────────────────┘
               │  SpscRing<CpuFrame, 2>  (drop-oldest on overflow)
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  VaapiEncoder (encoder_vaapi.cpp)                                    │
│  - BGRX → NV12 via vaDeriveImage + CPU loop  (*)                     │
│  - VAConfigAttrib auto-selection:                                    │
│      profile = ConstrainedBaseline / Main / High                     │
│      entrypoint = EncSlice / EncSliceLP                              │
│  - Packed headers: SPS + PPS + slice header (we parse + build them) │
│  - CQP rate control, infinite GOP, IPPP                              │
│  - Prepends Annex-B latency SEI (UUID + frame_id + capture_ns)       │
│  - Emits EncodedPacket { NAL bytes, capture_ns, encode_ns, key_frame}│
└──────────────┬──────────────────────────────────────────────────────┘
               │  SpscRing<EncodedPacket, 4>  (drop-oldest on overflow)
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  QuicOutbound (quic_transport.cpp)                                   │
│  - msquic v2.4.5 + OpenSSL3 backend                                  │
│  - Self-signed cert at connect time (EC P-256, generated in-process) │
│  - One stream per outbound direction                                 │
│  - Framed: 4B BE length prefix + NAL payload                         │
└─────────────────────────────────────────────────────────────────────┘
```

(*) BGRX→NV12 on CPU is a cost PR 10 can replace with VA-API's
VPP (`VAProcPipelineParameterBuffer`) if we need it. At 1920×1080
it's ~3 ms on a 2019-era Intel integrated GPU — not the
hot-path bottleneck.

### 3.1 XShm fast path

`capture_xcomposite.cpp` uses the MIT-SHM extension to skip the
X protocol round-trip. Every capture rect allocates an `XImage`
backed by a shared memory segment; `XShmGetImage` fills it
in-place, no network copy.

Window ancestor resolution walks parents up to the root and
applies the `IsViewable` mask so occluded subwindows don't
corrupt the capture. EWMH `_NET_CLIENT_LIST_STACKING` is read
at init to preserve Z order when compositing multiple windows.

---

## 4. Data plane — Windows source (Desktop → QUIC)

Two paths depending on encoder capability (PR 7 Day 2 added the
GPU path; the CPU path is the fallback).

### 4.1 GPU zero-copy path (NVENC, PR 7 Day 2)

```
┌─────────────────────────────────────────────────────────────────────┐
│  Windows 10 1903+ desktop composition                                 │
└──────────────┬──────────────────────────────────────────────────────┘
               │ Direct3D11CaptureFramePool::CreateFreeThreaded
               │ (DispatcherQueueController) — 2 frame pool, BGRA
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  WgcCapture::OnFrame   (capture_wgc.cpp, WinRT worker thread)        │
│  shared D3D11 device with encoder (see 4.3)                          │
│                                                                      │
│  wgc frame.Surface() → ID3D11Texture2D (BGRA, WGC-owned)             │
│                                  │                                   │
│                                  │ CopyResource                      │
│                                  ▼                                   │
│  gpu_pool[i]  (ID3D11Texture2D, 2 slots, DEFAULT usage,              │
│                D3D11_BIND_SHADER_RESOURCE | RENDER_TARGET)           │
│                                  │                                   │
│                                  ▼                                   │
│  GpuFrameReady(GpuFrame{texture*, w, h, frame_id, capture_ns})       │
│      — runs synchronously under WgcCapture::frame_mu                 │
└──────────────┬──────────────────────────────────────────────────────┘
               │ (no intermediate ring — encode runs in this callback)
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  NvencEncoder::EncodeGpu   (encoder_nvenc.cpp)                       │
│  NvEncRegisterResource(texture)  ← cached per pool slot              │
│  NvEncMapInputResource           ← per frame                         │
│  nvEncEncodePicture              ← reads GPU memory directly         │
│  NvEncUnmapInputResource                                             │
│  + latency SEI prepend                                               │
└──────────────┬──────────────────────────────────────────────────────┘
               │ SpscRing<EncodedPacket, 4>
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  QuicOutbound → net                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

Three CPU touchpoints eliminated vs CPU path: no staging-Map, no
BGRA memcpy into `CpuFrame`, no `nvEncLockInputBuffer` upload.
Measured: 31.35 ms → 15.48 ms p50 Windows loopback (see §11).

### 4.2 CPU path (fallback, VA-API-on-Windows future encoders)

Same WGC pool feeding a D3D11 staging texture, `Map(D3D11_MAP_READ)`
gives BGRA on CPU, copied into a `CpuFrame`, pushed through the
normal SpscRing to a dedicated encode thread. Retained for
encoders that don't expose a D3D11 device via
`Encoder::NativeD3d11Device()`.

### 4.3 Shared D3D11 device

`Encoder::AcceptsGpu() && Encoder::NativeD3d11Device()` is the
runtime switch. `StreamManager::StartOutbound` creates the encoder
first (NVENC creates an `ID3D11Device` in `Init`), then hands that
device to `WgcCapture::Open(shared_device)`. WGC adopts it via
`IDirect3DDxgiInterfaceAccess`; both sides end up on the same
device, so captured textures are directly registerable with NVENC.

### 4.4 Lifetime — the "WGC ghost lambda" bug

`WgcCapture::Impl` is a `std::shared_ptr`; the `FrameArrived`
handler captures a `std::weak_ptr<Impl>`. Close holds
`Impl::frame_mu` across the full teardown so any in-flight
`OnFrame` on the WinRT worker thread completes before D3D11
resources reset. Any `OnFrame` invoked *after* teardown returns
`nullptr` from `weak.lock()` and is a no-op. Covers both the
in-flight race and the "WGC releases handler later than pool" race
that caused `d3d11.dll+0x1a5a7c` crashes before PR 6 Task 14.

---

## 5. H.264 — shared parser + builder + latency SEI

`h264_parse.cpp/h` is the single source of truth for bitstream
parsing, used by both encoders (packed header generation) and
both decoders (SPS/PPS/slice header parse + SEI extraction).

### 5.1 What's parsed

- `ParsedSps`: profile_idc, level_idc, chroma_format_idc,
  pic_width/height_in_mbs, frame_mbs_only_flag,
  direct_8x8_inference_flag, log2_max_frame_num_minus4,
  pic_order_cnt_type, num_ref_frames, crop_* .
- `ParsedPps`: entropy_coding_mode_flag,
  bottom_field_pic_order_in_frame_present_flag,
  num_ref_idx_l{0,1}_default_active_minus1,
  weighted_pred_flag, weighted_bipred_idc, pic_init_qp_minus26,
  pic_init_qs_minus26, chroma_qp_index_offset,
  deblocking_filter_control_present_flag,
  constrained_intra_pred_flag, redundant_pic_cnt_present_flag.
- `ParsedSliceHeader`: first_mb_in_slice, slice_type (canonical
  and raw), pic_parameter_set_id, frame_num, idr_pic_id,
  slice_qp_delta, deblocking offsets,
  num_ref_idx_l0_active_minus1 (with override flag),
  `slice_data_bit_offset` (required by D3D11VA + VA-API).

Scope is deliberately narrow: ConstrainedBaseline / Main profile,
CAVLC only, `pic_order_cnt_type = 2`, frame_mbs_only,
weighted_pred = 0, single slice / single reference. B-frames /
FMO / ASO / field pictures / CABAC → parse returns `false` and
the decoder drops the frame.

### 5.2 Annex-B helpers

- `ScanAnnexB(bytes, len)` → vector of `NalSpan{offset, length}`.
  Offsets skip the start code (the NAL header byte is at
  `offset`). Handles both `00 00 01` and `00 00 00 01`.
- `StripEmulationPrevention(ebsp, n)` → fresh RBSP buffer with
  `0x03` bytes removed per H.264 7.4.1.1.
- `BitReader` + `BitWriter` do MSB-first u(n) / ue(v) / se(v)
  per 9.1 exponential-Golomb.
- `BuildSps`, `BuildPps`, `BuildSliceHeader` emit packed headers
  the driver submits verbatim via
  `VA_ENC_PACKED_HEADER_SEQUENCE` / `PICTURE` / `SLICE`.

### 5.3 Latency SEI

```
┌─────────────────────────────────────────────────────────────┐
│  Annex-B NAL                                                 │
│                                                              │
│  [00 00 00 01] [06] [05] [20] [16B UUID "unio-pipe/lat1"]    │
│                 NAL  ptype psize                             │
│                 SEI  =5    =32                               │
│                                                              │
│                [8B frame_id BE] [8B capture_ns BE] [80]      │
│                                                   rbsp stop │
└─────────────────────────────────────────────────────────────┘
```

Emulation prevention applied to the whole SEI body in case the
two BE `u64`s contain `00 00 00` triplets. Both encoders prepend
this SEI to every slice packet; both decoders parse it and
propagate `frame_id` + `capture_monotonic_ns` into the
`DecodedFrame` handed to the presenter. That lets the sink's
latency CSV compute glass-to-glass on one clock (single-machine
loopback) or across NTP-synced peers.

### 5.4 Decoder quirks that cost us an afternoon

Captured as comments in-source so the next touch doesn't burn
the same hour:

**VA-API (Intel iHD 1.20)** — `src/decoder_vaapi.cpp`:
- Intel iHD silently leaves the output surface at fill value
  128 (mid-gray) unless an IQMatrix buffer is supplied, even
  when no scaling lists are present. Every `VAStatus` returns
  `SUCCESS`. Always submit an all-16 IQMatrix.
- `vaRenderPicture` needs each buffer submitted in its own
  call — batching them into one `vaRenderPicture(bufs, 3)`
  runs cleanly but the driver skips the decode.
- `VASliceDataBufferType` wants the Annex-B start code
  (`00 00 00 01`) prepended, not just the NAL header byte.

**D3D11VA (NVIDIA 561.x on Win10 22H2)** — `src/decoder_d3d11va.cpp`:
- NVIDIA advertises nine `H264_VLD_NOFGT` configs; most are
  `ConfigBitstreamRaw=2`, a few are `raw=1`. Only `raw=2`
  decodes — `raw=1` returns `SUCCESS` from
  `SubmitDecoderBuffers` and leaves the NV12 surface zero-filled.
  Intel iHD and AMD advertise `raw=1` first, so "take the first
  advertised config" works on all three vendors.
- `DXVA_PicParams_H264::ContinuationFlag` must be 1. Without
  it the driver reads only the first half of the struct.
- `RefPicFlag` (bit 6 of `wBitFields`) must be 1 for any
  reference picture. Our IPPP stream marks every frame as one.
- `DXVA_Slice_H264_Short::SliceBytesInBuffer` covers the
  128-byte trailing pad of the bitstream buffer, not just
  start-code + NAL payload.
- The decoder output pool can't carry both `D3D11_BIND_DECODER`
  and `D3D11_BIND_SHADER_RESOURCE` on NVIDIA; we keep two
  parallel NV12 pools (decoder-only, shader-only) and
  `CopyResource` between them on the same D3D11 device.

---

## 6. Transport — msquic + self-signed TLS

`quic_transport.cpp` wraps MsQuic with two classes:

- `QuicOutbound` — client; opens one bidirectional stream to the
  peer, writes length-prefixed NAL blobs.
- `QuicInbound` — server; listens on a UDP port, accepts one
  peer, reads NAL blobs and invokes a callback.

### 6.1 Why msquic

MIT-licensed, commercial-safe, TLS 1.3 comes with the protocol,
`FetchContent` builds from source in ~40 s against OpenSSL 3 on
both OSes. Microsoft's own HTTP/3 stack uses it in production —
stability is not our concern.

### 6.2 Schannel vs OpenSSL on Windows

Win10 22H2's Schannel has TLS 1.3 disabled by default. Rather
than ship a registry flip at install time, we build msquic
against its `openssl3` TLS backend on both OSes and statically
link OpenSSL into `msquic.dll` — there's no runtime OpenSSL
dependency on the install target. The build-time cost is one
5-minute OpenSSL compile on first configure, amortised thereafter.

### 6.3 Self-signed cert

Outbound peers generate a fresh EC P-256 cert at `connect` time
using OpenSSL's C API (no file on disk). Inbound peers accept
any cert (we trust the pairing check at the Python layer, not
the cert chain). Cipher suite list is forced to
`AES_128_GCM_SHA256 + AES_256_GCM_SHA384` — dropping ChaCha20
keeps Schannel compatibility viable as a future option.

### 6.4 Lifetime + shutdown

MsQuic callbacks are async on worker threads. `ConnectionClose`
guarantees no further callbacks fire after it returns, which
lets `QuicOutbound::~` call `ConnectionClose` safely without a
use-after-free on the callback context pointer.

---

## 7. Data plane — Linux sink (QUIC → X11)

```
┌─────────────────────────────────────────────────────────────────────┐
│  QuicInbound → NAL bytes (callback on msquic worker thread)          │
└──────────────┬──────────────────────────────────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  VaapiDecoder::Feed   (decoder_vaapi.cpp)                            │
│  - ScanAnnexB, StripEmulationPrevention, dispatch by NAL type:       │
│      SPS → HandleSps         (parse + cache)                         │
│      PPS → HandlePps         (parse + cache)                         │
│      SEI → ParseLatencySei   (pending frame_id + capture_ns)          │
│      IDR/non-IDR → HandleSlice:                                      │
│         vaBeginPicture(surf) → vaRenderPicture(pic_param, buf_type,  │
│         iq, slice_param, slice_data) → vaEndPicture → vaSyncSurface  │
│  - Emits DecodedFrame {VASurfaceID, VADisplay, w, h,                 │
│      decode_done_ns, frame_id, capture_ns, key_frame}                │
└──────────────┬──────────────────────────────────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  EglX11Presenter  (presenter_egl_x11.cpp)                            │
│                                                                      │
│   Per surface ID (cached):                                           │
│     vaExportSurfaceHandle(SEPARATE_LAYERS)                           │
│       → per-plane DMA-BUF fd {Y fd, UV fd}                           │
│       → eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)                     │
│           Y plane → DRM_FORMAT_R8                                    │
│           UV plane → DRM_FORMAT_GR88                                 │
│       → glEGLImageTargetTexture2DOES  onto GL_TEXTURE_2D             │
│                                                                      │
│   Per frame:                                                         │
│     glBindTexture(Y); glBindTexture(UV);                             │
│     glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);                           │
│     eglSwapBuffers  (tear-present, SyncInterval=0)                   │
│                                                                      │
│   Fragment shader: BT.601 limited-range YUV→RGB.                     │
│   GR88 maps to R8G8_UNORM; sample .r = U, .g = V.                    │
└─────────────────────────────────────────────────────────────────────┘
```

### 7.1 Override-redirect window

The sink window is created with
`XSetWindowAttributes.override_redirect = True`, giving us a
borderless, window-manager-ignored window the compositor can't
decorate. Sized by the `start_inbound` RPC.

### 7.2 Zero-copy DMA-BUF

The import path goes: VA surface → DRM-PRIME handle → EGLImage →
GL texture. Steady-state per-frame cost in the render loop is
two `glBindTexture` calls + one draw + one swap, no CPU touch,
no format conversion on our side — the GL driver converts YUV in
hardware via the native sampler. Decode→present measured at
**0.17 ms p50** on a 2019-era Intel integrated GPU.

EGLImages are cached by `VASurfaceID` so the expensive handle
import happens once per slot, not per frame. Teardown order in
`InboundStream::~` releases images before the decoder releases
surfaces so dangling fds can't linger.

---

## 8. Data plane — Windows sink (QUIC → desktop)

```
┌─────────────────────────────────────────────────────────────────────┐
│  QuicInbound → NAL bytes                                             │
└──────────────┬──────────────────────────────────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  D3d11VaDecoder   (decoder_d3d11va.cpp)                              │
│  - Shares parser with VaapiDecoder                                   │
│  - DXVA2 H264_VLD_NOFGT, take first advertised config (raw=2 on NV)  │
│  - DecoderBeginFrame → SubmitDecoderBuffers(pic, iq, bits, slice) →  │
│    DecoderEndFrame                                                   │
│  - CopyResource from decoder pool (BIND_DECODER) into shader pool   │
│    (BIND_SHADER_RESOURCE) — same device, GPU-to-GPU.                │
│  - Emits DecodedFrame {ID3D11Texture2D* NV12, ID3D11Device*,         │
│      w, h, decode_done_ns, frame_id, capture_ns, key_frame}          │
└──────────────┬──────────────────────────────────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────────────────────────────────┐
│  DxgiFlipPresenter  (presenter_dxgi.cpp)                             │
│                                                                      │
│   Pipeline (built lazily, adopts DecodedFrame.native_device):        │
│     ID3D11ShaderResourceView on Y plane  (DXGI_FORMAT_R8_UNORM)      │
│     ID3D11ShaderResourceView on UV plane (DXGI_FORMAT_R8G8_UNORM)    │
│     IDXGISwapChain1 FLIP_DISCARD, SyncInterval=0, DirectComposition  │
│     Fullscreen triangle from SV_VertexID, no VBO                     │
│     HLSL pixel shader: BT.601 limited YUV→RGB                        │
│     Rasterizer CullMode=NONE (SV_VertexID triangle is CCW in screen │
│       space; default CullMode=BACK drops every pixel)                │
│     Message pump in the present-loop (DWM ghosts the HWND otherwise) │
└─────────────────────────────────────────────────────────────────────┘
```

HWND is `WS_POPUP | WS_VISIBLE | WS_EX_NOACTIVATE | WS_EX_TOPMOST`,
created on the present thread (which owns its message queue).

Decode→present measured at **0.35 ms p50 / 0.87 ms p95** on a
mid-range Turing-generation NVIDIA discrete GPU.

### 8.1 Session 0 caveat

DXGI `CreateSwapChainForHwnd` fails outside an interactive
desktop session. A helper launched over SSH is Session 0 — the
sink's decoder still runs and counts frames, but you get no
window. Visual validation needs `schtasks /IT /RL LIMITED`
(see `packaging/build-remote-win.py --launch`).

### 8.2 Firewall

One-time Windows Firewall rule at install:

```
New-NetFirewallRule -DisplayName unio-pipe-quic \
                    -Direction Inbound -Protocol UDP \
                    -LocalPort 5080-5090 -Action Allow
```

---

## 9. Threading + ring buffers

### 9.1 Outbound (source) threading

CPU path:

```
  WGC/XComposite worker thread  ────► SpscRing<CpuFrame, 2>  ────► encode_thread
                                                                    │
                                                                    ▼
                                                        SpscRing<EncodedPacket, 4>
                                                                    │
                                                                    ▼
                                                              send_thread  ────► msquic
```

Windows GPU zero-copy path (PR 7 Day 2):

```
  WGC WinRT worker thread   (single-threaded, serialised by frame_mu)
         │
         │  CopyResource + NvEncRegisterResource + Map + EncodePicture
         │  (no frame ring, no encode thread — encode happens here)
         ▼
  SpscRing<EncodedPacket, 4>  ────► send_thread  ────► msquic
```

### 9.2 Inbound (sink) threading

```
  msquic worker thread ──► decoder.Feed(bytes)   (synchronous decode)
                                     │
                                     ▼
                    DecodedFrame    ──►  presenter.Present(frame)
                                              │
                                              │  std::deque<DecodedFrame, max 2>
                                              ▼
                                         present_thread
                                         (EGL/DXGI loop)
```

### 9.3 SPSC ring (`include/spsc_ring.h`)

Fixed-depth wait-free single-producer / single-consumer ring. 74
lines. Depth-2 on the frame path (drop-oldest = always show
freshest capture), depth-4 on the packet path (absorbs msquic
send bursts). Overflow is reported via
`OutboundStream::dropped_at_ring` / `dropped_at_send` atomic
counters and surfaces through `helper_status`.

### 9.4 Backpressure rule

Drop-oldest at every ring. The sink always wants the newest
pixels; stalling the capture thread to wait for a slow encoder
would lag the whole stream. Matches the scope memo:

> Backpressure: SPSC rings depth 2; overflow drops older frame.

---

## 10. Latency instrumentation

### 10.1 Timebase

`NowNs()` on every TU uses `std::chrono::system_clock` (Unix
epoch). `steady_clock` was unsuitable: its epoch is per-boot on
glibc and per-process on MSVC, so cross-machine CSV rows were
dominated by uptime differences (≈14 hours of apparent "skew"
between two hosts with different boot times).
`system_clock` with NTP gives directly comparable timestamps
across hosts.

### 10.2 Propagation

```
  capture_ns  (source wall clock)  ──►  encoder SEI prepend  ──►  net
                                                                  │
                                                                  ▼
                                                 decoder SEI parse
                                                          │
                                                          │ pending_frame_id_,
                                                          │ pending_capture_ns_
                                                          ▼
                                              DecodedFrame.capture_ns
                                                          │
                                                          ▼
                                            presenter logs row:
                                            frame_id, capture_ns,
                                            decode_done_ns, present_done_ns
```

### 10.3 CSV emitter (`src/latency_log.cpp`)

`LogLatency(env_var, ...)` lazily opens the path from the named
env var on first call and writes:

```
frame_id, capture_ns, decode_done_ns, present_done_ns,
width, height, capture_to_decode_us,
decode_to_present_us, capture_to_present_us
```

Enable with `UNIO_PIPE_LATENCY_CSV=/tmp/lat.csv` on the sink
helper (presenters write the rows). Mutex-guarded for safety
when both Linux and Windows presenters share the TU.

---

## 11. Measured latency

All numbers at 1920×1080, NTP-synced clocks, NV12 420, first 30
frames skipped for warm-up, rows with unsigned underflow
(NTP-backwards steps) filtered.

Reference hardware used for these numbers:
- Linux host — kernel 6.8, Intel UHD 630 integrated GPU,
  VA-API 1.20 (iHD driver), Mesa 24.
- Windows host — Windows 10 22H2, NVIDIA GTX 1650 Ti, driver
  561.x.

PR 6 baseline:

| path | cap→dec p50 | dec→pre p50 | **glass p50 / p95** |
|---|---|---|---|
| Linux → Linux loopback | 11.53 ms | **0.17 ms** | **11.70** / 14.03 ms |
| Windows → Linux | 21.54 ms | 0.17 ms | 21.70 / 33.16 ms |
| Linux → Windows | 29.02 ms | 0.57 ms | 29.42 / 36.92 ms |
| Windows → Windows loopback | 30.80 ms | 0.35 ms | 31.35 / 46.89 ms |

PR 7 Day 2 (zero-copy WGC → NVENC) result:

| path | **glass p50** | vs PR 6 |
|---|---|---|
| Windows → Windows loopback | **15.48 ms** | −15.9 ms |
| (same path p95) | 23.55 ms | −23.3 ms |

Target (scope memo): ≤ 16 ms GPU host, ≤ 20 ms no-GPU. Windows
loopback now meets ≤ 16 ms on a path that includes both NVENC
encode AND D3D11VA decode. Cross-machine numbers with the PR 7
Day 2 source still need a fresh measurement pass once NTP drift
on the test hosts is back within ±10 ms.

---

## 12. Build

### 12.1 Linux

Prerequisites (Ubuntu / Debian package names; equivalent
packages on Fedora / Arch work fine):

```
sudo apt install build-essential cmake git \
                 libva-dev libva-drm-dev libssl-dev \
                 libx11-dev libxext-dev \
                 libegl-dev libgles2-mesa-dev
```

Build:

```
cmake -S unio-pipe -B unio-pipe/build
cmake --build unio-pipe/build -j
```

First configure pulls msquic via `FetchContent` (v2.4.5, ~2 min
submodule clone + ~40 s OpenSSL3 + msquic build). Subsequent
incremental builds are seconds. Pre-built msquic works too:

```
cmake -S unio-pipe -B unio-pipe/build \
      -DMSQUIC_ROOT=/path/to/installed/msquic
```

The environment variable `MSQUIC_ROOT` also works. Runtime
expects VA-API 1.20+ (`va_openDriver` returns a ≥ 0 status) and
an X server with MIT-SHM + XComposite + XRandR extensions; EGL
1.5 with `EGL_KHR_image_base` and `EGL_EXT_image_dma_buf_import`
for the zero-copy presenter path.

### 12.2 Windows

Prerequisites:

- Visual Studio 2019 or 2022 (Community is fine), with the
  Desktop C++ workload.
- CMake 3.20+ (ships with VS, or standalone).
- Strawberry Perl for msquic's OpenSSL3 sub-build —
  `winget install StrawberryPerl.StrawberryPerl`.
- NVIDIA driver with NVENC + D3D11VA — any recent GeForce driver
  ships these.

Build:

```
cmake -S unio-pipe -B unio-pipe\build -G "Visual Studio 16 2019" -A x64
cmake --build unio-pipe\build --config Release
```

First build ~5 min (OpenSSL + msquic from source), incremental
seconds. The built `msquic.dll` statically links OpenSSL; no
runtime OpenSSL dependency on the install target. Runtime needs
Windows 10 1903+ (for `Direct3D11CaptureFramePool`) and a
firewall rule allowing inbound UDP on the chosen QUIC port range
(see §8.2).

### 12.3 Usage

`unio-pipe` is a long-lived daemon. One process per machine,
started at login, controlled over its local IPC socket.

```
unio-pipe --socket <path>
```

`<path>` is a UDS path on Linux (e.g. `/tmp/unio-pipe.sock`) or
a named-pipe path on Windows (e.g. `\\.\pipe\unio-pipe`). If
another helper already owns the pipe/socket name, the new process
logs and `exit(0)`s cleanly; see the singleton guard in §2.1.

The helper is never invoked directly by end-users; it's spawned
by the Python control plane (`unio/features/helper_bridge.py`,
coming in PR 8). For development and measurement it's convenient
to drive it with small scripts.

**Check capabilities.** Send `{"cmd":"helper_caps"}` as a
length-prefixed JSON message and read the reply:

```python
# drive_linux.py — minimal client
import json, socket, struct
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/unio-pipe.sock")
b = json.dumps({"cmd": "helper_caps"}).encode()
s.sendall(struct.pack("<I", len(b)) + b)
rl = b""
while len(rl) < 4: rl += s.recv(4 - len(rl))
n = struct.unpack("<I", rl)[0]
r = b""
while len(r) < n: r += s.recv(n - len(r))
print(r.decode())
```

**Start a stream (sender side).** On the sender's helper:

```json
{"cmd": "start_outbound",
 "stream_id": "t1",
 "peer_addr": "10.0.0.42",
 "peer_port": 5085,
 "width": 1920, "height": 1080, "fps": 30,
 "capture_x": 0, "capture_y": 0}
```

`capture_x` / `capture_y` are optional; omitted means top-left
of the primary monitor / X display. On a multi-monitor X display,
pass `(x_offset, y_offset, width, height)` of the target monitor
from `xrandr`.

**Start a stream (receiver side).** On the receiver's helper,
*before* the sender connects:

```json
{"cmd": "start_inbound",
 "stream_id": "t1",
 "listen_port": 5085,
 "window_w": 1920, "window_h": 1080}
```

The receiver creates a borderless output window of
`window_w × window_h` and stretches whatever resolution the
sender pushes.

**Monitor.** Either side can poll `{"cmd":"helper_status"}` for
counters:

```
{"per_stream": [
  {"stream_id": "t1",
   "direction": "outbound",
   "encoder": "nvenc",
   "captured": 942, "encoded": 940,
   "bytes_emitted": 8617330,
   "quic_connected": true, ...}
]}
```

**Force an IDR** (e.g. for mid-stream subscribe or after loss):

```json
{"cmd": "request_idr", "stream_id": "t1"}
```

**Stop a stream:**

```json
{"cmd": "stop", "stream_id": "t1"}
```

Tears down the capture / encoder / decoder / presenter and
closes the QUIC side of the stream. The helper process stays
alive for the next RPC.

**Per-frame latency CSV.** Set on the receiver's helper before
launch:

```bash
UNIO_PIPE_LATENCY_CSV=/tmp/lat.csv unio-pipe --socket /tmp/unio-pipe.sock
```

Every presented frame appends a row:

```
frame_id,capture_ns,decode_done_ns,present_done_ns,
width,height,capture_to_decode_us,decode_to_present_us,
capture_to_present_us
```

Same-machine loopback gives directly-readable glass-to-glass.
For cross-machine measurement, NTP-sync both hosts first
(§10.1); otherwise the `capture_to_*` columns encode a boot-time
offset, not a real delay.

**Bitstream dump** for offline validation with FFmpeg:

```bash
UNIO_PIPE_BITSTREAM_DUMP=/tmp/dump.h264 unio-pipe --socket ...
```

The receiver writes every received NAL byte to the file; point
`ffprobe` / `ffplay` at it to confirm Annex-B validity
independent of our decoder.

### 12.4 Platform constraints

- **Linux (source):** MIT-SHM, XComposite, XRandR. Requires a
  running X server with `DISPLAY` set. Wayland is out of scope
  for this round.
- **Linux (sink):** EGL 1.5, GL_OES_EGL_image, Mesa
  `EGL_EXT_image_dma_buf_import_modifiers` (present on Intel
  iHD + Mesa radeonsi + NVIDIA proprietary).
- **Windows (source):** Windows 10 1903+, WGC runtime,
  `dwmapi.dll` for capture. An NVIDIA driver for NVENC
  (PR 10 adds AMF / oneVPL).
- **Windows (sink):** Any D3D11.1-capable GPU for D3D11VA
  decode + DXGI flip-model present. An interactive desktop
  session — DXGI `CreateSwapChainForHwnd` fails in Windows
  Session 0, so SSH-launched helpers only run the decoder (the
  present window stays invisible). Use `schtasks /IT /RL LIMITED`
  to launch into the user's Session 1 for visual validation.
- **Network:** QUIC over UDP on user-configurable ports. The
  receiver's firewall must allow inbound UDP on the chosen port.

### 12.5 Source graph

```
main.cpp
   │
   ├── control_socket.cpp ──► stream_manager.cpp ──► capture_* + encoder_* + quic_transport.cpp
   │                                 │
   │                                 └──► decoder_* + presenter_*
   │
   ├── caps.cpp / json_mini.cpp  (control plane bytes)
   │
   └── (shared) h264_parse.cpp / latency_log.cpp
```

Linux-only TUs: `capture_xcomposite.cpp`, `encoder_vaapi.cpp`,
`decoder_vaapi.cpp`, `presenter_egl_x11.cpp`.
Windows-only TUs: `capture_wgc.cpp`, `encoder_nvenc.cpp`,
`decoder_d3d11va.cpp`, `presenter_dxgi.cpp`.
Each per-OS TU has a stub at the bottom returning `nullptr`
on the "other" OS so `stream_manager.cpp`'s call sites link on
both without `#ifdef`.

---

## 13. What's next

See `README.md` → "What's left after PR 6 + PR 7" for the
ordered PR-by-PR plan. In summary:

- **PR 8** — `helper_bridge.py` Python integration (the last
  step before `unio-pipe` is the actual hot path of the app).
- **PR 9** — Decoder completeness: D3D11VA polish + NVDEC-Linux.
- **PR 10** — Hardware matrix (AMF / oneVPL / NVENC-Linux) +
  runtime capability probe + no-GPU software fallback +
  dynamic path selection. The biggest remaining PR by scope.

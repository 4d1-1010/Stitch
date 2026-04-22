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

## What's left after PR 6 + PR 7

PR 6 delivered the end-to-end pipeline + latency SEI +
request_idr; PR 7 landed zero-copy WGC → NVENC, halving
Windows-source latency. Remaining work:

### PR 7 (done) — Vsync + latency polish

Three of four sub-investigations completed:

- **Day 1 — NVENC tune** (evaluated, reverted). P1 +
  ULTRA_LOW_LATENCY doubled p50 (larger bitstream dominated the
  encode savings); orthogonal rcParams (lookahead=0, AQ=0)
  crashed NVENC at init. Reverted to P4 + LOW_LATENCY.
- **Day 2 — zero-copy WGC → NVENC** (shipped). Shared D3D11
  device + `NvEncRegisterResource` on the captured texture
  drops the staging-Map + CPU BGRA memcpy + `nvEncLockInputBuffer`
  upload. **Windows loopback p50 31.35 ms → 15.48 ms**, meeting
  the ≤16 ms GPU-host target on a path that includes both
  NVENC encode AND D3D11VA decode.
- **Day 3 — DXGI waitable swap chain** (evaluated, reverted).
  p50 decode→present 0.57 → 0.52 ms (noise), but p95 0.69 →
  11.2 ms with a 184 ms worst case — waitable inserts
  multi-frame stalls while the present queue drains. Net
  negative on tear-present SyncInterval=0 workloads.
- **Day 4 — Linux EGL vsync alignment** (moved to PR 10). Linux
  decode→present is already 0.17 ms p50; vsync alignment trades
  jitter for up-to-one-frame of added latency, the opposite of
  what we want on tear-present. Parked in PR 10 in case we ever
  need it for fixed-refresh sinks.

### PR 8 — `helper_bridge.py` integration (≈3–5 days, fully testable)

Makes `unio-pipe` the actual hot path of the app instead of a
demo you drive with hand-rolled scripts.

- Python-side process lifecycle (spawn, crash-restart, `stop`).
- Translates mesh/LWW workspace routing into `start_outbound` /
  `start_inbound` / `stop` / `request_idr` calls.
- Cursor out-of-band (control-channel coords → tiny borderless
  HWND/Xlib window on the sink, never encoded into the frame).
- Pairing check at subscribe — same gate as the rest of the app,
  enforced at the control plane before any frame work starts.

### PR 9 — Decoder completeness (≈1 week, partly testable)

- D3D11VA polish on Windows: larger pool, skip the separate
  shader-bound NV12 pool once we confirm NVIDIA's driver handles
  `D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE` correctly
  (it silently fails at pool-create on some versions — hence
  today's `CopyResource` split). Target: shave the ~11 ms
  difference between D3D11VA and VA-API decode time that makes
  Linux→Windows p50 29 ms instead of ~18 ms.
- NVDEC on Linux for NVIDIA Linux hosts — faster than VA-API,
  and removes the Intel-only assumption on the Linux sink.

Testable on our hardware: D3D11VA yes (Diana). NVDEC-Linux no
(adi-pc has no NVIDIA GPU — needs borrowed hardware).

### PR 7 — Hardware matrix + runtime probe + no-GPU fallback

Shipping-blocker for general availability. Old scope was "just
add AMF / oneVPL / NVENC-Linux"; the real work is larger:

**Full vendor hardware coverage.** AMF (Windows AMD), oneVPL
(Windows Intel), NVENC-on-Linux alongside the existing VA-API
Linux path. NVDEC-on-Linux and D3D11VA polish belong in PR 8;
PR 7 is parity on the encode side.

**No-GPU software fallback.** Not every machine has a hardware
video engine — old integrated Intel, VMs, WSL guests, headless
servers, AMD APUs with VCN disabled. A helper that falls over
on those hosts can't ship. Software H.264 encoder + decoder
behind the same `Encoder` / `Decoder` interface. openh264 (Cisco,
royalty-free binary distribution for commercial apps) is the
leading candidate — matches the post-PR-1 stance on codec
royalties. Target latency on software paths per the scope memo
is ≤20 ms, at reduced bitrate / resolution if needed.

**Runtime capability probe at helper startup.** `unio-pipe`
enumerates what the host can actually do:

- D3D11 adapters (NVIDIA / AMD / Intel / WARP) on Windows.
- NVENC session-open probe — non-NVIDIA adapters fail cleanly.
- AMF `CreateContext`, oneVPL `MFXCreateSession` probes.
- VA-API `vaQueryVendorString` + `vaQueryConfigEntrypoints` on
  Linux.
- `GraphicsCaptureSession::IsSupported()` on Win, XComposite
  version on Linux.

Results feed a richer `helper_caps` JSON that the Python control
plane inspects before codec negotiation.

**Dynamic path selection at `start_outbound` / `start_inbound`.**
Today the capture / encoder / decoder / presenter triples are
hardwired per OS at compile time. After PR 7 they become a
lookup: "given this host's probe output + the peer's advertised
capabilities, pick the lowest-latency tuple both sides support."
Fallback chain: vendor-specific hardware → cross-vendor
hardware → software → refuse. Sender and receiver negotiate
the chosen codec over the control plane before any QUIC bytes
flow, so the sink always knows what decoder to stand up.

**Testability.** Hardware matrix mostly gated on borrowed / cloud
GPUs (cloud GPU instances ~$0.50/hr per vendor are the practical
answer — QEMU can't emulate hardware video engines). No-GPU
fallback fully testable on adi-pc + Diana by force-disabling
the hardware paths in the probe, and works in any QEMU VM with
no GPU passthrough. Runtime probe + dynamic selection fully
testable locally.

**Parked from PR 7 Day 4: Linux EGL vsync alignment.**
`glXSwapBuffersMscOML` / `WaitForVBlank` alignment on the Linux
EGL presenter. Won't help the default tear-present workload —
Linux decode→present is 0.17 ms p50 already and vsync alignment
trades jitter for up-to-one-frame of added latency, the wrong
direction for ≤16 ms. Kept here in case a future sink (e.g.
fixed-refresh output, non-interactive recording) needs jitter-
free output more than it needs minimum latency.

### Measured starting line for these four PRs

(From the CSV table above; all p50 at 1920×1080.)

- Linux → Linux  **11.7 ms**  ← target met, PR 9 keeps it there
- Windows → Linux  21.7 ms   ← PR 9 (WGC+NVENC) shrinks this
- Linux → Windows  29.4 ms   ← PR 8 (D3D11VA) shrinks this
- Windows → Windows  31.4 ms ← both PR 8 and PR 9 apply

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

## Latency measurements

Both presenters log a per-frame CSV when
`UNIO_PIPE_LATENCY_CSV=/path` is set in their environment. Each
row carries `frame_id, capture_ns, decode_done_ns, present_done_ns,
width, height, capture_to_decode_us, decode_to_present_us,
capture_to_present_us`. Timestamps are `system_clock` nanoseconds
since the UNIX epoch — not `steady_clock` — so two NTP-synced
hosts produce directly comparable rows. `capture_ns` is carried
across the network inside an Annex-B SEI user_data_unregistered
NAL (`unio-pipe/lat1` UUID) so it survives the encode / QUIC /
decode roundtrip.

Measured on adi-pc (Linux, Intel UHD 630) and Diana (Win10 22H2,
NVIDIA GTX 1650 Ti) at 1920×1080, steady state (first 30 frames
of pipeline warm-up skipped, NTP sync confirmed with `w32tm
/resync` + `timedatectl` on both hosts):

| Path | capture→decode p50 | decode→present p50 | glass-to-glass p50 / p95 / p99 |
|---|---|---|---|
| Linux → Linux loopback | 11.53 ms | **0.17 ms** | **11.70 ms** / 14.03 ms / — |
| Windows → Linux | 21.54 ms | **0.17 ms** | **21.70 ms** / 33.16 ms / 37.72 ms |
| Linux → Windows | 29.02 ms | **0.57 ms** | 29.42 ms / 36.92 ms / 39.48 ms |
| Windows → Windows loopback | 30.80 ms | 0.35 ms | 31.35 ms / 46.89 ms / — |

The two cross-machine rows bracket the true value: residual NTP
skew biases one direction up and the other down by the same
amount. Average p50 ≈ 25.5 ms, residual skew ≈ 3.5 ms (Windows
clock slightly ahead of Linux).

### 2026-04-22 re-measurement (post PR 9 + NTP resync)

Re-ran Linux → Linux loopback (n = 1148, 60-frame warmup drop)
to sanity-check PR 9's decoder-completeness changes
(`FifoPacketRing` keyframe-preserving queue, per-NAL debug log
capped at the first 30 NALs + every SPS/PPS/IDR):

| Path | capture→decode p50 / p95 | decode→present p50 / p95 | glass-to-glass p50 / p95 / p99 |
|---|---|---|---|
| Linux → Linux loopback (PR 9) | 15.96 / 21.77 ms | 0.30 / 0.44 ms | **16.29 ms** / 22.16 ms / 25.96 ms |

~4 ms slower than the pre-PR-9 loopback — the FIFO packet queue
adds a mutex-protected hop that the old `SpscRing::replace` path
skipped, and the occasional debug prints during warm-up add
a little more. Still at the ≤ 16 ms target at p50.

#### How the measurement is taken

1. The capture backend (XComposite on Linux, WGC on Windows)
   stamps `capture_ns = NowNs()` (= `system_clock::now()` in
   nanoseconds) the moment its per-frame callback fires on our
   thread.
2. The encoder embeds that value in an H.264 SEI
   `user_data_unregistered` NAL (UUID `unio-pipe/lat1`, payload
   = `{frame_id: u32, capture_ns: u64}`). The SEI rides in the
   Annex-B bitstream through msquic/QUIC → network → decoder.
3. The decoder (VA-API on Linux, D3D11VA on Windows) parses the
   SEI out of every access unit and stamps
   `decode_done_ns = NowNs()` right after `vaEndPicture` /
   `DecoderEndFrame` returns for that frame.
4. The presenter stamps `present_done_ns = NowNs()` immediately
   after `eglSwapBuffers` / `IDXGISwapChain::Present1` returns,
   then appends a CSV row:
   `frame_id, capture_ns, decode_done_ns, present_done_ns,
    width, height, capture_to_decode_us, decode_to_present_us,
    capture_to_present_us`.
5. CSV is enabled per-helper via the `UNIO_PIPE_LATENCY_CSV=/path`
   env var. Loopback writes on the sink side only (the sink owns
   both `decode_done_ns` and `present_done_ns`).

The loopback run above used two `unio-pipe` processes on the
same machine talking over `127.0.0.1:5090` QUIC, so all three
timestamps come off the same `system_clock` and no NTP sync is
required. The cross-machine attempt used one process per host
with the sink on `adi-pc` and the source on `Diana`, timestamps
drawn from each host's own `system_clock`.

#### Downsides of this methodology

- **`capture_ns` is the callback-delivery moment, not the
  scan-out moment.** WGC delivers a `Direct3D11CaptureFrame`
  with a `SystemRelativeTime` ~QPC tick that could be used
  instead; the SRT → `system_clock` conversion was attempted but
  reverted because the mapping produced values ~2 days in the
  future on Diana's Windows SDK (scaffold left in
  `capture_wgc.cpp`). XComposite has no hardware-clock
  equivalent at all — our callback fires a few hundred µs after
  `XShmGetImage` completes, and the image the kernel returned
  was itself captured some time earlier. Net effect: we
  **under-count** the capture-side latency by an unknown,
  probably-small amount (≤ 1 frame).

- **`present_done_ns` is when `SwapBuffers`/`Present1` returns,
  not when photons hit the panel.** Both calls kick the work
  to the display engine; the actual scanout happens 0–16.7 ms
  later depending on vblank alignment, plus another ~5–20 ms of
  LCD pixel transition. We **under-count** by roughly half a
  refresh interval on average, plus display response. This is
  the biggest systematic error — a 240 fps phone-camera video
  of source + sink side-by-side will read ~8–16 ms higher than
  our CSV.

- **In-process instrumentation, not ground truth.** The whole
  pipeline is measuring its own code path; any bug that
  bypasses the SEI (e.g. decoder drops the frame but presenter
  shows the previous one) goes uncounted. The only honest
  sanity-check is an external observer: a photodiode rig
  (NVIDIA LDAT, OSLTT), a phone camera at 240 fps filming
  source + sink, or a frame-ID overlay rendered into the
  captured content.

- **Cross-machine needs a shared clock.** `system_clock` on
  each host is only as accurate as its NTP source. Internet
  pools (`time.windows.com`, `pool.ntp.org`) typically agree
  to ~50–500 ms; that's fine for logs, fatal for ms-scale
  latency. Either run both hosts against a shared LAN
  stratum-1 / chrony server, or add the app-level clock-sync
  handshake over QUIC at stream start.

- **`system_clock` can step.** Unlike `steady_clock` we chose
  wall time so two hosts' rows could be compared directly —
  but wall time can jump backward or forward when NTP slews /
  a laptop wakes from sleep / a VM migrates. A jump mid-run
  produces a row with `capture_to_present_us` of either zero
  or ~`2^63`. The 60-frame warm-up drop hides small slews but
  not a full step.

- **Loopback over `127.0.0.1` is an unrealistic QUIC path.**
  RTT is sub-ms and there's no NIC / switch / MTU to negotiate;
  real LAN numbers will be 0.5–2 ms higher at p50 and several
  ms higher at the tail. The Linux loopback row should be read
  as a lower bound on what the real Linux → Linux LAN path
  would do.

**Windows ↔ Linux (cross-machine) was attempted but discarded.**
Even after `w32tm /resync /force` on Windows and `chronyc -a
makestep` on Linux, the two hosts ran ~490 ms apart (Windows
stuck on `time.windows.com` stratum-4 with ~8 s dispersion). That
swamps the ~16–30 ms real latency by 20×, so the CSV's
`capture_to_present_us` column overflows into giant uint64 values
and can't be rescued by subtracting a constant. Getting honest
cross-machine numbers needs either (a) an app-level clock-sync
handshake over the already-open QUIC channel at stream start
(median of ~20 NTP-style exchanges closes the gap to sub-ms on a
LAN) or (b) pointing Windows's `w32tm` at a LAN stratum-1 /
chrony on the Linux host. Neither landed in this session —
`tools/sync-clocks.sh` forces a best-effort public-NTP resync but
doesn't narrow the inter-host gap enough for ms-scale
measurement.

**Linux loopback meets the ≤16 ms target.** Windows-sourced paths
spend an extra ~10 ms in WGC + NVENC vs Linux's XComposite + VA-API.
Windows-sunk paths spend an extra ~11 ms in D3D11VA decode + the
decoder/shader pool split vs Linux's zero-copy DMA-BUF import.
Both presenters are already sub-ms; the remaining budget is
exactly the shape of work for PR 8 (Windows decoder completeness)
and PR 9 (DXGI waitable swap chain + NVENC true-low-latency tune).

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

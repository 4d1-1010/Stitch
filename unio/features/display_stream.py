"""Display streaming — Phase 1 proof-of-pipeline.

One source captures a physical monitor, encodes each frame as JPEG,
and fans the stream out over a dedicated TCP port to every sink
currently routing to it. One sink decodes + paints into a borderless
tk.Toplevel covering the destination monitor.

The transport is intentionally dumb for v1: length-prefixed JPEG
frames over one TCP connection per sink-source pair. The control
plane (who routes where) lives in the LWW `route:<ws_id>` entries —
see shell._workspace_routes. This module just pushes pixels once a
shell wires it up.

Latency budget for this path is 60–120 ms on LAN — enough to prove
"the desktop actually shows up on the other PC" without building the
whole DXGI/NVENC fast path first (that's Phase 5).

Capture backends in priority order:
  1. `mss` — pure-userspace Python, works on Linux X11, Windows,
     macOS. Preferred because it doesn't need any extra binaries.
  2. `PIL.ImageGrab` — Pillow's screenshot helper. Windows and
     macOS native; Linux needs `scrot` / `gnome-screenshot`.

If neither is available the source reports "unsupported" and the
sink shows a placeholder card instead of a black screen.
"""

from __future__ import annotations

import asyncio
import logging
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

log = logging.getLogger(__name__)


STREAM_PORT = 24802
FRAME_HEADER = "<I"              # 4-byte little-endian NAL length
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER)
# H.264-only after PR 4. The JPEG path was proof-of-life for
# Phase 1; once Phase 5 hardware H.264 shipped it became dead
# weight. 60 fps matches every supported sink's refresh.
DEFAULT_FPS_H264 = 60

# Request header sent by the sink right after connecting. Plain JSON
# lets the source pick which monitor to stream without a second round
# trip.
REQUEST_MAGIC = b"UNIO"          # 4 bytes, sanity check
REQUEST_HDR_FMT = "<I"           # 4-byte payload length

# Response header sent by the source back to the sink after receiving
# the request. JSON body carries the chosen codec, frame geometry,
# and fps so the sink can size its decoder + render surface exactly.
RESPONSE_MAGIC = b"UNIR"         # distinct magic so mixing source /
                                 # sink versions fails fast
RESPONSE_HDR_FMT = "<I"

# H.264 is the only codec. The field stays in the wire handshake
# so older sinks that still ask for ``"h264"`` keep working; any
# sink that requests something else gets rejected at negotiate
# time with a clear log.
CODEC_H264 = "h264"
DEFAULT_CODEC_PREFERENCE = (CODEC_H264,)


_CAPTURE_BACKEND_NAME = "unknown"


def capture_backend_name() -> str:
    """Returns the label of the currently-active capture backend
    (``"pipewire"`` on Wayland, ``"wgc"`` on Windows, ``"xcomposite"``
    on X11, ``"none"`` if the probe failed). Kept as a thin accessor
    so the shell doesn't depend on the module global directly."""
    return _CAPTURE_BACKEND_NAME


def capture_backend_respects_exclusion() -> bool:
    """True when the active backend genuinely excludes our overlay
    HWNDs/xids at the pixel-read level. Both supported backends
    (WGC on Windows, XComposite on X11 Linux, PipeWire on Wayland
    Linux) honour the exclusion list. The accessor stays so the
    shell's overlay-feedback code paths keep compiling; when the
    probe fails there's no stream anyway."""
    return _CAPTURE_BACKEND_NAME in ("wgc", "xcomposite", "pipewire")


# Active capture backend instance — set by `_capture_backend()` when
# an exclusion-aware backend is chosen. Shell pushes overlay xids
# here so the backend can skip them during capture.
_CAPTURE_INSTANCE = None


def set_excluded_overlay_xids(xids) -> None:
    """Tell the active capture backend which window IDs belong to
    our own StreamWindow overlays, so the backend can skip them
    while reading pixels. No-op on backends that don't support
    exclusion (mss / Pillow / PrintWindow)."""
    inst = _CAPTURE_INSTANCE
    if inst is None:
        return
    setter = getattr(inst, "set_exclude_xids", None)
    if setter is not None:
        try:
            setter(xids)
        except Exception:
            log.exception("set_exclude_xids failed on %s backend",
                          _CAPTURE_BACKEND_NAME)


def _capture_backend():
    """Pick the capture backend for this host. Exactly one per OS:
    XComposite on Linux, Windows.Graphics.Capture on Windows. No
    fallbacks — a probe failure returns None and the StreamServer
    reports "unsupported", so the sink sees an explicit "no
    capture" error instead of silently hitting a legacy path that
    can't honour WDA_EXCLUDEFROMCAPTURE / xid exclusion.

    Returns a callable taking a ``{x,y,width,height}`` bbox and
    returning a PIL.Image, or None when capture is unavailable."""
    global _CAPTURE_BACKEND_NAME, _CAPTURE_INSTANCE
    _CAPTURE_INSTANCE = None
    import sys as _sys
    if _sys.platform.startswith("linux"):
        # Detect display server to pick the right backend.
        from . import display_server as _ds
        _display = _ds.detect()
        log.info("Display server detected: %s", _display)

        if _display == "wayland":
            # Wayland: try PipeWire / xdg-desktop-portal first.
            try:
                from .capture_pipewire import (
                    PipeWireCapture,
                    available as _pw_available,
                )
                log.info("PipeWire probe: available=%s", _pw_available())
                if _pw_available():
                    pw_cap = PipeWireCapture()
                    if pw_cap.open():
                        probe = pw_cap.grab(
                            {"x": 0, "y": 0,
                             "width": 16, "height": 16})
                        if probe is not None:
                            _CAPTURE_BACKEND_NAME = "pipewire"
                            _CAPTURE_INSTANCE = pw_cap
                            log.info("Capture backend: PipeWire")

                            def _pw_grab(bbox: dict):
                                return pw_cap.grab(bbox)
                            return _pw_grab
                        pw_cap.close()
            except Exception:
                log.exception("PipeWire backend init failed")
            log.info("PipeWire unavailable on this Wayland host — "
                     "display streaming will not start. "
                     "Needs xdg-desktop-portal with ScreenCast "
                     "support and a running PipeWire daemon.")
            _CAPTURE_BACKEND_NAME = "none"
            return None

        # X11: XComposite (unchanged).
        try:
            from .capture_xcomposite import (
                XCompositeCapture,
                available as _xc_available,
            )
            log.info("XComposite probe: libs available=%s",
                     _xc_available())
            if _xc_available():
                xc_cap = XCompositeCapture()
                if xc_cap.open():
                    probe = xc_cap.grab(
                        {"x": 0, "y": 0, "width": 16, "height": 16})
                    if probe is not None:
                        _CAPTURE_BACKEND_NAME = "xcomposite"
                        _CAPTURE_INSTANCE = xc_cap
                        log.info("Capture backend: XComposite")

                        def _xc_grab(bbox: dict):
                            return xc_cap.grab(bbox)
                        return _xc_grab
                    xc_cap.close()
        except Exception:
            log.exception("XComposite backend init failed")
        log.warning("XComposite unavailable on this Linux host — "
                    "display streaming will not start. Needs a "
                    "running X11 compositor (Mutter / KWin / picom).")
        _CAPTURE_BACKEND_NAME = "none"
        return None

    if _sys.platform == "win32":
        try:
            from .capture_windows_wgc import (
                WGCCapture,
                available as _wgc_available,
            )
            if _wgc_available():
                wgc_cap = WGCCapture()
                if wgc_cap.open():
                    probe = wgc_cap.grab(
                        {"x": wgc_cap.origin_x,
                         "y": wgc_cap.origin_y,
                         "width": 16, "height": 16})
                    if probe is not None:
                        _CAPTURE_BACKEND_NAME = "wgc"
                        _CAPTURE_INSTANCE = wgc_cap
                        log.info("Capture backend: "
                                 "Windows.Graphics.Capture")

                        def _wgc_grab(bbox: dict):
                            return wgc_cap.grab(bbox)
                        return _wgc_grab
                    wgc_cap.close()
        except Exception:
            log.exception("WGC backend init failed")
        log.warning("Windows.Graphics.Capture unavailable on this "
                    "Windows host — display streaming will not "
                    "start. Needs Windows 10 20H1+ (WGC v1) or "
                    "Windows 11 (WGC v2). Citrix / RDP / enterprise "
                    "lock-down can also block it.")
        _CAPTURE_BACKEND_NAME = "none"
        return None

    _CAPTURE_BACKEND_NAME = "none"
    return None


def _nal_starts_with_sps_or_pps(chunk: bytes) -> bool:
    """True when the bytes start with an Annex-B start code followed
    by an SPS (NAL type 7) or PPS (NAL type 8). Our encoders emit
    SPS+PPS immediately before each IDR frame, so detecting either
    is equivalent to detecting a GOP boundary.

    NAL type sits in the low 5 bits of the byte after the start
    code. Start code is either `00 00 00 01` or `00 00 01`.
    """
    if len(chunk) < 5:
        return False
    # Skip the start code.
    if chunk[:4] == b"\x00\x00\x00\x01":
        kind = chunk[4] & 0x1F
    elif chunk[:3] == b"\x00\x00\x01":
        kind = chunk[3] & 0x1F
    else:
        return False
    return kind in (7, 8)   # 7=SPS, 8=PPS


# ── Source side ──────────────────────────────────────────────────────


@dataclass
class _SourceMonitor:
    """One physical monitor being offered as a stream source."""
    monitor_id: str
    x: int
    y: int
    width: int
    height: int
    h264_subscribers: set = field(default_factory=set)
    capture_task: Optional[asyncio.Task] = None
    # H.264 keyframe cache — bytes emitted since the most recent
    # SPS/PPS/IDR boundary. When a new sink joins mid-stream we
    # send this chunk first so its decoder has an anchor to start
    # from, rather than waiting up to 1 s for the encoder to emit
    # its next scheduled keyframe.
    h264_keyframe_cache: bytes = b""

    @property
    def all_subscribers(self) -> set:
        return self.h264_subscribers

    def has_subscribers(self) -> bool:
        return bool(self.h264_subscribers)


class StreamServer:
    """One-per-peer TCP listener + capture multiplexer. Accepts
    connections on STREAM_PORT, reads the sink's request (which
    monitor it wants), and streams H.264 NALs to it."""

    def __init__(self, machine_id: str, port: int = STREAM_PORT):
        self.machine_id = machine_id
        self.port = port
        self._server: Optional[asyncio.AbstractServer] = None
        self._monitors: dict[str, _SourceMonitor] = {}
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._capture = _capture_backend()
        self._lock = threading.Lock()
        # Subscribe-time pairing check. Called with the sink's
        # claimed machine_id right after the handshake; returns
        # True when that machine is in the peer's current workspace
        # membership (i.e. authed, paired, and opted in to sharing
        # with us). None means "not wired yet" and lets the
        # connection through — during the brief window between
        # StreamServer.start() and Peer setting the callback we
        # don't want to drop legitimate reconnects. Once the shell
        # has finished wiring, the callback is non-None for the
        # rest of the process lifetime.
        self.is_sink_allowed: Optional[Callable[[str], bool]] = None
        # Gossip-settle grace window: for the first ~2 s after
        # start(), mesh membership may still be propagating from
        # presence gossip and a legitimate reconnect could land
        # before allowed_peer_ids has converged. We accept all
        # subscribes during that window and let the 24800 auth on
        # the control plane catch anything truly unauthorised;
        # after the grace expires, the strict is_sink_allowed
        # check kicks in.
        self._start_grace_seconds = 2.0
        self._started_at: Optional[float] = None

    def capture_supported(self) -> bool:
        return self._capture is not None

    async def start(self) -> None:
        self._loop = asyncio.get_running_loop()
        try:
            self._server = await asyncio.start_server(
                self._on_sink, host="0.0.0.0", port=self.port,
            )
        except OSError as e:
            log.warning("StreamServer bind %d failed: %s", self.port, e)
            self._server = None
            return
        self._started_at = time.monotonic()
        log.info("StreamServer %s listening on TCP %d (capture=%s)",
                 self.machine_id, self.port,
                 "yes" if self._capture else "unsupported")

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            try:
                await self._server.wait_closed()
            except Exception:
                pass
            self._server = None
        for src in list(self._monitors.values()):
            for w in list(src.all_subscribers):
                try:
                    w.close()
                except Exception:
                    pass
            if src.capture_task:
                src.capture_task.cancel()
        self._monitors.clear()

    def set_monitors(self, monitors: list[dict]) -> None:
        """Update the list of monitors we're willing to serve. Called
        whenever the peer's own monitor snapshot changes so a plug /
        unplug mid-session immediately reshapes the source list."""
        with self._lock:
            present = set()
            for m in monitors:
                mid = str(m.get("monitor_id") or m.get("id") or "")
                if not mid:
                    continue
                present.add(mid)
                src = self._monitors.get(mid)
                if src is None:
                    self._monitors[mid] = _SourceMonitor(
                        monitor_id=mid,
                        x=int(m.get("local_x", m.get("x", 0))),
                        y=int(m.get("local_y", m.get("y", 0))),
                        width=int(m.get("width", 0)),
                        height=int(m.get("height", 0)),
                    )
                else:
                    src.x = int(m.get("local_x", m.get("x", 0)))
                    src.y = int(m.get("local_y", m.get("y", 0)))
                    src.width = int(m.get("width", 0))
                    src.height = int(m.get("height", 0))
            # Drop any source that unplugged — close subscribers so
            # the sink sees EOF and can surface "source disconnected".
            gone = [mid for mid, src in self._monitors.items()
                    if mid not in present]
            for mid in gone:
                src = self._monitors.pop(mid)
                for w in list(src.all_subscribers):
                    try:
                        w.close()
                    except Exception:
                        pass
                if src.capture_task:
                    src.capture_task.cancel()

    async def _on_sink(self, reader: asyncio.StreamReader,
                       writer: asyncio.StreamWriter) -> None:
        """Handle one sink connection. Read the request, negotiate a
        codec with the sink, subscribe to the matching source's
        fan-out set, then block until the writer closes (capture loop
        does the sending)."""
        peer = writer.get_extra_info("peername")
        log.info("stream sink connected from %s", peer)
        try:
            magic = await reader.readexactly(4)
            if magic != REQUEST_MAGIC:
                log.info("stream: bad magic from %s: %r", peer, magic)
                return
            hdr = await reader.readexactly(struct.calcsize(REQUEST_HDR_FMT))
            (plen,) = struct.unpack(REQUEST_HDR_FMT, hdr)
            if plen <= 0 or plen > 4096:
                log.info("stream: bad request len %d from %s", plen, peer)
                return
            body = await reader.readexactly(plen)
            import json as _json
            req = _json.loads(body.decode("utf-8", errors="replace"))
            monitor_id = str(req.get("monitor_id") or "")
            sink_machine_id = str(req.get("sink_machine_id") or "")
            # Sink sends its preferred codec list — source picks the
            # first one it can serve. The source is H.264-only after
            # PR 4; sinks that don't ask for h264 get rejected below.
            sink_codecs = req.get("codecs") or [CODEC_H264]
            if isinstance(sink_codecs, str):
                sink_codecs = [sink_codecs]
        except (asyncio.IncompleteReadError, ValueError, UnicodeDecodeError):
            log.info("stream: malformed request from %s", peer)
            return

        # Pairing gate: the subscribing sink must identify as a
        # machine in the current workspace's membership. Without
        # this check, anyone on the LAN who can reach STREAM_PORT
        # can subscribe to any of our displays — no auth, no
        # pairing challenge. The mesh's sign-in auth already gates
        # the 24800 control plane; this closes the parallel hole
        # on the 24802 data plane.
        #
        # Grace window: for the first ``_start_grace_seconds``
        # after start(), mesh gossip may still be converging; we
        # accept and log instead of rejecting, so a peer that
        # reconnected faster than we learned about it doesn't get
        # a false-reject.
        if self.is_sink_allowed is not None:
            in_grace = False
            if self._started_at is not None:
                in_grace = (time.monotonic() - self._started_at
                            < self._start_grace_seconds)
            if not sink_machine_id:
                log.info("stream: rejecting %s — missing "
                         "sink_machine_id in request", peer)
                try:
                    writer.close()
                except Exception:
                    pass
                return
            if not self.is_sink_allowed(sink_machine_id):
                if in_grace:
                    log.info(
                        "stream: accepting %s within gossip grace "
                        "(%.1fs) sink_machine_id=%r",
                        peer,
                        self._start_grace_seconds,
                        sink_machine_id)
                else:
                    log.info(
                        "stream: rejecting %s — sink_machine_id=%r "
                        "not in active workspace membership",
                        peer, sink_machine_id)
                    try:
                        writer.close()
                    except Exception:
                        pass
                    return

        with self._lock:
            src = self._monitors.get(monitor_id)
        if src is None or not self._capture:
            log.info("stream: no source %s (have=%s capture=%s)",
                     monitor_id, list(self._monitors), bool(self._capture))
            try:
                writer.close()
            except Exception:
                pass
            return

        # H.264-only: a sink that asks for anything else (or an
        # older client that still asks for JPEG) gets rejected
        # with a loud log. The field stays in the handshake so the
        # wire error the sink sees is "connection closed after
        # RESPONSE_MAGIC", which its existing error path handles.
        if CODEC_H264 not in sink_codecs:
            log.info(
                "stream: rejecting %s — sink_codecs=%r does not "
                "include h264 (JPEG was removed in PR 4)",
                peer, sink_codecs)
            try:
                writer.close()
            except Exception:
                pass
            return
        codec = CODEC_H264
        fps = DEFAULT_FPS_H264
        import json as _json
        resp_body = _json.dumps({
            "codec": codec,
            "width": src.width,
            "height": src.height,
            "fps": fps,
        }).encode("utf-8")
        try:
            writer.write(RESPONSE_MAGIC
                         + struct.pack(RESPONSE_HDR_FMT, len(resp_body))
                         + resp_body)
            await writer.drain()
        except (ConnectionResetError, OSError):
            return

        src.h264_subscribers.add(writer)
        # Catch the new subscriber up with the last keyframe chunk
        # so their decoder doesn't block waiting for the next
        # scheduled IDR (~0.25 s away). Skipped when the cache is
        # empty (fresh source, no keyframes emitted yet).
        if src.h264_keyframe_cache:
            cache = src.h264_keyframe_cache
            try:
                writer.write(
                    struct.pack(FRAME_HEADER, len(cache)) + cache
                )
            except Exception:
                pass
        log.info("stream sink %s subscribed to %s via %s",
                 peer, monitor_id, codec)
        if src.capture_task is None or src.capture_task.done():
            src.capture_task = asyncio.create_task(
                self._capture_loop(src),
            )

        try:
            while True:
                chunk = await reader.read(1024)
                if not chunk:
                    break
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        finally:
            src.h264_subscribers.discard(writer)
            try:
                writer.close()
            except Exception:
                pass
            log.info("stream sink %s disconnected from %s",
                     peer, monitor_id)

    async def _capture_loop(self, src: _SourceMonitor) -> None:
        """One capture loop per active source monitor. Grabs the
        screen, feeds the H.264 encoder if any sink is subscribed,
        and fans NALs out over the subscriber sockets. Dies as
        soon as the subscriber set empties so an unused source
        stops burning CPU."""
        log.info("capture loop starting for %s (%dx%d at %d,%d)",
                 src.monitor_id, src.width, src.height, src.x, src.y)
        grab = self._capture
        # Per-source latency tracer. Stamps at capture-grab-return,
        # encoder-submit, encoder-out (in _hw_reader_loop), and
        # socket-send (also in _hw_reader_loop). Frame IDs are just
        # a monotonic source counter — meaningful within this stream,
        # not across machines.
        from .latency_trace import get_tracer, Stage
        tracer = get_tracer(f"src:{src.monitor_id}")
        src._latency_tracer = tracer
        frame_counter = 0
        hw_encoder = None
        hw_reader_task: Optional[asyncio.Task] = None
        # XDamage watcher (Linux X11 only) — gates the whole capture
        # + encode cycle. Returns True on first call so the initial
        # keyframe always ships; subsequent calls only return True
        # when the X server reports pixel changes.
        damage = None
        try:
            from .dirty_rect_x11 import XDamageWatcher, available as _xd_ok
            if _xd_ok():
                damage = XDamageWatcher()
                if not damage.open():
                    damage = None
        except Exception:
            log.exception("XDamage init failed")
            damage = None
        first_tick = True
        fps = DEFAULT_FPS_H264
        period = 1.0 / fps
        next_tick = time.monotonic()
        while src.has_subscribers():
            # XDamage short-circuit: if nothing on the X display
            # changed since last grab, skip the whole capture +
            # encode cycle. The sink keeps rendering its last frame
            # and H.264's motion compensation produces nothing on
            # the wire. First tick always runs so the new subscriber
            # gets its keyframe.
            if damage is not None and not first_tick:
                if not damage.has_damage():
                    await asyncio.sleep(1.0 / fps)
                    continue
            first_tick = False
            rect = {"x": src.x, "y": src.y,
                    "width": src.width, "height": src.height}
            try:
                img = await asyncio.get_running_loop().run_in_executor(
                    None, grab, rect,
                )
            except Exception:
                log.exception("capture failed for %s", src.monitor_id)
                await asyncio.sleep(1.0)
                continue
            if img is None:
                await asyncio.sleep(0.2)
                continue

            # Stamp end-of-capture. Every downstream source-side
            # stage (encoder-in, encoder-out, send) is measured as
            # a delta from this point, so the "grab->enc_in" number
            # is pure Python/asyncio handoff cost and "encode" is
            # pure ffmpeg time.
            frame_counter += 1
            current_frame_id = frame_counter
            tracer.stamp(current_frame_id, Stage.CAPTURE_GRAB)

            if src.h264_subscribers:
                if hw_encoder is None:
                    hw_encoder = self._build_hw_encoder(src, fps)
                    if hw_encoder is not None:
                        hw_reader_task = asyncio.create_task(
                            self._hw_reader_loop(src, hw_encoder))
                # H.264 needs every frame fed to the encoder — the
                # codec's own motion compensation handles the
                # "unchanged" case by emitting tiny P-frames, so the
                # dirty-rect skip would actually starve the decoder
                # of the keepalive it needs to start producing output.
                #
                # Fast path: when the backend published a raw BGRX
                # buffer (xcomposite fills one during its composite
                # pass), feed ffmpeg those bytes directly with
                # ``-pix_fmt bgra``. Skips a PIL BGRX→RGB conversion
                # on our side AND the slower RGB→YUV SIMD path on
                # ffmpeg's side. Falls back to RGB24 when the
                # backend doesn't expose a raw buffer.
                if hw_encoder is not None:
                    try:
                        raw = None
                        if hw_encoder.pix_fmt == "bgra":
                            inst = _CAPTURE_INSTANCE
                            getter = getattr(
                                inst, "last_bgra_bytes", None)
                            if getter is not None:
                                raw = getter()
                        if raw is not None \
                                and len(raw) == hw_encoder.frame_bytes:
                            hw_encoder.write_frame(raw)
                        else:
                            hw_encoder.write_frame(img.tobytes())
                        tracer.stamp(current_frame_id, Stage.ENC_SUBMIT)
                        # Queue the frame_id for the hw reader loop
                        # so it can tag the next NAL it reads. ffmpeg
                        # is mostly one-in-one-out at zerolatency,
                        # so a FIFO keeps encode/output paired up
                        # without fancy correlation.
                        try:
                            src._enc_frame_queue.append(
                                current_frame_id)
                        except AttributeError:
                            src._enc_frame_queue = [current_frame_id]
                    except Exception:
                        log.exception("hw encoder write failed")

            next_tick += period
            now = time.monotonic()
            sleep_for = max(0.0, next_tick - now)
            if sleep_for == 0.0 and now - next_tick > period:
                next_tick = now
            await asyncio.sleep(sleep_for)

        if hw_encoder is not None:
            hw_encoder.stop()
        if hw_reader_task is not None:
            hw_reader_task.cancel()
        if damage is not None:
            damage.close()
        src.capture_task = None
        log.info("capture loop ending for %s", src.monitor_id)

    def _build_hw_encoder(self, src: _SourceMonitor, fps: int):
        try:
            from .hw_pipeline import HWEncoder, pick_hw_encoder
        except Exception:
            return None
        # No CPU fallback any more (PR 1 stripped libopenh264 for
        # commercial-redist compliance). If the host has no HW
        # encoder, log loud and return None — the sink sees the
        # stream go dead instead of silently getting a legally
        # shaky CPU path. The source keeps running and will pick
        # up the next time a compatible host subscribes.
        picked = pick_hw_encoder()
        if not picked:
            log.warning(
                "no HW H.264 encoder available on this host — "
                "stream for %s will NOT start. Install NVENC / QSV "
                "/ AMF / VA-API drivers to enable.",
                src.monitor_id)
            return None
        # If the active capture backend publishes a raw BGRA buffer
        # (xcomposite), hand ffmpeg 4-byte BGRA directly — saves the
        # per-frame BGRX→RGB conversion AND gives ffmpeg a faster
        # SIMD path to YUV. Backends without that accessor stay on
        # the rgb24 path.
        pix_fmt = "rgb24"
        inst = _CAPTURE_INSTANCE
        if inst is not None and hasattr(inst, "last_bgra_bytes"):
            pix_fmt = "bgra"
        enc = HWEncoder(width=src.width, height=src.height, fps=fps,
                        encoder=picked, pix_fmt=pix_fmt)
        if not enc.start():
            return None
        return enc

    async def _hw_reader_loop(self, src: _SourceMonitor, enc) -> None:
        """Drain the HW encoder's stdout in its own task so the
        capture loop never blocks on ffmpeg. Chunks the Annex-B
        byte stream into length-prefixed transport frames matching
        the sink's expected framing.

        Also maintains the keyframe cache. H.264 Annex-B marks
        parameter sets and IDR frames with a start-code-prefixed
        NAL unit; we detect SPS/PPS boundaries and reset the cache
        at each one, bounding it to keep a single GOP's worth of
        bytes on hand. That cache primes any late-joining sink so
        its decoder has an anchor frame immediately."""
        loop = asyncio.get_running_loop()
        total = 0
        cache = bytearray()
        # Hard cap so a misbehaving encoder can't grow the cache
        # without bound. One second at 8 Mbps ≈ 1 MiB; 4× overhead
        # covers bursty I-frames.
        cache_cap = 4 * 1024 * 1024
        from .latency_trace import Stage
        tracer = getattr(src, "_latency_tracer", None)
        while src.h264_subscribers:
            nal = await loop.run_in_executor(None, enc.read_nal)
            if not nal:
                await asyncio.sleep(0.005)
                continue
            # Stamp encoder-out + send. ffmpeg at zerolatency is
            # effectively one-in-one-out, so popping the earliest
            # queued frame_id per emitted NAL gives us a sound
            # correlation. If the queue is empty (encoder emitted
            # parameter sets independently) we skip the stamp.
            emitted_frame_id: Optional[int] = None
            if tracer is not None:
                q = getattr(src, "_enc_frame_queue", None)
                if q:
                    emitted_frame_id = q.pop(0)
                    tracer.stamp(emitted_frame_id, Stage.ENC_OUT)
            total += len(nal)
            if _nal_starts_with_sps_or_pps(nal):
                # New GOP — cache starts fresh.
                cache = bytearray()
            cache.extend(nal)
            if len(cache) > cache_cap:
                # Drop everything before the most recent start-code
                # pair so we keep a self-describing prefix.
                idx = cache.rfind(b"\x00\x00\x00\x01", 0,
                                  len(cache) - 64)
                if idx > 0:
                    del cache[:idx]
                else:
                    cache = bytearray(cache[-cache_cap:])
            src.h264_keyframe_cache = bytes(cache)

            frame = struct.pack(FRAME_HEADER, len(nal)) + nal
            dead = []
            for w in list(src.h264_subscribers):
                try:
                    w.write(frame)
                except Exception:
                    dead.append(w)
            if tracer is not None and emitted_frame_id is not None:
                tracer.stamp(emitted_frame_id, Stage.SEND)
                tracer.maybe_log()
            for w in dead:
                src.h264_subscribers.discard(w)
                try:
                    w.close()
                except Exception:
                    pass
        log.info("hw reader loop ending for %s (sent %d bytes)",
                 src.monitor_id, total)


# ── Sink side ────────────────────────────────────────────────────────


class StreamSink:
    """Receive a stream from a source peer. Runs a background socket
    reader thread that calls `on_frame(data, codec)` for every
    decoded packet.

    `data` is JPEG bytes when the negotiated codec is JPEG and raw
    RGB bytes (width*height*3) when it's H.264. `codec` lets the
    renderer branch without having to sniff the data itself. The
    reader thread also exposes the negotiated (width, height, fps)
    after handshake via `negotiated` so the caller can size the
    render surface exactly.
    """

    def __init__(self, host: str, port: int, monitor_id: str,
                 sink_machine_id: str,
                 on_frame: Callable[[bytes, str], None],
                 on_error: Optional[Callable[[str], None]] = None,
                 preferred_codecs: tuple = DEFAULT_CODEC_PREFERENCE):
        self.host = host
        self.port = port
        self.monitor_id = monitor_id
        self.sink_machine_id = sink_machine_id
        self.on_frame = on_frame
        self.on_error = on_error
        # H.264 only after PR 4. The codec list still rides in the
        # handshake for forward-compat but always resolves to h264.
        self.preferred_codecs = (CODEC_H264,)
        self._thread: Optional[threading.Thread] = None
        self._stopping = threading.Event()
        # Filled from the source's response header on handshake —
        # width/height/fps for the negotiated stream. Callers
        # interested in sizing a render surface can poll this after
        # start() has been alive for ~200 ms.
        self.negotiated: Optional[dict] = None

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stopping.clear()
        self._thread = threading.Thread(
            target=self._run, daemon=True,
            name=f"stream-sink:{self.monitor_id}",
        )
        self._thread.start()

    def stop(self) -> None:
        """Signal the reader thread to exit. Returns immediately —
        the caller doesn't block waiting for socket cleanup, so a
        route change doesn't freeze the UI for up to a second while
        the old sink times out its recv. The thread finishes in the
        background; its socket + ffmpeg process get closed via
        their respective finally blocks."""
        self._stopping.set()

    def _run(self) -> None:
        try:
            sock = socket.create_connection((self.host, self.port),
                                            timeout=3.0)
        except OSError as e:
            log.info("stream connect %s:%d failed: %s",
                     self.host, self.port, e)
            if self.on_error:
                self.on_error(str(e))
            return
        sock.settimeout(1.0)

        import json as _json
        req = _json.dumps({
            "sink_machine_id": self.sink_machine_id,
            "monitor_id": self.monitor_id,
            "codecs": list(self.preferred_codecs),
        }).encode("utf-8")
        try:
            sock.sendall(REQUEST_MAGIC + struct.pack(REQUEST_HDR_FMT,
                                                     len(req)) + req)
        except OSError as e:
            log.info("stream request send failed: %s", e)
            sock.close()
            if self.on_error:
                self.on_error(str(e))
            return

        # Read the source's response header before touching frames.
        buf = bytearray()
        codec = CODEC_H264
        width = height = 0
        try:
            while len(buf) < 4 and not self._stopping.is_set():
                chunk = sock.recv(4 - len(buf))
                if not chunk:
                    raise OSError("eof before response magic")
                buf.extend(chunk)
            if bytes(buf[:4]) != RESPONSE_MAGIC:
                raise OSError(f"bad response magic {bytes(buf[:4])!r}")
            del buf[:4]
            while len(buf) < struct.calcsize(RESPONSE_HDR_FMT):
                chunk = sock.recv(struct.calcsize(RESPONSE_HDR_FMT)
                                  - len(buf))
                if not chunk:
                    raise OSError("eof in response length")
                buf.extend(chunk)
            (plen,) = struct.unpack(
                RESPONSE_HDR_FMT,
                bytes(buf[:struct.calcsize(RESPONSE_HDR_FMT)]))
            del buf[:struct.calcsize(RESPONSE_HDR_FMT)]
            while len(buf) < plen:
                chunk = sock.recv(plen - len(buf))
                if not chunk:
                    raise OSError("eof in response body")
                buf.extend(chunk)
            body = bytes(buf[:plen])
            del buf[:plen]
            resp = _json.loads(body.decode("utf-8", errors="replace"))
            codec = str(resp.get("codec") or CODEC_H264)
            if codec != CODEC_H264:
                raise OSError(
                    f"source returned unsupported codec {codec!r} "
                    "— only h264 is supported after PR 4")
            width = int(resp.get("width") or 0)
            height = int(resp.get("height") or 0)
            self.negotiated = {
                "codec": codec, "width": width, "height": height,
                "fps": int(resp.get("fps") or 0),
            }
            log.info("stream sink negotiated %s @ %dx%d",
                     codec, width, height)
        except (OSError, ValueError, UnicodeDecodeError) as e:
            log.info("stream handshake failed: %s", e)
            sock.close()
            if self.on_error:
                self.on_error(str(e))
            return

        # Only H.264 is supported after PR 4; handshake rejects
        # anything else before this point.
        try:
            self._recv_h264(sock, buf, width, height)
        finally:
            try:
                sock.close()
            except Exception:
                pass
            log.info("stream sink thread for %s stopped",
                     self.monitor_id)

    def _recv_h264(self, sock, initial_buf: bytearray,
                   width: int, height: int) -> None:
        """H.264 path: drive an ffmpeg decoder subprocess. Incoming
        TCP bytes are length-prefixed Annex-B NAL groups from our
        encoder; we feed ffmpeg a concatenated stream and read raw
        RGB frames out, then fire on_frame for each completed frame.
        Two background threads (feeder + puller) keep ffmpeg's pipes
        from deadlocking on full buffers."""
        try:
            from .hw_pipeline import HWDecoder
        except Exception:
            log.exception("hw_pipeline import failed; giving up on h264 sink")
            return
        dec = HWDecoder(width=width, height=height)
        if not dec.start():
            log.info("HW decoder start failed; falling back to JPEG "
                     "would require reconnect — giving up for now")
            if self.on_error:
                self.on_error("ffmpeg decoder unavailable")
            return

        stop_reading = threading.Event()
        # Per-sink tracer. Stamps at RECV (NAL fully pulled from
        # socket), DEC_IN (bytes pushed into ffmpeg), DEC_OUT
        # (decoded RGB frame read back). PRESENT is stamped by
        # the renderer in stream_window via self._latency_tracer.
        from .latency_trace import get_tracer, Stage
        tracer = get_tracer(f"sink:{self.monitor_id}")
        self._latency_tracer = tracer
        # FIFO of sink-side frame IDs. Encoder→decoder at
        # zerolatency is effectively 1:1 at NAL level for P-frames;
        # SPS/PPS don't emit a decoded frame. The queue gets a
        # frame_id appended for every NAL fed in, and the puller
        # pops one for every RGB frame read. Mismatches are
        # tolerated (SPS/PPS): the puller recovers on the next
        # decoded frame.
        frame_counter = [0]  # list for nonlocal mutation
        dec_frame_queue: list = []

        def _puller():
            while not stop_reading.is_set():
                frame = dec.read_frame()
                if frame is None:
                    break
                # Pair DEC_OUT with the MOST RECENT DEC_IN instead of
                # the oldest. Many NALs don't emit a decoded frame
                # (SPS / PPS / SEI), and the decoder's startup buffers
                # several NALs before producing its first output.
                # Oldest-wins would bind DEC_OUT to a pre-startup NAL
                # and blow the "decode" transition to seconds. Newest-
                # wins approximates "the frame we just got out" and
                # gives meaningful percentiles in steady state.
                fid = None
                if dec_frame_queue:
                    fid = dec_frame_queue[-1]
                    dec_frame_queue.clear()
                    tracer.stamp(fid, Stage.DEC_OUT)
                try:
                    self.on_frame(frame, CODEC_H264)
                except Exception:
                    log.exception("h264 on_frame callback failed")
                # Stamp PRESENT here (right after the sink callback
                # returns) rather than inside StreamWindow. Tk's paste
                # may still be pending an idle callback but the sink
                # side of the pipeline is done. Keeping both stamps
                # under the same fid is what makes DEC_OUT→PRESENT
                # meaningful; the window's own counter was unrelated.
                if fid is not None:
                    tracer.stamp(fid, Stage.PRESENT)
                tracer.maybe_log()

        puller = threading.Thread(target=_puller, daemon=True,
                                  name=f"h264-pull:{self.monitor_id}")
        puller.start()

        def _feed_nal(nal_bytes: bytes) -> bool:
            """Push a NAL into the decoder with RECV+DEC_IN stamps."""
            frame_counter[0] += 1
            fid = frame_counter[0]
            tracer.stamp(fid, Stage.RECV)
            ok = dec.write_nal(nal_bytes)
            if ok:
                tracer.stamp(fid, Stage.DEC_IN)
                dec_frame_queue.append(fid)
            return ok

        buf = initial_buf
        try:
            # Consume any leftover from the handshake immediately.
            while buf and not self._stopping.is_set():
                if len(buf) >= FRAME_HEADER_SIZE:
                    (frame_len,) = struct.unpack(
                        FRAME_HEADER, bytes(buf[:FRAME_HEADER_SIZE]))
                    if len(buf) >= FRAME_HEADER_SIZE + frame_len:
                        nal = bytes(
                            buf[FRAME_HEADER_SIZE:
                                FRAME_HEADER_SIZE + frame_len])
                        del buf[:FRAME_HEADER_SIZE + frame_len]
                        _feed_nal(nal)
                        continue
                break
            while not self._stopping.is_set():
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    continue
                except OSError as e:
                    log.info("stream recv failed (h264): %s", e)
                    break
                if not chunk:
                    break
                buf.extend(chunk)
                while len(buf) >= FRAME_HEADER_SIZE:
                    (frame_len,) = struct.unpack(
                        FRAME_HEADER, bytes(buf[:FRAME_HEADER_SIZE]))
                    if frame_len <= 0 or frame_len > 8 * 1024 * 1024:
                        log.info("stream: bogus h264 frame len %d",
                                 frame_len)
                        buf.clear()
                        self._stopping.set()
                        break
                    if len(buf) < FRAME_HEADER_SIZE + frame_len:
                        break
                    nal = bytes(
                        buf[FRAME_HEADER_SIZE:
                            FRAME_HEADER_SIZE + frame_len])
                    del buf[:FRAME_HEADER_SIZE + frame_len]
                    if not _feed_nal(nal):
                        break
        finally:
            stop_reading.set()
            dec.stop()
            puller.join(timeout=1.0)

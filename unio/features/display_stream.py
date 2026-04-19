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
import io
import logging
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

log = logging.getLogger(__name__)


STREAM_PORT = 24802
FRAME_HEADER = "<I"              # 4-byte little-endian JPEG length
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER)
# Phase 1 target 15 fps (JPEG). Phase 5 pushes this to 30 fps once
# hardware H.264 is on — see DEFAULT_FPS_H264 below.
DEFAULT_FPS = 15
DEFAULT_FPS_H264 = 30
JPEG_QUALITY = 60

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

# Codecs the source knows how to emit. Negotiated per-connection so a
# modern sink gets H.264 on hosts with ffmpeg while an older sink
# still falls back to the JPEG path.
CODEC_H264 = "h264"
CODEC_JPEG = "jpeg"
DEFAULT_CODEC_PREFERENCE = (CODEC_H264, CODEC_JPEG)


def _capture_backend():
    """Pick the best available capture backend. Returns a callable
    that, given a dict with x/y/width/height, returns a PIL.Image
    snapshot of that rectangle — or None if capture is unsupported
    on this host.

    The mss path creates a fresh `mss.mss()` instance per thread
    because mss stashes its X11 / D3D handles in thread-local state —
    running from an asyncio executor without this handling crashes
    with "_thread._local object has no attribute 'display'".
    """
    try:
        import mss  # type: ignore
        import threading as _threading
        # Smoke-test once on the main thread so we fail fast if mss
        # can't find the display at all.
        mss.mss().close()
        _tls = _threading.local()

        def _grab(bbox: dict):
            from PIL import Image  # local import; Pillow is already a dep
            sct = getattr(_tls, "sct", None)
            if sct is None:
                sct = mss.mss()
                _tls.sct = sct
            region = {"left": bbox["x"], "top": bbox["y"],
                      "width": bbox["width"], "height": bbox["height"]}
            raw = sct.grab(region)
            return Image.frombytes("RGB", raw.size, raw.rgb)
        return _grab
    except Exception as e:
        log.info("mss capture unavailable (%s); trying PIL.ImageGrab", e)

    try:
        from PIL import ImageGrab  # type: ignore

        def _grab(bbox: dict):
            x, y, w, h = bbox["x"], bbox["y"], bbox["width"], bbox["height"]
            return ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)
        # Probe once so we know it actually works on this host.
        try:
            _grab({"x": 0, "y": 0, "width": 16, "height": 16})
        except Exception as e:
            log.info("PIL.ImageGrab probe failed: %s", e)
            return None
        return _grab
    except Exception as e:
        log.info("PIL.ImageGrab unavailable (%s)", e)
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
    # Subscribers are split per codec so a JPEG sink and an H.264
    # sink on the same source don't cross-pollute streams. Each set
    # holds asyncio.StreamWriter instances.
    jpeg_subscribers: set = field(default_factory=set)
    h264_subscribers: set = field(default_factory=set)
    capture_task: Optional[asyncio.Task] = None
    # Virtual sources don't have a physical panel to capture from —
    # we serve a placeholder stream to their subscribers instead so
    # the sink side renders a clear "virtual display not yet backed"
    # card instead of timing out.
    is_virtual: bool = False
    placeholder_text: str = ""
    # H.264 keyframe cache — bytes emitted since the most recent
    # SPS/PPS/IDR boundary. When a new sink joins mid-stream we
    # send this chunk first so its decoder has an anchor to start
    # from, rather than waiting up to 1 s for the encoder to emit
    # its next scheduled keyframe.
    h264_keyframe_cache: bytes = b""

    @property
    def all_subscribers(self) -> set:
        return self.jpeg_subscribers | self.h264_subscribers

    def has_subscribers(self) -> bool:
        return bool(self.jpeg_subscribers) or bool(self.h264_subscribers)


class StreamServer:
    """One-per-peer TCP listener + capture multiplexer. Accepts
    connections on STREAM_PORT, reads the sink's request (which
    monitor it wants), and starts streaming JPEG frames."""

    def __init__(self, machine_id: str, port: int = STREAM_PORT):
        self.machine_id = machine_id
        self.port = port
        self._server: Optional[asyncio.AbstractServer] = None
        self._monitors: dict[str, _SourceMonitor] = {}
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._capture = _capture_backend()
        self._lock = threading.Lock()
        # Optional callback: (monitor_id) -> PIL.Image | None. Used
        # by the virtual-source path to pull live frames from a
        # backend (evdi / IDD) instead of rendering the placeholder.
        # Shell wires this up when the VirtualDisplayManager spawns
        # a live phantom; when it's None or returns None, we fall
        # back to the static placeholder card.
        self.virtual_frame_provider: Optional[
            Callable[[str], object]] = None

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

    def set_virtuals(self, virtuals: list[dict],
                     owner_label: str = "") -> None:
        """Register the virtual displays this PC owns. Without an
        IDD / evdi backend the virtuals have no real framebuffer, so
        the server serves a placeholder card to any subscriber. The
        sink still renders the card as a valid JPEG stream — no
        timeout, no EOF — so the Layout canvas shows a clear
        explanatory image instead of the 'source disconnected' trap
        we saw otherwise.

        `virtuals` is a list of `{monitor_id, width, height}` dicts
        matching the shell's per-workspace virtual-display entries.
        `owner_label` is the human-readable name of the owning PC
        (usually machine_id or hostname) — rendered on the
        placeholder so the sink side knows where the virtual belongs.
        """
        with self._lock:
            wanted_ids = set()
            for v in virtuals:
                mid = str(v.get("monitor_id") or "")
                if not mid:
                    continue
                wanted_ids.add(mid)
                w = int(v.get("width") or 1920)
                h = int(v.get("height") or 1080)
                src = self._monitors.get(mid)
                if src is None:
                    self._monitors[mid] = _SourceMonitor(
                        monitor_id=mid,
                        x=0, y=0,
                        width=w, height=h,
                        is_virtual=True,
                        placeholder_text=self._placeholder_text(
                            mid, owner_label),
                    )
                else:
                    # Pre-existing entry. If it was a physical, leave
                    # it alone; the physical path takes priority when
                    # a monitor_id collides (unlikely — virtual ids
                    # use the V- prefix).
                    if not src.is_virtual:
                        continue
                    src.width = w
                    src.height = h
                    src.placeholder_text = self._placeholder_text(
                        mid, owner_label)
            # Drop virtuals that the workspace no longer contains.
            gone = [mid for mid, src in self._monitors.items()
                    if src.is_virtual and mid not in wanted_ids]
            for mid in gone:
                src = self._monitors.pop(mid)
                for writer in list(src.all_subscribers):
                    try:
                        writer.close()
                    except Exception:
                        pass
                if src.capture_task:
                    src.capture_task.cancel()

    @staticmethod
    def _placeholder_text(monitor_id: str, owner_label: str) -> str:
        if owner_label:
            return (f"Virtual display\n{owner_label} · {monitor_id}\n\n"
                    "No framebuffer backend.\n"
                    "Install IDD (Windows) or evdi (Linux)\n"
                    "to stream real pixels.")
        return (f"Virtual display\n{monitor_id}\n\n"
                "No framebuffer backend.")

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
                    src.is_virtual = False
                    src.placeholder_text = ""
            # Drop any source that unplugged — close subscribers so the
            # sink's placeholder-card logic kicks in. Keep virtuals
            # around regardless: they're managed via set_virtuals,
            # not set_monitors.
            gone = [mid for mid, src in self._monitors.items()
                    if mid not in present and not src.is_virtual]
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
            # Sink sends its preferred codec list — source picks the
            # first one it can serve. Old sinks (pre-Phase 5) omit
            # the key; we default them to JPEG-only so the wire stays
            # backward-compatible.
            sink_codecs = req.get("codecs") or [CODEC_JPEG]
            if isinstance(sink_codecs, str):
                sink_codecs = [sink_codecs]
        except (asyncio.IncompleteReadError, ValueError, UnicodeDecodeError):
            log.info("stream: malformed request from %s", peer)
            return

        with self._lock:
            src = self._monitors.get(monitor_id)
        # Virtuals don't need the _capture backend — they generate
        # placeholder frames directly. Physical sources do.
        if src is None or (not src.is_virtual and not self._capture):
            log.info("stream: no source %s (have=%s capture=%s)",
                     monitor_id, list(self._monitors), bool(self._capture))
            try:
                writer.close()
            except Exception:
                pass
            return

        codec = self._negotiate_codec(sink_codecs)
        fps = DEFAULT_FPS_H264 if codec == CODEC_H264 else DEFAULT_FPS
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

        if codec == CODEC_H264:
            src.h264_subscribers.add(writer)
            # Catch the new subscriber up with the last keyframe
            # chunk so their decoder doesn't block waiting for the
            # next scheduled IDR (~1 s away). Skipped when the cache
            # is empty (fresh source, no keyframes emitted yet).
            if src.h264_keyframe_cache:
                cache = src.h264_keyframe_cache
                try:
                    writer.write(
                        struct.pack(FRAME_HEADER, len(cache)) + cache
                    )
                except Exception:
                    pass
        else:
            src.jpeg_subscribers.add(writer)
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
            src.jpeg_subscribers.discard(writer)
            src.h264_subscribers.discard(writer)
            try:
                writer.close()
            except Exception:
                pass
            log.info("stream sink %s disconnected from %s",
                     peer, monitor_id)

    def _negotiate_codec(self, sink_codecs: list) -> str:
        """Pick the best codec both sides can handle. Sink preference
        wins when available; source falls back to JPEG otherwise (it's
        always supported). Phase 5 probes ffmpeg once and caches the
        result so we don't re-fork the binary per connection."""
        if CODEC_H264 in sink_codecs and self._h264_available():
            return CODEC_H264
        return CODEC_JPEG

    def _h264_available(self) -> bool:
        cached = getattr(self, "_h264_probe", None)
        if cached is not None:
            return cached
        try:
            from .hw_pipeline import ffmpeg_available
            ok = ffmpeg_available()
        except Exception:
            ok = False
        self._h264_probe = ok
        return ok

    async def _capture_loop(self, src: _SourceMonitor) -> None:
        """One capture loop per active source monitor. Grabs the
        screen, feeds whichever encoders have active subscribers, and
        fans out the resulting bytes. Dies as soon as every subscriber
        set empties so an unused source stops burning CPU.

        The encoder set is evaluated per tick — a new sink subscribing
        mid-stream lights up the matching path without restarting the
        capture loop. Phase 6 dirty-rect skip wraps the encode step
        so an idle desktop produces zero bytes on either path."""
        log.info("capture loop starting for %s (%dx%d at %d,%d, virtual=%s)",
                 src.monitor_id, src.width, src.height, src.x, src.y,
                 src.is_virtual)
        grab = self._capture
        # H.264 subscriber → the shared HWEncoder feeding them all.
        # Spun up lazily so a JPEG-only stream never forks ffmpeg.
        hw_encoder = None
        hw_reader_task: Optional[asyncio.Task] = None
        try:
            from .frame_delta import DirtyRectDetector
            dirty = DirtyRectDetector(src.width, src.height)
        except Exception:
            dirty = None
        # fps is the max of both paths so a mixed-codec source still
        # gives H.264 sinks their higher rate — JPEG sinks get their
        # frames on every grab anyway.
        fps = DEFAULT_FPS_H264 if self._h264_available() else DEFAULT_FPS
        period = 1.0 / fps
        next_tick = time.monotonic()
        cached_placeholder = None
        while src.has_subscribers():
            if src.is_virtual:
                # Virtual source: prefer a live frame from the
                # driver-level bridge (evdi / IDD). Fall back to the
                # static placeholder card when no backend provides
                # pixels — same bytes re-emitted each tick so the
                # dirty-rect detector keeps bandwidth at ~0.
                img = None
                if self.virtual_frame_provider is not None:
                    try:
                        img = self.virtual_frame_provider(src.monitor_id)
                    except Exception:
                        log.exception(
                            "virtual_frame_provider failed for %s",
                            src.monitor_id)
                if img is None:
                    if cached_placeholder is None:
                        cached_placeholder = self._render_placeholder(src)
                    img = cached_placeholder
            else:
                try:
                    img = await asyncio.get_running_loop().run_in_executor(
                        None, grab,
                        {"x": src.x, "y": src.y,
                         "width": src.width, "height": src.height},
                    )
                except Exception:
                    log.exception("capture failed for %s", src.monitor_id)
                    await asyncio.sleep(1.0)
                    continue
                if img is None:
                    await asyncio.sleep(0.2)
                    continue

            # Phase 6: skip the whole encode+fan-out step when the
            # frame hasn't changed. Keeps idle-desktop bandwidth at
            # ~0. `changed` is False only when the block hash is
            # identical across ticks AND we've seen at least one
            # frame go out since — catches the "just started, first
            # frame must ship" case.
            changed = True
            if dirty is not None:
                changed = dirty.update(img)

            if changed and src.jpeg_subscribers:
                self._fanout_jpeg(src, img)
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
                if hw_encoder is not None:
                    try:
                        hw_encoder.write_frame(img.tobytes())
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
        src.capture_task = None
        log.info("capture loop ending for %s", src.monitor_id)

    def _render_placeholder(self, src: _SourceMonitor):
        """Draw a solid-colour placeholder image for virtual sources
        that have no real framebuffer. Pillow is already a dep so no
        new imports cost here."""
        from PIL import Image, ImageDraw, ImageFont
        # Deep-purple lilac palette matching the rest of the UI.
        bg = (26, 27, 48)
        ink = (232, 233, 244)
        ink_faint = (170, 172, 200)
        img = Image.new("RGB", (src.width, src.height), bg)
        draw = ImageDraw.Draw(img)
        try:
            title_font = ImageFont.truetype(
                "DejaVuSans-Bold.ttf", max(18, src.height // 18))
            body_font = ImageFont.truetype(
                "DejaVuSans.ttf", max(12, src.height // 32))
        except Exception:
            title_font = ImageFont.load_default()
            body_font = ImageFont.load_default()
        lines = src.placeholder_text.splitlines()
        if not lines:
            lines = ["Virtual display"]
        # Measure + centre as a single block. Line 0 is big, rest are
        # body text; keeps the hierarchy visible at any resolution.
        line_heights = []
        line_widths = []
        for i, line in enumerate(lines):
            font = title_font if i == 0 else body_font
            try:
                bbox = draw.textbbox((0, 0), line, font=font)
                w = bbox[2] - bbox[0]
                h = bbox[3] - bbox[1]
            except Exception:
                w, h = font.getsize(line) if hasattr(font, "getsize") \
                    else (len(line) * 8, 16)
            line_widths.append(w)
            line_heights.append(h)
        spacing = max(6, src.height // 60)
        total_h = sum(line_heights) + spacing * (len(lines) - 1)
        cy = src.height // 2 - total_h // 2
        for i, line in enumerate(lines):
            font = title_font if i == 0 else body_font
            colour = ink if i == 0 else ink_faint
            cx = src.width // 2 - line_widths[i] // 2
            draw.text((cx, cy), line, fill=colour, font=font)
            cy += line_heights[i] + spacing
        return img

    def _fanout_jpeg(self, src: _SourceMonitor, img) -> None:
        try:
            buf = io.BytesIO()
            img.save(buf, format="JPEG", quality=JPEG_QUALITY)
            jpeg = buf.getvalue()
        except Exception:
            log.exception("JPEG encode failed")
            return
        frame = struct.pack(FRAME_HEADER, len(jpeg)) + jpeg
        dead = []
        for w in list(src.jpeg_subscribers):
            try:
                w.write(frame)
            except Exception:
                dead.append(w)
        for w in dead:
            src.jpeg_subscribers.discard(w)
            try:
                w.close()
            except Exception:
                pass

    def _build_hw_encoder(self, src: _SourceMonitor, fps: int):
        try:
            from .hw_pipeline import HWEncoder
        except Exception:
            return None
        enc = HWEncoder(width=src.width, height=src.height, fps=fps)
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
        while src.h264_subscribers:
            nal = await loop.run_in_executor(None, enc.read_nal)
            if not nal:
                await asyncio.sleep(0.005)
                continue
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
        self.preferred_codecs = preferred_codecs
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
        codec = CODEC_JPEG
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
            codec = str(resp.get("codec") or CODEC_JPEG)
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

        # Dispatch to the right recv loop for the negotiated codec.
        # Leftover bytes in `buf` (unlikely — handshake reads exact)
        # get handed over so we don't drop the first frame.
        try:
            if codec == CODEC_H264:
                self._recv_h264(sock, buf, width, height)
            else:
                self._recv_framed(sock, buf, codec)
        finally:
            try:
                sock.close()
            except Exception:
                pass
            log.info("stream sink thread for %s stopped",
                     self.monitor_id)

    def _recv_framed(self, sock, initial_buf: bytearray,
                     codec: str) -> None:
        """Length-prefixed frame reader — shared path for JPEG. Each
        frame is a single, complete encoded image."""
        buf = initial_buf
        while not self._stopping.is_set():
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as e:
                log.info("stream recv failed: %s", e)
                break
            if not chunk:
                break
            buf.extend(chunk)
            while len(buf) >= FRAME_HEADER_SIZE:
                (frame_len,) = struct.unpack(
                    FRAME_HEADER, bytes(buf[:FRAME_HEADER_SIZE]))
                if frame_len <= 0 or frame_len > 8 * 1024 * 1024:
                    log.info("stream: bogus frame len %d, dropping link",
                             frame_len)
                    buf.clear()
                    self._stopping.set()
                    break
                if len(buf) < FRAME_HEADER_SIZE + frame_len:
                    break
                data = bytes(
                    buf[FRAME_HEADER_SIZE:FRAME_HEADER_SIZE + frame_len])
                del buf[:FRAME_HEADER_SIZE + frame_len]
                try:
                    self.on_frame(data, codec)
                except Exception:
                    log.exception("on_frame callback failed")

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

        def _puller():
            while not stop_reading.is_set():
                frame = dec.read_frame()
                if frame is None:
                    break
                try:
                    self.on_frame(frame, CODEC_H264)
                except Exception:
                    log.exception("h264 on_frame callback failed")

        puller = threading.Thread(target=_puller, daemon=True,
                                  name=f"h264-pull:{self.monitor_id}")
        puller.start()

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
                        dec.write_nal(nal)
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
                    if not dec.write_nal(nal):
                        break
        finally:
            stop_reading.set()
            dec.stop()
            puller.join(timeout=1.0)

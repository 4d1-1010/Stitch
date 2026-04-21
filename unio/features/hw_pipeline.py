"""Phase 5 hardware path — ffmpeg-backed H.264 encode + decode.

Phase 1 ships JPEG-over-TCP which is a proof-of-pipeline only: ~100 ms
end-to-end because JPEG re-encodes the whole frame every tick and
can't do motion compensation. Phase 5 swaps in H.264 with hardware
acceleration wherever the host offers one. Target is sub-20 ms on
LAN for modern NVIDIA / Intel / AMD GPUs.

This module keeps the same fan-out shape as `display_stream.py`
(capture a frame, encode it, hand it to the transport) but routes
raw RGB through an ffmpeg subprocess running one of:

  * **NVENC**        — NVIDIA GPUs, `h264_nvenc`
  * **QuickSync**    — Intel iGPUs + dGPUs, `h264_qsv`
  * **AMF**          — AMD GPUs, `h264_amf` (Windows)
  * **VA-API**       — Linux Intel / AMD / newer NVIDIA, `h264_vaapi`
  * **VideoToolbox** — macOS only, `h264_videotoolbox`
  * **libopenh264**  — CPU fallback when no HW encoder lands (BSD)

All presets are tuned for low latency (no B-frames, `tune=zerolatency`
or its codec-specific equivalent, minimal lookahead). Throughput is
intentionally capped per-frame because a burst would queue up inside
ffmpeg and raise the observed latency.

Zero-copy GPU-to-GPU (DXGI texture → NVENC D3D11 texture) is a
Phase 5 bullet the plan lists but is NOT in this build — that needs
ctypes bindings to D3D11 and nvEncCreateInputBuffer which is
significantly more code. The ffmpeg-subprocess path gets us the
codec win (~30 ms → ~8 ms encode) without the native-binding tax.
"""

from __future__ import annotations

import logging
import os
import shutil
import subprocess
import sys
from typing import Optional

log = logging.getLogger(__name__)


# Flag bundle to pass to subprocess.Popen so ffmpeg (a CLI program
# that normally allocates a console) doesn't pop a cmd.exe window on
# Windows every time we spawn it. Zero on non-Windows so the dict
# unpack stays inert.
_POPEN_NO_WINDOW_FLAGS: dict = (
    {"creationflags": getattr(
        subprocess, "CREATE_NO_WINDOW", 0x08000000)}
    if sys.platform == "win32" else {}
)


# Priority order per OS. First entry that ffmpeg accepts wins. This is
# a heuristic — some hosts will list a codec the kernel can't actually
# hand a device for (e.g. headless VMs with `libva` but no `/dev/dri`).
# `probe_hw_encoder` verifies by encoding one dummy frame.
#
# The CPU fallback is ``libopenh264`` (Cisco's BSD-licensed OpenH264).
# Cisco covers the H.264 patent royalties for end users up to their
# published cap, so an LGPL ffmpeg without ``libx264`` still has a
# redistributable CPU path. ``libx264`` is deliberately absent — it
# is GPL-only and pulling it in would taint the whole binary.
_ENCODER_PRIORITY = {
    "linux":   ("h264_nvenc", "h264_vaapi", "h264_qsv",
                "libopenh264"),
    "win32":   ("h264_nvenc", "h264_qsv", "h264_amf",
                "libopenh264"),
    "darwin":  ("h264_videotoolbox", "libopenh264"),
}


def _bundled_ffmpeg_path() -> Optional[str]:
    """Return the path to the ffmpeg binary we ship with the app, or
    None if this build doesn't include one / we're running from
    source without the vendor drop.

    Checks, in order:
      * PyInstaller runtime extraction dir (``sys._MEIPASS``) with
        a ``ffmpeg/`` subdir — where the .spec places the bundled
        binary at build time.
      * ``vendor/ffmpeg/<os>-<arch>/ffmpeg[.exe]`` relative to the
        repo root, populated by ``packaging/fetch-ffmpeg.sh`` in
        dev checkouts.
    """
    exe = "ffmpeg.exe" if sys.platform == "win32" else "ffmpeg"
    meipass = getattr(sys, "_MEIPASS", None)
    if meipass:
        candidate = os.path.join(meipass, "ffmpeg", exe)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))
    os_arch = (
        "windows-x86_64" if sys.platform == "win32"
        else "linux-x86_64"
    )
    candidate = os.path.join(
        repo, "vendor", "ffmpeg", os_arch, exe)
    if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
        return candidate
    return None


def ffmpeg_path() -> Optional[str]:
    """Return the ffmpeg binary to use. Prefers the bundled LGPL
    build shipped with the app; falls back to whatever is on PATH
    for source-tree runs where the vendor drop is absent."""
    bundled = _bundled_ffmpeg_path()
    if bundled is not None:
        return bundled
    return shutil.which("ffmpeg")


def ffmpeg_available() -> bool:
    return ffmpeg_path() is not None


def list_known_encoders() -> list[str]:
    """What ffmpeg actually compiled with. Runs `ffmpeg -encoders`
    once, parses the table. Any parsing failure returns an empty
    list; pick_hw_encoder then picks the BSD libopenh264 fallback
    (never libx264 — GPL, not redistributable with our bundle)."""
    bin_path = ffmpeg_path()
    if not bin_path:
        return []
    try:
        out = subprocess.run(
            [bin_path, "-hide_banner", "-encoders"],
            capture_output=True, text=True, timeout=3.0,
            **_POPEN_NO_WINDOW_FLAGS,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    encoders: list[str] = []
    for line in out.stdout.splitlines():
        parts = line.strip().split(None, 2)
        if len(parts) < 2:
            continue
        # ffmpeg's table prefixes each encoder with "V." / "A." / "S."
        # (media type) followed by capability flags, then the name.
        if parts[0].startswith("V") and parts[1].startswith("h264"):
            encoders.append(parts[1])
    return encoders


def pick_hw_encoder() -> str:
    """Walk the per-OS priority list and return the first encoder
    ffmpeg says it has. Returns 'libopenh264' (Cisco BSD, covers H.264
    patent royalties) as the guaranteed fallback — libx264 is GPL
    and would contaminate the LGPL bundle, so it's never picked."""
    import sys
    available = set(list_known_encoders())
    if not available:
        return "libopenh264" if ffmpeg_available() else ""
    for candidate in _ENCODER_PRIORITY.get(
            "win32" if sys.platform == "win32" else
            "darwin" if sys.platform == "darwin" else "linux", ()):
        if candidate in available:
            return candidate
    return "libopenh264"


def encoder_args(encoder: str, width: int, height: int,
                 fps: int, quality: int = 20,
                 pix_fmt: str = "rgb24") -> list[str]:
    """Build the ffmpeg command-line bits for a given encoder + the
    low-latency knobs each one wants. Kept as a dict-lookup because
    NVENC / QSV / VA-API disagree on every single flag name.

    ``quality`` is a CQP-style index (0 = lossless, ~51 = garbage).
    20 is visually indistinguishable from the desktop source on
    typical UI content at 1080p; pushing lower than ~17 trades
    bitrate for diminishing returns.

    ``pix_fmt`` is the raw input format. "rgb24" is the 3-byte
    per-pixel default; "bgra" lets the capture backend hand ffmpeg
    its native 4-byte BGRA composite buffer with no conversion pass
    on the Python side — and ffmpeg's BGRA→YUV SIMD is faster than
    the RGB→YUV path too, so we win twice.
    """
    common_input = [
        "-f", "rawvideo",
        "-pix_fmt", pix_fmt,
        "-s", f"{width}x{height}",
        "-r", str(fps),
        "-i", "pipe:0",
    ]
    # Every encoder: no B-frames, short keyframe interval so late-
    # joining sinks don't stare at a black overlay while waiting
    # for a decodable I-frame. ~0.25 s at 60 fps is a reasonable
    # balance between bandwidth (keyframes are ~3–5× bigger than
    # P-frames) and subscription responsiveness.
    gop = max(4, fps // 4)
    common_out = [
        "-an",                        # no audio
        "-f", "h264",                 # raw Annex-B on the wire
        "-g", str(gop),
        "pipe:1",
    ]

    if encoder == "h264_nvenc":
        # Constant-quality mode at CQP ~20 — NVENC's cheapest preset
        # ("p1") with the low-latency tune is <5 ms for 1080p60 on
        # anything Turing or newer, and zerolatency=1 disables the
        # 1-frame lookahead delay the encoder would otherwise add.
        # ``forced-idr 1`` guarantees each keyframe is a real IDR,
        # so the keyframe cache we mirror to new subscribers is a
        # valid decoder-reset point.
        return common_input + [
            "-c:v", "h264_nvenc",
            "-preset", "p1",
            "-tune", "ll",
            "-rc", "constqp",
            "-qp", str(quality),
            "-bf", "0",
            "-zerolatency", "1",
            "-delay", "0",
            "-forced-idr", "1",
            "-pix_fmt", "yuv420p",
        ] + common_out
    if encoder == "h264_qsv":
        return common_input + [
            "-c:v", "h264_qsv",
            "-preset", "veryfast",
            "-look_ahead", "0",
            "-bf", "0",
            "-global_quality", str(quality),
            "-pix_fmt", "nv12",
        ] + common_out
    if encoder == "h264_amf":
        return common_input + [
            "-c:v", "h264_amf",
            "-quality", "speed",
            "-usage", "lowlatency",
            "-rc", "cqp",
            "-qp_i", str(quality),
            "-qp_p", str(quality),
            "-bf", "0",
            "-pix_fmt", "yuv420p",
        ] + common_out
    if encoder == "h264_vaapi":
        # VA-API on Linux needs an explicit device + hwupload. The
        # ``-hwaccel vaapi -vaapi_device /dev/dri/renderD128`` args
        # belong BEFORE -i, which means we can't just append them —
        # the caller builds them from ``vaapi_prefix()`` instead.
        return vaapi_prefix() + common_input + [
            "-vf", "format=nv12,hwupload",
            "-c:v", "h264_vaapi",
            "-bf", "0",
            "-rc_mode", "CQP",
            "-qp", str(quality),
        ] + common_out
    if encoder == "h264_videotoolbox":
        return common_input + [
            "-c:v", "h264_videotoolbox",
            "-realtime", "1",
            "-allow_sw", "0",
            "-bf", "0",
            "-q:v", str(quality),
            "-pix_fmt", "yuv420p",
        ] + common_out
    # libopenh264 — BSD-licensed Cisco CPU encoder. Cisco covers the
    # H.264 patent royalties for end users up to their published cap,
    # so this stays redistributable under LGPL. Slower than any HW
    # encoder but the only CPU option that keeps the bundle GPL-free.
    return common_input + [
        "-c:v", "libopenh264",
        "-profile:v", "constrained_baseline",
        "-allow_skip_frames", "1",
        "-slices", "0",
        "-bf", "0",
        "-pix_fmt", "yuv420p",
    ] + common_out


def vaapi_prefix() -> list[str]:
    """Arguments that must precede `-i` for h264_vaapi. Isolated so
    the encoder_args composition stays readable."""
    return ["-hwaccel", "vaapi",
            "-vaapi_device", "/dev/dri/renderD128"]


class HWEncoder:
    """Spawns an ffmpeg subprocess configured for low-latency H.264.
    Feed frames in via `write_frame(rgb_bytes)`; read encoded NAL
    units out via `read_nal()`. Caller owns the framing layer (TCP
    length-prefix) — ffmpeg's own `-f h264` is Annex-B without any
    application-level header."""

    def __init__(self, width: int, height: int, fps: int,
                 encoder: Optional[str] = None,
                 quality: int = 20,
                 pix_fmt: str = "rgb24"):
        self.width = width
        self.height = height
        self.fps = fps
        self.encoder = encoder or pick_hw_encoder()
        self.quality = quality
        self.pix_fmt = pix_fmt
        # Frame payload size the caller MUST send per write_frame.
        # 3 bytes/pixel for RGB, 4 bytes/pixel for the BGRA fast path.
        self.frame_bytes = width * height * (
            4 if pix_fmt in ("bgra", "bgr0", "rgba", "rgb0") else 3)
        self._proc: Optional[subprocess.Popen] = None

    def start(self) -> bool:
        bin_path = ffmpeg_path()
        if not bin_path:
            log.info("ffmpeg not available — HW encoder disabled")
            return False
        args = [bin_path, "-hide_banner", "-loglevel", "error"] + \
            encoder_args(self.encoder, self.width, self.height,
                         self.fps, self.quality, self.pix_fmt)
        try:
            self._proc = subprocess.Popen(
                args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                # stderr to DEVNULL so ffmpeg's status spam can't
                # deadlock the pipe once the OS stderr buffer fills.
                stderr=subprocess.DEVNULL,
                bufsize=0,
                **_POPEN_NO_WINDOW_FLAGS,
            )
        except OSError as e:
            log.warning("HW encoder spawn failed (%s): %s",
                        self.encoder, e)
            return False
        # Non-blocking stdout so read_nal can return b"" when nothing
        # is ready yet. Without this the capture loop deadlocks on
        # the first tick because the encoder needs a handful of frames
        # before emitting the first IDR, and read() waits for ever.
        _set_nonblocking(self._proc.stdout)
        log.info("HW encoder started: %s @ %dx%d %d fps pix_fmt=%s",
                 self.encoder, self.width, self.height,
                 self.fps, self.pix_fmt)
        return True

    def write_frame(self, rgb: bytes) -> bool:
        if self._proc is None or self._proc.stdin is None:
            return False
        try:
            self._proc.stdin.write(rgb)
            return True
        except (BrokenPipeError, OSError):
            return False

    def read_nal(self, max_bytes: int = 1 << 16) -> bytes:
        """Read up to max_bytes from the encoder's stdout. Non-blocking-
        ish: if nothing's available we return b"" instead of hanging
        the caller, so the capture loop stays responsive even when
        ffmpeg is mid-encode and hasn't emitted a NAL yet."""
        if self._proc is None or self._proc.stdout is None:
            return b""
        try:
            import os as _os
            fd = self._proc.stdout.fileno()
            return _os.read(fd, max_bytes)
        except BlockingIOError:
            return b""
        except OSError:
            return b""

    def stop(self) -> None:
        if self._proc is None:
            return
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
        except Exception:
            pass
        try:
            self._proc.terminate()
            self._proc.wait(timeout=1.0)
        except Exception:
            try:
                self._proc.kill()
            except Exception:
                pass
        self._proc = None


class HWDecoder:
    """Mirror of HWEncoder for the sink. Feeds H.264 NAL units in,
    reads raw RGB frames out. Uses `-hwaccel auto` so ffmpeg picks the
    best decoder on the host — NVDEC, DXVA2, VAAPI, VideoToolbox — or
    falls back to software h264_dec."""

    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self._proc: Optional[subprocess.Popen] = None

    def start(self) -> bool:
        bin_path = ffmpeg_path()
        if not bin_path:
            return False
        args = [
            bin_path, "-hide_banner", "-loglevel", "error",
            "-fflags", "nobuffer", "-flags", "low_delay",
            "-hwaccel", "auto",
            "-f", "h264",
            "-i", "pipe:0",
            "-an",
            "-f", "rawvideo",
            "-pix_fmt", "rgb24",
            "-s", f"{self.width}x{self.height}",
            "pipe:1",
        ]
        try:
            self._proc = subprocess.Popen(
                args,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                # stderr to DEVNULL so ffmpeg's status spam can't
                # deadlock the pipe once the OS stderr buffer fills.
                stderr=subprocess.DEVNULL,
                bufsize=0,
                **_POPEN_NO_WINDOW_FLAGS,
            )
        except OSError as e:
            log.warning("HW decoder spawn failed: %s", e)
            return False
        log.info("HW decoder started @ %dx%d", self.width, self.height)
        return True

    def write_nal(self, nal: bytes) -> bool:
        if self._proc is None or self._proc.stdin is None:
            return False
        try:
            self._proc.stdin.write(nal)
            return True
        except (BrokenPipeError, OSError):
            return False

    def read_frame(self) -> Optional[bytes]:
        """Block until one complete raw-RGB frame is available.
        Returns exactly width*height*3 bytes, or None on EOF."""
        if self._proc is None or self._proc.stdout is None:
            return None
        size = self.width * self.height * 3
        buf = bytearray()
        while len(buf) < size:
            try:
                chunk = self._proc.stdout.read(size - len(buf))
            except OSError:
                return None
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)

    def stop(self) -> None:
        if self._proc is None:
            return
        try:
            if self._proc.stdin:
                self._proc.stdin.close()
        except Exception:
            pass
        try:
            self._proc.terminate()
            self._proc.wait(timeout=1.0)
        except Exception:
            try:
                self._proc.kill()
            except Exception:
                pass
        self._proc = None


# ── Quick self-test ─────────────────────────────────────────────────


def _set_nonblocking(file_obj) -> None:
    """Mark a pipe's FD O_NONBLOCK so reads return immediately when
    there's no data. Only works on POSIX; Windows pipes default to
    blocking and the encoder reader runs in an executor anyway."""
    import sys as _sys
    if _sys.platform == "win32":
        return
    try:
        import fcntl as _fcntl
        import os as _os
        fd = file_obj.fileno()
        flags = _fcntl.fcntl(fd, _fcntl.F_GETFL)
        _fcntl.fcntl(fd, _fcntl.F_SETFL, flags | _os.O_NONBLOCK)
    except (OSError, AttributeError):
        pass


def describe_hw_support() -> str:
    """Human-readable summary of what this host will end up using.
    Surfaced in the Settings tab once it's wired there, and useful
    from the CLI when debugging latency reports."""
    if not ffmpeg_available():
        return "ffmpeg not installed — display streaming falls back to Phase 1 JPEG"
    encoders = list_known_encoders()
    picked = pick_hw_encoder()
    return (f"ffmpeg: yes, HW encoder: {picked}, "
            f"also available: {', '.join(encoders) or 'none'}")

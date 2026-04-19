"""Phase 6 dirty-rect detection.

The cheapest way to drop bandwidth on an idle desktop is to not
encode the frame at all. This module does exactly that: it block-
hashes each captured frame and reports whether it differs
meaningfully from the previous one. The source's capture loop uses
the result to skip the encode + fan-out entirely when the desktop
sat still.

Three detection strategies, in order of preference:

  1. **OS-level dirty hints** — DXGI `GetFrameDirtyRects` on Windows,
     XDamage on Linux X11, PipeWire dirty regions on Wayland. Free,
     authoritative, catches sub-pixel-precise changes. These are
     wired in Phase 6's follow-up work; v1 ships with the
     application-level fallback below.

  2. **Block-hash diff (this module)** — split the frame into 32×32
     blocks, xxhash each one, compare against the previous pass.
     Any changed block counts as a dirty region. Cost: ~1–3 ms on
     a 4K frame on a modern CPU, negligible next to encode time.

  3. **Full-hash stale catch** — every 100 ms, hash the entire frame
     even if blocks report no change. If the full hash differs but
     no block did, the block grid lied (happens on DWM-bypass games
     that present to the GPU directly). Bail to full re-capture.
     Sunshine uses the same trick; we copy it here.

The module exposes a single class `DirtyRectDetector.update(img) ->
bool`: True when the caller should encode + send, False when the
frame can be skipped. A companion `rects` property holds the list of
dirty rectangles in image coords — Phase 5's crop-encode path can
feed just those rects to the encoder instead of the whole frame.
"""

from __future__ import annotations

import hashlib
import logging
import time
from typing import Optional

log = logging.getLogger(__name__)


# Block size for the hash grid. 32×32 hits a sweet spot:
#   * small enough to catch a single typed character's cursor blink
#   * big enough to keep the hash count manageable on a 4K frame
#     (8100 hashes at 3840×2160 — ~2 ms with hashlib.md5)
# Raising this trades sensitivity for CPU; lowering trades CPU for
# false-positive blocks (anti-aliasing noise looks like a change).
BLOCK_SIZE = 32

# Full-frame hash interval — how often we double-check the block
# grid against a full-frame hash. 100 ms matches Sunshine's value
# and keeps the worst-case latency-to-unstick bounded.
STALE_HASH_INTERVAL = 0.100

# Minimum fraction of dirty blocks to count as "the frame changed".
# Anti-aliasing jitter on scrollbar highlights can trigger a single
# block per tick even when nothing visually moved; 0.1% weeds those
# out without hiding real small movements (e.g. a cursor blink).
DIRTY_THRESHOLD = 0.001


class DirtyRectDetector:
    """Tracks block-hashes across frames and reports whether the
    next frame needs to ship.

    Call `update(PIL.Image)` from the capture loop. Returns True if
    the caller should encode + fan out this frame, False if it's
    safe to skip. The first call always returns True (we have no
    previous frame to diff against, so every sink needs at least
    one keyframe to start from).
    """

    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self._prev_blocks: Optional[list[bytes]] = None
        self._prev_full_hash: Optional[bytes] = None
        self._last_full_check = 0.0
        self._dirty_rects: list[tuple[int, int, int, int]] = []
        self._frames_skipped_in_a_row = 0

    @property
    def dirty_rects(self) -> list[tuple[int, int, int, int]]:
        """List of (x, y, w, h) rectangles (image coords) that
        changed on the most recent `update` call. Phase 5+ crop-encode
        can send just these regions instead of the whole frame."""
        return list(self._dirty_rects)

    def update(self, img) -> bool:
        if img is None:
            return False
        try:
            width, height = img.size
        except AttributeError:
            return True
        if width != self.width or height != self.height:
            # Geometry changed mid-stream — treat as a full reset so
            # the next frame is unconditionally a keyframe.
            self.width = width
            self.height = height
            self._prev_blocks = None
            self._prev_full_hash = None
            self._last_full_check = 0.0
            return True

        raw = img.tobytes()
        stride = width * len(img.getbands())
        blocks = _block_hashes(raw, width, height, stride)

        if self._prev_blocks is None:
            # First frame; ship it so sinks have a keyframe to build on.
            self._prev_blocks = blocks
            self._prev_full_hash = _full_hash(raw)
            self._last_full_check = time.monotonic()
            self._dirty_rects = [(0, 0, width, height)]
            self._frames_skipped_in_a_row = 0
            return True

        # Compare block-by-block. Any mismatch = dirty.
        changed_indices: list[int] = []
        prev = self._prev_blocks
        for idx, (a, b) in enumerate(zip(prev, blocks)):
            if a != b:
                changed_indices.append(idx)
        self._prev_blocks = blocks

        # Stale-hash fallback: the block grid is a heuristic; every
        # STALE_HASH_INTERVAL we also full-hash the frame. If the
        # full hash changed but no block flagged dirty, a DWM-bypass
        # game (or a transparency effect that draws into the
        # composite surface without hitting our grid cleanly) is
        # lying. Force-ship.
        now = time.monotonic()
        force_ship = False
        if now - self._last_full_check >= STALE_HASH_INTERVAL:
            full = _full_hash(raw)
            if (not changed_indices
                    and self._prev_full_hash is not None
                    and full != self._prev_full_hash):
                force_ship = True
                log.debug("block grid clean but full hash changed — "
                          "forcing re-encode")
            self._prev_full_hash = full
            self._last_full_check = now

        if not changed_indices and not force_ship:
            self._dirty_rects = []
            self._frames_skipped_in_a_row += 1
            return False

        # Translate changed block indices back into rectangles. For
        # Phase 6 v1 we just report each changed block as its own
        # rect; a later pass can coalesce adjacent blocks into larger
        # regions to shrink the rect list on scroll-heavy content.
        self._dirty_rects = _blocks_to_rects(
            changed_indices or _all_blocks(width, height),
            width, height,
        )
        self._frames_skipped_in_a_row = 0

        # Reject the frame as "unchanged" when the dirty ratio is
        # below threshold — saves encode time on purely noisy AA
        # flutter. Full-hash force-ship bypasses this so a real-but-
        # tiny change still gets through.
        if (not force_ship
                and changed_indices
                and len(changed_indices) / max(1, len(blocks))
                < DIRTY_THRESHOLD):
            self._dirty_rects = []
            self._frames_skipped_in_a_row += 1
            return False
        return True

    @property
    def skipped_in_a_row(self) -> int:
        return self._frames_skipped_in_a_row


# ── Internals ───────────────────────────────────────────────────────


def _block_hashes(raw: bytes, width: int, height: int,
                  stride: int) -> list[bytes]:
    """Compute one MD5-digest per BLOCK_SIZE×BLOCK_SIZE tile. MD5 is
    fast, non-cryptographic collisions are fine here — we only need
    "did these bytes change?" not "prove they didn't". Pure Python is
    OK at 4K (~3 ms); swap in xxhash for finer grain."""
    cols = (width + BLOCK_SIZE - 1) // BLOCK_SIZE
    rows = (height + BLOCK_SIZE - 1) // BLOCK_SIZE
    block_stride = BLOCK_SIZE * len(raw) // (width * height)
    hashes: list[bytes] = []
    # For each tile row, slice each block's BLOCK_SIZE scanlines out
    # of raw via per-row offsets. Pillow's tobytes() is row-major
    # RGB-packed, so `raw[row * stride + col * bpp:...]` gives one
    # scanline slice per block row.
    bpp = stride // width
    for by in range(rows):
        y0 = by * BLOCK_SIZE
        y1 = min(y0 + BLOCK_SIZE, height)
        for bx in range(cols):
            x0 = bx * BLOCK_SIZE
            x1 = min(x0 + BLOCK_SIZE, width)
            h = hashlib.md5()
            for y in range(y0, y1):
                row_off = y * stride
                h.update(raw[row_off + x0 * bpp:
                             row_off + x1 * bpp])
            hashes.append(h.digest())
    return hashes


def _full_hash(raw: bytes) -> bytes:
    return hashlib.md5(raw).digest()


def _blocks_to_rects(indices, width: int,
                     height: int) -> list[tuple[int, int, int, int]]:
    cols = (width + BLOCK_SIZE - 1) // BLOCK_SIZE
    out = []
    for idx in indices:
        bx = idx % cols
        by = idx // cols
        x0 = bx * BLOCK_SIZE
        y0 = by * BLOCK_SIZE
        w = min(BLOCK_SIZE, width - x0)
        h = min(BLOCK_SIZE, height - y0)
        out.append((x0, y0, w, h))
    return out


def _all_blocks(width: int, height: int) -> range:
    cols = (width + BLOCK_SIZE - 1) // BLOCK_SIZE
    rows = (height + BLOCK_SIZE - 1) // BLOCK_SIZE
    return range(cols * rows)

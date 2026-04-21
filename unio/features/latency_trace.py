"""Per-stage latency tracer for the display-stream hot path.

Every frame touches a fixed sequence of stages on each side:

  Source:  CAPTURE_GRAB -> ENC_SUBMIT -> ENC_OUT -> SEND
  Sink:    RECV         -> DEC_IN     -> DEC_OUT -> PRESENT

The tracer records ``time.monotonic_ns()`` at each stage for each
frame, bounded to the most recent N frames (ring). From that we can
compute per-transition percentiles (p50/p95/p99) and dump a raw CSV
for offline analysis, so every latency claim downstream becomes
falsifiable instead of inferred.

Cross-machine end-to-end isn't computed here — the two sides have
independent monotonic clocks with unknown skew. That's solved
separately by either (a) a ``--loopback`` mode that runs source and
sink in the same process, sharing one clock, or (b) stamping
capture-time into the on-wire header so the sink can subtract.
Both are follow-ups; this module is the substrate they both depend
on.
"""

from __future__ import annotations

import csv
import logging
import os
import threading
import time
from collections import defaultdict, deque
from enum import IntEnum
from typing import Optional

log = logging.getLogger(__name__)


class Stage(IntEnum):
    """Fixed sequence of stages every frame traverses. Integer
    values are stable on disk so an older CSV can be re-analysed by
    a newer build."""
    CAPTURE_GRAB = 0   # source: grab() returned
    ENC_SUBMIT = 1     # source: encoder.write_frame() returned
    ENC_OUT = 2        # source: encoder.read_nal() returned
    SEND = 3           # source: writer.write(nal) returned
    RECV = 4           # sink:   NAL bytes fully read from socket
    DEC_IN = 5         # sink:   decoder.write_nal() returned
    DEC_OUT = 6        # sink:   decoder.read_frame() returned
    PRESENT = 7        # sink:   frame handed to compositor


STAGE_NAMES = {int(s): s.name for s in Stage}

# Transitions we surface in the rolling summary. Order matters for
# the log output. Each entry is (from_stage, to_stage, label).
_TRANSITIONS = (
    (Stage.CAPTURE_GRAB, Stage.ENC_SUBMIT, "grab->enc_in"),
    (Stage.ENC_SUBMIT, Stage.ENC_OUT, "encode"),
    (Stage.ENC_OUT, Stage.SEND, "enc_out->send"),
    (Stage.RECV, Stage.DEC_IN, "recv->dec_in"),
    (Stage.DEC_IN, Stage.DEC_OUT, "decode"),
    (Stage.DEC_OUT, Stage.PRESENT, "dec_out->present"),
)


def _percentiles(samples: list, pcts=(50, 95, 99)) -> dict:
    """Cheap percentile estimator. Sorts a copy — fine at 10k
    samples and ~1 Hz summary cadence, don't optimise prematurely."""
    if not samples:
        return {p: 0.0 for p in pcts}
    ordered = sorted(samples)
    n = len(ordered)
    out = {}
    for p in pcts:
        idx = min(n - 1, max(0, int(p / 100.0 * n) - (1 if p == 100 else 0)))
        out[p] = ordered[idx]
    return out


class LatencyTracer:
    """One tracer per active stream (source or sink). Lock-protected
    so capture / encoder-reader / socket threads can all stamp into
    the same instance."""

    def __init__(self, capacity: int = 10_000, tag: str = ""):
        self.capacity = capacity
        self.tag = tag
        # Per-frame stamps. dict[frame_id] -> list[int] of length 8,
        # indexed by Stage. Zero means "not yet stamped". Bounded by
        # an insertion-order deque of frame_ids so old frames fall
        # out in O(1).
        self._lock = threading.Lock()
        self._frames: dict[int, list] = {}
        self._order: deque = deque()
        # Rolling window of completed stage-to-stage deltas in ms.
        # Size-bounded so summary() stays cheap.
        self._deltas: dict[str, deque] = defaultdict(
            lambda: deque(maxlen=capacity))
        self._last_summary_at = 0.0

    def stamp(self, frame_id: int, stage: Stage) -> None:
        """Record ``monotonic_ns`` for (frame_id, stage). Called from
        wherever in the pipeline the stage completes. Safe to call
        from any thread."""
        ns = time.monotonic_ns()
        with self._lock:
            entry = self._frames.get(frame_id)
            if entry is None:
                entry = [0] * len(Stage)
                self._frames[frame_id] = entry
                self._order.append(frame_id)
                if len(self._order) > self.capacity:
                    evicted = self._order.popleft()
                    self._frames.pop(evicted, None)
            entry[int(stage)] = ns
            # If this stamp closes a known transition, record the
            # delta into the rolling window. Cheap — at most 6
            # lookups per stamp.
            for a, b, label in _TRANSITIONS:
                if stage == b and entry[int(a)] != 0:
                    delta_ms = (ns - entry[int(a)]) / 1e6
                    self._deltas[label].append(delta_ms)
                    break

    def summary(self) -> dict:
        """Per-transition p50/p95/p99 over the rolling window. Keys
        are the labels in ``_TRANSITIONS``."""
        with self._lock:
            snapshot = {k: list(v) for k, v in self._deltas.items()}
        return {k: _percentiles(v) for k, v in snapshot.items()}

    def maybe_log(self, period: float = 5.0) -> None:
        """Called from hot paths; throttles to one summary per
        ``period`` seconds and logs p50/p95/p99 for each transition
        we've seen. No-op if no samples are in the window yet."""
        now = time.monotonic()
        if now - self._last_summary_at < period:
            return
        self._last_summary_at = now
        s = self.summary()
        parts = []
        for _, _, label in _TRANSITIONS:
            pct = s.get(label)
            if not pct:
                continue
            parts.append(
                f"{label} p50={pct[50]:.1f} p95={pct[95]:.1f} "
                f"p99={pct[99]:.1f}")
        if parts:
            log.info("latency %s ms: %s", self.tag, " | ".join(parts))

    def dump_csv(self, path: str) -> None:
        """Write every recorded (frame_id, stage, monotonic_ns) row
        to ``path``. Call from shutdown; the CSV is the source of
        truth for offline percentile or per-frame analysis. Creates
        parent dirs as needed."""
        with self._lock:
            rows = []
            for fid in self._order:
                entry = self._frames.get(fid)
                if entry is None:
                    continue
                for stage_idx, ns in enumerate(entry):
                    if ns == 0:
                        continue
                    rows.append((fid, stage_idx,
                                 STAGE_NAMES[stage_idx], ns))
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", newline="") as fh:
                w = csv.writer(fh)
                w.writerow(["frame_id", "stage_idx",
                            "stage_name", "monotonic_ns"])
                w.writerows(rows)
            log.info("latency trace %s written: %d rows, %d frames",
                     path, len(rows), len(self._order))
        except Exception:
            log.exception("failed to write latency trace %s", path)


# ── Per-process registry ─────────────────────────────────────────


_TRACERS: dict[str, LatencyTracer] = {}
_TRACERS_LOCK = threading.Lock()


def get_tracer(tag: str, capacity: int = 10_000) -> LatencyTracer:
    """Get or create the tracer for a stream identified by ``tag``.
    Tags are free-form; typical values are ``src:<monitor_id>`` for
    outbound streams and ``sink:<monitor_id>`` for inbound. The
    registry lets shutdown hooks dump every tracer without every
    subsystem having to plumb its tracer instance up to peer.stop."""
    with _TRACERS_LOCK:
        inst = _TRACERS.get(tag)
        if inst is None:
            inst = LatencyTracer(capacity=capacity, tag=tag)
            _TRACERS[tag] = inst
        return inst


def dump_all_traces(dir_path: str) -> None:
    """Dump every registered tracer to ``dir_path`` as
    ``latency-<tag>-<unix_ts>.csv``. Safe to call from peer.stop
    even if no streams ever ran."""
    with _TRACERS_LOCK:
        tracers = dict(_TRACERS)
    if not tracers:
        return
    ts = int(time.time())
    for tag, tracer in tracers.items():
        safe_tag = tag.replace("/", "_").replace(":", "_")
        path = os.path.join(
            dir_path, f"latency-{safe_tag}-{ts}.csv")
        tracer.dump_csv(path)

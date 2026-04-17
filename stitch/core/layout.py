"""
Global layout manager.

Maintains the unified coordinate space across all machines/monitors.
Handles edge detection and cursor transitions between monitors.
"""

import logging
from dataclasses import dataclass, field
from typing import Optional

log = logging.getLogger(__name__)

EDGE_THRESHOLD = 1  # pixels from edge to trigger crossing


@dataclass
class GlobalMonitor:
    """A monitor placed in the global virtual coordinate space."""
    machine_id: str
    monitor_id: str
    global_x: int
    global_y: int
    width: int
    height: int

    @property
    def right(self) -> int:
        return self.global_x + self.width

    @property
    def bottom(self) -> int:
        return self.global_y + self.height

    def contains(self, gx: int, gy: int) -> bool:
        return (self.global_x <= gx < self.right and
                self.global_y <= gy < self.bottom)

    def to_dict(self) -> dict:
        return {
            "machine_id": self.machine_id,
            "monitor_id": self.monitor_id,
            "global_x": self.global_x,
            "global_y": self.global_y,
            "width": self.width,
            "height": self.height,
        }


@dataclass
class Edge:
    """Represents one edge of a monitor."""
    direction: str          # "left", "right", "top", "bottom"
    monitor: GlobalMonitor
    # The edge line segment in global coords
    start: int              # start of edge (y for left/right, x for top/bottom)
    end: int                # end of edge
    position: int           # x for left/right, y for top/bottom


class LayoutManager:
    """
    Manages the global monitor layout and computes edge crossings.

    Supports two modes:
    1. Auto-layout: monitors are placed left-to-right in registration order.
    2. Config-based: positions come from a config file.
    """

    def __init__(self):
        self.monitors: list[GlobalMonitor] = []
        self._adjacency: dict[str, list[tuple[Edge, GlobalMonitor]]] = {}
        # Per-machine global origin: the offset added to local coords.
        # Stored as {machine_id: (origin_gx, origin_gy)}.
        self._machine_origin: dict[str, tuple[int, int]] = {}

    def clear(self):
        self.monitors.clear()
        self._adjacency.clear()
        self._machine_origin.clear()

    def add_monitors(self, machine_id: str, monitor_defs: list[dict],
                     offsets: Optional[dict] = None):
        """
        Add monitors for a machine.

        Args:
            machine_id: unique machine identifier
            monitor_defs: list of dicts with keys: id, local_x, local_y, width, height
            offsets: optional dict {machine_id: (global_x_offset, global_y_offset)}
        """
        if offsets and machine_id in offsets:
            ox, oy = offsets[machine_id]
        else:
            # Auto-place: put to the right of all existing monitors
            ox = 0
            if self.monitors:
                ox = max(m.right for m in self.monitors)
            oy = 0

        self._machine_origin[machine_id] = (ox, oy)

        for mdef in monitor_defs:
            gm = GlobalMonitor(
                machine_id=machine_id,
                monitor_id=mdef["id"],
                global_x=ox + mdef["local_x"],
                global_y=oy + mdef["local_y"],
                width=mdef["width"],
                height=mdef["height"],
            )
            self.monitors.append(gm)
            log.info("Added monitor %s:%s at global (%d,%d) %dx%d",
                     machine_id, gm.monitor_id,
                     gm.global_x, gm.global_y, gm.width, gm.height)

        self._rebuild_adjacency()

    def remove_machine(self, machine_id: str):
        self.monitors = [m for m in self.monitors if m.machine_id != machine_id]
        self._rebuild_adjacency()

    def _rebuild_adjacency(self):
        """Pre-compute which monitor edges are adjacent to other monitors."""
        self._adjacency.clear()
        for m in self.monitors:
            key = f"{m.machine_id}:{m.monitor_id}"
            self._adjacency[key] = []

        for m in self.monitors:
            key = f"{m.machine_id}:{m.monitor_id}"
            for other in self.monitors:
                if m is other:
                    continue
                # Check if 'other' is adjacent to the right edge of 'm'
                if other.global_x == m.right:
                    overlap_start = max(m.global_y, other.global_y)
                    overlap_end = min(m.bottom, other.bottom)
                    if overlap_start < overlap_end:
                        edge = Edge("right", m, overlap_start, overlap_end, m.right)
                        self._adjacency[key].append((edge, other))

                # Left edge
                if other.right == m.global_x:
                    overlap_start = max(m.global_y, other.global_y)
                    overlap_end = min(m.bottom, other.bottom)
                    if overlap_start < overlap_end:
                        edge = Edge("left", m, overlap_start, overlap_end, m.global_x)
                        self._adjacency[key].append((edge, other))

                # Bottom edge
                if other.global_y == m.bottom:
                    overlap_start = max(m.global_x, other.global_x)
                    overlap_end = min(m.right, other.right)
                    if overlap_start < overlap_end:
                        edge = Edge("bottom", m, overlap_start, overlap_end, m.bottom)
                        self._adjacency[key].append((edge, other))

                # Top edge
                if other.bottom == m.global_y:
                    overlap_start = max(m.global_x, other.global_x)
                    overlap_end = min(m.right, other.right)
                    if overlap_start < overlap_end:
                        edge = Edge("top", m, overlap_start, overlap_end, m.global_y)
                        self._adjacency[key].append((edge, other))

    def get_monitor_at(self, gx: int, gy: int) -> Optional[GlobalMonitor]:
        """Find which monitor contains the given global coordinates."""
        for m in self.monitors:
            if m.contains(gx, gy):
                return m
        return None

    def get_monitors_for_machine(self, machine_id: str) -> list[GlobalMonitor]:
        return [m for m in self.monitors if m.machine_id == machine_id]

    def find_crossing_target(self, edge: str, global_x: int,
                             global_y: int) -> Optional[tuple[GlobalMonitor, int, int]]:
        """
        Given an edge hit at global coords, find the target monitor on
        the other side.

        Returns:
            (target_monitor, entry_global_x, entry_global_y) or None
        """
        # Find current monitor
        current = self.get_monitor_at(global_x, global_y)
        if current is None:
            # Cursor is at edge, try adjusting
            if edge == "right":
                current = self.get_monitor_at(global_x - 1, global_y)
            elif edge == "left":
                current = self.get_monitor_at(global_x + 1, global_y)
            elif edge == "bottom":
                current = self.get_monitor_at(global_x, global_y - 1)
            elif edge == "top":
                current = self.get_monitor_at(global_x, global_y + 1)

        if current is None:
            return None

        key = f"{current.machine_id}:{current.monitor_id}"
        adjacents = self._adjacency.get(key, [])

        # Must exceed the client-side EDGE_MARGIN (2px) or the newly-active
        # client will instantly detect itself at the edge and bounce the
        # cursor straight back.
        ENTRY_INSET = 8

        for adj_edge, target in adjacents:
            if adj_edge.direction != edge:
                continue
            if edge in ("left", "right"):
                if adj_edge.start <= global_y < adj_edge.end:
                    if edge == "right":
                        entry_x = target.global_x + ENTRY_INSET
                    else:
                        entry_x = target.right - 1 - ENTRY_INSET
                    entry_y = max(target.global_y,
                                  min(global_y, target.bottom - 1))
                    entry_x = max(target.global_x,
                                  min(entry_x, target.right - 1))
                    return (target, entry_x, entry_y)
            else:
                if adj_edge.start <= global_x < adj_edge.end:
                    if edge == "bottom":
                        entry_y = target.global_y + ENTRY_INSET
                    else:
                        entry_y = target.bottom - 1 - ENTRY_INSET
                    entry_x = max(target.global_x,
                                  min(global_x, target.right - 1))
                    entry_y = max(target.global_y,
                                  min(entry_y, target.bottom - 1))
                    return (target, entry_x, entry_y)

        return None

    def global_to_local(self, machine_id: str, gx: int,
                        gy: int) -> Optional[tuple[int, int]]:
        """
        Convert global virtual coords to local X screen coords.

        Uses the machine's stored origin offset:
            global = origin + local  →  local = global - origin
        """
        origin = self._machine_origin.get(machine_id)
        if origin is None:
            return None
        ox, oy = origin
        return (gx - ox, gy - oy)

    def local_to_global(self, machine_id: str, local_x: int,
                        local_y: int) -> Optional[tuple[int, int]]:
        """
        Convert local X screen coords to global virtual coords.

        Uses the machine's stored origin offset:
            global = origin + local
        """
        origin = self._machine_origin.get(machine_id)
        if origin is None:
            return None
        ox, oy = origin
        return (local_x + ox, local_y + oy)

    def get_layout_info(self) -> list[dict]:
        """Return serializable layout info for all monitors."""
        return [m.to_dict() for m in self.monitors]

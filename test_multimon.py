#!/usr/bin/env python3
"""
Tests for multi-monitor scenarios.

Covers: dual-monitor PCs, mixed resolutions, vertical stacking,
L-shaped layouts, internal vs external edge detection, and
coordinate conversion accuracy.
"""

from ud.layout import LayoutManager, GlobalMonitor


def test_dual_monitor_same_machine():
    """Two monitors on one PC — internal edges should NOT be cross-machine."""
    lm = LayoutManager()
    lm.add_monitors("pc1", [
        {"id": "HDMI-1", "local_x": 0, "local_y": 0,
         "width": 1920, "height": 1080},
        {"id": "DP-1", "local_x": 1920, "local_y": 0,
         "width": 1920, "height": 1080},
    ])
    lm.add_monitors("pc2", [
        {"id": "HDMI-1", "local_x": 0, "local_y": 0,
         "width": 1920, "height": 1080},
    ])

    # Internal edge: right side of pc1:HDMI-1 → left side of pc1:DP-1
    result = lm.find_crossing_target("right", 1919, 540)
    assert result is not None
    assert result[0].machine_id == "pc1", (
        f"Internal edge should stay on pc1, got {result[0].machine_id}")
    assert result[0].monitor_id == "DP-1"
    print("  [PASS] Internal right edge stays on same machine")

    # External edge: right side of pc1:DP-1 → pc2
    result = lm.find_crossing_target("right", 3839, 540)
    assert result is not None
    assert result[0].machine_id == "pc2"
    print("  [PASS] External right edge crosses to pc2")

    # Left edge of pc1:HDMI-1 → nothing (outer boundary)
    result = lm.find_crossing_target("left", 0, 540)
    assert result is None
    print("  [PASS] Outer left edge → no crossing")


def test_dual_monitor_both_machines():
    """Both PCs have two monitors each."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (3840, 0)}
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "s1", "local_x": 1920, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "s1", "local_x": 1920, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)

    assert len(lm.monitors) == 4
    # pc1:s0 at (0,0), pc1:s1 at (1920,0), pc2:s0 at (3840,0), pc2:s1 at (5760,0)

    # Cross from pc1:s1 → pc2:s0
    result = lm.find_crossing_target("right", 3839, 540)
    assert result is not None
    assert result[0].machine_id == "pc2"
    assert result[0].monitor_id == "s0"
    print("  [PASS] 4-monitor layout: pc1:s1 → pc2:s0")

    # Cross from pc2:s0 → pc1:s1
    result = lm.find_crossing_target("left", 3840, 540)
    assert result is not None
    assert result[0].machine_id == "pc1"
    assert result[0].monitor_id == "s1"
    print("  [PASS] 4-monitor layout: pc2:s0 → pc1:s1")

    # Internal: pc2:s0 → pc2:s1 (same machine)
    result = lm.find_crossing_target("right", 5759, 540)
    assert result is not None
    assert result[0].machine_id == "pc2"
    assert result[0].monitor_id == "s1"
    print("  [PASS] 4-monitor layout: pc2 internal edge correctly identified")


def test_mixed_resolutions():
    """PCs with different monitor resolutions."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (2560, 0)}
    # PC1: 2560x1440
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 2560, "height": 1440},
    ], offsets)
    # PC2: 1920x1080 — shorter vertically
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)

    # Crossing within overlapping Y range (0-1080)
    result = lm.find_crossing_target("right", 2559, 500)
    assert result is not None
    assert result[0].machine_id == "pc2"
    print("  [PASS] Mixed res: crossing in overlap zone")

    # Crossing at Y=1200 — below pc2's monitor, no target
    result = lm.find_crossing_target("right", 2559, 1200)
    assert result is None
    print("  [PASS] Mixed res: no crossing below shorter monitor")

    # Entry Y should be clamped
    result = lm.find_crossing_target("right", 2559, 1079)
    assert result is not None
    _, ex, ey = result
    assert ey == 1079
    print(f"  [PASS] Mixed res: entry at edge → ({ex},{ey})")


def test_vertical_dual_monitor():
    """Monitor stacked vertically on one PC."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (1920, 0)}
    # PC1: two monitors, one above the other
    lm.add_monitors("pc1", [
        {"id": "top", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "bottom", "local_x": 0, "local_y": 1080, "width": 1920, "height": 1080},
    ], offsets)
    # PC2: one monitor, aligned with PC1's top monitor
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)

    # PC1:top right edge → PC2 (overlapping Y: 0-1080)
    result = lm.find_crossing_target("right", 1919, 500)
    assert result is not None
    assert result[0].machine_id == "pc2"
    print("  [PASS] Vertical stack: top monitor → pc2")

    # PC1:bottom right edge → nothing (pc2 only overlaps with top)
    result = lm.find_crossing_target("right", 1919, 1500)
    assert result is None
    print("  [PASS] Vertical stack: bottom monitor right edge → no crossing")

    # PC1 internal: top → bottom (same machine)
    result = lm.find_crossing_target("bottom", 960, 1079)
    assert result is not None
    assert result[0].machine_id == "pc1"
    assert result[0].monitor_id == "bottom"
    print("  [PASS] Vertical stack: internal top→bottom edge")


def test_l_shaped_layout():
    """L-shaped monitor arrangement."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (1920, 0), "pc3": (1920, 1080)}
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    lm.add_monitors("pc3", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)

    # Layout:
    # [PC1] [PC2]
    #       [PC3]

    # PC1 → PC2 (right)
    result = lm.find_crossing_target("right", 1919, 540)
    assert result is not None
    assert result[0].machine_id == "pc2"
    print("  [PASS] L-shape: pc1 → pc2 (right)")

    # PC2 → PC3 (bottom)
    result = lm.find_crossing_target("bottom", 2880, 1079)
    assert result is not None
    assert result[0].machine_id == "pc3"
    print("  [PASS] L-shape: pc2 → pc3 (down)")

    # PC3 → PC2 (top)
    result = lm.find_crossing_target("top", 2880, 1080)
    assert result is not None
    assert result[0].machine_id == "pc2"
    print("  [PASS] L-shape: pc3 → pc2 (up)")

    # PC1 bottom → nothing (no monitor below pc1)
    result = lm.find_crossing_target("bottom", 960, 1079)
    assert result is None
    print("  [PASS] L-shape: pc1 bottom → no crossing")


def test_coord_conversion_multimon():
    """Coordinate conversion with multi-monitor machines."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (3840, 0)}

    # PC1: two 1920x1080 monitors
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "s1", "local_x": 1920, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    # PC2: one monitor
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)

    # PC1 local (500, 300) → global (500, 300)
    g = lm.local_to_global("pc1", 500, 300)
    assert g == (500, 300), f"Expected (500,300), got {g}"

    # PC1 local (2000, 300) → global (2000, 300) — on second monitor
    g = lm.local_to_global("pc1", 2000, 300)
    assert g == (2000, 300), f"Expected (2000,300), got {g}"

    # PC2 local (100, 100) → global (3940, 100)
    g = lm.local_to_global("pc2", 100, 100)
    assert g == (3940, 100), f"Expected (3940,100), got {g}"

    # Reverse: global (3940, 100) → PC2 local (100, 100)
    l = lm.global_to_local("pc2", 3940, 100)
    assert l == (100, 100), f"Expected (100,100), got {l}"

    # Reverse: global (2000, 300) → PC1 local (2000, 300)
    l = lm.global_to_local("pc1", 2000, 300)
    assert l == (2000, 300), f"Expected (2000,300), got {l}"

    print("  [PASS] Coordinate conversion with multi-monitor PCs")


def test_coord_conversion_vertical_stack():
    """Coordinate conversion with vertically stacked monitors."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0)}

    lm.add_monitors("pc1", [
        {"id": "top", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "bot", "local_x": 0, "local_y": 1080, "width": 1920, "height": 1080},
    ], offsets)

    # Local (960, 1500) is on the bottom monitor → global (960, 1500)
    g = lm.local_to_global("pc1", 960, 1500)
    assert g == (960, 1500)

    l = lm.global_to_local("pc1", 960, 1500)
    assert l == (960, 1500)
    print("  [PASS] Coord conversion with vertical stack")


def test_three_machines_horizontal():
    """Three machines in a row."""
    lm = LayoutManager()
    # Auto-placement: left to right
    lm.add_monitors("left", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])
    lm.add_monitors("center", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 2560, "height": 1440},
    ])
    lm.add_monitors("right", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])

    assert lm.monitors[0].global_x == 0       # left
    assert lm.monitors[1].global_x == 1920    # center
    assert lm.monitors[2].global_x == 4480    # right (1920+2560)

    # left → center
    result = lm.find_crossing_target("right", 1919, 540)
    assert result is not None
    assert result[0].machine_id == "center"

    # center → right
    result = lm.find_crossing_target("right", 4479, 540)
    assert result is not None
    assert result[0].machine_id == "right"

    # center → left
    result = lm.find_crossing_target("left", 1920, 540)
    assert result is not None
    assert result[0].machine_id == "left"

    # Y outside overlap: center is 1440 tall, right is 1080 tall
    # At Y=1200 on center's right edge → right monitor doesn't reach
    result = lm.find_crossing_target("right", 4479, 1200)
    assert result is None

    print("  [PASS] Three machines horizontal with mixed sizes")


def test_edge_hit_filtering():
    """
    Simulate what the client does: detect edge, check if target is
    a different machine, only send if so.
    """
    lm = LayoutManager()
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "s1", "local_x": 1920, "local_y": 0, "width": 1920, "height": 1080},
    ])
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])

    machine_id = "pc1"
    test_cases = [
        # (local_x, local_y, expected_edge, should_cross_machine)
        (1919, 540, "right", False),   # right edge of s0 → s1 (same machine)
        (3839, 540, "right", True),    # right edge of s1 → pc2
        (0, 540, "left", False),       # left edge of s0 → nothing
        (1920, 540, None, False),      # left edge of s1 → s0 (same machine, but not at edge)
        (960, 0, "top", False),        # top edge → nothing
        (960, 1079, "bottom", False),  # bottom edge → nothing
    ]

    EDGE_MARGIN = 2
    for local_x, local_y, expected_edge, should_cross in test_cases:
        gx, gy = lm.local_to_global(machine_id, local_x, local_y)
        # Find current monitor
        my_monitors = lm.get_monitors_for_machine(machine_id)
        current = None
        for m in my_monitors:
            if m.contains(gx, gy):
                current = m
                break
        if current is None:
            for m in my_monitors:
                if abs(gx - m.right) <= 1 or abs(gx - m.global_x) <= 1:
                    current = m
                    break

        # Detect edge (matching client logic)
        edge = None
        if current:
            if gx <= current.global_x + EDGE_MARGIN:
                edge = "left"
            elif gx >= current.right - 1 - EDGE_MARGIN:
                edge = "right"
            elif gy <= current.global_y + EDGE_MARGIN:
                edge = "top"
            elif gy >= current.bottom - 1 - EDGE_MARGIN:
                edge = "bottom"

        if expected_edge:
            assert edge == expected_edge, (
                f"At ({local_x},{local_y}): expected edge={expected_edge}, got {edge}")

        would_cross = False
        if edge:
            result = lm.find_crossing_target(edge, gx, gy)
            if result and result[0].machine_id != machine_id:
                would_cross = True

        assert would_cross == should_cross, (
            f"At ({local_x},{local_y}) edge={edge}: "
            f"expected cross={should_cross}, got {would_cross}")

    print("  [PASS] Edge hit filtering matches client logic for all cases")


if __name__ == "__main__":
    print("Multi-monitor tests:\n")

    print("1. Dual monitor same machine")
    test_dual_monitor_same_machine()

    print("\n2. Dual monitors on both machines")
    test_dual_monitor_both_machines()

    print("\n3. Mixed resolutions")
    test_mixed_resolutions()

    print("\n4. Vertical dual monitor")
    test_vertical_dual_monitor()

    print("\n5. L-shaped layout")
    test_l_shaped_layout()

    print("\n6. Coordinate conversion (multi-monitor)")
    test_coord_conversion_multimon()

    print("\n7. Coordinate conversion (vertical stack)")
    test_coord_conversion_vertical_stack()

    print("\n8. Three machines horizontal")
    test_three_machines_horizontal()

    print("\n9. Edge hit filtering (simulated client)")
    test_edge_hit_filtering()

    print("\n\nAll multi-monitor tests passed!")

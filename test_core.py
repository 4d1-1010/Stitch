#!/usr/bin/env python3
"""Tests for protocol serialization and layout edge detection."""

from ud.protocol import (
    MsgType, encode_message, decode_header, decode_payload,
    MouseMoveRelMsg, RegisterMsg, EdgeHitMsg, HEADER_SIZE,
)
from ud.layout import LayoutManager


def test_protocol_roundtrip():
    """Messages survive encode → decode."""
    msg = MouseMoveRelMsg(dx=42, dy=-7)
    data = encode_message(MsgType.MOUSE_MOVE_REL, msg)
    mt, length = decode_header(data[:HEADER_SIZE])
    assert mt == MsgType.MOUSE_MOVE_REL
    payload = decode_payload(mt, data[HEADER_SIZE:])
    assert payload.dx == 42
    assert payload.dy == -7
    print("  [PASS] protocol roundtrip")


def test_protocol_register():
    """Register message with nested monitors."""
    msg = RegisterMsg(
        machine_id="pc1",
        monitors=[{"id": "HDMI-1", "local_x": 0, "local_y": 0,
                    "width": 1920, "height": 1080}],
    )
    data = encode_message(MsgType.REGISTER, msg)
    mt, length = decode_header(data[:HEADER_SIZE])
    payload = decode_payload(mt, data[HEADER_SIZE:])
    assert payload.machine_id == "pc1"
    assert len(payload.monitors) == 1
    assert payload.monitors[0]["width"] == 1920
    print("  [PASS] register message")


def test_layout_auto_place():
    """Auto-placement puts monitors left-to-right."""
    lm = LayoutManager()
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])
    assert len(lm.monitors) == 2
    assert lm.monitors[0].global_x == 0
    assert lm.monitors[1].global_x == 1920
    print("  [PASS] auto-placement")


def test_layout_edge_crossing():
    """Cursor at right edge of PC1 should cross to PC2."""
    lm = LayoutManager()
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])

    # Cursor at right edge of PC1 monitor, middle Y
    result = lm.find_crossing_target("right", 1919, 540)
    assert result is not None, "Should find crossing target"
    target, ex, ey = result
    assert target.machine_id == "pc2"
    assert target.global_x == 1920
    assert ex >= 1920
    print(f"  [PASS] right-edge crossing → PC2 at ({ex},{ey})")

    # Cursor at left edge of PC2 should cross back
    result = lm.find_crossing_target("left", 1920, 540)
    assert result is not None
    target, ex, ey = result
    assert target.machine_id == "pc1"
    print(f"  [PASS] left-edge crossing → PC1 at ({ex},{ey})")


def test_layout_no_crossing():
    """Cursor at the outer edge (no neighbor) should not cross."""
    lm = LayoutManager()
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])
    result = lm.find_crossing_target("right", 1919, 540)
    assert result is None
    print("  [PASS] no crossing at outer edge")


def test_layout_explicit_offsets():
    """Explicit offsets place monitors correctly."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (1920, 0)}
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    assert lm.monitors[1].global_x == 1920
    print("  [PASS] explicit offsets")


def test_layout_multi_monitor():
    """Multi-monitor machine with edge crossing."""
    lm = LayoutManager()
    # PC1 has two monitors side by side
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
        {"id": "s1", "local_x": 1920, "local_y": 0, "width": 1920, "height": 1080},
    ])
    # PC2 placed to the right
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ])
    assert lm.monitors[2].global_x == 3840
    # Crossing from PC1's second monitor to PC2
    result = lm.find_crossing_target("right", 3839, 540)
    assert result is not None
    assert result[0].machine_id == "pc2"
    print("  [PASS] multi-monitor crossing")


def test_layout_vertical():
    """Vertical layout — top/bottom crossing."""
    lm = LayoutManager()
    offsets = {"top": (0, 0), "bottom": (0, 1080)}
    lm.add_monitors("top", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    lm.add_monitors("bottom", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    result = lm.find_crossing_target("bottom", 960, 1079)
    assert result is not None
    assert result[0].machine_id == "bottom"
    print(f"  [PASS] vertical crossing → bottom at ({result[1]},{result[2]})")


def test_coord_conversion():
    """Global ↔ local coordinate conversion."""
    lm = LayoutManager()
    offsets = {"pc1": (0, 0), "pc2": (1920, 0)}
    lm.add_monitors("pc1", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)
    lm.add_monitors("pc2", [
        {"id": "s0", "local_x": 0, "local_y": 0, "width": 1920, "height": 1080},
    ], offsets)

    # PC2 local (100, 200) → global (2020, 200)
    g = lm.local_to_global("pc2", 100, 200)
    assert g == (2020, 200), f"Expected (2020,200), got {g}"

    # Global (2020, 200) → PC2 local (100, 200)
    l = lm.global_to_local("pc2", 2020, 200)
    assert l == (100, 200), f"Expected (100,200), got {l}"
    print("  [PASS] coordinate conversion")


if __name__ == "__main__":
    print("Protocol tests:")
    test_protocol_roundtrip()
    test_protocol_register()

    print("\nLayout tests:")
    test_layout_auto_place()
    test_layout_edge_crossing()
    test_layout_no_crossing()
    test_layout_explicit_offsets()
    test_layout_multi_monitor()
    test_layout_vertical()
    test_coord_conversion()

    print("\nAll tests passed!")

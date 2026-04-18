#!/usr/bin/env python3
"""Tests for cross-platform keycode mapping."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from unio.backends.keycodes import (
    evdev_to_hid, hid_to_evdev, x11_to_hid, hid_to_x11,
    vk_to_hid, hid_to_vk, mac_to_hid, hid_to_mac,
    HID_A, HID_Z, HID_0, HID_1, HID_9, HID_ENTER, HID_ESCAPE,
    HID_LEFT_SHIFT, HID_RIGHT_CTRL, HID_SPACE, HID_F1, HID_F12,
    HID_LEFT, HID_RIGHT, HID_UP, HID_DOWN, HID_TAB,
    HID_LEFT_GUI, HID_LEFT_ALT,
)


def test_evdev_roundtrip():
    """evdev → HID → evdev is identity for known keys."""
    test_keys = [
        (30, HID_A), (44, HID_Z), (2, HID_1), (11, HID_0),
        (28, HID_ENTER), (1, HID_ESCAPE), (42, HID_LEFT_SHIFT),
        (57, HID_SPACE), (59, HID_F1), (88, HID_F12),
        (105, HID_LEFT), (106, HID_RIGHT), (103, HID_UP), (108, HID_DOWN),
    ]
    for evdev, expected_hid in test_keys:
        hid = evdev_to_hid(evdev)
        assert hid == expected_hid, f"evdev {evdev}: expected HID 0x{expected_hid:02x}, got 0x{hid:02x}"
        back = hid_to_evdev(hid)
        assert back == evdev, f"HID 0x{hid:02x}: expected evdev {evdev}, got {back}"
    print("  [PASS] evdev ↔ HID roundtrip (14 keys)")


def test_x11_roundtrip():
    """X11 keycode = evdev + 8, so x11 → HID → x11 works."""
    for evdev_code in [30, 44, 28, 1, 42, 57, 105, 108]:
        x11 = evdev_code + 8
        hid = x11_to_hid(x11)
        assert hid != 0, f"x11 {x11} mapped to HID 0"
        back = hid_to_x11(hid)
        assert back == x11, f"HID 0x{hid:02x} → x11 {back}, expected {x11}"
    print("  [PASS] X11 ↔ HID roundtrip (8 keys)")


def test_windows_roundtrip():
    """Windows VK → HID → VK roundtrip."""
    test_keys = [
        (0x41, HID_A),   # VK_A
        (0x5A, HID_Z),   # VK_Z
        (0x30, HID_0),   # VK_0
        (0x39, HID_9),   # VK_9
        (0x0D, HID_ENTER),
        (0x1B, HID_ESCAPE),
        (0xA0, HID_LEFT_SHIFT),
        (0x20, HID_SPACE),
        (0x70, HID_F1),  # VK_F1
        (0x7B, HID_F12),
        (0x25, HID_LEFT),
        (0x28, HID_DOWN),
        (0x5B, HID_LEFT_GUI),  # VK_LWIN
        (0x09, HID_TAB),
    ]
    for vk, expected_hid in test_keys:
        hid = vk_to_hid(vk)
        assert hid == expected_hid, f"VK 0x{vk:02x}: expected HID 0x{expected_hid:02x}, got 0x{hid:02x}"
        back = hid_to_vk(hid)
        assert back == vk, f"HID 0x{hid:02x}: expected VK 0x{vk:02x}, got 0x{back:02x}"
    print("  [PASS] Windows VK ↔ HID roundtrip (14 keys)")


def test_macos_roundtrip():
    """macOS keycode → HID → macOS roundtrip."""
    test_keys = [
        (0x00, HID_A),
        (0x06, HID_Z),
        (0x24, HID_ENTER),
        (0x35, HID_ESCAPE),
        (0x38, HID_LEFT_SHIFT),
        (0x31, HID_SPACE),
        (0x7A, HID_F1),
        (0x6F, HID_F12),
        (0x7B, HID_LEFT),
        (0x7C, HID_RIGHT),
        (0x37, HID_LEFT_GUI),  # Command key
        (0x3A, HID_LEFT_ALT),  # Option key
    ]
    for mac, expected_hid in test_keys:
        hid = mac_to_hid(mac)
        assert hid == expected_hid, f"mac 0x{mac:02x}: expected HID 0x{expected_hid:02x}, got 0x{hid:02x}"
        back = hid_to_mac(hid)
        assert back == mac, f"HID 0x{hid:02x}: expected mac 0x{mac:02x}, got 0x{back:02x}"
    print("  [PASS] macOS ↔ HID roundtrip (12 keys)")


def test_cross_platform_identity():
    """Same HID code maps to correct native code on each platform."""
    # Press 'A': should work on all platforms
    hid = HID_A
    assert hid_to_evdev(hid) == 30        # Linux evdev KEY_A
    assert hid_to_x11(hid) == 38          # X11 = evdev + 8
    assert hid_to_vk(hid) == 0x41         # Windows VK_A
    assert hid_to_mac(hid) == 0x00        # macOS kVK_ANSI_A
    print("  [PASS] HID_A maps correctly on all platforms")

    hid = HID_ENTER
    assert hid_to_evdev(hid) == 28
    assert hid_to_x11(hid) == 36
    assert hid_to_vk(hid) == 0x0D
    assert hid_to_mac(hid) == 0x24
    print("  [PASS] HID_ENTER maps correctly on all platforms")

    hid = HID_LEFT_SHIFT
    assert hid_to_evdev(hid) == 42
    assert hid_to_vk(hid) == 0xA0         # VK_LSHIFT
    assert hid_to_mac(hid) == 0x38
    print("  [PASS] HID_LEFT_SHIFT maps correctly on all platforms")


def test_unknown_codes():
    """Unknown native codes return 0, unknown HID codes return 0/-1."""
    assert evdev_to_hid(9999) == 0
    assert hid_to_evdev(0xFF) == 0
    assert vk_to_hid(0xFF) == 0
    assert hid_to_vk(0xFF) == 0
    assert mac_to_hid(0xFF) == 0
    assert hid_to_mac(0xFF) == -1
    print("  [PASS] Unknown codes handled gracefully")


if __name__ == "__main__":
    print("Keycode mapping tests:\n")
    test_evdev_roundtrip()
    test_x11_roundtrip()
    test_windows_roundtrip()
    test_macos_roundtrip()
    test_cross_platform_identity()
    test_unknown_codes()
    print("\nAll keycode tests passed!")

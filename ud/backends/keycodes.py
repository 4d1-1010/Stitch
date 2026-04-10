"""
Universal keycode mapping using USB HID Usage IDs.

All platforms convert their native keycodes to/from HID scan codes
at the boundary. The wire protocol carries HID codes exclusively,
making it OS-agnostic.

Reference: USB HID Usage Tables, Section 10 (Keyboard/Keypad Page 0x07)
"""

# ── HID Usage IDs (canonical key identifiers) ───────────────────

# Letters
HID_A = 0x04
HID_B = 0x05
HID_C = 0x06
HID_D = 0x07
HID_E = 0x08
HID_F = 0x09
HID_G = 0x0A
HID_H = 0x0B
HID_I = 0x0C
HID_J = 0x0D
HID_K = 0x0E
HID_L = 0x0F
HID_M = 0x10
HID_N = 0x11
HID_O = 0x12
HID_P = 0x13
HID_Q = 0x14
HID_R = 0x15
HID_S = 0x16
HID_T = 0x17
HID_U = 0x18
HID_V = 0x19
HID_W = 0x1A
HID_X = 0x1B
HID_Y = 0x1C
HID_Z = 0x1D

# Numbers
HID_1 = 0x1E
HID_2 = 0x1F
HID_3 = 0x20
HID_4 = 0x21
HID_5 = 0x22
HID_6 = 0x23
HID_7 = 0x24
HID_8 = 0x25
HID_9 = 0x26
HID_0 = 0x27

# Control keys
HID_ENTER = 0x28
HID_ESCAPE = 0x29
HID_BACKSPACE = 0x2A
HID_TAB = 0x2B
HID_SPACE = 0x2C

# Symbols row 1
HID_MINUS = 0x2D
HID_EQUAL = 0x2E
HID_LBRACKET = 0x2F
HID_RBRACKET = 0x30
HID_BACKSLASH = 0x31
HID_HASH = 0x32          # Non-US # and ~
HID_SEMICOLON = 0x33
HID_APOSTROPHE = 0x34
HID_GRAVE = 0x35          # ` and ~
HID_COMMA = 0x36
HID_PERIOD = 0x37
HID_SLASH = 0x38

# Lock keys
HID_CAPS_LOCK = 0x39

# Function keys
HID_F1 = 0x3A
HID_F2 = 0x3B
HID_F3 = 0x3C
HID_F4 = 0x3D
HID_F5 = 0x3E
HID_F6 = 0x3F
HID_F7 = 0x40
HID_F8 = 0x41
HID_F9 = 0x42
HID_F10 = 0x43
HID_F11 = 0x44
HID_F12 = 0x45

# System keys
HID_PRINT_SCREEN = 0x46
HID_SCROLL_LOCK = 0x47
HID_PAUSE = 0x48

# Navigation
HID_INSERT = 0x49
HID_HOME = 0x4A
HID_PAGE_UP = 0x4B
HID_DELETE = 0x4C
HID_END = 0x4D
HID_PAGE_DOWN = 0x4E
HID_RIGHT = 0x4F
HID_LEFT = 0x50
HID_DOWN = 0x51
HID_UP = 0x52

# Keypad
HID_NUM_LOCK = 0x53
HID_KP_DIVIDE = 0x54
HID_KP_MULTIPLY = 0x55
HID_KP_MINUS = 0x56
HID_KP_PLUS = 0x57
HID_KP_ENTER = 0x58
HID_KP_1 = 0x59
HID_KP_2 = 0x5A
HID_KP_3 = 0x5B
HID_KP_4 = 0x5C
HID_KP_5 = 0x5D
HID_KP_6 = 0x5E
HID_KP_7 = 0x5F
HID_KP_8 = 0x60
HID_KP_9 = 0x61
HID_KP_0 = 0x62
HID_KP_PERIOD = 0x63

# Additional
HID_NON_US_BACKSLASH = 0x64
HID_APPLICATION = 0x65    # Menu/Context key
HID_F13 = 0x68
HID_F14 = 0x69
HID_F15 = 0x6A
HID_F16 = 0x6B
HID_F17 = 0x6C
HID_F18 = 0x6D
HID_F19 = 0x6E
HID_F20 = 0x6F

# Modifiers (HID page 0x07, usage 0xE0-0xE7)
HID_LEFT_CTRL = 0xE0
HID_LEFT_SHIFT = 0xE1
HID_LEFT_ALT = 0xE2
HID_LEFT_GUI = 0xE3       # Windows/Super/Command
HID_RIGHT_CTRL = 0xE4
HID_RIGHT_SHIFT = 0xE5
HID_RIGHT_ALT = 0xE6
HID_RIGHT_GUI = 0xE7


# ── Linux evdev → HID ───────────────────────────────────────────
# evdev scancodes from <linux/input-event-codes.h>

_EVDEV_TO_HID: dict[int, int] = {
    1: HID_ESCAPE, 2: HID_1, 3: HID_2, 4: HID_3, 5: HID_4, 6: HID_5,
    7: HID_6, 8: HID_7, 9: HID_8, 10: HID_9, 11: HID_0,
    12: HID_MINUS, 13: HID_EQUAL, 14: HID_BACKSPACE, 15: HID_TAB,
    16: HID_Q, 17: HID_W, 18: HID_E, 19: HID_R, 20: HID_T,
    21: HID_Y, 22: HID_U, 23: HID_I, 24: HID_O, 25: HID_P,
    26: HID_LBRACKET, 27: HID_RBRACKET, 28: HID_ENTER,
    29: HID_LEFT_CTRL, 30: HID_A, 31: HID_S, 32: HID_D, 33: HID_F,
    34: HID_G, 35: HID_H, 36: HID_J, 37: HID_K, 38: HID_L,
    39: HID_SEMICOLON, 40: HID_APOSTROPHE, 41: HID_GRAVE,
    42: HID_LEFT_SHIFT, 43: HID_BACKSLASH,
    44: HID_Z, 45: HID_X, 46: HID_C, 47: HID_V, 48: HID_B,
    49: HID_N, 50: HID_M, 51: HID_COMMA, 52: HID_PERIOD, 53: HID_SLASH,
    54: HID_RIGHT_SHIFT, 55: HID_KP_MULTIPLY, 56: HID_LEFT_ALT,
    57: HID_SPACE, 58: HID_CAPS_LOCK,
    59: HID_F1, 60: HID_F2, 61: HID_F3, 62: HID_F4, 63: HID_F5,
    64: HID_F6, 65: HID_F7, 66: HID_F8, 67: HID_F9, 68: HID_F10,
    69: HID_NUM_LOCK, 70: HID_SCROLL_LOCK,
    71: HID_KP_7, 72: HID_KP_8, 73: HID_KP_9, 74: HID_KP_MINUS,
    75: HID_KP_4, 76: HID_KP_5, 77: HID_KP_6, 78: HID_KP_PLUS,
    79: HID_KP_1, 80: HID_KP_2, 81: HID_KP_3, 82: HID_KP_0,
    83: HID_KP_PERIOD, 86: HID_NON_US_BACKSLASH,
    87: HID_F11, 88: HID_F12,
    96: HID_KP_ENTER, 97: HID_RIGHT_CTRL,
    98: HID_KP_DIVIDE, 99: HID_PRINT_SCREEN, 100: HID_RIGHT_ALT,
    102: HID_HOME, 103: HID_UP, 104: HID_PAGE_UP,
    105: HID_LEFT, 106: HID_RIGHT, 107: HID_END,
    108: HID_DOWN, 109: HID_PAGE_DOWN, 110: HID_INSERT, 111: HID_DELETE,
    119: HID_PAUSE, 125: HID_LEFT_GUI, 126: HID_RIGHT_GUI,
    127: HID_APPLICATION,
}

_HID_TO_EVDEV: dict[int, int] = {v: k for k, v in _EVDEV_TO_HID.items()}

# X11 keycode = evdev + 8
EVDEV_X11_OFFSET = 8


def evdev_to_hid(evdev_code: int) -> int:
    """Convert Linux evdev scancode to HID usage ID. Returns 0 if unknown."""
    return _EVDEV_TO_HID.get(evdev_code, 0)


def hid_to_evdev(hid_code: int) -> int:
    """Convert HID usage ID to Linux evdev scancode. Returns 0 if unknown."""
    return _HID_TO_EVDEV.get(hid_code, 0)


def x11_to_hid(x11_keycode: int) -> int:
    """Convert X11 keycode to HID usage ID."""
    return evdev_to_hid(x11_keycode - EVDEV_X11_OFFSET)


def hid_to_x11(hid_code: int) -> int:
    """Convert HID usage ID to X11 keycode."""
    evdev = hid_to_evdev(hid_code)
    return evdev + EVDEV_X11_OFFSET if evdev else 0


# ── Windows Virtual Key codes → HID ─────────────────────────────

_VK_TO_HID: dict[int, int] = {
    # Letters (VK_A=0x41 .. VK_Z=0x5A)
    **{0x41 + i: HID_A + i for i in range(26)},
    # Numbers (VK_0=0x30 .. VK_9=0x39)
    0x30: HID_0, 0x31: HID_1, 0x32: HID_2, 0x33: HID_3, 0x34: HID_4,
    0x35: HID_5, 0x36: HID_6, 0x37: HID_7, 0x38: HID_8, 0x39: HID_9,
    # Function keys
    0x70: HID_F1, 0x71: HID_F2, 0x72: HID_F3, 0x73: HID_F4,
    0x74: HID_F5, 0x75: HID_F6, 0x76: HID_F7, 0x77: HID_F8,
    0x78: HID_F9, 0x79: HID_F10, 0x7A: HID_F11, 0x7B: HID_F12,
    # Modifiers
    0xA0: HID_LEFT_SHIFT, 0xA1: HID_RIGHT_SHIFT,
    0xA2: HID_LEFT_CTRL, 0xA3: HID_RIGHT_CTRL,
    0xA4: HID_LEFT_ALT, 0xA5: HID_RIGHT_ALT,
    0x5B: HID_LEFT_GUI, 0x5C: HID_RIGHT_GUI,
    # Navigation
    0x25: HID_LEFT, 0x26: HID_UP, 0x27: HID_RIGHT, 0x28: HID_DOWN,
    0x24: HID_HOME, 0x23: HID_END, 0x21: HID_PAGE_UP, 0x22: HID_PAGE_DOWN,
    0x2D: HID_INSERT, 0x2E: HID_DELETE,
    # Control
    0x0D: HID_ENTER, 0x1B: HID_ESCAPE, 0x08: HID_BACKSPACE,
    0x09: HID_TAB, 0x20: HID_SPACE, 0x14: HID_CAPS_LOCK,
    0x90: HID_NUM_LOCK, 0x91: HID_SCROLL_LOCK,
    0x2C: HID_PRINT_SCREEN, 0x13: HID_PAUSE,
    0x5D: HID_APPLICATION,
    # Symbols
    0xBD: HID_MINUS, 0xBB: HID_EQUAL,
    0xDB: HID_LBRACKET, 0xDD: HID_RBRACKET,
    0xDC: HID_BACKSLASH, 0xBA: HID_SEMICOLON,
    0xDE: HID_APOSTROPHE, 0xC0: HID_GRAVE,
    0xBC: HID_COMMA, 0xBE: HID_PERIOD, 0xBF: HID_SLASH,
    # Numpad
    0x60: HID_KP_0, 0x61: HID_KP_1, 0x62: HID_KP_2, 0x63: HID_KP_3,
    0x64: HID_KP_4, 0x65: HID_KP_5, 0x66: HID_KP_6, 0x67: HID_KP_7,
    0x68: HID_KP_8, 0x69: HID_KP_9,
    0x6E: HID_KP_PERIOD, 0x6A: HID_KP_MULTIPLY, 0x6B: HID_KP_PLUS,
    0x6D: HID_KP_MINUS, 0x6F: HID_KP_DIVIDE,
}

_HID_TO_VK: dict[int, int] = {v: k for k, v in _VK_TO_HID.items()}


def vk_to_hid(vk_code: int) -> int:
    """Convert Windows VK code to HID usage ID. Returns 0 if unknown."""
    return _VK_TO_HID.get(vk_code, 0)


def hid_to_vk(hid_code: int) -> int:
    """Convert HID usage ID to Windows VK code. Returns 0 if unknown."""
    return _HID_TO_VK.get(hid_code, 0)


# ── macOS virtual keycodes → HID ────────────────────────────────
# macOS keycodes from <Carbon/Events.h> (kVK_* constants)

_MAC_TO_HID: dict[int, int] = {
    # Letters (macOS keycodes are NOT alphabetical)
    0x00: HID_A, 0x01: HID_S, 0x02: HID_D, 0x03: HID_F, 0x04: HID_H,
    0x05: HID_G, 0x06: HID_Z, 0x07: HID_X, 0x08: HID_C, 0x09: HID_V,
    0x0B: HID_B, 0x0C: HID_Q, 0x0D: HID_W, 0x0E: HID_E, 0x0F: HID_R,
    0x10: HID_Y, 0x11: HID_T, 0x20: HID_U, 0x22: HID_I, 0x1F: HID_O,
    0x23: HID_P, 0x26: HID_J, 0x28: HID_K, 0x25: HID_L, 0x2E: HID_M,
    0x2D: HID_N,
    # Numbers
    0x12: HID_1, 0x13: HID_2, 0x14: HID_3, 0x15: HID_4, 0x17: HID_5,
    0x16: HID_6, 0x1A: HID_7, 0x1C: HID_8, 0x19: HID_9, 0x1D: HID_0,
    # Symbols
    0x1B: HID_MINUS, 0x18: HID_EQUAL,
    0x21: HID_LBRACKET, 0x1E: HID_RBRACKET,
    0x2A: HID_BACKSLASH, 0x29: HID_SEMICOLON,
    0x27: HID_APOSTROPHE, 0x32: HID_GRAVE,
    0x2B: HID_COMMA, 0x2F: HID_PERIOD, 0x2C: HID_SLASH,
    # Control
    0x24: HID_ENTER, 0x35: HID_ESCAPE, 0x33: HID_BACKSPACE,
    0x30: HID_TAB, 0x31: HID_SPACE, 0x39: HID_CAPS_LOCK,
    # Function keys
    0x7A: HID_F1, 0x78: HID_F2, 0x63: HID_F3, 0x76: HID_F4,
    0x60: HID_F5, 0x61: HID_F6, 0x62: HID_F7, 0x64: HID_F8,
    0x65: HID_F9, 0x6D: HID_F10, 0x67: HID_F11, 0x6F: HID_F12,
    0x69: HID_F13, 0x6B: HID_F14, 0x71: HID_F15, 0x6A: HID_F16,
    # Navigation
    0x7B: HID_LEFT, 0x7C: HID_RIGHT, 0x7D: HID_DOWN, 0x7E: HID_UP,
    0x73: HID_HOME, 0x77: HID_END, 0x74: HID_PAGE_UP, 0x79: HID_PAGE_DOWN,
    0x72: HID_INSERT, 0x75: HID_DELETE,
    # Modifiers
    0x38: HID_LEFT_SHIFT, 0x3C: HID_RIGHT_SHIFT,
    0x3B: HID_LEFT_CTRL, 0x3E: HID_RIGHT_CTRL,
    0x3A: HID_LEFT_ALT, 0x3D: HID_RIGHT_ALT,  # Option keys
    0x37: HID_LEFT_GUI, 0x36: HID_RIGHT_GUI,   # Command keys
    # Numpad
    0x52: HID_KP_0, 0x53: HID_KP_1, 0x54: HID_KP_2, 0x55: HID_KP_3,
    0x56: HID_KP_4, 0x57: HID_KP_5, 0x58: HID_KP_6, 0x59: HID_KP_7,
    0x5B: HID_KP_8, 0x5C: HID_KP_9,
    0x41: HID_KP_PERIOD, 0x43: HID_KP_MULTIPLY, 0x45: HID_KP_PLUS,
    0x4E: HID_KP_MINUS, 0x4B: HID_KP_DIVIDE, 0x4C: HID_KP_ENTER,
    0x47: HID_NUM_LOCK,
}

_HID_TO_MAC: dict[int, int] = {v: k for k, v in _MAC_TO_HID.items()}


def mac_to_hid(mac_keycode: int) -> int:
    """Convert macOS virtual keycode to HID usage ID. Returns 0 if unknown."""
    return _MAC_TO_HID.get(mac_keycode, 0)


def hid_to_mac(hid_code: int) -> int:
    """Convert HID usage ID to macOS virtual keycode. Returns -1 if unknown."""
    return _HID_TO_MAC.get(hid_code, -1)

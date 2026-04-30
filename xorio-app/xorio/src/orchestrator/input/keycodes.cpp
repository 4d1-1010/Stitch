/// @file keycodes.cpp
/// @brief Lookup-table data + accessors for @ref keycodes.hpp.
///
/// HID page-0x07 constants live inside an anonymous namespace so
/// they don't pollute the surrounding orchestrator namespace —
/// only the four translation functions are public.

#include "orchestrator/input/keycodes.hpp"

#include <array>

namespace xorio_ui::orchestrator::input {

namespace {

// HID Usage Tables, page 0x07 (Keyboard/Keypad).
constexpr std::uint8_t HID_A = 0x04;
constexpr std::uint8_t HID_B = 0x05;
constexpr std::uint8_t HID_C = 0x06;
constexpr std::uint8_t HID_D = 0x07;
constexpr std::uint8_t HID_E = 0x08;
constexpr std::uint8_t HID_F = 0x09;
constexpr std::uint8_t HID_G = 0x0A;
constexpr std::uint8_t HID_H = 0x0B;
constexpr std::uint8_t HID_I = 0x0C;
constexpr std::uint8_t HID_J = 0x0D;
constexpr std::uint8_t HID_K = 0x0E;
constexpr std::uint8_t HID_L = 0x0F;
constexpr std::uint8_t HID_M = 0x10;
constexpr std::uint8_t HID_N = 0x11;
constexpr std::uint8_t HID_O = 0x12;
constexpr std::uint8_t HID_P = 0x13;
constexpr std::uint8_t HID_Q = 0x14;
constexpr std::uint8_t HID_R = 0x15;
constexpr std::uint8_t HID_S = 0x16;
constexpr std::uint8_t HID_T = 0x17;
constexpr std::uint8_t HID_U = 0x18;
constexpr std::uint8_t HID_V = 0x19;
constexpr std::uint8_t HID_W = 0x1A;
constexpr std::uint8_t HID_X = 0x1B;
constexpr std::uint8_t HID_Y = 0x1C;
constexpr std::uint8_t HID_Z = 0x1D;

constexpr std::uint8_t HID_1 = 0x1E;
constexpr std::uint8_t HID_2 = 0x1F;
constexpr std::uint8_t HID_3 = 0x20;
constexpr std::uint8_t HID_4 = 0x21;
constexpr std::uint8_t HID_5 = 0x22;
constexpr std::uint8_t HID_6 = 0x23;
constexpr std::uint8_t HID_7 = 0x24;
constexpr std::uint8_t HID_8 = 0x25;
constexpr std::uint8_t HID_9 = 0x26;
constexpr std::uint8_t HID_0 = 0x27;

constexpr std::uint8_t HID_ENTER     = 0x28;
constexpr std::uint8_t HID_ESCAPE    = 0x29;
constexpr std::uint8_t HID_BACKSPACE = 0x2A;
constexpr std::uint8_t HID_TAB       = 0x2B;
constexpr std::uint8_t HID_SPACE     = 0x2C;

constexpr std::uint8_t HID_MINUS         = 0x2D;
constexpr std::uint8_t HID_EQUAL         = 0x2E;
constexpr std::uint8_t HID_LBRACKET      = 0x2F;
constexpr std::uint8_t HID_RBRACKET      = 0x30;
constexpr std::uint8_t HID_BACKSLASH     = 0x31;
constexpr std::uint8_t HID_HASH          = 0x32;
constexpr std::uint8_t HID_SEMICOLON     = 0x33;
constexpr std::uint8_t HID_APOSTROPHE    = 0x34;
constexpr std::uint8_t HID_GRAVE         = 0x35;
constexpr std::uint8_t HID_COMMA         = 0x36;
constexpr std::uint8_t HID_PERIOD        = 0x37;
constexpr std::uint8_t HID_SLASH         = 0x38;

constexpr std::uint8_t HID_CAPS_LOCK = 0x39;

constexpr std::uint8_t HID_F1  = 0x3A;
constexpr std::uint8_t HID_F2  = 0x3B;
constexpr std::uint8_t HID_F3  = 0x3C;
constexpr std::uint8_t HID_F4  = 0x3D;
constexpr std::uint8_t HID_F5  = 0x3E;
constexpr std::uint8_t HID_F6  = 0x3F;
constexpr std::uint8_t HID_F7  = 0x40;
constexpr std::uint8_t HID_F8  = 0x41;
constexpr std::uint8_t HID_F9  = 0x42;
constexpr std::uint8_t HID_F10 = 0x43;
constexpr std::uint8_t HID_F11 = 0x44;
constexpr std::uint8_t HID_F12 = 0x45;

constexpr std::uint8_t HID_PRINT_SCREEN = 0x46;
constexpr std::uint8_t HID_SCROLL_LOCK  = 0x47;
constexpr std::uint8_t HID_PAUSE        = 0x48;

constexpr std::uint8_t HID_INSERT    = 0x49;
constexpr std::uint8_t HID_HOME      = 0x4A;
constexpr std::uint8_t HID_PAGE_UP   = 0x4B;
constexpr std::uint8_t HID_DELETE    = 0x4C;
constexpr std::uint8_t HID_END       = 0x4D;
constexpr std::uint8_t HID_PAGE_DOWN = 0x4E;
constexpr std::uint8_t HID_RIGHT     = 0x4F;
constexpr std::uint8_t HID_LEFT      = 0x50;
constexpr std::uint8_t HID_DOWN      = 0x51;
constexpr std::uint8_t HID_UP        = 0x52;

constexpr std::uint8_t HID_NUM_LOCK    = 0x53;
constexpr std::uint8_t HID_KP_DIVIDE   = 0x54;
constexpr std::uint8_t HID_KP_MULTIPLY = 0x55;
constexpr std::uint8_t HID_KP_MINUS    = 0x56;
constexpr std::uint8_t HID_KP_PLUS     = 0x57;
constexpr std::uint8_t HID_KP_ENTER    = 0x58;
constexpr std::uint8_t HID_KP_1        = 0x59;
constexpr std::uint8_t HID_KP_2        = 0x5A;
constexpr std::uint8_t HID_KP_3        = 0x5B;
constexpr std::uint8_t HID_KP_4        = 0x5C;
constexpr std::uint8_t HID_KP_5        = 0x5D;
constexpr std::uint8_t HID_KP_6        = 0x5E;
constexpr std::uint8_t HID_KP_7        = 0x5F;
constexpr std::uint8_t HID_KP_8        = 0x60;
constexpr std::uint8_t HID_KP_9        = 0x61;
constexpr std::uint8_t HID_KP_0        = 0x62;
constexpr std::uint8_t HID_KP_PERIOD   = 0x63;

constexpr std::uint8_t HID_NON_US_BACKSLASH = 0x64;
constexpr std::uint8_t HID_APPLICATION      = 0x65;

constexpr std::uint8_t HID_LEFT_CTRL   = 0xE0;
constexpr std::uint8_t HID_LEFT_SHIFT  = 0xE1;
constexpr std::uint8_t HID_LEFT_ALT    = 0xE2;
constexpr std::uint8_t HID_LEFT_GUI    = 0xE3;
constexpr std::uint8_t HID_RIGHT_CTRL  = 0xE4;
constexpr std::uint8_t HID_RIGHT_SHIFT = 0xE5;
constexpr std::uint8_t HID_RIGHT_ALT   = 0xE6;
constexpr std::uint8_t HID_RIGHT_GUI   = 0xE7;

// One pair (evdev, hid) = one entry. The init list is short
// enough to keep readable; the lookup tables are derived at
// program-start by walking it.
struct Pair {
    std::uint8_t native;
    std::uint8_t hid;
};

constexpr Pair kEvdevPairs[] = {
    {  1, HID_ESCAPE   }, {  2, HID_1        }, {  3, HID_2        },
    {  4, HID_3        }, {  5, HID_4        }, {  6, HID_5        },
    {  7, HID_6        }, {  8, HID_7        }, {  9, HID_8        },
    { 10, HID_9        }, { 11, HID_0        },
    { 12, HID_MINUS    }, { 13, HID_EQUAL    }, { 14, HID_BACKSPACE},
    { 15, HID_TAB      },
    { 16, HID_Q        }, { 17, HID_W        }, { 18, HID_E        },
    { 19, HID_R        }, { 20, HID_T        }, { 21, HID_Y        },
    { 22, HID_U        }, { 23, HID_I        }, { 24, HID_O        },
    { 25, HID_P        },
    { 26, HID_LBRACKET }, { 27, HID_RBRACKET }, { 28, HID_ENTER    },
    { 29, HID_LEFT_CTRL},
    { 30, HID_A        }, { 31, HID_S        }, { 32, HID_D        },
    { 33, HID_F        }, { 34, HID_G        }, { 35, HID_H        },
    { 36, HID_J        }, { 37, HID_K        }, { 38, HID_L        },
    { 39, HID_SEMICOLON}, { 40, HID_APOSTROPHE}, { 41, HID_GRAVE   },
    { 42, HID_LEFT_SHIFT}, { 43, HID_BACKSLASH },
    { 44, HID_Z        }, { 45, HID_X        }, { 46, HID_C        },
    { 47, HID_V        }, { 48, HID_B        }, { 49, HID_N        },
    { 50, HID_M        }, { 51, HID_COMMA    }, { 52, HID_PERIOD   },
    { 53, HID_SLASH    },
    { 54, HID_RIGHT_SHIFT}, { 55, HID_KP_MULTIPLY},
    { 56, HID_LEFT_ALT }, { 57, HID_SPACE    }, { 58, HID_CAPS_LOCK},
    { 59, HID_F1       }, { 60, HID_F2       }, { 61, HID_F3       },
    { 62, HID_F4       }, { 63, HID_F5       }, { 64, HID_F6       },
    { 65, HID_F7       }, { 66, HID_F8       }, { 67, HID_F9       },
    { 68, HID_F10      },
    { 69, HID_NUM_LOCK }, { 70, HID_SCROLL_LOCK},
    { 71, HID_KP_7     }, { 72, HID_KP_8     }, { 73, HID_KP_9     },
    { 74, HID_KP_MINUS },
    { 75, HID_KP_4     }, { 76, HID_KP_5     }, { 77, HID_KP_6     },
    { 78, HID_KP_PLUS  },
    { 79, HID_KP_1     }, { 80, HID_KP_2     }, { 81, HID_KP_3     },
    { 82, HID_KP_0     }, { 83, HID_KP_PERIOD},
    { 86, HID_NON_US_BACKSLASH },
    { 87, HID_F11      }, { 88, HID_F12      },
    { 96, HID_KP_ENTER }, { 97, HID_RIGHT_CTRL},
    { 98, HID_KP_DIVIDE}, { 99, HID_PRINT_SCREEN},
    {100, HID_RIGHT_ALT},
    {102, HID_HOME     }, {103, HID_UP       }, {104, HID_PAGE_UP  },
    {105, HID_LEFT     }, {106, HID_RIGHT    }, {107, HID_END      },
    {108, HID_DOWN     }, {109, HID_PAGE_DOWN}, {110, HID_INSERT   },
    {111, HID_DELETE   },
    {119, HID_PAUSE    },
    {125, HID_LEFT_GUI }, {126, HID_RIGHT_GUI}, {127, HID_APPLICATION},
};

constexpr Pair kVkPairs[] = {
    // Letters: VK_A=0x41 .. VK_Z=0x5A → HID_A .. HID_Z.
    {0x41, HID_A}, {0x42, HID_B}, {0x43, HID_C}, {0x44, HID_D},
    {0x45, HID_E}, {0x46, HID_F}, {0x47, HID_G}, {0x48, HID_H},
    {0x49, HID_I}, {0x4A, HID_J}, {0x4B, HID_K}, {0x4C, HID_L},
    {0x4D, HID_M}, {0x4E, HID_N}, {0x4F, HID_O}, {0x50, HID_P},
    {0x51, HID_Q}, {0x52, HID_R}, {0x53, HID_S}, {0x54, HID_T},
    {0x55, HID_U}, {0x56, HID_V}, {0x57, HID_W}, {0x58, HID_X},
    {0x59, HID_Y}, {0x5A, HID_Z},
    // Top-row digits: VK_0=0x30 .. VK_9=0x39.
    {0x30, HID_0}, {0x31, HID_1}, {0x32, HID_2}, {0x33, HID_3},
    {0x34, HID_4}, {0x35, HID_5}, {0x36, HID_6}, {0x37, HID_7},
    {0x38, HID_8}, {0x39, HID_9},
    // Function keys.
    {0x70, HID_F1 }, {0x71, HID_F2 }, {0x72, HID_F3 }, {0x73, HID_F4 },
    {0x74, HID_F5 }, {0x75, HID_F6 }, {0x76, HID_F7 }, {0x77, HID_F8 },
    {0x78, HID_F9 }, {0x79, HID_F10}, {0x7A, HID_F11}, {0x7B, HID_F12},
    // Modifiers — distinguished L/R variants (the LL hook delivers
    // these reliably; RawInput collapses them to VK_SHIFT etc.,
    // but the orchestrator's source on Windows is the LL hook).
    {0xA0, HID_LEFT_SHIFT }, {0xA1, HID_RIGHT_SHIFT},
    {0xA2, HID_LEFT_CTRL  }, {0xA3, HID_RIGHT_CTRL },
    {0xA4, HID_LEFT_ALT   }, {0xA5, HID_RIGHT_ALT  },
    {0x5B, HID_LEFT_GUI   }, {0x5C, HID_RIGHT_GUI  },
    // Navigation.
    {0x25, HID_LEFT     }, {0x26, HID_UP       }, {0x27, HID_RIGHT  },
    {0x28, HID_DOWN     },
    {0x24, HID_HOME     }, {0x23, HID_END      }, {0x21, HID_PAGE_UP},
    {0x22, HID_PAGE_DOWN},
    {0x2D, HID_INSERT   }, {0x2E, HID_DELETE   },
    // Editing / control.
    {0x0D, HID_ENTER    }, {0x1B, HID_ESCAPE   }, {0x08, HID_BACKSPACE},
    {0x09, HID_TAB      }, {0x20, HID_SPACE    },
    {0x14, HID_CAPS_LOCK}, {0x90, HID_NUM_LOCK }, {0x91, HID_SCROLL_LOCK},
    {0x2C, HID_PRINT_SCREEN}, {0x13, HID_PAUSE  },
    {0x5D, HID_APPLICATION},
    // Symbols (VK_OEM_*).
    {0xBD, HID_MINUS    }, {0xBB, HID_EQUAL    },
    {0xDB, HID_LBRACKET }, {0xDD, HID_RBRACKET },
    {0xDC, HID_BACKSLASH}, {0xBA, HID_SEMICOLON},
    {0xDE, HID_APOSTROPHE}, {0xC0, HID_GRAVE   },
    {0xBC, HID_COMMA    }, {0xBE, HID_PERIOD   }, {0xBF, HID_SLASH},
    // Numpad.
    {0x60, HID_KP_0}, {0x61, HID_KP_1}, {0x62, HID_KP_2}, {0x63, HID_KP_3},
    {0x64, HID_KP_4}, {0x65, HID_KP_5}, {0x66, HID_KP_6}, {0x67, HID_KP_7},
    {0x68, HID_KP_8}, {0x69, HID_KP_9},
    {0x6E, HID_KP_PERIOD  }, {0x6A, HID_KP_MULTIPLY},
    {0x6B, HID_KP_PLUS    }, {0x6D, HID_KP_MINUS   },
    {0x6F, HID_KP_DIVIDE  },
};

// Build forward + reverse lookup tables from a pair list at
// program start. Both axes fit in 256 entries (evdev caps at
// ~127 for keys we care about, VK at 0xFE, HID at 0xE7).
constexpr std::size_t kTableSize = 256;
using Table = std::array<std::uint8_t, kTableSize>;

template <std::size_t N>
Table build_native_to_hid(const Pair (&pairs)[N]) {
    Table t{};  // zero-initialised — 0 is the "unknown" sentinel.
    for (const auto& p : pairs) t[p.native] = p.hid;
    return t;
}

template <std::size_t N>
Table build_hid_to_native(const Pair (&pairs)[N]) {
    Table t{};
    for (const auto& p : pairs) t[p.hid] = p.native;
    return t;
}

const Table kEvdevToHid = build_native_to_hid(kEvdevPairs);
const Table kHidToEvdev = build_hid_to_native(kEvdevPairs);
const Table kVkToHid    = build_native_to_hid(kVkPairs);
const Table kHidToVk    = build_hid_to_native(kVkPairs);

inline std::uint32_t lookup(const Table& t, std::uint32_t k) {
    return k < kTableSize ? t[k] : 0u;
}

}  // namespace

std::uint32_t evdev_to_hid(std::uint32_t evdev) {
    return lookup(kEvdevToHid, evdev);
}

std::uint32_t hid_to_evdev(std::uint32_t hid) {
    return lookup(kHidToEvdev, hid);
}

std::uint32_t vk_to_hid(std::uint32_t vk) {
    return lookup(kVkToHid, vk);
}

std::uint32_t hid_to_vk(std::uint32_t hid) {
    return lookup(kHidToVk, hid);
}

}  // namespace xorio_ui::orchestrator::input

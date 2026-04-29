/// @file keycodes.hpp
/// @brief Bidirectional translation between platform-native key
/// codes and USB HID usage IDs (the wire format).
///
/// Scope: pure lookup tables. Each function returns 0 for codes
/// that aren't in the canonical Keyboard/Keypad page (0x07) of
/// the HID Usage Tables — the caller drops those events rather
/// than guessing.
///
/// Tables are ported from `unio/backends/keycodes.py` so the C++
/// runtime and the Python tree share the exact same code-set
/// translations.
#pragma once

#include <cstdint>

namespace unio_ui::orchestrator::input {

/// @brief Linux evdev scancode → HID usage ID. 0 if unknown.
std::uint32_t evdev_to_hid(std::uint32_t evdev);

/// @brief HID usage ID → Linux evdev scancode. 0 if unknown.
std::uint32_t hid_to_evdev(std::uint32_t hid);

/// @brief Windows virtual-key code → HID usage ID. 0 if unknown.
std::uint32_t vk_to_hid(std::uint32_t vk);

/// @brief HID usage ID → Windows virtual-key code. 0 if unknown.
std::uint32_t hid_to_vk(std::uint32_t hid);

}  // namespace unio_ui::orchestrator::input

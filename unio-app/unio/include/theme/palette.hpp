/// @file palette.hpp
/// @brief Named colour tokens used across the UI.
#pragma once

#include "imgui.h"

namespace unio_ui::theme {

/// @brief Build an @c ImVec4 from a packed 24-bit RGB value.
/// @param rgb  `0xRRGGBB`.
/// @param a    Alpha in the range 0..1.
constexpr ImVec4 rgba_hex(unsigned int rgb, float a = 1.0f) {
    return ImVec4(
        ((rgb >> 16) & 0xFFu) / 255.0f,
        ((rgb >>  8) & 0xFFu) / 255.0f,
        ((rgb      ) & 0xFFu) / 255.0f,
        a);
}

/// @brief Named colour tokens.
namespace palette {

inline constexpr ImVec4 paper_bg        = rgba_hex(0xffffff);
inline constexpr ImVec4 paper_surface   = rgba_hex(0xf4f5f8);
inline constexpr ImVec4 paper_rail      = rgba_hex(0xeef0f5);
inline constexpr ImVec4 paper_rail_deep = rgba_hex(0xc9cfd9);
inline constexpr ImVec4 paper_border    = rgba_hex(0xe2e4ec);
inline constexpr ImVec4 paper_text      = rgba_hex(0x1f2024);
inline constexpr ImVec4 paper_muted     = rgba_hex(0x6d7286);
inline constexpr ImVec4 paper_faint     = rgba_hex(0x9a9db0);

inline constexpr ImVec4 lilac       = rgba_hex(0xa050f0);  ///< Primary accent.
inline constexpr ImVec4 lilac_hover = rgba_hex(0x8a3fd4);
inline constexpr ImVec4 lilac_soft  = rgba_hex(0xf3e7fe);
inline constexpr ImVec4 mint        = rgba_hex(0x5cc9a3);
inline constexpr ImVec4 amber       = rgba_hex(0xe8b04c);
inline constexpr ImVec4 coral       = rgba_hex(0xff6b5b);

}  // namespace palette
}  // namespace unio_ui::theme

/// @file palette.hpp
/// @brief Named colour tokens used across the UI.
#pragma once

#include "imgui.h"

namespace xorio::theme {

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
inline constexpr ImVec4 paper_rail_top  = rgba_hex(0xf1ebf9);  ///< Lilac at rail head.
inline constexpr ImVec4 paper_rail_bot  = rgba_hex(0xe8dcf6);  ///< Lilac at rail foot.
// Intermediate gradient stops aligned with the rail nav buttons —
// produces a piecewise-linear vertical gradient that hits each
// listed colour at the button's centre.
inline constexpr ImVec4 paper_rail_workspaces = rgba_hex(0xf7f3fb);
inline constexpr ImVec4 paper_rail_layout   = rgba_hex(0xf7f3fb);
inline constexpr ImVec4 paper_rail_settings = rgba_hex(0xf8f3fa);
inline constexpr ImVec4 paper_rail_access   = rgba_hex(0xe6d9f5);
inline constexpr ImVec4 paper_rail_help     = rgba_hex(0xe7daf6);
inline constexpr ImVec4 paper_rail_edge   = rgba_hex(0xe0d5ef);  ///< Lilac stripe along the rail's right edge.
inline constexpr ImVec4 paper_rail_active = rgba_hex(0xede3f8);  ///< Background pill for the selected nav button.
inline constexpr ImVec4 paper_widget_bg     = rgba_hex(0xfefefe);  ///< Activity-tab widget card background.
inline constexpr ImVec4 paper_widget_border = rgba_hex(0xf1f1f4);  ///< Activity-tab widget card outline.
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
}  // namespace xorio::theme

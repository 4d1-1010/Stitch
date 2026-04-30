/// @file metrics.hpp
/// @brief Spacing + corner-radius tokens in pixels.
#pragma once

namespace xorio::theme {

/// @brief Horizontal / vertical spacing tokens in pixels.
namespace space {
inline constexpr float xs =  4.0f;
inline constexpr float sm =  8.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 18.0f;
inline constexpr float xl = 28.0f;

/// @brief Page-level edges — distance from the rail's right edge
/// to the start of any tab's content (`page_x`) and from the
/// content's top to its first widget (`page_y`). Matched to the
/// Python tree's `SPACE_LG` so the C++ port reads with the same
/// rhythm. Used by `shell.cpp::render_content` via Indent + Dummy
/// (rather than WindowPadding) so the inset is unambiguous
/// regardless of how ImGui's child windows resolve padding.
inline constexpr float page_x = 18.0f;
inline constexpr float page_y = 14.0f;
}  // namespace space

/// @brief Corner-radius tokens in pixels.
namespace radius {
inline constexpr float sm =  6.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 18.0f;
}  // namespace radius

}  // namespace xorio::theme

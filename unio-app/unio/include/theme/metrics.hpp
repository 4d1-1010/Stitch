/// @file metrics.hpp
/// @brief Spacing + corner-radius tokens in pixels.
#pragma once

namespace unio_ui::theme {

/// @brief Horizontal / vertical spacing tokens in pixels.
namespace space {
inline constexpr float xs =  4.0f;
inline constexpr float sm =  8.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 18.0f;
inline constexpr float xl = 28.0f;
}  // namespace space

/// @brief Corner-radius tokens in pixels.
namespace radius {
inline constexpr float sm =  6.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 18.0f;
}  // namespace radius

}  // namespace unio_ui::theme

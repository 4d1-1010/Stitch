/// @file typography.hpp
/// @brief Font size tokens and lazily-populated @c ImFont* handles.
#pragma once

#include "imgui.h"

namespace unio_ui::theme::font {

inline constexpr float size_xs    = 11.0f;
inline constexpr float size_sm    = 13.0f;
inline constexpr float size_base  = 14.0f;
inline constexpr float size_lg    = 16.0f;
inline constexpr float size_xl    = 20.0f;
inline constexpr float size_title = 28.0f;

/// @name Font handles
/// Populated by @ref unio_ui::theme::load_fonts. @c nullptr before first load.
/// @{
extern ImFont* body;
extern ImFont* body_sm;
extern ImFont* body_xs;
extern ImFont* body_lg;
extern ImFont* bold;
extern ImFont* bold_xs;
extern ImFont* bold_xl;
extern ImFont* title;
/// @}

}  // namespace unio_ui::theme::font

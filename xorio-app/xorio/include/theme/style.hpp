/// @file style.hpp
/// @brief Atlas + ImGui-style initialisation entry points.
#pragma once

namespace xorio_ui::theme {

/// @brief Load the embedded Inter faces into the ImGui atlas.
/// Call once after @c ImGui::CreateContext() and before the
/// renderer backend init.
void load_fonts();

/// @brief Apply the palette + rounding tokens to the active ImGui style.
/// Call once after @ref load_fonts.
void apply_style();

}  // namespace xorio_ui::theme

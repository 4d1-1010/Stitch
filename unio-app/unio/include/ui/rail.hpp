/// @file rail.hpp
/// @brief Left-rail navigation button + vector icons.
#pragma once

namespace unio_ui::ui {

/// @brief Vector icon kind drawn inside a @ref rail_button.
enum class RailIcon {
    Activity,
    Layout,
    Settings,
    Access,
    Help,
};

/// @brief Rail navigation entry: icon above label.
/// @param id      Unique ImGui ID within the rail.
/// @param icon    Vector glyph to draw.
/// @param label   Caption under the icon.
/// @param active  Selected state (lilac tint when @c true).
/// @return @c true when clicked.
bool rail_button(const char* id, RailIcon icon,
                 const char* label, bool active);

}  // namespace unio_ui::ui

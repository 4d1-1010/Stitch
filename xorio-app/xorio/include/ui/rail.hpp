/// @file rail.hpp
/// @brief Left-rail navigation button + vector icons.
#pragma once

namespace xorio::ui {

/// @brief Vector icon kind drawn inside a @ref rail_button.
enum class RailIcon {
    Workspaces,
    Layout,
    LiveView,
    Settings,
    Access,
    Help,
};

/// @brief Rail dimensions shared between the shell and the button.
inline constexpr float kRailWidth        = 168.0f;
inline constexpr float kRailButtonHeight = 52.0f;

/// @brief Rail navigation entry: icon on the left, label on the right.
/// @param id      Unique ImGui ID within the rail.
/// @param icon    Vector glyph to draw.
/// @param label   Caption to the right of the icon.
/// @param active  Selected state (lilac tint + bg pill).
/// @param badge   Draw an attention dot on the icon (e.g. when a
///                workspace has an unanswered alone-online prompt).
/// @return @c true when clicked.
bool rail_button(const char* id, RailIcon icon,
                 const char* label, bool active,
                 bool badge = false);

}  // namespace xorio::ui

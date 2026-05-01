/// @file primitives.hpp
/// @brief Reusable widget primitives (buttons, dots, rules, cards).
#pragma once

#include "imgui.h"
#include "theme/metrics.hpp"

namespace xorio::ui {

/// @brief Semantic variants of @ref pill_button.
enum class PillVariant {
    Primary,
    Secondary,
    Ghost,
    Danger,
};

/// @brief Flat rounded button with semantic variants.
/// @param label          UTF-8 caption.
/// @param variant        Visual style.
/// @param size_override  Font size override in px (0 = body).
/// @return @c true when clicked.
bool pill_button(const char* label,
                 PillVariant variant = PillVariant::Primary,
                 float size_override = 0.0f);

/// @brief Semantic state for @ref status_dot.
enum class DotState {
    Ok,
    Warn,
    Bad,
    Idle,
};

/// @brief Filled circle indicator.
/// @param state  Semantic fill colour.
/// @param size   Diameter in pixels.
void status_dot(DotState state, float size = 10.0f);

/// @brief 1-pixel separator.
/// @param axis  `'x'` horizontal, `'y'` vertical.
void hairline(char axis = 'x');

/// @brief Open a paper-surface card.
/// @param id      Unique ImGui ID.
/// @param size    Card dimensions (0, 0 = auto).
/// @param accent  Optional left-edge accent strip colour.
/// @param pad     Interior padding in pixels.
void begin_card(const char* id, ImVec2 size = ImVec2(0, 0),
                const ImVec4* accent = nullptr,
                float pad = theme::space::lg);

/// @brief Close the card opened by @ref begin_card.
void end_card();

/// @brief Padlock-icon button used as a drop-in replacement for
/// the Edit pill on workspace cards when the local PC isn't
/// allowed to edit. The button itself is non-actionable — it
/// exists only to host a hover tooltip explaining the lock
/// state ("Locked — members only" / "Master-locked by …").
/// @param id_suffix  Per-row tag mixed into the ImGui ID.
/// @param tooltip    Plain-text tooltip shown on hover.
void lock_icon_button(const char* id_suffix, const char* tooltip);

}  // namespace xorio::ui

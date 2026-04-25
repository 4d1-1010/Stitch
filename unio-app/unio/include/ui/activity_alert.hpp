/// @file activity_alert.hpp
/// @brief Inline banner shown at the top of the Activity tab after
/// an action (workspace created / saved / deleted, etc).
///
/// Scope: pure rendering primitive. State (level + text) is owned
/// by the calling screen; this TU never holds it. Dismissing the
/// banner mutates the caller's optional via the supplied reference.
#pragma once

#include <optional>
#include <string>

namespace unio_ui::ui::alert {

/// @brief Visual variant of an alert.
enum class Level {
    Info,    ///< Lilac wash, lilac text. Default.
    Warn,    ///< Coral wash, coral text.
};

/// @brief A single ephemeral alert.
struct Banner {
    Level       level = Level::Info;
    std::string text;
};

/// @brief Render @p banner if it carries a value, with a close
/// button that clears the optional in-place.
///
/// No-op when @p banner is empty — safe to call unconditionally
/// from the screen body.
void render(std::optional<Banner>& banner);

}  // namespace unio_ui::ui::alert

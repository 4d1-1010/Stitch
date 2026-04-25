/// @file status_bar.hpp
/// @brief Persistent bottom-of-window status bar for transient
/// notifications.
///
/// Scope: a single process-wide notification surface. Any UI code
/// can `post()` a message; the shell renders one bar per frame at
/// the bottom of the viewport. Messages auto-fade after a short
/// dwell so the bar never grows unbounded.
///
/// Single instance — state lives in `src/ui/status_bar.cpp` as
/// translation-unit-local data behind a mutex. The interface is
/// intentionally narrow so the implementation can grow (queues,
/// per-message ids, action buttons) without rippling through every
/// call site.
#pragma once

#include <string>

namespace unio_ui::ui::status {

/// @brief Visual variant of a status message.
enum class Level {
    Info,    ///< Lilac wash; default for confirmations.
    Warn,    ///< Coral wash; surfaces destructive actions / errors.
};

/// @brief Push a message into the bar. Thread-safe; replaces any
/// in-flight message so the bar always reflects the most recent
/// action.
void post(Level level, std::string text);

/// @brief Draw the bar at the bottom of the current ImGui viewport.
/// Must be called from the UI thread, after every other widget so
/// the bar lays out on top.
///
/// Auto-fades the current message after a short dwell — the bar
/// renders empty (still occupying its row) once the dwell elapses,
/// keeping the layout stable.
void render();

}  // namespace unio_ui::ui::status

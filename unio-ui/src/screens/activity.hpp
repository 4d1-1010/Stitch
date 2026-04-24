/*! @file activity.hpp
 *  @brief Activity tab — first screen users see.
 *
 *  Port of `unio/apps/shell.py`'s `_build_activity_tab` +
 *  `_activity_alone_state` / `_activity_running`. First cut: just
 *  the alone-state welcome. The running-state (workspace cards,
 *  PC tiles, create / edit / delete flows) depends on the
 *  orchestrator stub (task #79) and lands later.
 */
#pragma once

namespace unio_ui::screens::activity {

/// Render the Activity tab body into the current ImGui content
/// region. Called from `Shell::render_content` when the tab is
/// active.
void render();

}  // namespace unio_ui::screens::activity

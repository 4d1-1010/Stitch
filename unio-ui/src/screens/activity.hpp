/*! @file activity.hpp
 *  @brief Activity tab — first screen users see.
 *
 *  Port of `unio/apps/shell.py`'s `_build_activity_tab` +
 *  `_activity_alone_state` / `_activity_running`. The full
 *  running-state (workspace cards) lands with the real
 *  orchestrator impl; the stub returns two demo peers so the
 *  list-view renders something.
 */
#pragma once

namespace unio_ui::orchestrator { class IOrchestrator; }

namespace unio_ui::screens::activity {

/// Render the Activity tab body. Called from
/// `Shell::render_content` when the tab is active.
void render(orchestrator::IOrchestrator& orch);

}  // namespace unio_ui::screens::activity

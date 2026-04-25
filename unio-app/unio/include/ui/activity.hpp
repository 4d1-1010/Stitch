/// @file activity.hpp
/// @brief Activity-tab body.
#pragma once

namespace unio_ui::orchestrator { class IOrchestrator; }

namespace unio_ui::ui::activity {

/// @brief Render the Activity tab body.
/// @param orch  Orchestrator consulted for peers + auth state.
void render(orchestrator::IOrchestrator& orch);

}  // namespace unio_ui::ui::activity

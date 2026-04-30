/// @file layout.hpp
/// @brief Two-row layout canvas: PCs above, displays below.
#pragma once

namespace xorio_ui::orchestrator { class IOrchestrator; }

namespace xorio_ui::ui::layout {

/// @brief Render the Layout tab body.
/// @param orch  Orchestrator consulted for the display set.
void render(orchestrator::IOrchestrator& orch);

}  // namespace xorio_ui::ui::layout

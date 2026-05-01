/// @file layout.hpp
/// @brief Two-row layout canvas: PCs above, displays below.
#pragma once

namespace xorio::orchestrator { class IOrchestrator; }

namespace xorio::ui::layout {

/// @brief Render the Layout tab body.
/// @param orch  Orchestrator consulted for the display set.
void render(orchestrator::IOrchestrator& orch);

}  // namespace xorio::ui::layout

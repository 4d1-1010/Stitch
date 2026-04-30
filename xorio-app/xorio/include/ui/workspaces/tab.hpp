/// @file tab.hpp
/// @brief Workspaces-tab body — the entry point Shell calls when
/// the rail's Workspaces tab is active.
#pragma once

namespace xorio::orchestrator { class IOrchestrator; }

namespace xorio::ui::workspaces {

/// @brief Render the Workspaces tab body. Disambiguated from the
/// inner manager dispatcher (@ref render(orchestrator&, ViewState&))
/// by name so both can live in the same namespace without
/// overload confusion.
/// @param orch  Orchestrator consulted for peers + auth state.
void render_tab(orchestrator::IOrchestrator& orch);

}  // namespace xorio::ui::workspaces

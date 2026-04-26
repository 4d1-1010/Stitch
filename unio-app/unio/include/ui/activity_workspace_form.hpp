/// @file activity_workspace_form.hpp
/// @brief Inline Create / Edit form for the Activity-tab workspaces
/// section.
///
/// Scope: the form sub-view only — the cards list, header, and
/// section dispatch live in @ref activity_workspaces.hpp. Mutates
/// the same @ref ViewState the cards view uses, so the caller can
/// pivot between list and form without losing scratch state.
#pragma once

#include "ui/activity_workspaces.hpp"

namespace unio_ui::orchestrator {
class  IOrchestrator;
struct Workspace;
}  // namespace unio_ui::orchestrator

namespace unio_ui::ui::workspaces {

/// @brief Open the Create form with a default name + no members.
void start_create(ViewState& state);

/// @brief Open the Edit form populated from @p ws.
void start_edit(const orchestrator::Workspace& ws, ViewState& state);

/// @brief Render the Create / Edit form.
/// @return @c true once the form closed (Save / Cancel / Delete) so
/// the caller can flip back to list mode.
bool render_form(orchestrator::IOrchestrator& orch, ViewState& state);

}  // namespace unio_ui::ui::workspaces

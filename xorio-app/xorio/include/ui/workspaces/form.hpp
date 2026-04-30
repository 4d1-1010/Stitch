/// @file form.hpp
/// @brief Inline Create / Edit form for the Workspaces-tab
/// manager, plus the Add / remove computers member-picker view
/// pushed over the form.
///
/// Scope: the form + picker sub-views only — the cards list,
/// header, and manager dispatch live in @ref list.hpp. Mutates
/// the same @ref ViewState the cards view uses, so the caller
/// can pivot between list and form without losing scratch
/// state.
#pragma once

#include "ui/workspaces/list.hpp"

namespace xorio::orchestrator {
class  IOrchestrator;
struct Workspace;
}  // namespace xorio::orchestrator

namespace xorio::ui::workspaces {

/// @brief Open the Create form with a default name + no members.
void start_create(ViewState& state);

/// @brief Open the Edit form populated from @p ws.
void start_edit(const orchestrator::Workspace& ws, ViewState& state);

/// @brief Render the Create / Edit form.
/// @return @c true once the form closed (Save / Cancel / Delete) so
/// the caller can flip back to list mode.
bool render_form(orchestrator::IOrchestrator& orch, ViewState& state);

/// @brief Render the "Add / remove computers" picker as a full-
/// page navigation push over the form. The host (activity.cpp)
/// owns the back-arrow + title; this function only paints the
/// scrollable peer-checkbox list inside the manager card body.
/// Toggling a checkbox immediately mutates `state.form_members`
/// (and the cap sets) so the form view sees the change the
/// moment the user navigates back.
void render_member_picker(orchestrator::IOrchestrator& orch,
                           ViewState& state);

}  // namespace xorio::ui::workspaces

/// @file activity_workspaces.hpp
/// @brief Workspaces section of the Activity tab.
///
/// Scope: render the workspaces sub-region. Mutates a caller-owned
/// @ref ViewState to track which mode (list / create / edit) the
/// section is in. Pushes alerts into a caller-owned banner when an
/// action completes.
///
/// Owned by `src/ui/activity_workspaces.cpp` — workspace data
/// model lives in `orchestrator/workspace.hpp`.
#pragma once

#include "ui/activity_alert.hpp"

#include <array>
#include <optional>
#include <string>
#include <unordered_set>

namespace unio_ui::orchestrator { class IOrchestrator; }

namespace unio_ui::ui::workspaces {

/// @brief Which view the section is currently rendering.
enum class Mode {
    List,    ///< Default: header + cards + "+ Create" button.
    Create,  ///< Inline form for a new workspace.
    Edit,    ///< Inline form populated from an existing workspace.
};

/// @brief Persistent state across frames.
///
/// Owned by the Activity tab so view + form-buffer + member
/// selection survive between renders. Reset to defaults whenever
/// @ref reset() is called.
struct ViewState {
    Mode                            mode = Mode::List;

    /// @brief Valid only when @c mode == Mode::Edit. Empty otherwise.
    std::string                     editing_id;

    /// @brief Workspace name text-input buffer (form scratch).
    std::array<char, 96>            name_buffer{};

    /// @brief Selected machine_ids while the form is open.
    std::unordered_set<std::string> form_members;

    /// @brief Reset every transient field — used when leaving a
    /// form back to the list view.
    void reset() {
        mode = Mode::List;
        editing_id.clear();
        name_buffer.fill(0);
        form_members.clear();
    }
};

/// @brief Render the workspaces section.
/// @param orch    Façade queried for peers + workspaces.
/// @param banner  Caller-owned slot the section pushes alerts into.
/// @param state   Caller-owned view state (persisted across frames).
void render(orchestrator::IOrchestrator& orch,
            std::optional<alert::Banner>& banner,
            ViewState& state);

}  // namespace unio_ui::ui::workspaces

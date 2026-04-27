/// @file activity_workspaces.hpp
/// @brief Workspaces section of the Activity tab.
///
/// Scope: render the workspaces sub-region. Mutates a caller-owned
/// @ref ViewState to track which mode (list / create / edit) the
/// section is in. Action results surface through the global
/// status bar (see @ref status::post).
///
/// Owned by `src/ui/activity_workspaces.cpp` — workspace data
/// model lives in `orchestrator/workspace.hpp`.
#pragma once

#include <array>
#include <cstdint>
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

    /// @brief Members of the workspace while the form is open
    /// (source of truth for the Member checkbox column).
    std::unordered_set<std::string> form_members;
    /// @brief Per-member capability sets — always subsets of
    /// @ref form_members. Toggling a Member off automatically
    /// drops the PC from these too.
    std::unordered_set<std::string> form_input_members;
    std::unordered_set<std::string> form_keyboard_members;
    std::unordered_set<std::string> form_clipboard_members;

    // ── Settings buffers (mirror the Workspace struct) ─────────
    int          clipboard_max          = 1;     ///< Index 0..4 in ClipboardLimit.
    bool         clipboard_rich         = true;
    bool         clipboard_files        = false;
    int          cursor_edge_margin     = 4;
    bool         cursor_require_modifier = false;
    bool         cursor_block_hotkeys   = false;
    int          auto_unlock            = 0;     ///< Index 0..3 in AutoUnlock.

    // ── Snapshot at seed time ──────────────────────────────────
    // Mirrors the buffers above; re-seeded by start_create /
    // start_edit. Save uses (form != snapshot) as the dirty
    // gate so an unchanged workspace can't be re-saved (and
    // an Edit form's Save button only "lights up" when the
    // user actually changed something).
    std::string                     baseline_name;
    std::unordered_set<std::string> baseline_members;
    std::unordered_set<std::string> baseline_input_members;
    std::unordered_set<std::string> baseline_keyboard_members;
    std::unordered_set<std::string> baseline_clipboard_members;
    int          baseline_clipboard_max          = 1;
    bool         baseline_clipboard_rich         = true;
    bool         baseline_clipboard_files        = false;
    int          baseline_cursor_edge_margin     = 4;
    bool         baseline_cursor_require_modifier = false;
    bool         baseline_cursor_block_hotkeys   = false;
    int          baseline_auto_unlock            = 0;

    /// @brief Reset every transient field — used when leaving a
    /// form back to the list view.
    void reset() {
        mode = Mode::List;
        editing_id.clear();
        name_buffer.fill(0);
        form_members.clear();
        form_input_members.clear();
        form_keyboard_members.clear();
        form_clipboard_members.clear();
        clipboard_max = 1;
        clipboard_rich = true;
        clipboard_files = false;
        cursor_edge_margin = 4;
        cursor_require_modifier = false;
        cursor_block_hotkeys = false;
        auto_unlock = 0;
        baseline_name.clear();
        baseline_members.clear();
        baseline_input_members.clear();
        baseline_keyboard_members.clear();
        baseline_clipboard_members.clear();
        baseline_clipboard_max          = 1;
        baseline_clipboard_rich         = true;
        baseline_clipboard_files        = false;
        baseline_cursor_edge_margin     = 4;
        baseline_cursor_require_modifier = false;
        baseline_cursor_block_hotkeys   = false;
        baseline_auto_unlock            = 0;
    }
};

/// @brief Render the workspaces section.
/// @param orch   Façade queried for peers + workspaces.
/// @param state  Caller-owned view state (persisted across frames).
/// @return @c true when the host should leave manager mode (the
///         user clicked Delete on a workspace, so there's nothing
///         meaningful to keep showing in the manager). The host
///         flips its `ws_manager_open` to false in response.
bool render(orchestrator::IOrchestrator& orch,
            ViewState& state);

}  // namespace unio_ui::ui::workspaces

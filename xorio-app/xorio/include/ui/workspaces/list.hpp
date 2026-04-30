/// @file list.hpp
/// @brief Workspaces-tab manager: cards list dispatcher.
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xorio::orchestrator {
struct Peer;
struct Workspace;
class IOrchestrator;
}  // namespace xorio::orchestrator

namespace xorio::ui::workspaces {

/// @brief Which view the section is currently rendering.
enum class Mode {
    List,    ///< Default: header + cards + "+ Create" button.
    Create,  ///< Inline form for a new workspace.
    Edit,    ///< Inline form populated from an existing workspace.
};

/// @brief Persistent state across frames.
///
/// Owned by the Workspaces tab so view + form-buffer + member
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
    /// drops the PC from these too. Input covers both cursor
    /// and keyboard sharing (one checkbox in the form).
    std::unordered_set<std::string> form_input_members;
    std::unordered_set<std::string> form_clipboard_members;

    // ── Settings buffers (mirror the Workspace struct) ─────────
    int          clipboard_max          = 1;     ///< Index 0..4 in ClipboardLimit.
    bool         clipboard_rich         = true;
    bool         clipboard_files        = false;
    int          cursor_edge_margin     = 4;
    bool         cursor_require_modifier = false;
    bool         cursor_block_hotkeys   = false;
    bool         locked                 = false;
    int          lock_unlock_after_h    = 0;
    bool         master_locked          = false;
    int          master_lock_unlock_after_h = 0;

    // ── Snapshot at seed time ──────────────────────────────────
    // Mirrors the buffers above; re-seeded by start_create /
    // start_edit. Save uses (form != snapshot) as the dirty
    // gate so an unchanged workspace can't be re-saved (and
    // an Edit form's Save button only "lights up" when the
    // user actually changed something).
    std::string                     baseline_name;
    std::unordered_set<std::string> baseline_members;
    std::unordered_set<std::string> baseline_input_members;
    std::unordered_set<std::string> baseline_clipboard_members;
    int          baseline_clipboard_max          = 1;
    bool         baseline_clipboard_rich         = true;
    bool         baseline_clipboard_files        = false;
    int          baseline_cursor_edge_margin     = 4;
    bool         baseline_cursor_require_modifier = false;
    bool         baseline_cursor_block_hotkeys   = false;
    bool         baseline_locked                 = false;
    int          baseline_lock_unlock_after_h    = 0;
    bool         baseline_master_locked          = false;
    int          baseline_master_lock_unlock_after_h = 0;

    /// @brief When true, the form's "Add / remove computers"
    /// picker is showing as a full-page navigation push over the
    /// form. The Activity manager treats it like a sibling view:
    /// title becomes "Add / remove computers", back arrow closes
    /// only the picker (returns to the form). Reset on form open
    /// so a re-entry never inherits a stale picker state.
    bool         show_member_picker              = false;

    /// @brief Display names of PCs that were sitting in this
    /// form's member buffer when a remote peer's Save claimed
    /// them for a different workspace. Drives the orange "X was
    /// removed — added to another workspace" banner at the top of
    /// the form. Cleared on form open / close; appended each
    /// frame the per-PC "is now elsewhere" check fires.
    std::vector<std::string> stolen_member_names;


    /// @brief Reset every transient field — used when leaving a
    /// form back to the list view.
    void reset() {
        mode = Mode::List;
        editing_id.clear();
        name_buffer.fill(0);
        form_members.clear();
        form_input_members.clear();
        form_clipboard_members.clear();
        clipboard_max = 1;
        clipboard_rich = true;
        clipboard_files = false;
        cursor_edge_margin = 4;
        cursor_require_modifier = false;
        cursor_block_hotkeys = false;
        locked = false;
        lock_unlock_after_h = 0;
        master_locked = false;
        master_lock_unlock_after_h = 0;
        baseline_name.clear();
        baseline_members.clear();
        baseline_input_members.clear();
        baseline_clipboard_members.clear();
        baseline_clipboard_max          = 1;
        baseline_clipboard_rich         = true;
        baseline_clipboard_files        = false;
        baseline_cursor_edge_margin     = 4;
        baseline_cursor_require_modifier = false;
        baseline_cursor_block_hotkeys   = false;
        baseline_locked                 = false;
        baseline_lock_unlock_after_h    = 0;
        baseline_master_locked          = false;
        baseline_master_lock_unlock_after_h = 0;
        show_member_picker              = false;
        stolen_member_names.clear();
    }
};

/// @brief Render the workspaces manager view (list dispatcher
/// to either the cards list or the inline Create / Edit form).
/// Disambiguated from @ref render_tab by name so the tab entry
/// and the inner manager dispatcher can both live in the same
/// `xorio::ui::workspaces` namespace without overload
/// confusion.
/// @param orch   Façade queried for peers + workspaces.
/// @param state  Caller-owned view state (persisted across frames).
/// @return @c true when the host should leave manager mode (the
///         user clicked Delete on a workspace, so there's nothing
///         meaningful to keep showing in the manager). The host
///         flips its `ws_manager_open` to false in response.
bool render_manager(orchestrator::IOrchestrator& orch,
                     ViewState& state);

/// @brief Build the hover tooltip for a locked workspace's lock
/// icon. Distinguishes Lock ("Locked — members only") from
/// Master-Lock ("Master-locked by <peer name>", with the peer
/// resolved via @p peer_index when known and falling back to
/// the raw machine_id otherwise). Empty string when the
/// workspace isn't locked.
std::string lock_tooltip(
    const orchestrator::Workspace& ws,
    const std::unordered_map<std::string, orchestrator::Peer>& peer_index);

/// @brief Render one peer-tile row inside a workspace card body
/// — coloured dot at @p dot_size px, peer's display name, and
/// "Offline" suffix when @p online is false. Dot alpha is
/// halved for offline so the row reads as faded even before the
/// suffix is parsed.
void render_workspace_member_row(const std::string& machine_id,
                                  const std::string& display_name,
                                  bool                online,
                                  float               dot_size);

}  // namespace xorio::ui::workspaces

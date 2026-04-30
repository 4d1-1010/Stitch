/// @file workspace.hpp
/// @brief Workspaces — user-defined groups of mesh PCs that share
/// keyboard, mouse, and clipboard between members.
///
/// Scope: data model + manager interface only. The mock impl in
/// `src/orchestrator/mock/workspace_manager.cpp` keeps state
/// in-process for this iteration; a future PR replaces it with a
/// CRDT-backed impl that propagates changes across the mesh.
///
/// A PC can belong to at most one workspace at a time. The Activity
/// tab enforces this on the UI side; the manager rejects mutations
/// that would violate it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xorio::orchestrator {

/// @brief Per-PC LWW stamp for workspace membership. Each PC owns
/// its own stamp; remote writes only override the local copy when
/// `logical_clock` exceeds the local one (ties → `is_member=false`
/// wins so a leave is sticky against an older stale "you're still
/// in" stamp). The legacy @ref Workspace::members set is recomputed
/// from these stamps on every load / mutation / merge so existing
/// readers don't need to know about the per-PC layer.
struct MemberStamp {
    bool          is_member     = false;
    std::uint64_t logical_clock = 0;
};

/// @brief Maximum clipboard payload size enforced by the workspace
/// (mirrors the Python tree's options). The mock currently stores
/// the choice and propagates it across the mesh; the real clipboard
/// pipeline reads the value at send time once it lands.
enum class ClipboardLimit : std::uint8_t {
    Kb100     = 0,
    Mb1       = 1,
    Mb5       = 2,
    Mb10      = 3,
    Unlimited = 4,
};

/// @brief Auto-unlock idle threshold.
enum class AutoUnlock : std::uint8_t {
    Off    = 0,
    Min5   = 1,
    Min15  = 2,
    Hour1  = 3,
};

/// @brief One row of the per-workspace display layout — the
/// global-space coordinates the user arranged for a single
/// monitor. Persisted alongside workspace settings; carried on
/// the LAN announce so every peer sees the same arrangement.
struct DisplayLayoutEntry {
    std::string  machine_id;   ///< Owner of the monitor.
    std::string  monitor_id;   ///< OS display name (eDP-1, \\\\.\\DISPLAY1, …).
    std::int32_t global_x = 0;
    std::int32_t global_y = 0;

    friend bool operator==(const DisplayLayoutEntry& a,
                           const DisplayLayoutEntry& b) {
        return a.machine_id == b.machine_id
            && a.monitor_id == b.monitor_id
            && a.global_x   == b.global_x
            && a.global_y   == b.global_y;
    }
    friend bool operator!=(const DisplayLayoutEntry& a,
                           const DisplayLayoutEntry& b) {
        return !(a == b);
    }
};

/// @brief Bag of settings that travel with a workspace. Bundled so
/// the UI can write every value in one call (and the manager can
/// bump version_ns + persist + broadcast once per Save click,
/// instead of N times for N individual setters).
struct WorkspaceSettings {
    ClipboardLimit clipboard_max          = ClipboardLimit::Mb1;
    bool           clipboard_rich         = true;
    bool           clipboard_files        = false;
    std::int32_t   cursor_edge_margin     = 4;
    bool           cursor_require_modifier = false;
    bool           cursor_block_hotkeys   = false;
    /// @brief When true, only workspace members can edit. Non-
    /// members still see the workspace + layout but the form's
    /// Save / Delete and the Layout's Apply are read-only for
    /// them.
    bool           locked                 = false;
    /// @brief Idle hours after which the regular Lock auto-clears.
    /// 0 disables — the lock then persists until manually toggled
    /// off. "Idle" is no whole-row mutation (matches the version_ns
    /// bump path) for this many hours.
    std::uint32_t  lock_unlock_after_h    = 0;
    // Master-Lock isn't carried here — it has owner-attribution
    // semantics that the settings struct can't express cleanly.
    // See @ref IWorkspaceManager::set_master_lock instead.
};

/// @brief One workspace.
///
/// Carries a per-record `version_ns` so peers can resolve concurrent
/// edits via last-writer-wins (highest version wins). Deletes are
/// modelled as tombstones — the manager keeps the row with
/// `tombstone == true` so the deletion propagates over the next
/// announce; @ref IWorkspaceManager::list() filters these out so the
/// UI never sees them.
/// Membership + per-cap toggles use the same per-PC LWW shape:
/// `*_stamps` is canonical (each PC owns its own stamp; the
/// merge picks newest clock, ties → `is_member==false` so a
/// "turn off" sticks), and the matching flat `members` /
/// `input_members` / `clipboard_members` sets are derived
/// caches recomputed by `recompute_members_locked` after every
/// mutation/merge so existing readers (UI, eviction, clipboard
/// coordinator) don't have to know about the stamp layer.
/// `input_members` / `clipboard_members` are always subsets of
/// `members` — Cursor / Clipboard caps gate behaviour for PCs
/// already in the workspace; non-members ignore them.
struct Workspace {
    std::string                     id;          ///< Stable opaque token.
    std::string                     name;        ///< User-editable label.

    std::unordered_map<std::string, MemberStamp> member_stamps;            ///< canonical per-PC LWW
    std::unordered_set<std::string>              members;                  ///< derived cache
    std::unordered_map<std::string, MemberStamp> input_member_stamps;      ///< canonical per-PC LWW
    std::unordered_set<std::string>              input_members;            ///< derived cache
    std::unordered_map<std::string, MemberStamp> clipboard_member_stamps;  ///< canonical per-PC LWW
    std::unordered_set<std::string>              clipboard_members;        ///< derived cache

    /// @brief Edit-lock bookkeeping. Empty string when unlocked.
    /// Set to a peer's machine_id while that peer is editing the
    /// workspace via the inline form. Used to grey-out remote-side
    /// edit controls; not a real distributed lock.
    std::string                     locked_by;

    /// @brief Lamport-style monotonic version. Bumped on every
    /// mutation; receivers accept an incoming record only when its
    /// version_ns exceeds their local copy's version_ns.
    std::uint64_t                   version_ns = 0;

    /// @brief Tombstone flag. Set true when the workspace is
    /// destroyed; the row stays in the catalogue so the deletion
    /// propagates on the wire. Not surfaced to the UI by list().
    bool                            tombstone  = false;

    // ── Clipboard policy ───────────────────────────────────────
    ClipboardLimit                  clipboard_max   = ClipboardLimit::Mb1;
    bool                            clipboard_rich  = true;
    bool                            clipboard_files = false;

    // ── Cursor policy ──────────────────────────────────────────
    /// @brief Edge-detection margin in display pixels — how close
    /// the cursor must be to a monitor edge before the mesh
    /// considers a hop to a neighbouring PC. 0 disables.
    std::int32_t                    cursor_edge_margin     = 4;
    /// @brief Require Ctrl+Shift to be held for cross-PC moves.
    bool                            cursor_require_modifier = false;
    /// @brief Block forwarding of OS-reserved hotkeys (Win+L, …).
    bool                            cursor_block_hotkeys    = false;

    // ── Auto-unlock (legacy) ───────────────────────────────────
    /// @brief Pre-Lock auto-unlock dropdown — kept on the wire +
    /// in JSON for backward compatibility. New code should ignore
    /// this and use the @ref locked / @ref master_locked fields
    /// below instead.
    AutoUnlock                      auto_unlock = AutoUnlock::Off;

    // ── Lock state ─────────────────────────────────────────────
    /// @brief See @ref WorkspaceSettings::locked.
    bool                            locked              = false;
    std::uint32_t                   lock_unlock_after_h = 0;
    /// @brief See @ref WorkspaceSettings::master_locked.
    bool                            master_locked              = false;
    std::uint32_t                   master_lock_unlock_after_h = 0;
    /// @brief machine_id of the peer that flipped Master-Lock on.
    /// Only that peer can mutate this workspace while Master-Lock
    /// is active; the field clears when Master-Lock toggles off
    /// (manually or via the idle auto-unlock).
    std::string                     master_locked_by;

    /// @brief User-arranged mesh-global positions for every
    /// monitor in the workspace. Empty until the user clicks
    /// Apply on the Layout tab; missing entries fall back to the
    /// per-peer local-probe coordinates (which is also where new
    /// monitors land before being moved).
    std::vector<DisplayLayoutEntry> layout;
};

/// @brief Edit-permission predicate matching the Lock / Master-
/// Lock UI: Master-Lock restricts edits to `master_locked_by`;
/// Lock restricts edits to current members; otherwise anyone may
/// edit (workspaces are visible to every peer regardless of
/// membership). Centralised so the UI gating, the orchestrator's
/// future write-side enforcement, and the auto-unlock worker
/// stay consistent.
inline bool can_edit_workspace(const Workspace& ws,
                                const std::string& local_machine_id) {
    if (ws.master_locked) {
        return ws.master_locked_by == local_machine_id;
    }
    if (ws.locked) {
        return ws.members.count(local_machine_id) > 0;
    }
    return true;
}

/// @brief Manager that owns the local workspace catalogue.
///
/// All actions are synchronous and apply immediately. Callers
/// register an `on_changed` callback to know when the catalogue
/// mutated (added / renamed / deleted / membership-changed) and
/// re-query via @ref list().
class IWorkspaceManager {
public:
    /// @brief Fired after every mutation. Argument is the affected
    /// workspace id, empty string for catalogue-wide changes
    /// (e.g. multiple deletes).
    using OnChangedFn = std::function<void(const std::string& workspace_id)>;

    virtual ~IWorkspaceManager() = default;

    // ── Queries ────────────────────────────────────────────────
    /// @brief Snapshot every workspace, sorted by name (stable
    /// across calls so the UI's iteration order doesn't jump).
    virtual std::vector<Workspace> list() const = 0;

    /// @brief Look up one workspace by id. Empty optional if it
    /// doesn't exist.
    virtual std::optional<Workspace> get(const std::string& id) const = 0;

    /// @brief Lookup table machine_id → workspace_id. PCs not in
    /// any workspace are absent from the map.
    virtual std::vector<std::pair<std::string, std::string>>
    pc_assignments() const = 0;

    // ── Mutations ──────────────────────────────────────────────
    /// @brief Create a new workspace with the given name +
    /// membership + per-capability sets. PCs already in another
    /// workspace are silently moved. The capability sets are
    /// clamped to subsets of @p members.
    /// @return The new workspace id.
    virtual std::string create(const std::string& name,
                               const std::unordered_set<std::string>& members,
                               const std::unordered_set<std::string>& input_members,
                               const std::unordered_set<std::string>& clipboard_members) = 0;

    /// @brief Rename @p id. No-op if the id is unknown.
    virtual void rename(const std::string& id, const std::string& new_name) = 0;

    /// @brief Replace the membership + per-capability sets of
    /// @p id. PCs already in another workspace are moved over
    /// (a PC can belong to at most one workspace). The
    /// capability sets are clamped to subsets of @p members.
    virtual void set_members(const std::string& id,
                             const std::unordered_set<std::string>& members,
                             const std::unordered_set<std::string>& input_members,
                             const std::unordered_set<std::string>& clipboard_members) = 0;

    /// @brief Replace the workspace's settings (clipboard, cursor,
    /// regular Lock). Bumps version_ns, persists, and notifies.
    /// Master-Lock is mutated separately via @ref set_master_lock
    /// so the owner-attribution rule (only the peer that flipped
    /// it on can clear it) doesn't bleed into a settings struct
    /// that has no concept of caller identity.
    virtual void set_settings(const std::string& id,
                              const WorkspaceSettings& settings) = 0;

    /// @brief Toggle Master-Lock. On a fresh @c true, the manager
    /// records @p caller_machine_id as `master_locked_by`; on
    /// @c false, that field clears. @p unlock_after_h is the idle
    /// auto-clear threshold in hours (0 disables). No-op when the
    /// workspace doesn't exist or is tombstoned.
    virtual void set_master_lock(const std::string& id,
                                  bool                enable,
                                  std::uint32_t       unlock_after_h,
                                  const std::string&  caller_machine_id) = 0;

    /// @brief Replace the workspace's display layout. Bumps
    /// version_ns + persists + notifies, so the new arrangement
    /// rides the next announce to every peer.
    virtual void set_layout(const std::string& id,
                            const std::vector<DisplayLayoutEntry>& layout) = 0;

    /// @brief Remove the workspace and orphan its members.
    virtual void destroy(const std::string& id) = 0;

    /// @brief Write the local PC's own membership stamp to false
    /// for @p id with a fresh logical clock. Only the per-PC
    /// stamp changes; whole-record fields (name, settings, layout)
    /// keep their existing version_ns. If the projected count of
    /// `is_member==true` stamps falls below 2 the workspace is
    /// tombstoned locally — the tombstone propagates through the
    /// existing whole-row LWW, and per-stamp updates ride the new
    /// member_stamps wire field, so peers converge whether they
    /// receive the leave first or the tombstone first.
    virtual void leave(const std::string& id,
                       const std::string& machine_id) = 0;

    // ── Replication ────────────────────────────────────────────
    /// @brief Snapshot every workspace including tombstoned ones,
    /// for broadcast on the wire. Sorted by id for stable output.
    virtual std::vector<Workspace> wire_state() const = 0;

    /// @brief Merge a remote-peer-supplied workspace list into the
    /// local catalogue using last-writer-wins keyed on
    /// @ref Workspace::version_ns. Per-id semantics:
    ///   * remote not in local: insert (skipping pure-tombstone
    ///     records that have no matching local entry — there's no
    ///     point holding a tombstone for something we never knew).
    ///   * remote.version_ns > local.version_ns: replace local
    ///     with remote (this is how renames, member changes, and
    ///     deletions all propagate).
    ///   * otherwise: keep local untouched.
    /// Fires @ref OnChangedFn at most once per call when at least
    /// one row was inserted or replaced.
    virtual void merge_remote(const std::vector<Workspace>& remote) = 0;

    // ── Edit lock ──────────────────────────────────────────────
    /// @brief Mark the workspace as being edited by @p machine_id.
    /// Same-id re-acquires are idempotent. No mesh propagation in
    /// the local-only impl; the lock just guards UI re-entry.
    virtual void acquire_lock(const std::string& id,
                              const std::string& machine_id) = 0;

    /// @brief Release the edit lock if @p machine_id holds it.
    virtual void release_lock(const std::string& id,
                              const std::string& machine_id) = 0;

    /// @brief Register the change-notification callback. Replaces
    /// any previously-registered callback.
    virtual void set_on_changed(OnChangedFn cb) = 0;
};

}  // namespace xorio::orchestrator

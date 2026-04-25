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
#include <unordered_set>
#include <vector>

namespace unio_ui::orchestrator {

/// @brief One workspace.
struct Workspace {
    std::string                     id;          ///< Stable opaque token.
    std::string                     name;        ///< User-editable label.
    std::unordered_set<std::string> members;     ///< Set of machine_ids.

    /// @brief Edit-lock bookkeeping. Empty string when unlocked.
    /// Set to a peer's machine_id while that peer is editing the
    /// workspace via the inline form. Used to grey-out remote-side
    /// edit controls; not a real distributed lock.
    std::string                     locked_by;
};

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
    /// @brief Create a new workspace with the given name + members.
    /// Members already in another workspace are silently moved.
    /// @return The new workspace id.
    virtual std::string create(const std::string& name,
                               const std::unordered_set<std::string>& members) = 0;

    /// @brief Rename @p id. No-op if the id is unknown.
    virtual void rename(const std::string& id, const std::string& new_name) = 0;

    /// @brief Replace the member set of @p id. Members already in
    /// another workspace are moved over. Empty member set is
    /// allowed — the workspace stays empty until populated.
    virtual void set_members(const std::string& id,
                             const std::unordered_set<std::string>& members) = 0;

    /// @brief Remove the workspace and orphan its members.
    virtual void destroy(const std::string& id) = 0;

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

}  // namespace unio_ui::orchestrator

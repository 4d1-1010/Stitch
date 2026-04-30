/// @file orchestrator.hpp
/// @brief Public façade the UI consumes. Queries + actions only;
/// push notifications arrive through @ref OrchestratorCallbacks.
#pragma once

#include "orchestrator/auth_state.hpp"
#include "orchestrator/callbacks.hpp"
#include "orchestrator/display.hpp"
#include "orchestrator/peer.hpp"
#include "orchestrator/stream.hpp"
#include "orchestrator/workspace.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace xorio::orchestrator {

/// @brief Single entry point the UI depends on.
///
/// All query methods are safe to call every frame; the façade
/// returns snapshots of internal state guarded by a mutex.
/// Action methods return immediately and complete asynchronously
/// — results surface through the callbacks registered at
/// construction.
class IOrchestrator {
public:
    virtual ~IOrchestrator() = default;

    // ── Identity queries ───────────────────────────────────
    virtual std::string local_machine_id() const = 0;
    virtual std::string local_display_name() const = 0;
    virtual AuthState   auth_state() const = 0;

    // ── Access gate ────────────────────────────────────────
    //
    // Pre-launch placeholder: a single hardcoded key gates the
    // rest of the app. Real licence-token verification (paste +
    // signature check + state machine) lands on a follow-up
    // branch and replaces this surface without renaming it.
    //
    /// @return @c true once the user has entered the correct key
    /// since process start. The Access tab uses this to unlock
    /// the rest of the rail; other tabs render a locked-state
    /// banner while it is @c false.
    virtual bool access_authorized() const = 0;

    /// @brief Compare @p key to the hardcoded gate value and, on
    /// match, transition to authorized.
    /// @return @c true if @p key matched and @c access_authorized()
    /// is now @c true; @c false otherwise (state unchanged).
    virtual bool try_authorize(const std::string& key) = 0;

    // ── Mesh queries ───────────────────────────────────────
    virtual std::vector<Peer>    peers() const = 0;
    virtual std::vector<Display> displays() const = 0;
    virtual StreamState stream_state(StreamId id) const = 0;

    // ── Actions ────────────────────────────────────────────

    /// @brief Start a stream from @p src to @p dst.
    /// @return A new @ref StreamId on success; @c StreamId{0} if
    /// the request failed synchronous validation. Subsequent
    /// progress surfaces through @c on_stream_started /
    /// @c on_stream_failed callbacks.
    virtual StreamId start_stream(DisplayRef src, DisplayRef dst,
                                  RoutingMode mode) = 0;

    /// @brief Stop the stream identified by @p id.
    virtual void stop_stream(StreamId id) = 0;

    /// @brief Initiate an outgoing pairing request.
    /// @param machine_id  Remote peer fingerprint.
    /// @param invite_code Shared secret shown on the remote
    ///                    peer's Access screen (QR / 6-digit PIN).
    virtual void request_pair(const std::string& machine_id,
                              const std::string& invite_code) = 0;

    /// @brief Accept an incoming pairing request.
    virtual void accept_pairing(const std::string& machine_id) = 0;

    /// @brief Reject an incoming pairing request.
    virtual void reject_pairing(const std::string& machine_id,
                                const std::string& reason) = 0;

    /// @brief Revoke an existing pairing. Closes the control
    /// connection and drops the peer's CRDT slot.
    virtual void unpair(const std::string& machine_id) = 0;

    /// @brief Local user clicked Identify. Bumps the local
    /// identify counter (broadcast in the next announce so every
    /// mesh peer fires their own Identify overlay) and invokes
    /// `OrchestratorCallbacks::on_identify_request` so this PC's
    /// overlays fire too.
    virtual void request_identify() = 0;

    // ── Workspace queries ──────────────────────────────────
    //
    // Workspaces group mesh PCs that share keyboard / mouse /
    // clipboard. State is local-only in this iteration; a future
    // PR replaces the mock with a CRDT-backed manager so the
    // catalogue replicates across the mesh.

    /// @brief Snapshot the workspace catalogue, sorted by name.
    virtual std::vector<Workspace> workspaces() const = 0;

    /// @brief Look up one workspace by id. Empty if it doesn't exist.
    virtual std::optional<Workspace>
    workspace(const std::string& id) const = 0;

    /// @brief Map machine_id → workspace_id for every PC currently
    /// claimed by a workspace.
    virtual std::vector<std::pair<std::string, std::string>>
    pc_workspace_assignments() const = 0;

    // ── Workspace actions ──────────────────────────────────
    /// @brief Create a workspace; PCs already in another workspace
    /// are moved over silently. Membership is the explicit member
    /// set; input / clipboard are per-member capability subsets.
    /// @return The new workspace id.
    virtual std::string
    create_workspace(const std::string& name,
                     const std::unordered_set<std::string>& members,
                     const std::unordered_set<std::string>& input_members,
                     const std::unordered_set<std::string>& clipboard_members) = 0;

    /// @brief Rename @p workspace_id to @p new_name. No-op on
    /// unknown id.
    virtual void rename_workspace(const std::string& workspace_id,
                                  const std::string& new_name) = 0;

    /// @brief Replace @p workspace_id's membership + per-cap sets.
    virtual void set_workspace_members(
        const std::string& workspace_id,
        const std::unordered_set<std::string>& members,
        const std::unordered_set<std::string>& input_members,
        const std::unordered_set<std::string>& clipboard_members) = 0;

    /// @brief Replace @p workspace_id's settings (clipboard, cursor,
    /// auto-unlock).
    virtual void set_workspace_settings(
        const std::string& workspace_id,
        const WorkspaceSettings& settings) = 0;

    /// @brief Replace @p workspace_id's display layout (each
    /// monitor's mesh-global position). Persists + syncs to every
    /// peer via the same LWW path as the rest of workspace state.
    virtual void set_workspace_layout(
        const std::string& workspace_id,
        const std::vector<DisplayLayoutEntry>& layout) = 0;

    /// @brief Remove @p workspace_id and orphan its members.
    virtual void delete_workspace(const std::string& workspace_id) = 0;

    /// @brief Local PC leaves @p workspace_id — writes only its
    /// own per-PC membership stamp to false. The workspace stays
    /// alive on its other members until the projection drops below
    /// 2 active stamps anywhere; the local PC sees the workspace
    /// disappear from @ref workspaces() immediately.
    virtual void leave_workspace(const std::string& workspace_id) = 0;

    // ── Alone-online state ──────────────────────────────────
    //
    // Whenever a workspace has exactly 1 online member (the local
    // PC), we surface a banner asking "stay or leave". The flag
    // below is session-local — it resets on app launch and on the
    // ≥2 → 1 transition. UI calls @ref mark_alone_prompt_answered
    // when the user clicks Stay (or any non-Leave action that
    // implies "I'm aware"); the banner stays visible while
    // `is_alone_in_workspace && !is_alone_prompt_answered`.

    /// @brief True iff @p workspace_id has the local PC as its
    /// only currently-online member (and the local PC is still a
    /// member). Used by the UI to drive the inline banner.
    virtual bool is_alone_in_workspace(const std::string& workspace_id) const = 0;

    /// @brief True iff the user has already acted on the current
    /// alone-prompt for @p workspace_id (clicked Stay). Resets
    /// when the workspace returns to ≥2 online or on app launch.
    virtual bool is_alone_prompt_answered(const std::string& workspace_id) const = 0;

    /// @brief Mark the alone-prompt for @p workspace_id as
    /// answered (Stay). The banner dismisses, the badge clears,
    /// and a "Leave workspace" pill replaces the banner in the
    /// workspace's row until the alone state ends.
    virtual void mark_alone_prompt_answered(const std::string& workspace_id) = 0;

    /// @brief Mark @p workspace_id as edited by the local PC. Used
    /// by the Activity tab to gate the inline edit form against
    /// re-entry; not a real distributed lock.
    virtual void acquire_workspace_lock(const std::string& workspace_id) = 0;

    /// @brief Release the local PC's edit lock on @p workspace_id.
    virtual void release_workspace_lock(const std::string& workspace_id) = 0;
};

/// @brief Construct a mock orchestrator populated with simulated
/// mesh state (Diana appears after a short delay, local peer
/// transitions from GracePeriod to SignedIn).
///
/// @param callbacks  Push callbacks invoked from the worker
/// thread as simulated events fire. Any member may be @c nullptr.
///
/// @return Owned pointer; holds its worker thread until destroyed.
std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks);

}  // namespace xorio::orchestrator

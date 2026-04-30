/// @file cursor_router.hpp
/// @brief Owner of the "is the cursor here?" state machine.
///
/// Scope: edge-detection + handoff over the control channel. This
/// is the bridge between the local cursor poller and the network:
///   * When this peer is *active* and the cursor crosses a local
///     monitor edge that maps to a peer's display, fire a Handoff
///     and become *dormant*.
///   * When dormant, the poller's outputs are ignored — the cursor
///     stays put on this PC because it's not the source.
///   * On receiving a Handoff, become active and warp the local
///     cursor to the entry point.
///
/// The router holds no transport — the orchestrator wires it to
/// the control channel + input backend via simple callbacks. That
/// keeps this file pure logic and easy to unit-test later.
///
/// Threading: methods are called from the cursor-poller thread and
/// the control-channel reader thread. All shared state is guarded
/// by the internal mutex.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace unio_ui::orchestrator {

/// @brief One monitor rectangle. Carries both the local OS
/// position (where the cursor lives in this peer's screen
/// coordinate space) and the mesh-global position (where the
/// user has arranged the monitor in the Layout canvas). The
/// router translates between the two when the cursor approaches
/// an edge so adjacency follows the user-arranged layout.
struct RouterMonitor {
    std::string  machine_id;
    std::int32_t global_x = 0;   ///< Position in mesh-global coords (after layout).
    std::int32_t global_y = 0;
    std::int32_t local_x  = 0;   ///< Original OS-local position (only used for the local peer).
    std::int32_t local_y  = 0;
    std::int32_t width    = 0;
    std::int32_t height   = 0;
};

class CursorRouter {
public:
    /// @brief Fired when the active peer hands the cursor off.
    /// @p target  receiver's machine_id.
    /// @p entry_x / entry_y  coords *in the receiver's local OS
    /// virtual screen space*. The router clamps before warping.
    using OnHandoffSendFn = std::function<void(const std::string& target,
                                                 std::int32_t entry_x,
                                                 std::int32_t entry_y)>;

    /// @brief Fired when this peer takes the cursor — the
    /// orchestrator warps the local cursor to (@p x, @p y) via
    /// the input backend.
    using OnWarpLocalFn = std::function<void(std::int32_t x,
                                              std::int32_t y)>;

    /// @brief Fired while this peer is *dormant* and the user is
    /// still moving the physical mouse: the router pins the local
    /// cursor at the handoff edge and forwards the user's frame-
    /// to-frame deltas to the peer that owns the cursor. The
    /// orchestrator wraps these in a MouseRel control frame.
    using OnForwardMotionFn =
        std::function<void(const std::string& target,
                            std::int32_t dx,
                            std::int32_t dy)>;

    /// @brief Apply a relative cursor delta to the local OS
    /// cursor. Fired by apply_remote_delta when this peer is
    /// receiving a MouseRel from a remote peer but not in
    /// tracked mode (i.e. the cursor is locally home and a
    /// peer is forwarding their mouse motion to us as the
    /// active source). The orchestrator wires this to
    /// get_cursor_pos + inject_mouse_move(cur+delta).
    using OnRelativeMotionFn =
        std::function<void(std::int32_t dx, std::int32_t dy)>;

    /// @param local_machine_id  Identifies us in the peer strip.
    /// @param on_handoff_send   Invoked when we hand off.
    /// @param on_warp_local     Invoked when we take the cursor.
    /// @param on_forward_motion Invoked per frame while dormant
    ///                          to forward the user's motion
    ///                          deltas to the active peer.
    /// @param on_relative_motion Invoked when a remote peer
    ///                          forwards a delta and we're
    ///                          locally active (cursor home);
    ///                          orchestrator translates to a
    ///                          local OS cursor move.
    CursorRouter(std::string         local_machine_id,
                 OnHandoffSendFn     on_handoff_send,
                 OnWarpLocalFn       on_warp_local,
                 OnForwardMotionFn   on_forward_motion,
                 OnRelativeMotionFn  on_relative_motion);

    /// @brief Update the router's view of every peer's monitor
    /// rects. Indexed by machine_id; the router looks up the
    /// receiver's monitors when computing the entry point.
    void set_monitors(std::vector<RouterMonitor> monitors);

    /// @brief Cursor poller: "the cursor moved to (x, y) on me".
    /// If we're active and the new position is past a monitor edge
    /// that maps to a neighbour, fire the handoff.
    void on_local_cursor_move(std::int32_t local_x, std::int32_t local_y);

    /// @brief Control channel: "@p source says I now own the cursor."
    void on_remote_handoff(const std::string& source,
                            std::int32_t entry_x,
                            std::int32_t entry_y);

    /// @brief Apply a relative motion delta arriving from the
    /// peer that just handed us the cursor. Updates our internal
    /// tracked workspace position, warps the OS cursor to follow,
    /// and runs edge detection against the tracked position so
    /// touchpad-driver phantom events on the OS cursor (Synaptics
    /// / ELAN / Precision Touchpad on Windows laptops) can't
    /// trip a return handoff. Mirrors Barrier's client-side model:
    /// the local OS cursor is downstream of our state, never an
    /// input to it.
    void apply_remote_delta(std::int32_t dx, std::int32_t dy);

    /// @brief Hook for the platform raw-input listener: real
    /// local hardware mouse motion just fired. Used as a flag
    /// the dormant poll path consults to filter touchpad-driver
    /// phantom-correction events out of the forwarded delta
    /// stream — polled cursor changes that aren't accompanied
    /// by a raw event are ghosts, not user input. The raw-
    /// capture layer already strips synthetic events (Win32
    /// hDevice == 0, X11 XI_RawMotion is HID-only) so this
    /// only ever sees real hardware motion.
    void note_local_hardware_motion();

    /// @brief When this peer is a tracked-mode receiver but
    /// can't initiate handoffs (Input unchecked), the local
    /// user is allowed to move the visiting cursor with their
    /// own mouse — the cursor stays under remote control,
    /// but the *position* should follow whatever local input
    /// produced. Otherwise the next apply_remote_delta from
    /// the remote driver would warp the cursor away from
    /// where the local user just moved it back to where
    /// tracked thinks the cursor is. Sync tracked_global to
    /// the polled (local OS) cursor position so the remote
    /// stream resumes from there.
    void sync_tracked_to_polled(std::int32_t local_x,
                                  std::int32_t local_y);

    /// @brief True when this peer is the active cursor source.
    bool is_local_active() const;

    /// @brief machine_id of the peer that currently owns the
    /// cursor while we're dormant — empty while we're active or
    /// before the first handoff. Used by the orchestrator to
    /// route forwarded button / scroll frames.
    std::string forward_target() const;

    /// @brief Force the router into a dormant state. Used at
    /// startup so a single peer doesn't claim the cursor before
    /// any peers are even visible.
    void force_dormant();

    /// @brief Update the per-workspace capability flags for the
    /// local peer. @p is_cursor_member controls whether local
    /// mouse motion can initiate a handoff (i.e. push the cursor
    /// onto another peer); @p is_keyboard_member controls
    /// whether local key events forward while we're dormant.
    /// Non-cursor-members can still *receive* a handoff and act
    /// as a destination — they just can't be the initiator from
    /// their own physical mouse.
    void set_local_member_flags(bool is_cursor_member,
                                 bool is_keyboard_member);

    /// @brief Set the edge-trigger margin in pixels. The active
    /// path fires a handoff once the cursor sits within this
    /// distance of a monitor edge that maps to a peer; the entry
    /// inset on the receiving side is also derived from it.
    /// Updated from @ref refresh_cursor_router_state when the
    /// active workspace's `cursor_edge_margin` setting changes.
    /// Clamped to a minimum of 1 px so edge-detect math never
    /// degenerates to "cursor must be exactly on the edge".
    void set_edge_margin(std::int32_t margin);

    /// @brief Reclaim active state if @p machine_id was the peer
    /// currently holding the cursor (we're dormant, forwarding to
    /// it) or visiting us (it sent us a handoff and stayed
    /// remotely-active on its end). Without this, a peer crash /
    /// app close while it holds the cursor would strand us in
    /// dormant mode forever — input grabbed, cursor hidden, no
    /// way to recover except restart. The orchestrator calls this
    /// from the control-channel on_disconnected callback.
    void on_peer_lost(const std::string& machine_id);

    /// @brief Set the workspace's "Hold Ctrl+Shift to cross"
    /// gate. When true, edge-triggered handoffs (active and
    /// tracked paths both) only fire while the modifier is held;
    /// when false, the gate is transparent. Updated from
    /// @ref refresh_cursor_router_state along with the rest of
    /// the per-workspace settings.
    void set_require_modifier(bool require);

    /// @brief Push the live "Ctrl+Shift held" state into the
    /// router. The orchestrator computes this from both local
    /// raw key events (the user typing on this PC's keyboard)
    /// and forwarded inbound keys (the user typing on a dormant
    /// peer's keyboard while the cursor lives here), so the gate
    /// works correctly regardless of which side the user is
    /// physically at. No-op when @ref set_require_modifier was
    /// false on the active workspace.
    void set_modifier_held(bool held);

    /// @brief True while local key events should be forwarded to
    /// the active peer. Read by the orchestrator before sending
    /// a KeyEvent frame so we don't leak local typing onto a
    /// peer the user opted out of via the Keyboard checkbox.
    bool keyboard_forwardable() const;

    /// @brief Compute a pin-warp target for the dormant peer's
    /// cursor — the centre of the local monitor the cursor is
    /// currently on, returned IF the cursor sits within
    /// @c edge_threshold pixels of any local-monitor edge.
    /// Returns false (no warp needed) when the cursor is
    /// safely in the interior. The orchestrator's on_motion
    /// path uses this after forwarding a delta so the source's
    /// polled cursor is kept away from the OS clamp — without
    /// it, a user pushing past their own monitor's edge stops
    /// generating polled deltas and the receiver's cursor hits
    /// an invisible "wall" mid-screen.
    bool pin_warp_target(std::int32_t local_x,
                          std::int32_t local_y,
                          std::int32_t edge_threshold,
                          std::int32_t& warp_local_x,
                          std::int32_t& warp_local_y) const;

    /// @brief Snapshot of the workspace member flags. Used by
    /// the orchestrator's grab-state sync — pointer grab is
    /// only meaningful for cursor members, keyboard grab only
    /// for keyboard members.
    bool is_cursor_member() const;
    bool is_keyboard_member() const;

private:
    /// @brief Bounds of every monitor whose machine_id matches
    /// @p mid, packed for the locked-section computations.
    std::vector<const RouterMonitor*> monitors_for_locked(
        const std::string& mid) const;

    mutable std::mutex          m_;
    std::string                 local_id_;
    OnHandoffSendFn             on_handoff_send_;
    OnWarpLocalFn               on_warp_local_;
    OnForwardMotionFn           on_forward_motion_;
    OnRelativeMotionFn          on_relative_motion_;

    std::vector<RouterMonitor>  monitors_;
    bool                        active_         = true;
    /// @brief When dormant, the machine_id of the active peer we
    /// forward motion deltas to. Empty while active or before the
    /// first handoff.
    std::string                 forward_target_;
    /// @brief Local OS pixel where we pin the cursor while
    /// dormant — set to the cursor's position at the moment we
    /// fired the outbound handoff. The router warps the cursor
    /// back here on every poll so the user's continuing motion
    /// shows up as a clean delta.
    std::int32_t                pinned_local_x_ = 0;
    std::int32_t                pinned_local_y_ = 0;

    /// @brief Previous sample's local cursor position. Dormant
    /// deltas are computed against this, not against pinned, so
    /// the warp-back-to-pin doesn't itself look like user motion
    /// once the cursor is settled.
    std::int32_t                last_sample_x_  = 0;
    std::int32_t                last_sample_y_  = 0;

    /// @brief When we just issued a warp (entering dormant or
    /// re-pinning after a forwarded delta), wait until we see a
    /// cursor sample at the warp target before resuming delta
    /// forwarding. Without this gate the very-next poll lands
    /// before X has processed the warp and we'd forward the
    /// (huge) pre-warp position as user motion.
    bool                        warp_pending_       = false;
    std::int32_t                warp_target_x_      = 0;
    std::int32_t                warp_target_y_      = 0;
    /// @brief Bound on how many polls we wait for the warp to
    /// land before giving up and resyncing — otherwise a missed
    /// warp would silently kill all forwarding.
    int                         warp_pending_count_ = 0;

    /// @brief Per-workspace capability flags. Default to true so
    /// a router that has never been configured behaves like the
    /// pre-checkbox build. Updated via @ref set_local_member_flags.
    bool                        is_cursor_member_   = true;
    bool                        is_keyboard_member_ = true;
    /// @brief True when our active state was triggered by a
    /// received handoff (a remote peer sent the cursor to us).
    /// Cleared the moment we fire a handoff back. Lets a non-
    /// cursor-member peer still hand the cursor BACK to the
    /// peer that visited it — without unlocking handoffs from
    /// the unchecked peer's own local mouse.
    bool                        remotely_active_    = false;

    /// @brief Tracked cursor position in mesh-global coords while
    /// we're remotely active — i.e. the cursor is visiting us,
    /// driven by deltas the source peer forwards. Updated only
    /// in @ref apply_remote_delta. Edge detection runs against
    /// this position instead of the polled OS cursor so any
    /// touchpad-driver phantom corrections to the OS cursor
    /// don't trip a return handoff. Valid iff @ref tracked_valid_.
    /// @ref tracked_last_at_ stamps each apply_remote_delta so
    /// the active poll path can drop tracked mode after an
    /// idle timeout — when the source peer stops forwarding,
    /// the cursor is "home" again and the local user's
    /// hardware should drive it via the polled path.
    bool                        tracked_valid_     = false;
    std::int32_t                tracked_global_x_  = 0;
    std::int32_t                tracked_global_y_  = 0;
    std::chrono::steady_clock::time_point tracked_last_at_{};

    /// @brief Counter of hardware-origin raw motion events seen
    /// since the last dormant-mode poll. A change in the
    /// polled cursor is forwarded only when this counter is
    /// non-zero — otherwise it's a touchpad-driver ghost
    /// moving the OS cursor without any real user input,
    /// which would drag the active peer's cursor if forwarded.
    /// The dormant poll path consumes (zeroes) the counter
    /// on each tick.
    std::uint32_t               raw_motion_since_poll_ = 0;

    /// @brief Wall-clock of the last received handoff and the
    /// peer that sent it. We suppress *return* handoffs to that
    /// same peer for a short window so a small mouse drift
    /// while the user reaches for the keyboard doesn't ping-pong
    /// the cursor straight back. Crosses to *other* peers stay
    /// unaffected so the user can keep moving across the mesh.
    std::chrono::steady_clock::time_point last_received_at_{};
    std::string                            last_received_from_;
    /// @brief Sticky edge-hit flag — once a handoff fires, we
    /// don't send another until the cursor moves *off* the edge
    /// (mirrors the Python tree's `_edge_hit_sent` debounce so a
    /// user lingering at the edge doesn't ping-pong). Default
    /// `true` so the very first cursor sample after startup
    /// can't spuriously fire — the user has to move the cursor
    /// away from the edge once before any handoff is allowed.
    bool                        edge_hit_sent_  = true;
    /// @brief Pixel margin on each edge — cursor at this distance
    /// from an edge counts as "at the edge". Updated from the
    /// workspace's `cursor_edge_margin` setting via
    /// @ref set_edge_margin.
    std::int32_t                edge_margin_    = 4;

    /// @brief Workspace's "Hold Ctrl+Shift to cross" gate. When
    /// true, edge-triggered handoffs only fire while the modifier
    /// is held. Updated via @ref set_require_modifier.
    bool                        require_modifier_ = false;
    /// @brief Live Ctrl+Shift held state pushed in by the
    /// orchestrator. Updated via @ref set_modifier_held.
    bool                        modifier_held_    = false;
};

}  // namespace unio_ui::orchestrator
